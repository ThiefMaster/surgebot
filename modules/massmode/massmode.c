#include <errno.h>
#include "global.h"
#include "module.h"
#include "stringbuffer.h"
#include "dict.h"
#include "irc.h"
#include "chanuser.h"
#include "conf.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "modules/pushmode/pushmode.h"
#include "modules/tools/tools.h"

MODULE_DEPENDS("commands", "pushmode", "tools", NULL);

static const long	default_max_modes = 1;
static char *		default_override_rule = "group(admins)";

static struct {
	unsigned long max_modes;
	char *override_rule;
} massmode_conf;

static void massmode_readconf();
COMMAND(massmode);

static unsigned int massmode_override_rule = 0;

MODULE_INIT
{
	DEFINE_COMMAND(self, "massmode", massmode, 2, CMD_ACCEPT_CHANNEL|CMD_REQUIRE_CHANNEL, "group(admins)");

	// make sure we don't have random garbage to start from
	memset(&massmode_conf, 0, sizeof(massmode_conf));

	// now properly read in the configuration
	reg_conf_reload_func(massmode_readconf);
	massmode_readconf();
}

MODULE_FINI
{
	unreg_conf_reload_func(massmode_readconf);
	free(massmode_conf.override_rule);

	if(massmode_override_rule) {
		command_rule_free(massmode_override_rule);
	}
}

/*
 * This command expects two arguments, a mode to set and a mask of users to match
 */
COMMAND(massmode)
{
	// check whether bot is oped
	struct irc_user *ircuser = user_find(bot.nickname);
	assert_return(ircuser != NULL, 0);
	struct irc_chanuser *chanuser = channel_user_find(channel, ircuser);
	assert_return(chanuser != NULL, 0);
	if(!(chanuser->flags & MODE_OP)) {
		reply("I must be opped on $b%s$b to set channel modes.", channel->name);
		return 0;
	}

	char *usermask = strdup(argv[2]);
	char *usermode = argv[1];
	struct stringbuffer *sbuf = stringbuffer_create();
	struct ptrlist *userlist = ptrlist_create();
	ptrlist_set_free_func(userlist, (ptrlist_free_f*)free);

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
		if(match(usermask, sbuf->string) == 0
				&& channel_mode_changes_state(channel, usermode, user->nick)) {
			ptrlist_add(userlist, 0, strdup(user->nick));
		}
		stringbuffer_empty(sbuf);
	}

	// TODO: this is not the real usercount, as users with the mode already set are counted too
	if(userlist->count > massmode_conf.max_modes
			&& command_rule_exec(massmode_override_rule, src, user, channel, channel->name) != CR_ALLOW) {
		reply("This command affects too many users.");
	}
	else {
		for(size_t i = 0; i < userlist->count; ++i) {
			pushmode(channel, usermode, userlist->data[i]->ptr);
		}
		reply("$b%d$b user(s) affected.", userlist->count);
	}

	free(usermask);
	ptrlist_free(userlist);
	stringbuffer_free(sbuf);
	return 1;
}

static void massmode_readconf()
{
	const char *max_modes = conf_get("massmode/max_modes", DB_STRING);
	massmode_conf.max_modes = -1;
	if(!max_modes) {
		debug("massmode: Config node massmode/max_modes has not been configured.");
		debug("massmode: Please specify a maximum number of modes you can set without override access.");
	}
	else {
		errno = 0;
		unsigned long max_modes_long = strtoul(max_modes, NULL, 0);
		if(errno != 0) {
			debug("massmode: Could not parse config node massmode/max_modes: (%d) %s", errno, strerror(errno));
		}
		else {
			massmode_conf.max_modes = max_modes_long;
			debug("massmode: massmode/max_modes = %lu", max_modes_long);
		}
	}

	if(massmode_conf.max_modes == (unsigned long)-1) {
		// not modified, choose default
		debug("massmode: defaulting to max_modes=%lu", default_max_modes);
		massmode_conf.max_modes = default_max_modes;
	}

	if(massmode_override_rule) {
		command_rule_free(massmode_override_rule);
	}
	free(massmode_conf.override_rule);
	char *override_rule = conf_get("massmode/override_rule", DB_STRING);
	if(!override_rule) {
		debug("massmode: Config node massmode/override_rule has not been configured.");
		debug("massmode: Defaulting to override_rule=%s", default_override_rule);
		override_rule = default_override_rule;
	}
	massmode_conf.override_rule = strdup(override_rule);
	debug("massmode: massmode/override_rule = %s", override_rule);
	massmode_override_rule = command_rule_compile(override_rule);
}
