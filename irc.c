#include "global.h"
#include "sock.h"
#include "irc.h"
#include "timer.h"
#include "irc_handler.h"
#include "stringlist.h"
#include "surgebot.h"
#include "conf.h"
#include "chanuser.h"

IMPLEMENT_LIST(disconnected_func_list, disconnected_f *)

extern struct surgebot_conf bot_conf;
extern int quit_poll;
static struct disconnected_func_list *disconnected_funcs;

static void irc_conf_reload();
static void irc_sock_read(struct sock *sock, char *buf, size_t len);
static void irc_sock_event(struct sock *sock, enum sock_event event, int err);
static void irc_connected();
static void irc_disconnected();
static void irc_connect_timeout(void *bound, void *data);
static void irc_ping(void *bound, void *data);
static void irc_stoned(void *bound, void *data);
static void irc_error(int err);
static void irc_schedule_reconnect(unsigned int wait);
static void irc_reconnect(void *bound, void *data);
static void irc_poll_sendq();

void irc_init()
{
	if(bot_conf.throttle)
		reg_loop_func(irc_poll_sendq);

	reg_conf_reload_func(irc_conf_reload);
	disconnected_funcs = disconnected_func_list_create();
}

void irc_fini()
{
	disconnected_func_list_free(disconnected_funcs);
	unreg_conf_reload_func(irc_conf_reload);
	unreg_loop_func(irc_poll_sendq);
}

static void irc_conf_reload()
{
	if(bot_conf.throttle && !conf_bool_old("uplink/throttle"))
		reg_loop_func(irc_poll_sendq);
	else if(!bot_conf.throttle && conf_bool_old("uplink/throttle") && bot.sendq->count == 0)
		unreg_loop_func(irc_poll_sendq);
}

int irc_connect()
{
	int res;

	if(bot.server_sock)
	{
		debug("Closing old server socket %p", bot.server_sock);
		sock_close(bot.server_sock);
	}

	bot.ready = 0;
	bot.server_sock = sock_create((bot_conf.server_ipv6 ? SOCK_IPV6 : SOCK_IPV4) | (bot_conf.server_ssl ? SOCK_SSL : 0), irc_sock_event, irc_sock_read);
	if(bot.server_sock == NULL)
		return -2;

	if(bot_conf.local_host)
		sock_bind(bot.server_sock, bot_conf.local_host, 0);

	log_append(LOG_INFO, "Connecting to %s:%d", bot_conf.server_host, bot_conf.server_port);
	res = sock_connect(bot.server_sock, bot_conf.server_host, bot_conf.server_port);
	if(res != 0)
	{
		bot.server_sock = NULL;
		return -3;
	}

	sock_set_readbuf(bot.server_sock, MAXLEN, "\r\n");

	timer_add(&bot, "server_connect_timeout", now + 15, irc_connect_timeout, bot.server_sock, 0);
	return 0;
}

static void irc_connected()
{
	log_append(LOG_INFO, "Connection to server successful, logging in");
	timer_del_boundname(&bot, "server_connect_timeout");
	bot.linked = now;

	if(bot_conf.server_pass)
		irc_send("PASS :%s", bot_conf.server_pass);

	irc_send("USER %s * * :%s", bot_conf.username, bot_conf.realname);
	irc_send("NICK %s", bot_conf.nickname);

	if(bot.nickname) free(bot.nickname);
	if(bot.username) free(bot.username);
	if(bot.realname) free(bot.realname);
	if(bot.hostname) free(bot.hostname);

	bot.nickname = strdup(bot_conf.nickname);
	bot.username = strdup(bot_conf.username);
	bot.realname = strdup(bot_conf.realname);
	bot.hostname = NULL;

	bot.last_msg = now;

	timer_add(&bot, "server_ping", now + 90, irc_ping, NULL, 0);
	timer_add(&bot, "server_stoned", now + 180, irc_stoned, NULL, 0);
}

void irc_watchdog_reset()
{
	timer_del_boundname(&bot, "server_stoned");
	timer_add(&bot, "server_stoned", now + 180, irc_stoned, NULL, 0);
}

static void irc_ping(void *bound, void *data)
{
	irc_send_fast("PING :%lu", now);
	timer_add(&bot, "server_ping", now + 90, irc_ping, NULL, 0);
}

static void irc_stoned(void *bound, void *data)
{
	irc_send_fast("QUIT :Server seems to be stoned; reconnecting");
	irc_schedule_reconnect(0);
}

static void irc_disconnected()
{
	log_append(LOG_INFO, "Connection closed by the server");
	timer_del_boundname(&bot, "server_connect_timeout");
	bot.server_sock = NULL;
	for(unsigned int i = 0; i < disconnected_funcs->count; i++)
		disconnected_funcs->data[i]();
	irc_schedule_reconnect(5);
}

static void irc_connect_timeout(void *bound, void *data)
{
	if(data != bot.server_sock)
	{
		log_append(LOG_WARNING, "Ignoring old connect timeout (%p != %p)", data, bot.server_sock);
		return;
	}

	log_append(LOG_INFO, "Could not connect to server - timeout");
	sock_close(bot.server_sock);
	bot.server_sock = NULL;
	for(unsigned int i = 0; i < disconnected_funcs->count; i++)
		disconnected_funcs->data[i]();
	irc_schedule_reconnect(30);
}

static void irc_error(int err)
{
	timer_del_boundname(&bot, "server_connect_timeout");
	bot.server_sock = NULL;
	for(unsigned int i = 0; i < disconnected_funcs->count; i++)
		disconnected_funcs->data[i]();
	irc_schedule_reconnect(15);
}

static void irc_schedule_reconnect(unsigned int wait)
{
	if(timer_exists_boundname(&bot, "server_reconnect"))
		return;

	timer_del_boundname(&bot, "server_ping");
	timer_del_boundname(&bot, "server_stoned");

	bot.server_tries++;
	if(bot_conf.max_server_tries && bot.server_tries > bot_conf.max_server_tries)
	{
		log_append(LOG_WARNING, "Max. server connect attempts (%d) exceeded - exiting", bot_conf.max_server_tries);
		quit_poll = 1;
		return;
	}

	timer_add(&bot, "server_reconnect", now + wait, irc_reconnect, NULL, 0);
}

static void irc_reconnect(void *bound, void *data)
{
	if(bot_conf.max_server_tries > 0)
		log_append(LOG_INFO, "Reconnecting (%d reconnect attempt(s) left)", bot_conf.max_server_tries - bot.server_tries);
	else
		log_append(LOG_INFO, "Reconnecting");

	// reset everything
	chanuser_flush();
	bot.ready = 0;
	bot.burst_count = 0;
	if(bot.burst_lines->count)
	{
		stringlist_free(bot.burst_lines);
		bot.burst_lines = stringlist_create();
	}

	if(bot.sendq->count)
	{
		stringlist_free(bot.sendq);
		bot.sendq = stringlist_create();
	}

	irc_connect();
}

void irc_send_raw(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN];

	assert(bot.server_sock);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	if(bot_conf.throttle)
		stringlist_add(bot.sendq, strdup(buf));
	else
		sock_write_fmt(bot.server_sock, "%s\r\n", buf);
	log_append(LOG_SEND, "%s", buf);
	bot.lines_sent++;
}

void irc_send(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN], *formatted;

	assert(bot.server_sock);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	formatted = irc_format_line(buf);
	if(bot_conf.throttle)
		stringlist_add(bot.sendq, strdup(formatted));
	else
		sock_write_fmt(bot.server_sock, "%s\r\n", formatted);
	log_append(LOG_SEND, "%s", formatted);
	bot.lines_sent++;
}

void irc_send_fast(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN], *formatted;

	assert(bot.server_sock);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	formatted = irc_format_line(buf);
	sock_write_fmt(bot.server_sock, "%s\r\n", formatted);
	log_append(LOG_SEND, "%s", formatted);
	bot.lines_sent++;
}

void irc_send_msg(const char *target, const char *cmd, const char *format, ...)
{
	va_list args;
	char buf[MAXLEN];
	int len;

	len = snprintf(buf, sizeof(buf), "%s %s :", cmd, target);

	va_start(args, format);
	vsnprintf(buf + len, sizeof(buf) - len, format, args);
	va_end(args);

	irc_send("%s", buf);
}

static void irc_poll_sendq()
{
	char *line;

	if(bot.server_sock == NULL)
	{
		stringlist_free(bot.sendq);
		bot.sendq = stringlist_create();
		return;
	}

	if(now > bot.last_msg)
		bot.last_msg = now;

	while(bot.sendq->count > 0 && (bot.last_msg - now) < 10)
	{
		line = stringlist_shift(bot.sendq);
		sock_write_fmt(bot.server_sock, "%s\r\n", line);
		bot.last_msg += (2 + strlen(line) / 120);
		free(line);
	}

	if(!bot_conf.throttle && bot.sendq->count == 0)
		unreg_loop_func(irc_poll_sendq);
}

static void irc_sock_event(struct sock *sock, enum sock_event event, int err)
{
	assert(sock == bot.server_sock);

	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Socket error on socket %d: %s (%d)", sock->fd, strerror(err), err);
		irc_error(err);
	}
	else if(event == EV_HANGUP)
	{
		log_append(LOG_WARNING, "Socket %d hung up", sock->fd);
		irc_disconnected();
	}
	else if(event == EV_CONNECT)
	{
		irc_connected();
	}
}

void irc_parse_line(const char *line)
{
	char *line_dup, *orig_argv[MAXARG], **argv, *raw_src;
	int argc;
	struct irc_source src;

	line_dup = strdup(line);
	memset(&src, 0, sizeof(struct irc_source));

	argc = itokenize(line_dup, orig_argv, MAXARG, ' ', ':');

	if(*orig_argv[0] == ':') // message has a source
	{
		orig_argv[0]++; // get rid of colon
		raw_src = orig_argv[0];
		argv = orig_argv + 1;
		argc--;

		char *ptr;
		src.nick = raw_src;
		ptr = strchr(raw_src, '!');
		if(ptr) // we only have ident/host if the source contains a '!'
		{
			*ptr = '\0';
			src.ident = ++ptr;
			ptr = strchr(ptr, '@');
			*ptr = '\0';
			src.host = ++ptr;
		}
	}
	else
	{
		raw_src = NULL;
		argv = orig_argv;
	}

	if(argc == 0)
	{
		log_append(LOG_WARNING, "Ignoring message from irc server that doesn't contain a command");
		return;
	}

	irc_handle_msg(argc, argv, (raw_src ? &src : NULL), line);
	free(line_dup);
}

static void irc_sock_read(struct sock *sock, char *buf, size_t len)
{
	assert(sock == bot.server_sock);

	log_append(LOG_RECEIVE, "%s", buf);
	bot.lines_received++;
	irc_parse_line(buf);
}

char *irc_format_line(const char *msg)
{
	static char buf[MAXLEN];
	const char *ptr = msg;
	size_t pos = 0;
	memset(buf, 0, sizeof(buf));

	while(*ptr && pos < sizeof(buf))
	{
		if(*ptr == '$' && *(ptr + 1))
		{
			ptr++;
			switch(*ptr)
			{
				case '$':
					buf[pos++] = '$';
					break;
				case 'b': // bold
					buf[pos++] = '\002';
					break;
				case 'c':
					buf[pos++] = '\003';
					break;
				case 'o': // clear
					buf[pos++] = '\017';
					break;
				case 'r': // reverse
					buf[pos++] = '\026';
					break;
				case 'u': // underline
					buf[pos++] = '\037';
					break;
				case 'N': // bot nick
					safestrncpy(buf + pos, bot.nickname, sizeof(buf) - pos);
					pos += strlen(bot.nickname);
					break;
				case 'U': // bot username
					safestrncpy(buf + pos, bot.username, sizeof(buf) - pos);
					pos += strlen(bot.username);
					break;
				case 'H': // bot host
					safestrncpy(buf + pos, bot.hostname, sizeof(buf) - pos);
					pos += strlen(bot.hostname);
					break;
				case 'R': // bot realname
					safestrncpy(buf + pos, bot.realname, sizeof(buf) - pos);
					pos += strlen(bot.realname);
					break;
				default:
					buf[pos++] = *(ptr - 1);
					buf[pos++] = *ptr;
			}
		}
		else
		{
			buf[pos++] = *ptr;
		}

		ptr++;
	}

	if(buf[pos - 1] == '\n') // Some functions like ctime add a trailing newline
		buf[pos - 1] = '\0'; // -> remove it!

	return buf;
}

void reg_disconnected_func(disconnected_f *func)
{
	disconnected_func_list_add(disconnected_funcs, func);
}

void unreg_disconnected_func(disconnected_f *func)
{
	disconnected_func_list_del(disconnected_funcs, func);
}
