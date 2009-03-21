#ifndef __CHANSERV_CHANNELS_H__
#define __CHANSERV_CHANNELS_H__

#include "modules/chanserv/chanserv.h"
#include <time.h>

typedef void (chanserv_access_f)(const char *channel, const char *nick, int access);

struct chanserv_channel
{
	struct chanreg *reg;

	struct dict *users;
	struct ptrlist *events;

	time_t last_event_ts;
	unsigned int user_count;

	enum {
		CS_P_NONE,
		CS_P_CHANINFO,
		CS_P_USERLIST,
		CS_P_EVENTS,
		CS_P_NAMES
	} process;

	unsigned int active : 1;
};

#define CHANSERV_TIMEOUT		-1
#define CHANSERV_ACCESS_DENIED	-2

struct chanserv_access_request
{
	char *channel;
	char *nick;
	struct timer *timer;

	int access; // 0 - 500 or above constants for errors

	chanserv_access_f *callback;
};

extern struct ptrlist *chanserv_channels;
extern struct ptrlist *chanserv_access_requests;

void chanserv_channels_init();
void chanserv_channels_fini();
void chanserv_channels_populate();

void chanserv_db_read(struct dict *db_nodes, struct chanreg *reg);
int chanserv_db_write(struct database_object *dbo, struct chanreg *reg);

struct chanserv_channel *chanserv_channel_create(struct chanreg *reg);
struct chanserv_channel *chanserv_channel_find(const char *channelname);
void chanserv_channel_free(struct chanserv_channel *cschan);

void chanserv_timer_add();
void chanserv_timer_del();

void chanserv_channel_complete_hook(struct irc_channel *channel);
void chanserv_report(const char *channel, const char *format, ...);
void chanserv_chanreg_add(struct chanreg *reg);
void chanserv_chanreg_del(struct chanreg *reg);

void chanserv_get_access_callback(const char *channel, const char *nick, chanserv_access_f *);
void chanserv_access_request_timer(void *bound, struct chanserv_access_request *request);
void chanserv_access_request_handle_raw(const char *channel, const char *nick, int access);
void chanserv_access_request_free(struct chanserv_access_request *);

#endif // __CHANSERV_CHANNELS_H__
