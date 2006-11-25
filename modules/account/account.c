#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "account.h"
#include "group.h"
#include "sha1.h"
#include "irc.h"
#include "stringlist.h"
#include "conf.h"

MODULE_DEPENDS("commands", "help", NULL);

COMMAND(register);
COMMAND(auth);
COMMAND(pass);
COMMAND(unregister);
COMMAND(accountinfo);

COMMAND(group_list);
COMMAND(group_info);
COMMAND(group_add);
COMMAND(group_del);
COMMAND(group_member_add);
COMMAND(group_member_del);

MODULE_INIT
{
	help_load(self, "account.help");

	/* Regular commands */
	DEFINE_COMMAND(self, "register",	register,	3, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_KEEP_BOUND, "true");
	DEFINE_COMMAND(self, "auth",		auth,		3, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_KEEP_BOUND, "true");
	DEFINE_COMMAND(self, "pass",		pass,		3, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_REQUIRE_AUTHED, "true");
	DEFINE_COMMAND(self, "unregister",	unregister,	2, CMD_LOG_HOSTMASK | CMD_ONLY_PRIVMSG | CMD_REQUIRE_AUTHED, "true");
	DEFINE_COMMAND(self, "accountinfo",	accountinfo,	1, 0, "true");

	/* Administrative commands */
	DEFINE_COMMAND(self, "group list",	group_list,		1, 0, "group(admins)");
	DEFINE_COMMAND(self, "group info",	group_info,		2, 0, "group(admins)");
	DEFINE_COMMAND(self, "group create",	group_add,		2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group remove",	group_del,		2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group addmember",	group_member_add,	3, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "group delmember",	group_member_del,	3, CMD_REQUIRE_AUTHED, "group(admins)");
}

MODULE_FINI
{

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

	if(user->account)
	{
		reply("You are already authed to account $b%s$b.", user->account->name);
		argv[2] = "AUTHED";
		return 0;
	}

	if((account = account_find(argv[1])) == NULL)
	{
		if(!stealth)
			reply("Could not find account $b%s$b - did you register yet?", argv[1]);
		argv[2] = "BADACCOUNT";
		return 0;
	}

	if(strcmp(sha1(argv[2]), account->pass)) // Invalid password
	{
		if(!stealth)
			reply("Invalid password for account $b%s$b.", account->name);
		argv[2] = "BADPASS";
		return 1;
	}

	account_user_add(account, user);
	reply("You are now authed to account $b%s$b.", account->name);
	argv[2] = "****";
	return 1;

}

COMMAND(pass)
{
	if(strcmp(sha1(argv[1]), user->account->pass)) // Invalid old password
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
		for(int i = 0; i < lines->count; i++)
			reply("  $bGroups:         $b %s", lines->data[i]);

		stringlist_free(slist);
		stringlist_free(lines);
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
		for(int i = 0; i < lines->count; i++)
			reply("  $bCurrent nicks:  $b %s", lines->data[i]);

		stringlist_free(slist);
		stringlist_free(lines);
	}

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
	for(int i = 0; i < lines->count; i++)
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
		for(int i = 0; i < group->members->count; i++)
			stringlist_add(slist, strdup(group->members->data[i]));

		stringlist_sort(slist);
		lines = stringlist_to_irclines(src->nick, slist);
		for(int i = 0; i < lines->count; i++)
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

