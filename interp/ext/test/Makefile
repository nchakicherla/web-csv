# interp/ext/test/Makefile - native (plain cc, not emcc) tests for the C
# layer: grammar parsing, the native-function hook, the column store, and
# the col/sum/gpu_sum/filter_gt/emit builtins. Fast (no toolchain beyond a
# C compiler needed, runs in well under a second) - see emscripten.h (this
# directory's shim, not the real SDK header) for what this covers and what
# it deliberately doesn't (Asyncify, the real WGSL shader).
#
#   make test     build and run
#   make clean

CC = gcc
VENDOR_SRC = ../../vendor/repl2/src
EXT_SRC = ..
CFLAGS = -Wall -Wextra -std=c11 -I$(VENDOR_SRC) -I$(EXT_SRC) -I. -g -O0
# After the sources (link order matters): llround/floor live in libm, which
# newer glibc toolchains no longer pull in implicitly.
LDLIBS = -lm

SRC = $(wildcard $(VENDOR_SRC)/*.c) $(wildcard $(EXT_SRC)/*.c) js_stubs.c test_builtins.c
BIN = test_builtins

.PHONY: test clean

test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) emscripten.h
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDLIBS)

clean:
	rm -f $(BIN)
