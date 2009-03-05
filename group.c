#include "global.h"
#include "group.h"
#include "account.h"
#include "database.h"

static struct dict *group_list;
static struct database *group_db;

static void group_db_read(struct database *db);
static int group_db_write(struct database *db);


void group_init()
{
	struct access_group *group;

	group_list = dict_create();

	group_db = database_create("groups", group_db_read, group_db_write);
	database_read(group_db, 1);
	database_set_write_interval(group_db, 300);

	// create admin group
	if((group = group_find("admins")) == NULL)
		group = group_add("admins", 1);
	group->internal = 1;
	group->root = 1;
}

void group_fini()
{
	database_write(group_db);
	database_delete(group_db);

	while(dict_size(group_list))
		group_del(dict_first_data(group_list));

	dict_free(group_list);
}

static void group_db_read(struct database *db)
{
	dict_iter(rec, db->nodes)
	{
		struct dict *obj = ((struct db_node *)rec->data)->data.object;
		struct access_group *group;
		char *name = rec->key;
		struct stringlist *members = database_fetch(obj, "members", DB_STRINGLIST);

		if((group = group_add(name, 0)) && members != NULL)
		{
			for(unsigned int i = 0; i < members->count; i++)
			{
				struct user_account *account = account_find(members->data[i]);
				if(account == NULL)
				{
					log_append(LOG_WARNING, "Account %s which is a member of group %s does not exist", members->data[i], group->name);
					continue;
				}

				group_member_add(group, account);
			}
		}
	}
}

static int group_db_write(struct database *db)
{
	dict_iter(node, group_list)
	{
		struct access_group *group = node->data;

		database_begin_object(db, group->name);
			database_write_stringlist(db, "members", group->members);
		database_end_object(db);
	}
	return 0;
}


struct dict *group_dict()
{
	return group_list;
}

struct access_group *group_add(const char *name, unsigned char internal)
{
	struct access_group *group = malloc(sizeof(struct access_group));
	memset(group, 0, sizeof(struct access_group));

	group->name = strdup(name);
	group->internal = internal;
	group->members = stringlist_create();

	dict_insert(group_list, group->name, group);
	return group;
}

struct access_group *group_find(const char *name)
{
	return dict_find(group_list, name);
}

void group_del(struct access_group *group)
{
	for(unsigned int i = 0; i < group->members->count; i++)
	{
		struct user_account *account = account_find(group->members->data[i]);
		assert_continue(account);
		group_member_del(group, account);
		i--;
	}

	dict_delete(group_list, group->name);
	stringlist_free(group->members);
	free(group->name);
	free(group);
}

void group_member_add(struct access_group *group, struct user_account *account)
{
	stringlist_add(group->members, strdup(account->name));
	dict_insert(account->groups, group->name, group);
}

void group_member_del(struct access_group *group, struct user_account *account)
{
	int pos = stringlist_find(group->members, account->name);
	assert(pos >= 0);
	stringlist_del(group->members, pos);
	dict_delete(account->groups, group->name);
	debug("removed %s from %s", group->name, account->name);
}

int group_has_member(const char *group_name, const struct user_account *account)
{
	struct access_group *group;

	if((group = group_find(group_name)) == NULL)
	{
		log_append(LOG_WARNING, "Invalid group '%s' in group_is_member()", group_name);
		return -1;
	}

	return (dict_find(account->groups, group->name) == NULL) ? 0 : 1;
}

