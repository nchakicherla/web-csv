#ifndef PARSER_H
#define PARSER_H

/* parser.h - the front end, start to finish.
 *
 *   initParser -> parserSetGrammar -> parserParseSource -> termParser
 *
 * Everything the parser allocates lives in its own arena, so termParser is the
 * only cleanup needed regardless of where things failed.
 */

#include "arena.h"
#include "common.h"
#include "registry.h"
#include "grammar.h"
#include "scanner.h"
#include "ast.h"
#include "ast_node.h"

typedef enum {
	PARSE_OK = 0,
	PARSE_NOT_INITIALIZED,
	PARSE_GRAMMAR_FAILED,
	PARSE_NO_GRAMMAR,
	PARSE_NO_SOURCE,
	PARSE_SCAN_FAILED,
	PARSE_FAILED,
} ParseStatus;

typedef struct s_Parser {
	Arena arena;
	Registry reg;
	Grammar grammar;

	Token *tokens;
	size_t n_tokens;
	TokenStream stream;

	SyntaxNode *ast;

	bool is_initialized;
	bool is_grammar_set;
} Parser;

void initParser(Parser *parser);
void termParser(Parser *parser);

ParseStatus parserSetGrammar(Parser *parser, const char *grammar_file);

/* Tokenizes and parses in one step; on success `parser->ast` is the tree. */
ParseStatus parserParseSource(Parser *parser, const char *source);

const char *parseStatusMessage(ParseStatus status);

#endif // PARSER_H
