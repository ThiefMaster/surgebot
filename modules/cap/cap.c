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

	txt_caps = cap_to_text(cap_get_proc(), NULL);
	log_append(LOG_INFO, "Current caps: %s", txt_caps);
	cap_free(txt_caps);

	caps = cap_from_text("CAP_NET_ADMIN=ep");
	if(!caps)
		log_append(LOG_ERROR, "cap_from_text() failed");
	else if(cap_set_proc(caps) != 0)
		log_append(LOG_WARNING, "Could not set caps: %s", strerror(errno));

	txt_caps = cap_to_text(cap_get_proc(), NULL);
	log_append(LOG_INFO, "Mew caps: %s", txt_caps);
	cap_free(txt_caps);

	/*
	debug("uid: %d, euid: %d", getuid(), geteuid());
	if(geteuid() == 0)
	{
		log_append(LOG_WARNING, "Running with ROOT privileges; setting CAP_NET_ADMIN and dropping root privs");
		cap_t caps = cap_from_text("CAP_NET_ADMIN=ep");
		if(!caps)
			log_append(LOG_WARNING, "Could not get CAP_NET_ADMIN");
		else
		{
			if(cap_set_proc(caps) != 0)
				log_append(LOG_WARNING, "Could not set CAP_NET_ADMIN (while root)");
		}

		setegid(getgid());
		seteuid(getuid());

		if(cap_set_proc(caps) != 0)
			log_append(LOG_WARNING, "Could not set CAP_NET_ADMIN (after dropping root)");
		cap_free(caps);

		debug("uid: %d, euid: %d", getuid(), geteuid());

		// re-enable core dumps
		if(prctl(PR_SET_DUMPABLE, 1) == -1)
			log_append(LOG_WARNING, "Could not re-enable core dumps: %s", strerror(errno));
	}
	*/
}

MODULE_FINI
{

}

