#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "sock.h"
#include "table.h"

MODULE_DEPENDS("commands", NULL);

struct paster {
	struct sock *listener;
	struct sock *sock;
	unsigned int port;
	unsigned int paste_started : 1;
	char *owner;
	char *channel;
};

DECLARE_LIST(paster_list, struct paster *)
IMPLEMENT_LIST(paster_list, struct paster *)

COMMAND(paste);
COMMAND(stats_paste);
static void paster_free(struct paster *paster);

static struct module *this;
static struct paster_list *pasters;

MODULE_INIT
{
	this = self;
	pasters = paster_list_create();

	DEFINE_COMMAND(this, "paste", paste, 1, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(this, "stats paste", stats_paste, 1, 0, "group(admins)");
}

MODULE_FINI
{
	while(pasters->count)
		paster_free(pasters->data[0]);
	paster_list_free(pasters);
}


static void paster_free(struct paster *paster)
{
	paster_list_del(pasters, paster);
	if(paster->listener)
		sock_close(paster->listener);
	if(paster->sock)
		sock_close(paster->sock);
	free(paster->owner);
	free(paster->channel);
	free(paster);
}

static void paste_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ERROR || event == EV_HANGUP)
	{
		struct paster *paster = NULL;

		for(int i = 0; i < pasters->count; i++)
		{
			if(pasters->data[i]->sock == sock)
			{
				paster = pasters->data[i];
				break;
			}
		}

		if(!paster)
			return;

		debug("Paste client %s->%s disconnected", paster->owner, paster->channel);
		paster->sock = NULL;
		paster_free(paster);
	}
}

static void paste_read(struct sock *sock, char *buf, size_t len)
{
	struct paster *paster = NULL;

	for(int i = 0; i < pasters->count; i++)
	{
		if(pasters->data[i]->sock == sock)
		{
			paster = pasters->data[i];
			break;
		}
	}

	if(!paster)
		return;

	if(!paster->paste_started)
	{
		irc_send("PRIVMSG %s :Paste from $b%s$b:", paster->channel, paster->owner);
		paster->paste_started = 1;
	}

	irc_send_raw("PRIVMSG %s :%s", paster->channel, buf);
}

static void paste_listener_event(struct sock *sock, enum sock_event event, int err)
{
	if(event == EV_ACCEPT)
	{
		struct paster *paster = NULL;

		for(int i = 0; i < pasters->count; i++)
		{
			if(pasters->data[i]->listener == sock)
			{
				paster = pasters->data[i];
				break;
			}
		}

		if(!paster)
		{
			log_append(LOG_WARNING, "Got connection on paste socket which has no paster assigned");
			sock_close(sock);
			return;
		}

		if(!(paster->sock = sock_accept(sock, paste_event, paste_read)))
		{
			log_append(LOG_WARNING, "accept() failed for paster %s->%s", paster->owner, paster->channel);
			paster_free(paster);
			return;
		}

		debug("Accepted paste connection from %s for paster %s->%s", inet_ntoa(((struct sockaddr_in *)paster->sock->sockaddr_remote)->sin_addr), paster->owner, paster->channel);
		sock_set_readbuf(paster->sock, 512, "\r\n");
		sock_close(paster->listener);
		paster->listener = NULL;
	}
}

COMMAND(paste)
{
	struct sock *listener;
	struct paster *paster;
	unsigned int port;
	unsigned char dcc = 0;

	port = argc > 1 ? atoi(argv[1]) : 0;
	if(port && (port <= 1024 || port > 65535))
	{
		reply("Invalid port number; must be in range 1025..65535");
		return 0;
	}

	if(port)
	{
		for(int i = 0; i < pasters->count; i++)
		{
			struct paster *tmp = pasters->data[i];
			if(tmp->port == port && tmp->listener)
			{
				reply("Port $b%d$b is already used by another paster (%s -> %s).", port, tmp->owner, tmp->channel);
				return 0;
			}
		}
	}

	listener = sock_create(SOCK_IPV4, paste_listener_event, NULL);
	if(!listener)
	{
		reply("Could not create paste socket.");
		return 0;
	}

	if(sock_bind(listener, "0.0.0.0", port) != 0)
	{
		reply("Could not bind paste socket to port $b%d$b.", port);
		free(listener);
		return 0;
	}

	if(!port)
	{
		struct sockaddr_in local_addr;
		socklen_t len = sizeof(struct sockaddr_in);
		if(getsockname(listener->fd, (struct sockaddr *)&local_addr, &len) != 0)
		{
			reply("Could not retrieve random port.");
			free(listener);
			return 0;
		}

		port = ntohs(local_addr.sin_port);
		dcc = 1;
	}

	if(sock_listen(listener, NULL) != 0)
	{
		reply("Could not enable listening mode for paste socket.");
		free(listener);
		return 0;
	}

	paster = malloc(sizeof(struct paster));
	memset(paster, 0, sizeof(struct paster));
	paster->listener = listener;
	paster->port = port;
	paster->owner = (argc > 2 ? strdup(argv[2]) : strdup(src->nick));
	paster->channel = strdup(channel->name);
	paster_list_add(pasters, paster);

	if(dcc)
	{
		struct sockaddr_in local_addr;
		socklen_t len = sizeof(struct sockaddr_in);
		if(getsockname(bot.server_sock->fd, (struct sockaddr *)&local_addr, &len) != 0)
		{
			reply("Could not retrieve local IP required for DCC.");
			return 0;
		}

		irc_send("PRIVMSG %s :\001DCC CHAT chat %d %d\001", paster->owner, ntohl(local_addr.sin_addr.s_addr), port);
	}
	else
	{
		reply("Paster is listening on port $b%d$b.", port);
	}
	return 1;
}

COMMAND(stats_paste)
{
	if(!pasters->count)
	{
		reply("There are no active pasters right now.");
		return 0;
	}

	struct table *table = table_create(4, pasters->count);
	table_set_header(table, "Paster", "Channel", "Port", "Connected");

	for(int i = 0; i < pasters->count; i++)
	{
		struct paster *paster = pasters->data[i];
		table->data[i][0] = paster->owner;
		table->data[i][1] = paster->channel;
		table->data[i][2] = strtab(paster->port);
		table->data[i][3] = (paster->sock ? "yes" : "no");
	}

	table_send(table, src->nick);
	table_free(table);

	return 1;
}
