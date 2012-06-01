#include "global.h"
#include "module.h"
#include "irc_handler.h"
#include "irc.h"
#include "ptrlist.h"
#include "sock.h"
#include "modules/scripting/scripting.h"

MODULE_DEPENDS("scripting", NULL);

SCRIPTING_FUNC(reg_irc_handler);
SCRIPTING_FUNC(unreg_irc_handler);
SCRIPTING_FUNC(irc_send);
SCRIPTING_FUNC(sock_create);
SCRIPTING_FUNC(sock_close);
SCRIPTING_FUNC(sock_connect);
SCRIPTING_FUNC(sock_bind);
SCRIPTING_FUNC(sock_listen);
SCRIPTING_FUNC(sock_accept);
SCRIPTING_FUNC(sock_remote_addr);
SCRIPTING_FUNC(sock_write);
SCRIPTING_FUNC(sock_resolve_64);
static void module_unloaded(struct module *module);
static void _irc_handler(int argc, char **argv, struct irc_source *src, struct scripting_arg *func);
static int _cmp_func(struct scripting_arg *func_a, struct scripting_arg *func_b);
static struct scripting_arg *_irc_source_to_arg(struct irc_source *src);
static void _sock_event(struct sock *sock, enum sock_event event, int err);
static void _sock_read(struct sock *sock, char *buf, size_t len);

struct scripting_sock {
	struct module *module;
	struct sock *sock;
	struct scripting_arg *event_func;
	struct scripting_arg *read_func;
};

static struct module *this;
static int num_irc_handlers;
static struct ptrlist *sockets;

MODULE_INIT
{
	this = self;
	sockets = ptrlist_create();
	reg_module_load_func(NULL, module_unloaded);
	REG_SCRIPTING_FUNC(reg_irc_handler);
	REG_SCRIPTING_FUNC(unreg_irc_handler);
	REG_SCRIPTING_FUNC(irc_send);
	REG_SCRIPTING_FUNC(sock_create);
	REG_SCRIPTING_FUNC(sock_close);
	REG_SCRIPTING_FUNC(sock_connect);
	REG_SCRIPTING_FUNC(sock_bind);
	REG_SCRIPTING_FUNC(sock_listen);
	REG_SCRIPTING_FUNC(sock_accept);
	REG_SCRIPTING_FUNC(sock_remote_addr);
	REG_SCRIPTING_FUNC(sock_write);
	REG_SCRIPTING_FUNC(sock_resolve_64);
}

MODULE_FINI
{
	unreg_module_load_func(NULL, module_unloaded);
	for(unsigned int i = 0; i < sockets->count; i++) {
		struct scripting_sock *ssock = sockets->data[i]->ptr;
		if(ssock->sock) {
			sock_close(ssock->sock);
		}
		scripting_arg_free(ssock->event_func);
		scripting_arg_free(ssock->read_func);
		free(ssock);
	}
	ptrlist_free(sockets);
}

static void module_unloaded(struct module *module)
{
	if(num_irc_handlers) {
		dict_iter(node, irc_handlers_dict()) {
			struct irc_handler_list *list = node->data;
			for(int i = 0; i < (int)list->count; i++) {
				struct irc_handler *handler = list->data[i];
				if(handler->func != (irc_handler_f *)_irc_handler) {
					continue;
				}
				struct scripting_arg *func = handler->extra;
				if(func->callable_module == module) {
					debug("unreg irc handler %p", func);
					_unreg_irc_handler(node->key, (irc_handler_f *)_irc_handler, func);
					num_irc_handlers--;
					i--;
				}
			}
		}
	}

	for(unsigned int i = 0; i < sockets->count; i++) {
		struct scripting_sock *ssock = sockets->data[i]->ptr;
		if(ssock->module != module) {
			continue;
		}
		if(ssock->sock) {
			sock_close(ssock->sock);
		}
		scripting_arg_free(ssock->event_func);
		scripting_arg_free(ssock->read_func);
		free(ssock);
	}
}

SCRIPTING_FUNC(reg_irc_handler)
{
	const char *cmd = scripting_arg_get(args, "cmd", SCRIPTING_ARG_TYPE_STRING);
	if(!cmd) {
		return scripting_raise_error("Required argument missing: cmd");
	}
	struct scripting_arg *func = scripting_arg_get(args, "func", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!func) {
		return scripting_raise_error("Required argument missing: func");
	}
	debug("reg_irc_handler: cmd=%s, func=%p,%p", cmd, func, func->callable);
	_reg_irc_handler(cmd, (irc_handler_f *)_irc_handler, func, (irc_handler_extra_cmp_f*)_cmp_func);
	num_irc_handlers++;
	return NULL;
}

SCRIPTING_FUNC(unreg_irc_handler)
{
	const char *cmd = scripting_arg_get(args, "cmd", SCRIPTING_ARG_TYPE_STRING);
	if(!cmd) {
		return scripting_raise_error("Required argument missing: cmd");
	}
	struct scripting_arg *func = scripting_arg_get(args, "func", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!func) {
		return scripting_raise_error("Required argument missing: func");
	}
	debug("unreg_irc_handler: cmd=%s, func=%p,%p", cmd, func, func->callable);
	_unreg_irc_handler(cmd, (irc_handler_f *)_irc_handler, func);
	num_irc_handlers--;
	return NULL;
}

SCRIPTING_FUNC(irc_send)
{
	const char *msg = scripting_arg_get(args, "msg", SCRIPTING_ARG_TYPE_STRING);
	if(!msg) {
		return scripting_raise_error("Required argument missing: msg");
	}
	int *raw = scripting_arg_get(args, "raw", SCRIPTING_ARG_TYPE_BOOL);
	if(!raw || !*raw)
		irc_send("%s", msg);
	else
		irc_send_raw("%s", msg);
	return NULL;
}

static void _irc_handler(int argc, char **argv, struct irc_source *src, struct scripting_arg *func)
{
	struct dict *args = scripting_args_create_dict();
	struct scripting_arg *arg;
	// args: list of the handler args
	arg = scripting_arg_create(SCRIPTING_ARG_TYPE_LIST, NULL);
	for(int i = 0; i < argc; i++) {
		struct scripting_arg *val = scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(argv[i]));
		ptrlist_add(arg->data.list, 0, val);
	}
	dict_insert(args, strdup("args"), arg);
	// src: dict containing the message source
	dict_insert(args, strdup("src"), _irc_source_to_arg(src));
	SCRIPTING_CALL(func, args);
	dict_free(args);
}

static int _cmp_func(struct scripting_arg *func_a, struct scripting_arg *func_b)
{
	return func_a->callable == func_b->callable;
}

static struct scripting_arg *_irc_source_to_arg(struct irc_source *src)
{
	struct scripting_arg *arg, *val;
	if(!src) {
		return scripting_arg_create(SCRIPTING_ARG_TYPE_NULL);
	}
	arg = scripting_arg_create(SCRIPTING_ARG_TYPE_DICT,
			"nick", scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->nick)),
			"ident", src->ident ? scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->ident)) : NULL,
			"host", src->host ? scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(src->host)) : NULL,
			NULL);
	return arg;
}


SCRIPTING_FUNC(sock_create)
{
	long *type = scripting_arg_get(args, "type", SCRIPTING_ARG_TYPE_INT);
	if(!type) {
		return scripting_raise_error("Required argument missing: type");
	}
	else if(*type > USHRT_MAX) {
		return scripting_raise_error("Invalid value for argument: type");
	}
	struct scripting_arg *event_func = scripting_arg_get(args, "event", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!event_func) {
		return scripting_raise_error("Required argument missing: event");
	}
	struct scripting_arg *read_func = scripting_arg_get(args, "read", SCRIPTING_ARG_TYPE_CALLABLE);
	debug("module %s, type: %lu, event: %p, read: %p", module->name, *type, event_func, read_func);
	struct sock *sock = sock_create(*type, _sock_event, read_func ? _sock_read : NULL);
	if(!sock) {
		scripting_arg_free(event_func);
		scripting_arg_free(read_func);
		return scripting_raise_error("Socket creation failed.");
	}
	const char *bufdelimiter = scripting_arg_get(args, "buffer.delimiter", SCRIPTING_ARG_TYPE_STRING);
	long *bufsize = scripting_arg_get(args, "buffer.size", SCRIPTING_ARG_TYPE_INT);
	if(bufdelimiter && *bufdelimiter && bufsize && *bufsize) {
		sock_set_readbuf(sock, *bufsize, bufdelimiter);
	}
	struct scripting_sock *ctx = malloc(sizeof(struct scripting_sock));
	memset(ctx, 0, sizeof(struct scripting_sock));
	ctx->module = module;
	ctx->sock = sock;
	ctx->event_func = event_func;
	ctx->read_func = read_func;
	sock->ctx = ctx;
	ptrlist_add(sockets, 0, ctx);
	SCRIPTING_RETURN(scripting_arg_create(SCRIPTING_ARG_TYPE_RESOURCE, ctx));
}

static void _sock_event(struct sock *sock, enum sock_event event, int err)
{
	struct scripting_sock *ctx = sock->ctx;
	assert(ctx->event_func);
	struct dict *args = scripting_args_create_dict();
	dict_insert(args, strdup("sock"), scripting_arg_create(SCRIPTING_ARG_TYPE_RESOURCE, ctx));
	dict_insert(args, strdup("event"), scripting_arg_create(SCRIPTING_ARG_TYPE_INT, event));
	dict_insert(args, strdup("err"), scripting_arg_create(SCRIPTING_ARG_TYPE_INT, err));
	SCRIPTING_CALL(ctx->event_func, args);
	dict_free(args);
}

static void _sock_read(struct sock *sock, char *buf, size_t len)
{
	struct scripting_sock *ctx = sock->ctx;
	assert(ctx->read_func);
	struct dict *args = scripting_args_create_dict();
	dict_insert(args, strdup("sock"), scripting_arg_create(SCRIPTING_ARG_TYPE_RESOURCE, ctx));
	dict_insert(args, strdup("str"), scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strndup(buf, len)));
	SCRIPTING_CALL(ctx->read_func, args);
	dict_free(args);
}

SCRIPTING_FUNC(sock_connect)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	const char *host = scripting_arg_get(args, "host", SCRIPTING_ARG_TYPE_STRING);
	if(!host) {
		return scripting_raise_error("Required argument missing: host");
	}
	long *port = scripting_arg_get(args, "port", SCRIPTING_ARG_TYPE_INT);
	if(!port) {
		return scripting_raise_error("Required argument missing: port");
	}
	else if(*port < 1 || *port > 65535) {
		return scripting_raise_error("Invalid value for argument: port");
	}
	if(sock_connect(ctx->sock, host, *port)) {
		ctx->sock = NULL;
		return scripting_raise_error("Could not connect socket");
	}
	return NULL;
}

SCRIPTING_FUNC(sock_bind)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	else if(!ctx->sock) {
		return scripting_raise_error("Cannot operate on a destroyed socket");
	}
	const char *host = scripting_arg_get(args, "host", SCRIPTING_ARG_TYPE_STRING);
	if(!host) {
		return scripting_raise_error("Required argument missing: host");
	}
	long *port = scripting_arg_get(args, "port", SCRIPTING_ARG_TYPE_INT);
	if(!port) {
		port = alloca(sizeof(long));
		*port = 0;
	}
	else if(*port < 1 || *port > 65535) {
		return scripting_raise_error("Invalid value for argument: port");
	}
	if(sock_bind(ctx->sock, host, *port)) {
		ctx->sock = NULL;
		return scripting_raise_error("Could not bind socket");
	}
	return NULL;
}

SCRIPTING_FUNC(sock_listen)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	else if(!ctx->sock) {
		return scripting_raise_error("Cannot operate on a destroyed socket");
	}
	const char *ssl_pem = scripting_arg_get(args, "ssl_pem", SCRIPTING_ARG_TYPE_STRING);
	if(ssl_pem && !*ssl_pem) {
		ssl_pem = NULL;
	}
	if(sock_listen(ctx->sock, ssl_pem)) {
		ctx->sock = NULL;
		return scripting_raise_error("Could not switch socket to listening mode");
	}
	return NULL;
}

SCRIPTING_FUNC(sock_accept)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	else if(!ctx->sock) {
		return scripting_raise_error("Cannot operate on a destroyed socket");
	}
	struct scripting_arg *event_func = scripting_arg_get(args, "event", SCRIPTING_ARG_TYPE_CALLABLE);
	if(!event_func) {
		return scripting_raise_error("Required argument missing: event");
	}
	struct scripting_arg *read_func = scripting_arg_get(args, "read", SCRIPTING_ARG_TYPE_CALLABLE);
	debug("module %s, event: %p, read: %p", module->name, event_func, read_func);
	struct sock *sock = sock_accept(ctx->sock, _sock_event, read_func ? _sock_read : NULL);
	if(!sock) {
		scripting_arg_free(event_func);
		scripting_arg_free(read_func);
		return scripting_raise_error("Could not accept new connection");
	}
	const char *bufdelimiter = scripting_arg_get(args, "buffer.delimiter", SCRIPTING_ARG_TYPE_STRING);
	long *bufsize = scripting_arg_get(args, "buffer.size", SCRIPTING_ARG_TYPE_INT);
	if(bufdelimiter && *bufdelimiter && bufsize && *bufsize) {
		sock_set_readbuf(sock, *bufsize, bufdelimiter);
	}
	struct scripting_sock *ctx2 = malloc(sizeof(struct scripting_sock));
	memset(ctx2, 0, sizeof(struct scripting_sock));
	ctx2->module = module;
	ctx2->sock = sock;
	ctx2->event_func = event_func;
	ctx2->read_func = read_func;
	sock->ctx = ctx2;
	ptrlist_add(sockets, 0, ctx2);
	SCRIPTING_RETURN(scripting_arg_create(SCRIPTING_ARG_TYPE_RESOURCE, ctx2));
}

SCRIPTING_FUNC(sock_remote_addr)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	else if(!ctx->sock) {
		return scripting_raise_error("Cannot operate on a destroyed socket");
	}
	else if(!(ctx->sock->flags & (SOCK_IPV4|SOCK_IPV6))) {
		return scripting_raise_error("Socket is not using IPv4/IPv6");
	}
	char buf[INET6_ADDRSTRLEN];
	void *addr = NULL;
	if(ctx->sock->flags & SOCK_IPV4) {
		addr = &((struct sockaddr_in *)(ctx->sock)->sockaddr_remote)->sin_addr;
	}
	else {
		addr = &((struct sockaddr_in6 *)(ctx->sock)->sockaddr_remote)->sin6_addr;
	}
	inet_ntop(ctx->sock->flags & SOCK_IPV4 ? AF_INET : AF_INET6, addr, buf, INET6_ADDRSTRLEN);
	SCRIPTING_RETURN(scripting_arg_create(SCRIPTING_ARG_TYPE_STRING, strdup(buf)));
}

SCRIPTING_FUNC(sock_write)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	else if(!ctx->sock) {
		return scripting_raise_error("Cannot operate on a destroyed socket");
	}
	const char *msg = scripting_arg_get(args, "msg", SCRIPTING_ARG_TYPE_STRING);
	if(!msg) {
		return scripting_raise_error("Required argument missing: msg");
	}
	sock_write(ctx->sock, msg, strlen(msg));
	return NULL;
}

SCRIPTING_FUNC(sock_close)
{
	struct scripting_sock *ctx = scripting_arg_get(args, "sock", SCRIPTING_ARG_TYPE_RESOURCE);
	if(!ctx) {
		return scripting_raise_error("Required argument missing: sock");
	}
	ptrlist_del_ptr(sockets, ctx);
	if(ctx->sock) {
		sock_close(ctx->sock);
	}
	scripting_arg_free(ctx->event_func);
	scripting_arg_free(ctx->read_func);
	free(ctx);
	return NULL;
}

SCRIPTING_FUNC(sock_resolve_64)
{
	const char *host = scripting_arg_get(args, "host", SCRIPTING_ARG_TYPE_STRING);
	if(!host) {
		return scripting_raise_error("Required argument missing: host");
	}
	SCRIPTING_RETURN(scripting_arg_create(SCRIPTING_ARG_TYPE_INT, sock_resolve_64(host)));
}
