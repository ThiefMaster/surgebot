#include "global.h"
#include "module.h"
#include "irc.h"
#include "database.h"
#include "conf.h"
#include "modules/commands/commands.h"


MODULE_DEPENDS("commands", NULL);

static struct
{
	struct stringlist *allowed_voters;
	struct stringlist *vote_options;
} klostervote_conf;

COMMAND(klostervote);
COMMAND(klostervotedetails);
static void klostervote_conf_reload();
static void klostervote_db_read(struct database *db);
static int klostervote_db_write(struct database *db);

static struct module *this;
static struct database *klostervote_db = NULL;
static struct dict *voted;

MODULE_INIT
{
	this = self;

	voted = dict_create();
	dict_set_free_funcs(voted, free, free);

	reg_conf_reload_func(klostervote_conf_reload);
	klostervote_conf_reload();

	klostervote_db = database_create("klostervote", klostervote_db_read, klostervote_db_write);
	database_read(klostervote_db, 1);
	database_set_write_interval(klostervote_db, 300);

	DEFINE_COMMAND(self, "klostervote",		klostervote,		0, 0, "inchannel(#kloster)");
	DEFINE_COMMAND(self, "klostervotedetails",	klostervotedetails,	0, 0, "inchannel(#kloster)");
}

MODULE_FINI
{
	database_write(klostervote_db);
	database_delete(klostervote_db);

	unreg_conf_reload_func(klostervote_conf_reload);

	dict_free(voted);
}

static void klostervote_conf_reload()
{
	klostervote_conf.allowed_voters = conf_get("klostervote/users", DB_STRINGLIST);
	klostervote_conf.vote_options = conf_get("klostervote/options", DB_STRINGLIST);
}


static void klostervote_db_read(struct database *db)
{
	struct dict *db_node;

	if((db_node = database_fetch(db->nodes, "voted", DB_OBJECT)))
	{
		dict_iter(rec, db_node)
		{
			const char *account = rec->key;
			const char *option = ((struct db_node *)rec->data)->data.string;
			dict_insert(voted, strdup(account), strdup(option));
		}
	}
}

static int klostervote_db_write(struct database *db)
{
	database_begin_object(db, "voted");
		dict_iter(node, voted)
		{
			database_write_string(db, node->key, node->data);
		}
	database_end_object(db);
	return 0;
}

COMMAND(klostervote)
{
	char *account, *dot, *option;
	int pos;

	if(match("*.*.support", src->host) && match("*.*.gamesurge", src->host))
	{
		reply("Your fakehost must be account.*.support or account.*.gamesurge to use this command.");
		return 0;
	}

	account = strdup(src->host);
	if((dot = strchr(account, '.')))
		*dot = '\0';

	if(!klostervote_conf.vote_options)
	{
		reply("No vote options defined.");
		free(account);
		return 0;
	}

	if(!klostervote_conf.allowed_voters || stringlist_find(klostervote_conf.allowed_voters, account) == -1)
	{
		reply("You are not allowed to vote.");
		free(account);
		return 0;
	}

	if(argc < 2)
	{
		struct dict *tmp = dict_create();
		for(unsigned int i = 0; i < klostervote_conf.vote_options->count; i++)
			dict_insert(tmp, klostervote_conf.vote_options->data[i], "0");

		dict_iter(node, voted)
		{
			const char *value;
			if(!(value = dict_find(tmp, node->data))) // should not happen
				dict_insert(tmp, node->data, "1");
			else
			{
				value = strtab(atoi(value) + 1);
				dict_delete(tmp, node->data);
				dict_insert(tmp, node->data, (char *)value);
			}
		}

		reply("Current vote results:");
		dict_iter(node, tmp)
			reply("  $b%s$b: %s", node->key, (const char *)node->data);
		if(!dict_find(voted, account))
			reply("To vote, use /msg $N %s <option>", argv[0]);

		dict_free(tmp);
		free(account);
		return 0;
	}

	if((option = dict_find(voted, account)))
	{
		reply("You have already voted for $b%s$b.", option);
		free(account);
		return 0;
	}

	if(!klostervote_conf.vote_options || (pos = stringlist_find(klostervote_conf.vote_options, argv[1])) == -1)
	{
		reply("Invalid options. Possible vote options are:");
		for(unsigned int i = 0; i < klostervote_conf.vote_options->count; i++)
			reply("  %s", klostervote_conf.vote_options->data[i]);
		free(account);
		return 0;
	}

	option = klostervote_conf.vote_options->data[pos];

	dict_insert(voted, strdup(account), strdup(option));
	reply("Voted successfully for $b%s$b.", option);

	free(account);
	return 1;
}

COMMAND(klostervotedetails)
{
	reply("Vote details ($b%d$b votes total):", dict_size(voted));
	dict_iter(node, voted)
		reply("  %s: $b%s$b", node->key, (const char *)node->data);
	return 0;
}
