#ifndef COMMANDS_H
#define COMMANDS_H

#define COMMAND(CMD)	static int __command_ ## CMD(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, int argc, char **argv)
typedef int (command_f)(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, int argc, char **argv);
#define DEFINE_COMMAND(MOD, NAME, FUNC, ARGC, FLAGS)	command_add(MOD, NAME, __command_ ## FUNC, ARGC, FLAGS)

#define CMD_ACCEPT_CHANNEL	0x001 // accept channel argument
#define CMD_LOG_HOSTMASK	0x002 // log full mask
#define CMD_ONLY_PRIVMSG	0x004 // do not allow public use of this command
#define CMD_ALLOW_UNKNOWN	0x008 // allow unknown users (no common channels) to use this command
#define CMD_REQUIRE_AUTHED	0x010 // user must be authed to use this command
#define CMD_KEEP_BOUND		0x020 // do not allow unbinding last binding of this command

struct command
{
	char		*name;
	struct module	*module;
	const char	*key;

	command_f	*func;

	unsigned 	min_argc;
	int		flags;

	unsigned 	bind_count;
};

struct cmd_binding
{
	char		*name;

	char		*module_name;
	struct module	*module;

	char		*cmd_name;
	struct command	*cmd;

	char		*alias;
};

struct command *command_add(struct module *module, const char *name, command_f *func, int min_argc, int flags);
struct command *command_find(struct module *module, const char *name);
struct cmd_binding *binding_add(const char *name, const char *module_name, const char *cmd_name, const char *alias);
struct cmd_binding *binding_find(const char *name);
struct cmd_binding *binding_find_active(const char *name);
void binding_del(struct cmd_binding *binding);

#endif
