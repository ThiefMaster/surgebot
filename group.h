#ifndef GROUP_H
#define GROUP_H

#include "stringlist.h"

#define VALID_GROUP_CHARS	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890_-"

struct access_group
{
	char			*name;
	unsigned char		internal;
	unsigned char		root;
	struct stringlist	*members;
};

void group_init();
void group_fini();

struct dict *group_dict();
struct access_group *group_add(const char *name, unsigned char internal);
struct access_group *group_find(const char *name);
void group_del(struct access_group *group);

void group_member_add(struct access_group *group, struct user_account *account);
void group_member_del(struct access_group *group, struct user_account *account);
int group_has_member(const char *group_name, const struct user_account *account);

#endif

