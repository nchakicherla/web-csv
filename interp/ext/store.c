#include "store.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	char *name;
	Column *col;
} NamedEntry;

static NamedEntry *g_named = NULL;
static size_t g_n_named = 0, g_cap_named = 0;

static Column **g_tracked = NULL;
static size_t g_n_tracked = 0, g_cap_tracked = 0;

/* strdup is POSIX, not C11; repl2's own C source sticks to the standard
 * library only (see vendor/repl2), so this stays consistent with that. */
static char *dupStr(const char *s) {
	size_t len = strlen(s) + 1;
	char *out = malloc(len);
	if (out) {
		memcpy(out, s, len);
	}
	return out;
}

void storeInit(void) {
	g_named = NULL;
	g_n_named = 0;
	g_cap_named = 0;
	g_tracked = NULL;
	g_n_tracked = 0;
	g_cap_tracked = 0;
}

void storeTerm(void) {
	for (size_t i = 0; i < g_n_named; i++) {
		columnFree(g_named[i].col);
		free(g_named[i].name);
	}
	free(g_named);
	g_named = NULL;
	g_n_named = g_cap_named = 0;

	for (size_t i = 0; i < g_n_tracked; i++) {
		columnFree(g_tracked[i]);
	}
	free(g_tracked);
	g_tracked = NULL;
	g_n_tracked = g_cap_tracked = 0;
}

void storeSetNamed(const char *name, Column *col) {
	for (size_t i = 0; i < g_n_named; i++) {
		if (0 == strcmp(g_named[i].name, name)) {
			columnFree(g_named[i].col);
			g_named[i].col = col;
			return;
		}
	}
	if (g_n_named == g_cap_named) {
		size_t new_cap = g_cap_named ? g_cap_named * 2 : 8;
		NamedEntry *grown = realloc(g_named, new_cap * sizeof(NamedEntry));
		if (!grown) {
			return; /* leaks `col`; OOM handling matches the rest of this PoC's C layer */
		}
		g_named = grown;
		g_cap_named = new_cap;
	}
	g_named[g_n_named].name = dupStr(name);
	g_named[g_n_named].col = col;
	g_n_named++;
}

Column *storeGetNamed(const char *name) {
	for (size_t i = 0; i < g_n_named; i++) {
		if (0 == strcmp(g_named[i].name, name)) {
			return g_named[i].col;
		}
	}
	return NULL;
}

void storeTrack(Column *col) {
	if (g_n_tracked == g_cap_tracked) {
		size_t new_cap = g_cap_tracked ? g_cap_tracked * 2 : 8;
		Column **grown = realloc(g_tracked, new_cap * sizeof(Column *));
		if (!grown) {
			return;
		}
		g_tracked = grown;
		g_cap_tracked = new_cap;
	}
	g_tracked[g_n_tracked++] = col;
}
