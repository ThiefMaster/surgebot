#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "modules/help/help.h"
#include "irc.h"
#include "conf.h"
#include "sock.h"
#include "timer.h"

MODULE_DEPENDS("commands", "help", NULL);

struct module *this;

static void module_deps_recursive(struct irc_source *src, struct module *module, unsigned int depth);
static int do_backslash_arg(unsigned int start, unsigned int argc, char ***argv_ptr);
COMMAND(rehash);
COMMAND(conf_get);
COMMAND(die);
COMMAND(raw);
COMMAND(module_list);
COMMAND(module_deps);
COMMAND(module_add);
COMMAND(module_del);
COMMAND(module_reload);
COMMAND(binding_add);
COMMAND(binding_del);
COMMAND(binding_rule);
COMMAND(writeall);
COMMAND(trigger_timer);
COMMAND(exec);

MODULE_INIT
{
	this = self;

	help_load(self, "admin.help");

	DEFINE_COMMAND(self, "rehash",		rehash,		0, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "conf get",	conf_get,	0, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "die",		die,		0, CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "raw",		raw,		1, CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module list",	module_list,	0, 0, "group(admins)");
	DEFINE_COMMAND(self, "module deps",	module_deps,	0, 0, "group(admins)");
	DEFINE_COMMAND(self, "module add",	module_add,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module del",	module_del,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "module reload",	module_reload,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "binding add",	binding_add,	2, CMD_REQUIRE_AUTHED | CMD_KEEP_BOUND, "group(admins)");
	DEFINE_COMMAND(self, "binding del",	binding_del,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "binding rule",	binding_rule,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "writeall",	writeall,	0,	0,	"group(admins)");
	DEFINE_COMMAND(self, "timer trigger",	trigger_timer, 1, 0, "group(admins)");
	DEFINE_COMMAND(self, "exec",		exec,		1, CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED | CMD_ACCEPT_CHANNEL, "group(admins)");
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

COMMAND(conf_get)
{
	struct db_node *node;

	if(argc < 2)
	{
		reply("The following keys can be retrieved:");
		dict_iter(subnode, conf_root())
			reply(" %s", subnode->key);
		return 1;
	}

	if(!(node = conf_node(argv[1])))
	{
		reply("$b%s$b has not been set.", argv[1]);
		return 0;
	}

	if(node->type == DB_STRING)
		reply("$b%s$b is set to $b%s$b.", argv[1], node->data.string);
	else if(node->type == DB_STRINGLIST)
	{
		reply("$b%s$b contains the following values:", argv[1]);
		for(unsigned int i = 0; i < node->data.slist->count; i++)
			reply(" %s", node->data.slist->data[i]);
	}
	else if(node->type == DB_OBJECT)
	{
		reply("$b%s$b contains the following keys:", argv[1]);
		dict_iter(subnode, node->data.object)
			reply(" %s", subnode->key);
	}

	return 1;
}

COMMAND(die)
{
	extern int quit_poll;
	quit_poll = 1;

	if(argc > 1)
		irc_send_fast("Quit :Received DIE [%s]", argline + (argv[1] - argv[0]));
	else
		irc_send_fast("QUIT :Received DIE");

	sock_poll();
	return 1;
}

COMMAND(raw)
{
	char *msg = untokenize(argc - 1, argv + 1, " ");
	irc_send_fast("%s", msg);
	free(msg);
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
	for(unsigned int i = 0; i < lines->count; i++)
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
			return 1;
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

COMMAND(writeall)
{
	struct dict *databases = database_dict();
	struct timeval start, stop;
	int count = 0, bad_count = 0;

	gettimeofday(&start, NULL);
	dict_iter(node, databases)
	{
		if(!database_write(node->data))
			count++;
		else
			bad_count++;
	}
	gettimeofday(&stop, NULL);
	stop.tv_sec -= start.tv_sec;
	stop.tv_usec -= start.tv_usec;

	if(stop.tv_usec < 0)
	{
		stop.tv_sec -= 1;
		stop.tv_usec += 1000000;
	}

	if(!bad_count)
		reply("Wrote all databases in %ld.%ld seconds.", stop.tv_sec, stop.tv_usec);
	else
		reply("Wrote %d out of %d dataases in %ld.%ld seconds", count, (count + bad_count), stop.tv_sec, stop.tv_usec);

	return 1;
}

COMMAND(trigger_timer)
{
	unsigned long id;
	struct dict *timers;

	if(!aredigits(argv[1]))
	{
		reply("The timer id needs to be a positive integral number.");
		return 0;
	}

	errno = 0;
	id = strtoul(argv[1], NULL, 10);
	if(id == ULONG_MAX && errno == ERANGE)
	{
		reply("The timer ID needs to be positive and below %lu.", ULONG_MAX);
		return 0;
	}

	timers = timer_dict();
	dict_iter(node, timers)
	{
		struct timer *timer = node->data;
		if(timer->id == id)
		{
			timer->time = now;
			reply("Triggering timer $b%lu$b.", id);
			return 1;
		}
	}

	reply("There is no timer with the ID $b%lu$b.", id);
	return 0;
}

static void exec_sock_read(struct sock *sock, char *buf, size_t len)
{
	assert(sock->ctx);
	buf[len] = '\0';
	irc_send("%s %s :%s", (IsChannelName(sock->ctx) ? "PRIVMSG" : "NOTICE"), (const char *)sock->ctx, buf);
}

static void exec_sock_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR || event == EV_HANGUP)
	{
		if(sock->read_buf_used)
			exec_sock_read(sock, sock->read_buf, sock->read_buf_used);
		free(sock->ctx);
		sock->ctx = NULL;
	}
}

COMMAND(exec)
{
	char *args[4];
	struct sock *sock;
	const char *target = channel ? channel->name : src->nick;

	if(!(sock = sock_create(SOCK_EXEC, exec_sock_event, exec_sock_read)))
		return 0;

	args[0] = "sh";
	args[1] = "-c";
	args[2] = untokenize(argc - 1, argv + 1, " ");
	args[3] = NULL;

	irc_send("%s %s :Executing: %s", (IsChannelName(target) ? "PRIVMSG" : "NOTICE"), target, args[2]);

	sock_exec(sock, (const char **) args);
	sock_set_readbuf(sock, 512, "\r\n");
	sock->ctx = strdup(target);
	free(args[2]);
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

static int do_backslash_arg(unsigned int start, unsigned int argc, char ***argv_ptr)
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
