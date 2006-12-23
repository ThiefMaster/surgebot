#ifndef CHANREG_H
#define CHANREG_H

#include "list.h"

struct chanreg_user;
DECLARE_LIST(chanreg_user_list, struct chanreg_user *)

#define CHANREG_USER_SUSPENDED	0x1

enum user_levels
{
    UL_PEON	= 100,
    UL_OP	= 200,
    UL_MASTER	= 300,
    UL_COOWNER	= 400,
    UL_OWNER 	= 500
};

struct chanreg
{
	char *channel;
	unsigned int active : 1;
	const char *last_error;
	struct chanreg_user_list *users;
};

struct chanreg_user
{
	struct chanreg *reg;
	struct user_account *account;
	unsigned short level;
	unsigned int flags;
};

#define CHANREG_COMMAND								\
	struct chanreg *reg;							\
	if(!channel)								\
	{									\
		reply("You must provide the name of a channel that exists.");	\
		return 0;							\
	}									\
	else if(!(reg = chanreg_find(channel->name)))				\
	{									\
		reply("$b%s$b is not registered.", channel->name);		\
		return 0;							\
	}

extern unsigned int chanreg_staff_rule;
#define IsStaff()	(chanreg_staff_rule && command_rule_exec(chanreg_staff_rule, src, user, channel) == CR_ALLOW)

#endif
