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
	unsigned int port;
	char *path;
	unsigned char ssl;
};

struct HTTPRequest
{
	char *id;

	struct HTTPHost *host;
	struct sock *sock;
	int status;

	struct dict *request_headers;
	struct dict *response_headers;
	struct stringbuffer *buf;
	unsigned char in_headers;

	struct ptrlist *read_funcs;
	struct ptrlist *event_funcs;

	/*
	 * Options that can be set by the caller
	 */

	// Follow redirecting headers
	unsigned char forward_request;
	// Follow redirecting headers only on local site
	unsigned char forward_request_foreign;
	// Pass the HTTP response line by line to the read function
	unsigned char read_linewise;
	// request method/body
	const char *method; // GET, POST, ...
	const char *payload_type; // content type (application/json, application/x-www-form-urlencoded)
	char *payload; // POST body; free'd after request has been sent
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
void HTTPRequest_cancel(struct HTTPRequest *);


void HTTPRequest_add_header(struct HTTPRequest *, const char *name, const char *content);
void HTTPRequest_del_header(struct HTTPRequest *, const char *name);

void HTTPRequest_connect(struct HTTPRequest *);
void HTTPRequest_disconnect(struct HTTPRequest *);
#endif // __HTTP_H__

