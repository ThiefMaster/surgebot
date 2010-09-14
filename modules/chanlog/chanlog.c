#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/tools/tools.h"
#include "irc.h"
#include "irc_handler.h"
#include "table.h"
#include "chanuser.h"
#include "conf.h"
#include "timer.h"

#define CHANLOG_ACTIVE if(argc < 2 || !chanreg_module_active(cmod, argv[1])) return

MODULE_DEPENDS("tools", "commands", "chanreg", NULL);

struct chanlog {
	char *target;
	FILE *fd;
};

static struct {
	const char *directory;
} chanlog_conf;

static struct dict *chanlogs;
static struct chanreg_module *cmod;

static void chanlog_init();
static void chanlog_fini();

static FILE *fd_find(const char *);
static int chanlog_add(const char *);
static void chanlog_del(const char *);
static void chanlog_readconf();
static void chanlog_timer_add();
static void chanlog_timer_del();
static int chanlog_purge(const char *target);
static void chanlog_rotate();

static void chanuser_del_hook(struct irc_chanuser *user, unsigned int del_type, const char *reason);
static void chanlog_timer(void *bound, void *data);
static void chanlog_chmod(const char *target, unsigned int mode);

static void chanlog(const char *target, const char *format, ...);
static void chanlog_free(struct chanlog *);
static int cmod_enabled(struct chanreg *, enum cmod_enable_reason);
static int cmod_disabled(struct chanreg *, unsigned int, enum cmod_disable_reason);

int cset_purgeafter_validator(struct chanreg *reg, struct irc_source *src, const char *value);
const char *cset_purgeafter_formatter(struct chanreg *reg, const char *value);

int cset_logfilemode_validator(struct chanreg *reg, struct irc_source *src, const char *value);
const char *cset_logfilemode_encoder(struct chanreg *reg, const char *old_value, const char *value);

IRC_HANDLER(join);
IRC_HANDLER(kick);
IRC_HANDLER(mode);
IRC_HANDLER(nick);
IRC_HANDLER(notice);
IRC_HANDLER(privmsg);
IRC_HANDLER(topic);

MODULE_INIT
{
	cmod = chanreg_module_reg("Chanlog", CMOD_STAFF | CMOD_HIDDEN, NULL, NULL, cmod_enabled, cmod_disabled, NULL);
	chanreg_module_setting_reg(cmod, "PurgeAfter", "7", cset_purgeafter_validator, cset_purgeafter_formatter, NULL);
	chanreg_module_setting_reg(cmod, "LogFileMode", "600", cset_logfilemode_validator, NULL, cset_logfilemode_encoder);

	reg_irc_handler("JOIN", join);
	reg_irc_handler("KICK", kick);
	reg_irc_handler("MODE", mode);
	reg_irc_handler("NICK", nick);
	reg_irc_handler("NOTICE", notice);
	reg_irc_handler("PRIVMSG", privmsg);
	reg_irc_handler("TOPIC", topic);
	reg_conf_reload_func(chanlog_readconf);
	reg_chanuser_del_hook(chanuser_del_hook);
	chanlogs = dict_create();
	dict_set_free_funcs(chanlogs, NULL, (dict_free_f*)chanlog_free);

	chanlog_readconf();
	chanlog_init();
}

MODULE_FINI
{
	chanlog_fini();

	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("KICK", kick);
	unreg_irc_handler("MODE", mode);
	unreg_irc_handler("NICK", nick);
	unreg_irc_handler("NOTICE", notice);
	unreg_irc_handler("PRIVMSG", privmsg);
	unreg_irc_handler("TOPIC", topic);
	unreg_conf_reload_func(chanlog_readconf);
	unreg_chanuser_del_hook(chanuser_del_hook);
	chanlog_timer_del();

	chanreg_module_unreg(cmod);
	dict_free(chanlogs);
}

static void chanlog_init()
{
	struct irc_user *me;
	assert((me = user_find(bot.nickname)));

	dict_iter(node, me->channels)
	{
		if(chanreg_module_active(cmod, node->key))
			chanlog_add(node->key);
	}
}

static void chanlog_fini()
{
	dict_iter(node, chanlogs)
		chanlog_del(((struct chanlog *)node->data)->target);
}

static FILE *fd_find(const char *target)
{
	if(!target)
		return NULL;

	struct chanlog *clog = dict_find(chanlogs, target);
	if(!clog)
	{
		debug("Could not find associated fd for target '%s'", target);
		return NULL;
	}
	return clog->fd;
}

static int chanlog_add(const char *target)
{
	char dirname[512], *str;
	const char *szMode;
	struct tm *timeinfo;
	struct chanlog *item;
	FILE *fd;
	int len;
	struct chanreg *reg;
	unsigned int mode = 0600;

	if(!target)
		return -1;

	if(!chanlog_conf.directory)
	{
		debug("Configuration for module chanlog is not complete, missing key 'chanlog/directory'");
		return -1;
	}
	else if(!IsChannelName(target))
	{
		debug("Got a target in chanlog_add() which is no valid channel (%s)", target);
		return -1;
	}

	assert_return((reg = chanreg_find(target)), -1);

	if((szMode = chanreg_setting_get(reg, cmod, "LogFileMode")))
		mode = ((szMode[0] - '0') << 6) | ((szMode[1] - '0') << 3) | (szMode[2] - '0');

	snprintf(dirname, sizeof(dirname), "%s", chanlog_conf.directory);
	strtolower(dirname);
	if(mkdir(dirname, mode | 0700) && errno != EEXIST)
	{
		debug("Could not create directory '%s' in chanlog-module, disabling module", chanlog_conf.directory);
		return -1;
	}

	timeinfo = localtime(&now);

	len = strlen(dirname);
	snprintf(dirname + len, sizeof(dirname) - len, "/%s", target);
	strtolower(dirname + len);
	if(mkdir(dirname, mode | 0700) && errno != EEXIST)
	{
		debug("Could not create directory '%s' in chanlog-module, disabling module", dirname);
		return -1;
	}

	len = strlen(dirname);
	snprintf(dirname + len, sizeof(dirname) - len, "/%d-%02d-%02d.log", timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);

	strtolower(dirname + len);
	// Try to open file for reading
	fd = fopen(dirname, "a");
	if(!fd)
	{
		// File doesn't seem to exist, opening it for writing will create it
		fd = fopen(dirname, "w");
		if(!fd)
		{
			debug("Could not open/create logfile '%s' in chanlog-module, disabling module", dirname);
			chanreg_module_disable(reg, cmod, 0, CDR_DISABLED);
			return -1;
		}
	}

	// Make sure the correct filemode was set
	if(chmod(dirname, mode))
		log_append(LOG_ERROR, "Chanlog: Could not set provided filemode %o on: %s", mode, dirname);

	item = malloc(sizeof(struct chanlog));
	memset(item, 0, sizeof(struct chanlog));
	item->target = strdup(target);
	item->fd = fd;
	dict_insert(chanlogs, item->target, item);

	str = asctime(timeinfo);
	str[strlen(str) - 1] = '\0';

	fprintf(fd, "Session Start: %s\n", str);
	fflush(fd);

	return 0;
}

static void chanlog_del(const char *target)
{
	FILE *fd;
	struct tm *timeinfo;
	char *str;

	assert((fd = fd_find(target)));
	timeinfo = localtime(&now);
	str = asctime(timeinfo);
	str[strlen(str) - 1] = '\0';
	fprintf(fd, "Session Close: %s\n\n", str);
	dict_delete(chanlogs, target);
}

static void chanlog_free(struct chanlog *clog)
{
	fclose(clog->fd);
	free(clog->target);
	free(clog);
}

static void chanlog_readconf()
{
	chanlog_conf.directory = conf_get("chanlog/directory", DB_STRING);
	if(!chanlog_conf.directory)
	{
		debug("Could not read path 'chanlog/directory' from configuration file, please complete your configuration.");
		return;
	}

	chanlog_timer_add();
}

static void chanlog_timer_add()
{
	chanlog_timer_del();

	struct tm *timeinfo = localtime(&now);
	time_t timestamp;

	timeinfo->tm_hour = 0;
	timeinfo->tm_min = 0;
	timeinfo->tm_sec = 0;
	timeinfo->tm_mday++;

	assert((timestamp = mktime(timeinfo)) > 0);
	timer_add(NULL, "chanlog_timer", timestamp, chanlog_timer, NULL, 0, 0);
}

static void chanlog_timer_del()
{
	timer_del_boundname(NULL, "chanlog_timer");
}

static int chanlog_purge(const char *target)
{
	DIR *dir, *subdir;
	char cwd[PATH_MAX], filedate[11] = {0};
	const char *str;
	struct dirent *direntry;
	struct stat attribut;
	int day, month, year, purge_after, count = 0;
	time_t date, today;
	struct tm *timeinfo;
	struct chanreg *creg;

	if(!chanlog_conf.directory)
		return -1;

	if(!(dir = opendir(chanlog_conf.directory)))
		return -2;

	today = time(NULL);
	timeinfo = localtime(&today);
	timeinfo->tm_hour = timeinfo->tm_min = timeinfo->tm_sec = 0;
	today = mktime(timeinfo);
	if(today == -1)
		return -3;

	while((direntry = readdir(dir)))
	{
		if(!IsChannelName(direntry->d_name) || (target && strcasecmp(target, direntry->d_name)))
			continue;

		// Only check channel if it needs log purging at all
		creg = chanreg_find(direntry->d_name);
		if(!creg)
			continue;

		str = chanreg_setting_get(creg, cmod, "PurgeAfter");
		if(!str || !(purge_after = atoi(str)))
			continue;

		snprintf(cwd, sizeof(cwd), "%s/%s", chanlog_conf.directory, direntry->d_name);
		stat(cwd, &attribut);

		if(attribut.st_mode & S_IFDIR)
		{
			// This is a directory, loop through the logs
			if((subdir = opendir(cwd)))
			{
				int len = strlen(cwd);
				while((direntry = readdir(subdir)))
				{
					snprintf(cwd + len, sizeof(cwd) - len, "/%s", direntry->d_name);
					stat(cwd, &attribut);

					// This also takes care of . and .. // To avoid trigraph warnings
					if(!(attribut.st_mode & S_IFREG) || match("????" "-??" "-??" ".log", direntry->d_name))
						continue;

					// Now try to convert the first 10 chars to a valid date so we can compare it...
					strncpy(filedate, direntry->d_name, sizeof(filedate) - 1);
					sscanf(filedate, "%4d-%2d-%2d", &year, &month, &day);
					if(!check_date(day, month, year))
						continue;

					// We got a year, a month and a day
					timeinfo->tm_year = year - 1900;
					timeinfo->tm_mon = month - 1;
					timeinfo->tm_mday = day;
					if((date = mktime(timeinfo)) == -1)
					{
						debug("mktime() returned -1");
						continue;
					}

					if(((today - date) / 60 / 60 / 24) >= purge_after)
					{
						debug("Purging logfile %s", direntry->d_name);
						count++;
						unlink(cwd);
					}

				}
				closedir(subdir);
			}
			else
				debug("Could not open subdirectory: %s", direntry->d_name);
		}
	}
	closedir(dir);
	return count;
}

static void chanlog_rotate()
{
	debug("Rotating channel logfiles.");
	chanlog_fini();
	chanlog_init();
}

static void chanlog(const char *target, const char *format, ...)
{
	va_list args;
	FILE *fd;
	char buf[900] = {0}, timestamp[50] = {0};

	if(!(fd = fd_find(target)))
		return;

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	struct tm *timeinfo = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "[%d.%m.%Y %H:%M:%S]", timeinfo);
	fprintf(fd, "%s %s\n", timestamp, strip_codes(buf));

	fflush(fd);
}

static int cmod_enabled(struct chanreg *creg, enum cmod_enable_reason reason)
{
	if(reason == CER_REG)
		return 1;

	return chanlog_add(creg->channel);
}

static int cmod_disabled(struct chanreg *creg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	int ret = 0;
	chanlog_del(creg->channel);
	if(delete_data && chanlog_conf.directory)
	{
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/%s", chanlog_conf.directory, creg->channel);
		strtolower(dir);
		ret = remdir(dir, 1);
	}
	return ret;
}

int cset_purgeafter_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	long val;

	if(!aredigits(value))
		reply("You can only use an integral number of days to be purged after.");

	else if(value[0] == '0' && value[1] == '\0')
		return 1;

	else if((val = atol(value)) < 0)
		reply("The number you entered is too small.");

	else if(val > 30)
		reply("The number of days to be purged after must not exceed 30 days.");

	else if(!val)
		reply("You entered an invalid number");

	else
		return 1;

	return 0;
}

const char *cset_purgeafter_formatter(struct chanreg *reg, const char *value)
{
	static char str[30];
	int iValue = atoi(value);
	if(value[0] == '0' && value[1] == '\0')
		return "0 - Never purge logs";

	else if(iValue == 0)
		return value;

	else
	{
		snprintf(str, sizeof(str), "%d day%s", iValue, (iValue != 1 ? "s" : ""));
		return str;
	}
}

int cset_logfilemode_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	if(!aredigits(value) || strlen(value) != 3 || value[0] > '7' || value[1] > '7' || value[2] > '7')
	{
		reply("$b%s$b is not a valid filemode.", value);
		return 0;
	}

	if(!((value[0] - 48) & 2))
		reply("I, the owner, need to be able to write to the file. I'm going to grant myself writing permissions.");

	return 1;
}

const char *cset_logfilemode_encoder(struct chanreg *reg, const char *old_value, const char *value)
{
	static char str[4];
	memset(str, 0, sizeof(str));

	strncpy(str, value, sizeof(str));
	if(!((str[0] - '0') & 2))
		str[0] = ((str[0] - '0') | 6) + '0';

	if(chanlog_conf.directory)
	{
		char path[PATH_MAX], file[PATH_MAX];
		DIR *dir;
		struct stat attribute;
		unsigned int mode = ((str[0] - '0') << 6) | ((str[1] - '0') << 3) | (str[2] - '0');

		snprintf(path, sizeof(path), "%s/%s", chanlog_conf.directory, reg->channel);
		stat(path, &attribute);
		if((attribute.st_mode & S_IFDIR))
		{
			if((dir = opendir(path)))
			{
				struct dirent *entry;
				while((entry = readdir(dir)))
				{
					snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
					stat(file, &attribute);
					if(!(attribute.st_mode & S_IFREG) || match("????" "-??" "-??" ".log", entry->d_name))
						continue;

					chanlog_chmod(file, mode);
				}
			}
		}
	}
	return str;
}

static void chanuser_del_hook(struct irc_chanuser *user, unsigned int del_type, const char *reason)
{
	char *modechar;
	char del_reason[512] = {0};

	if((del_type != DEL_QUIT && del_type != DEL_PART) || !chanreg_module_active(cmod, user->channel->name))
		return;

	if(reason && *reason != '\0')
		snprintf(del_reason, sizeof(del_reason), " (%s)", reason);

	modechar = get_mode_char(user);
	chanlog(user->channel->name, "*** %s: %s%s (%s@%s)%s", (del_type == DEL_PART ? "Parts" : "Quits"),  modechar, user->user->nick, user->user->ident, user->user->host, del_reason);

	if(!strcasecmp(user->user->nick, bot.nickname))
		chanlog_del(user->channel->name);
}

static void chanlog_timer(void *bound, void *data)
{
	chanlog_rotate();
	chanlog_purge(NULL);
	chanlog_timer_add();
}

static void chanlog_chmod(const char *file, unsigned int mode)
{
	if(chmod(file, mode) != 0)
		log_append(LOG_ERROR, "Could not set filemode %o for file %s", mode, file);
}

IRC_HANDLER(join)
{
	assert(argc > 1);
	if(chanreg_module_active(cmod, argv[1]))
	{
		if(!strcasecmp(src->nick, bot.nickname))
			if(chanlog_add(argv[1]))
				return;

		chanlog(argv[1], "*** Joins: %s (%s@%s)", src->nick, src->ident, src->host);
	}
}

IRC_HANDLER(kick)
{
	CHANLOG_ACTIVE;
	assert(argc > 3);
	chanlog(argv[1], "*** %s kicked %s (%s)", src->nick, argv[2], argv[3]);

	if(!strcasecmp(argv[3], bot.nickname))
		chanlog_del(argv[1]);
}

IRC_HANDLER(mode)
{
	CHANLOG_ACTIVE;
	assert(argc > 2);
	char *str = untokenize(argc - 2, argv + 2, " ");
	chanlog(argv[1], "*** %s sets mode: %s", src->nick, str);
	free(str);
}

IRC_HANDLER(nick)
{
	assert(argc > 1);
	struct irc_user *user;
	assert((user = user_find(argv[1])));
	dict_iter(node, user->channels)
	{
		struct irc_chanuser *cuser = node->data;
		if(chanreg_module_active(cmod, cuser->channel->name))
			chanlog(cuser->channel->name, "*** %s is now known as %s", src->nick, argv[1]);
	}
}

IRC_HANDLER(notice)
{
	assert(argc > 2);
	char *str = argv[1];
	if(*str == '@' || *str == '+')
		str++;
	if(!IsChannelName(str))
		return;
	if(!chanreg_module_active(cmod, str))
		return;

	chanlog(str, "-%s:%s- %s", src->nick, argv[1], argv[2]);
}

IRC_HANDLER(privmsg)
{
	assert(argc > 2);

	if(!IsChannelName(argv[1]))
		return;

	CHANLOG_ACTIVE;

	char *modechar = "";
	struct irc_user *iuser = user_find(src->nick);
	if(iuser)
	{
		struct irc_chanuser *cuser = channel_user_find(channel_find(argv[1]), user_find(src->nick));
		modechar = get_mode_char(cuser);
	}

	if(*argv[2] == '\001') // /me?
	{
		char *str = strdup(argv[2]);
		char *tmp = str + strlen(str) - 1; // points to last character
		if(*tmp == '\001')
			*tmp = '\0';

		chanlog(argv[1], "* %s%s %s", modechar, src->nick, str + 8); // strlen(chr(1)) + strlen("ACTION ")
		free(str);
	}
	else
		chanlog(argv[1], "<%s%s> %s", modechar, src->nick, argv[2]);
}

IRC_HANDLER(topic)
{
	CHANLOG_ACTIVE;
	assert(argc > 2);
	chanlog(argv[1], "*** %s changes topic to '%s'", src->nick, argv[2]);
}
