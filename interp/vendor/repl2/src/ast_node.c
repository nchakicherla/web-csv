#include "ast_node.h"

void initSyntaxNode(SyntaxNode *node) {
	node->type = STX__NONE;
	node->is_token = false;
	node->is_anonymous = true;
	node->n_children = 0;
	node->children = NULL;
	node->parent = NULL;
}
