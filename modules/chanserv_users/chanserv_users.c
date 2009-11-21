#include "global.h"
#include "chanuser.h"
#include "irc.h"
#include "irc_handler.h"
#include "module.h"
#include "ptrlist.h"
#include "table.h"
#include "timer.h"
#include "modules/commands/commands.h"
#include "modules/tools/tools.h"
#include "modules/srvx/srvx.h"

MODULE_DEPENDS("commands", "tools", "srvx", NULL);

// Interval to fetch channel userlists
#define CHANSERV_UPDATE_USERS_INTERVAL 1800

struct chanserv_channel
{
	struct irc_channel *channel;
	struct dict *users;
	struct dict *tmp_users;
};

enum chanserv_user_status
{
	CS_USER_NORMAL,
	CS_USER_BOT,
	CS_USER_SUSPENDED,
	CS_USER_VACATION,
};

struct chanserv_account
{
	char *name;
	unsigned int vacation : 1;

	struct ptrlist *irc_users;
	// How many users use this account?
	unsigned int refcount;
};

struct chanserv_user
{
	unsigned int access;
	enum chanserv_user_status status;
	time_t last_seen;

	struct chanserv_account *account;
};

IRC_HANDLER(join);
COMMAND(users);
COMMAND(update);
PARSER_FUNC(chanserv);
static void chanuser_del_hook(struct irc_chanuser *chanuser, unsigned int del_type, const char *reason);
static void channel_complete_hook(struct irc_channel *channel);
static void channel_del_hook(struct irc_channel *channel, const char *reason);
static struct chanserv_channel *cschan_find(const char *channelname);
static struct chanserv_channel *cschan_add(struct irc_channel *channel);
static void cschan_del(struct chanserv_channel *cschan);
static void cschan_free(struct chanserv_channel *cschan);
static void csuser_free(struct chanserv_user *csuser);
static struct chanserv_account *csaccount_find(const char *name);
static void csaccount_free(struct chanserv_account *csaccount);
static void chanserv_update_users(void *bound, void *data);
static void chanserv_fetch_users(struct chanserv_channel *cschan);
static void srvx_response_users(struct srvx_request *r, const char *channelname);
static void srvx_response_names(struct srvx_request *r, const char *channelname);
static void chanserv_parse_name(struct chanserv_channel *cschan, const char *item);
static void chanserv_parse_user(struct chanserv_channel *cschan, const char *line, int argc, char **argv);
static unsigned long parse_chanserv_duration(const char *duration);

static struct module *this;
static struct dict *chanserv_accounts;
static struct dict *chanserv_channels;

MODULE_INIT
{
	this = self;
	reg_channel_del_hook(channel_del_hook);
	reg_channel_complete_hook(channel_complete_hook);
	reg_irc_handler("JOIN", join);

	chanserv_accounts = dict_create();
	dict_set_free_funcs(chanserv_accounts, NULL, (dict_free_f *)csaccount_free);
	chanserv_channels = dict_create();
	dict_set_free_funcs(chanserv_channels, NULL, (dict_free_f *)cschan_free);

	// Import existing channels
	dict_iter(node, channel_dict())
	{
		struct irc_channel *chan = node->data;
		if(chan->burst_state == BURST_FINISHED)
			channel_complete_hook(chan);
	}

	REG_COMMAND_RULE("chanserv", chanserv);
	DEFINE_COMMAND(this, "users", users, 1, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(this, "update", update, 1, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");

	timer_add(this, "chanserv_update_users", now + CHANSERV_UPDATE_USERS_INTERVAL, chanserv_update_users, NULL, 0, 1);
}

MODULE_FINI
{
	timer_del_boundname(this, "chanserv_update_users");
	command_rule_unreg("chanserv");
	dict_free(chanserv_channels);
	dict_free(chanserv_accounts);
	unreg_channel_complete_hook(channel_complete_hook);
	unreg_channel_del_hook(channel_del_hook);
}

COMMAND(users)
{
	struct chanserv_channel *cschan;
	struct table *table;

	if(!(cschan = cschan_find(channel->name)))
	{
		reply("$b%s$b is not registered with ChanServ.", channel->name);
		return 0;
	}

	assert_return(cschan->users->count, 0);

	table = table_create(5, cschan->users->count);
	table_set_header(table, "Access", "Account", "Last seen", "Status", "Nicks");

	int row = 0;
	struct stringbuffer *sbuf = stringbuffer_create();

	dict_iter_rev(node, cschan->users)
	{
		struct chanserv_user *csuser = node->data;

		table->data[row][0] = strtab(csuser->access);
		table->data[row][1] = csuser->account->name;

		if(csuser->last_seen == 0)
			table->data[row][2] = "Here";
		else if(csuser->last_seen == -1)
			table->data[row][2] = "-";
		else
			table->data[row][2] = strdupa(duration2string(csuser->last_seen));

		if(csuser->status == CS_USER_SUSPENDED)
			table->data[row][3] = "Suspended";
		else if(csuser->status == CS_USER_BOT)
			table->data[row][3] = "Bot";
		else if(csuser->status == CS_USER_VACATION)
			table->data[row][3] = "Vacation";
		else
			table->data[row][3] = "Normal";

		for(unsigned int i = 0; i < csuser->account->irc_users->count; i++)
		{
			struct irc_user *user = csuser->account->irc_users->data[i]->ptr;
			if(!channel_user_find(channel, user))
				continue;

			if(sbuf->len)
				stringbuffer_append_string(sbuf, ", ");

			stringbuffer_append_string(sbuf, user->nick);
		}

		if(sbuf->len)
			table->data[row][4] = strdupa(sbuf->string);
		else
			table->data[row][4] = "-";

		stringbuffer_flush(sbuf);
		row++;
	}

	stringbuffer_free(sbuf);

	table_send(table, src->nick);
	table_free(table);
	return 1;

}

COMMAND(update)
{
	struct chanserv_channel *cschan;
	if(!(cschan = cschan_find(channel->name)))
	{
		reply("$b%s$b is not registered with ChanServ.", channel->name);
		return 0;
	}

	reply("Updating ChanServ userlist for $b%s$b.", channel->name);
	chanserv_fetch_users(cschan);
	return 1;
}

PARSER_FUNC(chanserv)
{
	int res;
	struct command_rule_context *cr_ctx = ctx;
	struct chanserv_channel *cschan = NULL;
	char *pos, *access_string;
	unsigned int access;

	// If no argument given, there must be a channel with ChanServ in it
	if(!arg)
	{
		if(cr_ctx->channel && cschan_find(cr_ctx->channel->name))
			return RET_TRUE;

		return RET_FALSE;
	}

	// There is at least one argument, see if there are more
	if((pos = strchr(arg, ',')))
	{
		char *channel = trim(strndup(arg, pos - arg));

		// Not a channel or no access level given
		if(!IsChannelName(channel) || *++pos == '\0')
		{
			log_append(LOG_ERROR, "Invalid channel or no access level given in rule chanserv(%s).", arg);
			free(channel);
			return RET_NONE;
		}

		if(!(cschan = cschan_find(channel)))
		{
			free(channel);
			return RET_FALSE;
		}
		// Not needed anymore
		free(channel);
		access_string = trim(strdup(pos));
	}
	else
	{
		// One argument, associate channel
		assert_return(cr_ctx->channel, RET_NONE);
		if(!(cschan = cschan_find(cr_ctx->channel->name)))
		{
			log_append(LOG_ERROR, "ChanServ channel %s does not exist", cr_ctx->channel->name);
			return RET_NONE;
		}

		access_string = trim(strdup(arg));
	}

	access = atoi(access_string);
	if(!aredigits(access_string) || access > 500)
	{
		log_append(LOG_ERROR, "Invalid access level: %s (%u)", access_string, access);
		free(access_string);
		return RET_NONE;
	}
	// Not needed anymore
	free(access_string);

	dict_iter(node, cschan->users)
	{
		struct chanserv_user *csuser = node->data;
		if(csuser->access < access)
			continue;

		struct chanserv_account *csaccount = csuser->account;
		if(ptrlist_find(csaccount->irc_users, cr_ctx->user) != -1)
			return RET_TRUE;
	}
	return RET_FALSE;
}

IRC_HANDLER(join)
{
	struct irc_channel *channel;
	struct irc_user *user;

	assert(argc > 1);
	if(strcmp(src->nick, "ChanServ"))
		return;

	assert((user = user_find("ChanServ")));
	assert((channel = channel_find(argv[1])));
	assert(channel_user_find(channel, user));

	debug("chanserv_users: Adding %s (ChanServ joined)", channel->name);
	cschan_add(channel);
}

static void chanuser_del_hook(struct irc_chanuser *chanuser, unsigned int del_type, const char *reason)
{
	struct chanserv_channel *cschan;

	if(strcmp(chanuser->user->nick, "ChanServ"))
		return;

	if(!(cschan = cschan_find(chanuser->channel->name)))
	{
		debug("chanserv_users: Not deleting %s (not tracked)", chanuser->channel->name);
		return;
	}

	debug("chanserv_users: Deleting %s (ChanServ left: %s)", chanuser->channel->name, reason);
	cschan_del(cschan);
}

static void channel_complete_hook(struct irc_channel *channel)
{
	struct irc_user *user = user_find("ChanServ");
	if(!user || !channel_user_find(channel, user))
	{
		debug("chanserv_users: Not adding %s (no ChanServ)", channel->name);
		return;
	}

	debug("chanserv_users: Adding %s (new channel)", channel->name);
	cschan_add(channel);
}

static void channel_del_hook(struct irc_channel *channel, const char *reason)
{
	struct chanserv_channel *cschan;
	if(!(cschan = cschan_find(channel->name)))
	{
		debug("chanserv_users: Not deleting %s (not tracked)", channel->name);
		return;
	}

	debug("chanserv_users: Deleting %s (channel deleted: %s)", channel->name, reason);
	cschan_del(cschan);
}

static struct chanserv_channel *cschan_find(const char *channelname)
{
	return dict_find(chanserv_channels, channelname);
}

static struct chanserv_channel *cschan_add(struct irc_channel *channel)
{
	struct chanserv_channel *cschan = malloc(sizeof(struct chanserv_channel));
	memset(cschan, 0, sizeof(struct chanserv_channel));
	cschan->channel = channel;
	dict_insert(chanserv_channels, cschan->channel->name, cschan);
	chanserv_fetch_users(cschan);
	return cschan;
}

static void cschan_del(struct chanserv_channel *cschan)
{
	dict_delete(chanserv_channels, cschan->channel->name);
}

static void cschan_free(struct chanserv_channel *cschan)
{
	if(cschan->users)
		dict_free(cschan->users);
	if(cschan->tmp_users)
		dict_free(cschan->users);
	free(cschan);
}

static void csuser_free(struct chanserv_user *csuser)
{
	if(csuser->account && !--csuser->account->refcount)
		dict_delete(chanserv_accounts, csuser->account->name);
	free(csuser);
}

static struct chanserv_account *csaccount_find(const char *name)
{
	return dict_find(chanserv_accounts, name);
}

static void csaccount_free(struct chanserv_account *csaccount)
{
	assert(csaccount->refcount == 0);
	ptrlist_free(csaccount->irc_users);
	free(csaccount->name);
	free(csaccount);
}

static void chanserv_update_users(void *bound, void *data)
{
	dict_iter(node, chanserv_channels)
	{
		chanserv_fetch_users(node->data);
	}

	timer_add(this, "chanserv_update_users", now + CHANSERV_UPDATE_USERS_INTERVAL, chanserv_update_users, NULL, 0, 1);
}

static void chanserv_fetch_users(struct chanserv_channel *cschan)
{
	assert(!cschan->tmp_users);

	cschan->tmp_users = dict_create();
	dict_set_free_funcs(cschan->tmp_users, NULL, (dict_free_f *)csuser_free);

	if(cschan->channel->modes & (MODE_INVITEONLY|MODE_KEYED|MODE_SECRET))
		srvx_send_ctx_noqserver((srvx_response_f *)srvx_response_users, strdup(cschan->channel->name), 1, "ChanServ USERS %s", cschan->channel->name);
	else
		srvx_send_ctx((srvx_response_f *)srvx_response_users, strdup(cschan->channel->name), 1, "ChanServ USERS %s", cschan->channel->name);
}

static void srvx_response_users(struct srvx_request *r, const char *channelname)
{
	struct chanserv_channel *cschan = cschan_find(channelname);
	assert(cschan);

	if(!r)
	{
		dict_free(cschan->tmp_users);
		cschan->tmp_users = NULL;
		return;
	}

	for(unsigned int ii = 0; ii < r->count; ii++)
	{
		struct srvx_response_line *line = r->lines[ii];
		assert_continue(!strcasecmp(line->nick, "ChanServ"));

		char *str, *vec[8];
		unsigned int count;
		struct chanserv_channel *chan;

		str = strip_codes(line->msg); // modifies line->msg but it's not needed anymore
		count = tokenize(strdupa(str), vec, ArraySize(vec), ' ', 0);

		// First line of userlist is of the format "#channel users from level 1 to 500:"
		if(IsChannelName(vec[0]) && !strcmp(str + (vec[1] - vec[0]), "users from level 1 to 500:"))
		{
			// Those assertions failing cause memory leaks (cschan->tmp_users), but they shouldn't fail at all.
			assert(!strcasecmp(vec[0], cschan->channel->name));
			assert(dict_size(cschan->tmp_users) == 0);
			continue;
		}

		chanserv_parse_user(cschan, str, count, vec);
	}

	debug("Fetched userlist from channel %s, requesting names", cschan->channel->name);
	if(cschan->channel->modes & (MODE_INVITEONLY|MODE_KEYED|MODE_SECRET))
		srvx_send_ctx_noqserver((srvx_response_f *)srvx_response_names, strdup(cschan->channel->name), 1, "ChanServ NAMES %s", cschan->channel->name);
	else
		srvx_send_ctx((srvx_response_f *)srvx_response_names, strdup(cschan->channel->name), 1, "ChanServ NAMES %s", cschan->channel->name);
}

static void chanserv_parse_user(struct chanserv_channel *cschan, const char *line, int argc, char **argv)
{
	int access;

	// Skip first line
	if(!strcmp(argv[0], "Access"))
		return;

	access = atoi(argv[0]);
	if(!access)
		log_append(LOG_ERROR, "Call to atoi(%s) returns 0 for user's access from line '%s'", argv[0], line);
	else if(access > 500)
		log_append(LOG_WARNING, "Call to atoi(%s) returns %u for user's access from line '%s'", argv[0], access, line);

	if(argc >= 3)
	{
		char *tmp;
		struct chanserv_user *csuser = malloc(sizeof(struct chanserv_user));
		memset(csuser, 0, sizeof(struct chanserv_user));

		csuser->access = access;

		if(!(csuser->account = csaccount_find(argv[1])))
		{
			struct chanserv_account *csaccount = malloc(sizeof(struct chanserv_account));
			memset(csaccount, 0, sizeof(struct chanserv_account));

			csaccount->name = strdup(argv[1]);
			csaccount->irc_users = ptrlist_create();

			dict_insert(chanserv_accounts, csaccount->name, csaccount);
			csuser->account = csaccount;
		}

		csuser->account->refcount++;

		tmp = trim(strndup(line + (argv[2] - argv[0]), (argv[argc - 1] - argv[2]) - 1));
		csuser->last_seen = parse_chanserv_duration(tmp);
		free(tmp);

		csuser->account->vacation = 0;
		if(!strcmp(argv[argc - 1], "Vacation"))
		{
			csuser->status = CS_USER_VACATION;
			csuser->account->vacation = 1;
		}
		else if(!strcmp(argv[argc - 1], "Suspended"))
		{
			csuser->status = CS_USER_SUSPENDED;
			// "Suspended" overrides "Vacation" so we cannot
			// remove a vacation flag from the account
		}
		else if(!strcmp(argv[argc - 1], "Bot"))
		{
			csuser->status = CS_USER_BOT;
			csuser->account->vacation = 0;
		}
		else
		{
			csuser->status = CS_USER_NORMAL;
			csuser->account->vacation = 0;
		}

		dict_insert(cschan->tmp_users, csuser->account->name, csuser);
	}
}


static void srvx_response_names(struct srvx_request *r, const char *channelname)
{
	struct chanserv_channel *cschan = cschan_find(channelname);
	assert(cschan);

	if(!r)
	{
		dict_free(cschan->tmp_users);
		cschan->tmp_users = NULL;
		return;
	}

	for(unsigned int ii = 0; ii < r->count; ii++)
	{
		struct srvx_response_line *line = r->lines[ii];
		assert_continue(!strcasecmp(line->nick, "ChanServ"));

		char *str, *vec[4];
		unsigned int count;

		str = strip_codes(line->msg); // modifies line->msg but it's not needed anymore
		count = tokenize(strdupa(str), vec, ArraySize(vec), ' ', 0);

		// Line of names list
		if(!strncmp(str, "Users in", 8))
		{
			// No users
			if(count < 4)
				continue;

			// There can *never* be more than 74 names in a single line.
			// Actually, the maximum is even lower since not the whole 512 chars of
			// an irc line can be used for the names and there are not enough 1-letter-names.
			char *namev[74];
			unsigned int namec;

			debug("Names: '%s'", vec[3]);
			namec = tokenize(vec[3], namev, ArraySize(namev), ' ', 1);
			debug("Count: %u", namec);
			for(unsigned int jj = 0; jj < namec; jj++)
				chanserv_parse_name(cschan, namev[jj]);
		}
	}

	if(cschan->users)
		dict_free(cschan->users);
	cschan->users = cschan->tmp_users;
	cschan->tmp_users = NULL;
}

static void chanserv_parse_name(struct chanserv_channel *cschan, const char *item)
{
	const char *ptr = item;
	char *nick = NULL, *accountname = NULL, *ptr2;
	struct chanserv_account *csaccount;
	unsigned long access;
	struct irc_user *user;

	access = strtoul(item, &ptr2, 10);
	assert(*ptr2 == ':');
	assert(access <= 500);

	// End of nick
	assert((ptr = strchr(++ptr2, '(')));
	nick = strndup(ptr2, ptr - ptr2);

	// End of accountname
	assert((ptr2 = strchr(++ptr, ')')));
	accountname = strndup(ptr, ptr2 - ptr);

	// Does this account already exist?
	assert_goto((csaccount = csaccount_find(accountname)), out);

	// Get user
	assert_goto((user = user_find(nick)), out);

	// See if this nick needs to be added
	if(ptrlist_find(csaccount->irc_users, user) == -1)
		ptrlist_add(csaccount->irc_users, 0, user);

out:
	MyFree(nick);
	MyFree(accountname);
}

static unsigned long parse_chanserv_duration(const char *duration)
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
	} while(*tmp2);

	return ret;
}

