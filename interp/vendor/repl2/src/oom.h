#ifndef OOM_H
#define OOM_H

/* oom.h - the one place an arena allocation failure is handled.
 *
 * db4's Arena (arena.h) returns NULL from arena_alloc/arena_zalloc/arena_grow
 * on a real malloc failure or on hitting its memory budget, rather than
 * aborting - that is deliberate, so a long-lived embedder (db4 itself) can
 * decide how to recover. This project has no such recovery story yet, so
 * every direct arena_* call is wrapped in checkAlloc(), which restores the
 * simpler fail-fast behavior the old bump allocator this replaced (mempool.c)
 * used to give for free: print a clear reason and exit, instead of writing
 * through a NULL pointer a few lines later.
 *
 * A failed arena latches (see arena_failed()): every further allocation on
 * the same arena returns NULL too, so wrapping the low-level append/growth
 * helpers (registry.c's growArray/internChars, grammar.c's node/list
 * allocators, ast.c's node allocators, scanner.c's token array growth,
 * file.c's tryReadFile) is enough to make everything built on top of them
 * safe as well, without needing a check at every call site in this codebase.
 */

void *checkAlloc(void *ptr);

#endif // OOM_H
