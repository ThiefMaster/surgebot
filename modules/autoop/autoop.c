#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "irc.h"
#include "irc_handler.h"
#include "chanuser.h"
#include "stringlist.h"

MODULE_DEPENDS("commands", "chanreg", NULL);

COMMAND(aop_add);
COMMAND(aop_del);
COMMAND(aop_list);
COMMAND(aop_sync);
IRC_HANDLER(join);
static void autoop_db_read(struct dict *db_nodes, struct chanreg *reg);
static int autoop_db_write(struct database_object *dbo, struct chanreg *reg);
static int autoop_enabled(struct chanreg *reg, enum cmod_enable_reason reason);
static int autoop_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void autoop_moved(struct chanreg *reg, const char *from, const char *to);
static unsigned int check_aop_users(struct chanreg *reg, const char *host);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *aop_hosts;

MODULE_INIT
{
	this = self;

	aop_hosts = dict_create();
	dict_set_free_funcs(aop_hosts, free, (dict_free_f *)stringlist_free);

	cmod = chanreg_module_reg("AutoOp", 0, autoop_db_read, autoop_db_write, autoop_enabled, autoop_disabled, autoop_moved);
	chanreg_module_readdb(cmod);

	reg_irc_handler("JOIN", join);

	DEFINE_COMMAND(self, "aop add",		aop_add,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300)");
	DEFINE_COMMAND(self, "aop del",		aop_del,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300)");
	DEFINE_COMMAND(self, "aop list",	aop_list,	1, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(self, "aop sync",	aop_sync,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300)");
}

MODULE_FINI
{
	unreg_irc_handler("JOIN", join);

	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(aop_hosts);
}

static void autoop_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct stringlist *slist;

	if((slist = database_fetch(db_nodes, "aop_hosts", DB_STRINGLIST)))
		dict_insert(aop_hosts, strdup(reg->channel), stringlist_copy(slist));
}

static int autoop_db_write(struct database_object *dbo, struct chanreg *reg)
{
	struct stringlist *channel_aop_hosts;
	if((channel_aop_hosts = dict_find(aop_hosts, reg->channel)))
		database_obj_write_stringlist(dbo, "aop_hosts", channel_aop_hosts);
	return 0;
}

static int autoop_enabled(struct chanreg *reg, enum cmod_enable_reason reason)
{
	if(reason == CER_ENABLED)
	{
		struct irc_channel *channel;
		struct irc_user *me;
		struct irc_chanuser *chanuser;
		assert_return((channel = channel_find(reg->channel)), 1);
		assert_return((me = user_find(bot.nickname)), 1);
		chanuser = channel_user_find(channel, me);
		if(!chanuser || !(chanuser->flags & MODE_OP))
		{
			irc_send("NOTICE @%s :AutoOp cannot work without the bot being opped in $b%s$b.", reg->channel, reg->channel);
			return 1;
		}
	}

	check_aop_users(reg, NULL);
	return 0;
}

static int autoop_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
		dict_delete(aop_hosts, reg->channel);
	return 0;
}

static void autoop_moved(struct chanreg *reg, const char *from, const char *to)
{
	struct dict_node *node = dict_find_node(aop_hosts, from);
	if(!node)
		return;
	free(node->key);
	node->key = strdup(to);
}

static unsigned int check_aop_users(struct chanreg *reg, const char *host)
{
	struct stringlist *channel_aop_hosts;
	struct irc_channel *channel = channel_find(reg->channel);
	unsigned int opped = 0;

	if(!channel)
		return 0;

	if(!(channel_aop_hosts = dict_find(aop_hosts, channel->name)))
		return 0;

	dict_iter(node, channel->users)
	{
		struct irc_chanuser *chanuser = node->data;
		if(!chanuser->user->host || (chanuser->flags & MODE_OP))
			continue;

		if(host && match(host, chanuser->user->host) == 0)
		{
			irc_send("MODE %s +o %s", channel->name, chanuser->user->nick);
			opped++;
		}
		else if(!host) // we need to check all aop hosts
		{
			for(unsigned int i = 0; i < channel_aop_hosts->count; i++)
			{
				if(match(channel_aop_hosts->data[i], chanuser->user->host) == 0)
				{
					opped++;
					irc_send("MODE %s +o %s", channel->name, chanuser->user->nick);
					break;
				}
			}
		}
	}

	return opped;
}

IRC_HANDLER(join)
{
	struct stringlist *channel_aop_hosts;

	assert(argc > 1);

	if(!chanreg_module_active(cmod, argv[1]))
		return;

	if(!(channel_aop_hosts = dict_find(aop_hosts, argv[1])))
		return;

	for(unsigned int i = 0; i < channel_aop_hosts->count; i++)
	{
		if(src->host && match(channel_aop_hosts->data[i], src->host) == 0)
		{
			irc_send("MODE %s +o %s", argv[1], src->nick);
			break;
		}
	}
}

COMMAND(aop_add)
{
	struct stringlist *channel_aop_hosts;

	CHANREG_MODULE_COMMAND(cmod)

	if(!(channel_aop_hosts = dict_find(aop_hosts, reg->channel)))
	{
		channel_aop_hosts = stringlist_create();
		dict_insert(aop_hosts, strdup(reg->channel), channel_aop_hosts);
	}

	if(stringlist_find(channel_aop_hosts, argv[1]) != -1)
	{
		reply("$b%s$b is already on the AutoOp list.", argv[1]);
		return 0;
	}

	stringlist_add(channel_aop_hosts, strdup(argv[1]));
	reply("Added host $b%s$b to the AutoOp list.", argv[1]);
	check_aop_users(reg, argv[1]);
	return 1;
}

COMMAND(aop_del)
{
	struct stringlist *channel_aop_hosts;
	int pos;

	CHANREG_MODULE_COMMAND(cmod)

	if(!(channel_aop_hosts = dict_find(aop_hosts, reg->channel)) || (pos = stringlist_find(channel_aop_hosts, argv[1])) == -1)
	{
		reply("$b%s$b is not on the AutoOp list.", argv[1]);
		return 0;
	}

	stringlist_del(channel_aop_hosts, pos);
	reply("Deleted host $b%s$b from the AutoOp list.", argv[1]);
	return 1;
}

COMMAND(aop_list)
{
	struct stringlist *channel_aop_hosts;

	CHANREG_MODULE_COMMAND(cmod)

	if(!(channel_aop_hosts = dict_find(aop_hosts, reg->channel)) || !channel_aop_hosts->count)
	{
		reply("There are no AutoOp hosts added in $b%s$b.", reg->channel);
		return 0;
	}

	reply("$bAutoOp hosts:$b");
	for(unsigned int i = 0; i < channel_aop_hosts->count; i++)
		reply("  %s", channel_aop_hosts->data[i]);
	reply("Found $b%d$b hosts.", channel_aop_hosts->count);
	return 1;
}

COMMAND(aop_sync)
{
	struct stringlist *channel_aop_hosts;
	unsigned int opped;

	CHANREG_MODULE_COMMAND(cmod)

	if(!(channel_aop_hosts = dict_find(aop_hosts, reg->channel)) || !channel_aop_hosts->count)
	{
		reply("There are no AutoOp hosts added in $b%s$b.", reg->channel);
		return 0;
	}

	opped = check_aop_users(reg, NULL);
	reply("Synchronized channel op list with AutoOp list; opped $b%d$b users.", opped);
	return 1;
}
