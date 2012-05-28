#include "global.h"
#include "module.h"
#include "ptrlist.h"
#include "scripting.h"
#include "stringbuffer.h"

MODULE_DEPENDS(NULL);

static void module_unloaded(struct module *module);
static void free_function(struct scripting_func *func);
static struct scripting_arg *scripting_arg_callable_copy(struct scripting_arg *arg);

static struct module *this;
static struct dict *scripting_funcs;
static const char *scripting_error;

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

struct dict *scripting_call_function(struct scripting_func *func, struct dict *args, struct module *module)
{
	scripting_error = NULL;
	return func->caller(func->extra, args, module);
}

void *scripting_raise_error(const char *msg)
{
	scripting_error = msg;
	return NULL;
}

const char *scripting_get_error()
{
	return scripting_error;
}

static void module_unloaded(struct module *module)
{
	dict_iter(node, scripting_funcs) {
		struct scripting_func *func = node->data;
		if(func->module == module) {
			dict_delete_node(scripting_funcs, node);
		}
	}
}

static void free_function(struct scripting_func *func)
{
	debug("Unregistered function: %s.%s", func->module->name, func->name);
	if(func->freeer) {
		func->freeer(func->extra, &func->extra);
	}
	free(func->name);
}

// arguments
struct scripting_arg *scripting_arg_create(enum scripting_arg_type type, ...)
{
	va_list args;
	char *key;
	void *value;
	struct scripting_arg *arg;
	va_start(args, type);

	arg = malloc(sizeof(struct scripting_arg));
	memset(arg, 0, sizeof(struct scripting_arg));
	arg->type = type;

	switch(type) {
		case SCRIPTING_ARG_TYPE_NULL:
			break;
		case SCRIPTING_ARG_TYPE_BOOL:
			arg->data.integer = malloc(sizeof(long));
			*arg->data.integer = !!va_arg(args, int);
			break;
		case SCRIPTING_ARG_TYPE_INT:
			arg->data.integer = malloc(sizeof(long));
			*arg->data.integer = va_arg(args, long);
			break;
		case SCRIPTING_ARG_TYPE_DOUBLE:
			arg->data.dbl = malloc(sizeof(double));
			*arg->data.dbl = va_arg(args, double);
			break;
		case SCRIPTING_ARG_TYPE_STRING:
			arg->data.string = va_arg(args, char *);
			break;
		case SCRIPTING_ARG_TYPE_LIST:
			arg->data.list = scripting_args_create_list();
			while((value = va_arg(args, struct scripting_arg *))) {
				ptrlist_add(arg->data.list, 0, value);
			}
			break;
		case SCRIPTING_ARG_TYPE_DICT:
			arg->data.dict = scripting_args_create_dict();
			while((key = va_arg(args, char *))) {
				value = va_arg(args, struct scripting_arg *);
				dict_insert(arg->data.dict, strdup(key), value ? value : scripting_arg_create(SCRIPTING_ARG_TYPE_NULL));
			}
			break;
		case SCRIPTING_ARG_TYPE_CALLABLE:
			break;
		case SCRIPTING_ARG_TYPE_RESOURCE:
			arg->resource = va_arg(args, void *);
			break;
	}

	va_end(args);
	return arg;
}

struct dict *scripting_args_create_dict()
{
	struct dict *args = dict_create();
	dict_set_free_funcs(args, free, (dict_free_f*)scripting_arg_free);
	return args;
}

struct ptrlist *scripting_args_create_list()
{
	struct ptrlist *list = ptrlist_create();
	ptrlist_set_free_func(list, (ptrlist_free_f*)scripting_arg_free);
	return list;
}

void scripting_arg_free(struct scripting_arg *arg)
{
	if(!arg) {
		return;
	}
	switch(arg->type) {
		case SCRIPTING_ARG_TYPE_NULL:
			break;
		case SCRIPTING_ARG_TYPE_BOOL:
		case SCRIPTING_ARG_TYPE_INT:
		case SCRIPTING_ARG_TYPE_DOUBLE:
		case SCRIPTING_ARG_TYPE_STRING:
			free(arg->data.ptr);
			break;
		case SCRIPTING_ARG_TYPE_LIST:
			ptrlist_free(arg->data.list);
			break;
		case SCRIPTING_ARG_TYPE_DICT:
			dict_free(arg->data.dict);
			break;
		case SCRIPTING_ARG_TYPE_CALLABLE:
			arg->callable_freeer(arg->callable, &arg->callable);
			break;
		case SCRIPTING_ARG_TYPE_RESOURCE:
			break;
	}

	free(arg);
}

static struct scripting_arg *scripting_arg_callable_copy(struct scripting_arg *arg)
{
	assert_return(arg->type == SCRIPTING_ARG_TYPE_CALLABLE, NULL);
	struct scripting_arg *copy = malloc(sizeof(struct scripting_arg));
	memcpy(copy, arg, sizeof(struct scripting_arg));
	copy->callable_taker(copy->callable, &copy->callable);
	return copy;
}

void scripting_arg_callable_free(struct scripting_arg *arg)
{
	assert(arg->type == SCRIPTING_ARG_TYPE_CALLABLE);
	scripting_arg_free(arg);
}


void *scripting_arg_get(struct dict *args, const char *arg_path, enum scripting_arg_type type)
{
	char *path = strdup(arg_path);
	char *orig_path = path;
	struct stringbuffer *buf = stringbuffer_create();
	struct scripting_arg *arg;

	if(*path == '.') { // leading dot -> get rid of it
		path++;
	}

	if(strlen(path) && path[strlen(path) - 1] == '.') { // trailing dot -> get rid of it
		path[strlen(path) - 1] = '\0';
	}

	if(!strlen(path)) {
		stringbuffer_free(buf);
		return NULL;
	}

	while(strchr(path, '.')) {
		if(*path == '.') { // next path element starting -> update record with previous path element
			arg = dict_find(args, buf->string);
			if(!arg || arg->type != SCRIPTING_ARG_TYPE_DICT) { // not found or not a dict
				stringbuffer_free(buf);
				free(orig_path);
				return NULL;
			}
			args = arg->data.dict;
			stringbuffer_flush(buf);
			path++;
		}
		else { // path not yet complete
			stringbuffer_append_char(buf, *path);
			path++;
		}
	}

	arg = dict_find(args, path); // find node in last path path
	stringbuffer_free(buf);
	free(orig_path);
	if(!arg || arg->type != type) {
		return NULL;
	}

	switch(arg->type) {
		case SCRIPTING_ARG_TYPE_NULL:
			return NULL; // nonsense, why fetch an arg knowing it's NULL
		case SCRIPTING_ARG_TYPE_BOOL:
		case SCRIPTING_ARG_TYPE_INT:
			return arg->data.integer;
		case SCRIPTING_ARG_TYPE_DOUBLE:
			return arg->data.dbl;
		case SCRIPTING_ARG_TYPE_STRING:
			return arg->data.string;
		case SCRIPTING_ARG_TYPE_LIST:
			return arg->data.list;
		case SCRIPTING_ARG_TYPE_DICT:
			return arg->data.dict;
		case SCRIPTING_ARG_TYPE_CALLABLE:
			return scripting_arg_callable_copy(arg);
		case SCRIPTING_ARG_TYPE_RESOURCE:
			return arg->resource;
	}

	return NULL;
}
