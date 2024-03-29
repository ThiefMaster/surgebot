#include "global.h"
#include "account.h"
#include "group.h"
#include "sha1.h"
#include "database.h"
#include "chanuser.h"
#include "irc.h"

IMPLEMENT_HOOKABLE(account_del);

static struct dict *account_list;
static struct database *account_db;

static struct user_account *account_add(const char *name, const char *pass, time_t regtime, struct stringlist *login_masks);
static void account_db_read(struct database *db);
static int account_db_write(struct database *db);

void account_init()
{
	account_list = dict_create();

	account_db = database_create("accounts", account_db_read, account_db_write);
	database_read(account_db, 1);
	database_set_write_interval(account_db, 300);
}

void account_fini()
{
	database_write(account_db);
	database_delete(account_db);

	while(dict_size(account_list))
		account_del(dict_first_data(account_list));

	dict_free(account_list);
	clear_account_del_hooks();
}

static void account_db_read(struct database *db)
{
	dict_iter(rec, db->nodes)
	{
		struct dict *obj = ((struct db_node *)rec->data)->data.object;
		char *name = rec->key;
		char *pass = database_fetch(obj, "password", DB_STRING);
		char *registered = database_fetch(obj, "registered", DB_STRING);

		struct stringlist *login_masks;
		// try to fetch a stringlist of loginmasks
		login_masks = database_fetch(obj, "loginmasks", DB_STRINGLIST);
		// if there is no such node, the config may still have the old format where a single loginmask was stored as a string
		if(!login_masks)
		{
			login_masks = stringlist_create();

			char *login_mask = database_fetch(obj, "loginmask", DB_STRING);
			if(login_mask)
			{
				stringlist_add(login_masks, strdup(login_mask));
			}
		}
		else
		{
			login_masks = stringlist_copy(login_masks);
		}

		struct user_account *account = account_add(name, pass, registered ? atoi(registered) : 0, login_masks);
	}
}

static int account_db_write(struct database *db)
{
	dict_iter(node, account_list)
	{
		struct user_account *account = node->data;

		database_begin_object(db, account->name);
			database_write_string(db, "password", account->pass);
			database_write_long(db, "registered", account->registered);
			database_write_stringlist(db, "loginmasks", account->login_masks);
		database_end_object(db);
	}
	return 0;
}


struct dict *account_dict()
{
	return account_list;
}

struct user_account *account_register(const char *name, const char *pass)
{
	struct user_account *account;
	struct access_group *group;

	account = account_add(name, sha1(pass), now, stringlist_create());

	if(dict_size(account_list) == 1)
	{
		// Try adding the first account to the admin group
		assert_return(group = group_find("admins"), account);
		group_member_add(group, account);
	}

	return account;
}

static struct user_account *account_add(const char *name, const char *pass, time_t regtime, struct stringlist *login_masks)
{
	struct user_account *account = malloc(sizeof(struct user_account));
	memset(account, 0, sizeof(struct user_account));

	account->name = strdup(name);
	safestrncpy(account->pass, pass, sizeof(account->pass));
	account->registered = regtime;
	account->users = dict_create();
	account->groups = dict_create();
	account->login_masks = login_masks;

	dict_insert(account_list, account->name, account);
	return account;
}

void account_set_pass(struct user_account *account, const char *pass)
{
	safestrncpy(account->pass, sha1(pass), sizeof(account->pass));
}

struct user_account *account_find(const char *name)
{
	return dict_find(account_list, name);
}

void account_del(struct user_account *account)
{
	CALL_HOOKS(account_del, (account));

	dict_iter(node, account->users)
	{
		struct irc_user *user = node->data;
		user->account = NULL;
	}

	dict_iter(node, account->groups)
	{
		struct access_group *group = node->data;
		group_member_del(group, account);
	}

	dict_delete(account_list, account->name);

	dict_free(account->users);
	dict_free(account->groups);
	free(account->name);
	stringlist_free(account->login_masks);
	free(account);
}

struct user_account *account_find_smart(struct irc_source *src, const char *name)
{
	struct user_account *account;
	struct irc_user *target;

	if(name == NULL || strlen(name) == 0)
	{
		reply("No nick/account provided.");
		return NULL;
	}

	if(*name == '*')
	{
		name++;

		if(!strlen(name)) // only "*"
		{
			reply("Empty account name.");
			return NULL;
		}

		if((account = account_find(name)) == NULL)
		{
			reply("Account $b%s$b has not been registered.", name);
			return NULL;
		}

		return account;
	}
	else
	{
		if((target = user_find(name)) == NULL)
		{
			reply("User with nick $b%s$b not found.", name);
			return NULL;
		}

		if(target->account == NULL)
		{
			reply("User $b%s$b is not authed.", name);
			return NULL;
		}

		return target->account;
	}

}

struct user_account *account_find_bynick(const char *nick)
{
	struct irc_user *user;

	if(!(user = user_find(nick)))
		return NULL;

	return user->account;
}

void account_user_add(struct user_account *account, struct irc_user *user)
{
	assert(dict_find(account->users, user->nick) == NULL);

	user->account = account;
	dict_insert(account->users, user->nick, user);
}

void account_user_del(struct user_account *account, struct irc_user *user)
{
	assert(dict_find(account->users, user->nick));

	user->account = NULL;
	dict_delete(account->users, user->nick);
}

