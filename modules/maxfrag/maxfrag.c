#include "global.h"
#include "chanuser.h"
#include "conf.h"
#include "database.h"
#include "irc.h"
#include "irc_handler.h"
#include "module.h"
#include "timer.h"
#include "modules/commands/commands.h"
#include "modules/chanjoin/chanjoin.h"
#include "modules/chanserv_access/chanserv_access.h"

MODULE_DEPENDS("chanserv_access", "chanserv_users", "commands", "chanjoin", NULL);

static struct
{
	int check_access_interval;
	int min_user_chanserv_access;
	int min_bot_chanserv_access;
	const char *debug_channel;
} maxfrag_conf;

struct maxfrag_reg
{
	char *channel;
	int active;
	const char *last_error;
	time_t registered;

	int ads;
};

COMMAND(request);
COMMAND(unregister);
COMMAND(ads);
COMMAND(advertise);
COMMAND(announce);
COMMAND(greeting);
IRC_HANDLER(part);
IRC_HANDLER(join);
static void maxfrag_conf_reload();
static void maxfrag_db_read(struct database *db);
static int maxfrag_db_write(struct database *db);
static struct maxfrag_reg *maxfrag_reg_add(const char *channel);
static struct maxfrag_reg *maxfrag_reg_find(const char *channel);
static void maxfrag_reg_free(struct maxfrag_reg *reg);
static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason);
static void maxfrag_join(struct maxfrag_reg *reg);
static void maxfrag_check_access_tmr(struct module *self, void *data);
static void maxfrag_check_access_cb(const char *channel, const char *nick, int access, void *ctx);

static void maxfrag_req_cj_success(struct cj_channel *chan, const char *key, const char *nick, unsigned int first_time);
static void maxfrag_req_cj_error(struct cj_channel *chan, const char *key, const char *nick, const char *reason);
static void maxfrag_req_success_tmr(struct module *self, char *chan);
static void maxfrag_req_user_chanserv_access_cb(const char *channel, const char *nick, int access, void *ctx);
static void maxfrag_req_bot_chanserv_access_cb(const char *channel, const char *bot_nick, int access, char *nick);

static struct module *this;
static struct database *maxfrag_db = NULL;
static struct dict *regs;
static struct stringlist *active_requests;
static struct stringlist *greeting = NULL;

MODULE_INIT
{
	this = self;

	regs = dict_create();
	dict_set_free_funcs(regs, NULL, (dict_free_f *) maxfrag_reg_free);

	DEFINE_COMMAND(self, "request",		request,	0, CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(self, "unregister",	unregister,	0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL | CMD_LOG_HOSTMASK, "chanserv(400)");
	DEFINE_COMMAND(self, "ads",		ads,		0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "chanserv(400)");
	DEFINE_COMMAND(self, "advertise",	advertise,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(self, "announce",	announce,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(self, "greeting",	greeting,	0, CMD_REQUIRE_AUTHED, "group(admins)");

	reg_conf_reload_func(maxfrag_conf_reload);
	reg_irc_handler("PART", part);
	reg_irc_handler("JOIN", join);
	maxfrag_conf_reload();

	maxfrag_db = database_create("maxfrag", maxfrag_db_read, maxfrag_db_write);
	database_read(maxfrag_db, 1);
	database_set_write_interval(maxfrag_db, 300);

	active_requests = stringlist_create();
	if(!greeting)
		greeting = stringlist_create();
	timer_add(this, "maxfrag_check_access", now + maxfrag_conf.check_access_interval, (timer_f *)maxfrag_check_access_tmr, NULL, 0, 0);
}

MODULE_FINI
{
	timer_del(this, NULL, 0, NULL, NULL, TIMER_IGNORE_ALL & ~TIMER_IGNORE_BOUND);

	database_write(maxfrag_db);
	database_delete(maxfrag_db);

	stringlist_free(active_requests);
	stringlist_free(greeting);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("JOIN", join);
	unreg_conf_reload_func(maxfrag_conf_reload);
	dict_free(regs);
}

static void maxfrag_conf_reload()
{
	char *str;

	str = conf_get("maxfrag/check_access_interval", DB_STRING);
	maxfrag_conf.check_access_interval = str ? atoi(str) : 3600;

	str = conf_get("maxfrag/min_user_chanserv_access", DB_STRING);
	maxfrag_conf.min_user_chanserv_access = str ? atoi(str) : 400;

	str = conf_get("maxfrag/min_bot_chanserv_access", DB_STRING);
	maxfrag_conf.min_bot_chanserv_access = str ? atoi(str) : 1;

	maxfrag_conf.debug_channel = conf_get("maxfrag/debug_channel", DB_STRING);
}

static void maxfrag_db_read(struct database *db)
{
	struct dict *db_node;
	struct stringlist *slist;

	if((db_node = database_fetch(db->nodes, "regs", DB_OBJECT)))
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			char *str;
			struct maxfrag_reg *reg;
			const char *channel = rec->key;

			reg = maxfrag_reg_add(channel);

			if((str = database_fetch(obj, "registered", DB_STRING)))
				reg->registered = strtoul(str, NULL, 10);

			if((str = database_fetch(obj, "ads", DB_STRING)))
				reg->ads = true_string(str);
		}
	}

	if((slist = database_fetch(db->nodes, "greeting", DB_STRINGLIST)))
	{
		assert(!greeting);
		greeting = stringlist_copy(slist);
	}

}

static int maxfrag_db_write(struct database *db)
{
	database_begin_object(db, "regs");
		dict_iter(node, regs)
		{
			struct maxfrag_reg *reg = node->data;

			database_begin_object(db, reg->channel);
				database_write_long(db, "registered", reg->registered);
				database_write_long(db, "ads", reg->ads);
			database_end_object(db);
		}
	database_end_object(db);

	database_write_stringlist(db, "greeting", greeting);
	return 0;
}

static void maxfrag_check_access_tmr(struct module *self, void *data)
{
	dict_iter(node, regs)
	{
		struct maxfrag_reg *reg = node->data;
		if(reg->active)
			chanserv_get_access_callback(reg->channel, bot.nickname, (chanserv_access_f *)maxfrag_check_access_cb, NULL);
	}

	timer_add(this, "maxfrag_check_access", now + maxfrag_conf.check_access_interval, (timer_f *)maxfrag_check_access_tmr, NULL, 0, 0);
}

static void maxfrag_check_access_cb(const char *channel, const char *nick, int access, void *ctx)
{
	struct maxfrag_reg *reg;

	if(access >= maxfrag_conf.min_bot_chanserv_access)
		return;

	if(!(reg = maxfrag_reg_find(channel)))
		return;

	if(maxfrag_conf.debug_channel)
	{
		if(reg->ads)
			irc_send("PRIVMSG %s :Disabled ads: %s (not enough access [%d])", maxfrag_conf.debug_channel, reg->channel, access);
		else
			irc_send("PRIVMSG %s :Low access: %s (%d)", maxfrag_conf.debug_channel, reg->channel, access);
	}

	reg->ads = 0;
}

static struct maxfrag_reg *maxfrag_reg_add(const char *channel)
{
	struct maxfrag_reg *reg = malloc(sizeof(struct maxfrag_reg));
	memset(reg, 0, sizeof(struct maxfrag_reg));

	reg->channel = strdup(channel);
	reg->registered = time(NULL);
	reg->last_error = "No Error";

	maxfrag_join(reg);
	dict_insert(regs, reg->channel, reg);
	return reg;
}

static struct maxfrag_reg *maxfrag_reg_find(const char *channel)
{
	return dict_find(regs, channel);
}

static void maxfrag_reg_free(struct maxfrag_reg *reg)
{
	if(!reloading_module)
		chanjoin_delchan(reg->channel, this, "reg");
	free(reg->channel);
	free(reg);
}

static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time)
{
	struct maxfrag_reg *reg = ctx;
	reg->active = 1;
	reg->last_error = "No Error";
}

static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason)
{
	struct maxfrag_reg *reg = ctx;
	reg->active = 0;
	reg->last_error = reason;
	if(maxfrag_conf.debug_channel)
		irc_send("PRIVMSG %s :Unjoinable channel: %s (%s)", maxfrag_conf.debug_channel, reg->channel, reason);
	// XXX: maybe unregister immediately
}

static void maxfrag_join(struct maxfrag_reg *reg)
{
	chanjoin_addchan(reg->channel, this, "reg", cj_success, cj_error, reg, NULL, 0);
}

IRC_HANDLER(part)
{
	struct maxfrag_reg *reg;
	assert(argc > 1);

	if(strcmp(src->nick, "ChanServ"))
		return;

	if(argc < 3 || match("* registration expired.", argv[2]))
		return;

	if(!(reg = maxfrag_reg_find(argv[1])))
		return;

	dict_delete(regs, reg->channel);
	if(maxfrag_conf.debug_channel)
		irc_send("PRIVMSG %s :Unregistered channel: %s (expired)", maxfrag_conf.debug_channel, argv[1]);
}

IRC_HANDLER(join)
{
	struct maxfrag_reg *reg;
	assert(argc > 1);

	if(!(reg = maxfrag_reg_find(argv[1])) || !reg->ads)
		return;

	for(unsigned int i = 0; i < greeting->count; i++)
	{
		if(*greeting->data[i] != '\0')
			irc_send("NOTICE %s :(%s) %s", src->nick, argv[1], greeting->data[i]);
	}
}

COMMAND(request)
{
	struct maxfrag_reg *reg;

	if(!channelname)
	{
		reply("You must provide a valid channel name.");
		return 0;
	}

	if((reg = maxfrag_reg_find(channelname)))
	{
		reply("$b%s$b is already registered.", reg->channel);
		return 0;
	}

	reply("Thanks for requesting $N. I will join $b%s$b now and check your and the bot's access.", channelname);
	chanjoin_addchan(channelname, this, "request", (chanjoin_success_f *)maxfrag_req_cj_success,
			 (chanjoin_error_f *)maxfrag_req_cj_error, strdup(user->nick), free, 1);
	stringlist_add(active_requests, strdup(channelname));
	return 1;
}

// Request stuff
static void maxfrag_req_cj_success(struct cj_channel *chan, const char *key, const char *nick, unsigned int first_time)
{
	struct irc_user *user = NULL;
	struct irc_chanuser *chanuser = NULL;

	if(chan->channel->modes & MODE_REGISTERED) // channel is +z
	{
		if((user = user_find("ChanServ")) && channel_user_find(chan->channel, user))
		{
			chanserv_get_access_callback(chan->name, nick, maxfrag_req_user_chanserv_access_cb, NULL);
			return;
		}
		else
		{
			reply_nick(nick, "Sorry, but your channel has mode +z (registered) but there's no ChanServ in it.");
			reply_nick(nick, "Either ChanServ is currently down or your channel is suspended.");
		}
	}
	else // no +z => reject
	{
		reply_nick(nick, "Sorry, but your channel must be registered with ChanServ to request it.");
	}

	// remove running request
	int pos = stringlist_find(active_requests, chan->name);
	assert(pos >= 0);
	stringlist_del(active_requests, pos);

	// delete channel from chanjoin module as soon as possible (its impossible to call chanjoin_del() here)
	timer_add(this, "maxfrag_req_success", now, (timer_f *)maxfrag_req_success_tmr, strdup(chan->name), 1, 0);
}

static void maxfrag_req_cj_error(struct cj_channel *chan, const char *key, const char *nick, const char *reason)
{
	reply_nick(nick, "Sorry, but I was not able to join $b%s$b.", chan->name);

	if(!strcmp(reason, "keyed") || !strcmp(reason, "inviteonly")) // invite only / keyed channel
	{
		reply_nick(nick, "Looks like you set a $bkey$b for your channel or it's $binvite only.$b");
		reply_nick(nick, "Please add me to your channel userlist with access to use ChanServ's INVITEME command.");
	}
	else if(!strcmp(reason, "limit")) // limit reached
	{
		reply_nick(nick, "Looks like your channel $bis full.$b");
		reply_nick(nick, "Please add me to your channel userlist with access to use ChanServ's INVITEME command.");
	}
	else if(!strcmp(reason, "banned")) // banned channels
	{
		reply_nick(nick, "Looks like I'm $bbanned$b in this channel.");
		reply_nick(nick, "Please add me to your channel userlist with access to use ChanServ's UNBANME command (200).");
	}

	// remove running request
	int pos = stringlist_find(active_requests, chan->name);
	assert(pos >= 0);
	stringlist_del(active_requests, pos);
}

static void maxfrag_req_success_tmr(struct module *self, char *chan)
{
	chanjoin_delchan(chan, this, "request");
}

static void maxfrag_req_user_chanserv_access_cb(const char *channel, const char *nick, int access, void *ctx)
{
	if(access >= maxfrag_conf.min_user_chanserv_access)
	{
		chanserv_get_access_callback(channel, bot.nickname, (chanserv_access_f *)maxfrag_req_bot_chanserv_access_cb, strdup(nick));
		return;
	}
	else if(access >= 0)
	{
		reply_nick(nick, "Sorry but you don't have enough ChanServ access in $b%s$b to request $N", channel);
		reply_nick(nick, "Required access: $b%d$b. Your access: $b%d$b", maxfrag_conf.min_user_chanserv_access, access);
	}
	else
	{
		reply_nick(nick, "It was not possible to get your ChanServ access in $b%s$b.", channel);
		reply_nick(nick, "Please retry requesting the channel later.");
	}

	// remove running request
	int pos = stringlist_find(active_requests, channel);
	assert(pos >= 0);
	stringlist_del(active_requests, pos);

	// delete channel from chanjoin module as soon as possible (we could delete it here but this way is much safer)
	timer_add(this, "maxfrag_req_success", now, (timer_f *)maxfrag_req_success_tmr, strdup(channel), 1, 0);
}

static void maxfrag_req_bot_chanserv_access_cb(const char *channel, const char *bot_nick, int access, char *nick)
{
	if(access >= maxfrag_conf.min_bot_chanserv_access)
	{
		struct maxfrag_reg *reg = maxfrag_reg_add(channel);
		reg->ads = 1;

		if(maxfrag_conf.debug_channel)
			irc_send("PRIVMSG %s :New channel: %s (by %s)", maxfrag_conf.debug_channel, reg->channel, nick);

		reply_nick(nick, "Channel registered successfully.");
		reply_nick(nick, "To disable ads, use $b/msg $N ads %s off$b", reg->channel);
	}
	else if(access >= 0)
	{
		reply_nick(nick, "Sorry but $N doesn't have enough ChanServ access in $b%s$b.", channel);
		reply_nick(nick, "Required access: $b%d$b. $N's access: $b%d$b", maxfrag_conf.min_bot_chanserv_access, access);
	}
	else
	{
		reply_nick(nick, "It was not possible to get $N's ChanServ access in $b%s$b.", channel);
		reply_nick(nick, "Please retry requesting the channel later.");
	}

	// remove running request
	int pos = stringlist_find(active_requests, channel);
	assert(pos >= 0);
	stringlist_del(active_requests, pos);

	// delete channel from chanjoin module as soon as possible (we could delete it here but this way is much safer)
	timer_add(this, "maxfrag_req_success", now, (timer_f *)maxfrag_req_success_tmr, strdup(channel), 1, 0);

	free(nick);
}



COMMAND(unregister)
{
	if(!maxfrag_reg_find(channelname))
	{
		reply("$b%s$b is not registered.", channelname);
		return 0;
	}

	dict_delete(regs, channelname);
	if(maxfrag_conf.debug_channel)
		irc_send("PRIVMSG %s :Unregistered channel: %s (by %s)", maxfrag_conf.debug_channel, channelname, src->nick);
	reply("$b%s$b has been unregistered.", channelname);
	return 1;
}

COMMAND(ads)
{
	struct maxfrag_reg *reg;
	int changed = 0;

	if(!(reg = maxfrag_reg_find(channelname)))
	{
		reply("$b%s$b is not registered.", channelname);
		return 0;
	}

	if(argc > 1)
	{
		changed = 1;
		if(true_string(argv[1]))
			reg->ads = 1;
		else if(false_string(argv[1]))
			reg->ads = 0;
		else
		{
			changed = 0;
			reply("Invalid binary value: %s", argv[1]);
		}
	}

	reply("Ads: $b%s$b", reg->ads ? "Enabled" : "Disabled");
	return changed;
}

COMMAND(advertise)
{
	dict_iter(node, regs)
	{
		struct maxfrag_reg *reg = node->data;
		if(reg->active)
			irc_send("PRIVMSG %s :[$bADVERTISEMENT$b from $b%s$b] %s", node->key, src->nick, argline + (argv[1] - argv[0]));
	}
	return 1;
}

COMMAND(announce)
{
	dict_iter(node, regs)
	{
		struct maxfrag_reg *reg = node->data;
		if(reg->active)
			irc_send("PRIVMSG %s :[$bANNOUNCEMENT$b from $b%s$b] %s", node->key, src->nick, argline + (argv[1] - argv[0]));
	}
	return 1;
}

COMMAND(greeting)
{
	if(argc > 1)
	{
		unsigned int idx = atoi(argv[1]);
		if(idx < 1 || idx > greeting->count + 1)
		{
			reply("Invalid greeting index; must be between $b1$b and $b%d$b", greeting->count);
			return 0;
		}

		idx -= 1; // now we have a valid array index
		if(argc > 2)
		{
			if(!strcmp(argv[2], "*")) // unset greeting
			{
				if(idx == greeting->count)
					reply("Cannot delete non-existent greeting.");
				else if(idx == (greeting->count - 1)) // last one -> simply delete it
				{
					greeting->data[idx][0] = '\0';
					while(greeting->count && greeting->data[greeting->count - 1][0] == '\0')
						stringlist_del(greeting, greeting->count - 1);
				}
				else
					greeting->data[idx][0] = '\0';
			}
			else // set greeting
			{
				if(idx == greeting->count) // new greeting
				{
					stringlist_add(greeting, untokenize(argc - 2, argv + 2, " "));
				}
				else // change existing greeting
				{
					free(greeting->data[idx]);
					greeting->data[idx] = untokenize(argc - 2, argv + 2, " ");
				}
			}
		}

		const char *tmp = "(None)";
		if(idx < greeting->count && *greeting->data[idx])
			tmp = greeting->data[idx];
		reply("Greeting $b%d$b: %s", (idx + 1), tmp);
		return 1;
	}

	if(!greeting->count)
	{
		reply("There are no global greetings set.");
		return 0;
	}

	reply("$bGlobal greetings:$b");
	for(unsigned int i = 0; i < greeting->count; i++)
		reply("  [%d] %s", (i + 1), (*greeting->data[i] ? greeting->data[i] : "(None)"));
	reply("Found $b%d$b greeting%s.", greeting->count, (greeting->count != 1 ? "s" : ""));
	return 1;
}
