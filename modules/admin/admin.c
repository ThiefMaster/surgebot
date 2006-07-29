#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "conf.h"
#include "sock.h"

MODULE_DEPENDS("commands", NULL);

struct module *this;

void module_reload_cb(const char *name, unsigned char success, unsigned int errors, void *ctx);
static void module_deps_recursive(struct irc_source *src, struct module *module, unsigned int depth);
COMMAND(rehash);
COMMAND(die);
COMMAND(module_list);
COMMAND(module_deps);
COMMAND(module_add);
COMMAND(module_del);
COMMAND(module_reload);

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
