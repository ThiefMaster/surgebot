#include "global.h"
#include "module.h"
#include "irc.h"
#include "conf.h"
#include "tools.h"
#include "irc_handler.h"
#include "ptrlist.h"
#include "database.h"
#include "modules/tools/tools.h"

MODULE_DEPENDS("tools", NULL);

enum ctcp_type
{
	CTCP_STRING,
	CTCP_STRINGLIST
};

struct ctcp
{
	char *name;
	enum ctcp_type type;
	union {
		char *string;
		struct stringlist *slist;
	} reply;
};

struct ptrlist *ctcp_replies;
static void ctcp_free(struct ctcp *ctcp);
static void ctcp_reply(const char *target, const char *request, const char *reply);
static void ctcp_conf_reload();

IRC_HANDLER(privmsg);

MODULE_INIT
{
	ctcp_replies = ptrlist_create();
	ptrlist_set_free_func(ctcp_replies, (ptrlist_free_f*)ctcp_free);

	reg_irc_handler("PRIVMSG", privmsg);
	reg_conf_reload_func(ctcp_conf_reload);

	ctcp_conf_reload();
}

MODULE_FINI
{
	unreg_conf_reload_func(ctcp_conf_reload);
	unreg_irc_handler("PRIVMSG", privmsg);

	ptrlist_free(ctcp_replies);
}

static void ctcp_free(struct ctcp *ctcp)
{
	if(ctcp->type == CTCP_STRING)
		free(ctcp->reply.string);
	else
		stringlist_free(ctcp->reply.slist);

	free(ctcp->name);
	free(ctcp);
}

static void ctcp_conf_reload()
{
	struct db_node *db_node = conf_node("ctcp");
	ptrlist_clear(ctcp_replies);

	if(!db_node)
	{
		log_append(LOG_ERROR, "Config node \"ctcp\" has not been configured.");
		return;
	}

	if(db_node->type != DB_OBJECT)
	{
		log_append(LOG_ERROR, "Config path \"ctcp\" is not an object!");
		return;
	}

	dict_iter(node, db_node->data.object)
	{
		struct db_node *child = node->data;
		struct ctcp *ctcp;

		if(child->type != DB_STRING && child->type != DB_STRINGLIST)
		{
			log_append(LOG_INFO, "CTCP Config key \"%s\" is neither string nor stringlist.", node->key);
			continue;
		}

		ctcp = malloc(sizeof(struct ctcp));
		ctcp->name = trim(strtoupper(strdup(node->key)));
		if(child->type == DB_STRING)
		{
			ctcp->type = CTCP_STRING;
			ctcp->reply.string = strdup(child->data.string);
		}
		else // if(child->type == DB_STRINGLIST)
		{
			ctcp->type = CTCP_STRINGLIST;
			ctcp->reply.slist = stringlist_copy(child->data.slist);
		}
		ptrlist_add(ctcp_replies, 0, ctcp);
	}
}

static void ctcp_reply(const char *target, const char *request, const char *reply)
{
	irc_send("NOTICE %s :\001%s %s\001", target, request, reply);
}

IRC_HANDLER(privmsg)
{
	assert(argc > 2);

	if(!src || strcasecmp(argv[1], bot.nickname))
		return;

	// Is this a CTCP request (first character == ascii 1)?
	if(*argv[2] != '\001')
		return;

	for(int i = 0; i < ctcp_replies->count; i++)
	{
		struct ctcp *ctcp = ctcp_replies->data[i]->ptr;
		int len = strlen(ctcp->name);

		if(!strncasecmp(argv[2] + 1, ctcp->name, len) && (!ctcp->name[len] || ctcp->name[len] == '\001' || ctcp->name[len] == ' '))
		{
			if(ctcp->type == CTCP_STRING)
				ctcp_reply(src->nick, ctcp->name, ctcp->reply.string);
			else
			{
				for(int i = 0; i < ctcp->reply.slist->count; i++)
					ctcp_reply(src->nick, ctcp->name, ctcp->reply.slist->data[i]);
			}
			return;
		}
	}
}
