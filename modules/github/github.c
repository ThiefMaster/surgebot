#include "global.h"
#include "module.h"
#include "modules/chanreg/chanreg.h"
#include "modules/httpd/http.h"
#include "modules/bitly/bitly.h"
#include "modules/help/help.h"
#include "modules/tools/tools.h"
#include "irc.h"
#include "stringlist.h"
#include "stringbuffer.h"

#include <libgen.h>
#include <json-c/json.h>

MODULE_DEPENDS("chanreg", "httpd", "bitly", "help", "tools", NULL);

struct github_ctx
{
	struct stringlist *messages;
	char *channel;
	char *repo;
	char *ref;
	char *before;
	char *after;
};

HTTP_HANDLER(http_github);
static void url_shortened(const char *url, int success, struct github_ctx *ctx);

static struct module *this;
static struct chanreg_module *cmod;

static struct http_handler http_handlers[] = {
	{ "/github/*", http_github },
	{ NULL, NULL }
};

MODULE_INIT
{
	this = self;
	help_load(self, "github.help");

	cmod = chanreg_module_reg("GitHub", 0, NULL, NULL, NULL, NULL, NULL);
	chanreg_module_setting_reg(cmod, "Key", NULL, NULL, null_none, asterisk_null);

	http_handler_add_list(http_handlers);
}

MODULE_FINI
{
	http_handler_del_list(http_handlers);
	chanreg_module_unreg(cmod);
}

HTTP_HANDLER(http_github)
{
	struct dict *post_vars = http_parse_vars(client, HTTP_POST);
	const char *payload_str = dict_find(post_vars, "payload");
	char *tmp;
	int not_github = (tmp = dict_find(post_vars, "not_github")) ? !!atoi(tmp) : 0;
	const char *channel_raw = argc > 1 ? argv[1] : NULL;
	const char *key = argc > 2 ? argv[2] : NULL;
	const char *chankey;
	char *channel = NULL;
	struct chanreg *reg;
	json_object *payload = NULL;
	struct stringlist *messages = NULL;

	if(!channel_raw)
	{
		debug("Got no channel name from github: %s", channel);
		goto out;
	}

	if(channel_raw && *channel_raw == '#')
		channel = strdup(channel_raw);
	else
	{
		channel = malloc(strlen(channel_raw) + 2);
		snprintf(channel, strlen(channel_raw) + 2, "#%s", channel_raw);
	}

	if(!IsChannelName(channel))
	{
		debug("Got invalid channel name from github: %s", channel);
		goto out;
	}

	if(!(reg = chanreg_find(channel)))
	{
		debug("Got valid channel name %s from github but this channel is not registered", channel);
		goto out;
	}
	else if(!chanreg_module_active(cmod, channel))
	{
		debug("Got valid channel name %s from github but the GitHub module is not enabled", channel);
		goto out;
	}
	else if((chankey = chanreg_setting_get(reg, cmod, "Key")) && (!key || strcmp(chankey, key)))
	{
		debug("Got valid channel name %s from github but the given key '%s' is invalid (expected: '%s')", channel, key, chankey);
		goto out;
	}


	debug("json payload: %s", payload_str);
	if(!payload_str || !(payload = json_tokener_parse(payload_str)) || is_error(payload))
	{
		log_append(LOG_WARNING, "Got invalid json payload from github: %s", payload_str);
		goto out;
	}

	debug("Got valid request from github; channel=%s, key=%s, not_github=%u", channel, key, not_github);

	// Handle payload
	const char *repo_name = json_get_path(payload, "repository.name", json_type_string);
	const char *ref_name = json_get_path(payload, "ref", json_type_string);
	struct array_list *commits = json_get_path(payload, "commits", json_type_array);
	assert_goto(repo_name, out);
	assert_goto(ref_name, out);
	assert_goto(commits, out);

	if(!strncmp(ref_name, "refs/heads/", 11))
		ref_name += 11;
	else if(!strncmp(repo_name, "refs/tags/", 10))
		ref_name += 10;

	messages = stringlist_create();
	for(int i = 0; i < commits->length; i++)
	{
		char msgbuf[MAXLEN], shabuf[7], commitbuf[MAXLEN];
		json_object *commit = commits->array[i];
		const char *message = json_get_path(commit, "message", json_type_string);
		const char *author = json_get_path(commit, "author.name", json_type_string);
		const char *sha1 = json_get_path(commit, "id", json_type_string);
		struct array_list *files = json_get_path(commit, "modified", json_type_array);
		assert_continue(message);
		assert_continue(author);
		assert_continue(sha1);
		assert_continue(files);

		// Shorten message if it has multiple lines
		strlcpy(msgbuf, message, sizeof(msgbuf) - 3); // Leave space for " ...". Only 3 chars needed as \n is overwritten
		tmp = strchr(msgbuf, '\n');
		if(tmp)
			strcpy(tmp, " ...");

		// Get short hash
		strlcpy(shabuf, sha1, sizeof(shabuf));

		// Get file/dir count
		struct stringlist *dirs = stringlist_create();
		for(int i = 0; i < files->length; i++)
		{
			assert_continue(json_object_is_type(files->array[i], json_type_string));
			char *file = strdup(json_object_get_string(files->array[i]));
			const char *dir = dirname(file);
			if(stringlist_find(dirs, dir) == -1)
				stringlist_add(dirs, strdup(dir));
			free(file);
		}

        	snprintf(commitbuf, sizeof(commitbuf),
			"$b%s:$b $c07%s$c $c03%s$c * $b%s$b (%u file%s in %u dir%s): %s",
			repo_name, ref_name, author, shabuf, files->length, (files->length != 1 ? "s" : ""), dirs->count, (dirs->count != 1 ? "s" : ""), msgbuf);
		stringlist_add(messages, strdup(commitbuf));
		stringlist_free(dirs);
	}

	if(messages->count > 1)
	{
		char afterbuf[7], beforebuf[7], urlbuf[256] = { 0 };
		const char *before = json_get_path(payload, "before", json_type_string);
		const char *after = json_get_path(payload, "after", json_type_string);
		const char *repo_url = json_get_path(payload, "repository.url", json_type_string);
		assert_goto(before, out);
		assert_goto(after, out);
		assert_goto(not_github || repo_url, out);
		strlcpy(beforebuf, before, sizeof(beforebuf));
		strlcpy(afterbuf, after, sizeof(afterbuf));
		if(!not_github)
			snprintf(urlbuf, sizeof(urlbuf), "%s/compare/%s...%s", repo_url, beforebuf, afterbuf);
		else
		{
			const char *repo_url_type = json_get_path(payload, "repository.url_type", json_type_string);
			if(!repo_url_type)
				; /* do nothing. if it's not github we url_type is required to build a proper link */
			else if(!strcmp(repo_url_type, "cgit"))
				snprintf(urlbuf, sizeof(urlbuf), "%s/diff/?id=%s&id2=%s", repo_url, afterbuf, beforebuf);
		}

		struct github_ctx *ctx = malloc(sizeof(struct github_ctx));
		memset(ctx, 0, sizeof(struct github_ctx));
		ctx->messages = messages;
		messages = NULL; // so it's not free'd at the end of this function
		ctx->channel = strdup(channel);
		ctx->repo = strdup(repo_name);
		ctx->ref = strdup(ref_name);
		ctx->before = strndup(before, 6);
		ctx->after = strndup(after, 6);
		if(!*urlbuf)
			url_shortened(NULL, 1, ctx);
		else
			bitly_shorten(urlbuf, (bitly_shortened_f *)url_shortened, ctx, NULL);
	}
	else if(messages->count == 1 && commits->length)
	{
		const char *url = json_get_path(commits->array[0], "url", json_type_string);
		assert_goto(not_github || url, out);

		struct github_ctx *ctx = malloc(sizeof(struct github_ctx));
		memset(ctx, 0, sizeof(struct github_ctx));
		ctx->messages = messages;
		messages = NULL; // so it's not free'd at the end of this function
		ctx->channel = strdup(channel);
		if(!url)
			url_shortened(NULL, 1, ctx);
		else
			bitly_shorten(url, (bitly_shortened_f *)url_shortened, ctx, NULL);
	}
	else
	{
		for(unsigned int i = 0; i < messages->count; i++)
			irc_send("PRIVMSG %s :%s", channel, messages->data[i]);
	}

	// Clean up
	out:
	MyFree(channel);
	dict_free(post_vars);
	if(payload)
		json_object_put(payload);
	if(messages)
		stringlist_free(messages);
}

static void url_shortened(const char *url, int success, struct github_ctx *ctx)
{
	if(ctx->messages->count > 1)
	{
		char summarybuf[MAXLEN];
		if(url)
		{
			snprintf(summarybuf, sizeof(summarybuf),
				"$b%s:$b $c07%s$c commits $b%s$b...$b%s$b - %s",
				ctx->repo, ctx->ref, ctx->before, ctx->after, url);
		}
		else
		{
			snprintf(summarybuf, sizeof(summarybuf),
				"$b%s:$b $c07%s$c commits $b%s$b...$b%s$b",
				ctx->repo, ctx->ref, ctx->before, ctx->after);
		}
		stringlist_add(ctx->messages, strdup(summarybuf));
	}
	else if(ctx->messages->count == 1 && url)
	{
		char commitbuf[MAXLEN];
		snprintf(commitbuf, sizeof(commitbuf), "%s - %s", ctx->messages->data[0], url);
		free(ctx->messages->data[0]);
		ctx->messages->data[0] = strdup(commitbuf);
	}

	for(unsigned int i = 0; i < ctx->messages->count; i++)
		irc_send("PRIVMSG %s :%s", ctx->channel, ctx->messages->data[i]);

	stringlist_free(ctx->messages);
	MyFree(ctx->channel);
	MyFree(ctx->repo);
	MyFree(ctx->ref);
	MyFree(ctx->before);
	MyFree(ctx->after);
	free(ctx);
}
