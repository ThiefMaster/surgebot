#include "global.h"
#include "module.h"
#include "conf.h"

#include <locale.h>

MODULE_DEPENDS(NULL);

static void locale_conf_reload();

MODULE_INIT
{
	reg_conf_reload_func(locale_conf_reload);
	locale_conf_reload();
}

MODULE_FINI
{
	unreg_conf_reload_func(locale_conf_reload);
}

static void locale_conf_reload()
{
	char *str;
	str = conf_get("locale/lc_ctype", DB_STRING);
	str = setlocale(LC_CTYPE, str ? str : "C");
	log_append(LOG_INFO, "Changed locale LC_CTYPE to %s", str ? str : "(null)");
}
