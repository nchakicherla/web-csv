# web-csv

CSV analysis in the browser: upload a CSV, then query and transform it
with [repl2](https://github.com/nchakicherla/repl2)'s configurable-grammar
tree-walking interpreter, compiled to WASM. Numeric, categorical, *and*
date columns are supported (`col()`, `filter_gt()`, `groupby()` with
sum/count/avg/min/max aggregates, `date()`/`date_part()` for parsing and
bucketing dates by year/month/day/weekday). Parallelizable operations (currently:
summing a numeric column) run as WebGPU compute shaders on eligible
hardware, falling back to plain WASM otherwise - as either `gpu_sum()`
(fast, f32 - see the precision tradeoff below) or `gpu_sum_exact()`
(same GPU parallelism, but exact, for money). Results render as bar
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
rows, `id`/`date`/`amount`/`category` columns. Upload it and run the
default query in the textbox as-is:

- `filter_gt(col("amount"), 100)` then `sum` -> `8183.23`
- `gpu_sum(col("amount"))` (falls back to CPU at this size - see the
  eligibility gate in ARCHITECTURE.md) -> `8512.01`
- `groupby(col("category"), col("amount"), "sum")` -> `{groceries: 177.1,
  electronics: 1289.43, coffee: 62.23, rent: 2075.25, utilities: 580,
  travel: 4328}` (emitted as `{type:'groups', labels, values}`, not a
  plain object - see `agg` for which aggregate ran)
- `groupby(date_part(col("date"), "month"), col("amount"), "sum")` ->
  `{2024-01: 257.49, 2024-02: 1264.34, 2024-03: 1779.5, 2024-04: 4358.49,
  2024-05: 852.19}` - `date_part()` turns the loaded `date` column (parsed
  as UTC epoch seconds by `parse.js`, not a string) into per-row `"YYYY-MM"`
  labels, which is what makes it groupable at all; see "Date/time columns"
  below.
- `groupby(col("category"))` (1-arg form) -> counts per category without
  needing a numeric column: `{groceries: 4, electronics: 3, coffee: 5,
  rent: 2, utilities: 3, travel: 3}` (sums to 20, the row count)
- `emit(col("category"))` -> all 20 category values, in row order,
  resolved from the dictionary (`{type:'column', dtype:'string',
  values:[...]}`) - not summarized, unlike `emit(col("amount"))` for a
  numeric column, which now also streams every value rather than a sum

All hand-checked and matching what a real browser run actually returned.

### Date/time columns

A column loads as a date, not a plain string, when every non-empty cell
matches `YYYY-MM-DD` (optionally with a `T`- or space-separated
`HH:MM[:SS]`) - `parse.js`'s `DATE_RE`. It's converted to UTC epoch seconds
*in JS* (`Date.UTC(...)` on the regex-captured fields, not `Date.parse()` -
see the comment on `parseIsoDateToEpochSeconds` for why: `Date.parse()`
treats a date-*time* string with no explicit offset as the browser's local
time zone per spec, which would make the same CSV load different data
depending on where it's opened) before it ever reaches WASM, so the C side
never parses a date string at all except for a script's own `date("...")`
literals.

Two new builtins, both in `interp/ext/builtins_gpu.c`/`interp/ext/datetime.c`:

- `date("2024-06-01")` -> a plain number (epoch seconds), for building a
  comparison threshold: `filter_gt(col("order_date"), date("2024-06-01"))`
  works today because `filter_gt` now accepts date columns as well as f64
  (comparing epoch-seconds numbers is exactly comparing dates).
- `date_part(col, "year" | "month" | "day" | "weekday")` -> a new
  categorical column, one formatted label per row (`"2024"`, `"2024-06"`,
  `"2024-06-01"`, `"Sat"`). This is what makes a date column composable
  with `groupby()`, which needs a categorical column to group by -
  `date_part(col("order_date"), "month")` is the "group by month" a real
  BI tool would call a native operation, built here from two small pieces
  instead of one bespoke one.

`sum()` explicitly rejects a date column (summing epoch seconds isn't
meaningful) rather than silently returning whatever `cpuSum()`'s
unrecognized-type fallback would - see `interp/ext/test/test_builtins.c`'s
`test_sum_rejects_date_columns`.

### A larger CSV, to actually exercise the GPU path

The 20-row file above can't clear `WC_GPU_MIN_LEN` (50,000 - see
ARCHITECTURE.md's eligibility gate), so `gpu_sum` always takes the CPU
fallback against it - same correct answer, but the WGSL shader never
actually runs.
[web/sample-data/transactions-large.csv](web/sample-data/transactions-large.csv) -
100,000 rows, `id`/`date`/`month`/`category`/`region`/`channel`/`quantity`/`amount` -
is big enough to force the real dispatch. It's generated
deterministically by
[web/sample-data/generate.mjs](web/sample-data/generate.mjs) (fixed-seed
PRNG, so regenerating it reproduces the exact same file byte for byte);
regenerate or resize it with `node generate.mjs <rows> <outfile>`.

Upload it and run:

```
emit(sum(col("amount")));
emit(gpu_sum(col("amount")));
emit(gpu_sum_exact(col("amount")));
groupby(col("category"), col("amount"), "sum");
groupby(col("month"));
groupby(date_part(col("date"), "month"), col("amount"), "sum");
```

Expected, verified two independent ways (a plain-JS reduce over the raw
CSV, and the real interpreter under Node) before ever touching a browser,
then confirmed a third way in the real browser:

- `sum(col("amount"))` -> **`26559531.420000315`** - "exact" in the sense
  of not losing precision to the GPU path, but note it isn't a clean
  `.42` either: ordinary f64 addition has its own small representation
  noise summing 100,000 decimal amounts (money is exactly where floats in
  general are a known footgun, not just the f32-specific GPU issue below).
- `gpu_sum(col("amount"))` -> **a close but *different* number** (in one
  real run, `26559531.3515625`) - and that's correct, not a bug: past the
  threshold this genuinely dispatches through `reduce_sum.wgsl` on the
  GPU, which sums in f32 (WGSL has no f64 - see ARCHITECTURE.md §8), so
  visible drift on a sum this large is expected. If it ever comes back
  bit-identical to the CPU sum, that's the more suspicious result - it'd
  suggest the CPU fallback silently ran instead.
- `gpu_sum_exact(col("amount"))` -> **`26559531.42`** - clean, and
  genuinely exact, not merely close: it converts every value to integer
  cents before upload and sums as `i32` on the GPU (`reduce_sum_i32.wgsl`),
  and integer addition doesn't round. Confirmed to match a real dispatch
  through actual `GPUBuffer`s, not the CPU fallback - see ARCHITECTURE.md
  §8's "Fixing gpu_sum's precision, for money" for how and why this works
  and what it assumes.
- `groupby(col("category"), col("amount"), "sum")` -> `{coffee: 188033.92,
  groceries: 3433538.13, utilities: 2156688.61, dining: 1158195.76,
  transport: 676631.37, electronics: 9594373.57, rent: 3451725.51,
  travel: 5900344.55}`
- `groupby(col("month"))` -> 12 groups, `jan`..`dec` in that order (the
  generator writes rows chronologically, and dictionary order is
  first-seen order - see ARCHITECTURE.md §5), each ~8,333 (100,000 / 12)
- `groupby(date_part(col("date"), "month"), col("amount"), "sum")` -> 12
  groups, `2025-01`..`2025-12`, **the same row split as `groupby(col("month"))`
  above, bucket for bucket** (8334, 8333, 8333, 8334, ...) - confirming
  `date_part()` derives the identical grouping from the real `date` column
  that `generate.mjs` used to *write* the redundant `month` column in the
  first place (see that file's own comment, now closed by this feature).
  Sums: `{2025-01: 2219094.36, 2025-02: 2143173.97, 2025-03: 2211123.01,
  2025-04: 2261224.10, 2025-05: 2240433.42, 2025-06: 2270369.99,
  2025-07: 2213446.68, 2025-08: 2137260.87, 2025-09: 2183450.03,
  2025-10: 2292573.16, 2025-11: 2197190.72, 2025-12: 2190191.11}`

**A real bug turned up building this file** - large enough data to be
worth recording. Loading `transactions-large.csv` through the UI crashed
with `RuntimeError: memory access out of bounds` while loading the
categorical columns. Cause: `main.js`'s `loadStringColumn` passed the
whole joined-values string as a `ccall` `'string'`-typed argument, which
Emscripten marshals through a *stack* allocation (a fixed, small default -
64KB) rather than the heap - fine for the 20-row file's short strings,
silent memory corruption once a column's every-row-value string reaches
the hundreds of KB a 100k-row column produces. Fixed by allocating that
one argument on the heap explicitly (`_malloc` + `stringToUTF8`, both now
in `interp/ext/Makefile`'s `EXPORTED_RUNTIME_METHODS`) instead of relying
on `ccall`'s automatic string marshaling. Covered by a new regression test
in `interp/ext/test/wasm_smoke.test.mjs` (a 60,000-row categorical column,
well past the 64KB stack) so a regression back to the `ccall` shortcut
fails a test instead of a real upload.

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
10. ✅ A 100,000-row CSV (`sample-data/transactions-large.csv`) loads and
    queries correctly - large enough to actually clear `WC_GPU_MIN_LEN`
    and force `gpu_sum` through the real GPU dispatch rather than the CPU
    fallback, confirmed by its result genuinely differing from the exact
    CPU sum (real f32 rounding drift - see "A larger CSV" above). Every
    value cross-checked against an independent plain-JS computation over
    the raw file before ever touching the interpreter.
11. ✅ `gpu_sum_exact()` - the integer-cents GPU path that fixes #10's
    drift for money - works correctly, verified the same three ways as
    every other builtin, plus one more: the real browser test explicitly
    confirmed dispatch reached the actual `reduceSumExact` bridge function
    (not just inferred it from the result), and its GPU-computed result
    on a hand-checkable dataset (`-300`) matched hand calculation exactly.
    Against `transactions-large.csv` it returned a clean `26559531.42`
    where both `sum` and `gpu_sum` show floating-point noise of different
    kinds - see "A larger CSV" above.
12. ✅ Date/time columns (`COL_DATE`, `date()`/`date_part()`) work
    end to end in a real browser: `parse.js` detects and correctly parses
    ISO date columns in both sample CSVs as UTC epoch seconds; `date_part(col,
    "month")` composed with `groupby()` reproduces the exact same per-month
    row counts as the hand-maintained `month` categorical column on the
    100,000-row CSV (bucket for bucket - see "A larger CSV" above); a date
    before the Unix epoch round-trips correctly through `emit()`
    (`wcFormatDatePart`'s `floor()`, not truncation); `filter_gt()` on a
    date column against a `date("...")` threshold correctly keeps only
    later dates; and `sum()` on a date column fails loudly rather than
    returning a silently meaningless 0.

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
- `main.js`'s `loadStringColumn` passed a whole column's joined values as
  a `ccall` `'string'`-typed argument, which Emscripten marshals through a
  *stack* allocation (a fixed, small default - 64KB) rather than the heap.
  Fine at 20 rows; a 100,000-row categorical column's joined string is
  megabytes, and reliably crashed with `RuntimeError: memory access out of
  bounds` - real memory corruption, not a clean error. Fixed by allocating
  that argument on the heap explicitly (`_malloc` + `stringToUTF8`) instead
  of relying on `ccall`'s automatic marshaling; see "A larger CSV" above.

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
  a bar chart). There's now a real date column type (`COL_DATE` -
  `date()`/`date_part()`, see "Date/time columns" above) to plot against,
  but no dedicated line/time-series chart yet - a `date_part()` +
  `groupby()` result renders as an ordinary bar chart today (which is
  arguably still the right form for a handful of monthly buckets; it stops
  being the right form once "date range" means finer-grained points than a
  bar chart reads well as).
- Loading a saved dashboard doesn't auto-run it (a deliberate choice: the
  CSV needs to be loaded first, and running immediately against nothing
  loaded would just error) - the user has to click "Run dashboard"
  afterward, which is one extra click but avoids a confusing failure.
- Only `sum` has a GPU path (as `gpu_sum` and, exactly, `gpu_sum_exact`).
  `filter_gt`/`groupby` are CPU-only; GPU filter/groupby/sort/join are all
  real engineering effort (see ARCHITECTURE.md's shader-scope note) - a
  project on their own, not a quick follow-up.
- `gpu_sum_exact()` assumes at most 2 meaningful decimal places (it rounds
  to the nearest cent - the standard accounting assumption, but a real one:
  sub-cent data would be silently rounded), and its i32-bounded GPU path
  tops out around ±$21.47M in cents - past that (checked via a conservative
  sum-of-absolute-values bound, not the actual total, so it triggers a bit
  before the true limit) it transparently falls back to the same exact
  answer computed on CPU, never an overflowed one, just not GPU-accelerated
  for that column.
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
