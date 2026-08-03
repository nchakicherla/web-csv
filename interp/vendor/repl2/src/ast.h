#ifndef AST_H
#define AST_H

/* ast.h - matching a token stream against a rule tree.
 *
 * Every parse function obeys one invariant: on failure it returns NULL and
 * leaves the stream position exactly where it found it. That is what makes
 * alternation and repetition safe to backtrack through.
 */

#include "arena.h"
#include "common.h"
#include "grammar.h"
#include "scanner.h"
#include "ast_node.h"

#include <stdio.h>

typedef struct s_TokenStream {
	Token *tk;
	size_t n;
	size_t pos;
	size_t furthest; /* high-water mark, so a failed parse can say where it gave up */
	unsigned depth;
	bool depth_exceeded; /* set once; almost always means a left-recursive rule */
} TokenStream;

/* Matches `rnode` at the current position. */
SyntaxNode *parseNode(const RuleNode *rnode, TokenStream *stream, Arena *arena);

/* Matches one named rule at the current position, reporting nothing on failure
 * and leaving the position untouched when it fails. A match must consume at
 * least one token, so a rule that can match empty cannot spin the caller's
 * loop. The REPL uses this to walk an entry statement by statement, trying
 * candidate rules at each step.
 *
 * `stream->furthest` is never reset here, so repeated calls accumulate the best
 * position reached across every attempt - which is what makes the eventual
 * error message point at the real problem. */
SyntaxNode *parseRuleAt(const Grammar *grammar, SYNTAX_TYPE type,
                        TokenStream *stream, Arena *arena);

/* As above, but starts at the beginning and requires the whole stream to be
 * consumed. */
SyntaxNode *parseWithRule(const Grammar *grammar, SYNTAX_TYPE type,
                          TokenStream *stream, Arena *arena);

/* Matches the grammar's start rule and requires the whole stream to be
 * consumed. Returns NULL and reports the furthest position reached on failure. */
SyntaxNode *parseTokenStream(const Grammar *grammar, TokenStream *stream, Arena *arena);

void printSyntaxNode(Registry *reg, const SyntaxNode *node, unsigned indent);
void fPrintSyntaxNode(Registry *reg, const SyntaxNode *node, unsigned indent, FILE *file);

#endif // AST_H
