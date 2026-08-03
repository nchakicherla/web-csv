# Architecture

CSV analysis in the browser: repl2's configurable-grammar tree-walking
interpreter (compiled to WASM via Emscripten) as both query language and
scripting language, with WebGPU compute shaders taking the parallelizable
parts of the work on eligible hardware. A small Node/SQLite service
persists saved queries and dashboards; everything else runs client-side.

## Layers

```
web/               browser UI, CSV parsing, WebGPU bridge, static JS
  index.html
  src/
    csv/parse.js        CSV text -> typed columns
    gpu/device.js        shared WebGPU device
    gpu/bridge.js         Module.gpuBridge - the JS side of the async boundary
    gpu/shaders/*.wgsl    compute shaders
    api/client.js         talks to server/
    main.js               wires it all together
    wasm/                 emcc build output (gitignored, not committed)

interp/
  vendor/repl2/src/   vendored repl2 core (see vendor/VENDORED.md) + one
                       small patch: a native-function hook in interp.c/.h
  ext/                web-csv's own C layer, compiled alongside the vendor
                       tree by ext/Makefile
    column.h/.c          typed column type (f64 / i32 / dict-encoded string)
    store.h/.c            the session's named + tracked columns
    builtins_gpu.h/.c     col()/sum()/gpu_sum()/filter_gt()/emit() - the
                          interpreter's native-function hook implementation
    web_main.c            Emscripten entry point (wc_init/wc_run/...)

server/             persistence API (saved queries, dashboards) + serves web/
  src/index.js, db.js, routes/

resources/
  grammar-csv.txt    default DSL grammar

docs/ARCHITECTURE.md  this file
```

## Column store — the shared contract

A CSV column becomes one typed, contiguous buffer: `Float64Array`-shaped
for numeric columns, dictionary-encoded `int32` codes for
strings/categoricals. This is the one representation all three layers
(interpreter, CPU builtins, GPU bridge) read and write against - no boxing
per row, no copying between "the interpreter's view" and "the GPU's view".

Columns are `malloc`/`free`'d (interp/ext/column.c), not arena-allocated:
their lifetime is "until the CSV is unloaded or a result is replaced",
independent of any one script run's arena. The interpreter wraps a
`Column*` as an `Object` via `PTR_TYPE` plus a magic-number tag
(`objColumn`/`objAsColumn`) so a native builtin can accept a column
argument without repl2's `object.h` needing to know columns exist.

**Gap:** `wc_load_column_f64` (web_main.c) is the only JS-facing loader.
`COL_STR_DICT` exists on the C side (column.c/store.c) but has no JS entry
point yet - `parse.js` infers and returns string columns, they just aren't
pushed into the WASM store. A `wc_load_column_str_dict` export is the
natural next addition.

## Grammar/DSL layer

repl2's interpreter dispatches on a fixed set of built-in `STX_*` tags
(scope, if, while, fncall, expr, ...) - see `interp/vendor/repl2/src/interp.c`.
A grammar file only defines *concrete syntax* that produces those tags;
it can't introduce new runtime semantics on its own. `resources/grammar-csv.txt`
is one such grammar (`let x := expr;`, no braces required at the top
level) - swap it for a different grammar file and the same interpreter
runs a differently-shaped language, which is the feature repl2 brings to
this project that a hand-rolled query parser wouldn't.

A SQL-shaped surface (`SELECT sum(amount) FROM t WHERE amount > 100`) is a
plausible grammar to add later, but it needs a new `STX_SELECT` tag *and*
a corresponding case in `interp.c`'s `execNode`/`evalNode` switch to mean
anything - repl2 has no macro/desugaring system that would let a grammar
alone rewrite that into nested `STX_FNCALL`s. That's real interpreter work,
not just a new grammar file, and isn't attempted in this scaffold.

## Builtin bridge (interpreter <-> GPU) + Asyncify scoping

`interp/ext/builtins_gpu.c` registers `col`, `sum`, `gpu_sum`, `filter_gt`,
`emit` through the native-function hook (`interpSetNativeHook`, patched
into vendored `interp.c`/`interp.h` - see `vendor/VENDORED.md` for exactly
what changed and why). `sum` is a synchronous CPU loop. `gpu_sum` is the
one actual async boundary: above `WC_GPU_MIN_LEN` elements, on an f64
column, when `navigator.gpu` resolves, it calls `wcGpuReduceSum` - an
`EM_ASYNC_JS` import whose body is `await Module.gpuBridge.reduceSum(...)`
(`web/src/gpu/bridge.js`) - and falls back to the CPU loop otherwise.

Asyncify has to instrument every function on the call path from an
exported entry point down to that async import - for `gpu_sum` that's
`wc_run` through `interp.c`'s `evalCall`/`evalNode`/`execNode`/`interpExec`
chain, not just the leaf trampoline. `interp/ext/Makefile` currently
builds with `ASYNCIFY=1` (instrument everything - correct, but pays
size/speed cost on functions that never touch that path, like the scanner
or grammar loader). This now builds and has been confirmed (Node, with a
fake `navigator.gpu` and a stub bridge with a real `setTimeout` delay) to
actually suspend and resume the C call stack correctly across a real async
JS boundary - so narrowing to `ASYNCIFY_ONLY` is a real, doable
optimization now, not a hopeful TODO: get the exact function list from
`emcc ... -s ASYNCIFY_ADVISE=1` against this build rather than guessing it
by hand. Not done here since `ASYNCIFY=1` is already correct and this
wasn't the ask.

## GPU shader library scope

`web/src/gpu/shaders/reduce_sum.wgsl` - a two-stage parallel reduction
(per-workgroup partial sums on GPU, final add of the small partials array
on CPU) - is the only shader in this scaffold. Filter (predicate -> mask)
is the next straightforward one. Sort and hash-join are real engineering
effort (bitonic sort network, GPU hash join are established techniques,
not quick additions) and are out of scope here; the CPU path covers those
operations until then.

**WGSL has no f64.** Its core numeric types are f32/i32/u32. `bridge.js`
downcasts an f64 column to f32 before upload, which is a real precision
tradeoff (visible drift possible on large sums or values spanning many
orders of magnitude), not an oversight - `cpuSum` in `builtins_gpu.c`
stays exact f64 and is what small/precision-sensitive sums use via the
eligibility gate below.

## Eligibility gate

Three conditions, not just "hardware supports WebGPU" (`doSum` in
builtins_gpu.c):

1. the operation is a GPU-friendly shape (columnar reduce, not per-row
   branchy scripting - `gpu_sum` is only wired for `sum`)
2. the column is above `WC_GPU_MIN_LEN` (currently 50,000, an unbenchmarked
   starting guess - buffer upload + pipeline dispatch + mapped readback
   all cost real wall-clock time a plain loop doesn't)
3. `navigator.gpu` actually resolves an adapter (`wcGpuAvailable`)

Below the threshold, or when any condition fails, `sum()`'s CPU loop runs
- faster below threshold anyway, and skips Asyncify's overhead for that
call.

## Deployment model

WebGPU compute runs client-side, so the heavy lifting happens on the
user's own hardware - the app itself could be nearly static. This project
does want persistence (saved queries, saved dashboards, multi-user), so
`server/` exists for exactly that: a small Express + SQLite service that
serves `web/` as static files and exposes `/api/queries` and
`/api/dashboards`. It has no involvement in running a query - a browser
with the page open and no backend reachable can still load a CSV and run
scripts against it.

**Open decision, deliberately not resolved here:** `server/src/routes/auth.js`
is a dev-only stub (trusts an `x-user-id` header, no real login). Real
auth - sessions vs. OAuth/SSO vs. something else - is a decision with
tradeoffs that shouldn't get made implicitly by scaffolding code; it needs
to happen before this is exposed anywhere beyond localhost.

## What's actually verified vs. not

Verified by a native `cc` build during initial scaffolding (standing in
for `emcc`, which wasn't installed yet): grammar parsing, the
native-function hook, the column store, and `col`/`sum`/`filter_gt`/`emit`
all work correctly together end to end.

Verified next, against a real `emcc` 6.0.5 (`ASYNCIFY=1`) build run under
Node: `wc_init`/`wc_load_column_f64`/`wc_run` all work against the actual
build output, and - by faking `navigator.gpu` and a stub async bridge with
a real `setTimeout` delay to force `gpu_sum`'s GPU-eligible branch -
Asyncify genuinely suspends the C call stack (`wc_run` → `evalCall` → ...
→ `wcGpuReduceSum`) across a real async JS boundary and resumes with the
correct result.

Verified fully since, in a real browser (Chromium/Electron, real WebGPU
adapter and device): the whole page flow works - uploading a CSV through
the actual file input, running a query through `filter_gt`/`sum`/`emit`,
getting back correct results - and, forcing a column past
`WC_GPU_MIN_LEN`, `gpu_sum` actually round-trips through `reduce_sum.wgsl`
on the real `GPUDevice`/`GPUBuffer`, not a stub, returning the exact
correct sum. Getting here surfaced three real bugs, now fixed:
`EXPORTED_RUNTIME_METHODS` needed `HEAPF64` added explicitly (this
Emscripten version doesn't expose typed-array heap views by default);
`MODULARIZE=1` alone emits a UMD/CommonJS factory with no real `export`,
so `main.js`'s static `import` silently got `undefined` - needed
`-s EXPORT_ES6=1`; and the preloaded grammar file resolved against the
*page's* URL rather than `main.js`'s own, 404ing, since `index.html` and
`web/src/wasm/` aren't siblings - fixed with an explicit `locateFile` in
`main.js`. See README's "Bugs found" for the exact symptoms, useful if
any of these regress on a different Emscripten version.

The persistence server is verified too: `npm install && npm start` works
(`better-sqlite3` from a prebuilt binary, no native compile needed), and
`/api/queries`/`/api/dashboards` round-trip correctly through a real
SQLite file with correct per-user isolation. One more real bug turned up
here - `client.js` never actually sent the `x-user-id` header `auth.js`'s
stub requires, so the UI's "Save query" button 401'd unconditionally,
silently - fixed by generating a stable per-browser dev identity in
`localStorage`. Every item on README's "First build checklist" is now
verified; what's left is the "Known gaps" list there and above, which are
deliberate scope cuts, not open questions about whether things work.
