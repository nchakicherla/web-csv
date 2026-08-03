#ifndef TOKEN_TYPES_H
#define TOKEN_TYPES_H

/* The token alphabet is generated from token.def so the enum, the name table
 * and the scanner's dispatch tables cannot drift out of sync. To add a token,
 * edit token.def and nothing else.
 *
 * TOKEN_TYPE is a plain int rather than a bare enum because grammar files can
 * declare additional tokens (#token / #keyword) that get IDs past TK__COUNT. */

enum {
#define TK_PUNCT(name, lexeme)   name,
#define TK_KEYWORD(name, lexeme) name,
#define TK_SPECIAL(name)         name,
#include "token.def"
#undef TK_PUNCT
#undef TK_KEYWORD
#undef TK_SPECIAL
	TK__COUNT
};

typedef int TOKEN_TYPE;

/* "no such token" - distinct from TK_ERROR, which is a real token the scanner
 * emits for malformed input. */
#define TK__NONE (-1)

#endif // TOKEN_TYPES_H
