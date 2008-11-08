#ifndef __HTTP_H__
#define __HTTP_H__

#include "sock.h"
#include "dict.h"
#include "ptrlist.h"
#include "stringbuffer.h"

#define HTTPRequest_get_response_header(HTTP, NAME) (dict_find((HTTP)->response_headers, NAME))
#define HTTPRequest_get_request_header(HTTP, NAME) (dict_find((HTTP)->request_headers, NAME))

struct HTTPRequest;
enum HTTPRequest_event;

typedef void (http_read_f)(struct HTTPRequest *, const char *buf, unsigned int len);
typedef void (http_event_f)(struct HTTPRequest *, enum HTTPRequest_event);

struct HTTPHost
{
	char *host;
	char *path;
};

struct HTTPRequest
{
	char *id;
	
	struct HTTPHost *host;
	unsigned int port;
	struct sock *sock;
	
	struct dict *request_headers;
	struct dict *response_headers;
	struct stringbuffer *buf;
	unsigned char in_headers;
	
	struct ptrlist *read_funcs;
	struct ptrlist *event_funcs;
};

enum HTTPRequest_event
{
	// Prefix with H to avoid enum constant name conflicts
	H_EV_HANGUP,
	H_EV_TIMEOUT
};

// The host is meant to contain the port or assume 80 by default
struct HTTPRequest *HTTPRequest_create(const char *host, http_event_f *, http_read_f *);
void HTTPRequest_free(struct HTTPRequest *);

void HTTPRequest_add_header(struct HTTPRequest *, const char *name, const char *content);
void HTTPRequest_del_header(struct HTTPRequest *, const char *name);

void HTTPRequest_connect(struct HTTPRequest *);
void HTTPRequest_disconnect(struct HTTPRequest *);
#endif // __HTTP_H__