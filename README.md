# web-csv

CSV analysis in the browser: upload a CSV, then query and transform it
with [repl2](https://github.com/nchakicherla/repl2)'s configurable-grammar
tree-walking interpreter, compiled to WASM. Numeric *and* categorical
columns are supported (`col()`, `filter_gt()`, `groupby()` with
sum/count/avg/min/max aggregates). Parallelizable operations (currently:
summing a numeric column) run as WebGPU compute shaders on eligible
hardware, falling back to plain WASM otherwise. Results render as bar
charts, stat tiles, or tables (`web/src/charts/`, no chart library - plain
SVG/DOM against the dataviz skill's reference palette), and a query can be
saved as a dashboard tile, assembled with others, and persisted. A small
Node/SQLite service persists saved queries and dashboards; the compute
path itself needs no backend.

**New here?** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains how the
whole thing works from the data model up, assuming a data-analysis
background rather than a compilers/GPU one - including a worked
walkthrough of one query through every layer, why columnar storage and
dictionary encoding matter, and a suggested reading order through the
code. It also carries the design rationale, the tradeoffs, and what's
verified vs. not.

This is a proof of concept, not a finished app - see "First build
checklist" below for what's proven to work.

**Status:** builds against real Emscripten (`emcc` 6.0.5) and has been run
end to end in a real browser with real WebGPU hardware - CSV upload
through the actual UI, a query using `filter_gt`/`sum`/`gpu_sum`/`emit`,
(separately, with a large enough column to clear the GPU eligibility
threshold) a real dispatch through `reduce_sum.wgsl` on a real
`GPUDevice`, and the persistence server (`npm install && npm start`,
saving/listing a query through the real UI code path against a real
SQLite file) - all returning correct, hand-checked results. Every item on
the checklist below is now verified; see "Known gaps" for what's still
deliberately unbuilt.

## Layout

```
interp/vendor/repl2/   vendored repl2 core + one small patch (see VENDORED.md)
interp/ext/            web-csv's C layer: column store, GPU-aware builtins,
                       the Emscripten entry point, and its build (Makefile)
interp/ext/test/        native C tests + the Node/wasm smoke test
resources/             grammar-csv.txt, the default query/scripting grammar
web/                   browser UI, CSV parsing, WebGPU bridge
web/src/charts/         bar chart / stat tile / table renderers + theme.css
web/src/dashboard.js     tile add/remove/run/save/load
web/src/csv/parse.test.js  CSV parser unit tests
web/src/charts/format.test.js  chart number-formatting unit tests
server/                persistence API (saved queries/dashboards) + static host
server/test/             API integration tests
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
query in the textbox as-is:

- `filter_gt(col("amount"), 100)` then `sum` -> `8183.23`
- `gpu_sum(col("amount"))` (falls back to CPU at this size - see the
  eligibility gate in ARCHITECTURE.md) -> `8512.01`
- `groupby(col("category"), col("amount"), "sum")` -> `{groceries: 177.1,
  electronics: 1289.43, coffee: 62.23, rent: 2075.25, utilities: 580,
  travel: 4328}` (emitted as `{type:'groups', labels, values}`, not a
  plain object - see `agg` for which aggregate ran)
- `groupby(col("category"))` (1-arg form) -> counts per category without
  needing a numeric column: `{groceries: 4, electronics: 3, coffee: 5,
  rent: 2, utilities: 3, travel: 3}` (sums to 20, the row count)
- `emit(col("category"))` -> all 20 category values, in row order,
  resolved from the dictionary (`{type:'column', dtype:'string',
  values:[...]}`) - not summarized, unlike `emit(col("amount"))` for a
  numeric column, which now also streams every value rather than a sum

All hand-checked and matching what a real browser run actually returned.

## Testing

```bash
make test
```

Runs everything: the native C suite (`interp/ext/test`, grammar/native-hook/
column-store/builtin logic - no `emcc` needed, ~instant), a Node smoke test
against the real `emcc` build (`wc_init`/`wc_run`, plus a forced-GPU-path
check that Asyncify actually suspends/resumes around a real async
boundary - skips itself with a clear message if `interp/ext`'s `make`
hasn't been run yet), the CSV parser's and chart-formatting unit tests,
and the server's API integration tests (real Express + a throwaway SQLite
file per run, needs `npm install` in `server/` first). Each suite also runs standalone - see
the `Makefile` at the repo root for the individual targets
(`test-c`/`test-wasm`/`test-web`/`test-server`).

This locks in everything the "First build checklist" below verified by
hand originally, as an automated regression suite - what it deliberately
does *not* cover is the real WGSL shader/`GPUDevice` path, since that
needs a real browser with WebGPU and there's no headless-browser-with-GPU
setup in this repo (yet), and the chart/dashboard DOM code
(`web/src/charts/bar.js`/`table.js`/`stat.js`/`dashboard.js`) - there's no
jsdom (or similar) dependency in this project to unit-test DOM
construction without a real browser, so only `format.js`'s pure
number-formatting functions get `node --test` coverage; the rendering
itself is verified the same way the WebGPU path is, by hand in a real
browser - see the checklist's items 5 and 9.

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
6. ✅ `cd server && npm install && npm start` works - `better-sqlite3`
   installed from a prebuilt binary (no native compile needed), the
   server serves `web/` correctly, and `/api/queries`/`/api/dashboards`
   round-trip through a real SQLite file (`server/data/web-csv.sqlite`,
   WAL mode) with correct per-user isolation (a saved query under one
   `x-user-id` correctly doesn't show up when listing under another).
   One real integration bug found and fixed: `web/src/api/client.js`
   never actually sent the `x-user-id` header `auth.js` requires, so the
   "Save query" button 401'd unconditionally - see "Bugs found".
7. ✅ Categorical columns (`wc_load_column_str_dict`) and `groupby()`
   (sum/count/avg/min/max) work correctly - verified three ways: the
   native C suite (dictionary encoding, group aggregation math), a Node
   script against the real `emcc` build exercising the actual
   `wcEmitGroups`/`UTF8ToString` JS path, and the real browser/UI,
   uploading the sample CSV's `category` column and running
   `groupby(col("category"), col("amount"), "sum")` - every category's
   sum matched hand-computed values exactly.
8. ✅ `emit()` on a column now streams every value instead of summarizing
   it (`wcEmitNumberArray`/`wcEmitStringArray`), and `groupby(cat_col)`
   (1-arg, count-only, no numeric column needed) both work correctly -
   verified the same three ways as #7, including in the real browser
   against the sample CSV: `emit(col("category"))` returned all 20
   values in exact row order, `groupby(col("category"))` returned counts
   summing to 20.
9. ✅ The dashboard UI works end to end in a real browser: a query's
   results render as the right chart form (stat tile for a number, bar
   chart for a `groupby` result, table for a raw column) with correct
   values, hover tooltips, and a working table-view toggle; adding a tile,
   running the whole dashboard, saving it, reloading the page, loading it
   back from the saved-dashboards dropdown, and running it again all
   round-tripped correctly through the real persistence API - same
   `8183.23`/`8512.01`/per-category values every time.

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
- `client.js`'s `fetch` calls to `/api/queries`/`/api/dashboards` never
  sent an `x-user-id` header, so every request 401'd against `auth.js`'s
  stub auth - the UI's "Save query" button was silently broken. Fixed by
  generating a stable per-browser dev identity (`localStorage`) and
  sending it on every request; see `client.js`'s `getDevUserId`.

## Known gaps

Deliberate scope cuts, each with a reason it's staying that way for now
rather than a TODO waiting to be picked up:

- Dashboards persist queries, not data - loading a saved dashboard doesn't
  restore whatever CSV was loaded when it was built, and running it
  against no CSV (or a different one, with different column names) fails
  the way any query without the right columns loaded would. Persisting a
  CSV snapshot alongside a dashboard is a real feature, not attempted here.
- A tile's query is fixed once added - there's no in-place editor on the
  dashboard, only "Remove" and re-add via the query box above. Editable
  tiles are a natural follow-up once the tile card has a reason to be more
  than a display.
- Only three chart forms exist: stat tile (a bare number), bar chart (a
  `groupby` result), and table (a raw column or the accessibility twin of
  a bar chart). There's no line/time-series chart, because there's no
  date/time column type yet to plot against - see the column-types gap.
- Loading a saved dashboard doesn't auto-run it (a deliberate choice: the
  CSV needs to be loaded first, and running immediately against nothing
  loaded would just error) - the user has to click "Run dashboard"
  afterward, which is one extra click but avoids a confusing failure.
- Only `sum` has a GPU path. `filter_gt`/`groupby` are CPU-only; GPU
  filter/groupby/sort/join are all real engineering effort (see
  ARCHITECTURE.md's shader-scope note) - a project on their own, not a
  quick follow-up.
- `groupby()`'s dictionary build (`columnCreateStrDict`) is an O(n *
  distinct_values) linear scan against the dict-so-far, not a hash table -
  fine for a CSV's worth of categories (tens to low hundreds), not for
  high-cardinality columns. A performance concern, not a correctness one,
  and not yet benchmarked as an actual problem.
- `groupby()` emits its result directly rather than returning a value the
  script can keep composing with (same role `print`/`emit` already have) -
  `sum(groupby(...))` isn't a thing. A dedicated result type that could
  carry labels alongside values would remove this limitation - better
  designed once the dashboard UI defines what "chartable data" actually
  needs to look like than guessed at ahead of that.
- `ASYNCIFY=1` instruments the whole interpreter rather than the narrower
  `ASYNCIFY_ONLY` call path - fine for correctness (verified working, see
  the checklist), worth tightening once there's a reason size/speed
  actually matters here.
- Auth is a localhost-only stub (`server/src/routes/auth.js`) - real auth
  (sessions vs. OAuth/SSO vs. something else) is a decision with
  tradeoffs that needs to be made deliberately, not defaulted to by
  scaffolding code.
- `resources/grammar-csv.txt` is a function-call-style DSL, not SQL - see
  ARCHITECTURE.md's grammar section for why a SQL surface needs new
  interpreter semantics, not just a new grammar file - a genuinely
  separate feature, not a gap in the current one.
