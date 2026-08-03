# Emscripten build for the web-csv interpreter: repl2's vendored core
# (../vendor/repl2/src) plus this directory's web-specific layer (column
# store, GPU-aware builtins, the web_main.c entry point).
#
# Requires the Emscripten SDK (emcc) on PATH - not installed in the
# sandbox this scaffold was written in, so this Makefile is untested.
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
  _wc_init,_wc_load_column_f64,_wc_run,_wc_reset_session,_malloc,_free

# HEAPF64 has to be listed explicitly - main.js/loadColumn's
# wasmModule.HEAPF64.set(...) (and bridge.js's read of it) would otherwise
# be undefined, since typed-array heap views aren't exported by default.
EXPORTED_RUNTIME = \
  ccall,cwrap,FS,HEAPF64

# ASYNCIFY note: gpu_sum() (builtins_gpu.c) reaches an EM_ASYNC_JS import
# (wcGpuReduceSum), so every function on the call path from wc_run() down
# to it - interp.c's evalCall/evalNode/execNode/interpExec chain included -
# needs Asyncify instrumentation, not just the leaf trampoline. ASYNCIFY=1
# below instruments the whole binary, which is correct but costs size/speed
# across functions that never touch that path (the scanner, grammar
# loader, ...). Once this actually builds, replace it with ASYNCIFY_ONLY
# listing just that call chain - get the exact (possibly name-mangled)
# list from `emcc ... -s ASYNCIFY_ADVISE=1` rather than guessing it here.
LDFLAGS = \
  -s WASM=1 \
  -s ASYNCIFY=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS=$(EXPORTED_FUNCS) \
  -s EXPORTED_RUNTIME_METHODS=$(EXPORTED_RUNTIME) \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createInterpModule \
  --preload-file $(GRAMMAR_DEFAULT)@/resources/grammar-csv.txt

.PHONY: all clean

all: $(OUT_JS)

$(OUT_JS): $(SRC)
	mkdir -p $(OUT_DIR)
	$(EMCC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $(OUT_JS)

clean:
	rm -f $(OUT_DIR)/interp.js $(OUT_DIR)/interp.wasm $(OUT_DIR)/interp.data
