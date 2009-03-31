#include "global.h"
#include "module.h"
#include "dict.h"
#include "timer.h"
#include "irc.h"
#include "conf.h"
#include "modules/help/help.h"
#include "modules/commands/commands.h"
#include <time.h>

MODULE_DEPENDS("commands", "help", NULL);

static const char * const timer_name = "custom_timer";

static struct {
	unsigned long min_interval;
	unsigned long max_lines;
	unsigned long max_timers;
} timer_conf;

struct user_timer_channel {
	char *channel;
	struct dict *timers;
};

struct user_timer {
	char *name;
	struct stringlist *lines;
	unsigned long interval;
};

static void user_timer_func(struct user_timer_channel *channel, struct user_timer *timer);
static void user_timer_conf_reload(void);
static void user_timer_db_read(struct database *db);
static int user_timer_db_write(struct database *db);

static struct user_timer_channel *user_timer_channel_create(const char *channel);
static struct user_timer_channel *user_timer_channel_find(const char *channel);
static void user_timer_channel_free(struct user_timer_channel *channel);

static struct user_timer *user_timer_create(struct user_timer_channel *channel, const char *name, unsigned long interval);
static void user_timer_free(struct user_timer *timer);

static void user_timer_add_timer(struct user_timer_channel *channel, struct user_timer *timer);
static void user_timer_del_timer(struct user_timer *timer);

static struct dict *user_timer_channels;
static struct database *timer_db;

COMMAND(timer_list);
COMMAND(timer_add);
COMMAND(timer_msg);
COMMAND(timer_del);

MODULE_INIT
{
	user_timer_channels = dict_create();
	dict_set_free_funcs(user_timer_channels, NULL, (dict_free_f*)user_timer_channel_free);

	DEFINE_COMMAND(self, "timer list", timer_list, 1, CMD_ACCEPT_CHANNEL, "chanuser(500)");
	DEFINE_COMMAND(self, "timer add", timer_add, 3, CMD_ACCEPT_CHANNEL, "chanuser(500)");
	DEFINE_COMMAND(self, "timer msg", timer_msg, 2, CMD_ACCEPT_CHANNEL, "chanuser(500)");
	DEFINE_COMMAND(self, "timer del", timer_del, 1, CMD_ACCEPT_CHANNEL, "chanuser(500)");

	timer_db = database_create("timers", user_timer_db_read, user_timer_db_write);
	database_read(timer_db, 1);
	database_set_write_interval(timer_db, 300);

	reg_conf_reload_func(user_timer_conf_reload);
	user_timer_conf_reload();

	help_load(self, "timers.help");
}

MODULE_FINI
{
	unreg_conf_reload_func(user_timer_conf_reload);

	database_write(timer_db);
	database_delete(timer_db);

	dict_free(user_timer_channels);
	timer_del(NULL, timer_name, 0, NULL, NULL, TIMER_IGNORE_ALL & ~TIMER_IGNORE_NAME);
}

COMMAND(timer_list)
{
	if(!channel)
	{
		reply("No channel provided.");
		return 0;
	}
	struct user_timer_channel *timer_chan = user_timer_channel_find(channel->name);
	if(!timer_chan)
	{
		reply("You have not set any timers yet.");
		return 0;
	}

	// No more arguments given, display all timers
	dict_iter(node, timer_chan->timers)
	{
		struct user_timer *timer = node->data;
		reply("$uTimer $b%s$b (Every %lu seconds):", node->key, timer->interval);
		for(unsigned int i = 0; i < timer->lines->count; i++)
		{
			// Skip empty line
			if(!*timer->lines->data[i])
				continue;

			reply("$b%3u$b: %s", i + 1, timer->lines->data[i]);
		}
	}
	reply("$b%u$b timer%s.", timer_chan->timers->count, timer_chan->timers->count == 1 ? "" : "s");
	return 0;
}

COMMAND(timer_add)
{
	if(!channel)
	{
		reply("No channel provided.");
		return 0;
	}

	if(!aredigits(argv[2]))
	{
		reply("$b%s$b is not a valid interval.", argv[2]);
		return 0;
	}
	unsigned long interval = strtoul(argv[2], NULL, 10);
	if(timer_conf.min_interval && interval < timer_conf.min_interval)
	{
		reply("The minimum interval for custom timers is $b%lu$b seconds.", timer_conf.min_interval);
		return 0;
	}

	struct user_timer_channel *timer_chan = user_timer_channel_create(channel->name);

	// First argument is the timer's name
	struct user_timer *timer = dict_find(timer_chan->timers, argv[1]);
	if(timer)
	{
		reply("Timer $b%s$b already exists.", argv[1]);
		if(timer->interval != interval)
		{
			reply("Setting new interval of $b%lu$b seconds.", interval);
			timer->interval = interval;
			return 1;
		}
		return 0;
	}

	if(timer_conf.max_timers && timer_chan->timers->count >= timer_conf.max_timers)
	{
		reply("The maximum of allowed timers is already reached. You may not add any more.");
		return 0;
	}

	user_timer_create(timer_chan, argv[1], interval);
	reply("Timer $b%s$b has been created and will be executed every $b%lu$b seconds.", argv[1], interval);
	return 1;
}

COMMAND(timer_msg)
{
	if(!channel)
	{
		reply("No channel provided.");
		return 0;
	}
	struct user_timer_channel *timer_chan = user_timer_channel_find(channel->name);
	if(!timer_chan)
	{
		reply("No timers have been added for this channel yet.");
		return 0;
	}
	struct user_timer *timer = dict_find(timer_chan->timers, argv[1]);
	if(!timer)
	{
		reply("There is no timer called $b%s$b.", argv[1]);
		return 0;
	}

	// No timer line given, display all lines
	if(argc <= 2)
	{
		reply("$uTimer $b%s$b (Every %lu seconds):", timer->name, timer->interval);
		for(unsigned int i = 0; i < timer->lines->count; i++)
		{
			// Skip empty line
			if(!*timer->lines->data[i])
				continue;

			reply("$b%3u$b: %s", i + 1, timer->lines->data[i]);
		}
		return 0;
	}

	int index = atoi(argv[2]) - 1;
	if(index < 0 || (unsigned int)index >= timer->lines->count)
	{
		unsigned int max_index = timer->lines->count + 1;
		if(timer_conf.max_lines && index >= 0 && (unsigned int)index >= timer_conf.max_lines)
			max_index = timer_conf.max_lines;

		reply("The message index needs to be between $b1$b and $b%u$b.", max_index);
		return 0;
	}

	// Valid index
	// No third argument, show corresponding message
	if(argc <= 3)
	{
		if((unsigned int)index >= timer->lines->count || !*timer->lines->data[index])
		{
			reply("There is no line with index $b%d$b.", index + 1);
			return 0;
		}
		reply("$b%3d$b: %s", index + 1, timer->lines->data[index]);
		return 0;
	}
	// Third argument given, delete if it's *
	if(argc == 4 && !strcmp("*", argv[3]))
	{
		if((unsigned int)index >= timer->lines->count)
		{
			reply("There is no line with index $b%d$b.", index + 1);
			return 0;
		}
		// Delete line if it's the last line in the list
		if((unsigned int)index == (timer->lines->count - 1))
		{
			stringlist_del(timer->lines, index);
			// See if we can drop previous strings as well in case they are empty
			while(timer->lines->count && !*timer->lines->data[timer->lines->count - 1])
				stringlist_del(timer->lines, timer->lines->count - 1);

			// If no lines remain, drop core timer
			if(!timer->lines->count)
				user_timer_del_timer(timer);
		}
		// This is not the last line, set it to empty string as not to remove it from the stringlist
		else
			timer->lines->data[index][0] = '\0';

		reply("Timer line $b%d$b deleted.", index + 1);
		return 1;
	}

	char *line = untokenize(argc - 3, argv + 3, " ");
	// Index already exists, replace it
	if((unsigned int)index < timer->lines->count)
	{
		free(timer->lines->data[index]);
		timer->lines->data[index] = line;
	}
	// Otherwise, create it
	else
		stringlist_add(timer->lines, line);

	reply("$b%3d$b: %s", index + 1, timer->lines->data[index]);
	// If this is the first line, we need to add the core timer
	if(timer->lines->count == 1)
		user_timer_add_timer(timer_chan, timer);

	return 1;
}

COMMAND(timer_del)
{
	if(!channel)
	{
		reply("No channel provided.");
		return 0;
	}
	struct user_timer_channel *timer_chan = user_timer_channel_find(channel->name);
	if(!timer_chan)
	{
		reply("No timers have been added for this channel yet.");
		return 0;
	}
	struct user_timer *timer = dict_find(timer_chan->timers, argv[1]);
	if(!timer)
	{
		reply("There is no timer called $b%s$b.", argv[1]);
		return 0;
	}

	dict_delete(timer_chan->timers, argv[1]);
	reply("Timer $b%s$b has been deleted.", argv[1]);
	return 0;
}

static void user_timer_func(struct user_timer_channel *channel, struct user_timer *timer)
{
	for(unsigned int i = 0; i < timer->lines->count; i++)
	{
		if(*timer->lines->data[i])
		{
			// Lines prefixed with a resetting format code so people can't make the bot execute arbitrary commands
			irc_send("PRIVMSG %s :\017%s", channel->channel, timer->lines->data[i]);
		}
	}
	user_timer_add_timer(channel, timer);
}

static void user_timer_conf_reload(void)
{
	char *str;
	timer_conf.min_interval = ((str = conf_get("timers/min_interval", DB_STRING)) ? strtoul(str, NULL, 10) : 30);
	timer_conf.max_lines = ((str = conf_get("timers/max_lines", DB_STRING)) ? strtoul(str, NULL, 10) : 5);
	timer_conf.max_timers = ((str = conf_get("timers/max_timers", DB_STRING)) ? strtoul(str, NULL, 10) : 3);
}

static void user_timer_db_read(struct database *db)
{
	dict_iter(node, db->nodes)
	{
		struct db_node *db_node = node->data;
		if(db_node->type != DB_OBJECT)
		{
			log_append(LOG_ERROR, "Custom timer path \"%s\" is not a valid db node.", node->key);
			continue;
		}

		dict_iter(subnode, db_node->data.object)
		{
			char *sz_interval;
			unsigned long interval;
			struct stringlist *slist;

			struct db_node *subdb_node = subnode->data;

			if(!(sz_interval = database_fetch(subdb_node->data.object, "interval", DB_STRING)))
			{
				log_append(LOG_ERROR, "Custom timer path \"%s/%s/interval\" is not configured or not a string.", node->key, subnode->key);
				continue;
			}

			if(!(interval = strtoul(sz_interval, NULL, 10)))
			{
				log_append(LOG_ERROR, "Custom timer path \"%s/%s/interval\"=%s is not a valid interval.", node->key, subnode->key, sz_interval);
				continue;
			}

			if(timer_conf.min_interval && interval < timer_conf.min_interval)
			{
				log_append(LOG_WARNING, "Interval given in \"%s/%s/interval\" is too small (interval=%lu).", node->key, subnode->key, interval);
				continue;
			}

			if(!(slist = database_fetch(subdb_node->data.object, "lines", DB_STRINGLIST)))
			{
				log_append(LOG_ERROR, "Custom timer path \"%s/%s/lines\" is not stored or not a stringlist.", node->key, subnode->key);
				continue;
			}

			struct user_timer_channel *channel = user_timer_channel_create(node->key);
			if(timer_conf.max_timers && channel->timers->count >= timer_conf.max_timers)
				continue;

			struct user_timer *timer = user_timer_create(channel, subnode->key, interval);
			// timer->lines is already an allocated stringlist, copy items one by one
			for(unsigned int i = 0; i < slist->count; i++)
				stringlist_add(timer->lines, strdup(slist->data[i]));

			// Here again, if there are lines, we need to add the core timer
			if(timer->lines->count)
				user_timer_add_timer(channel, timer);
		}
	}
}

static int user_timer_db_write(struct database *db)
{
	dict_iter(node, user_timer_channels)
	{
		struct user_timer_channel *timer_chan = node->data;

		database_begin_object(db, node->key);
			dict_iter(timer_node, timer_chan->timers)
			{
				struct user_timer *timer = timer_node->data;
				database_begin_object(db, timer_node->key);
					database_write_long(db, "interval", (long)timer->interval);
					database_write_stringlist(db, "lines", timer->lines);
				database_end_object(db);
			}
		database_end_object(db);
	}
	return 0;
}

static struct user_timer_channel *user_timer_channel_create(const char *channel)
{
	struct user_timer_channel *user_chan;
	if((user_chan = user_timer_channel_find(channel)))
		return user_chan;

	debug("Creating new custom timer channel %s", channel);
	user_chan = malloc(sizeof(struct user_timer_channel));
	memset(user_chan, 0, sizeof(struct user_timer_channel));

	user_chan->channel = strdup(channel);
	user_chan->timers = dict_create();
	dict_set_free_funcs(user_chan->timers, NULL, (dict_free_f*)user_timer_free);

	dict_insert(user_timer_channels, user_chan->channel, user_chan);
	return user_chan;
}

static struct user_timer_channel *user_timer_channel_find(const char *channel)
{
	dict_iter(node, user_timer_channels)
	{
		struct user_timer_channel *timer_chan = node->data;
		if(!strcasecmp(timer_chan->channel, channel))
			return timer_chan;
	}
	return NULL;
}

static void user_timer_channel_free(struct user_timer_channel *channel)
{
	dict_free(channel->timers);
	free(channel->channel);
	free(channel);
}

static struct user_timer *user_timer_create(struct user_timer_channel *channel, const char *name, unsigned long interval)
{
	debug("Creating new custom timer %s", name);
	struct user_timer *timer = malloc(sizeof(struct user_timer));
	memset(timer, 0, sizeof(struct user_timer));

	timer->lines = stringlist_create();
	timer->interval = interval;
	timer->name = strdup(name);

	dict_insert(channel->timers, timer->name, timer);
	return timer;
}

static void user_timer_free(struct user_timer *timer)
{
	user_timer_del_timer(timer);

	stringlist_free(timer->lines);
	free(timer->name);
	free(timer);
}

static void user_timer_add_timer(struct user_timer_channel *channel, struct user_timer *timer)
{
	timer_add(channel, timer_name, now + timer->interval, (timer_f*)user_timer_func, timer, 0, 0);
}

static void user_timer_del_timer(struct user_timer *timer)
{
	timer_del(NULL, timer_name, 0, NULL, timer, TIMER_IGNORE_ALL & ~TIMER_IGNORE_DATA);
}
