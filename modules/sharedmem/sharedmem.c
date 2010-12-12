/*
 *TODO:das war mal
 * shared_memory is a dict with the module's names as keys and dicts as data.
 * These subsidiary dicts hold the actual shared memory, stored as
 * key -> shared data.
 */

#include "global.h"
#include "module.h"
#include "dict.h"
#include "sharedmem.h"
MODULE_DEPENDS(NULL);

static struct dict *shared_memory;

MODULE_INIT
{
	shared_memory = dict_create();
	dict_set_free_funcs(shared_memory, free, (dict_free_f*)dict_free);
}

MODULE_FINI
{
	dict_free(shared_memory);
}

void shared_memory_set_free_func(struct module *module, shared_memory_free_f *free_func) {
	// find node for module
	struct dict *module_node = dict_find(shared_memory, module->name);
	assert(module_node != NULL);
	dict_set_free_funcs(module_node, NULL, free_func);
}

void shared_memory_set(struct module *module, const char *key, void *data) {
	// find module node
	struct dict *module_node = dict_find(shared_memory, module->name);

	if(module_node == NULL) {
		// module has no node yet, create a new one
		module_node = dict_create();
		// add it to shared memory
		dict_insert(shared_memory, module->name, module_node);
	}
	dict_insert(module_node, strdup(key), data);
}

void *shared_memory_get(const char *module_name, const char *key, void *fallback) {
	// get dict node to module's memory
	struct dict *memory = dict_find(shared_memory, module_name);
	// if the given module has no shared memory, return the specified fallback
	if(memory == NULL) {
		return fallback;
	}
	// get data
	void *data = dict_find(memory, key);

	return data == NULL ? fallback : data;
}
