#include "global.h"
#include "irc_handler.h"
#include "irc.h"

IMPLEMENT_LIST(irc_handler_list, irc_handler_f *)

static struct dict *irc_handlers;

static void reg_default_handlers();

void irc_handler_init()
{
	irc_handlers = dict_create();
	dict_set_free_funcs(irc_handlers, free, (dict_free_f*) irc_handler_list_free);

	reg_default_handlers();
}

void irc_handler_fini()
{
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

void irc_handle_msg(int argc, char **argv, struct irc_source *src)
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
}

static void reg_default_handlers()
{
	reg_irc_handler("ping", ping);
	reg_irc_handler("001", num_welcome);
}
