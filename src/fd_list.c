#define __FD_LIST_C

#include <errno.h>
#include "afuse.h"
#include "utils.h"
#include "fd_list.h"

void fd_list_add(fd_list_t **fd_list, int fd)
{
	fd_list_t *new_fd;

	new_fd = my_malloc( sizeof(fd_list_t) );
	new_fd->fd = fd;
	new_fd->next = *fd_list;
	new_fd->prev = NULL;

	*fd_list = new_fd;
}

void fd_list_remove(fd_list_t **fd_list, int fd)
{
	fd_list_t *current_fd = *fd_list;
	
	while(current_fd) {
		if(current_fd->fd == fd) {
			if(current_fd->prev)
				current_fd->prev->next = current_fd->next;
			else
				*fd_list = current_fd->next;
			if(current_fd->next)
				current_fd->next->prev = current_fd->prev;
			free(current_fd);

			return;
		}

		current_fd = current_fd->next;
	}
}

void fd_list_close_all(fd_list_t **fd_list)
{
	while(*fd_list) {
		int retries;
		
		for(retries = 0; retries < CLOSE_MAX_RETRIES &&
		                 close((*fd_list)->fd) == -1   &&
		                 errno == EINTR;
		    retries++);
		fd_list_remove(fd_list, (*fd_list)->fd);
	}
}

bool fd_list_empty(fd_list_t *fd_list)
{
	return (fd_list == NULL) ? true : false;
}
