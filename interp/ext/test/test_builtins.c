/* test_builtins.c - locks in the interpreter core's behavior: grammar
 * parsing, the native-function hook, the column store, and the
 * col/sum/gpu_sum/filter_gt/emit builtins, driven through the exact same
 * entry points (wc_init/wc_load_column_f64/wc_run) the real WASM build
 * exposes. See emscripten.h (this directory's shim) for what this does
 * and doesn't cover.
 *
 * No test framework - a `CHECK` macro that counts and reports, same
 * spirit as repl2's own minimal style. Exit code is 0 iff everything
 * passed, for `make test` / CI use.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
	do { \
		if (cond) { \
			g_pass++; \
		} else { \
			g_fail++; \
			printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		} \
	} while (0)

#define CHECK_DBL_EQ(a, b, msg) CHECK(fabs((a) - (b)) < 1e-9, msg)

extern int wc_init(const char *grammar_path);
extern int wc_load_column_f64(const char *name, double *values, unsigned int len);
extern int wc_run(const char *source);

/* js_stubs.c */
extern int g_wc_gpu_available;
extern int g_gpu_path_taken;
extern double g_emitted[64];
extern int g_n_emitted;

static void resetEmitted(void) {
	g_n_emitted = 0;
	g_gpu_path_taken = 0;
}

static void test_init_and_basic_query(void) {
	resetEmitted();
	int rc = wc_init("../../../resources/grammar-csv.txt");
	CHECK(rc == 0, "wc_init succeeds with the default grammar");

	double amounts[] = {10, 250, 40, 999, 5, 120, 300.5};
	int lrc = wc_load_column_f64("amount", amounts, 7);
	CHECK(lrc == 0, "wc_load_column_f64 succeeds");

	int rrc = wc_run(
		"let big := filter_gt(col(\"amount\"), 100);\n"
		"emit(sum(big));\n"
		"emit(sum(col(\"amount\")));\n"
	);
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK(g_n_emitted == 2, "emit() was called twice");
	/* Same values a real browser run returned against this exact data -
	 * see README's "Sample data" section. */
	CHECK_DBL_EQ(g_emitted[0], 1669.5, "filter_gt(>100) then sum matches the hand-checked value");
	CHECK_DBL_EQ(g_emitted[1], 1724.5, "sum of the full column matches the hand-checked value");
}

static void test_sum_on_missing_column_is_a_runtime_error(void) {
	resetEmitted();
	/* col() on an unknown name returns nil (doCol always "handles" the
	 * call); sum() on a non-column argument declines to handle it
	 * (objAsColumn returns NULL), so the interpreter falls through to
	 * "undefined function 'sum'" rather than a clearer "sum() needs a
	 * column" message. That's the actual current behavior - a bit of a
	 * rough edge worth knowing about, not something this test suite
	 * silently papers over. */
	int rrc = wc_run("emit(sum(col(\"does-not-exist\")));");
	CHECK(rrc == -3, "sum() on a missing column surfaces as a runtime error, not a silent 0/NaN");
}

static void test_filter_gt_is_independent_of_its_source(void) {
	resetEmitted();
	double vals[] = {1, 2, 3, 4, 5};
	wc_load_column_f64("small", vals, 5);

	int rrc = wc_run(
		"let f := filter_gt(col(\"small\"), 3);\n"
		"emit(sum(f));\n"
		"emit(sum(col(\"small\")));\n"
	);
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK_DBL_EQ(g_emitted[0], 9.0, "filter_gt keeps only values > 3 (4 + 5 = 9)");
	CHECK_DBL_EQ(g_emitted[1], 15.0, "the source column is unmodified by filter_gt (1+2+3+4+5 = 15)");
}

static void test_session_persists_across_runs(void) {
	resetEmitted();
	int r1 = wc_run("let x := 5;");
	CHECK(r1 == 0, "first wc_run (a bare declaration) succeeds");

	int r2 = wc_run("emit(x + 1);");
	CHECK(r2 == 0, "second wc_run succeeds");
	CHECK_DBL_EQ(g_emitted[0], 6.0, "variables persist across wc_run calls (REPL-style session, not one-shot)");
}

static void test_gpu_sum_falls_back_below_threshold(void) {
	resetEmitted();
	g_wc_gpu_available = 1; /* even with the GPU "available"... */
	double vals[] = {1, 2, 3};
	wc_load_column_f64("tiny", vals, 3);

	int rrc = wc_run("emit(gpu_sum(col(\"tiny\")));");
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK(g_gpu_path_taken == 0, "...gpu_sum still stays on the CPU path below WC_GPU_MIN_LEN");
	CHECK_DBL_EQ(g_emitted[0], 6.0, "and the CPU-path result is correct");

	g_wc_gpu_available = 0;
}

static void test_gpu_sum_takes_gpu_path_above_threshold(void) {
	resetEmitted();
	g_wc_gpu_available = 1;

	static double big[60000];
	for (int i = 0; i < 60000; i++) {
		big[i] = 1.0;
	}
	wc_load_column_f64("huge", big, 60000);

	int rrc = wc_run("emit(gpu_sum(col(\"huge\")));");
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK(g_gpu_path_taken == 1, "gpu_sum takes the GPU branch once eligible (len >= threshold, f64, GPU available)");
	CHECK_DBL_EQ(g_emitted[0], 60000.0, "the GPU-path result is correct");

	g_wc_gpu_available = 0;
}

static void test_reload_column_replaces_not_duplicates(void) {
	resetEmitted();
	double v1[] = {1, 1, 1};
	double v2[] = {10, 10};
	wc_load_column_f64("reload", v1, 3);
	wc_load_column_f64("reload", v2, 2); /* same name - should replace, not stack */

	int rrc = wc_run("emit(sum(col(\"reload\")));");
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK_DBL_EQ(g_emitted[0], 20.0, "reloading a column by name replaces it rather than accumulating both loads");
}

int main(void) {
	test_init_and_basic_query();
	test_sum_on_missing_column_is_a_runtime_error();
	test_filter_gt_is_independent_of_its_source();
	test_session_persists_across_runs();
	test_gpu_sum_falls_back_below_threshold();
	test_gpu_sum_takes_gpu_path_above_threshold();
	test_reload_column_replaces_not_duplicates();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
