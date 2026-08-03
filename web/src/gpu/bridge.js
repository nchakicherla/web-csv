// bridge.js - the JS side of interp/ext/builtins_gpu.c's async boundary.
//
// wcGpuReduceSum (builtins_gpu.c) is declared EM_ASYNC_JS and its body is
// `await Module.gpuBridge.reduceSum(ptr, len)` - so main.js must attach
// this module's createGpuBridge(wasmModule) output as `.gpuBridge` on the
// exact wasm module instance before any script calls gpu_sum(). Everything
// on the C call path from wc_run() down to that import is what Asyncify
// has to instrument (see interp/ext/Makefile).
//
// A factory rather than a module-level singleton bound to a global
// `Module`, because this build uses MODULARIZE=1 - there is no global
// `Module`, only whatever instance main.js got back from
// createInterpModule().

import { getDevice } from './device.js';

const WORKGROUP_SIZE = 256;

let pipelinePromise = null;

async function getReduceSumPipeline(device) {
	if (!pipelinePromise) {
		pipelinePromise = (async () => {
			const url = new URL('./shaders/reduce_sum.wgsl', import.meta.url);
			const code = await (await fetch(url)).text();
			const shaderModule = device.createShaderModule({ code });
			return device.createComputePipeline({
				layout: 'auto',
				compute: { module: shaderModule, entryPoint: 'main' },
			});
		})();
	}
	return pipelinePromise;
}

export function createGpuBridge(wasmModule) {
	return {
		/* `ptr` is a byte offset into the wasm heap (as passed from C),
		 * `len` a count of f64 elements. Returns a plain JS number - the
		 * value that flows back into wcGpuReduceSum's C return, then into
		 * the interpreter as a DBL_TYPE Object. */
		async reduceSum(ptr, len) {
			if (len === 0) {
				return 0;
			}

			const device = await getDevice();
			const pipeline = await getReduceSumPipeline(device);

			// Re-read HEAPF64 fresh rather than caching it at module scope:
			// ALLOW_MEMORY_GROWTH can replace the underlying ArrayBuffer,
			// which would leave a cached view pointing at detached memory.
			const heap = wasmModule.HEAPF64;
			const f32 = new Float32Array(len);
			const base = ptr >> 3; // 8 bytes per f64
			for (let i = 0; i < len; i++) {
				f32[i] = heap[base + i];
			}

			const inputBuffer = device.createBuffer({
				size: f32.byteLength,
				usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
			});
			device.queue.writeBuffer(inputBuffer, 0, f32);

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
			const partials = new Float32Array(readBuffer.getMappedRange().slice(0));
			readBuffer.unmap();

			let total = 0;
			for (let i = 0; i < partials.length; i++) {
				total += partials[i];
			}

			inputBuffer.destroy();
			partialsBuffer.destroy();
			readBuffer.destroy();

			return total;
		},
	};
}
