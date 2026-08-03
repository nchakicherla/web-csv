#ifndef SCANNER_H
#define SCANNER_H

/* scanner.h - lexical analysis.
 *
 * The scanner holds no global state: every entry point takes a Scanner*, so
 * several sources (a grammar file and a script, say) can be scanned
 * independently. Keyword and punctuation recognition is driven entirely by the
 * Registry rather than hand-written switches, which is what lets a grammar file
 * introduce tokens the scanner has never heard of.
 */

#include "arena.h"
#include "common.h"
#include "registry.h"
#include "token_types.h"

typedef struct s_Token {
	TOKEN_TYPE type;
	const char *start;
	size_t len;
	size_t line;
} Token;

typedef struct s_Scanner {
	const char *start;
	const char *current;
	size_t line;
	Registry *reg;
} Scanner;

void initScanner(Scanner *sc, const char *source, Registry *reg);
Token scanToken(Scanner *sc);

/* Scans the whole source into an arena-allocated array in one pass. On success
 * returns the array and writes the count to out_n. On a malformed token returns
 * NULL and, if out_err is non-NULL, writes the offending token there so the
 * caller can report a line number. */
Token *tokenizeAll(const char *source, Registry *reg, Arena *arena,
                   size_t *out_n, Token *out_err);

void printToken(Registry *reg, const Token *token);

#endif // SCANNER_H
