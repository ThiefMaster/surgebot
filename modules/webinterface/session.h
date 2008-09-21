#ifndef SESSION_H
#define SESSION_H

struct user_account;
struct http_client;

struct session
{
	char *sid;
	time_t lastactivity;
	struct in_addr ip;
	struct user_account *account;
};

void session_init();
void session_fini();

struct session *session_create(struct user_account *account, struct in_addr ip);
struct session *session_find_sid(struct http_client *client, const char *sid);
struct session *session_find(struct http_client *client, char *uri, int argc, char **argv);
void session_del(struct session *session);

#endif
