#ifndef FILE_H
#define FILE_H

#include <stdio.h>
#include <stdlib.h>

#include "arena.h"

char *tryReadFile(const char *name, Arena *arena);
int tryWriteChars(const char *name, const char *source);
FILE *tryFileOpen(const char *name, const char *mode);

#endif // FILE_H
