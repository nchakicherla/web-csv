# web-csv

CSV analysis in the browser: upload a CSV, then query and transform it
with [repl2](https://github.com/nchakicherla/repl2)'s configurable-grammar
tree-walking interpreter, compiled to WASM. Parallelizable operations
(currently: summing a numeric column) run as WebGPU compute shaders on
eligible hardware, falling back to plain WASM otherwise. A small
Node/SQLite service persists saved queries and dashboards; the compute
path itself needs no backend.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design, what's
built vs. stubbed, and known gaps. This is a scaffold from an initial
design pass, not a finished app - see "First build checklist" below for
what's proven to work vs. still untested.

## Layout

```
interp/vendor/repl2/   vendored repl2 core + one small patch (see VENDORED.md)
interp/ext/            web-csv's C layer: column store, GPU-aware builtins,
                       the Emscripten entry point, and its build (Makefile)
resources/             grammar-csv.txt, the default query/scripting grammar
web/                   browser UI, CSV parsing, WebGPU bridge
server/                persistence API (saved queries/dashboards) + static host
docs/                  architecture notes
```

## Prerequisites

Not installed in the sandbox this scaffold was written in - none of the
build/run steps below have been executed for real. See "First build
checklist".

- **Emscripten SDK** (`emcc`) - https://emscripten.org/docs/getting_started/downloads.html
- **Node.js** (for `server/`) - any reasonably recent LTS
- A browser with WebGPU (recent Chrome/Edge; Firefox/Safari support is
  still landing) to exercise the `gpu_sum` path - everything else works
  without it, falling back to CPU

## Building the interpreter

```bash
cd interp/ext
make
```

Produces `web/src/wasm/interp.js` + `interp.wasm` (+ `.data` for the
preloaded default grammar). Gitignored - rebuild after pulling.

## Running

```bash
cd server
npm install
npm start
```

Serves the frontend and the persistence API on `http://localhost:8787`
(see `server/.env.example` for `PORT`/`WC_DB_PATH`). Open that URL, upload
a CSV, and run the default query in the textbox - or without `server/` at
all, serve `web/` with any static file server (e.g. `npx serve web`) if
you don't need saved queries/dashboards yet.

## First build checklist

Verified during scaffolding, with a native (`cc`, not `emcc`) build
standing in for the interpreter core: grammar parsing, the native-function
hook, the column store, and `col()`/`sum()`/`filter_gt()`/`emit()` all
work correctly together.

Not yet verified - do these first, in roughly this order, since each
depends on the last:

1. `cd interp/ext && make` actually succeeds against real `emcc`/Asyncify
2. `wc_init`/`wc_run` work from a browser console against the built `.js`/`.wasm`
3. `gpu_sum` actually round-trips through `reduce_sum.wgsl` on real WebGPU
   hardware (`web/src/gpu/bridge.js`) - if this hangs or errors, check
   Asyncify is instrumenting the full call path first (see
   ARCHITECTURE.md's Asyncify section) before suspecting the shader
4. `cd server && npm install && npm start` - dependency versions in
   `package.json` are unverified against current npm

## Known gaps

- String/categorical columns are parsed (`web/src/csv/parse.js`) but not
  loaded into the interpreter's column store - only `wc_load_column_f64`
  exists. `COL_STR_DICT` is implemented C-side (`column.c`/`store.c`);
  it just has no JS-facing loader yet.
- `emit()` on a column argument summarizes it (sums it) rather than
  streaming the whole column back to JS - real chart-from-a-result-column
  support needs a pointer/length export like `wc_load_column_f64`'s
  counterpart, in reverse.
- Only `sum` has a GPU path. `filter_gt` is CPU-only; GPU filter/groupby/
  sort/join are all future work (see ARCHITECTURE.md's shader-scope note).
- `ASYNCIFY=1` instruments the whole interpreter rather than the narrower
  `ASYNCIFY_ONLY` call path - fine for correctness, worth tightening once
  there's a real build to derive the exact list from.
- Auth is a localhost-only stub (`server/src/routes/auth.js`) - not
  suitable for anything beyond one developer's own machine.
- `resources/grammar-csv.txt` is a function-call-style DSL, not SQL - see
  ARCHITECTURE.md's grammar section for why a SQL surface needs new
  interpreter semantics, not just a new grammar file.
