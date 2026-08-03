#include "object.h"
#include "oom.h"

#include <stdio.h>
#include <string.h>

Object objNil(void) {
	Object o;
	o.type = NIL_TYPE;
	o.val.ptr = NULL;
	return o;
}

Object objI64(int64_t v) {
	Object o;
	o.type = I64_TYPE;
	o.val.i64 = v;
	return o;
}

Object objDbl(double v) {
	Object o;
	o.type = DBL_TYPE;
	o.val.dbl = v;
	return o;
}

Object objBln(bool v) {
	Object o;
	o.type = BLN_TYPE;
	o.val.bln = v;
	return o;
}

Object objStr(const char *s) {
	Object o;
	o.type = STR_TYPE;
	o.val.str = s;
	return o;
}

Object objPtr(void *p) {
	Object o;
	o.type = PTR_TYPE;
	o.val.ptr = p;
	return o;
}

bool objIsNumber(Object o) {
	return o.type == I64_TYPE || o.type == DBL_TYPE;
}

bool objIsTruthy(Object o) {
	switch (o.type) {
		case NIL_TYPE: return false;
		case BLN_TYPE: return o.val.bln;
		case I64_TYPE: return o.val.i64 != 0;
		case DBL_TYPE: return o.val.dbl != 0.0;
		case STR_TYPE: return o.val.str && o.val.str[0] != '\0';
		case PTR_TYPE: return o.val.ptr != NULL;
	}
	return false;
}

double objAsDouble(Object o) {
	switch (o.type) {
		case I64_TYPE: return (double)o.val.i64;
		case DBL_TYPE: return o.val.dbl;
		case BLN_TYPE: return o.val.bln ? 1.0 : 0.0;
		default:       return 0.0;
	}
}

const char *objTypeName(OBJ_TYPE type) {
	switch (type) {
		case NIL_TYPE: return "nil";
		case STR_TYPE: return "Str";
		case I64_TYPE: return "int";
		case DBL_TYPE: return "flt";
		case BLN_TYPE: return "bool";
		case PTR_TYPE: return "ptr";
	}
	return "?";
}

void objPrint(Object o) {
	switch (o.type) {
		case NIL_TYPE: printf("nil"); break;
		case STR_TYPE: printf("%s", o.val.str ? o.val.str : ""); break;
		case I64_TYPE: printf("%lld", (long long)o.val.i64); break;
		case DBL_TYPE: printf("%g", o.val.dbl); break;
		case BLN_TYPE: printf("%s", o.val.bln ? "true" : "false"); break;
		case PTR_TYPE: printf("<ptr %p>", o.val.ptr); break;
	}
}

const char *objInternChars(Arena *arena, const char *s, size_t len) {
	char *out = checkAlloc(arena_alloc(arena, len + 1, _Alignof(char)));
	memcpy(out, s, len);
	out[len] = '\0';
	return out;
}
