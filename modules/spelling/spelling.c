#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "irc.h"
#include "irc_handler.h"
#include "table.h"

MODULE_DEPENDS("commands", "chanreg", NULL);

COMMAND(word_add);
COMMAND(word_del);
COMMAND(word_list);
IRC_HANDLER(privmsg);
static void spelling_db_read(struct dict *db_nodes, struct chanreg *reg);
static int spelling_db_write(struct database_object *dbo, struct chanreg *reg);
static void spelling_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *words;

MODULE_INIT
{
	this = self;

	words = dict_create();
	dict_set_free_funcs(words, free, (dict_free_f *)dict_free);

	cmod = chanreg_module_reg("Spelling", 0, spelling_db_read, spelling_db_write, NULL, spelling_disabled);
	chanreg_module_readdb(cmod);

	reg_irc_handler("PRIVMSG", privmsg);

	DEFINE_COMMAND(self, "word add",	word_add,	3, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "word del",	word_del,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "word list",	word_list,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
}

MODULE_FINI
{
	unreg_irc_handler("PRIVMSG", privmsg);

	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(words);
}

static void spelling_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct dict *db_node;

	if((db_node = database_fetch(db_nodes, "words", DB_OBJECT)))
	{
		struct dict *channel_words = dict_create();
		dict_set_free_funcs(channel_words, free, free);
		dict_insert(words, strdup(reg->channel), channel_words);

		dict_iter(rec, db_node)
		{
			const char *message = ((struct db_node *)rec->data)->data.string;
			dict_insert(channel_words, strdup(rec->key), strdup(message));
		}

	}
}

static int spelling_db_write(struct database_object *dbo, struct chanreg *reg)
{
	struct dict *channel_words;
	if((channel_words = dict_find(words, reg->channel)))
	{
		database_obj_begin_object(dbo, "words");
			dict_iter(node, channel_words)
			{
				database_obj_write_string(dbo, node->key, node->data);
			}
		database_obj_end_object(dbo);
	}
	return 0;
}

static void spelling_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
		dict_delete(words, reg->channel);
}

IRC_HANDLER(privmsg)
{
	struct dict *channel_words;

	assert(argc > 2);
	if(!IsChannelName(argv[1]))
		return;

	if(!chanreg_module_active(cmod, argv[1]))
		return;

	if(!(channel_words = dict_find(words, argv[1])))
		return;

	dict_iter(node, channel_words)
	{
		if(match(node->key, argv[2]) == 0)
			irc_send("NOTICE %s :%s", src->nick, (const char *)node->data);
	}
}

COMMAND(word_add)
{
	struct dict *channel_words;

	CHANREG_MODULE_COMMAND;

	if(!(channel_words = dict_find(words, reg->channel)))
	{
		channel_words = dict_create();
		dict_set_free_funcs(channel_words, free, free);
		dict_insert(words, strdup(reg->channel), channel_words);
	}

	if(dict_find(channel_words, argv[1]))
	{
		reply("$b%s$b is already added; overwriting it.", argv[1]);
		dict_delete(channel_words, argv[1]);
	}

	dict_insert(channel_words, strdup(argv[1]), untokenize(argc - 2, argv + 2, " "));
	reply("Added word $b%s$b.", argv[1]);
	return 1;
}

COMMAND(word_del)
{
	struct dict *channel_words;

	CHANREG_MODULE_COMMAND;

	if(!(channel_words = dict_find(words, reg->channel)) || !dict_find(channel_words, argv[1]))
	{
		reply("$b%s$b is not added.", argv[1]);
		return 0;
	}

	dict_delete(channel_words, argv[1]);
	reply("Deleted word $b%s$b.", argv[1]);
	return 1;
}

static int sort_words(const void *a_, const void *b_)
{
	const char *a = (*((const char ***)a_))[0];
	const char *b = (*((const char ***)b_))[0];
	return strcasecmp(a, b);
}

COMMAND(word_list)
{
	struct dict *channel_words;
	struct table *table;
	unsigned int row = 0;

	CHANREG_MODULE_COMMAND;

	if(!(channel_words = dict_find(words, reg->channel)) || !dict_size(channel_words))
	{
		reply("There are no words added in $b%s$b.", reg->channel);
		return 0;
	}

	table = table_create(2, dict_size(channel_words));
	table_set_header(table, "Word", "Message");

	dict_iter(node, channel_words)
	{
		table->data[row][0] = node->key;
		table->data[row][1] = node->data;
		row++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_words);
	table_send(table, src->nick);
	table_free(table);
	return 1;
}

