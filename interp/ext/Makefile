# Emscripten build for the web-csv interpreter: repl2's vendored core
# (../vendor/repl2/src) plus this directory's web-specific layer (column
# store, GPU-aware builtins, the web_main.c entry point).
#
# Requires the Emscripten SDK (emcc) on PATH - verified against emcc 6.0.5
# via emsdk (see README's Prerequisites), including a full real-browser
# run with real WebGPU hardware.
# Install: https://emscripten.org/docs/getting_started/downloads.html
#
#   make            build web/src/wasm/interp.{js,wasm}
#   make clean

EMCC ?= emcc

VENDOR_SRC = ../vendor/repl2/src
EXT_SRC    = .

# main.c/repl.c/external/linenoise are excluded from the vendored copy
# already (see ../vendor/VENDORED.md) - this is every remaining .c file
# in both directories.
SRC = $(wildcard $(VENDOR_SRC)/*.c) $(wildcard $(EXT_SRC)/*.c)

OUT_DIR = ../../web/src/wasm
OUT_JS  = $(OUT_DIR)/interp.js

GRAMMAR_DEFAULT = ../../resources/grammar-csv.txt

CFLAGS = -Wall -Wextra -std=c11 -I$(VENDOR_SRC) -I$(EXT_SRC) -O2

EXPORTED_FUNCS = \
  _wc_init,_wc_load_column_f64,_wc_load_column_str_dict,_wc_load_column_date,_wc_run,_wc_reset_session,_malloc,_free

# HEAPF64 has to be listed explicitly - main.js/loadColumn's
# wasmModule.HEAPF64.set(...) (and bridge.js's read of it) would otherwise
# be undefined, since typed-array heap views aren't exported by default.
# UTF8ToString is what wcEmitGroups (builtins_gpu.c's groupby output path)
# uses to decode the '\x1f'-joined label string it gets back.
# lengthBytesUTF8/stringToUTF8: wc_load_column_str_dict's joined-values
# argument can be large (every row's value, for the whole column) - ccall's
# automatic 'string' marshaling allocates on Emscripten's *stack* (default
# 64KB), which silently corrupts memory past that size instead of erroring
# cleanly. main.js allocates that one argument on the heap explicitly with
# these instead of passing it as a ccall 'string' type - see main.js's
# loadStringColumn for the real crash this fixes (a 100k-row categorical
# column reliably segfaulted before this).
# HEAP32: gpu_sum_exact()'s bridge (bridge.js's reduceSumExact) reads the
# i32 array doGpuSumExact scaled and uploaded, the same way reduceSum
# reads HEAPF64 for gpu_sum()'s f32 path.
EXPORTED_RUNTIME = \
  ccall,cwrap,FS,HEAPF64,HEAP32,UTF8ToString,lengthBytesUTF8,stringToUTF8

# ASYNCIFY note: gpu_sum() (builtins_gpu.c) reaches an EM_ASYNC_JS import
# (wcGpuReduceSum), so every function on the call path from wc_run() down
# to it - interp.c's evalCall/evalNode/execNode/interpExec chain included -
# needs Asyncify instrumentation, not just the leaf trampoline. ASYNCIFY=1
# below instruments the whole binary, which is correct but costs size/speed
# across functions that never touch that path (the scanner, grammar
# loader, ...). Once this actually builds, replace it with ASYNCIFY_ONLY
# listing just that call chain - get the exact (possibly name-mangled)
# list from `emcc ... -s ASYNCIFY_ADVISE=1` rather than guessing it here.
#
# EXPORT_ES6=1: without it, MODULARIZE=1 alone emits a UMD/CommonJS
# factory (module.exports = createInterpModule) with no real `export`
# statement - main.js's `import createInterpModule from './wasm/interp.js'`
# then silently gets `undefined` as the default export in a browser (no
# `module`/`exports` globals there for the UMD branch to hit). Confirmed
# by an actual browser run failing with "createInterpModule is not a
# function" before this flag was added.
LDFLAGS = \
  -s WASM=1 \
  -s ASYNCIFY=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS=$(EXPORTED_FUNCS) \
  -s EXPORTED_RUNTIME_METHODS=$(EXPORTED_RUNTIME) \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=createInterpModule \
  --preload-file $(GRAMMAR_DEFAULT)@/resources/grammar-csv.txt

.PHONY: all clean

all: $(OUT_JS)

$(OUT_JS): $(SRC)
	mkdir -p $(OUT_DIR)
	$(EMCC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(OUT_JS)

clean:
	rm -f $(OUT_DIR)/interp.js $(OUT_DIR)/interp.wasm $(OUT_DIR)/interp.data
