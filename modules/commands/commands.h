#ifndef COMMANDS_H
#define COMMANDS_H

#include "command_rule.h"

#define COMMAND(CMD)	static int __command_ ## CMD(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, const char *channelname, int argc, char **argv)
typedef int (command_f)(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, const char *channelname, int argc, char **argv);
#define DEFINE_COMMAND(MOD, NAME, FUNC, ARGC, FLAGS, RULE)	command_add(MOD, NAME, __command_ ## FUNC, ARGC, FLAGS, RULE)

#define CMD_ACCEPT_CHANNEL	0x001 // accept channel argument
#define CMD_LAZY_ACCEPT_CHANNEL	(0x001 | 0x002) // accept channel argument even if channel is unknown to the bot
#define CMD_LOG_HOSTMASK	0x004 // log full mask
#define CMD_ONLY_PRIVMSG	0x008 // do not allow public use of this command
#define CMD_ALLOW_UNKNOWN	0x010 // allow unknown users (no common channels) to use this command
#define CMD_REQUIRE_AUTHED	0x020 // user must be authed to use this command
#define CMD_KEEP_BOUND		0x040 // do not allow unbinding last binding of this command
#define CMD_REQUIRE_CHANNEL     0x080 // require an existing channel
#define CMD_IGNORE_LOGINMASK	0x100

struct command
{
	char		*name;
	struct module	*module;
	const char	*key;

	command_f	*func;

	unsigned int	min_argc;
	int		flags;
	char		*rule;

	unsigned int	bind_count;
};

struct cmd_binding
{
	char		*name;

	char		*module_name;
	struct module	*module;

	char		*cmd_name;
	struct command	*cmd;

	char		*alias;

	char		*rule;
	unsigned int	comp_rule;
};

struct dict *command_dict();
struct dict *binding_dict();
struct command *command_add(struct module *module, const char *name, command_f *func, int min_argc, int flags, const char *rule);
struct command *command_find(struct module *module, const char *name);
struct cmd_binding *binding_add(const char *name, const char *module_name, const char *cmd_name, const char *alias, const char *rule, unsigned char force);
struct cmd_binding *binding_find(const char *name);
struct cmd_binding *binding_find_active(const char *name);
int binding_set_rule(struct cmd_binding *binding, const char *rule);
void binding_del(struct cmd_binding *binding);

#endif
