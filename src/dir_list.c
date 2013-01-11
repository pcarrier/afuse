#define __DIR_LIST_C

#include <stdlib.h>
#include <errno.h>
#include "afuse.h"
#include "utils.h"
#include "dir_list.h"

void dir_list_add(dir_list_t ** dir_list, DIR * dir)
{
	dir_list_t *new_dir;

	new_dir = my_malloc(sizeof(dir_list_t));
	new_dir->dir = dir;
	new_dir->next = *dir_list;
	new_dir->prev = NULL;

	*dir_list = new_dir;
}

void dir_list_remove(dir_list_t ** dir_list, DIR * dir)
{
	dir_list_t *current_dir = *dir_list;

	while (current_dir) {
		if (current_dir->dir == dir) {
			if (current_dir->prev)
				current_dir->prev->next = current_dir->next;
			else
				*dir_list = current_dir->next;
			if (current_dir->next)
				current_dir->next->prev = current_dir->prev;
			free(current_dir);

			return;
		}

		current_dir = current_dir->next;
	}
}

void dir_list_close_all(dir_list_t ** dir_list)
{
	while (*dir_list) {
		int retries;

		for (retries = 0; retries < CLOSE_MAX_RETRIES &&
		     closedir((*dir_list)->dir) == -1 &&
		     errno == EINTR; retries++) ;
		dir_list_remove(dir_list, (*dir_list)->dir);
	}
}

bool dir_list_empty(dir_list_t * dir_list)
{
	return (dir_list == NULL) ? true : false;
}
