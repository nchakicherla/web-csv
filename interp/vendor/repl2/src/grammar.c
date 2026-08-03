#include "grammar.h"
#include "file.h"
#include "oom.h"

#include <stdio.h>
#include <string.h>

/* Cheap insurance against a pathological or malicious grammar file recursing
 * the C stack to death. The deepest rule in a realistic grammar is well under
 * a dozen levels. */
#define MAX_GRAMMAR_DEPTH 256

typedef struct s_GParser {
	Token *tk;
	size_t n;
	size_t pos;
	Registry *reg;
	Arena *arena;
	const char *filename;
	unsigned depth;
	bool had_error;
} GParser;

typedef struct s_NodeList {
	RuleNode **items;
	size_t n;
	size_t cap;
} NodeList;

typedef struct s_PendingRule {
	SYNTAX_TYPE stype;
	RuleNode *head;
	size_t line;
} PendingRule;

typedef struct s_RuleList {
	PendingRule *items;
	size_t n;
	size_t cap;
} RuleList;

/* --- token cursor -------------------------------------------------------- */

/* Safe unconditionally: tokenizeAll always terminates the array with TK_EOF and
 * advance() refuses to step past it, so cur() can never run off the end. This
 * is what replaces the unbounded `while(true)` scans in the old builder. */
static Token *cur(GParser *gp) {
	return &gp->tk[gp->pos];
}

static bool check(GParser *gp, TOKEN_TYPE type) {
	return cur(gp)->type == type;
}

static void advance(GParser *gp) {
	if (gp->tk[gp->pos].type != TK_EOF) {
		gp->pos++;
	}
}

static bool match(GParser *gp, TOKEN_TYPE type) {
	if (check(gp, type)) {
		advance(gp);
		return true;
	}
	return false;
}

static void gerror(GParser *gp, const Token *at, const char *msg) {
	gp->had_error = true;
	fprintf(stderr, "%s:%zu: grammar error: %s (near \"%.*s\")\n",
	        gp->filename, at->line, msg, (int)at->len, at->start);
}

/* --- growable arrays ----------------------------------------------------- */

static void nodeListPush(NodeList *list, RuleNode *node, Arena *arena) {
	if (list->n == list->cap) {
		size_t new_cap = (list->cap == 0) ? 8 : list->cap * 2;
		list->items = checkAlloc(arena_grow(arena, list->items, list->cap * sizeof(RuleNode *),
		                                    new_cap * sizeof(RuleNode *), _Alignof(RuleNode *)));
		list->cap = new_cap;
	}
	list->items[list->n++] = node;
}

static void ruleListPush(RuleList *list, PendingRule rule, Arena *arena) {
	if (list->n == list->cap) {
		size_t new_cap = (list->cap == 0) ? 64 : list->cap * 2;
		list->items = checkAlloc(arena_grow(arena, list->items, list->cap * sizeof(PendingRule),
		                                    new_cap * sizeof(PendingRule), _Alignof(PendingRule)));
		list->cap = new_cap;
	}
	list->items[list->n++] = rule;
}

static RuleNode *newNode(GParser *gp, RULE_NODE_TYPE kind) {
	RuleNode *node = checkAlloc(arena_zalloc(gp->arena, sizeof(RuleNode), _Alignof(RuleNode)));
	node->kind = kind;
	return node;
}

static RuleNode *newComposite(GParser *gp, GRAMMAR_TYPE g, NodeList *children) {
	RuleNode *node = newNode(gp, RULE_GRM);
	node->as.g = g;
	node->children = children->items;
	node->n_children = children->n;
	for (size_t i = 0; i < children->n; i++) {
		children->items[i]->parent = node;
	}
	return node;
}

/* --- the rule expression parser -----------------------------------------
 *
 * Three mutually recursive functions, one pass, no rescanning and no bracket
 * depth counters - recursion tracks nesting for free. This replaces
 * getPrevalentType, getPrevalentGrammarType, countChildren, getDelimOffset,
 * getSemicolonOffsetFromRuleStart and fillGrammarNode.
 *
 * Precedence follows conventional EBNF: parseAlt sits above parseSeq, so '|'
 * binds looser than ',' and a rule body is an alternation of sequences. In
 * recursive descent the outermost function handles the loosest operator, so
 * swapping these two calls is all it would take to change the convention.
 */

static RuleNode *parseAlt(GParser *gp);

static RuleNode *parseAtom(GParser *gp) {
	Token *t = cur(gp);

	if (t->type == TK_LPAREN || t->type == TK_LSQUARE || t->type == TK_LBRACE) {
		TOKEN_TYPE open = t->type;
		TOKEN_TYPE close = (open == TK_LPAREN)  ? TK_RPAREN
		                 : (open == TK_LSQUARE) ? TK_RSQUARE
		                                        : TK_RBRACE;
		RuleNode *inner;
		NodeList one = {0};

		if (++gp->depth > MAX_GRAMMAR_DEPTH) {
			gerror(gp, t, "grammar nested too deeply");
			return NULL;
		}
		advance(gp);
		inner = parseAlt(gp); /* brackets re-enter at the loosest level */
		gp->depth--;

		if (!inner) {
			return NULL;
		}
		if (!match(gp, close)) {
			gerror(gp, cur(gp), "unclosed group");
			return NULL;
		}

		/* '(' ')' is grouping only - it exists to override precedence and
		 * leaves no trace in the rule tree. */
		if (open == TK_LPAREN) {
			return inner;
		}

		nodeListPush(&one, inner, gp->arena);
		return newComposite(gp, (open == TK_LSQUARE) ? GRM_OPT : GRM_REPEAT, &one);
	}

	if (t->type == TK_IDENTIFIER) {
		TOKEN_TYPE as_token = registryFindToken(gp->reg, t->start, t->len);
		RuleNode *node;

		advance(gp);

		/* A name the registry knows as a token is a terminal; anything else is
		 * a rule reference, interned on first sight. Names that are referenced
		 * but never defined are caught after the whole file is read. */
		if (as_token != TK__NONE) {
			node = newNode(gp, RULE_TK);
			node->as.t = as_token;
		} else {
			node = newNode(gp, RULE_STX);
			node->as.s = registryInternSyntax(gp->reg, t->start, t->len);
		}
		return node;
	}

	gerror(gp, t, "expected a token name, rule name, or group");
	return NULL;
}

static RuleNode *parseSeq(GParser *gp) {
	RuleNode *first = parseAtom(gp);
	NodeList list = {0};

	if (!first || !check(gp, TK_COMMA)) {
		return first;
	}

	nodeListPush(&list, first, gp->arena);
	while (match(gp, TK_COMMA)) {
		RuleNode *next = parseAtom(gp);
		if (!next) {
			return NULL;
		}
		nodeListPush(&list, next, gp->arena);
	}
	return newComposite(gp, GRM_SEQ, &list);
}

static RuleNode *parseAlt(GParser *gp) {
	RuleNode *first = parseSeq(gp);
	NodeList list = {0};

	if (!first || !check(gp, TK_PIPE)) {
		return first;
	}

	nodeListPush(&list, first, gp->arena);
	while (match(gp, TK_PIPE)) {
		RuleNode *next = parseSeq(gp);
		if (!next) {
			return NULL;
		}
		nodeListPush(&list, next, gp->arena);
	}
	return newComposite(gp, GRM_ALT, &list);
}

/* --- directives ---------------------------------------------------------- */

static bool tokenTextIs(const Token *t, const char *word) {
	return t->len == strlen(word) && 0 == memcmp(t->start, word, t->len);
}

/* '#token' NAME "lexeme" ';'  or  '#keyword' NAME "lexeme" ';'
 *
 * This is what makes a new vocabulary usable rather than merely nameable: the
 * scanner's tables are built from the registry, so a token declared here is
 * scannable in the source file immediately. */
static bool parseDirective(GParser *gp) {
	Token *kind;
	Token *name;
	Token *lexeme;
	const char *lex;
	size_t lex_len;
	TOKEN_TYPE added;

	advance(gp); /* '#' */

	if (!check(gp, TK_IDENTIFIER)) {
		gerror(gp, cur(gp), "expected 'token' or 'keyword' after '#'");
		return false;
	}
	kind = cur(gp);
	advance(gp);

	if (!check(gp, TK_IDENTIFIER)) {
		gerror(gp, cur(gp), "expected a token name");
		return false;
	}
	name = cur(gp);
	advance(gp);

	if (!check(gp, TK_CHARS)) {
		gerror(gp, cur(gp), "expected a quoted lexeme");
		return false;
	}
	lexeme = cur(gp);
	advance(gp);

	if (!match(gp, TK_SEMICOLON)) {
		gerror(gp, cur(gp), "expected ';' after directive");
		return false;
	}

	lex = lexeme->start + 1; /* strip the quotes the scanner kept */
	lex_len = lexeme->len - 2;

	if (tokenTextIs(kind, "token")) {
		added = registryAddPunct(gp->reg, name->start, name->len, lex, lex_len);
		if (added == TK__NONE) {
			gerror(gp, name, "duplicate token name, or lexeme is not 1-2 characters");
			return false;
		}
	} else if (tokenTextIs(kind, "keyword")) {
		added = registryAddKeyword(gp->reg, name->start, name->len, lex, lex_len);
		if (added == TK__NONE) {
			gerror(gp, name, "duplicate token name, or empty lexeme");
			return false;
		}
	} else {
		gerror(gp, kind, "unknown directive, expected 'token' or 'keyword'");
		return false;
	}
	return true;
}

/* --- reference resolution ------------------------------------------------ */

static bool resolveRefs(Grammar *grammar, RuleNode *node, const char *filename) {
	bool ok = true;

	if (node->kind == RULE_STX) {
		node->rule_reference = grammar->rules[node->as.s].head;
		if (!node->rule_reference) {
			fprintf(stderr, "%s: grammar error: rule '%s' is referenced but never defined\n",
			        filename, syntaxName(grammar->reg, node->as.s));
			return false;
		}
		return true;
	}

	/* Only composites have children. Walking explicitly avoids stamping a
	 * bogus rule_reference onto a RULE_GRM node. */
	for (size_t i = 0; i < node->n_children; i++) {
		if (!resolveRefs(grammar, node->children[i], filename)) {
			ok = false;
		}
	}
	return ok;
}

/* --- entry point --------------------------------------------------------- */

int loadGrammar(Grammar *grammar, const char *filename, Registry *reg, Arena *arena) {
	char *source;
	Token *tokens;
	size_t n_tokens = 0;
	Token bad_token;
	GParser gp = {0};
	RuleList pending = {0};
	bool ok = true;

	grammar->reg = reg;
	grammar->rules = NULL;
	grammar->n_rules = 0;
	grammar->start = STX__NONE;

	source = tryReadFile(filename, arena);
	if (!source) {
		fprintf(stderr, "could not read grammar file '%s'\n", filename);
		return 1;
	}

	tokens = tokenizeAll(source, reg, arena, &n_tokens, &bad_token);
	if (!tokens) {
		fprintf(stderr, "%s:%zu: grammar error: %.*s\n",
		        filename, bad_token.line, (int)bad_token.len, bad_token.start);
		return 2;
	}

	gp.tk = tokens;
	gp.n = n_tokens;
	gp.pos = 0;
	gp.reg = reg;
	gp.arena = arena;
	gp.filename = filename;

	while (!check(&gp, TK_EOF)) {
		Token *name;
		PendingRule rule;

		if (check(&gp, TK_HASH)) {
			if (!parseDirective(&gp)) {
				return 3;
			}
			continue;
		}

		if (!check(&gp, TK_IDENTIFIER)) {
			gerror(&gp, cur(&gp), "expected a rule name");
			return 3;
		}
		name = cur(&gp);
		advance(&gp);

		if (!match(&gp, TK_EQUAL)) {
			gerror(&gp, cur(&gp), "expected '=' after rule name");
			return 3;
		}

		rule.stype = registryInternSyntax(reg, name->start, name->len);
		rule.line = name->line;
		rule.head = parseAlt(&gp); /* a rule body starts at the loosest level */
		if (!rule.head) {
			return 3;
		}

		if (!match(&gp, TK_SEMICOLON)) {
			gerror(&gp, cur(&gp), "expected ';' at end of rule");
			return 3;
		}

		ruleListPush(&pending, rule, arena);
	}

	if (pending.n == 0) {
		fprintf(stderr, "%s: grammar error: no rules defined\n", filename);
		return 4;
	}

	/* Syntax IDs are dense, so the rule table can be indexed by ID directly.
	 * The count is only known now, after interning every name the file used. */
	grammar->n_rules = registrySyntaxCount(reg);
	grammar->rules = checkAlloc(arena_zalloc(arena, arena_checked_mul(arena, grammar->n_rules, sizeof(GrammarRule)),
	                                         _Alignof(GrammarRule)));
	for (size_t i = 0; i < grammar->n_rules; i++) {
		grammar->rules[i].stype = (SYNTAX_TYPE)i;
		grammar->rules[i].head = NULL;
	}

	for (size_t i = 0; i < pending.n; i++) {
		SYNTAX_TYPE st = pending.items[i].stype;
		if (grammar->rules[st].head) {
			fprintf(stderr, "%s:%zu: grammar error: rule '%s' defined more than once\n",
			        filename, pending.items[i].line, syntaxName(reg, st));
			ok = false;
			continue;
		}
		grammar->rules[st].head = pending.items[i].head;
	}

	/* The first rule in the file is the start symbol, so retargeting the parser
	 * at a different language is purely a grammar-file edit. */
	grammar->start = pending.items[0].stype;

	for (size_t i = 0; i < pending.n; i++) {
		if (!resolveRefs(grammar, pending.items[i].head, filename)) {
			ok = false;
		}
	}

	return ok ? 0 : 5;
}

const RuleNode *grammarRuleFor(const Grammar *grammar, SYNTAX_TYPE type) {
	if (type < 0 || (size_t)type >= grammar->n_rules) {
		return NULL;
	}
	return grammar->rules[type].head;
}

/* --- debug output -------------------------------------------------------- */

static const char *grammarTypeName(GRAMMAR_TYPE g) {
	switch (g) {
		case GRM_SEQ:    return "SEQ";
		case GRM_ALT:    return "ALT";
		case GRM_OPT:    return "OPT";
		case GRM_REPEAT: return "REPEAT";
	}
	return "?";
}

static void fPrintRuleNode(const Grammar *grammar, const RuleNode *node,
                           unsigned indent, FILE *file) {
	for (unsigned i = 0; i < indent; i++) {
		fputc('\t', file);
	}

	switch (node->kind) {
		case RULE_GRM:
			fprintf(file, "%s\n", grammarTypeName(node->as.g));
			for (size_t i = 0; i < node->n_children; i++) {
				fPrintRuleNode(grammar, node->children[i], indent + 1, file);
			}
			break;
		case RULE_STX:
			fprintf(file, "SYNTAX --- %s\n", syntaxName(grammar->reg, node->as.s));
			break;
		case RULE_TK:
			fprintf(file, "TOKEN  --- %s\n", tokenName(grammar->reg, node->as.t));
			break;
	}
}

void fPrintGrammar(const Grammar *grammar, FILE *file) {
	for (size_t i = 0; i < grammar->n_rules; i++) {
		if (!grammar->rules[i].head) {
			continue;
		}
		fprintf(file, "RULE: %s%s\n",
		        syntaxName(grammar->reg, grammar->rules[i].stype),
		        (grammar->rules[i].stype == grammar->start) ? "   (start)" : "");
		fprintf(file, "-\n");
		fPrintRuleNode(grammar, grammar->rules[i].head, 1, file);
		fprintf(file, "\n----\n----\n\n");
	}
}
