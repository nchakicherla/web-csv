#include "env.h"
#include "oom.h"

#include <string.h>

void envInit(Env *env, Arena *arena) {
	env->arena = arena;
	env->vars = NULL;
	env->n_vars = 0;
	env->cap_vars = 0;
	env->frame_base = 0;
}

void envDefine(Env *env, const char *name, size_t len, Object val) {
	if (env->n_vars == env->cap_vars) {
		size_t new_cap = (env->cap_vars == 0) ? 32 : env->cap_vars * 2;
		env->vars = checkAlloc(arena_grow(env->arena, env->vars,
		                                  env->cap_vars * sizeof(Binding),
		                                  new_cap * sizeof(Binding), _Alignof(Binding)));
		env->cap_vars = new_cap;
	}
	env->vars[env->n_vars].name = name;
	env->vars[env->n_vars].len = len;
	env->vars[env->n_vars].val = val;
	env->n_vars++;
}

Object *envLookup(Env *env, const char *name, size_t len) {
	size_t i = env->n_vars;
	while (i > env->frame_base) {
		i--;
		if (env->vars[i].len == len && 0 == memcmp(env->vars[i].name, name, len)) {
			return &env->vars[i].val;
		}
	}
	return NULL;
}

size_t envMark(const Env *env) {
	return env->n_vars;
}

void envRelease(Env *env, size_t mark) {
	env->n_vars = mark;
}

EnvFrame envPushFrame(Env *env) {
	EnvFrame frame;
	frame.mark = env->n_vars;
	frame.saved_frame_base = env->frame_base;
	env->frame_base = frame.mark;
	return frame;
}

void envPopFrame(Env *env, EnvFrame frame) {
	env->frame_base = frame.saved_frame_base;
	env->n_vars = frame.mark;
}
