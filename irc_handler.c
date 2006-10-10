#include "global.h"
#include "irc_handler.h"
#include "irc.h"
#include "chanuser.h"
#include "chanuser_irc.h"

IMPLEMENT_LIST(irc_handler_list, irc_handler_f *)
IMPLEMENT_LIST(connected_func_list, connected_f *)

static struct dict *irc_handlers;
static struct connected_func_list *connected_funcs;

static void reg_default_handlers();

void irc_handler_init()
{
	irc_handlers = dict_create();
	dict_set_free_funcs(irc_handlers, free, (dict_free_f*) irc_handler_list_free);
	connected_funcs = connected_func_list_create();

	reg_default_handlers();
}

void irc_handler_fini()
{
	connected_func_list_free(connected_funcs);
	dict_free(irc_handlers);
}

void _reg_irc_handler(const char *cmd, irc_handler_f *func)
{
	struct irc_handler_list *list;
	debug("Adding irc handler for %s: %p", cmd, func);
	if((list = dict_find(irc_handlers, cmd)) == NULL) // no handler list -> create one
	{
		list = irc_handler_list_create();
		dict_insert(irc_handlers, strdup(cmd), list);
	}

	irc_handler_list_add(list, func);
}

void _unreg_irc_handler(const char *cmd, irc_handler_f *func)
{
	struct irc_handler_list *list;
	debug("Removing irc handler for %s: %p", cmd, func);
	if((list = dict_find(irc_handlers, cmd)) == NULL) // no handler list -> nothing to delete
		return;

	irc_handler_list_del(list, func);
}

void reg_connected_func(connected_f *func)
{
	connected_func_list_add(connected_funcs, func);
}

void unreg_connected_func(connected_f *func)
{
	connected_func_list_del(connected_funcs, func);
}

void irc_handle_msg(int argc, char **argv, struct irc_source *src, const char *raw_line)
{
	struct irc_handler_list *list;
	irc_handler_f *func;
	int i;

#ifdef IRC_HANDLER_DEBUG
	if(src)
		debug("src: %s!%s@%s", src->nick, src->ident, src->host);

	debug("argc: %d", argc);
	for(i = 0; i < argc; i++)
		debug("argv[%d]: %s", i, argv[i]);
#endif

	if(chanuser_irc_handler(argc, argv, src, raw_line) == -1) // message delayed due to burst; don't continue here
		return;

	if((list = dict_find(irc_handlers, argv[0])) != NULL && list->count > 0)
	{
		for(i = 0; i < list->count; i++)
		{
			func = list->data[i];
			func(argc, argv, src);
		}
	}
}

IRC_HANDLER(ping)
{
	assert(argc > 1);
	irc_send_fast("PONG :%s", argv[1]);
}

IRC_HANDLER(num_welcome)
{
	log_append(LOG_INFO, "Successfully logged in to the irc server");
	bot.server_tries = 0;

	assert(argc > 1);
	assert(bot.nickname);

	if(strcmp(bot.nickname, argv[1]))
	{
		debug("Actual nickname %s does not match initial nickname %s", argv[1], bot.nickname);
		free(bot.nickname);
		bot.nickname = strdup(argv[1]);
	}

	irc_send("WHOIS %s", bot.nickname);
}

IRC_HANDLER(num_myinfo)
{
	assert(argc > 2);
	if(bot.server_name)
		free(bot.server_name);
	bot.server_name = strdup(argv[2]);
	log_append(LOG_INFO, "We are connected to %s", argv[2]);
}

IRC_HANDLER(num_whoisuser)
{
	assert(argc > 6);

	if(!strcmp(argv[2], bot.nickname)) // own whois information
	{
		if(strcmp(bot.username, argv[3])) // username does not match -> change
		{
			debug("Own username does not match; changing from %s to %s", bot.username, argv[3]);
			free(bot.username);
			bot.username = strdup(argv[3]);
		}

		if(bot.hostname == NULL || strcmp(bot.hostname, argv[4])) // hostname does not match -> change
		{
			debug("Own hostname does not match; changing from %s to %s", bot.hostname, argv[4]);
			if(bot.hostname)
				free(bot.hostname);
			bot.hostname = strdup(argv[4]);
		}

		if(strcmp(bot.realname, argv[6])) // realname does not match -> change
		{
			debug("Own realname does not match; changing from %s to %s", bot.realname, argv[6]);
			free(bot.realname);
			bot.realname = strdup(argv[6]);
		}

		// First time after connect? Then let modules know that we are connected.
		if(!bot.ready)
		{
			bot.ready = 1;
			for(int i = 0; i < connected_funcs->count; i++)
			{
				connected_funcs->data[i]();
			}
		}
	}
}

IRC_HANDLER(num_hosthidden)
{
	assert(argc > 2);
	assert(!strcasecmp(argv[1], bot.nickname));

	debug("Changing own (fake)host from %s to %s", bot.hostname, argv[2]);
	if(bot.hostname)
		free(bot.hostname);
	bot.hostname = strdup(argv[2]);
}

static void reg_default_handlers()
{
	reg_irc_handler("PING", ping);
	reg_irc_handler("001", num_welcome);
	reg_irc_handler("004", num_myinfo);
	reg_irc_handler("311", num_whoisuser);
	reg_irc_handler("396", num_hosthidden);
}
