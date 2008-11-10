#include "global.h"
#include "account.h"
#include "session.h"
#include "ajax-app.h"
#include "static.h"
#include "usercount.h"
#include "rules.h"
#include "module.h"
#include "module-config.h"
#include "app-core.h"

MODULE_DEPENDS("httpd", "tools", "parser",
#ifdef WITH_MODULE_chanserv
		"chanserv",
#endif
		NULL);

MODULE_INIT
{
	rules_init();
	session_init();
	static_init();
	ajaxapp_init();
	usercount_init();
}

MODULE_FINI
{
	usercount_fini();
	ajaxapp_fini();
	static_fini();
	session_fini();
	rules_fini();
}
