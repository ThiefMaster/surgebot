#ifndef CHANREG_H
#define CHANREG_H

#include "hook.h"
#include "list.h"
#include "stringlist.h"
#include "database.h"

struct chanreg;
struct chanreg_user;

DECLARE_LIST(chanreg_list, struct chanreg *)
DECLARE_LIST(chanreg_user_list, struct chanreg_user *)

// Channel user flags
#define CHANREG_USER_SUSPENDED	0x1

// Chanreg module flags
#define CMOD_STAFF	0x1	// Only staff may enable/disable this module
#define CMOD_HIDDEN	0x2	// Hidden to non-staff

enum user_levels
{
	UL_PEON		= 100,
	UL_OP		= 200,
	UL_MASTER	= 300,
	UL_COOWNER	= 400,
	UL_OWNER 	= 500
};

enum cmod_enable_reason
{
	CER_REG		= 1,
	CER_ENABLED	= 2
};

enum cmod_disable_reason
{
	CDR_UNREG	= 1,
	CDR_DISABLED	= 2
};

typedef int (cset_validator_f)(struct chanreg *reg, struct irc_source *src, const char *value);
typedef const char* (cset_format_f)(struct chanreg *reg, const char *value);
typedef const char* (cset_encode_f)(struct chanreg *reg, const char *old_value, const char *value);
typedef int (cmod_enable_f)(struct chanreg *reg, enum cmod_enable_reason reason);
typedef int (cmod_disable_f)(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
typedef void (cmod_move_f)(struct chanreg *reg, const char *from, const char *to);
typedef void (cmod_db_read_f)(struct dict *db_nodes, struct chanreg *reg);
typedef int (cmod_db_write_f)(struct database_object *dbo, struct chanreg *reg);

struct chanreg
{
	char *channel;
	unsigned int active : 1;
	const char *last_error;

	time_t registered;
	char *registrar;

	struct chanreg_user_list *users;
	struct dict *settings;
	struct dict *db_data;

	struct stringlist *modules;
	struct stringlist *active_modules;
};

struct chanreg_user
{
	struct chanreg *reg;
	struct user_account *account;
	unsigned short level;
	unsigned int flags;
};

struct chanreg_module
{
	char *name;
	struct dict *settings;
	unsigned int flags;
	cmod_db_read_f *db_read;
	cmod_db_write_f *db_write;
	cmod_enable_f *enable_func;
	cmod_disable_f *disable_func;
	cmod_move_f *move_func;
	struct chanreg_list *channels;
};

struct chanreg_module_setting
{
	char *name;
	char *default_value;
	cset_validator_f *validator;
	cset_format_f *formatter;
	cset_encode_f *encoder;
};

// Regular chanreg commands just need to check if the channel is registered.
#define CHANREG_COMMAND								\
	struct chanreg *reg;							\
	if(!channelname)							\
	{									\
		reply("You must provide the name of a channel that exists.");	\
		return 0;							\
	}									\
	else if(!(reg = chanreg_find(channelname)))				\
	{									\
		reply("$b%s$b is not registered.", channelname);		\
		return 0;							\
	}

// Chanreg module commands also need to check if the module is enabled for the channel.
#define CHANREG_MODULE_COMMAND(CMOD)							\
	CHANREG_COMMAND									\
	else if(stringlist_find(reg->active_modules, (CMOD)->name) == -1)		\
	{										\
		reply("Module $b%s$b is not enabled for this channel.", (CMOD)->name);	\
		return 0;								\
	}

extern unsigned int chanreg_staff_rule;
#define IsStaff()	(chanreg_staff_rule && command_rule_exec(chanreg_staff_rule, src, user, channel, channelname) == CR_ALLOW)

struct chanreg *chanreg_add(const char *channel, const struct stringlist *modules);
struct chanreg *chanreg_find(const char *channel);
struct chanreg_user *chanreg_user_add(struct chanreg *reg, const char *accountname, unsigned short level);
void chanreg_user_del(struct chanreg *reg, struct chanreg_user *c_user);
struct chanreg_user *chanreg_user_find(struct chanreg *reg, const char *accountname);
struct chanreg_list *chanreg_get_access_channels(struct user_account *account, unsigned short min_access, unsigned int check_staff);
unsigned int chanreg_check_access(struct chanreg *reg, struct user_account *account, unsigned short min_access, unsigned int check_staff);

void chanreg_setting_set(struct chanreg *reg, struct chanreg_module *cmod, const char *setting, const char *value);
const char *chanreg_setting_get(struct chanreg *reg, struct chanreg_module *cmod, const char *setting);
int chanreg_setting_get_int(struct chanreg *reg, struct chanreg_module *cmod, const char *setting);

struct chanreg_module *chanreg_module_reg(const char *name, unsigned int flags, cmod_db_read_f *db_read, cmod_db_write_f *db_write, cmod_enable_f *enable_func, cmod_disable_f *disable_func, cmod_move_f *move_func);
void chanreg_module_unreg(struct chanreg_module *cmod);
void chanreg_module_readdb(struct chanreg_module *cmod);
void chanreg_module_writedb(struct chanreg_module *cmod);
struct chanreg_module *chanreg_module_find(const char *name);
struct chanreg_module_setting *chanreg_module_setting_reg(struct chanreg_module *cmod, const char *name, const char *default_value, cset_validator_f *validator, cset_format_f *formatter, cset_encode_f *encoder);
unsigned int chanreg_module_active(struct chanreg_module *cmod, const char *channel);

int chanreg_module_disable(struct chanreg *reg, struct chanreg_module *cmod, unsigned int delete_data, enum cmod_disable_reason reason);

// Default validators for module settings
int boolean_validator(struct chanreg *reg, struct irc_source *src, const char *value);
int access_validator(struct chanreg *reg, struct irc_source *src, const char *value);

// Default formatters
const char *null_none(struct chanreg *reg, const char *value);

// Default encoders
const char *asterisk_null(struct chanreg *reg, const char *old_value, const char *value);
const char *access_encoder(struct chanreg *reg, const char *old_Value, const char *value);

DECLARE_HOOKABLE(chanreg_del, (struct chanreg *reg));
DECLARE_HOOKABLE(chanreg_add, (struct chanreg *reg));

#endif
