/*
 * Unfortunately, SRVX don't provide easily parsable lists for events and users
 * which makes it rather complicated to cache them automatically.
 *
 * This module will constantly keep track of the chanserv users in a channel but
 * only keep events when the EventLog channel module is enabled. This means it
 * needs to check regularly whether ChanServ is in the channel and whether we
 * do have enough access to fetch events and unload the module in case either
 * does not apply (anymore).
 */

#include "modules/chanserv/chanserv.h"

struct module *this;
struct chanreg_module *cmod;

MODULE_DEPENDS("chanreg", "commands", "tools", "db", NULL);

COMMAND(users);
COMMAND(events);
COMMAND(csaccess);
IRC_HANDLER(join);
IRC_HANDLER(part);
IRC_HANDLER(notice);
DB_SELECT_CB(show_events_cb);

// To keep track of which channel to retrieve info for
static struct chanserv_channel *curchan;

// Event database table
struct db_table *event_table;

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
	curchan = NULL;

	chanserv_timer_add();
	chanserv_channels = ptrlist_create();
	ptrlist_set_free_func(chanserv_channels, (ptrlist_free_f*)chanserv_channel_free);

	chanserv_accounts = dict_create();
	dict_set_free_funcs(chanserv_accounts, NULL, (dict_free_f*)chanserv_account_free);
	chanserv_access_requests = ptrlist_create();
	ptrlist_set_free_func(chanserv_access_requests, (ptrlist_free_f*)chanserv_access_request_free);

	this = self;
	cmod = chanreg_module_reg(sz_chanserv_chanmod_events_name, 0, chanserv_db_read, chanserv_db_write, cmod_enabled, cmod_disabled);

	chanserv_channels_populate();
	chanserv_timer_add();

	DEFINE_COMMAND(self, "events",	events,	0,	CMD_ACCEPT_CHANNEL, "chanuser(350) || group(admins)");
	DEFINE_COMMAND(self, "users",	users,	0,	CMD_ACCEPT_CHANNEL, "chanuser(350) || group(admins)");
	DEFINE_COMMAND(self, "csaccess",	csaccess, 3, 0, "group(admins)");

	event_table = db_table_open(sz_chanserv_db_table, event_table_cols);
	if(!event_table)
		log_append(LOG_ERROR, "Could not open eventlog table.");

	chanreg_module_readdb(cmod);

	reg_irc_handler("NOTICE", notice);
	reg_irc_handler("PART", part);
	reg_irc_handler("JOIN", join);

	reg_channel_complete_hook(chanserv_channel_complete_hook);
	reg_user_del_hook(chanserv_user_del);
	chanserv_event_timer_add();
}

MODULE_FINI
{
	chanserv_event_timer_del();
	chanserv_timer_del();

	unreg_user_del_hook(chanserv_user_del);
	unreg_channel_complete_hook(chanserv_channel_complete_hook);

	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("NOTICE", notice);

	if(event_table)
		db_table_close(event_table);

	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	ptrlist_free(chanserv_access_requests);
	ptrlist_free(chanserv_channels);
	dict_free(chanserv_accounts);
}

const struct column_desc *chanserv_event_table_cols()
{
	return event_table_cols;
}

unsigned long parse_chanserv_duration(const char *duration)
{
	unsigned long number;
	const char *tmp, *tmp2;
	char last[51], cur[51]; // This should be fairly enough
	int diff;
	unsigned int i;
	size_t ret;

	static const struct {
		const char *msg_single;
		const char *msg_plural;
		long length;
	} unit[] = {
		{ "year",   "years", 365 * 24 * 60 * 60 },
		{ "week",   "weeks",   7 * 24 * 60 * 60 },
		{ "day",    "days",        24 * 60 * 60 },
		{ "hour",   "hours",            60 * 60 },
		{ "minute", "minutes",               60 },
		{ "second", "seconds",                1 }
	};

	if(!duration || !strcmp(duration, "Here"))
		return 0;

	if(!strcmp(duration, "Never"))
		return -1;

	tmp2 = tmp = duration;
	*last = '\0';
	ret = 0;

	do
	{
		// End of string
		if(!(tmp2 = strchr(tmp, ' ')))
			tmp2 = tmp + strlen(tmp);

		if((unsigned int)(tmp2 - tmp) > (sizeof(last) - 1))
		{
			log_append(LOG_ERROR, "String exceeding max length %lu: '%s'", (unsigned long)(sizeof(last) - 1), duration);
			return 0;
		}

		diff = tmp2 - tmp;
		strncpy(cur, tmp, diff);
		cur[diff] = '\0';
		tmp = tmp2 + 1;

		if(!*last)
		{
			if(aredigits(cur) && !((cur[0] == '0') && (cur[1] == '\0')))
				strcpy(last, cur);
			continue;
		}

		if(!(number = strtoul(last, NULL, 10)))
		{
			log_append(LOG_ERROR, "String '%s' doesn't seem to be a number", last);
			*last = '\0';
			continue;
		}

		*last = '\0';

		for(i = 0; i < ArraySize(unit); i++)
		{
			if(!strcasecmp(cur, unit[i].msg_single) || !strcasecmp(cur, unit[i].msg_plural))
			{
				ret += number * unit[i].length;
				break;
			}
		}
	}
	while(*tmp2);

	return ret;
}

static void chanserv_access_callback(const char *channel, const char *nick, int access)
{
	irc_send("PRIVMSG #surgebot.test :%s has access %d in %s", nick, access, channel);
}

COMMAND(csaccess)
{
	if(!IsChannelName(argv[1]))
	{
		reply("Not a valid channelname.");
		return 0;
	}

	chanserv_get_access_callback(argv[1], argv[2], chanserv_access_callback);
	return 0;
}

COMMAND(events)
{
	CHANREG_MODULE_COMMAND(cmod);

	if(!event_table)
	{
		reply("The connection to the database could not be established.");
		return 0;
	}

	db_async_select(event_table,
		show_events_cb,
		strdup(src->nick),
		free,
		"channel", reg->channel, NULL,
		"time", "channel", "nick", "account", "command", "ident", "host", NULL,
		NULL);

	return 1;
}

COMMAND(users)
{
	struct chanreg *reg;

	if(!(reg = chanreg_find(channelname)))
	{
		reply("$b%s$b is not registered with %s.", channelname, sz_chanserv_botname);
		return 0;
	}

	struct chanserv_channel *cschan = chanserv_channel_find(reg->channel);
	assert_return(cschan, 0);
	assert_return(cschan->users->count, 0);

	struct table *table = table_create(5, cschan->users->count);
	table_set_header(table, "Access", "Account", "Last seen", "Status", "Nick(s)");

	int i = 0;
	struct stringbuffer *sbuf = stringbuffer_create();

	dict_iter_rev(node, cschan->users)
	{
		struct chanserv_user *user = node->data;

		table->data[i][0] = strtab(user->access);
		table->data[i][1] = user->account->name;

		if(user->last_seen == 0)
			table->data[i][2] = "Here";
		else if(user->last_seen == -1)
			table->data[i][2] = "-";
		else
			table->data[i][2] = strdupa(duration2string(user->last_seen));

		if(user->status == CS_USER_SUSPENDED)
			table->data[i][3] = "Suspended";
		else if(user->status == CS_USER_VACATION)
			table->data[i][3] = "Vacation";
		else
			table->data[i][3] = "Normal";

		for(unsigned int i = 0; i < user->account->irc_users->count; i++)
		{
			struct irc_user *iuser = user->account->irc_users->data[i]->ptr;
			if(!dict_find(channel->users, iuser->nick))
				continue;

			if(sbuf->len)
				stringbuffer_append_string_n(sbuf, ", ", 2);

			stringbuffer_append_string(sbuf, iuser->nick);
		}
		if(sbuf->len)
			table->data[i][4] = strdupa(sbuf->string);
		else
			table->data[i][4] = "-";

		stringbuffer_flush(sbuf);
		i++;
	}

	stringbuffer_free(sbuf);

	table_send(table, src->nick);
	table_free(table);
	return 1;
}

IRC_HANDLER(join)
{
	struct chanreg *reg;

	if(strcmp(src->nick, sz_chanserv_botname))
		return;

	if(!(reg = chanreg_find(argv[1])))
		return;

	chanserv_channel_create(reg);
}

IRC_HANDLER(part)
{
	struct chanserv_channel *cschan;

	if(strcmp(src->nick, sz_chanserv_botname))
		return;

	if(!(cschan = chanserv_channel_find(argv[1])))
		return;

	if(stringlist_find(cschan->reg->active_modules, cmod->name) != -1)
	{
		if(chanreg_module_disable(cschan->reg, cmod, 0, CDR_DISABLED) == 0)
			chanserv_report(argv[1], "Module $b%s$b has automatically been disabled as ChanServ left the channel.", cmod->name);
	}
	ptrlist_del_ptr(chanserv_channels, cschan);
}

IRC_HANDLER(notice)
{
	extern int errno;
	char *str, *vec[8], *dup;
	unsigned int count;

	if(!src || strcasecmp(src->nick, sz_chanserv_botname) || strcasecmp(argv[1], bot.nickname) != 0)
		return;

	str = strip_codes(strdupa(argv[2]));
	dup = strdupa(str);

	count = tokenize(str, vec, ArraySize(vec), ' ', 0);

	// Three possible replies after requesting one's access
	//	PainCake (PainCake) lacks access to #gamesurge.
	//	PainCake (PainCake) has access 350 in #'.
	//	You must be in #staff (or on its userlist) to use this command.
	if(count >= 6 && !strncmp(dup + (vec[2] - vec[0]), "lacks access to", 15))
	{
		char *channel = strndup(vec[5], strlen(vec[5]) - 1);
		chanserv_access_request_handle_raw(channel, vec[0], 0);
		free(channel);
		return;
	}

	else if(count >= 7 && !strncmp(dup + (vec[2] - vec[0]), "has access", 10))
	{
		char *channel;
		if(count > 7) // Security override
			channel = strdup(vec[6]);
		else
			channel = strndup(vec[6], strlen(vec[6]) - 1);

		chanserv_access_request_handle_raw(channel, vec[0], atoi(vec[4]));
		free(channel);
		return;
	}

	else if(!strncmp(dup, "You must be in", 14))
	{
		chanserv_access_request_handle_raw(vec[4], NULL, -1);
		return;
	}

	// "You lack access..." after requesting events
	// -> Turn off eventlog module
	if((count == 5 && strncmp(dup, "You lack access to", 18) == 0) || (count > 5 && strncmp(dup, "You lack sufficient access in", 29) == 0))
	{
		if(count == 5)
		{
			// Remove trailing dot
			char *tmp = strndup(vec[4], strlen(vec[4]) - 1);
			curchan = chanserv_channel_find(tmp);
			free(tmp);
		}
		else
			curchan = chanserv_channel_find(vec[5]);

		if(curchan)
		{
			struct chanserv_channel *cschan = curchan;

			curchan->process = CS_P_NONE;
			curchan = NULL;

			if(chanreg_module_disable(cschan->reg, cmod, 0, CDR_DISABLED) == 0)
				chanserv_report(cschan->reg->channel, "My access has been clvl'ed to less than 350. Module $b%s$b has been disabled.", cmod->name);
		}
		return;
	}

	// First line of channel info
	if(count == 2 && IsChannelName(vec[0]) && !strcmp(vec[1], "Information:"))
	{
		if((curchan = chanserv_channel_find(vec[0])))
			curchan->process = CS_P_CHANINFO;
		return;
	}

	// First line of userlist is of the format "#channel users from level 1 to 500:"
	if(IsChannelName(vec[0]) && !strcmp(dup + (vec[1] - vec[0]), "users from level 1 to 500:"))
	{
		if((curchan = chanserv_channel_find(vec[0])))
		{
			curchan->process = CS_P_USERLIST;
			dict_clear(curchan->users);
		}
		return;
	}

	// First line of events
	if(!strcmp(dup, "The following channel events were found:"))
		return;

	// Last line of events
	if((count == 3 && !strcmp(vec[0], "Found") && !strcmp(vec[2], "matches.")) || !strcmp(dup, "Nothing matched the criteria of your search."))
	{
		if(curchan)
		{
			curchan->process = CS_P_NONE;
			curchan = NULL;
		}
		return;
	}

	// Line of events
	struct tm calendar;
	memset(&calendar, 0, sizeof(calendar));
	if(sscanf(dup, "[%2d:%2d:%2d %2d/%2d/%4d]",
		&calendar.tm_hour, &calendar.tm_min, &calendar.tm_sec,
		&calendar.tm_mon, &calendar.tm_mday, &calendar.tm_year) == 6)
	{
		char *tmp, *tmp2, *channel, *issuer, *command;
		// (ChanServ:#') [Someone:Someone]: adduser *user 200

		// #') [Someone:Someone]: adduser *user 200
		tmp = dup + 32; // 32 = length of "[??:??:?? ??/??/????] (ChanServ:"
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

		chanserv_event_add(calendar, channel, issuer, command);
		free(channel);
		free(issuer);
		free(command);
		return;
	}

	// Line of names list
	if(!strncmp(dup, "Users in", 8))
	{
		// No users
		if(count < 4)
			return;

		// First line?
		if(!curchan)
		{
			char *channel = strndup(vec[2], vec[3] - vec[2] - 2);
			if(!(curchan = chanserv_channel_find(channel)))
			{
				free(channel);
				return;
			}
			free(channel);
		}

		curchan->process = CS_P_NAMES;
		chanserv_user_parse_names(curchan, dup + (vec[3] - vec[0]));
		return;
	}

	// Last line of names
	if(!strncasecmp(dup, "End of names in", 15))
	{
		if(curchan)
		{
			curchan->process = CS_P_NONE;
			curchan = NULL;
		}
		return;
	}

	if(!curchan)
		return;

	// In channel info
	if(curchan->process == CS_P_CHANINFO)
	{
		if(!strncasecmp(dup, "Total User Count:", 17) && aredigits(vec[3]) && (curchan->user_count = atoi(vec[3])) > 0)
		{
			if(!curchan->user_count)
				log_append(LOG_ERROR, "Line '%s' returns usercount 0, something's wrong...", dup);
			// Retrieve userlist
			else
				irc_send(sz_chanserv_fetch_users, curchan->reg->channel);
		}
		return;
	}

	// In userlist
	if(curchan->process == CS_P_USERLIST)
		if(chanserv_user_add(curchan, dup, count, vec) != 0)
			curchan = NULL;
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
