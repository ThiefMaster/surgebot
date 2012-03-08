#include "global.h"
#include "module.h"
#include "conf.h"
#include "modules/http/http.h"

static void radioapi_conf_reload();
static void radioapi_read_func(struct HTTPRequest *http, const char *buf, unsigned int len);

static struct module *this;

struct {
	const char *key;
	struct dict *urls;
} radioapi_conf;

MODULE_DEPENDS("http", NULL);

MODULE_INIT
{
	this = self;
	reg_conf_reload_func(radioapi_conf_reload);
	radioapi_conf_reload();
}

MODULE_FINI
{
	unreg_conf_reload_func(radioapi_conf_reload);
}

static void radioapi_conf_reload()
{
	const char *str;

	str = conf_get("radioapi/key", DB_STRING);
	radioapi_conf.key = (str && *str) ? str : NULL;
	radioapi_conf.urls = conf_get("radioapi/urls", DB_OBJECT);
}

void radioapi_call(const char *api, const char *payload)
{
	const char *url;

	if(!radioapi_conf.urls || !(url = database_fetch(radioapi_conf.urls, api, DB_STRING)))
	{
		log_append(LOG_ERROR, "Unknown API called: %s", api);
		return;
	}
	else if(!*url)
	{
		return;
	}

	struct HTTPRequest *req;
	req = HTTPRequest_create(url, NULL, radioapi_read_func);
	req->method = "POST";
	req->payload_type = "application/json";
	req->payload = strdup(payload);
	HTTPRequest_add_header(req, "X-API-Key", radioapi_conf.key);
	HTTPRequest_connect(req);
	debug("API call sent to %s: %s", api, payload);
}

static void radioapi_read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	enum log_level lvl = LOG_DEBUG;
	if(http->status != 200)
		lvl = LOG_WARNING;
	log_append(lvl, "API call finished (%u): %s", http->status, buf);
}
