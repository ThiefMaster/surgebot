#ifndef MODULE_H
#define MODULE_H

#include "list.h"
#include "stringlist.h"
#include "hook.h"

#define MODULE_DEPENDS(NAME, ...)	void mod_depends(struct module *self)		\
					{							\
						module_set_depends(self, NAME, ##__VA_ARGS__);	\
					}

#define MODULE_INIT	void mod_init(struct module *self, unsigned int reload)
#define MODULE_FINI	void mod_fini(struct module *self, unsigned int reload)

extern unsigned int reloading_module;

struct module;
typedef void (module_f)(struct module *self);
typedef void (module_reload_f)(const char *name, unsigned char success, unsigned int errors, void *ctx);

enum module_states {
	MODULE_UNKNOWN,
	MODULE_LOADING,
	MODULE_ACTIVE
};

struct module {
	char		*name;
	void		*handle;

	enum module_states	state;

	struct stringlist	*depend;
	struct stringlist	*rdepend;

	module_f	*init_func;
	module_f	*fini_func;
};


void module_init();
void module_fini();

struct dict *module_dict();
int module_add(const char *name);
void module_reload_cmd(const char *name, const char *src_nick);
int module_reload(const char *name);
void module_get_rdeps(struct module *module, struct stringlist *rdeps);
struct module *module_find(const char *name);
int module_del(const char *name);

void module_set_depends(struct module *mod, const char *name, ...);

void reg_module_load_func(module_f *load_func, module_f *unload_func);
void unreg_module_load_func(module_f *load_func, module_f *unload_func);

DECLARE_LIST(module_load_func_list, module_f *)
DECLARE_LIST(module_unload_func_list, module_f *)

DECLARE_HOOKABLE(modules_loaded, ());

#endif
