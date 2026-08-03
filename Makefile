# Makefile - runs every test suite in this repo. Each one also runs
# standalone (see its own directory/README) - this is one command for a
# full "did I break anything" pass.
#
#   make test          run everything
#   make test-c         interp/ext/test's native C suite (no emcc needed)
#   make test-wasm       Node smoke test against the real emcc build -
#                        skips itself if `make -C interp/ext` hasn't run
#   make test-web        web/src/csv's CSV parser unit tests
#   make test-server      server/'s API integration tests - needs
#                         `npm install` in server/ first
#
# Needs Node on PATH (`source /path/to/emsdk/emsdk_env.sh` if it's only
# installed via emsdk - see README's Prerequisites).

.PHONY: test test-c test-wasm test-web test-server

test: test-c test-wasm test-web test-server

test-c:
	$(MAKE) -C interp/ext/test test

test-wasm:
	node --test interp/ext/test/wasm_smoke.test.mjs

test-web:
	node --test web/src/csv/parse.test.js

test-server:
	npm test --prefix server
