#ifndef HTTP_H
#define HTTP_H

// #define HTTP_THREADS

#include "sock.h"
#include "stringbuffer.h"

#ifdef HTTP_THREADS
#include <pthread.h>
#endif

#define REQUEST_MAX_SIZE 8192

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"

struct http_client;
struct header_list;

typedef void (http_handler_f)(struct http_client *client, char *uri, int argc, char **argv);
typedef void (http_dead_f)(struct http_client *client);
typedef void* (http_thread_f)(struct http_client *client);

struct http_handler
{
	char *uri;
	http_handler_f *func;
};

enum http_method
{
	HTTP_NONE,
	HTTP_GET,
	HTTP_HEAD,
	HTTP_POST
};

struct http_client
{
	struct sock *sock;
	struct stringbuffer *rbuf; // read buf
	struct stringbuffer *hbuf; // response header buf
	struct stringbuffer *wbuf; // response content buf
	struct header_list *headers; // request headers

	char *uri;
	char *query_string;
	enum http_method method;
	unsigned int ppos; // last parsed position
	unsigned int content_start;
	unsigned int content_length;
	const char *content;
	int state;
	unsigned char version_minor;
	time_t if_modified_since;

	http_handler_f *handler;
	http_dead_f *dead_callback;

	unsigned char delay;

	void *custom; // for custom data. never touched by http module
	void *custom2; // for custom data. never touched by http module

#ifdef HTTP_THREADS
	pthread_t thread;
	pthread_mutex_t thread_mutex;
	pthread_cond_t thread_cond;
	pthread_mutex_t thread_cond_mutex;
#endif
};

#define HTTP_CONNECTION_CLOSE	0x01
#define HTTP_CONNECTION_SERVED	0x02
#define HTTP_HEADERS_SENT	0x04
#define HTTP_HEADERS_DONE	0x08

void http_init();
void http_fini();

unsigned long http_num_served_requests();

void http_handler_add(const char *uri, http_handler_f *func);
void http_handler_del(const char *uri);
void http_handler_add_list(const struct http_handler *handlers);
void http_handler_del_list(const struct http_handler *handlers);
void http_write_header_redirect(struct http_client *client, const char *fmt, ...) PRINTF_LIKE(2, 3);
void http_write_header_status(struct http_client *client, int code);
void http_write_header(struct http_client *client, const char *name, const char *fmt, ...) PRINTF_LIKE(3, 4);
void http_write(struct http_client *client, const char *fmt, ...) PRINTF_LIKE(2, 3);
void http_send_error(struct http_client *client, int code);
const char *http_header_get(struct http_client *client, const char *key);
struct dict *http_parse_vars(struct http_client *client, enum http_method type);
struct dict *http_parse_cookies(struct http_client *client);
void http_request_finalize(struct http_client *client);
void http_request_detach(struct http_client *client, http_thread_f *func);
void http_request_finish_int(struct http_client *client);

#define HTTP_HANDLER(X) static void X(struct http_client *client, char *uri, int argc, char **argv)
#define http_reply_redir(FMT, ...)		http_write_header_redirect(client, FMT, ##__VA_ARGS__)
#define http_reply_header(NAME, FMT, ...)	http_write_header(client, NAME, FMT, ##__VA_ARGS__)
#define http_reply(FMT, ...)			http_write(client, FMT, ##__VA_ARGS__)

#ifdef HTTP_THREADS
#define http_request_finish(CLIENT)	do { 	\
					http_request_finish_int((CLIENT)); \
					pthread_exit(NULL); \
					} while(0)
#else
#define http_request_finish(CLIENT)	http_request_finish_int((CLIENT))
#endif

#endif
