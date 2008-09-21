#ifndef SOCK_H
#define SOCK_H

#include "list.h"

#define SOCK_IPV4	0x001
#define SOCK_IPV6	0x002
#define SOCK_UNIX	0x004
#define SOCK_SSL	0x008
#define SOCK_LISTEN	0x010
#define SOCK_CONNECT	0x020
#define SOCK_ZOMBIE	0x040
#define SOCK_NOSOCK	0x080 // Not a socket but something else with a fd (file, pipe, etc.)
#define SOCK_QUIET	0x100 // Do not show socket debug messages

#define sock_debug(sock, text...) { if(!(sock->flags & SOCK_QUIET)) log_append(LOG_DEBUG, ## text); }

#define REMOTE_IP(SOCK)	(((struct sockaddr_in *)(SOCK)->sockaddr_remote)->sin_addr)

DECLARE_LIST(sock_list, struct sock *)

enum sock_event
{
	// EV_READ is only used if there is no read_func
	// EV_WRITE is used AFTER something was written
	EV_READ = 1,
	EV_WRITE,
	EV_ERROR,
	EV_CONNECT,
	EV_ACCEPT,
	EV_HANGUP
};

typedef void (sock_event_f)(struct sock *sock, enum sock_event event, int err);
typedef void (sock_read_f)(struct sock *sock, char *buf, size_t len);

struct sock
{
	int		fd;

#ifdef HAVE_SSL
	SSL		*ssl_handle;
#endif

	struct sockaddr	*sockaddr_local;
	struct sockaddr	*sockaddr_remote;

	unsigned short	flags;

	sock_event_f	*event_func;
	sock_read_f	*read_func;

	char		*read_buf;
	char		*read_buf_delimiter;
	size_t		read_buf_len;
	size_t		read_buf_used;

	char		*send_queue;
	size_t		send_queue_len;
};

void sock_init();
void sock_fini();
struct sock* sock_create(unsigned short type, sock_event_f *event_func, sock_read_f *read_func);
int sock_bind(struct sock *sock, const char *addr, unsigned int port);
int sock_connect(struct sock *sock, const char *addr, unsigned int port);
int sock_listen(struct sock *sock, const char *ssl_pem);
void sock_set_fd(struct sock *sock, int fd);
int sock_close(struct sock *sock);
void sock_close_timed(struct sock *sock, unsigned int delay);
struct sock *sock_accept(struct sock *sock, sock_event_f *event_func, sock_read_f *read_func);
int sock_write(struct sock *sock, char *buf, size_t len);
int sock_write_fmt(struct sock *sock, const char *format, ...) PRINTF_LIKE(2, 3);
int sock_poll();
void sock_set_readbuf(struct sock *sock, size_t len, const char *buf_delimiter);

#endif
