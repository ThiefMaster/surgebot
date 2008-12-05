#define _GNU_SOURCE
#include <time.h>
#include "global.h"
#include "string.h"
#include "timer.h"
#include "modules/chanserv/chanserv.h"

extern struct db_table *event_table;

inline void chanserv_event_add(struct tm calendar, const char *channel, const char *issuer, const char *command)
{
	time_t stamp;
	char *tmp, source[513] = {0}, account[513] = {0};
	struct chanserv_event *event;
	struct chanserv_channel *cschan;
	struct irc_source *src;
	size_t len;

	calendar.tm_year -= 1900;
	calendar.tm_mon--;

	if((stamp = mktime(&calendar)) == -1)
	{
		log_append(LOG_ERROR, "Could not convert given time to timestamp.");
		return;
	}

	assert(cschan = chanserv_channel_find(channel));
	cschan->process = CS_P_EVENTS;

	// Make sure this is newer than the last event we captured
	if(stamp <= cschan->last_event_ts)
	{
		debug("Event seems to be outdated; %lu <= %lu", stamp, cschan->last_event_ts);
		return;
	}

	len = strlen(issuer);

	event = malloc(sizeof(struct chanserv_event));
	memset(event, 0, sizeof(struct chanserv_event));
	event->timestamp = stamp;

	src = malloc(sizeof(struct irc_source));
	if((tmp = strchr(issuer, ':')))
	{
		int diff = tmp - issuer;
		if(diff > (sizeof(source) - 1))
		{
			log_append(LOG_ERROR, "IRC Source issuer '%s' exceeds %lu chars", issuer, (unsigned long)(sizeof(source) - 1));
			free(event);
			return;
		}
		strncpy(source, issuer, diff);
		if((len - diff + 1) > (sizeof(account) - 1))
		{
			log_append(LOG_ERROR, "IRC Source account '%s' exceeds %lu chars", tmp, (unsigned long)(sizeof(account) - 1));
			free(event);
			return;
		}
		strcpy(account, tmp + 1);
	}
	else
	{
		if(len > (sizeof(source) - 1))
		{
			log_append(LOG_ERROR, "IRC Source '%s' exceeds %lu chars", issuer, (unsigned long)(sizeof(source) - 1));
			free(event);
			return;
		}
		strcpy(source, issuer);
	}

	if(!(tmp = strchr(source, '@')))
	{
		src->nick = strdup(source);
		src->ident = src->host = NULL;
	}
	else
	{
		char *tmp2;
		if(!(tmp2 = strchr(source, '!')) || tmp2 > tmp)
		{
			log_append(LOG_ERROR, "Invalid IRC mask '%s'", source);
			free(event);
			return;
		}

		src->nick = strndup(source, tmp2 - source);
		tmp2++;
		src->ident = strndup(tmp2, tmp - tmp2);
		src->host = strdup(tmp + 1);
	}

	if(!strcmp(src->nick, bot.nickname))
	{
		debug("We issued this event ourselves, skipping.");
		cschan->last_event_ts = stamp;

		free(src->nick);
		MyFree(src->ident);
		MyFree(src->host);
		free(src);
		free(event);
		return;
	}

	event->src = src;
	event->account = strdup(account);
	event->command = strdup(command);

	ptrlist_add(cschan->events, 0, event);
	cschan->last_event_ts = event->timestamp;

	if(event_table)
		db_row_insert(event_table,
			"time",		event->timestamp,
			"channel",	cschan->reg->channel,
			"nick",		event->src->nick,
			"ident",	event->src->ident,
			"host",		event->src->host,
			"account",	event->account,
			"command",	event->command,
			NULL);

	ptrlist_add(cschan->events, 0, event);
}

void chanserv_fetch_events(void *bound, struct chanserv_channel *cschan)
{
	if(cschan)
		return irc_send(sz_chanserv_fetch_events, cschan->reg->channel, u_chanserv_fetch_events_amount);

	for(int i = 0; i < cmod->channels->count; i++)
	{
		struct chanreg *reg = cmod->channels->data[i];
		irc_send(sz_chanserv_fetch_events, reg->channel, u_chanserv_fetch_events_amount);
	}

	chanserv_event_timer_add();
}

void chanserv_event_free(struct chanserv_event *event)
{
	free(event->src->nick);

	if(event->src->ident)
		free(event->src->ident);
	if(event->src->host);
		free(event->src->host);

	free(event->src);
	free(event->account);
	free(event->command);
	free(event);
}

int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason)
{
	if(reason == CER_ENABLED)
	{
		struct chanserv_channel *cschan;
		if(!(cschan = chanserv_channel_find(reg->channel)))
		{
			log_append(LOG_INFO, "Channel %s is not registered with ChanServ.", reg->channel);
			return 1;
		}
		chanserv_fetch_events(NULL, cschan);
	}
	return 0;
}

int cmod_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
	{
		struct chanserv_channel *cschan = chanserv_channel_find(reg->channel);
		if(!cschan)
			log_append(LOG_ERROR, "Channel %s is not registered with ChanServ.", reg->channel);


		if(event_table)
		{
			db_row_drop(event_table, "channel", reg->channel, NULL);
			cschan->last_event_ts = 0;
		}
	}

	return 0;
}

void chanserv_event_timer_add()
{
	chanserv_event_timer_del();
	timer_add(cmod, sz_chanserv_event_timer_name, now + u_chanserv_fetch_events_interval, (timer_f*)chanserv_fetch_events, NULL, 0, 1);
}

void chanserv_event_timer_del(struct chanserv_channel *cschan)
{
	timer_del_boundname(cmod, sz_chanserv_event_timer_name);
}
