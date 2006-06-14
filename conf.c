#include "global.h"
#include "conf.h"
#include "database.h"

IMPLEMENT_LIST(conf_reload_func_list, conf_reload_f *)

static struct dict *cfg, *old_cfg;
static struct conf_reload_func_list *conf_reload_funcs;

int conf_init()
{
	conf_reload_funcs = conf_reload_func_list_create();

	old_cfg = NULL;
	if((cfg = database_load(CFG_FILE)) == NULL)
	{
		log_append(LOG_ERROR, "Could not parse bot config file (%s)", CFG_FILE);
		return 1;
	}

	return 0;
}

void conf_fini()
{
	dict_free(cfg);
	if(old_cfg)
		dict_free(old_cfg);
	conf_reload_func_list_free(conf_reload_funcs);
}

int conf_reload()
{
	struct dict *new_cfg;
	int i;

	if((new_cfg = database_load(CFG_FILE)) == NULL)
	{
		log_append(LOG_WARNING, "Could not parse new config - continuing with old config");
		return 1;
	}

	if(old_cfg)
		dict_free(old_cfg);

	old_cfg = cfg;
	cfg = new_cfg;

	for(i = 0; i < conf_reload_funcs->count; i++)
		conf_reload_funcs->data[i]();

	return 0;
}

void *conf_get(const char *path, enum db_type type)
{
	assert_return(cfg, NULL);
	return database_fetch(cfg, path, type);
}

void *conf_get_old(const char *path, enum db_type type)
{
	assert_return(old_cfg, NULL);
	return database_fetch(old_cfg, path, type);
}

void reg_conf_reload_func(conf_reload_f *func)
{
	conf_reload_func_list_add(conf_reload_funcs, func);
}

void unreg_conf_reload_func(conf_reload_f *func)
{
	conf_reload_func_list_del(conf_reload_funcs, func);
}
