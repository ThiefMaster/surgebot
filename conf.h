#ifndef CONF_H
#define CONF_H

#include "list.h"
#include "database.h"

#define CFG_FILE "surgebot.cfg"

typedef void (conf_reload_f)();

int conf_init();
void conf_fini();

int conf_reload();
void *conf_get(const char *path, enum db_type type);
void *conf_get_old(const char *path, enum db_type type);
#define conf_bool(PATH)		true_string(conf_get((PATH), DB_STRING))
#define conf_bool_old(PATH)	true_string(conf_get_old((PATH), DB_STRING))

void reg_conf_reload_func(conf_reload_f *func);
void unreg_conf_reload_func(conf_reload_f *func);

DECLARE_LIST(conf_reload_func_list, conf_reload_f *)

#endif

