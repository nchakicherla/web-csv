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
});
