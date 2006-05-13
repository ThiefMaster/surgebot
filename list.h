#ifndef LIST_H
#define LIST_H

#define DECLARE_LIST(LISTNAME,ITEMTYPE)				\
	struct LISTNAME						\
	{							\
		unsigned int count;				\
		unsigned int size;				\
		ITEMTYPE *data;					\
	};							\
								\
	struct LISTNAME* LISTNAME##_create();				\
	void LISTNAME##_free(struct LISTNAME *list);			\
	void LISTNAME##_flush(struct LISTNAME *list);			\
	int LISTNAME##_add(struct LISTNAME *list, ITEMTYPE item);	\
	void LISTNAME##_del(struct LISTNAME *list, ITEMTYPE item);


#define IMPLEMENT_LIST(LISTNAME,ITEMTYPE)				\
struct LISTNAME* LISTNAME##_create()					\
{									\
	struct LISTNAME *list = malloc(sizeof(struct LISTNAME));	\
	list->count = 0;						\
	list->size = 2;							\
	list->data = calloc(list->size, sizeof(ITEMTYPE));		\
	return list;							\
}									\
									\
void LISTNAME##_free(struct LISTNAME *list)	\
{						\
	free(list->data);			\
	free(list);				\
}						\
						\
void LISTNAME##_flush(struct LISTNAME *list)			\
{								\
	if(list->count == 0)					\
		return;						\
	free(list->data);					\
	list->count = 0;					\
	list->size = 2;						\
	list->data = calloc(list->size, sizeof(ITEMTYPE));	\
}								\
								\
int LISTNAME##_add(struct LISTNAME *list, ITEMTYPE item)				\
{											\
	if(list->count == list->size)							\
	{										\
		list->size <<= 1;							\
		list->data = realloc(list->data, list->size * sizeof(ITEMTYPE));	\
	}										\
											\
	list->data[list->count++] = item;						\
	return (list->count - 1);							\
}											\
											\
void LISTNAME##_del(struct LISTNAME *list, ITEMTYPE item)		\
{									\
	unsigned int i;							\
	for(i = 0; i < list->count; i++)				\
	{								\
		if(list->data[i] == item)				\
		{							\
			list->data[i] = list->data[--list->count];	\
		}							\
	}								\
}

#endif
