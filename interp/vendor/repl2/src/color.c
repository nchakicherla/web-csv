#include "color.h"

#include <stdio.h>

void setColor(char *s) {
	printf("%s", s);
}

void resetColor(void) {
	printf(ANSI_RESET);
}
