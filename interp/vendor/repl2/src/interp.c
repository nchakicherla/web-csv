#include "interp.h"
#include "env.h"
#include "object.h"
#include "oom.h"
#include "syntax_types.h"
#include "token_types.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CALL_DEPTH 256

typedef enum {
	FLOW_NORMAL,
	FLOW_BREAK,
	FLOW_RETURN,
	FLOW_EXIT,
	FLOW_ERROR,
} Flow;

typedef struct s_FnEntry {
	const char *name;
	size_t len;
	const SyntaxNode *node;
} FnEntry;

struct s_Interp {
	Registry *reg;
	Arena *arena;

	Env env;

	FnEntry *fns;
	size_t n_fns;
	size_t cap_fns;

	Object ret;
	int exit_code;
	unsigned depth;

	/* web-csv local addition (see vendor/VENDORED.md) */
	NativeFn native_fn;
	void *native_userdata;
};

static Flow execNode(Interp *in, const SyntaxNode *node);
static Flow evalNode(Interp *in, const SyntaxNode *node, Object *out);

/* --- tree navigation ------------------------------------------------------
 *
 * Rule bodies flatten into a node's child list mixed with the punctuation that
 * matched, so almost everything here is "the k-th child that isn't a token".
 * These are linear scans, but the lists are a handful of entries long and this
 * avoids allocating a working array on every loop iteration. */

static size_t namedCount(const SyntaxNode *node) {
	size_t count = 0;
	for (size_t i = 0; i < node->n_children; i++) {
		if (!node->children[i]->is_token) {
			count++;
		}
	}
	return count;
}

static const SyntaxNode *namedAt(const SyntaxNode *node, size_t k) {
	for (size_t i = 0; i < node->n_children; i++) {
		if (node->children[i]->is_token) {
			continue;
		}
		if (k == 0) {
			return node->children[i];
		}
		k--;
	}
	return NULL;
}

/* Locating children by role rather than by index keeps the interpreter working
 * when a grammar file reorders a rule - which is the entire point of making the
 * grammar a data file. */
static const SyntaxNode *namedOfType(const SyntaxNode *node, SYNTAX_TYPE type) {
	for (size_t i = 0; i < node->n_children; i++) {
		if (!node->children[i]->is_token && node->children[i]->type == type) {
			return node->children[i];
		}
	}
	return NULL;
}

static const Token *firstTokenChild(const SyntaxNode *node) {
	for (size_t i = 0; i < node->n_children; i++) {
		if (node->children[i]->is_token) {
			return &node->children[i]->token;
		}
	}
	return NULL;
}

/* Line of the first terminal anywhere under `node`, for error messages. */
static size_t nodeLine(const SyntaxNode *node) {
	if (node->is_token) {
		return node->token.line;
	}
	for (size_t i = 0; i < node->n_children; i++) {
		size_t line = nodeLine(node->children[i]);
		if (line) {
			return line;
		}
	}
	return 0;
}

static Flow rtError(Interp *in, const SyntaxNode *node, const char *fmt, ...) {
	va_list args;
	(void)in;
	fprintf(stderr, "%zu: runtime error: ", node ? nodeLine(node) : 0);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	return FLOW_ERROR;
}

/* --- functions --------------------------------------------------------- */

static void defineFunction(Interp *in, const char *name, size_t len,
                           const SyntaxNode *node) {
	/* Redefinition replaces in place. Definitions are hoisted and then executed
	 * again in statement order, and a definition inside a loop would otherwise
	 * append an entry per iteration. */
	for (size_t i = 0; i < in->n_fns; i++) {
		if (in->fns[i].len == len && 0 == memcmp(in->fns[i].name, name, len)) {
			in->fns[i].node = node;
			return;
		}
	}
	if (in->n_fns == in->cap_fns) {
		size_t new_cap = (in->cap_fns == 0) ? 16 : in->cap_fns * 2;
		in->fns = checkAlloc(arena_grow(in->arena, in->fns, in->cap_fns * sizeof(FnEntry),
		                               new_cap * sizeof(FnEntry), _Alignof(FnEntry)));
		in->cap_fns = new_cap;
	}
	in->fns[in->n_fns].name = name;
	in->fns[in->n_fns].len = len;
	in->fns[in->n_fns].node = node;
	in->n_fns++;
}

static const SyntaxNode *lookupFunction(Interp *in, const char *name, size_t len) {
	for (size_t i = in->n_fns; i > 0; i--) {
		if (in->fns[i - 1].len == len && 0 == memcmp(in->fns[i - 1].name, name, len)) {
			return in->fns[i - 1].node;
		}
	}
	return NULL;
}

/* --- literals ------------------------------------------------------------- */

static Object numberFromToken(const Token *t) {
	char buf[64];
	size_t len = (t->len < sizeof(buf) - 1) ? t->len : sizeof(buf) - 1;
	bool is_real = false;

	memcpy(buf, t->start, len);
	buf[len] = '\0';

	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '.' || buf[i] == 'e') {
			is_real = true;
			break;
		}
	}
	return is_real ? objDbl(strtod(buf, NULL))
	               : objI64((int64_t)strtoll(buf, NULL, 10));
}

/* The scanner keeps the surrounding quotes in a TK_CHARS token. */
static Object stringFromToken(Interp *in, const Token *t) {
	if (t->len < 2) {
		return objStr("");
	}
	return objStr(objInternChars(in->arena, t->start + 1, t->len - 2));
}

/* --- operators ------------------------------------------------------------ */

/* STX_ARITHOP / STX_BOOLOP / STX_ASSIGNOP are one-child wrappers around the
 * concrete operator rule. */
static SYNTAX_TYPE unwrapOp(const SyntaxNode *node) {
	if (node->type == STX_ARITHOP || node->type == STX_BOOLOP ||
	    node->type == STX_ASSIGNOP) {
		if (node->n_children > 0) {
			return node->children[0]->type;
		}
	}
	return node->type;
}

/* The grammar file cannot express precedence - an expression parses into a flat
 * operand/operator list - so the interpreter imposes it here. */
static int opPrec(SYNTAX_TYPE op) {
	switch (op) {
		case STX_OR:            return 1;
		case STX_AND:           return 2;
		case STX_GREATER:
		case STX_LESS:
		case STX_EQUAL_EQUAL:
		case STX_NOT_EQUAL:
		case STX_GREATER_EQUAL:
		case STX_LESS_EQUAL:    return 3;
		case STX_SUM:
		case STX_DIFF:          return 4;
		case STX_MULT:
		case STX_DIV:
		case STX_MOD:           return 5;
		default:                return 0;
	}
}

static bool bothInts(Object a, Object b) {
	return a.type == I64_TYPE && b.type == I64_TYPE;
}

static Flow applyBinary(Interp *in, const SyntaxNode *at, SYNTAX_TYPE op,
                        Object lhs, Object rhs, Object *out) {
	/* String concatenation is the one non-numeric binary operation. */
	if (op == STX_SUM && lhs.type == STR_TYPE && rhs.type == STR_TYPE) {
		size_t la = strlen(lhs.val.str);
		size_t lb = strlen(rhs.val.str);
		char *joined = checkAlloc(arena_alloc(in->arena, la + lb + 1, _Alignof(char)));
		memcpy(joined, lhs.val.str, la);
		memcpy(joined + la, rhs.val.str, lb);
		joined[la + lb] = '\0';
		*out = objStr(joined);
		return FLOW_NORMAL;
	}

	if (op == STX_EQUAL_EQUAL || op == STX_NOT_EQUAL) {
		bool equal;
		if (lhs.type == STR_TYPE && rhs.type == STR_TYPE) {
			equal = (0 == strcmp(lhs.val.str, rhs.val.str));
		} else if (objIsNumber(lhs) && objIsNumber(rhs)) {
			equal = bothInts(lhs, rhs) ? (lhs.val.i64 == rhs.val.i64)
			                           : (objAsDouble(lhs) == objAsDouble(rhs));
		} else {
			equal = (lhs.type == rhs.type) && (objIsTruthy(lhs) == objIsTruthy(rhs));
		}
		*out = objBln(op == STX_EQUAL_EQUAL ? equal : !equal);
		return FLOW_NORMAL;
	}

	if (!objIsNumber(lhs) || !objIsNumber(rhs)) {
		return rtError(in, at, "cannot apply operator to %s and %s",
		               objTypeName(lhs.type), objTypeName(rhs.type));
	}

	switch (op) {
		case STX_SUM:
			*out = bothInts(lhs, rhs) ? objI64(lhs.val.i64 + rhs.val.i64)
			                          : objDbl(objAsDouble(lhs) + objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_DIFF:
			*out = bothInts(lhs, rhs) ? objI64(lhs.val.i64 - rhs.val.i64)
			                          : objDbl(objAsDouble(lhs) - objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_MULT:
			*out = bothInts(lhs, rhs) ? objI64(lhs.val.i64 * rhs.val.i64)
			                          : objDbl(objAsDouble(lhs) * objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_DIV:
			if (bothInts(lhs, rhs)) {
				if (rhs.val.i64 == 0) {
					return rtError(in, at, "division by zero");
				}
				/* INT64_MIN / -1 is signed overflow - undefined behavior in C,
				 * not just a wrong answer. Two's complement has no positive
				 * counterpart for INT64_MIN, so this can never be represented. */
				if (lhs.val.i64 == INT64_MIN && rhs.val.i64 == -1) {
					return rtError(in, at, "integer overflow");
				}
				*out = objI64(lhs.val.i64 / rhs.val.i64);
			} else {
				if (objAsDouble(rhs) == 0.0) {
					return rtError(in, at, "division by zero");
				}
				*out = objDbl(objAsDouble(lhs) / objAsDouble(rhs));
			}
			return FLOW_NORMAL;
		case STX_MOD:
			if (!bothInts(lhs, rhs)) {
				return rtError(in, at, "'%%' requires integer operands");
			}
			if (rhs.val.i64 == 0) {
				return rtError(in, at, "division by zero");
			}
			/* Same overflow case as STX_DIV - INT64_MIN % -1 is undefined
			 * behavior too, even though the mathematical result (0) fits. */
			if (lhs.val.i64 == INT64_MIN && rhs.val.i64 == -1) {
				return rtError(in, at, "integer overflow");
			}
			*out = objI64(lhs.val.i64 % rhs.val.i64);
			return FLOW_NORMAL;
		case STX_GREATER:
			*out = objBln(objAsDouble(lhs) > objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_LESS:
			*out = objBln(objAsDouble(lhs) < objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_GREATER_EQUAL:
			*out = objBln(objAsDouble(lhs) >= objAsDouble(rhs));
			return FLOW_NORMAL;
		case STX_LESS_EQUAL:
			*out = objBln(objAsDouble(lhs) <= objAsDouble(rhs));
			return FLOW_NORMAL;
		default:
			return rtError(in, at, "unsupported operator");
	}
}

/* --- expressions ----------------------------------------------------------
 *
 * Precedence climbing over the flat list of named children. `idx` is a cursor
 * shared by the whole descent. */

static Flow evalExprSeq(Interp *in, const SyntaxNode *expr, size_t *idx,
                        int min_prec, Object *out);

static void skipOperand(const SyntaxNode *expr, size_t *idx) {
	const SyntaxNode *n = namedAt(expr, *idx);
	while (n && (n->type == STX_NOT || n->type == STX_NEGATE)) {
		(*idx)++;
		n = namedAt(expr, *idx);
	}
	if (n) {
		(*idx)++;
	}
}

/* Advances the cursor past a sub-expression without evaluating it, so `and` and
 * `or` can short-circuit rather than merely discarding the right-hand result. */
static void skipExprSeq(const SyntaxNode *expr, size_t *idx, int min_prec) {
	size_t count = namedCount(expr);

	skipOperand(expr, idx);
	while (*idx < count) {
		const SyntaxNode *op_node = namedAt(expr, *idx);
		int prec = opPrec(unwrapOp(op_node));
		if (prec == 0 || prec < min_prec) {
			break;
		}
		(*idx)++;
		skipExprSeq(expr, idx, prec + 1);
	}
}

static Flow evalOperand(Interp *in, const SyntaxNode *expr, size_t *idx, Object *out) {
	const SyntaxNode *n = namedAt(expr, *idx);
	SYNTAX_TYPE t;
	Object inner;
	Flow flow;

	if (!n) {
		return rtError(in, expr, "expected an operand");
	}

	t = n->type;
	if (t == STX_NOT || t == STX_NEGATE) {
		(*idx)++;
		flow = evalOperand(in, expr, idx, &inner);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
		if (t == STX_NOT) {
			*out = objBln(!objIsTruthy(inner));
			return FLOW_NORMAL;
		}
		if (inner.type == I64_TYPE) {
			*out = objI64(-inner.val.i64);
		} else if (inner.type == DBL_TYPE) {
			*out = objDbl(-inner.val.dbl);
		} else {
			return rtError(in, n, "cannot negate %s", objTypeName(inner.type));
		}
		return FLOW_NORMAL;
	}

	(*idx)++;
	return evalNode(in, n, out);
}

static Flow evalExprSeq(Interp *in, const SyntaxNode *expr, size_t *idx,
                        int min_prec, Object *out) {
	size_t count = namedCount(expr);
	Flow flow = evalOperand(in, expr, idx, out);

	if (flow != FLOW_NORMAL) {
		return flow;
	}

	while (*idx < count) {
		const SyntaxNode *op_node = namedAt(expr, *idx);
		SYNTAX_TYPE op = unwrapOp(op_node);
		int prec = opPrec(op);
		Object rhs;

		if (prec == 0 || prec < min_prec) {
			break;
		}
		(*idx)++;

		if (op == STX_AND || op == STX_OR) {
			bool left = objIsTruthy(*out);
			if ((op == STX_AND && !left) || (op == STX_OR && left)) {
				skipExprSeq(expr, idx, prec + 1);
				*out = objBln(left);
				continue;
			}
			flow = evalExprSeq(in, expr, idx, prec + 1, &rhs);
			if (flow != FLOW_NORMAL) {
				return flow;
			}
			*out = objBln(objIsTruthy(rhs));
			continue;
		}

		flow = evalExprSeq(in, expr, idx, prec + 1, &rhs);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
		flow = applyBinary(in, op_node, op, *out, rhs, out);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
	}
	return FLOW_NORMAL;
}

/* --- calls ---------------------------------------------------------------- */

static Flow callUserFunction(Interp *in, const SyntaxNode *fn,
                             const SyntaxNode *call, Object *out);

static Flow evalCall(Interp *in, const SyntaxNode *call, Object *out) {
	const Token *name = firstTokenChild(call);
	const SyntaxNode *first = namedAt(call, 0);
	const SyntaxNode *fn;

	if (first && first->type == STX_MEMBER) {
		return rtError(in, call, "method calls are not supported");
	}
	if (!name || name->type != TK_IDENTIFIER) {
		return rtError(in, call, "call target is not a plain identifier");
	}

	if (name->len == 5 && 0 == memcmp(name->start, "print", 5)) {
		size_t count = namedCount(call);
		Object *values = count ? checkAlloc(arena_alloc(in->arena, count * sizeof(Object),
		                                                _Alignof(Object)))
		                       : NULL;
		size_t n_values = 0;

		/* Every argument is evaluated to completion before anything is
		 * printed - otherwise a nested print() (or any other side effect)
		 * encountered while evaluating one argument would interleave its
		 * own output into the middle of this call's, instead of appearing
		 * to happen entirely before it, as strict left-to-right argument
		 * evaluation implies. Values are collected here rather than printed
		 * as they're evaluated for exactly that reason. */
		for (size_t i = 0; i < count; i++) {
			const SyntaxNode *arg = namedAt(call, i);
			Flow flow;

			if (arg->type == STX_NIL) {
				continue;
			}
			flow = evalNode(in, arg, &values[n_values]);
			if (flow != FLOW_NORMAL) {
				return flow;
			}
			n_values++;
		}

		/* Separators are counted against n_values, not the raw argument
		 * index - a nil argument is skipped entirely (see above), so it
		 * must not still count toward "is this the first thing printed". */
		for (size_t i = 0; i < n_values; i++) {
			if (i > 0) {
				putchar(' ');
			}
			objPrint(values[i]);
		}
		putchar('\n');
		*out = objNil();
		return FLOW_NORMAL;
	}

	/* web-csv local addition (see vendor/VENDORED.md): give a registered
	 * native hook first look, same priority print() gets, before falling
	 * through to a user-defined function of the same name. Arguments are
	 * evaluated up front for the same left-to-right, side-effects-in-order
	 * reason print()'s are above. */
	if (in->native_fn) {
		size_t count = namedCount(call);
		Object *args = count ? checkAlloc(arena_alloc(in->arena, count * sizeof(Object),
		                                              _Alignof(Object)))
		                    : NULL;
		bool handled;

		for (size_t i = 0; i < count; i++) {
			const SyntaxNode *arg = namedAt(call, i);
			Flow flow;

			if (arg->type == STX_NIL) {
				args[i] = objNil();
				continue;
			}
			flow = evalNode(in, arg, &args[i]);
			if (flow != FLOW_NORMAL) {
				return flow;
			}
		}

		handled = in->native_fn(in, name->start, name->len, args, count, out,
		                        in->native_userdata);
		if (handled) {
			return FLOW_NORMAL;
		}
	}

	fn = lookupFunction(in, name->start, name->len);
	if (!fn) {
		return rtError(in, call, "undefined function '%.*s'", (int)name->len, name->start);
	}
	return callUserFunction(in, fn, call, out);
}

/* STX_FNDEF's named children are (VTYPE, VAR) parameter pairs - or a lone
 * STX_NIL - then STX_RARROW, the return type, and the body. */
static size_t fnRarrowIndex(const SyntaxNode *fn) {
	size_t count = namedCount(fn);
	for (size_t i = 0; i < count; i++) {
		if (namedAt(fn, i)->type == STX_RARROW) {
			return i;
		}
	}
	return count;
}

static Flow callUserFunction(Interp *in, const SyntaxNode *fn,
                             const SyntaxNode *call, Object *out) {
	size_t rarrow = fnRarrowIndex(fn);
	size_t n_named = namedCount(fn);
	const SyntaxNode *body = (n_named > 0) ? namedAt(fn, n_named - 1) : NULL;
	size_t n_params = 0;
	size_t n_args = namedCount(call);
	Object *args = NULL;
	EnvFrame frame;
	Flow flow;

	if (!body || body->type != STX_SCOPE) {
		return rtError(in, call, "function has no body");
	}

	if (rarrow > 0 && namedAt(fn, 0)->type != STX_NIL) {
		n_params = rarrow / 2; /* (VTYPE, VAR) pairs */
	}

	if (n_args != n_params) {
		return rtError(in, call, "expected %zu argument(s), got %zu", n_params, n_args);
	}
	if (++in->depth > MAX_CALL_DEPTH) {
		in->depth--;
		return rtError(in, call, "call depth exceeded");
	}

	/* Arguments are evaluated in the caller's frame before it is swapped out. */
	if (n_args > 0) {
		args = checkAlloc(arena_alloc(in->arena, n_args * sizeof(Object), _Alignof(Object)));
		for (size_t i = 0; i < n_args; i++) {
			flow = evalNode(in, namedAt(call, i), &args[i]);
			if (flow != FLOW_NORMAL) {
				in->depth--;
				return flow;
			}
		}
	}

	frame = envPushFrame(&in->env);

	for (size_t i = 0; i < n_params; i++) {
		const SyntaxNode *param = namedAt(fn, i * 2 + 1); /* the VAR of each pair */
		const Token *pname = firstTokenChild(param);
		if (!pname) {
			envPopFrame(&in->env, frame);
			in->depth--;
			return rtError(in, fn, "malformed parameter list");
		}
		envDefine(&in->env, pname->start, pname->len, args[i]);
	}

	in->ret = objNil();
	flow = execNode(in, body);

	envPopFrame(&in->env, frame);
	in->depth--;

	if (flow == FLOW_RETURN) {
		*out = in->ret;
		return FLOW_NORMAL;
	}
	if (flow == FLOW_NORMAL) {
		*out = objNil();
		return FLOW_NORMAL;
	}
	if (flow == FLOW_BREAK) {
		/* A call boundary isolates control flow the same way it isolates
		 * scope (see envPushFrame): a break with no loop of its own inside
		 * this function must not reach back out and terminate a loop in
		 * whatever called it. */
		return rtError(in, call, "break outside a loop");
	}
	return flow; /* FLOW_EXIT and FLOW_ERROR keep propagating */
}

/* --- increment / decrement ------------------------------------------------ */

static Flow evalStep(Interp *in, const SyntaxNode *node, int delta, Object *out) {
	const SyntaxNode *target = namedAt(node, 0);
	const Token *name;
	Object *slot;

	if (!target || target->type != STX_VAR) {
		return rtError(in, node, "can only increment or decrement a variable");
	}
	name = firstTokenChild(target);
	if (!name) {
		return rtError(in, node, "malformed variable reference");
	}

	slot = envLookup(&in->env, name->start, name->len);
	if (!slot) {
		return rtError(in, node, "undefined variable '%.*s'", (int)name->len, name->start);
	}

	*out = *slot; /* postfix: the old value is the expression's result */
	if (slot->type == I64_TYPE) {
		slot->val.i64 += delta;
	} else if (slot->type == DBL_TYPE) {
		slot->val.dbl += delta;
	} else {
		return rtError(in, node, "cannot step a %s", objTypeName(slot->type));
	}
	return FLOW_NORMAL;
}

/* --- evaluation ----------------------------------------------------------- */

static Flow evalNode(Interp *in, const SyntaxNode *node, Object *out) {
	if (node->is_token) {
		return rtError(in, node, "expected an expression");
	}

	switch (node->type) {
		case STX_EXPR: {
			size_t idx = 0;
			return evalExprSeq(in, node, &idx, 1, out);
		}
		case STX_GEXPR: {
			const SyntaxNode *inner = namedAt(node, 0);
			if (!inner) {
				return rtError(in, node, "empty parenthesised expression");
			}
			return evalNode(in, inner, out);
		}
		case STX_NUM: {
			const Token *t = firstTokenChild(node);
			if (!t) {
				return rtError(in, node, "malformed number");
			}
			*out = numberFromToken(t);
			return FLOW_NORMAL;
		}
		case STX_STRLIT: {
			const Token *t = firstTokenChild(node);
			if (!t) {
				return rtError(in, node, "malformed string");
			}
			*out = stringFromToken(in, t);
			return FLOW_NORMAL;
		}
		case STX_TRUE:  *out = objBln(true);  return FLOW_NORMAL;
		case STX_FALSE: *out = objBln(false); return FLOW_NORMAL;
		case STX_NIL:   *out = objNil();      return FLOW_NORMAL;
		case STX_VAR: {
			const Token *name = firstTokenChild(node);
			Object *slot;
			if (!name) {
				return rtError(in, node, "malformed variable reference");
			}
			slot = envLookup(&in->env, name->start, name->len);
			if (!slot) {
				return rtError(in, node, "undefined variable '%.*s'",
				               (int)name->len, name->start);
			}
			*out = *slot;
			return FLOW_NORMAL;
		}
		case STX_FNCALL:    return evalCall(in, node, out);
		case STX_INCREMENT: return evalStep(in, node, 1, out);
		case STX_DECREMENT: return evalStep(in, node, -1, out);
		case STX_MEMBER:
			return rtError(in, node, "member access is not supported");
		case STX_INDEX:
			return rtError(in, node, "indexing is not supported");
		case STX_THIS:
			return rtError(in, node, "'this' is not supported");
		default:
			return rtError(in, node, "cannot evaluate /%s",
			               syntaxName(in->reg, node->type));
	}
}

/* --- statements ----------------------------------------------------------- */

static Flow execScope(Interp *in, const SyntaxNode *node) {
	size_t mark = envMark(&in->env);
	size_t count = namedCount(node);

	for (size_t i = 0; i < count; i++) {
		Flow flow = execNode(in, namedAt(node, i));
		if (flow != FLOW_NORMAL) {
			envRelease(&in->env, mark);
			return flow;
		}
	}
	envRelease(&in->env, mark);
	return FLOW_NORMAL;
}

static Flow execDeclare(Interp *in, const SyntaxNode *node) {
	const SyntaxNode *var = namedOfType(node, STX_VAR);
	const Token *name;

	if (!var) {
		return rtError(in, node, "malformed declaration");
	}
	name = firstTokenChild(var);
	if (!name) {
		return rtError(in, node, "malformed declaration");
	}
	/* No initialiser: the binding exists but holds nil until assigned. */
	envDefine(&in->env, name->start, name->len, objNil());
	return FLOW_NORMAL;
}

static Flow execInit(Interp *in, const SyntaxNode *node) {
	size_t count = namedCount(node);
	const SyntaxNode *var = namedOfType(node, STX_VAR);
	/* The initialiser is always last; what precedes it (type, '=') varies. */
	const SyntaxNode *value_expr = (count > 0) ? namedAt(node, count - 1) : NULL;
	const Token *name;
	Object value;
	Flow flow;

	if (!var || !value_expr) {
		return rtError(in, node, "malformed initialisation");
	}
	name = firstTokenChild(var);
	if (!name) {
		return rtError(in, node, "malformed initialisation");
	}

	flow = evalNode(in, value_expr, &value);
	if (flow != FLOW_NORMAL) {
		return flow;
	}
	envDefine(&in->env, name->start, name->len, value);
	return FLOW_NORMAL;
}

static bool isAssignOp(SYNTAX_TYPE op) {
	switch (op) {
		case STX_EQUAL:
		case STX_PLUS_EQUAL:
		case STX_MINUS_EQUAL:
		case STX_STAR_EQUAL:
		case STX_DIV_EQUAL:
		case STX_MOD_EQUAL:
			return true;
		default:
			return false;
	}
}

static Flow execAssign(Interp *in, const SyntaxNode *node) {
	size_t count = namedCount(node);
	const SyntaxNode *target = namedOfType(node, STX_VAR);
	const SyntaxNode *value_expr = (count > 0) ? namedAt(node, count - 1) : NULL;
	const Token *name;
	Object *slot;
	Object value;
	SYNTAX_TYPE op = STX_EQUAL;
	Flow flow;

	for (size_t i = 0; i < count; i++) {
		SYNTAX_TYPE candidate = unwrapOp(namedAt(node, i));
		if (isAssignOp(candidate)) {
			op = candidate;
			break;
		}
	}

	if (!value_expr) {
		return rtError(in, node, "malformed assignment");
	}
	if (!target) {
		return rtError(in, node, "can only assign to a variable");
	}
	name = firstTokenChild(target);
	if (!name) {
		return rtError(in, node, "malformed assignment target");
	}

	/* Evaluate before resolving the target. Evaluation can call a function,
	 * which declares bindings and may reallocate the binding array - a pointer
	 * taken beforehand would be left pointing into the stale copy. */
	flow = evalNode(in, value_expr, &value);
	if (flow != FLOW_NORMAL) {
		return flow;
	}

	slot = envLookup(&in->env, name->start, name->len);
	if (!slot) {
		return rtError(in, node, "undefined variable '%.*s'", (int)name->len, name->start);
	}

	if (op == STX_EQUAL) {
		*slot = value;
		return FLOW_NORMAL;
	}

	switch (op) {
		case STX_PLUS_EQUAL:  op = STX_SUM;  break;
		case STX_MINUS_EQUAL: op = STX_DIFF; break;
		case STX_STAR_EQUAL:  op = STX_MULT; break;
		case STX_DIV_EQUAL:   op = STX_DIV;  break;
		case STX_MOD_EQUAL:   op = STX_MOD;  break;
		default:
			return rtError(in, node, "unsupported assignment operator");
	}
	return applyBinary(in, node, op, *slot, value, slot);
}

/* STX_IF flattens to alternating condition/body pairs, with a trailing lone
 * body when there is an `else`. */
static Flow execIf(Interp *in, const SyntaxNode *node) {
	size_t count = namedCount(node);

	for (size_t i = 0; i + 1 < count; i += 2) {
		Object cond;
		Flow flow = evalNode(in, namedAt(node, i), &cond);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
		if (objIsTruthy(cond)) {
			return execNode(in, namedAt(node, i + 1));
		}
	}
	if (count % 2 == 1) {
		return execNode(in, namedAt(node, count - 1));
	}
	return FLOW_NORMAL;
}

static Flow execWhile(Interp *in, const SyntaxNode *node) {
	const SyntaxNode *cond = namedAt(node, 0);
	const SyntaxNode *body = namedAt(node, 1);

	if (!cond) {
		return rtError(in, node, "malformed while");
	}

	while (true) {
		Object value;
		Flow flow = evalNode(in, cond, &value);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
		if (!objIsTruthy(value)) {
			return FLOW_NORMAL;
		}
		if (!body) {
			continue;
		}
		flow = execNode(in, body);
		if (flow == FLOW_BREAK) {
			return FLOW_NORMAL;
		}
		if (flow != FLOW_NORMAL) {
			return flow;
		}
	}
}

static Flow execFor(Interp *in, const SyntaxNode *node) {
	size_t count = namedCount(node);
	const SyntaxNode *init = namedAt(node, 0);
	const SyntaxNode *cond = namedAt(node, 1);
	const SyntaxNode *step = namedAt(node, 2);
	/* The rule allows a bare ';' for the body, in which case there is no
	 * fourth named child. */
	const SyntaxNode *body = (count > 3) ? namedAt(node, 3) : NULL;
	size_t mark;
	Flow flow;

	if (!init || !cond || !step) {
		return rtError(in, node, "malformed for");
	}

	/* The loop variable belongs to the loop, not the enclosing scope. */
	mark = envMark(&in->env);
	flow = execNode(in, init);
	if (flow != FLOW_NORMAL) {
		envRelease(&in->env, mark);
		return flow;
	}

	while (true) {
		Object value;
		flow = evalNode(in, cond, &value);
		if (flow != FLOW_NORMAL) {
			break;
		}
		if (!objIsTruthy(value)) {
			flow = FLOW_NORMAL;
			break;
		}

		if (body) {
			flow = execNode(in, body);
			if (flow == FLOW_BREAK) {
				flow = FLOW_NORMAL;
				break;
			}
			if (flow != FLOW_NORMAL) {
				break;
			}
		}

		flow = evalNode(in, step, &value);
		if (flow != FLOW_NORMAL) {
			break;
		}
	}

	envRelease(&in->env, mark);
	return flow;
}

static Flow execReturn(Interp *in, const SyntaxNode *node) {
	const SyntaxNode *value_expr = namedAt(node, 0);
	Object value = objNil();

	/* Evaluate into a local, not into in->ret: the expression may contain calls
	 * that set in->ret themselves, which would clobber the partial result being
	 * accumulated. */
	if (value_expr) {
		Flow flow = evalNode(in, value_expr, &value);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
	}
	in->ret = value;
	return FLOW_RETURN;
}

static Flow execExit(Interp *in, const SyntaxNode *node) {
	const SyntaxNode *value_expr = namedAt(node, 0);
	Object value = objNil();

	if (value_expr) {
		Flow flow = evalNode(in, value_expr, &value);
		if (flow != FLOW_NORMAL) {
			return flow;
		}
	}
	in->exit_code = (value.type == I64_TYPE) ? (int)value.val.i64 : 0;
	return FLOW_EXIT;
}

static Flow execFnDef(Interp *in, const SyntaxNode *node) {
	const Token *name = firstTokenChild(node);

	/* The first token child is TK_FN; the name is the one after it. */
	for (size_t i = 0; i < node->n_children; i++) {
		if (node->children[i]->is_token &&
		    node->children[i]->token.type == TK_IDENTIFIER) {
			name = &node->children[i]->token;
			break;
		}
	}
	if (!name || name->type != TK_IDENTIFIER) {
		return rtError(in, node, "function definition has no name");
	}
	defineFunction(in, name->start, name->len, node);
	return FLOW_NORMAL;
}

static Flow execNode(Interp *in, const SyntaxNode *node) {
	Object discard;

	if (node->is_token) {
		return FLOW_NORMAL; /* punctuation that survived flattening */
	}

	switch (node->type) {
		case STX_SCOPE:   return execScope(in, node);
		case STX_DECLARE: return execDeclare(in, node);
		case STX_INIT:    return execInit(in, node);
		case STX_ASSIGN:  return execAssign(in, node);
		case STX_IF:      return execIf(in, node);
		case STX_WHILE:   return execWhile(in, node);
		case STX_FOR:     return execFor(in, node);
		case STX_RETURN:  return execReturn(in, node);
		case STX_EXIT:    return execExit(in, node);
		case STX_FNDEF:   return execFnDef(in, node);
		case STX_BREAK:   return FLOW_BREAK;
		case STX_CLASS:
			return rtError(in, node, "classes are not supported");
		case STX_SWITCH:
		case STX_CASE:
			return rtError(in, node, "switch/case is not supported");
		case STX_FORRANGE:
			return rtError(in, node, "for-in is not supported");
		case STX_ECHO: {
			/* An expression statement: evaluate for its effects, drop the value. */
			const SyntaxNode *inner = namedAt(node, 0);
			if (!inner) {
				return FLOW_NORMAL;
			}
			return evalNode(in, inner, &discard);
		}
		default:
			/* A name the grammar file invented carries no built-in semantics,
			 * so treat it as a transparent grouping. That lets a custom grammar
			 * introduce its own structural rules (a STX_PROGRAM wrapper, a
			 * STX_STATEMENT alternation) and still run. */
			if (node->type >= STX__COUNT) {
				size_t count = namedCount(node);
				for (size_t i = 0; i < count; i++) {
					Flow flow = execNode(in, namedAt(node, i));
					if (flow != FLOW_NORMAL) {
						return flow;
					}
				}
				return FLOW_NORMAL;
			}
			return evalNode(in, node, &discard);
	}
}

/* --- entry point ---------------------------------------------------------- */

/* Registers definitions before execution so a call can precede its definition.
 * Descends through grammar-minted grouping nodes for the same reason execNode
 * treats them as transparent. */
static void hoistFunctions(Interp *in, const SyntaxNode *node) {
	size_t count = namedCount(node);

	for (size_t i = 0; i < count; i++) {
		const SyntaxNode *child = namedAt(node, i);
		if (child->type == STX_FNDEF) {
			execFnDef(in, child);
		} else if (child->type >= STX__COUNT) {
			hoistFunctions(in, child);
		}
	}
}

Interp *interpCreate(Registry *reg, Arena *arena) {
	Interp *in = checkAlloc(arena_zalloc(arena, sizeof(Interp), _Alignof(Interp)));
	in->reg = reg;
	in->arena = arena;
	envInit(&in->env, arena);
	in->ret = objNil();
	return in;
}

/* web-csv local addition (see vendor/VENDORED.md) */
void interpSetNativeHook(Interp *in, NativeFn fn, void *userdata) {
	in->native_fn = fn;
	in->native_userdata = userdata;
}

bool interpIsExpression(const SyntaxNode *node) {
	if (node->is_token) {
		return false;
	}
	switch (node->type) {
		case STX_EXPR:
		case STX_GEXPR:
		case STX_NUM:
		case STX_STRLIT:
		case STX_TRUE:
		case STX_FALSE:
		case STX_NIL:
		case STX_VAR:
		case STX_FNCALL:
		case STX_MEMBER:
		case STX_INDEX:
		case STX_THIS:
		case STX_INCREMENT:
		case STX_DECREMENT:
			return true;
		default:
			return false;
	}
}

/* Kept in sync with execNode's own switch by hand, the same way shape.c's
 * CONTRACTS table is kept in sync with what each tag structurally needs -
 * there is no way to derive this automatically from the switch itself. */
bool interpIsRunnable(SYNTAX_TYPE type) {
	switch (type) {
		case STX_SCOPE:
		case STX_DECLARE:
		case STX_INIT:
		case STX_ASSIGN:
		case STX_IF:
		case STX_WHILE:
		case STX_FOR:
		case STX_RETURN:
		case STX_EXIT:
		case STX_FNDEF:
		case STX_BREAK:
		case STX_CLASS:
		case STX_SWITCH:
		case STX_CASE:
		case STX_FORRANGE:
		case STX_ECHO:
		case STX_EXPR:
		case STX_GEXPR:
		case STX_NUM:
		case STX_STRLIT:
		case STX_TRUE:
		case STX_FALSE:
		case STX_NIL:
		case STX_VAR:
		case STX_FNCALL:
		case STX_MEMBER:
		case STX_INDEX:
		case STX_THIS:
		case STX_INCREMENT:
		case STX_DECREMENT:
			return true;
		default:
			/* Grammar-minted names are transparent grouping to execNode
			 * (see its own default case), so they're always worth trying. */
			return type >= STX__COUNT;
	}
}

static ExecResult finish(Interp *in, Flow flow, int *out_exit) {
	switch (flow) {
		case FLOW_ERROR:
			*out_exit = 1;
			return EXEC_ERROR;
		case FLOW_EXIT:
			*out_exit = in->exit_code;
			return EXEC_EXITED;
		case FLOW_RETURN:
			*out_exit = (in->ret.type == I64_TYPE) ? (int)in->ret.val.i64 : 0;
			return EXEC_OK;
		case FLOW_BREAK:
			/* Reaching all the way here means nothing along the way - no
			 * enclosing loop, no enclosing call (see callUserFunction's own
			 * check) - ever caught it, so there was no loop to break out of
			 * in the first place. */
			fprintf(stderr, "runtime error: break outside a loop\n");
			*out_exit = 1;
			return EXEC_ERROR;
		default:
			*out_exit = 0;
			return EXEC_OK;
	}
}

ExecResult interpExec(Interp *in, const SyntaxNode *root, int *out_exit) {
	return finish(in, execNode(in, root), out_exit);
}

ExecResult interpExecEcho(Interp *in, const SyntaxNode *root, int *out_exit) {
	Object value;
	Flow flow;

	if (!interpIsExpression(root)) {
		return interpExec(in, root, out_exit);
	}

	flow = evalNode(in, root, &value);
	if (flow == FLOW_NORMAL && value.type != NIL_TYPE) {
		objPrint(value);
		putchar('\n');
	}
	return finish(in, flow, out_exit);
}

int runProgram(const SyntaxNode *root, Registry *reg, Arena *arena, int *out_exit) {
	Interp *in = interpCreate(reg, arena);

	if (!root->is_token) {
		hoistFunctions(in, root);
	}
	return (interpExec(in, root, out_exit) == EXEC_ERROR) ? 1 : 0;
}
