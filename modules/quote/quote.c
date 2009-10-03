#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"
#include "irc.h"
#include "table.h"

MODULE_DEPENDS("commands", "chanreg", "help", NULL);

COMMAND(quote);
COMMAND(quotes_add);
COMMAND(quotes_del);
COMMAND(quotes_info);
COMMAND(quotes_list);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *quotes;

static void quote_db_read(struct dict *db_nodes, struct chanreg *reg);
static int quote_db_write(struct database_object *dbo, struct chanreg *reg);
static int quote_enable(struct chanreg *reg, enum cmod_enable_reason reason);
static int quote_disable(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void quote_move(struct chanreg *reg, const char *from, const char *to);

MODULE_INIT
{
	this = self;

	help_load(this, "quote.help");

	quotes = dict_create();
	dict_set_free_funcs(quotes, free, (dict_free_f *)stringlist_free);

	cmod = chanreg_module_reg("Quote", 0, quote_db_read, quote_db_write, NULL, quote_disable, quote_move);
	chanreg_module_readdb(cmod);

	DEFINE_COMMAND(self, "quote",		quote,		0, CMD_LAZY_ACCEPT_CHANNEL, "true");
	DEFINE_COMMAND(self, "quotes add",	quotes_add,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "quotes del",	quotes_del,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "quotes info",	quotes_info,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "quotes list",	quotes_list,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");

	srand(now);
}

MODULE_FINI
{
	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(quotes);
}

static void quote_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct stringlist *slist;

	if((slist = database_fetch(db_nodes, "quotes", DB_STRINGLIST)))
	{
		dict_insert(quotes, strdup(reg->channel), stringlist_copy(slist));
	}
}

static int quote_db_write(struct database_object *dbo, struct chanreg *reg)
{
	struct stringlist *channel_quotes;

	if((channel_quotes = dict_find(quotes, reg->channel)) && channel_quotes->count)
	{
		database_obj_write_stringlist(dbo, "quotes", channel_quotes);
	}

	return 0;
}

static int quote_disable(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	dict_delete(quotes, reg->channel);

	return 0;
}

static void quote_move(struct chanreg *reg, const char *from, const char *to)
{
	struct dict_node *node = dict_find_node(quotes, from);
	if(!node)
		return;
	free(node->key);
	node->key = strdup(to);
}

COMMAND(quote)
{
	struct stringlist *channel_quotes;

	CHANREG_MODULE_COMMAND(cmod)

	channel_quotes = dict_find(quotes, reg->channel);

	if(!channel_quotes || !channel_quotes->count)
	{
		reply("There are no quotes in $b%s$b", reg->channel);
		return 0;
	}

	unsigned int item = 0;

	if(argc > 1)
	{
		item = atoi(argv[1]);

		if(!(item > 0 && item <= channel_quotes->count))
		{
			reply("You must enter a valid quote id or none at all for a random quote!");
			return 0;
		}

		item--; // stringlist is 0-based, public quote ids are 1-based
	}
	else
	{
		item = rand() % channel_quotes->count;
	}

	irc_send("PRIVMSG %s :$bQuote %u$b: %s", reg->channel, item + 1, channel_quotes->data[item]);
	return 0;
}

COMMAND(quotes_add)
{
	struct stringlist *channel_quotes;

	CHANREG_MODULE_COMMAND(cmod)

	assert_return(argc > 1, 1);

	if(!(channel_quotes = dict_find(quotes, reg->channel)))
	{
		channel_quotes = stringlist_create();
		dict_insert(quotes, strdup(reg->channel), channel_quotes);
	}

	char *quote = untokenize(argc - 1, argv + 1, " ");
	stringlist_add(channel_quotes, quote);
	reply("Quote added with id $b%u$b", channel_quotes->count);

	return 1;
}

COMMAND(quotes_del)
{
	struct stringlist *channel_quotes;

	CHANREG_MODULE_COMMAND(cmod)

	assert_return(argc > 1, 1);

	unsigned int item = atoi(argv[1]);

	if(!(channel_quotes = dict_find(quotes, reg->channel)) || !(item > 0 && item <= channel_quotes->count))
	{
		reply("You must enter a valid quote id to delete!");
		return 0;
	}

	reply("Deleted quote $b%u$b: %s", item, channel_quotes->data[item - 1]);
	stringlist_del(channel_quotes, item - 1);

	return 1;
}

COMMAND(quotes_info)
{
	struct stringlist *channel_quotes;

	CHANREG_MODULE_COMMAND(cmod)

	channel_quotes = dict_find(quotes, reg->channel);
	reply("There are currently $b%u$b quotes in my database for $b%s$b", (channel_quotes ? channel_quotes->count : 0), reg->channel);

	return 1;
}

COMMAND(quotes_list)
{
	struct stringlist *channel_quotes;

	CHANREG_MODULE_COMMAND(cmod)

	if(!(channel_quotes = dict_find(quotes, reg->channel)))
	{
		reply("There are no quotes in $b%s$b", reg->channel);
		return 0;
	}

	struct table *quotes_table = table_create(2, channel_quotes->count);
	table_set_header(quotes_table, "ID", "Quote");

	for(unsigned int i = 0; i < channel_quotes->count; i++)
	{
		quotes_table->data[i][0] = strtab(i + 1);
		quotes_table->data[i][1] = channel_quotes->data[i];
	}

	reply("Quote database entries for $b%s$b:", reg->channel);
	table_send(quotes_table, src->nick);
	table_free(quotes_table);

	return 1;
}

