#ifndef AST_NODE_H
#define AST_NODE_H

#include "common.h"
#include "scanner.h"
#include "syntax_types.h"

/* A node is exactly one of three things:
 *
 *   is_token      a matched terminal; `token` is meaningful, no children
 *   is_anonymous  a grouping node produced by a sequence/repetition in the
 *                 grammar; it carries no name and its children are spliced into
 *                 its parent rather than nesting
 *   named         `type` is the rule that produced it
 *
 * The old code signalled "anonymous" by leaving `type` at STX_ERROR, which
 * collided with the real STX_ERROR rule and indexed one past the end of the
 * label table when printed. */
typedef struct s_SyntaxNode {
	SYNTAX_TYPE type; /* STX__NONE while anonymous */

	bool is_token;
	bool is_anonymous;
	Token token;

	size_t n_children;
	struct s_SyntaxNode **children;
	struct s_SyntaxNode *parent;
} SyntaxNode;

void initSyntaxNode(SyntaxNode *node);

#endif // AST_NODE_H
