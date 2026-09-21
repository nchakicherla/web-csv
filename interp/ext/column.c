#include "column.h"

#include <stdlib.h>
#include <string.h>

#define WC_COLUMN_MAGIC 0x574c4331u /* "WLC1" */

static char *dupStr(const char *s) {
	size_t len = strlen(s);
	char *out = malloc(len + 1);
	if (!out) {
		return NULL;
	}
	memcpy(out, s, len + 1);
	return out;
}

static Column *allocColumn(const char *name, ColumnType type, uint32_t len) {
	Column *col = calloc(1, sizeof(Column));
	if (!col) {
		return NULL;
	}
	col->magic = WC_COLUMN_MAGIC;
	col->type = type;
	col->len = len;
	col->name = dupStr(name);
	return col;
}

Column *columnCreateF64(const char *name, const double *values, uint32_t len) {
	Column *col = allocColumn(name, COL_F64, len);
	if (!col) {
		return NULL;
	}
	col->data.f64 = malloc(sizeof(double) * (len ? len : 1));
	if (!col->data.f64) {
		free(col->name);
		free(col);
		return NULL;
	}
	if (len) {
		memcpy(col->data.f64, values, sizeof(double) * len);
	}
	return col;
}

/* Same physical layout as columnCreateF64 (epoch seconds, f64) - COL_DATE
 * exists as a distinct tag so builtins can tell "a number" from "a date
 * that happens to be stored as a number" (date_part() requires it,
 * doSum() refuses it, doFilterGt() accepts both). See datetime.h for what
 * the values mean. */
Column *columnCreateDate(const char *name, const double *values, uint32_t len) {
	Column *col = allocColumn(name, COL_DATE, len);
	if (!col) {
		return NULL;
	}
	col->data.f64 = malloc(sizeof(double) * (len ? len : 1));
	if (!col->data.f64) {
		free(col->name);
		free(col);
		return NULL;
	}
	if (len) {
		memcpy(col->data.f64, values, sizeof(double) * len);
	}
	return col;
}

Column *columnCreateI32(const char *name, const int32_t *values, uint32_t len) {
	Column *col = allocColumn(name, COL_I32, len);
	if (!col) {
		return NULL;
	}
	col->data.i32 = malloc(sizeof(int32_t) * (len ? len : 1));
	if (!col->data.i32) {
		free(col->name);
		free(col);
		return NULL;
	}
	if (len) {
		memcpy(col->data.i32, values, sizeof(int32_t) * len);
	}
	return col;
}

Column *columnCreateStrDictOwned(const char *name, int32_t *codes, uint32_t len,
                                 char **dict, uint32_t dict_len) {
	Column *col = allocColumn(name, COL_STR_DICT, len);
	if (!col) {
		return NULL;
	}
	col->data.i32 = codes;
	col->dict = dict;
	col->dict_len = dict_len;
	return col;
}

Column *columnCreateStrDict(const char *name, const char *const *values, uint32_t len) {
	int32_t *codes = malloc(sizeof(int32_t) * (len ? len : 1));
	char **dict = NULL;
	uint32_t dict_cap = 0, dict_len = 0;
	uint32_t i, d;
	Column *col;

	if (!codes) {
		return NULL;
	}

	for (i = 0; i < len; i++) {
		int32_t code = -1;

		for (d = 0; d < dict_len; d++) {
			if (0 == strcmp(dict[d], values[i])) {
				code = (int32_t)d;
				break;
			}
		}

		if (code < 0) {
			if (dict_len == dict_cap) {
				uint32_t new_cap = dict_cap ? dict_cap * 2 : 8;
				char **grown = realloc(dict, new_cap * sizeof(char *));
				if (!grown) {
					for (d = 0; d < dict_len; d++) {
						free(dict[d]);
					}
					free(dict);
					free(codes);
					return NULL;
				}
				dict = grown;
				dict_cap = new_cap;
			}
			dict[dict_len] = dupStr(values[i]);
			code = (int32_t)dict_len;
			dict_len++;
		}

		codes[i] = code;
	}

	col = columnCreateStrDictOwned(name, codes, len, dict, dict_len);
	if (!col) {
		for (d = 0; d < dict_len; d++) {
			free(dict[d]);
		}
		free(dict);
		free(codes);
	}
	return col;
}

/* Sort key for numeric dedupe: value, then original row index, so the
 * first entry of every run of equal values is that value's first-seen
 * row. NaN sorts last and equals itself (== would say NaN != NaN and
 * make qsort's ordering inconsistent). */
typedef struct {
	double v;
	uint32_t idx;
} UniqueKey;

static int cmpUniqueKey(const void *pa, const void *pb) {
	const UniqueKey *a = pa, *b = pb;
	int a_nan = a->v != a->v, b_nan = b->v != b->v;

	if (a_nan || b_nan) {
		if (a_nan != b_nan) {
			return a_nan ? 1 : -1;
		}
	} else if (a->v != b->v) {
		return a->v < b->v ? -1 : 1;
	}
	return (a->idx > b->idx) - (a->idx < b->idx);
}

static int uniqueKeysEqual(double a, double b) {
	return a == b || (a != a && b != b);
}

static Column *uniqueStrDict(const Column *col) {
	uint32_t i, n = 0;
	int32_t *remap = malloc(sizeof(int32_t) * (col->dict_len ? col->dict_len : 1));
	int32_t *codes = malloc(sizeof(int32_t) * (col->dict_len ? col->dict_len : 1));
	char **dict = malloc(sizeof(char *) * (col->dict_len ? col->dict_len : 1));
	Column *out = NULL;

	if (!remap || !codes || !dict) {
		goto done;
	}
	for (i = 0; i < col->dict_len; i++) {
		remap[i] = -1;
	}
	/* First-seen order by row, not by dictionary index: the dictionary
	 * can hold entries no row references, and its order needn't match
	 * row order for a column built by columnCreateStrDictOwned. */
	for (i = 0; i < col->len; i++) {
		int32_t code = col->data.i32[i];
		if (code < 0 || (uint32_t)code >= col->dict_len || remap[code] >= 0) {
			continue;
		}
		dict[n] = dupStr(col->dict[code]);
		if (!dict[n]) {
			while (n > 0) {
				free(dict[--n]);
			}
			goto done;
		}
		remap[code] = (int32_t)n;
		codes[n] = (int32_t)n;
		n++;
	}
	out = columnCreateStrDictOwned(col->name, codes, n, dict, n);
	if (out) {
		codes = NULL; /* owned by `out` now */
		dict = NULL;
	} else {
		while (n > 0) {
			free(dict[--n]);
		}
	}
done:
	free(remap);
	free(codes);
	free(dict);
	return out;
}

static Column *uniqueNumeric(const Column *col) {
	uint32_t i, n = 0, len = col->len;
	UniqueKey *keys = malloc(sizeof(UniqueKey) * (len ? len : 1));
	uint8_t *first = calloc(len ? len : 1, 1);
	double *vals = malloc(sizeof(double) * (len ? len : 1));
	Column *out = NULL;

	if (!keys || !first || !vals) {
		goto done;
	}
	for (i = 0; i < len; i++) {
		keys[i].v = col->data.f64[i];
		keys[i].idx = i;
	}
	qsort(keys, len, sizeof(UniqueKey), cmpUniqueKey);
	for (i = 0; i < len; i++) {
		if (i == 0 || !uniqueKeysEqual(keys[i].v, keys[i - 1].v)) {
			first[keys[i].idx] = 1;
		}
	}
	for (i = 0; i < len; i++) {
		if (first[i]) {
			vals[n++] = col->data.f64[i];
		}
	}
	out = col->type == COL_DATE ? columnCreateDate(col->name, vals, n) : columnCreateF64(col->name, vals, n);
done:
	free(keys);
	free(first);
	free(vals);
	return out;
}

Column *columnUnique(const Column *col) {
	switch (col->type) {
	case COL_STR_DICT:
		return uniqueStrDict(col);
	case COL_F64:
	case COL_DATE:
		return uniqueNumeric(col);
	default:
		return NULL;
	}
}

void columnFree(Column *col) {
	if (!col) {
		return;
	}
	if (col->type == COL_STR_DICT) {
		for (uint32_t i = 0; i < col->dict_len; i++) {
			free(col->dict[i]);
		}
		free(col->dict);
	}
	free(col->data.f64); /* union: same storage regardless of active member */
	free(col->name);
	col->magic = 0;
	free(col);
}

uint32_t columnLen(const Column *col) {
	return col->len;
}

ColumnType columnType(const Column *col) {
	return col->type;
}

const char *columnName(const Column *col) {
	return col->name;
}

uint32_t columnDictLen(const Column *col) {
	return col->type == COL_STR_DICT ? col->dict_len : 0;
}

/* Shared by columnDictJoined (n = dict_len distinct entries) and
 * columnResolveJoined (n = len per-row resolved values) - same
 * '\x1f'-join, different source array. */
static char *joinStrings(const char *const *strs, uint32_t n) {
	size_t total = 0;
	char *out, *p;
	uint32_t i;

	for (i = 0; i < n; i++) {
		total += strlen(strs[i]) + 1; /* +1 for the '\x1f' separator or trailing '\0' */
	}
	out = malloc(total ? total : 1);
	if (!out) {
		return NULL;
	}

	p = out;
	for (i = 0; i < n; i++) {
		size_t len = strlen(strs[i]);
		memcpy(p, strs[i], len);
		p += len;
		*p++ = (i + 1 < n) ? '\x1f' : '\0';
	}
	if (n == 0) {
		out[0] = '\0';
	}
	return out;
}

char *columnDictJoined(const Column *col) {
	if (col->type != COL_STR_DICT) {
		return NULL;
	}
	return joinStrings((const char *const *)col->dict, col->dict_len);
}

char *columnResolveJoined(const Column *col) {
	char **resolved;
	char *out;
	uint32_t i;

	if (col->type != COL_STR_DICT) {
		return NULL;
	}
	resolved = malloc(sizeof(char *) * (col->len ? col->len : 1));
	if (!resolved) {
		return NULL;
	}
	for (i = 0; i < col->len; i++) {
		int32_t code = col->data.i32[i];
		resolved[i] = (code >= 0 && (uint32_t)code < col->dict_len) ? col->dict[code] : "";
	}
	out = joinStrings((const char *const *)resolved, col->len);
	free(resolved);
	return out;
}

double *columnDataF64(Column *col) {
	return (col->type == COL_F64 || col->type == COL_DATE) ? col->data.f64 : NULL;
}

int32_t *columnDataI32(Column *col) {
	return (col->type == COL_I32 || col->type == COL_STR_DICT) ? col->data.i32 : NULL;
}

Object objColumn(Column *col) {
	return objPtr(col);
}

Column *objAsColumn(Object o) {
	Column *col;
	if (o.type != PTR_TYPE || !o.val.ptr) {
		return NULL;
	}
	/* Best-effort, not foolproof: this dereferences whatever the pointer is
	 * to check the tag, which is only safe because every PTR_TYPE Object
	 * that reaches interpreted code today was made by objColumn(). If
	 * native_fn ever wraps some other native pointer type as PTR_TYPE too,
	 * this needs a real tagged-union discriminant instead. */
	col = (Column *)o.val.ptr;
	return col->magic == WC_COLUMN_MAGIC ? col : NULL;
}
