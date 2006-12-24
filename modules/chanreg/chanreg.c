#include "global.h"
#include "module.h"
#include "chanreg.h"
#include "modules/commands/commands.h"
#include "modules/commands/command_rule.h"
#include "modules/help/help.h"
#include "modules/chanjoin/chanjoin.h"
#include "chanuser.h"
#include "account.h"
#include "database.h"
#include "irc.h"
#include "irc_handler.h"
#include "stringlist.h"
#include "conf.h"
#include "table.h"

MODULE_DEPENDS("commands", "help", "chanjoin", NULL);

static struct
{
	struct stringlist *default_modules;
	char *staff_rule;
} chanreg_conf;

IMPLEMENT_LIST(chanreg_user_list, struct chanreg_user *)

PARSER_FUNC(chanuser);
PARSER_FUNC(privchan);
COMMAND(cregister);
COMMAND(cunregister);
COMMAND(stats_chanreg);
COMMAND(adduser);
COMMAND(deluser);
COMMAND(clvl);
COMMAND(giveownership);
COMMAND(suspend);
COMMAND(unsuspend);
COMMAND(access);
COMMAND(users);
static void chanreg_conf_reload();
static void chanreg_db_read(struct database *db);
static int chanreg_db_write(struct database *db);
static int sort_channels(const void *a_, const void *b_);
static int sort_channel_users(const void *a_, const void *b_);
static struct chanreg *chanreg_add(const char *channel);
static struct chanreg *chanreg_find(const char *channel);
static void chanreg_free(struct chanreg *reg);
static struct chanreg_user *chanreg_user_add(struct chanreg *reg, const char *accountname, unsigned short level);
static struct chanreg_user *chanreg_user_find(struct chanreg *reg, const char *accountname);
static void chanreg_user_del(struct chanreg *reg, struct chanreg_user *c_user);
static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason);

static struct module *this;
static struct database *chanreg_db = NULL;
static struct dict *chanregs;
unsigned int chanreg_staff_rule = 0;

MODULE_INIT
{
	this = self;

	chanregs = dict_create();
	dict_set_free_funcs(chanregs, NULL, (dict_free_f *)chanreg_free);

	REG_COMMAND_RULE("chanuser", chanuser);
	REG_COMMAND_RULE("privchan", privchan);

	help_load(self, "chanreg.help");
	DEFINE_COMMAND(self, "cregister",	cregister,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "cunregister",	cunregister,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "stats chanreg",	stats_chanreg,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "adduser",		adduser,	3, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "deluser",		deluser,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "clvl",		clvl,		3, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "giveownership",	giveownership,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "suspend",		suspend,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "unsuspend",	unsuspend,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "access",		access,		1, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(self, "users",		users,		1, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");

	reg_conf_reload_func(chanreg_conf_reload);
	chanreg_conf_reload();

	chanreg_db = database_create("chanreg", chanreg_db_read, chanreg_db_write);
	database_read(chanreg_db, 1);
	database_set_write_interval(chanreg_db, 300);
}

MODULE_FINI
{
	database_write(chanreg_db);
	database_delete(chanreg_db);

	unreg_conf_reload_func(chanreg_conf_reload);

	command_rule_unreg("chanuser");
	command_rule_unreg("privchan");

	dict_free(chanregs);

	if(chanreg_staff_rule)
		command_rule_free(chanreg_staff_rule);
}

static void chanreg_conf_reload()
{
	chanreg_conf.default_modules = conf_get("chanreg/default_modules", DB_STRINGLIST);
	chanreg_conf.staff_rule = conf_get("chanreg/staff_rule", DB_STRING);

	if(chanreg_staff_rule)
		command_rule_free(chanreg_staff_rule);
	chanreg_staff_rule = chanreg_conf.staff_rule ? command_rule_compile(chanreg_conf.staff_rule) : 0;
}


static void chanreg_db_read(struct database *db)
{
	struct dict *db_node;

	if((db_node = database_fetch(db->nodes, "chanregs", DB_OBJECT)))
	{
		dict_iter(rec, db_node)
		{
			struct dict *obj = ((struct db_node *)rec->data)->data.object;
			struct dict *users;
			struct chanreg *reg;
			const char *channel = rec->key;

			reg = chanreg_add(channel);
			if((users = database_fetch(obj, "users", DB_OBJECT)))
			{
				dict_iter(rec, users)
				{
					struct dict *obj = ((struct db_node *)rec->data)->data.object;
					const char *account = rec->key;
					const char *level_str = database_fetch(obj, "level", DB_STRING);
					const char *flags_str = database_fetch(obj, "flags", DB_STRING);
					unsigned short level;

					if(level_str && (level = strtoul(level_str, NULL, 10)) && level <= UL_OWNER)
					{
						struct chanreg_user *c_user = chanreg_user_add(reg, account, level);
						c_user->flags = flags_str ? strtoul(flags_str, NULL, 10) : 0;
					}
				}
			}
		}
	}
}

static int chanreg_db_write(struct database *db)
{
	database_begin_object(db, "chanregs");
		dict_iter(node, chanregs)
		{
			struct chanreg *reg = node->data;

			database_begin_object(db, reg->channel);
				database_begin_object(db, "users");
					for(int j = 0; j < reg->users->count; j++)
					{
						struct chanreg_user *c_user = reg->users->data[j];
						database_begin_object(db, c_user->account->name);
							database_write_long(db, "level", c_user->level);
							database_write_long(db, "flags", c_user->flags);
						database_end_object(db);
					}
				database_end_object(db);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

static struct chanreg *chanreg_add(const char *channel)
{
	struct chanreg *reg = malloc(sizeof(struct chanreg));
	memset(reg, 0, sizeof(struct chanreg));

	reg->channel = strdup(channel);
	reg->last_error = "No Error";
	reg->users = chanreg_user_list_create();
	chanjoin_addchan(channel, this, NULL, cj_success, cj_error, reg);

	dict_insert(chanregs, reg->channel, reg);
	return reg;
}

static struct chanreg *chanreg_find(const char *channel)
{
	return dict_find(chanregs, channel);
}

static void chanreg_free(struct chanreg *reg)
{
	if(reg->active && !reloading_module)
		chanjoin_delchan(reg->channel, this, NULL);

	for(int i = 0; i < reg->users->count; i++)
		free(reg->users->data[i]);
	chanreg_user_list_free(reg->users);

	free(reg->channel);
	free(reg);
}

static struct chanreg_user *chanreg_user_add(struct chanreg *reg, const char *accountname, unsigned short level)
{
	struct chanreg_user *c_user;
	struct user_account *account;

	if(!(account = account_find(accountname)))
		return NULL;

	c_user = malloc(sizeof(struct chanreg_user));
	memset(c_user, 0, sizeof(struct chanreg_user));

	c_user->reg = reg;
	c_user->account = account;
	c_user->level = level;
	chanreg_user_list_add(reg->users, c_user);

	return c_user;
}

static void chanreg_user_del(struct chanreg *reg, struct chanreg_user *c_user)
{
	chanreg_user_list_del(reg->users, c_user);
	free(c_user);
}

static struct chanreg_user *chanreg_user_find(struct chanreg *reg, const char *accountname)
{
	for(int i = 0; i < reg->users->count; i++)
	{
		struct chanreg_user *c_user = reg->users->data[i];
		if(!strcasecmp(c_user->account->name, accountname))
			return c_user;
	}

	return NULL;
}

static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time)
{
	struct chanreg *reg = ctx;
	reg->active = 1;
	reg->last_error = "No Error";
}

static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason)
{
	struct chanreg *reg = ctx;
	reg->active = 0;
	reg->last_error = reason;
}


PARSER_FUNC(chanuser)
{
	struct command_rule_context *cr_ctx = ctx;
	struct chanreg *reg;
	struct chanreg_user *c_user;
	unsigned int min_level;

	min_level = arg ? atoi(arg) : 0;

	if(!cr_ctx->channelname)
		return RET_TRUE;

	// Not registered -> return true since the command function is supposed to use
	// the CHANREG_COMMAND; macro which aborts if the channel is not registered.
	if(!(reg = chanreg_find(cr_ctx->channelname)))
		return RET_TRUE;

	if(cr_ctx->user->account && (c_user = chanreg_user_find(reg, cr_ctx->user->account->name)) &&
	   c_user->level >= min_level && !(c_user->flags & CHANREG_USER_SUSPENDED))
		return RET_TRUE;

	return RET_FALSE;
}

PARSER_FUNC(privchan)
{
	struct command_rule_context *cr_ctx = ctx;

	if(!cr_ctx->channel)
		return RET_TRUE;

	if(cr_ctx->channel->modes & (MODE_KEYED | MODE_INVITEONLY | MODE_SECRET))
		return RET_TRUE;

	return RET_FALSE;
}


COMMAND(cregister)
{
	struct chanreg *reg;
	struct user_account *account;

	if(!channelname)
	{
		reply("You must provide a valid channel name.");
		return 0;
	}

	if((reg = chanreg_find(channelname)))
	{
		reply("$b%s$b is already registered.", reg->channel);
		return 0;
	}

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	reg = chanreg_add(channelname);
	chanreg_user_add(reg, account->name, UL_OWNER);
	reply("Channel $b%s$b registered to $b%s$b.", channelname, argv[1]);
	return 1;
}

COMMAND(cunregister)
{
	CHANREG_COMMAND;

	reply("$b%s$b has been unregistered.", channelname);
	dict_delete(chanregs, channelname);

	return 1;
}

COMMAND(stats_chanreg)
{
	struct table *table;
	unsigned int i = 0;

	table = table_create(2, dict_size(chanregs));
	table_set_header(table, "Channel", "State");

	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;

		table->data[i][0] = reg->channel;
		table->data[i][1] = reg->active ? "Active" : reg->last_error;
		i++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_channels);
	table_send(table, src->nick);
	table_free(table);

	return 1;
}

COMMAND(adduser)
{
	struct user_account *account;
	struct chanreg_user *actor, *victim;
	unsigned short level;

	CHANREG_COMMAND;

	if(!(level = strtoul(argv[2], NULL, 10)) || level > UL_OWNER)
	{
		reply("$b%s$b is an invalid access level.", argv[2]);
		return 0;
	}

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if((actor = chanreg_user_find(reg, user->account->name)) && actor->level <= level && !IsStaff())
	{
		reply("You cannot give users access greater than or equal to your own.");
		return 0;
	}

	if((victim = chanreg_user_find(reg, account->name)))
	{
		reply("$b%s$b is already on the $b%s$b user list with $b%d$b access.", account->name, channelname, victim->level);
		return 0;
	}

	chanreg_user_add(reg, account->name, level);
	reply("Added $b%s$b to the $b%s$b user list with $b%d$b access.", account->name, channelname, level);
	return 1;
}

COMMAND(deluser)
{
	struct user_account *account;
	struct chanreg_user *actor, *victim;

	CHANREG_COMMAND;

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(account == user->account && !IsStaff())
	{
		reply("You cannot delete your own access.");
		return 0;
	}

	if(!(victim = chanreg_user_find(reg, account->name)))
	{
		reply("$b%s$b lacks access to $b%s$b.", account->name, channelname);
		return 0;
	}

	if((actor = chanreg_user_find(reg, user->account->name)) && actor->level <= victim->level && !IsStaff())
	{
		reply("$b%s$b outranks you.", account->name);
		return 0;
	}

	reply("Removed $b%s$b (with $b%d$b access) from the $b%s$b user list.", account->name, victim->level, channelname);
	chanreg_user_del(reg, victim);
	return 1;
}

COMMAND(clvl)
{
	struct user_account *account;
	struct chanreg_user *actor, *victim;
	unsigned short level;

	CHANREG_COMMAND;

	if(!(level = strtoul(argv[2], NULL, 10)) || level > UL_OWNER)
	{
		reply("$b%s$b is an invalid access level.", argv[2]);
		return 0;
	}

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(account == user->account && !IsStaff())
	{
		reply("You cannot change your own access.");
		return 0;
	}

	if(!(victim = chanreg_user_find(reg, account->name)))
	{
		reply("$b%s$b lacks access to $b%s$b.", account->name, channelname);
		return 0;
	}

	actor = chanreg_user_find(reg, user->account->name);
	if(actor && actor->level <= level && !IsStaff())
	{
		reply("You cannot give users access greater than or equal to your own.");
		return 0;
	}

	if(actor && actor->level <= victim->level && !IsStaff())
	{
		reply("$b%s$b outranks you.", account->name);
		return 0;
	}

	victim->level = level;
	reply("$b%s$b now has $b%d$b access in $b%s$b.", account->name, level, channelname);
	return 1;
}

COMMAND(giveownership)
{
	struct user_account *account;
	struct chanreg_user *victim, *current_owner = NULL;
	unsigned int owner_count = 0;

	CHANREG_COMMAND;

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(account == user->account)
	{
		reply("You cannot transfer ownership to yourself.");
		return 0;
	}

	for(int i = 0; i < reg->users->count; i++)
	{
		if(reg->users->data[i]->level == UL_OWNER)
		{
			current_owner = reg->users->data[i];
			owner_count++;
		}
	}

	if(owner_count > 1)
	{
		reply("You cannot use giveownership in a channel with multiple owners.");
		return 0;
	}

	if(!(victim = chanreg_user_find(reg, account->name)))
	{
		if(argc > 2 && !strcasecmp(argv[2], "force") && IsStaff())
			victim = chanreg_user_add(reg, account->name, UL_COOWNER);
		else
		{
			reply("$b%s$b lacks access to $b%s$b.", account->name, channelname);
			return 0;
		}
	}

	if(current_owner)
		current_owner->level = ((victim->level > UL_COOWNER) ? victim->level : UL_COOWNER);
	victim->level = UL_OWNER;

	reply("Ownership of $b%s$b has been transferred to $b%s$b.", channelname, account->name);
	return 1;
}

COMMAND(suspend)
{
	struct user_account *account;
	struct chanreg_user *actor, *victim;

	CHANREG_COMMAND;

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(account == user->account && !IsStaff())
	{
		reply("You cannot suspend yourself.");
		return 0;
	}

	if(!(victim = chanreg_user_find(reg, account->name)))
	{
		reply("$b%s$b lacks access to $b%s$b.", account->name, channelname);
		return 0;
	}

	if((actor = chanreg_user_find(reg, user->account->name)) && actor->level <= victim->level && !IsStaff())
	{
		reply("$b%s$b outranks you.", account->name);
		return 0;
	}

	if(victim->flags & CHANREG_USER_SUSPENDED)
	{
		reply("$b%s$b is already suspended.", account->name);
		return 0;
	}

	victim->flags |= CHANREG_USER_SUSPENDED;
	reply("$b%s$b's access to $b%s$b has been suspended.", account->name, channelname);
	return 1;
}

COMMAND(unsuspend)
{
	struct user_account *account;
	struct chanreg_user *actor, *victim;

	CHANREG_COMMAND;

	if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(!(victim = chanreg_user_find(reg, account->name)))
	{
		reply("$b%s$b lacks access to $b%s$b.", account->name, channelname);
		return 0;
	}

	if((actor = chanreg_user_find(reg, user->account->name)) && actor->level <= victim->level && !IsStaff())
	{
		reply("$b%s$b outranks you.", account->name);
		return 0;
	}

	if(!(victim->flags & CHANREG_USER_SUSPENDED))
	{
		reply("$b%s$b is not suspended.", account->name);
		return 0;
	}

	victim->flags &= ~CHANREG_USER_SUSPENDED;
	reply("$b%s$b's access to $b%s$b has been restored.", account->name, channelname);
	return 1;
}

COMMAND(access)
{
	struct user_account *account = NULL;
	struct chanreg_user *c_user;
	const char *target;
	char prefix[MAXLEN];

	CHANREG_COMMAND;

	if(argc < 2) // No nick|*account -> return own information
		account = user->account;
	else if(!(account = account_find_smart(src, argv[1])))
		return 0;

	if(!account)
	{
		reply("$b%s$b is not authed.", ((argc < 2) ? user->nick : argv[1]));
		return 1;
	}

	if(argc < 2)
	{
		snprintf(prefix, sizeof(prefix), "$b%s$b (%s)", user->nick, account->name);
		target = user->nick;
	}
	else if(*argv[1] == '*')
	{
		snprintf(prefix, sizeof(prefix), "$b%s$b", account->name);
		target = account->name;
	}
	else
	{
		snprintf(prefix, sizeof(prefix), "$b%s$b (%s)", argv[1], account->name);
		target = argv[1];
	}

	if(!(c_user = chanreg_user_find(reg, account->name)))
	{
		reply("%s lacks access to $b%s$b.", prefix, channelname);
		return 1;
	}

	reply("%s has $b%d$b access in $b%s$b.", prefix, c_user->level, channelname);
	if(c_user->flags & CHANREG_USER_SUSPENDED)
		reply("$b%s$b's access has been suspended.", target);
	return 1;
}

COMMAND(users)
{
	struct table *table;
	unsigned int i = 0;

	CHANREG_COMMAND;

	table = table_create(3, reg->users->count);
	table_set_header(table, "Access", "Account", "Status");

	for(int j = 0; j < reg->users->count; j++)
	{
		struct chanreg_user *c_user = reg->users->data[j];

		table->data[i][0] = strtab(c_user->level);
		table->data[i][1] = c_user->account->name;
		table->data[i][2] = ((c_user->flags & CHANREG_USER_SUSPENDED) ? "Suspended" : "Normal");
		i++;
	}

	qsort(table->data, table->rows, sizeof(table->data[0]), sort_channel_users);
	reply("$b%s$b users:", channelname);
	table_send(table, src->nick);
	table_free(table);

	return 1;
}

static int sort_channels(const void *a_, const void *b_)
{
	const char *name_a = (*((const char ***)a_))[0];
	const char *name_b = (*((const char ***)b_))[0];
	return strcasecmp(name_a, name_b);
}

static int sort_channel_users(const void *a_, const void *b_)
{
	unsigned int lvl_a = atoi((*((const char ***)a_))[0]);
	unsigned int lvl_b = atoi((*((const char ***)b_))[0]);
	if(lvl_a > lvl_b)
		return -1;
	else if(lvl_a < lvl_b)
		return 1;
	else
		return 0;
}
