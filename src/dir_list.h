#ifndef __DIR_LIST_H
#define __DIR_LIST_H

#include <stdbool.h>
#include <dirent.h>

typedef struct _dir_list_t {
	struct _dir_list_t *next;
	struct _dir_list_t *prev;

	DIR *dir;
} dir_list_t;

#undef EXTERN
#ifdef __DIR_LIST_C
#define EXTERN
#else
#define EXTERN extern
#endif

EXTERN void dir_list_add(dir_list_t **dir_list, DIR *dir);
EXTERN void dir_list_remove(dir_list_t **dir_list, DIR *dir);
EXTERN void dir_list_close_all(dir_list_t **dir_list);
EXTERN bool dir_list_empty(dir_list_t *dir_list);

#endif // __DIR_LIST_H
