#include "global.h"
#include "module.h"
#include "irc.h"
#include "tools.h"
#include "modules/http/http.h"
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "modules/tools/tools.h"

MODULE_DEPENDS("http", "chanreg", "commands", "tools", NULL);

// todo: expire cache after one hour

struct php_cache
{
	char *func_name;
	
	char *description;
	char *synopsis;
	
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

static struct chanreg_module *cmod;
static struct dict *php_requests;
static struct dict *php_cache;

COMMAND(php);

MODULE_INIT
{
	cmod = chanreg_module_reg("PHP", 0, NULL, NULL, NULL, NULL);
	chanreg_module_setting_reg(cmod, "Language", "en", cmod_lang_validator, NULL, NULL);
	DEFINE_COMMAND(self, "php", php, 2, 0, "group(admins)");
	
	php_requests = dict_create();
	dict_set_free_funcs(php_requests, NULL, (dict_free_f*)php_request_free);
	
	php_cache = dict_create();
	dict_set_free_funcs(php_cache, free, (dict_free_f*)dict_free);
}

MODULE_FINI
{
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
	
	if(channel)
	{
		CHANREG_COMMAND;
		target = channel->name;
		myreg = reg;
	}
	else
		target = src->nick;
	
	// Find first space in long argument
	if(!(tmp = strchr(argv[1], ' ')))
		func_name = strdup(argv[1]);
	else
		func_name = strndup(argv[1], (tmp - argv[1]));
	
	// Did we already retrieve information for this function?
	if((cache = php_cache_find(myreg, func_name)))
	{
		free(func_name);
		php_report(cache, target);
		return 1;
	}
	
	php = malloc(sizeof(struct php_request));
	
	php->target = strdup(target);
	php->func_name = func_name;
	
	language = myreg ? chanreg_setting_get(myreg, cmod, "Language") : "en";
	
	tmp = str_replace(php->func_name, "_", "-", 1);
	snprintf(request, sizeof(request), "www.php.net/manual/%s/function.%s.php", language, tmp);
	free(tmp);
	php->http = HTTPRequest_create(request, event_func, read_func);
	
	HTTPRequest_add_header(php->http, "Content-language", "en");
	HTTPRequest_connect(php->http);
	
	dict_insert(php_requests, php->func_name, php);
	return 1;
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
	
	language = (reg ? chanreg_setting_get(reg, cmod, "Language") : "en");
	
	if(!(cache = dict_find(php_cache, language)))
		return NULL;
	
	return dict_find(cache, func_name);
}

static void php_cache_free(struct php_cache *cache)
{
	free(cache->func_name);
	free(cache->description);
	free(cache->synopsis);
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
	irc_send("PRIVMSG %s :$b%s$b - %s (http://www.php.net/%s)", target, cache->func_name, cache->description, cache->func_name);
	irc_send("PRIVMSG %s :$uSynopsis$u: %s", target, cache->synopsis);
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	char *tmp, *tmp2, *description, *synopsis, *syn;
	const char *language;
	struct php_cache *cache;
	struct dict_node *node;
	unsigned char white;
	struct chanreg *reg;
	struct php_request *php;
	
	assert((php = php_request_find(http)));
	
	if(IsChannelName(php->target))
	{
		assert((reg = chanreg_find(php->target)));
		language = chanreg_setting_get(reg, cmod, "Language");
	}
	else
		language = "en";
	
	// There shouldn't be a cache entry for this function yet
	// If there is, we will update it
	if((node = dict_find_node(php_cache, php->func_name)))
		dict_delete_node(php_cache, node);
	
	if(!(tmp = strstr(buf, "<p class=\"refpurpose dc-title\">")))
	{
		tmp = str_replace(php->func_name, "$", "$$", 1);
		tmp2 = str_replace(tmp, "_", "-", 1);
		irc_send("PRIVMSG %s :$b[PHP]$b There is no function called $b%s$b. (http://www.php.net/manual-lookup.php?pattern=%s&lang=%s)", php->target, tmp, tmp2, language);
		free(tmp2);
		free(tmp);
		return;
	}
	
	tmp += 31; // strlen("<p class=\"refpurpose dc-title\">")
	
	// Find dash to get to the start of the description
	assert((tmp = strstr(tmp, "—"))); // Unicode representation for long dash
	
	tmp += 4;
	
	assert((tmp2 = strstr(tmp, "</p>")));
	
	syn = strndup(tmp, tmp2 - tmp);
	description = str_replace(syn, "\n", " ", 1);
	free(syn);
	
	// Now find the synopsis
	if(!(tmp = strstr(tmp2, "<div class=\"methodsynopsis dc-description\">")))
	{
		debug("Could not find start of synopsis for PHP function %s", php->func_name);
		free(description);
		return;
	}
	tmp += 43; // strlen("<div class=\"methodsynopsis dc-description\">")
	if(!(tmp2 = strstr(tmp, "</div>")))
	{
		debug("Could not find end of synopsis for PHP function %s", php->func_name);
		free(description);
		return;
	}
	
	synopsis = strndup(tmp, tmp2 - tmp);
	syn = str_replace(synopsis, "\n", " ", 1);
	free(synopsis);
	synopsis = str_replace(syn, "$", "$$", 1);
	free(syn);
	
	// Create php cache object
	cache = malloc(sizeof(struct php_cache));
	cache->func_name	= strdup(php->func_name);
	cache->description	= trim(strip_html_tags(description));
	cache->synopsis		= trim(strip_html_tags(synopsis));
	cache->added		= now;
	
	tmp = cache->synopsis;
	len = strlen(tmp) + 1;
	tmp2 = NULL;
	white = 0;
	// Strip multiple spaces
	while(*tmp)
	{
		if(isspace(*tmp))
		{
			if(white && !tmp2)
				tmp2 = tmp;
			
			white = 1;
			tmp++;
			continue;
		}
		else
		{
			if(tmp2)
			{
				// We need to move everything back to tmp2
				memmove(tmp2, tmp, len - (tmp - cache->synopsis));
				tmp = tmp2;
				tmp2 = NULL;
				continue;
			}
			white = 0;
			tmp++;
		}
	}
	
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
	int i;
	
	if(!strlen(value) || (strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_") != strlen(value)))
		return 0;
	
	old_value = chanreg_setting_get(reg, cmod, "Language");
	if(!strcasecmp(old_value, "en"))
		return 1;
	
	for(i = 0; i < cmod->channels->count; i++)
	{
		struct chanreg *reg = cmod->channels->data[i];
		if(!strcasecmp(chanreg_setting_get(reg, cmod, "Language"), old_value))
			return 1;
	}
	
	dict_delete(php_cache, old_value);
	return 1;
}
