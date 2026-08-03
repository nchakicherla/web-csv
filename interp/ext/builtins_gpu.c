#include "builtins_gpu.h"
#include "column.h"
#include "store.h"

#include <math.h>
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
 * Requires exactly 3 args and a numeric column even for "count", which
 * only needs categorical_col - simpler and more consistent than a
 * variable-arity signature, at the cost of the caller passing an
 * otherwise-unused numeric column just to count rows per category. */
static bool doGroupby(Object *args, size_t n_args, Object *out) {
	Column *cat, *num;
	const char *agg;
	uint32_t n_groups, len, i, g;
	int32_t *codes;
	double *values;
	double *sums, *mins, *maxs, *result;
	uint32_t *counts;
	char *labels_joined;

	if (n_args != 3) {
		return false;
	}
	cat = objAsColumn(args[0]);
	num = objAsColumn(args[1]);
	if (!cat || columnType(cat) != COL_STR_DICT) {
		return false;
	}
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

	len = columnLen(cat);
	if (columnLen(num) != len) {
		return false; /* mismatched column lengths */
	}

	n_groups = columnDictLen(cat);
	codes = columnDataI32(cat);
	values = columnDataF64(num);

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
		sums[code] += values[i];
		counts[code]++;
		if (values[i] < mins[code]) {
			mins[code] = values[i];
		}
		if (values[i] > maxs[code]) {
			maxs[code] = values[i];
		}
	}

	for (g = 0; g < n_groups; g++) {
		if (strcmp(agg, "sum") == 0) {
			result[g] = sums[g];
		} else if (strcmp(agg, "count") == 0) {
			result[g] = (double)counts[g];
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

/* emit(x) - the PoC's only channel back to JS for a value the UI should
 * show. A column argument is summarized (summed) rather than streamed back
 * whole; handing a full result column's buffer to JS for charting is a
 * real next step (expose its pointer/length the way wc_load_column_f64
 * ingests one), just not built here yet - see README's "Known gaps". */
static bool doEmit(Object *args, size_t n_args, Object *out) {
	if (n_args != 1) {
		return false;
	}
	if (objIsNumber(args[0])) {
		wcEmitNumber(objAsDouble(args[0]));
	} else {
		Column *col = objAsColumn(args[0]);
		if (col && columnType(col) == COL_F64) {
			wcEmitNumber(cpuSum(col));
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
