#include "global.h"
#include "srvx.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "irc_handler.h"
#include "timer.h"
#include "conf.h"
#include "sock.h"
#include "mtrand.h"

MODULE_DEPENDS("commands", NULL);

static struct
{
	const char *local_host;
	const char *qserver_host;
	unsigned int qserver_port;
	const char *qserver_pass;

	const char *account_name;
	const char *account_pass;
} srvx_conf;


static void srvx_conf_reload();
static char *qserv_token(char type);
static void srvx_request_free(struct srvx_request *r);
static void srvx_cancel_requests(int shutdown);
static void srvx_sock_connect();
static void srvx_sock_event(struct sock *sock, enum sock_event event, int err);
static void srvx_send_raw(const char *format, ...) PRINTF_LIKE(1,2);
static void srvx_sock_read(struct sock *sock, char *buf, size_t len);
static void srvx_handle_qserver_response(char **argv, int argc, char *orig_line);
static void srvx_sock_timeout(void *bound, void *data);
static void srvx_sock_schedule_reconnect(unsigned int wait);
static void srvx_sock_reconnect_tmr(void *bound, void *data);
static void srvx_auth_response(struct srvx_request *r, void *ctx);
IRC_HANDLER(msg);
COMMAND(srvx_reconnect);
COMMAND(srvx_exec);
COMMAND(srvx_exec_noq);

static struct module *this;
static struct dict *requests;
static struct srvx_request *active_request = NULL;
static struct srvx_request *active_request_irc = NULL;
static struct sock *srvx_sock = NULL;
static unsigned int srvx_authed = 0;

MODULE_INIT
{
	this = self;

	requests = dict_create();
	dict_set_free_funcs(requests, NULL, (dict_free_f *)srvx_request_free);

	reg_conf_reload_func(srvx_conf_reload);
	srvx_conf_reload();

	reg_irc_handler("PRIVMSG", msg);
	reg_irc_handler("NOTICE", msg);

	DEFINE_COMMAND(this, "srvx reconnect",	srvx_reconnect,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(this, "srvx exec",	srvx_exec,	2, CMD_REQUIRE_AUTHED | CMD_LOG_HOSTMASK, "group(admins)");
	DEFINE_COMMAND(this, "srvx execnoq",	srvx_exec_noq,	2, CMD_REQUIRE_AUTHED | CMD_LOG_HOSTMASK, "group(admins)");
	srvx_sock_connect();
}

MODULE_FINI
{
	unreg_conf_reload_func(srvx_conf_reload);

	unreg_irc_handler("PRIVMSG", msg);
	unreg_irc_handler("NOTICE", msg);

	if(srvx_sock)
		sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests(1);

	dict_free(requests);

	timer_del_boundname(this, "srvx_reconnect");
	timer_del_boundname(this, "srvx_connect_timeout");
}

static void srvx_conf_reload()
{
	char *str;

	srvx_conf.local_host = conf_get("srvx/local_host", DB_STRING);

	str = conf_get("srvx/qserver_host", DB_STRING);
	srvx_conf.qserver_host = str;

	str = conf_get("srvx/qserver_port", DB_STRING);
	srvx_conf.qserver_port = str ? atoi(str) : 7702;

	str = conf_get("srvx/qserver_pass", DB_STRING);
	srvx_conf.qserver_pass = str ? str : "hello";

	str = conf_get("srvx/account_name", DB_STRING);
	srvx_conf.account_name = (str && strlen(str)) ? str : NULL;

	str = conf_get("srvx/account_pass", DB_STRING);
	srvx_conf.account_pass = (str && strlen(str)) ? str : NULL;

	// We could reconnect at this point when the srvx host/port changed,
	// but if that happens, you can simply use the "srvx reconnect" command.
}

static char *qserv_token(char type)
{
	static char token[9];
	snprintf(token, sizeof(token), "GS%c%05X", type, mt_rand(1, 65535));
	return token;
}

static void srvx_request_free(struct srvx_request *r)
{
	for(unsigned int i = 0; i < r->count; i++)
	{
		free(r->lines[i]->nick);
		free(r->lines[i]->msg);
		free(r->lines[i]);
	}

	if(r->free_ctx)
		free(r->ctx);
	if(r->nick)
		free(r->nick);
	free(r->lines);
	free(r->token);
	free(r);
}

static void srvx_cancel_requests(int shutdown)
{
	dict_iter(node, requests)
	{
		struct srvx_request *req = node->data;
		debug("Cancelling srvx request %s", req->token);
		if(!shutdown)
			req->callback(NULL, req->ctx);
		dict_delete(requests, req->token);
	}

	assert(dict_size(requests) == 0);
}

static void srvx_sock_connect()
{
	if(srvx_sock)
		sock_close(srvx_sock);

	if(!srvx_conf.qserver_host)
		return;

	srvx_authed = 0;

	srvx_sock = sock_create(SOCK_IPV4, srvx_sock_event, srvx_sock_read);
	assert(srvx_sock);

	if(srvx_conf.local_host)
		sock_bind(srvx_sock, srvx_conf.local_host, 0);

	if(sock_connect(srvx_sock, srvx_conf.qserver_host, srvx_conf.qserver_port) != 0)
	{
		log_append(LOG_WARNING, "connect() to srvx qserver (%s:%d) failed.", srvx_conf.qserver_host, srvx_conf.qserver_port);
		srvx_sock = NULL;
		srvx_cancel_requests(0);
		srvx_sock_schedule_reconnect(15);
		return;
	}

	sock_set_readbuf(srvx_sock, MAXLEN, "\r\n");
	timer_add(this, "srvx_connect_timeout", now + 15, (timer_f *)srvx_sock_timeout, NULL, 0, 0);
}

static void srvx_sock_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR)
	{
		log_append(LOG_WARNING, "Srvx socket error on socket %d: %s (%d)", sock->fd, strerror(err), err);
		srvx_sock = NULL;
		srvx_cancel_requests(0);
		srvx_sock_schedule_reconnect(10);
	}
	else if(event == EV_HANGUP)
	{
		log_append(LOG_WARNING, "Srvx socket %d hung up", sock->fd);
		srvx_sock = NULL;
		srvx_cancel_requests(0);
		srvx_sock_schedule_reconnect(5);
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "srvx_connect_timeout");
		if(strlen(srvx_conf.qserver_pass))
			srvx_send_raw("%s PASS %s", qserv_token('A'), srvx_conf.qserver_pass);

		// This is hackish but the only way to send the auth command without failing the "authed" assertion.
		srvx_authed = 1;
		srvx_send(srvx_auth_response, "AuthServ AUTH %s %s", srvx_conf.account_name, srvx_conf.account_pass);
		srvx_authed = 0;
	}
}

static void srvx_auth_response(struct srvx_request *r, void *ctx)
{
	if(r)
	{
		for(unsigned int i = 0; i < r->count; i++)
		{
			if(!strcmp(r->lines[i]->msg, "I recognize you."))
			{
				debug("Successfully authenticated with srvx.");
				srvx_authed = 1;
				return;
			}
		}
	}

	log_append(LOG_WARNING, "Could not authenticate with srvx.");
	if(srvx_sock)
		sock_close(srvx_sock);
	srvx_sock = NULL;
}

void srvx_send_ctx(srvx_response_f *func, void *ctx, unsigned int free_ctx, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	srvx_vsend_ctx(func, ctx, free_ctx, 0, format, args);
	va_end(args);
}

void srvx_send_ctx_noqserver(srvx_response_f *func, void *ctx, unsigned int free_ctx, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	srvx_vsend_ctx(func, ctx, free_ctx, 1, format, args);
	va_end(args);
}

void srvx_vsend_ctx(srvx_response_f *func, void *ctx, unsigned int free_ctx, unsigned int no_qserver, const char *format, va_list args)
{
	char buf[MAXLEN];
	char *token;
	struct srvx_request *req = NULL;
	int use_qserver;

	if(srvx_sock && !no_qserver)
		assert(srvx_authed);

	vsnprintf(buf, sizeof(buf) - 10, format, args); // Leave some space for token

	token = qserv_token((srvx_sock && !no_qserver) ? 'Q' : 'I');

	if(func)
	{
		req = malloc(sizeof(struct srvx_request));
		memset(req, 0, sizeof(struct srvx_request));
		req->callback = func;
		req->ctx = ctx;
		req->free_ctx = free_ctx;
		req->token = strdup(token);
		req->count = 0;
		req->size = 2;
		req->lines = calloc(req->size, sizeof(struct srvx_response_line *));

		dict_insert(requests, req->token, req);
	}

	if(srvx_sock && !no_qserver)
	{
		sock_write_fmt(srvx_sock, "%s %s\n", token, buf);
		debug("Sent to srvx: %s", buf);
	}
	else
	{
		char *nick_end = strchr(buf, ' ');
		assert(nick_end);
		*nick_end = '\0';

		if(req)
			req->nick = strdup(buf);
		irc_send("PRIVMSG %s :\001PING %s S\001", buf, token);
		irc_send("PRIVMSG %s :%s", buf, nick_end + 1);
		irc_send("PRIVMSG %s :\001PING %s E\001", buf, token);
		debug("Sent to srvx: (%s) %s", buf, nick_end + 1);
	}
}

static void srvx_send_raw(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	if(srvx_sock)
	{
		sock_write_fmt(srvx_sock, "%s\n", buf);
		debug("Sent to srvx: %s", buf);
	}
	else
	{
		char *nick_end = strchr(buf, ' ');
		assert(nick_end);
		*nick_end = '\0';

		irc_send("PRIVMSG %s :%s", buf, nick_end + 1);
		debug("Sent to srvx: (%s) %s", buf, nick_end + 1);
	}
}


static void srvx_sock_read(struct sock *sock, char *buf, size_t len)
{
	char *orig;
	char *argv[4];
	int argc;

	orig = strdup(buf);
	argc = itokenize(buf, argv, sizeof(argv), ' ', ':');
	assert(argc > 1);

	srvx_handle_qserver_response(argv, argc, orig);

	free(orig);
}

IRC_HANDLER(msg)
{
	assert(argc > 2);
	if(!src || strcmp(argv[1], bot.nickname))
		return;

	// CTCP PING
	if(!strncasecmp(argv[2], "\001PING ", 6) && !strcmp(argv[0], "NOTICE"))
	{
		char *msg;
		char token[9];
		char type;

		msg = argv[2] + 6;
		if(strlen(msg) != 11) // "GSIxxxxx X\1"
			return;

		assert(msg[2] == 'I'); // IRC

		strlcpy(token, msg, 9);
		type = msg[9];

		if(type == 'S')
		{
			debug("Start: %s", token);
			assert(!active_request_irc);
			active_request_irc = dict_find(requests, token);
			assert(active_request_irc);
		}
		else if(type == 'E')
		{
			debug("End: %s", token);
			assert(active_request_irc);
			assert(!strcmp(active_request_irc->token, token));
			active_request_irc->callback(active_request_irc, active_request_irc->ctx);
			dict_delete(requests, token);
			active_request_irc = NULL;
		}
		else
		{
			char tmp = msg[strlen(msg) - 1];
			msg[strlen(msg) - 1] = '\0';
			log_append(LOG_WARNING, "Unexpected response ping from srvx: %s", msg);
			msg[strlen(msg) - 1] = tmp;
		}
	}
	else if(active_request_irc && active_request_irc->nick && !strcasecmp(src->nick, active_request_irc->nick))
	{
		struct srvx_response_line *line = malloc(sizeof(struct srvx_response_line));
		memset(line, 0, sizeof(struct srvx_response_line));
		line->nick = strdup(src->nick);
		line->msg = strdup(argv[2]);

		if(active_request_irc->count == active_request_irc->size) // list is full, we need to allocate more memory
		{
			active_request_irc->size <<= 1; // double size
			active_request_irc->lines = realloc(active_request_irc->lines, active_request_irc->size * sizeof(struct srvx_response_line *));
		}

		debug("Line: %s", line->msg);
		active_request_irc->lines[active_request_irc->count++] = line;
	}
}

static void srvx_handle_qserver_response(char **argv, int argc, char *orig_line)
{
	struct srvx_response_line *line;

	if(*argv[1] == 'P' || *argv[1] == 'N') // PRIVMSG/NOTICE
	{
		assert(argc > 2);
		assert(active_request);
		assert(!active_request->nick); // should not be for requests using qserver

		line = malloc(sizeof(struct srvx_response_line));
		memset(line, 0, sizeof(struct srvx_response_line));
		line->nick = strdup(argv[0]);
		line->msg = strdup(argv[2]);

		if(active_request->count == active_request->size) // list is full, we need to allocate more memory
		{
			active_request->size <<= 1; // double size
			active_request->lines = realloc(active_request->lines, active_request->size * sizeof(struct srvx_response_line *));
		}

		debug("Line: %s", line->msg);
		active_request->lines[active_request->count++] = line;
	}
	else if(*argv[1] == 'S') // Response begin
	{
		debug("Start: %s", argv[0]);
		assert(argv[0][2] == 'Q'); // qserver
		assert(!active_request);
		active_request = dict_find(requests, argv[0]);
		assert(active_request);
	}
	else if(*argv[1] == 'E') // Response end
	{
		debug("End: %s", argv[0]);
		assert(argv[0][2] == 'Q'); // qserver
		assert(active_request);
		assert(!strcmp(active_request->token, argv[0]));
		active_request->callback(active_request, active_request->ctx);
		dict_delete(requests, argv[0]);
		active_request = NULL;
	}
	else
	{
		log_append(LOG_WARNING, "Unexpected response from srvx qserver: %s", orig_line);
	}
}

static void srvx_sock_timeout(void *bound, void *data)
{
	log_append(LOG_WARNING, "Could not connect to srvx qserver %s:%d; timeout.", srvx_conf.qserver_host, srvx_conf.qserver_port);
	sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests(0);
	srvx_sock_schedule_reconnect(30);
}

static void srvx_sock_schedule_reconnect(unsigned int wait)
{
	timer_del_boundname(this, "srvx_reconnect");
	timer_del_boundname(this, "srvx_connect_timeout");
	timer_add(this, "srvx_reconnect", now + wait, (timer_f *)srvx_sock_reconnect_tmr, NULL, 0, 0);
}

static void srvx_sock_reconnect_tmr(void *bound, void *data)
{
	if(!srvx_conf.qserver_host)
		return;
	debug("Reconnecting to srvx qserver %s:%d", srvx_conf.qserver_host, srvx_conf.qserver_port);
	srvx_sock_connect();
}


COMMAND(srvx_reconnect)
{
	if(srvx_sock)
		sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests(0);
	srvx_sock_schedule_reconnect(1);

	reply("Reconnecting to srvx.");
	return 1;
}

static void srvx_exec_cb(struct srvx_request *r, char *ctx)
{
	if(!r)
	{
		irc_send("NOTICE %s :Srvx disconnected during request.", ctx);
		return;
	}

	for(unsigned int i = 0; i < r->count; i++)
		irc_send("NOTICE %s :[%s] %s", ctx, r->lines[i]->nick, r->lines[i]->msg);
}

COMMAND(srvx_exec)
{
	char *line = untokenize(argc - 1, argv + 1, " ");
	srvx_send_ctx((srvx_response_f *)srvx_exec_cb, strdup(src->nick), 1, "%s", line);
	free(line);
	reply("Sent command to srvx.");
	return 1;
}

COMMAND(srvx_exec_noq)
{
	char *line = untokenize(argc - 1, argv + 1, " ");
	srvx_send_ctx_noqserver((srvx_response_f *)srvx_exec_cb, strdup(src->nick), 1, "%s", line);
	free(line);
	reply("Sent command to srvx.");
	return 1;
}
