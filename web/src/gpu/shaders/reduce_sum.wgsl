// reduce_sum.wgsl - partial-sum pass of a two-stage parallel reduction.
//
// WGSL's core numeric types are f32/i32/u32 - there is no f64 - so columns
// are downcast to f32 before upload (see bridge.js). That's a real
// precision tradeoff, not an oversight: the GPU path can drift on very
// large sums or values spanning many orders of magnitude. The CPU path
// (interp/ext/builtins_gpu.c's cpuSum) stays exact f64 and is what
// small/precision-sensitive sums use instead (see the WC_GPU_MIN_LEN gate).
//
// Each workgroup reduces its slice of `input` into one element of
// `partials` using workgroup-shared memory; bridge.js finishes the (now
// much smaller) final sum on the CPU after reading `partials` back, rather
// than this shader trying to combine across workgroups itself - WGSL has
// no f32 atomic add, and a second dispatch pass to fully finish on-GPU
// isn't worth it for one final add of a few thousand numbers at most.

const WORKGROUP_SIZE: u32 = 256u;

@group(0) @binding(0) var<storage, read> input: array<f32>;
@group(0) @binding(1) var<storage, read_write> partials: array<f32>;

var<workgroup> shared_sum: array<f32, 256>;

@compute @workgroup_size(256)
fn main(
	@builtin(global_invocation_id) global_id: vec3<u32>,
	@builtin(local_invocation_id) local_id: vec3<u32>,
	@builtin(workgroup_id) group_id: vec3<u32>
) {
	let idx = global_id.x;
	if (idx < arrayLength(&input)) {
		shared_sum[local_id.x] = input[idx];
	} else {
		shared_sum[local_id.x] = 0.0;
	}
	workgroupBarrier();

	var stride = WORKGROUP_SIZE / 2u;
	while (stride > 0u) {
		if (local_id.x < stride) {
			shared_sum[local_id.x] = shared_sum[local_id.x] + shared_sum[local_id.x + stride];
		}
		workgroupBarrier();
		stride = stride / 2u;
	}

	if (local_id.x == 0u) {
		partials[group_id.x] = shared_sum[0];
	}
}
