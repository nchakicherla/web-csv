/* web_main.c - the Emscripten entry point. Replaces repl2's main.c/repl.c
 * (native CLI, terminal-driven) with a small set of exported functions a
 * browser session drives directly - see web/src/main.js.
 *
 * Session model matches repl2's own REPL (src/repl.c, not vendored here):
 * one Parser + one Interp, created once in wc_init() and reused across
 * every wc_run() call, so `let x := 5;` in one call is still visible to
 * `x + 1` in the next - a notebook, not a fresh process per query.
 */

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>

#include "parser.h"
#include "interp.h"
#include "builtins_gpu.h"
#include "store.h"

static Parser g_parser;
static Interp *g_interp = NULL;

/* `grammar_path` must already exist in Emscripten's virtual FS - either
 * bundled at link time (Makefile's --preload-file) or written there first
 * via Module.FS.writeFile() from JS, which is how the browser-side grammar
 * picker/editor swaps in a different DSL without a rebuild. */
EMSCRIPTEN_KEEPALIVE
int wc_init(const char *grammar_path) {
	initParser(&g_parser);
	storeInit();

	if (parserSetGrammar(&g_parser, grammar_path) != PARSE_OK) {
		return 1;
	}

	g_interp = interpCreate(&g_parser.reg, &g_parser.arena);
	interpSetNativeHook(g_interp, wcNativeDispatch, NULL);
	return 0;
}

/* Takes ownership of `values` (must be a pointer returned by Module._malloc,
 * WC_GPU_MIN_LEN doubles) - column.c copies it into its own buffer, so the
 * caller frees the original after this returns. Replaces any existing
 * column of the same name. */
EMSCRIPTEN_KEEPALIVE
int wc_load_column_f64(const char *name, double *values, uint32_t len) {
	Column *col = columnCreateF64(name, values, len);
	if (!col) {
		return 1;
	}
	storeSetNamed(name, col);
	return 0;
}

/* Runs one script entry against the persistent session Interp. Output
 * happens two ways: print(...) writes to stdout (captured via Module.print,
 * same as any other Emscripten console output), and emit(...) pushes
 * structured values onto Module.wcResults (see builtins_gpu.c) for the UI
 * to read after the call resolves.
 *
 * Exported as an ordinary function, not specially marked async - Asyncify
 * makes any exported function that transitively reaches gpu_sum()'s
 * EM_ASYNC_JS import awaitable automatically. JS must call it with
 * Module.ccall(..., {async: true}) (or a cwrap'd async wrapper) to get a
 * Promise back rather than blocking; see web/src/main.js.
 *
 * Returns 0 on a clean run, -1 if wc_init() hasn't succeeded yet, -2 on a
 * parse error, -3 on a runtime error - not the script's own exit code,
 * which repl2's REPL-style per-entry execution doesn't surface separately
 * from these anyway (see src/repl.c upstream for the same tradeoff). */
EMSCRIPTEN_KEEPALIVE
int wc_run(const char *source) {
	int exit_code = 0;
	ExecResult result;

	if (!g_interp) {
		return -1;
	}
	if (parserParseSource(&g_parser, source) != PARSE_OK) {
		return -2;
	}

	result = interpExecEcho(g_interp, g_parser.ast, &exit_code);
	return (result == EXEC_ERROR) ? -3 : 0;
}

EMSCRIPTEN_KEEPALIVE
void wc_reset_session(void) {
	storeTerm();
	storeInit();
	termParser(&g_parser);
	g_interp = NULL;
}
