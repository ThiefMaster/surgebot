#include "global.h"
#include "module.h"
#include "scripting.h"

MODULE_DEPENDS(NULL);

static void module_unloaded(struct module *module);
static void free_function(struct scripting_func *func);

static struct module *this;
static struct dict *scripting_funcs;

MODULE_INIT
{
	this = self;
	scripting_funcs = dict_create();
	dict_set_free_funcs(scripting_funcs, NULL, (dict_free_f*)free_function);
	reg_module_load_func(NULL, module_unloaded);
}

MODULE_FINI
{
	dict_free(scripting_funcs);
	unreg_module_load_func(NULL, module_unloaded);
}

struct scripting_func *scripting_register_function(struct module *module, const char *name)
{
	struct scripting_func *func;
	if((func = scripting_find_function(name))) {
		log_append(LOG_WARNING, "Function %s is already defined in module %s", func->name, func->module->name);
		return NULL;
	}
	func = malloc(sizeof(struct scripting_func));
	memset(func, 0, sizeof(struct scripting_func));
	func->module = module;
	func->name = strdup(name);
	dict_insert(scripting_funcs, func->name, func);
	debug("Registered function: %s.%s", module->name, func->name);
	return func;
}

uint8_t scripting_unregister_function(struct module *module, const char *name)
{
	struct scripting_func *func = scripting_find_function(name);
	if(!func) {
		return 0;
	}
	if(func->module != module) {
		return 1;
	}
	dict_delete(scripting_funcs, name);
	return 0;
}

struct scripting_func *scripting_find_function(const char *name)
{
	return dict_find(scripting_funcs, name);
}

void scripting_call_function(struct scripting_func *func)
{
	func->caller(func);
}

static void module_unloaded(struct module *module)
{
	dict_iter(node, scripting_funcs)
	{
		struct scripting_func *func = node->data;
		if(func->module == module) {
			dict_delete_node(scripting_funcs, node);
		}
	}
}

static void free_function(struct scripting_func *func)
{
	debug("Unregistered function: %s.%s", func->module->name, func->name);
	func->freeer(func);
	free(func->name);
}
