#include "parser.h"

#include <stdio.h>

void initParser(Parser *parser) {
	arena_init(&parser->arena);
	initRegistry(&parser->reg, &parser->arena);

	parser->tokens = NULL;
	parser->n_tokens = 0;
	parser->stream.tk = NULL;
	parser->stream.n = 0;
	parser->stream.pos = 0;
	parser->stream.furthest = 0;
	parser->stream.depth = 0;
	parser->stream.depth_exceeded = false;
	parser->ast = NULL;

	parser->is_initialized = true;
	parser->is_grammar_set = false;
}

void termParser(Parser *parser) {
	arena_term(&parser->arena);
	parser->is_initialized = false;
	parser->is_grammar_set = false;
}

ParseStatus parserSetGrammar(Parser *parser, const char *grammar_file) {
	if (!parser->is_initialized) {
		return PARSE_NOT_INITIALIZED;
	}

	if (0 != loadGrammar(&parser->grammar, grammar_file, &parser->reg, &parser->arena)) {
		return PARSE_GRAMMAR_FAILED;
	}

	parser->is_grammar_set = true;
	return PARSE_OK;
}

ParseStatus parserParseSource(Parser *parser, const char *source) {
	Token bad_token;

	if (!parser->is_initialized) {
		return PARSE_NOT_INITIALIZED;
	}
	/* The grammar defines the token alphabet as well as the rules, so it has to
	 * be loaded before the source can even be scanned. */
	if (!parser->is_grammar_set) {
		return PARSE_NO_GRAMMAR;
	}
	if (!source) {
		return PARSE_NO_SOURCE;
	}

	parser->tokens = tokenizeAll(source, &parser->reg, &parser->arena,
	                             &parser->n_tokens, &bad_token);
	if (!parser->tokens) {
		fprintf(stderr, "%zu: scan error: %.*s\n",
		        bad_token.line, (int)bad_token.len, bad_token.start);
		return PARSE_SCAN_FAILED;
	}

	parser->stream.tk = parser->tokens;
	parser->stream.n = parser->n_tokens;
	parser->stream.pos = 0;
	parser->stream.furthest = 0;
	parser->stream.depth = 0;
	parser->stream.depth_exceeded = false;

	parser->ast = parseTokenStream(&parser->grammar, &parser->stream, &parser->arena);
	if (!parser->ast) {
		return PARSE_FAILED;
	}
	return PARSE_OK;
}

const char *parseStatusMessage(ParseStatus status) {
	switch (status) {
		case PARSE_OK:              return "ok";
		case PARSE_NOT_INITIALIZED: return "parser was not initialized";
		case PARSE_GRAMMAR_FAILED:  return "grammar could not be loaded";
		case PARSE_NO_GRAMMAR:      return "no grammar has been set";
		case PARSE_NO_SOURCE:       return "no source provided";
		case PARSE_SCAN_FAILED:     return "source could not be tokenized";
		case PARSE_FAILED:          return "source did not match the grammar";
	}
	return "unknown error";
}
