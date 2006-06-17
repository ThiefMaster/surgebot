#include "global.h"
#include "module.h"
#include "conf.h"

IMPLEMENT_LIST(module_unload_func_list, module_f *)

static struct module_unload_func_list *module_unload_funcs;
static struct dict *module_list;

static void module_conf_reload();
static struct module *module_find(const char *name);
static void module_release_dependencies(struct module *module);
static void module_free(struct module *module);
static int module_solve_dependencies(struct module *module);
static const char *module_get_filename(const char *name);


void module_init()
{
	struct stringlist *slist;
	int i;

	module_unload_funcs = module_unload_func_list_create();
	module_list = dict_create();
	reg_conf_reload_func(module_conf_reload);

	if((slist = conf_get("core/modules", DB_STRINGLIST)))
	{
		for(i = 0; i < slist->count; i++)
			module_add(slist->data[i]);
	}
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
	module_unload_func_list_free(module_unload_funcs);
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

static struct module *module_find(const char *name)
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

	return 0;
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

void reg_module_unload_func(module_f *func)
{
	module_unload_func_list_add(module_unload_funcs, func);
}

void unreg_module_unload_func(module_f *func)
{
	module_unload_func_list_del(module_unload_funcs, func);
}
