#include "global.h"
#include "account.h"
#include "group.h"
#include "sha1.h"
#include "database.h"
#include "chanuser.h"
#include "irc.h"

static struct dict *account_list;
static struct database *account_db;

static struct user_account *account_add(const char *name, const char *pass, time_t regtime);
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
}

static void account_db_read(struct database *db)
{
	dict_iter(rec, db->nodes)
	{
		struct dict *obj = ((struct db_node *)rec->data)->data.object;
		char *name = rec->key;
		char *pass = database_fetch(obj, "password", DB_STRING);
		char *registered = database_fetch(obj, "registered", DB_STRING);

		account_add(name, pass, registered ? atoi(registered) : 0);
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

	account = account_add(name, sha1(pass), now);

	if(dict_size(account_list) == 1)
	{
		// Try adding the first account to the admin group
		assert_return(group = group_find("admins"), account);
		group_member_add(group, account);
	}


	return account;
}

static struct user_account *account_add(const char *name, const char *pass, time_t regtime)
{
	struct user_account *account = malloc(sizeof(struct user_account));
	memset(account, 0, sizeof(struct user_account));

	account->name = strdup(name);
	safestrncpy(account->pass, pass, sizeof(account->pass));
	account->registered = regtime;
	account->users = dict_create();
	account->groups = dict_create();

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

