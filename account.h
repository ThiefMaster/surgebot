#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "hook.h"

#define VALID_ACCOUNT_CHARS	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890_-[]{}|^`"

void account_init();
void account_fini();

struct dict *account_dict();
struct user_account *account_register(const char *name, const char *pass);
void account_set_pass(struct user_account *account, const char *pass);
struct user_account *account_find(const char *name);
void account_del(struct user_account *account);

struct user_account *account_find_smart(struct irc_source *src, const char *name);
struct user_account *account_find_bynick(const char *nick);

void account_user_add(struct user_account *account, struct irc_user *user);
void account_user_del(struct user_account *account, struct irc_user *user);

DECLARE_HOOKABLE(account_del, (struct user_account *account));

#endif

