#include "global.h"
#include "module.h"
#include "bitly.h"
#include "conf.h"
#include "timer.h"
#include "irc.h"
#include "chanuser.h"
#include "modules/http/http.h"
#include "modules/tools/tools.h"
#include "modules/commands/commands.h"

MODULE_DEPENDS("http", "tools", "commands", NULL);

struct bitly_request
{
	char *url;
	struct HTTPRequest *http;
	bitly_shortened_f *callback;
	bitly_free_ctx_f *free_f;
	void *ctx;
	int handled;
};

DECLARE_LIST(bitly_list, struct bitly_request *)
IMPLEMENT_LIST(bitly_list, struct bitly_request *)

static void bitly_timeout(void *bound, struct bitly_request *request);
static void bitly_finished(struct bitly_request *request, const char *url);
static void bitly_conf_reload();
static struct bitly_request *bitly_find_http(struct HTTPRequest *http);
static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event);
static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len);

COMMAND(bitly);

static struct module *this;
static struct bitly_list *requests;

static struct {
	const char *api_user;
	const char *api_key;
	unsigned int timeout;
} bitly_conf;

MODULE_INIT
{
	this = self;
	requests = bitly_list_create();
	reg_conf_reload_func(bitly_conf_reload);
	bitly_conf_reload();

	DEFINE_COMMAND(self, "bitly", bitly, 2, 0, "group(admins)");
}

MODULE_FINI
{
	timer_del_boundname(this, "request_timeout");
	unreg_conf_reload_func(bitly_conf_reload);

	for(unsigned int i = 0; i < requests->count; i++)
	{
		struct bitly_request *request = requests->data[i];
		if(!request->handled)
			bitly_finished(request, NULL);
		HTTPRequest_cancel(request->http);
		free(request->url);
		free(request);
	}

	bitly_list_free(requests);
}

static void bitly_conf_reload()
{
	char *str;

	str = conf_get("bitly/api_user", DB_STRING);
	bitly_conf.api_user = str ? str : "";

	str = conf_get("bitly/api_key", DB_STRING);
	bitly_conf.api_key = str ? str : "";

	str = conf_get("bitly/timeout", DB_STRING);
	bitly_conf.timeout = str ? atoi(str) : 5;
}

void bitly_shorten(const char *url, bitly_shortened_f *callback, void *ctx, bitly_free_ctx_f *free_f)
{
	char urlbuf[1024], encoded_buf[1024], *encoded_url;
	// Forbidden chars that must not appear in an url
	static const char *forbidden_url_chars = "!*'();:@&=+$,/?#[] ";

	struct bitly_request *request = malloc(sizeof(struct bitly_request));
	memset(request, 0, sizeof(struct bitly_request));

	// See if any forbidden chars appear in the url
	if(strcspn(url, forbidden_url_chars) < strlen(url))
	{
		// and encode them
		encoded_url = urlencode(url);
		strncpy(encoded_buf, encoded_url, sizeof(encoded_buf));
		free(encoded_url);
	}
	else
		strncpy(encoded_buf, url, sizeof(encoded_buf));

	snprintf(urlbuf, sizeof(urlbuf), "http://api.bit.ly/v3/shorten?login=%s&apiKey=%s&format=txt&longUrl=%s", bitly_conf.api_user, bitly_conf.api_key, encoded_buf);

	request->url = strdup(url);
	request->ctx = ctx;
	request->callback = callback;
	request->free_f = free_f;
	request->http = HTTPRequest_create(urlbuf, event_func, read_func);
	HTTPRequest_connect(request->http);
	if(bitly_conf.timeout > 0)
		timer_add(this, "request_timeout", now + bitly_conf.timeout, (timer_f *)bitly_timeout, request, 0, 0);

	bitly_list_add(requests, request);
}

static void bitly_timeout(void *bound, struct bitly_request *request)
{
	if(request->ctx && request->free_f)
		request->free_f(request->ctx);
	
	bitly_finished(request, NULL);
	HTTPRequest_cancel(request->http);
	bitly_list_del(requests, request);
	free(request->url);
	free(request);
}

static void bitly_finished(struct bitly_request *request, const char *url)
{
	request->handled = 1;
	timer_del(this, "request_timeout", 0, NULL, request, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);

	if(url)
		request->callback(url, 1, request->ctx);
	else
		request->callback(request->url, 0, request->ctx);
}

static struct bitly_request *bitly_find_http(struct HTTPRequest *http)
{
	for(unsigned int i = 0; i < requests->count; i++)
	{
		struct bitly_request *request = requests->data[i];
		if(request->http == http)
			return request;
	}

	return NULL;
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	struct bitly_request *request = bitly_find_http(http);

	if(http->status != 200)
		log_append(LOG_WARNING, "bit.ly reported error %u: %s", http->status, buf);

	bitly_finished(request, (http->status == 200) ? buf : NULL);
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	struct bitly_request *request = bitly_find_http(http);
	if(!request)
		return;
	if(!request->handled)
		bitly_finished(request, NULL);

	bitly_list_del(requests, request);
	free(request->url);
	free(request);
}

void bitly_shortened_callback(const char *url, int success, void *ctx)
{
	char *nick = ctx;
	
	if(success == 0)
		irc_send("NOTICE %s :Your URL could not be shortened, some error seems to have occurred.", nick);
	else
		irc_send("NOTICE %s :Your URL has been shortened to %s", nick, url);
}

COMMAND(bitly)
{
	bitly_shorten(argv[1], bitly_shortened_callback, strdup(src->nick), free);
	return 1;
}

