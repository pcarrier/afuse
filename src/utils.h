#ifndef __UTILS_H
#define __UTILS_H

#include <stdlib.h>

#undef EXTERN
#ifdef __UTILS_C
#define EXTERN
#else
#define EXTERN extern
#endif

EXTERN void *my_malloc(size_t size);
EXTERN char *my_strdup(const char *str);

#endif // __UTILS_H
