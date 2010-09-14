#include "global.h"
#include "module.h"

#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

MODULE_DEPENDS(NULL);

MODULE_INIT
{
	char *txt_caps;
	cap_t caps;
	cap_value_t cap_list[1];

	caps = cap_get_proc();

	// Show current caps
	txt_caps = cap_to_text(caps, NULL);
	log_append(LOG_INFO, "Current caps: %s", txt_caps);
	cap_free(txt_caps);

	// Add CAP_NET_ADMIN to CAP_EFFECTICE and clear it from CAP_INERITABLE
	cap_list[0] = CAP_NET_ADMIN;
	if(cap_set_flag(caps, CAP_EFFECTIVE, 1, cap_list, CAP_SET) != 0)
		log_append(LOG_WARNING, "Could not set cap flag: %s", strerror(errno));
	if(cap_set_flag(caps, CAP_INHERITABLE, 1, cap_list, CAP_CLEAR) != 0)
		log_append(LOG_WARNING, "Could not set cap flag: %s", strerror(errno));

	// Apply new caps
	if(cap_set_proc(caps) != 0)
		log_append(LOG_WARNING, "Could not set caps: %s", strerror(errno));

	cap_free(caps);

	// Show new caps
	caps = cap_get_proc();
	txt_caps = cap_to_text(caps, NULL);
	log_append(LOG_INFO, "Mew caps: %s", txt_caps);
	cap_free(txt_caps);
	cap_free(caps);
}

MODULE_FINI
{

}

