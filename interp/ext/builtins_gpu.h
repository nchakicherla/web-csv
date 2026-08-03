#ifndef WC_BUILTINS_GPU_H
#define WC_BUILTINS_GPU_H

/* builtins_gpu.h - the native functions a CSV-analysis grammar's scripts
 * can call: col(), sum(), gpu_sum(), filter_gt(), emit(). Wired in as the
 * interpreter's NativeFn hook (see vendor/VENDORED.md) - repl2 itself knows
 * nothing about any of this.
 */

#include "interp.h"

bool wcNativeDispatch(Interp *in, const char *name, size_t len, Object *args,
                      size_t n_args, Object *out, void *userdata);

#endif // WC_BUILTINS_GPU_H
