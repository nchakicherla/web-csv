#include "oom.h"
#include "budget.h"

#include <stdio.h>
#include <stdlib.h>

void *checkAlloc(void *ptr) {
	if (!ptr) {
		char desc[128];
		budget_describe(desc, sizeof desc);
		fprintf(stderr, "repl2: out of memory (%s)\n", desc);
		exit(1);
	}
	return ptr;
}
