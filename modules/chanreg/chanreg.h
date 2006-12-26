#ifndef CHANREG_H
#define CHANREG_H

#include "list.h"
#include "stringlist.h"

struct chanreg;
struct chanreg_user;

DECLARE_LIST(chanreg_user_list, struct chanreg_user *)

// Channel user flags
#define CHANREG_USER_SUSPENDED	0x1

// Chanreg module flags
#define CMOD_STAFF	0x1	// Only staff may enable/disable this module
#define CMOD_HIDDEN	0x2	// Hidden to non-staff

typedef int (cset_validator_f)(struct irc_source *src, const char *value);
typedef const char* (cset_format_f)(const char *value);
typedef void (cmod_enable_f)(struct chanreg *reg);

enum user_levels
{
    UL_PEON	= 100,
    UL_OP	= 200,
    UL_MASTER	= 300,
    UL_COOWNER	= 400,
    UL_OWNER 	= 500
};

struct chanreg
{
	char *channel;
	unsigned int active : 1;
	const char *last_error;

	time_t registered;
	char *registrar;

	struct chanreg_user_list *users;
	struct dict *settings;

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
	cmod_enable_f *enable_func;
	cmod_enable_f *disable_func;
};

struct chanreg_module_setting
{
	char *name;
	char *default_value;
	cset_validator_f *validator;
	cset_format_f *formatter;
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
#define CHANREG_MODULE_COMMAND								\
	CHANREG_COMMAND									\
	else if(stringlist_find(reg->active_modules, cmod->name) == -1)			\
	{										\
		reply("Module $b%s$b is not enabled for this channel.", cmod->name);	\
		return 0;								\
	}

extern unsigned int chanreg_staff_rule;
#define IsStaff()	(chanreg_staff_rule && command_rule_exec(chanreg_staff_rule, src, user, channel, channelname) == CR_ALLOW)


struct chanreg *chanreg_find(const char *channel);
struct chanreg_user *chanreg_user_find(struct chanreg *reg, const char *accountname);

void chanreg_setting_set(struct chanreg *reg, struct chanreg_module *cmod, const char *setting, const char *value);
const char *chanreg_setting_get(struct chanreg *reg, struct chanreg_module *cmod, const char *setting);
int chanreg_setting_get_int(struct chanreg *reg, struct chanreg_module *cmod, const char *setting);

struct chanreg_module *chanreg_module_reg(const char *name, unsigned int flags, cmod_enable_f *enable_func, cmod_enable_f *disable_func);
void chanreg_module_unreg(struct chanreg_module *cmod);
struct chanreg_module_setting *chanreg_module_setting_reg(struct chanreg_module *cmod, const char *name, const char *default_value, cset_validator_f *validator, cset_format_f *formatter);

#endif
