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

**Status:** builds against real Emscripten (`emcc` 6.0.5) and has been
run end to end in a real browser with real WebGPU hardware - CSV upload
through the actual UI, a query using `filter_gt`/`sum`/`gpu_sum`/`emit`,
and (separately, with a large enough column to clear the GPU eligibility
threshold) a real dispatch through `reduce_sum.wgsl` on a real
`GPUDevice`, all returning correct, hand-checked results. Only the
persistence server (`server/`) hasn't been run yet - see the checklist.

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

- **Emscripten SDK** (`emcc`) - https://emscripten.org/docs/getting_started/downloads.html.
  Installed via `emsdk` (`git clone https://github.com/emscripten-core/emsdk`,
  `./emsdk install latest && ./emsdk activate latest`), not Homebrew -
  `source /path/to/emsdk/emsdk_env.sh` before building.
- **Node.js** (for `server/`, and it's what `emsdk` itself bundles) - any
  reasonably recent version
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
all, serve `web/` with any static file server (e.g. `python3 -m http.server
8080 --directory web`, or `npx serve web`) if you don't need saved
queries/dashboards yet.

## Sample data

[web/sample-data/transactions.csv](web/sample-data/transactions.csv) - 20
rows, `id`/`amount`/`category` columns. Upload it and run the default
query in the textbox as-is: `filter_gt(col("amount"), 100)` then `sum`
should return `8183.23`, and `gpu_sum(col("amount"))` (falls back to CPU
at this size - see the eligibility gate in ARCHITECTURE.md) `8512.01`.
Both are hand-checked and match what a real browser run actually returned.

## First build checklist

Verified, against the real toolchain (not a stand-in), in a real browser
with real WebGPU hardware:

1. ✅ Grammar parsing, the native-function hook, the column store, and
   `col()`/`sum()`/`filter_gt()`/`emit()` all work correctly together.
2. ✅ `cd interp/ext && make` succeeds against real `emcc` 6.0.5 with
   `ASYNCIFY=1`, producing `web/src/wasm/interp.{js,wasm,data}`. Two real
   build/glue issues turned up and are now fixed in the Makefile/`main.js`
   (see "Bugs found" below) - `EXPORTED_RUNTIME_METHODS` needing `HEAPF64`
   explicitly, `EXPORT_ES6=1` needing to be set for the `import` in
   `main.js` to work at all, and `locateFile` needing to be set so the
   preloaded grammar/wasm resolve against `main.js`'s own URL rather than
   the page's.
3. ✅ Asyncify genuinely suspends and resumes the C call stack around a
   real async JS boundary (confirmed under Node first, with a forced
   GPU-eligible path and a stub bridge with a real delay, before the
   browser test existed).
4. ✅ The full page flow works in a real browser: uploading
   `sample-data/transactions.csv` through the actual file input, running
   the default query, and getting back the correct, hand-checked results
   (`8183.23`, `8512.01`) via `filter_gt`/`sum`/`emit`.
5. ✅ `gpu_sum` actually round-trips through `reduce_sum.wgsl` on real
   WebGPU hardware - forced past the `WC_GPU_MIN_LEN` eligibility
   threshold with a 60,000-element column (the sample CSV is too small to
   take this path on its own), it returned the exact correct sum through a
   real `GPUDevice`/`GPUBuffer` dispatch, not a stub.

Not yet verified:

6. `cd server && npm install && npm start` - dependency versions in
   `package.json` are unverified against current npm, and the persistence
   API/routes haven't been exercised at all yet.

### Bugs found doing the above (all fixed)

- `EXPORTED_RUNTIME_METHODS` needed `HEAPF64` listed explicitly - this
  Emscripten version doesn't expose wasm heap typed-array views by default,
  and `main.js`/`bridge.js` both read/write it directly.
- `main.js`'s `import createInterpModule from './wasm/interp.js'` silently
  got `undefined` as the default export - `MODULARIZE=1` alone emits a
  UMD/CommonJS factory (`module.exports = ...`), not a real ES module.
  Needed `-s EXPORT_ES6=1`.
- The preloaded grammar file (`interp.data`) 404'd at the page's root
  (`GET /interp.data`) instead of next to the build output
  (`/src/wasm/interp.data`), because `index.html` and `wasm/` aren't
  siblings in this repo's layout. Fixed by passing `locateFile` to
  `createInterpModule()` in `main.js`, resolved against
  `import.meta.url` rather than the page's own URL.

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
