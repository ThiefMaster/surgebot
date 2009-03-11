#include "global.h"
#include "module.h"
#include "timer.h"
#include "chanuser.h"
#include "timer.h"
#include "irc.h"
#include "stringbuffer.h"
#include "stringlist.h"
#include "account.h"

#include "modules/commands/commands.h"
#include "modules/chanjoin/chanjoin.h"
#include "modules/chanreg/chanreg.h"

static struct module *this;
static struct dict *blockedChannels;
static struct stringlist *activeRequests;

//COMMANDS
COMMAND(request);

//normal functions
static void channelrequest_success (struct cj_channel *chan, const char *key, char *ctx, unsigned int first_time);
static void channelrequest_error (struct cj_channel *chan, const char *key, char *ctx, const char *reason);
static void setChannelBlock(char *nick, char *channel);
static void removeChannelFromActive(char *channel);

//timer functions
static void channelrequest_success_tmr (struct module *self, char *chan);
static void channelrequest_cleanup_blockedChannel_tmr (struct module *self, void *ctx);

MODULE_DEPENDS("chanjoin", "chanreg", NULL);

MODULE_INIT
{
	this = self;

        blockedChannels = dict_create();
        dict_set_free_funcs(blockedChannels, free, free);

	DEFINE_COMMAND(self, "request",	request, 1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "true");

	activeRequests = stringlist_create();

	//delete channel from chanjoin module
	timer_add(this, "chanrequest_blockedChannel_cleaner", now+(15*60), (timer_f *)channelrequest_cleanup_blockedChannel_tmr, NULL, 0, 0);
}

MODULE_FINI
{
	stringlist_free(activeRequests);
	dict_free(blockedChannels);
}

COMMAND(request)
{
        struct chanreg *reg;

        if(!channelname)
        {
                reply("You must provide a valid channel name.");
                return 0;
        }

        if((reg = chanreg_find(channelname)))
        {
                reply("$b%s$b is already registered.", reg->channel);
                return 0;
        }

	time_t *blockedUntil = dict_find(blockedChannels, channelname);
	if(blockedUntil && now < *blockedUntil)
	{
		reply("Sorry you can't request $N for $b%s$b. You have to wait %s.", channelname, duration2string(*(blockedUntil) - now));
		return 0;
	}
	else if(stringlist_find(activeRequests, channelname) == 0)
	{
		reply("Sorry you can't request $N for $b%s$b. There's allready a request running.", channelname);
		return 0;

	}

	reply("Thanks for requesting $N. I will join $b%s$b now and check your access.", channelname);
	reply("To register your channel with $N, $byou need to be opped$b in the channel.");

	stringlist_add(activeRequests, strdup(channelname));
	chanjoin_addchan(channelname, this, NULL, (chanjoin_success_f*)channelrequest_success, (chanjoin_error_f*)channelrequest_error, strdup(user->nick));

	return 1;
}

void channelrequest_success(struct cj_channel *chan, const char *key, char *ctx, unsigned int first_time)
{
	//find user
	struct irc_user* user = user_find(ctx);
	//find chanuser
	struct irc_chanuser* chanuser = channel_user_find(chan->channel, user);

	if(!(chanuser->flags & MODE_OP)) //check for op
	{
		irc_send("NOTICE %s :Sorry, you are not opped in %s", ctx, chan->name);
		setChannelBlock(ctx, chan->name);
	}
	else if (0) //TODO: check for chanserv access
	{
		irc_send("NOTICE %s :Sorry you lack access to %s", ctx, chan->name);
		setChannelBlock(ctx, chan->name);
	}
	else
	{
		//set owner
		struct user_account *account = account_find(ctx);

		if(account)
		{
			struct stringbuffer *strbuff = stringbuffer_create();
			stringbuffer_append_string(strbuff, account->name);
			stringbuffer_append_string(strbuff, " (via REQUEST Command)");
			struct chanreg *reg = chanreg_add(chan->name, NULL);
			stringbuffer_free(strbuff);
			reg->registrar = strdup(strbuff->string);
			chanreg_user_add(reg, account->name, UL_OWNER);
			irc_send("NOTICE %s :Concratulations! I successfully registered $b%s$b to you.", ctx, chan->name);
		}
	}

	//remove running request
	removeChannelFromActive(chan->name);

	//delete channel from chanjoin module
	timer_add(chan, "chanrequest_success_cleaner", now, (timer_f *)channelrequest_success_tmr, strdup(chan->name), 1, 0);

	//we can free the ctx here becouse the timer will prevent any call of our error function and so on.
	free(ctx);
}

void channelrequest_error(struct cj_channel *chan, const char *key, char *ctx, const char *reason)
{
	irc_send("NOTICE %s :Sorry, i was not able to join %s!", ctx, chan->name);

	if(!strcmp(reason, "keyed") || !strcmp(reason, "inviteonly")) //invite only / keys channel
	{
		irc_send("NOTICE %s :Looks like you set a key for your channel or it's invite only.", ctx);
		irc_send("NOTICE %s :Please invite me or add me to your channel userlist with access to use ChanServs INVITEME command.", ctx);
	}
	else if(!strcmp(reason, "limit")) //limit reached
	{
		irc_send("NOTICE %s :Looks like your channel is full.", ctx);
		irc_send("NOTICE %s :Please increase the limit or add me to your channel userlist with access to use ChanServs INVITEME command.", ctx);
	}
	else if(!strcmp(reason, "banned")) //banned channels
	{
		irc_send("NOTICE %s :Looks like I'm banned in this channel.", ctx);
		irc_send("NOTICE %s :Please add me to your channel userlist with access to use ChanServs UNBANME command (200).", ctx);
	}

	setChannelBlock(ctx, chan->name);

	//remove running request
	removeChannelFromActive(chan->name);

	free(ctx);
}

static void setChannelBlock(char *nick, char *channel)
{
	time_t *blockedUntil = dict_find(blockedChannels, channel);

	if(blockedUntil) //check if we need to block the next request.
	{
		*blockedUntil = now + 5*60;
		irc_send("NOTICE %s :You now have to wait %s until you may request this channel again.", nick, duration2string(*(blockedUntil) - now));
	}
	else //create a block records so we can see someone requested this chan allready
	{
		blockedUntil = malloc(sizeof(time_t));
		*blockedUntil = now;
		dict_insert(blockedChannels, strdup(channel), blockedUntil);
	}
}

static void removeChannelFromActive(char *channel)
{
	int pos = stringlist_find(activeRequests, channel);
	if (pos >= 0) stringlist_del(activeRequests, pos);
}

static void channelrequest_success_tmr(struct module *self, char *chan)
{
	chanjoin_delchan(chan, this, NULL);
}

static void channelrequest_cleanup_blockedChannel_tmr (struct module *self, void *ctx)
{
	dict_iter(node, blockedChannels)
	{
		if(now > *((time_t *)node->data))
			dict_delete(blockedChannels, node->key);
	}
	timer_add(this, "chanrequest_blockedChannel_cleaner", now+(15*60), (timer_f *)channelrequest_cleanup_blockedChannel_tmr, NULL, 0, 0);
}
