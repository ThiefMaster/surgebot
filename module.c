#include "global.h"
#include "module.h"
#include "conf.h"
#include "surgebot.h"
#include "irc.h"

IMPLEMENT_LIST(module_load_func_list, module_f *)
IMPLEMENT_LIST(module_unload_func_list, module_f *)

IMPLEMENT_HOOKABLE(modules_loaded);

static struct module_load_func_list *module_load_funcs;
static struct module_unload_func_list *module_unload_funcs;
static struct dict *module_list;
unsigned int reloading_module = 0;

static void module_conf_reload();
static void module_do_cmd_reload();
static void module_release_dependencies(struct module *module);
static void module_free(struct module *module);
static int module_solve_dependencies(struct module *module);
static const char *module_get_filename(const char *name);

static struct {
	char	*name;
	void	*nick;
} cmd_reload;


void module_init()
{
	struct stringlist *slist;
	int i;

	module_load_funcs = module_load_func_list_create();
	module_unload_funcs = module_unload_func_list_create();
	module_list = dict_create();
	reg_conf_reload_func(module_conf_reload);

	memset(&cmd_reload, 0, sizeof(cmd_reload));

	if((slist = conf_get("core/modules", DB_STRINGLIST)))
	{
		for(i = 0; i < slist->count; i++)
			module_add(slist->data[i]);
	}

	CALL_HOOKS(modules_loaded, ());
}

void module_fini()
{
	unreg_conf_reload_func(module_conf_reload);

	while(dict_size(module_list) > 0)
	{
		struct dict_node *node = module_list->tail;
		while(node)
		{
			struct module *module = node->data;
			node = node->prev;

			if(module->rdepend->count == 0)
				module_del(module->name);
		}
	}

	dict_free(module_list);
	module_load_func_list_free(module_load_funcs);
	module_unload_func_list_free(module_unload_funcs);

	clear_modules_loaded_hooks();
}

static void module_conf_reload()
{
	struct stringlist *slist;
	int i;

	if((slist = conf_get("core/modules", DB_STRINGLIST)) == NULL)
	{
		log_append(LOG_WARNING, "Config entry core/modules missing; to unload all modules set it to an empty stringlist");
		return;
	}

	for(i = 0; i < slist->count; i++)
	{
		if(module_find(slist->data[i]) == NULL)
			module_add(slist->data[i]);
	}

	// we must try deleting modules multiple times to ensure all modules get unloaded which were just dependencies
	i = dict_size(module_list);
	while(i--)
	{
		struct dict_node *node = module_list->tail;
		while(node)
		{
			struct module *module = node->data;
			node = node->prev;

			if(stringlist_find(slist, module->name) == -1 && module->rdepend->count == 0)
				module_del(module->name);
		}
	}
}

struct dict *module_dict()
{
	return module_list;
}

struct module *module_find(const char *name)
{
	return dict_find(module_list, name);
}

int module_add(const char *name)
{
	struct module *module;
	char filename[PATH_MAX];
	module_f *depend_func;

	if((module = module_find(name)))
	{
		log_append(LOG_INFO, "Module %s is already loaded", name);
		return -1;
	}

	module = malloc(sizeof(struct module));
	memset(module, 0, sizeof(struct module));
	module->name	= strdup(name);
	module->state	= MODULE_LOADING;
	module->depend	= stringlist_create();
	module->rdepend	= stringlist_create();

	safestrncpy(filename, module_get_filename(name), sizeof(filename));
	log_append(LOG_INFO, "Loading module %s (%s)", name, filename);

	module->handle = dlopen(filename, RTLD_LAZY);
	if(module->handle == NULL)
	{
		log_append(LOG_WARNING, "Could not load module %s: %s", name, dlerror());
		module_free(module);
		return -2;
	}

	if(dlsym(module->handle, "mod_init") == NULL)
	{
		log_append(LOG_WARNING, "Module %s does not contain mod_init() function: %s", name, dlerror());
		module_free(module);
		return -3;
	}

	if(dlsym(module->handle, "mod_fini") == NULL)
	{
		log_append(LOG_WARNING, "Module %s does not contain mod_fini() function: %s", name, dlerror());
		module_free(module);
		return -4;
	}

	if((depend_func = dlsym(module->handle, "mod_depends")) == NULL)
	{
		log_append(LOG_WARNING, "Module %s does not contain mod_depends() function: %s", name, dlerror());
		module_free(module);
		return -5;
	}

	depend_func(module);
	dict_insert(module_list, module->name, module);

	if(module_solve_dependencies(module) != 0)
	{
		log_append(LOG_WARNING, "Module %s has unresolvable dependencies", name);
		module_release_dependencies(module);
		module_free(module);
		return -6;
	}

	dlclose(module->handle);
	module->handle = dlopen(filename, RTLD_NOW|RTLD_GLOBAL|RTLD_DEEPBIND);
	if(module->handle == NULL)
	{
		log_append(LOG_WARNING, "Could not initialize module %s: %s", name, dlerror());
		module_release_dependencies(module);
		module_free(module);
		return -7;
	}

	module->init_func = dlsym(module->handle, "mod_init");
	module->fini_func = dlsym(module->handle, "mod_fini");

	if(module->init_func == NULL || module->fini_func == NULL)
	{
		log_append(LOG_WARNING, "Could not initialize module %s; mod_init() or mod_fini() could not be loaded: %s", name, dlerror());
		module_release_dependencies(module);
		module_free(module);
		return -8;
	}

	module->state = MODULE_ACTIVE;
	module->init_func(module);

	for(int i = 0; i < module_load_funcs->count; i++)
		module_load_funcs->data[i](module);

	return 0;
}

void module_reload_cmd(const char *name, const char *src_nick)
{
	assert(cmd_reload.name == NULL);
	cmd_reload.name = strdup(name);
	cmd_reload.nick = strdup(src_nick);

	reg_loop_func(module_do_cmd_reload);
}

static void module_do_cmd_reload()
{
	unreg_loop_func(module_do_cmd_reload);
	assert(cmd_reload.name);

	if(module_reload(cmd_reload.name) == 0)
		irc_send_msg(cmd_reload.nick, "NOTICE", "Reloaded module $b%s$b.", cmd_reload.name);
	else
		irc_send_msg(cmd_reload.nick, "NOTICE", "Reloading module $b%s$b (or depending modules) failed - see logfile for details.", cmd_reload.name);

	free(cmd_reload.name);
	free(cmd_reload.nick);
	memset(&cmd_reload, 0, sizeof(cmd_reload));
}

int module_reload(const char *name)
{
	struct module *module, *depmod;
	struct stringlist *modules;
	unsigned int unloaded = 0, ret = 0;

	if((module = module_find(name)) == NULL)
	{
		debug("Module %s is not loaded", name);
		return -1;
	}

	reloading_module = 1;
	modules = stringlist_create();
	stringlist_add(modules, strdup(module->name));
	module_get_rdeps(module, modules);

	while(unloaded < modules->count)
	{
		for(int i = 0; i < modules->count; i++)
		{
			depmod = module_find(modules->data[i]);
			if(depmod && depmod->rdepend->count == 0)
			{
				module_del(modules->data[i]);
				unloaded++;
			}
		}
	}

	for(int i = 0; i < modules->count; i++)
	{
		if(module_find(modules->data[i]) == NULL)
			ret += module_add(modules->data[i]) ? 1 : 0;
	}

	stringlist_free(modules);
	reloading_module = 0;
	return ret;
}

void module_get_rdeps(struct module *module, struct stringlist *rdeps)
{
	for(int i = 0; i < module->rdepend->count; i++)
	{
		struct module *depmod = module_find(module->rdepend->data[i]);
		assert_continue(depmod);
		if(stringlist_find(rdeps, depmod->name) == -1)
			stringlist_add(rdeps, strdup(depmod->name));
		module_get_rdeps(depmod, rdeps);
	}
}

int module_del(const char *name)
{
	struct module *module;
	int i;

	if((module = module_find(name)) == NULL)
	{
		debug("Module %s is not loaded", name);
		return 0;
	}

	if(module->rdepend->count)
	{
		log_append(LOG_WARNING, "Cannot unload module %s; %d other modules depend on it", module->name, module->rdepend->count);
		return -1;
	}

	log_append(LOG_INFO, "Unloading module %s", module->name);
	for(i = 0; i < module_unload_funcs->count; i++)
		module_unload_funcs->data[i](module);

	module_release_dependencies(module);

	module->fini_func(module);
	module_free(module);
	return 0;
}

static void module_release_dependencies(struct module *module)
{
	int i, j;
	for(i = 0; i < module->depend->count; i++)
	{
		struct module *depmod = module_find(module->depend->data[i]);
		if(depmod)
		{
			for(j = 0; j < depmod->rdepend->count; j++)
			{
				if(!strcasecmp(depmod->rdepend->data[j], module->name))
				{
					stringlist_del(depmod->rdepend, j);
					j--;
				}
			}
		}
	}
}

static void module_free(struct module *module)
{
	if(module->handle)
		dlclose(module->handle);

	dict_delete(module_list, module->name);

	stringlist_free(module->depend);
	stringlist_free(module->rdepend);
	free(module->name);
	free(module);
}

static int module_solve_dependencies(struct module *module)
{
	int i;
	for(i = 0; i < module->depend->count; i++)
	{
		char *name = module->depend->data[i];
		struct module *depmod = module_find(name);

		if(!strcasecmp(module->name, name))
		{
			log_append(LOG_WARNING, "Module %s depends on itself, skipping dependency", module->name);
			continue;
		}

		if(depmod && depmod->state == MODULE_LOADING)
		{
			log_append(LOG_WARNING, "Dependency loop %s <-> %s", module->name, depmod->name);
			return -1;
		}
		else if(depmod) // module loaded
		{
			debug("Dependency %s already loaded", name);
			stringlist_add(depmod->rdepend, strdup(module->name));
			continue;
		}

		if((module_add(name) == 0) && (depmod = module_find(name))) // loaded successfully
		{
			log_append(LOG_INFO, "Loaded dependency %s for module %s", name, module->name);
			stringlist_add(depmod->rdepend, strdup(module->name));
		}
		else // could not load module
		{
			log_append(LOG_WARNING, "Module %s depends on module %s which could not be loaded", module->name, name);
			return -2;
		}
	}

	return 0;
}

void module_set_depends(struct module *module, const char *name, ...)
{
	va_list args;
	if(name == NULL)
	{
		debug("Module %s has no dependencies", module->name);
		return;
	}

	va_start(args, name);
	debug("Module %s depends on: ", module->name);

	for(; name; name = va_arg(args, const char *))
	{
		stringlist_add(module->depend, strdup(name));
		debug(" - %s", name);
	}

	va_end(args);
}

static const char *module_get_filename(const char *name)
{
	static char buf[PATH_MAX];
	const char *path;

	path = conf_get("core/module_path", DB_STRING);
	if(!path || strlen(path) == 0)
		path = ".";

	snprintf(buf, sizeof(buf), "%s/%s.so", path, name);
	return buf;
}

void reg_module_load_func(module_f *load_func, module_f *unload_func)
{
	if(load_func)
		module_load_func_list_add(module_load_funcs, load_func);
	if(unload_func)
		module_unload_func_list_add(module_unload_funcs, unload_func);
}

void unreg_module_load_func(module_f *load_func, module_f *unload_func)
{
	if(load_func)
		module_load_func_list_del(module_load_funcs, load_func);
	if(unload_func)
		module_unload_func_list_del(module_unload_funcs, unload_func);
}
