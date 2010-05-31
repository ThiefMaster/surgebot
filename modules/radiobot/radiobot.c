#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "modules/help/help.h"
#include "modules/httpd/http.h"
#include "irc.h"
#include "irc_handler.h"
#include "timer.h"
#include "conf.h"
#include "sock.h"
#include "stringbuffer.h"
#include "list.h"
#include "database.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <json/json.h>

#define RRD_DEFAULT_COLORS "-c BACK#FFFFFF -c CANVAS#F3F3F3 -c SHADEA#C8C8C8 -c SHADEB#969696 -c GRID#8C8C8C -c MGRID#821E1E -c FONT#000000 -c FRAME#000000 -c ARROW#FF0000"

// %s: DJ
// %s: Show title
#define TOPIC_FMT	"-=={{ Radio eXodus }}=={{ OnAir: %s }}=={{ Showtitel: %s }}=={{ http://www.radio-eXodus.de }}=={{ Befehle: *dj *stream *status *wunsch *gruss }}==-"
// only used if CACHE_STATS is defined:
#define STATS_DELAY	15
// #define CACHE_STATS
#define CMDSOCK_MAXLEN 10240
// duration after which a polling ajax request is finished
#define HTTP_POLL_DURATION 1800

MODULE_DEPENDS("commands", "help", "httpd", NULL);

static struct
{
	const char *stream_ip;
	unsigned int stream_port;
	const char *stream_pass;
	const char *stream_ip_stats;
	unsigned int stream_port_stats;
	const char *stream_pass_stats;
	const char *stream_url;
	const char *stream_url_2;
	const char *stream_url_3;
	const char *radiochan;
	const char *teamchan;
	const char *cmd_sock_host;
	unsigned int cmd_sock_port;
	const char *cmd_sock_pass;
	const char *cmd_sock_read_pass;
	unsigned int rrd_enabled;
	const char *rrdtool_path;
	const char *rrd_dir;
	const char *graph_dir;
} radiobot_conf;

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

DECLARE_LIST(cmd_client_list, struct cmd_client *)
IMPLEMENT_LIST(cmd_client_list, struct cmd_client *)
DECLARE_LIST(http_client_list, struct http_client *)
IMPLEMENT_LIST(http_client_list, struct http_client *)

HTTP_HANDLER(http_root);
HTTP_HANDLER(http_stream_status);
PARSER_FUNC(mod_active);
IRC_HANDLER(nick);
COMMAND(setmod);
COMMAND(setplaylist);
COMMAND(settitle);
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
static void http_stream_status_send(struct http_client *client, int timeout);
void set_current_title(const char *title);
const char *get_streamtitle();
static time_t check_queue_full();
static unsigned int rrd_exists(const char *name);
static void rrd_update();
static void rrd_graph();

extern void nfqueue_init();
extern void nfqueue_fini();

static struct module *this;
static struct database *radiobot_db = NULL;
static struct sock *kicksrc_sock = NULL;
static struct sock *stats_sock = NULL;
static struct sock *cmd_sock = NULL;
static struct cmd_client_list *cmd_clients;
static struct http_client_list *http_clients;
static struct stringbuffer *stats_data;
static char *current_mod = NULL;
static char *current_playlist = NULL;
static char *current_streamtitle = NULL;
static char *current_title = NULL;
static char *current_show = NULL;
static char *status_nick = NULL;
static char *listener_nick = NULL;
static int last_peak = -1;
static time_t last_peak_time;
static char *last_peak_mod = NULL;
static time_t queue_full = 0;

static struct http_handler handlers[] = {
	{ "/", http_root },
	{ "/stream-status", http_stream_status },
	{ NULL, NULL }
};

MODULE_INIT
{
	this = self;

	help_load(this, "radiobot.help");

	LIBXML_TEST_VERSION

	stats_data = stringbuffer_create();
	cmd_clients = cmd_client_list_create();
	http_clients = http_client_list_create();

	reg_conf_reload_func(radiobot_conf_reload);
	radiobot_conf_reload();
	stats_sock_connect();

	radiobot_db = database_create("radiobot", radiobot_db_read, radiobot_db_write);
	database_read(radiobot_db, 1);

	nfqueue_init();

	reg_irc_handler("NICK", nick);
	REG_COMMAND_RULE("mod_active", mod_active);

	http_handler_add_list(handlers);

	DEFINE_COMMAND(this, "setmod",		setmod,		1, 0, "group(admins)");
	DEFINE_COMMAND(this, "setplaylist",	setplaylist,	1, 0, "group(admins)");
	DEFINE_COMMAND(this, "settitle",	settitle,	1, 0, "group(admins)");
	DEFINE_COMMAND(this, "queuefull",	queuefull,	1, 0, "group(admins)");
	DEFINE_COMMAND(this, "playlist",	playlist,	1, 0, "true");
	DEFINE_COMMAND(this, "dj",		dj,		1, 0, "true");
	DEFINE_COMMAND(this, "stream",		stream,		1, 0, "true");
	DEFINE_COMMAND(this, "schedule",	schedule,	1, 0, "true");
	DEFINE_COMMAND(this, "teamspeak",	teamspeak,	1, 0, "true");
	DEFINE_COMMAND(this, "status",		status,		1, 0, "true");
	DEFINE_COMMAND(this, "listener",	listener,	1, 0, "true");
	DEFINE_COMMAND(this, "title",		title,		1, 0, "group(admins)");
	DEFINE_COMMAND(this, "peak",		peak,		1, 0, "true");
	DEFINE_COMMAND(this, "wish",		wish,		1, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "greet",		greet,		1, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "kicksrc",		kicksrc,	1, CMD_LOG_HOSTMASK, "group(admins)");
}

MODULE_FINI
{
	http_handler_del_list(handlers);
	command_rule_unreg("mod_active");
	unreg_irc_handler("NICK", nick);

	database_write(radiobot_db);
	database_delete(radiobot_db);

	nfqueue_fini();

	unreg_conf_reload_func(radiobot_conf_reload);

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

	for(unsigned int i = 0; i < http_clients->count; i++)
	{
		struct http_client *client = http_clients->data[i];
		http_stream_status_send(client, 0);
	}

	cmd_client_list_free(cmd_clients);
	http_client_list_free(http_clients);
	stringbuffer_free(stats_data);

	MyFree(current_mod);
	MyFree(current_playlist);
	MyFree(current_title);
	MyFree(current_streamtitle);
	MyFree(current_show);
	MyFree(status_nick);
	MyFree(listener_nick);
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

	// url listeners can use
	str = conf_get("radiobot/stream_url", DB_STRING);
	radiobot_conf.stream_url = str ? str : "n/a";

	str = conf_get("radiobot/stream_url_2", DB_STRING);
	radiobot_conf.stream_url_2 = str ? str : "n/a";

	str = conf_get("radiobot/stream_url_3", DB_STRING);
	radiobot_conf.stream_url_3 = str ? str : "n/a";

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
}

static void radiobot_db_read(struct database *db)
{
	char *str;

	// show info
	if((str = database_fetch(db->nodes, "showinfo/mod", DB_STRING)))
		current_mod = strdup(str);
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

static time_t check_queue_full()
{
	if(now > queue_full)
		queue_full = 0;
	return queue_full;
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
	http_reply_redir("http://www.radio-exodus.de");

}

static void http_stream_status_send(struct http_client *client, int timeout)
{
	http_client_list_del(http_clients, client);
	timer_del(this, "http_poll_timeout", 0, NULL, client, TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);

	if(http_client_dead(client))
	{
		debug("Client is dead");
		return;
	}

	struct json_object *response = json_object_new_object();
	json_object_object_add(response, "timeout", json_object_new_boolean(timeout));
	json_object_object_add(response, "mod", current_mod ? json_object_new_string(current_mod) : NULL);
	json_object_object_add(response, "show", current_show ? json_object_new_string(current_show): NULL);
	json_object_object_add(response, "song", current_title ? json_object_new_string(current_title) : NULL);
	json_object_object_add(response, "listeners", json_object_new_int(stream_stats.listeners_current));
	json_object_object_add(response, "bitrate", json_object_new_int(stream_stats.bitrate));

	http_reply_header("Content-Type", "application/json");
	http_reply("%s", json_object_to_json_string(response));
	json_object_put(response);
	http_request_finalize(client);
}

static void http_stream_status_timeout(void *bound, struct http_client *client)
{
	http_stream_status_send(client, 1);
}

HTTP_HANDLER(http_stream_status)
{
	struct dict *get_vars = http_parse_vars(client, HTTP_GET);
	int wait = true_string(dict_find(get_vars, "wait"));
	dict_free(get_vars);
	http_request_detach(client, NULL);
	if(wait)
	{
		timer_add(this, "http_poll_timeout", now + HTTP_POLL_DURATION, (timer_f *)http_stream_status_timeout, client, 0, 1);
		http_client_list_add(http_clients, client);
	}
	else
	{
		http_stream_status_send(client, 0);
	}
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
	}
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

	MyFree(current_mod);
	MyFree(current_show);

	if(!strcasecmp(showtitle, "Playlist"))
	{
		free(showtitle);
		showtitle = strdup("Playlist");
		MyFree(current_playlist);
	}
	else
	{
		current_mod = strdup(src->nick);
	}

	if(!same_mod)
		MyFree(current_playlist);

	current_show = showtitle;
	MyFree(current_streamtitle);
	current_streamtitle = strdup(showtitle);
	queue_full = 0;

	irc_send("TOPIC %s :" TOPIC_FMT, radiobot_conf.radiochan, (current_mod ? current_mod : "Playlist"), current_show);
	irc_send("PRIVMSG %s :Mod geändert auf $b%s$b (Showtitel/Streamtitel: $b%s$b).", radiobot_conf.teamchan, (current_mod ? current_mod : "[Playlist]"), current_show);
	reply("Aktueller Mod: $b%s$b (Showtitel/Streamtitel: $b%s$b)", (current_mod ? current_mod : "[Playlist]"), current_show);
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

	irc_send("PRIVMSG %s :Streamsongtitel geändert auf $b%s$b.", radiobot_conf.teamchan, (current_streamtitle ? current_streamtitle : "n/a"));
	reply("Aktueller Streamsongtitel: $b%s$b", (current_streamtitle ? current_streamtitle : "n/a"));
	database_write(radiobot_db);
	show_updated();
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
	reply("Im Moment onAir: $b%s$b", (current_mod ? current_mod : "[Playlist]"));
	return 1;
}

COMMAND(stream)
{
	reply("Stream: $b%s$b / $b%s$b / $b%s$b", radiobot_conf.stream_url, radiobot_conf.stream_url_2, radiobot_conf.stream_url_3);
	return 1;
}

COMMAND(schedule)
{
	reply("Sendeplan: $bhttp://www.radio-exodus.de/schedule.php$b");
	return 1;
}

COMMAND(teamspeak)
{
	reply("Teamspeak: $bts.radio-exodus.de:8767$b");
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
	irc_send_msg(nick, "NOTICE", "Im Moment onAir: $b%s$b", (current_mod ? current_mod : "[Playlist]"));
	irc_send_msg(nick, "NOTICE", "Listener: $b%d$b/%d (%d unique); Peak: %d ~~~ Quali: %d kbps ~~~ %s", stream_stats.listeners_current, stream_stats.listeners_max, stream_stats.listeners_unique, stream_stats.listeners_peak, stream_stats.bitrate, stream_stats.title);
	irc_send_msg(nick, "NOTICE", "Stream: $b%s$b", radiobot_conf.stream_url);
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

	msg = untokenize(argc - 1, argv + 1, " ");
	irc_send("PRIVMSG %s :IRC-Wunsch von \0033$b$u%s$u$b\003: \0033$b%s$b\003", current_mod, src->nick, msg);
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

	msg = untokenize(argc - 1, argv + 1, " ");
	irc_send("PRIVMSG %s :IRC-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod, src->nick, msg);
	reply("Dein Gruß wurde weitergeleitet.");
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
		sock_write_fmt(sock, "SUCCESS %lu\n", check_queue_full());
		return;
	}
	else if(argc > 2 && (!strcasecmp(argv[0], "GREET") || !strcasecmp(argv[0], "QGREET")))
	{
		if(!current_mod)
		{
			sock_write_fmt(sock, "ERR NOMOD :No mod active\n");
			return;
		}

		irc_send("PRIVMSG %s :%s-Gruß von \0034$b$u%s$u$b\003: \0034$b%s$b\003", current_mod,
			 (!strcasecmp(argv[0], "QGREET") ? "QNet" : "Web"), argv[1], argv[2]);
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
		sock_write_fmt(client->sock, "SHOW_MOD %s\n", (current_mod ? current_mod : "Playlist"));
		sock_write_fmt(client->sock, "SONGTITLE %s\n", (current_title ? current_title : "n/a"));
		sock_write_fmt(client->sock, "LISTENERS %d\n", stream_stats.listeners_current);
		sock_write_fmt(client->sock, "SHOWINFO_COMPLETE\n");
		return;
	}

	sock_write_fmt(client->sock, "SHOW_MOD %s\n", (current_mod ? current_mod : "Playlist"));
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
		struct http_client *client = http_clients->data[i];
		http_stream_status_send(client, 0);
	}
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
		return;
	}

	timer_add(this, "stats_timeout", now + 15, stats_sock_timeout, NULL, 0, 1);
}

static void stats_sock_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Socket error on stats socket: %s (%d)", strerror(err), err);
		timer_del_boundname(this, "stats_timeout");
		stats_sock = NULL;
		timer_add(this, "update_stats", now + 60, stats_update_tmr, NULL, 0, 1);

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
		timer_add(this, "update_stats", now + STATS_DELAY, stats_update_tmr, NULL, 0, 1);
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

	timer_add(this, "update_stats", now + 120, stats_update_tmr, NULL, 0, 1);
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
		stream_stats.bitrate = 128; // atoi(value);
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
	rrd_graph_draw("Listeners (1 day)", "day", "-1day");
	rrd_graph_draw("Listeners (1 week)", "week", "-1week");
	rrd_graph_draw("Listeners (1 month)", "month", "-1month");
	rrd_graph_draw("Listeners (1 year)", "year", "-1year");
}
