#ifndef SIMPLELIST_H
#define SIMPLELIST_H

#define DECLARE_SLIST(LISTNAME,ITEMTYPE)				\
	struct LISTNAME						\
	{							\
		unsigned int count;				\
		unsigned int size;				\
		ITEMTYPE *data;					\
	};							\
								\
	void LISTNAME##_init(struct LISTNAME* list);	\
	void LISTNAME##_clear(struct LISTNAME *list);			\
	void LISTNAME##_add(struct LISTNAME *list, ITEMTYPE item);	\


#define IMPLEMENT_SLIST(LISTNAME,ITEMTYPE)				\
void LISTNAME##_init(struct LISTNAME *list)				\
{									\
	list->count = 0;						\
	list->size = 4;							\
	list->data = calloc(list->size, sizeof(ITEMTYPE));		\
}									\
									\
void LISTNAME##_clear(struct LISTNAME *list)			\
{								\
	free(list->data);					\
	list->count = 0;					\
	list->size = 0;						\
	list->data = NULL;					\
}								\
								\
void LISTNAME##_add(struct LISTNAME *list, ITEMTYPE item)				\
{											\
	if(list->count == list->size)							\
	{										\
		list->size = list->size ? (list->size << 1) : 4;			\
		list->data = realloc(list->data, list->size * sizeof(ITEMTYPE));	\
	}										\
											\
	list->data[list->count++] = item;						\
}											\

#endif
