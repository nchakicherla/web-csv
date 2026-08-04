# Architecture

This document explains how web-csv works, from the data model up. It is
written to be readable if you know data analysis (spreadsheets, pandas,
SQL) but not compilers, WebAssembly, or GPU programming — the jargon those
layers need is introduced as it comes up, and §2 is a glossary you can
jump back to.

**Contents**

1. [What this is, and why it's shaped this way](#1-what-this-is-and-why-its-shaped-this-way)
2. [Vocabulary](#2-vocabulary)
3. [One query, end to end](#3-one-query-end-to-end)
4. [Layers (file map)](#4-layers-file-map)
5. [The column store — the shared data model](#5-the-column-store--the-shared-data-model)
6. [The interpreter — how text becomes a running query](#6-the-interpreter--how-text-becomes-a-running-query)
7. [The builtin bridge — where the interpreter meets the data](#7-the-builtin-bridge--where-the-interpreter-meets-the-data)
8. [The GPU path](#8-the-gpu-path)
9. [Charts and the dashboard](#9-charts-and-the-dashboard)
10. [The server](#10-the-server)
11. [Design decisions and tradeoffs](#11-design-decisions-and-tradeoffs)
12. [What's actually verified vs. not](#12-whats-actually-verified-vs-not)
13. [Suggested reading order in the code](#13-suggested-reading-order-in-the-code)

---

## 1. What this is, and why it's shaped this way

You upload a CSV, type a query, and get numbers and charts back. That
much is ordinary. Three things underneath are not:

**Everything computes in your browser.** There is no "upload your data to
our servers" step — the CSV is read by JavaScript, handed to an analysis
engine compiled into the page, and never leaves your machine. The small
server in `server/` exists only to remember saved queries and dashboard
layouts; unplug it and the analysis still works.

**The query language is a swappable file, not compiled-in code.** The
thing that reads `groupby(col("category"), col("amount"), "sum")` and
turns it into a parse tree is driven by
[`resources/grammar-csv.txt`](../resources/grammar-csv.txt) — a plain text
file of grammar rules. Point the engine at a different grammar file and it
parses a differently-shaped language, with no recompile. That is the
feature the vendored [repl2](https://github.com/nchakicherla/repl2)
interpreter brings; a hand-written query parser would not have it.

**Heavy numeric work can run on the GPU.** Summing a large column
dispatches a *compute shader* — a small program that runs on thousands of
GPU threads at once — instead of a serial loop, when the hardware supports
it and the column is big enough to be worth it.

### How this compares to tools you already know

Be clear-eyed about this: **web-csv is a proof of concept, not a
competitor to pandas or DuckDB.** Those are mature, heavily optimized, and
support hundreds of operations; this supports six. What it offers instead:

| | pandas / DuckDB | web-csv |
|---|---|---|
| Where it runs | your machine, via Python/CLI | the browser tab, no install |
| Data leaves your machine? | no | no |
| Query language | fixed (pandas API / SQL) | **defined by a swappable grammar file** |
| Operations | hundreds | six (`col`, `sum`, `gpu_sum`, `filter_gt`, `groupby`, `emit`) |
| GPU acceleration | rarely / via extensions | built into the eligibility path |

The genuinely useful thing about reading this codebase is that it is a
*small* analytics engine with nothing hidden. pandas and DuckDB do
everything described in §5 and §7 — columnar storage, dictionary encoding,
grouped aggregation — but behind hundreds of thousands of lines. Here each
of those is a few dozen lines of C you can read in an afternoon.

---

## 2. Vocabulary

Terms this document uses that a data-analysis background wouldn't
necessarily cover. Skim now, refer back later.

**Columnar (column-oriented) storage** — storing a table as one array per
*column* rather than one record per *row*. See §5; this is the single most
important idea in the codebase.

**Dictionary encoding** — storing repeated text as small integer codes
plus one lookup table of the distinct values. pandas calls this a
`Categorical`; Parquet calls it a dictionary page. See §5.

**f64 / f32 / i32** — a 64-bit float ("double precision", what
spreadsheets and pandas use by default), a 32-bit float (half the
precision), and a 32-bit integer. The f64-vs-f32 distinction matters in
§8.

**WebAssembly (WASM)** — a binary instruction format browsers can run at
near-native speed. It lets code written in C (like this project's
interpreter) run in a web page instead of being rewritten in JavaScript.

**Emscripten / `emcc`** — the toolchain that compiles C to WebAssembly and
generates the JavaScript "glue" that loads it and lets JS call into it.

**Interpreter / tree-walking interpreter** — a program that runs source
code directly, rather than compiling it to machine code first. A
*tree-walking* one runs the program by recursively walking its parse tree.
See §6.

**Grammar / token / AST** — a grammar is the set of rules describing a
language's syntax; a token is one lexical atom (`groupby`, `(`, `"sum"`);
an AST (abstract syntax tree) is the tree those tokens parse into. See §6.

**Compute shader** — a program that runs on the GPU for general
computation rather than drawing graphics. **WebGPU** is the browser API
that dispatches them; **WGSL** is the language they're written in.

**Reduction** — collapsing many values into one (a sum, a min, a max).
"Parallel reduction" is the standard GPU technique for doing that with
many threads. See §8.

**Fixed-point representation** — storing a decimal number as a scaled
integer (`$42.50` as the integer `4250`, "cents") instead of a float.
Integer arithmetic has no rounding, so this trades a fixed, known
precision (whatever the scale factor covers - 2 decimal places, here) for
exactness - the classic reason financial software avoids floats. See §8's
`gpu_sum_exact`.

**Asyncify** — an Emscripten feature that lets compiled C code pause
mid-call, wait on a JavaScript promise, and resume. See §7.

**Arena allocation** — a memory strategy where many allocations come from
one big block that is freed all at once, instead of being freed
individually. repl2 uses one for parse trees.

**Epoch seconds** — the number of seconds since 1970-01-01 00:00:00 UTC
("the Unix epoch"), the same reference point `Date.now() / 1000` in
JavaScript or Python's `time.time()` use. This codebase's date columns
store dates this way (as an ordinary `f64`) rather than as text, so
comparing or bucketing dates is just comparing numbers. See §5's `COL_DATE`
and §8's date/time note.

---

## 3. One query, end to end

The fastest way to understand the system is to follow a single query
through every layer. Take this, typed into the query box:

```
groupby(col("category"), col("amount"), "sum");
```

### Step 0 — loading the CSV (happens first, once)

[`web/src/csv/parse.js`](../web/src/csv/parse.js) splits the file text
into lines and fields, then decides each column's type: if every value in
a column is numeric or empty, it's `f64`; otherwise it's `string`.

[`web/src/main.js`](../web/src/main.js) then pushes each column into the
WASM engine's memory — `wc_load_column_f64` for numbers,
`wc_load_column_str_dict` for text. Those live in the *column store* (§5)
under their CSV header name, which is what `col("category")` will look up.

### Step 1 — JS asks the engine to run the text

`main.js`'s `runQuery()` empties `Module.wcResults` (the array results get
pushed onto) and calls the exported C function `wc_run` with the query
string.

### Step 2 — text becomes a tree

`wc_run` ([`interp/ext/web_main.c`](../interp/ext/web_main.c)) hands the
string to repl2's parser. The scanner splits it into tokens using the
vocabulary the grammar file declared, and the parser builds an AST —
roughly:

```
FNCALL "groupby"
├── FNCALL "col"  └── STRLIT "category"
├── FNCALL "col"  └── STRLIT "amount"
└── STRLIT "sum"
```

### Step 3 — the tree gets walked

`interpExecEcho` walks that tree. Reaching the `groupby` call node it
calls `evalCall` ([`interp.c`](../interp/vendor/repl2/src/interp.c)),
which evaluates the arguments left to right first. Each `col(...)` is
itself a call, so this recurses: `doCol` looks the name up in the column
store and returns a handle to that column.

### Step 4 — a native function does the actual work

`evalCall` doesn't know what `groupby` means. It offers the name to a
*native hook* — a C callback this project registered — which routes it to
`doGroupby` in
[`builtins_gpu.c`](../interp/ext/builtins_gpu.c) (§7).

`doGroupby` validates its arguments, then makes **one pass over the rows**.
For each row it reads the category's integer code and uses it as an index
into per-group accumulator arrays:

```c
for (i = 0; i < len; i++) {
    int32_t code = codes[i];
    if (code < 0 || (uint32_t)code >= n_groups) continue;

    counts[code]++;                 /* always — this alone answers "count" */
    if (values) {                   /* NULL in the 1-arg count-only form   */
        sums[code] += values[i];
        if (values[i] < mins[code]) mins[code] = values[i];
        if (values[i] > maxs[code]) maxs[code] = values[i];
    }
}
```

That's the whole grouped-aggregation algorithm. Note that it accumulates
*all* the aggregates in the single pass and picks the requested one
afterward — cheaper than branching on `agg` per row. `avg` is then just
`sums[g] / counts[g]`.

It works with no sorting and no hash lookups *because* the category column
is dictionary-encoded — the code **is** the array index. This is the
payoff of the storage decision in §5.

### Step 5 — the result crosses back into JavaScript

A grouped result is two parallel arrays: group labels and one aggregate
per group. It's shipped back by `wcEmitGroups`, which decodes the labels
and pushes `{type: 'groups', agg: 'sum', labels: [...], values: [...]}`
onto `Module.wcResults`.

### Step 6 — the shape picks the chart

`wc_run` returns; `runQuery` hands `wcResults` to `renderResults`
([`charts/render.js`](../web/src/charts/render.js)), which dispatches on
each result's *shape*: a `groups` object becomes a bar chart, a bare
number becomes a stat tile, a whole column becomes a table (§9).

**Every layer in this project appears in that path**, which is why it's
worth reading once before the sections below.

---

## 4. Layers (file map)

```
web/                      the browser app — no build step, plain ES modules
  index.html               the page itself
  sample-data/             transactions.csv (20 rows, incl. a date
                           column) and transactions-large.csv (100k,
                           generate.mjs)
  src/
    csv/parse.js            CSV text -> typed columns          (§5)
    csv/parse.test.js        its unit tests
    gpu/device.js           one shared WebGPU device           (§8)
    gpu/bridge.js            the JS half of the async boundary (§7, §8)
    gpu/shaders/*.wgsl       the compute shaders               (§8)
    charts/render.js        picks a chart form by result shape (§9)
    charts/bar.js            SVG bar chart
    charts/stat.js           stat tile
    charts/table.js          table + every chart's table view
    charts/format.js         number formatting (+ .test.js)
    charts/theme.css         color/spacing tokens, light + dark
    dashboard.js            tile add/remove/run/save/load       (§9)
    api/client.js           talks to server/                    (§10)
    main.js                 wires it all together
    wasm/                   emcc build output (gitignored)

interp/                   the analysis engine, written in C
  vendor/repl2/src/         vendored repl2 interpreter          (§6)
                             + one local patch (VENDORED.md)
  ext/                      web-csv's own C layer
    column.h/.c              the column type                    (§5)
    datetime.h/.c            civil-calendar math for COL_DATE    (§5)
    store.h/.c               the session's live columns         (§5)
    builtins_gpu.h/.c        col/sum/gpu_sum/gpu_sum_exact/
                             filter_gt/groupby/date/date_part/
                             emit                                 (§7)
    web_main.c               entry points JS calls (wc_run, …)
    Makefile                 the emcc build
    test/                    native C tests + a WASM smoke test  (§12)

server/                   persistence only — never runs a query (§10)
  src/app.js, index.js, db.js, routes/
  test/api.test.js

resources/grammar-csv.txt the query language's grammar          (§6)
Makefile                  `make test` runs every suite          (§12)
```

---

## 5. The column store — the shared data model

### Why columnar

A CSV looks row-oriented on disk:

```
id,amount,category
1,42.50,groceries
2,199.99,electronics
```

Store it that way in memory — an array of record objects — and summing
`amount` means visiting 20 separate objects and pulling one field out of
each. The values you want are scattered, each behind a pointer, each
interleaved with data you don't want.

Store it *columnar* — one contiguous array per column — and `amount`
becomes `[42.50, 199.99, …]`, one packed block of doubles. Now summing is
a tight loop over adjacent memory. That matters for three reasons:

1. **Cache locality.** CPUs fetch memory in blocks. Adjacent values mean
   every fetch delivers useful data instead of mostly-unwanted neighbors.
2. **You only touch the columns you use.** A query over `amount` never
   reads `category` at all.
3. **It's the only shape a GPU can use.** A compute shader wants a flat
   buffer of numbers (§8). A columnar array already *is* one — no
   conversion step.

This is not a web-csv invention: it is why pandas stores DataFrames as
per-column NumPy arrays, why Parquet is a columnar file format, and why
DuckDB is a columnar engine. web-csv just does it small enough to read.

### The four column types

[`interp/ext/column.h`](../interp/ext/column.h):

| Type | Holds | Used for |
|---|---|---|
| `COL_F64` | `double*` | numeric columns |
| `COL_I32` | `int32_t*` | integer columns (defined, not yet produced by the CSV path) |
| `COL_STR_DICT` | `int32_t*` codes + `char**` dictionary | text / categorical columns |
| `COL_DATE` | `double*` (epoch seconds, UTC) | date / date-time columns |

`COL_DATE` is physically identical to `COL_F64` — same `double*` storage,
same `columnDataF64()` accessor returns it for both — it's a distinct *tag*
so builtins can tell "a number" from "a date that happens to be stored as
a number." That's not a wasted distinction: `sum()` refuses a `COL_DATE`
column (summing epoch seconds isn't meaningful) where it would silently
sum a `COL_F64` one, `date_part()` requires `COL_DATE` and rejects
`COL_F64`, and `filter_gt()` accepts *both* (comparing epoch-seconds
numbers is exactly comparing dates, so the same `>` predicate is correct
for either). `interp/ext/datetime.c` holds the actual calendar math this
type needs — parsing an ISO string to epoch seconds and formatting epoch
seconds back to a calendar part (year/month/day/weekday) — kept separate
from `column.c` because it's civil-calendar arithmetic, not storage.

### Dictionary encoding, concretely

The sample CSV's `category` column has 20 rows but only 6 distinct values.
Storing 20 strings would mean 20 separate allocations and 20 pointer-chases
per pass. Instead, `columnCreateStrDict` assigns each distinct value a code
in first-seen order:

```
dictionary: ["groceries", "electronics", "coffee", "rent", "utilities", "travel"]
                  0             1            2        3         4          5

rows:       groceries  electronics  coffee  rent  groceries  coffee  …
codes:  →       0            1         2      3       0         2    …
```

The column now holds one flat `int32` array. Three consequences:

- **It's smaller** — 4 bytes per row instead of a pointer plus the string.
- **Comparisons are integer comparisons** — no `strcmp` per row.
- **Grouping is free.** As §3 step 4 showed, the code *is* the accumulator
  array index. No hash table, no sort. This is exactly why pandas
  `Categorical` speeds up `groupby`.

The cost, honestly: building the dictionary is currently an O(n ×
distinct) linear scan (each new value is compared against the dictionary
so far). Fine for tens or hundreds of categories; a hash table would be
the fix for a high-cardinality column, and isn't built yet.

The `\x1f` in `wc_load_column_str_dict` is a plumbing detail: JS can't
hand C an array of strings directly, so values are joined with ASCII Unit
Separator (0x1F) — a control character real CSV text essentially never
contains, so no escaping scheme is needed — and split back apart in C.

### Lifetimes and the store

Columns are `malloc`/`free`'d rather than arena-allocated (§2) because
their lifetime is "until the CSV is replaced," which doesn't line up with
any single query run. [`store.c`](../interp/ext/store.c) holds two kinds:

- **named** — CSV columns, reachable as `col("amount")`. Loading the same
  name again frees the old one and replaces it.
- **tracked** — intermediates a builtin created (e.g. `filter_gt`'s
  output). Not reachable by name, but owned by the store so they're freed
  when the session ends instead of leaking.

The interpreter carries a column as a generic pointer value tagged with a
magic number (`objColumn` / `objAsColumn`), which is how a builtin can
take a column argument without repl2's own value type needing to know
columns exist.

---

## 6. The interpreter — how text becomes a running query

### The pipeline

```
grammar file ─┐
              ├─► registry (token vocabulary + rule tree)
query text  ──┘        │
                       ▼
            scanner ─► tokens ─► parser ─► AST ─► tree-walking evaluator
```

**The scanner** turns characters into tokens. What counts as a token is
not hardcoded — it's built at load time from what the grammar file
declared, which is why a grammar can introduce syntax (`:=`, `let`) the C
source has never heard of.

**The parser** matches tokens against the grammar's rules and builds an
AST. Each node carries a tag (`STX_FNCALL`, `STX_EXPR`, `STX_INIT`, …).

**The evaluator** (`interp.c`) walks that tree recursively and does what
each tag means: an `STX_INIT` node binds a variable, an `STX_FNCALL` node
calls a function, an `STX_EXPR` node computes a value. That recursive walk
is what "tree-walking interpreter" means — no bytecode, no machine-code
generation, just recursion over the tree.

### What the grammar file can and cannot change

This is the most commonly misunderstood part of the design, so it's worth
being precise.

The evaluator switches on a **fixed set of built-in tags**. A grammar file
defines *concrete syntax* — what the language looks like — that produces
those tags. So a grammar can freely change:

- keywords and operators (`let x := 5` vs `int x = 5`)
- statement shapes (braces required or not, parens around `if` or not)
- the start symbol and overall program structure

What a grammar **cannot** do is invent new runtime meaning. Writing a
`STX_SELECT` rule for SQL-style `SELECT sum(amount) FROM t` would parse
fine and produce a tree — and then do nothing, because no case in the
evaluator's switch handles that tag. Adding SQL means editing the
evaluator too; repl2 has no macro or desugaring system that would let a
grammar rewrite `SELECT …` into nested function calls on its own. That's
real interpreter work, and it's why the SQL surface is listed as a
separate feature rather than a quick win.

### Session semantics

One parser and one interpreter are created at startup and reused for every
run, so state persists across queries like a notebook:

```
run 1:  let x := 5;
run 2:  emit(x + 1);   →  6
```

---

## 7. The builtin bridge — where the interpreter meets the data

### The native-function hook

repl2 knows nothing about CSVs, columns, or GPUs. web-csv adds one small
patch to it (documented in
[`interp/vendor/VENDORED.md`](../interp/vendor/VENDORED.md)): a hook that
`evalCall` consults before falling through to user-defined functions. Our
C code registers one callback, `wcNativeDispatch`, which matches on the
function name:

| Builtin | What it does |
|---|---|
| `col("name")` | look a column up in the store |
| `sum(col)` | CPU sum of a numeric column |
| `gpu_sum(col)` | same, but eligible for the GPU path, f32, approximate (§8) |
| `gpu_sum_exact(col)` | same GPU eligibility, integer cents, exact (§8) |
| `filter_gt(col, n)` | new column of values greater than `n` (numeric or date columns) |
| `groupby(cat)` | count of rows per category |
| `groupby(cat, num, agg)` | `sum`/`count`/`avg`/`min`/`max` per category |
| `date(str)` | parse an ISO date string to epoch seconds |
| `date_part(date_col, unit)` | new categorical column of per-row labels (`"year"`/`"month"`/`"day"`/`"weekday"`) |
| `emit(x)` | send a value or a whole column back to JS |

The hook is deliberately generic — it knows nothing about columns — so
this patch is plausibly upstreamable to repl2 rather than being a
permanent fork.

### How results get back to JavaScript

Three `EM_JS` imports (C declarations whose bodies are JavaScript) push
onto `Module.wcResults`:

| Function | Pushes |
|---|---|
| `wcEmitNumber` | a bare number |
| `wcEmitNumberArray` / `wcEmitStringArray` | `{type:'column', dtype, values}` |
| `wcEmitGroups` | `{type:'groups', agg, labels, values}` |

`emit()` on a column streams **every value** rather than summarizing —
and for a categorical column it resolves each row's code back through the
dictionary, so you get the actual per-row text, not the distinct list.
Callers who want a summary ask for one explicitly with `sum()` or
`groupby()`.

`groupby` is an *output* operation, like `print` — it pushes its result
rather than returning one, so `sum(groupby(...))` isn't expressible. The
reason is that a grouped result is two parallel arrays and the
interpreter's value type has no way to carry a pair like that; giving it
one is a real change, deliberately deferred (§11).

### Asyncify — the part that isn't obvious

Here's the problem. Asking the GPU for a result is *asynchronous* in the
browser: you submit work, then `await` a promise. But the call stack
asking for it is compiled C:

```
wc_run → interpExec → execNode → evalNode → evalCall → doSum → "…await?"
```

C has no `await`. That whole stack is live on the WebAssembly call stack,
and JavaScript can't just suspend it.

**Asyncify** is Emscripten's solution: it rewrites the compiled code so
those functions can *unwind* — save their local state, return control to
the browser's event loop, and later *rewind* back to exactly where they
left off when the promise resolves. From the C code's point of view,
`wcGpuReduceSum(...)` looks like an ordinary blocking call that returns a
double.

The catch is that **every function on the path** to the async call needs
that instrumentation, not just the one making it. The build currently uses
`ASYNCIFY=1`, which instruments everything — correct, but it pays a size
and speed cost in functions that never touch the GPU path (the scanner,
the grammar loader). Narrowing it to `ASYNCIFY_ONLY` with an explicit
function list is a real available optimization; the list should come from
`emcc -s ASYNCIFY_ADVISE=1` rather than guesswork. Not done because
correctness is already there and nothing has measured a need.

---

## 8. The GPU path

### What a compute shader is doing here

A CPU sums a million numbers by visiting them one at a time. A GPU has
thousands of small cores and wants every one of them working at once — but
"add all these up" isn't obviously parallel, since a running total is a
single shared thing.

The standard answer is a **parallel reduction**, a tournament bracket:

```
values:  3   1   4   1   5   9   2   6
          \ /     \ /     \ /     \ /
step 1:    4       5      14       8
            \     /         \     /
step 2:        9              22
                 \          /
step 3:            31
```

Each step halves the number of live values, so a million values finish in
about 20 steps instead of a million, with every core busy at each step.

[`reduce_sum.wgsl`](../web/src/gpu/shaders/reduce_sum.wgsl) does exactly
this within each *workgroup* (a batch of 256 threads that share fast
memory), producing one partial sum per workgroup. Those partials — a few
thousand numbers at most — get read back and finished on the CPU, because
a second GPU pass isn't worth it for that few values, and WGSL has no
atomic float add to combine them on-GPU directly.

### The f32 problem — a real precision tradeoff

**WGSL has no f64.** Its numeric types are f32, i32, u32. So `bridge.js`
downcasts the column to f32 before uploading.

That is a genuine loss of precision, not a technicality. f32 carries about
7 significant decimal digits; f64 carries about 15. Summing many values,
or values spanning wide magnitudes, can visibly drift. This is the same
class of problem behind floating-point surprises in any analysis tool —
worth knowing about generally, not just here.

The mitigation is the eligibility gate below: the CPU path stays exact
f64, and it's what small or precision-sensitive sums take. For a large
sum that specifically needs to be *exact* - money - there's a stronger
fix than just falling back to CPU, below.

### Fixing gpu_sum's precision, for money

`gpu_sum_exact(col)` gets real GPU acceleration *and* an exact answer, by
sidestepping floating point entirely rather than trying to make f32 more
precise. The technique is **fixed-point representation**: instead of
storing `$42.50` as a float and hoping additions round kindly, store it as
the integer `4250` (cents) and do the arithmetic in integers. Integer
addition has no rounding step at all — `4250 + 1999` is `6249`, exactly,
every time, in any order, on any hardware. WGSL's `i32` is a real,
first-class type (unlike f64), so this works as native GPU arithmetic, not
an emulation trick.

`doGpuSumExact` (`builtins_gpu.c`):

1. Converts every value to cents (`llround(value * 100)`), building an
   `i32` array to upload — and, as a free side effect of that single pass
   over the column, also accumulates the *exact* total as an `int64_t`.
   One pass does double duty: preparing the GPU buffer and computing the
   CPU-exact answer are the same work.
2. Checks an overflow guard (next paragraph). If it fails, or the column
   doesn't clear the same `WC_GPU_MIN_LEN` threshold as `gpu_sum`, or GPU
   isn't available — it returns the already-computed exact CPU total
   directly. Unlike `gpu_sum`, there is no "CPU fallback" in the sense of
   a *different, slower* path: the CPU total was always right there.
3. Otherwise, it dispatches to
   [`reduce_sum_i32.wgsl`](../web/src/gpu/shaders/reduce_sum_i32.wgsl) —
   the same two-stage tree reduction as `reduce_sum.wgsl`, over `i32`
   instead of `f32` — and divides the result by 100 back into dollars.

**The overflow guard.** A tree reduction's *intermediate* partial sums
aren't bounded by the final total when values can cancel (a big charge and
a big refund summing to something small) — but they're always bounded by
the sum of every value's *absolute* value, in any grouping or order. So
checking `Σ|cents| ≤ INT32_MAX` (roughly ±$21.47M in cents) is enough to
guarantee no `i32` overflow anywhere in the reduction, not merely in the
answer. This is a conservative check — it can decline GPU dispatch for a
column that would actually have been fine — deliberately, since a
conservative-but-safe check is worth more here than a tighter one that's
harder to prove correct.

**Why the GPU and CPU answers are now guaranteed to match, not just
close.** Integer addition is associative — `(a+b)+c` and `a+(b+c)` give
identical results, exactly, unlike float addition, where rounding at each
step makes the order matter. That's *why* `gpu_sum`'s tree-shaped
reduction can disagree with a linear CPU sum: same values, different
grouping, different rounding. `gpu_sum_exact`'s tree reduction has no such
risk — a real dispatch, confirmed in the browser against a hand-checkable
dataset, returned a value that matched by-hand arithmetic exactly (see
README's "A larger CSV").

**What this assumes, honestly.** Two decimal places of meaningful
precision (cents) — sub-cent data is silently rounded, the standard
accounting assumption but a real one to know about. And the ±$21.47M
bound above, past which the answer is still exact, just not
GPU-accelerated for that column.

### The eligibility gate

Three conditions must *all* hold before work goes to the GPU (`doSum` and
`doGpuSumExact` in `builtins_gpu.c`):

1. **The operation suits the GPU** — a columnar reduction, not branchy
   per-row scripting. Only `sum` (as `gpu_sum` and `gpu_sum_exact`) is
   wired for this today.
2. **The column is large enough** — at least `WC_GPU_MIN_LEN` (50,000)
   elements. Below that the CPU wins, because uploading a buffer,
   dispatching a pipeline, and reading the result back all cost real time
   a plain loop doesn't. *This threshold is an unbenchmarked starting
   guess.*
3. **WebGPU is actually available** — `navigator.gpu` resolves an adapter.

`gpu_sum_exact` adds a fourth, its own overflow guard (previous section) -
none of these three conditions know or care about integer overflow, since
they're shared with `gpu_sum`'s float path, which has no such concept.

Fail any one and the exact CPU loop runs instead - f64 for `gpu_sum`, the
already-computed exact `int64_t` total for `gpu_sum_exact`. This is the general
lesson worth taking from the GPU section: parallel hardware has a fixed
setup cost, so it only pays off past a data-size threshold, and knowing
where that threshold is matters more than knowing the shader.

### Shader scope

Two shaders exist: `reduce_sum.wgsl` (f32, `gpu_sum`) and
`reduce_sum_i32.wgsl` (i32, `gpu_sum_exact`) — the same tree-reduction
structure, differing only in element type, which is also why
`bridge.js`'s `dispatchReduction` helper runs both rather than each having
its own copy of the buffer/dispatch/readback plumbing. A GPU filter
(predicate → mask) is the natural next one; a GPU `groupby` would extend
the same reduction idea, keyed by category code instead of one global
bucket. Sort and hash-join are established but genuinely substantial
techniques (bitonic sort networks, GPU hash joins) and are out of scope —
the CPU covers all of these today.

### Date/time columns

Not a GPU feature — dates are ordinary CPU-side epoch-seconds doubles
(`COL_DATE`, §5), and there's no `gpu_date_*` anything — but worth placing
next to the two precision sections above, because it makes the same kind
of "where does the number actually come from" decision they do.

The epoch-seconds conversion happens **in JS**, not in the WASM build:
`web/src/csv/parse.js` detects a date-shaped column (`DATE_RE`) and
converts every cell with `Date.UTC(...)` on regex-captured
year/month/day/hour/minute/second fields *before* the value ever reaches
`wc_load_column_date`. `Date.parse()` is deliberately not used for this —
per the ECMA-262 spec, a bare *date* string (`"2024-01-15"`) parses as
UTC, but a *date-time* string with no explicit offset (`"2024-01-15T08:30"`)
parses as the browser's **local** time zone. Relying on it would mean the
same CSV silently loading different epoch values depending on where it's
opened — exactly the kind of environment-dependent bug that's easy to
miss in development (one machine, one time zone) and only shows up for a
user somewhere else. Building the epoch value from explicit UTC fields
sidesteps the ambiguity entirely, and it's also why `datetime.c`'s
`wcParseDate` (which a script's own `date("...")` literal goes through)
implements the identical UTC interpretation independently in C rather
than calling any timezone-aware libc function — the two need to agree,
and "no timezone concept at all" is the simplest way to guarantee that.

The C-side calendar math (`interp/ext/datetime.c`) is Howard Hinnant's
`days_from_civil`/`civil_from_days` algorithm — closed-form arithmetic
that converts a (year, month, day) triple to a day count relative to the
epoch and back, correct for any proleptic-Gregorian year without a lookup
table or a loop. It's used both directions: `wcParseDate` (civil → epoch,
for `date("...")`) and `wcFormatDatePart` (epoch → civil, for
`date_part()` and for formatting a date column back to `"YYYY-MM-DD"` in
`emit()`). `wcFormatDatePart` floors rather than truncates when dividing
epoch seconds by 86400 specifically so a moment *before* the epoch
(negative epoch seconds) lands on the correct prior day instead of
rounding toward 1970-01-01 — exercised directly by
`test_builtins.c`'s date test, which loads `-86400` (one day before the
epoch) and checks it formats as `1969-12-31`, not `1970-01-01`.

---

## 9. Charts and the dashboard

### The result shape picks the chart

`charts/render.js` dispatches on the *shape* of each result, not on what
produced it:

| Result | Form | Why |
|---|---|---|
| a bare number | stat tile | one value has no axes to plot against |
| `{type:'groups'}` | bar chart | comparing magnitude across categories |
| `{type:'column'}` | table | row-level data has no natural chart form |

The same dispatch serves both the query box and every dashboard tile,
because a tile is just "a query, run, its results rendered."

### Why the bars are all one color

Charts are hand-rolled SVG (no chart library — `web/` has no build step),
following the `dataviz` skill's method. The color choice follows from what
the color is *doing*: these charts are single-series magnitude comparisons
— one aggregate per category — so color is doing a **sequential** job, not
an identity job. Every bar therefore takes one already-validated accent
hue rather than the eight-hue categorical palette.

That's also why no palette validation run was needed: validation checks
that *distinct* hues stay distinguishable under color-vision deficiency,
which is not a question a one-color chart raises. Coloring each bar
differently would have spent the identity channel re-encoding what bar
length already shows.

Each chart also ships a table view, a hover tooltip (value first, label
second), and hit targets larger than the bars themselves.

### What a dashboard is

A named list of tiles, each holding one query string. Saving persists
`{title, source}` pairs — **queries, not results**. So a loaded dashboard
always reflects whatever CSV is currently loaded, never a stale snapshot
baked in at save time.

The tradeoff is real and worth stating: load a dashboard without loading
its CSV first and its queries fail the same way any query missing its
columns would. There's no stored data to fall back on.

---

## 10. The server

`server/` is Express + SQLite and does exactly one job: remember saved
queries and dashboard layouts. It also serves `web/` as static files for
convenience. **It never runs a query** — all computation is client-side,
so the page works with the backend unreachable; you just lose saving.

Schema is three tables: `users`, `queries`, `dashboards`, each row owned by
a user id.

**Open decision, deliberately unresolved:**
[`server/src/routes/auth.js`](../server/src/routes/auth.js) is a
development stub — it trusts an `x-user-id` header with no login behind
it, and the browser generates a random per-browser identity for it. That
is fine on localhost and a real hole anywhere else. Choosing real auth
(sessions vs. OAuth/SSO vs. something else) is a decision with tradeoffs
that shouldn't be made implicitly by scaffolding code, so it wasn't.

---

## 11. Design decisions and tradeoffs

Collected in one place — each of these is a deliberate cut, not an
oversight.

| Decision | Why | What it costs |
|---|---|---|
| Vendor repl2 rather than submodule it | repo builds standalone; the one local patch lives in-tree | manual re-sync when repl2 moves |
| Columns `malloc`'d, not arena-allocated | lifetime is "until the CSV changes", not "until this query ends" | manual free discipline in C |
| Dictionary built by linear scan | simple; fine at CSV category counts | O(n × distinct); wrong for high-cardinality columns |
| `groupby` emits instead of returning | a grouped result is two parallel arrays; the value type can't hold a pair | `sum(groupby(...))` isn't expressible |
| 2-arg `groupby` rejected outright | genuinely ambiguous — is arg 2 the numeric column or the aggregate name? | one more arity to remember |
| `ASYNCIFY=1` (instrument everything) | correct with no analysis needed | size/speed cost in code that never awaits |
| GPU threshold at 50,000 | dispatch overhead must be earned back | unbenchmarked guess; may be wrong in either direction |
| GPU sums in f32 (`gpu_sum`) | WGSL has no f64 | precision drift on large/wide-ranging sums (§8) |
| `gpu_sum_exact` uses fixed-point (cents), not f32 | sidesteps float rounding entirely for money, rather than tolerating it | assumes ≤2 decimal places; i32-bounded to ±$21.47M in cents |
| Only `sum` has a GPU path | filter/groupby/sort/join on GPU are each real projects | everything else stays CPU-bound |
| Dashboards persist queries, not data | results always reflect current data | need the right CSV loaded before running |
| Dev-stub auth | real auth is the user's decision to make | unusable beyond localhost as-is |
| Function-call DSL, not SQL | SQL needs new evaluator semantics, not just a grammar (§6) | less familiar syntax for analysts |
| No chart library | `web/` has no build step; keeps deps at zero | chart features are hand-built |
| Dates stored as epoch-seconds `f64` (`COL_DATE`), not a string/struct | comparing/bucketing dates becomes plain number comparison; reuses `COL_F64`'s storage and `filter_gt` unchanged | no timezone concept at all — every date is UTC, always (§8's date/time note) |
| Date parsing done in JS (`parse.js`), not handed to WASM as raw strings | JS's `Date.UTC(...)` on regex-captured fields sidesteps `Date.parse()`'s spec-mandated local-time ambiguity for date-*time* strings | the C-side `wcParseDate` (for script `date(...)` literals) has to independently implement the same UTC interpretation, not share the JS logic |

---

## 12. What's actually verified vs. not

This project was built with a rule: claims here are things that were
actually run, not things that ought to work.

### Verified

**The C engine**, first against a native compiler standing in for
Emscripten, now against real `emcc` 6.0.5: grammar parsing, the native
hook, the column store, and every builtin, including dictionary encoding
and the aggregation math.

**The WASM build and Asyncify**, under Node against the real build output.
The Asyncify check specifically forced the GPU-eligible branch with a fake
`navigator.gpu` and a stub bridge holding a real timer, confirming the C
call stack genuinely suspends and resumes across an async JavaScript
boundary — not merely that the build didn't error.

**The real GPU path**, in a browser with a real WebGPU adapter: a column
pushed past the 50,000 threshold round-tripped through `reduce_sum.wgsl`
on a real `GPUDevice`/`GPUBuffer`. The first such test used uniform data
(all 1s) that happens to sum exactly in both f32 and f64, so it confirmed
the dispatch but not the precision tradeoff (§8) at all. A later run
against 100,000 rows of realistic, varied amounts (README's "A larger
CSV") showed the expected f32 drift directly: `gpu_sum` returned a value
close to but genuinely different from the exact f64 CPU sum — the correct
outcome, not a bug, and better confirmation than an exact match would have
been that the real shader ran rather than a fallback.

**`gpu_sum_exact`'s real GPU path**, verified more strongly than `gpu_sum`'s
since exactness is the whole point: a browser test explicitly confirmed
dispatch reached the actual `reduceSumExact` bridge function (not merely
inferred from the answer), and its result on a hand-checkable 60,000-value
dataset (`-300`) matched hand arithmetic exactly. Against
`transactions-large.csv` it returned a clean `26559531.42`, where both
`sum` (f64) and `gpu_sum` (f32) show floating-point noise of different
kinds - see README's "A larger CSV".

**Date/time columns**, in a real browser against both sample CSVs:
`parse.js` correctly detected and converted the ISO `date` column in each,
`date_part(col("date"), "month")` composed with `groupby()` produced the
exact same per-month row counts as the hand-maintained `month` categorical
column on the 100,000-row file — bucket for bucket, not just
approximately — confirming the two really do derive the same grouping
from the same underlying dates. `filter_gt()` against a `date("...")`
threshold, `emit()` round-tripping a date column back to ISO strings
(including a date before the Unix epoch, exercising `wcFormatDatePart`'s
`floor()`), and `sum()` correctly refusing a date column were all also
confirmed live, not just in the native/WASM test suites.

**The full UI**, in a real browser: CSV upload through the actual file
input, queries returning hand-checked values, each result shape rendering
as the right chart form, the hover tooltip and table-view toggle both
working, and the whole dashboard lifecycle — add a tile, run, save, reload
the page, load from the dropdown, run again — round-tripping correctly
through the real persistence API.

**The server**: `npm install && npm start`, both API routes round-tripping
through a real SQLite file, with per-user isolation confirmed.

Getting there surfaced five real bugs, all fixed and documented in the
README's "Bugs found": a missing `HEAPF64` export, `MODULARIZE=1` emitting
a CommonJS factory instead of an ES module, the preloaded grammar file
404ing against the wrong base URL, and the API client never sending the
auth header the server required — all four caught the first time any real
browser run was attempted, at any data size, since they were wiring bugs
rather than size-dependent ones. The fifth was different in kind: `ccall`'s
automatic string marshaling silently corrupting memory for a large
categorical column (it allocates on Emscripten's small, fixed stack rather
than the heap) only showed up once a realistic 100,000-row CSV was tried -
the original 20-row sample's short joined strings never got close to the
stack limit. See README's "A larger CSV" section for exactly why that file
exists, and the general lesson: a fixed small sample dataset can hide a
whole class of bug that only a real, sized dataset would ever surface.

### Automated regression coverage

`make test` runs four suites — the native C tests, the Node/WASM smoke
test (which skips itself with a clear message if the build hasn't been
run), the CSV-parser and chart-formatting unit tests, and the server's API
integration tests against a throwaway database.

### Not covered by automated tests

- **The WGSL shader and WebGPU code.** Node has no WebGPU and there's no
  headless-browser-with-GPU setup here. Verified by hand, as above.
- **The chart and dashboard DOM code.** There's no jsdom-equivalent
  dependency to build DOM without a browser, so only the pure
  number-formatting functions have unit tests; rendering correctness rests
  on the manual browser verification.

Both are honest gaps in the *test suite*, not unverified functionality —
they were checked, just not in a way that reruns automatically.

---

## 13. Suggested reading order in the code

If you want to learn from this codebase rather than just use it, this
order builds up naturally:

1. **[`web/src/csv/parse.js`](../web/src/csv/parse.js)** — plain
   JavaScript, no new concepts. How raw text becomes typed columns.
2. **[`interp/ext/column.h`](../interp/ext/column.h)** — the data model
   (§5). The header comments explain the reasoning; read it before the
   `.c`.
3. **[`interp/ext/column.c`](../interp/ext/column.c)** —
   `columnCreateStrDict` is dictionary encoding in about 40 lines.
4. **[`interp/ext/builtins_gpu.c`](../interp/ext/builtins_gpu.c)** — the
   operations. `doGroupby` is the one to read closely; it's a complete
   grouped-aggregation engine in one pass. `doGpuSumExact` right above it
   is a worked example of fixed-point arithmetic (§8, §2) fixing a real
   precision bug rather than just describing one.
5. **[`resources/grammar-csv.txt`](../resources/grammar-csv.txt)** — the
   language, as data. Try changing a keyword and rerunning.
6. **[`web/src/gpu/shaders/reduce_sum.wgsl`](../web/src/gpu/shaders/reduce_sum.wgsl)**
   and its i32 sibling, `reduce_sum_i32.wgsl` — the parallel reduction
   (§8), about 40 lines each, differing only in element type.
7. **[`web/src/gpu/bridge.js`](../web/src/gpu/bridge.js)** — what actually
   dispatching GPU work looks like: buffers, bind groups, readback.
8. **[`interp/vendor/repl2/src/interp.c`](../interp/vendor/repl2/src/interp.c)**
   — the biggest file here, and optional. `evalCall` is the interesting
   part; the rest is a tree-walking evaluator in full.

Good first changes to make, roughly in increasing difficulty: add a
`filter_lt` next to `filter_gt`; add a `median` aggregate to `groupby`
(note it needs the values, not just a running accumulator — that's the
interesting part); add a horizontal bar chart form; give the dictionary
build a hash table.
