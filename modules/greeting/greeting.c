#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"
#include "irc.h"
#include "irc_handler.h"
#include "stringlist.h"
#include "conf.h"

MODULE_DEPENDS("commands", "chanreg", "help", NULL);

static struct
{
	unsigned long max_greetings;
} greeting_conf;

COMMAND(greeting);
IRC_HANDLER(join);
static void greeting_db_read(struct dict *db_nodes, struct chanreg *reg);
static int greeting_db_write(struct database_object *dbo, struct chanreg *reg);
static int greeting_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void greeting_moved(struct chanreg *reg, const char *from, const char *to);
int greeting_reply_validator(struct chanreg *reg, struct irc_source *src, const char *value);
static void greeting_conf_reload(void);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *greetings;

static const unsigned long conf_max_greetings = 10;

MODULE_INIT
{
	this = self;

	help_load(this, "greeting.help");

	greetings = dict_create();
	dict_set_free_funcs(greetings, free, (dict_free_f *)stringlist_free);

	cmod = chanreg_module_reg("Greeting", 0, greeting_db_read, greeting_db_write, NULL, greeting_disabled, greeting_moved);
	chanreg_module_setting_reg(cmod, "Reply", "NOTICE", greeting_reply_validator, NULL, NULL);
	chanreg_module_readdb(cmod);

	reg_irc_handler("JOIN", join);

	reg_conf_reload_func(greeting_conf_reload);
	greeting_conf_reload();

	DEFINE_COMMAND(self, "greeting", greeting, 1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(400)");
}

MODULE_FINI
{
	unreg_conf_reload_func(greeting_conf_reload);
	unreg_irc_handler("JOIN", join);

	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(greetings);
}

static void greeting_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct stringlist *slist;

	if((slist = database_fetch(db_nodes, "greetings", DB_STRINGLIST)))
		dict_insert(greetings, strdup(reg->channel), stringlist_copy(slist));
}

static int greeting_db_write(struct database_object *dbo, struct chanreg *reg)
{
	struct stringlist *channel_greetings;
	if((channel_greetings = dict_find(greetings, reg->channel)) && channel_greetings->count)
		database_obj_write_stringlist(dbo, "greetings", channel_greetings);
	return 0;
}

static int greeting_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
		dict_delete(greetings, reg->channel);
	return 0;
}

static void greeting_moved(struct chanreg *reg, const char *from, const char *to)
{
	struct dict_node *node = dict_find_node(greetings, from);
	if(!node)
		return;
	free(node->key);
	node->key = strdup(to);
}

IRC_HANDLER(join)
{
	struct stringlist *channel_greetings;
	struct chanreg *reg;

	assert(argc > 1);

	if(!chanreg_module_active(cmod, argv[1]))
		return;

	if(!(channel_greetings = dict_find(greetings, argv[1])))
		return;

	assert((reg = chanreg_find(argv[1])));

	for(unsigned int i = 0; i < channel_greetings->count; i++)
	{
		if(*channel_greetings->data[i] != '\0')
			irc_send("%s %s :(%s) %s", chanreg_setting_get(reg, cmod, "Reply"), src->nick, argv[1], channel_greetings->data[i]);
	}
}

COMMAND(greeting)
{
	struct stringlist *channel_greetings = NULL;

	CHANREG_MODULE_COMMAND(cmod)

	if(argc > 1)
	{
		unsigned int idx = atoi(argv[1]);
		channel_greetings = dict_find(greetings, reg->channel);
		if(idx < 1 || idx > (channel_greetings ? (channel_greetings->count + 1) : 1))
		{
			reply("Invalid greeting index; must be between $b1$b and $b%d$b", channel_greetings ? (channel_greetings->count + 1) : 1);
			return 0;
		}

		idx -= 1; // now we have a valid array index
		if(argc > 2)
		{
			if(!strcmp(argv[2], "*")) // unset greeting
			{
				if(!channel_greetings || idx == channel_greetings->count)
					reply("Cannot delete non-existent greeting.");
				else if(idx == (channel_greetings->count - 1)) // last one -> simply delete it
				{
					channel_greetings->data[idx][0] = '\0';
					while(channel_greetings->count && channel_greetings->data[channel_greetings->count - 1][0] == '\0')
						stringlist_del(channel_greetings, channel_greetings->count - 1);
				}
				else
					channel_greetings->data[idx][0] = '\0';

				if(channel_greetings && channel_greetings->count == 0)
				{
					dict_delete(greetings, reg->channel);
					channel_greetings = NULL;
				}
			}
			else // set greeting
			{
				if(!channel_greetings)
				{
					channel_greetings = stringlist_create();
					dict_insert(greetings, strdup(reg->channel), channel_greetings);
				}

				if(idx == channel_greetings->count) // new greeting
				{
					if(greeting_conf.max_greetings && idx >= greeting_conf.max_greetings)
					{
						reply("Only $b%lu$b greetings are allowed per channel.", greeting_conf.max_greetings);
						return 0;
					}
					else
						stringlist_add(channel_greetings, untokenize(argc - 2, argv + 2, " "));
				}
				else // change existing greeting
				{
					free(channel_greetings->data[idx]);
					channel_greetings->data[idx] = untokenize(argc - 2, argv + 2, " ");
				}
			}
		}

		const char *greeting = "(None)";
		if(channel_greetings && idx < channel_greetings->count && *channel_greetings->data[idx])
			greeting = channel_greetings->data[idx];
		reply("Greeting $b%d$b: %s", (idx + 1), greeting);
		return 1;
	}

	if(!(channel_greetings = dict_find(greetings, reg->channel)))
	{
		reply("There are no greetings set in $b%s$b.", reg->channel);
		return 0;
	}

	reply("$bGreetings:$b");
	for(unsigned int i = 0; i < channel_greetings->count; i++)
		reply("  [%d] %s", (i + 1), (*channel_greetings->data[i] ? channel_greetings->data[i] : "(None)"));
	reply("Found $b%d$b greeting%s.", channel_greetings->count, (channel_greetings->count != 1 ? "s" : ""));
	return 1;
}

int greeting_reply_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	if(!strcasecmp(value, "NOTICE"))
		return 1;

	if(!strcasecmp(value, "PRIVMSG"))
	{
		reply("$b$uBy enabling greetings in PM, you agree with not abusing it to advertise in PM (see http://www.gamesurge.net/aup/). Doing so will result in punishments to the offending user by GameSurge staff.$u$b");
		return 1;
	}

	reply("The reply method can be either $bNOTICE$b or $bPRIVMSG$b.");
	return 0;
}

static void greeting_conf_reload(void)
{
	char *str;

	greeting_conf.max_greetings = ((str = conf_get("greeting/max_amount", DB_STRING)) && aredigits(str) ? strtoul(str, NULL, 10) : 10);
}
