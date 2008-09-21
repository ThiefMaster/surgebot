#include "global.h"
#include "account.h"
#include "session.h"
#include "ajax-app.h"
#include "static.h"
#include "module.h"
#include "app-core.h"

MODULE_DEPENDS("httpd", "tools", NULL);

MODULE_INIT
{
	session_init();
	static_init();
	ajaxapp_init();
}

MODULE_FINI
{
	ajaxapp_fini();
	static_fini();
	session_fini();
}
