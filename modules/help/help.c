#include "global.h"
#include "help.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "database.h"
#include "stringlist.h"
#include "irc.h"
#include "conf.h"

MODULE_DEPENDS("commands", NULL);

struct help_entry
{
	char		*key;
	struct module	*module;
	struct stringlist	*text;
};

static struct dict *help_entries;

static void module_unloaded(struct module *module);
COMMAND(help);

MODULE_INIT
{
	help_entries = dict_create();
	reg_module_load_func(NULL, module_unloaded);

	DEFINE_COMMAND(self, "help", help, 1, CMD_ALLOW_UNKNOWN, "true");
}

MODULE_FINI
{
	unreg_module_load_func(NULL, module_unloaded);
	assert(dict_size(help_entries) == 0);
	dict_free(help_entries);
}

void help_load(struct module *module, const char *file)
{
	struct dict *help_db;
	const char *module_dir;
	char path[PATH_MAX];

	module_dir = conf_get("core/module_path", DB_STRING);
	if(!module_dir || strlen(module_dir) == 0)
		module_dir = ".";

	snprintf(path, sizeof(path), "%s/%s/%s", module_dir, module->name, file);
	if((help_db = database_load(path)) == NULL)
	{
		log_append(LOG_WARNING, "Could not load help file '%s'", path);
		return;
	}

	dict_iter(node, help_db)
	{
		struct db_node *helpfile_node = node->data;
		if(helpfile_node->type != DB_STRINGLIST)
		{
			log_append(LOG_WARNING, "Helpfile entry %s/%s is not a stringlist, skipping", file, node->key);
			continue;
		}
		else if(strlen(node->key) == 0)
		{
			log_append(LOG_WARNING, "Helpfile entry %s/%s has an empty key, skipping", file, node->key);
			continue;
		}
		else if(helpfile_node->data.slist->count == 0)
		{
			log_append(LOG_WARNING, "Helpfile entry %s/%s is empty, skipping", file, node->key);
			continue;
		}

		debug("Adding help entry '%s' for module %s", node->key, module->name);
		struct help_entry *help = malloc(sizeof(struct help_entry));
		help->key = strdup(node->key);
		help->module = module;
		help->text = stringlist_copy(helpfile_node->data.slist);
		dict_insert(help_entries, help->key, help);
	}

	dict_free(help_db);
}

static void module_unloaded(struct module *module)
{
	dict_iter(node, help_entries)
	{
		struct help_entry *help = node->data;
		if(help->module == module)
		{
			debug("Deleting help entry '%s'", help->key);
			dict_delete(help_entries, help->key);
			free(help->key);
			stringlist_free(help->text);
			free(help);
		}
	}

}


COMMAND(help)
{
	struct cmd_binding *binding;
	struct help_entry *help;
	char *key, *cmd_key = NULL;

	// If there is no arg, we use '*' which indicates the help index, otherwise we use all command arguments
	key = (argc > 1 ? untokenize(argc - 1, argv + 1, " ") : strdup("*"));

	// Check for active binding. If we find one, we check for a help entry named like the command
	if(argc > 1 && (binding = binding_find_active(key)) != NULL)
		cmd_key = binding->cmd_name;

	// if we have a cmd key, we check it first and fallback to the specified key; otherwise we only check the specified key
	if(cmd_key && (help = dict_find(help_entries, cmd_key)) != NULL)
	{
		free(key);
		key = strdup(cmd_key); // so we can use key later to reference the used topic
	}
	else if((help = dict_find(help_entries, key)) == NULL)
	{
		reply("No help on that topic");
		free(key);
		return 0;
	}

	if(cmd_key)
		reply("Help for command $b%s$b:", cmd_key);
	else if(argc > 1)
		reply("Help for $b%s$b:", key);
	else
		reply("Help:");

	for(unsigned int i = 0; i < help->text->count; i++)
		reply("%s", help->text->data[i]);

	free(key);
	return 1;
}
