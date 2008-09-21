#include "global.h"
#include "modules/httpd/http.h"
#include "modules/tools/tools.h"
#include "account.h"
#include "session.h"
#include "sha1.h"
#include "chanuser.h"
#include "irc.h"
#include "ajax-app.h"
#include <json/json.h>

#define REQUIRE_SESSION 	struct session *session; \
				if(!(session = session_find(client, uri, argc, argv))) {\
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
				if(argc > 2 && !(session = session_find(client, uri, argc, argv))) {\
					struct json_object *nosession_response = json_object_new_object(); \
					json_object_object_add(nosession_response, "success", json_object_new_boolean(0)); \
					json_object_object_add(nosession_response, "error", json_object_new_string("sessionInvalid")); \
					http_reply_header("Content-Type", "text/javascript"); \
					http_reply_header("Set-Cookie", "sessionID=; Path=/"); \
					http_reply("%s", json_object_to_json_string(nosession_response)); \
					json_object_put(nosession_response); \
					return; \
				}


struct menu
{
	char *id;
	char *title;
	unsigned int guest : 1;
};

HTTP_HANDLER(ajax_index_handler);
HTTP_HANDLER(ajax_raw_handler);
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
	{ "/ajax/raw/*", ajax_raw_handler },
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

void ajaxapp_init()
{
	menu_items = dict_create();
	dict_set_free_funcs(menu_items, NULL, (dict_free_f*)menu_free);

	http_handler_add_list(handlers);

	// guest menu
	menu_add("login", "Login", 1);
	menu_add("signup", "Signup", 1);
	// user Menu
	menu_add("index", "Index", 0);
	menu_add("raw", "Raw commands", 0);
	menu_add("logout", "Logout", 0);
}

void ajaxapp_fini()
{
	menu_del("index");
	menu_del("raw");
	menu_del("logout");
	menu_del("login");
	menu_del("signup");

	http_handler_del_list(handlers);
	dict_free(menu_items);
}

HTTP_HANDLER(ajax_index_handler)
{
	struct json_object *response;
	REQUIRE_SESSION

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

HTTP_HANDLER(ajax_raw_handler)
{
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
	json_object_object_add(response, "success", json_object_new_boolean(1));
	items = json_object_new_object();

	dict_iter_rev(node, menu_items)
	{
		struct menu *menu = node->data;
		if((session && menu->guest) || (!session && !menu->guest))
			continue;
		json_object_object_add(items, menu->id, json_object_new_string(menu->title));
	}

	json_object_object_add(response, "items", items);
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

void menu_add(const char *id, const char *title, unsigned int guest)
{
	struct menu *menu = malloc(sizeof(struct menu));
	memset(menu, 0, sizeof(struct menu));
	menu->id = strdup(id);
	menu->title = strdup(title);
	menu->guest = guest;
	dict_insert(menu_items, menu->id, menu);
}

void menu_del(const char *id)
{
	dict_delete(menu_items, id);
}

static void menu_free(struct menu *menu)
{
	free(menu->id);
	free(menu->title);
	free(menu);
}
