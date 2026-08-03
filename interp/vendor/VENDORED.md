# Vendored: repl2

`repl2/src/` in this directory is a vendored copy of
[nchakicherla/repl2](https://github.com/nchakicherla/repl2), pinned at:

```
commit 9a4b3f6b9f9d50eec5fea547554dc7b4cbe89e3d
date   2026-08-03 11:23:59 -0700
```

Vendored (not a git submodule) so this repo builds standalone without a
submodule-init step, and so the one local patch below can just live in the
tree instead of as a diff someone has to remember to reapply.

`main.c`, `repl.c`, `repl.h`, and `external/linenoise` were dropped — those
are the native CLI's entry point and terminal line-editing, neither of
which applies to a browser build. `web-csv` supplies its own entry point at
[`interp/ext/web_main.c`](../ext/web_main.c).

## Local patch: native-function hook

`interp.h` / `interp.c` gained one small, generic extension point that
upstream repl2 doesn't have: a way for an embedder to register a C callback
that `evalCall` tries before falling through to user-defined functions.
Without it, adding a builtin means hardcoding a name check into `evalCall`
the way `print` already is — fine for one builtin baked into the language,
not something an embedder bolting on `gpu_sum`/`gpu_filter` should have to
patch `interp.c` for every time.

Look for `NativeFn` / `interpSetNativeHook` in `interp.h`, and the block in
`evalCall` (interp.c) right after the `print` special case. It's a handful
of lines, reuses `evalCall`'s existing argument-evaluation pattern, and
doesn't know anything about columns, GPUs, or CSVs — that all lives in
`interp/ext/`, which is the actual web-csv-specific code.

This hook is generic enough it's plausibly worth upstreaming into repl2
proper rather than carrying as a fork forever. Re-sync this vendored copy
by re-copying `repl2/src/` and reapplying the same patch (or dropping it,
if it's landed upstream by then).
