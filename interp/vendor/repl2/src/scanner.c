#include "scanner.h"
#include "oom.h"

#include <stdio.h>
#include <string.h>

void initScanner(Scanner *sc, const char *source, Registry *reg) {
	sc->start = source;
	sc->current = source;
	sc->line = 1;
	sc->reg = reg;
}

static bool isAtEnd(Scanner *sc) {
	return *sc->current == '\0';
}

static char advance(Scanner *sc) {
	sc->current++;
	return sc->current[-1];
}

static char peek(Scanner *sc) {
	return *sc->current;
}

static char peekNext(Scanner *sc) {
	if (isAtEnd(sc)) {
		return '\0';
	}
	return sc->current[1];
}

static bool isDigit(char c) {
	return c >= '0' && c <= '9';
}

static bool isAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static Token makeToken(Scanner *sc, TOKEN_TYPE type) {
	Token token;
	token.type = type;
	token.start = sc->start;
	token.len = (size_t)(sc->current - sc->start);
	token.line = sc->line;
	return token;
}

static Token makeErrorToken(Scanner *sc, const char *message) {
	Token token;
	token.type = TK_ERROR;
	token.start = message;
	token.len = strlen(message);
	token.line = sc->line;
	return token;
}

static void skipWhitespace(Scanner *sc) {
	while (true) {
		switch (peek(sc)) {
			case ' ':
			case '\r':
			case '\t':
				advance(sc);
				break;
			case '\n':
				sc->line++;
				advance(sc);
				break;
			case '/':
				if (peekNext(sc) == '/') {
					while (peek(sc) != '\n' && !isAtEnd(sc)) {
						advance(sc);
					}
				} else if (peekNext(sc) == '*') {
					advance(sc);
					advance(sc);
					while (!isAtEnd(sc)) {
						if (peek(sc) == '*' && peekNext(sc) == '/') {
							advance(sc);
							advance(sc);
							break;
						}
						if (peek(sc) == '\n') {
							sc->line++;
						}
						advance(sc);
					}
				} else {
					return; /* a bare '/' is punctuation, not a comment */
				}
				break;
			default:
				return;
		}
	}
}

static Token scanString(Scanner *sc) {
	while (peek(sc) != '"' && !isAtEnd(sc)) {
		if (peek(sc) == '\n') {
			return makeErrorToken(sc, "unterminated string");
		}
		advance(sc);
	}
	if (isAtEnd(sc)) {
		return makeErrorToken(sc, "unterminated string");
	}
	advance(sc); /* closing quote */
	return makeToken(sc, TK_CHARS);
}

static Token scanNumber(Scanner *sc) {
	while (isDigit(peek(sc))) {
		advance(sc);
	}
	if (peek(sc) == '.' && isDigit(peekNext(sc))) {
		advance(sc);
		while (isDigit(peek(sc))) {
			advance(sc);
		}
	}
	if (peek(sc) == 'e' && isDigit(peekNext(sc))) {
		advance(sc);
		while (isDigit(peek(sc))) {
			advance(sc);
		}
	}
	return makeToken(sc, TK_NUM);
}

/* Keyword recognition is a registry lookup rather than a hand-written trie, so
 * adding a keyword means adding one line to token.def. */
static Token scanIdentifier(Scanner *sc) {
	TOKEN_TYPE kw;
	size_t len;

	while (isAlpha(peek(sc)) || isDigit(peek(sc))) {
		advance(sc);
	}
	len = (size_t)(sc->current - sc->start);

	kw = registryMatchKeyword(sc->reg, sc->start, len);
	return makeToken(sc, (kw == TK__NONE) ? TK_IDENTIFIER : kw);
}

Token scanToken(Scanner *sc) {
	char c;
	char next;
	TOKEN_TYPE punct;
	size_t punct_len;

	skipWhitespace(sc);
	sc->start = sc->current;

	if (isAtEnd(sc)) {
		return makeToken(sc, TK_EOF);
	}

	c = peek(sc);

	if (isAlpha(c)) {
		return scanIdentifier(sc);
	}
	if (isDigit(c)) {
		return scanNumber(sc);
	}
	if (c == '"') {
		advance(sc);
		return scanString(sc);
	}

	next = peekNext(sc);
	punct = registryMatchPunct(sc->reg, c, next, &punct_len);
	if (punct != TK__NONE) {
		for (size_t i = 0; i < punct_len; i++) {
			advance(sc);
		}
		return makeToken(sc, punct);
	}

	advance(sc);
	return makeErrorToken(sc, "unexpected character");
}

Token *tokenizeAll(const char *source, Registry *reg, Arena *arena,
                   size_t *out_n, Token *out_err) {
	Scanner sc;
	Token *tokens = NULL;
	size_t n = 0;
	size_t cap = 0;

	initScanner(&sc, source, reg);

	/* One pass: doubling the array as it fills costs a few copies and halves
	 * the lexing compared to scanning once to count and again to fill. */
	while (true) {
		Token token = scanToken(&sc);

		if (token.type == TK_ERROR) {
			if (out_err) {
				*out_err = token;
			}
			return NULL;
		}

		if (n == cap) {
			size_t new_cap = (cap == 0) ? 256 : cap * 2;
			tokens = checkAlloc(arena_grow(arena, tokens, cap * sizeof(Token),
			                              new_cap * sizeof(Token), _Alignof(Token)));
			cap = new_cap;
		}
		tokens[n++] = token;

		if (token.type == TK_EOF) {
			break;
		}
	}

	*out_n = n;
	return tokens;
}

void printToken(Registry *reg, const Token *token) {
	printf("LINE: %6zu TYPE: %16s - \"%.*s\"\n",
	       token->line,
	       tokenName(reg, token->type),
	       (int)token->len, token->start);
}
