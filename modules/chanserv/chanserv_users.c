#define _GNU_SOURCE
#include <time.h>
#include <string.h>
#include "global.h"
#include "modules/chanserv/chanserv.h"

struct dict *chanserv_accounts;

int chanserv_user_add(struct chanserv_channel *cschan, const char *line, int argc, char **argv)
{
	int access;
	struct chanserv_user *cs_user;
	char *tmp;

	// Skip first line
	if(!strcmp(argv[0], "Access"))
		return 0;

	access = atoi(argv[0]);
	if(!access)
		log_append(LOG_ERROR, "Call to atoi returns 0 for user's access from line '%s'", line);

	if(argc >= 3)
	{
		cs_user = malloc(sizeof(struct chanserv_user));
		memset(cs_user, 0, sizeof(struct chanserv_user));

		cs_user->access = access;

		if(!(cs_user->account = dict_find(chanserv_accounts, argv[1])))
		{
			cs_user->account = malloc(sizeof(struct chanserv_account));
			memset(cs_user->account, 0, sizeof(struct chanserv_account));

			cs_user->account->name = strdup(argv[1]);
			cs_user->account->irc_users = ptrlist_create();

			dict_insert(chanserv_accounts, cs_user->account->name, cs_user->account);
		}
		cs_user->account->refcount++;

		tmp = trim(strndup(line + (argv[2] - argv[0]), (argv[argc - 1] - argv[2]) - 1));
		cs_user->last_seen = parse_chanserv_duration(tmp);
		free(tmp);

		if(!strcmp(argv[argc - 1], "Vacation"))
		{
			cs_user->status = CS_USER_VACATION;
			cs_user->account->vacation = 1;
		}
		else if(!strcmp(argv[argc - 1], "Suspended"))
			cs_user->status = CS_USER_SUSPENDED;
		else
			cs_user->status = CS_USER_NORMAL;

		dict_insert(cschan->users, cs_user->account->name, cs_user);
		if(cschan->users->count == cschan->user_count)
		{
			debug("Fetched userlist from channel %s, %d users", cschan->reg->channel, cschan->users->count);
			cschan->process = CS_P_NONE;
			// Retrieve channel names (nick -> account association)
			irc_send(sz_chanserv_fetch_names, cschan->reg->channel);
			return 1;
		}
	}

	return 0;
}

static void chanserv_user_parse_names_item(struct chanserv_channel *cschan, const char *item)
{
	const char *ptr = item;
	char *nick, *accountname, *ptr2;
	struct chanserv_account *account;
	unsigned long access;
	struct irc_user *user;

	access = strtoul(item, &ptr2, 10);
	assert((*ptr2 == ':'));

	// End of nick
	assert((ptr = strchr(++ptr2, '(')));
	nick = strndup(ptr2, ptr - ptr2);

	// End of accountname
	assert((ptr2 = strchr(++ptr, ')')));
	accountname = strndup(ptr, ptr2 - ptr);

	// Does this account already exist?
	assert_goto((account = dict_find(chanserv_accounts, accountname)), free_vars);

	// Get usernode
	assert_goto((user = user_find(nick)), free_vars);

	// See if this nick needs to be added
	if(ptrlist_find(account->irc_users, user) == (unsigned int)-1)
		ptrlist_add(account->irc_users, 0, user);

free_vars:
	free(nick);
	free(accountname);
}

void chanserv_user_parse_names(struct chanserv_channel *cschan, const char *line)
{
	char *backup = strdup(line), *tmp, *last;
	last = tmp = backup;

	while((tmp = strchr(tmp, ' ')))
	{
		// This doesn't include the space
		char *copy = strndup(last, tmp - last);
		chanserv_user_parse_names_item(cschan, copy);
		free(copy);

		last = ++tmp;
	}

	chanserv_user_parse_names_item(cschan, last);
	free(backup);
}

void chanserv_user_del(struct irc_user *user, unsigned int quit, const char *reason)
{
	dict_iter(node, chanserv_accounts)
	{
		struct chanserv_account *account = node->data;
		ptrlist_del_ptr(account->irc_users, user);
	}
}

void chanserv_user_free(struct chanserv_user *user)
{
	if(user->account && !--user->account->refcount)
		dict_delete(chanserv_accounts, user->account->name);

	free(user);
}

void chanserv_account_free(struct chanserv_account *account)
{
	ptrlist_free(account->irc_users);
	free(account->name);
	free(account);
}
