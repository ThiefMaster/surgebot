#include "global.h"
#include "ptrlist.h"

struct ptrlist *ptrlist_create()
{
	struct ptrlist *list = malloc(sizeof(struct ptrlist));
	memset(list, 0, sizeof(struct ptrlist));
	list->count = 0;
	list->size = 2;
	list->data = calloc(list->size, sizeof(struct ptrlist_node *));
	return list;
}

void ptrlist_free(struct ptrlist *list)
{
	unsigned int i;
	for(i = 0; i < list->count; i++)
		free(list->data[i]);
	free(list->data);
	free(list);
}

unsigned int ptrlist_add(struct ptrlist *list, int ptr_type, void *ptr)
{
	struct ptrlist_node *node;
	unsigned int pos;

	if(list->count == list->size) // list is full, we need to allocate more memory
	{
		list->size <<= 1; // double size
		list->data = realloc(list->data, list->size * sizeof(struct ptrlist_node *));
	}

	node = malloc(sizeof(struct ptrlist_node));
	node->type = ptr_type;
	node->ptr = ptr;

	pos = list->count++;
	list->data[pos] = node;
	return pos;
}

void ptrlist_del(struct ptrlist *list, unsigned int pos, unsigned int *pos_ptr)
{
	assert(pos < list->count);
	free(list->data[pos]);
	list->data[pos] = list->data[--list->count]; // copy last element into empty position
	if(pos_ptr != NULL && *pos_ptr == list->count)
		*pos_ptr = pos;
}

