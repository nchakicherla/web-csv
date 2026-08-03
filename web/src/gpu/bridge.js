// bridge.js - the JS side of interp/ext/builtins_gpu.c's async boundary.
//
// wcGpuReduceSum/wcGpuReduceSumExact (builtins_gpu.c) are declared
// EM_ASYNC_JS and their bodies are `await Module.gpuBridge.reduceSum*(...)`
// - so main.js must attach this module's createGpuBridge(wasmModule)
// output as `.gpuBridge` on the exact wasm module instance before any
// script calls gpu_sum()/gpu_sum_exact(). Everything on the C call path
// from wc_run() down to either import is what Asyncify has to instrument
// (see interp/ext/Makefile).
//
// A factory rather than a module-level singleton bound to a global
// `Module`, because this build uses MODULARIZE=1 - there is no global
// `Module`, only whatever instance main.js got back from
// createInterpModule().

import { getDevice } from './device.js';

const WORKGROUP_SIZE = 256;

const pipelineCache = new Map(); // shader URL -> Promise<GPUComputePipeline>

async function getPipeline(device, shaderUrl) {
	if (!pipelineCache.has(shaderUrl)) {
		pipelineCache.set(shaderUrl, (async () => {
			const code = await (await fetch(shaderUrl)).text();
			const shaderModule = device.createShaderModule({ code });
			return device.createComputePipeline({
				layout: 'auto',
				compute: { module: shaderModule, entryPoint: 'main' },
			});
		})());
	}
	return pipelineCache.get(shaderUrl);
}

// Runs `pipeline` (a two-stage-reduction shader - see the .wgsl files: one
// partial sum per workgroup, this function finishes the small remaining
// add itself) over `values` and returns the total. `PartialsArrayType` is
// Float32Array or Int32Array, matching the shader's element type - the
// reduceSum/reduceSumExact wrappers below differ only in that type, which
// shader they load, and how they read their input out of the wasm heap.
async function dispatchReduction(device, pipeline, values, PartialsArrayType) {
	const len = values.length;
	if (len === 0) {
		return 0;
	}

	const inputBuffer = device.createBuffer({
		size: values.byteLength,
		usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
	});
	device.queue.writeBuffer(inputBuffer, 0, values);

	const numWorkgroups = Math.ceil(len / WORKGROUP_SIZE);
	const partialsBuffer = device.createBuffer({
		size: numWorkgroups * 4,
		usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
	});

	const bindGroup = device.createBindGroup({
		layout: pipeline.getBindGroupLayout(0),
		entries: [
			{ binding: 0, resource: { buffer: inputBuffer } },
			{ binding: 1, resource: { buffer: partialsBuffer } },
		],
	});

	const encoder = device.createCommandEncoder();
	const pass = encoder.beginComputePass();
	pass.setPipeline(pipeline);
	pass.setBindGroup(0, bindGroup);
	pass.dispatchWorkgroups(numWorkgroups);
	pass.end();

	const readBuffer = device.createBuffer({
		size: numWorkgroups * 4,
		usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST,
	});
	encoder.copyBufferToBuffer(partialsBuffer, 0, readBuffer, 0, numWorkgroups * 4);
	device.queue.submit([encoder.finish()]);

	await readBuffer.mapAsync(GPUMapMode.READ);
	const partials = new PartialsArrayType(readBuffer.getMappedRange().slice(0));
	readBuffer.unmap();

	let total = 0;
	for (let i = 0; i < partials.length; i++) {
		total += partials[i];
	}

	inputBuffer.destroy();
	partialsBuffer.destroy();
	readBuffer.destroy();

	return total;
}

export function createGpuBridge(wasmModule) {
	return {
		/* `ptr` is a byte offset into the wasm heap (as passed from C),
		 * `len` a count of f64 elements. Returns a plain JS number - the
		 * value that flows back into wcGpuReduceSum's C return, then into
		 * the interpreter as a DBL_TYPE Object. Downcasts to f32 before
		 * upload (WGSL has no f64) - a real precision tradeoff, see
		 * ARCHITECTURE.md §8. For an exact sum, see reduceSumExact below. */
		async reduceSum(ptr, len) {
			if (len === 0) {
				return 0;
			}
			const device = await getDevice();
			const pipeline = await getPipeline(device, new URL('./shaders/reduce_sum.wgsl', import.meta.url));

			// Re-read HEAPF64 fresh rather than caching it at module scope:
			// ALLOW_MEMORY_GROWTH can replace the underlying ArrayBuffer,
			// which would leave a cached view pointing at detached memory.
			const heap = wasmModule.HEAPF64;
			const f32 = new Float32Array(len);
			const base = ptr >> 3; // 8 bytes per f64
			for (let i = 0; i < len; i++) {
				f32[i] = heap[base + i];
			}

			return dispatchReduction(device, pipeline, f32, Float32Array);
		},

		/* `ptr` points at `len` already-scaled i32 values (integer cents -
		 * see doGpuSumExact in builtins_gpu.c, which does the scaling and
		 * the overflow guard before calling this). Integer addition has no
		 * rounding, so this reduction's result is exact, not approximate -
		 * unlike reduceSum's f32 path. */
		async reduceSumExact(ptr, len) {
			if (len === 0) {
				return 0;
			}
			const device = await getDevice();
			const pipeline = await getPipeline(device, new URL('./shaders/reduce_sum_i32.wgsl', import.meta.url));

			const heap = wasmModule.HEAP32;
			const base = ptr >> 2; // 4 bytes per i32
			const i32 = new Int32Array(len);
			for (let i = 0; i < len; i++) {
				i32[i] = heap[base + i];
			}

			return dispatchReduction(device, pipeline, i32, Int32Array);
		},
	};
}
