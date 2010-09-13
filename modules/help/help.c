#include "global.h"
#include "help.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "database.h"
#include "stringlist.h"
#include "stringbuffer.h"
#include "ptrlist.h"
#include "irc.h"
#include "conf.h"

MODULE_DEPENDS("commands", NULL);

struct help_category
{
	char *name;
	char *full_name;
	char *short_desc;
	struct stringlist *description;
	struct dict *entries;
	struct dict *subcategories;
	struct ptrlist *used_by;
};

struct help_entry
{
	char *name;
	struct help_category *parent;
	struct module *module;
	char *description;
	struct stringlist *text;
	struct stringlist *see_also;
};

static struct module *this;
static struct help_category *help_root;
static struct ptrlist *help_entries;
static struct stringlist *default_cat_description;

typedef void (help_replacer_func)(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg);

// module functions
static struct help_category *help_category_create_or_find(const char *name, struct help_category *parent);
static void help_category_free(struct help_category *category);
static void help_entry_free(struct help_entry *entry);
static void help_load_entry(struct module *module, const char *name, struct dict *data, struct help_category *category);
static void help_load_category(struct module *module, struct dict *entries, struct help_category *parent);
static void free_module_help(struct help_category *category, struct module *module);
static void module_unloaded(struct module *module);
// debugging functions
static void dump_help_entry(struct irc_source *src, struct help_entry *entry, unsigned int indent_len);
static void dump_help_category(struct irc_source *src, struct help_category *category, unsigned int indent_len);
// commands
COMMAND(help);
COMMAND(helpdebug);

MODULE_INIT
{
	this = self;

	default_cat_description = stringlist_create();
	stringlist_add(default_cat_description, strdup("{HELP_CATEGORY_LIST}"));
	stringlist_add(default_cat_description, strdup("{HELP_COMMAND_LIST}"));

	help_entries = ptrlist_create();
	help_root = help_category_create_or_find("*", NULL); // root category
	ptrlist_add(help_root->used_by, 0, this);
	reg_module_load_func(NULL, module_unloaded);

	DEFINE_COMMAND(self, "help", help, 0, CMD_ALLOW_UNKNOWN, "true");
	DEFINE_COMMAND(self, "helpdebug", helpdebug, 0, 0, "group(admins)");
}

MODULE_FINI
{
	unreg_module_load_func(NULL, module_unloaded);
	if(help_root->subcategories->count)
		log_append(LOG_WARNING, "help_root->subcategories->count = %d; should be 0", help_root->subcategories->count);
	if(dict_size(help_root->entries))
		log_append(LOG_WARNING, "dict_size(help_root->entries) = %d; should be 0", dict_size(help_root->entries));
	help_category_free(help_root);
	ptrlist_free(help_entries);
	stringlist_free(default_cat_description);
}

static struct help_category *help_category_create_or_find(const char *name, struct help_category *parent)
{
	struct help_category *category;
	if(parent && (category = dict_find(parent->subcategories, name)))
		return category;

	category = malloc(sizeof(struct help_category));
	memset(category, 0, sizeof(struct help_category));

	category->name = strdup(name);
	if(!parent) // this = root category
		category->full_name = strdup("");
	else if(strlen(parent->full_name) == 0) // parent = root category
		category->full_name = strdup(category->name);
	else
	{
		category->full_name = malloc(strlen(parent->full_name) + 1 + strlen(name) + 1); // parent + " " + name + \0
		sprintf(category->full_name, "%s %s", parent->full_name, category->name);
	}

	category->entries = dict_create();
	dict_set_free_funcs(category->entries, NULL, (dict_free_f *)help_entry_free);

	category->subcategories = dict_create();
	dict_set_free_funcs(category->subcategories, NULL, (dict_free_f *)help_category_free);

	category->used_by = ptrlist_create();

	if(parent)
		dict_insert(parent->subcategories, category->name, category);
	debug("Created help category %s in %s", name, parent ? parent->name : "(root)");
	return category;
}

static void help_category_free(struct help_category *category)
{
	free(category->name);
	free(category->full_name);
	dict_free(category->entries);
	dict_free(category->subcategories);
	ptrlist_free(category->used_by);

	if(category->short_desc)
		free(category->short_desc);
	if(category->description)
		stringlist_free(category->description);

	free(category);
}

static void help_entry_free(struct help_entry *entry)
{
	free(entry->name);
	free(entry->description);
	stringlist_free(entry->text);
	if(entry->see_also)
		stringlist_free(entry->see_also);
	ptrlist_del_ptr(help_entries, entry);
	free(entry);
}

static void help_load_entry(struct module *module, const char *name, struct dict *data, struct help_category *category)
{
	struct stringlist *text, *slist;
	const char *str;

	if(!(text = database_fetch(data, "help", DB_STRINGLIST)))
	{
		log_append(LOG_WARNING, "Found help entry %s without 'help' subkey.", name);
		return;
	}

	struct help_entry *entry = malloc(sizeof(struct help_entry));
	memset(entry, 0, sizeof(struct help_entry));
	entry->name = strdup(name);
	entry->parent = category;
	entry->module = module;
	entry->description = (str = database_fetch(data, "description", DB_STRING)) ? strdup(str) : strdup("");
	entry->text = stringlist_copy(text);
	entry->see_also = (slist = database_fetch(data, "see_also", DB_STRINGLIST)) ? stringlist_copy(slist) : NULL;
	dict_insert(category->entries, entry->name, entry);
	ptrlist_add(help_entries, 0, entry);
}

static void help_load_category(struct module *module, struct dict *entries, struct help_category *parent)
{
	dict_iter(node, entries)
	{
		const char *key = node->key;
		struct db_node *helpfile_node = node->data;

		if(*key == '*' && key[1] == '\0') // index
		{
			if(helpfile_node->type == DB_STRING)
			{
				if(parent->short_desc)
				{
					log_append(LOG_WARNING, "Found additional shortdesc entry for category '%s'", parent->name);
					continue;
				}

				parent->short_desc = strdup(helpfile_node->data.string);
			}
			else if(helpfile_node->type == DB_STRINGLIST)
			{
				if(parent->description)
				{
					log_append(LOG_WARNING, "Found additional description entry for category '%s'", parent->name);
					continue;
				}

				parent->description = stringlist_copy(helpfile_node->data.slist);
			}
			else
			{
				log_append(LOG_WARNING, "Found description entry which is not a stringlist/string.");
				continue;
			}
		}
		else if(*key == '*') // subcategory
		{
			if(helpfile_node->type != DB_OBJECT)
			{
				log_append(LOG_WARNING, "Found subcategory key '%s' which is not an object.", key);
				continue;
			}

			struct help_category *subcat = help_category_create_or_find(key + 1, parent);
			ptrlist_add(subcat->used_by, 0, module);
			help_load_category(module, helpfile_node->data.object, subcat);
		}
		else // help entry
		{
			if(helpfile_node->type != DB_OBJECT)
			{
				log_append(LOG_WARNING, "Found entry key '%s' which is not an object.", key);
				continue;
			}

			help_load_entry(module, key, helpfile_node->data.object, parent);
		}
	}
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

	help_load_category(module, help_db, help_root);
	dict_free(help_db);
}

static void free_module_help(struct help_category *category, struct module *module)
{
	debug("Freeing module helps (category: %s, subcategories: %d, entries: %d, modrefs: %d)", category->name, dict_size(category->subcategories), dict_size(category->entries), category->used_by->count);
	dict_iter(node, category->subcategories)
	{
		struct help_category *subcat = node->data;
		free_module_help(subcat, module);
		ptrlist_del_ptr(subcat->used_by, module);
		debug("Subcategories/Entries/Modrefs remaining in category %s: %d/%d/%d", subcat->name, dict_size(subcat->subcategories), dict_size(subcat->entries), subcat->used_by->count);
		if(dict_size(subcat->entries) == 0 && dict_size(subcat->subcategories) == 0 && subcat->used_by->count == 0)
		{
			debug("Deleting empty category %s from %s", subcat->name, category->name);
			dict_delete(category->subcategories, subcat->name);
		}
	}

	dict_iter(node, category->entries)
	{
		struct help_entry *entry = node->data;
		if(entry->module == module)
			dict_delete(category->entries, node->key);
	}
}

static void module_unloaded(struct module *module)
{
	free_module_help(help_root, module);
}

// Helper function to dump help entry
static void dump_help_entry(struct irc_source *src, struct help_entry *entry, unsigned int indent_len)
{
	unsigned int indent = 2*indent_len;

	reply("%*s$uModule:$u %s", indent, " ", entry->module->name);
	reply("%*s$uDescription:$u %s", indent, " ", entry->description);
	reply("%*s$uText:$u", indent, " ");
	for(unsigned int i = 0; i < entry->text->count; i++)
		reply("%*s  %s", indent, " ", entry->text->data[i]);
	reply("%*s$uSee also:$u", indent, " ");
	if(!entry->see_also)
		reply("%*s  (none)", indent, " ");
	else
	{
		for(unsigned int i = 0; i < entry->see_also->count; i++)
			reply("%*s  %s", indent, " ", entry->see_also->data[i]);
	}
}

// Helper function to dump help category
static void dump_help_category(struct irc_source *src, struct help_category *category, unsigned int indent_len)
{
	unsigned int indent = 2*indent_len;

	reply("%*s$uDescription:$u", indent, " ");
	if(!category->description)
		reply("%*s  (none)", indent, " ");
	else
	{
		for(unsigned int i = 0; i < category->description->count; i++)
			reply("%*s  %s", indent, " ", category->description->data[i]);
	}

	reply("%*s$uEntries:$u (%d)", indent, " ", dict_size(category->entries));
	dict_iter(node, category->entries)
	{
		reply("%*s  $b%s$b:", indent, " ", node->key);
		dump_help_entry(src, node->data, indent_len + 2);
	}

	reply("%*s$uSubcategories:$u (%d)", indent, " ", dict_size(category->subcategories));
	dict_iter(node, category->subcategories)
	{
		struct help_category *subcat = node->data;
		reply("%*s  $uSubcategory:$u %s", indent, " ", node->key);
		dump_help_category(src, subcat, indent_len + 2);
	}

	reply("%*s$uUsed by:$u (%d)", indent, " ", category->used_by->count);
	for(unsigned int i = 0; i < category->used_by->count; i++)
	{
		struct module *module = category->used_by->data[i]->ptr;
		reply("%*s  %s", indent, " ", module->name);
	}
}

COMMAND(helpdebug)
{
	reply("Help dump:");
	dump_help_category(src, help_root, 0);
	return 1;
}

static void help_replacer_category_list(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg)
{
	if(dict_size(category->subcategories))
		stringbuffer_append_printf(sbuf, "%s\n", (arg ? arg : "$uSubcategories:$u"));

	size_t longest_name = 0;
	dict_iter(node, category->subcategories)
	{
		struct help_category *subcat = node->data;
		size_t len = strlen(subcat->full_name);
		if(len > longest_name)
			longest_name = len;
	}

	dict_iter(node, category->subcategories)
	{
		struct help_category *subcat = node->data;
		if(subcat->short_desc)
		{
			stringbuffer_append_printf(sbuf, "    %-*s    %s\n", longest_name, subcat->full_name, subcat->short_desc);
		}
		else
		{
			stringbuffer_append_string(sbuf, "    ");
			stringbuffer_append_string(sbuf, subcat->full_name);
			stringbuffer_append_char(sbuf, '\n');
		}
	}
}

static void help_replacer_command_list(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg)
{
	if(dict_size(category->entries))
		stringbuffer_append_printf(sbuf, "%s\n", (arg ? arg : "$uCommands:$u"));

	dict_iter(node, category->entries)
	{
		struct help_entry *entry = node->data;
		struct command *cmd = command_find(entry->module, entry->name);
		if(!cmd)
		{
			log_append(LOG_WARNING, "Command %s.%s has a help entry but doesn't exist.", entry->module->name, entry->name);
			continue;
		}
		else if(!cmd->bind_count)
		{
			debug("Command %s.%s has no bindings; skipping it", entry->module->name, entry->name);
			continue;
		}

		assert_continue(cmd->bindings->count);
		for(unsigned int i = 0; i < cmd->bindings->count; i++)
		{
			struct cmd_binding *binding = cmd->bindings->data[i]->ptr;
			// Prefer a binding named like the command or use the newest binding if no preferred binding was found.
			if(strcasecmp(binding->name, entry->name) && i < cmd->bindings->count - 1)
				continue;
			stringbuffer_append_printf(sbuf, "    %-18s  %s", binding->name, entry->description);
			stringbuffer_append_char(sbuf, '\n');
			break;
		}
	}
}

static void help_replacer_binding(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg)
{
	stringbuffer_append_string(sbuf, binding->name);
}

static void help_replacer_command(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg)
{
	char *cmd_name, *mod_name;
	struct module *module;
	struct command *cmd;

	if(!arg)
		return;

	mod_name = arg;
	if((cmd_name = strchr(mod_name, '.')) == NULL || *(cmd_name + 1) == '\0')
	{
		stringbuffer_append_string(sbuf, "!Error!");
		return;
	}

	*cmd_name++ = '\0';
	if(!(module = module_find(mod_name)))
	{
		log_append(LOG_WARNING, "HELP_COMMAND: Module '%s' not found", mod_name);
		return;
	}

	if(!(cmd = command_find(module, cmd_name)))
	{
		log_append(LOG_WARNING, "HELP_COMMAND: Command '%s' not found in module '%s'", cmd_name, module->name);
		return;
	}

	if(!cmd->bind_count)
	{
		stringbuffer_append_printf(sbuf, "(%s.%s)", module->name, cmd->name);
		return;
	}

	for(unsigned int i = 0; i < cmd->bindings->count; i++)
	{
		struct cmd_binding *binding = cmd->bindings->data[i]->ptr;
		// Prefer a binding named like the command or use the newest binding if no preferred binding was found.
		if(strcasecmp(binding->name, cmd->name) && i < cmd->bindings->count - 1)
			continue;
		stringbuffer_append_string(sbuf, binding->name);
		break;
	}
}

static void help_replacer_conf_str(struct stringbuffer *sbuf, struct help_category *category, struct help_entry *help_entry, struct cmd_binding *binding, char *arg)
{
	const char *value;

	if(!arg)
		return;

	value = conf_get(arg, DB_STRING);
	if(value)
		stringbuffer_append_string(sbuf, value);
}

// ... = [key, callback] pairs for replacements, then NULL
static void send_help(struct irc_source *src, struct help_category *category, struct help_entry *entry, struct cmd_binding *binding, struct stringlist *text, ...)
{
	va_list args;
	const char *key;

	for(unsigned int i = 0; i < text->count; i++)
	{
		char *line = text->data[i];

		if(!*line) // empty string -> send line containing a hard space
		{
			reply("\xa0");
			continue;
		}

		struct stringbuffer *linebuf = stringbuffer_create();
		struct stringbuffer *funcres = stringbuffer_create();
		stringbuffer_append_string(linebuf, line);
		va_start(args, text);
		while((key = va_arg(args, const char *)))
		{
			help_replacer_func *func = va_arg(args, help_replacer_func *);
			char *key_start, *line_start;

			line_start = linebuf->string;
			while((key_start = strstr(line_start, key)) && key_start != line_start && *(--key_start) == '{')
			{
				char *key_end = key_start + 1 + strlen(key);
				char *arg = NULL;

				if(*key_end == '}')
					key_end++;
				else if(*key_end == ':' && *(key_end + 1))
				{
					char *arg_start = key_end + 1;
					key_end = strchr(arg_start, '}');
					if(!key_end)
					{
						log_append(LOG_WARNING, "Found invalid help replacer: {%s:%s", key, arg_start);
						continue;
					}

					arg = strndup(arg_start, key_end - arg_start);
					key_end++;
				}
				else
				{
					log_append(LOG_WARNING, "Found invalid help replacer: {%s%c", key, *key_end);
					continue;
				}

				stringbuffer_erase(linebuf, key_start - linebuf->string, key_end - key_start);
				func(funcres, category, entry, binding, arg);
				stringbuffer_insert(linebuf, key_start - linebuf->string, funcres->string);
				stringbuffer_flush(funcres);
				if(arg)
					free(arg);
				line_start = linebuf->string + (key_end - key_start);
			}
		}
		va_end(args);
		stringbuffer_free(funcres);

		char *str = linebuf->string;
		while(str && *str)
		{
			char *linebreak = strchr(str, '\n');
			if(linebreak)
				*linebreak = '\0';
			if(*str) // no need to send empty lines
				reply("%s", str);
			str = linebreak ? linebreak + 1 : NULL;
		}
		stringbuffer_free(linebuf);
	}

	if(binding && entry && entry->see_also) // show "see also" list
	{
		struct stringbuffer *sbuf = stringbuffer_create();
		for(unsigned int i = 0; i < entry->see_also->count; i++)
		{
			struct module *module;
			struct command *cmd;
			char *mod_name = strdup(entry->see_also->data[i]);
			char *cmd_name = strchr(mod_name, '.');
			if(!cmd_name || cmd_name[1] == '\0')
			{
				log_append(LOG_WARNING, "Found invalid see_also entry '%s' for '%s' in %s", entry->see_also->data[i], entry->name, category->name);
				free(mod_name);
				continue;
			}

			*cmd_name++ = '\0'; // Cuts off mod_name at the dot
			if(!(module = module_find(mod_name)))
			{
				debug("Skipping see_also entry '%s'; module doesn't exist", entry->see_also->data[i]);
				free(mod_name);
				continue;
			}

			if(!strcasecmp(binding->module->name, mod_name) && !strcasecmp(binding->cmd->name, cmd_name))
			{
				debug("Skipping see_also entry '%s'; command is currently shown command", entry->see_also->data[i]);
				free(mod_name);
				continue;
			}

			if(!(cmd = command_find(module, cmd_name)))
			{
				log_append(LOG_WARNING, "Skipping see_also entry '%s'; module exists but command doesn't", entry->see_also->data[i]);
				free(mod_name);
				continue;
			}

			if(!cmd->bind_count)
			{
				debug("Skipping see_also entry '%s'; command has no bindings", entry->see_also->data[i]);
				free(mod_name);
				continue;
			}

			if(!cmd->bindings->count)
			{
				log_append(LOG_ERROR, "Found command %s with bind_count=%u but bindings->count=%u", cmd->name, cmd->bind_count, cmd->bindings->count);
				free(mod_name);
				continue;
			}

			for(unsigned int i = 0; i < cmd->bindings->count; i++)
			{
				struct cmd_binding *cmd_binding = cmd->bindings->data[i]->ptr;
				// Prefer a binding named like the command or use the newest binding if no preferred binding was found.
				if(strcasecmp(cmd_binding->name, cmd_name) && i < cmd->bindings->count - 1)
					continue;
				if(sbuf->len)
					stringbuffer_append_string(sbuf, ", ");
				stringbuffer_append_string(sbuf, cmd_binding->name);
				break;
			}

			free(mod_name);
		}

		reply("$uSee also:$u %s", sbuf->string);
		stringbuffer_free(sbuf);
	}
}

COMMAND(help)
{
	struct help_category *cat;
	const char *key;
	struct cmd_binding *binding;

	if(argc < 2)
	{
		if(!help_root->description)
			reply("No help available.");
		else
			send_help(src, help_root, NULL, NULL, help_root->description, "HELP_CATEGORY_LIST", help_replacer_category_list,
										      "HELP_COMMAND_LIST", help_replacer_command_list,
										      "HELP_COMMAND", help_replacer_command,
										      "HELP_CONF_STR", help_replacer_conf_str,
										      NULL);
		return 1;
	}

	key = argline + (argv[1] - argv[0]);
	if((binding = binding_find_active(key)))
	{
		struct help_entry *entry = NULL;
		for(unsigned int i = 0; i < help_entries->count; i++)
		{
			struct help_entry *tmp = help_entries->data[i]->ptr;
			if(tmp->module == binding->module && !strcasecmp(tmp->name, binding->cmd->name))
			{
				entry = tmp;
				break;
			}
		}

		if(entry)
		{
			send_help(src, entry->parent, entry, binding, entry->text, "HELP_BINDING", help_replacer_binding,
										   "HELP_COMMAND", help_replacer_command,
										   "HELP_CONF_STR", help_replacer_conf_str,
										   NULL);
			return 1;
		}
	}

	// No binding found -> check for category
	cat = help_root;
	for(unsigned int i = 1; i < argc; i++)
	{
		cat = dict_find(cat->subcategories, argv[i]);
		if(!cat)
			break;
	}

	if(!cat)
	{
		reply("No help on that topic.");
		return 0;
	}

	send_help(src, cat, NULL, NULL, cat->description ? cat->description : default_cat_description,
		  "HELP_CATEGORY_LIST", help_replacer_category_list,
		  "HELP_COMMAND_LIST", help_replacer_command_list,
		  "HELP_COMMAND", help_replacer_command,
		  "HELP_CONF_STR", help_replacer_conf_str,
		  NULL);
	return 1;
}
