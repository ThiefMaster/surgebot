#include "global.h"
#include "module.h"
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "modules/tools/tools.h"
#include "modules/http/http.h"
#include "chanuser.h"
#include "dict.h"
#include "tools.h"
#include "irc.h"
#include "conf.h"

MODULE_DEPENDS("tools", "commands", "http", "chanreg", "help", NULL);

static struct
{
	char *url;
} google_conf;

struct google_object
{
	char *id;
	char *channel;
	char *nick;
	char *request;
	struct HTTPRequest *http;
};

static void google_object_free(struct google_object *);

static struct google_object *google_object_find(struct HTTPRequest *http);
static void google_msg(struct google_object *, const char *format, ...);
static void google_error(struct google_object *, const char *format, ...);

static void read_func(struct HTTPRequest *, const char *, unsigned int);
static void event_func(struct HTTPRequest *, enum HTTPRequest_event);

static void google_readconf();

int replycount_validator(struct chanreg *reg, struct irc_source *src, const char *value);

static struct dict *objects;
static struct chanreg_module *cmod;

COMMAND(google);

MODULE_INIT
{
	cmod = chanreg_module_reg("Google", 0, NULL, NULL, NULL, NULL, NULL);
	chanreg_module_setting_reg(cmod, "MinAccess", "1", access_validator, NULL, access_encoder);
	chanreg_module_setting_reg(cmod, "PubReply", "1", boolean_validator, boolean_formatter_onoff, boolean_encoder);
	chanreg_module_setting_reg(cmod, "ReplyCount", "2", replycount_validator, NULL, NULL);

	DEFINE_COMMAND(self, "google", google, 2, 0, "true");

	objects = dict_create();
	dict_set_free_funcs(objects, free, (dict_free_f*)google_object_free);

	reg_conf_reload_func(google_readconf);

	memset(&google_conf, 0, sizeof(google_conf));
	google_readconf();

	help_load(self, "google.help");
}

MODULE_FINI
{
	unreg_conf_reload_func(google_readconf);
	dict_free(objects);

	if(google_conf.url)
		free(google_conf.url);

	chanreg_module_unreg(cmod);
}

COMMAND(google)
{
	struct HTTPRequest *http;
	struct google_object *obj = malloc(sizeof(struct google_object));
	char *request_encoded, *request;
	struct chanreg_user *creg_user;
	int level;

	if(channel)
	{
		char *str;
		CHANREG_MODULE_COMMAND(cmod);

		if((level = chanreg_setting_get_int(reg, cmod, "MinAccess")) > 0)
		{
			if(!user->account || ((creg_user = chanreg_user_find(reg, user->account->name)) && creg_user->level < level))
			{
				reply("You do not have enough access to run this command.");
				return 0;
			}
		}

		if(chanreg_setting_get_bool(reg, cmod, "PubReply"))
			obj->channel = strdup(channelname);
		else
			obj->channel = NULL;
	}
	else
		obj->channel = NULL;

	obj->nick = strdup(src->nick);
	obj->request = untokenize(argc - 1, argv + 1, " ");

	request = malloc(strlen(obj->request) * 3 + strlen(google_conf.url) - 2 + 1);
	request_encoded = urlencode(obj->request);
	sprintf(request, google_conf.url, request_encoded);

	http = HTTPRequest_create(request, event_func, read_func);

	obj->http = http;
	obj->id = strdup(http->id);

	debug("Created new google object using HTTP request %s (Query: %s)", http->id, obj->request);

	dict_insert(objects, obj->id, obj);
	HTTPRequest_connect(http);

	free(request_encoded);
	free(request);
	return 0;
}

static void google_object_free(struct google_object *obj)
{
	if(obj->channel)
		free(obj->channel);
	free(obj->nick);
	free(obj->request);
	free(obj);
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	/* If Google finds results, each result will be enclosed in <h2> and </h2>.
	 * If this request was handled by the calculator (i.e. the request 5+2)
	 * there is no link within this h2, as opposed to a "normal" result
	 * from Google including a link to the target site.
	 *
	 * To be more precise, the opening <h2> has the exact form
	 * <h2 class=r> hence why I'll be using its length = 12
	 */

	const char *tmp;
	char *tmp2, *tmp3, *tmp4, *result, *link, *read_buf;
	struct google_object *obj;
	int i; // Counting variable to print out a certain amount of results
	long count_setting_value, i_max_results = 2;
	const char *sz_max_results;
	struct chanreg *reg;

	obj = google_object_find(http);
	assert(obj);

	if(obj->channel)
	{
		reg = chanreg_find(obj->channel);
		assert(reg);

		sz_max_results = chanreg_setting_get(reg, cmod, "ReplyCount");
		if(sz_max_results && (count_setting_value = strtol(sz_max_results, NULL, 10)))
			i_max_results = count_setting_value;
	}

	i = 0;

	if((tmp = strstr(buf, "<h2 class=r")))
	{
		// Find end of string
		char *tmp2 = strstr(tmp, "</h2");
		if(!tmp2)
		{
			log_append(LOG_ERROR, "(Google Request %s) Invalid Google calculator Response, missing closing h2-tag", obj->id);
			google_error(obj, NULL);
			return;
		}

		read_buf = strndup(tmp, tmp2 - tmp);
		result = html_decode(strip_html_tags(read_buf));
		google_msg(obj, "[$b%s$b] %s", obj->nick, result);
		free(read_buf);
		return;
	}

	tmp = buf;
	while(i < i_max_results && (tmp = strstr(tmp, "<h3 class=r>")))
	{
		tmp += 12;

		// Find end of match
		if(!(tmp2 = strstr(tmp, "</h3")))
		{
			log_append(LOG_ERROR, "(Google Request %s) Invalid Google Response, couldn't find closing h3-tag", obj->id);
			google_error(obj, NULL);
			return;
		}

		i++;

		// Find start of link
		if((tmp3 = strstr(tmp, "<a")) && (tmp3 < tmp2))
		{
			// First result
			if(i == 1)
			{
				char *request = str_replace(obj->request, "$", "$$", 1);
				google_msg(obj, "[$b%s$b] Searching Google for $u%s$u:", obj->nick, request);
				free(request);
			}

			// Point to start of link target
			if(!(tmp3 = strstr(tmp3, "href=\"")) || (tmp3 > tmp2))
			{
				log_append(LOG_ERROR, "(Google Request %s) Could not find link for result %d.", obj->id, i);
				google_error(obj, NULL);
				return;
			}

			tmp3 += 6; // strlen("href=\"")
			// Point to end of link target = closing quotes
			if(!(tmp4 = strstr(tmp3, "\"")))
			{
				log_append(LOG_ERROR, "(Google Request %s) Could not find end of link for result %d.", obj->id, i);
				google_error(obj, NULL);
				return;
			}

			// If there is a link within the h2, treat as "normal" result
			result = html_decode(strip_html_tags(strndup(tmp, tmp2 - tmp)));
			link = strndup(tmp3, tmp4 - tmp3);
			// Does the link really need to be decoded?
			//link = html_decode(link);
			google_msg(obj, "%d: $b%s$b (%s)", i, result, link);
			free(link);
			free(result);
		}
	}

	if(!i)
		google_msg(obj, "No results found.");
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	/* event will be either H_EV_HANGUP or H_EV_ERROR where in both cases,
	 * the object needs to be deleted
	 */

	struct google_object *obj = google_object_find(http);
	assert(obj);
	dict_delete(objects, obj->id);
}

static struct google_object *google_object_find(struct HTTPRequest *http)
{
	dict_iter(node, objects)
	{
		if(((struct google_object*)node->data)->http == http)
			return node->data;
	}

	return NULL;
}

static void google_msg(struct google_object *obj, const char *format, ...)
{
	va_list va;
	char str[MAXLEN];

	va_start(va, format);
	vsnprintf(str, sizeof(str), format, va);
	va_end(va);

	if(obj->channel)
		irc_send("PRIVMSG %s :%s", obj->channel, str);
	else
		irc_send("NOTICE %s :%s", obj->nick, str);
}

static void google_error(struct google_object *obj, const char *format, ...)
{
	va_list va;
	char str[MAXLEN];
	char *request = str_replace(obj->request, "$", "$$", 1);

	if(format)
	{
		va_start(va, format);
		vsnprintf(str, sizeof(str), format, va);
		va_end(va);

		google_msg(obj, "[$b%s$b] Google Error while searching \"%s\": %s", obj->nick, request, str);
	}
	else
		google_msg(obj, "[$b%s$b] An error has occurred while searching \"%s\".", obj->nick, request);

	free(request);
}

static void google_readconf()
{
	char *str, *tmp, *str2;
	size_t len;

	// Default values to use in the config
	const char *default_url = "www.google.com/search?q=%s";

	tmp = conf_get("google/url", DB_STRING);
	if(!tmp)
	{
		log_append(LOG_INFO, "Could not read path 'google/url' from configuration file, defaulting to %s.", default_url);

		if(google_conf.url)
			free(google_conf.url);

		google_conf.url = strdup(default_url);
	}
	else
	{
		// An url was given, let's validate it
		unsigned int str_count = 0, percent_count = 0;

		if(strcspn(tmp, " \t\n\r") != strlen(tmp))
		{
			log_append(LOG_ERROR, "Configured Google URL to connect to may not contain spaces. Escape them using %%20 or \"+\".");
			log_append(LOG_INFO, "Setting default google URL: %s", default_url);

			if(google_conf.url)
				free(google_conf.url);

			google_conf.url = strdup(default_url);
		}

		str = tmp;
		while((str = strchr(str, '%')))
		{
			if(str[1] == 's')
			{
				if(str_count)
				{
					log_append(LOG_INFO, "Google url to connect to contains more than one string placeholder (%%s), defaulting to %s.", default_url);

					if(google_conf.url)
						free(google_conf.url);

					google_conf.url = strdup(default_url);
					break;
				}

				str++, str_count++;
			}

			else if(str[1] == '%')
				str += 2;

			else
				percent_count++, str++;
		}

		do
		{
			if(!str_count)
			{
				log_append(LOG_INFO, "Missing string placeholder (%%s) in config path 'google/url', defaulting to %s.", default_url);
				if(google_conf.url)
					free(google_conf.url);

				google_conf.url = strdup(default_url);
				break;
			}

			// Valid url, escape % signs
			if(!str)
			{
				char *pos;

				if(google_conf.url)
					free(google_conf.url);

				// Preceding http:// or https://?
				str = tmp;

				if(!strncasecmp(str, "http://", 7))
					str += 7;
				else if(!strncasecmp(str, "https://", 8))
					str += 8;

				google_conf.url = malloc(strlen(str) + percent_count + 1);
				google_conf.url[0] = '\0';

				pos = str;
				while((str2 = strstr(str, "%")))
				{
					if(str2[1] == '%' || str2[1] == 's')
					{
						str = str2 + 2;
						continue;
					}
					else
					{
						// Copy string up to percent sign
						strncat(google_conf.url, pos, (str2 - pos));
						// Append additional escaping percent sign
						strcat(google_conf.url, "%");
						pos = str = str2 + 1;
					}
				}
				// Append end of string
				strcat(google_conf.url, pos);
			}
		}
		while(0);
	}
}

int replycount_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	long replycount = strtol(value, NULL, 10);
	int ret = replycount > 0 && replycount <= 10;

	if(!ret)
		reply("The replycount has to be a valid number between 1 and 10.");

	return ret;
}
