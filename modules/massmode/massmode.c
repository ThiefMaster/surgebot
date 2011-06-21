#include "global.h"
#include "module.h"
#include "stringbuffer.h"
#include "dict.h"
#include "irc.h"
#include "modules/commands/commands.h"
#include "modules/pushmode/pushmode.h"

MODULE_DEPENDS("commands", "pushmode", NULL);

COMMAND(massmode);

MODULE_INIT
{
	DEFINE_COMMAND(self, "massmode", massmode, 2, CMD_ACCEPT_CHANNEL|CMD_REQUIRE_CHANNEL, "group(admins)");
}

MODULE_FINI
{
}

/*
 * This command expects two arguments, a mode to set and a mask of users to match
 */
COMMAND(massmode)
{
	char *usermask = strdup(argv[2]);
	char *usermode = argv[1];
	struct stringbuffer *sbuf = stringbuffer_create();

	// if only nickmask given, expand to hostmask
	if(strchr(usermask, '!') == NULL && strchr(usermask, '@') == NULL) {
		char *usermask_backup = usermask;
		const char *mask_suffix = "!*@*";
		usermask = malloc(strlen(usermask_backup) + strlen(mask_suffix) + 1);
		strcpy(usermask, usermask_backup);
		strcat(usermask, mask_suffix);
		free(usermask_backup);
	}

	// iterate through all channel users to find matching masks
	dict_iter(node, channel->users)
	{
		struct irc_user *user = ((struct irc_chanuser *)node->data)->user;
		stringbuffer_printf(sbuf, "%s!%s@%s", user->nick, user->ident, user->info);
		if(match(usermask, sbuf->string) == 0) {
			pushmode(channel, usermode, user->nick);
		}
		stringbuffer_empty(sbuf);
	}

	free(usermask);
	stringbuffer_free(sbuf);
	return 1;
}
