#ifndef REGISTRY_H
#define REGISTRY_H

/* registry.h - the single place that maps names to type IDs, both ways.
 *
 * Built-in names come from token.def and syntax.def and keep their
 * compile-time enum values, so the interpreter can still switch on them.
 * Grammar files can extend both vocabularies at load time:
 *
 *   - a rule head naming an unknown STX_* mints a new syntax ID (>= STX__COUNT)
 *   - a #token / #keyword directive declares a new token and its lexeme
 *
 * The scanner's dispatch tables are built from this registry rather than
 * hand-written, which is what makes new tokens actually scannable.
 */

#include "arena.h"
#include "common.h"
#include "token_types.h"
#include "syntax_types.h"

typedef struct s_NameTable {
	const char **names;
	size_t n;
	size_t cap;
} NameTable;

typedef struct s_PunctEntry {
	char c0;
	char c1; /* '\0' for single-character punctuation */
	TOKEN_TYPE type;
} PunctEntry;

typedef struct s_KeywordEntry {
	const char *word;
	size_t len;
	TOKEN_TYPE type;
} KeywordEntry;

typedef struct s_Registry {
	Arena *arena;

	NameTable tokens;
	NameTable syntax;

	/* single-character dispatch, TK__NONE where the byte is not punctuation */
	TOKEN_TYPE punct1[256];

	/* two-character punctuation, tried before punct1 so longest match wins */
	PunctEntry *punct2;
	size_t n_punct2;
	size_t cap_punct2;

	/* kept sorted by first byte; kw_start/kw_count bucket by that byte */
	KeywordEntry *keywords;
	size_t n_keywords;
	size_t cap_keywords;
	size_t kw_start[256];
	size_t kw_count[256];
} Registry;

void initRegistry(Registry *reg, Arena *arena);

/* Names -> IDs. Lookups take an explicit length so they can run straight off a
 * Token without copying. */
TOKEN_TYPE registryFindToken(Registry *reg, const char *name, size_t len);
SYNTAX_TYPE registryFindSyntax(Registry *reg, const char *name, size_t len);
SYNTAX_TYPE registryInternSyntax(Registry *reg, const char *name, size_t len);

/* IDs -> names. Always in range: unknown IDs return a placeholder rather than
 * indexing off the end of the table. */
const char *tokenName(Registry *reg, TOKEN_TYPE type);
const char *syntaxName(Registry *reg, SYNTAX_TYPE type);

size_t registrySyntaxCount(Registry *reg);
size_t registryTokenCount(Registry *reg);

/* Grammar-declared tokens. Return TK__NONE if the name is already taken or the
 * lexeme is unusable (empty, or longer than two characters for punctuation). */
TOKEN_TYPE registryAddPunct(Registry *reg, const char *name, size_t nlen,
                            const char *lex, size_t llen);
TOKEN_TYPE registryAddKeyword(Registry *reg, const char *name, size_t nlen,
                              const char *lex, size_t llen);

/* Scanner-facing lookups. */
TOKEN_TYPE registryMatchPunct(Registry *reg, char c0, char c1, size_t *out_len);
TOKEN_TYPE registryMatchKeyword(Registry *reg, const char *word, size_t len);

#endif // REGISTRY_H
