#include "global.h"
#include "timer.h"
#include "mtrand.h"
#include "account.h"
#include "session.h"
#include "modules/httpd/http.h"

#define SESSION_TIMEOUT 1800

static void session_free(struct session *session);
static void session_expire(void *bound, void *data);
static char *generate_sid();

static struct dict *sessions;

void session_init()
{
	sessions = dict_create();
	dict_set_free_funcs(sessions, NULL, (dict_free_f*)session_free);
	timer_add(NULL, "expire_sessions", now + 120, session_expire, NULL, 0, 1);
}

void session_fini()
{
	timer_del(NULL, "expire_sessions", 0, NULL, NULL, TIMER_IGNORE_ALL & ~TIMER_IGNORE_NAME);
	dict_free(sessions);
}

void session_del(struct session *session)
{
	assert(session);
	dict_delete(sessions, session->sid);
}

struct session *session_create(struct user_account *account, struct in_addr ip)
{
	struct session *session = malloc(sizeof(struct session));
	memset(session, 0, sizeof(struct session));
	session->sid		= generate_sid();
	session->lastactivity	= now;
	session->ip		= ip;
	session->account	= account;
	dict_insert(sessions, session->sid, session);

	debug("Created new session %s", session->sid);
	return session;
}

struct session *session_find_sid(struct http_client *client, const char *sid)
{
	struct session *session;
	if((session = dict_find(sessions, sid)))
	{
		if(((struct sockaddr_in *)client->sock->sockaddr_remote)->sin_addr.s_addr != session->ip.s_addr)
		{
			session_del(session);
			return NULL;
		}

		session->lastactivity = now;
		return session;
	}

	return NULL;
}

struct session *session_find(struct http_client *client, char *uri, int argc, char **argv)
{
	return argc > 2 ? session_find_sid(client, argv[2]) : NULL;
}

static char *generate_sid()
{
	static char chars[62];
	static unsigned int firsttime = 1;
	if(firsttime)
	{
		unsigned char c;
		unsigned int i = 0;
		for(c = 'A'; c <= 'Z'; c++)
		{
			chars[i++] = c;
			chars[i++] = c + 32; // lowercase
		}

		for(c = '0'; c <= '9'; c++)
			chars[i++] = c;
		firsttime = 0;
	}

	char *sid = malloc(17);
	for(unsigned int i = 0; i < 17; i++)
		sid[i] = chars[mt_rand(0, 61)];
	sid[10] = '\0';
	return sid;
}

static void session_expire(void *bound, void *data)
{
	dict_iter(node, sessions)
	{
		struct session *s = node->data;
		if(s->lastactivity < now - SESSION_TIMEOUT)
		{
			debug("Expiring session %s (inactive for %s)", s->sid, duration2string(now - s->lastactivity));
			dict_delete(sessions, s->sid);
		}
	}
	timer_add(NULL, "expire_sessions", now + 120, session_expire, NULL, 0, 1);
}

static void session_free(struct session *session)
{
	free(session->sid);
	free(session);
}

