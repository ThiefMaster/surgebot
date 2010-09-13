#include "global.h"
#include "module.h"
#include "modules/help/help.h"

MODULE_DEPENDS("help", NULL);

MODULE_INIT
{
	help_load(self, "default.help");
}

MODULE_FINI
{

}
