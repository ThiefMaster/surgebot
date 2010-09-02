#include "global.h"
#include "module.h"
#include "timer.h"
#include "modules/srvx/srvx.h"

MODULE_DEPENDS("srvx", NULL);

static struct module *this;
static unsigned long last_usercount = 0, last_usercount_disp = 0;

static void update_users_tmr(void *bound, void *data);
static void count_ipv6_cb(struct srvx_request *r, void *ctx);

MODULE_INIT
{
	this = self;
	timer_add(this, "update_users", now + 15, update_users_tmr, NULL, 0, 1);
}

MODULE_FINI
{
	timer_del_boundname(this, "update_users");
}

static void update_users_tmr(void *bound, void *data)
{
	srvx_send(count_ipv6_cb, "OPSERV count_ipv6");
}

static void count_ipv6_cb(struct srvx_request *r, void *ctx)
{
	timer_add(this, "update_users", now + 120, update_users_tmr, NULL, 0, 1);

	if(!r)
		return;

	assert(r->count == 1);
	struct srvx_response_line *line = r->lines[0];
	if(!match("Found * matches.", line->msg))
	{
		unsigned long count = strtoul(line->msg + 6, NULL, 10);
		debug("IPv6 usercount changed: %lu -> %lu", last_usercount, count);
		last_usercount = count;

		if(abs(count - last_usercount_disp) > 2)
		{
			debug("Displaying new IPv6 usercount %lu (old: %lu)", count, last_usercount_disp);
			last_usercount_disp = count;
			srvx_sendonly("CHANSERV TOPIC #ipv6 %lu", count);
		}
	}
}
