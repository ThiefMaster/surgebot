#include "global.h"
#include "modules/httpd/http.h"
#include "chanuser.h"
#include "usercount.h"
#include <json/json.h>

HTTP_HANDLER(usercount_handler);

static struct http_handler handlers[] = {
	{ "/usercount/?*", usercount_handler },
	{ NULL, NULL }
};

void usercount_init()
{
	http_handler_add_list(handlers);
}

void usercount_fini()
{
	http_handler_del_list(handlers);
}

HTTP_HANDLER(usercount_handler)
{
	struct json_object *response;
	struct irc_channel *channel;
	char *channel_name;
	int user_count = 0;

	assert(argc > 1)
	http_reply_header("Content-Type", "text/javascript");

	channel_name = strdup(uri + 1 + strlen(argv[0]));
	channel_name[0] = '#';

	if((channel = channel_find(channel_name)) && !(channel->modes & (MODE_SECRET|MODE_PRIVATE)))
		user_count = dict_size(channel->users);
	else // no channel or secret/private channel
		channel = NULL;

	response = json_object_new_object();
	json_object_object_add(response, "channel", (channel ? json_object_new_string(channel->name) : NULL));
	json_object_object_add(response, "users", (channel ? json_object_new_int(user_count) : NULL));
	json_object_object_add(response, "topic", ((channel && channel->topic) ? json_object_new_string(channel->topic) : NULL));
	http_reply("ircUsercountCallback(%s);", json_object_to_json_string(response));
	json_object_put(response);
	free(channel_name);
}

