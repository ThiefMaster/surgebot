#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "irc.h"
#include "conf.h"
#include "radioschedule.h"

MODULE_DEPENDS("commands", "help", NULL);

COMMAND(schedule_extend);
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

	DEFINE_COMMAND(this, "schedule extend",		schedule_extend,	1, 0, "group(admins)");
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

COMMAND(schedule_extend)
{
	int rc = -1;
	struct show_info show_info, conflict;
	struct tm tm_until;
	int hours, minutes;
	time_t until, old_endtime;

	if(sscanf(argv[1], "%2d:%2d", &hours, &minutes) != 2)
	{
		reply("Syntax: $b%s hh:mm$b", argv[0]);
		return 0;
	}

	if(minutes != 0 && minutes != 30)
	{
		reply("Sendungen können nur um :00 oder :30 enden.");
		return 0;
	}

	if(get_current_show_info(&show_info, 1500) != 0 || !show_info.entryid)
	{
		reply("Es wurde keine aktuelle Sendung gefunden");
		return 0;
	}

	assert_return(localtime_r(&now, &tm_until), 1);
	tm_until.tm_hour = hours;
	tm_until.tm_min = minutes;
	tm_until.tm_sec = 0;
	until = mktime(&tm_until);
	if(until < show_info.endtime)
	{
		tm_until.tm_mday++;
		until = mktime(&tm_until);
	}

	if(until < show_info.endtime)
	{
		// Happens if show ends 2 days later.. will probably never happen
		reply("Diese Sendung kann nur über die Website verlängert werden");
		return 0;
	}

	if((until - show_info.endtime) > 3*3600)
	{
		reply("Wenn du die Sendung wirklich um mehr als 3 Stunden verlängern willst, verlängere sie in 3-Stunden-Schritten oder benutze die Website.");
		return 0;
	}

	if(until == show_info.endtime)
	{
		reply("Die Sendung endet bereits zu dieser Uhrzeit");
		return 0;
	}

	old_endtime = show_info.endtime;
	show_info.endtime = until;

	do
	{
		rc = get_conflicting_show_info(&show_info, &conflict);
		if(rc != 0)
		{
			reply("Es ist ein Fehler aufgetreten.");
			return 0;
		}
		else if(conflict.entryid)
		{
			char buf1[8], buf2[8];
			strftime(buf1, sizeof(buf1), "%H:%M", localtime(&show_info.endtime));
			strftime(buf2, sizeof(buf2), "%H:%M", localtime(&conflict.starttime));
			reply("Die Sendung kann nicht bis %s verlängert werden, da um %s eine andere Sendung beginnt.", buf1, buf2);
			// If the conflicting show doesn't start immediately after our show, extend as much as possible
			if(conflict.starttime > (old_endtime + 1800))
			{
				show_info.endtime = conflict.starttime;
				continue;
			}
			return 0;
		}
	}
	while(0); // if we want to re-run the loop, we continue.

	if((rc = extend_show(&show_info)) == 0)
	{
		char buf[8];
		strftime(buf, sizeof(buf), "%H:%M", localtime(&show_info.endtime));
		reply("Die Sendung wurde bis $b%s$b verlängert.", buf);
	}

	return (rc == 0);
}

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
