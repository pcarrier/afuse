#ifndef __FD_LIST_H
#define __FD_LIST_H

#include <stdbool.h>

// Link list holding open file descriptors associated with a mount

typedef struct _fd_list_t {
	struct _fd_list_t *next;
	struct _fd_list_t *prev;

	int fd;
} fd_list_t;

#undef EXTERN
#ifdef __DIR_LIST_C
#define EXTERN
#else
#define EXTERN extern
#endif

EXTERN void fd_list_add(fd_list_t ** fd_list, int fd);
EXTERN void fd_list_remove(fd_list_t ** fd_list, int fd);
EXTERN bool fd_list_empty(fd_list_t * fd_list);
EXTERN void fd_list_close_all(fd_list_t ** fd_list);

#endif				// __FD_LIST_H
