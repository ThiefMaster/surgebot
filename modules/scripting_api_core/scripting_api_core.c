#include "global.h"
#include "module.h"
#include "irc_handler.h"
#include "irc.h"
#include "ptrlist.h"
#include "modules/scripting/scripting.h"

MODULE_DEPENDS("scripting", NULL);

SCRIPTING_FUNC(reg_irc_handler);
SCRIPTING_FUNC(unreg_irc_handler);
SCRIPTING_FUNC(irc_send);
static void module_unloaded(struct module *module);
static void _irc_handler(int argc, char **argv, struct irc_source *src, struct scripting_arg *func);
static int _cmp_func(struct scripting_arg *func_a, struct scripting_arg *func_b);
static struct scripting_arg *_irc_source_to_arg(struct irc_source *src);

static struct module *this;
static int num_irc_handlers;

MODULE_INIT
{
	this = self;
	reg_module_load_func(NULL, module_unloaded);
	REG_SCRIPTING_FUNC(reg_irc_handler);
	REG_SCRIPTING_FUNC(unreg_irc_handler);
	REG_SCRIPTING_FUNC(irc_send);
}

MODULE_FINI
{
	unreg_module_load_func(NULL, module_unloaded);
}

static void module_unloaded(struct module *module)
{
	if(num_irc_handlers) {
		dict_iter(node, irc_handlers_dict()) {
			struct irc_handler_list *list = node->data;
			for(int i = 0; i < (int)list->count; i++) {
				struct irc_handler *handler = list->data[i];
				if(handler->func != (irc_handler_f *)_irc_handler) {
					continue;
				}
				struct scripting_arg *func = handler->extra;
				if(func->callable_module == module) {
					debug("unreg irc handler %p", func);
					_unreg_irc_handler(node->key, (irc_handler_f *)_irc_handler, func);
					num_irc_handlers--;
					i--;
				}
			}
		}
	}
}

SCRIPTING_FUNC(reg_irc_handler)
{
	const char *cmd = scripting_arg_get(args, "cmd", SCRIPTING_ARG_TYPE_STRING);
	if(!cmd) {
		return scripting_raise_error("Required argument missing: cmd");
	}
	struct scripting_arg *func = scripting_arg_get(args, "func", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!func) {
		return scripting_raise_error("Required argument missing: func");
	}
	debug("reg_irc_handler: cmd=%s, func=%p,%p", cmd, func, func->callable);
	_reg_irc_handler(cmd, (irc_handler_f *)_irc_handler, func, (irc_handler_extra_cmp_f*)_cmp_func);
	num_irc_handlers++;
	return NULL;
}

SCRIPTING_FUNC(unreg_irc_handler)
{
	const char *cmd = scripting_arg_get(args, "cmd", SCRIPTING_ARG_TYPE_STRING);
	if(!cmd) {
		return scripting_raise_error("Required argument missing: cmd");
	}
	struct scripting_arg *func = scripting_arg_get(args, "func", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!func) {
		return scripting_raise_error("Required argument missing: func");
	}
	debug("unreg_irc_handler: cmd=%s, func=%p,%p", cmd, func, func->callable);
	_unreg_irc_handler(cmd, (irc_handler_f *)_irc_handler, func);
	num_irc_handlers--;
	return NULL;
}

SCRIPTING_FUNC(irc_send)
{
	const char *msg = scripting_arg_get(args, "msg", SCRIPTING_ARG_TYPE_STRING);
	if(!msg) {
		return scripting_raise_error("Required argument missing: msg");
	}
	int *raw = scripting_arg_get(args, "raw", SCRIPTING_ARG_TYPE_BOOL);
	if(!raw || !*raw)
		irc_send("%s", msg);
	else
		irc_send_raw("%s", msg);
	return NULL;
}

static void _irc_handler(int argc, char **argv, struct irc_source *src, struct scripting_arg *func)
{
	struct dict *args = scripting_args_create_dict();
	struct scripting_arg *arg;
	// args: list of the handler args
	arg = scripting_arg_create(SCRIPTING_ARG_TYPE_LIST, NULL);
	for(int i = 0; i < argc; i++) {
		struct scripting_arg *val = scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(argv[i]));
		ptrlist_add(arg->data.list, 0, val);
	}
	dict_insert(args, strdup("args"), arg);
	// src: dict containing the message source
	dict_insert(args, strdup("src"), _irc_source_to_arg(src));
	func->callable_caller(func->callable, args);
	dict_free(args);
}

static int _cmp_func(struct scripting_arg *func_a, struct scripting_arg *func_b)
{
	return func_a->callable == func_b->callable;
}

static struct scripting_arg *_irc_source_to_arg(struct irc_source *src)
{
	struct scripting_arg *arg, *val;
	if(!src) {
		return scripting_arg_create(SCRIPTING_ARG_TYPE_NULL);
	}
	arg = scripting_arg_create(SCRIPTING_ARG_TYPE_DICT,
			"nick", scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->nick)),
			"ident", src->ident ? scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->ident)) : NULL,
			"host", src->host ? scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->host)) : NULL,
			NULL);
	return arg;
}
