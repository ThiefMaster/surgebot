#include "global.h"
#include "timer.h"
#include "tokenize.h"
#include "sock.h"
#include "conf.h"
#include "http.h"

#ifdef SURGEBOT_MODULE
	#include "module.h"
	#include "modules/tools/tools.h"
	MODULE_DEPENDS("tools", NULL);
	static struct module *this;
#else
	#define MODULE_INIT void http_init()
	#define MODULE_FINI void http_fini()
	static void *this = (void *)0xdeadbeef;
#endif

#define REQUEST_TIMEOUT		30
#define HTTP_404_RESPONSE	"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\n<p>The requested URL <b>%s</b> was not found on this server.</p>\n</body></html>"

DECLARE_LIST(client_list, struct http_client *)
IMPLEMENT_LIST(client_list, struct http_client *)
struct http_header;
DECLARE_LIST(header_list, struct http_header *)
IMPLEMENT_LIST(header_list, struct http_header *)
DECLARE_LIST(handler_list, struct http_handler *)
IMPLEMENT_LIST(handler_list, struct http_handler *)

static unsigned long requests_served = 0;

struct http_response_code
{
	int code;
	const char *phrase;
} http_response_codes[] =
{
	{ 100, "Continue" },
	{ 200, "OK" },
	{ 300, "Multiple Choices" },
	{ 302, "Found" },
	{ 400, "Bad Request" },
	{ 401, "Unauthorized" },
	{ 403, "Forbidden" },
	{ 404, "Not Found" },
	{ 405, "Method Not Allowed" },
	{ 408, "Request Timeout" },
	{ 411, "Length Required" },
	{ 413, "Request Entity Too Large" },
	{ 500, "Internal Server Error" },
	{ 501, "Not Implemented" },
	{ 503, "Service Unavailable" },
	{ 505, "HTTP Version Not Supported" }
};

struct http_method_map
{
	const char *name;
	unsigned int len;
	enum http_method method;
} http_methods[] =
{
	{ "GET ",  4, HTTP_GET },
	{ "HEAD ", 5, HTTP_HEAD },
	{ "POST ", 5, HTTP_POST }
};

struct http_header
{
	const char *key;
	const char *value;

	unsigned int klen;
	unsigned int vlen;
};

static struct {
	char *listen_ip;
	unsigned int listen_port;
	char *listen_pem;
} http_conf;


static void http_conf_reload();
static void listener_event(struct sock *sock, enum sock_event event, int err);
static void listener_start();
static void http_client_event(struct sock *sock, enum sock_event event, int err);
static void http_client_read(struct sock *sock, char *buf, size_t len);
static const char *http_get_response_phrase(int code);
static void http_write_header_default(struct http_client *client);
static void http_headers_flush(struct header_list *headers);
static void http_handler_404(struct http_client *client, char *uri, int argc, char **argv);
static int http_parse_request_line(struct http_client *client, const char *line, unsigned int len);
static int http_parse_header(struct http_client *client, const char *line, unsigned int n);
static int http_parse(struct http_client *client);
static void http_process_request(struct http_client *client);
static void http_request_complete(struct http_client *client);
static void http_request_timeout(void *bound, struct http_client *client);
static void http_writesock(struct http_client *client);
static struct http_client *http_client_accept(struct sock *listener);
static void http_client_del(struct http_client *client, unsigned char close_sock);
static struct http_client *http_client_bysock(struct sock *sock);
static http_handler_f *http_handler_find(const char *uri);
static void http_handler_del_handler(struct http_handler *handler);

static struct client_list *clients;
static struct handler_list *handlers;
static struct sock *listener;

MODULE_INIT
{
	#ifdef SURGEBOT_MODULE
	this = self;
	#endif

	reg_conf_reload_func(http_conf_reload);
	http_conf_reload();

	clients = client_list_create();
	handlers = handler_list_create();
	listener_start();
}

MODULE_FINI
{
	sock_close(listener);
	timer_del(this, "http_client_timeout", 0, NULL, NULL, TIMER_IGNORE_ALL & ~(TIMER_IGNORE_NAME|TIMER_IGNORE_BOUND));
	while(clients->count)
		http_client_del(clients->data[clients->count - 1], 1);
	client_list_free(clients);
	while(handlers->count)
		http_handler_del_handler(handlers->data[handlers->count - 1]);
	handler_list_free(handlers);
	unreg_conf_reload_func(http_conf_reload);
}

unsigned long http_num_served_requests()
{
	return requests_served;
}

static void http_conf_reload()
{
	char *str;

	unsigned int old_port = http_conf.listen_port;
	http_conf.listen_ip	= ((str = conf_get("httpd/listen_ip", DB_STRING)) ? str : "0.0.0.0");
	http_conf.listen_port	= ((str = conf_get("httpd/listen_port", DB_STRING)) ? atoi(str) : 8000);
	http_conf.listen_pem	= ((str = conf_get("httpd/listen_pem", DB_STRING)) ? str : NULL);

	if(!http_conf.listen_port)
		log_append(LOG_ERROR, "/http/listen_port must be set");

	const char *old_pem = old_port ? conf_get_old("http/listen_pem", DB_STRING) : NULL;
	if(old_port && // no old port = first call = don't mess with the listener here
	   ((http_conf.listen_port != old_port) || // port changed
	    (old_pem && !http_conf.listen_pem) || (!old_pem && http_conf.listen_pem) || // ssl enabled/disabled
	    (old_pem && http_conf.listen_pem && strcasecmp(old_pem, http_conf.listen_pem)))) // new certificate
	{
		debug("Changing listen port from %d to %d and/or ssl cert from %s to %s", old_port, http_conf.listen_port, old_pem, http_conf.listen_pem);
		listener_start();
	}
}

static void listener_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ACCEPT)
	{
		struct http_client *client = http_client_accept(sock);
		if(client)
			log_append(LOG_INFO, "New connection from %s on socket %d", inet_ntoa(((struct sockaddr_in *)client->sock->sockaddr_remote)->sin_addr), client->sock->fd);
	}
}

static void listener_start()
{
	if(listener)
		sock_close(listener);
	log_append(LOG_INFO, "Creating listener on %s:%d with%s SSL", http_conf.listen_ip, http_conf.listen_port, (http_conf.listen_pem ? "" : "out"));
	listener = sock_create(SOCK_IPV4|(http_conf.listen_pem ? SOCK_SSL : 0), listener_event, NULL);
	sock_bind(listener, http_conf.listen_ip, http_conf.listen_port);
	sock_listen(listener, http_conf.listen_pem);
}

static void http_client_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR || event == EV_HANGUP)
	{
		if(event == EV_ERROR)
			log_append(LOG_WARNING, "Socket error on socket %d: %s (%d)", sock->fd, strerror(err), err);
		else
			log_append(LOG_INFO, "Socket %d hung up", sock->fd);
		http_client_del(http_client_bysock(sock), 1);
	}
	else if(event == EV_WRITE)
	{
		struct http_client *client = http_client_bysock(sock);
		if(!client)
		{
			log_append(LOG_WARNING, "Got EV_WRITE on socket %d which belongs to no client", sock->fd);
			return;
		}

		if(sock->send_queue_len == 0 && (client->state & (HTTP_CONNECTION_CLOSE|HTTP_HEADERS_SENT))) // wrote everything
		{
			http_request_complete(client);
		}
	}
}

static void http_client_read(struct sock *sock, char *buf, size_t len)
{
	struct http_client *client = http_client_bysock(sock);
	if(!client)
	{
		log_append(LOG_WARNING, "Got data on socket which belongs to no client");
		return;
	}

	if(client->rbuf->len + len > REQUEST_MAX_SIZE)
	{
		http_send_error(client, 413);
		return;
	}

	stringbuffer_append_string_n(client->rbuf, buf, len);
	http_process_request(client);
}

static const char *http_get_response_phrase(int code)
{
	const char *phrase = "";
	for(int i = 0; i < ArraySize(http_response_codes); i++)
	{
		if(http_response_codes[i].code == code)
			phrase = http_response_codes[i].phrase;
	}

	return phrase;
}

void http_write_header_redirect(struct http_client *client, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	http_write_header_status(client, 302);
	stringbuffer_append_string(client->hbuf, "Location: ");
	stringbuffer_append_vprintf(client->hbuf, fmt, args);
	stringbuffer_append_string(client->hbuf, "\r\n");
	va_end(args);
}

void http_write_header_status(struct http_client *client, int code)
{
	client->hbuf->len = 0;
	stringbuffer_append_printf(client->hbuf, "HTTP/1.1 %d %s\r\n", code, http_get_response_phrase(code));
}

void http_send_error(struct http_client *client, int code)
{
	http_write_header_status(client, code);
	client->state |= HTTP_CONNECTION_CLOSE;
	http_writesock(client);
}

static void http_write_header_default(struct http_client *client)
{
	http_write_header(client, "Content-Length", "%d", client->wbuf->len);
	if(client->state & HTTP_CONNECTION_CLOSE)
		http_write_header(client, "Connection", "close");
	else
		http_write_header(client, "Connection", "keep-alive");
}

void http_write_header(struct http_client *client, const char *name, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	stringbuffer_append_string(client->hbuf, name);
	stringbuffer_append_string(client->hbuf, ": ");
	stringbuffer_append_vprintf(client->hbuf, fmt, args);
	stringbuffer_append_string(client->hbuf, "\r\n");
	va_end(args);
}

void http_write(struct http_client *client, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	stringbuffer_append_vprintf(client->wbuf, fmt, args);
	va_end(args);
}

static int cmp_http_header_field(const void *a_, const void *b_)
{
	const struct http_header *a = *(const struct http_header **)a_;
	const struct http_header *b = *(const struct http_header **)b_;
	return strncasecmp(a->key, b->key, a->klen < b->klen ? a->klen : b->klen);
}

const char *http_header_get(struct http_client *client, const char *key)
{
	struct http_header *search, **res, *field;
	static char buf[8192];
	unsigned int len;

	/* Binary search the header list for the requested header. */
	search = alloca(sizeof(struct http_header));
	memset(search, 0, sizeof(struct http_header));
	search->key = key;
	search->klen = strlen(key);
	if(!(res = bsearch(&search, client->headers->data, client->headers->count, sizeof(search), cmp_http_header_field)))
		return NULL;

	field = *res;
	len = field->vlen < sizeof(buf) ? field->vlen : sizeof(buf) - 1;
	strncpy(buf, field->value, len);
	buf[len] = '\0';
	return buf;
}

static void http_headers_flush(struct header_list *headers)
{
	while(headers->count)
	{
		struct http_header *header = headers->data[headers->count - 1];
		header_list_del(headers, header);
		free(header);
	}
}

static void http_handler_404(struct http_client *client, char *uri, int argc, char **argv)
{
	char *tmp = strdup(uri);
	strip_html_tags(tmp);
	http_write(client, HTTP_404_RESPONSE, tmp);
	free(tmp);
	http_write_header_status(client, 404);
	http_write_header_default(client);
	http_write_header(client, "Content-Type", "text/html");
}

static http_handler_f *http_handler_find(const char *uri)
{
	struct http_handler *found = NULL;
	size_t found_len = 0;

	// we search for all matching handlers and take the one with the longest
	// mask assuming it contains less wildcarded parts then the others
	for(unsigned int i = 0; i < handlers->count; i++)
	{
		struct http_handler *handler = handlers->data[i];
		if(match(handler->uri, uri) == 0 && strlen(handler->uri) > found_len)
		{
			found = handler;
			found_len = strlen(handler->uri);
		}
	}

	return (found ? found->func : NULL);
}

void http_handler_add(const char *uri, http_handler_f *func)
{
	struct http_handler *handler = malloc(sizeof(struct http_handler));
	memset(handler, 0, sizeof(struct http_handler));
	handler->uri = strdup(uri);
	handler->func = func;
	handler_list_add(handlers, handler);
}

void http_handler_del(const char *uri)
{
	for(unsigned int i = 0; i < handlers->count; i++)
	{
		struct http_handler *handler = handlers->data[i];
		if(!strcmp(handler->uri, uri))
		{
			http_handler_del_handler(handler);
			return;
		}
	}
}

void http_handler_add_list(const struct http_handler *handlers)
{
	while(handlers->uri)
	{
		http_handler_add(handlers->uri, handlers->func);
		handlers++;
	}
}

void http_handler_del_list(const struct http_handler *handlers)
{
	while(handlers->uri)
	{
		http_handler_del(handlers->uri);
		handlers++;
	}
}

void http_handler_del_handler(struct http_handler *handler)
{
	handler_list_del(handlers, handler);
	free(handler->uri);
	free(handler);
}

static int http_parse_request_line(struct http_client *client, const char *line, unsigned int len)
{
	unsigned int ii, uri, pos = 0;

	/* Extract the method portion of the Request-Line. */
	for(ii = 0; ii < ArraySize(http_methods); ii++)
	{
		if(strncasecmp(line, http_methods[ii].name, http_methods[ii].len))
			continue;

		client->method = http_methods[ii].method;
		pos += http_methods[ii].len;
		break;
	}

	if(client->method == HTTP_NONE)
		return 501;

	/* Locate the end of the request's URI and verify version information
	   is present -- while we're at it, make sure the major version is 1. */
	for(ii = uri = pos; ii < len; ii++)
		if(line[ii] == ' ')
			break;
	if(ii == uri || (ii + 8) > len)
		return 400;
	if(strncmp(line + ii + 1, "HTTP/1.", 7))
		return 505;

	/* Save the request URI. */
	client->uri = malloc(ii - uri + 1);
	strncpy(client->uri, line + uri, ii - uri);
	client->uri[ii - uri] = '\0';
	client->uri = urldecode(client->uri);

	/* Map the URI to a handler. */
	client->handler = http_handler_find(client->uri);
	if(!client->handler)
		client->handler = http_handler_404;

	/* Extract the request's HTTP minor version. */
	client->version_minor = atoi(&line[ii + 8]);
	if(client->version_minor == 0)
		client->state |= HTTP_CONNECTION_CLOSE;

	return 0;
}

static int http_parse_header(struct http_client *client, const char *line, unsigned int n)
{
	struct http_header *field;
	unsigned int cpos, vpos;

	/* Look for a colon and treat everything preceding it as a key
	   and anything following optional trailing spaces as a value.
	   (non-RFC behavior) */
	for(cpos = 0; cpos < n - 1; cpos++)
		if(line[cpos] == ':')
			break;

	if(line[cpos] != ':')
		return 400;
	for(vpos = cpos + 1; vpos < n; vpos++)
		if(line[vpos] != ' ')
			break;

	/* Save field key/value positions for lookup later. */
	field = malloc(sizeof(struct http_header));
	memset(field, 0, sizeof(struct http_header));
	field->key = line;
	field->value = line + vpos;
	field->klen = cpos;
	field->vlen = n - vpos;

	char tmp1[128], tmp2[256];
	strlcpy(tmp1, field->key, min(field->klen + 1, 128));
	strlcpy(tmp2, field->value, min(field->vlen + 1, 256));
	debug("Header: %s: %s", tmp1, tmp2);
	header_list_add(client->headers, field);

	/* Extract the values of some key headers for internal use. */
	if(!strncasecmp("Connection", field->key, field->klen))
	{
		if(!strncasecmp("close", field->value, field->vlen))
			client->state |= HTTP_CONNECTION_CLOSE;
	}
	else if(!strncasecmp("Content-Length", field->key, field->klen))
	{
		/* Reject requests containing ambiguous Content-Length headers. */
		if(client->content_length != 0)
			return 400;

		client->content_length = atoi(field->value);
		if(client->content_length == 0)
			return 400;
	}
	else if(!strncasecmp("Transfer-Encoding", field->key, field->klen))
	{
		/* Reject requests using an unsupported Transfer-Encoding (not
		   identity). */
		if(strncasecmp("identity", field->value, field->vlen))
			return 501;
	}

	return 0;
}


static int http_parse(struct http_client *client)
{
	unsigned int start, size, len;
	const char *header = client->rbuf->string;
	len = client->rbuf->len;

	if(!len)
		return 0;

	/* Scan the HTTP header starting from the last parsed line's end. */
	unsigned int ii = start = client->ppos;
	while(ii < len)
	{
		int code = 0;

		/* Execute the main loop body only if a line terminator is found. */
		if((header[ii] == '\r' && header[ii + 1] == '\n') || header[ii] == '\n') // blank line
			size = 0;
		else
		{
			if(header[ii + 1] != '\r' && header[ii + 1] != '\n')
			{
				ii++;
				continue;
			}
			size = ii - start + 1;
		}

		if(size == 0)
		{
			/* Handle blank lines. */
			switch(client->method)
			{
				case HTTP_NONE:
					/* No request line found yet; ignore it. */
					break;
				case HTTP_POST:
					/* Require content for POST requests; fall through. */
					if(!client->content_length)
					{
						code = 411;
						break;
					}
				default:
					/* Finish request processing: store content location and
					   and sort headers for later lookup. */
					if(header[ii] == '\r' && header[ii + 1] == '\n')
						ii += 2;
					else if(header[ii] == '\n')
						ii += 1;
					client->content_start = ii;
					start = len;
			}


			if(client->content_start)
			{
				qsort(client->headers->data, client->headers->count, sizeof(client->headers->data[0]), cmp_http_header_field);
				break;
			}
		}
		else if(client->method == HTTP_NONE)
		{
			/* Parse the first non-blank line as the Request-Line. */
			code = http_parse_request_line(client, header + start, size);
		}
		else
		{
			/* Parse everything else as a key: value pair. */
			code = http_parse_header(client, header + start, size);
		}

		if(code)
		{
			http_send_error(client, code);
			return 0;
		}

		ii = start + size;
		if(header[ii] == '\r' && header[ii + 1] == '\n')
			ii += 2;
		else if(header[ii] == '\n')
			ii += 1;

		start = ii;
	}

	client->ppos = start;
	return client->content_start;
}

static void http_process_request(struct http_client *client)
{
	char *uriv[16], *uri_dup;
	int uric;

	debug("Processing http request for client %p", client);
	if(!client->content_start && !http_parse(client)) // http_parse returns non-zero when headers are parsed completely
		return;

	unsigned int size = client->content_start + client->content_length;
	if(client->rbuf->len < size) // do we have to read more data?
		return;

	client->content = client->rbuf->string + client->content_start;

	uri_dup = strdup(client->uri);
	uric = *(uri_dup + 1) == '\0' ? 0 : tokenize(uri_dup + 1, uriv, ArraySize(uriv), '/', 0);

	if(client->handler != http_handler_404)
		http_write_header_status(client, 200);
	client->handler(client, client->uri, uric, uriv);
	free(uri_dup);
	http_write_header_default(client);
	http_writesock(client);
}

static void http_request_complete(struct http_client *client)
{
	unsigned int size;

	debug("Request for '%s' (client %p) is completed", client->uri, client);
	requests_served++;
	if(client->state & HTTP_CONNECTION_CLOSE)
	{
		http_client_del(client, 1);
		return;
	}

	size = client->content_start + client->content_length;
	client->rbuf->len -= size;
	memmove(client->rbuf->string, client->rbuf->string + size, client->rbuf->len);

	MyFree(client->uri);
	http_headers_flush(client->headers);
	stringbuffer_flush(client->hbuf);
	stringbuffer_flush(client->wbuf);

	client->ppos = client->content_start = client->content_length = 0;
	client->state = HTTP_CONNECTION_SERVED;
	client->method = HTTP_NONE;

	timer_del(this, "http_client_timeout", 0, NULL, client, TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC);
	timer_add(this, "http_client_timeout", now + REQUEST_TIMEOUT, (timer_f*)http_request_timeout, client, 0, 1);

	if(client->rbuf->len)
		http_process_request(client);
}

struct dict *http_parse_vars(struct http_client *client, enum http_method type)
{
	char *str, *orig_str, *key, *ptr;
	struct dict *vars;
	unsigned int end = 0;

	vars = dict_create();
	dict_set_free_funcs(vars, free, free);

	if(type == HTTP_POST)
		str = strdup(client->content);
	else if(type == HTTP_GET)
	{
		str = strchr(client->uri, '?');
		if(str)
			str = strdup(str + 1);
		else
			return vars;
	}

	if(!str || strlen(str) == 0)
	{
		MyFree(str);
		return vars;
	}

	orig_str = str;
	key = NULL;
	while(!end)
	{
		if(!(ptr = strchr(str, '=')))
			break;
		*ptr = '\0';
		key = str;
		str = ptr + 1;

		ptr = strchrnul(str, '&');
		if(*ptr == '\0')
			end = 1;
		*ptr = '\0';
		dict_insert(vars, strdup(urldecode(key)), strlen(str) ? strdup(urldecode(str)) : NULL);
		if(!end)
			str = ptr + 1;
	}

	free(orig_str);
	return vars;
}

struct dict *http_parse_cookies(struct http_client *client)
{
	char *str, *orig_str, *key, *ptr;
	const char *cookie;
	struct dict *vars;
	unsigned int end = 0;

	vars = dict_create();
	dict_set_free_funcs(vars, free, free);

	cookie = http_header_get(client, "Cookie");
	if(!cookie || strlen(cookie) == 0)
		return vars;

	orig_str = str = strdup(cookie);
	key = NULL;
	while(!end)
	{
		if(!(ptr = strchr(str, '=')))
			break;
		*ptr = '\0';
		key = str;
		str = ptr + 1;

		ptr = strchrnul(str, ';');
		if(*ptr == '\0')
			end = 1;
		*ptr = '\0';
		while(*ptr == ' ')
			ptr++;
		dict_insert(vars, strdup(urldecode(key)), strlen(str) ? strdup(urldecode(str)) : NULL);
		if(!end)
			str = ptr + 1;
	}

	free(orig_str);
	return vars;
}

static void http_request_timeout(void *bound, struct http_client *client)
{
	if(client->state & HTTP_CONNECTION_SERVED)
	{
		debug("Client %p timed out (served request: yes)", client);
		http_client_del(client, 1);
		return;
	}

	debug("Client %p timed out (served request: no)", client);
	http_send_error(client, 408);
}


static void http_writesock(struct http_client *client)
{
	struct stringbuffer *buf;

	if(client->state & HTTP_HEADERS_SENT)
		buf = client->wbuf;
	else
	{
		buf = client->hbuf;

		// terminate headers if not done yet
		if(!(client->state & HTTP_HEADERS_DONE))
		{
			stringbuffer_append_string(client->hbuf, "\r\n");
			client->state |= HTTP_HEADERS_DONE;
		}
	}

	sock_write(client->sock, buf->string, buf->len);
	stringbuffer_flush(buf);

	if(!(client->state & HTTP_HEADERS_SENT))
	{
		client->state |= HTTP_HEADERS_SENT;
		http_writesock(client); // headers done; now write the body
	}
}

struct http_client *http_client_accept(struct sock *listener)
{
	struct http_client *client;
	struct sock *sock = sock_accept(listener, http_client_event, http_client_read);
	if(!sock)
		return NULL;

	client = malloc(sizeof(struct http_client));
	memset(client, 0, sizeof(struct http_client));
	client->sock = sock;
	client->rbuf = stringbuffer_create();
	client->hbuf = stringbuffer_create();
	client->wbuf = stringbuffer_create();
	client->headers = header_list_create();
	client_list_add(clients, client);
	timer_add(this, "http_client_timeout", now + REQUEST_TIMEOUT, (timer_f*)http_request_timeout, client, 0, 1);
	return client;
}

static struct http_client *http_client_bysock(struct sock *sock)
{
	for(int i = 0; i < clients->count; i++)
	{
		if(clients->data[i]->sock == sock)
			return clients->data[i];
	}

	return NULL;
}

static void http_client_del(struct http_client *client, unsigned char close_sock)
{
	if(!client)
		return;

	client_list_del(clients, client);
	if(close_sock)
		sock_close(client->sock);
	timer_del(this, "http_client_timeout", 0, NULL, client, TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC);
	stringbuffer_free(client->rbuf);
	stringbuffer_free(client->hbuf);
	stringbuffer_free(client->wbuf);
	http_headers_flush(client->headers);
	header_list_free(client->headers);
	MyFree(client->uri);
	free(client);
}
