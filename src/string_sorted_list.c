#include <stdlib.h> 
#include <string.h> 

#include <stdio.h>

#include "string_sorted_list.h"

struct list_t {
	struct list_t *prev, *next;
	char *name;
};


static char *stralloccopy(char const*const str)
{
	char *s = malloc((strlen(str) + 1) * sizeof(*s));
	if (s != NULL)
		strcpy(s, str);
	return s;
}

static struct list_t *new_list_element(struct list_t *prev, struct list_t *next,
		char const*const name)
{
	struct list_t *e;
	e = malloc(sizeof(*e));
	if (!e) {
		return NULL;
	}
	e->prev = prev;
	e->next = next;
	e->name = stralloccopy(name);
	if (!e->name) {
		free(e);
		return NULL;
	}
	return e;
}

static struct list_t* insert_between(struct list_t *prev, struct list_t *next,
		char const*const name)
{
	struct list_t *e;

	e = new_list_element(prev, next, name);
	if (e == NULL)
		return NULL;

	if (prev)
		prev->next = e;
	if (next)
		next->prev = e;

	return e;
}

static int update_list(struct list_t **list, struct list_t *prev,
		struct list_t *next, char const*const name)
{
	struct list_t *e;

	e = insert_between(prev, next, name);
	if (e == NULL)
		return -1;

	*list = e;

	return 0;
}


int insert_sorted_if_unique(struct list_t **list, char const*const name)
{
	struct list_t *prev, *curr, *next;
	int cmp;

	if (*list == NULL)
		return update_list(list, NULL, NULL, name);

	curr = NULL;
	next = *list;
	do {
		prev = curr;
		curr = next;
		cmp = strcmp(name, curr->name);
		if (cmp == 0)
			return 1;
		else if (cmp > 0)
			next = curr->next;
		else
			next = curr->prev;
	} while (next && next != prev);

	if (cmp > 0)
		return update_list(list, curr, curr->next, name);
	else
		return update_list(list, curr->prev, curr, name);
}

void destroy_list(struct list_t **list)
{
	struct list_t *curr, *next;

	if (*list == NULL)
		return;

	curr = (*list)->next;
	while (curr) {
		next = curr->next;
		free(curr);
		curr = next;
	}

	curr = (*list);
	while (curr) {
		next = curr->prev;
		free(curr);
		curr = next;
	}

	*list = NULL;
}

void print_list(struct list_t const* curr)
{
	if (curr == NULL) {
		fprintf(stderr, "list is empty\n");
		return;
	}

	fprintf(stderr, "Sorted list: ");

	/* go to beginning */
	while (curr->prev)
		curr = curr->prev;

	do {
		fprintf(stderr, " %s,", curr->name);
		curr = curr->next;
	} while (curr);
	fprintf(stderr, "\n");
}
