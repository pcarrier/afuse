#define __UTILS_C

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "utils.h"

void *my_malloc(size_t size)
{
	void *p;

	p = malloc(size);

	if (!p) {
		fprintf(stderr, "Failed to allocate: %zu bytes of memory.\n",
			size);
		exit(1);
	}

	return p;
}

char *my_strdup(const char *str)
{
	char *new_str;

	new_str = my_malloc(strlen(str) + 1);
	strcpy(new_str, str);

	return new_str;
}
