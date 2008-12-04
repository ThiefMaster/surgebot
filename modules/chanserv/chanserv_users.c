#define _GNU_SOURCE
#include <time.h>
#include <string.h>
#include "global.h"
#include "modules/chanserv/chanserv.h"

inline int chanserv_user_add(struct chanserv_channel *cschan, const char *line, int argc, char **argv)
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
		cs_user->name = strdup(argv[1]);
		cs_user->access = access;

		tmp = trim(strndup(line + (argv[2] - argv[0]), (argv[argc - 1] - argv[2]) - 1));
		cs_user->last_seen = parse_chanserv_duration(tmp);
		free(tmp);

		if(!strcmp(argv[argc - 1], "Vacation"))
			cs_user->status = CS_USER_VACATION;
		else if(!strcmp(argv[argc - 1], "Suspended"))
			cs_user->status = CS_USER_SUSPENDED;
		else
			cs_user->status = CS_USER_NORMAL;

		dict_insert(cschan->users, cs_user->name, cs_user);
		if(cschan->users->count == cschan->user_count)
		{
			debug("Fetched userlist from channel %s, %d users", cschan->reg->channel, cschan->users->count);
			cschan->process = CS_P_NONE;
			return 1;
		}
	}

	return 0;
}

void chanserv_user_free(struct chanserv_user *user)
{
	free(user->name);
	free(user);
}
