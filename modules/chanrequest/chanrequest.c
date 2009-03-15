//header files from bot core
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

//header files from module depends
#include "modules/commands/commands.h"
#include "modules/chanjoin/chanjoin.h"
#include "modules/chanserv/chanserv.h"
#include "modules/chanreg/chanreg.h"

//module depends
MODULE_DEPENDS("chanserv", "chanjoin", "chanreg", NULL);

//some variables
static struct module *this;
static struct dict *blockedChannels;
static struct stringlist *activeRequests;

static struct {
	unsigned int		blockTime;
	unsigned int		cleanInterval;
	int					minAccess;
} chanrequest_conf;

//the command(s) the modul provides
COMMAND(request);

// callback functions
static void chanrequest_chanjoin_success(struct cj_channel *chan, const char *key, const char *ctx, unsigned int first_time);
static void chanrequest_chanjoin_error(struct cj_channel *chan, const char *key, const char *ctx, const char *reason);
static void chanrequest_success_tmr(struct module *self, char *chan);
static void chanrequest_cleanup_blockedChannel_tmr(struct module *self, void *ctx);
static void chanrequest_chanserv_get_access_callback(const char *channel, const char *nick, int access);
static void chanrequest_conf_reload(void);

//helper functions
static void setChannelBlock(const char *nick, const char *channel);
static void removeChannelFromActiveRequests(const char *channel);
static void registerChannelToNick(const char* channel, const char *nick);

MODULE_INIT
{
	this = self;

	blockedChannels = dict_create();
	dict_set_free_funcs(blockedChannels, free, free);

	DEFINE_COMMAND(self, "request",	request, 1, CMD_LAZY_ACCEPT_CHANNEL, "true");

	activeRequests = stringlist_create();

	reg_conf_reload_func(chanrequest_conf_reload);
	chanrequest_conf_reload();

	//some timer to cleanup blocked channels
	timer_add(this, "chanrequest_blockedChannel_cleaner", now + chanrequest_conf.cleanInterval, (timer_f *)chanrequest_cleanup_blockedChannel_tmr, NULL, 0, 0);
}

MODULE_FINI
{
	dict_free(blockedChannels);

	for(unsigned int i = 0; i < activeRequests->count; i++)
		chanjoin_delchan(activeRequests->data[i], this, NULL);

	timer_del_boundname(this, "chanrequest_blockedChannel_cleaner");
	timer_del_boundname(this, "chanrequest_success_cleaner");

	stringlist_free(activeRequests);
}

COMMAND(request)
{
	if(!(user->account))
	{
		reply("If you want me to join your channel, you first need to register.");
		reply("In order to do so, take a look at $b/msg $N HELP register$b");
		reply("If you do already have an account, see $b/msg $N HELP auth$b");
		return 0;
	}

	if(!channelname)
	{
		reply("You must provide a valid channel name.");
		return 0;
	}

	struct chanreg *reg;
	if((reg = chanreg_find(channelname)))
	{
		reply("$b%s$b is already registered.", reg->channel);
		return 0;
	}

	time_t *blockedUntil = dict_find(blockedChannels, channelname);
	if(blockedUntil && now < *blockedUntil)
	{
		reply("Sorry you can't request $N for $b%s$b now. You have to wait %s.", channelname, duration2string(*(blockedUntil) - now));
		return 0;
	}
	else if(stringlist_find(activeRequests, channelname) > -1)
	{
		reply("Sorry you can't request $N for $b%s$b. There's already a request running.", channelname);
		return 0;
	}

	reply("Thanks for requesting $N. I will join $b%s$b now and check your access.", channelname);

	stringlist_add(activeRequests, strdup(channelname));
	chanjoin_addchan(channelname, this, NULL, (chanjoin_success_f*)chanrequest_chanjoin_success, (chanjoin_error_f*)chanrequest_chanjoin_error, strdup(user->nick), free);

	return 1;
}

void chanrequest_chanjoin_success(struct cj_channel *chan, const char *key, const char *ctx, unsigned int first_time)
{
	struct irc_user* user = NULL;
	struct irc_chanuser* chanuser = NULL;

	if(chan->channel->modes & MODE_REGISTERED) //channel +z?
	{
		if((user = user_find("ChanServ")) && channel_user_find(chan->channel, user))
		{
			chanserv_get_access_callback(chan->name, ctx, chanrequest_chanserv_get_access_callback);
			return;
		}
		else
		{
			irc_send("NOTICE %s :Sorry, but your channel has mode +z (registered) but there's no ChanServ in it.", ctx);
			irc_send("NOTICE %s :Either ChanServ is currently down or something is borken. Maybe you should contact bot staff.", ctx);
		}
	}
	else // no +z => op is enough => register
	{
		if((user = user_find(ctx)) && (chanuser = channel_user_find(chan->channel, user)))
		{
			if(!(chanuser->flags & MODE_OP)) // check for op
			{
				irc_send("NOTICE %s :Sorry, you are not opped in $b%s$b! You can't request $N.", ctx, chan->name);
				setChannelBlock(ctx, chan->name);
			}
			else //ok user is oped
			{
				registerChannelToNick(chan->name, ctx);
			}
		}
		else
		{
			irc_send("NOTICE %s :Sorry, but you must be in $b%s$b to request it.", ctx, chan->name);
		}
	}

	// remove running request
	removeChannelFromActiveRequests(chan->name);

	// delete channel from chanjoin module as soon as possible
	timer_add(chan, "chanrequest_success_cleaner", now, (timer_f *)chanrequest_success_tmr, strdup(chan->name), 1, 0);
}

static void chanrequest_chanjoin_error(struct cj_channel *chan, const char *key, const char *ctx, const char *reason)
{
	irc_send("NOTICE %s :Sorry, i was not able to join $b%s$b!", ctx, chan->name);

	if(!strcmp(reason, "keyed") || !strcmp(reason, "inviteonly")) // invite only / keyed channel
	{
		irc_send("NOTICE %s :Looks like you set a $bkey$b for your channel or it's $binvite only.$b", ctx);
		irc_send("NOTICE %s :Please invite me or add me to your channel userlist with access to use ChanServ's INVITEME command.", ctx);
	}
	else if(!strcmp(reason, "limit")) // limit reached
	{
		irc_send("NOTICE %s :Looks like your channel $bis full.$b", ctx);
		irc_send("NOTICE %s :Please increase the limit or add me to your channel userlist with access to use ChanServ's INVITEME command.", ctx);
	}
	else if(!strcmp(reason, "banned")) // banned channels
	{
		irc_send("NOTICE %s :Looks like I'm banned in this channel.", ctx);
		irc_send("NOTICE %s :Please add me to your channel userlist with access to use ChanServ's UNBANME command (200).", ctx);
	}

	setChannelBlock(ctx, chan->name);

	//remove running request
	removeChannelFromActiveRequests(chan->name);
}

static void chanrequest_chanserv_get_access_callback(const char *channel, const char *nick, int access)
{
	if(access >= chanrequest_conf.minAccess)
	{
		registerChannelToNick(channel, nick);
	}
	else if(access >= 0)
	{
		irc_send("NOTICE %s :Sorry but you don't have enough ChanServ access in $b%s$b to request $N", nick, channel);
		irc_send("NOTICE %s :Required access: $b%d$b Found access is only $b%d$b", nick, chanrequest_conf.minAccess , access);
	}
	else
	{
		irc_send("NOTICE %s :It was not possible to get your ChanServ access in $b%s$b.", nick, channel);
		irc_send("NOTICE %s :Please retry requesting your channel or better contact bot staff.", nick);
	}
	// remove running request
	removeChannelFromActiveRequests(channel);

	// delete channel from chanjoin module as soon as possible
	timer_add(this, "chanrequest_success_cleaner", now, (timer_f *)chanrequest_success_tmr, strdup(channel), 1, 0);
}

static void chanrequest_conf_reload()
{
	char *str;
	chanrequest_conf.cleanInterval = ((str = conf_get("chanrequest/clean_interval", DB_STRING)) ? atoi(str) : 15*60);
	chanrequest_conf.blockTime = ((str = conf_get("chanrequest/block_time", DB_STRING)) ? atoi(str) : 5*60);
	chanrequest_conf.minAccess = ((str = conf_get("chanrequest/min_access", DB_STRING)) ? atoi(str) : 400);
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

/*
 * HELPER FUNCTIONS
 */
static void setChannelBlock(const char *nick, const char *channel)
{
	time_t *blockedUntil = dict_find(blockedChannels, channel);

	if(blockedUntil) //check if there was a request
	{
		*blockedUntil = now + chanrequest_conf.blockTime;
		irc_send("NOTICE %s :You now have to wait %s until you may request this channel again.", nick, duration2string(*blockedUntil - now));
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

static void registerChannelToNick(const char* channel, const char *nick)
{
	//get account from nick
	struct user_account *account = account_find_bynick(nick);

	if(account) //we found an account, register the channel
	{
		//build a nice "Registrar: " string
		struct stringbuffer *strbuff = stringbuffer_create();
		stringbuffer_append_string(strbuff, account->name);
		stringbuffer_append_string(strbuff, " (using ChanRequest Modul)");

		//set this string for the channel
		struct chanreg *reg = chanreg_add(channel, NULL);
		reg->registrar = strdup(strbuff->string);
		stringbuffer_free(strbuff);

		//register channel with account as owner
		chanreg_user_add(reg, account->name, UL_OWNER);
		irc_send("NOTICE %s :Congratulations! $b%s$b has been successfully registered to you.", nick, channel);
	}
}
