#include "global.h"
#include "module.h"
#include "chanspy.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "modules/chanjoin/chanjoin.h"
#include "chanuser.h"
#include "database.h"
#include "irc.h"
#include "irc_handler.h"
#include "table.h"
#include "timer.h"
#include "conf.h"
#include "sock.h"
#include "list.h"

MODULE_DEPENDS("commands", "help", "chanjoin", NULL);

struct chanspy_client
{
	struct sock *sock;
	char *chan;
	unsigned int authed : 1;
	unsigned int dead : 1;
};

static struct
{
	const char *listen_host;
	unsigned int listen_port;
	const char *password;
} chanspy_conf;

DECLARE_LIST(chanspy_client_list, struct chanspy_client *)
IMPLEMENT_LIST(chanspy_client_list, struct chanspy_client *)

COMMAND(chanspy_add);
COMMAND(chanspy_rejoin);
COMMAND(chanspy_del);
COMMAND(chanspy_list);
IRC_HANDLER(privmsg);
IRC_HANDLER(notice);
IRC_HANDLER(mode);
IRC_HANDLER(join);
IRC_HANDLER(part);
IRC_HANDLER(kick);
static void user_del_hook(struct irc_user *user, unsigned int quit, const char *reason);
IRC_HANDLER(nick);
IRC_HANDLER(topic);
static void chanspy_conf_reload();
static void spy_db_read(struct database *db);
static int spy_db_write(struct database *db);
static void chanspy_server_event(struct sock *sock, enum sock_event event, int err);
static void chanspy_server_read(struct sock *sock, char *buf, size_t len);
static void chanspy_server_drop_client_tmr(void *bound, struct sock *sock);
static void chanspy_client_connect(struct chanspy *spy);
static void chanspy_client_event(struct sock *sock, enum sock_event event, int err);
static void chanspy_client_read(struct sock *sock, char *buf, size_t len);
static void chanspy_client_timeout(void *bound, struct chanspy *spy);
static void chanspy_client_schedule_reconnect(struct chanspy *spy, unsigned int wait);
static void chanspy_client_reconnect_tmr(void *bound, struct chanspy *spy);
static int sort_spies(const void *a_, const void *b_);
static struct chanspy *spy_add(const char *name, const char *channel, const char *target, int flags);
static struct chanspy *spy_find(const char *name);
static void spy_free(struct chanspy *spy);
static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason);
static void spy_notice(struct chanspy *spy, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void spy_gotmsg(const char *channel, unsigned int remote, unsigned int type, const char *fmt, ...)  PRINTF_LIKE(4, 5);
static char *modechar(const char *chan, const char *nick);

static struct module *this;
static struct database *chanspy_db = NULL;
static struct dict *spies;
static const char *spy_flags = "PANQmjpkqnt";
static char spy_flags_inverse[256];
static struct sock *listen_sock = NULL;
static struct chanspy_client_list *chanspy_clients;

MODULE_INIT
{
	this = self;

	spies = dict_create();
	dict_set_free_funcs(spies, NULL, (dict_free_f *)spy_free);

	memset(spy_flags_inverse, 0, sizeof(spy_flags_inverse));
	for(unsigned int i = 0; spy_flags[i]; i++)
		spy_flags_inverse[(unsigned char)spy_flags[i]] = i + 1;

	reg_irc_handler("PRIVMSG", privmsg);
	reg_irc_handler("NOTICE", notice);
	reg_irc_handler("MODE", mode);
	reg_irc_handler("JOIN", join);
	reg_irc_handler("PART", part);
	reg_irc_handler("KICK", kick);
	reg_user_del_hook(user_del_hook);
	reg_irc_handler("NICK", nick);
	reg_irc_handler("TOPIC", topic);

	help_load(self, "chanspy.help");
	DEFINE_COMMAND(self, "chanspy add",	chanspy_add,	4, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy rejoin",	chanspy_rejoin,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy del",	chanspy_del,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chanspy list",	chanspy_list,	0, CMD_REQUIRE_AUTHED, "group(admins)");

	chanspy_clients = chanspy_client_list_create();

	reg_conf_reload_func(chanspy_conf_reload);
	chanspy_conf_reload();

	chanspy_db = database_create("chanspy", spy_db_read, spy_db_write);
	database_read(chanspy_db, 1);
	database_set_write_interval(chanspy_db, 300);
}

MODULE_FINI
{
	database_write(chanspy_db);
	database_delete(chanspy_db);

	unreg_conf_reload_func(chanspy_conf_reload);

	if(listen_sock)
		sock_close(listen_sock);

	for(unsigned int i = 0; i < chanspy_clients->count; i++)
	{
		struct chanspy_client *client = chanspy_clients->data[i];
		sock_close(client->sock);
		if(client->chan)
			free(client->chan);
		free(client);
	}
	chanspy_client_list_free(chanspy_clients);

	unreg_irc_handler("PRIVMSG", privmsg);
	unreg_irc_handler("NOTICE", notice);
	unreg_irc_handler("MODE", mode);
	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("PART", part);
	unreg_irc_handler("KICK", kick);
	unreg_user_del_hook(user_del_hook);
	unreg_irc_handler("NICK", nick);
	unreg_irc_handler("TOPIC", topic);

	dict_free(spies);
}

static void chanspy_conf_reload()
{
	char *str;

	chanspy_conf.listen_host = conf_get("chanspy/listen_host", DB_STRING);
	chanspy_conf.listen_port = ((str = conf_get("chanspy/listen_port", DB_STRING)) ? atoi(str) : 0);
	chanspy_conf.password = conf_get("chanspy/password", DB_STRING);

	if(chanspy_conf.listen_host && strlen(chanspy_conf.listen_host) == 0)
		chanspy_conf.listen_host = NULL;
	if(chanspy_conf.password && strlen(chanspy_conf.password) == 0)
		chanspy_conf.password = NULL;

	if(listen_sock)
	{
		sock_close(listen_sock);
		listen_sock = NULL;
	}

	if(chanspy_conf.listen_host && chanspy_conf.listen_port && chanspy_conf.password)
	{
		listen_sock = sock_create(SOCK_IPV4, chanspy_server_event, NULL);
		assert(listen_sock);
		sock_bind(listen_sock, chanspy_conf.listen_host, chanspy_conf.listen_port);
		if(sock_listen(listen_sock, NULL) != 0)
			listen_sock = NULL;
		else
			debug("Chanspy listener started on %s:%d", chanspy_conf.listen_host, chanspy_conf.listen_port);

		// This is a bit dirty, but we don't want to show outdated status messages.
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(!strcasecmp(spy->last_error, "Listener disabled"))
				spy->last_error = "Not connected";
		}
	}
	else
	{
		// Not listening anymore -> drop all clients
		for(unsigned int i = 0; i < chanspy_clients->count; i++)
		{
			struct chanspy_client *client = chanspy_clients->data[i];
			sock_close(client->sock);
			chanspy_client_list_del(chanspy_clients, client);
			if(client->chan)
				free(client->chan);
			free(client);
			i--;
		}

		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(*spy->channel == '<' && spy->active)
			{
				spy->active = 0;
				spy->last_error = "Listener disabled";
				spy_notice(spy, "Chanspy server disabled");
			}
		}
	}
}


static void spy_db_read(struct database *db)
{
	struct dict *db_node;

	if((db_node = database_fetch(db->nodes, "spies", DB_OBJECT)) != NULL)
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			const char *name = rec->key;
			const char *channel = database_fetch(obj, "channel", DB_STRING);
			const char *target = database_fetch(obj, "target", DB_STRING);
			const char *flags_str = database_fetch(obj, "flags", DB_STRING);
			unsigned int flags;

			if(flags_str && (flags = atoi(flags_str)))
				spy_add(name, channel, target, flags);
		}
	}
}

static int spy_db_write(struct database *db)
{
	database_begin_object(db, "spies");
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;

			database_begin_object(db, spy->name);
				database_write_string(db, "channel", spy->channel);
				database_write_string(db, "target", spy->target);
				database_write_long(db, "flags", spy->flags);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

static void chanspy_server_event(struct sock *sock, enum sock_event event, int err)
{
	if(sock == listen_sock)
	{
		if(event == EV_ACCEPT)
		{
			struct sock *client_sock;
			if((client_sock = sock_accept(sock, chanspy_server_event, chanspy_server_read)))
			{
				struct chanspy_client *client;
				debug("Accepted chanspy connection from %s", inet_ntoa(((struct sockaddr_in *)client_sock->sockaddr_remote)->sin_addr));
				sock_set_readbuf(client_sock, MAXLEN, "\r\n");
				client = malloc(sizeof(struct chanspy_client));
				memset(client, 0, sizeof(struct chanspy_client));
				client->sock = client_sock;
				chanspy_client_list_add(chanspy_clients, client);
			}
		}
	}
	else
	{
		if(event == EV_ERROR || event == EV_HANGUP)
		{
			debug("Chanspy client disconnected");
			for(unsigned int i = 0; i < chanspy_clients->count; i++)
			{
				struct chanspy_client *client = chanspy_clients->data[i];
				if(client->sock == sock)
				{
					if(client->chan)
					{
						dict_iter(node, spies)
						{
							struct chanspy *spy = node->data;
							if((*spy->channel == '<' && !strcasecmp(spy->channel + 1, client->chan)))
							{
								spy->active = 0;
								spy->last_error = (event == EV_ERROR) ? strerror(err) : "Disconnected";
								spy_notice(spy, "Chanspy client disconnected: %s", ((event == EV_ERROR) ? strerror(err) : "Client hung up"));
							}
						}
					}

					chanspy_client_list_del(chanspy_clients, client);
					if(client->chan)
						free(client->chan);
					free(client);
					i--;
				}
			}
		}
	}
}

static void chanspy_server_read(struct sock *sock, char *buf, size_t len)
{
	struct chanspy_client *client = NULL;
	char *argv[MAXARG];
	int argc;

	debug("Received line on chanspy server socket: %s", buf);
	argc = itokenize(buf, argv, MAXARG, ' ', ':');

	for(unsigned int i = 0; i < chanspy_clients->count; i++)
	{
		if(chanspy_clients->data[i]->sock == sock)
		{
			client = chanspy_clients->data[i];
			break;
		}
	}

	assert(client);

	if(client->dead)
		return;

	if(argc > 1 && !strcmp(argv[0], "PWD") && !strcmp(argv[1], chanspy_conf.password))
	{
		client->authed = 1;
		return;
	}

	if(argc > 1 && !strcmp(argv[0], "CHAN") && IsSpySourceChannelName(argv[1]) && client->authed)
	{
		if(client->chan)
			free(client->chan);
		client->chan = strdup(argv[1]);

		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if((*spy->channel == '<' && !strcasecmp(spy->channel + 1, client->chan)))
			{
				spy->active = 1;
				spy_notice(spy, "Chanspy client connected from %s.", inet_ntoa(((struct sockaddr_in *)sock->sockaddr_remote)->sin_addr));
			}
		}

		return;
	}

	// Drop client unless he is authed
	if(!client->authed || !client->chan)
	{
		sock_write_fmt(sock, "What are you trying to tell me? I don't understand you!\n");
		client->dead = 1;
		// We need a timer so our goodbye messages gets delivered.
		timer_add(this, "drop_client", now + 2, (timer_f *)chanspy_server_drop_client_tmr, sock, 0, 0);
		return;
	}

	// Process data from authed client
	if(argc > 2 && !strcasecmp(argv[0], "SPY"))
	{
		spy_gotmsg(client->chan, 1, atoi(argv[1]), "%s", argv[2]);
		return;
	}
	else if(argc > 1 && !strcasecmp(argv[0], "MSG"))
	{
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(*spy->channel == '<' && !strcasecmp(spy->channel + 1, client->chan))
				spy_notice(spy, "%s", argv[1]);
		}
	}
}

static void chanspy_server_drop_client_tmr(void *bound, struct sock *sock)
{
	for(unsigned int i = 0; i < chanspy_clients->count; i++)
	{
		struct chanspy_client *client = chanspy_clients->data[i];
		if(client->sock == sock)
		{
			sock_close(sock);
			chanspy_client_list_del(chanspy_clients, client);
			if(client->chan)
				free(client->chan);
			free(client);
			i--;
		}
	}
}

static void chanspy_client_connect(struct chanspy *spy)
{
	if(spy->sock)
		sock_close(spy->sock);

	spy->sock = sock_create(SOCK_IPV4, chanspy_client_event, chanspy_client_read);
	assert(spy->sock);

	if(sock_connect(spy->sock, spy->target_host, spy->target_port) != 0)
	{
		log_append(LOG_WARNING, "connect() to chanspy server (%s:%d) failed.", spy->target_host, spy->target_port);
		spy->sock = NULL;
		chanspy_client_schedule_reconnect(spy, 15);
		return;
	}

	sock_set_readbuf(spy->sock, MAXLEN, "\r\n");
	timer_add(this, "chanspy_connect_timeout", now + 15, (timer_f *)chanspy_client_timeout, spy, 0, 0);
}

static void chanspy_client_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Chanspy socket error on socket %d: %s (%d)", sock->fd, strerror(err), err);
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(spy->sock == sock)
			{
				spy->sock = NULL;
				chanspy_client_schedule_reconnect(spy, 10);
				break;
			}
		}
	}
	else if(event == EV_HANGUP)
	{
		log_append(LOG_WARNING, "Chanspy socket %d hung up", sock->fd);
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(spy->sock == sock)
			{
				spy->sock = NULL;
				chanspy_client_schedule_reconnect(spy, 5);
				break;
			}
		}
	}
	else if(event == EV_CONNECT)
	{
		dict_iter(node, spies)
		{
			struct chanspy *spy = node->data;
			if(spy->sock == sock)
			{
				timer_del(this, "chanspy_connect_timeout", 0, NULL, spy, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
				sock_write_fmt(sock, "PWD %s\n", spy->target_pass);
				sock_write_fmt(sock, "CHAN %s\n", ((*spy->channel == '<') ? spy->channel + 1 : spy->channel));
				break;
			}
		}
	}
}

static void chanspy_client_read(struct sock *sock, char *buf, size_t len)
{
	debug("Received line on chanspy client socket: %s", buf);
}

static void chanspy_client_timeout(void *bound, struct chanspy *spy)
{
	log_append(LOG_WARNING, "Could not connect to chanspy server %s:%d; timeout.", spy->target_host, spy->target_port);
	sock_close(spy->sock);
	spy->sock = NULL;
	chanspy_client_schedule_reconnect(spy, 30);
}

static void chanspy_client_schedule_reconnect(struct chanspy *spy, unsigned int wait)
{
	timer_del(this, "chanspy_reconnect", 0, NULL, spy, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
	timer_del(this, "chanspy_connect_timeout", 0, NULL, spy, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
	timer_add(this, "chanspy_reconnect", now + wait, (timer_f *)chanspy_client_reconnect_tmr, spy, 0, 0);
}

static void chanspy_client_reconnect_tmr(void *bound, struct chanspy *spy)
{
	debug("Reconnecting to chanspy server %s:%d", spy->target_host, spy->target_port);
	chanspy_client_connect(spy);
}

static struct chanspy *spy_add(const char *name, const char *channel, const char *target, int flags)
{
	struct chanspy *spy = malloc(sizeof(struct chanspy));
	memset(spy, 0, sizeof(struct chanspy));

	spy->name = strdup(name);
	spy->channel = strdup(channel);
	spy->target = strdup(target);
	spy->flags = flags;
	if(*channel == '<')
	{
		spy->active = 0;
		spy->last_error = "Not connected";

		for(unsigned int i = 0; i < chanspy_clients->count; i++)
		{
			struct chanspy_client *client = chanspy_clients->data[i];
			if(client->chan && !strcasecmp(client->chan, channel + 1))
			{
				spy->active = 1;
				spy->last_error = "No Error";
				break;
			}
		}
	}
	else
	{
		spy->active = 1;
		spy->last_error = "No Error";
	}

	if(*channel != '<' && strcmp(channel, "*"))
		chanjoin_addchan(channel, this, name, cj_success, cj_error, spy, NULL, 0);

	if(*spy->target == '>')
	{
		char *target_dup, *colon, *slash;
		target_dup = strdup(target + 1);

		// Split at ':'
		assert_return(colon = strchr(target_dup, ':'), NULL);
		*colon = '\0';
		colon++;
		// Split at '/'
		assert_return(slash = strchr(colon, '/'), NULL);
		*slash = '\0';
		slash++;

		spy->target_host = strdup(target_dup);
		spy->target_port = atoi(colon);
		spy->target_pass = strdup(slash);
		free(target_dup);

		chanspy_client_connect(spy);
	}

	dict_insert(spies, spy->name, spy);
	return spy;
}

static struct chanspy *spy_find(const char *name)
{
	return dict_find(spies, name);
}

static void spy_free(struct chanspy *spy)
{
	if(spy->active && !reloading_module)
	{
		if(*spy->channel != '<' && strcmp(spy->channel, "*"))
			chanjoin_delchan(spy->channel, this, spy->name);
	}

	free(spy->name);
	free(spy->channel);
	free(spy->target);
	if(spy->target_host)
		free(spy->target_host);
	if(spy->target_pass)
		free(spy->target_pass);
	if(spy->sock)
		sock_close(spy->sock);
	timer_del(this, NULL, 0, NULL, spy, TIMER_IGNORE_NAME | TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
	free(spy);
}

static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time)
{
	struct chanspy *spy;
	assert((spy = spy_find(key)) == ctx);
	spy->active = 1;
	spy->last_error = "No Error";

	spy_notice(spy, "%s %s (%s) successfully; started spying.", (first_time ? "Joined" : "Rejoined"), spy->channel, spy->name);
}

static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason)
{
	struct chanspy *spy;
	assert((spy = spy_find(key)) == ctx);
	spy->active = 0;
	spy->last_error = reason;

	spy_notice(spy, "Chanspy on %s (%s) reported error: %s", spy->channel, spy->name, reason);
}

static void spy_notice(struct chanspy *spy, const char *fmt, ...)
{
	char buf[MAXLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if(*spy->target != '>') // Not a remote target
		irc_send_raw("NOTICE %s :%s", spy->target, buf);
	else if(spy->sock) // Remote target with a valid socket
		sock_write_fmt(spy->sock, "MSG :%s\n", buf);

}

static void spy_gotmsg(const char *channel, unsigned int remote, unsigned int type, const char *fmt, ...)
{
	char buf[MAXLEN];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	dict_iter(node, spies)
	{
		struct chanspy *spy = node->data;
		if(!(spy->flags & type))
			continue;

		// spy->channel is "<#channel" when the spy data is read from a socket
		if((!remote && !strcasecmp(spy->channel, channel)) || (remote && *spy->channel == '<' && !strcasecmp(spy->channel + 1, channel)))
		{
			if(*spy->target != '>') // Not a remote target
				irc_send_raw("PRIVMSG %s :[%s] %s", spy->target, channel, buf);
			else if(spy->sock) // Remote target and connected to it
				sock_write_fmt(spy->sock, "SPY %d :%s\n", type, buf);
		}
	}
}

static char *modechar(const char *chan, const char *nick)
{
	static char modechar[2];
	struct irc_channel *channel;
	struct irc_chanuser *chanuser;

	strcpy(modechar, "");
	if((channel = channel_find(chan)) && (chanuser = dict_find(channel->users, nick)))
	{
		if(chanuser->flags & MODE_OP)
			strcpy(modechar, "@");
		else if(chanuser->flags & MODE_VOICE)
			strcpy(modechar, "+");
	}

	return modechar;
}

IRC_HANDLER(privmsg)
{
	struct irc_user *user;
	assert(argc > 2);

	if(!strcasecmp(argv[1], bot.nickname))
	{
		user = user_find(src->nick);
		if(!strncasecmp(argv[2], "\001ACTION ", 8))
		{
			char *action = strdup(argv[2] + 8);
			action[strlen(action)-1] = '\0';
			if(user)
			{
				dict_iter(node, user->channels)
				{
					struct irc_chanuser *chanuser = node->data;
					spy_gotmsg(chanuser->channel->name, 0, CSPY_QUERY, "[PM] * %s %s", src->nick, action);
				}
			}

			if(!user || !user->account)
				spy_gotmsg("*", 0, CSPY_QUERY, "[PM] * %s %s", src->nick, action);

			free(action);
		}
		else
		{
			if(user)
			{
				dict_iter(node, user->channels)
				{
					struct irc_chanuser *chanuser = node->data;
					spy_gotmsg(chanuser->channel->name, 0, CSPY_QUERY, "[PM] <%s> %s", src->nick, argv[2]);
				}
			}

			if(!user || !user->account)
				spy_gotmsg("*", 0, CSPY_QUERY, "[PM] <%s> %s", src->nick, argv[2]);
		}
	}
	else if(IsChannelName(argv[1]))
	{
		if(!strncasecmp(argv[2], "\001ACTION ", 8))
		{
			char *action = strdup(argv[2] + 8);
			action[strlen(action)-1] = '\0';
			spy_gotmsg(argv[1], 0, CSPY_ACTION, "* %s%s %s", modechar(argv[1], src->nick), src->nick, action);
			free(action);
		}
		else
		{
			spy_gotmsg(argv[1], 0, CSPY_PRIVMSG, "<%s%s> %s", modechar(argv[1], src->nick), src->nick, argv[2]);
		}
	}
}

IRC_HANDLER(notice)
{
	assert(argc > 2);

	if(!src || (!IsChannelName(argv[1]) && (*argv[1] != '@' || !IsChannelName(argv[1] + 1))))
		return;

	if(IsChannelName(argv[1]))
		spy_gotmsg(argv[1], 0, CSPY_NOTICE, "-%s:%s- %s", src->nick, argv[1], argv[2]);
	else if(*argv[1] == '@' && IsChannelName(argv[1] + 1))
		spy_gotmsg((argv[1] + 1), 0, CSPY_NOTICE, "-%s:%s- %s", src->nick, argv[1], argv[2]);
}

IRC_HANDLER(mode)
{
	char *modestr;
	assert(argc > 2);
	modestr = untokenize(argc - 2, argv + 2, " ");
	spy_gotmsg(argv[1], 0, CSPY_MODE, "* %s sets mode: %s", src->nick, modestr);
	free(modestr);
}

IRC_HANDLER(join)
{
	assert(argc > 1);
	spy_gotmsg(argv[1], 0, CSPY_JOIN, "* Joins: %s (%s@%s)", src->nick, src->ident, src->host);
}

IRC_HANDLER(part)
{
	assert(argc > 1);
	if(argc > 2)
		spy_gotmsg(argv[1], 0, CSPY_PART, "* Parts: %s (%s@%s) (%s)", src->nick, src->ident, src->host, argv[2]);
	else
		spy_gotmsg(argv[1], 0, CSPY_PART, "* Parts: %s (%s@%s)", src->nick, src->ident, src->host);
}

IRC_HANDLER(kick)
{
	assert(argc > 3);
	spy_gotmsg(argv[1], 0, CSPY_KICK, "* %s was kicked by %s (%s)", argv[2], src->nick, argv[3]);
}

static void user_del_hook(struct irc_user *user, unsigned int quit, const char *reason)
{
	if(!quit)
		return;

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		spy_gotmsg(chanuser->channel->name, 0, CSPY_QUIT, "* Quits: %s%s (%s@%s) (%s)", modechar(chanuser->channel->name, user->nick), user->nick, user->ident, user->host, reason);
	}
}

IRC_HANDLER(nick)
{
	struct irc_user *user;
	assert(argc > 1);
	assert(user = user_find(argv[1])); // chanuser_irc handles it first -> the user is already renamed

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		spy_gotmsg(chanuser->channel->name, 0, CSPY_NICK, "* %s is now known as %s", src->nick, argv[1]);
	}
}

IRC_HANDLER(topic)
{
	assert(argc > 2);
	spy_gotmsg(argv[1], 0, CSPY_TOPIC, "* %s changes topic to '%s'", src->nick, argv[2]);
}


// argv[1]: spy name
// argv[2]: #from
// argv[3]: #target or nick
// argv[4]: flags (P = privmsg, A = ctcp action, N = notice, M = mode, J = join, L = part, K = kick, Q = quit, N = nick, T = topic, * = everything)
COMMAND(chanspy_add)
{
	struct chanspy *spy;
	char c;
	unsigned int pos, flags = 0;

	if((spy = spy_find(argv[1])))
	{
		reply("There is already a chanspy named $b%s$b.", spy->name);
		return 0;
	}

	if(!IsSpySourceChannelName(argv[2]) && (*argv[2] != '<' || !IsSpySourceChannelName(argv[2] + 1)))
	{
		reply("You must specify a valid source channel or <#channel to read from a socket.");
		return 0;
	}

	if(*argv[3] == '>')
	{
		char *target_dup, *colon, *slash;
		int port;
		target_dup = strdup(argv[3] + 1);
		if(!(colon = strchr(target_dup, ':')) || !(slash = strchr(colon + 1, '/')))
		{
			reply("Target $b%s$b does not look like ip:port/password.", target_dup);
			free(target_dup);
			return 0;
		}

		// Split at ':'
		*colon = '\0';
		colon++;

		// Split at '/'
		*slash = '\0';
		slash++;
		port = atoi(colon);

		if(port < 1 || port > 65535)
		{
			reply("$b%s$b does not look like a valid tcp port.", colon);
			free(target_dup);
			return 0;
		}

		if(strlen(slash) == 0)
		{
			reply("Password may not be empty.");
			free(target_dup);
			return 0;
		}

		free(target_dup);
	}
	else if(*argv[3] == '#' && !IsChannelName(argv[3]))
	{
		reply("You must specify a valid target channel or nick.");
		return 0;
	}
	else if(*argv[3] != '#' && !validate_string(argv[3], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890_-[]{}|^`", &c))
	{
		reply("You must specify a valid target channel or nick. $b%c$b doesn't look like something that belongs in a nickname.", c);
		return 0;
	}

	if(!strcmp(argv[4], "*"))
		flags = CSPY_ALL;
	else
	{
		for(unsigned int i = 0; argv[4][i]; i++)
		{
			if(!(pos = spy_flags_inverse[(unsigned char)argv[4][i]]))
			{
				reply("$b%c$b is not a valid chanspy flag (%s).", argv[4][i], spy_flags);
				return 0;
			}

			flags |= 1 << (pos - 1);
		}
	}

	// !IsChannelName at this position means that it's "*" or "<*"
	if(!IsChannelName(argv[2]) && (*argv[2] != '<' || !IsChannelName(argv[2] + 1)) && flags != CSPY_QUERY)
	{
		reply("You may only use '$bQ$b' (PMs) when setting the source channel to $b*$b.");
		return 0;
	}

	spy_add(argv[1], argv[2], argv[3], flags);
	reply("Chanspy for $b%s$b added. Check $b%s$b for status notices.", argv[2], argv[3]);
	return 1;
}

COMMAND(chanspy_rejoin)
{
	unsigned int found = 0;

	if(!IsChannelName(argv[1]))
	{
		reply("You must specify a valid channel name.");
		return 0;
	}

	dict_iter(node, spies)
	{
		struct chanspy *spy = node->data;

		if(!strcasecmp(spy->channel, argv[1]))
		{
			found = 1;
			chanjoin_addchan(spy->channel, this, spy->name, cj_success, cj_error, spy, NULL, 0);
		}
	}

	if(!found)
	{
		reply("No chanspy for $b%s$b found.", argv[1]);
		return 0;
	}
	else
	{
		reply("Trying to rejoin %s.", argv[1]);
		return 1;
	}
}

COMMAND(chanspy_del)
{
	struct chanspy *spy;

	if(!(spy = spy_find(argv[1])))
	{
		reply("There is no chanspy named $b%s$b.", argv[1]);
		return 0;
	}

	reply("Chanspy $b%s$b deleted.", spy->name);
	dict_delete(spies, spy->name);

	return 1;
}

COMMAND(chanspy_list)
{
	struct table *table;
	unsigned int i = 0;

	table = table_create(5, dict_size(spies));
	table_set_header(table, "Name", "Channel", "Target", "Flags", "State");

	dict_iter(node, spies)
	{
		char flags[32];
		unsigned int pos = 0;
		struct chanspy *spy = node->data;

		for(unsigned int ii = 0; spy_flags[ii]; ii++)
			if(spy->flags & (1 << ii))
				flags[pos++] = spy_flags[ii];
		flags[pos] = '\0';

		table->data[i][0] = spy->name;
		table->data[i][1] = spy->channel;
		table->data[i][2] = spy->target;
		table->data[i][3] = ((spy->flags == CSPY_ALL) ? "*" : strdupa(flags));
		table_col_str(table, i, 4, table->data[i][4] = spy->active ? "Active" : (char*)spy->last_error);
		i++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_spies);
	table_send(table, src->nick);
	table_free(table);

	return 1;
}

static int sort_spies(const void *a_, const void *b_)
{
	const char *name_a = (*((const char ***)a_))[0];
	const char *name_b = (*((const char ***)b_))[0];
	return strcasecmp(name_a, name_b);
}
