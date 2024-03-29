#include <stdarg.h>
#include "global.h"
#include "module.h"
#include "dict.h"
#include "sock.h"
#include "irc.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/http/http.h"
#include "modules/tools/tools.h"

MODULE_DEPENDS("chanreg", "commands", "http", "tools", NULL);

const unsigned int urbandict_request_amount = 3;

struct urbandict_request
{
	struct HTTPRequest *http;
	char *issuer;
	char *request;
	char *channel;
	enum { PRIVMSG, NOTICE } reply;

	int linecount;
	struct stringbuffer *sbuf;
};

static void read_func(struct HTTPRequest *, const char *, unsigned int);
static void event_func(struct HTTPRequest *, enum HTTPRequest_event);
static int urbandict_request_find(struct HTTPRequest *http);
static void urbandict_report(struct urbandict_request *req, const char *format,  ...);
static void urbandict_request_free(struct urbandict_request *req);

int ud_reply_validator(struct chanreg *reg, struct irc_source *src, const char *value);

static struct ptrlist *requests;
static struct chanreg_module *cmod;

COMMAND(urbandict);

MODULE_INIT
{
	cmod = chanreg_module_reg("UrbanDict", 0, NULL, NULL, NULL, NULL, NULL);
	chanreg_module_setting_reg(cmod, "MinAccess", "0", access_validator, NULL, NULL);
	chanreg_module_setting_reg(cmod, "Reply", "NOTICE", reply_validator, NULL, NULL);

	requests = ptrlist_create();
	ptrlist_set_free_func(requests, (ptrlist_free_f*)urbandict_request_free);

	DEFINE_COMMAND(self, "urbandict", urbandict, 2, 0, "true");
}

MODULE_FINI
{
	ptrlist_free(requests);
	chanreg_module_unreg(cmod);
}

COMMAND(urbandict)
{
	const char *reply = "NOTICE";

	if(channel)
	{
		unsigned long access;
		struct chanreg_user *cuser;

		CHANREG_MODULE_COMMAND(cmod);

		access = strtoul(chanreg_setting_get(reg, cmod, "MinAccess"), NULL, 10);

		if(access > 0 && (!user->account || !(cuser = chanreg_user_find(reg, user->account->name)) || cuser->level < access))
		{
			reply("You lack the minimum access required in this channel to use $b%s$b.", argv[0]);
			return 0;
		}
		reply = chanreg_setting_get(reg, cmod, "Reply");
	}

	struct urbandict_request *req;
	char *request_encoded;
	char *target;

	char *url_prefix = "http://cheops.php-4.info/urbanDict.php?search=";

	req = malloc(sizeof(struct urbandict_request));
	memset(req, 0, sizeof(struct urbandict_request));

	req->sbuf = stringbuffer_create();
	req->request = untokenize(argc - 1, argv + 1, " ");
	request_encoded = urlencode(req->request);

	req->issuer = strdup(src->nick);
	req->reply = !strcasecmp(reply, "NOTICE") ? NOTICE : PRIVMSG;
	if(req->reply == PRIVMSG)
		req->channel = strdup(channel->name);

	target = malloc(strlen(url_prefix) + strlen(request_encoded) + 1);
	sprintf(target, "%s%s", url_prefix, request_encoded);
	debug("UrbanDict: Querying %s", target);

	req->http = HTTPRequest_create(target, event_func, read_func);
	req->http->read_linewise = 1;
	HTTPRequest_connect(req->http);

	ptrlist_add(requests, 0, req);

	free(target);
	free(request_encoded);
	return 0;
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	int pos;
	struct urbandict_request *req;
	char *str;

	assert((pos = urbandict_request_find(http)) != -1);
	req = requests->data[pos]->ptr;

	if(req->linecount == -1 || req->linecount > 3)
		return;

	if(!strncasecmp(buf, "__noresults__", 13))
	{
		urbandict_report(req, "[Urban Dictionary] Could not find any results for $u%s$u", req->request);
		req->linecount = -1;
		return;
	}

	if(!strncasecmp(buf, "__error__", 9))
	{
		urbandict_report(req, "[Urban Dictionary] An error occurred while searching for $u%s$u: %s", req->request, buf + 9);
		req->linecount = -1;
		return;
	}

	if(!req->linecount)
	{
		urbandict_report(req, "Urban Dictionary [$b%s$b]:", req->request);
		req->linecount++;
		return;
	}

	if(!strcmp(buf, "__end__"))
	{
		if(req->sbuf->len)
			urbandict_report(req, "%d) %s", req->linecount, req->sbuf->string);
		return;
	}

	if(req->sbuf->len)
	{
		if(*buf == '*') // Line of new result
		{
			urbandict_report(req, "%d) %s", req->linecount++, req->sbuf->string);
			stringbuffer_flush(req->sbuf);
		}
		else
			stringbuffer_append_char(req->sbuf, ' ');
	}

	str = html_decode(trim(strdup(buf + 2)));
	stringbuffer_append_string(req->sbuf, str);
	free(str);
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	int pos;
	assert((pos = urbandict_request_find(http)) != -1);
	ptrlist_del(requests, pos, NULL);
}

static int urbandict_request_find(struct HTTPRequest *http)
{
	for(unsigned int i = 0; i < requests->count; i++)
	{
		struct urbandict_request *req = requests->data[i]->ptr;
		if(req->http == http)
			return i;
	}
	return -1;
}

static void urbandict_report(struct urbandict_request *req, const char *format,  ...)
{
	va_list va;
	char buf[501];

	va_start(va, format);
	vsnprintf(buf, sizeof(buf), format, va);

	if(req->reply == NOTICE || !req->channel)
		irc_send("NOTICE %s :%s", req->issuer, buf);
	else
		irc_send("PRIVMSG %s :%s", req->channel, buf);

	va_end(va);
}

static void urbandict_request_free(struct urbandict_request *req)
{
	stringbuffer_free(req->sbuf);
	free(req->request);
	free(req->issuer);
	free(req->channel);
	free(req);
}
