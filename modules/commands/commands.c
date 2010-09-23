#include "global.h"
#include "module.h"
#include "command_rule.h"
#include "commands.h"
#include "group.h"
#include "database.h"
#include "stringbuffer.h"
#include "irc.h"
#include "irc_handler.h"
#include "chanuser.h"
#include "conf.h"
#include "account.h"

MODULE_DEPENDS("parser",  NULL);

static struct
{
	unsigned int stealth;
	const char *log_channel;
} command_conf;

extern struct surgebot_conf bot_conf;
static struct dict *command_list;
static struct dict *binding_list;
static struct database *command_db;

IRC_HANDLER(privmsg);
static void command_conf_reload();
static void command_db_read(struct database *db);
static int command_db_write(struct database *db);
static void handle_command(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, const char *msg);
static int binding_expand_alias(struct cmd_binding *binding, struct irc_source *src, unsigned int argc, char **argv, char **exp_argv);
static int binding_check_access(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, char *channelname, struct cmd_binding *binding, unsigned int quiet);
static int show_subcmds(struct irc_source *src, struct irc_user *user, const char *prefix, int check_access);
static char *make_cmd_key(struct module *module, const char *cmd);
static void module_loaded(struct module *module);
static void module_unloaded(struct module *module);
static void command_del(struct command *command);

MODULE_INIT
{
	command_list = dict_create();
	binding_list = dict_create();
	dict_set_free_funcs(command_list, free, NULL);

	command_rule_init();
	reg_module_load_func(module_loaded, module_unloaded);
	reg_irc_handler("PRIVMSG", privmsg);

	reg_conf_reload_func(command_conf_reload);
	command_conf_reload();

	command_db = database_create("commands", command_db_read, command_db_write);
	database_read(command_db, 1);
	database_set_write_interval(command_db, 300);
}

MODULE_FINI
{
	database_write(command_db);
	database_delete(command_db);

	unreg_conf_reload_func(command_conf_reload);
	unreg_irc_handler("PRIVMSG", privmsg);
	unreg_module_load_func(module_loaded, module_unloaded);

	while(dict_size(binding_list))
		binding_del(dict_first_data(binding_list));

	command_rule_fini();
	dict_free(command_list);
	dict_free(binding_list);
}


static void command_conf_reload()
{
	const char *str;
	command_conf.stealth = conf_bool("commands/stealth");

	str = conf_get("commands/log_channel", DB_STRING);
	command_conf.log_channel = str && *str ? str : NULL;
}

static void command_db_read(struct database *db)
{
	struct dict *db_node;

	if((db_node = database_fetch(db->nodes, "bindings", DB_OBJECT)) != NULL)
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			char *name = rec->key;
			char *module_name = database_fetch(obj, "module", DB_STRING);
			char *cmd_name = database_fetch(obj, "cmd", DB_STRING);
			char *rule = database_fetch(obj, "rule", DB_STRING);
			char *alias = database_fetch(obj, "alias", DB_STRING);

			binding_add(name, module_name, cmd_name, alias, rule, 1);
		}
	}
}

static int command_db_write(struct database *db)
{
	database_begin_object(db, "bindings");
		dict_iter(node, binding_list)
		{
			struct cmd_binding *binding = node->data;

			database_begin_object(db, binding->name);
				database_write_string(db, "module", binding->module_name);
				database_write_string(db, "cmd", binding->cmd_name);
				if(binding->rule)
					database_write_string(db, "rule", binding->rule);
				if(binding->alias)
					database_write_string(db, "alias", binding->alias);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

struct dict *command_dict()
{
	return command_list;
}

struct dict *binding_dict()
{
	return binding_list;
}

IRC_HANDLER(privmsg)
{
	struct irc_user *user;
	struct irc_channel *channel;

	assert(argc > 2);
	user = user_find(src->nick);

	if(!strcasecmp(bot.nickname, argv[1]) && *argv[2] != '\001')
		handle_command(src, user, NULL, argv[2]);
	else if(bot_conf.trigger)
	{
		size_t trigger_len = strlen(bot_conf.trigger);
		if((channel = channel_find(argv[1])) && !strncmp(argv[2], bot_conf.trigger, trigger_len) && *(argv[2] + trigger_len))
			handle_command(src, user, channel, (argv[2] + trigger_len));
	}
}

static void handle_command(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, const char *msg)
{
	int is_privmsg = (channel == NULL);
	char *orig_argv[MAXARG], *exp_argv[MAXARG], **argv, *msg_dup, *arg_string, *channel_arg = NULL;
	unsigned int argc, count;
	int ret;
	struct stringbuffer *name, *log_entry;
	struct cmd_binding *binding = NULL, *fallback = NULL;
	struct command *cmd;

	msg_dup = strdup(msg);
	argc = tokenize(msg_dup, orig_argv, MAXARG, ' ', 0);
	argv = orig_argv;

	if(argc && IsChannelName(argv[0]))
	{
		channel_arg = argv[0];
		argv++;
		argc--;
	}

	if(!argc)
	{
		if(!command_conf.stealth || (user && user->account))
			reply("Command missing.");
		free(msg_dup);
		return;
	}

	fallback = binding_find_active(argv[0]);

	// more arguments -> check if we have a two-part command (like "gline add")
	if(argc > 1)
	{
		name = stringbuffer_create();
		stringbuffer_append_string(name, argv[0]);
		stringbuffer_append_char(name, ' ');
		stringbuffer_append_string(name, argv[1]);

		if((binding = binding_find_active(name->string)))
		{
			debug("Found binding: %s", binding->name);

			// argv[0] should always be the command, so merge argv[0] and argv[1] and remove argv[1]
			argv[0][strlen(argv[0])] = ' ';
			argv[1] = argv[0];
			argv++;
			argc--;
		}

		stringbuffer_free(name);
	}

	// no command and no fallback -> search for other commands starting with argv[0] and display them
	if(binding == NULL && fallback == NULL)
	{
		count = show_subcmds(src, user, argv[0], 1);

		if(!count && is_privmsg && (!command_conf.stealth || (user && user->account)))
			reply("$b%s$b is an unknown command.", argv[0]);

		free(msg_dup);
		return;
	}
	else if(binding == NULL && fallback)
	{
		binding = fallback;
	}

	/* From this point on, binding always links to an existing command */
	assert(cmd = binding->cmd);

	if(!is_privmsg && (cmd->flags & CMD_ONLY_PRIVMSG))
	{
		if(!command_conf.stealth || (user && user->account))
			reply("$b%s$b can only be used via $b/msg $N %s$b", binding->name, binding->name);
		free(msg_dup);
		return;
	}

	if(channel_arg && !(cmd->flags & CMD_ACCEPT_CHANNEL))
	{
		if(!command_conf.stealth || (user && user->account))
			reply("You cannot put a channel name before $b%s$b.", binding->name);
		free(msg_dup);
		return;
	}

	if((cmd->flags & CMD_ACCEPT_CHANNEL) && (channel_arg || (argc > 1 && IsChannelName(argv[1]))))
	{
		if(!channel_arg)
		{
			channel_arg = argv[1];
			argv[1] = argv[0];
			argc--;
			argv++;
		}

		// If the channel does not exist, channel_find returns NULL.
		// In this case we return an error since otherwise ".cmd #invalidchannel" done in "#validchannel" would
		// result in 'channel' being set to channel_find("#validchannel") which is not expected
		if((channel = channel_find(channel_arg)) == NULL && !(cmd->flags & CMD_LAZY_ACCEPT_CHANNEL))
		{
			if(!command_conf.stealth || (user && user->account))
				reply("You must provide a channel name that exists and is known to the bot.");
			free(msg_dup);
			return;
		}
	}

	if(binding->alias)
	{
		argc = binding_expand_alias(binding, src, argc, argv, exp_argv);
		if(!argc)
		{
			if(!command_conf.stealth || (user && user->account))
				reply("Alias for $b%s$b could not be expanded; check log file for details.", binding->name);
			free(msg_dup);
			return;
		}

		argv = exp_argv;

		// Again, try finding a channel name
		if((cmd->flags & CMD_ACCEPT_CHANNEL) && argc > 1 && IsChannelName(argv[1]))
		{
			if((channel = channel_find(argv[1])) == NULL && !(cmd->flags & CMD_LAZY_ACCEPT_CHANNEL))
			{
				if(!command_conf.stealth || (user && user->account))
					reply("You must provide a channel name that exists and is known to the bot.");
				free(msg_dup);
				return;
			}

			argv[1] = argv[0];
			argc--;
			argv++;
		}
	}

	if((cmd->flags & CMD_REQUIRE_CHANNEL) && !channel)
	{
		reply("This command can only be used in channels.");
		free(msg_dup);
		return;
	}

	if(channel)
		channel_arg = channel->name;

	if(!binding_check_access(src, user, channel, channel_arg, binding, 0))
	{
		// Replies are done by binding_check_access() if the user lacks access
		free(msg_dup);
		return;
	}

	// Ignore (first) command argument for argument count check
	if((argc - 1) < cmd->min_argc)
	{
		if(!command_conf.stealth || (user && user->account))
			reply("$b%s$b requires more arguments.", binding->name);
		free(msg_dup);
		return;
	}

	arg_string = untokenize(argc, argv, " ");
	// Call command function and log it if the return value is >0.
	ret = cmd->func(src, user, channel, channel_arg, argc, argv, arg_string);
	free(arg_string);

	if(ret == -1) // Not enough arguments
	{
		if(!command_conf.stealth || (user && user->account))
			reply("$b%s$b requires more arguments.", binding->name);
	}
	else if(ret == 0) // Do not log it
	{
		// Nothing to do...
	}
	else if(ret > 0) // Success, log it
	{
		log_entry = stringbuffer_create();
		stringbuffer_append_char(log_entry, '(');
		stringbuffer_append_string(log_entry, (channel_arg ? channel_arg : ""));
		stringbuffer_append_string(log_entry, ") [");
		if(src->nick)
			stringbuffer_append_string(log_entry, src->nick);
		if(cmd->flags & CMD_LOG_HOSTMASK)
		{
			if(src->ident)
			{
				if(src->nick) // append '!' if we had a nick
					stringbuffer_append_char(log_entry, '!');
				stringbuffer_append_string(log_entry, src->ident);
			}

			if(src->nick || src->ident) // append '@' only if we had a nick or ident
				stringbuffer_append_char(log_entry, '@');

			stringbuffer_append_string(log_entry, src->host); // we assume there is always a host
		}

		if(user && user->account)
		{
			stringbuffer_append_char(log_entry, ':');
			stringbuffer_append_string(log_entry, user->account->name);
		}

		stringbuffer_append_string(log_entry, "]: ");

		if(binding->alias)
		{
			stringbuffer_append_char(log_entry, '(');
			stringbuffer_append_string(log_entry, binding->name);
			stringbuffer_append_string(log_entry, ") ");
			argv[0] = binding->cmd_name;
		}

		// Unsplit args again in case some were updated (e.g. passwords replaced by ****)
		arg_string = untokenize(argc, argv, " ");
		stringbuffer_append_string(log_entry, arg_string);
		free(arg_string);

		log_append(LOG_CMD, "%s", log_entry->string);
		if(command_conf.log_channel)
			irc_send("NOTICE @%s :%s", command_conf.log_channel, log_entry->string);
		stringbuffer_free(log_entry);
	}
	else
	{
		log_append(LOG_WARNING, "Command %s.%s returned unknown value %d", cmd->module->name, cmd->name, ret);
	}

	free(msg_dup);
}

static int binding_expand_alias(struct cmd_binding *binding, struct irc_source *src, unsigned int argc, char **argv, char **exp_argv)
{
	char *alias = binding->alias;
	static char buf[MAXLEN];
	unsigned int buf_used = 0, buf_size = sizeof(buf);
	unsigned int lbound, ubound, lnumlen, unumlen;
	char *num_end = NULL, *num_end_2 = NULL;

	memset(buf, 0, sizeof(buf));
	buf_size -= strlen(argv[0]) + 1; // space for the command so total argv len does not exceed MAXLEN

	while(*alias && buf_used < buf_size)
	{
		if(*alias == '\\') // something escaped
		{
			alias++;
			if(*alias == '\0')
			{
				log_append(LOG_WARNING, "Unexpected \\ at the end of alias for %s: %s", binding->name, binding->alias);
				return 0;
			}

			buf[buf_used++] = *alias++;
		}
		else if(*alias != '$') // regular char
		{
			buf[buf_used++] = *alias++;
		}
		else // *alias == '$' -> alias replacement
		{
			alias++;

			if(*alias == '\0')
			{
				log_append(LOG_WARNING, "Unexpected $ at the end of alias for %s: %s", binding->name, binding->alias);
				return 0;
			}
			else if(!strncasecmp(alias, "nick", 4))
			{
				safestrncpy(buf+buf_used, src->nick, buf_size-buf_used);
				buf_used += strlen(src->nick);
				alias += 4;
			}
			else if(!strncasecmp(alias, "ident", 5))
			{
				safestrncpy(buf+buf_used, src->ident, buf_size-buf_used);
				buf_used += strlen(src->ident);
				alias += 5;
			}
			else if(!strncasecmp(alias, "host", 4))
			{
				safestrncpy(buf+buf_used, src->host, buf_size-buf_used);
				buf_used += strlen(src->host);
				alias += 4;
			}
			else if(ct_isdigit(*alias))
			{
				lbound = strtoul(alias, &num_end, 10);
				if(*num_end == '-') // possibly multiple arguments
				{
					lnumlen = num_end - alias;

					num_end++;
					if(ct_isdigit(*num_end)) // end arg
					{
						ubound = strtoul(num_end, &num_end_2, 10);
						unumlen = num_end_2 - num_end;
					}
					else if(*num_end == '*') // as many args as possible
					{
						ubound = argc - 1;
						unumlen = 1;
					}
					else if(*num_end == '\0') // end of alias and ubound missing
					{
						log_append(LOG_WARNING, "Invalid alias argument '$%d-' in alias for %s: %s", lbound, binding->name, binding->alias);
						return 0;
					}
					else // ubound missing
					{
						log_append(LOG_WARNING, "Invalid alias argument '$%d-%c' in alias for %s: %s", lbound, *num_end, binding->name, binding->alias);
						return 0;
					}

					alias += lnumlen + 1 + unumlen;
				}
				else // only a single argument
				{
					ubound = lbound;
					alias = num_end;
				}

				if(lbound < argc)
				{
					if(ubound >= argc)
						ubound = argc - 1;

					for(unsigned int i = lbound; i <= ubound; i++)
					{
						safestrncpy(buf+buf_used, argv[i], buf_size-buf_used);
						buf_used += strlen(argv[i]);
						if(buf_used < buf_size && i < ubound)
							buf[buf_used++] = ' ';
					}
				}
			}
			else
			{
				log_append(LOG_WARNING, "Invalid alias argument '$%c' in alias for %s: %s", *alias, binding->name, binding->alias);
				return 0;
			}
		}
	}

	debug("Alias:       \"%s\"", binding->alias);
	debug("expanded to: \"%s\"", buf);
	exp_argv[0] = argv[0];
	return tokenize(buf, exp_argv+1, MAXARG-1, ' ', 0) + 1;
}

static int binding_check_access(struct irc_source *src, struct irc_user *user, struct irc_channel *channel, char *channelname, struct cmd_binding *binding, unsigned int quiet)
{
	enum command_rule_result res;
	char *user_mask;

	if(user && !user->account && !(binding->cmd->flags & CMD_IGNORE_LOGINMASK))
	{
		struct dict *accounts = account_dict();
		user_mask = malloc(strlen(src->ident) + strlen(src->host) + 2);
		sprintf(user_mask, "%s@%s", src->ident, src->host);
		dict_iter(node, accounts)
		{
			struct user_account *acc = node->data;
			if(acc->login_mask && !match(acc->login_mask, user_mask))
			{
				account_user_add(acc, user);
				reply("You have been logged into account $b%s$b, because its loginmask matches your current host.", acc->name);
				if(command_conf.log_channel)
					irc_send("PRIVMSG %s :User $b%s$b (%s) has automatically been authed to account $b%s$b, matching loginmask (%s)", command_conf.log_channel, src->nick, user_mask, acc->name, acc->login_mask);
				break;
			}
		}
		free(user_mask);
	}

	if(!user && !(binding->cmd->flags & CMD_ALLOW_UNKNOWN))
	{
		if(!quiet && (!command_conf.stealth || (user && user->account)))
			reply("I can't see you - please join one of my channels to use $b%s$b.", binding->name);
		return 0;
	}

	if((!user || !user->account) && (binding->cmd->flags & CMD_REQUIRE_AUTHED))
	{
		if(!quiet && (!command_conf.stealth || (user && user->account)))
			reply("You must be authed to use $b%s$b.", binding->name);
		return 0;
	}

	if(!binding->comp_rule || !command_rule_executable(binding->comp_rule))
	{
		if(!quiet && (!command_conf.stealth || (user && user->account)))
			reply("Access rule for $b%s$b is incomplete.", binding->name);
		return 0;
	}

	res = command_rule_exec(binding->comp_rule, src, user, channel, channelname);

	// Hack to prevent people from removing their access to important commands.
	// However, it does not prevent them from setting a bad access rule to the auth command.
	if(res == CR_DENY && (binding->cmd->flags & CMD_KEEP_BOUND) && user && user->account && group_has_member("admins", user->account))
	{
		reply("You have no access to $b%s$b but overrode its access rule since you are in the admin group.", binding->name);
		res = CR_ALLOW;
	}

	if(res == CR_DENY)
	{
		if(!quiet && (!command_conf.stealth || (user && user->account)))
			reply("You are not permitted to use $b%s$b.", binding->name);
		return 0;
	}
	else if(res == CR_ERROR)
	{
		if(!quiet && (!command_conf.stealth || (user && user->account)))
			reply("Execution of access rule failed for $b%s$b.", binding->name);
		return 0;
	}

	return 1;
}

static int show_subcmds(struct irc_source *src, struct irc_user *user, const char *prefix, int check_access)
{
	struct stringlist *completions = stringlist_create();
	int count;

	dict_iter(node, binding_list)
	{
		struct cmd_binding *binding = node->data;
		if(binding->module == NULL || binding->cmd == NULL)
			continue;

		if(!strncasecmp(binding->name, prefix, strlen(prefix)) && binding->name[strlen(prefix)] == ' ')
		{
			if(!check_access || binding_check_access(src, user, NULL, NULL, binding, 1))
				stringlist_add(completions, strdup(binding->name));
		}
	}

	if(completions->count && (!command_conf.stealth || (user && user->account)))
	{
		reply("Possible sub-commands of $b%s$b are:", prefix);
		for(unsigned int i = 0; i < completions->count; i++)
			reply("  %s", completions->data[i]);
	}

	count = completions->count;
	stringlist_free(completions);
	return count;
}


static char *make_cmd_key(struct module *module, const char *cmd)
{
	char *name = malloc(strlen(module->name) + 2 + strlen(cmd) + 1);
	sprintf(name, "%s::%s", module->name, cmd);
	return name;
}

static void module_loaded(struct module *module)
{
	dict_iter(node, binding_list)
	{
		struct cmd_binding *binding = node->data;
		if(!strcasecmp(binding->module_name, module->name))
		{
			debug("Enabling binding %s", binding->name);
			binding->module = module;
			binding->cmd = command_find(module, binding->cmd_name);
			assert_continue(binding->cmd);
			if(!binding->comp_rule)
				binding->comp_rule = command_rule_compile(binding->rule ? binding->rule : binding->cmd->rule);
			ptrlist_add(binding->cmd->bindings, 0, binding);
			binding->cmd->bind_count++;
		}
	}

	dict_iter(node, command_list)
	{
		struct command *command = node->data;
		if(command->bind_count == 0 && (command->flags & CMD_KEEP_BOUND))
		{
			log_append(LOG_INFO, "%s.%s has no bindings but CMD_KEEP_BOUND set", command->module->name, command->name);
			if(binding_find(command->name) == NULL)
				binding_add(command->name, command->module->name, command->name, NULL, NULL, 0);
			else
			{
				char *tmp = malloc(strlen(command->module->name) + 1 + strlen(command->name) + 1);
				debug("Binding %s already exists, using %s-%s", command->name, command->module->name, command->name);
				sprintf(tmp, "%s-%s", command->module->name, command->name);
				if(binding_find(tmp) == NULL)
					binding_add(tmp, command->module->name, command->name, NULL, NULL, 0);
				else
					log_append(LOG_WARNING, "Neither %s nor %s were available to bind KEEP_BOUND command %s.%s", command->name, tmp, command->module->name, command->name);
				free(tmp);
			}
		}
	}
}

static void module_unloaded(struct module *module)
{
	dict_iter(node, command_list)
	{
		struct command *command = node->data;
		if(command->module == module)
		{
			debug("Deleting command %s", command->name);
			command_del(command);
		}
	}

	dict_iter(node, binding_list)
	{
		struct cmd_binding *binding = node->data;

		if(binding->module == module)
		{
			debug("Disabling binding %s", binding->name);
			binding->module = NULL;
			binding->cmd = NULL;

			// Free compiled rule if we inherited it from the command
			if(binding->rule == NULL && binding->comp_rule)
			{
				command_rule_free(binding->comp_rule);
				binding->comp_rule = 0;
			}
		}
	}
}


struct command *command_add(struct module *module, const char *name, command_f *func, int min_argc, int flags, const char *rule)
{
	char *key, *pos;
	struct command *command;

	assert_return(module, NULL);
	assert_return(strstr(name, "::") == NULL, NULL);

	// do not allow commands with more than two parts
	if((pos = strchr(name, ' ')))
	{
		pos++;
		assert_return(strchr(pos, ' ') == NULL, NULL);
	}

	key = make_cmd_key(module, name);

	if((command = dict_find(command_list, key)))
	{
		debug("Command %s already exists", key);
		free(key);
		return command;
	}

	if(rule == NULL || !strlen(rule))
	{
		log_append(LOG_WARNING, "Command %s has empty/null rule; use 'true' for a public command", key);
		free(key);
		return NULL;
	}

	debug("Adding command %s.%s", module->name, name);
	command = malloc(sizeof(struct command));
	memset(command, 0, sizeof(struct command));

	command->name     = strdup(name);
	command->module	  = module;
	command->key      = key;
	command->func     = func;
	command->min_argc = min_argc;
	command->flags    = flags;
	command->rule     = strdup(rule);
	command->bindings = ptrlist_create();

	dict_insert(command_list, key, command);
	return command;
}

struct command *command_find(struct module *module, const char *name)
{
	struct command *command;
	char *key;

	if(module == NULL)
		return NULL;

	key = make_cmd_key(module, name);
	command = dict_find(command_list, key);
	free(key);
	return command;

}

static void command_del(struct command *command)
{
	dict_delete(command_list, command->key);
	ptrlist_free(command->bindings);
	free(command->name);
	free(command->rule);
	free(command);
}


struct cmd_binding *binding_add(const char *name, const char *module_name, const char *cmd_name, const char *alias, const char *rule, unsigned char force)
{
	struct cmd_binding *binding;

	if((binding = binding_find(name)))
	{
		log_append(LOG_WARNING, "Binding %s (-> %s.%s) already exists; deleting", binding->name, binding->module_name, binding->cmd_name);
		binding_del(binding);
	}

	debug("Adding binding %s for %s.%s", name, module_name, cmd_name);
	binding = malloc(sizeof(struct cmd_binding));
	memset(binding, 0, sizeof(struct cmd_binding));
	binding->name = strdup(name);
	binding->module_name = strdup(module_name);
	binding->module = module_find(module_name);
	binding->cmd_name = strdup(cmd_name);
	binding->cmd = command_find(binding->module, cmd_name);
	binding->alias = (alias && strlen(alias)) ? strdup(alias) : NULL;

	if(rule)
	{
		binding->rule = strdup(rule);
		binding->comp_rule = command_rule_compile(rule);
	}
	else
	{
		// We only have a rule to parse if the command the binding points to already exists
		binding->rule = NULL;
		binding->comp_rule = binding->cmd ? command_rule_compile(binding->cmd->rule) : 0;
	}

	if(binding->cmd)
	{
		binding->cmd->bind_count++;
		ptrlist_add(binding->cmd->bindings, 0, binding);
	}

	dict_insert(binding_list, binding->name, binding);
	return binding;
}

struct cmd_binding *binding_find(const char *name)
{
	return dict_find(binding_list, name);
}

struct cmd_binding *binding_find_active(const char *name)
{
	struct cmd_binding *binding = dict_find(binding_list, name);

	if(binding == NULL || binding->module == NULL || binding->cmd == NULL)
		return NULL;

	return binding;
}

int binding_set_rule(struct cmd_binding *binding, const char *rule)
{
	// Free old rule
	if(binding->rule)
		free(binding->rule);
	if(binding->comp_rule)
		command_rule_free(binding->comp_rule);

	if(rule && !command_rule_validate(rule))
	{
		log_append(LOG_WARNING, "Could not parse access rule: %s", rule);
		return -1;
	}

	if(rule) // We want to set a new rule
	{
		binding->rule = strdup(rule);
		binding->comp_rule = command_rule_compile(rule);
	}
	else
	{
		// We only have a rule to parse if the command the binding points to already exists
		if(binding->cmd && !command_rule_validate(binding->cmd->rule))
		{
			log_append(LOG_WARNING, "Could not parse access rule from command: %s", binding->cmd->rule);
			return -2;
		}

		binding->rule = NULL;
		binding->comp_rule = binding->cmd ? command_rule_compile(binding->cmd->rule) : 0;
	}

	return 0;
}

void binding_del(struct cmd_binding *binding)
{
	dict_delete(binding_list, binding->name);
	if(binding->cmd)
	{
		binding->cmd->bind_count--;
		ptrlist_del_ptr(binding->cmd->bindings, binding);
	}
	free(binding->name);
	free(binding->module_name);
	free(binding->cmd_name);
	if(binding->alias)
		free(binding->alias);
	if(binding->rule)
		free(binding->rule);
	if(binding->comp_rule)
		command_rule_free(binding->comp_rule);
	free(binding);
}
