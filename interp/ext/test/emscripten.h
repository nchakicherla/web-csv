#ifndef WC_TEST_EMSCRIPTEN_SHIM_H
#define WC_TEST_EMSCRIPTEN_SHIM_H

/* Test-only stand-in for <emscripten.h>, letting builtins_gpu.c and
 * web_main.c compile and run completely unmodified under a plain native
 * compiler - not part of the real build (interp/ext/Makefile links
 * against the real emcc headers/glue). js_stubs.c in this directory
 * provides native C definitions for the handful of EM_JS/EM_ASYNC_JS
 * externs this expands to, standing in for what Emscripten's generated
 * glue and web/src/gpu/bridge.js provide in the real build.
 *
 * This does NOT exercise Asyncify (native code has no such mechanism) or
 * the real WGSL shader - see ../../../web/src/wasm's Node smoke test for
 * the Asyncify check, and README's "First build checklist" for the real
 * WebGPU verification, which needs a real browser. What this DOES cover:
 * grammar parsing, the native-function hook, the column store, and the
 * CPU-side logic (including the GPU eligibility gate's branching) of
 * every builtin - the part most likely to actually break as the project
 * grows.
 */
#define EMSCRIPTEN_KEEPALIVE
#define EM_JS(ret, name, params, ...) extern ret name params
#define EM_ASYNC_JS(ret, name, params, ...) extern ret name params

#endif
