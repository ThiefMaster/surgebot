#ifndef SCRIPTING_H
#define SCRIPTING_H

struct scripting_func;

typedef void (scripting_func_caller)(struct scripting_func *func);
typedef void (scripting_func_freeer)(struct scripting_func *func);

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
void scripting_call_function(struct scripting_func *func);

#endif
