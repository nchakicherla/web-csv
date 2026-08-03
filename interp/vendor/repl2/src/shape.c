#include "shape.h"

#include <stddef.h>

/* --- tree navigation --------------------------------------------------------
 *
 * Same style as the original flexible-parser interp.c's own helpers (namedAt/
 * namedOfType/firstTokenChild) - locate parts by role, not by index, so this
 * check tolerates the same grammar reshuffling the rest of this project does.
 */

static size_t namedCount(const SyntaxNode *node) {
	size_t n = 0;
	for (size_t i = 0; i < node->n_children; i++) {
		if (!node->children[i]->is_token) {
			n++;
		}
	}
	return n;
}

static size_t namedCountOfType(const SyntaxNode *node, SYNTAX_TYPE type) {
	size_t n = 0;
	for (size_t i = 0; i < node->n_children; i++) {
		if (!node->children[i]->is_token && node->children[i]->type == type) {
			n++;
		}
	}
	return n;
}

/* TK__NONE means "any token type accepted". */
static bool hasTokenOfType(const SyntaxNode *node, TOKEN_TYPE type) {
	for (size_t i = 0; i < node->n_children; i++) {
		if (node->children[i]->is_token &&
		    (type == TK__NONE || node->children[i]->token.type == type)) {
			return true;
		}
	}
	return false;
}

static const SyntaxNode *lastNamed(const SyntaxNode *node) {
	const SyntaxNode *last = NULL;
	for (size_t i = 0; i < node->n_children; i++) {
		if (!node->children[i]->is_token) {
			last = node->children[i];
		}
	}
	return last;
}

/* Line of the first terminal anywhere under `node`, for error messages -
 * same idea as interp.c's own nodeLine(). */
static size_t nodeLine(const SyntaxNode *node) {
	if (node->is_token) {
		return node->token.line;
	}
	for (size_t i = 0; i < node->n_children; i++) {
		size_t line = nodeLine(node->children[i]);
		if (line) {
			return line;
		}
	}
	return 0;
}

/* --- contracts ---------------------------------------------------------- */

typedef enum {
	SHAPE_MIN_NAMED,  /* at least `min` named (non-token) children, any type */
	SHAPE_TYPE_COUNT, /* `min`..`max` named children of type `type` (max<0 = unbounded) */
	SHAPE_HAS_TOKEN,  /* at least one token child of type `type` (TK__NONE = any) */
	SHAPE_LAST_IS,    /* the last named child, if any, must be of type `type` */
} ShapeRuleKind;

typedef struct {
	ShapeRuleKind kind;
	int type; /* a SYNTAX_TYPE or TOKEN_TYPE, depending on kind - unused for SHAPE_MIN_NAMED */
	int min;
	int max; /* -1 = unbounded */
	const char *why;
} ShapeRule;

#define MAX_RULES 2

typedef struct {
	SYNTAX_TYPE tag;
	ShapeRule rules[MAX_RULES];
	size_t n_rules;
} ShapeContract;

/* Derived from what src/interp.c *would* need if built (see repl2's README -
 * there is no interpreter here), read directly off the original
 * flexible-parser's interp.c: execDeclare/execInit/execAssign look a STX_VAR
 * up by type; execIf/execWhile/execFor read their parts by position and
 * arity alone; callUserFunction requires its function's last named child to
 * be an STX_SCOPE; evalCall/numberFromToken/stringFromToken/STX_VAR's lookup
 * all require a raw token to read text from. */
static const ShapeContract CONTRACTS[] = {
	{ STX_DECLARE, {
		{ SHAPE_TYPE_COUNT, STX_VAR, 1, -1, "a variable to declare" },
	}, 1 },
	{ STX_INIT, {
		{ SHAPE_TYPE_COUNT, STX_VAR, 1, -1, "a variable to initialise" },
		{ SHAPE_MIN_NAMED, 0, 2, -1, "a variable and an initial value (2 named parts)" },
	}, 2 },
	{ STX_ASSIGN, {
		{ SHAPE_TYPE_COUNT, STX_VAR, 1, -1, "a variable to assign to" },
		{ SHAPE_MIN_NAMED, 0, 2, -1, "a target and a value (2 named parts)" },
	}, 2 },
	{ STX_IF, {
		{ SHAPE_MIN_NAMED, 0, 2, -1, "a condition and a body (2 named parts)" },
	}, 1 },
	{ STX_WHILE, {
		{ SHAPE_MIN_NAMED, 0, 1, -1, "a condition" },
	}, 1 },
	{ STX_FOR, {
		{ SHAPE_MIN_NAMED, 0, 3, -1, "an init, a condition, and a step (3 named parts)" },
	}, 1 },
	{ STX_FNDEF, {
		{ SHAPE_HAS_TOKEN, TK_IDENTIFIER, 1, -1, "a name" },
		{ SHAPE_LAST_IS, STX_SCOPE, 0, 0, "a body (its last named part must be a scope)" },
	}, 2 },
	{ STX_FNCALL, {
		{ SHAPE_HAS_TOKEN, TK_IDENTIFIER, 1, -1, "a callee name" },
	}, 1 },
	{ STX_NUM, {
		{ SHAPE_HAS_TOKEN, TK__NONE, 1, -1, "a literal token" },
	}, 1 },
	{ STX_STRLIT, {
		{ SHAPE_HAS_TOKEN, TK__NONE, 1, -1, "a literal token" },
	}, 1 },
	{ STX_VAR, {
		{ SHAPE_HAS_TOKEN, TK__NONE, 1, -1, "an identifier token" },
	}, 1 },
};

#define N_CONTRACTS (sizeof(CONTRACTS) / sizeof(CONTRACTS[0]))

static bool ruleHolds(const SyntaxNode *node, const ShapeRule *rule) {
	switch (rule->kind) {
		case SHAPE_MIN_NAMED:
			return namedCount(node) >= (size_t)rule->min;
		case SHAPE_TYPE_COUNT: {
			size_t n = namedCountOfType(node, (SYNTAX_TYPE)rule->type);
			return n >= (size_t)rule->min && (rule->max < 0 || n <= (size_t)rule->max);
		}
		case SHAPE_HAS_TOKEN:
			return hasTokenOfType(node, (TOKEN_TYPE)rule->type);
		case SHAPE_LAST_IS: {
			const SyntaxNode *last = lastNamed(node);
			return last && last->type == rule->type;
		}
	}
	return true;
}

static void checkNode(Registry *reg, const SyntaxNode *node, FILE *out, size_t *violations) {
	if (!node->is_token) {
		for (size_t c = 0; c < N_CONTRACTS; c++) {
			const ShapeContract *contract = &CONTRACTS[c];
			if (contract->tag != node->type) {
				continue;
			}
			for (size_t r = 0; r < contract->n_rules; r++) {
				const ShapeRule *rule = &contract->rules[r];
				if (!ruleHolds(node, rule)) {
					fprintf(out, "%zu: shape warning: rule tagged /%s is missing %s\n",
					        nodeLine(node), syntaxName(reg, node->type), rule->why);
					(*violations)++;
				}
			}
			break; /* one contract per tag */
		}
	}

	for (size_t i = 0; i < node->n_children; i++) {
		checkNode(reg, node->children[i], out, violations);
	}
}

size_t checkShapes(Registry *reg, const SyntaxNode *root, FILE *out) {
	size_t violations = 0;
	checkNode(reg, root, out, &violations);
	return violations;
}
