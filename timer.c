#include "global.h"
#include "timer.h"

static struct dict *timers;
static unsigned long next_timer_id = 0;

void timer_init()
{
	timers = dict_create();
}

void timer_fini()
{
	timer_del(NULL, NULL, 0, NULL, NULL, TIMER_IGNORE_ALL); // delete all timers
	dict_free(timers);
}

void timer_add(void *bound, const char *name, time_t time, timer_f *func, void *data, unsigned int free_data)
{
	debug("Adding timer %s (%lu) - triggered in %lu secs", name ? name : "-noname-", next_timer_id, time - now);
	struct timer *tmr = malloc(sizeof(struct timer));
	tmr->id = next_timer_id;
	tmr->name = strdup(name);
	tmr->bound = bound;
	tmr->time = time;
	tmr->func = func;
	tmr->data = data;
	tmr->free_data = free_data;
	tmr->triggered = 0;
	dict_insert(timers, NULL, tmr);

	next_timer_id++;
}

unsigned int timer_exists(void *bound, const char *name, time_t time, timer_f *func, void *data, long flags)
{
	struct dict_node *node;
	node = timers->head;
	while(node)
	{
		struct timer *tmr = node->data;
		if(((flags & TIMER_IGNORE_BOUND) || tmr->bound == bound) &&	// check bound object
		   ((flags & TIMER_IGNORE_NAME) || !strcmp(tmr->name, name)) &&	// check timer name
		   ((flags & TIMER_IGNORE_TIME) || tmr->time == time) &&	// check time
		   ((flags & TIMER_IGNORE_FUNC) || tmr->func == func) &&	// check timer function
		   ((flags & TIMER_IGNORE_DATA) || tmr->data == data) &&	// check timer data
		    !tmr->triggered)						// must not have been triggered yet
		{
			return 1;
		}
		else
		{
			node = node->next;
		}
	}
	return 0;
}

void timer_del(void *bound, const char *name, time_t time, timer_f *func, void *data, long flags)
{
	struct dict_node *node, *tmp;
	node = timers->head;
	while(node)
	{
		struct timer *tmr = node->data;
		if(((flags & TIMER_IGNORE_BOUND) || tmr->bound == bound) &&
		   ((flags & TIMER_IGNORE_NAME) || !strcmp(tmr->name, name)) &&
		   ((flags & TIMER_IGNORE_TIME) || tmr->time == time) &&
		   ((flags & TIMER_IGNORE_FUNC) || tmr->func == func) &&
		   ((flags & TIMER_IGNORE_DATA) || tmr->data == data) &&
		    !tmr->triggered)
		{
			debug("Deleting timer %s (%lu)", (tmr->name ? tmr->name : "-noname-"), tmr->id);
			if(tmr->free_data)
				free(tmr->data);
			free(tmr->name);
			free(tmr);
			tmp = node;
			node = node->next;
			dict_delete_node(timers, tmp);
		}
		else
		{
			node = node->next;
		}
	}
}

void timer_poll()
{
	struct dict_node *node, *tmp;
	node = timers->head;
	while(node)
	{
		struct timer *tmr = node->data;
		if(tmr->time <= now)
		{
			debug("Triggering timer %s (%lu)", (tmr->name ? tmr->name : "-noname-"), tmr->id);
			tmr->triggered = 1;
			tmr->func(tmr->bound, tmr->data);
			if(tmr->free_data)
				free(tmr->data);
			free(tmr->name);
			free(tmr);
			tmp = node;
			node = node->next;
			dict_delete_node(timers, tmp);
		}
		else
		{
			node = node->next;
		}
	}
}
