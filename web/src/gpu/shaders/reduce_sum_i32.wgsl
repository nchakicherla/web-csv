// reduce_sum_i32.wgsl - the same two-stage parallel reduction as
// reduce_sum.wgsl, but over i32 instead of f32, for gpu_sum_exact()
// (interp/ext/builtins_gpu.c). Integer addition has no rounding, so -
// unlike reduce_sum.wgsl's f32 path, which trades some precision for GPU
// eligibility - this shader's result is exactly reproducible on the CPU,
// not just close, as long as the caller has bounded the values so no
// partial sum can overflow i32 (see doGpuSumExact's sum-of-absolute-
// values guard).
//
// This shader doesn't know or care that the values represent money in
// cents - that scaling happens entirely on the C side before upload and
// is undone after readback.

const WORKGROUP_SIZE: u32 = 256u;

@group(0) @binding(0) var<storage, read> input: array<i32>;
@group(0) @binding(1) var<storage, read_write> partials: array<i32>;

var<workgroup> shared_sum: array<i32, 256>;

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
		shared_sum[local_id.x] = 0;
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
