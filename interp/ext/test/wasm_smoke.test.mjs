// wasm_smoke.test.mjs - runs the REAL emcc build (web/src/wasm/interp.js,
// gitignored - `make` in interp/ext/ produces it) under Node, automating
// the manual verification from the initial browser-run session: wc_init/
// wc_load_column_f64/wc_run against the real build output, and a
// forced-GPU-path check (fake navigator.gpu + a stub async bridge with a
// real delay) that confirms Asyncify genuinely suspends and resumes the C
// call stack around a real async JS boundary, not just that the build
// didn't error.
//
// What this does NOT cover: the real WGSL shader / GPUDevice/GPUBuffer
// code in web/src/gpu/bridge.js - Node has no WebGPU. That needs a real
// browser; see README's "First build checklist".
//
// Skips (not fails) if the build hasn't been run yet, so `node --test`
// from a fresh checkout doesn't error confusingly.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const wasmJsPath = join(__dirname, '..', '..', '..', 'web', 'src', 'wasm', 'interp.js');
const built = existsSync(wasmJsPath);

// Resolves interp.wasm/interp.data next to interp.js regardless of the
// test runner's cwd - readFileSync (the Node-side loader path Emscripten's
// glue takes) needs a plain filesystem path, not a URL string.
function locateFile(path) {
	return new URL(`./${path}`, pathToFileURL(wasmJsPath)).pathname;
}

test('wasm build smoke test', { skip: built ? false : 'run `make` in interp/ext/ first (web/src/wasm/interp.js not found)' }, async (t) => {
	const { default: createInterpModule } = await import(pathToFileURL(wasmJsPath).href);

	await t.test('wc_init/wc_load_column_f64/wc_run work against the real build', async () => {
		const mod = await createInterpModule({ locateFile });

		const rc1 = mod.ccall('wc_init', 'number', ['string'], ['/resources/grammar-csv.txt']);
		assert.equal(rc1, 0);

		const amounts = new Float64Array([10, 250, 40, 999, 5, 120, 300.5]);
		const ptr = mod._malloc(amounts.length * 8);
		mod.HEAPF64.set(amounts, ptr >> 3);
		const rc2 = mod.ccall('wc_load_column_f64', 'number', ['string', 'number', 'number'], ['amount', ptr, amounts.length]);
		mod._free(ptr);
		assert.equal(rc2, 0);

		mod.wcResults = [];
		const run = mod.cwrap('wc_run', 'number', ['string'], { async: true });
		const rc3 = await run(
			'let big := filter_gt(col("amount"), 100);\n' +
			'emit(sum(big));\n' +
			'emit(sum(col("amount")));\n'
		);
		assert.equal(rc3, 0);
		// Same hand-checked values as README's "Sample data" section / the
		// native test suite's test_init_and_basic_query.
		assert.deepEqual(mod.wcResults, [1669.5, 1724.5]);
	});

	await t.test('Asyncify genuinely suspends and resumes across a real async JS boundary', async () => {
		const mod = await createInterpModule({ locateFile });

		// Node has no navigator - patch just the property wcGpuAvailable()
		// checks, matching how the earlier manual verification did this.
		if (!globalThis.navigator) {
			globalThis.navigator = {};
		}
		Object.defineProperty(globalThis.navigator, 'gpu', { value: {}, configurable: true });

		let bridgeCalled = false;
		mod.gpuBridge = {
			async reduceSum(ptr, len) {
				bridgeCalled = true;
				// A real delay: this only proves suspend/resume if the
				// C call stack actually waits for it rather than the
				// `await` happening to resolve synchronously.
				await new Promise((resolve) => setTimeout(resolve, 50));
				let total = 0;
				for (let i = 0; i < len; i++) total += mod.HEAPF64[(ptr >> 3) + i];
				return total;
			},
		};

		const rc1 = mod.ccall('wc_init', 'number', ['string'], ['/resources/grammar-csv.txt']);
		assert.equal(rc1, 0);

		// Must clear WC_GPU_MIN_LEN (50,000) or gpu_sum stays on the CPU
		// path and never reaches the async bridge at all.
		const n = 60000;
		const values = new Float64Array(n).fill(1);
		const ptr = mod._malloc(n * 8);
		mod.HEAPF64.set(values, ptr >> 3);
		const rc2 = mod.ccall('wc_load_column_f64', 'number', ['string', 'number', 'number'], ['big', ptr, n]);
		mod._free(ptr);
		assert.equal(rc2, 0);

		mod.wcResults = [];
		const run = mod.cwrap('wc_run', 'number', ['string'], { async: true });
		const before = Date.now();
		const rc3 = await run('emit(gpu_sum(col("big")));');
		const elapsed = Date.now() - before;

		assert.equal(rc3, 0);
		assert.equal(bridgeCalled, true, 'gpu_sum should have reached the GPU-eligible branch');
		assert.ok(elapsed >= 45, `expected the real ~50ms bridge delay to be observed (got ${elapsed}ms) - if this is fast, Asyncify may not be suspending the call stack`);
		assert.equal(mod.wcResults[0], n);
	});

	await t.test('a large categorical column loads without corrupting memory', async () => {
		// Regression test: wc_load_column_str_dict's joined-values argument
		// used to be passed as a ccall 'string' type, which Emscripten
		// marshals through a stack allocation (a fixed, small default -
		// 64KB) rather than the heap. A 100k-row categorical column's
		// joined string is comfortably megabytes, and reliably crashed with
		// "memory access out of bounds" - not a clean JS exception, actual
		// memory corruption - before main.js/loadStringColumn switched to
		// an explicit _malloc + stringToUTF8 heap allocation. This
		// reproduces that scale (60k rows, well past the 64KB stack) using
		// the same fixed pattern, so a regression back to the ccall
		// 'string' shortcut would crash this test the same way it crashed
		// a real 100k-row CSV upload.
		const mod = await createInterpModule({ locateFile });
		const rc1 = mod.ccall('wc_init', 'number', ['string'], ['/resources/grammar-csv.txt']);
		assert.equal(rc1, 0);

		const n = 60000;
		const cats = ['north', 'south', 'east', 'west'];
		const values = Array.from({ length: n }, (_, i) => cats[i % cats.length]);
		const joined = values.join('\x1f');

		const byteLen = mod.lengthBytesUTF8(joined) + 1;
		const ptr = mod._malloc(byteLen);
		mod.stringToUTF8(joined, ptr, byteLen);
		const rc2 = mod.ccall('wc_load_column_str_dict', 'number', ['string', 'number', 'number'], ['region', ptr, n]);
		mod._free(ptr);
		assert.equal(rc2, 0);

		mod.wcResults = [];
		const run = mod.cwrap('wc_run', 'number', ['string'], { async: true });
		const rc3 = await run('groupby(col("region"));');
		assert.equal(rc3, 0);
		assert.deepEqual(mod.wcResults[0].labels, cats);
		// n=60000 split evenly across 4 categories by i % 4.
		assert.deepEqual(mod.wcResults[0].values, [15000, 15000, 15000, 15000]);
	});

	await t.test('gpu_sum_exact takes the real GPU-eligible branch and matches the exact CPU sum bit-for-bit', async () => {
		// Unlike gpu_sum's f32 path (which is only ever approximately
		// correct - see the Asyncify test above, which deliberately uses
		// data where f32 and f64 happen to agree), gpu_sum_exact's whole
		// point is that the GPU result and the CPU result must be
		// *identical*, since integer addition doesn't round. This asserts
		// exact equality, not closeness.
		const mod = await createInterpModule({ locateFile });

		if (!globalThis.navigator) {
			globalThis.navigator = {};
		}
		Object.defineProperty(globalThis.navigator, 'gpu', { value: {}, configurable: true });

		let bridgeCalled = false;
		mod.gpuBridge = {
			async reduceSumExact(ptr, len) {
				bridgeCalled = true;
				await new Promise((resolve) => setTimeout(resolve, 50));
				let total = 0n;
				for (let i = 0; i < len; i++) total += BigInt(mod.HEAP32[(ptr >> 2) + i]);
				return Number(total);
			},
		};

		const rc1 = mod.ccall('wc_init', 'number', ['string'], ['/resources/grammar-csv.txt']);
		assert.equal(rc1, 0);

		const n = 60000;
		const values = new Float64Array(n);
		let expectedCents = 0n;
		for (let i = 0; i < n; i++) {
			values[i] = ((i % 10000) / 100) - 50; // mixed sign, 2 decimal places
			expectedCents += BigInt(Math.round(values[i] * 100));
		}
		const ptr = mod._malloc(n * 8);
		mod.HEAPF64.set(values, ptr >> 3);
		const rc2 = mod.ccall('wc_load_column_f64', 'number', ['string', 'number', 'number'], ['bigmoney', ptr, n]);
		mod._free(ptr);
		assert.equal(rc2, 0);

		mod.wcResults = [];
		const run = mod.cwrap('wc_run', 'number', ['string'], { async: true });
		const before = Date.now();
		const rc3 = await run('emit(gpu_sum_exact(col("bigmoney")));');
		const elapsed = Date.now() - before;

		assert.equal(rc3, 0);
		assert.equal(bridgeCalled, true, 'gpu_sum_exact should have reached the GPU-eligible branch');
		assert.ok(elapsed >= 45, `expected the real ~50ms bridge delay (got ${elapsed}ms)`);
		assert.equal(mod.wcResults[0], Number(expectedCents) / 100);
	});
});
