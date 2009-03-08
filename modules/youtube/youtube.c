#include "global.h"
#include "module.h"
#include "modules/chanreg/chanreg.h"
#include "modules/tools/tools.h"
#include "modules/http/http.h"
#include "chanuser.h"
#include "dict.h"
#include "tools.h"
#include "irc.h"
#include "conf.h"
#include "irc_handler.h"
#include "timer.h"
#include <ctype.h>

// Duration to cache a request (Will only be checked once every 10 minutes)
static const unsigned int refresh_delay = 600;
// Amount of requests to keep
static const unsigned int max_requests = 0;

MODULE_DEPENDS("http", "tools", "chanreg", NULL);

struct youtube_target
{
	char *target;

	time_t requested;
	unsigned char responded; // Response already sent?
};

struct youtube_request
{
	char *id;
	struct dict *targets;
	char *response;

	struct HTTPRequest *http;

	// We are going to cache this request for the duration in refresh_delay
	time_t requested;
};

static struct youtube_request *youtube_request_create(const char *id, const char *target);
static void youtube_request_free(struct youtube_request *);
static void youtube_report(struct youtube_request *);

static void youtube_add_target(struct youtube_request *, const char *);
static struct youtube_request *youtube_request_find(struct HTTPRequest *);

static void read_func(struct HTTPRequest *, const char *, unsigned int);
static void event_func(struct HTTPRequest *, enum HTTPRequest_event);

static void youtube_timer(void *, void *);
static inline void youtube_timer_add();
static inline void youtube_timer_del();

static int youtube_disable_cmod(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);

extern time_t now;
static struct dict *requests;
static struct chanreg_module *cmod;

IRC_HANDLER(privmsg);

MODULE_INIT
{
	cmod = chanreg_module_reg("YouTube", 0, NULL, NULL, NULL, youtube_disable_cmod);

	reg_irc_handler("privmsg", privmsg);

	requests = dict_create();
	dict_set_free_funcs(requests, NULL, (dict_free_f*)youtube_request_free);

	if(refresh_delay > 0)
		youtube_timer_add();
}

MODULE_FINI
{
	unreg_irc_handler("privmsg", privmsg);
	dict_free(requests);

	youtube_timer_del();
	chanreg_module_unreg(cmod);
}

IRC_HANDLER(privmsg)
{
	const char *str, *tmp, *end, *arg, *cur, *slash;
	char *id;
	int i, j;
	struct chanreg *reg;

	// Only work messages sent in public
	if(!IsChannelName(argv[1]))
		return;

	if(!(reg = chanreg_find(argv[1])) || (stringlist_find(reg->active_modules, cmod->name) == -1))
		return;

	str = arg = argv[2];

next_iteration:
	while((str = strcasestr(str, "youtube.")))
	{
		tmp = cur = str;
		str += 8; // strlen("youtube.")

		// Find beginning of link (Space or beginning of line)
		while((tmp > arg) && (*(tmp - 1) != ' '))
			tmp--;

		// Find end of link (Space or end of line)
		if(!(end = strchr(str, ' ')))
			end = str + strlen(str); // Point to end of string (\0)

		// Remove http prefix
		if(!strncasecmp(tmp, "http://", 7))
			tmp += 8;
		else if(!strncasecmp(tmp, "https://", 8))
			tmp += 8;

		// See if only letters, numbers and .- are in the host
		if(!(slash = strstr(tmp, "/")) || (strspn(tmp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") < (unsigned int)(slash - tmp)))
			continue;

		// Valid subdomain? 0 or more than 1 and not more than 3 chars (4 incl. dot)
		if((i = (cur - tmp)) && ((i < 2) || (i > 4) || (tmp[i] == '.')))
			continue;

		if(i)
		{
			i--; // Remove dot from the end
			// All letters?
			for(j = 0; j < i; j++)
				if(!isalpha(tmp[j]))
					goto next_iteration;

			tmp += i + 1;
		}

		// After the subdomain incl. dot should be "youtube"
		if(strncasecmp(tmp, "youtube.", 8))
			continue;

		tmp += 8;
		// After youtube, there must be a valid ending of at least 2 and at most 4 chars
		// (there can be more, but I expect youtube to use at most 4 (.info))
		if(!(i = (slash - tmp)) || (i < 2) || (i > 4))
			continue;

		for(j = 0; j < i; j++)
			if(!isalpha(tmp[j]))
				goto next_iteration;

		tmp += i + 1;

		if(strncasecmp(tmp, "watch?v=", 8))
			continue;

		tmp += 8;

		// we finally got to the ID... Read ID which may consist of letters, numbers and .-_ and has at least 10 chars
		if((i = strspn(tmp, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")) < 10 || (i > 50))
			continue;

		id = strndup(tmp, i);
		youtube_request_create(id, argv[1]);
		free(id);
	}
}

static struct youtube_request *youtube_request_create(const char *id, const char *target)
{
	struct youtube_request *req = dict_find(requests, id);

	char *request;

	if(req)
	{
		if(!dict_find(req->targets, target))
			youtube_add_target(req, target);

		// Request is being processed, return
		if(!req->response)
			return req;

		youtube_report(req);
		return req;
	}

	req = malloc(sizeof(struct youtube_request));
	req->id = strdup(id);
	req->requested = now;
	req->targets = dict_create();
	req->response = NULL;

	youtube_add_target(req, target);

	request = malloc(24 + strlen(req->id) + 1); // strlen("www.youtube.com/watch?v=") == 24
	sprintf(request, "www.youtube.com/watch?v=%s", req->id);
	req->http = HTTPRequest_create(request, event_func, read_func);
	free(request);

	dict_set_free_funcs(req->targets, free, free);

	debug("Created new youtube request using HTTP request %s (ID: %s)", req->http->id, req->id);

	dict_insert(requests, req->id, req);
	HTTPRequest_connect(req->http);

	return req;
}

static void youtube_request_free(struct youtube_request *req)
{
	debug("Freeing youtube request %s", req->id);

	dict_free(req->targets);
	free(req->id);
	free(req->response);
	free(req);
}

static void youtube_add_target(struct youtube_request *req, const char *target)
{
	struct youtube_target *t;

	if((t = dict_find(req->targets, target)))
		return;

	t = malloc(sizeof(struct youtube_target));
	t->target = strdup(target);
	t->requested = now;
	t->responded = 0;

	req->requested = now;
	dict_insert(req->targets, t->target, t);
}

static struct youtube_request *youtube_request_find(struct HTTPRequest *http)
{
	dict_iter(node, requests)
	{
		if(((struct youtube_request *)node->data)->http == http)
			return node->data;
	}

	return NULL;
}

static void youtube_report(struct youtube_request *req)
{
	dict_iter(node, req->targets)
	{
		struct youtube_target *target = node->data;

		if(!target->responded)
		{
			target->responded = 1;
			irc_send("PRIVMSG %s :YouTube [%s] $b%s$b", target->target, req->id, req->response);
		}
	}
}

static void read_func(struct HTTPRequest *http, const char *buf, unsigned int len)
{
	// Youtube sends the movie's title in several ways, I'll use the title-<div>

	char *tmp, *tmp2;

	struct youtube_request *req = youtube_request_find(http);
	assert(req);

	// Find beginning of <div>
	if(!(tmp = strstr(buf, "<div id=\"watch-vid-title\"")))
		return;

	// Find end of <div> tag
	tmp += 25; // strlen("<div id=\"watch-vid-title\"")
	if(!(tmp = strchr(tmp, '>')))
		return;

	tmp++;
	// Find end of <div>
	if(!(tmp2 = strstr(tmp, "</div>")))
		return;

	// Duplicate title string
	tmp = strndup(tmp, tmp2 - tmp);
	req->response = html_decode(strip_html_tags(strip_duplicate_whitespace(str_replace(tmp, "\n", "", 1))));
	free(tmp);

	youtube_report(req);
}

static void event_func(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	struct youtube_request *req = youtube_request_find(http);
	assert(req);

	switch(event)
	{
		case H_EV_HANGUP:
			if(req->response)
				break;
			// Fall through

		case H_EV_TIMEOUT:
			dict_delete(requests, req->id);
	}
}

static void youtube_timer(void *bound, void *data)
{
	// See if any requests exceeded the maximum cache duration
	dict_iter(node, requests)
	{
		struct youtube_request *req = node->data;

		if(req->requested < (time_t)(now - refresh_delay))
			dict_delete(requests, req->id);
	}

	// Does the total amount of requests exceed the maximum?
	if(max_requests)
		while(requests->count > max_requests)
			dict_delete_node(requests, requests->head);

	youtube_timer_add();
}

static inline void youtube_timer_add()
{
	youtube_timer_del();
	timer_add(NULL, "youtube_cleanup", now + 600, youtube_timer, NULL, 0, 0);
}

static inline void youtube_timer_del()
{
	timer_del_boundname(NULL, "youtube_cleanup");
}

static int youtube_disable_cmod(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	dict_iter(node, requests)
	{
		struct youtube_request *req = node->data;

		dict_iter(req_node, req->targets)
		{
			struct youtube_target *target = req_node->data;

			if(!strcasecmp(target->target, reg->channel))
				dict_delete_node(req->targets, req_node);
		}
	}
	return 0;
}
