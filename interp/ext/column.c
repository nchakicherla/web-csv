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

char *columnDictJoined(const Column *col) {
	size_t total = 0;
	char *out, *p;
	uint32_t i;

	if (col->type != COL_STR_DICT) {
		return NULL;
	}
	for (i = 0; i < col->dict_len; i++) {
		total += strlen(col->dict[i]) + 1; /* +1 for the '\x1f' separator or trailing '\0' */
	}
	out = malloc(total ? total : 1);
	if (!out) {
		return NULL;
	}

	p = out;
	for (i = 0; i < col->dict_len; i++) {
		size_t len = strlen(col->dict[i]);
		memcpy(p, col->dict[i], len);
		p += len;
		*p++ = (i + 1 < col->dict_len) ? '\x1f' : '\0';
	}
	if (col->dict_len == 0) {
		out[0] = '\0';
	}
	return out;
}

double *columnDataF64(Column *col) {
	return col->type == COL_F64 ? col->data.f64 : NULL;
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
