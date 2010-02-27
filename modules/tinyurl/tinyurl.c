#define _GNU_SOURCE
#include <time.h>
#include "module.h"
#include "global.h"
#include "irc_handler.h"
#include "irc.h"
#include "dict.h"
#include "string.h"
#include "stringlist.h"
#include "timer.h"
#include "modules/http/http.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"

MODULE_DEPENDS("http", "chanreg", "help", NULL);

struct {
	const char *domain;
	const char *servicename;
} tinyurl_services[] = {
	{ "bit.ly", "bit.ly" },
	{ "i8t.de", "i8t.de" },
	{ "tiny.cc", "tiny.cc" },
	{ "tinyurl.com", "TinyURL" },
	{ "to.", "to."}
};

struct tinyurl {
	char *alias;
	char *target;
	unsigned int service_index;

	struct stringlist *channels;
	struct HTTPRequest *http;

	time_t fetched;
};

static struct tinyurl *tinyurl_create(char *channel, const char *alias, unsigned int server_index);
static void tinyurl_free(struct tinyurl *tinyurl);
static void tinyurl_send(struct tinyurl *tinyurl);

static inline void tinyurl_timer_add();
static inline void tinyurl_timer_del();
static void tinyurl_timer(void *bound, void *data);

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event);
static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len);

static struct chanreg_module *cmod;
static struct dict *tinyurls;
IRC_HANDLER(privmsg);

MODULE_INIT
{
	cmod = chanreg_module_reg("TinyURL", 0, NULL, NULL, NULL, NULL, NULL);
	reg_irc_handler("PRIVMSG", privmsg);

	tinyurls = dict_create();
	dict_set_free_funcs(tinyurls, NULL, (dict_free_f*)tinyurl_free);

        help_load(self, "tinyurl.help");
}

MODULE_FINI
{
	dict_free(tinyurls);
	unreg_irc_handler("PRIVMSG", privmsg);
	chanreg_module_unreg(cmod);
}

IRC_HANDLER(privmsg)
{
	char *tmp, *tmp2, *alias;
	struct chanreg *reg;
	struct tinyurl *tinyurl;

	// Only work messages sent in public
	if(!IsChannelName(argv[1]))
		return;

	// Channel registered and module enabled?
	if(!(reg = chanreg_find(argv[1])) || (stringlist_find(reg->active_modules, cmod->name) == -1))
		return;

	// Iterate through all services to see if we need to parse this
	for(unsigned int i = 0; i < ArraySize(tinyurl_services); i++) {
		if(!(tmp = strcasestr(argv[2], tinyurl_services[i].domain)))
			continue;

		// Find introducing slash
		if(!(tmp2 = strchr(tmp, '/')))
			continue;

		// No space in front of the slash
		if((tmp = strchr(tmp, ' ')) && tmp < tmp2)
			continue;

		// Point tmp2 to the beginning of the alias
		tmp2++;
		// There should be an alias
		if(!*tmp2)
			continue;

		alias = strndup(tmp2, tmp - tmp2);
		tinyurl = tinyurl_create(argv[1], alias, i);
		free(alias);
	}
}

static struct tinyurl *tinyurl_create(char *channel, const char *alias, unsigned int service_index)
{
	struct tinyurl *tinyurl;
	char *request;

	if((tinyurl = dict_find(tinyurls, alias)))
	{
		if(stringlist_find(tinyurl->channels, channel) == -1)
			stringlist_add(tinyurl->channels, strdup(channel));

		// Only send response when result was already fetched
		if(!tinyurl->http)
			tinyurl_send(tinyurl);
		return tinyurl;
	}

	tinyurl = malloc(sizeof(struct tinyurl));
	memset(tinyurl, 0, sizeof(struct tinyurl));

	tinyurl->channels = stringlist_create();
	tinyurl->alias = strdup(alias);
	tinyurl->fetched = now;
	tinyurl->service_index = service_index;

	request = malloc(12 /* strlen("tinyurl.com/") */ + strlen(alias) + 1);
	sprintf(request, "%s/%s", tinyurl_services[service_index].domain, alias);
	tinyurl->http = HTTPRequest_create(request, event_func, read_func);
	tinyurl->http->forward_request = 1;
	tinyurl->http->forward_request_foreign = 0;
	HTTPRequest_connect(tinyurl->http);
	free(request);

	stringlist_add(tinyurl->channels, strdup(channel));

	dict_insert(tinyurls, tinyurl->alias, tinyurl);
	return tinyurl;
}

static void tinyurl_free(struct tinyurl *tinyurl)
{
	stringlist_free(tinyurl->channels);
	free(tinyurl->alias);
	free(tinyurl);
}

static void tinyurl_send(struct tinyurl *tinyurl)
{
	while(tinyurl->channels->count)
	{
		irc_send("PRIVMSG %s :%s [$b%s$b]: %s", tinyurl->channels->data[0], tinyurl_services[tinyurl->service_index].servicename, tinyurl->alias, tinyurl->target);
		stringlist_del(tinyurl->channels, 0);
	}
}

static void tinyurl_timer(void *bound, void *data)
{
	// See if any requests exceeded the maximum cache duration
	dict_iter(node, tinyurls)
	{
		struct tinyurl *tinyurl = node->data;
		if(tinyurl->fetched <= (now - 3600))
			dict_delete(tinyurls, tinyurl->alias);
	}
	tinyurl_timer_add();
}

static inline void tinyurl_timer_add()
{
	tinyurl_timer_del();
	timer_add(NULL, "tinyurl_cleanup", now + 600, tinyurl_timer, NULL, 0, 0);
}

static inline void tinyurl_timer_del()
{
	timer_del_boundname(NULL, "tinyurl_cleanup");
}

static struct tinyurl *tinyurl_find_http(struct HTTPRequest *http)
{
	dict_iter(node, tinyurls)
	{
		struct tinyurl *tinyurl = node->data;
		if(tinyurl->http == http)
			return tinyurl;
	}
	return NULL;
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	struct tinyurl *tinyurl;
	char *location;

	assert((tinyurl = tinyurl_find_http(http)));
	if(http->status == 200)
	{
		for(unsigned int i = 0; i < tinyurl->channels->count; i++)
			irc_send("PRIVMSG %s :%s [$b%s$b] does not exist.", tinyurl->channels->data[i], tinyurl_services[tinyurl->service_index].servicename, tinyurl->alias);

		return;
	}

	assert((location = HTTPRequest_get_response_header(http, "Location")));

	tinyurl->target = strdup(location);
	tinyurl_send(tinyurl);
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	struct tinyurl *tinyurl;
	assert((tinyurl = tinyurl_find_http(http)));
	if(!tinyurl->target)
		dict_delete(tinyurls, tinyurl->alias);
}
