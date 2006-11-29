#include "global.h"
#include "module.h"
#include "chanspy.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "modules/chanjoin/chanjoin.h"
#include "chanuser.h"
#include "database.h"
#include "irc.h"
#include "irc_handler.h"
#include "table.h"
#include "timer.h"

MODULE_DEPENDS("commands", "help", "chanjoin", NULL);


COMMAND(chanspy_add);
COMMAND(chanspy_rejoin);
COMMAND(chanspy_del);
COMMAND(chanspy_list);
IRC_HANDLER(privmsg);
IRC_HANDLER(notice);
IRC_HANDLER(mode);
IRC_HANDLER(join);
IRC_HANDLER(part);
IRC_HANDLER(kick);
static void user_del_hook(struct irc_user *user, unsigned int quit, const char *reason);
IRC_HANDLER(nick);
IRC_HANDLER(topic);
static void spy_db_read(struct database *db);
static int spy_db_write(struct database *db);
static int sort_spies(const void *a_, const void *b_);
static struct chanspy *spy_add(const char *name, const char *channel, const char *target, int flags);
static struct chanspy *spy_find(const char *name);
static void spy_free(struct chanspy *spy);
static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason);
static void spy_gotmsg(const char *channel, unsigned int type, const char *fmt, ...)  PRINTF_LIKE(3, 4);
static char *modechar(const char *chan, const char *nick);

static struct module *this;
static struct database *chanspy_db = NULL;
static struct dict *spies;
static const char *spy_flags = "PANQmjpkqnt";
static char spy_flags_inverse[256];

MODULE_INIT
{
	this = self;

	spies = dict_create();
	dict_set_free_funcs(spies, NULL, (dict_free_f *)spy_free);

	memset(spy_flags_inverse, 0, sizeof(spy_flags_inverse));
	for(unsigned int i = 0; spy_flags[i]; i++)
		spy_flags_inverse[(unsigned char)spy_flags[i]] = i + 1;

	reg_irc_handler("PRIVMSG", privmsg);
	reg_irc_handler("NOTICE", notice);
	reg_irc_handler("MODE", mode);
	reg_irc_handler("JOIN", join);
	reg_irc_handler("PART", part);
	reg_irc_handler("KICK", kick);
	chanuser_reg_user_del_hook(user_del_hook);
	reg_irc_handler("NICK", nick);
	reg_irc_handler("TOPIC", topic);

	help_load(self, "chanspy.help");
	DEFINE_COMMAND(self, "chanspy add",	chanspy_add,	5, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy rejoin",	chanspy_rejoin,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy del",	chanspy_del,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy list",	chanspy_list,	1, CMD_REQUIRE_AUTHED, "group(admins)");

	chanspy_db = database_create("chanspy", spy_db_read, spy_db_write);
	database_read(chanspy_db, 1);
	database_set_write_interval(chanspy_db, 300);
}

MODULE_FINI
{
	database_write(chanspy_db);
	database_delete(chanspy_db);

	unreg_irc_handler("PRIVMSG", privmsg);
	unreg_irc_handler("NOTICE", notice);
	unreg_irc_handler("MODE", mode);
	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("KICK", kick);
	chanuser_unreg_user_del_hook(user_del_hook);
	unreg_irc_handler("NICK", nick);
	unreg_irc_handler("TOPIC", topic);

	dict_free(spies);
}

static void spy_db_read(struct database *db)
{
	struct dict *db_node;

	if((db_node = database_fetch(db->nodes, "spies", DB_OBJECT)) != NULL)
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			const char *name = rec->key;
			const char *channel = database_fetch(obj, "channel", DB_STRING);
			const char *target = database_fetch(obj, "target", DB_STRING);
			const char *flags_str = database_fetch(obj, "flags", DB_STRING);
			unsigned int flags;

			if(flags_str && (flags = atoi(flags_str)))
				spy_add(name, channel, target, flags);
		}
	}
}

static int spy_db_write(struct database *db)
{
	database_begin_object(db, "spies");
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;

			database_begin_object(db, spy->name);
				database_write_string(db, "channel", spy->channel);
				database_write_string(db, "target", spy->target);
				database_write_long(db, "flags", spy->flags);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

static struct chanspy *spy_add(const char *name, const char *channel, const char *target, int flags)
{
	struct chanspy *spy = malloc(sizeof(struct chanspy));
	memset(spy, 0, sizeof(struct chanspy));

	spy->name = strdup(name);
	spy->channel = strdup(channel);
	spy->target = strdup(target);
	spy->flags = flags;
	spy->active = 1;
	spy->last_error = "No Error";

	chanjoin_addchan(channel, this, name, cj_success, cj_error, spy);
	dict_insert(spies, spy->name, spy);
	return spy;
}

static struct chanspy *spy_find(const char *name)
{
	return dict_find(spies, name);
}

static void spy_free(struct chanspy *spy)
{
	if(spy->active && !reloading_module)
		chanjoin_delchan(spy->channel, this, spy->name);
	free(spy->name);
	free(spy->channel);
	free(spy->target);
	free(spy);
}

static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time)
{
	struct chanspy *spy;
	assert((spy = spy_find(key)) == ctx);
	spy->active = 1;
	spy->last_error = "No Error";
	irc_send_msg(spy->target, "NOTICE", "%s %s (%s) successfully; started spying.", (first_time ? "Joined" : "Rejoined"), spy->channel, spy->name);
}

static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason)
{
	struct chanspy *spy;
	assert((spy = spy_find(key)) == ctx);
	spy->active = 0;
	spy->last_error = reason;
	irc_send_msg(spy->target, "NOTICE", "Chanspy on %s (%s) reported error: %s", spy->channel, spy->name, reason);
}

static void spy_gotmsg(const char *channel, unsigned int type, const char *fmt, ...)
{
	char buf[MAXLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	dict_iter(node, spies)
	{
		struct chanspy *spy = node->data;
		if(!strcasecmp(spy->channel, channel) && (spy->flags & type))
			irc_send_raw("PRIVMSG %s :[%s] %s", spy->target, channel, buf);
	}
}

static char *modechar(const char *chan, const char *nick)
{
	static char modechar[2];
	struct irc_channel *channel;
	struct irc_chanuser *chanuser;

	strcpy(modechar, "");
	if((channel = channel_find(chan)) && (chanuser = dict_find(channel->users, nick)))
	{
		if(chanuser->flags & MODE_OP)
			strcpy(modechar, "@");
		else if(chanuser->flags & MODE_VOICE)
			strcpy(modechar, "+");
	}

	return modechar;
}

IRC_HANDLER(privmsg)
{
	struct irc_user *user;
	assert(argc > 2);

	if(!strcasecmp(argv[1], bot.nickname) && (user = user_find(src->nick)))
	{
		if(!strncasecmp(argv[2], "\001ACTION ", 8))
		{
			char *action = strdup(argv[2] + 8);
			action[strlen(action)-1] = '\0';
			dict_iter(node, user->channels)
			{
				struct irc_chanuser *chanuser = node->data;
				spy_gotmsg(chanuser->channel->name, CSPY_QUERY, "[PM] * %s %s", src->nick, action);
			}
			free(action);
		}
		else
		{
			dict_iter(node, user->channels)
			{
				struct irc_chanuser *chanuser = node->data;
				spy_gotmsg(chanuser->channel->name, CSPY_QUERY, "[PM] <%s> %s", src->nick, argv[2]);
			}
		}
	}
	else if(IsChannelName(argv[1]))
	{
		if(!strncasecmp(argv[2], "\001ACTION ", 8))
		{
			char *action = strdup(argv[2] + 8);
			action[strlen(action)-1] = '\0';
			spy_gotmsg(argv[1], CSPY_ACTION, "* %s%s %s", modechar(argv[1], src->nick), src->nick, action);
			free(action);
		}
		else
		{
			spy_gotmsg(argv[1], CSPY_PRIVMSG, "<%s%s> %s", modechar(argv[1], src->nick), src->nick, argv[2]);
		}
	}
}

IRC_HANDLER(notice)
{
	assert(argc > 2);

	if(!src || (!IsChannelName(argv[1]) && (*argv[1] != '@' || !IsChannelName(argv[1] + 1))))
		return;

	if(IsChannelName(argv[1]))
		spy_gotmsg(argv[1], CSPY_NOTICE, "-%s:%s- %s", src->nick, argv[1], argv[2]);
	else if(*argv[1] == '@' && IsChannelName(argv[1] + 1))
		spy_gotmsg((argv[1] + 1), CSPY_NOTICE, "-%s:%s- %s", src->nick, argv[1], argv[2]);
}

IRC_HANDLER(mode)
{
	char *modestr;
	assert(argc > 2);
	modestr = untokenize(argc - 2, argv + 2, " ");
	spy_gotmsg(argv[1], CSPY_MODE, "* %s sets mode: %s", src->nick, modestr);
	free(modestr);
}

IRC_HANDLER(join)
{
	assert(argc > 1);
	spy_gotmsg(argv[1], CSPY_JOIN, "* Joins: %s (%s@%s)", src->nick, src->ident, src->host);
}

IRC_HANDLER(part)
{
	assert(argc > 1);
	if(argc > 2)
		spy_gotmsg(argv[1], CSPY_PART, "* Parts: %s (%s@%s) (%s)", src->nick, src->ident, src->host, argv[2]);
	else
		spy_gotmsg(argv[1], CSPY_PART, "* Parts: %s (%s@%s)", src->nick, src->ident, src->host);
}

IRC_HANDLER(kick)
{
	assert(argc > 3);
	spy_gotmsg(argv[1], CSPY_KICK, "* %s was kicked by %s (%s)", argv[2], src->nick, argv[3]);
}

static void user_del_hook(struct irc_user *user, unsigned int quit, const char *reason)
{
	if(!quit)
		return;

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		spy_gotmsg(chanuser->channel->name, CSPY_QUIT, "* Quits: %s%s (%s@%s) (%s)", modechar(chanuser->channel->name, user->nick), user->nick, user->ident, user->host, reason);
	}
}

IRC_HANDLER(nick)
{
	struct irc_user *user;
	assert(argc > 1);
	assert(user = user_find(argv[1])); // chanuser_irc handles it first -> the user is already renamed

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		spy_gotmsg(chanuser->channel->name, CSPY_NICK, "* %s is now known as %s", src->nick, argv[1]);
	}
}

IRC_HANDLER(topic)
{
	assert(argc > 2);
	spy_gotmsg(argv[1], CSPY_TOPIC, "* %s changes topic to '%s'", src->nick, argv[2]);
}


// argv[1]: spy name
// argv[2]: #from
// argv[3]: #target or nick
// argv[4]: flags (P = privmsg, A = ctcp action, N = notice, M = mode, J = join, L = part, K = kick, Q = quit, N = nick, T = topic, * = everything)
COMMAND(chanspy_add)
{
	struct chanspy *spy;
	char c;
	unsigned int pos, flags = 0;

	if((spy = spy_find(argv[1])))
	{
		reply("There is already a chanspy named $b%s$b.", spy->name);
		return 0;
	}

	if(!IsChannelName(argv[2]))
	{
		reply("You must specify a valid source channel.");
		return 0;
	}

	// XXX: We could allow either a channel or nick@host targets so the nick gets only pm'd if he has the correct mask.

	if(*argv[3] == '#' && !IsChannelName(argv[3]))
	{
		reply("You must specify a valid target channel or nick.");
		return 0;
	}
	else if(*argv[3] != '#' && !validate_string(argv[3], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890_-[]{}|^`", &c))
	{
		reply("You must specify a valid target channel or nick. $b%c$b doesn't look like something that belongs in a nickname.", c);
		return 0;
	}

	if(!strcmp(argv[4], "*"))
		flags = CSPY_ALL;
	else
	{
		for(unsigned int i = 0; argv[4][i]; i++)
		{
			if(!(pos = spy_flags_inverse[(unsigned char)argv[4][i]]))
			{
				reply("$b%c$b is not a valid chanspy flag (%s).", argv[4][i], spy_flags);
				return 0;
			}

			flags |= 1 << (pos - 1);
		}
	}

	spy_add(argv[1], argv[2], argv[3], flags);
	reply("Chanspy for $b%s$b added. Check $b%s$b for status notices.", argv[2], argv[3]);
	return 1;
}

COMMAND(chanspy_rejoin)
{
	unsigned int found = 0;

	if(!IsChannelName(argv[1]))
	{
		reply("You must specify a valid channel name.");
		return 0;
	}

	dict_iter(node, spies)
	{
		struct chanspy *spy = node->data;

		if(!strcasecmp(spy->channel, argv[1]))
		{
			found = 1;
			chanjoin_addchan(spy->channel, this, spy->name, cj_success, cj_error, spy);
		}
	}

	if(!found)
	{
		reply("No chanspy for $b%s$b found.", argv[1]);
		return 0;
	}
	else
	{
		reply("Trying to rejoin %s.", argv[1]);
		return 1;
	}
}

COMMAND(chanspy_del)
{
	struct chanspy *spy;

	if(!(spy = spy_find(argv[1])))
	{
		reply("There is no chanspy named $b%s$b.", argv[1]);
		return 0;
	}

	reply("Chanspy $b%s$b deleted.", spy->name);
	dict_delete(spies, spy->name);

	return 1;
}

COMMAND(chanspy_list)
{
	struct table *table;
	unsigned int i = 0;

	table = table_create(5, dict_size(spies));
	table_set_header(table, "Name", "Channel", "Target", "Flags", "State");

	dict_iter(node, spies)
	{
		char flags[32];
		unsigned int pos = 0;
		struct chanspy *spy = node->data;

		for(unsigned int ii = 0; spy_flags[ii]; ii++)
			if(spy->flags & (1 << ii))
				flags[pos++] = spy_flags[ii];
		flags[pos] = '\0';

		table->data[i][0] = spy->name;
		table->data[i][1] = spy->channel;
		table->data[i][2] = spy->target;
		table->data[i][3] = ((spy->flags == CSPY_ALL) ? "*" : strdupa(flags));
		table->data[i][4] = spy->active ? "Active" : spy->last_error;
		i++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_spies);
	table_send(table, src->nick);
	table_free(table);

	return 1;
}

static int sort_spies(const void *a_, const void *b_)
{
	const char *name_a = (*((const char ***)a_))[0];
	const char *name_b = (*((const char ***)b_))[0];
	return strcasecmp(name_a, name_b);
}
