#include "global.h"
#include "module.h"
#include "conf.h"
#include "modules/sharedmem/sharedmem.h"
#include "modules/pgsql/pgsql.h"

static void radiologger_conf_reload();
static void shared_memory_changed(struct module *module, const char *key, void *old, void *new);

static struct module *this;
static struct pgsql *pg_conn;

static struct {
	const char *db_conn_string;
} radiologger_conf;

MODULE_DEPENDS("sharedmem", "pgsql", NULL);

MODULE_INIT
{
	this = self;
	reg_conf_reload_func(radiologger_conf_reload);
	radiologger_conf_reload();
	reg_shared_memory_changed_hook(shared_memory_changed);
}

MODULE_FINI
{
	unreg_shared_memory_changed_hook(shared_memory_changed);
	unreg_conf_reload_func(radiologger_conf_reload);
}

static void radiologger_conf_reload()
{
	const char *str;

	str = conf_get("radiologger/db_conn_string", DB_STRING);
	radiologger_conf.db_conn_string = str ? str : "";

	if(!pg_conn || !(str = conf_get_old("radiologger/db_conn_string", DB_STRING)) || strcmp(str, radiologger_conf.db_conn_string))
	{
		struct pgsql *new_conn = pgsql_init(radiologger_conf.db_conn_string);
		if(new_conn)
		{
			if(pg_conn)
				pgsql_fini(pg_conn);
			pg_conn = new_conn;
		}
	}
}

static void shared_memory_changed(struct module *module, const char *key, void *old, void *new)
{
	const char *listeners, *mod;
	char *artist = NULL, *title = NULL;

	if(strcmp(module->name, "radiobot") || strcmp(key, "song") || !new)
		return;

	char *buf = strdup(new);
	char *pos = strstr(buf, " - ");
	if(pos)
	{
		*pos = '\0';
		artist = buf;
		title = strdup(pos + 3);
	}
	else
	{
		artist = NULL;
		title = buf;
	}

	listeners = shared_memory_get("radiobot", "listeners", NULL);
	mod = shared_memory_get("radiobot", "mod", NULL);

	debug("Radiostatus: %s | %s | %s | %s", listeners, mod, artist, title);
	if(listeners && pg_conn)
	{
		pgsql_query(pg_conn, "INSERT INTO title_history (artist, title, mod, listeners) VALUES ($1, $2, $3, $4)", 0,
				stringlist_build_n(4, artist, title, mod, listeners));
	}
	free(artist);
	free(title);
}
