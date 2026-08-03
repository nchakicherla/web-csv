#include "builtins_gpu.h"
#include "column.h"
#include "store.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

/* --- CPU reference path ---------------------------------------------------
 *
 * Also what gpu_sum() falls back to when WebGPU isn't available, or the
 * column is too small for GPU dispatch overhead to pay off - see
 * WC_GPU_MIN_LEN below and ARCHITECTURE.md's eligibility-gate note. */
static double cpuSum(Column *col) {
	uint32_t len = columnLen(col);
	double total = 0;

	if (columnType(col) == COL_F64) {
		double *d = columnDataF64(col);
		for (uint32_t i = 0; i < len; i++) {
			total += d[i];
		}
	} else if (columnType(col) == COL_I32) {
		int32_t *d = columnDataI32(col);
		for (uint32_t i = 0; i < len; i++) {
			total += d[i];
		}
	}
	return total;
}

/* Not tuned - a starting guess (buffer upload + pipeline dispatch + mapped
 * readback all cost real wall-clock time that a plain loop doesn't) to
 * revisit once there's a real benchmark. */
#define WC_GPU_MIN_LEN 50000u

EM_JS(int, wcGpuAvailable, (void), {
	return (typeof navigator !== 'undefined' && navigator.gpu) ? 1 : 0;
});

/* The actual async boundary. Submits a WebGPU compute pass that reduces
 * ptr[0..len) to a single sum and awaits the mapped readback - see
 * web/src/gpu/bridge.js's reduceSum(). Everything on the C call path down
 * to this import needs Asyncify instrumentation; see
 * interp/ext/Makefile's ASYNCIFY note. */
EM_ASYNC_JS(double, wcGpuReduceSum, (double *ptr, uint32_t len), {
	return await Module.gpuBridge.reduceSum(ptr, len);
});

/* gpu_sum_exact()'s async boundary - same shape as wcGpuReduceSum, but
 * against web/src/gpu/shaders/reduce_sum_i32.wgsl over pre-scaled i32
 * cents rather than raw f32 dollars. Integer addition doesn't round, so
 * this result is exact - see doGpuSumExact below for the scaling and the
 * overflow guard that makes it safe to call at all. */
EM_ASYNC_JS(double, wcGpuReduceSumExact, (int32_t *ptr, uint32_t len), {
	return await Module.gpuBridge.reduceSumExact(ptr, len);
});

/* groupby's output channel: `labels_joined` is `\x1f`-separated (see
 * column.h's columnDictJoined), `values` an n_groups-length f64 array in
 * the same dictionary order. Pushes a {type:'groups', ...} object onto
 * Module.wcResults, alongside whatever plain numbers emit() has pushed
 * there - the frontend distinguishes by shape. */
EM_JS(void, wcEmitGroups, (const char *labels_joined, double *values, uint32_t n_groups, const char *agg_name), {
	const labels = UTF8ToString(labels_joined).split('\x1f');
	const vals = [];
	for (let i = 0; i < n_groups; i++) {
		vals.push(HEAPF64[(values >> 3) + i]);
	}
	Module.wcResults = Module.wcResults || [];
	Module.wcResults.push({ type: 'groups', agg: UTF8ToString(agg_name), labels, values: vals });
});

/* emit()'s output channels for a whole column - see doEmit. Both push
 * {type:'column', dtype, values} onto Module.wcResults, mirroring
 * wcEmitGroups' shape so the frontend can tell result kinds apart by
 * `type` rather than by guessing from JS typeof. */
EM_JS(void, wcEmitNumberArray, (double *ptr, uint32_t len), {
	const values = [];
	for (let i = 0; i < len; i++) {
		values.push(HEAPF64[(ptr >> 3) + i]);
	}
	Module.wcResults = Module.wcResults || [];
	Module.wcResults.push({ type: 'column', dtype: 'f64', values });
});

EM_JS(void, wcEmitStringArray, (const char *values_joined, uint32_t len), {
	const values = len === 0 ? [] : UTF8ToString(values_joined).split('\x1f');
	Module.wcResults = Module.wcResults || [];
	Module.wcResults.push({ type: 'column', dtype: 'string', values });
});

static bool doSum(Object *args, size_t n_args, Object *out, bool allow_gpu) {
	Column *col;

	if (n_args != 1) {
		return false;
	}
	col = objAsColumn(args[0]);
	if (!col) {
		return false;
	}

	if (allow_gpu && columnType(col) == COL_F64 && columnLen(col) >= WC_GPU_MIN_LEN
	    && wcGpuAvailable()) {
		*out = objDbl(wcGpuReduceSum(columnDataF64(col), columnLen(col)));
	} else {
		*out = objDbl(cpuSum(col));
	}
	return true;
}

/* gpu_sum_exact(col) - like gpu_sum, but exact: no float rounding at any
 * point, at the cost of a documented assumption (values are money, at
 * most 2 meaningful decimal places) instead of gpu_sum's silent f32
 * precision loss on arbitrary magnitudes. See README's "A larger CSV"
 * section for the real drift this replaces and ARCHITECTURE.md §8 for
 * why WGSL's lack of f64 causes it in the first place.
 *
 * Each value is rounded to the nearest cent and converted to an integer
 * (`llround(value * 100)`); the whole column is then summed as i32 -
 * integer addition is exact and associative, so a GPU tree reduction and
 * a plain CPU loop are *guaranteed* to agree bit-for-bit, not just
 * approximately, unlike gpu_sum's f32 path. That CPU sum (as an int64,
 * so it can't itself overflow at any realistic scale) is computed as a
 * side effect of building the scaled array anyway - one pass over the
 * column is needed either way - so this never has to return an
 * approximate answer: below the GPU threshold, or when GPU access isn't
 * available or safe, it returns that already-correct CPU total directly
 * rather than attempting a dispatch just to get the same number back
 * slower.
 *
 * Overflow guard: a tree reduction's intermediate partial sums aren't
 * bounded by the final total when there's cancellation between positive
 * and negative values (refunds, say) - but they're always bounded by the
 * sum of *absolute* values, in any grouping or order. Checking that bound
 * fits i32 is therefore sufficient to guarantee no overflow anywhere in
 * the reduction, not just in the final answer. */
static bool doGpuSumExact(Object *args, size_t n_args, Object *out) {
	Column *col;
	uint32_t len, i;
	double *values;
	int32_t *cents;
	int64_t exact_cents = 0;
	int64_t abs_cents = 0;

	if (n_args != 1) {
		return false;
	}
	col = objAsColumn(args[0]);
	if (!col || columnType(col) != COL_F64) {
		return false;
	}

	len = columnLen(col);
	values = columnDataF64(col);

	cents = malloc(sizeof(int32_t) * (len ? len : 1));
	if (!cents) {
		return false;
	}

	for (i = 0; i < len; i++) {
		long long c = llround(values[i] * 100.0);
		/* Only trusted for GPU use once abs_cents is confirmed to fit i32
		 * below - the cast itself is implementation-defined, not
		 * undefined, when c is out of range, and that branch never reads
		 * this array; exact_cents (computed from the full-precision `c`,
		 * not this cast) is what gets returned instead. */
		cents[i] = (int32_t)c;
		exact_cents += c;
		abs_cents += (c < 0) ? -c : c;
	}

	if (abs_cents <= INT32_MAX && len >= WC_GPU_MIN_LEN && wcGpuAvailable()) {
		double gpu_cents = wcGpuReduceSumExact(cents, len);
		free(cents);
		*out = objDbl(gpu_cents / 100.0);
		return true;
	}

	free(cents);
	*out = objDbl((double)exact_cents / 100.0);
	return true;
}

static bool doCol(Object *args, size_t n_args, Object *out) {
	Column *col;

	if (n_args != 1 || args[0].type != STR_TYPE) {
		return false;
	}
	col = storeGetNamed(args[0].val.str);
	*out = col ? objColumn(col) : objNil();
	return true;
}

/* filter_gt(col, threshold) -> new column of col's values > threshold.
 * f64 columns only, and the predicate is fixed rather than a passed-in
 * comparator - keeps this PoC's builtin surface small; a real query
 * language would want filter(col, expr) with the grammar's own comparison
 * operators, not a native function per predicate shape. */
static bool doFilterGt(Object *args, size_t n_args, Object *out) {
	Column *col, *result;
	double threshold, *src, *dst;
	uint32_t len, kept = 0;

	if (n_args != 2) {
		return false;
	}
	col = objAsColumn(args[0]);
	if (!col || columnType(col) != COL_F64 || !objIsNumber(args[1])) {
		return false;
	}

	threshold = objAsDouble(args[1]);
	len = columnLen(col);
	src = columnDataF64(col);

	dst = malloc(sizeof(double) * (len ? len : 1));
	if (!dst) {
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (src[i] > threshold) {
			dst[kept++] = src[i];
		}
	}

	result = columnCreateF64(columnName(col), dst, kept);
	free(dst);
	if (!result) {
		return false;
	}
	storeTrack(result);
	*out = objColumn(result);
	return true;
}

/* groupby(categorical_col, numeric_col, agg) -> aggregates numeric_col's
 * values per group of categorical_col ("sum"/"count"/"avg"/"min"/"max"),
 * one result per distinct category, and emits it directly as a
 * {labels, values} pair rather than returning a value the script can keep
 * composing with - same "it's an output operation" role print()/emit()
 * already have, not a pure function. A dedicated result type/Column
 * variant that could carry labels alongside values would be the
 * alternative, but that's more machinery than a first pass needs; this
 * keeps every new concept (columns, the dictionary, emit) reused rather
 * than adding one more.
 *
 * groupby(categorical_col) (1 arg) is a count-only shorthand - the only
 * aggregate that doesn't need a numeric column at all, so it doesn't ask
 * for one. 2 args is deliberately unsupported (rejected outright, not
 * guessed at as "must mean count") since it's genuinely ambiguous
 * whether the second argument was meant to be the numeric column (with
 * agg implied) or the agg name (with the numeric column omitted) -
 * exactly 1 or exactly 3 args, nothing in between. */
static bool doGroupby(Object *args, size_t n_args, Object *out) {
	Column *cat, *num = NULL;
	const char *agg = "count";
	uint32_t n_groups, len, i, g;
	int32_t *codes;
	double *values = NULL;
	double *sums, *mins, *maxs, *result;
	uint32_t *counts;
	char *labels_joined;

	if (n_args != 1 && n_args != 3) {
		return false;
	}

	cat = objAsColumn(args[0]);
	if (!cat || columnType(cat) != COL_STR_DICT) {
		return false;
	}

	if (n_args == 3) {
		num = objAsColumn(args[1]);
		if (!num || columnType(num) != COL_F64) {
			return false;
		}
		if (args[2].type != STR_TYPE) {
			return false;
		}
		agg = args[2].val.str;
		if (strcmp(agg, "sum") != 0 && strcmp(agg, "count") != 0 && strcmp(agg, "avg") != 0
		    && strcmp(agg, "min") != 0 && strcmp(agg, "max") != 0) {
			return false; /* unrecognized aggregate name */
		}
		if (columnLen(num) != columnLen(cat)) {
			return false; /* mismatched column lengths */
		}
		values = columnDataF64(num);
	}

	len = columnLen(cat);
	n_groups = columnDictLen(cat);
	codes = columnDataI32(cat);

	sums = calloc(n_groups ? n_groups : 1, sizeof(double));
	counts = calloc(n_groups ? n_groups : 1, sizeof(uint32_t));
	mins = malloc(sizeof(double) * (n_groups ? n_groups : 1));
	maxs = malloc(sizeof(double) * (n_groups ? n_groups : 1));
	result = malloc(sizeof(double) * (n_groups ? n_groups : 1));
	if (!sums || !counts || !mins || !maxs || !result) {
		free(sums);
		free(counts);
		free(mins);
		free(maxs);
		free(result);
		return false;
	}
	for (g = 0; g < n_groups; g++) {
		mins[g] = HUGE_VAL;
		maxs[g] = -HUGE_VAL;
	}

	for (i = 0; i < len; i++) {
		int32_t code = codes[i];
		if (code < 0 || (uint32_t)code >= n_groups) {
			continue;
		}
		counts[code]++;
		if (values) {
			sums[code] += values[i];
			if (values[i] < mins[code]) {
				mins[code] = values[i];
			}
			if (values[i] > maxs[code]) {
				maxs[code] = values[i];
			}
		}
	}

	for (g = 0; g < n_groups; g++) {
		if (strcmp(agg, "count") == 0) {
			result[g] = (double)counts[g];
		} else if (strcmp(agg, "sum") == 0) {
			result[g] = sums[g];
		} else if (strcmp(agg, "avg") == 0) {
			result[g] = counts[g] ? sums[g] / counts[g] : 0.0;
		} else if (strcmp(agg, "min") == 0) {
			result[g] = counts[g] ? mins[g] : 0.0;
		} else {
			result[g] = counts[g] ? maxs[g] : 0.0; /* "max", the only remaining validated option */
		}
	}

	labels_joined = columnDictJoined(cat);
	if (labels_joined) {
		wcEmitGroups(labels_joined, result, n_groups, agg);
		free(labels_joined);
	}

	free(sums);
	free(counts);
	free(mins);
	free(maxs);
	free(result);

	*out = objNil();
	return true;
}

EM_JS(void, wcEmitNumber, (double v), {
	Module.wcResults = Module.wcResults || [];
	Module.wcResults.push(v);
});

/* emit(x) - the PoC's channel back to JS for a value the UI should show.
 * A numeric argument emits as a single number; a column argument streams
 * every value (COL_F64 as numbers, COL_STR_DICT resolved back to its
 * per-row strings) rather than being reduced to a summary - callers that
 * want a summary already have sum()/groupby() for that, explicitly:
 * emit(sum(col("amount"))) for a total, emit(col("amount")) for the whole
 * column. */
static bool doEmit(Object *args, size_t n_args, Object *out) {
	if (n_args != 1) {
		return false;
	}
	if (objIsNumber(args[0])) {
		wcEmitNumber(objAsDouble(args[0]));
	} else {
		Column *col = objAsColumn(args[0]);
		if (col && columnType(col) == COL_F64) {
			wcEmitNumberArray(columnDataF64(col), columnLen(col));
		} else if (col && columnType(col) == COL_STR_DICT) {
			char *joined = columnResolveJoined(col);
			if (joined) {
				wcEmitStringArray(joined, columnLen(col));
				free(joined);
			}
		}
	}
	*out = objNil();
	return true;
}

bool wcNativeDispatch(Interp *in, const char *name, size_t len, Object *args,
                      size_t n_args, Object *out, void *userdata) {
	(void)in;
	(void)userdata;

	if (len == 3 && 0 == memcmp(name, "col", 3)) {
		return doCol(args, n_args, out);
	}
	if (len == 3 && 0 == memcmp(name, "sum", 3)) {
		return doSum(args, n_args, out, false);
	}
	if (len == 7 && 0 == memcmp(name, "gpu_sum", 7)) {
		return doSum(args, n_args, out, true);
	}
	if (len == 13 && 0 == memcmp(name, "gpu_sum_exact", 13)) {
		return doGpuSumExact(args, n_args, out);
	}
	if (len == 9 && 0 == memcmp(name, "filter_gt", 9)) {
		return doFilterGt(args, n_args, out);
	}
	if (len == 7 && 0 == memcmp(name, "groupby", 7)) {
		return doGroupby(args, n_args, out);
	}
	if (len == 4 && 0 == memcmp(name, "emit", 4)) {
		return doEmit(args, n_args, out);
	}
	return false;
}
