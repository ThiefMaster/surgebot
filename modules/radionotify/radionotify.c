#include "global.h"
#include "chanuser.h"
#include "conf.h"
#include "database.h"
#include "irc.h"
#include "irc_handler.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/perform/perform.h"
#include "modules/radiobot/radiobot.h"

MODULE_DEPENDS("commands", "perform", "radiobot", NULL);

static struct
{
	const char *debug_channel;
} radionotify_conf;

struct radionotify_reg
{
	char *channel;
	time_t registered;
	unsigned char join_later;
	unsigned char new;
	unsigned char dead;
	char *nick;
};

COMMAND(announce);
IRC_HANDLER(part);
IRC_HANDLER(invite);
static void radionotify_conf_reload();
static void radionotify_db_read(struct database *db);
static int radionotify_db_write(struct database *db);
static struct radionotify_reg *radionotify_reg_add(const char *channel);
static struct radionotify_reg *radionotify_reg_find(const char *channel);
static void radionotify_reg_free(struct radionotify_reg *reg);
static void channel_complete_hook(struct irc_channel *channel);
static void channel_del_hook(struct irc_channel *channel, const char *reason);
static void radionotify_join(struct radionotify_reg *reg);
static void perform_done();
static void irc_disconnected();
static void radiobot_notification(struct radiobot_conf *conf, const char *action, ...);

static struct module *this;
static struct database *radionotify_db = NULL;
static struct dict *regs;
static unsigned int ready_to_join = 0;

MODULE_INIT
{
	this = self;

	regs = dict_create();
	dict_set_free_funcs(regs, NULL, (dict_free_f *) radionotify_reg_free);

	DEFINE_COMMAND(self, "announce", announce, 2, CMD_REQUIRE_AUTHED | CMD_LOG_HOSTMASK, "group(admins)");

	reg_conf_reload_func(radionotify_conf_reload);
	reg_irc_handler("PART", part);
	reg_irc_handler("INVITE", invite);
	reg_channel_del_hook(channel_del_hook);
	reg_channel_complete_hook(channel_complete_hook);
	perform_func_reg("radionotify_join_channels", perform_done);
	reg_disconnected_func(irc_disconnected);
	radionotify_conf_reload();

	// Hack to determine if we are likely to be connected and ready to join
	if(dict_size(channel_dict()))
		ready_to_join = 1;

	radiobot_set_notify_func(radiobot_notification);

	radionotify_db = database_create("radionotify", radionotify_db_read, radionotify_db_write);
	database_read(radionotify_db, 1);
	database_set_write_interval(radionotify_db, 300);
}

MODULE_FINI
{
	database_write(radionotify_db);
	database_delete(radionotify_db);
	radiobot_set_notify_func(NULL);
	unreg_disconnected_func(irc_disconnected);
	perform_func_unreg("radionotify_join_channels");
	unreg_channel_del_hook(channel_del_hook);
	unreg_channel_complete_hook(channel_complete_hook);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("INVITE", invite);
	unreg_conf_reload_func(radionotify_conf_reload);
	dict_free(regs);
}

static void radionotify_conf_reload()
{
	radionotify_conf.debug_channel = conf_get("radionotify/debug_channel", DB_STRING);
}

static void radionotify_db_read(struct database *db)
{
	struct dict *db_node;
	struct stringlist *slist;

	if((db_node = database_fetch(db->nodes, "regs", DB_OBJECT)))
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			char *str;
			struct radionotify_reg *reg;
			const char *channel = rec->key;

			reg = radionotify_reg_add(channel);

			if((str = database_fetch(obj, "registered", DB_STRING)))
				reg->registered = strtoul(str, NULL, 10);
			if((str = database_fetch(obj, "nick", DB_STRING)))
				reg->nick = strdup(str);
		}
	}
}

static int radionotify_db_write(struct database *db)
{
	database_begin_object(db, "regs");
		dict_iter(node, regs)
		{
			struct radionotify_reg *reg = node->data;
			if(reg->new && !reg->join_later) // not joined yet
				continue;

			database_begin_object(db, reg->channel);
				database_write_long(db, "registered", reg->registered);
				if(reg->nick)
					database_write_string(db, "nick", reg->nick);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

static struct radionotify_reg *radionotify_reg_add(const char *channel)
{
	struct radionotify_reg *reg = malloc(sizeof(struct radionotify_reg));
	memset(reg, 0, sizeof(struct radionotify_reg));

	reg->channel = strdup(channel);
	reg->registered = time(NULL);

	radionotify_join(reg);
	dict_insert(regs, reg->channel, reg);
	return reg;
}

static struct radionotify_reg *radionotify_reg_find(const char *channel)
{
	return dict_find(regs, channel);
}

static void radionotify_reg_free(struct radionotify_reg *reg)
{
	if(!reg->dead && !reloading_module && channel_find(reg->channel))
		irc_send("PART %s", reg->channel);
	MyFree(reg->nick);
	free(reg->channel);
	free(reg);
}

static void channel_complete_hook(struct irc_channel *channel)
{
	struct radionotify_reg *reg = radionotify_reg_find(channel->name);
	if(!reg)
		return;

	reg->dead = 0;
	if(reg->new)
	{
		reg->new = 0;
		irc_send("PRIVMSG %s :Ich wurde von $b%s$b in diesen Channel eingeladen.", reg->channel, reg->nick);
		irc_send("PRIVMSG %s :Immer wenn ein Moderator bei Radio eXodus auf den Stream geht, werde ich es im Channel bekanntgeben.", reg->channel);
		irc_send("PRIVMSG %s :Unser Radiochannel ist #radio-exodus und unsere Website ist http://www.radio-exodus.de", reg->channel);
		irc_send("PRIVMSG %s :Um mich wieder loszuwerden, reicht ein Kick aus.", reg->channel);
	}
}

static void channel_del_hook(struct irc_channel *channel, const char *reason)
{
	struct radionotify_reg *reg = radionotify_reg_find(channel->name);
	if(!reg)
		return;

	if(radionotify_conf.debug_channel)
		irc_send("PRIVMSG %s :Unregistered channel: %s (%s)", radionotify_conf.debug_channel, channel->name, reason);

	// Kick or part
	reg->dead = 1;
	if(reason)
		dict_delete(regs, channel->name);
}

static void radionotify_join(struct radionotify_reg *reg)
{
	if(!ready_to_join)
	{
		reg->join_later = 1;
		return;
	}

	irc_send("JOIN %s", reg->channel);
	reg->join_later = 0;
}

static void perform_done()
{
	ready_to_join = 1;

	dict_iter(node, regs)
	{
		struct radionotify_reg *reg = node->data;
		if(reg->join_later)
			radionotify_join(reg);
	}
}

static void irc_disconnected()
{
	ready_to_join = 0;

	dict_iter(node, regs)
	{
		struct radionotify_reg *reg = node->data;
		reg->join_later = 1;
	}
}

IRC_HANDLER(part)
{
	struct radionotify_reg *reg;
	assert(argc > 1);

	if(strcmp(src->nick, "ChanServ"))
		return;

	if(argc < 3 || match("* registration expired.", argv[2]))
		return;

	if(!(reg = radionotify_reg_find(argv[1])))
		return;

	dict_delete(regs, reg->channel);
	if(radionotify_conf.debug_channel)
		irc_send("PRIVMSG %s :Unregistered channel: %s (expired)", radionotify_conf.debug_channel, argv[1]);
}

IRC_HANDLER(invite)
{
	struct radionotify_reg *reg;

	assert(argc > 2);

	// should never happen, you cannot invite someone into a channel where he already is
	if((reg = radionotify_reg_find(argv[2])))
	{
		log_append(LOG_WARNING, "Got INVITE for registered channel %s", reg->channel);
		dict_delete(regs, reg->channel);
	}

	reg = radionotify_reg_add(argv[2]);
	reg->new = 1;
	reg->nick = strdup(src->nick);
	if(radionotify_conf.debug_channel)
		irc_send("PRIVMSG %s :New channel: %s (by %s)", radionotify_conf.debug_channel, reg->channel, src->nick);
}

static void radiobot_notification(struct radiobot_conf *conf, const char *action, ...)
{
	struct stringlist *lines = stringlist_create();
	char msg[MAXLEN];
	va_list args;

	va_start(args, action);
	if(!strcmp(action, "setmod"))
	{
		const char *mod = va_arg(args, const char *);
		const char *show = va_arg(args, const char *);
		snprintf(msg, sizeof(msg), "$b%s$b ist jetzt bei Radio eXodus on Air: $b%s$b", mod, show);
		stringlist_add(lines, strdup(msg));
		snprintf(msg, sizeof(msg), "Zum Zuhören geh einfach auf $b%s$b oder besuch uns in $b%s$b", conf->site_url, conf->radiochan);
		stringlist_add(lines, strdup(msg));
	}
	va_end(args);

	if(!lines->count)
	{
		stringlist_free(lines);
		return;
	}

	dict_iter(node, regs)
	{
		struct radionotify_reg *reg = node->data;
		for(unsigned int i = 0; i < lines->count; i++)
			irc_send("PRIVMSG %s :%s", reg->channel, lines->data[i]);
	}

	stringlist_free(lines);
}

COMMAND(announce)
{
	dict_iter(node, regs)
	{
		struct radionotify_reg *reg = node->data;
		irc_send("PRIVMSG %s :[$bANNOUNCEMENT$b from $b%s$b] %s", reg->channel, src->nick, argline + (argv[1] - argv[0]));
	}

	return 1;
}
