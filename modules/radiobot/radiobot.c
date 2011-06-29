#include "global.h"
#include "module.h"
#include "radiobot.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "modules/help/help.h"
#include "modules/httpd/http.h"
#include "modules/sharedmem/sharedmem.h"
#include "modules/tools/tools.h"
#include "chanuser.h"
#include "irc.h"
#include "irc_handler.h"
#include "timer.h"
#include "conf.h"
#include "sock.h"
#include "stringbuffer.h"
#include "stringlist.h"
#include "list.h"
#include "database.h"
#include "table.h"
#include "versioning.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <json/json.h>
#include <sys/capability.h>
#include <libmemcached/memcached.h>

#define RRD_DEFAULT_COLORS "-c BACK#FFFFFF -c CANVAS#F3F3F3 -c SHADEA#C8C8C8 -c SHADEB#969696 -c GRID#8C8C8C -c MGRID#821E1E -c FONT#000000 -c FRAME#000000 -c ARROW#FF0000"

// %s: DJ
// %s: Show title
#define TOPIC_FMT	"-=={{ Radio eXodus }}=={{ OnAir: %s }}=={{ Showtitel: %s }}=={{ %s }}=={{ Befehle: *dj *stream *status *title *wunsch *gruss }}==-"
// only used if CACHE_STATS is defined:
#define STATS_DELAY	15
// #define CACHE_STATS
#define CMDSOCK_MAXLEN 10240
// duration after which a polling ajax request is finished
#define HTTP_POLL_DURATION 1800

MODULE_DEPENDS("commands", "help", "httpd", "sharedmem", "tools", NULL);

static struct
{
	unsigned int listeners_current;
	unsigned int listeners_max;
	unsigned int listeners_peak;
	unsigned int listeners_unique;
	unsigned int bitrate;
	char *title;
} stream_stats;

struct cmd_client
{
	struct sock *sock;
	unsigned int authed : 1;
	unsigned int readonly : 1;
	unsigned int dead : 1;
	unsigned int showinfo : 1;
};

struct rb_http_client
{
	struct http_client *client;
	char *clientname;
	char *clientver;
	char *nick;
	uint32_t playerState;
	char uuid[37];
	struct {
		char *version;
		char *url;
	} update;
};

DECLARE_LIST(cmd_client_list, struct cmd_client *)
IMPLEMENT_LIST(cmd_client_list, struct cmd_client *)
DECLARE_LIST(rb_http_client_list, struct rb_http_client *)
IMPLEMENT_LIST(rb_http_client_list, struct rb_http_client *)

HTTP_HANDLER(http_root);
HTTP_HANDLER(http_stream_info);
HTTP_HANDLER(http_stream_status);
HTTP_HANDLER(http_wish_greet);
PARSER_FUNC(mod_active);
IRC_HANDLER(nick);
COMMAND(stats_clients);
COMMAND(notify);
COMMAND(send_update);
COMMAND(setmod);
COMMAND(setsecmod);
COMMAND(setplaylist);
COMMAND(settitle);
COMMAND(sendsongtitle);
COMMAND(queuefull);
COMMAND(playlist);
COMMAND(dj);
COMMAND(stream);
COMMAND(schedule);
COMMAND(teamspeak);
COMMAND(status);
COMMAND(listener);
COMMAND(title);
COMMAND(peak);
COMMAND(wish);
COMMAND(greet);
COMMAND(wishgreet);
COMMAND(kicksrc);
static void radiobot_conf_reload();
static void radiobot_db_read(struct database *db);
static int radiobot_db_write(struct database *db);
static void cmdsock_event(struct sock *sock, enum sock_event event, int err);
static void cmdsock_read(struct sock *sock, char *buf, size_t len);
static void cmdsock_drop_client_tmr(void *bound, struct sock *sock);
static void kicksrc_sock_connect();
static void kicksrc_sock_event(struct sock *sock, enum sock_event event, int err);
static void kicksrc_sock_read(struct sock *sock, char *buf, size_t len);
static void kicksrc_sock_timeout(void *bound, void *data);
static void stats_sock_connect();
static void stats_sock_event(struct sock *sock, enum sock_event event, int err);
static void stats_sock_read(struct sock *sock, char *buf, size_t len);
static void stats_sock_timeout(void *bound, void *data);
static void stats_update_tmr(void *bound, void *data);
static void stats_received();
static void iterate_stats_elements(xmlNode *a_node);
static void stats_parse_row(const char *key, const char *value);
static void send_status(const char *nick);
static void send_showinfo(struct cmd_client *client);
static void show_updated();
static void show_updated_readonly();
static void rb_http_client_free(struct rb_http_client *client);
static void rb_http_client_list_del_client(struct rb_http_client_list *list, struct http_client *client);
static struct rb_http_client *rb_http_client_by_uuid(const char *uuid);
static struct rb_http_client *rb_http_client_by_httpclient(struct http_client *client);
static int rb_http_client_outdated(struct rb_http_client *rb_client);
static void http_stream_status_send(struct http_client *client, int timeout);
void set_current_title(const char *title);
const char *get_streamtitle();
static void shared_memory_changed(struct module *module, const char *key, void *old, void *new);
static void memcache_set(const char *key, time_t ttl, const char *format, ...);
static time_t check_queue_full();
static int in_wish_greet_channel(struct irc_user *user);
static const char *sanitize_nick(const char *raw_nick);
static unsigned int rrd_exists(const char *name);
static void rrd_update();
static void rrd_graph();

extern void nfqueue_init();
extern void nfqueue_fini();

static struct module *this;
static struct radiobot_conf radiobot_conf;
static struct database *radiobot_db = NULL;
static struct sock *kicksrc_sock = NULL;
static struct sock *stats_sock = NULL;
static struct sock *cmd_sock = NULL;
static struct cmd_client_list *cmd_clients;
static struct rb_http_client_list *http_clients;
static struct stringbuffer *stats_data;
static char *current_mod = NULL;
static char *current_mod_2 = NULL;
static char *current_playlist = NULL;
static char *current_streamtitle = NULL;
static char *current_title = NULL;
static char *current_show = NULL;
static char *status_nick = NULL;
static char *listener_nick = NULL;
static char *playlist_genre = NULL;
static int last_peak = -1;
static time_t last_peak_time;
static char *last_peak_mod = NULL;
static time_t queue_full = 0;
static radiobot_notify_func *notify_func = NULL;
static int nfqueue_available = 0;
static memcached_st *mc = NULL;

static struct http_handler handlers[] = {
	{ "/", http_root },
	{ "/stream-info", http_stream_info },
	{ "/stream-status", http_stream_status },
	{ "/wish-greet", http_wish_greet },
	{ NULL, NULL }
};

MODULE_INIT
{
	const char *str;

	this = self;

	help_load(this, "radiobot.help");

	LIBXML_TEST_VERSION

	stats_data = stringbuffer_create();
	cmd_clients = cmd_client_list_create();
	http_clients = rb_http_client_list_create();

	reg_conf_reload_func(radiobot_conf_reload);
	radiobot_conf_reload();
	stats_sock_connect();

	radiobot_db = database_create("radiobot", radiobot_db_read, radiobot_db_write);
	database_read(radiobot_db, 1);

	// We check for CAP_NET_ADMIN as the nfqueue system only works if this cap is set
	cap_t caps = cap_get_proc();
	if(caps)
	{
		cap_flag_value_t cap_value;
		if(cap_get_flag(caps, CAP_NET_ADMIN, CAP_EFFECTIVE, &cap_value) == 0 && cap_value == CAP_SET)
			nfqueue_available = 1;
		cap_free(caps);
	}

	log_append(LOG_INFO, "nfqueue available (CAP_NET_ADMIN set): %s", nfqueue_available ? "yes" : "no");

	// allow disabling nfqueue via config
	if(true_string(conf_get("radiobot/disable_nfqueue", DB_STRING)))
	{
		if(nfqueue_available)
			log_append(LOG_INFO, "nfqueue available but disabled by config");
		nfqueue_available = 0;
	}


	if(nfqueue_available)
		nfqueue_init();

	reg_irc_handler("NICK", nick);
	REG_COMMAND_RULE("mod_active", mod_active);

	http_handler_add_list(handlers);

	reg_shared_memory_changed_hook(shared_memory_changed);
	playlist_genre = (str = shared_memory_get("radioplaylist", "genre", NULL)) ? strdup(str) : NULL;
	debug("current playlist genre: %s", playlist_genre);

	DEFINE_COMMAND(this, "stats clients",	stats_clients,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "notify",		notify,		0, 0, "group(admins)");
	DEFINE_COMMAND(this, "send_update",	send_update,	3, 0, "group(admins)");
	DEFINE_COMMAND(this, "setmod",		setmod,		0, 0, "group(admins)");
	DEFINE_COMMAND(this, "setsecmod",	setsecmod,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "setplaylist",	setplaylist,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "settitle",	settitle,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "sendsongtitle",	sendsongtitle,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "queuefull",	queuefull,	0, 0, "group(admins)");
	DEFINE_COMMAND(this, "playlist",	playlist,	0, 0, "true");
	DEFINE_COMMAND(this, "dj",		dj,		0, 0, "true");
	DEFINE_COMMAND(this, "stream",		stream,		0, 0, "true");
	DEFINE_COMMAND(this, "schedule",	schedule,	0, 0, "true");
	DEFINE_COMMAND(this, "teamspeak",	teamspeak,	0, 0, "true");
	DEFINE_COMMAND(this, "status",		status,		0, 0, "true");
	DEFINE_COMMAND(this, "listener",	listener,	0, 0, "true");
	DEFINE_COMMAND(this, "title",		title,		0, 0, "group(admins)");
	DEFINE_COMMAND(this, "peak",		peak,		0, 0, "true");
	DEFINE_COMMAND(this, "wish",		wish,		0, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "greet",		greet,		0, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "wishgreet",	wishgreet,	0, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "kicksrc",		kicksrc,	0, CMD_LOG_HOSTMASK, "group(admins)");
}

MODULE_FINI
{
	unreg_shared_memory_changed_hook(shared_memory_changed);
	http_handler_del_list(handlers);
	command_rule_unreg("mod_active");
	unreg_irc_handler("NICK", nick);

	database_write(radiobot_db);
	database_delete(radiobot_db);

	if(nfqueue_available)
		nfqueue_fini();

	unreg_conf_reload_func(radiobot_conf_reload);
	if(mc)
		memcached_free(mc);

	timer_del_boundname(this, "kicksrc_timeout");
	timer_del_boundname(this, "stats_timeout");
	timer_del_boundname(this, "update_stats");
	timer_del_boundname(this, "http_poll_timeout");

	if(kicksrc_sock)
		sock_close(kicksrc_sock);
	if(stats_sock)
		sock_close(stats_sock);
	if(cmd_sock)
		sock_close(cmd_sock);

	for(unsigned int i = 0; i < cmd_clients->count; i++)
	{
		struct cmd_client *client = cmd_clients->data[i];
		sock_close(client->sock);
		free(client);
	}

	while(http_clients->count)
	{
		struct rb_http_client *client = http_clients->data[http_clients->count - 1];
		http_stream_status_send(client->client, 0);
	}

	cmd_client_list_free(cmd_clients);
	rb_http_client_list_free(http_clients);
	stringbuffer_free(stats_data);

	MyFree(current_mod);
	MyFree(current_mod_2);
	MyFree(current_playlist);
	MyFree(current_title);
	MyFree(current_streamtitle);
	MyFree(current_show);
	MyFree(status_nick);
	MyFree(listener_nick);
	MyFree(playlist_genre);
	MyFree(last_peak_mod);
	MyFree(stream_stats.title);

	xmlCleanupParser();
}

static void radiobot_conf_reload()
{
	char *str;

	// stream data for kicksrc
	str = conf_get("radiobot/stream_ip", DB_STRING);
	radiobot_conf.stream_ip = str ? str : "127.0.0.1";

	str = conf_get("radiobot/stream_port", DB_STRING);
	radiobot_conf.stream_port = str ? atoi(str) : 8000;

	str = conf_get("radiobot/stream_pass", DB_STRING);
	radiobot_conf.stream_pass = str ? str : "secret";

	// stream data for stats
	str = conf_get("radiobot/stream_ip_stats", DB_STRING);
	radiobot_conf.stream_ip_stats = str ? str : radiobot_conf.stream_ip;

	str = conf_get("radiobot/stream_port_stats", DB_STRING);
	radiobot_conf.stream_port_stats = str ? (unsigned int)atoi(str) : radiobot_conf.stream_port;

	str = conf_get("radiobot/stream_pass_stats", DB_STRING);
	radiobot_conf.stream_pass_stats = str ? str : radiobot_conf.stream_pass;

	// various stuff, mainly for json information interface
	str = conf_get("radiobot/site_url", DB_STRING);
	radiobot_conf.site_url = str ? str : "n/a";

	str = conf_get("radiobot/schedule_url", DB_STRING);
	radiobot_conf.schedule_url = str ? str : "n/a";

	str = conf_get("radiobot/teamspeak_url", DB_STRING);
	radiobot_conf.teamspeak_url = str ? str : "n/a";

	radiobot_conf.sanitize_nick_regexps = conf_get("radiobot/sanitize_nick_regexps", DB_STRINGLIST);

	str = conf_get("radiobot/stream_url", DB_STRING);
	radiobot_conf.stream_url = str ? str : "n/a";

	// stream urls listeners can use
	str = conf_get("radiobot/stream_url_pls", DB_STRING);
	radiobot_conf.stream_url_pls = str ? str : "n/a";

	str = conf_get("radiobot/stream_url_asx", DB_STRING);
	radiobot_conf.stream_url_asx = str ? str : "n/a";

	str = conf_get("radiobot/stream_url_ram", DB_STRING);
	radiobot_conf.stream_url_ram = str ? str : "n/a";

	str = conf_get("radiobot/radiochan", DB_STRING);
	radiobot_conf.radiochan = str;

	str = conf_get("radiobot/teamchan", DB_STRING);
	radiobot_conf.teamchan = str;

	// server for wishes/greets from website
	radiobot_conf.cmd_sock_host = conf_get("radiobot/cmd_sock_host", DB_STRING);
	radiobot_conf.cmd_sock_port = ((str = conf_get("radiobot/cmd_sock_port", DB_STRING)) ? atoi(str) : 0);
	radiobot_conf.cmd_sock_pass = conf_get("radiobot/cmd_sock_pass", DB_STRING);
	radiobot_conf.cmd_sock_read_pass = conf_get("radiobot/cmd_sock_read_pass", DB_STRING);

	if(radiobot_conf.cmd_sock_host && strlen(radiobot_conf.cmd_sock_host) == 0)
		radiobot_conf.cmd_sock_host = NULL;
	if(radiobot_conf.cmd_sock_pass && strlen(radiobot_conf.cmd_sock_pass) == 0)
		radiobot_conf.cmd_sock_pass = NULL;
	if(radiobot_conf.cmd_sock_read_pass && strlen(radiobot_conf.cmd_sock_read_pass) == 0)
		radiobot_conf.cmd_sock_read_pass = NULL;


	// rrdtool options
	str = conf_get("radiobot/rrd_enabled", DB_STRING);
	radiobot_conf.rrd_enabled = str ? true_string(str) : 0;

	str = conf_get("radiobot/rrdtool_path", DB_STRING);
	radiobot_conf.rrdtool_path = str ? str : "/usr/bin/rrdtool";

	str = conf_get("radiobot/rrd_dir", DB_STRING);
	radiobot_conf.rrd_dir = str ? str : ".";

	str = conf_get("radiobot/graph_dir", DB_STRING);
	radiobot_conf.graph_dir = str ? str : ".";

	// sidebar update
	str = conf_get("radiobot/gadget_update_url", DB_STRING);
	radiobot_conf.gadget_update_url = (str && *str) ? str : NULL;

	str = conf_get("radiobot/gadget_current_version", DB_STRING);
	radiobot_conf.gadget_current_version = (str && *str) ? str : NULL;

	// memcached
	str = conf_get("radiobot/memcached_config", DB_STRING);
	radiobot_conf.memcached_config = (str && *str) ? str : NULL;

	// special config-related actions
	if(cmd_sock)
	{
		sock_close(cmd_sock);
		cmd_sock = NULL;
	}

	if(radiobot_conf.cmd_sock_host && radiobot_conf.cmd_sock_port && radiobot_conf.cmd_sock_pass)
	{
		cmd_sock = sock_create(SOCK_IPV4, cmdsock_event, NULL);
		assert(cmd_sock);
		sock_bind(cmd_sock, radiobot_conf.cmd_sock_host, radiobot_conf.cmd_sock_port);
		if(sock_listen(cmd_sock, NULL) != 0)
			cmd_sock = NULL;
		else
			debug("Command listener started on %s:%d", radiobot_conf.cmd_sock_host, radiobot_conf.cmd_sock_port);
	}
	else
	{
		// Not listening anymore -> drop all clients
		for(unsigned int i = 0; i < cmd_clients->count; i++)
		{
			struct cmd_client *client = cmd_clients->data[i];
			sock_close(client->sock);
			cmd_client_list_del(cmd_clients, client);
			free(client);
			i--;
		}
	}

	if(mc)
	{
		memcached_free(mc);
		mc = NULL;
	}

	if(radiobot_conf.memcached_config)
		mc = memcached(radiobot_conf.memcached_config, strlen(radiobot_conf.memcached_config));
}

static void radiobot_db_read(struct database *db)
{
	char *str;

	// show info
	if((str = database_fetch(db->nodes, "showinfo/mod", DB_STRING)))
		current_mod = strdup(str);
	if((str = database_fetch(db->nodes, "showinfo/mod2", DB_STRING)))
		current_mod_2 = strdup(str);
	if((str = database_fetch(db->nodes, "showinfo/playlist", DB_STRING)))
		current_playlist = strdup(str);
	if((str = database_fetch(db->nodes, "showinfo/streamtitle", DB_STRING)))
		current_streamtitle = strdup(str);
	if((str = database_fetch(db->nodes, "showinfo/show", DB_STRING)))
		current_show = strdup(str);
	if((str = database_fetch(db->nodes, "showinfo/queue_full", DB_STRING)))
		queue_full = strtoul(str, NULL, 10);
	check_queue_full();

	// peak data
	if((str = database_fetch(db->nodes, "peak/listeners", DB_STRING)))
		last_peak = atoi(str);
	if((str = database_fetch(db->nodes, "peak/time", DB_STRING)))
		last_peak_time = strtoul(str, NULL, 10);
	if((str = database_fetch(db->nodes, "peak/mod", DB_STRING)))
		last_peak_mod = strdup(str);
}

static int radiobot_db_write(struct database *db)
{
	database_begin_object(db, "showinfo");
		if(current_mod)
			database_write_string(db, "mod", current_mod);
		if(current_mod_2)
			database_write_string(db, "mod2", current_mod_2);
		if(current_playlist)
			database_write_string(db, "playlist", current_playlist);
		if(current_streamtitle)
			database_write_string(db, "streamtitle", current_streamtitle);
		if(current_show)
			database_write_string(db, "show", current_show);
		database_write_long(db, "queue_full", check_queue_full());
	database_end_object(db);
	database_begin_object(db, "peak");
		if(last_peak > 0)
			database_write_long(db, "listeners", last_peak);
		if(last_peak_time)
			database_write_long(db, "time", last_peak_time);
		if(last_peak_mod)
			database_write_string(db, "mod", last_peak_mod);
	database_end_object(db);
	return 0;
}

void set_current_title(const char *title)
{
	MyFree(current_title);
	current_title = strdup(title);
	show_updated_readonly();
}

const char *get_streamtitle()
{
	return current_streamtitle ? current_streamtitle : "n/a";
}

static void shared_memory_changed(struct module *module, const char *key, void *old, void *new)
{
	if(strcmp(module->name, "radioplaylist") || strcmp(key, "genre") || !new)
		return;

	MyFree(playlist_genre);
	playlist_genre = strdup((const char *)new);

	// Is there a real mod? Don't do anything
	if(current_mod)
		return;

	MyFree(current_streamtitle);
	asprintf(&current_streamtitle, "Playlist [%s]", (const char *)new);
	MyFree(current_show);
	current_show = strdup(current_streamtitle);
	debug("Streamtitle changed -> %s", current_streamtitle);
	irc_send("TOPIC %s :" TOPIC_FMT, radiobot_conf.radiochan, "Playlist", current_streamtitle, radiobot_conf.site_url);
	show_updated();
}

static void memcache_set(const char *key, time_t ttl, const char *format, ...)
{
	va_list args;
	char buf[1024];
	memcached_return_t rc;

	if(!mc)
		return;

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	rc = memcached_set(mc, key, strlen(key), buf, strlen(buf), ttl, 0);
	if(rc != MEMCACHED_SUCCESS)
		rc = memcached_set(mc, key, strlen(key), buf, strlen(buf), ttl, 0); // retry
	if(rc != MEMCACHED_SUCCESS)
		log_append(LOG_WARNING, "memcached_set() failed: %s", memcached_strerror(mc, rc));
}

static time_t check_queue_full()
{
	if(now > queue_full)
		queue_full = 0;
	return queue_full;
}

static int in_wish_greet_channel(struct irc_user *user)
{
	struct irc_channel *chan;
	if((chan = channel_find(radiobot_conf.radiochan)) && channel_user_find(chan, user))
		return 1;
	else if((chan = channel_find(radiobot_conf.teamchan)) && channel_user_find(chan, user))
		return 1;
	return 0;
}

static const char *sanitize_nick(const char *raw_nick)
{
	static char nickbuf[64];
	char *nick;
	size_t nicklen;
	strlcpy(nickbuf, raw_nick, sizeof(nickbuf));
	nick = nickbuf;
	nicklen = strlen(nick);
	if(!strncasecmp(nick, "eX`", 3))
		nick += 3, nicklen -= 3;
	if(nicklen >= 4)
	{
		if(!strcasecmp(nick + nicklen - 4, "`off") || !strcasecmp(nick + nicklen - 4, "`afk") || !strcasecmp(nick + nicklen - 4, "`dnd"))
			nick[nicklen - 4] = '\0', nicklen -= 4;
		else if(nicklen >= 6 && !strcasecmp(nick + nicklen - 6, "`onAir"))
			nick[nicklen - 6] = '\0', nicklen -= 6;
		else if(nicklen >= 7 && !strncasecmp(nick + nicklen - 7, "`on", 3) && !strcasecmp(nick + nicklen - 3, "Air"))
			nick[nicklen - 7] = '\0', nicklen -= 7;
	}
	for(char *c = nick; *c; c++)
	{
		if(*c == '|' || *c == '`')
			memmove(c, c + 1, strlen(c));
	}

	return nick;
}

static time_t strtotime(const char *str)
{
	int hours, minutes;
	struct tm *tm;

	if(sscanf(str, "%2d:%2d", &hours, &minutes) != 2)
		return 0;

	tm = localtime(&now);
	if(hours < tm->tm_hour || (hours == tm->tm_hour && minutes < tm->tm_min))
		tm->tm_mday++;
	tm->tm_hour = hours;
	tm->tm_min = minutes;
	tm->tm_sec = 0;
	return mktime(tm);
}

HTTP_HANDLER(http_root)
{
	http_reply_redir("%s", radiobot_conf.site_url);
}

HTTP_HANDLER(http_stream_info)
{
	struct json_object *list;
	struct json_object *response = json_object_new_object();
	json_object_object_add(response, "stream", json_object_new_string(radiobot_conf.stream_url));
	json_object_object_add(response, "site", json_object_new_string(radiobot_conf.site_url));
	json_object_object_add(response, "schedule", json_object_new_string(radiobot_conf.schedule_url));
	json_object_object_add(response, "teamspeak", json_object_new_string(radiobot_conf.teamspeak_url));
	list = json_object_new_array();
	if(radiobot_conf.sanitize_nick_regexps)
		for(unsigned int i = 0; i < radiobot_conf.sanitize_nick_regexps->count; i++)
			json_object_array_add(list, json_object_new_string(radiobot_conf.sanitize_nick_regexps->data[i]));
	json_object_object_add(response, "sanitize_nick_regexps", list);

	http_reply_header("Content-Type", "application/json; charset=utf-8");
        http_reply_header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        http_reply_header("Cache-Control", "must-revalidate");
        http_reply_header("Pragma", "no-cache");
	http_reply_header("Access-Control-Allow-Origin", "*");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
}

static void rb_http_client_free(struct rb_http_client *client)
{
	MyFree(client->clientname);
	MyFree(client->clientver);
	MyFree(client->nick);
	MyFree(client->update.version);
	MyFree(client->update.url);
	free(client);
}

static void rb_http_client_list_del_client(struct rb_http_client_list *list, struct http_client *client)
{
	for(unsigned int i = 0; i < list->count; i++)
	{
		struct rb_http_client *rb_client = list->data[i];
		if(rb_client->client == client)
		{
			rb_http_client_list_del(list, rb_client);
			rb_http_client_free(rb_client);
			break;
		}
	}
}

static struct rb_http_client *rb_http_client_by_uuid(const char *uuid)
{
	if(!uuid || !*uuid)
		return NULL;

	for(unsigned int i = 0; i < http_clients->count; i++)
	{
		struct rb_http_client *rb_client = http_clients->data[i];
		if(!strcmp(rb_client->uuid, uuid))
			return rb_client;
	}

	return NULL;
}

static struct rb_http_client *rb_http_client_by_httpclient(struct http_client *client)
{
	if(!client)
		return NULL;

	for(unsigned int i = 0; i < http_clients->count; i++)
	{
		struct rb_http_client *rb_client = http_clients->data[i];
		if(rb_client->client == client)
			return rb_client;
	}

	return NULL;
}

static int rb_http_client_outdated(struct rb_http_client *rb_client)
{
	if(!radiobot_conf.gadget_current_version && !rb_client->update.version)
		return 0;
	return version_compare(rb_client->clientver, rb_client->update.version ? rb_client->update.version : radiobot_conf.gadget_current_version) == -1;
}

static void http_stream_status_send(struct http_client *client, int timeout)
{
	struct rb_http_client *rb_client = rb_http_client_by_httpclient(client);
	client->dead_callback = NULL;
	if(rb_client)
		rb_http_client_list_del(http_clients, rb_client);
	timer_del(this, "http_poll_timeout", 0, NULL, client, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);

	struct json_object *response = json_object_new_object();
	json_object_object_add(response, "timeout_seconds", json_object_new_int(HTTP_POLL_DURATION));
	json_object_object_add(response, "timeout", json_object_new_boolean(timeout));
	json_object_object_add(response, "mod", current_mod ? json_object_new_string(current_mod) : NULL);
	json_object_object_add(response, "mod2", current_mod_2 ? json_object_new_string(current_mod_2) : NULL);
	json_object_object_add(response, "show", current_show ? json_object_new_string(to_utf8(current_show)) : NULL);
	json_object_object_add(response, "song", current_title ? json_object_new_string(to_utf8(current_title)) : NULL);
	json_object_object_add(response, "listeners", json_object_new_int(stream_stats.listeners_current));
	json_object_object_add(response, "bitrate", json_object_new_int(stream_stats.bitrate));
	if(rb_client && rb_client->clientname && !strcmp(rb_client->clientname, "windows-sidebar") && rb_client->clientver)
	{
		if(!rb_http_client_outdated(rb_client))
			json_object_object_add(response, "update", NULL);
		else
		{
			struct json_object *update = json_object_new_object();
			if(rb_client->update.version && rb_client->update.url)
			{
				json_object_object_add(update, "version", json_object_new_string(rb_client->update.version));
				json_object_object_add(update, "url", json_object_new_string(rb_client->update.url));
			}
			else
			{
				json_object_object_add(update, "version", json_object_new_string(radiobot_conf.gadget_current_version));
				json_object_object_add(update, "url", json_object_new_string(radiobot_conf.gadget_update_url));
			}
			json_object_object_add(response, "update", update);
		}
	}

	http_reply_header("Content-Type", "application/json; charset=utf-8");
        http_reply_header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        http_reply_header("Cache-Control", "must-revalidate");
        http_reply_header("Pragma", "no-cache");
	http_reply_header("Access-Control-Allow-Origin", "*");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	if(client->delay)
		http_request_finalize(client);
	if(rb_client)
		rb_http_client_free(rb_client);
}

static void http_stream_status_timeout(void *bound, struct http_client *client)
{
	http_stream_status_send(client, 1);
}

static void http_stream_status_dead(struct http_client *client)
{
	rb_http_client_list_del_client(http_clients, client);
	timer_del(this, "http_poll_timeout", 0, NULL, client, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
}

HTTP_HANDLER(http_stream_status)
{
	struct rb_http_client *rb_client;
	char *str;
	struct dict *get_vars = http_parse_vars(client, HTTP_GET);
	int wait = true_string(dict_find(get_vars, "wait"));
	if(!wait)
	{
		http_stream_status_send(client, 0);
		dict_free(get_vars);
		return;
	}

	http_request_detach(client, NULL);
	client->dead_callback = http_stream_status_dead;
	timer_add(this, "http_poll_timeout", now + HTTP_POLL_DURATION, (timer_f *)http_stream_status_timeout, client, 0, 0);

	rb_client = malloc(sizeof(struct rb_http_client));
	memset(rb_client, 0, sizeof(struct rb_http_client));
	rb_client->client = client;
	if((str = dict_find(get_vars, "client")) && !strpbrk(str, "\r\n"))
		rb_client->clientname = strdup(str);
	if((str = dict_find(get_vars, "clientver")) && !strpbrk(str, "\r\n"))
		rb_client->clientver = strdup(str);
	if((str = dict_find(get_vars, "nick")) && !strpbrk(str, "\r\n"))
		rb_client->nick = strdup(str);
	if((str = dict_find(get_vars, "psh")) && !strpbrk(str, "\r\n"))
		rb_client->playerState = strtoul(str, NULL, 10);
	if((str = dict_find(get_vars, "uuid")) && !strpbrk(str, "\r\n"))
		strlcpy(rb_client->uuid, str, sizeof(rb_client->uuid));
	//debug("Client connected: %p %s %s", rb_client, rb_client->uuid, rb_client->nick);
	rb_http_client_list_add(http_clients, rb_client);
	dict_free(get_vars);
}

HTTP_HANDLER(http_wish_greet)
{
	struct dict *get_vars = http_parse_vars(client, HTTP_GET);
	struct json_object *response = json_object_new_object();

	const char *type = dict_find(get_vars, "type");
	const char *name = dict_find(get_vars, "name");
	const char *msg = dict_find(get_vars, "msg");
	struct rb_http_client *rb_client = rb_http_client_by_uuid(dict_find(get_vars, "uuid"));

	if(!type || !name || !msg || !*type || !*name || !*msg)
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "message", json_object_new_string("data_missing"));
		json_object_object_add(response, "type", type ? json_object_new_string(type) : NULL);
	}
	else if(strcasecmp(type, "wish") && strcasecmp(type, "greet"))
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "message", json_object_new_string("invalid_type"));
		json_object_object_add(response, "type", json_object_new_string(type));
	}
	else if(strpbrk(name, "\r\n") || strpbrk(msg, "\r\n"))
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "message", json_object_new_string("security_violation"));
		json_object_object_add(response, "type", json_object_new_string(type));
	}
	else if(!current_mod)
	{
		json_object_object_add(response, "success", json_object_new_boolean(0));
		json_object_object_add(response, "message", json_object_new_string("no_mod"));
		json_object_object_add(response, "type", json_object_new_string(type));
	}
	else
	{
		json_object_object_add(response, "success", json_object_new_boolean(1));
		json_object_object_add(response, "message", NULL);
		json_object_object_add(response, "type", json_object_new_string(type));
		json_object_object_add(response, "queue_full", json_object_new_int(check_queue_full()));

		if(rb_client)
			debug("Wish/Greet: %p %s %s %s %s", rb_client, rb_client->uuid, rb_client->nick, name, type);

		if(!strcasecmp(type, "wish"))
		{
			irc_send("PRIVMSG %s :Desktop-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod, name, msg);
			if(current_mod_2)
				irc_send("PRIVMSG %s :Desktop-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod_2, name, msg);
		}
		else if(!strcasecmp(type, "greet"))
		{
			irc_send("PRIVMSG %s :Desktop-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod, name, msg);
			if(current_mod_2)
				irc_send("PRIVMSG %s :Desktop-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod_2, name, msg);
		}
	}

	http_reply_header("Content-Type", "application/json; charset=utf-8");
        http_reply_header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        http_reply_header("Cache-Control", "must-revalidate");
        http_reply_header("Pragma", "no-cache");
	http_reply_header("Access-Control-Allow-Origin", "*");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	dict_free(get_vars);
}

PARSER_FUNC(mod_active)
{
	return current_mod ? RET_TRUE : RET_FALSE;
}


IRC_HANDLER(nick)
{
	assert(argc > 1);
	if(current_mod && !strcmp(current_mod, src->nick))
	{
		MyFree(current_mod);
		current_mod = strdup(argv[1]);
		database_write(radiobot_db);
	}

	if(current_mod_2 && !strcmp(current_mod_2, src->nick))
	{
		MyFree(current_mod_2);
		current_mod_2 = strdup(argv[1]);
		database_write(radiobot_db);
	}
}

COMMAND(stats_clients)
{
	struct table *table;

	if(!http_clients->count)
	{
		reply("Derzeit sind keine HTTP-Clients verbunden.");
		return 1;
	}

	table = table_create(7, http_clients->count);
	table_set_header(table, "IP", "UUID", "Nick", "Volume", "Playing", "Client", "Version");
	table->col_flags[3] |= TABLE_CELL_FREE | TABLE_CELL_ALIGN_RIGHT;
	table->col_flags[4] |= TABLE_CELL_FREE;

	for(unsigned int i = 0; i < http_clients->count; i++)
	{
		struct rb_http_client *client = http_clients->data[i];
		uint8_t playing = ((client->playerState >> 0) & 0x1);
		uint8_t muted = ((client->playerState >> 1) & 0x1);
		uint8_t volume = (client->playerState >> 2) & 0xff;
		uint32_t playtime = (client->playerState >> 10) & 0xfffff;
		table->data[i][0] = client->client->ip;
		table->data[i][1] = client->uuid;
		table->data[i][2] = client->nick ? strndupa(client->nick, 50) : "";
		if(client->playerState == 0) // assume no data is available
		{
			table_col_str(table, i, 3, strdup(""));
			table_col_str(table, i, 4, strdup(""));
		}
		else
		{
			table_col_fmt(table, i, 3, "%d", muted ? -volume : volume);
			if(playing)
				table_col_fmt(table, i, 4, "%02u:%02u:%02u", playtime / 3600, (playtime % 3600) / 60, (playtime % 3600) % 60);
			else
				table_col_str(table, i, 4, strdup("No"));
		}
		table->data[i][5] = client->clientname ? client->clientname : "";
		table->data[i][6] = client->clientver ? client->clientver : "";
	}

	table_send(table, src->nick);
	table_free(table);
	return 1;
}

COMMAND(notify)
{
	if(argc < 2)
	{
		show_updated_readonly();
		reply("All clients have been notified.");
		return 1;
	}
	else if(match("????????""-????""-????""-????""-????????????", argv[1]) == 0) // UUID
	{
		int found = 0;
		for(unsigned int i = 0; i < http_clients->count; i++)
		{
			struct rb_http_client *rb_client = http_clients->data[i];

			if(strcmp(rb_client->uuid, argv[1]))
				continue;

			reply("Notifying client %s %s %s", rb_client->uuid, rb_client->nick, rb_client->client->ip);
			debug("Notifying client %p %s %s", rb_client, rb_client->uuid, rb_client->nick);
			http_stream_status_send(rb_client->client, 0);
			i--;
			found++;
		}

		if(!found)
			reply("No clients with this UUID are currently connected.");
		return (found > 0);
	}

	reply("Unknown notification target.");
	return 0;
}

COMMAND(send_update)
{
	if(match("????????""-????""-????""-????""-????????????", argv[1]) == 0) // UUID
	{
		int found = 0;
		for(unsigned int i = 0; i < http_clients->count; i++)
		{
			struct rb_http_client *rb_client = http_clients->data[i];

			if(strcmp(rb_client->uuid, argv[1]))
				continue;

			MyFree(rb_client->update.version);
			MyFree(rb_client->update.url);
			rb_client->update.version = strdup(argv[2]);
			rb_client->update.url = strdup(argv[3]);
			reply("Offering update for client %s %s %s", rb_client->uuid, rb_client->nick, rb_client->client->ip);
			debug("Offering update for client %p %s %s", rb_client, rb_client->uuid, rb_client->nick);
			http_stream_status_send(rb_client->client, 0);
			i--;
			found++;
		}

		if(!found)
			reply("No clients with this UUID are currently connected.");
		return (found > 0);
	}

	reply("Unknown target.");
	return 0;
}

COMMAND(setmod)
{
	char *showtitle;
	unsigned int same_mod = 0;

	if(argc < 2)
	{
		reply("Aktueller Mod: $b%s$b (Showtitel: $b%s$b)", (current_mod ? current_mod : "[Playlist/Unknown]"), current_show);
		return 0;
	}

	showtitle = untokenize(argc - 1, argv + 1, " ");
	if(!strcasecmp(showtitle, src->nick))
	{
		reply("Der Showtitel sollte nicht genau deinem Nick entsprechen.");
		free(showtitle);
		return 0;
	}

	if(current_mod && !strcasecmp(src->nick, current_mod))
		same_mod = 1;

	if(current_mod_2)
	{
		reply("Zweitmoderator (%s) wurde gelöscht!", current_mod_2);
		MyFree(current_mod_2);
		current_mod_2 = NULL;
	}

	MyFree(current_mod);
	MyFree(current_show);

	if(!strcasecmp(showtitle, "Playlist"))
	{
		free(showtitle);
		if(playlist_genre)
			asprintf(&showtitle, "Playlist [%s]", playlist_genre);
		else
			showtitle = strdup("Playlist");
		MyFree(current_playlist);
	}
	else
	{
		current_mod = strdup(src->nick);
		if(notify_func && !same_mod)
			notify_func(&radiobot_conf, "setmod", current_mod, showtitle);
	}

	if(!same_mod)
		MyFree(current_playlist);

	current_show = showtitle;
	MyFree(current_streamtitle);
	current_streamtitle = strdup(showtitle);
	queue_full = 0;

	irc_send("TOPIC %s :" TOPIC_FMT, radiobot_conf.radiochan, (current_mod ? sanitize_nick(current_mod) : "Playlist"), current_show, radiobot_conf.site_url);
	irc_send("PRIVMSG %s :Mod geändert auf $b%s$b (Showtitel/Streamtitel: $b%s$b).", radiobot_conf.teamchan, (current_mod ? current_mod : "[Playlist]"), to_utf8(current_show));
	reply("Aktueller Mod: $b%s$b (Showtitel/Streamtitel: $b%s$b)", (current_mod ? current_mod : "[Playlist]"), current_show);
	database_write(radiobot_db);
	show_updated();
	show_updated_readonly();
	return 1;
}

COMMAND(setsecmod)
{
	if(argc < 2)
	{
		reply("Aktueller Zweitmoderator: $b%s$b", current_mod_2 ? current_mod_2 : "[niemand]");
		return 0;
	}

	if(!current_mod)
	{
		reply("Ein Zweitmoderator kann nur eingetragen werden wenn bereits ein Moderator aktiv ist");
		return 0;
	}

	MyFree(current_mod_2);
	current_mod_2 = !strcmp(argv[1], "*") ? NULL : strdup(argv[1]);
	irc_send("PRIVMSG %s :Zweitmod geändert auf $b%s$b", radiobot_conf.teamchan, (current_mod_2 ? current_mod_2 : "[niemand]"));
	reply("Aktueller Zweitmod: $b%s$b", current_mod_2 ? current_mod_2 : "[niemand]");
	database_write(radiobot_db);
	show_updated();
	show_updated_readonly();
	return 1;
}

COMMAND(setplaylist)
{
	if(argc < 2)
	{
		reply("Aktuelle Playlist: $b%s$b", (current_playlist ? current_playlist : "[keine]"));
		return 0;
	}

	MyFree(current_playlist);
	current_playlist = untokenize(argc - 1, argv + 1, " ");
	if(!strcmp(current_playlist, "*"))
		MyFree(current_playlist);

	if(current_playlist && !strcasecmp(current_playlist, "on"))
		irc_send("PRIVMSG %s :%s, bist du sicher, dass du die $bPlaylist-URL$b auf $b%s$b setzen wolltest, statt die Playlist einzuschalten?!", radiobot_conf.teamchan, src->nick, current_playlist);

	irc_send("PRIVMSG %s :Playlist geändert auf $b%s$b.", radiobot_conf.teamchan, (current_playlist ? current_playlist : "[Keine]"));
	reply("Aktuelle Playlist: $b%s$b", (current_playlist ? current_playlist : "[Keine]"));
	database_write(radiobot_db);
	show_updated();
	return 1;
}

COMMAND(settitle)
{
	if(argc < 2)
	{
		reply("Aktueller Streamsongtitel: $b%s$b", (current_streamtitle ? current_streamtitle : "n/a"));
		return 0;
	}

	MyFree(current_streamtitle);
	current_streamtitle = untokenize(argc - 1, argv + 1, " ");
	if(!strcmp(current_streamtitle, "*"))
	{
		MyFree(current_streamtitle);
		current_streamtitle = strdup(current_show);
	}

	irc_send("PRIVMSG %s :Streamsongtitel geändert auf $b%s$b.", radiobot_conf.teamchan, (current_streamtitle ? to_utf8(current_streamtitle) : "n/a"));
	reply("Aktueller Streamsongtitel: $b%s$b", (current_streamtitle ? current_streamtitle : "n/a"));
	database_write(radiobot_db);
	show_updated();
	return 1;
}

COMMAND(sendsongtitle)
{
	char *tmp;

	if(argc < 2)
	{
		reply("Aktueller Songtitel: $b%s$b", (current_title ? current_title : "n/a"));
		return 0;
	}

	tmp = untokenize(argc - 1, argv + 1, " ");
	set_current_title(tmp);
	free(tmp);

	irc_send("PRIVMSG %s :Songtitel geändert auf $b%s$b.", radiobot_conf.teamchan, to_utf8(current_title));
	reply("Aktueller Songtitel: $b%s$b", to_utf8(current_title));
	show_updated_readonly();
	return 1;
}

COMMAND(queuefull)
{
	char buf[32];

	if(argc < 2)
	{
		if(check_queue_full())
		{
			strftime(buf, sizeof(buf), "%H:%M", localtime(&queue_full));
			reply("Playlist voll bis: $b%s$b", buf);
		}
		else
			reply("Playlist voll bis: $bn/a$b");
		return 0;
	}

	if(!strcmp(argv[1], "*"))
	{
		queue_full = 0;
		reply("Playlist voll bis: $bn/a$b");
		database_write(radiobot_db);
		show_updated();
		return 1;
	}

	queue_full = strtotime(argv[1]);
	if(queue_full == 0)
	{
		reply("Syntax: $b%s hh:mm$b", argv[0]);
		return 0;
	}

	strftime(buf, sizeof(buf), "%d.%m.%Y, %H:%M", localtime(&queue_full));
	reply("Playlist voll bis: $b%s$b", buf);
	database_write(radiobot_db);
	show_updated();
	return 1;
}

COMMAND(playlist)
{
	reply("Aktuelle Playlist: $b%s$b", (current_playlist ? current_playlist : "[Keine]"));
	return 1;
}

COMMAND(dj)
{
	if(!current_mod)
		reply("Im Moment onAir: $b[Playlist]$b");
	else if(current_mod_2)
	{
		char buf[64];
		strlcpy(buf, sanitize_nick(current_mod_2), sizeof(buf));
		reply("Im Moment onAir: $b%s$b und $b%s$b", sanitize_nick(current_mod), buf);
	}
	else
		reply("Im Moment onAir: $b%s$b", sanitize_nick(current_mod));

	return 1;
}

COMMAND(stream)
{
	reply("Stream: $b%s$b | $b%s$b | $b%s$b", radiobot_conf.stream_url_pls, radiobot_conf.stream_url_asx, radiobot_conf.stream_url_ram);
	return 1;
}

COMMAND(schedule)
{
	reply("Sendeplan: $b%s$b", radiobot_conf.schedule_url);
	return 1;
}

COMMAND(teamspeak)
{
	reply("Teamspeak: $b%s$b", radiobot_conf.teamspeak_url);
	return 1;
}

#ifdef CACHE_STATS
COMMAND(status)
{
	if(argc > 1 && !status_nick)
	{
		MyFree(status_nick);
		status_nick = strdup(src->nick);
		stats_sock_connect();
		reply("Updating status...");
		return 1;
	}

	send_status(src->nick);
	return 1;
}

COMMAND(listener)
{
	if(argc > 1 && !listener_nick)
	{
		MyFree(listener_nick);
		listener_nick = strdup(src->nick);
		stats_sock_connect();
		reply("Updating status...");
		return 1;
	}

	reply("Listener: $b%d$b/%d (%d unique); Peak: %d", stream_stats.listeners_current, stream_stats.listeners_max, stream_stats.listeners_unique, stream_stats.listeners_peak);
	return 1;
}
#else
COMMAND(status)
{
	if(status_nick)
	{
		reply("Bitte warten - Streamstatus wird gerade aktualisiert.");
		return 0;
	}

	MyFree(status_nick);
	status_nick = strdup(src->nick);
	stats_sock_connect();
	return 1;
}

COMMAND(listener)
{
	if(listener_nick)
	{
		reply("Bitte warten - Streamstatus wird gerade aktualisiert.");
		return 0;
	}

	MyFree(listener_nick);
	listener_nick = strdup(src->nick);
	stats_sock_connect();
	return 1;
}
#endif

static void send_status(const char *nick)
{
	if(!current_mod)
		irc_send_msg(nick, "NOTICE", "Im Moment onAir: $b[Playlist]$b");
	else if(current_mod_2)
		irc_send_msg(nick, "NOTICE", "Im Moment onAir: $b%s$b und $b%s$b", current_mod, current_mod_2);
	else
		irc_send_msg(nick, "NOTICE", "Im Moment onAir: $b%s$b", current_mod);

	if(current_title)
		irc_send_msg(nick, "NOTICE", "Songtitel: $b%s$b", (current_title ? current_title : "n/a"));
	irc_send_msg(nick, "NOTICE", "Listener: $b%d$b/%d (%d unique); Peak: %d ~~~ Quali: %d kbps ~~~ %s", stream_stats.listeners_current, stream_stats.listeners_max, stream_stats.listeners_unique, stream_stats.listeners_peak, stream_stats.bitrate, stream_stats.title);
	irc_send_msg(nick, "NOTICE", "Stream: $b%s$b", radiobot_conf.stream_url_pls);
}

COMMAND(title)
{
	reply("Songtitel: $b%s$b", (current_title ? current_title : "n/a"));
	return 1;
}

COMMAND(peak)
{
	char str[32];

	if(last_peak <= 0 || !last_peak_mod)
	{
		reply("Noch kein Peak vorhanden.");
		return 0;
	}

	strftime(str, sizeof(str), "%d.%m.%Y, %H:%M", localtime(&last_peak_time));
	reply("Peak: $b%d$b Listener (von $b%s$b am $b%s$b)", last_peak, last_peak_mod, str);
	return 1;
}

COMMAND(wish)
{
	char *msg;

	if(!current_mod)
	{
		reply("Sorry, aber von der Playlist kannst du dir nichts wünschen.");
		return 0;
	}

	if(argc < 2)
	{
		reply("Du wünscht dir nichts? Falls doch, mach $b%s <hier dein wunsch>$b", argv[0]);
		return 0;
	}

	if(!in_wish_greet_channel(user))
	{
		reply("Bitte komm in unseren Channel $b%s$b um dir etwas zu wünschen.", radiobot_conf.radiochan);
		return 0;
	}

	msg = untokenize(argc - 1, argv + 1, " ");
	irc_send("PRIVMSG %s :IRC-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod, src->nick, msg);
	if(current_mod_2)
		irc_send("PRIVMSG %s :IRC-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod_2, src->nick, msg);
	if(check_queue_full())
	{
		char buf[32];
		strftime(buf, sizeof(buf), "%H:%M", localtime(&queue_full));
		reply("Dein Wunsch wurde weitergeleitet, allerdings ist die Playlist von %s bis $b%s$b voll.", current_mod, buf);
	}
	else
		reply("Dein Wunsch wurde weitergeleitet.");

	free(msg);
	return 1;
}

COMMAND(greet)
{
	char *msg;
	const char *msg_utf8;

	if(!current_mod)
	{
		reply("Sorry, aber die Playliste grüßt niemanden.");
		return 0;
	}

	if(argc < 2)
	{
		reply("Was sollen wir denn mit einem leeren Gruß?");
		return 0;
	}

	if(!in_wish_greet_channel(user))
	{
		reply("Bitte komm in unseren Channel $b%s$b um zu grüßen.", radiobot_conf.radiochan);
		return 0;
	}

	msg = untokenize(argc - 1, argv + 1, " ");
	msg_utf8 = to_utf8(msg);
	free(msg);

	irc_send("PRIVMSG %s :IRC-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod, src->nick, msg_utf8);
	if(current_mod_2)
		irc_send("PRIVMSG %s :IRC-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod_2, src->nick, msg_utf8);
	reply("Dein Gruß wurde weitergeleitet.");

	return 1;
}

COMMAND(wishgreet)
{
	char *msg;

	if(!current_mod)
	{
		reply("Sorry, aber die Playlist kann nicht grünschen.");
		return 0;
	}

	if(argc < 2)
	{
		reply("Du grünscht nichts? Falls doch, mach $b%s <hier dein grunsch>$b", argv[0]);
		return 0;
	}

	if(!in_wish_greet_channel(user))
	{
		reply("Bitte komm in unseren Channel $b%s$b um zu grünschen.", radiobot_conf.radiochan);
		return 0;
	}

	msg = untokenize(argc - 1, argv + 1, " ");
	irc_send("PRIVMSG %s :IRC-Grunsch von \0037$b$u%s$u$b\003: \0037$b%s$b\003", current_mod, src->nick, msg);
	if(current_mod_2)
		irc_send("PRIVMSG %s :IRC-Grunsch von \0037$b$u%s$u$b\003: \0037$b%s$b\003", current_mod_2, src->nick, msg);
	if(check_queue_full())
	{
		char buf[32];
		strftime(buf, sizeof(buf), "%H:%M", localtime(&queue_full));
		reply("Dein Grunsch wurde weitergeleitet, allerdings ist die Playlist von %s bis $b%s$b voll.", current_mod, buf);
	}
	else
		reply("Dein Grunsch wurde weitergeleitet.");

	free(msg);
	return 1;
}

COMMAND(kicksrc)
{
	if(argc < 2 || (strcasecmp(argv[1], "CONFIRM") && strcasecmp(argv[1], "FORCE")))
	{
		reply("Möchtest du den DJ (vermutlich ist das $b%s$b) wirklich vom Stream kicken? Wenn ja, verwende $b%s CONFIRM$b", (current_mod ? current_mod : "[Playlist]"), argv[0]);
		return 0;
	}

	if(kicksrc_sock)
	{
		reply("Der DJ wird gerade gekickt. Falls es nicht klappt, warte ein paar Sekunden und versuch es dann nochmal.");
		return 0;
	}

	kicksrc_sock_connect();

	reply("Wenn alles funktioniert, fliegt $b%s$b (oder wer auch immer gerade drauf ist) gleich vom Stream.", (current_mod ? current_mod : "[Playlist]"));
	return 1;
}

// cmd listener stuff
static void cmdsock_event(struct sock *sock, enum sock_event event, int err)
{
	if(sock == cmd_sock)
	{
		if(event == EV_ACCEPT)
		{
			struct sock *client_sock;
			if((client_sock = sock_accept(sock, cmdsock_event, cmdsock_read)))
			{
				struct cmd_client *client;
				debug("Accepted cmd connection from %s", inet_ntoa(((struct sockaddr_in *)client_sock->sockaddr_remote)->sin_addr));
				sock_set_readbuf(client_sock, CMDSOCK_MAXLEN, "\r\n");
				client = malloc(sizeof(struct cmd_client));
				memset(client, 0, sizeof(struct cmd_client));
				client->sock = client_sock;
				cmd_client_list_add(cmd_clients, client);
			}
		}
	}
	else
	{
		if(event == EV_ERROR || event == EV_HANGUP)
		{
			debug("Cmd client disconnected");
			for(unsigned int i = 0; i < cmd_clients->count; i++)
			{
				struct cmd_client *client = cmd_clients->data[i];
				if(client->sock == sock)
				{
					cmd_client_list_del(cmd_clients, client);
					free(client);
					i--;
				}
			}
		}
	}
}

static void cmdsock_read(struct sock *sock, char *buf, size_t len)
{
	struct cmd_client *client = NULL;
	char *argv[MAXARG];
	int argc;

	debug("Received line on cmd server socket: %s", buf);
	argc = tokenize(buf, argv, MAXARG, '\t', 0);

	for(unsigned int i = 0; i < cmd_clients->count; i++)
	{
		if(cmd_clients->data[i]->sock == sock)
		{
			client = cmd_clients->data[i];
			break;
		}
	}

	assert(client);

	if(client->dead)
		return;

	if(argc > 1 && !strcmp(argv[0], "PWD") && !strcmp(argv[1], radiobot_conf.cmd_sock_pass))
	{
		client->authed = 1;
		sock_write_fmt(sock, "SUCCESS\n");
		return;
	}

	if(argc > 1 && radiobot_conf.cmd_sock_read_pass && !strcmp(argv[0], "PWD") && !strcmp(argv[1], radiobot_conf.cmd_sock_read_pass))
	{
		client->readonly = 1;
		send_showinfo(client);
		return;
	}

	// Process commands for readonly clients and otherwise drop client unless he is authed
	if(client->readonly)
	{
		sock_write_fmt(sock, "ERR NOCMD :Unknown command\n");
		return;
	}
	else if(!client->authed)
	{
		sock_write_fmt(sock, "ERR WTF :What are you trying to tell me? I don't understand you!\n");
		client->dead = 1;
		// We need a timer so our goodbye messages gets delivered.
		timer_add(this, "drop_client", now + 2, (timer_f *)cmdsock_drop_client_tmr, sock, 0, 0);
		return;
	}

	// Process data from authed client
	if(argc > 2 && (!strcasecmp(argv[0], "WISH") || !strcasecmp(argv[0], "QWISH")))
	{
		if(!current_mod)
		{
			sock_write_fmt(sock, "ERR NOMOD :No mod active\n");
			return;
		}

		irc_send("PRIVMSG %s :%s-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod,
			 (!strcasecmp(argv[0], "QWISH") ? "QNet" : "Web"), argv[1], argv[2]);
		if(current_mod_2)
			irc_send("PRIVMSG %s :%s-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod_2,
				 (!strcasecmp(argv[0], "QWISH") ? "QNet" : "Web"), argv[1], argv[2]);
		sock_write_fmt(sock, "SUCCESS %lu\n", check_queue_full());
		return;
	}
	else if(argc > 2 && (!strcasecmp(argv[0], "GREET") || !strcasecmp(argv[0], "QGREET")))
	{
		const char *msg_utf8;

		if(!current_mod)
		{
			sock_write_fmt(sock, "ERR NOMOD :No mod active\n");
			return;
		}

		msg_utf8 = to_utf8(argv[2]);
		irc_send("PRIVMSG %s :%s-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod,
			 (!strcasecmp(argv[0], "QGREET") ? "QNet" : "Web"), argv[1], msg_utf8);
		if(current_mod_2)
			irc_send("PRIVMSG %s :%s-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod_2,
				 (!strcasecmp(argv[0], "QGREET") ? "QNet" : "Web"), argv[1], msg_utf8);
		sock_write_fmt(sock, "SUCCESS 0\n");
		return;
	}
	else if(argc > 1 && !strcasecmp(argv[0], "TEAMMSG"))
	{
		irc_send("PRIVMSG %s :%s", radiobot_conf.teamchan, argv[1]);
		sock_write_fmt(sock, "SUCCESS\n");
		return;
	}
	else if(argc > 0 && !strcasecmp(argv[0], "CURMOD"))
	{
		if(current_mod)
			sock_write_fmt(sock, "CURMOD %s\n", current_mod);
		else
			sock_write_fmt(sock, "NOMOD\n");
		return;
	}
	else if(argc > 0 && !strcasecmp(argv[0], "SHOWINFO"))
	{
		client->showinfo = 1;
		send_showinfo(client);
		return;
	}

	sock_write_fmt(sock, "ERR NOCMD :Unknown command\n");
}

static void send_showinfo(struct cmd_client *client)
{
	if(client->readonly)
	{
		sock_write_fmt(client->sock, "SHOWINFO_BEGIN\n");
		sock_write_fmt(client->sock, "SHOW_MOD %s\n", (current_mod ? sanitize_nick(current_mod) : "Playlist"));
		sock_write_fmt(client->sock, "SHOW_MOD2 %s\n", (current_mod_2 ? sanitize_nick(current_mod_2) : "*"));
		sock_write_fmt(client->sock, "SONGTITLE %s\n", (current_title ? current_title : "n/a"));
		sock_write_fmt(client->sock, "LISTENERS %d\n", stream_stats.listeners_current);
		sock_write_fmt(client->sock, "SHOWINFO_COMPLETE\n");
		return;
	}

	sock_write_fmt(client->sock, "SHOW_MOD %s\n", (current_mod ? sanitize_nick(current_mod) : "Playlist"));
	sock_write_fmt(client->sock, "SHOW_MOD2 %s\n", (current_mod_2 ? sanitize_nick(current_mod_2) : "*"));
	sock_write_fmt(client->sock, "SHOW_NAME %s\n", current_show);
	sock_write_fmt(client->sock, "SHOW_PLAYLIST %s\n", (current_playlist ? current_playlist : "[Keine]"));
	sock_write_fmt(client->sock, "SHOW_STREAMTITLE %s\n", get_streamtitle());
	sock_write_fmt(client->sock, "SHOW_QUEUE_FULL %lu\n", check_queue_full());
	sock_write_fmt(client->sock, "PEAK_LISTENERS %d\n", last_peak);
	sock_write_fmt(client->sock, "PEAK_TIME %lu\n", last_peak_time);
	sock_write_fmt(client->sock, "PEAK_MOD %s\n", (last_peak_mod ? last_peak_mod : "n/a"));
	sock_write_fmt(client->sock, "SHOWINFO_COMPLETE\n");
}

static void show_updated()
{
	for(unsigned int i = 0; i < cmd_clients->count; i++)
	{
		struct cmd_client *client = cmd_clients->data[i];
		if(client->dead || !client->authed || !client->showinfo)
			continue;
		send_showinfo(client);
	}
}

static void show_updated_readonly()
{
	for(unsigned int i = 0; i < cmd_clients->count; i++)
	{
		struct cmd_client *client = cmd_clients->data[i];
		if(client->dead || client->authed || !client->readonly)
			continue;
		send_showinfo(client);
	}

	for(unsigned int i = 0; i < http_clients->count; i++)
	{
		struct rb_http_client *client = http_clients->data[i];
		//debug("Notifying client %p %s %s", client, client->uuid, client->nick);
		http_stream_status_send(client->client, 0);
		i--;
	}

	memcache_set("radiobot.mod", 0, "%s", (current_mod ? sanitize_nick(current_mod) : ""));
	memcache_set("radiobot.mod2", 0, "%s", (current_mod_2 ? sanitize_nick(current_mod_2) : ""));
	memcache_set("radiobot.show", 0, "%s", (current_show ? to_utf8(current_show) : "Playlist"));
	memcache_set("radiobot.song", 0, "%s", (current_title ? to_utf8(current_title) : ""));
	memcache_set("radiobot.listeners", 0, "%u", stream_stats.listeners_current);
}

static void cmdsock_drop_client_tmr(void *bound, struct sock *sock)
{
	for(unsigned int i = 0; i < cmd_clients->count; i++)
	{
		struct cmd_client *client = cmd_clients->data[i];
		if(client->sock == sock)
		{
			sock_close(sock);
			cmd_client_list_del(cmd_clients, client);
			free(client);
			i--;
		}
	}
}


// kicksrc stuff
static void kicksrc_sock_connect()
{
	if(kicksrc_sock)
	{
		sock_close(kicksrc_sock);
		timer_del_boundname(this, "kicksrc_timeout");
	}

	kicksrc_sock = sock_create(SOCK_IPV4, kicksrc_sock_event, kicksrc_sock_read);
	assert(kicksrc_sock);

	if(sock_connect(kicksrc_sock, radiobot_conf.stream_ip, radiobot_conf.stream_port) != 0)
	{
		log_append(LOG_WARNING, "connect() to stream server (%s:%d) failed.", radiobot_conf.stream_ip, radiobot_conf.stream_port);
		kicksrc_sock = NULL;
		return;
	}

	sock_set_readbuf(kicksrc_sock, MAXLEN, "\r\n");
	timer_add(this, "kicksrc_timeout", now + 15, kicksrc_sock_timeout, NULL, 0, 0);
}

static void kicksrc_sock_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Socket error on kicksrc socket: %s (%d)", strerror(err), err);
		timer_del_boundname(this, "kicksrc_timeout");
		kicksrc_sock = NULL;
	}
	else if(event == EV_HANGUP)
	{
		debug("Kicksrc socket hung up");
		kicksrc_sock = NULL;
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "kicksrc_timeout");
		sock_write_fmt(sock, "GET /admin.cgi?pass=%s&mode=kicksrc HTTP/1.0\r\n", radiobot_conf.stream_pass);
		sock_write_fmt(sock, "User-agent: Mozilla compatible\r\n");
		sock_write_fmt(sock, "Host: %s\r\n", radiobot_conf.stream_ip);
		sock_write_fmt(sock, "Connection: close\r\n\r\n");
		irc_send("PRIVMSG %s :Mod wurde vom Stream gekickt.", radiobot_conf.teamchan);
	}
}

static void kicksrc_sock_read(struct sock *sock, char *buf, size_t len)
{
	debug("Received line on kicksrc socket: %s", buf);
}

static void kicksrc_sock_timeout(void *bound, void *data)
{
	log_append(LOG_WARNING, "Could not connect to stream server (timeout)");
	if(kicksrc_sock)
		sock_close(kicksrc_sock);
	kicksrc_sock = NULL;
}

// stats stuff
static void stats_sock_connect()
{
	if(stats_sock)
	{
		sock_close(stats_sock);
		timer_del_boundname(this, "stats_timeout");
		timer_del_boundname(this, "update_stats");
	}

	stats_sock = sock_create(SOCK_IPV4 | SOCK_QUIET, stats_sock_event, stats_sock_read);
	assert(stats_sock);

	stringbuffer_flush(stats_data);
	if(sock_connect(stats_sock, radiobot_conf.stream_ip_stats, radiobot_conf.stream_port_stats) != 0)
	{
		log_append(LOG_WARNING, "connect() to stream server (%s:%d) failed.", radiobot_conf.stream_ip_stats, radiobot_conf.stream_port_stats);
		stats_sock = NULL;
		timer_add(this, "stats_timeout", now + 60, stats_sock_timeout, NULL, 0, 0);
		return;
	}

	timer_add(this, "stats_timeout", now + 15, stats_sock_timeout, NULL, 0, 0);
}

static void stats_sock_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Socket error on stats socket: %s (%d)", strerror(err), err);
		timer_del_boundname(this, "stats_timeout");
		stats_sock = NULL;
		timer_add(this, "update_stats", now + 60, stats_update_tmr, NULL, 0, 0);

		if(status_nick)
		{
			irc_send_msg(status_nick, "NOTICE", "Fehler beim Statusabruf: %s", strerror(err));
			MyFree(status_nick);
		}

		if(listener_nick)
		{
			irc_send_msg(listener_nick, "NOTICE", "Fehler beim Statusabruf: %s", strerror(err));
			MyFree(listener_nick);
		}
	}
	else if(event == EV_HANGUP)
	{
		//debug("Stats socket hung up");
		stats_sock = NULL;
		stats_received();
		stringbuffer_flush(stats_data);
		timer_add(this, "update_stats", now + STATS_DELAY, stats_update_tmr, NULL, 0, 0);
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "stats_timeout");
		sock_write_fmt(sock, "GET /admin.cgi?pass=%s&mode=viewxml&page=0 HTTP/1.0\r\n", radiobot_conf.stream_pass_stats);
		sock_write_fmt(sock, "User-agent: Mozilla compatible\r\n");
		sock_write_fmt(sock, "Host: %s\r\n", radiobot_conf.stream_ip_stats);
		sock_write_fmt(sock, "Connection: close\r\n\r\n");
	}
}

static void stats_sock_read(struct sock *sock, char *buf, size_t len)
{
	stringbuffer_append_string(stats_data, buf);
}

static void stats_sock_timeout(void *bound, void *data)
{
	log_append(LOG_WARNING, "Could not connect to stream server (timeout)");
	if(status_nick)
	{
		irc_send_msg(status_nick, "NOTICE", "Fehler beim Statusabruf: Timeout");
		MyFree(status_nick);
	}

	if(listener_nick)
	{
		irc_send_msg(listener_nick, "NOTICE", "Fehler beim Statusabruf: Timeout");
		MyFree(listener_nick);
	}

	if(stats_sock)
		sock_close(stats_sock);
	stats_sock = NULL;

	timer_add(this, "update_stats", now + 120, stats_update_tmr, NULL, 0, 0);
}

static void stats_update_tmr(void *bound, void *data)
{
	stats_sock_connect();
}

#define HEADER_END "\r\n\r\n"
static void stats_received()
{
	unsigned int send_update = 0;

	// remove http headers
	char *data = strstr(stats_data->string, HEADER_END);
	if(data)
	{
		data += strlen(HEADER_END);
		if(*data == '\0')
			return;
	}

	// here the actual parsing starts
	xmlDocPtr doc;
	doc = xmlReadMemory(data, strlen(data), "stats.xml", "iso-8859-15", 0);
	if(!doc)
	{
		log_append(LOG_WARNING, "Could not parse xml stats");
		MyFree(status_nick);
		MyFree(listener_nick);
		return;
	}

	unsigned int listeners = stream_stats.listeners_current;
	xmlNode *root_element = xmlDocGetRootElement(doc);
	iterate_stats_elements(root_element);
	xmlFreeDoc(doc);

	if(listeners != stream_stats.listeners_current)
		send_update = 1;

	if(status_nick)
	{
		send_status(status_nick);
		MyFree(status_nick);
	}

	if(listener_nick)
	{
		irc_send_msg(listener_nick, "NOTICE", "Listener: $b%d$b/%d (%d unique); Peak: %d", stream_stats.listeners_current, stream_stats.listeners_max, stream_stats.listeners_unique, stream_stats.listeners_peak);
		MyFree(listener_nick);
	}

	if(last_peak == -1 || (int)stream_stats.listeners_peak > last_peak)
	{
		if(last_peak != -1)
			irc_send("PRIVMSG %s :$c4$b%s$b hat einen neuen Peak: $b%d$b$c", radiobot_conf.teamchan, (current_mod ? current_mod : "[Playlist]"), stream_stats.listeners_peak);
		last_peak = stream_stats.listeners_peak;
		last_peak_time = now;
		MyFree(last_peak_mod);
		last_peak_mod = strdup((current_mod ? current_mod : "[Playlist]"));
		show_updated();
		send_update = 1;
		database_write(radiobot_db);
	}

	if(send_update)
		show_updated_readonly();

	if(radiobot_conf.rrd_enabled)
	{
		rrd_update();
		rrd_graph();
	}
}

static void iterate_stats_elements(xmlNode *a_node)
{
	xmlNode *cur_node = NULL;

	for(cur_node = a_node; cur_node; cur_node = cur_node->next)
	{
		if(cur_node->type == XML_ELEMENT_NODE)
		{
			if(cur_node->children && cur_node->children->type == XML_TEXT_NODE)
				stats_parse_row((const char *)cur_node->name, (const char *)cur_node->children->content);
			else
				stats_parse_row((const char *)cur_node->name, NULL);
		}

		if(cur_node->type != XML_TEXT_NODE)
		{
			iterate_stats_elements(cur_node->children);
		}

	}
}

static void stats_parse_row(const char *key, const char *value)
{
	if(!value)
		value = "";

	if(!strcasecmp(key, "CURRENTLISTENERS"))
		stream_stats.listeners_current = atoi(value);
	else if(!strcasecmp(key, "PEAKLISTENERS"))
		stream_stats.listeners_peak = atoi(value);
	else if(!strcasecmp(key, "MAXLISTENERS"))
		stream_stats.listeners_max = atoi(value);
	else if(!strcasecmp(key, "REPORTEDLISTENERS"))
		stream_stats.listeners_unique = atoi(value);
	else if(!strcasecmp(key, "BITRATE"))
		stream_stats.bitrate = 192; // atoi(value);
	else if(!strcasecmp(key, "SONGTITLE"))
	{
		MyFree(stream_stats.title);
		stream_stats.title = strdup(value);
	}
}

// rrdtool stuff
static unsigned int rrd_exists(const char *name)
{
	char path[MAXLEN];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s.rrd", radiobot_conf.rrd_dir, name);
	if(!(fp = fopen(path, "r")))
		return 0;

	fclose(fp);
	return 1;
}

static void rrd_update()
{
	char str[MAXLEN];

	if(!rrd_exists("listeners"))
	{
		debug("Creating RRD for stream %s:%d", radiobot_conf.stream_ip_stats, radiobot_conf.stream_port_stats);
		snprintf(str, sizeof(str), "%s create %s/listeners.rrd --step 15"
				           "DS:listeners:GAUGE:600:U:U "
					   "DS:unique_listeners:GAUGE:600:U:U "
					   "RRA:AVERAGE:0.5:1:7200 "
					   "RRA:AVERAGE:0.5:5:4800 "
					   "RRA:AVERAGE:0.5:10:7200 "
					   "RRA:AVERAGE:0.5:60:6000 "
					   "RRA:AVERAGE:0.5:180:48000 "
					   "RRA:MAX:0.5:1:7200 "
					   "RRA:MAX:0.5:5:4800 "
					   "RRA:MAX:0.5:10:7200 "
					   "RRA:MAX:0.5:60:6000 "
					   "RRA:MAX:0.5:180:48000",
					   radiobot_conf.rrdtool_path, radiobot_conf.rrd_dir);
		system(str);
	}

	snprintf(str, sizeof(str), "%s update %s/listeners.rrd N:%u:%u", radiobot_conf.rrdtool_path, radiobot_conf.rrd_dir, stream_stats.listeners_current, stream_stats.listeners_unique);
	system(str);
}

static void rrd_graph_draw(const char *title, const char *file_suffix, const char *start)
{
	char str[4096];

	snprintf(str, sizeof(str),
		 "%1$s graph %2$s/listeners-%3$s.png" \
		 " %4$s" \
		 " -a PNG" \
		 " -h 200" \
		 " -s %5$s" \
		 " -v 'Listeners'" \
		 " -t '%6$s'" \
		 " DEF:listeners=%7$s/listeners.rrd:listeners:AVERAGE" \
		 " DEF:unique_listeners=%7$s/listeners.rrd:unique_listeners:AVERAGE" \
		 " AREA:listeners#FF0000:'Listeners'" \
		 " LINE2:unique_listeners#0000FF:'Unique Listeners'" \
		 " DEF:maxlisteners=%7$s/listeners.rrd:listeners:MAX" \
		 " DEF:maxunique_listeners=%7$s/listeners.rrd:unique_listeners:MAX" \
		 " COMMENT:'\\n'" \
		 " GPRINT:maxlisteners:MAX:'Listeners\\t\\t Max\\: %%.lf'" \
		 " GPRINT:listeners:AVERAGE:'   Average\\: %%.lf'" \
		 " GPRINT:listeners:LAST:'   Current\\: %%.lf\\l'" \
		 " COMMENT:'\\n'" \
		 " GPRINT:maxunique_listeners:MAX:'Unique Listeners\\t Max\\: %%.lf'" \
		 " GPRINT:unique_listeners:AVERAGE:'   Average\\: %%.lf'" \
		 " GPRINT:unique_listeners:LAST:'   Current\\: %%.lf\\l'" \
		 " > /dev/null",
		 radiobot_conf.rrdtool_path, radiobot_conf.graph_dir, file_suffix, RRD_DEFAULT_COLORS, start, title, radiobot_conf.rrd_dir);

	system(str);
}

static void rrd_graph()
{
	if(!rrd_exists("listeners"))
		return;

	rrd_graph_draw("Listeners (1 hour)", "hour", "-1hour");
	rrd_graph_draw("Listeners (3 hours)", "3hour", "-3hours");
	rrd_graph_draw("Listeners (1 day)", "day", "-1day");
	rrd_graph_draw("Listeners (1 week)", "week", "-1week");
	rrd_graph_draw("Listeners (1 month)", "month", "-1month");
	rrd_graph_draw("Listeners (1 year)", "year", "-1year");
}

void radiobot_set_notify_func(radiobot_notify_func *func)
{
	notify_func = func;
}
