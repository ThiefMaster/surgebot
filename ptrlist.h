#ifndef PTRLIST_H
#define PTRLIST_H

typedef void (ptrlist_free_f)(void *ptr);

struct ptrlist_node
{
	int	type;
	void	*ptr;
};

struct ptrlist
{
	unsigned int	count;
	unsigned int	size;

	ptrlist_free_f *free_func;
	struct ptrlist_node	**data;
};

struct ptrlist *ptrlist_create();
void ptrlist_free(struct ptrlist *list);

unsigned int ptrlist_add(struct ptrlist *list, int ptr_type, void *ptr);
unsigned int ptrlist_find(struct ptrlist *list, void *ptr);
void ptrlist_set_free_func(struct ptrlist *list, ptrlist_free_f *free_func);
void ptrlist_del(struct ptrlist *list, unsigned int pos, unsigned int *pos_ptr);

#endif
