#ifndef DICT_H
#define DICT_H

typedef void (dict_free_f)(void *);

struct dict
{
	unsigned int count;
	struct dict_node *head;
	struct dict_node *tail;
	
	dict_free_f *free_keys_func;
	dict_free_f *free_data_func;
};

struct dict_node
{
	char *key;
	void *data;

	struct dict_node *prev;
	struct dict_node *next;
};

struct dict *dict_create();
void dict_free(struct dict *dict);
void dict_set_free_funcs(struct dict *dict, dict_free_f free_keys_func, dict_free_f free_data_func);
void dict_insert(struct dict *dict, char *key, void *data);
void *dict_find(struct dict *dict, const char *key);
void dict_delete_node(struct dict *dict, struct dict_node *node);
unsigned int dict_delete(struct dict *dict, const char *key);
unsigned int dict_delete_key_value(struct dict *dict, const char *key, void *data);

#define dict_size(DICT)		((DICT) ? (DICT)->count : 0)
#define dict_first_data(DICT)	((DICT) && (DICT)->head ? (DICT)->head->data : NULL)

#define dict_iter(ENTRY, DICT)	struct dict_node* ENTRY;	\
				for(ENTRY = (DICT)->head; ENTRY; ENTRY = (ENTRY)->next)

#endif
