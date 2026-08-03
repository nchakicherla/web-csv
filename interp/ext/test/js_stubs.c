/* js_stubs.c - native stand-ins for what Emscripten's glue and
 * web/src/gpu/bridge.js provide in the real build: wcGpuAvailable(),
 * wcGpuReduceSum(), wcEmitNumber(), and wcEmitGroups() (all
 * builtins_gpu.c). Lets test_builtins.c drive wc_init/wc_load_column_f64/
 * wc_load_column_str_dict/wc_run (web_main.c) exactly as the real entry
 * points, unmodified.
 *
 * g_wc_gpu_available and g_gpu_path_taken exist so tests can force and
 * then verify which branch gpu_sum's eligibility gate actually took,
 * rather than only checking the final numeric result (which would be
 * identical whether the CPU or "GPU" stub path ran).
 */

#include <stdint.h>
#include <string.h>

int g_wc_gpu_available = 0;
int g_gpu_path_taken = 0;

double g_emitted[64];
int g_n_emitted = 0;

/* groupby() results, captured whole rather than split like the real JS
 * bridge does - test_builtins.c checks g_group_labels_joined directly
 * (still '\x1f'-separated) instead of re-parsing it, since the split
 * itself is JS-side logic this native suite doesn't exercise. */
char g_group_labels_joined[256];
double g_group_values[64];
uint32_t g_group_n = 0;
char g_group_agg[16];

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

void wcEmitGroups(const char *labels_joined, double *values, uint32_t n_groups, const char *agg_name) {
	uint32_t i;
	strncpy(g_group_labels_joined, labels_joined, sizeof(g_group_labels_joined) - 1);
	g_group_labels_joined[sizeof(g_group_labels_joined) - 1] = '\0';
	strncpy(g_group_agg, agg_name, sizeof(g_group_agg) - 1);
	g_group_agg[sizeof(g_group_agg) - 1] = '\0';
	g_group_n = n_groups < 64 ? n_groups : 64;
	for (i = 0; i < g_group_n; i++) {
		g_group_values[i] = values[i];
	}
}
