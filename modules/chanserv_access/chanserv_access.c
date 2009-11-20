#include "global.h"
#include "chanserv_access.h"
#include "chanuser.h"
#include "module.h"
#include "ptrlist.h"
#include "modules/commands/commands.h"
#include "modules/db/db.h"
#include "modules/tools/tools.h"
#include "modules/srvx/srvx.h"

MODULE_DEPENDS("commands", "tools", "srvx", NULL);

struct chanserv_access_request
{
	char *channel;
	char *nick;
	void *ctx;

	int access; // 0..500 or -1 if the bot couldn't check the access

	chanserv_access_f *callback;
};

static void chanserv_access_request_free(struct chanserv_access_request *req);
static void chanserv_access_response(struct srvx_request *r, struct chanserv_access_request *req);

static struct module *this;
static struct ptrlist *chanserv_access_requests;

MODULE_INIT
{
	this = self;

	chanserv_access_requests = ptrlist_create();
	ptrlist_set_free_func(chanserv_access_requests, (ptrlist_free_f *)chanserv_access_request_free);
}

MODULE_FINI
{
	ptrlist_free(chanserv_access_requests);
}

static void chanserv_access_request_free(struct chanserv_access_request *req)
{
	free(req->channel);
	free(req->nick);
	free(req);
}

void chanserv_get_access_callback(const char *channel, const char *nick, chanserv_access_f *func, void *ctx)
{
	struct chanserv_access_request *req = malloc(sizeof(struct chanserv_access_request));
	memset(req, 0, sizeof(struct chanserv_access_request));

	req->channel = strdup(channel);
	req->nick = strdup(nick);
	req->callback = func;
	req->ctx = ctx;
	req->access = -2;

	ptrlist_add(chanserv_access_requests, 0, req);
	srvx_send_ctx_noqserver((srvx_response_f *)chanserv_access_response, req, 0, "ChanServ %s ACCESS %s", req->channel, req->nick);
}

static void chanserv_access_response(struct srvx_request *r, struct chanserv_access_request *req)
{
	if(!r)
	{
		req->callback(req->channel, req->nick, -1, req->ctx);
		ptrlist_del_ptr(chanserv_access_requests, req);
		return;
	}

	for(unsigned int ii = 0; ii < r->count; ii++)
	{
		struct srvx_response_line *line = r->lines[ii];
		char *vec[8];
		int cnt;
		char *dup;

		strip_codes(line->msg);
		dup = strdupa(line->msg);

		cnt = tokenize(line->msg, vec, ArraySize(vec), ' ', 0);

		// "ThiefMaster (ThiefMaster) lacks access to #ircops."
		// "ThiefMaster (ThiefMaster) lacks access to #ircops but has security override enabled."
		if(cnt >= 6 && !strncmp(dup + (vec[2] - vec[0]), "lacks access to", 15))
		{
			debug("Access %s @ %s: 0", req->nick, req->channel);
			assert(!strcasecmp(vec[0], req->nick));
			if(cnt == 6) // No security override -> get rid of dot
				vec[5][strlen(vec[5]) - 1] = '\0';
			assert(!strcasecmp(vec[5], req->channel));
			req->access = 0;
			break;
		}
		// ThiefMaster (ThiefMaster) has access 500 in #ircops and has security override enabled.
		// ThiefMaster (ThiefMaster) has access 500 in #ircops.
		else if(cnt >= 7 && !strncmp(dup + (vec[2] - vec[0]), "has access", 10))
		{
			debug("Access %s @ %s: %s", req->nick, req->channel, vec[4]);
			assert(!strcasecmp(vec[0], req->nick));
			if(cnt == 7) // No security override -> get rid of dot
				vec[6][strlen(vec[6]) - 1] = '\0';
			assert(!strcasecmp(vec[6], req->channel));
			req->access = atoi(vec[4]);
			break;
		}
		else if(cnt >= 5 && !strncmp(dup, "You must be in", 14))
		{
			debug("Access %s @ %s: BotNoAccess", req->nick, req->channel);
			assert(!strcasecmp(vec[4], req->channel));
			req->access = -2;
			break;
		}
		else if(!strcmp(dup, "You must provide the name of a channel that exists."))
		{
			debug("Access %s @ %s: NoSuchChannel", req->nick, req->channel);
			req->access = -3;
			break;
		}
		else if(cnt >= 4 && !strncmp(dup, "User with nick", 14))
		{
			debug("Access %s @ %s: NoSuchNick", req->nick, req->channel);
			assert(!strcasecmp(vec[3], req->nick));
			req->access = -4;
			break;
		}
	}

	req->callback(req->channel, req->nick, req->access, req->ctx);
	ptrlist_del_ptr(chanserv_access_requests, req);
}
