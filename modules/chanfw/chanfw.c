#include "global.h"
#include "module.h"
#include "chanuser.h"
#include "irc.h"
#include "irc_handler.h"
#include "timer.h"
#include "modules/chanreg/chanreg.h"
#include "dict.h"

#define TIMER_NAME "chanfw_kick"

#define NOTIFY_PM	0x1
#define NOTIFY_CHANNEL	0x2
#define NOTIFY_NOTICE	0x4

MODULE_DEPENDS("chanreg", NULL);

IRC_HANDLER(join);
IRC_HANDLER(part);
IRC_HANDLER(kick);
IRC_HANDLER(quit);
IRC_HANDLER(nick);

struct chanfw_user
{
	char *channel;
	char *nick;
};

static void chanfw_user_free(struct chanfw_user *);
static void chanfw_new_victim(struct chanreg *reg, const char *nick, unsigned int send_channel_notify);
static void remove_user(void *bound, struct chanfw_user *data);
static void channel_complete_hook(struct irc_channel *);
static int number_validate(struct irc_source *src, const char *value);
static int notification_validate(struct irc_source *src, const char *value);
static const char *notification_format(const char *value);
static const char *notification_encode(const char *old_value, const char *value);
static const char *null_none(const char *value);
static const char *asterisk_null(const char *old_value, const char *value);
static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason);
static int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);

static struct module *this;
static struct chanreg_module *cmod;
static struct dict *users;

MODULE_INIT
{
	this = self;
	reg_irc_handler("JOIN", join);
	reg_irc_handler("PART", part);
	reg_irc_handler("KICK", kick);
	reg_irc_handler("QUIT", quit);
	reg_irc_handler("NICK", nick);
	chanuser_reg_channel_complete_hook(channel_complete_hook);

	cmod = chanreg_module_reg("ChanForward", 0, NULL, NULL, cmod_enabled, cmod_disabled);
	chanreg_module_setting_reg(cmod, "KickMsg", NULL, NULL, null_none, asterisk_null);
	chanreg_module_setting_reg(cmod, "JoinMsg", NULL, NULL, null_none, asterisk_null);
	chanreg_module_setting_reg(cmod, "KickDelay", "600", number_validate, NULL, NULL);
	chanreg_module_setting_reg(cmod, "BanDuration", "300", number_validate, NULL, NULL);
	chanreg_module_setting_reg(cmod, "NotifyType", "1", notification_validate, notification_format, notification_encode);

	users = dict_create();
	dict_set_free_funcs(users, free, (dict_free_f *)dict_free);
}

MODULE_FINI
{
	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("KICK", kick);
	unreg_irc_handler("QUIT", quit);
	unreg_irc_handler("NICK", nick);
	timer_del_boundname(this, TIMER_NAME);
	timer_del_boundname(this, "channel_complete");
	chanuser_unreg_channel_complete_hook(channel_complete_hook);
	chanreg_module_unreg(cmod);
	dict_free(users);
}

static void chanfw_new_victim(struct chanreg *reg, const char *nick, unsigned int send_channel_notify)
{
	long kick_delay = atol(chanreg_setting_get(reg, cmod, "KickDelay"));
	const char *kickmsg = chanreg_setting_get(reg, cmod, "KickMsg");
	const char *joinmsg = chanreg_setting_get(reg, cmod, "JoinMsg");
	unsigned int notify_types = atoi(chanreg_setting_get(reg, cmod, "NotifyType"));

	if(kickmsg)
	{
		struct dict *channel_users;
		if(!(channel_users = dict_find(users, reg->channel)))
		{
			channel_users = dict_create();
			dict_set_free_funcs(channel_users, NULL, (dict_free_f *)chanfw_user_free);
			dict_insert(users, strdup(reg->channel), channel_users);
		}

		struct chanfw_user *user = malloc(sizeof(struct chanfw_user));
		user->channel = strdup(reg->channel);
		user->nick = strdup(nick);
		dict_insert(channel_users, user->nick, user);
		debug("Adding chanfw user %s/%s", reg->channel, user->nick);
		timer_add(this, TIMER_NAME, now + kick_delay, (timer_f *)remove_user, user, 0);
	}

	if(joinmsg/* && kick_delay*/)
	{
		if(notify_types & NOTIFY_PM)
			irc_send("PRIVMSG %s :%s", nick, joinmsg);
		if(notify_types & NOTIFY_NOTICE)
			irc_send("NOTICE %s :%s", nick, joinmsg);
		if((notify_types & NOTIFY_CHANNEL) && send_channel_notify)
			irc_send("PRIVMSG %s :$b%s:$b %s", reg->channel, nick, joinmsg);
	}
}

IRC_HANDLER(join)
{
	if(!chanreg_module_active(cmod, argv[1]))
		return;

	struct chanreg *reg = chanreg_find(argv[1]);
	assert(reg);

	if(!strcasecmp(src->nick, bot.nickname))
		return;

	chanfw_new_victim(reg, src->nick, 1);
}

IRC_HANDLER(part)
{
	struct dict *channel_users;

	if(!chanreg_module_active(cmod, argv[1]))
		return;

	if(!(channel_users = dict_find(users, argv[1])))
		return;

	if(!strcasecmp(src->nick, bot.nickname))
	{
		// We need to delete all timers and the chanfw_user structs
		timer_del_boundname(this, TIMER_NAME);
		dict_iter(node, channel_users)
		{
			if(!strcasecmp(((struct chanfw_user *)node->data)->channel, argv[1]))
				dict_delete(channel_users, node->key);
		}
	}
	else
	{
		timer_del(this, TIMER_NAME, 0, NULL, dict_find(channel_users, src->nick), TIMER_IGNORE_FUNC | TIMER_IGNORE_TIME);
		debug("Deleting chanfw user %s/%s", argv[1], src->nick);
		dict_delete(channel_users, src->nick);
	}
}

IRC_HANDLER(kick)
{
	struct dict *channel_users;

	if(!chanreg_module_active(cmod, argv[1]))
		return;

	if(!(channel_users = dict_find(users, argv[1])))
		return;

	if(!strcasecmp(argv[2], bot.nickname))
	{
		// We need to delete all timers and the chanfw_user structs
		timer_del_boundname(this, TIMER_NAME);
		dict_iter(node, channel_users)
		{
			if(!strcasecmp(((struct chanfw_user *)node->data)->channel, argv[1]))
				dict_delete(channel_users, node->key);
		}
	}
	else
	{
		timer_del(this, TIMER_NAME, 0, NULL, dict_find(channel_users, argv[2]), TIMER_IGNORE_FUNC | TIMER_IGNORE_TIME);
		debug("Deleting chanfw user %s/%s", argv[1], argv[2]);
		dict_delete(channel_users, argv[2]);
	}
}

IRC_HANDLER(quit)
{
	dict_iter(node, users)
	{
		timer_del(this, TIMER_NAME, 0, NULL, dict_find(node->data, src->nick), TIMER_IGNORE_FUNC | TIMER_IGNORE_TIME);
		debug("Deleting chanfw user %s/%s", node->key, src->nick);
		dict_delete(node->data, src->nick);
	}
}

IRC_HANDLER(nick)
{
	dict_iter(chan_node, users)
	{
		dict_iter(user_node, (struct dict *)chan_node->data)
		{
			if(strcasecmp(user_node->key, src->nick))
				continue;
			struct chanfw_user *user = user_node->data;
			free(user->nick);
			user_node->key = user->nick = strdup(argv[1]);
			debug("Renaming chanfw user %s/%s -> %s", chan_node->key, src->nick, argv[1]);
		}
	}
}

void remove_user(void *bound, struct chanfw_user *data)
{
	struct dict *channel_users;
	struct chanreg *reg = chanreg_find(data->channel);
	assert(reg);

	const char *kickmsg = chanreg_setting_get(reg, cmod, "KickMsg");
	long ban_duration = atol(chanreg_setting_get(reg, cmod, "BanDuration"));
	if(kickmsg)
	{
		if(ban_duration > 0)
			irc_send("CHANSERV %s ADDTIMEDBAN %s %ld %s", data->channel, data->nick, ban_duration, kickmsg);
		else
			irc_send("CHANSERV %s KICKBAN %s %s", data->channel, data->nick, kickmsg);
	}

	if((channel_users = dict_find(users, data->channel)))
		dict_delete(channel_users, data->nick);
}

static void channel_complete_hook_tmr(void *bound, struct irc_channel *channel)
{
	if(!chanreg_module_active(cmod, channel->name))
		return;

	struct chanreg *reg = chanreg_find(channel->name);
	assert(reg);

	if(!chanreg_setting_get(reg, cmod, "JoinMsg"))
		return;

	dict_iter(node, channel->users)
	{
		struct irc_chanuser *chanuser = node->data;
		if(!strcasecmp(chanuser->user->nick, bot.nickname) || (chanuser->flags & MODE_OP))
			continue;

		debug("adding victim with nick %s, flags %d", chanuser->user->nick, chanuser->flags);
		chanfw_new_victim(reg, chanuser->user->nick, 0);
	}
}

static void channel_complete_hook(struct irc_channel *channel)
{
	// We must call it a second later so the channel_complete hook from the chanreg module got executed first
	// Otherwise chanreg_module_active() would return 0
	if(chanreg_find(channel->name))
		timer_add(this, "channel_complete", now, (timer_f *)channel_complete_hook_tmr, channel, 0);
}

static int number_validate(struct irc_source *src, const char *value)
{
	if(!aredigits(value))
	{
		reply("$b%s$b is not a valid number", value);
		return 0;
	}
	if(atol(value) > 3600)
	{
		reply("Maximum value for this setting is 1 hour (3600 seconds)");
		return 0;
	}
	return 1;
}

static unsigned int notification_flag(const char *name)
{
	if(!strcasecmp(name, "Query") || !strcasecmp(name, "PM"))
		return NOTIFY_PM;
	else if(!strcasecmp(name, "Channel"))
		return NOTIFY_CHANNEL;
	else if(!strcasecmp(name, "Notice"))
		return NOTIFY_NOTICE;
	else
		return 0;
}

static int notification_parse(unsigned int current_flags, const char *str, char **errptr)
{
	char *str_dup = strdup(str);
	int new_flags = 0;
	int modifier_found = -1;
	int flag;
	char *argv[8];
	int argc;

	argc = tokenize(str_dup, argv, sizeof(argv), ' ', 0);
	for(int i = 0; i < argc; i++)
	{
		if(*argv[i] == '+' || *argv[i] == '-')
		{
			if(modifier_found == 0)
			{
				if(errptr)
					*errptr = strdup("Cannot mix modifier and non-modifier arguments.");
				free(str_dup);
				return -1;
			}

			modifier_found = 1;

			if(!(flag = notification_flag(argv[i] + 1)))
			{
				if(errptr)
					asprintf(errptr, "Unknown value: $b%s$b", argv[i] + 1);
				free(str_dup);
				return -1;
			}

			if(*argv[i] == '-')
				current_flags &= ~flag;
			else
				current_flags |= flag;
		}
		else if(modifier_found == 1)
		{
			if(errptr)
				*errptr = strdup("Cannot mix modifier and non-modifier arguments.");
			free(str_dup);
			return -1;
		}
		else
		{
			modifier_found = 0;

			if(!(flag = notification_flag(argv[i])))
			{
				if(errptr)
					asprintf(errptr, "Unknown value: $b%s$b", argv[i] + 1);
				free(str_dup);
				return -1;
			}

			new_flags |= flag;
		}
	}

	if(modifier_found == 1)
		new_flags = current_flags;

	free(str_dup);
	return new_flags;
}

static int notification_validate(struct irc_source *src, const char *value)
{
	char *error = NULL;
	if(notification_parse(0, value, &error) < 0)
	{
		assert_return(error, 0);
		reply("Could not change setting: %s", error);
		free(error);
		return 0;
	}

	return 1;
}

static const char *notification_format(const char *value)
{
	static char buf[64];
	unsigned int flags = atoi(value);

	buf[0] = '\0';

	if(flags & NOTIFY_PM)
		strlcat(buf, "Query ", sizeof(buf));
	if(flags & NOTIFY_NOTICE)
		strlcat(buf, "Notice ", sizeof(buf));
	if(flags & NOTIFY_CHANNEL)
		strlcat(buf, "Channel", sizeof(buf));

	return buf;
}

static const char *notification_encode(const char *old_value, const char *value)
{
	static char flag_str[8];
	unsigned int flags, old_flags;

	old_flags = atoi(old_value);
	flags = notification_parse(old_flags, value, NULL);
	if(flags < 0)
		flags = old_flags;

	snprintf(flag_str, sizeof(flag_str), "%u", flags);
	return flag_str;
}

static const char *asterisk_null(const char *old_value, const char *value)
{
	if(!strcmp(value, "*"))
		return NULL;
	return value;
}

static const char *null_none(const char *value)
{
	return value ? value : "None";
}

static void chanfw_user_free(struct chanfw_user *user)
{
	free(user->nick);
	free(user->channel);
	free(user);
}

static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason)
{
	struct irc_channel *channel;
	if((channel = channel_find(reg->channel)))
		channel_complete_hook_tmr(this, channel);
	return 0;
}

static int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	struct dict *channel_users;

	if(!(channel_users = dict_find(users, reg->channel)))
		return 0;

	dict_iter(node, channel_users)
	{
		timer_del(this, TIMER_NAME, 0, NULL, node->data, TIMER_IGNORE_FUNC | TIMER_IGNORE_TIME);
		debug("Deleting chanfw user %s/%s", reg->channel, node->key);
	}

	dict_delete(users, reg->channel);
	return 0;
}

