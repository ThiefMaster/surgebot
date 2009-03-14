#ifndef __CHANSERV_USERS_H__
#define __CHANSERV_USERS_H__

#include "modules/chanserv/chanserv.h"

enum chanserv_user_status
{
	CS_USER_NORMAL,
	CS_USER_SUSPENDED,
	CS_USER_VACATION,
};

struct chanserv_account
{
	char *name;
	unsigned char vacation;

	struct ptrlist *irc_users;
	// How many users refer to this account?
	unsigned int refcount;
};

struct chanserv_user
{
	unsigned int access;
	enum chanserv_user_status status;
	time_t last_seen;

	struct chanserv_account *account;
};

extern struct dict *chanserv_accounts;

void chanserv_account_free(struct chanserv_account *account);

inline int chanserv_user_add(struct chanserv_channel *, const char *line, int argc, char **argv);
void chanserv_user_parse_names(struct chanserv_channel *cschan, const char *line);
void chanserv_user_del(struct irc_user *user, unsigned int quit, const char *reason);
void chanserv_user_free(struct chanserv_user *user);

#endif // __CHANSERV_USERS_H__
