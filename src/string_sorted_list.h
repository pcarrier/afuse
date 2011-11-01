#ifndef STRING_SORTED_LIST_H
#define STRING_SORTED_LIST_H



struct list_t;


int insert_sorted_if_unique(struct list_t **list, char const*const name);

void destroy_list(struct list_t **list);

void print_list(struct list_t const* curr);

#endif
