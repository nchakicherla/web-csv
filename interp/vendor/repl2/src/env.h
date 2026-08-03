#ifndef ENV_H
#define ENV_H

/* env.h - variable bindings for a tree-walking interpreter.
 *
 * A flat, arena-backed stack of (name, value) pairs - the same design the
 * original flexible-parser's interp.c used internally, pulled out as its own
 * piece rather than embedded in a bigger Interp struct, so it can be built
 * and tested before anything else that would depend on it exists.
 *
 * Two distinct operations, easy to conflate but not the same thing:
 *
 *   envMark / envRelease   - a lexical block entering/leaving (STX_SCOPE).
 *                            Only the stack height changes; frame_base stays
 *                            put, so the block can still see whatever its
 *                            enclosing scope already defined. This is what
 *                            makes `{ let x = 1; { let y = x + 1; } }` work.
 *
 *   envPushFrame / envPopFrame - a function call. Both the stack height AND
 *                            frame_base move, so envLookup inside the call
 *                            cannot see anything defined before the call -
 *                            a callee's variables are isolated from its
 *                            caller's, which is what keeps this lexically
 *                            (rather than dynamically) scoped.
 */

#include "arena.h"
#include "object.h"

typedef struct s_Binding {
	const char *name;
	size_t len;
	Object val;
} Binding;

typedef struct s_Env {
	Arena *arena;

	Binding *vars;
	size_t n_vars;
	size_t cap_vars;

	/* envLookup searches backwards down to here, not to 0. */
	size_t frame_base;
} Env;

typedef struct {
	size_t mark;
	size_t saved_frame_base;
} EnvFrame;

void envInit(Env *env, Arena *arena);

/* Appends a new binding. The same name can be defined more than once - an
 * inner declaration shadows an outer one, found first by envLookup's
 * backwards search - so re-declaring is never an error here. */
void envDefine(Env *env, const char *name, size_t len, Object val);

/* Backwards search from the top down to frame_base, so a shadowing
 * redeclaration is found before what it shadows. Returns the binding's
 * mutable slot (so assignment can write through it), or NULL if the name
 * is not defined in the current frame. */
Object *envLookup(Env *env, const char *name, size_t len);

/* Block-scope boundary: envRelease(env, envMark(env)) around a scope's body
 * discards whatever it defined without disturbing frame_base. */
size_t envMark(const Env *env);
void envRelease(Env *env, size_t mark);

/* Call-frame boundary: envPopFrame(env, envPushFrame(env)) around a call
 * both discards the callee's locals and restores the caller's frame_base. */
EnvFrame envPushFrame(Env *env);
void envPopFrame(Env *env, EnvFrame frame);

#endif // ENV_H
