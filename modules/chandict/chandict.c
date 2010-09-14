#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"
#include "irc.h"
#include "irc_handler.h"
#include "table.h"
#include "modules/chandict/chandict.h"

MODULE_DEPENDS("commands", "chanreg", "help", NULL)
;

COMMAND(chandict_add);
COMMAND(chandict_del);
COMMAND(chandict_addalias);
COMMAND(chandict_delalias);
COMMAND(chandict_list);
IRC_HANDLER(privmsg);
static void chandict_db_read(struct dict *db_nodes, struct chanreg *reg);
static int chandict_db_write(struct database_object *dbo, struct chanreg *reg);
static int chandict_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void chandict_moved(struct chanreg *reg, const char *from, const char *to);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *entries;
static struct dict *aliases;

MODULE_INIT
{
	this = self;

	aliases = dict_create();
	dict_set_free_funcs(aliases, free, (dict_free_f *) dict_free);

	entries = dict_create();
	dict_set_free_funcs(entries, free, (dict_free_f *) dict_free);

	cmod = chanreg_module_reg("Dictionary", 0, chandict_db_read, chandict_db_write, NULL, chandict_disabled, chandict_moved);
	chanreg_module_readdb(cmod);

	reg_irc_handler("PRIVMSG", privmsg);


	//commands for entries
	DEFINE_COMMAND(self, "chandict add", chandict_add, 2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "chandict del", chandict_del, 1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "chandict list", chandict_list, 0, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");


	//command for aliases
	DEFINE_COMMAND(self, "chandict addalias", chandict_addalias, 2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "chandict delalias", chandict_delalias, 1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");

	help_load(this, "chandict.help");
}

MODULE_FINI
{
	unreg_irc_handler("PRIVMSG", privmsg);

	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(entries);
	dict_free(aliases);
}

static void chandict_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct dict *db_node;


	//read all entries
	if ((db_node = database_fetch(db_nodes, "chandict", DB_OBJECT)))
	{
		struct dict *channel_entries = dict_create();
		dict_set_free_funcs(channel_entries, free, free);
		dict_insert(entries, strdup(reg->channel), channel_entries);

		dict_iter(rec, db_node)
		{
			const char *message = ((struct db_node *) rec->data)->data.string;
			dict_insert(channel_entries, strdup(rec->key), strdup(message));
		}
	}


	//read all aliases
	if ((db_node = database_fetch(db_nodes, "chandict_aliases", DB_OBJECT)))
	{
		struct dict *channel_aliases = dict_create();
		dict_set_free_funcs(channel_aliases, free, free);
		dict_insert(aliases, strdup(reg->channel), channel_aliases);

		dict_iter(rec, db_node)
		{
			const char *message = ((struct db_node *) rec->data)->data.string;
			dict_insert(channel_aliases, strdup(rec->key), strdup(message));
		}
	}
}

static int chandict_db_write(struct database_object *dbo, struct chanreg *reg)
{
	//write all entries
	struct dict *channel_entries;
	if ((channel_entries = dict_find(entries, reg->channel)))
	{
		database_obj_begin_object(dbo, "chandict");
		dict_iter(node, channel_entries)
		{
			database_obj_write_string(dbo, node->key, node->data);
		}
		database_obj_end_object(dbo);
	}

	//write all aliases
	struct dict *channel_aliases;
	if ((channel_aliases = dict_find(aliases, reg->channel)))
	{
		database_obj_begin_object(dbo, "chandict_aliases");
		dict_iter(node, channel_aliases)
		{
			database_obj_write_string(dbo, node->key, node->data);
		}
		database_obj_end_object(dbo);
	}
	return 0;
}

static int chandict_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if (delete_data)
	{
		dict_delete(entries, reg->channel);
		dict_delete(aliases, reg->channel);
	}
	return 0;
}

static void chandict_moved(struct chanreg *reg, const char *from, const char *to)
{
	dict_rename_key(entries, from, to);
	dict_rename_key(aliases, from, to);
}

IRC_HANDLER(privmsg)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;

	assert(argc > 2);
	if (!IsChannelName(argv[1]))
		return;

	if (!chanreg_module_active(cmod, argv[1]))
		return;

	if (!(channel_entries = dict_find(entries, argv[1])))
		return;

	if (strncmp(argv[2], "? ", 2))
		return;

	const struct dict_node *node = dict_find_node(channel_entries, argv[2] + 2); //search for entry
	if (!node) //no entry
	{
		if (!(channel_aliases = dict_find(aliases, argv[1])))
			return; //no aliases

		node = dict_find_node(channel_aliases, argv[2] + 2); //search for alias
		if (!node) //found no alias
			return;

		node = dict_find_node(channel_entries, (char *) node->data); //get entrie from alias

		if (!node) //failsave: alias for non existent entrie
			return;
	}

	irc_send("PRIVMSG %s :$b%s$b: %s", argv[1], node->key, (char *) node->data);
}

COMMAND(chandict_add)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;
	CHANREG_MODULE_COMMAND(cmod);
	char *old;

	if (!(channel_entries = dict_find(entries, reg->channel)))
	{
		channel_entries = dict_create();
		dict_set_free_funcs(channel_entries, free, free);
		dict_insert(entries, strdup(reg->channel), channel_entries);
	}

	if ((old = dict_find(channel_entries, argv[1])))
	{
		reply("$bWARNING:$b %s was already added; overwriting it.", argv[1]);
		reply("Old definition was: %s", old);
		dict_delete(channel_entries, argv[1]);
	}

	if ((channel_aliases = dict_find(aliases, reg->channel)))
	{
		char *alias = dict_find(channel_aliases, argv[1]);
		if (alias)
		{
			reply("$bWARNING:$b %s was an alias for %s; deleting it.", argv[1], alias);
			dict_delete(channel_aliases, argv[1]);
		}
	}

	dict_insert(channel_entries, strdup(argv[1]), untokenize(argc - 2, argv + 2, " "));
	reply("Added definition $b%s$b.", argv[1]);
	return 1;
}

COMMAND(chandict_del)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;
	CHANREG_MODULE_COMMAND(cmod);

	if (!(channel_entries = dict_find(entries, reg->channel)) || !dict_find(channel_entries, argv[1]))
	{
		reply("$b%s$b is not added.", argv[1]);
		return 0;
	}

	dict_delete(channel_entries, argv[1]);
	reply("Deleted definition $b%s$b.", argv[1]);

	if ((channel_aliases = dict_find(aliases, reg->channel)))
	{
		dict_iter(node, channel_aliases)
		{
			if (!strcasecmp(argv[1], node->data))
			{
				reply("Also deleted alias $b%s$b pointing to %s.", (char *)node->key, argv[1]);
				dict_delete(channel_aliases, node->key);
			}
		}
	}
	return 1;
}

static int sort_entries(const void *a_, const void *b_)
{
	const char *a = (*((const char ***) a_))[0];
	const char *b = (*((const char ***) b_))[0];
	return strcasecmp(a, b);
}

COMMAND(chandict_list)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;
	struct table *table;
	CHANREG_MODULE_COMMAND(cmod);

	if (!(channel_entries = dict_find(entries, reg->channel)) || !dict_size(channel_entries))
	{
		reply("There are no definitions added in $b%s$b.", reg->channel);
		return 0;
	}

	table = table_create(2, dict_size(channel_entries));
	table_set_header(table, "Word", "Text");

	unsigned int row = 0;
	dict_iter(node, channel_entries)
	{
		table->data[row][0] = node->key;
		table->data[row][1] = node->data;
		row++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_entries);
	reply("Dictionary entries in $b%s$b:", reg->channel);
	table_send(table, src->nick);
	table_free(table);

	if (!(channel_aliases = dict_find(aliases, reg->channel)) || !dict_size(channel_aliases))
		return 1;

	table = table_create(2, dict_size(channel_aliases));
	table_set_header(table, "Alias", "Word");

	row = 0;
	dict_iter(node, channel_aliases)
	{
		table->data[row][0] = node->key;
		table->data[row][1] = node->data;
		row++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_entries);
	reply("\xa0");
	reply("Dictionary aliases in $b%s$b:", reg->channel);
	table_send(table, src->nick);
	table_free(table);

	return 1;
}

COMMAND(chandict_addalias)
{
	struct dict *channel_aliases;
	struct dict *channel_entries;
	struct dict_node *entry_node;

	CHANREG_MODULE_COMMAND(cmod);

	if (!(channel_entries = dict_find(entries, reg->channel)) || !(entry_node = dict_find_node(channel_entries, argv[2])))
	{
		reply("Dictionary entry $b%s$b not found.", argv[2]);
		return 0;
	}

	const struct dict_node *node = dict_find_node(channel_entries, argv[1]);
	if (node)
	{
		reply("You can not add alias $b%s$b, it is already defined as: %s", (char *)node->key, (char *)node->data);
		return 0;
	}

	if (!(channel_aliases = dict_find(aliases, reg->channel)))
	{
		channel_aliases = dict_create();
		dict_set_free_funcs(channel_aliases, free, free);
		dict_insert(aliases, strdup(reg->channel), channel_aliases);
	}

	char *old = dict_find(channel_aliases, argv[1]);
	if (old)
	{
		reply("$bWARNING:$b %s was already added; overwriting it.", argv[1]);
		reply("Old alias was: %s", old);
		dict_delete(channel_aliases, argv[1]);
	}

	dict_insert(channel_aliases, strdup(argv[1]), strdup(entry_node->key));
	reply("Added alias $b%s$b.", argv[1]);
	return 1;
}

COMMAND(chandict_delalias)
{
	struct dict *channel_aliases;
	CHANREG_MODULE_COMMAND(cmod);

	if (!(channel_aliases = dict_find(aliases, reg->channel)) || !dict_find(channel_aliases, argv[1]))
	{
		reply("$b%s$b is not added.", argv[1]);
		return 0;
	}

	dict_delete(channel_aliases, argv[1]);
	reply("Deleted alias $b%s$b.", argv[1]);

	return 1;
}

struct dict *chandict_get_entries(const char* channel)
{
	return dict_find(entries, channel);
}

void chandict_add_entry(const char *channel, const char *entry, const char *data)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;

	if (!(channel_entries = dict_find(entries, channel)))
	{
		channel_entries = dict_create();
		dict_set_free_funcs(channel_entries, free, free);
		dict_insert(entries, strdup(channel), channel_entries);
	}

	if ((channel_aliases = dict_find(aliases, channel)))
		dict_delete(channel_aliases, entry);

	dict_delete(channel_entries, entry);
	dict_insert(channel_entries, strdup(entry), strdup(data));
}

void chandict_del_entry(const char *channel, const char *entry)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;

	if (!(channel_entries = dict_find(entries, channel))) //nothing for this channel
		return;

	//delete all aliases pointing to this entry
	if ((channel_aliases = dict_find(aliases, channel)))
	{
		dict_iter(node, channel_aliases)
		{
			if (!strcasecmp(entry, node->data))
				dict_delete(channel_aliases, node->key);
		}
	}
	dict_delete(channel_entries, entry);
}

struct dict *chandict_get_aliases(const char* channel)
{
	return dict_find(aliases, channel);
}

int chandict_add_alias(const char *channel, const char *alias, const char *entry)
{
	struct dict *channel_entries;
	struct dict *channel_aliases;
	struct dict_node *entry_node;

	// Check if we have a dictionary and have the entry linked to
	if (!(channel_entries = dict_find(entries, channel)) || !(entry_node = dict_find_node(channel_entries, entry)))
		return 0;

	// Check if we have an entry for this alias
	if (dict_find_node(channel_entries, alias))
		return 0;

	// Do we have an alias dict?
	if (!(channel_aliases = dict_find(aliases, channel)))
	{
		channel_aliases = dict_create();
		dict_set_free_funcs(channel_aliases, free, free);
		dict_insert(aliases, strdup(channel), channel_aliases);
	}

	// Add or overwrite the alias
	dict_delete(channel_aliases, alias);
	dict_insert(channel_aliases, strdup(alias), strdup(entry_node->key));
	return 1;
}

void chandict_del_alias(const char *channel, const char *alias)
{
	struct dict *channel_aliases;

	if (!(channel_aliases = dict_find(aliases, channel))) //nothing for this channel
		return;

	dict_delete(channel_aliases, alias);
}
