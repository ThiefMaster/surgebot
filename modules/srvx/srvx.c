#include "global.h"
#include "srvx.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
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
static char *qserv_token();
static void srvx_request_free(struct srvx_request *r);
static void srvx_cancel_requests();
static void srvx_sock_connect();
static void srvx_sock_event(struct sock *sock, enum sock_event event, int err);
static void srvx_send_raw(const char *format, ...) PRINTF_LIKE(1,2);
static void srvx_sock_read(struct sock *sock, char *buf, size_t len);
static void srvx_sock_timeout(void *bound, void *data);
static void srvx_sock_schedule_reconnect(unsigned int wait);
static void srvx_sock_reconnect_tmr(void *bound, void *data);
static void srvx_auth_response(struct srvx_request *r, void *ctx);
COMMAND(srvx_reconnect);
COMMAND(srvx_exec);

static struct module *this;
static struct dict *requests;
static struct srvx_request *active_request = NULL;
static struct sock *srvx_sock = NULL;
static unsigned int srvx_authed = 0;

MODULE_INIT
{
	this = self;

	requests = dict_create();
	dict_set_free_funcs(requests, NULL, (dict_free_f *)srvx_request_free);

	reg_conf_reload_func(srvx_conf_reload);
	srvx_conf_reload();

	DEFINE_COMMAND(this, "srvx reconnect",	srvx_reconnect,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(this, "srvx exec",	srvx_exec,	1, CMD_REQUIRE_AUTHED | CMD_LOG_HOSTMASK, "group(admins)");
	srvx_sock_connect();
}

MODULE_FINI
{
	unreg_conf_reload_func(srvx_conf_reload);

	if(srvx_sock)
		sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests();

	dict_free(requests);

	timer_del_boundname(this, "srvx_reconnect");
	timer_del_boundname(this, "srvx_connect_timeout");
}

static void srvx_conf_reload()
{
	char *str;

	srvx_conf.local_host = conf_get("srvx/local_host", DB_STRING);

	str = conf_get("srvx/qserver_host", DB_STRING);
	srvx_conf.qserver_host = str ? str : "127.0.0.1";

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

static char *qserv_token()
{
	static char token[8];
	snprintf(token, sizeof(token), "GS%05X", mt_rand(1, 65535));
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
	free(r->lines);
	free(r->token);
	free(r);
}

static void srvx_cancel_requests()
{
	dict_iter(node, requests)
	{
		struct srvx_request *req = node->data;
		debug("Cancelling srvx request %s", req->token);
		req->callback(NULL, req->ctx);
		dict_delete(requests, req->token);
	}

	assert(dict_size(requests) == 0);
}

static void srvx_sock_connect()
{
	if(srvx_sock)
		sock_close(srvx_sock);

	srvx_authed = 0;

	srvx_sock = sock_create(SOCK_IPV4, srvx_sock_event, srvx_sock_read);
	assert(srvx_sock);

	if(srvx_conf.local_host)
		sock_bind(srvx_sock, srvx_conf.local_host, 0);

	if(sock_connect(srvx_sock, srvx_conf.qserver_host, srvx_conf.qserver_port) != 0)
	{
		log_append(LOG_WARNING, "connect() to srvx qserver (%s:%d) failed.", srvx_conf.qserver_host, srvx_conf.qserver_port);
		srvx_sock = NULL;
		srvx_cancel_requests();
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
		srvx_cancel_requests();
		srvx_sock_schedule_reconnect(10);
	}
	else if(event == EV_HANGUP)
	{
		log_append(LOG_WARNING, "Srvx socket %d hung up", sock->fd);
		srvx_sock = NULL;
		srvx_cancel_requests();
		srvx_sock_schedule_reconnect(5);
	}
	else if(event == EV_CONNECT)
	{
		timer_del_boundname(this, "srvx_connect_timeout");
		if(strlen(srvx_conf.qserver_pass))
			srvx_send_raw("%s PASS %s", qserv_token(), srvx_conf.qserver_pass);

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
	char buf[MAXLEN];
	char *token;
	struct srvx_request *req;

	assert(srvx_sock);
	assert(srvx_authed);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf) - 10, format, args); // Leave some space for token
	va_end(args);

	token = qserv_token();

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

	sock_write_fmt(srvx_sock, "%s %s\n", token, buf);
	debug("Sent to srvx: %s", buf);
}

static void srvx_send_raw(const char *format, ...)
{
	va_list args;
	char buf[MAXLEN];

	assert(srvx_sock);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	sock_write_fmt(srvx_sock, "%s\n", buf);
	debug("Sent to srvx: %s", buf);
}


static void srvx_sock_read(struct sock *sock, char *buf, size_t len)
{
	char *orig;
	struct srvx_response_line *line;
	char *argv[4];
	int argc;

	orig = strdup(buf);
	argc = itokenize(buf, argv, sizeof(argv), ' ', ':');
	assert(argc > 1);

	if(*argv[1] == 'P' || *argv[1] == 'N') // PRIVMSG/NOTICE
	{
		assert(argc > 2);
		assert(active_request);

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
		assert(!active_request);
		active_request = dict_find(requests, argv[0]);
		assert(active_request);
	}
	else if(*argv[1] == 'E') // Response end
	{
		debug("End: %s", argv[0]);
		assert(active_request);
		assert(!strcmp(active_request->token, argv[0]));
		active_request->callback(active_request, active_request->ctx);
		dict_delete(requests, argv[0]);
		active_request = NULL;
	}
	else
	{
		log_append(LOG_WARNING, "Unexpected response from srvx qserver: %s", orig);
	}

	free(orig);
}

static void srvx_sock_timeout(void *bound, void *data)
{
	log_append(LOG_WARNING, "Could not connect to srvx qserver %s:%d; timeout.", srvx_conf.qserver_host, srvx_conf.qserver_port);
	sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests();
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
	debug("Reconnecting to srvx qserver %s:%d", srvx_conf.qserver_host, srvx_conf.qserver_port);
	srvx_sock_connect();
}


COMMAND(srvx_reconnect)
{
	if(srvx_sock)
		sock_close(srvx_sock);
	srvx_sock = NULL;
	srvx_cancel_requests();
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
