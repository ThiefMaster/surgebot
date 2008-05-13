#include "global.h"
#include <stdlib.h>
#include "module.h"
#include "chanuser.h"
#include "irc.h"
#include "tools.h"
#include "http.h" // Prototypes
#include "sock.h"
#include "log.h"

/*
 * Todo:
 *
 * - Implement timer of maybe 20 seconds for timeout (or provide it as argument?)
 */

MODULE_DEPENDS(NULL);

static struct dict *requests;

static void http_sock_event(struct sock *, enum sock_event, int err);
static void http_sock_read(struct sock *, char *buf, size_t len);

static void HTTPRequest_event(struct HTTPRequest *, enum HTTPRequest_event);

static struct HTTPRequest *http_find_sock(struct sock *);

static struct HTTPHost *parse_host(const char *);

static unsigned int next_id = 0;

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
	char tmp[MAXLEN];
	
	debug("Creating new HTTP request to host \"%s\" width ID %d", host, next_id);
	sprintf(tmp, "%u", next_id++);

	http = malloc(sizeof(struct HTTPRequest));
	memset(http, 0, sizeof(struct HTTPRequest));
	
	// Create structs/Assign main struct
	http->request_headers = dict_create();
	http->response_headers = dict_create();
	
	http->read_funcs = ptrlist_create();
	http->event_funcs = ptrlist_create();
	
	http->buf = stringbuffer_create();
	
	http->port = 80;
	http->host = parse_host(host);
	
	http->in_headers = 1;
	http->id = strdup(tmp);
	
	// Set appropriate struct settings
	dict_set_free_funcs(http->request_headers, free, free);
	dict_set_free_funcs(http->response_headers, free, free);
	
	// Fill struct with given values
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
	debug("Freeing HTTP Request %s", http->id);
	if(http)
	{
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
		free(http);
	}
}

void HTTPRequest_add_header(struct HTTPRequest *http, const char *name, const char *content)
{
	struct dict_node *node = dict_find_node(http->request_headers, name);
	
	// If the node already exists, only replace its content
	if(node)
	{
		if(node->data)
			free(node->data);
		
		node->data = strdup(name);
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
	
	http->sock = sock_create(SOCK_IPV4, http_sock_event, http_sock_read);
	debug("Connecting HTTP Request %s", http->id);
	sock_connect(http->sock, http->host->host, http->port);
}

static void http_sock_event(struct sock *sock, enum sock_event event, int err)
{
	struct HTTPRequest *http = http_find_sock(sock);
	assert(http);
	
	switch(event)
	{
		case EV_HANGUP:
		{
			// Socket was closed by remote side = reached end of http content
			// -> Dump read buffer to socket_read_functions
			unsigned int i;
			debug("Read %u chars for HTTP Request %s, passing to read funcs", i, http->id);
			for(i = 0; i < http->read_funcs->count; i++)
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
			sock_write_fmt(sock, "GET /%s HTTP/1.0\n", (http->host->path ? http->host->path : ""));
			dict_iter(node, http->request_headers)
			{
				sock_write_fmt(sock, "%s: %s\n", node->key, (char*)node->data);
			}
			// Empty line to signal end of headers
			sock_write(sock, "\n", 1);
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
	
	// If we aren't currently retrieving headers, there's nothing more we need to do
	if(!http->in_headers)
		return;
	
	while((str = stringbuffer_shift(http->buf, "\n", 1)))
	{
		len = strlen(str);
		// Do we have \r\n aka windows-type line endings?
		if(len && str[len - 1] == '\r')
			str[--len] = '\0';
		
		// Empty line... as in end of headers?
		if(!len)
		{
			http->in_headers = 0;
			free(str);
			break;
		}
		
		// So apparently, this is a header, let's find a colon
		if(!(tmp = strstr(str, ":")))
		{
			// No colon, this must be the first line of headers in the form "HTTP/1.0 302 Found"
			char *tmp2;
			if(!(tmp = strstr(str, " ")) || !(tmp2 = strstr(str + (tmp - str) + 1, " ")))
			{
				log_append(LOG_ERROR, "(HTTP Request %s) Invalid HTTP header format: %s", http->id, str);
				// Send timeout event and free request
				HTTPRequest_event(http, H_EV_TIMEOUT);
				dict_delete(requests, http->id);
				
				free(str);
				return;
			}
		}
		else
		{
			// Not the first line of headers, in the format "Name: Content"
			str[tmp - str] = '\0';
			HTTPRequest_add_header(http, str, str + (tmp - str) + 1);
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
	char *tmp;
	
	// Is there are slash introducing a possible path?
	if((tmp = strstr(host, "/")))
	{
		hhost->host = strndup(host, tmp - host);
		hhost->path = strdup(host + (tmp - host));
	}
	else
	{
		hhost->host = strdup(host);
		hhost->path = NULL;
	}
	
	return hhost;
}
