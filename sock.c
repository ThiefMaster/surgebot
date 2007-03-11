#include "global.h"
#include "sock.h"

IMPLEMENT_LIST(sock_list, struct sock *)

static struct sock_list *sock_list;
static struct pollfd *pollfds = NULL;

static void sock_destroy(struct sock *sock);
#ifdef HAVE_SSL
static int sock_enable_ssl(struct sock *sock, SSL_CTX *ctx);
#endif


void sock_init()
{
	sock_list = sock_list_create();

#ifdef HAVE_SSL
	SSL_library_init();
	SSL_load_error_strings();
#endif
}

void sock_fini()
{
	int i;
	for(i = 0; i < sock_list->count; i++)
	{
		sock_destroy(sock_list->data[i]);
		i--;
	}
	sock_list_free(sock_list);
	free(pollfds);

#ifdef HAVE_SSL
	ERR_remove_state(0);
	EVP_cleanup();
	ERR_free_strings();
	CRYPTO_cleanup_all_ex_data();
#endif
}

struct sock* sock_create(unsigned char type, sock_event_f *event_func, sock_read_f *read_func)
{
	struct sock *sock;
	int fd, domain_type, proto_type, flags, param;

	type &= ~(SOCK_LISTEN | SOCK_CONNECT | SOCK_ZOMBIE);

	switch(type & (SOCK_IPV4|SOCK_IPV6|SOCK_UNIX|SOCK_NOSOCK))
	{
		case SOCK_IPV4:
			domain_type = AF_INET;
			proto_type = IPPROTO_TCP;
			break;

		case SOCK_IPV6:
			domain_type = AF_INET6;
			proto_type = IPPROTO_TCP;
			break;

		case SOCK_UNIX:
			domain_type = AF_UNIX;
			proto_type = 0;
			break;

		case SOCK_NOSOCK:
			// Just create a "blank" socket struct (without a socket) and return it
			sock = malloc(sizeof(struct sock));
			memset(sock, 0, sizeof(struct sock));
			sock->flags = type;
			sock->event_func = event_func;
			sock->read_func = read_func;
			sock->fd = -1;
			return sock;
			break;

		default:
			log_append(LOG_ERROR, "Invalid socket type %d in sock_create(); use SOCK_IPV4, SOCK_IPV6, SOCK_UNIX or SOCK_NOSOCK", type);
			return NULL;
	}

#ifndef HAVE_SSL
	if(type & SOCK_SSL)
	{
		log_append(LOG_ERROR, "Could not create SSL socket: SSL not supported");
		return NULL;
	}
#endif

	if((fd = socket(domain_type, SOCK_STREAM, proto_type)) < 0)
	{
		log_append(LOG_ERROR, "Could not create socket with domain type %d: %s (%d)", domain_type, strerror(errno), errno);
		return NULL;
	}

	param = 1;
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&param, sizeof(int)))
	{
		log_append(LOG_ERROR, "setsockopt() for SO_REUSEADDR failed: %s (%d)", strerror(errno), errno);
		close(fd);
		return NULL;
	}

	if((flags = fcntl(fd, F_GETFL)) == -1)
	{
		log_append(LOG_ERROR, "fcntl(%d, F_GETFL) failed: %s (%d)", fd, strerror(errno), errno);
		close(fd);
		return NULL;
	}

	flags |= O_NONBLOCK;
	if(fcntl(fd, F_SETFL, flags))
	{
		log_append(LOG_ERROR, "fcntl(%d, F_SETFL) failed: %s (%d)", fd, strerror(errno), errno);
		close(fd);
		return NULL;
	}


	sock = malloc(sizeof(struct sock));
	memset(sock, 0, sizeof(struct sock));

	sock->flags = type;
	sock->event_func = event_func;
	sock->read_func = read_func;
	sock->fd = fd;

#ifdef HAVE_SSL
	if((type & SOCK_SSL) && (sock_enable_ssl(sock, NULL) != 0))
	{
		close(sock->fd);
		free(sock);
		return NULL;
	}
#endif

	debug("Created new socket %p with fd=%d", sock, sock->fd);
	return sock;
}

int sock_bind(struct sock *sock, const char *addr, unsigned int port)
{
	struct hostent *hp;

	if(sock->flags & SOCK_IPV4)
	{
		struct sockaddr_in *sin;

		if(!(hp = gethostbyname2(addr, AF_INET)))
		{
			log_append(LOG_WARNING, "Could not resolve %s (IPv4)", addr);
			return -1;
		}

		sin = malloc(sizeof(struct sockaddr_in));
		memset(sin, 0, sizeof(struct sockaddr_in));

		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		memcpy(&sin->sin_addr, hp->h_addr, sizeof(struct in_addr));

		if(bind(sock->fd, (struct sockaddr*)sin, sizeof(struct sockaddr_in)) < 0)
		{
			log_append(LOG_WARNING, "Could not bind to %s/%u (IPv4): %s (%d)", addr, port, strerror(errno), errno);
			free(sin);
			return -2;
		}

		sock->sockaddr_local = (struct sockaddr*)sin;
		return 0;
	}
	else if(sock->flags & SOCK_IPV6)
	{
		struct sockaddr_in6 *sin;

		if(!(hp = gethostbyname2(addr, AF_INET6)))
		{
			log_append(LOG_WARNING, "Could not resolve %s (IPv6)", addr);
			return -1;
		}

		sin = malloc(sizeof(struct sockaddr_in6));
		memset(sin, 0, sizeof(struct sockaddr_in6));

		sin->sin6_family = AF_INET6;
		sin->sin6_port = htons(port);
		memcpy(&sin->sin6_addr, hp->h_addr, sizeof(struct in6_addr));

		if(bind(sock->fd, (struct sockaddr*) sin, sizeof(struct sockaddr_in6)) < 0)
		{
			log_append(LOG_WARNING, "Could not bind to %s/%u (IPv6): %s (%d)", addr, port, strerror(errno), errno);
			free(sin);
			return -2;
		}

		sock->sockaddr_local = (struct sockaddr*) sin;
		return 0;
	}
	else if(sock->flags & SOCK_UNIX)
	{
		struct sockaddr_un *sun;

		sun = malloc(sizeof(struct sockaddr_un));
		memset(sun, 0, sizeof(struct sockaddr_un));

		sun->sun_family = AF_UNIX;
		strncpy(sun->sun_path, addr, sizeof(sun->sun_path));

		if(bind(sock->fd, (struct sockaddr*)sun, sizeof(struct sockaddr_un)) < 0)
		{
			log_append(LOG_WARNING, "Could not bind to %s (Unix): %s (%d)", addr, strerror(errno), errno);
			free(sun);
			return -2;
		}

		sock->sockaddr_local = (struct sockaddr*)sun;
		return 0;
	}

	return -3;
}

int sock_connect(struct sock *sock, const char *addr, unsigned int port)
{
	struct hostent *hp;

	if(sock->flags & SOCK_CONNECT)
		return 0;

	if(sock->flags & SOCK_IPV4)
	{
		struct sockaddr_in *sin;

		if((hp = gethostbyname2(addr, AF_INET)) == NULL)
		{
			log_append(LOG_WARNING, "Could not resolve %s/%u (IPv4)", addr, port);
			free(sock);
			return -1;
		}

		sin = malloc(sizeof(struct sockaddr_in));
		memset(sin, 0, sizeof(struct sockaddr_in));

		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		memcpy(&sin->sin_addr, hp->h_addr, sizeof(struct in_addr));

		if(connect(sock->fd, (struct sockaddr*)sin, sizeof(struct sockaddr_in)) < 0 && errno != EINPROGRESS)
		{
			log_append(LOG_WARNING, "Could not connect to %s/%u (IPv4): %s (%d)", addr, port, strerror(errno), errno);
			free(sin);
			free(sock);
			return -2;
		}

		sock_list_add(sock_list, sock);

		sock->sockaddr_remote = (struct sockaddr*)sin;
		sock->flags |= SOCK_CONNECT;
		return 0;
	}
	else if(sock->flags & SOCK_IPV6)
	{
		struct sockaddr_in6 *sin;

		if((hp = gethostbyname2(addr, AF_INET6)) == NULL)
		{
			log_append(LOG_WARNING, "Could not resolve %s/%u (IPv6)", addr, port);
			free(sock);
			return -1;
		}

		sin = malloc(sizeof(struct sockaddr_in6));
		memset(sin, 0, sizeof(struct sockaddr_in6));

		sin->sin6_family = AF_INET6;
		sin->sin6_port = htons(port);
		memcpy(&sin->sin6_addr, hp->h_addr, sizeof(struct in6_addr));

		if(connect(sock->fd, (struct sockaddr*)sin, sizeof(struct sockaddr_in6)) < 0 && errno != EINPROGRESS)
		{
			log_append(LOG_WARNING, "Could not connect to %s/%u (IPv6): %s (%d)", addr, port, strerror(errno), errno);
			free(sin);
			free(sock);
			return -2;
		}

		sock_list_add(sock_list, sock);

		sock->sockaddr_remote = (struct sockaddr*)sin;
		sock->flags |= SOCK_CONNECT;
		return 0;
	}
	else if(sock->flags & SOCK_UNIX)
	{
		struct sockaddr_un *sun;

		sun = malloc(sizeof(struct sockaddr_un));
		memset(sun, 0, sizeof(struct sockaddr_un));

		sun->sun_family = AF_UNIX;
		strncpy(sun->sun_path, addr, sizeof(sun->sun_path));

		if(connect(sock->fd, (struct sockaddr*)sun, sizeof(struct sockaddr_un)) < 0 && errno != EINPROGRESS)
		{
			log_append(LOG_WARNING, "Could not connect to %s (Unix): %s (%d)", addr, strerror(errno), errno);
			free(sun);
			free(sock);
			return -2;
		}

		sock_list_add(sock_list, sock);

		sock->sockaddr_remote = (struct sockaddr*)sun;
		sock->flags |= SOCK_CONNECT;
		return 0;
	}

	return -3;
}

int sock_listen(struct sock *sock, const char *ssl_pem)
{
	if(sock->flags & SOCK_LISTEN)
		return 0;

	if(sock->sockaddr_local == NULL)
		return -1;

#ifdef HAVE_SSL
	if((sock->flags & SOCK_SSL) && ssl_pem)
	{
		assert_return(sock->ssl_handle, -2);

		SSL_CTX *ctx = SSL_get_SSL_CTX(sock->ssl_handle);
		if(SSL_CTX_use_certificate_file(ctx, ssl_pem, SSL_FILETYPE_PEM) != 1)
		{
			int err = ERR_get_error();
			log_append(LOG_ERROR, "Could not set SSL certificate file %s: %s (%d)", ssl_pem, ERR_error_string(err, NULL), err);
			sock_destroy(sock);
			return -2;
		}

		if(SSL_CTX_use_RSAPrivateKey_file(ctx, ssl_pem, SSL_FILETYPE_PEM) != 1)
		{
			int err = ERR_get_error();
			log_append(LOG_ERROR, "Could not set SSL RSA private key file %s: %s (%d)", ssl_pem, ERR_error_string(err, NULL), err);
			sock_destroy(sock);
			return -3;
		}
	}
#endif

	if(listen(sock->fd, 0) < 0)
	{
		log_append(LOG_ERROR, "sock_listen(%d) failed: %s (%d)", sock->fd, strerror(errno), errno);
		sock_destroy(sock);
		return -4;
	}

	sock->flags |= SOCK_LISTEN;
	sock_list_add(sock_list, sock);

	return 0;
}

void sock_set_fd(struct sock *sock, int fd)
{
	if(!(sock->flags & SOCK_NOSOCK) || sock->fd != -1)
		return;

	sock->fd = fd;
	sock_list_add(sock_list, sock);
	debug("fd for sock %p set to %d", sock, fd);
}

struct sock *sock_accept(struct sock *sock, sock_event_f *event_func, sock_read_f *read_func)
{
	if(!(sock->flags & SOCK_LISTEN))
		return NULL;

	if(sock->flags & SOCK_IPV4)
	{
		struct sockaddr_in *sin;
		socklen_t sin_len;
		struct sock *new_sock;
		int fd;

		sin = malloc(sizeof(struct sockaddr_in));
		sin_len = sizeof(struct sockaddr_in);

		fd = accept(sock->fd, (struct sockaddr*)sin, &sin_len);
		if(fd < 0)
		{
			free(sin);
			log_append(LOG_ERROR, "Could not accept(%d): %s (%d)", fd, strerror(errno), errno);
			return NULL;
		}

		new_sock = malloc(sizeof(struct sock));
		memset(new_sock, 0, sizeof(struct sock));
		new_sock->sockaddr_local = malloc(sizeof(struct sockaddr_in));
		memcpy(new_sock->sockaddr_local, sock->sockaddr_local, sizeof(struct sockaddr_in));
		new_sock->sockaddr_remote = (struct sockaddr*)sin;
		new_sock->event_func = event_func;
		new_sock->read_func = read_func;
		new_sock->flags = SOCK_IPV4;
		new_sock->fd = fd;

		sock_list_add(sock_list, new_sock);

#ifdef HAVE_SSL
		if(sock->flags & SOCK_SSL)
		{
			new_sock->flags |= SOCK_SSL;
			sock_enable_ssl(new_sock, SSL_get_SSL_CTX(sock->ssl_handle));
			SSL_set_accept_state(new_sock->ssl_handle);
			int res = SSL_accept(new_sock->ssl_handle);
			int err = SSL_get_error(new_sock->ssl_handle, res);
			if(err == SSL_ERROR_SSL)
			{
				err = ERR_get_error();
				log_append(LOG_ERROR, "SSL error in SSL_accept(): %s (%d)", ERR_error_string(err, NULL), err);
				sock_destroy(new_sock);
				return NULL;
			}
		}
#endif

		return new_sock;
	}
	else if(sock->flags & SOCK_IPV6)
	{
		struct sockaddr_in6 *sin;
		socklen_t sin_len;
		struct sock *new_sock;
		int fd;

		sin = malloc(sizeof(struct sockaddr_in6));
		sin_len = sizeof(struct sockaddr_in6);

		fd = accept(sock->fd, (struct sockaddr*)sin, &sin_len);
		if(fd < 0)
		{
			free(sin);
			log_append(LOG_ERROR, "Could not accept(%d): %s (%d)", fd, strerror(errno), errno);
			return NULL;
		}

		new_sock = malloc(sizeof(struct sock));
		memset(new_sock, 0, sizeof(struct sock));
		new_sock->sockaddr_local = malloc(sizeof(struct sockaddr_in6));
		memcpy(new_sock->sockaddr_local, sock->sockaddr_local, sizeof(struct sockaddr_in6));
		new_sock->sockaddr_remote = (struct sockaddr*)sin;
		new_sock->event_func = event_func;
		new_sock->read_func = read_func;
		new_sock->flags = SOCK_IPV6;
		new_sock->fd = fd;

		sock_list_add(sock_list, new_sock);

#ifdef HAVE_SSL
		if(sock->flags & SOCK_SSL)
		{
			new_sock->flags |= SOCK_SSL;
			sock_enable_ssl(new_sock, SSL_get_SSL_CTX(sock->ssl_handle));
			SSL_set_accept_state(new_sock->ssl_handle);
			int res = SSL_accept(new_sock->ssl_handle);
			int err = SSL_get_error(new_sock->ssl_handle, res);
			if(err == SSL_ERROR_SSL)
			{
				err = ERR_get_error();
				log_append(LOG_ERROR, "SSL error in SSL_accept(): %s (%d)", ERR_error_string(err, NULL), err);
			}
		}
#endif

		return new_sock;
	}
	else if(sock->flags & SOCK_UNIX)
	{
		struct sockaddr_un *sun;
		socklen_t sun_len;
		struct sock *new_sock;
		int fd;

		sun = malloc(sizeof(struct sockaddr_un));
		sun_len = sizeof(struct sockaddr_un);

		fd = accept(sock->fd, (struct sockaddr*)sun, &sun_len);
		if(fd < 0)
		{
			free(sun);
			log_append(LOG_ERROR, "Could not accept(%d): %s (%d)", fd, strerror(errno), errno);
			return NULL;
		}

		new_sock = malloc(sizeof(struct sock));
		memset(new_sock, 0, sizeof(struct sock));
		new_sock->sockaddr_local = malloc(sizeof(struct sockaddr_un));
		memcpy(new_sock->sockaddr_local, sock->sockaddr_local, sizeof(struct sockaddr_un));
		new_sock->sockaddr_remote = (struct sockaddr*)sun;
		new_sock->event_func = event_func;
		new_sock->read_func = read_func;
		new_sock->flags = SOCK_UNIX;
		new_sock->fd = fd;

		sock_list_add(sock_list, new_sock);

#ifdef HAVE_SSL
		if(sock->flags & SOCK_SSL)
		{
			new_sock->flags |= SOCK_SSL;
			sock_enable_ssl(new_sock, SSL_get_SSL_CTX(sock->ssl_handle));
			SSL_set_accept_state(new_sock->ssl_handle);
			int res = SSL_accept(new_sock->ssl_handle);
			int err = SSL_get_error(new_sock->ssl_handle, res);
			if(err == SSL_ERROR_SSL)
			{
				err = ERR_get_error();
				log_append(LOG_ERROR, "SSL error in SSL_accept(): %s (%d)", ERR_error_string(err, NULL), err);
			}
		}
#endif

		return new_sock;
	}

	return NULL;
}

int sock_write(struct sock *sock, char *buf, size_t len)
{
	if(sock->send_queue == NULL)
	{
		//debug("New sock send_queue (%u bytes)", len);
		sock->send_queue = malloc(sizeof(char) * len);
		memcpy(sock->send_queue, buf, len);
		sock->send_queue_len = len;
	}
	else
	{
		char *new_queue;

		//debug("Extending old send_queue (%u bytes + %u bytes)", sock->send_queue_len, len);

		new_queue = malloc(sizeof(char) * len + sock->send_queue_len);
		memcpy(new_queue, sock->send_queue, sock->send_queue_len);
		memcpy(new_queue + sock->send_queue_len, buf, len);

		free(sock->send_queue);
		sock->send_queue = new_queue;
		sock->send_queue_len += len;
	}
	return 0;
}

int sock_write_fmt(struct sock *sock, const char *format, ...)
{
	va_list args;
	char buf[NET_BUF_SIZE];
	int len;

	va_start(args, format);
	len = vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	return sock_write(sock, buf, len);
}

int sock_close(struct sock *sock)
{
	if(sock->flags & SOCK_ZOMBIE)
		return -1;

	if(sock->fd > 0)
		close(sock->fd);

	sock->flags |= SOCK_ZOMBIE;
	sock->fd = -1;
	return 0;
}

static void sock_destroy(struct sock *sock)
{
	debug("Destroying socket %p", sock);
	sock_close(sock);

#ifdef HAVE_SSL
	if((sock->flags & SOCK_SSL) && sock->ssl_handle)
	{
		SSL_CTX *ctx = SSL_get_SSL_CTX(sock->ssl_handle);
		SSL_shutdown(sock->ssl_handle);
		SSL_free(sock->ssl_handle);

		if(ctx->references == 1)
			SSL_CTX_free(ctx);
	}
#endif

	if(sock->send_queue)
		free(sock->send_queue);
	if(sock->read_buf)
		free(sock->read_buf);
	if(sock->read_buf_delimiter)
		free(sock->read_buf_delimiter);
	if(sock->sockaddr_local)
		free(sock->sockaddr_local);
	if(sock->sockaddr_remote)
		free(sock->sockaddr_remote);

	sock_list_del(sock_list, sock);
	free(sock);
}

#ifdef HAVE_SSL
static int sock_enable_ssl(struct sock *sock, SSL_CTX *ctx)
{
	unsigned short new_ctx = 0;

	if(!(sock->flags & SOCK_SSL))
	{
		log_append(LOG_ERROR, "sock_enable_ssl() called for non-ssl socket %p (fd=%d)", sock, sock->fd);
		return -1;
	}

	if(!ctx)
	{
		new_ctx = 1;

		if((ctx = SSL_CTX_new(SSLv23_method())) == NULL)
		{
			log_append(LOG_ERROR, "Could not create SSL context");
			return -2;
		}

		debug("Created new SSL context %p", ctx);
	}
	else
	{
		debug("Re-using SSL context %p", ctx);
	}

	if(new_ctx)
	{
		SSL_CTX_set_options(ctx, SSL_OP_ALL);
	}

	if((sock->ssl_handle = SSL_new(ctx)) == NULL)
	{
		log_append(LOG_ERROR, "Could not create SSL handle");
		if(new_ctx)
			SSL_CTX_free(ctx);
		return -3;
	}

	SSL_set_fd(sock->ssl_handle, sock->fd);
	return 0;
}
#endif

int sock_poll()
{
	static int last_numsocks = 0;
	int i, res;

	for(i = 0; i < sock_list->count; i++)
	{
		struct sock *sock = sock_list->data[i];
		if(sock->flags & SOCK_ZOMBIE)
		{
			debug("Deleting zombie socket %p", sock);
			sock_destroy(sock);
			i--; // sock_list entry will be replaced with last element so we need to check it again
		}
	}

	if(last_numsocks != sock_list->count)
	{
		if(sock_list->count)
		{
			pollfds = realloc(pollfds, sizeof(struct pollfd) * (sock_list->count + 1));
		}
		else
		{
			free(pollfds);
			pollfds = NULL;
		}

		last_numsocks = sock_list->count;
	}

	if(sock_list->count == 0)
	{
		usleep(1000000);
		return 0;
	}

	for(i = 0; i < sock_list->count; i++)
	{
		struct sock *sock = sock_list->data[i];

		pollfds[i].fd = sock->fd;
		pollfds[i].events = POLLIN;
		pollfds[i].revents = 0;


		if(sock->flags & SOCK_CONNECT)
		{
			pollfds[i].events |= POLLOUT;
		}
		else if(!(sock->flags & SOCK_LISTEN) && sock->send_queue_len > 0)
		{
			pollfds[i].events |= POLLOUT;
		}
	}

	res = poll(pollfds, last_numsocks, 1000);
	if(res == -1)
	{
		if(errno != EINTR && errno != EAGAIN)
		{
			free(pollfds);
			pollfds = NULL;
			last_numsocks = 0;
			log_append(LOG_ERROR, "poll() failed: %s (%d)", strerror(errno), errno);
			return -1;
		}
	}

	for(i = 0; i < last_numsocks; i++)
	{
		struct sock *sock;
		unsigned char ev_read, ev_write;
		int error, len;

		sock = sock_list->data[i];

		if(sock->flags & SOCK_ZOMBIE)
			continue;

		assert_return(sock->fd == pollfds[i].fd, -1);

		ev_read  = ((pollfds[i].revents & POLLIN) ? 1 : 0);
		ev_write = ((pollfds[i].revents & POLLOUT) ? 1 : 0);

		len = sizeof(int);
		if(!(sock->flags & SOCK_NOSOCK) && (getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &len) != 0 || error))
		{
			sock->event_func(sock, EV_ERROR, error);
			sock_close(sock);
		}
		else if(sock->flags & SOCK_CONNECT)
		{
			if(ev_write)
			{
#ifdef HAVE_SSL
				if(sock->flags & SOCK_SSL)
				{
					SSL_set_connect_state(sock->ssl_handle);
					int res = SSL_connect(sock->ssl_handle);
					int err = SSL_get_error(sock->ssl_handle, res);
					if(err == SSL_ERROR_SSL)
					{
						err = ERR_get_error();
						log_append(LOG_ERROR, "SSL error in SSL_connect(): %s (%d)", ERR_error_string(err, NULL), err);
						sock_close(sock);
						continue;
					}
				}
#endif

				sock->event_func(sock, EV_CONNECT, 0);
				sock->flags &= ~SOCK_CONNECT;
			}
			else if(ev_read)
			{
				sock->event_func(sock, EV_ERROR, 0);
				sock_close(sock);
			}
		}
		else if(sock->flags & SOCK_LISTEN)
		{
			if(ev_read)
			{
				sock->event_func(sock, EV_ACCEPT, 0);
			}
		}
		else
		{
			if(ev_read)
			{
				if(sock->read_func == NULL)
				{
					sock->event_func(sock, EV_READ, 0);
				}
				else if(sock->read_buf) // line-buffered reading
				{
					int rres = 0;
					size_t skiplen, retlen, getlen;

#ifdef HAVE_SSL
					if(sock->flags & SOCK_SSL)
						rres = SSL_read(sock->ssl_handle, sock->read_buf + sock->read_buf_used, sock->read_buf_len - sock->read_buf_used);
					else
#endif
						rres = read(sock->fd, sock->read_buf + sock->read_buf_used, sock->read_buf_len - sock->read_buf_used);

					if(rres == -1 && !(sock->flags & SOCK_SSL) && errno != EINTR && errno != EAGAIN)
					{
						if(errno != ECONNRESET)
							log_append(LOG_WARNING, "read() on fd=%d failed: %s (%d)", sock->fd, strerror(errno), errno);

						sock->event_func(sock, EV_ERROR, errno);
						sock_close(sock);
					}
#ifdef HAVE_SSL
					else if(rres == -1 && (sock->flags & SOCK_SSL))
					{
						int err = SSL_get_error(sock->ssl_handle, rres);
						if(err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ)
						{
							log_append(LOG_WARNING, "SSL_read() on fd=%d failed: %d", sock->fd, err);
							sock->event_func(sock, EV_ERROR, errno);
							sock_close(sock);
						}
					}
#endif
					else if(rres == 0 && !(sock->flags & SOCK_NOSOCK))
					{
						sock->event_func(sock, EV_HANGUP, 0);
						sock_close(sock);
					}
					else if(rres > 0)
					{
						sock->read_buf_used += rres;
						sock->read_buf[sock->read_buf_used] = '\0';

						if((skiplen = strspn(sock->read_buf, sock->read_buf_delimiter)))
						{
							memmove(sock->read_buf, sock->read_buf + skiplen, sock->read_buf_len - skiplen);
							sock->read_buf_used -= skiplen;
						}

						while((retlen = strcspn(sock->read_buf, sock->read_buf_delimiter)) > 0 && strspn(sock->read_buf + retlen, sock->read_buf_delimiter))
						{
							getlen = retlen + strspn(sock->read_buf + retlen, sock->read_buf_delimiter);
							sock->read_buf[retlen] = '\0';
							sock->read_func(sock, sock->read_buf, retlen);

							memmove(sock->read_buf, sock->read_buf + getlen, sock->read_buf_len - getlen);
							sock->read_buf_used -= getlen;
							sock->read_buf[sock->read_buf_used] = '\0';
						}
					}
				}
				else // unbuffered reading
				{
					ssize_t rres;
					char in_buf[NET_BUF_SIZE];
					memset(in_buf, 0, NET_BUF_SIZE);

#ifdef HAVE_SSL
					if(sock->flags & SOCK_SSL)
						rres = SSL_read(sock->ssl_handle, in_buf, NET_BUF_SIZE);
					else
#endif
						rres = read(sock->fd, in_buf, NET_BUF_SIZE);

					if(rres == -1 && !(sock->flags & SOCK_SSL) && errno != EINTR && errno != EAGAIN)
					{
						if(errno != ECONNRESET)
							log_append(LOG_WARNING, "read() on fd=%d failed: %s (%d)", sock->fd, strerror(errno), errno);

						sock->event_func(sock, EV_ERROR, errno);
						sock_close(sock);
					}
#ifdef HAVE_SSL
					else if(rres == -1 && (sock->flags & SOCK_SSL))
					{
						int err = SSL_get_error(sock->ssl_handle, rres);
						if(err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ)
						{
							log_append(LOG_WARNING, "SSL_read() on fd=%d failed: %d", sock->fd, err);
							sock->event_func(sock, EV_ERROR, errno);
							sock_close(sock);
						}
					}
#endif
					else if(rres == 0)
					{
						sock->event_func(sock, EV_HANGUP, 0);
						sock_close(sock);
					}
					else if(rres > 0)
					{
						in_buf[rres] = '\0';
						sock->read_func(sock, in_buf, rres);
					}
				}
			}

			if(ev_write && sock->send_queue_len && !(sock->flags & SOCK_ZOMBIE))
			{
				ssize_t wres;

				//debug("sock send_queue contains %u bytes, trying to write!", sock->send_queue_len);
#ifdef HAVE_SSL
				if(sock->flags & SOCK_SSL)
					wres = SSL_write(sock->ssl_handle, sock->send_queue, sock->send_queue_len);
				else
#endif
					wres = write(sock->fd, sock->send_queue, sock->send_queue_len);

				if(wres < 0)
				{
#ifdef HAVE_SSL
					if(sock->flags & SOCK_SSL)
					{
						int err = SSL_get_error(sock->ssl_handle, wres);
						if(err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_WANT_READ)
							log_append(LOG_WARNING, "Could not write to ssl socket %d: %d", sock->fd, err);
					}
					else
#endif
						log_append(LOG_WARNING, "Could not write to socket %d: %s (%d)", sock->fd, strerror(errno), errno);
				}
				else if(wres == sock->send_queue_len) // everything was written to the socket
				{
					//debug("Wrote everything");
					free(sock->send_queue);
					sock->send_queue = NULL;
					sock->send_queue_len = 0;
				}
				else if(wres < sock->send_queue_len) // we were not able to write out the whole buffer
				{
					char *new_queue;

					//debug("Wrote %u bytes, %u remaining", wres, sock->send_queue_len - wres);

					new_queue = malloc(sock->send_queue_len - wres);
					memcpy(new_queue, sock->send_queue + wres, sock->send_queue_len - wres);
					free(sock->send_queue);
					sock->send_queue = new_queue;
					sock->send_queue_len -= wres;
				}
			}
		}
	}

	return sock_list->count;
}

void sock_set_readbuf(struct sock *sock, size_t len, const char *buf_delimiter)
{
	if(sock->read_buf)
	{
		log_append(LOG_WARNING, "Socket %d has already a read buffer (%lu bytes)", sock->fd, (unsigned long)sock->read_buf_len);
		return;
	}

	if(sock->read_func == NULL)
	{
		log_append(LOG_WARNING, "Only sockets with a read_func can have a read buffer");
		return;
	}

	sock->read_buf = malloc(len + 1);
	memset(sock->read_buf, 0, len + 1);
	sock->read_buf_len = len;
	sock->read_buf_used = 0;
	sock->read_buf_delimiter = strdup(buf_delimiter);

	debug("Read buffer of %lu bytes set for socket with fd=%d", (unsigned long)len, sock->fd);
}

