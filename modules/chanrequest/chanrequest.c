// header files from bot core
#include "global.h"
#include "module.h"
#include "timer.h"
#include "chanuser.h"
#include "timer.h"
#include "irc.h"
#include "stringbuffer.h"
#include "stringlist.h"
#include "account.h"
#include "conf.h"

// header files from module depends
#include "modules/commands/commands.h"
#include "modules/chanjoin/chanjoin.h"
#include "modules/chanserv_access/chanserv_access.h"
#include "modules/chanreg/chanreg.h"

// module depends
MODULE_DEPENDS("chanserv_access", "chanjoin", "chanreg", "commands", NULL);

// some variables
static struct module *this;
static struct dict *blockedChannels;
static struct stringlist *activeRequests;

static struct {
	unsigned int blockTime;
	unsigned int cleanInterval;
	int minAccess;
	int minTakeoverAccess;
	const char *logChannel;
} chanrequest_conf;

// the command(s) the module provides
COMMAND(request);

// module functions
static void chanrequest_conf_reload(void);

// callback functions
static void chanrequest_chanjoin_success(struct cj_channel *chan, const char *key, const char *nick, unsigned int first_time);
static void chanrequest_chanjoin_error(struct cj_channel *chan, const char *key, const char *nick, const char *reason);
static void chanrequest_success_tmr(struct module *self, char *chan);
static void chanrequest_cleanup_blockedChannel_tmr(struct module *self, void *ctx);
static void chanrequest_chanserv_get_access_callback(const char *channel, const char *nick, int access, void *ctx);

// helper functions
static void setChannelBlock(const char *nick, const char *channel);
static void removeChannelFromActiveRequests(const char *channel);
static void registerChannelToNick(const char* channel, const char *nick);

MODULE_INIT
{
	this = self;

	blockedChannels = dict_create();
	dict_set_free_funcs(blockedChannels, free, free);

	DEFINE_COMMAND(self, "request",	request, 1, 0, "true");

	activeRequests = stringlist_create();

	reg_conf_reload_func(chanrequest_conf_reload);
	chanrequest_conf_reload();

	//some timer to cleanup blocked channels
	timer_add(this, "chanrequest_blockedChannel_cleaner", now + chanrequest_conf.cleanInterval, (timer_f *)chanrequest_cleanup_blockedChannel_tmr, NULL, 0, 0);
}

MODULE_FINI
{
	// remove all timers
	timer_del(this, NULL, 0, NULL, NULL, TIMER_IGNORE_ALL & ~TIMER_IGNORE_BOUND);

	unreg_conf_reload_func(chanrequest_conf_reload);

	// delete all running requests from chanjoin module
	for(unsigned int i = 0; i < activeRequests->count; i++)
		chanjoin_delchan(activeRequests->data[i], this, NULL);
	stringlist_free(activeRequests);

	dict_free(blockedChannels);
}

static void chanrequest_conf_reload(void)
{
	char *str;
	chanrequest_conf.cleanInterval = ((str = conf_get("chanrequest/clean_interval", DB_STRING)) ? atoi(str) : 15*60);
	chanrequest_conf.blockTime = ((str = conf_get("chanrequest/block_time", DB_STRING)) ? atoi(str) : 5*60);
	chanrequest_conf.minAccess = ((str = conf_get("chanrequest/min_access", DB_STRING)) ? atoi(str) : 400);
	chanrequest_conf.minTakeoverAccess = ((str = conf_get("chanrequest/min_takeover_access", DB_STRING)) ? atoi(str) : 500);
	chanrequest_conf.logChannel = (((str = conf_get("chanrequest/log_channel", DB_STRING)) && *str) ? str : NULL);
}

COMMAND(request)
{
	struct chanreg *reg;
	char *channel_name = argv[1];

	if(!user->account)
	{
		reply("If you want me to join your channel, you first need to register.");
		reply("In order to do so, take a look at $b/msg $N HELP register$b");
		reply("If you do already have an account, see $b/msg $N HELP auth$b");
		return 0;
	}

	if(!IsChannelName(channel_name))
	{
		reply("You must provide a valid channel name.");
		if(*channel_name == '<')
		{
			char *tmp = channel_name + 1;
			if(tmp[strlen(tmp) - 1] == '>')
				tmp[strlen(tmp) - 1] = '\0';
			if(IsChannelName(tmp))
				reply("You probably wanted to use $b/msg $N request %s$b", tmp);
		}

		return 0;
	}

	if(*channel_name != '#')
	{
		reply("You can only request regular channels (starting with a #).");
		return 0;
	}

	if((reg = chanreg_find(channel_name)))
	{
		struct irc_channel *chan;
		struct irc_user *chanserv_user;
		struct chanreg_user *creg_user;
		if((creg_user = chanreg_user_find(reg, user->account->name)) && creg_user->level == UL_OWNER)
		{
			reply("$b%s$b is already registered to you.", reg->channel);
			return 0;
		}
		else
		{
			reply("$b%s$b is already registered to someone else.", reg->channel);
			if(!chanrequest_conf.minTakeoverAccess || !(chan = channel_find(reg->channel)) ||
			   !(chan->modes & MODE_REGISTERED) || !(chanserv_user = user_find("ChanServ")) ||
			   !(channel_user_find(chan, chanserv_user)))
				return 0;
			chanserv_get_access_callback(reg->channel, src->nick, chanrequest_chanserv_get_access_callback, "takeover");
			return 1;
		}
	}

	time_t *blockedUntil = dict_find(blockedChannels, channel_name);
	if(blockedUntil && now < *blockedUntil)
	{
		reply("Sorry you can't request $N for $b%s$b now. You have to wait %s.", channel_name, duration2string(*blockedUntil - now));
		return 0;
	}
	else if(stringlist_find(activeRequests, channel_name) > -1)
	{
		reply("Sorry you can't request $N for $b%s$b. There's already a request running.", channel_name);
		return 0;
	}

	stringlist_add(activeRequests, strdup(channel_name));
	reply("Thanks for requesting $N. I will join $b%s$b now and check your access.", channel_name);

	chanjoin_addchan(channel_name, this, NULL, (chanjoin_success_f*)chanrequest_chanjoin_success, (chanjoin_error_f*)chanrequest_chanjoin_error, strdup(user->nick), free, 1);

	return 1;
}

/*
 * CALLBACK FUNCTIONS
 */
void chanrequest_chanjoin_success(struct cj_channel *chan, const char *key, const char *nick, unsigned int first_time)
{
	struct irc_user *user = NULL;
	struct irc_chanuser *chanuser = NULL;

	if(chan->channel->modes & MODE_REGISTERED) //channel +z?
	{
		if((user = user_find("ChanServ")) && channel_user_find(chan->channel, user))
		{
			chanserv_get_access_callback(chan->name, nick, chanrequest_chanserv_get_access_callback, NULL);
			return;
		}
		else
		{
			reply_nick(nick, "Sorry, but your channel has mode +z (registered) but there's no ChanServ in it.");
			reply_nick(nick, "Either ChanServ is currently down or your channel is suspended. You could contact bot staff to register it.");
		}
	}
	else // no +z => op is enough => register
	{
		if((user = user_find(nick)) && (chanuser = channel_user_find(chan->channel, user)))
		{
			if(!(chanuser->flags & MODE_OP)) // check for op
			{
				reply_nick(nick, "Sorry, you are not opped in $b%s$b! You can't request $N.", chan->name);
				setChannelBlock(nick, chan->name);
			}
			else // ok user is opped
			{
				registerChannelToNick(chan->name, nick);
			}
		}
		else
		{
			reply_nick(nick, "Sorry, but you must be in $b%s$b to request it.", chan->name);
			setChannelBlock(nick, chan->name);
		}
	}

	// remove running request
	removeChannelFromActiveRequests(chan->name);

	// delete channel from chanjoin module as soon as possible (its impossible to call chanjoin_del() here)
	timer_add(this, "chanrequest_success_cleaner", now, (timer_f *)chanrequest_success_tmr, strdup(chan->name), 1, 0);
}

static void chanrequest_chanjoin_error(struct cj_channel *chan, const char *key, const char *nick, const char *reason)
{
	reply_nick(nick, "Sorry, i was not able to join $b%s$b!", chan->name);

	if(!strcmp(reason, "keyed") || !strcmp(reason, "inviteonly")) // invite only / keyed channel
	{
		reply_nick(nick, "Looks like you set a $bkey$b for your channel or it's $binvite only.$b");
		reply_nick(nick, "Please invite me or add me to your channel userlist with access to use ChanServ's INVITEME command.");
	}
	else if(!strcmp(reason, "limit")) // limit reached
	{
		reply_nick(nick, "Looks like your channel $bis full.$b");
		reply_nick(nick, "Please increase the limit or add me to your channel userlist with access to use ChanServ's INVITEME command.");
	}
	else if(!strcmp(reason, "banned")) // banned channels
	{
		reply_nick(nick, "Looks like I'm banned in this channel.");
		reply_nick(nick, "Please add me to your channel userlist with access to use ChanServ's UNBANME command (200).");
	}

	setChannelBlock(nick, chan->name);

	//remove running request
	removeChannelFromActiveRequests(chan->name);
}

static void chanrequest_success_tmr(struct module *self, char *chan)
{
	chanjoin_delchan(chan, this, NULL);
}

static void chanrequest_cleanup_blockedChannel_tmr(struct module *self, void *ctx)
{
	dict_iter(node, blockedChannels)
	{
		if(now > *((time_t *)node->data))
			dict_delete(blockedChannels, node->key);
	}

	timer_add(this, "chanrequest_blockedChannel_cleaner", now + chanrequest_conf.cleanInterval, (timer_f *)chanrequest_cleanup_blockedChannel_tmr, NULL, 0, 0);
}

static void chanrequest_chanserv_get_access_callback(const char *channel, const char *nick, int access, void *ctx)
{
	// takeover
	if(ctx && !strcmp(ctx, "takeover"))
	{
		struct chanreg *reg;

		if(access >= 0 && access < chanrequest_conf.minTakeoverAccess)
		{
			reply_nick(nick, "You have $b%u$b ChanServ access in $b%s$b; to take over the channel you need $b%u$b access.", access, channel, chanrequest_conf.minTakeoverAccess);
			return;
		}
		else if(access < 0)
		{
			reply_nick(nick, "It was not possible to get your ChanServ access in $b%s$b.", channel);
			reply_nick(nick, "Please retry or contact bot staff.");
			return;
		}
		if((reg = chanreg_find(channel)))
		{
			struct chanreg_user *victim;
			struct irc_user *user = user_find(nick);
			if(!user || !user->account) // user probably signed off
				return;
			reply_nick(nick, "You are now the $bowner$b of $b%s$b since you are the ChanServ owner of the channel.", channel);
			for(unsigned int i = 0; i < reg->users->count; i++)
			{
				struct chanreg_user *c_user = reg->users->data[i];
				if(c_user->level == UL_OWNER)
				{
					c_user->level = UL_OWNER - 1;
					reply_nick(nick, "The access of the previous owner $b%s$b has been lowered to $b499$b.", c_user->account->name);
				}
			}
			if(!(victim = chanreg_user_find(reg, user->account->name)))
				victim = chanreg_user_add(reg, user->account->name, UL_OWNER);
			victim->level = UL_OWNER;
		}
		return;
	}

	// registration
	if(access >= chanrequest_conf.minAccess)
	{
		registerChannelToNick(channel, nick);
	}
	else if(access >= 0)
	{
		reply_nick(nick, "Sorry but you don't have enough ChanServ access in $b%s$b to request $N", channel);
		reply_nick(nick, "Required access: $b%d$b. Your access: $b%d$b", chanrequest_conf.minAccess , access);
	}
	else
	{
		reply_nick(nick, "It was not possible to get your ChanServ access in $b%s$b.", channel);
		reply_nick(nick, "Please retry requesting your channel or contact bot staff.");
	}
	// remove running request
	removeChannelFromActiveRequests(channel);

	// delete channel from chanjoin module as soon as possible (we could delete it here but this way is much safer)
	timer_add(this, "chanrequest_success_cleaner", now, (timer_f *)chanrequest_success_tmr, strdup(channel), 1, 0);
}

/*
 * HELPER FUNCTIONS
 */
static void setChannelBlock(const char *nick, const char *channel)
{
	time_t *blockedUntil = dict_find(blockedChannels, channel);

	if(blockedUntil) //check if there was a request
	{
		*blockedUntil = now + chanrequest_conf.blockTime;
		reply_nick(nick, "You now have to wait %s until you may request this channel again.", duration2string(*blockedUntil - now));
	}
	else //create a (expired) block record so we can see someone requested this channel next time
	{
		blockedUntil = malloc(sizeof(time_t));
		*blockedUntil = now - 1;
		dict_insert(blockedChannels, strdup(channel), blockedUntil);
	}
}

static void removeChannelFromActiveRequests(const char *channel)
{
	int pos = stringlist_find(activeRequests, channel);
	assert(pos >= 0);
	stringlist_del(activeRequests, pos);
}

static void registerChannelToNick(const char *channel, const char *nick)
{
	// get account from nick
	struct user_account *account = account_find_bynick(nick);

	if(account) // we found an account, register the channel
	{
		// build a nice "Registrar: " string
		struct stringbuffer *strbuff = stringbuffer_create();
		stringbuffer_append_string(strbuff, account->name);
		stringbuffer_append_string(strbuff, " (via REQUEST)");

		// set this string for the channel
		struct chanreg *reg = chanreg_add(channel, NULL);
		reg->registrar = strdup(strbuff->string);
		stringbuffer_free(strbuff);

		//register channel with account as owner
		chanreg_user_add(reg, account->name, UL_OWNER);

		reply_nick(nick, "Congratulations! $b%s$b has been successfully registered to you.", channel);
		reply_nick(nick, "$uHint:$u You may want to have a look at all available modules with $b/msg $N cmod list %s$b", channel);
		reply_nick(nick, "You can enable those modules with $b/msg $N cmod enable %s <module>$b", channel);
		if(chanrequest_conf.logChannel)
			irc_send("PRIVMSG %s :Channel $b%s$b registered to $b%s$b (*%s).", chanrequest_conf.logChannel, channel, nick, account->name);
	}
}
