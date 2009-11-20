#include "global.h"
#include "chanserv_eventlog.h"
#include "string.h"
#include "chanuser.h"
#include "module.h"
#include "timer.h"
#include "irc.h"
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "modules/db/db.h"
#include "modules/tools/tools.h"
#include "modules/srvx/srvx.h"

MODULE_DEPENDS("chanreg", "commands", "tools", "db", "srvx", NULL);

// Not more than 200, srvx sets it to 10 otherwise!
#define CHANSERV_FETCH_EVENTS_MAX 200

struct chanserv_event
{
	time_t timestamp;
	struct irc_source src;
	char *account;
	char *command;
};

COMMAND(events);
DB_SELECT_CB(show_events_cb);
static void chanserv_events_db_read(struct dict *db_nodes, struct chanreg *reg);
static int chanserv_events_db_write(struct database_object *dbo, struct chanreg *reg);
static void srvx_response_events(struct srvx_request *r, const char *channelname);
static void chanserv_event_add(struct chanreg *reg, struct tm calendar, const char *channel, const char *issuer, const char *command);
static void chanserv_event_timer_add();
static void chanserv_event_timer_del();
static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason);
static int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void cmod_moved(struct chanreg *reg, const char *from, const char *to);
static void chanserv_fetch_events(void *bound, struct chanreg *reg);

static struct module *this;
static struct db_table *event_table;
static struct chanreg_module *cmod;
static struct dict *last_events;

static const struct column_desc event_table_cols[] = {
	{ "id", DBTYPE_SERIAL },
	{ "time", DBTYPE_DATETIME },
	{ "channel", DBTYPE_STRING },
	{ "nick", DBTYPE_STRING },
	{ "ident", DBTYPE_STRING },
	{ "host", DBTYPE_STRING },
	{ "account", DBTYPE_STRING },
	{ "command", DBTYPE_STRING },
	{ NULL, DBTYPE_NUM_TYPES }
};

MODULE_INIT
{
	this = self;
	cmod = chanreg_module_reg("EventLog", 0, chanserv_events_db_read, chanserv_events_db_write, cmod_enabled, cmod_disabled, cmod_moved);

	DEFINE_COMMAND(self, "events", events, 0, CMD_ACCEPT_CHANNEL, "chanuser(350) || group(admins)");

	event_table = db_table_open("chanserv_events", event_table_cols);
	if(!event_table)
		log_append(LOG_ERROR, "Could not open eventlog table.");

	last_events = dict_create();
	dict_set_free_funcs(last_events, NULL, free);

	chanreg_module_readdb(cmod);
	chanserv_event_timer_add();
}

MODULE_FINI
{
	chanserv_event_timer_del();
	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);
	dict_free(last_events);

	if(event_table)
		db_table_close(event_table);
}

static void chanserv_events_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	const char *str;

	if((str = database_fetch(db_nodes, "last_event_ts", DB_STRING)))
	{
		time_t *ts = malloc(sizeof(time_t));
		*ts = strtoul(str, NULL, 10);
		dict_insert(last_events, reg->channel, ts);
	}
}

static int chanserv_events_db_write(struct database_object *dbo, struct chanreg *reg)
{
	time_t *ts;
	if((ts = dict_find(last_events, reg->channel)))
		database_obj_write_long(dbo, "last_event_ts", *ts);

	return 0;
}


const struct column_desc *chanserv_event_table_cols()
{
	return event_table_cols;
}

static void srvx_response_events(struct srvx_request *r, const char *channelname)
{
	struct chanreg *reg;
	assert(r);

	if(!(reg = chanreg_find(channelname)))
	{
		log_append(LOG_WARNING, "Received events for unregistered channel %s", channelname);
		return;
	}

	for(unsigned int ii = 0; ii < r->count; ii++)
	{
		struct srvx_response_line *line = r->lines[ii];
		assert_continue(!strcasecmp(line->nick, "ChanServ"));

		char *str, *vec[8];
		unsigned int count;

		str = strip_codes(line->msg); // that messes up line->msg but it's not needed anymore

		// First line of events
		if(!strcmp(str, "The following channel events were found:"))
			continue;

		count = tokenize(strdupa(str), vec, ArraySize(vec), ' ', 0);

		// "You lack access..." after requesting events
		// -> Turn off eventlog module
		if((count == 5 && strncmp(str, "You lack access to", 18) == 0) || (count > 5 && strncmp(str, "You lack sufficient access in", 29) == 0))
		{
			if(chanreg_module_disable(reg, cmod, 0, CDR_DISABLED) == 0)
				irc_send("NOTICE @%s :My access has been clvl'ed to less than 350. Module $b%s$b has been disabled.", reg->channel, cmod->name);
			break;
		}

		// Last line of events
		if((count == 3 && !strcmp(vec[0], "Found") && !strcmp(vec[2], "matches.")) || !strcmp(str, "Nothing matched the criteria of your search."))
			break;

		// Line of events
		struct tm calendar;
		memset(&calendar, 0, sizeof(calendar));
		if(sscanf(str, "[%2d:%2d:%2d %2d/%2d/%4d]",
			&calendar.tm_hour, &calendar.tm_min, &calendar.tm_sec,
			&calendar.tm_mon, &calendar.tm_mday, &calendar.tm_year) == 6)
		{
			char *tmp, *tmp2, *channel, *issuer, *command;
			// (ChanServ:#') [Someone:Someone]: adduser *user 200

			// #') [Someone:Someone]: adduser *user 200
			tmp = str + 32; // 32 = length of "[??:??:?? ??/??/????] (ChanServ:"
			if(!(tmp2 = strchr(tmp, ' ')))
				return;
			channel = strndup(tmp, tmp2 - tmp - 1);

			// Someone:Someone]: adduser *user 200
			tmp = tmp2 + 2; // Point to issuer
			if((tmp2 = strchr(tmp, ' ')) == NULL)
			{
				free(channel);
				return;
			}

			issuer = strndup(tmp, tmp2 - tmp - 2);

			// adduser *user 200
			command = trim(strdup(tmp2 + 1));

			chanserv_event_add(reg, calendar, channel, issuer, command);
			free(channel);
			free(issuer);
			free(command);
			continue;
		}
	}
}

static void chanserv_event_add(struct chanreg *reg, struct tm calendar, const char *channel, const char *issuer, const char *command)
{
	time_t stamp, *last_event_ts;
	char *tmp, source[513] = {0}, account[513] = {0};
	struct chanserv_event event;
	size_t len;

	calendar.tm_year -= 1900;
	calendar.tm_mon--;

	if((stamp = mktime(&calendar)) == -1)
	{
		log_append(LOG_ERROR, "Could not convert given time to timestamp.");
		return;
	}

	last_event_ts = dict_find(last_events, reg->channel);
	if(!last_event_ts)
	{
		last_event_ts = malloc(sizeof(time_t));
		*last_event_ts = 0;
		dict_insert(last_events, reg->channel, last_event_ts);
	}

	// Make sure this is newer than the last event we captured
	if(stamp <= *last_event_ts)
	{
		debug("Event seems to be outdated; %lu <= %lu", stamp, *last_event_ts);
		return;
	}

	len = strlen(issuer);

	memset(&event, 0, sizeof(struct chanserv_event));
	event.timestamp = stamp;

	if((tmp = strchr(issuer, ':')))
	{
		int diff = tmp - issuer;
		if(diff > (int)(sizeof(source) - 1))
		{
			log_append(LOG_ERROR, "IRC Source issuer '%s' exceeds %lu chars", issuer, (unsigned long)(sizeof(source) - 1));
			return;
		}
		strncpy(source, issuer, diff);
		if((len - diff + 1) > (sizeof(account) - 1))
		{
			log_append(LOG_ERROR, "IRC Source account '%s' exceeds %lu chars", tmp, (unsigned long)(sizeof(account) - 1));
			return;
		}
		strcpy(account, tmp + 1);
	}
	else
	{
		if(len > (sizeof(source) - 1))
		{
			log_append(LOG_ERROR, "IRC Source '%s' exceeds %lu chars", issuer, (unsigned long)(sizeof(source) - 1));
			return;
		}
		strcpy(source, issuer);
	}

	if(!(tmp = strchr(source, '@')))
	{
		event.src.nick = strdup(source);
		event.src.ident = event.src.host = NULL;
	}
	else
	{
		char *tmp2;
		if(!(tmp2 = strchr(source, '!')) || tmp2 > tmp)
		{
			log_append(LOG_ERROR, "Invalid IRC mask '%s'", source);
			return;
		}

		event.src.nick = strndup(source, tmp2 - source);
		tmp2++;
		event.src.ident = strndup(tmp2, tmp - tmp2);
		event.src.host = strdup(tmp + 1);
	}

	if(!strcmp(event.src.nick, bot.nickname))
	{
		debug("We issued this event ourselves, skipping.");
		*last_event_ts = stamp;

		MyFree(event.src.nick);
		MyFree(event.src.ident);
		MyFree(event.src.host);
		return;
	}

	event.account = strdup(account);
	event.command = strdup(command);

	*last_event_ts = event.timestamp;

	if(event_table)
	{
		db_row_insert(event_table,
			"time",		event.timestamp,
			"channel",	reg->channel,
			"nick",		event.src.nick,
			"ident",	event.src.ident,
			"host",		event.src.host,
			"account",	event.account,
			"command",	event.command,
			NULL);
	}

	// If event should be kept at some point, store it and don't free it here
	MyFree(event.account);
	MyFree(event.command);
	MyFree(event.src.nick);
	MyFree(event.src.ident);
	MyFree(event.src.host);
}

static void chanserv_fetch_events(void *bound, struct chanreg *reg)
{
	if(reg)
	{
		assert(bound == NULL); // NOT called from timer!
		srvx_send_ctx((srvx_response_f*)srvx_response_events, strdup(reg->channel), 1, "CHANSERV %s EVENTS %u", reg->channel, CHANSERV_FETCH_EVENTS_MAX);
		return;
	}

	for(unsigned int i = 0; i < cmod->channels->count; i++)
	{
		struct chanreg *reg = cmod->channels->data[i];
		srvx_send_ctx((srvx_response_f*)srvx_response_events, strdup(reg->channel), 1, "CHANSERV %s EVENTS %u", reg->channel, CHANSERV_FETCH_EVENTS_MAX);
	}

	chanserv_event_timer_add();
}

static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason)
{
	if(reason == CER_ENABLED)
	{
		struct irc_channel *chan;
		struct irc_user *user;

		if(!(user = user_find("ChanServ")) || !(chan = channel_find(reg->channel)) || !channel_user_find(chan, user))
			return 1;
		if(!(chan->modes & MODE_REGISTERED))
			return 1;
		chanserv_fetch_events(NULL, reg);
	}
	return 0;
}

static int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
	{
		if(event_table)
		{
			db_row_drop(event_table, "channel", reg->channel, NULL);
			dict_delete(last_events, reg->channel);
		}
	}

	return 0;
}

static void cmod_moved(struct chanreg *reg, const char *from, const char *to)
{
	struct dict_node *node = dict_find_node(last_events, from);
	if(!node)
		return;
	free(node->key);
	node->key = strdup(to);
}


static void chanserv_event_timer_add()
{
	chanserv_event_timer_del();
	timer_add(this, "chanserv_update_events", now + 60, (timer_f*)chanserv_fetch_events, NULL, 0, 1);
}

static void chanserv_event_timer_del()
{
	timer_del_boundname(this, "chanserv_update_events");
}

DB_SELECT_CB(show_events_cb)
{
	char *src_nick = ctx;
	struct stringbuffer *sbuf;
	struct tm *tm;

	if(DB_EMPTY_RESULT())
	{
		reply_nick(src_nick, "No events found.");
		return 0;
	}

	// "time", "channel", "nick", "account", "command", "ident", "host"
	assert_return(values->count >= 5, 1);
	time_t time 	= values->data[0].u.datetime;
	char *channel	= values->data[1].u.string;
	char *nick	= values->data[2].u.string;
	char *account	= values->data[3].u.string;
	char *command	= values->data[4].u.string;
	char *ident	= values->data[5].u.string;
	char *host	= values->data[6].u.string;

	sbuf = stringbuffer_create();
	tm = localtime(&time);

	stringbuffer_append_printf(sbuf, "[%02d:%02d:%02d %02d/%02d/%04d] (%s) [%s",
		tm->tm_hour, tm->tm_min, tm->tm_sec,
		tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900,
		channel,
		nick);

	if(*ident && *host)
		stringbuffer_append_printf(sbuf, "!%s@%s", ident, host);

	if(*account)
	{
		stringbuffer_append_char(sbuf, ':');
		stringbuffer_append_string(sbuf, account);
	}

	stringbuffer_append_printf(sbuf, "]: %s", command);

	reply_nick(src_nick, "%s", sbuf->string);
	stringbuffer_free(sbuf);
	return 0;
}

COMMAND(events)
{
	int limit = 15;
	CHANREG_MODULE_COMMAND(cmod);

	if(!event_table)
	{
		reply("The connection to the database could not be established.");
		return 0;
	}

	if(argc > 1)
	{
		limit = atoi(argv[1]);
		if(limit < 1)
		{
			reply("Invalid limit: must be a positive integer.");
			return 0;
		}
	}

	db_async_select(event_table,
		show_events_cb,
		strdup(src->nick),
		free,
		"channel", reg->channel, NULL,
		"time", "channel", "nick", "account", "command", "ident", "host", NULL,
		"-time", "$LIMIT", limit, NULL);

	return 1;
}
