#ifndef GRAMMAR_H
#define GRAMMAR_H

/* grammar.h - loading rule trees from a grammar file.
 *
 * Grammar syntax:
 *
 *   rule       := NAME '=' alt ';'
 *   alt        := seq ( '|' seq )*
 *   seq        := atom ( ',' atom )*
 *   atom       := NAME | '(' alt ')' | '[' alt ']' | '{' alt '}'
 *   directive  := '#token' NAME STRING ';' | '#keyword' NAME STRING ';'
 *
 * Precedence is conventional EBNF: '|' binds LOOSER than ',', so a rule body
 * is an alternation of sequences.
 *
 *   a, b | c, d   ==   ALT(SEQ(a, b), SEQ(c, d))
 *
 * An alternation used as one element of a sequence therefore needs explicit
 * parentheses:
 *
 *   a, (b | c), d   ==   SEQ(a, ALT(b, c), d)
 *
 * A NAME that resolves in the token registry is a terminal; anything else is a
 * reference to another rule, interned on first sight. Referencing a rule that
 * is never defined is reported after the whole file is read.
 *
 * '(' ')' is pure grouping and produces no node - the inner tree is returned
 * directly.
 */

#include "arena.h"
#include "common.h"
#include "registry.h"
#include "scanner.h"
#include "syntax_types.h"
#include "token_types.h"

#include <stdio.h>

typedef enum {
	GRM_SEQ,    /* all children must match, in order        (',') */
	GRM_ALT,    /* first matching child wins                ('|') */
	GRM_OPT,    /* child may match zero or one time         ('[]') */
	GRM_REPEAT, /* child may match zero or more times       ('{}') */
} GRAMMAR_TYPE;

typedef enum {
	RULE_GRM, /* composite node; combine children per `as.g`  */
	RULE_STX, /* reference to another rule                    */
	RULE_TK,  /* terminal: match one token of type `as.t`     */
} RULE_NODE_TYPE;

typedef struct s_RuleNode {
	RULE_NODE_TYPE kind;

	union {
		GRAMMAR_TYPE g;
		SYNTAX_TYPE s;
		TOKEN_TYPE t;
	} as;

	struct s_RuleNode *rule_reference; /* RULE_STX only: head of target rule */

	size_t n_children;
	struct s_RuleNode **children;
	struct s_RuleNode *parent;
} RuleNode;

typedef struct s_GrammarRule {
	SYNTAX_TYPE stype;
	RuleNode *head; /* NULL if the name is referenced but never defined */
} GrammarRule;

typedef struct s_Grammar {
	Registry *reg;
	GrammarRule *rules; /* indexed directly by SYNTAX_TYPE */
	size_t n_rules;
	SYNTAX_TYPE start; /* first rule defined in the file */
} Grammar;

/* Reads, scans and compiles a grammar file. Returns 0 on success; on failure
 * prints a diagnostic naming the file and line and returns non-zero. */
int loadGrammar(Grammar *grammar, const char *filename, Registry *reg, Arena *arena);

const RuleNode *grammarRuleFor(const Grammar *grammar, SYNTAX_TYPE type);

void fPrintGrammar(const Grammar *grammar, FILE *file);

#endif // GRAMMAR_H
