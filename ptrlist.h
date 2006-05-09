#ifndef PTRLIST_H
#define PTRLIST_H

struct ptrlist_node
{
	int	type;
	void	*ptr;
};

struct ptrlist
{
	unsigned int	count;
	unsigned int	size;

	struct ptrlist_node	**data;
};

struct ptrlist *ptrlist_create();
void ptrlist_free(struct ptrlist *list);

unsigned int ptrlist_add(struct ptrlist *list, int ptr_type, void *ptr);
void ptrlist_del(struct ptrlist *list, unsigned int pos, unsigned int *pos_ptr);

#endif
