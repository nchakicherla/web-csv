#include "ast.h"
#include "color.h"
#include "oom.h"

#include <stdio.h>
#include <string.h>

static SyntaxNode *newAnonNode(Arena *arena) {
	SyntaxNode *node = checkAlloc(arena_alloc_type(arena, SyntaxNode));
	initSyntaxNode(node);
	return node;
}

static void addChild(SyntaxNode *parent, SyntaxNode *child, Arena *arena) {
	if (parent->n_children == 0) {
		parent->children = checkAlloc(arena_alloc(arena, sizeof(SyntaxNode *), _Alignof(SyntaxNode *)));
	} else {
		parent->children = checkAlloc(arena_grow(arena, parent->children,
		                              parent->n_children * sizeof(SyntaxNode *),
		                              (parent->n_children + 1) * sizeof(SyntaxNode *),
		                              _Alignof(SyntaxNode *)));
	}
	parent->children[parent->n_children] = child;
	parent->n_children++;
	child->parent = parent;
}

/* Anonymous nodes are grouping artifacts, not structure the caller asked for,
 * so their children are hoisted into the parent instead of nesting. Doing this
 * at every append keeps the tree flat by construction - the old code had to
 * special-case the splice in three different places. */
static void appendResult(SyntaxNode *parent, SyntaxNode *child, Arena *arena) {
	if (child->is_anonymous && !child->is_token) {
		for (size_t i = 0; i < child->n_children; i++) {
			appendResult(parent, child->children[i], arena);
		}
		return;
	}
	addChild(parent, child, arena);
}

static SyntaxNode *wrapNode(SyntaxNode *child, SYNTAX_TYPE stype, Arena *arena) {
	SyntaxNode *parent = newAnonNode(arena);
	parent->type = stype;
	parent->is_anonymous = false;
	addChild(parent, child, arena);
	return parent;
}

static SyntaxNode *parseTerminal(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	SyntaxNode *node;

	if (stream->pos > stream->furthest) {
		stream->furthest = stream->pos;
	}
	if (stream->tk[stream->pos].type != rnode->as.t) {
		return NULL;
	}

	node = newAnonNode(arena);
	node->is_token = true;
	node->token = stream->tk[stream->pos];
	stream->pos++;
	return node;
}

/* A rule reference names whatever its body produced. If the body already came
 * back named - or is a bare token - renaming it would destroy that name, so it
 * gets wrapped instead. */
static SyntaxNode *parseRuleRef(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	SyntaxNode *node = parseNode(rnode->rule_reference, stream, arena);

	if (!node) {
		return NULL;
	}
	if (node->is_token || !node->is_anonymous) {
		return wrapNode(node, rnode->as.s, arena);
	}
	node->type = rnode->as.s;
	node->is_anonymous = false;
	return node;
}

static SyntaxNode *parseSequence(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	SyntaxNode *node = newAnonNode(arena);
	size_t start = stream->pos;

	for (size_t i = 0; i < rnode->n_children; i++) {
		SyntaxNode *child = parseNode(rnode->children[i], stream, arena);
		if (!child) {
			stream->pos = start;
			return NULL;
		}
		appendResult(node, child, arena);
	}
	return node;
}

static SyntaxNode *parseAlternation(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	size_t start = stream->pos;

	for (size_t i = 0; i < rnode->n_children; i++) {
		SyntaxNode *child;
		stream->pos = start;
		child = parseNode(rnode->children[i], stream, arena);
		if (child) {
			return child;
		}
	}
	stream->pos = start;
	return NULL;
}

/* Optional and repeated groups never fail - they match empty. Because of that,
 * a sequence can treat any NULL child as a hard failure, which is what removes
 * the nest of "was this child optional?" checks from the old parseAnd. */
static SyntaxNode *parseOptional(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	size_t start = stream->pos;
	SyntaxNode *child = parseNode(rnode->children[0], stream, arena);

	if (!child) {
		stream->pos = start;
		return newAnonNode(arena);
	}
	return child;
}

static SyntaxNode *parseRepetition(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	SyntaxNode *node = newAnonNode(arena);

	while (true) {
		size_t reset = stream->pos;
		SyntaxNode *child = parseNode(rnode->children[0], stream, arena);

		if (!child) {
			stream->pos = reset;
			break;
		}
		/* A body that can match empty (say, a group of optionals) would spin
		 * here forever. Consuming nothing ends the repetition. */
		if (stream->pos == reset) {
			break;
		}
		appendResult(node, child, arena);
	}
	return node;
}

/* A rule that can reach itself without consuming a token - `A = A, b ;` and
 * friends - recurses forever. Recursive descent cannot parse left recursion at
 * all, so the only question is whether it reports that or blows the C stack.
 * The limit is far above any legitimate nesting depth. */
#define MAX_PARSE_DEPTH 1024

static SyntaxNode *parseNodeInner(const RuleNode *rnode, TokenStream *stream, Arena *arena);

SyntaxNode *parseNode(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	SyntaxNode *result;

	/* Once the limit is hit, abandon the whole parse rather than just this
	 * branch. Failing one branch would send alternation off to try the next
	 * one, which recurses just as deep - the search explodes and the arena
	 * grows until the process is killed. */
	if (stream->depth_exceeded) {
		return NULL;
	}
	if (stream->depth >= MAX_PARSE_DEPTH) {
		stream->depth_exceeded = true;
		return NULL;
	}
	stream->depth++;
	result = parseNodeInner(rnode, stream, arena);
	stream->depth--;
	return result;
}

static SyntaxNode *parseNodeInner(const RuleNode *rnode, TokenStream *stream, Arena *arena) {
	switch (rnode->kind) {
		case RULE_GRM:
			switch (rnode->as.g) {
				case GRM_SEQ:    return parseSequence(rnode, stream, arena);
				case GRM_ALT:    return parseAlternation(rnode, stream, arena);
				case GRM_OPT:    return parseOptional(rnode, stream, arena);
				case GRM_REPEAT: return parseRepetition(rnode, stream, arena);
			}
			return NULL;
		case RULE_STX:
			return parseRuleRef(rnode, stream, arena);
		case RULE_TK:
			return parseTerminal(rnode, stream, arena);
	}
	return NULL;
}

SyntaxNode *parseRuleAt(const Grammar *grammar, SYNTAX_TYPE type,
                        TokenStream *stream, Arena *arena) {
	const RuleNode *head = grammarRuleFor(grammar, type);
	size_t start = stream->pos;
	SyntaxNode *root;

	if (!head) {
		return NULL;
	}

	stream->depth = 0;
	stream->depth_exceeded = false;

	root = parseNode(head, stream, arena);

	/* A rule made entirely of optionals matches empty. Treating that as a
	 * failure keeps a caller looping over statements from spinning forever. */
	if (!root || stream->pos == start) {
		stream->pos = start;
		return NULL;
	}
	/* Nothing referenced this rule, so its own name has not been applied. */
	if (root->is_anonymous) {
		root->type = type;
		root->is_anonymous = false;
	}
	return root;
}

SyntaxNode *parseWithRule(const Grammar *grammar, SYNTAX_TYPE type,
                          TokenStream *stream, Arena *arena) {
	SyntaxNode *root;

	stream->pos = 0;
	root = parseRuleAt(grammar, type, stream, arena);
	if (!root) {
		return NULL;
	}
	/* Trailing junk is a failure, not a success with leftovers. */
	if (stream->pos < stream->n && stream->tk[stream->pos].type != TK_EOF) {
		stream->pos = 0;
		return NULL;
	}
	return root;
}

SyntaxNode *parseTokenStream(const Grammar *grammar, TokenStream *stream, Arena *arena) {
	SyntaxNode *root;

	if (!grammarRuleFor(grammar, grammar->start)) {
		fprintf(stderr, "parse error: grammar has no start rule\n");
		return NULL;
	}

	stream->furthest = 0;
	root = parseWithRule(grammar, grammar->start, stream, arena);

	if (stream->depth_exceeded) {
		fprintf(stderr, "parse error: rule nesting exceeded %d levels near line %zu.\n"
		                "  This usually means a left-recursive rule (one that can reach\n"
		                "  itself without consuming a token). Recursive descent cannot\n"
		                "  parse left recursion; rewrite the rule using {} repetition.\n",
		        MAX_PARSE_DEPTH, stream->tk[stream->furthest].line);
		return NULL;
	}

	if (!root) {
		const Token *at = &stream->tk[stream->furthest];
		fprintf(stderr, "%zu: parse error: unexpected %s \"%.*s\"\n",
		        at->line, tokenName(grammar->reg, at->type),
		        (int)at->len, at->start);
		return NULL;
	}
	return root;
}

/* --- debug output -------------------------------------------------------- */

void fPrintSyntaxNode(Registry *reg, const SyntaxNode *node, unsigned indent, FILE *file) {
	for (unsigned i = 0; i < indent; i++) {
		fputc('\t', file);
	}

	if (node->is_token) {
		fprintf(file, "- %s \"%.*s\"\n",
		        tokenName(reg, node->token.type),
		        (int)node->token.len, node->token.start);
		return;
	}

	fprintf(file, "- /%s\n", syntaxName(reg, node->type));
	for (size_t i = 0; i < node->n_children; i++) {
		fPrintSyntaxNode(reg, node->children[i], indent + 1, file);
	}
}

void printSyntaxNode(Registry *reg, const SyntaxNode *node, unsigned indent) {
	for (unsigned i = 0; i < indent; i++) {
		putchar('\t');
	}

	if (node->is_token) {
		printf("- ");
		setColor(ANSI_GREEN);
		printf("%s ", tokenName(reg, node->token.type));
		resetColor();
		setColor(ANSI_YELLOW);
		printf("%.*s\n", (int)node->token.len, node->token.start);
		resetColor();
		return;
	}

	printf("- ");
	setColor(ANSI_CYAN);
	printf("/%s\n", syntaxName(reg, node->type));
	resetColor();

	for (size_t i = 0; i < node->n_children; i++) {
		printSyntaxNode(reg, node->children[i], indent + 1);
	}
}
