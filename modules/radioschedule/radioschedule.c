#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "irc.h"
#include "conf.h"
#include "radioschedule.h"

MODULE_DEPENDS("commands", "help", NULL);

//COMMAND(schedule_add);
COMMAND(schedule_next);
COMMAND(schedule_current);
static void radioschedule_conf_reload();
extern void testblah();

static struct module *this;

MODULE_INIT
{
	this = self;

	reg_conf_reload_func(radioschedule_conf_reload);
	radioschedule_conf_reload();

	help_load(self, "radioschedule.help");

	//DEFINE_COMMAND(this, "schedule add",		schedule_add,		2, 0, "group(admins)");
	DEFINE_COMMAND(this, "schedule next",		schedule_next,		0, 0, "true");
	DEFINE_COMMAND(this, "schedule current",	schedule_current,	0, 0, "true");
}

MODULE_FINI
{
	unreg_conf_reload_func(radioschedule_conf_reload);
}

static void radioschedule_conf_reload()
{
	char *str;

	str = conf_get("radioschedule/mysql_host", DB_STRING);
	radioschedule_conf.mysql_host = str ? str : "localhost";

	str = conf_get("radioschedule/mysql_user", DB_STRING);
	radioschedule_conf.mysql_user = str ? str : "root";

	str = conf_get("radioschedule/mysql_pass", DB_STRING);
	radioschedule_conf.mysql_pass = str ? str : "";

	str = conf_get("radioschedule/mysql_db", DB_STRING);
	radioschedule_conf.mysql_db = str ? str : "test";
}

/*
COMMAND(schedule_add)
{

}
*/

COMMAND(schedule_next)
{
	next_show(src);
	return 1;
}

COMMAND(schedule_current)
{
	current_show(src);
	return 1;
}
