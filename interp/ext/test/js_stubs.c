/* js_stubs.c - native stand-ins for what Emscripten's glue and
 * web/src/gpu/bridge.js provide in the real build: wcGpuAvailable() and
 * wcGpuReduceSum() (builtins_gpu.c) and wcEmitNumber() (also
 * builtins_gpu.c). Lets test_builtins.c drive wc_init/wc_load_column_f64/
 * wc_run (web_main.c) exactly as the real entry points, unmodified.
 *
 * g_wc_gpu_available and g_gpu_path_taken exist so tests can force and
 * then verify which branch gpu_sum's eligibility gate actually took,
 * rather than only checking the final numeric result (which would be
 * identical whether the CPU or "GPU" stub path ran).
 */

#include <stdint.h>

int g_wc_gpu_available = 0;
int g_gpu_path_taken = 0;

double g_emitted[64];
int g_n_emitted = 0;

int wcGpuAvailable(void) {
	return g_wc_gpu_available;
}

double wcGpuReduceSum(double *ptr, uint32_t len) {
	g_gpu_path_taken = 1;
	double total = 0;
	for (uint32_t i = 0; i < len; i++) {
		total += ptr[i];
	}
	return total;
}

void wcEmitNumber(double v) {
	if (g_n_emitted < 64) {
		g_emitted[g_n_emitted++] = v;
	}
}
