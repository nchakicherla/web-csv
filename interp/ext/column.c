#include "column.h"

#include <stdlib.h>
#include <string.h>

#define WC_COLUMN_MAGIC 0x574c4331u /* "WLC1" */

static char *dupName(const char *name) {
	size_t len = strlen(name);
	char *out = malloc(len + 1);
	if (!out) {
		return NULL;
	}
	memcpy(out, name, len + 1);
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
	col->name = dupName(name);
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
