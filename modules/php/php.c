#include "global.h"
#include "module.h"
#include "irc.h"
#include "timer.h"
#include "tools.h"
#include "stringlist.h"
#include "modules/http/http.h"
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "modules/tools/tools.h"

MODULE_DEPENDS("http", "chanreg", "commands", "tools", NULL);

static const char *default_language = "en";

struct php_cache
{
	char *func_name;

	char *description;
	struct stringlist *synopsis;

	time_t added;
};

struct php_request
{
	char *target;
	char *func_name;

	struct HTTPRequest *http;
};

static void php_request_free(struct php_request *);

static void php_cache_add(const char *language, struct php_cache *);
static struct php_cache *php_cache_find(struct chanreg *, const char *func_name);
static void php_cache_free(struct php_cache *);

static struct php_request *php_request_find(struct HTTPRequest *http);
static void php_report(struct php_cache *, const char *target);

static void read_func(struct HTTPRequest *, const char *buf, unsigned int len);
static void event_func(struct HTTPRequest *, enum HTTPRequest_event);

static int cmod_lang_validator(struct chanreg *reg, struct irc_source *src, const char *value);

static void php_add_timer();
static void php_timer_func(void *bound, void *data);
static void php_del_timer();

static struct chanreg_module *cmod;
static struct dict *php_requests;
static struct dict *php_cache;

static struct module *this;

COMMAND(php);

MODULE_INIT
{
	this = self;

	cmod = chanreg_module_reg("PHP", 0, NULL, NULL, NULL, NULL, NULL);
	chanreg_module_setting_reg(cmod, "Language", default_language, cmod_lang_validator, NULL, NULL);
	DEFINE_COMMAND(self, "php", php, 2, 0, "group(admins)");

	php_requests = dict_create();
	dict_set_free_funcs(php_requests, NULL, (dict_free_f*)php_request_free);

	php_cache = dict_create();
	dict_set_free_funcs(php_cache, free, (dict_free_f*)dict_free);

	php_add_timer();
}

MODULE_FINI
{
	php_del_timer();

	chanreg_module_unreg(cmod);
	dict_free(php_requests);
	dict_free(php_cache);
}

COMMAND(php)
{
	struct php_request *php;
	struct php_cache *cache;
	char *tmp, request[MAXLEN], *func_name;
	const char *target;
	struct chanreg *myreg = NULL;
	const char *language;
	size_t spn;

	if(channel)
	{
		CHANREG_MODULE_COMMAND(cmod);
		target = channel->name;
		myreg = reg;
	}
	else
		target = src->nick;

	// Find function name
	spn = strspn(argv[1], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-1234567890");
	if(!spn)
	{
		reply("Please supply a correct function name.");
		return 0;
	}

	func_name = strndup(argv[1], spn);

	strip_codes(func_name);

	// Did we already retrieve information for this function?
	if((cache = php_cache_find(myreg, func_name)))
	{
		free(func_name);
		php_report(cache, target);
		return 0;
	}

	php = malloc(sizeof(struct php_request));

	php->target = strdup(target);
	php->func_name = func_name;

	language = myreg ? chanreg_setting_get(myreg, cmod, "Language") : default_language;

	tmp = str_replace(php->func_name, "_", "-", 1);
	snprintf(request, sizeof(request), "www.php.net/manual/%s/function.%s.php", language, tmp);
	free(tmp);
	php->http = HTTPRequest_create(request, event_func, read_func);

	HTTPRequest_add_header(php->http, "Content-language", default_language);
	HTTPRequest_connect(php->http);

	dict_insert(php_requests, php->func_name, php);
	return 0;
}

static void php_request_free(struct php_request *php)
{
	free(php->func_name);
	free(php->target);
	free(php);
}

static void php_cache_add(const char *language, struct php_cache *cache)
{
	struct dict *tmp;

	if(!(tmp = dict_find(php_cache, language)))
	{
		tmp = dict_create();
		dict_set_free_funcs(tmp, NULL, (dict_free_f*)php_cache_free);

		dict_insert(php_cache, strdup(language), tmp);
	}

	dict_insert(tmp, cache->func_name, cache);
}

static struct php_cache *php_cache_find(struct chanreg *reg, const char *func_name)
{
	const char *language;
	struct dict *cache;

	language = (reg ? chanreg_setting_get(reg, cmod, "Language") : default_language);

	if(!(cache = dict_find(php_cache, language)))
		return NULL;

	return dict_find(cache, func_name);
}

static void php_cache_free(struct php_cache *cache)
{
	free(cache->func_name);
	free(cache->description);
	stringlist_free(cache->synopsis);
	free(cache);
}

static struct php_request *php_request_find(struct HTTPRequest *http)
{
	dict_iter(node, php_requests)
	{
		struct php_request *php = node->data;
		if(php->http == http)
			return php;
	}

	return NULL;
}

static void php_report(struct php_cache *cache, const char *target)
{
	char *method = (IsChannelName(target) ? "PRIVMSG" : "NOTICE");
	irc_send("%s %s :$b%s$b - %s (http://www.php.net/%s)", method, target, cache->func_name, cache->description, cache->func_name);

	for(unsigned int i = 0; i < cache->synopsis->count; i++)
		irc_send("%s %s :$uSynopsis$u: %s", method, target, html_decode(cache->synopsis->data[i]));
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	char *tmp, *tmp2, *description, *synopsis, *syn;
	const char *language;
	struct php_cache *cache;
	struct dict_node *node;
	struct chanreg *reg;
	struct php_request *php;

	assert((php = php_request_find(http)));

	if(IsChannelName(php->target))
	{
		assert((reg = chanreg_find(php->target)));
		language = chanreg_setting_get(reg, cmod, "Language");
	}
	else
		language = default_language;

	// There shouldn't be a cache entry for this function yet
	// If there is, we will update it
	if((node = dict_find_node(php_cache, php->func_name)))
		dict_delete_node(php_cache, node);

	if(!(tmp = strstr(buf, "<p class=\"refpurpose")))
	{
		char *method = IsChannelName(php->target) ? "PRIVMSG" : "NOTICE";
		tmp = str_replace(php->func_name, "$", "$$", 1);
		tmp2 = str_replace(tmp, "_", "-", 1);
		irc_send("%s %s :$b[PHP]$b There is no function called $b%s$b. (http://www.php.net/manual-lookup.php?pattern=%s&lang=%s)", method, php->target, tmp, tmp2, language);
		free(tmp2);
		free(tmp);
		return;
	}

	if(!(tmp = strchr(tmp, '>')))
		return;

	tmp++;

	// Find dash to get to the start of the description
	if((tmp2 = strstr(tmp, "&mdash;")))
		tmp = tmp2 + 8;
	else if((tmp2 = strstr(tmp, "â~@~T"))) // Unicode representation for long dash
		tmp = tmp2 + 6;
	else
	{
		log_append(LOG_ERROR, "The HTML code from php.net seems to have changed. Could not find separating dash.");
		return;
	}

	assert((tmp2 = strstr(tmp, "</p>")));

	syn = strndup(tmp, tmp2 - tmp);
	description = str_replace(syn, "\n", " ", 1);
	free(syn);

	// Create php cache object
	cache = malloc(sizeof(struct php_cache));
	cache->synopsis = stringlist_create();

	// Now find the synopsis - there may be more than one...
	while(tmp2 && (tmp = strstr(tmp2 + 4, "<div class=\"methodsynopsis dc-description\">")))
	{
		tmp += 43; // strlen("<div class=\"methodsynopsis dc-description\">")

		if((tmp2 = strstr(tmp, "</div>")))
		{
			synopsis = strndup(tmp, tmp2 - tmp);
			syn = str_replace(synopsis, "\n", " ", 1);
			free(synopsis);
			synopsis = str_replace(syn, "$", "$$", 1);
			free(syn);

			synopsis = strip_duplicate_whitespace(strip_html_tags(synopsis));
			stringlist_add(cache->synopsis, synopsis);

			tmp2 += 6;
		}
	}

	cache->func_name	= strdup(php->func_name);
	cache->description	= strip_duplicate_whitespace(strip_html_tags(description));
	cache->added		= now;

	php_report(cache, php->target);
	php_cache_add(language, cache);
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	struct php_request *php = php_request_find(http);
	assert(php);

	// The two possible events are timeout/hangup and error
	// In both cases, the request needs to be freed
	dict_delete(php_requests, php->func_name);
}

static int cmod_lang_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	const char *old_value;

	if(!strlen(value) || (strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_") != strlen(value)))
		return 0;

	old_value = chanreg_setting_get(reg, cmod, "Language");

	// In case no more channels need this old language, free function cache for this language
	for(unsigned int i = 0; i < cmod->channels->count; i++)
	{
		struct chanreg *reg = cmod->channels->data[i];
		if(!strcasecmp(chanreg_setting_get(reg, cmod, "Language"), old_value))
			return 1;
	}

	dict_delete(php_cache, old_value);
	return 1;
}

static void php_add_timer()
{
	php_del_timer();
	timer_add(this, "PHPCleanUp", now + 600, php_timer_func, NULL, 0, 0);
}

static void php_timer_func(void *bound, void *data)
{
	// Iterate through all dicts to delete entries older than one hour
	dict_iter(node, php_cache)
	{
		struct dict *cache = node->data;
		dict_iter(node, cache)
		{
			struct php_cache *cache2 = node->data;
			if(cache2->added <= (now - 3600))
				dict_delete(cache, cache2->func_name);
		}
		if(!cache->count)
			dict_delete(php_cache, node->key);
	}

	php_add_timer();
}

static void php_del_timer()
{
	timer_del_boundname(this, "PHPCleanUp");
}
