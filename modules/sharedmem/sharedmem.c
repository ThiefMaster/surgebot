#include "global.h"
#include "module.h"
#include "dict.h"
#include "hook.h"
#include "sharedmem.h"
MODULE_DEPENDS(NULL);

IMPLEMENT_HOOKABLE(shared_memory_changed)

struct shared_memory
{
	shared_memory_free_f *free_func;
	void *data;
};

static void module_unloaded(struct module *module);
static void shared_memory_free_memory(struct shared_memory *memory);

static struct dict *shared_memory;


MODULE_INIT
{
	shared_memory = dict_create();
	dict_set_free_funcs(shared_memory, NULL, (dict_free_f*)dict_free);
	reg_module_load_func(NULL, module_unloaded);
}

MODULE_FINI
{
	clear_shared_memory_changed_hooks();
	unreg_module_load_func(NULL, module_unloaded);
	dict_free(shared_memory);
}

static void module_unloaded(struct module *module)
{
	struct dict *module_node = dict_find(shared_memory, module->name);
	if(module_node == NULL)
		return;

	dict_iter(node, module_node)
	{
		struct shared_memory *memory = node->data;
		CALL_HOOKS(shared_memory_changed, (module, node->key, memory->data, NULL));
	}

	dict_delete(shared_memory, module->name);
}

static void shared_memory_free_memory(struct shared_memory *memory)
{
	if(memory->free_func && memory->data)
		memory->free_func(memory->data);
	free(memory);
}

void shared_memory_set(struct module *module, const char *key, void *data, shared_memory_free_f *free_func)
{
	// find module node
	struct dict *module_node = dict_find(shared_memory, module->name);
	struct shared_memory *memory;
	void *old_data = NULL;
	shared_memory_free_f *old_free_func = NULL;

	if(module_node == NULL)
	{
		// module has no node yet, create a new one
		module_node = dict_create();
		dict_set_free_funcs(module_node, free, (dict_free_f *)shared_memory_free_memory);
		// add it to shared memory
		dict_insert(shared_memory, module->name, module_node);
	}

	if((memory = dict_find(module_node, key)))
	{
		// save old data/func so it can be used after calling hooks
		old_data = memory->data;
		old_free_func = memory->free_func;
	}
	else
	{
		// create new memory node
		memory = malloc(sizeof(struct shared_memory));
		memset(memory, 0, sizeof(struct shared_memory));
		dict_insert(module_node, strdup(key), memory);
	}

	memory->free_func = free_func;
	memory->data = data;

	CALL_HOOKS(shared_memory_changed, (module, key, old_data, data));

	// free old value
	if(old_free_func && old_data)
		old_free_func(old_data);
}

void *shared_memory_get(const char *module_name, const char *key, void *fallback)
{
	// get module node
	struct dict *module_node = dict_find(shared_memory, module_name);
	struct shared_memory *memory;

	// if the given module has no shared memory, return the specified fallback
	if(module_node == NULL)
		return fallback;

	// if the key does not exist, also return the fallback
	if((memory = dict_find(module_node, key)) == NULL)
		return fallback;

	return memory ? memory->data : NULL;
}
