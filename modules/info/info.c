#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "chanuser.h"
#include "irc.h"
#include "table.h"

MODULE_DEPENDS("commands", "help", NULL);

COMMAND(command);
COMMAND(stats_bot);
COMMAND(stats_commands);
COMMAND(stats_bindings);
COMMAND(stats_bindings_verbose);

static int sort_commands(const void *a_, const void *b_);
static int sort_bindings(const void *a_, const void *b_);

MODULE_INIT
{
	help_load(self, "info.help");

	DEFINE_COMMAND(self, "command",		command,		2, 0, "true");
	DEFINE_COMMAND(self, "stats bot",	stats_bot,		1, 0, "true");
	DEFINE_COMMAND(self, "stats commands",	stats_commands,		1, 0, "true");
	DEFINE_COMMAND(self, "stats bindings",	stats_bindings,		1, 0, "true");
	DEFINE_COMMAND(self, "stats bindings2",	stats_bindings_verbose,	1, 0, "true");
}

MODULE_FINI
{

}


COMMAND(command)
{
	struct cmd_binding *binding;
	char *name;

	name = untokenize(argc - 1, argv + 1, " ");
	binding = binding_find(name);

	if(!binding)
	{
		reply("There is no command bound as $b%s$b", name);
		free(name);
		return 0;
	}
	free(name);

	if(binding->alias)
		reply("$b%s$b is an alias for: $b%s.%s %s$b", binding->name, binding->module_name, binding->cmd_name, binding->alias);
	else
		reply("$b%s$b is a binding of: $b%s.%s$b", binding->name, binding->module_name, binding->cmd_name);

	if(binding->cmd && binding->cmd->min_argc > 1)
		reply("$b%s$b expects at least $b%d$b argument%s.", binding->name, binding->cmd->min_argc - 1, (binding->cmd->min_argc == 2 ? "" : "s"));

	if(binding->rule && strcasecmp(binding->rule, "true"))
		reply("The access rule for $b%s$b is: $b%s$b", binding->name, binding->rule);

	if(binding->cmd)
	{
		if(!binding->rule && strcasecmp(binding->cmd->rule, "true"))
			reply("The access rule for $b%s$b is: $b%s$b", binding->name, binding->cmd->rule);
		if(binding->cmd->flags & CMD_REQUIRE_AUTHED)
			reply("You must be authed to use this command.");
		if(binding->cmd->flags & CMD_ONLY_PRIVMSG)
			reply("You $bMUST$b use $b/msg $N %s$b to invoke this command.", binding->name);
	}
	else
	{
		if(!binding->rule)
			reply("The access rule for $b%s$b is unknown.", binding->name);
		reply("$b%s$b is from module $b%s$b which is currently not loaded.", binding->name, binding->module_name);
	}

	reply("End of information about $b%s$b.", binding->name);
	return 1;
}

COMMAND(stats_bot)
{
	reply("$bUptime:      $b %s",  time2string(now - bot.start));
	reply("$bLinked:      $b %s",  time2string(now - bot.linked));
	reply("$bServer:      $b %s",  bot.server_name);
	reply("$bLines (rcvd):$b %lu", bot.lines_received);
	reply("$bLines (sent):$b %lu", bot.lines_sent);
	reply("$bChannels:    $b %d",  dict_size(channel_dict()));
	reply("$bUsers:       $b %d",  dict_size(user_dict()));
	return 1;
}

COMMAND(stats_commands)
{
	struct dict *commands = command_dict();
	struct table *table;
	unsigned int i = 0;

	table = table_create(5, dict_size(commands));
	table_set_header(table, "Module", "Command", "Min. Args", "Bind Count", "Access Rule");

	dict_iter(node, commands)
	{
		struct command *command = node->data;
		table->data[i][0] = command->module->name;
		table->data[i][1] = command->name;
		table->data[i][2] = strtab(command->min_argc - 1);
		table->data[i][3] = strtab(command->bind_count);
		table->data[i][4] = command->rule;
		i++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_commands);
	table_send(table, src->nick);
	table_free(table);

	reply("$b%d$b commands registered.", dict_size(commands));
	return 1;
}

COMMAND(stats_bindings)
{
	struct dict *bindings = binding_dict();
	struct table *table;
	unsigned int i = 0;

	table = table_create(2, dict_size(bindings));
	table_set_header(table, "Name", "Access Rule");

	dict_iter(node, bindings)
	{
		struct cmd_binding *binding = node->data;

		if(argc > 1 && match(argv[1], binding->name))
			continue;

		table->data[i][0] = binding->name;
		table->data[i][1] = binding->rule ? binding->rule : (binding->cmd ? binding->cmd->rule : "(unknown)");
		i++;
	}

	table->rows = i;
	qsort(table->data, table->rows, sizeof(table->data[0]), sort_bindings);
	table_send(table, src->nick);
	table_free(table);
	return 1;
}

COMMAND(stats_bindings_verbose)
{
	struct dict *bindings = binding_dict();
	struct table *table;
	unsigned int i = 0;

	table = table_create(6, dict_size(bindings));
	table_set_header(table, "Name", "Module", "Command", "Alias", "Active", "Access Rule");

	dict_iter(node, bindings)
	{
		struct cmd_binding *binding = node->data;

		if(argc > 1 && match(argv[1], binding->name))
			continue;

		table->data[i][0] = binding->name;
		table->data[i][1] = binding->module_name;
		table->data[i][2] = binding->cmd_name;
		table->data[i][3] = binding->alias ? "Yes" : "No";
		table->data[i][4] = binding->cmd ? "Yes" : "No";
		table->data[i][5] = binding->rule ? binding->rule : (binding->cmd ? binding->cmd->rule : "(unknown)");
		i++;
	}

	table->rows = i;
	qsort(table->data, table->rows, sizeof(table->data[0]), sort_bindings);
	table_send(table, src->nick);
	table_free(table);
	return 1;
}

static int sort_commands(const void *a_, const void *b_)
{
	const char *mod_a = (*((const char ***)a_))[0];
	const char *mod_b = (*((const char ***)b_))[0];
	const char *name_a = (*((const char ***)a_))[1];
	const char *name_b = (*((const char ***)b_))[1];
	int res = strcasecmp(mod_a, mod_b);
	return res ? res : strcasecmp(name_a, name_b);
}

static int sort_bindings(const void *a_, const void *b_)
{
	const char *name_a = (*((const char ***)a_))[0];
	const char *name_b = (*((const char ***)b_))[0];
	return strcasecmp(name_a, name_b);
}
