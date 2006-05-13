#include "global.h"
#include "sock.h"
#include "irc.h"

extern struct surgebot_conf bot_conf;

static void irc_sock_read(struct sock *sock, char *buf, size_t len);
static void irc_sock_event(struct sock *sock, enum sock_event event, int err);
static void irc_connected();
static void irc_disconnected();
static void irc_error(int err);
static void irc_handle_msg(int argc, char **argv, struct irc_source *src);
static char *irc_format_line(const char *msg);

void irc_init()
{

}

void irc_fini()
{

}

int irc_connect()
{
	int res;

	if(bot.server_sock)
	{
		debug("Closing old server socket %p", bot.server_sock);
		sock_close(bot.server_sock);
	}

	bot.server_sock = sock_create(SOCK_IPV4 | (bot_conf.server_ssl ? SOCK_SSL : 0), irc_sock_event, irc_sock_read);
	if(bot.server_sock == NULL)
		return -2;

	res = sock_connect(bot.server_sock, bot_conf.server_host, bot_conf.server_port);
	if(res != 0)
		return -3;

	sock_set_readbuf(bot.server_sock, MAXLEN, "\r\n");

	// TODO: Add connect timeout
	return 0;
}

static void irc_connected()
{
	if(bot_conf.server_pass)
		irc_send("PASS :%s", bot_conf.server_pass);

	irc_send("USER %s * * :%s", bot_conf.username, bot_conf.realname);
	irc_send("NICK %s", bot_conf.nickname);

	bot.nickname = strdup(bot_conf.nickname);
	bot.username = strdup(bot_conf.username);
	bot.realname = strdup(bot_conf.realname);
}

static void irc_disconnected()
{
	bot.server_sock = NULL;
	// TODO: Schedule reconnect
}

static void irc_error(int err)
{
	bot.server_sock = NULL;
	// TODO: Schedule reconnect
}

// TODO: It might be a good idea to move that into a separate source file
static void irc_handle_msg(int argc, char **argv, struct irc_source *src)
{
	if(src)
		debug("src: %s/%s/%s", src->nick, src->ident, src->host);

	int i;
	for(i = 0; i < argc; i++)
		debug("argv[%d]: %s", i, argv[i]);


	if(!strcasecmp(argv[0], "PING"))
	{
		assert(argc > 1);
		irc_send("PONG :%s", argv[1]);
	}
}

int irc_send(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN], *formatted;
	int ret;

	assert_return(bot.server_sock, -1);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	formatted = irc_format_line(buf);
	ret = sock_write_fmt(bot.server_sock, "%s\r\n", formatted);
	log_append(LOG_SEND, "%s", formatted);
	bot.lines_sent++;
	return ret;
}

static void irc_sock_event(struct sock *sock, enum sock_event event, int err)
{
	assert(sock == bot.server_sock);
	//debug("s_event(%p, %d, %d)", sock, event, err);

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

static void irc_sock_read(struct sock *sock, char *buf, size_t len)
{
	char *orig_argv[MAXARG], **argv, *raw_src;
	int argc;
	struct irc_source src;

	assert(sock == bot.server_sock);
	log_append(LOG_RECEIVE, "%s", buf);

	memset(&src, 0, sizeof(struct irc_source));

	argc = itokenize(buf, orig_argv, MAXARG, ' ', ':');

	if(*orig_argv[0] == ':') // message has a source
	{
		*orig_argv[0]++; // get rid of colon
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

	irc_handle_msg(argc, argv, (raw_src ? &src : NULL));
}

static char *irc_format_line(const char *msg)
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

	return buf;
}
