#include "global.h"
#include "module-config.h"
#include "modules/httpd/http.h"
#include "modules/tools/tools.h"

#ifdef WITH_MODULE_chanserv
#include "modules/db/db.h"
#include "modules/chanreg/chanreg.h"
#include "modules/chanserv/chanserv.h"
#endif

#include "account.h"
#include "session.h"
#include "rules.h"
#include "sha1.h"
#include "chanuser.h"
#include "irc.h"
#include "ajax-app.h"
#include <json/json.h>

#define REQUIRE_SESSION 	struct session *session; \
				if(!(session = session_find(client, uri, argc, argv))) { \
					struct json_object *nosession_response = json_object_new_object(); \
					json_object_object_add(nosession_response, "success", json_object_new_boolean(0)); \
					json_object_object_add(nosession_response, "error", json_object_new_string("sessionInvalid")); \
					http_reply_header("Content-Type", "text/javascript"); \
					http_reply_header("Set-Cookie", "sessionID=; Path=/"); \
					http_reply("%s", json_object_to_json_string(nosession_response)); \
					json_object_put(nosession_response); \
					return; \
				}

// require session only if a sid was sent
#define REQUIRE_SESSION_IF_SID 	struct session *session = NULL; \
				if(argc > 2 && !(session = session_find(client, uri, argc, argv))) { \
					struct json_object *nosession_response = json_object_new_object(); \
					json_object_object_add(nosession_response, "success", json_object_new_boolean(0)); \
					json_object_object_add(nosession_response, "error", json_object_new_string("sessionInvalid")); \
					http_reply_header("Content-Type", "text/javascript"); \
					http_reply_header("Set-Cookie", "sessionID=; Path=/"); \
					http_reply("%s", json_object_to_json_string(nosession_response)); \
					json_object_put(nosession_response); \
					return; \
				}

#define CHECK_RULE(RULE)	enum rule_result _rule_res = rule_exec((RULE), session); \
				if(_rule_res != R_ALLOW) { \
					struct json_object *rulefailed_response = json_object_new_object(); \
					json_object_object_add(rulefailed_response, "success", json_object_new_boolean(0)); \
					if(_rule_res == R_DENY) \
						json_object_object_add(rulefailed_response, "error", json_object_new_string("accessDenied")); \
					else /* R_ERROR */ \
						json_object_object_add(rulefailed_response, "error", json_object_new_string("ruleExecFailed")); \
					http_reply_header("Content-Type", "text/javascript"); \
					http_reply("%s", json_object_to_json_string(rulefailed_response)); \
					json_object_put(rulefailed_response); \
					return; \
				}


struct menu
{
	char *id;
	char *title;
	unsigned int rule;
};

HTTP_HANDLER(ajax_index_handler);
#ifdef WITH_MODULE_chanserv
HTTP_HANDLER(ajax_events_handler);
#endif
HTTP_HANDLER(ajax_raw_handler);
HTTP_HANDLER(ajax_channels_handler);
HTTP_HANDLER(ajax_channel_handler);
HTTP_HANDLER(ajax_404_handler);
HTTP_HANDLER(ajax_menu_handler);
HTTP_HANDLER(ajax_init_handler);
HTTP_HANDLER(ajax_login_handler);
HTTP_HANDLER(ajax_signup_handler);
HTTP_HANDLER(ajax_logout_handler);
HTTP_HANDLER(page_handler);

static void menu_free(struct menu *menu);

static struct http_handler handlers[] = {
	{ "/ajax/index/*", ajax_index_handler },
#ifdef WITH_MODULE_chanserv
	{ "/ajax/events/*", ajax_events_handler },
#endif
	{ "/ajax/raw/*", ajax_raw_handler },
	{ "/ajax/channels/*", ajax_channels_handler },
	{ "/ajax/channel/*", ajax_channel_handler },
	// core handlers
	{ "/ajax/*", ajax_404_handler },
	{ "/ajax/init", ajax_init_handler },
	{ "/ajax/menu/*", ajax_menu_handler },
	{ "/ajax/login", ajax_login_handler },
	{ "/ajax/signup", ajax_signup_handler },
	{ "/ajax/logout/*", ajax_logout_handler },
	{ "/page/?*", page_handler },
	{ NULL, NULL }
};

static struct dict *menu_items;
#ifdef WITH_MODULE_chanserv
static unsigned int events_rule = 0;
#endif
static unsigned int raw_rule = 0;
static unsigned int channels_rule = 0;

#ifdef WITH_MODULE_chanserv
static struct db_table *event_table = NULL;
#endif

void ajaxapp_init()
{
	menu_items = dict_create();
	dict_set_free_funcs(menu_items, NULL, (dict_free_f*)menu_free);

	http_handler_add_list(handlers);

	// guest menu
	menu_add("login", "Login", "!loggedin()");
	menu_add("signup", "Signup", "!loggedin()");
	// user menu
	menu_add("index", "Index", "loggedin()");
#ifdef WITH_MODULE_chanserv
	events_rule = menu_add("events", "ChanServ Events", "loggedin()");
#endif
	raw_rule = menu_add("raw", "Raw commands", "group(admins)");
	channels_rule = menu_add("channels", "Channels", "group(helpers) || group(admins)");
	menu_add("logout", "Logout", "loggedin()");

#ifdef WITH_MODULE_chanserv
	event_table = db_table_open("chanserv_events", chanserv_event_table_cols());
	if(!event_table)
		log_append(LOG_ERROR, "Could not open eventlog table.");
#endif
}

void ajaxapp_fini()
{
#ifdef WITH_MODULE_chanserv
	if(event_table)
		db_table_close(event_table);
#endif

	menu_del("index");
#ifdef WITH_MODULE_chanserv
	menu_del("events");
#endif
	menu_del("raw");
	menu_del("channels");
	menu_del("logout");
	menu_del("login");
	menu_del("signup");

	http_handler_del_list(handlers);
	dict_free(menu_items);
}

HTTP_HANDLER(ajax_index_handler)
{
	REQUIRE_SESSION
	struct json_object *response;

	response = json_object_new_object();
	json_object_object_add(response, "success", json_object_new_boolean(1));
	json_object_object_add(response, "uptime", json_object_new_int(now - bot.start));
	json_object_object_add(response, "linked", json_object_new_int(now - bot.linked));
	json_object_object_add(response, "server", json_object_new_string(bot.server_name));
	json_object_object_add(response, "linesReceived", json_object_new_int(bot.lines_received));
	json_object_object_add(response, "linesSent", json_object_new_int(bot.lines_sent));
	json_object_object_add(response, "channels", json_object_new_int(dict_size(channel_dict())));
	json_object_object_add(response, "users", json_object_new_int(dict_size(user_dict())));

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
}

#ifdef WITH_MODULE_chanserv
DB_SELECT_CB(events_cb)
{
	struct json_object *response;
	struct http_client *client = ctx;

	if(error)
		goto finish;

	if(rownum <= 1) // rownum 0 if there were no rows at all
		client->custom2 = json_object_new_array();

	if(rowcount == 0)
		goto finish;

	// "time", "channel", "nick", "ident", "host", "account", "command"
	time_t time 	= values->data[0].u.datetime;
	//char *channel	= values->data[1].u.string;
	char *nick	= values->data[2].u.string;
	char *ident	= values->data[3].u.string;
	char *host	= values->data[4].u.string;
	char *account	= values->data[5].u.string;
	char *command	= values->data[6].u.string;

	struct json_object *row = json_object_new_object();
	json_object_object_add(row, "nick", json_object_new_string(nick));
	json_object_object_add(row, "ident", json_object_new_string(ident));
	json_object_object_add(row, "host", json_object_new_string(host));
	json_object_object_add(row, "account", json_object_new_string(account));
	json_object_object_add(row, "command", json_object_new_string(command));
	json_object_object_add(row, "time", json_object_new_int(time));
	json_object_array_add(client->custom2, row);

finish:
	if(error || rownum == rowcount || !rowcount) // !rowcount -> rownum == rowcount, but in this way it's more clear
	{
		response = json_object_new_object();
		if(error)
		{
			json_object_object_add(response, "success", json_object_new_boolean(0));
			json_object_object_add(response, "error", json_object_new_string("queryFailed"));
		}
		else
		{
			json_object_object_add(response, "success", json_object_new_boolean(1));
			json_object_object_add(response, "channel", json_object_new_string(client->custom));
			json_object_object_add(response, "events", client->custom2);
		}

		free(client->custom);
		http_reply_header("Content-Type", "text/javascript");
		http_reply("%s", json_object_to_json_string(response));
		json_object_put(response);
		http_request_finalize(client);
	}
	return 0;
}

HTTP_HANDLER(ajax_events_handler)
{
	REQUIRE_SESSION
	CHECK_RULE(events_rule)
	char *channel, *str;
	struct chanreg *chanreg;
	int rval;
	unsigned int offset = 0;
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	if(!(channel = dict_find(post_vars, "channel")))
	{
		struct json_object *list, *response = json_object_new_object();
		json_object_object_add(response, "success", json_object_new_boolean(1));
		list = json_object_new_array();
		struct chanreg_list *channels = chanreg_get_access_channels(session->account, 350, 1);
		for(unsigned int i = 0; i < channels->count; i++)
			json_object_array_add(list, json_object_new_string(channels->data[i]->channel));
		chanreg_list_free(channels);
		json_object_object_add(response, "channels", list);
		http_reply_header("Content-Type", "text/javascript");
		http_reply("%s", json_object_to_json_string(response));
		json_object_put(response);
		dict_free(post_vars);
		return;
	}

	if(!(chanreg = chanreg_find(channel)) || !chanreg_check_access(chanreg, session->account, 350, 1))
	{
		struct json_object *response = json_object_new_object();
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("accessDenied"));
		http_reply_header("Content-Type", "text/javascript");
		http_reply("%s", json_object_to_json_string(response));
		json_object_put(response);
		dict_free(post_vars);
		return;
	}

	if((str = dict_find(post_vars, "offset")))
		offset = atoi(str);

	client->custom = strdup(channel);
	rval = db_async_select(event_table, events_cb, client, NULL,
				"channel", client->custom, NULL,
				"time", "channel", "nick", "ident", "host", "account", "command", NULL,
				"-time", "$LIMIT", 25, "$OFFSET", offset, NULL);
	if(rval)
	{
		struct json_object *response = json_object_new_object();
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("queryFailed"));
		http_reply_header("Content-Type", "text/javascript");
		http_reply("%s", json_object_to_json_string(response));
		json_object_put(response);
		free(client->custom);
		dict_free(post_vars);
		return;
	}

	dict_free(post_vars);
	http_request_detach(client, NULL);
}
#endif

HTTP_HANDLER(ajax_raw_handler)
{
	REQUIRE_SESSION
	CHECK_RULE(raw_rule)
	struct json_object *response;
	char *command;
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	command = dict_find(post_vars, "command");

	response = json_object_new_object();
	if(!command)
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("dataMissing"));
	}
	else
	{
		irc_send("%s", command);
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "command", json_object_new_string(command));
	}

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(post_vars);
}

HTTP_HANDLER(ajax_channels_handler)
{
	REQUIRE_SESSION
	CHECK_RULE(channels_rule)
	struct json_object *response, *list;
	char *channelname;
	struct dict *channels = channel_dict();
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	channelname = dict_find(post_vars, "channel");

	response = json_object_new_object();
	json_object_object_add(response, "success", json_object_new_boolean(1));
	list = json_object_new_array();
	dict_iter(node, channels)
		json_object_array_add(list, json_object_new_string(node->key));
	json_object_object_add(response, "channels", list);

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(post_vars);
}

HTTP_HANDLER(ajax_channel_handler)
{
	REQUIRE_SESSION
	CHECK_RULE(channels_rule)
	struct json_object *response, *users, *ops, *voices, *regulars;
	char *channelname;
	struct irc_channel *channel;
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	channelname = dict_find(post_vars, "channel");
	channel = channelname ? channel_find(channelname) : NULL;

	response = json_object_new_object();
	if(channel)
	{
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "channel", json_object_new_string(channel->name));
		json_object_object_add(response, "topic", channel->topic ? json_object_new_string(channel->topic) : NULL);
		json_object_object_add(response, "modes", json_object_new_string((char*)chanmodes2string(channel->modes, channel->limit, channel->key)));
		users = json_object_new_object();
		ops = json_object_new_array();
		voices = json_object_new_array();
		regulars = json_object_new_array();
		dict_iter(node, channel->users)
		{
			struct irc_chanuser *chanuser = node->data;
			if(chanuser->flags & MODE_OP)
				json_object_array_add(ops, json_object_new_string(chanuser->user->nick));
			else if(chanuser->flags & MODE_VOICE)
				json_object_array_add(voices, json_object_new_string(chanuser->user->nick));
			else
				json_object_array_add(regulars, json_object_new_string(chanuser->user->nick));
		}

		json_object_object_add(users, "ops", ops);
		json_object_object_add(users, "voices", voices);
		json_object_object_add(users, "regulars", regulars);
		json_object_object_add(response, "users", users);
	}
	else
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("invalidChannel"));
	}

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(post_vars);
}

HTTP_HANDLER(ajax_404_handler)
{
	struct json_object *response;
	response = json_object_new_object();
	json_object_object_add(response, "success", json_object_new_boolean(0));
	json_object_object_add(response, "error", json_object_new_string("404"));
	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
}

HTTP_HANDLER(ajax_menu_handler)
{
	struct json_object *response, *items;
	REQUIRE_SESSION_IF_SID

	response = json_object_new_object();
	items = json_object_new_object();

	dict_iter_rev(node, menu_items)
	{
		struct menu *menu = node->data;
		enum rule_result res = rule_exec(menu->rule, session);
		if(res == R_ALLOW)
			json_object_object_add(items, menu->id, json_object_new_string(menu->title));
		else if(res == R_ERROR)
		{
			json_object_put(items);
			json_object_object_add(response, "success", json_object_new_boolean(0));
			json_object_object_add(response, "error", json_object_new_string("ruleExecFailed"));
			return;
		}
	}

	json_object_object_add(response, "items", items);
	json_object_object_add(response, "success", json_object_new_boolean(1));
	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
}

HTTP_HANDLER(ajax_init_handler)
{
	struct json_object *response;
	char *sid;
	struct session *session;
	struct dict *cookie_vars = http_parse_cookies(client);

	response = json_object_new_object();
	sid = dict_find(cookie_vars, "sessionID");
	if(sid && (session = session_find_sid(client, sid)))
	{
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "loggedIn", json_object_new_boolean(1));
		json_object_object_add(response, "sid", json_object_new_string(session->sid));
		json_object_object_add(response, "userName", json_object_new_string(session->account->name));
	}
	else
	{
		if(sid)
			http_reply_header("Set-Cookie", "sessionID=; Path=/"); // delete cookie
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "loggedIn", json_object_new_boolean(0));
		json_object_object_add(response, "sid", NULL);
		json_object_object_add(response, "userName", NULL);
	}

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(cookie_vars);
}

HTTP_HANDLER(ajax_login_handler)
{
	struct json_object *response;
	const char *username, *password;
	struct user_account *account;
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	username = dict_find(post_vars, "username");
	password = dict_find(post_vars, "password");

	response = json_object_new_object();
	if(!username || !password)
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("dataMissing"));
	}
	else if(!(account = account_find(username)))
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("invalidUser"));
	}
	else if(strcmp(sha1(password), account->pass))
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("invalidPass"));
	}
	else
	{
		debug("Logging in: %s", account->name);
		struct session *session = session_create(account, REMOTE_IP(client->sock));
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "sid", json_object_new_string(session->sid));
		json_object_object_add(response, "userName", json_object_new_string(account->name));
		http_reply_header("Set-Cookie", "sessionID=%s; Path=/", session->sid);
	}

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(post_vars);
}

HTTP_HANDLER(ajax_signup_handler)
{
	struct json_object *response;
	char *username, *password;
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);

	username = dict_find(post_vars, "username");
	password = dict_find(post_vars, "password");

	response = json_object_new_object();
	if(!username || !password)
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("dataMissing"));
	}
	else if(account_find(username))
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "error", json_object_new_string("userExists"));
	}
	else
	{
		debug("Registering new account: %s", username);
		struct user_account *account = account_register(username, password);
		struct session *session = session_create(account, REMOTE_IP(client->sock));
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "sid", json_object_new_string(session->sid));
		json_object_object_add(response, "userName", json_object_new_string(account->name));
		http_reply_header("Set-Cookie", "sessionID=%s; Path=/", session->sid);
	}

	http_reply_header("Content-Type", "text/javascript");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(post_vars);
}

HTTP_HANDLER(ajax_logout_handler)
{
	struct json_object *response;
	struct session *session;

	if((session = session_find(client, uri, argc, argv)))
		session_del(session);

	response = json_object_new_object();
	json_object_object_add(response, "success", json_object_new_boolean(1));
	http_reply_header("Content-Type", "text/javascript");
	if(argc > 2)
		http_reply_header("Set-Cookie", "sessionID=; Path=/"); // delete sid cookie
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
}

HTTP_HANDLER(page_handler)
{
	char *virtual, filename[PATH_MAX] = { 0 };
	size_t len = 0;
	FILE *fd;
	char buf[4096];
	assert(argc > 1);
	virtual = argv[1];

	len = strlcpy(filename, "modules/webinterface/files/pages/", sizeof(filename) - len);

	for(char *c = virtual; *c && len < PATH_MAX - 1; c++)
	{
		if(!ct_isalnum(*c))
			continue;
		filename[len++] = *c;
	}

	len += strlcpy(filename + len, "Page.html", sizeof(filename) - len);
	filename[len] = '\0';
	debug("filename='%s'", filename);

	fd = fopen(filename, "r");
	if(!fd)
	{
		http_write_header_status(client, 404);
		http_reply_header("Content-Type", "text/html");
		char *tmp = html_encode(filename);
		http_reply("File '%s' could not be opened for reading: %s (%d)", tmp, strerror(errno), errno);
		free(tmp);
		return;
	}

        http_reply_header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        http_reply_header("Cache-Control", "must-revalidate");
        http_reply_header("Pragma", "no-cache");

	http_reply_header("Content-Type", "text/html");
	while(!feof(fd))
	{
		size_t len = fread(buf, 1, sizeof(buf), fd);
		stringbuffer_append_string_n(client->wbuf, buf, len);
	}
	fclose(fd);
}

unsigned int menu_add(const char *id, const char *title, const char *rule)
{
	struct menu *menu = malloc(sizeof(struct menu));
	memset(menu, 0, sizeof(struct menu));
	menu->id = strdup(id);
	menu->title = strdup(title);
	menu->rule = rule_compile(rule);
	dict_insert(menu_items, menu->id, menu);
	return menu->rule;
}

void menu_del(const char *id)
{
	dict_delete(menu_items, id);
}

static void menu_free(struct menu *menu)
{
	free(menu->id);
	free(menu->title);
	rule_free(menu->rule);
	free(menu);
}
