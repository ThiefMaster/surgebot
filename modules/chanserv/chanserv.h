#ifndef __CHANSERV_H__
#define __CHANSERV_H__

#include "global.h"
#include "module.h"
#include "irc_handler.h"
#include "dict.h"
#include "tokenize.h"
#include "irc.h"
#include "chanuser.h"
#include "stringbuffer.h"
#include "irc_handler.h"
#include "timer.h"
#include "table.h"
#include <time.h>
#include <string.h>

// Module's dependencies' headers
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "modules/tools/tools.h"
#include "modules/db/db.h"

// Headers for this module
#include "modules/chanserv/chanserv_channels.h"
#include "modules/chanserv/chanserv_users.h"
#include "modules/chanserv/chanserv_events.h"

static const char * const	sz_chanserv_botname					= "ChanServ";
static const char * const	sz_chanserv_chanmod_events_name		= "EventLog";
static const char * const	sz_chanserv_db_field_timestamp		= "last_event_ts";
static const char * const	sz_chanserv_db_table				= "chanserv_events";
static const char * const	sz_chanserv_event_timer_name		= "chanserv_update_events";
static const unsigned int	 u_chanserv_fetch_events_amount		= 1024;
static const unsigned int	 u_chanserv_fetch_events_interval	= 60;
static const char * const	sz_chanserv_fetch_events			= "ChanServ %s events %u";
static const char * const	sz_chanserv_fetch_info				= "ChanServ %s info";
static const char * const	sz_chanserv_fetch_names				= "ChanServ %s names";
static const char * const	sz_chanserv_fetch_users				= "ChanServ %s users";
static const unsigned int	 u_chanserv_fetch_users_interval	= 60 * 60;
static const char * const	sz_chanserv_get_access				= "ChanServ %s a %s";
static const char * const	sz_chanserv_users_timer_name		= "chanserv_update_users";

const struct column_desc *chanserv_event_table_cols();
unsigned long parse_chanserv_duration(const char *);

struct chanreg_module *cmod;
struct module *this;

#endif // __CHANSERV_H__
