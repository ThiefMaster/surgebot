#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "modules/chanreg/chanreg.h"
#include "account.h"
#include "group.h"
#include "sha1.h"
#include "irc.h"
#include "stringlist.h"
#include "conf.h"
#include "dict.h"
#include "policer.h"
#include "timer.h"
#include "strnatcmp.h"

#define OPTION_FUNC(NAME) int NAME(struct irc_source *src, struct user_account *account, int argc, char **argv)
typedef OPTION_FUNC(option_func);

MODULE_DEPENDS("commands", "help", "chanreg", NULL);

struct dict *auth_policers;
struct policer_params *auth_policer_params;

struct auth_policer
{
	char *host;
	struct policer *policer;
	unsigned char gagged : 1;
};
void auth_policer_free(struct auth_policer *);
void auth_policer_add_timer(struct auth_policer *);
void auth_policer_timer_func(void *bound, struct auth_policer *policer);

COMMAND(register);
COMMAND(auth);
COMMAND(pass);
COMMAND(unregister);
COMMAND(accountinfo);
COMMAND(loginmask);
COMMAND(logout);

COMMAND(group_list);
COMMAND(group_info);
COMMAND(group_add);
COMMAND(group_del);
COMMAND(group_member_add);
COMMAND(group_member_del);
COMMAND(rename);
COMMAND(oset);
COMMAND(myaccess);

OPTION_FUNC(oset_password);
OPTION_FUNC(oset_loginmask);

MODULE_INIT
{
	help_load(self, "account.help");

	/* Regular commands */
	DEFINE_COMMAND(self, "register",	register,		2, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_KEEP_BOUND | CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "auth",		auth,			2, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_KEEP_BOUND | CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "pass",		pass,			0, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_REQUIRE_AUTHED | CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "unregister",	unregister,		1, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_REQUIRE_AUTHED | CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "accountinfo",	accountinfo,		0, CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "loginmask",	loginmask,		0, CMD_REQUIRE_AUTHED, "true");
	DEFINE_COMMAND(self, "logout",		logout,			0, CMD_REQUIRE_AUTHED | CMD_IGNORE_LOGINMASK, "true");
	DEFINE_COMMAND(self, "myaccess",	myaccess,		0, CMD_REQUIRE_AUTHED, "true");

	/* Administrative commands */
	DEFINE_COMMAND(self, "group list",	group_list,		0, 0, "group(admins)");
	DEFINE_COMMAND(self, "group info",	group_info,		1, 0, "group(admins)");
	DEFINE_COMMAND(self, "group create",	group_add,		1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group remove",	group_del,		1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group addmember",	group_member_add,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group delmember",	group_member_del,	2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "rename",		rename,			2, 0, "group(admins)");
	DEFINE_COMMAND(self, "oset",		oset,			2, CMD_REQUIRE_AUTHED | CMD_LOG_HOSTMASK, "group(admins)");

	auth_policers = dict_create();
	dict_set_free_funcs(auth_policers, NULL, (dict_free_f*)auth_policer_free);
	auth_policer_params = policer_params_create(5.0, 0.2);
}

MODULE_FINI
{
	timer_del_boundname(NULL, "auth_del_policer");
	dict_free(auth_policers);
	policer_params_free(auth_policer_params);
}

int chanreg_user_cmp(const void *a, const void *b)
{
	struct chanreg_user *u1 = (*(struct ptrlist_node **)a)->ptr;
	struct chanreg_user *u2 = (*(struct ptrlist_node **)b)->ptr;

	return ircnatcasecmp(u1->reg->channel, u2->reg->channel);
}

COMMAND(myaccess)
{
	struct ptrlist *accesslist = ptrlist_create();
	struct user_account *account = user->account;
	struct dict *chanregs = chanreg_dict();

	dict_iter(node, chanregs)
	{
		struct chanreg *chanreg = node->data;
		struct chanreg_user *user = chanreg_user_find(chanreg, account->name);

		if(user != NULL) {
			ptrlist_add(accesslist, 0, user);
		}
	}

	unsigned int count = accesslist->count;
	if(accesslist->count == 0) {
		reply("You don't have access to any channels yet.");
	}
	else {
		struct stringlist *slist = stringlist_create();
		// iterate over ptrlist and add one string for each item
		struct stringbuffer *sbuf = stringbuffer_create();

		ptrlist_sort(accesslist, chanreg_user_cmp);

		for(unsigned int i = 0; i < count; ++i) {
			struct chanreg_user *user = accesslist->data[i]->ptr;
			stringbuffer_empty(sbuf);

			const char *format_string = NULL;
			if(user->flags & CHANREG_USER_SUSPENDED)
				format_string = "(%ld):%s";
			else
				format_string = "$b%ld$b:%s";

			stringbuffer_append_printf(sbuf, format_string, user->level, user->reg->channel);
			stringlist_add(slist, strdup(sbuf->string));
		}

		struct stringlist *irc_lines = stringlist_to_irclines(user->nick, slist);
		for(unsigned int i = 0; i < irc_lines->count; ++i) {
			reply("%s", irc_lines->data[i]);
		}

		stringlist_free(irc_lines);
		stringlist_free(slist);
		stringbuffer_free(sbuf);
	}

	ptrlist_free(accesslist);
	return 1;
}

COMMAND(register)
{
	struct user_account *account;
	char c;

	if(user->account)
	{
		reply("You are already authed to account $b%s$b.", user->account->name);
		argv[2] = "AUTHED";
		return 0;
	}

	if(account_find(argv[1]))
	{
		reply("Account $b%s$b already exists.", argv[1]);
		argv[2] = "EXISTS";
		return 0;
	}

	if(!validate_string(argv[1], VALID_ACCOUNT_CHARS, &c))
	{
		reply("Account name contains invalid characters (first invalid char: %c)", c);
		argv[2] = "BADACCOUNT";
		return 0;
	}

	account = account_register(argv[1], argv[2]);
	account_user_add(account, user);
	reply("Account $b%s$b successfully registered to you.", account->name);
	argv[2] = "****";

	if(group_has_member("admins", account)) // Only the first account is added to the admins group
		reply("By registering the first account, you have been automatically added to the 'admins' group.");

	return 1;
}

COMMAND(auth)
{
	struct user_account *account;
	unsigned int stealth = conf_bool("commands/stealth");
	struct auth_policer *auth_policer;

	if(user->account)
	{
		reply("You are already authed to account $b%s$b.", user->account->name);
		argv[2] = "AUTHED";
		return 0;
	}

	if(!(auth_policer = dict_find(auth_policers, src->host)))
	{
		auth_policer = malloc(sizeof(struct auth_policer));
		memset(auth_policer, 0, sizeof(struct auth_policer));
		auth_policer->host = strdup(src->host);
		auth_policer->policer = policer_create(auth_policer_params);
		dict_insert(auth_policers, auth_policer->host, auth_policer);
	}
	else if(auth_policer->gagged)
		return 0;
	else if(!policer_conforms(auth_policer->policer, now, 1.0))
	{
		auth_policer->gagged = 1;
		auth_policer_add_timer(auth_policer);
		return 0;
	}

	if((account = account_find(argv[1])) == NULL)
	{
		if(!stealth)
			reply("Could not find account $b%s$b - did you register yet?", argv[1]);
		argv[2] = "BADACCOUNT";
		return 0;
	}

	if(!strcmp("*", account->pass) || strcmp(sha1(argv[2]), account->pass)) // Invalid password
	{
		if(!stealth)
			reply("Invalid password for account $b%s$b.", account->name);
		argv[2] = "BADPASS";
		return 1;
	}

	account_user_add(account, user);
	reply("You are now authed to account $b%s$b.", account->name);
	argv[2] = "****";
	timer_del(NULL, "auth_del_policer", 0, NULL, auth_policer, TIMER_IGNORE_BOUND | TIMER_IGNORE_TIME | TIMER_IGNORE_FUNC);
	return 1;
}

COMMAND(pass)
{
	if(argc == 1)
	{
		// Password set?
		if(strcmp(user->account->pass, "*"))
			reply("$bPassword:$b [Encrypted]");
		else
			reply("You have no password set. You can only be automatically authed based on your loginmask.");

		return 0;
	}

	if(argc <= 2)
	{
		reply("Not enough arguments.");
		return 0;
	}

	if(strcmp(user->account->pass, "*") && strcmp(sha1(argv[1]), user->account->pass)) // Invalid old password
	{
		reply("Invalid password for account $b%s$b.", user->account->name);
		argv[1] = "BADPASS";
		argv[2] = "****";
		return 1;
	}

	account_set_pass(user->account, argv[2]);
	reply("Password successfully changed.");
	argv[1] = "****";
	argv[2] = "****";
	return 1;
}

COMMAND(unregister)
{
	if(strcmp(sha1(argv[1]), user->account->pass)) // Invalid password
	{
		reply("Invalid password for account $b%s$b.", user->account->name);
		argv[1] = "BADPASS";
		return 1;
	}

	reply("Your account $b%s$b has been unregistered.", user->account->name);
	argv[1] = "****";
	account_del(user->account);
	return 1;
}

COMMAND(accountinfo)
{
	struct user_account *account;
	struct stringlist *slist, *lines;

	if(argc < 2) // No nick/*account -> show own accountinfo
	{
		if((account = user->account) == NULL) // not authed?
		{
			reply("You must authenticate first.");
			return 0;
		}
	}
	else if((account = account_find_smart(src, argv[1])) == NULL)
	{
		// no need to reply here, account_find_smart() tells the user why it returns NULL
		return 0;
	}

	reply("Account information for $b%s$b:", account->name);
	reply("  $bRegistered:     $b %s", ctime(&(account->registered)));

	if(dict_size(account->groups))
	{
		slist = stringlist_create();
		dict_iter(node, account->groups)
		{
			struct access_group *group = node->data;
			stringlist_add(slist, strdup(group->name));
		}

		stringlist_sort(slist);
		lines = stringlist_to_irclines(src->nick, slist);
		for(unsigned int i = 0; i < lines->count; i++)
			reply("  $bGroups:         $b %s", lines->data[i]);

		stringlist_free(slist);
		stringlist_free(lines);
	}

	if(account->login_mask
		// Getting here without argument means the user is authed and infoing himself, no more checks
		&& (
				(argc < 2)
			||	(user->account && !strcasecmp(user->account->name, account->name))
		)
	)
	{
		reply("  $bLoginmask:      $b %s", account->login_mask);
	}

	if(dict_size(account->users))
	{
		slist = stringlist_create();
		dict_iter(node, account->users)
		{
			struct irc_user *user = node->data;
			stringlist_add(slist, strdup(user->nick));
		}

		stringlist_sort(slist);
		lines = stringlist_to_irclines(src->nick, slist);
		for(unsigned int i = 0; i < lines->count; i++)
			reply("  $bCurrent nicks:  $b %s", lines->data[i]);

		stringlist_free(slist);
		stringlist_free(lines);
	}

	return 1;
}

COMMAND(loginmask)
{
	if(argc > 1)
		return  oset_loginmask(src, user->account, argc - 1, argv + 1);

	if(user->account->login_mask)
		reply("Your loginmask is $b%s$b.", user->account->login_mask);
	else
		reply("You have no loginmask set.");

	return 1;
}

COMMAND(logout)
{
	account_user_del(user->account, user);
	reply("You have been logged out.");
	return 1;
}

COMMAND(group_list)
{
	struct stringlist *slist, *lines;
	struct dict *group_list = group_dict();

	slist = stringlist_create();
	dict_iter(node, group_list)
	{
		struct access_group *group = node->data;
		stringlist_add(slist, strdup(group->name));
	}

	stringlist_sort(slist);
	lines = stringlist_to_irclines(src->nick, slist);
	for(unsigned int i = 0; i < lines->count; i++)
		reply("%s", lines->data[i]);

	stringlist_free(slist);
	stringlist_free(lines);
	return 1;
}

COMMAND(group_info)
{
	struct access_group *group;
	struct stringlist *slist, *lines;
	char *type;

	if((group = group_find(argv[1])) == NULL)
	{
		reply("Group $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(group->root)
		type = "root";
	else if(group->internal)
		type = "internal";
	else
		type = "user-defined";

	reply("Group information for $b%s$b:", group->name);
	reply("  $bType:    $b %s", type);

	if(group->members->count)
	{
		slist = stringlist_create();
		for(unsigned int i = 0; i < group->members->count; i++)
			stringlist_add(slist, strdup(group->members->data[i]));

		stringlist_sort(slist);
		lines = stringlist_to_irclines(src->nick, slist);
		for(unsigned int i = 0; i < lines->count; i++)
			reply("  $bMembers: $b %s", lines->data[i]);

		stringlist_free(slist);
		stringlist_free(lines);
	}
	else
		reply("  $bMembers: $b (none)");

	return 1;
}

COMMAND(group_add)
{
	char c;

	if(group_find(argv[1]) != NULL)
	{
		reply("Group $b%s$b already exists.", argv[1]);
		return 0;
	}

	if(!validate_string(argv[1], VALID_GROUP_CHARS, &c))
	{
		reply("Group name contains invalid characters (first invalid char: %c)", c);
		return 0;
	}

	group_add(argv[1], 0);
	reply("Group $b%s$b has been created.", argv[1]);
	return 1;
}

COMMAND(group_del)
{
	struct access_group *group;

	if((group = group_find(argv[1])) == NULL)
	{
		reply("Group $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(group->internal || group->root)
	{
		reply("Internal group $b%s$b cannot be deleted.", argv[1]);
		return 0;
	}

	if(group_has_member(group->name, user->account))
	{
		reply("Group $b%s$b cannot be deleted since you are a member of it.", argv[1]);
		return 0;
	}

	reply("Group $b%s$b has been deleted.", group->name);
	group_del(group);
	return 1;
}

COMMAND(group_member_add)
{
	struct access_group *group;
	struct user_account *account;

	if((group = group_find(argv[1])) == NULL)
	{
		reply("Group $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if((account = account_find_smart(src, argv[2])) == NULL)
		return 0;

	if(group_has_member(group->name, account))
	{
		reply("$b%s$b is already a member of group $b%s$b.", account->name, group->name);
		return 0;
	}

	group_member_add(group, account);
	reply("Account $b%s$b has been added to group $b%s$b.", account->name, group->name);
	return 1;
}

COMMAND(group_member_del)
{
	struct access_group *group;
	struct user_account *account;

	if((group = group_find(argv[1])) == NULL)
	{
		reply("Group $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if((account = account_find_smart(src, argv[2])) == NULL)
		return 0;

	if(!group_has_member(group->name, account))
	{
		reply("$b%s$b is not a member of group $b%s$b.", account->name, group->name);
		return 0;
	}

	group_member_del(group, account);
	reply("Account $b%s$b has been removed from group $b%s$b.", account->name, group->name);
	return 1;
}

COMMAND(rename)
{
	struct user_account *acc;
	char c;

	if(!(acc = account_find_smart(src, argv[1])))
		return 0;

	else if(account_find(argv[2]))
	{
		reply("An account named $b%s$b already exists.", argv[1]);
		return 0;
	}

	else if(!validate_string(argv[2], VALID_ACCOUNT_CHARS, &c))
	{
		reply("Account name contains invalid characters (first invalid char: %c)", c);
		return 0;
	}

	reply("Account $b%s$b has been renamed to $b%s$b", acc->name, argv[2]);

	// Update dict
	struct dict_node *node = dict_find_node(account_dict(), acc->name);
	free(acc->name);
	acc->name = node->key = strdup(argv[2]);
	return 1;
}

COMMAND(oset)
{
	struct user_account *acc;
	option_func *opfunc;

	if(!(acc = account_find_smart(src, argv[1])))
		return 0;

	if(!strcasecmp(argv[2], "password") || !strcasecmp(argv[2], "pass"))
		opfunc = oset_password;

	else if(!strcasecmp(argv[2], "loginmask"))
		opfunc = oset_loginmask;

	else
	{
		reply("There is no such setting.");
		return 0;
	}

	return opfunc(src, acc, argc - 3, argv + 3);
}

OPTION_FUNC(oset_password)
{
	if(argc > 0)
	{
		if(!strcmp(argv[0], "*"))
		{
			if(!account->login_mask)
			{
				reply("The password can only be deleted if there is a loginmask.");
				return 0;
			}
			strcpy(account->pass, argv[0]);
			reply("$bPassword:$b None");
		}
		else
		{
			account_set_pass(account, argv[0]);
			reply("$bPassword:$b ***");
		}
		return 1;
	}
	else
		reply("You need to specify a new password as well.");

	return 0;
}

OPTION_FUNC(oset_loginmask)
{
	if(!argc)
	{
		if(account->login_mask)
			reply("$bLoginmask:$b %s", account->login_mask);
		else
			reply("$b%s$b has no loginmask set.", account->name);

		return 0;
	}

	if(!strcmp("*", argv[0]))
	{
		if(!strcmp(account->pass, "*"))
		{
			reply("This account has no password set. You may not delete its loginmask.");
			return 0;
		}

		if(account->login_mask)
		{
			free(account->login_mask);
			account->login_mask = NULL;
		}

		reply("$bLoginmask:$b None");
		return 1;
	}
	if(!strcmp("*@*", argv[0]))
	{
		reply("The loginmask MUST NOT be *@* for security reasons, please choose another one.");
		return 0;
	}
	if(match("?*@?*", argv[0]))
	{
		reply("The provided hostmask does not match *@*, please choose another one.");
		return 0;
	}

	if(account->login_mask && strcasecmp(account->login_mask, argv[0]))
	{
		reply("$bLoginmask:$b %s -> $b%s$b.", account->login_mask, argv[0]);
		free(account->login_mask);
	}
	else
		reply("$bLoginmask:$b %s", argv[0]);

	account->login_mask = strdup(argv[0]);
	return 1;
}


void auth_policer_free(struct auth_policer *auth_policer)
{
	free(auth_policer->host);
	policer_free(auth_policer->policer);
	free(auth_policer);
}

void auth_policer_add_timer(struct auth_policer *policer)
{
	timer_add(NULL, "auth_del_policer", now + (60 * 60), (timer_f*)auth_policer_timer_func, policer, 0, 0);
}

void auth_policer_timer_func(void *bound, struct auth_policer *policer)
{
	auth_policer_free(policer);
}
