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
#include "conf.h"
#include "table.h"
#include "stringlist.h"
#include "timer.h"

MODULE_DEPENDS("commands", "help", "chanjoin", NULL);

static struct
{
	struct stringlist *default_modules;
	char *staff_rule;
} chanreg_conf;

IMPLEMENT_LIST(chanreg_list, struct chanreg *);
IMPLEMENT_LIST(chanreg_user_list, struct chanreg_user *);
IMPLEMENT_HOOKABLE(chanreg_add);
IMPLEMENT_HOOKABLE(chanreg_del);

PARSER_FUNC(chanuser);
PARSER_FUNC(privchan);
COMMAND(cregister);
COMMAND(cunregister);
COMMAND(stats_chanreg);
COMMAND(adduser);
COMMAND(deluser);
COMMAND(clvl);
COMMAND(giveownership);
COMMAND(deleteme);
COMMAND(suspend);
COMMAND(unsuspend);
COMMAND(access);
COMMAND(users);
COMMAND(cset);
COMMAND(cinfo);
COMMAND(cmod_list);
COMMAND(cmod_info);
COMMAND(cmod_enable);
COMMAND(cmod_disable);
COMMAND(rejoin);
COMMAND(move);
COMMAND(global);
static void chanreg_conf_reload();
static void chanreg_db_read(struct database *db);
static int chanreg_db_write(struct database *db);
static int sort_channels(const void *a_, const void *b_);
static int sort_channel_users(const void *a_, const void *b_);
static void chanreg_free(struct chanreg *reg);
static void _chanreg_setting_set(struct chanreg *reg, const char *module_name, const char *setting, const char *value);
static int chanreg_module_enable(struct chanreg *reg, struct chanreg_module *cmod, enum cmod_enable_reason reason);
static void chanreg_module_reg_enable_timer_func(void *bound, struct chanreg_module *cmod);
static void chanreg_module_setting_free(struct chanreg_module_setting *cset);
static void cj_success(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
static void cj_error(struct cj_channel *chan, const char *key, void *ctx, const char *reason);
static void chanreg_join(struct chanreg *creg);
static void chanreg_account_del(struct user_account *account);

static struct module *this;
static struct database *chanreg_db = NULL;
static struct dict *chanregs, *chanreg_modules;
unsigned int chanreg_staff_rule = 0;

MODULE_INIT
{
	this = self;

	chanregs = dict_create();
	chanreg_modules = dict_create();
	dict_set_free_funcs(chanregs, NULL, (dict_free_f *)chanreg_free);

	REG_COMMAND_RULE("chanuser", chanuser);
	REG_COMMAND_RULE("privchan", privchan);

	help_load(self, "chanreg.help");
	DEFINE_COMMAND(self, "cregister",	cregister,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "cunregister",	cunregister,	0, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "stats chanreg",	stats_chanreg,	0, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "adduser",		adduser,	2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "deluser",		deluser,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "clvl",		clvl,		2, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "giveownership",	giveownership,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "deleteme",	deleteme,	0, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "true");
	DEFINE_COMMAND(self, "suspend",		suspend,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "unsuspend",	unsuspend,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "access",		access,		0, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(self, "users",		users,		0, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(self, "cset",		cset,		0, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(300) || group(admins)");
	DEFINE_COMMAND(self, "cinfo",		cinfo,		0, CMD_LAZY_ACCEPT_CHANNEL, "chanuser() || inchannel() || !privchan() || group(admins)");
	DEFINE_COMMAND(self, "cmod list",	cmod_list,	0, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "cmod info",	cmod_info,	1, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "cmod enable",	cmod_enable,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "cmod disable",	cmod_disable,	1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL, "chanuser(500) || group(admins)");
	DEFINE_COMMAND(self, "rejoin",		rejoin,		0, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "move",		move,		2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "global",		global,		1, CMD_REQUIRE_AUTHED | CMD_LAZY_ACCEPT_CHANNEL | CMD_LOG_HOSTMASK, "group(admins)");

	reg_account_del_hook(chanreg_account_del);
	reg_conf_reload_func(chanreg_conf_reload);
	chanreg_conf_reload();

	chanreg_db = database_create("chanreg", chanreg_db_read, chanreg_db_write);
	database_read(chanreg_db, 1);
	database_set_write_interval(chanreg_db, 300);
}

MODULE_FINI
{
	timer_del_boundname(this, "chanreg_module_reg_enable");

	database_write(chanreg_db);
	database_delete(chanreg_db);

	unreg_conf_reload_func(chanreg_conf_reload);
	unreg_account_del_hook(chanreg_account_del);

	command_rule_unreg("chanuser");
	command_rule_unreg("privchan");

	dict_free(chanreg_modules);
	dict_free(chanregs);

	clear_chanreg_add_hooks();
	clear_chanreg_del_hooks();

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
			struct dict *dict;
			struct stringlist *slist;
			char *str;
			struct chanreg *reg;
			const char *channel = rec->key;

			slist = database_fetch(obj, "modules", DB_STRINGLIST);
			reg = chanreg_add(channel, slist);

			if((str = database_fetch(obj, "registered", DB_STRING)))
				reg->registered = strtoul(str, NULL, 10);

			if((str = database_fetch(obj, "registrar", DB_STRING)))
				reg->registrar = strdup(str);

			if((dict = database_fetch(obj, "users", DB_OBJECT)))
			{
				dict_iter(rec, dict)
				{
					struct dict *obj = ((struct db_node *)rec->data)->data.object;
					const char *account = rec->key;
					const char *level_str = database_fetch(obj, "level", DB_STRING);
					const char *flags_str = database_fetch(obj, "flags", DB_STRING);
					unsigned short level;

					if(level_str && (level = strtoul(level_str, NULL, 10)) && level <= UL_OWNER)
					{
						struct chanreg_user *c_user = chanreg_user_add(reg, account, level);
						if(!c_user)
						{
							log_append(LOG_WARNING, "Found non-existent user %s in %s (with %u access)", account, channel, level);
							continue;
						}

						c_user->flags = flags_str ? strtoul(flags_str, NULL, 10) : 0;
					}
				}
			}

			if((dict = database_fetch(obj, "settings", DB_OBJECT)))
			{
				dict_iter(rec, dict)
				{
					struct dict *obj = ((struct db_node *)rec->data)->data.object;
					const char *module = rec->key;

					dict_iter(rec, obj)
					{
						struct db_node *node = rec->data;
						if(node->type != DB_STRING)
						{
							log_append(LOG_WARNING, "Unexpected setting type for channel %s (module %s): %d; expected DB_STRING", reg->channel, module, node->type);
							continue;
						}

						_chanreg_setting_set(reg, module, rec->key, node->data.string);
					}
				}
			}

			if((dict = database_fetch(obj, "data", DB_OBJECT)))
			{
				dict_iter(rec, dict)
				{
					struct dict *obj = ((struct db_node *)rec->data)->data.object;
					dict_insert(reg->db_data, strdup(rec->key), database_copy_object(obj));
				}
			}
		}
	}
}

static int chanreg_db_write(struct database *db)
{
	// Call write functions of active channel modules.
	// During shutdown this list is empty so modules need to call
	// chanreg_module_writedb() manually in their fini function.
	dict_iter(node, chanreg_modules)
	{
		struct chanreg_module *cmod = node->data;
		if(cmod->db_write)
			chanreg_module_writedb(cmod);
	}

	database_begin_object(db, "chanregs");
		dict_iter(node, chanregs)
		{
			struct chanreg *reg = node->data;

			database_begin_object(db, reg->channel);
				database_write_stringlist(db, "modules", reg->modules);
				database_write_long(db, "registered", reg->registered);
				if(reg->registrar)
					database_write_string(db, "registrar", reg->registrar);

				database_begin_object(db, "users");
					for(unsigned int j = 0; j < reg->users->count; j++)
					{
						struct chanreg_user *c_user = reg->users->data[j];
						database_begin_object(db, c_user->account->name);
							database_write_long(db, "level", c_user->level);
							database_write_long(db, "flags", c_user->flags);
						database_end_object(db);
					}
				database_end_object(db);

				database_begin_object(db, "settings");
					dict_iter(node, reg->settings)
					{
						struct dict *module_settings = node->data;
						database_begin_object(db, node->key);
							dict_iter(node, module_settings)
							{
								database_write_string(db, node->key, node->data);
							}
						database_end_object(db);
					}
				database_end_object(db);

				database_begin_object(db, "data");
					for(unsigned int i = 0; i < reg->modules->count; i++)
					{
						struct dict *object = dict_find(reg->db_data, reg->modules->data[i]);
						if(object)
							database_write_object(db, reg->modules->data[i], object);
					}

					dict_iter(node, reg->db_data)
					{
						struct dict *object = node->data;
						if(stringlist_find(reg->modules, node->key) == -1)
							database_write_object(db, node->key, object);
					}
				database_end_object(db);
			database_end_object(db);
		}
	database_end_object(db);
	return 0;
}

struct dict *chanreg_dict()
{
	return chanregs;
}

struct chanreg *chanreg_add(const char *channel, const struct stringlist *modules)
{
	struct chanreg *reg = malloc(sizeof(struct chanreg));
	memset(reg, 0, sizeof(struct chanreg));

	reg->channel = strdup(channel);
	reg->registered = time(NULL);
	reg->registrar = NULL;
	reg->last_error = "No Error";
	reg->users = chanreg_user_list_create();
	reg->settings = dict_create();
	reg->db_data = dict_create();
	reg->modules = stringlist_copy(modules ? modules : chanreg_conf.default_modules);
	reg->active_modules = stringlist_create();

	dict_set_free_funcs(reg->settings, free, (dict_free_f *)dict_free);
	dict_set_free_funcs(reg->db_data, free, (dict_free_f *)dict_free);

	chanreg_join(reg);
	dict_insert(chanregs, reg->channel, reg);

	CALL_HOOKS(chanreg_add, (reg));
	return reg;
}

struct chanreg *chanreg_find(const char *channel)
{
	return dict_find(chanregs, channel);
}

struct chanreg_list *chanreg_get_access_channels(struct user_account *account, unsigned short min_access, unsigned int check_staff)
{
	struct irc_user user;
	struct chanreg_list *list = chanreg_list_create();

	memset(&user, 0, sizeof(struct irc_user));
	user.nick = "*webinterface*";
	user.info = "";
	user.channels = NULL;
	user.account = account;

	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;
		struct irc_channel *channel;
		unsigned int found = 0;

		for(unsigned int i = 0; i < reg->users->count; i++)
		{
			if(reg->users->data[i]->account != account)
				continue;
			if(reg->users->data[i]->level >= min_access)
			{
				found = 1;
				break;
			}
		}

		if(found)
		{
			chanreg_list_add(list, reg);
			continue;
		}

		debug("checking staff rule for %s", reg->channel);
		if(check_staff && chanreg_staff_rule && (channel = channel_find(reg->channel)) &&
		   command_rule_exec(chanreg_staff_rule, NULL, &user, channel, channel->name) == CR_ALLOW)
			chanreg_list_add(list, reg);
	}

	return list;
}

unsigned int chanreg_check_access(struct chanreg *reg, struct user_account *account, unsigned short min_access, unsigned int check_staff)
{
	struct irc_user user;
	struct irc_channel *channel;

	memset(&user, 0, sizeof(struct irc_user));
	user.nick = "*webinterface*";
	user.info = "";
	user.channels = NULL;
	user.account = account;

	channel = channel_find(reg->channel);
	for(unsigned int i = 0; i < reg->users->count; i++)
	{
		if(reg->users->data[i]->account == account && reg->users->data[i]->level >= min_access)
			return 1;
	}

	if(check_staff && chanreg_staff_rule && channel && command_rule_exec(chanreg_staff_rule, NULL, &user, channel, channel->name) == CR_ALLOW)
		return 1;

	return 0;
}

static void chanreg_free(struct chanreg *reg)
{
	CALL_HOOKS(chanreg_del, (reg));

	if(!reloading_module)
		chanjoin_delchan(reg->channel, this, NULL);

	for(unsigned int i = 0; i < reg->users->count; i++)
		free(reg->users->data[i]);
	chanreg_user_list_free(reg->users);

	dict_free(reg->settings);
	dict_free(reg->db_data);
	stringlist_free(reg->modules);
	stringlist_free(reg->active_modules);
	free(reg->registrar);
	free(reg->channel);
	free(reg);
}

struct chanreg_user *chanreg_user_add(struct chanreg *reg, const char *accountname, unsigned short level)
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

void chanreg_user_del(struct chanreg *reg, struct chanreg_user *c_user)
{
	chanreg_user_list_del(reg->users, c_user);
	free(c_user);
}

struct chanreg_user *chanreg_user_find(struct chanreg *reg, const char *accountname)
{
	for(unsigned int i = 0; i < reg->users->count; i++)
	{
		struct chanreg_user *c_user = reg->users->data[i];
		if(!strcasecmp(c_user->account->name, accountname))
			return c_user;
	}

	return NULL;
}

static void _chanreg_setting_set(struct chanreg *reg, const char *module_name, const char *setting, const char *value)
{
	struct dict *module_settings;
	if(!(module_settings = dict_find(reg->settings, module_name)))
	{
		module_settings = dict_create();
		dict_set_free_funcs(module_settings, free, free);
		dict_insert(reg->settings, strdup(module_name), module_settings);
	}

	dict_delete(module_settings, setting);
	if(value)
		dict_insert(module_settings, strdup(setting), strdup(value));
}

void chanreg_setting_set(struct chanreg *reg, struct chanreg_module *cmod, const char *setting, const char *value)
{
	_chanreg_setting_set(reg, cmod->name, setting, value);
}

const char *chanreg_setting_get(struct chanreg *reg, struct chanreg_module *cmod, const char *setting)
{
	struct dict *module_settings;
	struct chanreg_module_setting *cset;
	const char *value;

	if((module_settings = dict_find(reg->settings, cmod->name)) && (value = dict_find(module_settings, setting)))
		return value;

	if((cset = dict_find(cmod->settings, setting)))
		return cset->default_value;

	return NULL;
}

int chanreg_setting_get_int(struct chanreg *reg, struct chanreg_module *cmod, const char *setting)
{
	const char *value = chanreg_setting_get(reg, cmod, setting);
	if(!value)
		return 0;
	return atoi(value);
}

int chanreg_setting_get_bool(struct chanreg *reg, struct chanreg_module *cmod, const char *setting)
{
	const char *value = chanreg_setting_get(reg, cmod, setting);
	return (value && true_string(value));
}

struct chanreg_module *chanreg_module_reg(const char *name, unsigned int flags, cmod_db_read_f *db_read, cmod_db_write_f *db_write, cmod_enable_f *enable_func, cmod_disable_f *disable_func, cmod_move_f *move_func)
{
	struct chanreg_module *cmod = malloc(sizeof(struct chanreg_module));
	memset(cmod, 0, sizeof(struct chanreg_module));

	cmod->name = strdup(name);
	cmod->settings = dict_create();
	cmod->flags = flags;
	cmod->db_read = db_read;
	cmod->db_write = db_write;
	cmod->enable_func = enable_func;
	cmod->disable_func = disable_func;
	cmod->move_func = move_func;
	cmod->channels = chanreg_list_create();

	dict_set_free_funcs(cmod->settings, NULL, (dict_free_f *)chanreg_module_setting_free);
	dict_insert(chanreg_modules, cmod->name, cmod);

	// To be able to use the chanreg_module inside the enable-function, this must be postponed to after this function
	timer_add(this, "chanreg_module_reg_enable", now, (timer_f*)chanreg_module_reg_enable_timer_func, cmod, 0, 0);

	log_append(LOG_INFO, "Registered channel module: %s", name);
	return cmod;
}

void chanreg_module_reg_enable_timer_func(void *bound, struct chanreg_module *cmod)
{
	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;
		if(stringlist_find(reg->modules, cmod->name) != -1)
		{
			stringlist_add(reg->active_modules, strdup(cmod->name));
			chanreg_list_add(cmod->channels, reg);
			if(cmod->enable_func)
				cmod->enable_func(reg, CER_REG);
		}
	}
}

void chanreg_module_unreg(struct chanreg_module *cmod)
{
	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;
		int idx;
		if((idx = stringlist_find(reg->active_modules, cmod->name)) != -1)
			stringlist_del(reg->active_modules, idx);
	}

	dict_delete(chanreg_modules, cmod->name);
	chanreg_list_free(cmod->channels);
	dict_free(cmod->settings);
	free(cmod->name);
	free(cmod);
}

void chanreg_module_readdb(struct chanreg_module *cmod)
{
	assert(cmod->db_read);
	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;
		struct dict *db_nodes = dict_find(reg->db_data, cmod->name);
		if(db_nodes)
			cmod->db_read(db_nodes, reg);
	}
}

void chanreg_module_writedb(struct chanreg_module *cmod)
{
	struct database_object *dbo;

	assert(cmod->db_write);
	for(unsigned int i = 0; i < cmod->channels->count; i++)
	{
		struct chanreg *reg = cmod->channels->data[i];

		dbo = database_obj_create();
		if(cmod->db_write(dbo, reg))
		{
			dict_free(dbo->current);
			database_obj_free(dbo);
			continue;
		}

		dict_delete(reg->db_data, cmod->name);
		dict_insert(reg->db_data, strdup(cmod->name), dbo->current);
		database_obj_free(dbo);
	}
}

struct chanreg_module *chanreg_module_find(const char *name)
{
	return dict_find(chanreg_modules, name);
}

static int chanreg_module_enable(struct chanreg *reg, struct chanreg_module *cmod, enum cmod_enable_reason reason)
{
	assert_return(stringlist_find(reg->modules, cmod->name) == -1, -1);
	assert_return(stringlist_find(reg->active_modules, cmod->name) == -1, -1);

	if(cmod->enable_func && cmod->enable_func(reg, reason))
		return 1;

	stringlist_add(reg->modules, strdup(cmod->name));
	stringlist_add(reg->active_modules, strdup(cmod->name));

	chanreg_list_add(cmod->channels, reg);

	return 0;
}

int chanreg_module_disable(struct chanreg *reg, struct chanreg_module *cmod, unsigned int delete_data, enum cmod_disable_reason reason)
{
	int idx, ret = 0;

	if((idx = stringlist_find(reg->modules, cmod->name)) != -1)
		stringlist_del(reg->modules, idx);

	if((idx = stringlist_find(reg->active_modules, cmod->name)) != -1)
		stringlist_del(reg->active_modules, idx);

	if(cmod->disable_func)
		ret = cmod->disable_func(reg, delete_data, reason);

	chanreg_list_del(cmod->channels, reg);

	if(delete_data)
	{
		dict_delete(reg->settings, cmod->name);
		dict_delete(reg->db_data, cmod->name);
	}
	else if(cmod->db_write)
	{
		// If we don't delete the module's data, we must write them since the timed writes won't write
		// data from disabled modules.
		struct database_object *dbo = database_obj_create();
		if(cmod->db_write(dbo, reg))
			dict_free(dbo->current);
		else
		{
			dict_delete(reg->db_data, cmod->name);
			dict_insert(reg->db_data, strdup(cmod->name), dbo->current);
		}

		database_obj_free(dbo);
	}

	return ret;
}


struct chanreg_module_setting *chanreg_module_setting_reg(struct chanreg_module *cmod, const char *name, const char *default_value, cset_validator_f *validator, cset_format_f *formatter, cset_encode_f *encoder)
{
	struct chanreg_module_setting *cset = malloc(sizeof(struct chanreg_module_setting));

	memset(cset, 0, sizeof(struct chanreg_module_setting));
	cset->name = strdup(name);
	cset->default_value = default_value ? strdup(default_value) : NULL;
	cset->validator = validator;
	cset->formatter = formatter;
	cset->encoder = encoder;

	dict_insert(cmod->settings, cset->name, cset);
	return cset;
}

static void chanreg_module_setting_free(struct chanreg_module_setting *cset)
{
	free(cset->name);
	free(cset->default_value);
	free(cset);
}

unsigned int chanreg_module_active(struct chanreg_module *cmod, const char *channel)
{
	struct chanreg *reg = chanreg_find(channel);

	if(!channel)
		return 0;

	for(unsigned int i = 0; i < cmod->channels->count; i++)
	{
		if(cmod->channels->data[i] == reg)
			return 1;
	}

	return 0;
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

static void chanreg_join(struct chanreg *creg)
{
	chanjoin_addchan(creg->channel, this, NULL, cj_success, cj_error, creg, NULL, 0);
}

static void chanreg_account_del(struct user_account *account)
{
	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;
		for(unsigned int i = 0; i < reg->users->count; i++)
		{
			struct chanreg_user *c_user = reg->users->data[i];
			if(c_user->account == account)
			{
				chanreg_user_del(reg, c_user);
				break;
			}
		}
	}
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

	reg = chanreg_add(channelname, NULL);
	reg->registrar = strdup(user->account->name);
	chanreg_user_add(reg, account->name, UL_OWNER);
	reply("Channel $b%s$b registered to $b%s$b.", channelname, argv[1]);
	return 1;
}

COMMAND(cunregister)
{
	CHANREG_COMMAND;

	dict_iter(node, reg->db_data)
	{
		struct chanreg_module *cmod = chanreg_module_find(node->key);
		if(cmod)
			chanreg_module_disable(reg, cmod, 1, CDR_UNREG);
	}

	dict_delete(chanregs, channelname);
	reply("$b%s$b has been unregistered.", channelname);
	return 1;
}

COMMAND(stats_chanreg)
{
	struct table *table;
	unsigned int i = 0;
	char *mask = NULL;

	table = table_create(3, dict_size(chanregs));
	table_set_header(table, "Channel", "State", "Registrar");

	if(argc > 1)
		mask = argv[1];

	dict_iter(node, chanregs)
	{
		struct chanreg *reg = node->data;

		if(mask && match(mask, reg->channel))
			continue;

		table->data[i][0] = reg->channel;
		table_col_str(table, i, 1, reg->active ? "Active" : (char*)reg->last_error);
		table->data[i][2] = reg->registrar ? reg->registrar : "-";
		i++;
	}

	table->rows = i;
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

	for(unsigned int i = 0; i < reg->users->count; i++)
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

COMMAND(deleteme)
{
	struct chanreg_user *victim;

	CHANREG_COMMAND;

	if(!(victim = chanreg_user_find(reg, user->account->name)))
	{
		reply("You lack access to $b%s$b.", channelname);
		return 0;
	}

	if(victim->level == UL_OWNER)
	{
		reply("You cannot delete your owner access in $b%s$b.", channelname);
		return 0;
	}

	reply("Your $b%d$b access has been deleted from $b%s$b.", victim->level, channelname);
	chanreg_user_del(reg, victim);
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
	table->col_flags[0] |= TABLE_CELL_FREE;

	for(unsigned int j = 0; j < reg->users->count; j++)
	{
		struct chanreg_user *c_user = reg->users->data[j];

		table_col_num(table, i, 0, c_user->level);
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

COMMAND(cset)
{
	char *name_dup, *modname, *setting, *new_value;
	const char *value;
	struct chanreg_module *cmod = NULL;
	struct chanreg_module_setting *cset;

	CHANREG_COMMAND;

	if(argc < 2)
	{
		struct table *table;
		unsigned int row, header_sent = 0;
		struct stringlist *free_strings;

		stringlist_sort(reg->active_modules);
		for(unsigned int i = 0; i < reg->active_modules->count; i++)
		{
			struct chanreg_module *cmod = chanreg_module_find(reg->active_modules->data[i]);
			assert_continue(cmod);

			if(!cmod->settings->count || ((cmod->flags & CMOD_HIDDEN) && !IsStaff()))
				continue;

			table = table_create(2, cmod->settings->count);
			table->col_flags[1] |= TABLE_CELL_BOLD;
			table->prefix = strdup("  ");

			row = 0;
			free_strings = stringlist_create();
			dict_iter(node, cmod->settings)
			{
				struct chanreg_module_setting *cset = node->data;
				char *str = malloc(strlen(cset->name) + 2); // $b + name + :\0
				sprintf(str, "%s:", cset->name);
				stringlist_add(free_strings, str);

				value = chanreg_setting_get(reg, cmod, cset->name);
				if(cset->formatter)
					value = cset->formatter(reg, value);

				table->data[row][0] = str;
				table_col_str(table, row, 1, (char*)value);
				row++;
			}

			if(!header_sent)
			{
				reply("$b%s$b settings:", channelname);
				header_sent = 1;
			}

			reply("$u%s:$u", cmod->name);
			table_send(table, src->nick);
			table_free(table);
			stringlist_free(free_strings);
		}

		if(!header_sent)
			reply("$b%s$b has no settings.", channelname);

		return 1;
	}

	name_dup = strdup(argv[1]);
	if((setting = strchr(name_dup, '.')))
	{
		*setting++ = '\0'; // Split at '.' and point setting to char after '.'
		modname = name_dup;
	}
	else
	{
		setting = name_dup;
		modname = NULL;
	}

	if(!strlen(setting) || (modname && !strlen(modname)))
	{
		reply("$b%s$b does not look like a valid setting; use either $bmodule.setting$b or $bsetting$b", argv[1]);
		free(name_dup);
		return 0;
	}

	if(modname && !(cmod = chanreg_module_find(modname))) // Check if module exists.
	{
		reply("$b%s$b is not a valid module.", modname);
		free(name_dup);
		return 0;
	}
	else if(!modname) // Check if setting is not ambiguous.
	{
		for(unsigned int i = 0; i < reg->active_modules->count; i++)
		{
			struct chanreg_module *cmod_tmp = chanreg_module_find(reg->active_modules->data[i]);
			assert_continue(cmod_tmp);
			if(dict_find(cmod_tmp->settings, setting))
			{
				// This setting already exists in another visible module.
				if(cmod && (!(cmod->flags & CMOD_HIDDEN) || IsStaff()))
				{
					reply("$b%s$b is ambiguous; please use $b<module>.%s$b", setting, setting);
					free(name_dup);
					return 0;
				}

				cmod = cmod_tmp;
			}
		}
	}

	if(cmod && (cmod->flags & CMOD_HIDDEN) && !IsStaff())
	{
		reply("$b%s$b is not a valid module.", cmod->name);
		free(name_dup);
		return 0;
	}

	// User provided a module name -> check if the module is active for the channel.
	if(modname && stringlist_find(reg->active_modules, cmod->name) == -1)
	{
		reply("Module $b%s$b is not enabled in $b%s$b", modname, channelname);
		free(name_dup);
		return 0;
	}

	// No module name provided and no active module with specified setting -OR-
	// Module name provided but setting does not exist in this module
	if((!modname && !cmod) || (modname && !dict_find(cmod->settings, setting)))
	{
		reply("$b%s$b is not a valid setting.", argv[1]);
		free(name_dup);
		return 0;
	}

	assert_return(setting && cmod, 0);
	cset = dict_find(cmod->settings, setting);

	// User wants to change setting
	if(argc > 2)
	{
		new_value = untokenize(argc - 2, argv + 2, " ");
		if(!cset->validator || cset->validator(reg, src, new_value))
		{
			if(cset->encoder)
				chanreg_setting_set(reg, cmod, setting, cset->encoder(reg, chanreg_setting_get(reg, cmod, cset->name), new_value));
			else
				chanreg_setting_set(reg, cmod, setting, new_value);
		}
		free(new_value);
	}

	// Display current (or new) value
	value = chanreg_setting_get(reg, cmod, cset->name);
	if(cset->formatter)
		value = cset->formatter(reg, value);
	reply("$b%s.%s:$b  %s", cmod->name, cset->name, value);

	free(name_dup);
	return 1;
}

COMMAND(cinfo)
{
	struct stringlist *slist;
	char *str;

	CHANREG_COMMAND;

	reply("Information about $b%s$b:", channelname);
	for(unsigned int i = 0; i < reg->users->count; i++)
	{
		struct chanreg_user *c_user = reg->users->data[i];
		if(c_user->level == UL_OWNER)
			reply("$bOwner:      $b %s", c_user->account->name);
	}

	reply("$bUser Count: $b %d", reg->users->count);

	if(reg->active_modules->count)
	{
		slist = stringlist_create();
		for(unsigned int i = 0; i < reg->active_modules->count; i++)
		{
			struct chanreg_module *cmod = chanreg_module_find(reg->active_modules->data[i]);
			if(!(cmod->flags & CMOD_HIDDEN) || IsStaff())
				stringlist_add(slist, strdup(cmod->name));
		}


		if(slist->count)
		{
			stringlist_sort(slist);
			str = untokenize(slist->count, slist->data, ", ");
			reply("$bModules:    $b %s", str);
			free(str);
		}

		stringlist_free(slist);
	}

	if(reg->registrar)
		reply("$bRegistrar:  $b %s", reg->registrar);
	reply("$bRegistered: $b %s ago", duration2string(now - reg->registered));

	return 0;
}

COMMAND(cmod_list)
{
	unsigned int staff = IsStaff();
	struct stringlist *modules;
	char buf[MAXLEN];
	unsigned int pos = 0;

	CHANREG_COMMAND;

	modules = stringlist_create();
	dict_iter(node, chanreg_modules)
	{
		struct chanreg_module *cmod = node->data;
		if((cmod->flags & CMOD_HIDDEN) && !staff)
			continue;

		pos = 0;
		if(stringlist_find(reg->modules, cmod->name) == -1) // Disabled
			pos += snprintf(buf + pos, MAXLEN - pos, "%s", cmod->name);
		else // Enabled
			pos += snprintf(buf + pos, MAXLEN - pos, "$b%s$b", cmod->name);

		if(cmod->flags & CMOD_STAFF)
		{
			safestrncpy(buf + pos, " (staff)", MAXLEN - pos);
			pos += strlen(" (staff)");
		}

		if(cmod->flags & CMOD_HIDDEN)
		{
			safestrncpy(buf + pos, " (hidden)", MAXLEN - pos);
			pos += strlen(" (hidden)");
		}

		buf[pos] = '\0';

		stringlist_add(modules, strdup(buf));
	}

	if(modules->count)
	{
		reply("Available modules ($bbold$b modules are loaded):");
		stringlist_sort(modules);
		for(unsigned int i = 0; i < modules->count; i++)
			reply("  %s", modules->data[i]);
	}
	else
	{
		reply("No modules available.");
	}

	stringlist_free(modules);
	return 1;
}

COMMAND(cmod_info)
{
	struct chanreg_module *cmod;
	struct stringlist *slist, *channels;

	if(!(cmod = chanreg_module_find(argv[1])))
	{
		reply("A module named $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(!cmod->channels->count)
		reply("The module $b%s$b is not activated in any channels.", argv[1]);
	else
	{
		slist = stringlist_create();
		for(unsigned int i = 0; i < cmod->channels->count; i++)
			stringlist_add(slist, strdup(cmod->channels->data[i]->channel));
		stringlist_sort(slist);
		channels = stringlist_to_irclines(src->nick, slist);
		reply("The module $b%s$b is activated in the following channel%s:", argv[1], channels->count == 1 ? "" : "s");
		for(unsigned int i = 0; i < channels->count; i++)
			reply("  %s", channels->data[i]);

		stringlist_free(slist);
		stringlist_free(channels);
	}
	return 1;
}

COMMAND(cmod_enable)
{
	struct chanreg_module *cmod;

	CHANREG_COMMAND;

	if(!(cmod = chanreg_module_find(argv[1])) || ((cmod->flags & CMOD_HIDDEN) && !IsStaff()))
	{
		reply("A module named $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(stringlist_find(reg->modules, cmod->name) != -1)
	{
		reply("Module $b%s$b is already enabled in $b%s$b.", cmod->name, channelname);
		return 0;
	}

	if((cmod->flags & CMOD_STAFF) && !IsStaff())
	{
		reply("Only staff may enable module $b%s$b.", cmod->name);
		return 0;
	}

	if(!chanreg_module_enable(reg, cmod, CER_ENABLED))
		reply("Module $b%s$b has been enabled in $b%s$b.", cmod->name, channelname);
	else
		reply("An error occurred while attempting to load module %s.", cmod->name);
	return 1;
}

COMMAND(cmod_disable)
{
	struct chanreg_module *cmod;
	unsigned int delete_data = 0;
	int ret;

	CHANREG_COMMAND;

	if(!(cmod = chanreg_module_find(argv[1])) || ((cmod->flags & CMOD_HIDDEN) && !IsStaff()))
	{
		reply("A module named $b%s$b does not exist.", argv[1]);
		return 0;
	}

	if(stringlist_find(reg->modules, cmod->name) == -1)
	{
		reply("Module $b%s$b is not enabled in $b%s$b.", cmod->name, channelname);
		return 0;
	}

	if((cmod->flags & CMOD_STAFF) && !IsStaff())
	{
		reply("Only staff may disable module $b%s$b.", cmod->name);
		return 0;
	}

	if(argc > 2 && !strcasecmp(argv[2], "purge"))
		delete_data = 1;

	ret = chanreg_module_disable(reg, cmod, delete_data, CDR_DISABLED);
	reply("Module $b%s$b has been disabled in $b%s$b.", cmod->name, channelname);
	if(delete_data)
	{
		if(!ret)
			reply("All settings/data from this module have been deleted.");
		else
		{
			reply("All module-settings were deleted, some data could not be removed.");
			log_append(LOG_WARNING, "Some data from module %s could not be deleted.", cmod->name);
		}
	}
	return 1;
}

COMMAND(rejoin)
{
	struct chanreg *creg;
	unsigned int count = 0;

	if(argc > 1)
	{
		if(!IsChannelName(argv[1]))
		{
			reply("$b%s$b is no valid channel name.", argv[1]);
			return 0;
		}

		else if(!(creg = chanreg_find(argv[1])))
		{
			reply("$b%s$b is not registered.", argv[1]);
			return 0;
		}

		else if(creg->active)
		{
			reply("We are already in $b%s$b, no need to rejoin.", argv[1]);
			return 0;
		}

		chanreg_join(creg);
		reply("Attempting to rejoin $b%s$b (Last error: %s)", creg->channel, creg->last_error ? creg->last_error : "None");
	}
	else
	{
		struct stringlist *slist = stringlist_create();
		dict_iter(node, chanregs)
		{
			creg = node->data;
			if(creg->active)
				continue;

			stringlist_add(slist, strdup(creg->channel));
			chanreg_join(creg);
			count++;
		}
		if(count)
		{
			if(count == 1)
				reply("Attempting to rejoin $b%s$b.", slist->data[0]);
			else
			{
				struct stringlist *irclist = stringlist_to_irclines(src->nick, slist);
				reply("Attempting to rejoin the following $b%d$b channels:", count);
				for(unsigned int i = 0; i < irclist->count; i++)
					reply("  %s", irclist->data[i]);

				stringlist_free(irclist);
			}
		}
		else
			reply("There is no need to rejoin any channels at this very moment.");

		stringlist_free(slist);
	}

	return count ? 1 : 0;
}

COMMAND(move)
{
	struct chanreg *creg;
	if(!IsChannelName(argv[1]))
		reply("$b%s$b is no valid channel name.", argv[1]);

	else if(!IsChannelName(argv[2]))
		reply("$b%s$b is no valid channel name.", argv[2]);

	else if(!(creg = chanreg_find(argv[1])))
		reply("$b%s$b is not registered.", argv[1]);

	else if(chanreg_find(argv[2]))
		reply("$b%s$b is already registered.", argv[2]);

	else
	{
		struct dict_node *node = dict_find_node(chanregs, creg->channel);
		char *from = creg->channel;
		chanjoin_delchan(from, this, NULL);
		creg->channel = strdup(argv[2]);
		node->key = creg->channel;
		chanreg_join(creg);

		for(unsigned int i = 0; i < creg->active_modules->count; i++)
		{
			struct chanreg_module *cmod = chanreg_module_find(creg->active_modules->data[i]);
			if(cmod->move_func)
				cmod->move_func(creg, from, creg->channel);
		}

		free(from);
		reply("Channel registration has been moved to $b%s$b.", creg->channel);
		return 1;
	}

	return 0;
}

COMMAND(global)
{
	dict_iter(node, chanregs)
	{
		struct chanreg *dat = node->data;
		if(dat->active)
			irc_send("PRIVMSG %s :[$bGLOBAL$b from $b%s$b] %s", node->key, src->nick, argline + (argv[1] - argv[0]));
	}
	return 1;
}
// Validators

int boolean_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	return (true_string(value) || false_string(value));
}

int access_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	const char *str = value;
	int number;

	if(!aredigits(str))
	{
		reply("The access level has to be a positive integral value.");
		return 0;
	}

	while(*str == '0')
		str++;

	if(!(number = strlen(str)))
		return 1;

	if(number > 3 || ((number = atoi(str)) > 500))
	{
		reply("The access level must not exceed 500.");
		return 0;
	}

	return 1;
}

int reply_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	if(!strcasecmp(value, "NOTICE") || !strcasecmp(value, "PRIVMSG"))
		return 1;

	reply("The reply method can be either $bNOTICE$b or $bPRIVMSG$b.");
	return 0;
}


// Formatters

const char *null_none(struct chanreg *reg, const char *value)
{
	return value ? value : "None";
}

const char *boolean_formatter_onoff(struct chanreg *reg, const char *value)
{
	return (value && true_string(value)) ? "On" : "Off";
}

// Encoders

const char *asterisk_null(struct chanreg *reg, const char *old_value, const char *value)
{
	if(!strcmp(value, "*"))
		return NULL;
	return value;
}

const char *access_encoder(struct chanreg *reg, const char *old_Value, const char *value)
{
	const char *str = value;

	while(*str == '0')
		str++;

	if(!strlen(str))
		return "0";

	return str;
}

const char *boolean_encoder(struct chanreg *reg, const char *old_value, const char *value)
{
	return true_string(value) ? "1" : "0";
}
