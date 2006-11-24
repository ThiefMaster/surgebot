#include "global.h"
#include "chanuser.h"
#include "account.h"
#include "stringlist.h"

#define CHANUSER_IMPLEMENT_HOOKABLE(NAME) \
	static NAME##_f *NAME##_hooks; \
	static unsigned int NAME##_hooks_used = 0, NAME##_hooks_size = 0; \
	void chanuser_reg_##NAME##_hook(NAME##_f func) { \
		if (NAME##_hooks_used == NAME##_hooks_size) { \
			if (!NAME##_hooks_size) { \
				NAME##_hooks_size = 4; \
				NAME##_hooks = malloc(NAME##_hooks_size * sizeof(NAME##_hooks[0])); \
			} else { \
				NAME##_hooks_size <<= 1; \
				NAME##_hooks = realloc(NAME##_hooks, NAME##_hooks_size * sizeof(NAME##_hooks[0])); \
			} \
		} \
		NAME##_hooks[NAME##_hooks_used++] = func; \
	} \
	void chanuser_unreg_##NAME##_hook(NAME##_f func) { \
		for (unsigned int i = 0; i < NAME##_hooks_used; i++) { \
			if (NAME##_hooks[i] == func) { \
				memmove(NAME##_hooks+i, NAME##_hooks+i+1, (NAME##_hooks_size-i-1)*sizeof(NAME##_hooks[0])); \
				NAME##_hooks_used--; \
				break; \
			} \
		} \
	} \
	static void chanuser_clear_##NAME##_hooks() { \
		if (NAME##_hooks) free(NAME##_hooks); \
		NAME##_hooks = NULL; \
		NAME##_hooks_used = 0; \
		NAME##_hooks_size = 0; \
	}

#define CHANUSER_CALL_HOOKS(NAME, ARGS) \
	for (unsigned int ii = 0; ii < NAME##_hooks_used; ii++) \
		NAME##_hooks[ii] ARGS

static struct dict *channels;
static struct dict *users;

CHANUSER_IMPLEMENT_HOOKABLE(channel_del);
CHANUSER_IMPLEMENT_HOOKABLE(channel_complete);
CHANUSER_IMPLEMENT_HOOKABLE(user_del);

void chanuser_init()
{
	channels = dict_create();
	users = dict_create();
}

void chanuser_fini()
{
	chanuser_clear_channel_del_hooks();
	chanuser_clear_channel_complete_hooks();
	chanuser_clear_user_del_hooks();

	chanuser_flush();
	dict_free(users);
	dict_free(channels);
}

void chanuser_flush()
{
	while(dict_size(users))
		user_del(dict_first_data(users), 0, NULL);
	while(dict_size(channels))
		channel_del(dict_first_data(channels), NULL);
}


/* Channel functions */
struct dict *channel_dict()
{
	return channels;
}

struct irc_channel* channel_add(const char *name, int do_burst)
{
	struct irc_channel* channel;

	if((channel = channel_find(name)))
	{
		log_append(LOG_WARNING, "Channel %s was already added; re-adding it", channel->name);
		channel_del(channel, NULL);
	}

	channel = malloc(sizeof(struct irc_channel));
	memset(channel, 0, sizeof(struct irc_channel));

	channel->name	= strdup(name);
	channel->users	= dict_create();
	channel->bans	= dict_create();

	// if do_burst == 0 we assume everything is known about the channel
	channel->burst_state = do_burst ? BURST_NAMES : BURST_FINISHED;
	if(do_burst)
		bot.burst_count++;
	channel->burst_lines = stringlist_create();

	dict_insert(channels, channel->name, channel);
	if(!do_burst)
		channel_complete(channel);
	return channel;
}

// Called when all information about a channel is available (i.e. burst finished)
void channel_complete(struct irc_channel *channel)
{
	CHANUSER_CALL_HOOKS(channel_complete, (channel));
}

struct irc_channel* channel_find(const char *name)
{
	return dict_find(channels, name);
}

void channel_del(struct irc_channel *channel, const char *reason)
{
	if(reason)
		CHANUSER_CALL_HOOKS(channel_del, (channel, reason));

	dict_iter(node, channel->users)
	{
		struct irc_chanuser *chanuser = node->data;
		channel_user_del(channel, chanuser->user, 1);
	}

	dict_iter(node, channel->bans)
	{
		struct irc_ban *ban = node->data;
		channel_ban_del(channel, ban->mask);
	}

	dict_delete(channels, channel->name);
	free(channel->name);
	dict_free(channel->users);
	dict_free(channel->bans);
	if(channel->key)   free(channel->key);
	if(channel->topic) free(channel->topic);
	stringlist_free(channel->burst_lines);
	free(channel);
}

void channel_set_topic(struct irc_channel *channel, const char *topic, time_t ts)
{
	if(channel->topic)
		free(channel->topic);

	if(ts)
		channel->topic_ts = ts;

	if(topic == NULL || !strlen(topic))
		channel->topic = NULL;
	else
		channel->topic = strdup(topic);
}

void channel_set_key(struct irc_channel *channel, const char *key)
{
	if(channel->key)
		free(channel->key);

	if(key == NULL || !strlen(key))
		channel->key = NULL;
	else
		channel->key = strdup(key);
}

void channel_set_limit(struct irc_channel *channel, unsigned int limit)
{
	channel->limit = limit;
}


/* User functions */
struct dict *user_dict()
{
	return users;
}

struct irc_user* user_add(const char *nick, const char *ident, const char *host)
{
	struct irc_user *user = user_add_nick(nick);

	user->ident	= strdup(ident);
	user->host	= strdup(host);
	return user;
}

struct irc_user* user_add_nick(const char *nick)
{
	struct irc_user *user;

	if((user = user_find(nick)))
	{
		log_append(LOG_WARNING, "User %s was already added; re-adding it", user->nick);
		user_del(user, 0, NULL);
	}

	user = malloc(sizeof(struct irc_user));
	memset(user, 0, sizeof(struct irc_user));

	user->nick	= strdup(nick);
	user->channels	= dict_create();

	dict_insert(users, user->nick, user);
	return user;
}

void user_complete(struct irc_user *user, const char *ident, const char *host)
{
	if(user->ident && user->host)
		return;

	if(user->ident)	free(user->ident);
	if(user->host)	free(user->host);

	user->ident = strdup(ident);
	user->host  = strdup(host);
}

void user_set_info(struct irc_user *user, const char *info)
{
	if(user->info) free(user->info);
	user->info = strdup(info);
}

struct irc_user* user_find(const char *nick)
{
	return dict_find(users, nick);
}

void user_del(struct irc_user *user, unsigned int quit, const char *reason)
{
	CHANUSER_CALL_HOOKS(user_del, (user, quit, reason));

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		channel_user_del(chanuser->channel, user, 0);
	}

	if(user->account)
		account_user_del(user->account, user);

	dict_delete(users, user->nick);
	dict_free(user->channels);
	free(user->nick);
	if(user->ident)	free(user->ident);
	if(user->host)	free(user->host);
	if(user->info)	free(user->info);
	free(user);
}

void user_rename(struct irc_user *user, const char *nick)
{
	char *old_nick = user->nick;

	dict_delete(users, user->nick);
	user->nick = strdup(nick);
	dict_insert(users, user->nick, user);

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		dict_delete(chanuser->channel->users, old_nick);
		dict_insert(chanuser->channel->users, user->nick, chanuser);
	}

	if(user->account)
	{
		assert(dict_find(user->account->users, old_nick));
		dict_delete(user->account->users, old_nick);
		dict_insert(user->account->users, user->nick, user);
	}

	free(old_nick);
}


/* Channel user functions */
struct irc_chanuser* channel_user_add(struct irc_channel *channel, struct irc_user *user, int flags)
{
	struct irc_chanuser *chanuser;

	if((chanuser = channel_user_find(channel, user)))
	{
		log_append(LOG_WARNING, "User %s was already in %s; re-adding him", user->nick, channel->name);
		chanuser->flags = flags;
		return chanuser;
	}

	chanuser = malloc(sizeof(struct irc_chanuser));
	memset(chanuser, 0, sizeof(struct irc_chanuser));

	chanuser->channel = channel;
	chanuser->user    = user;
	chanuser->flags   = flags;

	dict_insert(channel->users, user->nick, chanuser);
	dict_insert(user->channels, channel->name, chanuser);

	return chanuser;
}

struct irc_chanuser* channel_user_find(struct irc_channel *channel, struct irc_user *user)
{
	return dict_find(channel->users, user->nick);
}

int channel_user_del(struct irc_channel *channel, struct irc_user *user, int check_dead)
{
	struct irc_chanuser *chanuser = channel_user_find(channel, user);
	assert_return(chanuser, 0);

	dict_delete(channel->users, user->nick);
	dict_delete(user->channels, channel->name);
	free(chanuser);

	if(check_dead && dict_size(user->channels) == 0)
	{
		debug("Deleting dead user %s", user->nick);
		user_del(user, 0, "dead");
		return 1;
	}

	return 0;
}


/* Ban functions */
struct irc_ban* channel_ban_add(struct irc_channel *channel, const char *mask)
{
	struct irc_ban *ban;

	if((ban = channel_ban_find(channel, mask)))
	{
		log_append(LOG_WARNING, "Mask %s is already banned in %s", mask, channel->name);
		return ban;
	}

	ban = malloc(sizeof(struct irc_ban));
	memset(ban, 0, sizeof(struct irc_ban));

	ban->channel = channel;
	ban->mask    = strdup(mask);

	dict_insert(channel->bans, ban->mask, ban);
	return ban;
}

struct irc_ban* channel_ban_find(struct irc_channel *channel, const char *mask)
{
	return dict_find(channel->bans, mask);
}

void channel_ban_del(struct irc_channel *channel, const char *mask)
{
	struct irc_ban *ban = channel_ban_find(channel, mask);
	assert(ban);

	dict_delete(channel->bans, ban->mask);
	free(ban->mask);
	free(ban);
}

