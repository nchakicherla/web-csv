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
#include <stdint.h>
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
extern int wc_load_column_str_dict(const char *name, const char *values_joined, uint32_t n_values);
extern int wc_run(const char *source);

/* js_stubs.c */
extern int g_wc_gpu_available;
extern int g_gpu_path_taken;
extern double g_emitted[64];
extern int g_n_emitted;
extern char g_group_labels_joined[256];
extern double g_group_values[64];
extern uint32_t g_group_n;
extern char g_group_agg[16];

static void resetEmitted(void) {
	g_n_emitted = 0;
	g_gpu_path_taken = 0;
	g_group_n = 0;
	g_group_labels_joined[0] = '\0';
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

static void test_str_dict_column_loads_and_dedupes(void) {
	resetEmitted();
	/* "\x1f"-joined, matching what wc_load_column_str_dict expects - see
	 * web_main.c and column.c's columnCreateStrDict. */
	const char *categories = "a" "\x1f" "b" "\x1f" "a" "\x1f" "c" "\x1f" "b" "\x1f" "a";
	int lrc = wc_load_column_str_dict("category", categories, 6);
	CHECK(lrc == 0, "wc_load_column_str_dict succeeds");

	/* No direct way to inspect a column's contents from script (no
	 * "count distinct" builtin yet) - groupby's own test below is the
	 * real exercise of this data; this just confirms the load itself
	 * doesn't error and the column is retrievable by name. */
	int rrc = wc_run("let c := col(\"category\");\n");
	CHECK(rrc == 0, "col(\"category\") resolves after loading");
}

static void test_groupby_aggregates_correctly(void) {
	resetEmitted();
	const char *categories = "a" "\x1f" "b" "\x1f" "a" "\x1f" "c" "\x1f" "b" "\x1f" "a";
	double amounts[] = {10, 20, 30, 40, 50, 60};
	/* a: rows 0,2,5 -> 10,30,60 (sum 100, count 3, avg 33.33, min 10, max 60)
	 * b: rows 1,4   -> 20,50    (sum 70,  count 2, avg 35,    min 20, max 50)
	 * c: row 3      -> 40       (sum 40,  count 1, avg 40,    min 40, max 40) */
	wc_load_column_str_dict("category", categories, 6);
	wc_load_column_f64("amount", amounts, 6);

	int rrc = wc_run("groupby(col(\"category\"), col(\"amount\"), \"sum\");");
	CHECK(rrc == 0, "wc_run succeeds");
	CHECK(g_group_n == 3, "3 distinct groups (a, b, c)");
	CHECK(0 == strcmp(g_group_labels_joined, "a" "\x1f" "b" "\x1f" "c"), "group labels are in first-seen dictionary order");
	CHECK(0 == strcmp(g_group_agg, "sum"), "the aggregate name round-trips");
	CHECK_DBL_EQ(g_group_values[0], 100.0, "group 'a' sum is correct");
	CHECK_DBL_EQ(g_group_values[1], 70.0, "group 'b' sum is correct");
	CHECK_DBL_EQ(g_group_values[2], 40.0, "group 'c' sum is correct");

	resetEmitted();
	wc_run("groupby(col(\"category\"), col(\"amount\"), \"count\");");
	CHECK_DBL_EQ(g_group_values[0], 3.0, "group 'a' count is correct");
	CHECK_DBL_EQ(g_group_values[1], 2.0, "group 'b' count is correct");
	CHECK_DBL_EQ(g_group_values[2], 1.0, "group 'c' count is correct");

	resetEmitted();
	wc_run("groupby(col(\"category\"), col(\"amount\"), \"avg\");");
	CHECK_DBL_EQ(g_group_values[0], 100.0 / 3.0, "group 'a' avg is correct");
	CHECK_DBL_EQ(g_group_values[1], 35.0, "group 'b' avg is correct");

	resetEmitted();
	wc_run("groupby(col(\"category\"), col(\"amount\"), \"min\");");
	CHECK_DBL_EQ(g_group_values[0], 10.0, "group 'a' min is correct");
	CHECK_DBL_EQ(g_group_values[1], 20.0, "group 'b' min is correct");

	resetEmitted();
	wc_run("groupby(col(\"category\"), col(\"amount\"), \"max\");");
	CHECK_DBL_EQ(g_group_values[0], 60.0, "group 'a' max is correct");
	CHECK_DBL_EQ(g_group_values[1], 50.0, "group 'b' max is correct");
}

static void test_groupby_rejects_bad_input(void) {
	resetEmitted();
	const char *categories = "a" "\x1f" "b";
	double amounts[] = {1, 2};
	wc_load_column_str_dict("cat2", categories, 2);
	wc_load_column_f64("amt2", amounts, 2);

	int rrc = wc_run("groupby(col(\"cat2\"), col(\"amt2\"), \"median\");");
	CHECK(rrc == -3, "an unrecognized aggregate name is a runtime error, not a silent no-op");
	CHECK(g_group_n == 0, "...and nothing was emitted");
}

int main(void) {
	test_init_and_basic_query();
	test_sum_on_missing_column_is_a_runtime_error();
	test_filter_gt_is_independent_of_its_source();
	test_session_persists_across_runs();
	test_gpu_sum_falls_back_below_threshold();
	test_gpu_sum_takes_gpu_path_above_threshold();
	test_reload_column_replaces_not_duplicates();
	test_str_dict_column_loads_and_dedupes();
	test_groupby_aggregates_correctly();
	test_groupby_rejects_bad_input();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
