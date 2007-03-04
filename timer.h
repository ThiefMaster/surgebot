#ifndef TIMER_H
#define TIMER_H

#define TIMER_IGNORE_BOUND	0x01
#define TIMER_IGNORE_NAME	0x02
#define TIMER_IGNORE_TIME	0x04
#define TIMER_IGNORE_FUNC	0x08
#define TIMER_IGNORE_DATA	0x10

#define TIMER_IGNORE_ALL	0x1F

typedef void (timer_f)(void *bound, void *data);

struct timer
{
	unsigned long	id;
	char		*name;
	void		*bound;
	time_t		time;
	timer_f		*func;
	void		*data;
	unsigned int	free_data : 1;
	unsigned int	triggered : 1;
};

void timer_init();
void timer_fini();

struct dict *timer_dict();

void timer_add(void *bound, const char *name, time_t time, timer_f *func, void *data, unsigned int free_data);
unsigned int timer_exists(void *bound, const char *name, time_t time, timer_f *func, void *data, long flags);
void timer_del(void *bound, const char *name, time_t time, timer_f *func, void *data, long flags);
void timer_poll();

#define timer_del_boundname(BOUND, NAME)	timer_del((BOUND), (NAME), 0, NULL, NULL, TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC|TIMER_IGNORE_DATA)
#define timer_exists_boundname(BOUND, NAME)	timer_exists((BOUND), (NAME), 0, NULL, NULL, TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC|TIMER_IGNORE_DATA)

#endif
