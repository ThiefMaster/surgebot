#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "irc.h"
#include "conf.h"
#include "sock.h"

MODULE_DEPENDS("commands", NULL);

struct module *this;

void module_reload_cb(const char *name, unsigned char success, unsigned int errors, void *ctx);
static void module_deps_recursive(struct irc_source *src, struct module *module, unsigned int depth);
static int do_backslash_arg(int start, int argc, char ***argv_ptr);
COMMAND(rehash);
COMMAND(die);
COMMAND(module_list);
COMMAND(module_deps);
COMMAND(module_add);
COMMAND(module_del);
COMMAND(module_reload);
COMMAND(binding_add);
COMMAND(binding_del);
COMMAND(binding_rule);

MODULE_INIT
{
	this = self;

	DEFINE_COMMAND(self, "rehash",		rehash,		1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "die",		die,		1, CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module list",	module_list,	1, 0, "group(admins)");
	DEFINE_COMMAND(self, "module deps",	module_deps,	1, 0, "group(admins)");
	DEFINE_COMMAND(self, "module add",	module_add,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module del",	module_del,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module reload",	module_reload,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "binding add",	binding_add,	3, CMD_REQUIRE_AUTHED | CMD_KEEP_BOUND, "group(admins)");
	DEFINE_COMMAND(self, "binding del",	binding_del,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "binding rule",	binding_rule,	2, CMD_REQUIRE_AUTHED, "group(admins)");
}

MODULE_FINI
{

}


COMMAND(rehash)
{
	if(conf_reload() == 0)
		reply("Config file reloaded");
	else
		reply("Could not reload config file");

	return 1;
}

COMMAND(die)
{
	extern int quit_poll;
	quit_poll = 1;
	irc_send_fast("QUIT :Received DIE");
	sock_poll();
	return 1;
}

COMMAND(module_list)
{
	struct stringlist *slist, *lines;
	struct dict *module_list = module_dict();

	reply("Loaded modules:");

	slist = stringlist_create();
	dict_iter(node, module_list)
	{
		struct module *module = node->data;
		stringlist_add(slist, strdup(module->name));
	}

	stringlist_sort(slist);
	lines = stringlist_to_irclines(src->nick, slist);
	for(int i = 0; i < lines->count; i++)
		reply("  %s", lines->data[i]);

	stringlist_free(slist);
	stringlist_free(lines);
	return 1;
}

COMMAND(module_deps)
{
	struct dict *module_list = module_dict();

	reply("Module dependencies (reverse):");
	dict_iter(node, module_list)
	{
		struct module *module = node->data;

		if(module->depend->count == 0)
			module_deps_recursive(src, module, 1);
	}

	return 1;
}

COMMAND(module_add)
{
	struct module *module;
	int result;

	if((module = module_find(argv[1])))
	{
		reply("Module $b%s$b is already loaded.", module->name);
		return 0;
	}

	if((result = module_add(argv[1])) == 0 && (module = module_find(argv[1])))
		reply("Module $b%s$b loaded.", module->name);
	else
		reply("Could not load module $b%s$b; error $b%d$b.", argv[1], result);

	return 1;
}

COMMAND(module_del)
{
	struct module *module;

	if((module = module_find(argv[1])) == NULL)
	{
		reply("Module $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(module == this)
	{
		reply("Module $b%s$b cannot be unloaded since it contains this command.", module->name);
		return 0;
	}

	if(module->rdepend->count)
	{
		reply("Module $b%s$b cannot be unloaded since $b%d$b other modules depend on it.", module->name, module->rdepend->count);
		return 0;
	}

	if(module_del(module->name) == 0)
		reply("Module $b%s$b unloaded.", argv[1]);
	else
		reply("Could not unload module $b%s$b.", module->name);

	return 1;
}


COMMAND(module_reload)
{
	struct module *module;

	if((module = module_find(argv[1])) == NULL)
	{
		reply("Module $b%s$b does not exist.", argv[1]);
		return 0;
	}

	module_reload_cmd(module->name, src->nick);
	return 1;
}

COMMAND(binding_add)
{
	struct dict *command_list = command_dict();
	struct module *module;
	struct command *cmd;
	char *tmp, *pos, *alias;
	unsigned int bound = 0;

	if((argc = do_backslash_arg(1, argc, &argv)) < 3)
		return -1;

	if((argc = do_backslash_arg(2, argc, &argv)) < 3)
		return -1;

	if(binding_find(argv[1]))
	{
		reply("There is already a binding named $b%s$b.", argv[1]);
		return 0;
	}

	tmp = strdup(argv[2]);
	if((pos = strchr(tmp, '.')) == NULL || *(pos + 1) == '\0')
	{
		reply("Module/command missing; use $bmodule.command$b.");
		free(tmp);
		return 0;
	}

	*pos++ = '\0';
	if((module = module_find(tmp)) == NULL)
	{
		reply("Module $b%s$b does not exist.", tmp);
		free(tmp);
		return 0;
	}

	if((!strcmp(argv[1], "*") &&  strcmp(pos, "*")) ||	// bind "* module.func"
	   ( strcmp(argv[1], "*") && !strcmp(pos, "*")))	// bind "name module.*"
	{
		reply("Cannot use wildcard name with function name (use $b%s * %s.*$b to bind all available commands)", argv[0], module->name);
		free(tmp);
		return 0;
	}

	if(!strcmp(argv[1], "*") && !strcmp(pos, "*")) // Bind all commands
	{
		dict_iter(node, command_list)
		{
			cmd = node->data;
			if(cmd->module != module || binding_find(cmd->name))
				continue;

			binding_add(cmd->name, module->name, cmd->name, NULL, NULL, 0);
			debug("Bound %s -> %s.%s", cmd->name, module->name, cmd->name);
			bound++;
		}

		reply("$b%d$b commands bound.", bound);
		free(tmp);
		return 1;
	}

	if((cmd = command_find(module, pos)) == NULL)
	{
		reply("Command $b%s$b not found in module $b%s$b.", pos, module->name);
		free(tmp);
		return 0;
	}

	alias = argc > 3 ? untokenize(argc-3, argv+3, " ") : NULL;
	binding_add(argv[1], module->name, cmd->name, alias, NULL, 0);
	reply("Bound $b%s$b to $b%s.%s%s%s$b", argv[1], module->name, cmd->name, alias ? " " : "", alias ? alias : "");

	if(alias)
		free(alias);
	free(tmp);
	return 1;
}

COMMAND(binding_del)
{
	struct cmd_binding *binding;

	if((argc = do_backslash_arg(1, argc, &argv)) < 1)
		return -1;

	if((binding = binding_find(argv[1])) == NULL)
	{
		reply("Binding $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(binding->cmd && binding->cmd->bind_count == 1 && (binding->cmd->flags & CMD_KEEP_BOUND))
	{
		reply("$b%s$b is the last binding for '%s.%s' and cannot be removed.", binding->name, binding->module_name, binding->cmd_name);
		return 0;
	}

	binding_del(binding);
	reply("Binding $b%s$b removed.", argv[1]);

	return 1;
}

COMMAND(binding_rule)
{
	struct cmd_binding *binding;
	char *rule;

	if((argc = do_backslash_arg(1, argc, &argv)) < 1)
		return -1;

	if((binding = binding_find(argv[1])) == NULL)
	{
		reply("Binding $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(argc > 2) // Change rule
	{
		if(!strcmp(argv[2], "*")) // Remove custom rule
		{
			binding_set_rule(binding, NULL);
			reply("Custom access rule for $b%s$b removed.", binding->name);
		}
		else // Set custom rule
		{
			rule = untokenize(argc-2, argv+2, " ");

			if(!command_rule_validate(rule))
				reply("Could not parse access rule '%s'", rule);
			else if(binding_set_rule(binding, rule) == 0)
				reply("Access rule for $b%s$b changed to '%s'.", binding->name, rule);
			else
				reply("Could not change rule.");

			free(rule);
			return 0;
		}
	}

	if(binding->rule)
		reply("Custom access rule for binding $b%s$b: %s", binding->name, binding->rule);
	else if(binding->cmd)
		reply("Access rule for $b%s$b (from command '$b%s.%s$b'): %s", binding->name, binding->module_name, binding->cmd_name, binding->cmd->rule);
	else
		reply("Access rule for $b%s$b (from command '$b%s.%s$b'): unknown - module not loaded", binding->name, binding->module_name, binding->cmd_name);

	return 1;
}


static void module_deps_recursive(struct irc_source *src, struct module *module, unsigned int depth)
{
	unsigned int i, nn, pos = 0;
	char prefix[512];

	assert(module);

	// This code to generate the prefix is based on trace_links() from srvx-1.3
	for(nn = 1; nn <= depth; )
		nn <<= 1;

	nn >>= 1;
	while(nn > 1)
	{
		nn >>= 1;
		prefix[pos++] = (depth & nn) ? ((nn == 1) ? '`' : ' ') : '|';
		prefix[pos++] = (nn == 1) ? '-': ' ';
	}
	prefix[pos] = '\0';


	reply("%s%s", prefix, module->name);
	if(module->rdepend->count == 0)
		return;

	for(i = 0; i < (module->rdepend->count - 1); i++)
		module_deps_recursive(src, module_find(module->rdepend->data[i]), (depth << 1));

	module_deps_recursive(src, module_find(module->rdepend->data[i]), (depth << 1)|1);
}

static int do_backslash_arg(int start, int argc, char ***argv_ptr)
{
	char **argv = *argv_ptr;

	if(argc < (start + 1))
		return -1;

	if(argv[start][strlen(argv[start])-1] == '\\')
	{
		if(argc < (start + 2))
			return -2;

		// Replace backslash with space and move the next argument one char back so it overlaps the \0 after the backslash
		argv[start][strlen(argv[start]) - 1] = ' ';
		memmove(argv[start+1]-1, argv[start+1], strlen(argv[start+1])+1);

		for(int i = start; i >= 0; i--)
			argv[i+1] = argv[i];

		(*argv_ptr)++;
		argc--;
	}

	return argc;
}

