#ifndef SCRIPTING_H
#define SCRIPTING_H

struct scripting_func;
struct ptrlist;

typedef struct dict *(scripting_func_caller)(void *func, struct dict *args);
typedef void (scripting_func_freeer)(void *func);
typedef void *(scripting_func_taker)(void *func);

struct scripting_func {
	struct module *module;
	char *name;
	scripting_func_caller *caller;
	scripting_func_freeer *freeer;
	void *extra;
};

struct scripting_func *scripting_register_function(struct module *module, const char *name);
uint8_t scripting_unregister_function(struct module *module, const char *name);
struct scripting_func *scripting_find_function(const char *name);
struct dict *scripting_call_function(struct scripting_func *func, struct dict *args);
void *scripting_raise_error(const char *msg);
const char *scripting_get_error();

// argument objects
enum scripting_arg_type {
	SCRIPTING_ARG_TYPE_NULL,
	SCRIPTING_ARG_TYPE_BOOL,
	SCRIPTING_ARG_TYPE_INT,
	SCRIPTING_ARG_TYPE_DOUBLE,
	SCRIPTING_ARG_TYPE_STRING,
	SCRIPTING_ARG_TYPE_LIST,
	SCRIPTING_ARG_TYPE_DICT,
	SCRIPTING_ARG_TYPE_CALLABLE
};

struct scripting_arg {
	enum scripting_arg_type type;
	union {
		void *ptr;
		long *integer;
		double *dbl;
		char *string;
		struct dict *dict;
		struct ptrlist *list;
	} data;
	void *callable;
	scripting_func_freeer *callable_freeer;
	scripting_func_taker *callable_taker;
};

struct scripting_arg *scripting_arg_create(enum scripting_arg_type type);
struct dict *scripting_args_create_dict();
struct ptrlist *scripting_args_create_list();
void scripting_arg_free(struct scripting_arg *arg);
void *scripting_arg_get(struct dict *args, const char *arg_path, enum scripting_arg_type type);
void scripting_arg_callable_free(struct scripting_arg *arg);

#define SCRIPTING_FUNC(NAME)		static struct dict * __scripting_func_ ## NAME(void *_func, struct dict *args)
#define REG_SCRIPTING_FUNC(NAME)	scripting_register_function(this, #NAME)->caller = __scripting_func_ ## NAME

#endif
