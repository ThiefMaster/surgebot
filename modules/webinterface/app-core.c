#include "global.h"
#include "account.h"
#include "session.h"
#include "ajax-app.h"
#include "static.h"
#include "rules.h"
#include "module.h"
#include "app-core.h"

MODULE_DEPENDS("httpd", "tools", "parser", NULL);

MODULE_INIT
{
	rules_init();
	session_init();
	static_init();
	ajaxapp_init();
}

MODULE_FINI
{
	ajaxapp_fini();
	static_fini();
	session_fini();
	rules_fini();
}
