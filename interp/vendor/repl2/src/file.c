#include "file.h"
#include "oom.h"

char *tryReadFile(const char *name, Arena *arena) {
	FILE *file = NULL;
	char *output = NULL;
	long size;
	size_t read;

	if (!arena) {
		return NULL;
	}

	file = fopen(name, "rb");
	if (!file) {
		return NULL;
	}

	// get length of file as size, then rewind pointer
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (size < 0) {
		fclose(file);
		return NULL;
	}

	output = checkAlloc(arena_alloc(arena, (size_t)size + 1, _Alignof(char)));

	// an empty file is legitimate; fread returning 0 is only an error when
	// there was something to read
	read = fread(output, 1, (size_t)size, file);
	output[read] = '\0';

	fclose(file);
	return output;
}

int tryWriteChars(const char *name, const char *source) {
	FILE *file = NULL;

	file = fopen(name, "w");
	if (!file) return 1;

	fprintf(file, "%s", source);
	fclose(file);
	return 0;
}

FILE *tryFileOpen(const char *name, const char *mode) {
	FILE *fp = NULL;
	if (!(fp = fopen(name, mode))) {
		return NULL;
	}
	return fp;
}
