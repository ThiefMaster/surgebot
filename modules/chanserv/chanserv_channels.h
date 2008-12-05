#ifndef __CHANSERV_CHANNELS_H__
#define __CHANSERV_CHANNELS_H__

#include "modules/chanserv/chanserv.h"
#include <time.h>

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
		CS_P_EVENTS
	} process;

	unsigned int active : 1;
};

extern struct ptrlist *chanserv_channels;

void chanserv_channels_init();
void chanserv_channels_fini();
inline void chanserv_channels_populate();

void chanserv_db_read(struct dict *db_nodes, struct chanreg *reg);
int chanserv_db_write(struct database_object *dbo, struct chanreg *reg);

struct chanserv_channel *chanserv_channel_create(struct chanreg *reg);
struct chanserv_channel *chanserv_channel_find(const char *channelname);
void chanserv_channel_free(struct chanserv_channel *cschan);

void chanserv_timer_add();
void chanserv_timer_del();

void chanserv_channel_complete_hook(struct irc_channel *channel);

void chanserv_report(const char *channel, const char *format, ...);

#endif // __CHANSERV_CHANNELS_H__