#include "builtins_gpu.h"
#include "column.h"
#include "store.h"

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
	if (len == 4 && 0 == memcmp(name, "emit", 4)) {
		return doEmit(args, n_args, out);
	}
	return false;
}
