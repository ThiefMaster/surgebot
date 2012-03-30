#include "global.h"
#include "module.h"
#include "scripting.h"

MODULE_DEPENDS(NULL);

DECLARE_LIST(scripting_func_list, struct scripting_func *)
IMPLEMENT_LIST(scripting_func_list, struct scripting_func *)

uint8_t func_matches(struct scripting_func *func, const char *module, const char *name);

static struct module *this;
static struct scripting_func_list *scripting_funcs;

MODULE_INIT
{
	this = self;
	scripting_funcs = scripting_func_list_create();
}

MODULE_FINI
{
	scripting_func_list_free(scripting_funcs);
}

uint8_t scripting_register_function(const char *module, const char *name, uint8_t num_args)
{
	if(scripting_find_function(module, name)) {
		log_append(LOG_WARNING, "Function %s.%s is already defined", module ? module : "<core>", name);
		return 1;
	}
	struct scripting_func *func = malloc(sizeof(struct scripting_func));
	memset(func, 0, sizeof(struct scripting_func));
	func->module = module ? strdup(module) : NULL;
	func->name = strdup(name);
	func->num_args = num_args;
	return 0;
}

void scripting_unregister_function(const char *module, const char *name)
{
	struct scripting_func *func = scripting_find_function(module, name);
	if(!func) {
		return;
	}
	scripting_func_list_del(scripting_funcs, func);
	MyFree(func->module);
	MyFree(func->name);
	// TODO: free args
}

void scripting_register_argument(struct scripting_func *func, const char *name, enum scripting_arg_type type, uint8_t required, struct scripting_arg_value *default_value)
{
	assert(func->cur_args < func->num_args);
	struct scripting_func_arg *arg = malloc(sizeof(struct scripting_func_arg));
	memset(arg, 0, sizeof(struct scripting_func_arg));
	arg->name = strdup(name);
	arg->type = type;
	arg->required = required;
	arg->default_value = default_value;
	func->args[func->cur_args++] = arg;
}

struct scripting_func *scripting_find_function(const char *module, const char *name)
{
	for(unsigned int i = 0; i < scripting_funcs->count; i++) {
		struct scripting_func *func = scripting_funcs->data[i];
		if(func_matches(func, module, name)) {
			return func;
		}
	}
	return NULL;
}

uint8_t func_matches(struct scripting_func *func, const char *module, const char *name) {
	if((!module && func->module) || (module && !func->module)) {
		return 0;
	}
	if(module && func->module && strcmp(module, func->module)) {
		return 0;
	}
	return !!strcmp(name, func->name);
}
