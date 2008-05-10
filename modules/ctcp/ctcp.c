#include "global.h"
#include "module.h"
#include "irc.h"
#include "conf.h"
#include "tools.h"
#include "irc_handler.h"

MODULE_DEPENDS(NULL);

void ctcp_conf_reload();

static struct
{
	const char *version;
} ctcp_conf;

IRC_HANDLER(privmsg);

MODULE_INIT
{
	reg_irc_handler("PRIVMSG", privmsg);
	reg_conf_reload_func(ctcp_conf_reload);

	ctcp_conf_reload();
}

MODULE_FINI
{
	unreg_conf_reload_func(ctcp_conf_reload);
	unreg_irc_handler("PRIVMSG", privmsg);
}

void ctcp_conf_reload()
{
	ctcp_conf.version = conf_get("ctcp/version", DB_STRING);
}

IRC_HANDLER(privmsg)
{
	char *request;
	
	assert(argc > 2);

	if(!src || strcasecmp(argv[1], bot.nickname))
		return;
	
	// Is this a CTCP request (first character is \001)?
	debug("argc: %d; argv[2] = %s (%ld chars)", argc, argv[2], strlen(argv[2]));
	if(*argv[2] != '\001')
		return;
	
	request = strdup(argv[2] + 1);
	debug("request points to %p and got %s (%ld chars)", request, request, strlen(request));
	
	if(ctcp_conf.version && !strncasecmp(request, "version", 7))
		reply("\001VERSION %s\001", ctcp_conf.version);
	
	else if(!strncasecmp(request, "time", 4))
		reply("\001TIME %s\001", time2string(time(NULL)));
	
	free(request);
}
