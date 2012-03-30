#ifndef SCRIPTING_H
#define SCRIPTING_H

enum scripting_arg_type {
	ARG_TYPE_NONE, // invalid
	ARG_TYPE_BOOL, // boolean
	ARG_TYPE_INTEGER, // integer
	ARG_TYPE_DOUBLE, // double
	ARG_TYPE_STRING, // string
	ARG_TYPE_LIST, // list/array
	ARG_TYPE_DICT // dict/object/associative array
};

struct scripting_arg_value {
	enum scripting_arg_type type;
	union
	{
		void *ptr;
		int64_t *integer;
		double *dbl;
		char *string;
		struct ptrlist *list;
		struct dict *object;
	} value;
};

struct scripting_func_arg {
	char *name;
	enum scripting_arg_type type;
	uint8_t required;
	struct scripting_arg_value *default_value;
};

struct scripting_func {
	char *module;
	char *name;
	uint8_t num_args;
	uint8_t cur_args;
	struct scripting_func_arg **args;
};

uint8_t scripting_register_function(const char *module, const char *name, uint8_t num_args);
void scripting_unregister_function(const char *module, const char *name);
void scripting_register_argument(struct scripting_func *func, const char *name, enum scripting_arg_type type, uint8_t required, struct scripting_arg_value *default_value);
struct scripting_func *scripting_find_function(const char *module, const char *name);

#endif
