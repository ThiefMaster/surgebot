#include "global.h"
#include "module.h"
#include "perform.h"
#include "irc.h"
#include "irc_handler.h"
#include "database.h"
#include "timer.h"
#include "conf.h"

MODULE_DEPENDS(NULL);

IRC_HANDLER(perform_handler);
static int perform_exec(struct dict *record, struct irc_source *src, int argc, char **argv);
static void perform_start();

static struct dict *perform = NULL;
static struct dict *perform_hooks = NULL;
static struct dict *perform_timers = NULL;
static struct dict *perform_funcs = NULL;


MODULE_INIT
{
	perform_funcs = dict_create();
	reg_connected_func(perform_start);
}

MODULE_FINI
{
	unreg_connected_func(perform_start);
	dict_free(perform_funcs);

	if(perform_hooks)
	{
		debug("Deleting remaining perform handlers");
		dict_iter(node, perform_hooks)
		{
			unreg_irc_handler(node->key, perform_handler);
		}

		dict_free(perform_hooks);
		perform_hooks = NULL;
	}

	if(perform_timers)
	{
		debug("Deleting remaining perform timers");
		dict_iter(node, perform_timers)
		{
			timer_del_boundname(perform, node->key);
		}

		dict_free(perform_timers);
		perform_timers = NULL;
	}

	if(perform)
	{
		debug("Deleting remaining perform");
		dict_free(perform);
		perform = NULL;
	}
}

void perform_func_reg(char *name, perform_f *func)
{
	dict_insert(perform_funcs, name, func);
}

void perform_func_unreg(const char *name)
{
	dict_delete(perform_funcs, name);
}

static void perform_start()
{
	char *filename;
	struct dict *start_record = NULL;

	if(perform_hooks)
	{
		debug("Deleting old perform handlers");
		dict_iter(node, perform_hooks)
		{
			unreg_irc_handler(node->key, perform_handler);
		}

		dict_free(perform_hooks);
		perform_hooks = NULL;
	}

	if(perform_timers)
	{
		debug("Deleting old perform timers");
		dict_iter(node, perform_timers)
		{
			timer_del_boundname(perform, node->key);
		}

		dict_free(perform_timers);
		perform_timers = NULL;
	}

	if(perform)
	{
		debug("Deleting old perform");
		dict_free(perform);
		perform = NULL;
	}

	filename = conf_get("perform/file", DB_STRING);
	log_append(LOG_INFO, "Loading perform from %s", filename);
	if(!filename || !(perform = database_load("perform.cfg")))
	{
		log_append(LOG_WARNING, "Could not load perform from %s.", filename);
		return;
	}

	perform_hooks = dict_create();
	dict_set_free_funcs(perform_hooks, free, NULL);
	perform_timers = dict_create();
	dict_set_free_funcs(perform_timers, free, NULL);

	dict_iter(node, perform)
	{
		struct db_node *dn = node->data;
		if(dn->type != DB_OBJECT)
		{
			debug("Invalid type for perform record '%s': %d, expected DB_OBJECT (%d)", node->key, dn->type, DB_OBJECT);
			continue;
		}

		struct dict *record = dn->data.object;
		char *key = strdup(node->key);
		char *arg = strchr(key, ':');
		if(!arg && strcasecmp(key, "*"))
		{
			debug("Invalid perform key '%s': arg missing", key);
			free(key);
			continue;
		}

		if(arg)
			*arg++ = '\0';

		if(!strlen(key) || (arg && !strlen(arg)))
		{
			debug("Key (%s) or arg (%s) is empty", key, arg);
			free(key);
			continue;
		}

		if(!strcasecmp(key, "cmd"))
		{
			// Register an irc hook for this but show a warning if it already exists
			if(dict_find(perform_hooks, arg) == NULL)
			{
				dict_insert(perform_hooks, strdup(arg), record);
				reg_irc_handler(arg, perform_handler);
			}
			else
			{
				log_append(LOG_WARNING, "There is already a perform handler for cmd:%s", arg);
			}
		}
		else if(!strcasecmp(key, "tmr"))
		{
			if(dict_find(perform_timers, arg) == NULL)
			{
				dict_insert(perform_timers, strdup(arg), record);
			}
			else
			{
				log_append(LOG_WARNING, "There is already a perform handler for tmr:%s", arg);
			}
		}
		else if(!strcasecmp(key, "*"))
		{
			if(start_record)
				log_append(LOG_WARNING, "There was already a start record (*); overwriting old one.");
			start_record = record;
		}
		else
		{
			debug("Unknown perform entry type: %s", key);
		}

		free(key);
	}

	if(start_record)
		perform_exec(start_record, NULL, 0, NULL);
}

IRC_HANDLER(perform_handler)
{
	debug("Perform IRC handler called for command %s", argv[0]);
	if(!perform || !perform_hooks || !dict_size(perform_hooks))
		return;

	struct dict *record = dict_find(perform_hooks, argv[0]);
	if(record == NULL)
	{
		log_append(LOG_WARNING, "Handler for %s called, but no matching perform record found.", argv[0]);
		return;
	}

	if(perform_exec(record, src, argc, argv) == 0)
	{
		unreg_irc_handler(argv[0], perform_handler);
		dict_delete(perform_hooks, argv[0]);
	}
}

static void perform_timer(void *bound, char *name)
{
	debug("Perform timer %s called", name);
	assert(bound == perform);

	if(!perform || !perform_timers || !dict_size(perform_timers))
		return;

	struct dict *record = dict_find(perform_timers, name);
	if(record == NULL)
	{
		log_append(LOG_WARNING, "Timer %s called, but no matching perform record found.", name);
		return;
	}

	if(perform_exec(record, NULL, 0, NULL) == 0)
	{
		dict_delete(perform_timers, name);
	}
}

static int perform_exec(struct dict *record, struct irc_source *src, int argc, char **argv)
{
	char *str;
	struct stringlist *slist;
	struct dict *dict;
	debug("Executing perform record");

	// Check requirements
	if((dict = database_fetch(record, "args", DB_OBJECT)))
	{
		dict_iter(node, dict)
		{
			struct db_node *dn = node->data;
			if(!strcasecmp(node->key, "src"))
			{
				if(!src || !src->nick)
				{
					debug("Ignoring arg 'src' in perform record which has no source");
					return 1;
				}

				if(dn->type != DB_STRING)
				{
					debug("Invalid type for perform record arg 'src': %d, expected DB_STRING (%d)", dn->type, DB_STRING);
					continue;
				}

				if(src->host) // nick!ident@host
				{
					str = malloc(strlen(src->nick) + strlen(src->ident) + strlen(src->host) + 4);
					sprintf(str, "%s!%s@%s", src->nick, src->ident, src->host);
				}
				else // Only a nick
				{
					str = strdup(src->nick);
				}

				if(match(irc_format_line(dn->data.string), str))
				{
					debug("'%s' does not match '%s' - not executing this perform record", str, dn->data.string);
					free(str);
					return 1;
				}

				free(str);
			}
			else
			{
				int num = atoi(node->key);
				if(dn->type != DB_STRING)
				{
					debug("Invalid type for perform record arg '%d': %d, expected DB_STRING (%d)", num, dn->type, DB_STRING);
					continue;
				}

				if(num >= argc)
				{
					debug("Cannot check arg %d, argc is %d", num, argc);
					return 1;
				}

				if(match(irc_format_line(dn->data.string), argv[num]))
				{
					debug("'%s' does not match '%s' - not executing this perform record", argv[num], dn->data.string);
					return 1;
				}
			}
		}
	}

	/* Now execute the commands of this record */
	// Send data to irc
	if((slist = database_fetch(record, "send", DB_STRINGLIST)) != NULL)
	{
		for(int i = 0; i < slist->count; i++)
			irc_send("%s", slist->data[i]);
	}

	if((str = database_fetch(record, "send", DB_STRING)) != NULL)
	{
		irc_send("%s", str);
	}

	// Delete timer(s)
	if((slist = database_fetch(record, "timerdel", DB_STRINGLIST)) != NULL)
	{
		for(int i = 0; i < slist->count; i++)
			timer_del_boundname(perform, slist->data[i]);
	}

	if((str = database_fetch(record, "timerdel", DB_STRING)) != NULL)
	{
		timer_del_boundname(perform, str);
	}

	// Start timer
	if((dict = database_fetch(record, "timer", DB_OBJECT)) != NULL)
	{
		char *timername = database_fetch(dict, "name", DB_STRING);
		time_t time = ((str = database_fetch(dict, "time", DB_STRING)) ? strtoul(str, NULL, 0) : 0);

		if(!timername || !strlen(timername) || time < 0)
		{
			debug("Invalid timer record: name (%s) or time (%lu) is missing/empty", timername, time);
		}
		else
		{
			debug("Adding perform timer %s (%lu secs)", timername, time);
			timer_add(perform, timername, now + time, (timer_f *)perform_timer, strdup(timername), 1);
		}
	}

	// Call function(s)
	if((slist = database_fetch(record, "call", DB_STRINGLIST)) != NULL)
	{
		perform_f *func;
		for(int i = 0; i < slist->count; i++)
		{
			if((func = dict_find(perform_funcs, str)))
				func();
			else
				log_append(LOG_WARNING, "Cannot call unregistered perform function %s", str);
		}
	}

	if((str = database_fetch(record, "call", DB_STRING)) != NULL)
	{
		perform_f *func;
		if((func = dict_find(perform_funcs, str)))
			func();
		else
			log_append(LOG_WARNING, "Cannot call unregistered perform function %s", str);
	}


	if((str = database_fetch(record, "static", DB_STRING)) && true_string(str))
	{
		debug("Not deleting static perform record");
		return 1;
	}

	return 0;
}
