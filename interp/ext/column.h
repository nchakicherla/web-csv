#ifndef WC_COLUMN_H
#define WC_COLUMN_H

/* column.h - the columnar value store shared by the interpreter, the CPU
 * builtins, and the WebGPU bridge.
 *
 * A CSV column becomes one typed, contiguous buffer (Float64Array-shaped for
 * numbers, dictionary-encoded int32 codes for strings/categoricals) so the
 * same bytes can be read by a plain C loop, uploaded to a GPUBuffer, or
 * handed back to JS - no per-row boxing, no copying between "the
 * interpreter's view" and "the GPU's view".
 *
 * Columns are allocated with malloc/free, not the interpreter's arena:
 * their lifetime is "until the CSV is unloaded or this result is replaced",
 * which doesn't line up with any one script run's arena, and GPU buffers
 * uploaded from a column's pointer need that pointer to stay valid (not get
 * bulk-freed) independent of interpreter/arena teardown.
 *
 * Interpreter values that wrap a Column* carry a magic tag as the struct's
 * first member so objAsColumn() can refuse to dereference a PTR_TYPE object
 * that happens to point somewhere else, rather than trusting the tag blindly.
 */

#include "common.h"
#include "object.h"

typedef enum {
	COL_F64,
	COL_I32,
	COL_STR_DICT, /* per-row int32 codes into `dict` */
} ColumnType;

typedef struct s_Column {
	uint32_t magic; /* WC_COLUMN_MAGIC; guards objAsColumn() */
	ColumnType type;
	uint32_t len;
	char *name; /* malloc'd, owned */

	union {
		double *f64;
		int32_t *i32; /* COL_STR_DICT: per-row dictionary codes */
	} data;

	/* COL_STR_DICT only */
	char **dict; /* malloc'd array of malloc'd strings */
	uint32_t dict_len;
} Column;

Column *columnCreateF64(const char *name, const double *values, uint32_t len);
Column *columnCreateI32(const char *name, const int32_t *values, uint32_t len);

/* Takes ownership of `codes` (must be malloc'd, len entries) and `dict`
 * (must be malloc'd, dict_len malloc'd strings) - both freed by
 * columnFree(). Callers that don't already have malloc'd buffers should
 * build them fresh rather than pass borrowed/stack memory. */
Column *columnCreateStrDictOwned(const char *name, int32_t *codes, uint32_t len,
                                 char **dict, uint32_t dict_len);

/* Builds the dictionary itself: scans `values` (len borrowed strings,
 * copied - ownership stays with the caller), assigning each distinct
 * value the next free code in first-seen order. O(len * distinct_values)
 * - a linear scan against the dict-so-far rather than a hash table, which
 * is fine for a CSV's worth of categories (tens to low hundreds) and not
 * for high-cardinality columns; a hash table is a reasonable upgrade if
 * that changes, not needed to be correct today. */
Column *columnCreateStrDict(const char *name, const char *const *values, uint32_t len);

void columnFree(Column *col);

uint32_t columnLen(const Column *col);
ColumnType columnType(const Column *col);
const char *columnName(const Column *col);

/* COL_STR_DICT only - 0 for any other type. */
uint32_t columnDictLen(const Column *col);

/* COL_STR_DICT only - the column's dictionary entries joined with '\x1f'
 * (ASCII Unit Separator - ordinary CSV/category text essentially never
 * contains it, so no escaping scheme is needed for the delimiter itself).
 * Malloc'd, caller frees. NULL for any other column type. */
char *columnDictJoined(const Column *col);

/* NULL if `col` is not that type - callers should check columnType() first;
 * these are for the JS/EMSCRIPTEN_KEEPALIVE accessors and the CPU builtins,
 * which already know what they asked for. */
double *columnDataF64(Column *col);
int32_t *columnDataI32(Column *col);

/* Wraps/unwraps a Column* as an interpreter Object (PTR_TYPE + magic tag).
 * objAsColumn returns NULL for a non-column PTR_TYPE, or any other Object
 * type - never dereferences an untrusted pointer. */
Object objColumn(Column *col);
Column *objAsColumn(Object o);

#endif // WC_COLUMN_H
