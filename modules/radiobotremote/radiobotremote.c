#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
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

// %s: DJ
// %s: Show title
#define TOPIC_FMT	"-=={{ Radio eXodus }}=={{ OnAir: %s }}=={{ Showtitel: %s }}=={{ http://www.radio-eXodus.de }}=={{ Befehle: *dj *stream *status *wunsch *gruss }}==-"
// only used if CACHE_STATS is defined:
#define STATS_DELAY	15
// #define CACHE_STATS
#define CMDSOCK_MAXLEN 10240

MODULE_DEPENDS("commands", NULL);

static struct
{
	const char *stream_ip_stats;
	unsigned int stream_port_stats;
	const char *stream_pass_stats;
	const char *stream_url;
	const char *radiochan;
	const char *autoop_host;
	const char *cmd_sock_host;
	unsigned int cmd_sock_port;
	const char *cmd_sock_pass;
} radiobotremote_conf;

static struct
{
	unsigned int listeners_current;
	unsigned int listeners_max;
	unsigned int listeners_peak;
	unsigned int listeners_unique;
	unsigned int bitrate;
	char *title;
} stream_stats;

IRC_HANDLER(join);
COMMAND(playlist);
COMMAND(dj);
COMMAND(stream);
COMMAND(schedule);
COMMAND(teamspeak);
COMMAND(status);
COMMAND(listener);
COMMAND(peak);
COMMAND(wish);
COMMAND(greet);
static void radiobotremote_conf_reload();
static void stats_sock_connect();
static void stats_sock_event(struct sock *sock, enum sock_event event, int err);
static void stats_sock_read(struct sock *sock, char *buf, size_t len);
static void stats_sock_timeout(void *bound, void *data);
#ifdef CACHE_STATS
static void stats_update_tmr(void *bound, void *data);
#endif
static void cmd_client_connect();
static void cmd_client_event(struct sock *sock, enum sock_event event, int err);
static void cmd_client_read(struct sock *sock, char *buf, size_t len);
static void cmd_client_timeout(void *bound, void *data);
static void cmd_client_schedule_reconnect(unsigned int wait);
static void cmd_client_reconnect_tmr(void *bound, void *data);
static void stats_received();
static void iterate_stats_elements(xmlNode *a_node);
static void stats_parse_row(const char *key, const char *value);
static void send_status(const char *nick);

static struct module *this;
static struct sock *stats_sock = NULL;
static struct sock *cmd_sock = NULL;
static struct stringbuffer *stats_data;
static char *current_mod = NULL;
static char *current_playlist = NULL;
static char *current_streamtitle = NULL;
static char *current_show = NULL;
static char *status_nick = NULL;
static char *listener_nick = NULL;
static unsigned int cmd_sock_active = 0;
static int last_peak = -1;
static time_t last_peak_time;
static char *last_peak_mod = NULL;
static time_t queue_full = 0;


MODULE_INIT
{
	this = self;

	LIBXML_TEST_VERSION

	stats_data = stringbuffer_create();

	reg_conf_reload_func(radiobotremote_conf_reload);
	radiobotremote_conf_reload();
	stats_sock_connect();

	reg_irc_handler("JOIN", join);

	DEFINE_COMMAND(this, "playlist",	playlist,	0, 0, "true");
	DEFINE_COMMAND(this, "dj",		dj,		0, 0, "true");
	DEFINE_COMMAND(this, "stream",		stream,		0, 0, "true");
	DEFINE_COMMAND(this, "schedule",	schedule,	0, 0, "true");
	DEFINE_COMMAND(this, "teamspeak",	teamspeak,	0, 0, "true");
	DEFINE_COMMAND(this, "status",		status,		0, 0, "true");
	DEFINE_COMMAND(this, "listener",	listener,	0, 0, "true");
	DEFINE_COMMAND(this, "peak",		peak,		0, 0, "true");
	DEFINE_COMMAND(this, "wish",		wish,		0, CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(this, "greet",		greet,		0, CMD_LOG_HOSTMASK, "true");
}

MODULE_FINI
{
	unreg_irc_handler("JOIN", join);
	unreg_conf_reload_func(radiobotremote_conf_reload);

	timer_del_boundname(this, "stats_timeout");
#ifdef CACHE_STATS
	timer_del_boundname(this, "update_stats");
#endif
	timer_del_boundname(this, "cmd_client_reconnect");
	timer_del_boundname(this, "cmd_client_connect_timeout");

	if(stats_sock)
		sock_close(stats_sock);
	if(cmd_sock)
		sock_close(cmd_sock);

	stringbuffer_free(stats_data);

	MyFree(current_mod);
	MyFree(current_playlist);
	MyFree(current_streamtitle);
	MyFree(current_show);
	MyFree(status_nick);
	MyFree(listener_nick);
	MyFree(last_peak_mod);
	MyFree(stream_stats.title);

	xmlCleanupParser();
}

static void radiobotremote_conf_reload()
{
	char *str;

	// stream data for stats
	str = conf_get("radiobotremote/stream_ip_stats", DB_STRING);
	radiobotremote_conf.stream_ip_stats = str ? str : "127.0.0.1";

	str = conf_get("radiobotremote/stream_port_stats", DB_STRING);
	radiobotremote_conf.stream_port_stats = str ? atoi(str) : 8000;

	str = conf_get("radiobotremote/stream_pass_stats", DB_STRING);
	radiobotremote_conf.stream_pass_stats = str ? str : "secret";

	// url listeners can use
	str = conf_get("radiobotremote/stream_url", DB_STRING);
	radiobotremote_conf.stream_url = str ? str : "n/a";

	str = conf_get("radiobotremote/radiochan", DB_STRING);
	radiobotremote_conf.radiochan = str;

	str = conf_get("radiobotremote/autoop_host", DB_STRING);
	radiobotremote_conf.autoop_host = str;

	// client for connection to main bot
	str = conf_get("radiobotremote/cmd_sock_host", DB_STRING);
	radiobotremote_conf.cmd_sock_host = str ? str : "127.0.0.1";

	str = conf_get("radiobotremote/cmd_sock_port", DB_STRING);
	radiobotremote_conf.cmd_sock_port = str ? atoi(str) : 45678;

	str = conf_get("radiobotremote/cmd_sock_pass", DB_STRING);
	radiobotremote_conf.cmd_sock_pass = str ? str : "secret";

	if(cmd_sock)
	{
		sock_close(cmd_sock);
		cmd_sock = NULL;
	}

	cmd_client_connect();
}

static time_t check_queue_full()
{
	if(now > queue_full)
		queue_full = 0;
	return queue_full;
}

IRC_HANDLER(join)
{
	assert(argc > 1);

	if(!radiobotremote_conf.autoop_host)
		return;

	if(strcasecmp(argv[1], radiobotremote_conf.radiochan))
		return;

	if(src->host && !match(radiobotremote_conf.autoop_host, src->host))
		irc_send("MODE %s +o %s", radiobotremote_conf.radiochan, src->nick);
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
	reply("Stream: $b%s$b", radiobotremote_conf.stream_url);
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

static void send_status(const char *nick)
{
	irc_send_msg(nick, "NOTICE", "Im Moment onAir: $b%s$b", (current_mod ? current_mod : "[Playlist]"));
	irc_send_msg(nick, "NOTICE", "Listener: $b%d$b/%d (%d unique); Peak: %d ~~~ Quali: %d kbps ~~~ %s", stream_stats.listeners_current, stream_stats.listeners_max, stream_stats.listeners_unique, stream_stats.listeners_peak, stream_stats.bitrate, stream_stats.title);
	irc_send_msg(nick, "NOTICE", "Stream: $b%s$b", radiobotremote_conf.stream_url);
}

COMMAND(wish)
{
	char *msg;

	if(!current_mod || !strcasecmp(current_mod, "Playlist"))
	{
		reply("Sorry, aber von der Playlist kannst du dir nichts wünschen.");
		return 0;
	}

	if(!cmd_sock_active || !cmd_sock)
	{
		reply("Keine Verbindung zum Hauptbot - versuch es bitte später nochmal.");
		return 0;
	}

	if(argc < 2)
	{
		reply("Du wünscht dir nichts? Falls doch, mach $b%s <hier dein wunsch>$b", argv[0]);
		return 0;
	}

	msg = untokenize(argc - 1, argv + 1, " ");
	sock_write_fmt(cmd_sock, "QWISH\t%s\t%s\n", src->nick, msg);

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

	if(!current_mod || !strcasecmp(current_mod, "Playlist"))
	{
		reply("Sorry, aber die Playliste grüßt niemanden.");
		return 0;
	}

	if(!cmd_sock_active || !cmd_sock)
	{
		reply("Keine Verbindung zum Hauptbot - versuch es bitte später nochmal.");
		return 0;
	}

	if(argc < 2)
	{
		reply("Was sollen wir denn mit einem leeren Gruß?");
		return 0;
	}

	msg = untokenize(argc - 1, argv + 1, " ");
	sock_write_fmt(cmd_sock, "QGREET\t%s\t%s\n", src->nick, msg);
	reply("Dein Gruß wurde weitergeleitet.");
	free(msg);

	return 1;
}

// cmdsock stuff
static void cmd_client_connect()
{
	if(cmd_sock)
		sock_close(cmd_sock);
	cmd_sock_active = 0;

	cmd_sock = sock_create(SOCK_IPV4, cmd_client_event, cmd_client_read);
	assert(cmd_sock);

	if(sock_connect(cmd_sock, radiobotremote_conf.cmd_sock_host, radiobotremote_conf.cmd_sock_port) != 0)
	{
		log_append(LOG_WARNING, "connect() to cmd server (%s:%d) failed.", radiobotremote_conf.cmd_sock_host, radiobotremote_conf.cmd_sock_port);
		cmd_sock = NULL;
		cmd_client_schedule_reconnect(15);
		return;
	}

	sock_set_readbuf(cmd_sock, MAXLEN, "\r\n");
	timer_add(this, "cmd_client_connect_timeout", now + 15, cmd_client_timeout, NULL, 0, 0);
}

static void cmd_client_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Cmd socket error on socket %d: %s (%d)", sock->fd, strerror(err), err);
		cmd_sock = NULL;
		cmd_sock_active = 0;
		cmd_client_schedule_reconnect(10);
	}
	else if(event == EV_HANGUP)
	{
		log_append(LOG_WARNING, "Cmd socket %d hung up", sock->fd);
		cmd_sock = NULL;
		cmd_sock_active = 0;
		cmd_client_schedule_reconnect(5);
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "cmd_client_connect_timeout");
		sock_write_fmt(sock, "PWD\t%s\n", radiobotremote_conf.cmd_sock_pass);
		sock_write_fmt(sock, "SHOWINFO\n");
	}
}

static void cmd_client_read(struct sock *sock, char *buf, size_t len)
{
	char *argv[2];
	int argc;

	debug("Received line on cmd client socket: %s", buf);
	if(!strcasecmp(buf, "SUCCESS"))
	{
		cmd_sock_active = 1;
		return;
	}

	argc = tokenize(buf, argv, 2, ' ', 0);

	if(argc > 1 && !strcasecmp(argv[0], "SHOW_MOD"))
	{
		MyFree(current_mod);
		current_mod = strdup(argv[1]);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "SHOW_NAME"))
	{
		MyFree(current_show);
		current_show = strdup(argv[1]);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "SHOW_PLAYLIST"))
	{
		MyFree(current_playlist);
		current_playlist = strdup(argv[1]);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "SHOW_STREAMTITLE"))
	{
		MyFree(current_streamtitle);
		current_streamtitle = strdup(argv[1]);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "SHOW_QUEUE_FULL"))
	{
		queue_full = strtoul(argv[1], NULL, 10);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "PEAK_LISTENERS"))
	{
		last_peak = atoi(argv[1]);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "PEAK_TIME"))
	{
		last_peak_time = strtoul(argv[1], NULL, 10);
	}
	else if(argc > 1 && !strcasecmp(argv[0], "PEAK_MOD"))
	{
		MyFree(last_peak_mod);
		if(strcmp(argv[1], "*"))
			last_peak_mod = strdup(argv[1]);
	}
	else if(argc > 0 && !strcasecmp(argv[0], "SHOWINFO_COMPLETE"))
	{
		irc_send("TOPIC %s :" TOPIC_FMT, radiobotremote_conf.radiochan, current_mod, current_show);
	}
}

static void cmd_client_timeout(void *bound, void *data)
{
	log_append(LOG_WARNING, "Could not connect to cmd server: timeout.");
	sock_close(cmd_sock);
	cmd_sock = NULL;
	cmd_client_schedule_reconnect(30);
}

static void cmd_client_schedule_reconnect(unsigned int wait)
{
	timer_del_boundname(this, "cmd_client_reconnect");
	timer_del_boundname(this, "cmd_client_connect_timeout");
	timer_add(this, "cmd_client_reconnect", now + wait, cmd_client_reconnect_tmr, NULL, 0, 0);
}

static void cmd_client_reconnect_tmr(void *bound, void *data)
{
	debug("Reconnecting to cmd server");
	cmd_client_connect();
}


// stats stuff
static void stats_sock_connect()
{
	if(stats_sock)
	{
		sock_close(stats_sock);
		timer_del_boundname(this, "stats_timeout");
#ifdef CACHE_STATS
		timer_del_boundname(this, "update_stats");
#endif
	}

	stats_sock = sock_create(SOCK_IPV4, stats_sock_event, stats_sock_read);
	assert(stats_sock);

	stringbuffer_flush(stats_data);
	if(sock_connect(stats_sock, radiobotremote_conf.stream_ip_stats, radiobotremote_conf.stream_port_stats) != 0)
	{
		log_append(LOG_WARNING, "connect() to stream server (%s:%d) failed.", radiobotremote_conf.stream_ip_stats, radiobotremote_conf.stream_port_stats);
		stats_sock = NULL;
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
#ifdef CACHE_STATS
		timer_add(this, "update_stats", now + 60, stats_update_tmr, NULL, 0, 1);
#endif
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
		debug("Stats socket hung up");
		stats_sock = NULL;
		stats_received();
		stringbuffer_flush(stats_data);
#ifdef CACHE_STATS
		timer_add(this, "update_stats", now + STATS_DELAY, stats_update_tmr, NULL, 0, 1);
#endif
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "stats_timeout");
		sock_write_fmt(sock, "GET /admin.cgi?pass=%s&mode=viewxml&page=0 HTTP/1.0\r\n", radiobotremote_conf.stream_pass_stats);
		sock_write_fmt(sock, "User-agent: Mozilla compatible\r\n");
		sock_write_fmt(sock, "Host: %s\r\n", radiobotremote_conf.stream_ip_stats);
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

#ifdef CACHE_STATS
		timer_add(this, "update_stats", now + 120, stats_update_tmr, NULL, 0, 1);
#endif
}

#ifdef CACHE_STATS
static void stats_update_tmr(void *bound, void *data)
{
	stats_sock_connect();
}
#endif

#define HEADER_END "\r\n\r\n"
static void stats_received()
{
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
	doc = xmlReadMemory(data, strlen(data), "stats.xml", NULL, 0);
	if(!doc)
	{
		log_append(LOG_WARNING, "Could not parse xml stats");
		return;
	}

	xmlNode *root_element = xmlDocGetRootElement(doc);
	iterate_stats_elements(root_element);
	xmlFreeDoc(doc);

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
		stream_stats.bitrate = atoi(value);
	else if(!strcasecmp(key, "SONGTITLE"))
	{
		MyFree(stream_stats.title);
		stream_stats.title = strdup(value);
	}
}

