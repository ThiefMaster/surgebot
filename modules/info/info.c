#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "chanuser.h"
#include "irc.h"
#include "table.h"
#include "timer.h"

MODULE_DEPENDS("commands", "help", NULL);

COMMAND(command);
COMMAND(stats_bot);
COMMAND(stats_commands);
COMMAND(stats_bindings);
COMMAND(stats_bindings_verbose);
COMMAND(stats_timers);

static int sort_commands(const void *a_, const void *b_);
static int sort_bindings(const void *a_, const void *b_);
static int sort_timers(const void *a_, const void *b_);

MODULE_INIT
{
	help_load(self, "info.help");

	DEFINE_COMMAND(self, "command",		command,		1, 0, "true");
	DEFINE_COMMAND(self, "stats bot",	stats_bot,		0, 0, "true");
	DEFINE_COMMAND(self, "stats commands",	stats_commands,		0, 0, "true");
	DEFINE_COMMAND(self, "stats bindings",	stats_bindings,		0, 0, "true");
	DEFINE_COMMAND(self, "stats bindings2",	stats_bindings_verbose,	0, 0, "true");
	DEFINE_COMMAND(self, "stats timers",	stats_timers,		0, 0, "group(admins)");
}

MODULE_FINI
{

}

COMMAND(stats_timers)
{
	struct dict *timers = timer_dict();
	struct table *table = table_create(3, dict_size(timers));
	unsigned int i = 0;
	const char *wildmask = (argc > 1 ? argv[1] : NULL);

	table_set_header(table, "Id", "Name", "Execute in");
	table->col_flags[0] |= TABLE_CELL_FREE | TABLE_CELL_ALIGN_RIGHT;

	dict_iter(node, timers)
	{
		struct timer *tmr = node->data;
		time_t *triggering = malloc(sizeof(time_t));

		if(tmr->triggered || !tmr->name || tmr->time <= now)
			continue;

		if(wildmask && match(wildmask, tmr->name))
			continue;

		*triggering = tmr->time - now;

		table_col_num(table, i, 0, tmr->id);
		table->data[i][1] = tmr->name;
		// To compare the timers, this needs to stay an int, so no 'real' converting for now
		table->data[i][2] = (char*)triggering;
		
		i++;
	}

	table->rows = i;
	qsort(table->data, table->rows, sizeof(table->data[0]), sort_timers);
	// Now convert all triggering times to 'true' strings
	for(unsigned int i = 0; i < table->rows; ++i)
	{
		time_t *backup = (time_t*)table->data[i][2];
		table->data[i][2] = strdupa(duration2string(*backup));
		free(backup);
	}

	if(i)
		table_send(table, src->nick);

	if(wildmask)
		reply("There are $b%d$b active timers matching $b%s$b.", i, wildmask);
	else
		reply("There are $b%d$b active timers.", i);
	
	table->rows = dict_size(timers); // restore old row count so all rows get free()'d
	table_free(table);
	return 1;
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
	time_t linked = now - bot.linked;

	reply("$bUptime:      $b %s",  duration2string(now - bot.start));
	reply("$bLinked:      $b %s",  duration2string(linked));
	reply("$bServer:      $b %s",  bot.server_name);
	reply("$bLines (rcvd):$b %lu (%.2f lines/min)", bot.lines_received, (float)bot.lines_received / ((float)linked / 60.0));
	reply("$bLines (sent):$b %lu", bot.lines_sent);
	reply("$bChannels:    $b %d",  dict_size(channel_dict()));
	reply("$bUsers:       $b %d",  dict_size(user_dict()));
	return 1;
}

COMMAND(stats_commands)
{
	struct dict *commands = command_dict();
	struct table *table;
	unsigned int i = 0, command_count;
	struct module *module = NULL;

	if(argc > 1 && !(module = module_find(argv[1])))
	{
		reply("There is no module called $b%s$b.", argv[1]);
		return 0;
	}

	table = table_create(5, dict_size(commands));
	table_set_header(table, "Module", "Command", "Min. Args", "Bind Count", "Access Rule");
	table->col_flags[2] |= TABLE_CELL_FREE;
	table->col_flags[3] |= TABLE_CELL_FREE;

	command_count = dict_size(commands);

	dict_iter(node, commands)
	{
		struct command *command = node->data;
		if(argc > 1 && command->module != module)
		{
			command_count--;
			continue;
		}

		table->data[i][0] = command->module->name;
		table->data[i][1] = command->name;
		table_col_num(table, i, 2, command->min_argc);
		table_col_num(table, i, 3, command->bind_count);
		table->data[i][4] = command->rule;
		i++;
	}

	table->rows = i;
	qsort(table->data, table->rows, sizeof(table->data[0]), sort_commands);

	if(command_count)
	{
		table_send(table, src->nick);
		reply("$b%d$b commands registered.", command_count);
	}
	else if(module)
		reply("Module $b%s$b does not implement any commands.", module->name);

	table_free(table);
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

		if(binding->comp_rule && command_rule_executable(binding->comp_rule))
		{
			// Only show commands the user is able to execute
			if(command_rule_exec(binding->comp_rule, src, user, channel, channelname) != CR_ALLOW)
				continue;
		}

		if(argc > 1 && match(argv[1], binding->name))
			continue;

		table->data[i][0] = binding->name;
		table->data[i][1] = binding->rule ? binding->rule : (binding->cmd ? binding->cmd->rule : "(unknown)");
		i++;
	}

	table->rows = i;
	qsort(table->data, table->rows, sizeof(table->data[0]), sort_bindings);
	table_send(table, src->nick);
	table->rows = dict_size(bindings); // restore old row count so all rows get free()'d
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
	table->rows = dict_size(bindings); // restore old row count so all rows get free()'d
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

static int sort_timers(const void *a_, const void *b_)
{
	time_t a = *(time_t*)((*(const char ***)a_)[2]);
	time_t b = *(time_t*)((*(const char ***)b_)[2]);
	return a - b;
}
