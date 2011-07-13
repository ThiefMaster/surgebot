#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "modules/radioplaylist/pgsql.h"
#include "irc.h"
#include "conf.h"
#include "radioschedule.h"

MODULE_DEPENDS("commands", "help", "tools", "radioplaylist", NULL);
// TODO: remove radioplaylist dependency as soon as pgsql is a module

COMMAND(schedule_add);
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

	DEFINE_COMMAND(this, "schedule add",		schedule_add,		3, 0, "group(admins)");
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

	str = conf_get("radioschedule/db_conn_string", DB_STRING);
	radioschedule_conf.db_conn_string = str ? str : "";

	if(!radioschedule_conf.pg_conn || !(str = conf_get_old("radioschedule/db_conn_string", DB_STRING)) || strcmp(str, radioschedule_conf.db_conn_string))
	{
		struct pgsql *new_conn = pgsql_init(radioschedule_conf.db_conn_string);
		if(new_conn)
		{
			if(radioschedule_conf.pg_conn)
				pgsql_fini(radioschedule_conf.pg_conn);
			radioschedule_conf.pg_conn = new_conn;
		}
	}
}

COMMAND(schedule_add)
{
	unsigned int arg_offset = 0;
	int rc;
	struct show_info show_info, conflict;
	struct tm tm_start, tm_end, tm_now;
	int hours_start, minutes_start;
	int hours_end, minutes_end;
	time_t start, end;
	char nickbuf[64], *nick;
	size_t nicklen;
	const char *show_title;

	memset(&show_info, 0, sizeof(struct show_info));

	if(match("??:??", argv[1]) != 0 && match("?:??", argv[1]) != 0)
	{
		debug("First argument doesn't seem to be a timespec; assuming nick");
		nick = argv[1];
		arg_offset = 1;
		if(argc < 5)
			return -1;
	}
	else
	{
		strlcpy(nickbuf, src->nick, sizeof(nickbuf));
		nick = nickbuf;
		nicklen = strlen(nick);
		if(!strncasecmp(nick, "eX`", 3))
			nick += 3, nicklen -= 3;
		if(nicklen >= 4)
		{
			if(!strcasecmp(nick + nicklen - 4, "`off") || !strcasecmp(nick + nicklen - 4, "`afk") || !strcasecmp(nick + nicklen - 4, "`dnd"))
				nick[nicklen - 4] = '\0', nicklen -= 4;
			else if(nicklen >= 6 && !strcasecmp(nick + nicklen - 6, "`onAir"))
				nick[nicklen - 6] = '\0', nicklen -= 6;
			else if(nicklen >= 7 && !strncasecmp(nick + nicklen - 7, "`on", 3) && !strcasecmp(nick + nicklen - 3, "Air"))
				nick[nicklen - 7] = '\0', nicklen -= 7;
		}
		for(char *c = nick; *c; c++)
		{
			if(*c == '|' || *c == '`')
				memmove(c, c + 1, strlen(c));
		}
		debug("Sanitized nick: %s", nick);
	}

	if((show_info.userid = lookup_userid(nick)) == 0)
	{
		reply("Es existiert kein Website-User mit dem Nick $b%s$b der streamberechtigt ist; bitte gib den korrekten Nick im ersten Argument an.", nick);
		return 0;
	}

	if(sscanf(argv[1 + arg_offset], "%2d:%2d", &hours_start, &minutes_start) != 2 || sscanf(argv[2 + arg_offset], "%2d:%2d", &hours_end, &minutes_end) != 2)
	{
		reply("Syntax: $b%s hh:mm hh:mm ...$b", argv[0]);
		return 0;
	}

	if(minutes_start != 0 && minutes_start != 30)
	{
		reply("Sendungen können nur um :00 oder :30 anfangen.");
		return 0;
	}

	if(minutes_end != 0 && minutes_end != 30)
	{
		reply("Sendungen können nur um :00 oder :30 aufhören.");
		return 0;
	}

	assert_return(localtime_r(&now, &tm_start), 1);
	assert_return(localtime_r(&now, &tm_end), 1);
	assert_return(localtime_r(&now, &tm_now), 1);

	tm_start.tm_hour = hours_start;
	tm_start.tm_min = minutes_start;
	tm_start.tm_sec = 0;
	tm_end.tm_hour = hours_end;
	tm_end.tm_min = minutes_end;
	tm_end.tm_sec = 0;

	if(hours_start < tm_now.tm_hour || (hours_start == tm_now.tm_hour && minutes_start < tm_now.tm_min))
	{
		// Only bump day if it's not someone who's just late with adding his show
		if(mktime(&tm_start) < (now - 1800))
			tm_start.tm_mday++;
	}

	if(hours_end < tm_now.tm_hour || (hours_end == tm_now.tm_hour && minutes_end < tm_now.tm_min))
		tm_end.tm_mday++;

	start = mktime(&tm_start);
	end = mktime(&tm_end);

	if(start > (now + 20*3600))
	{
		reply("Du kannst Sendungen max. 20h im Voraus über den Bot eintragen");
		return 0;
	}

	if(start < (now - 1800))
	{
		reply("Du willst in der Vergangenheit streamen?");
		return 0;
	}

	if((end - start) > (6 * 3600))
	{
		reply("Wenn die Sendung wirklich länger als 6 Stunden dauern soll, benutze bitte die Website.");
		return 0;
	}

	if(start > end)
	{
		reply("Der Endzeitpunkt darf nicht vor dem Startzeitpunkt liegen.");
		return 0;
	}
	else if(start == end)
	{
		reply("Start- und Endzeitpunkt dürfen nicht identisch sein.");
		return 0;
	}

	show_info.starttime = start;
	show_info.endtime = end;

	if(get_conflicting_show_info(&show_info, &conflict) != 0)
	{
		reply("Es ist ein Fehler aufgetreten.");
		return 0;
	}
	else if(conflict.entryid)
	{
		char buf1[8], buf2[8];
		strftime(buf1, sizeof(buf1), "%H:%M", localtime(&conflict.starttime));
		strftime(buf2, sizeof(buf2), "%H:%M", localtime(&conflict.endtime));
		reply("Die Sendung kann nicht eingetragen werden, da sie sich mit der Sendung von %s-%s überschneidet.", buf1, buf2);
		return 0;
	}

	show_title = argline + (argv[3 + arg_offset] - argv[0]);
	if((rc = add_show(&show_info, show_title)) == 0)
		reply("Die Sendung wurde erfolgreich eingetragen.");

	return (rc == 0);
}

COMMAND(schedule_extend)
{
	int rc;
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
			// If the conflicting show doesn't start immediately after our show (starttime == endtime), extend as much as possible
			if(conflict.starttime > old_endtime)
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
