#ifndef __CHANSERV_EVENTS_H__
#define __CHANSERV_EVENTS_H__

struct chanserv_event
{
	time_t timestamp;
	struct irc_source *src;
	char *account;
	char *command;
};

inline void chanserv_event_add(struct tm calendar, const char *channel, const char *issuer, const char *command);
void chanserv_event_timer(void *, void *);
void chanserv_event_free(struct chanserv_event *);
void chanserv_fetch_events(void *bound, struct chanserv_channel *cschan);

int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason);
int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);

void chanserv_event_timer_add();
void chanserv_event_timer_del();

#endif // __CHANSERV_EVENTS_H__
