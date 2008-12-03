#ifndef __CHANSERV_USERS_H__
#define __CHANSERV_USERS_H__

#include "modules/chanserv/chanserv.h"

enum chanserv_user_status
{
	CS_USER_NORMAL,
	CS_USER_SUSPENDED,
	CS_USER_VACATION,
};

struct chanserv_user
{
	char *name;
	unsigned int access;
	enum chanserv_user_status status;

	time_t last_seen;
};

inline int chanserv_user_add(struct chanserv_channel *, const char *line, int argc, char **argv);
void chanserv_user_free(struct chanserv_user *user);

#endif // __CHANSERV_USERS_H__
