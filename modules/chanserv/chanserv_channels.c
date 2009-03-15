#include "modules/chanserv/chanserv.h"
#include "ptrlist.h"

struct ptrlist *chanserv_channels = NULL;
struct ptrlist *chanserv_access_requests = NULL;

extern struct module *this;
extern struct chanreg_module *cmod;

static void chanserv_fetch_users(void *bound, struct chanserv_channel *cschan);
static struct chanserv_channel *chanserv_channel_create_fetch(struct chanreg *reg);

void chanserv_channels_populate()
{
	struct irc_user *user;
	// Find ChanServ
	if(!(user = user_find(sz_chanserv_botname)))
		return;

	dict_iter(node, user->channels)
	{
		struct irc_chanuser *cuser = node->data;
		struct chanreg *reg = chanreg_find(cuser->channel->name);
		if(reg)
			chanserv_channel_create_fetch(reg);
	}
}

static struct chanserv_channel *chanserv_channel_create_fetch(struct chanreg *reg)
{
	struct chanserv_channel *cschan;
	assert_return((cschan = chanserv_channel_create(reg)), NULL);

	chanserv_fetch_users(NULL, cschan);
	if(chanreg_module_active(cmod, reg->channel))
		chanserv_fetch_events(NULL, cschan);

	return cschan;
}

struct chanserv_channel *chanserv_channel_create(struct chanreg *reg)
{
	struct chanserv_channel *cschan = chanserv_channel_find(reg->channel);
	if(cschan)
		return cschan;

	cschan = malloc(sizeof(struct chanserv_channel));
	memset(cschan, 0, sizeof(struct chanserv_channel));
	cschan->reg = reg;
	cschan->users = dict_create();
	cschan->process = CS_P_NONE;
	cschan->events = ptrlist_create();

	dict_set_free_funcs(cschan->users, NULL, (dict_free_f*)chanserv_user_free);
	ptrlist_set_free_func(cschan->events, (ptrlist_free_f*)chanserv_event_free);

	ptrlist_add(chanserv_channels, 0, cschan);
	return cschan;
}

struct chanserv_channel *chanserv_channel_find(const char *channelname)
{
	for(unsigned int i = 0; i < chanserv_channels->count; i++)
	{
		struct chanserv_channel *cschan = chanserv_channels->data[i]->ptr;
		if(!strcasecmp(cschan->reg->channel, channelname))
			return cschan;
	}
	return NULL;
}

void chanserv_channel_free(struct chanserv_channel *cschan)
{
	ptrlist_free(cschan->events);
	dict_free(cschan->users);
	free(cschan);
}

static void chanserv_fetch_users(void *bound, struct chanserv_channel *cschan)
{
	if(cschan)
		return irc_send(sz_chanserv_fetch_info, cschan->reg->channel);

	for(unsigned int i = 0; i < chanserv_channels->count; i++)
	{
		struct chanserv_channel *cschan = chanserv_channels->data[i]->ptr;
		irc_send(sz_chanserv_fetch_info, cschan->reg->channel);
	}
	chanserv_timer_add();
}

void chanserv_channel_complete_hook(struct irc_channel *channel)
{
	struct chanreg *reg;
	struct irc_user *user;
	struct chanserv_channel *cschan;

	if(!(reg = chanreg_find(channel->name)))
		return;

	if(!(user = user_find(sz_chanserv_botname)))
		return;

	if(!channel_user_find(channel, user))
		return;

	cschan = chanserv_channel_create_fetch(reg);
}

void chanserv_report(const char *channel, const char *format, ...)
{
	va_list args;
	char buf[512];

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	irc_send("NOTICE @%s :%s", channel, buf);
	va_end(args);
}

void chanserv_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	const char *str;

	if((str = database_fetch(db_nodes, sz_chanserv_db_field_timestamp, DB_STRING)))
	{
		struct chanserv_channel *cschan = chanserv_channel_create(reg);
		cschan->last_event_ts = strtoul(str, NULL, 10);
	}
}

int chanserv_db_write(struct database_object *dbo, struct chanreg *reg)
{
	for(unsigned int i = 0; i < chanserv_channels->count; i++)
	{
		struct chanserv_channel *cschan = chanserv_channels->data[i]->ptr;
		if(cschan->reg == reg)
		{
			if(cschan->last_event_ts)
				database_obj_write_long(dbo, sz_chanserv_db_field_timestamp, cschan->last_event_ts);
			break;
		}
	}

	return 0;
}

void chanserv_get_access_callback(const char *channel, const char *nick, chanserv_access_f *callback)
{
	struct chanserv_access_request *req = malloc(sizeof(struct chanserv_access_request));
	memset(req, 0, sizeof(struct chanserv_access_request));

	req->channel = strdup(channel);
	req->callback = callback;
	req->nick = strdup(nick);
	req->access = CHANSERV_TIMEOUT;
	req->timer = timer_add(NULL, "chanserv_get_access", now + 1, (timer_f*)chanserv_access_request_timer, req, 0, 0);

	ptrlist_add(chanserv_access_requests, 0, req);
	irc_send(sz_chanserv_get_access, channel, nick);
}

// This function is required if for any reason whatsoever, ChanServ does not reply
// since chanserv_access_request_handle_raw and thus the callback wouldn't be called otherwise
void chanserv_access_request_timer(void *bound, struct chanserv_access_request *request)
{
	if(request->callback)
		request->callback(request->channel, request->nick, request->access);

	ptrlist_del_ptr(chanserv_access_requests, request);
}

void chanserv_access_request_handle_raw(const char *channel, const char *nick, int access)
{
	for(unsigned int i = 0; i < chanserv_access_requests->count; i++)
	{
		struct chanserv_access_request *req = chanserv_access_requests->data[i]->ptr;
		// Already triggered
		if(req->callback == NULL)
			continue;

		if(strcasecmp(req->channel, channel) != 0)
			continue;

		// This means if we have a channel but no nick, apply to all matching requests
		if(nick != NULL && strcasecmp(nick, req->nick) != 0)
			continue;

		req->access = access;
		req->callback(req->channel, req->nick, req->access);
		// Trigger timer
		req->timer->time = now;

		// To avoid being called again by the timer, unset the callback
		req->callback = NULL;
	}
}

void chanserv_access_request_free(struct chanserv_access_request *req)
{
	free(req->channel);
	free(req->nick);
	free(req);
}

void chanserv_timer_add()
{
	chanserv_timer_del();
	timer_add(this, sz_chanserv_users_timer_name, now + u_chanserv_fetch_users_interval, (timer_f*)chanserv_fetch_users, NULL, 0, 1);
}

void chanserv_timer_del()
{
	timer_del_boundname(this, sz_chanserv_users_timer_name);
}
