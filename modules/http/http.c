#include "global.h"
#include <stdlib.h>
#include "module.h"
#include "chanuser.h"
#include "irc.h"
#include "tools.h"
#include "http.h" // Prototypes
#include "sock.h"
#include "log.h"

//Todo: Implement timer of maybe 20 seconds for timeout (or provide it as argument?)

MODULE_DEPENDS(NULL);

static struct dict *requests;

static void http_sock_event(struct sock *, enum sock_event, int err);
static void http_sock_read(struct sock *, char *buf, size_t len);
static void HTTPRequest_event(struct HTTPRequest *, enum HTTPRequest_event);
static struct HTTPRequest *http_find_sock(struct sock *);
static struct HTTPHost *parse_host(const char *);
static void HTTPRequest_set_host(struct HTTPRequest *, const char *);

static unsigned long next_id = 0;

MODULE_INIT
{
	requests = dict_create();
	dict_set_free_funcs(requests, free, (dict_free_f*)HTTPRequest_free);
}

MODULE_FINI
{
	dict_free(requests);
}

struct HTTPRequest *HTTPRequest_create(const char *host, http_event_f *event_func, http_read_f *read_func)
{
	struct HTTPRequest *http;
	char tmp[32];

	debug("Creating new HTTP request to host \"%s\" with ID %lu", host, next_id);
	snprintf(tmp, sizeof(tmp), "%lu", next_id++);

	http = malloc(sizeof(struct HTTPRequest));
	memset(http, 0, sizeof(struct HTTPRequest));

	// Create structs/Assign main struct
	http->request_headers = dict_create();
	http->response_headers = dict_create();

	http->read_funcs = ptrlist_create();
	http->event_funcs = ptrlist_create();

	http->buf = stringbuffer_create();

	HTTPRequest_set_host(http, host);

	http->in_headers = 1;
	http->forward_request = 1;
	http->forward_request_foreign = 1;
	http->id = strdup(tmp);
	http->method = "GET";

	// Set appropriate struct settings
	dict_set_free_funcs(http->request_headers, free, free);
	dict_set_free_funcs(http->response_headers, free, free);

	// Fill struct with given values
	if(read_func)
		ptrlist_add(http->read_funcs, 0, read_func);

	if(event_func)
		ptrlist_add(http->event_funcs, 0, event_func);

	HTTPRequest_add_header(http, "Host", http->host->host);
	HTTPRequest_add_header(http, "Connection", "Close");

	dict_insert(requests, http->id, http);

	return http;
}

void HTTPRequest_free(struct HTTPRequest *http)
{
	if(http)
	{
		debug("Freeing HTTP Request %s", http->id);
		if(http->sock)
			sock_close(http->sock);

		if(http->buf)
			stringbuffer_free(http->buf);

		dict_free(http->request_headers);
		dict_free(http->response_headers);
		ptrlist_free(http->read_funcs);
		ptrlist_free(http->event_funcs);
		free(http->host->host);
		if(http->host->path)
			free(http->host->path);
		free(http->host);
		MyFree(http->payload);
		if(http->extra_str)
			free(http->extra_str);
		free(http);
	}
}

void HTTPRequest_cancel(struct HTTPRequest *http)
{
	debug("Cancelling http request %s (http://%s/%s)", http->id, http->host->host, http->host->path);
	dict_delete(requests, http->id);
}

void HTTPRequest_add_header(struct HTTPRequest *http, const char *name, const char *content)
{
	struct dict_node *node = dict_find_node(http->request_headers, name);

	// If the node already exists, only replace its content
	if(node)
	{
		if(node->data)
			free(node->data);

		node->data = strdup(content);
	}
	else
		dict_insert(http->request_headers, strdup(name), strdup(content));
}

void HTTPRequest_del_header(struct HTTPRequest *http, const char *name)
{
	dict_delete(http->request_headers, name);
}

void HTTPRequest_connect(struct HTTPRequest *http)
{
	assert(!http->sock);
	unsigned short sockflags = sock_resolve_64(http->host->host);
	if(!sockflags)
		sockflags = SOCK_IPV4; // so we get an error later. i know it's ugly!
	sockflags |= SOCK_QUIET;
	if(http->host->ssl)
		sockflags |= SOCK_SSL;

	http->sock = sock_create(sockflags, http_sock_event, http_sock_read);
	debug("Connecting HTTP Request %s [0x%x] => %s:%u", http->id, sockflags, http->host->host, http->host->port);
	sock_connect(http->sock, http->host->host, http->host->port);
}

void HTTPRequest_disconnect(struct HTTPRequest *http)
{
	sock_close(http->sock);
	http->sock = NULL;

	dict_clear(http->response_headers);
	stringbuffer_flush(http->buf);
	http->in_headers = 1;
}

static void http_sock_event(struct sock *sock, enum sock_event event, int err)
{
	struct HTTPRequest *http;
	assert((http = http_find_sock(sock)));

	switch(event)
	{
		case EV_HANGUP:
		{
			/* Socket was closed by remote side = reached end of http content
			 * -> Dump (remaining) read buffer to socket-read-functions
			 */
			for(unsigned int i = 0; i < http->read_funcs->count; i++)
				((http_read_f*)http->read_funcs->data[i]->ptr)(http, http->buf->string, (unsigned int)http->buf->len);

			stringbuffer_free(http->buf);
			http->buf = NULL;

			// Fall through
		}
		case EV_ERROR:
		{
			http->sock = NULL;
			HTTPRequest_event(http, (event == EV_ERROR ? H_EV_TIMEOUT : H_EV_HANGUP));
			dict_delete(requests, http->id);
			break;
		}

		case EV_CONNECT:
		{
			sock_write_fmt(sock, "%s /%s HTTP/1.0\r\n", http->method, (http->host->path ? http->host->path : ""));
			dict_iter(node, http->request_headers)
			{
				sock_write_fmt(sock, "%s: %s\r\n", node->key, (char*)node->data);
			}
			if(http->payload)
			{
				sock_write_fmt(sock, "Content-length: %lu\r\n", strlen(http->payload));
				sock_write_fmt(sock, "Content-type: %s\r\n", http->payload_type);
			}
			// Empty line to signalise end of headers
			sock_write(sock, "\r\n", 2);
			if(http->payload)
			{
				sock_write(sock, http->payload, strlen(http->payload));
			}
			break;
		}
		default:
			break;
	}
}

static void http_sock_read(struct sock *sock, char *buf, size_t len)
{
	char *str, *tmp;
	struct HTTPRequest *http = http_find_sock(sock);
	assert(http);
	stringbuffer_append_string_n(http->buf, buf, len);

	if(!http->in_headers && !http->read_linewise)
		return;

	while((str = stringbuffer_shift(http->buf, "\n", 1)))
	{
		len = strlen(str);
		// Do we have \r\n aka windows-type line endings?
		if(len && str[len - 1] == '\r')
			str[--len] = '\0';

		if(!http->in_headers)
		{
			for(unsigned int i = 0; i < http->read_funcs->count; i++)
			{
				http_read_f *read_f = http->read_funcs->data[i]->ptr;
				read_f(http, str, len);
			}
			continue;
		}

		// Empty line... as in end of headers?
		if(!len)
		{
			http->in_headers = 0;
			free(str);

			if(http->read_linewise)
				continue;
			break;
		}

		// So apparently, this is a header, let's find a colon
		if(!(tmp = strchr(str, ':')))
		{
			// No colon, this must be the first line of headers in the form "HTTP/1.0 302 Found"
			char *tmp2;
			if(!(tmp2 = strchr(str, ' ')) || !(http->status = atoi(tmp2)))
			{
				log_append(LOG_ERROR, "(HTTP Request %s) Invalid HTTP header format: %s", http->id, str);
				// Send timeout event and free request
				HTTPRequest_event(http, H_EV_TIMEOUT);
				sock_close(sock); // Will trigger a socket event deleting the HTTP Request

				free(str);
				return;
			}
		}
		else
		{
			// Not the first line of headers, in the format "Name: Content"
			str[tmp - str] = '\0';
			tmp += 2;
			dict_insert(http->response_headers, strdup(str), strdup(tmp));

			// In case we got a Location-header, we need to redirect the query
			if(!strcasecmp(str, "Location"))
			{
				if(http->forward_request)
				{
					struct HTTPHost *host = parse_host(tmp);
					if(http->forward_request_foreign || !strcasecmp(http->host->host, host->host))
					{
						debug("Redirecting HTTP Request %s to %s", http->id, tmp);
						HTTPRequest_disconnect(http);
						HTTPRequest_set_host(http, tmp);
						HTTPRequest_connect(http);
					}
				}
				free(str);
				return;
			}
		}

		free(str);
	}
}

static void HTTPRequest_event(struct HTTPRequest *http, enum HTTPRequest_event event)
{
	unsigned int i;
	for(i = 0; i < http->event_funcs->count; i++)
		((http_event_f*)http->event_funcs->data[i]->ptr)(http, event);
}

static struct HTTPRequest *http_find_sock(struct sock *sock)
{
	dict_iter(node, requests)
	{
		if(((struct HTTPRequest *)node->data)->sock == sock)
			return node->data;
	}

	return NULL;
}

static struct HTTPHost *parse_host(const char *host)
{
	struct HTTPHost *hhost = malloc(sizeof(struct HTTPHost));
	memset(hhost, 0, sizeof(struct HTTPHost));
	char *tmp;

	// Remove http part
	if(!strncasecmp(host, "https://", 8))
	{
		host += 8;
		hhost->port = 443;
		hhost->ssl = 1;
	}
	else if(!strncasecmp(host, "http://", 7))
	{
		host += 7;
		hhost->port = 80;
	}
	else
	{
		// assume no protocol
		hhost->port = 80;
	}

	// Is there a slash introducing a possible path?
	if((tmp = strstr(host, "/")))
	{
		hhost->host = strndup(host, tmp - host);
		hhost->path = strdup(tmp + 1);
	}
	else
	{
		hhost->host = strdup(host);
		hhost->path = NULL;
	}

	// Is there a colon instroducing a non-standard port number?
	if((tmp = strchr(hhost->host, ':')) && isdigit(*(tmp + 1)))
	{
		*tmp = '\0';
		hhost->port = atoi(tmp + 1);
	}

	return hhost;
}

static void HTTPRequest_set_host(struct HTTPRequest *http, const char *new_host)
{
	if(http->host)
	{
		free(http->host->host);
		free(http->host->path);
		free(http->host);
	}

	http->host = parse_host(new_host);
	HTTPRequest_add_header(http, "Host", http->host->host);
}

