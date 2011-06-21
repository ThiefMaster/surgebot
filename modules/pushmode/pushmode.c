#include "global.h"
#include "module.h"
#include "stringbuffer.h"
#include "dict.h"
#include "ptrlist.h"
#include "timer.h"
#include "irc.h"
#include "chanuser.h"
#include "modules/pushmode/pushmode.h"

MODULE_DEPENDS(NULL);

struct delayed_mode {
	char *target;
	char *mode;
};

static void pushmode_free(struct delayed_mode *mode);
static void pushmode_add_timer();
static void pushmode_del_timer();
static void pushmode_execute(void *bound, void *data);

static struct dict *pushmode_dict;
static long max_modes = 3;
static struct module *this;

MODULE_INIT
{
	this = self;
	pushmode_dict = dict_create();
	dict_set_free_funcs(pushmode_dict, (dict_free_f*)free, (dict_free_f*)ptrlist_free);

	// determine maximum amount of modes to set at once
	char *server_modes = dict_find(bot.server.capabilities, "MODES");
	if(server_modes) {
		long l = strtol(server_modes, NULL, 0);
		if(l > 0) {
			debug("pushmode: allowed to set %ld modes at once (MODES=%s)", l, server_modes);
			max_modes = l;
			return;
		}
		else {
			debug("pushmode: invalid modes line (MODES=%s converted to %ld)", server_modes, l);
		}
	}
	else {
		printf("pushmode: server did not provide a MODES directive.");
	}

	debug("pushmode: Using default of %ld modes to send per line.", max_modes);
}

MODULE_FINI
{
	pushmode_del_timer();
	dict_free(pushmode_dict);
}

static void pushmode_add_timer()
{
	// only if timer not yet added
	if(!timer_exists_boundname(this, "pushmode")) {
		extern time_t now;
		timer_add(this, "pushmode", now, pushmode_execute, NULL, 0, 0);
	}
}

static void pushmode_del_timer()
{
	timer_del_boundname(this, "pushmode");
}

static void pushmode_free(struct delayed_mode *mode)
{
	free(mode->mode);
	free(mode->target);
	free(mode);
}

void pushmode(struct irc_channel *channel, char *mode, const char *target)
{
	// is the mode and target setting a usermode
	// -> if so, check if it has already been set
	char *mode_bak = mode;
	char sign = '+';
	if(*mode_bak == '+' || *mode_bak == '-') {
		sign = *mode_bak++;
	}
	// make sure there is exactly one mode specifier
	assert(strlen(mode_bak) == 1);

	if(*mode_bak == 'v' || *mode_bak == 'o') {
		struct irc_user *ircuser = user_find(target);
		if(!ircuser) {
			debug("pushmode: User '%s' does not exist, tried to set usermode %s", target, mode);
			return;
		}
		struct irc_chanuser *chanuser = channel_user_find(channel, ircuser);
		if(!chanuser) {
			debug("pushmode: User '%s' is not on %s, can't set usermode %s", target, channel->name, mode);
			return;
		}

		int flag = 0;
		if(*mode_bak == 'v') flag = MODE_VOICE;
		else				 flag = MODE_OP;

		if((sign == '+' && (chanuser->flags & flag)) || (sign == '-' && !(chanuser->flags & flag))) {
			// mode already set, ignore
			return;
		}
	}

	// Is there a list of modes for this channel?
	struct ptrlist *chanlist = dict_find(pushmode_dict, channel->name);
	if(!chanlist) {
		chanlist = ptrlist_create();
		ptrlist_set_free_func(chanlist, (ptrlist_free_f*)pushmode_free);
		dict_insert(pushmode_dict, strdup(channel->name), chanlist);
	}

	struct delayed_mode *delayed_mode = malloc(sizeof(struct delayed_mode));
	delayed_mode->mode = strdup(mode);
	delayed_mode->target = target ? strdup(target) : NULL;
	ptrlist_add(chanlist, 0, delayed_mode);
	for(size_t i = 0; i < chanlist->count; ++i) {
		struct delayed_mode *mode = chanlist->data[i]->ptr;
	}

	pushmode_add_timer();
}

static void pushmode_execute(void *bound, void *data)
{
	struct stringbuffer *modes = stringbuffer_create();
	struct stringbuffer *targets = stringbuffer_create();

	dict_iter(node, pushmode_dict)
	{
		char *channel = node->key;
		struct ptrlist *chanlist = node->data;
		// collect modes
		for(size_t i = 0; i < chanlist->count; ++i) {
			struct delayed_mode *mode = chanlist->data[i]->ptr;
			// append to modes
			stringbuffer_append_string(modes, mode->mode);
			// append target
			if(targets->len > 0) {
				stringbuffer_append_char(targets, ' ');
			}
			stringbuffer_append_string(targets, mode->target);

			// every max_modes modes, flush and send actual mode command
			if(!((i+1) % max_modes)) {
				irc_send("MODE %s %s %s", channel, modes->string, targets->string);
				stringbuffer_erase(modes, 0, modes->len);
				stringbuffer_erase(targets, 0, targets->len);
			}
		}

		if(modes->len > 0) {
			irc_send("MODE %s %s %s", channel, modes->string, targets->string);
			stringbuffer_erase(modes, 0, modes->len);
			stringbuffer_erase(targets, 0, targets->len);
		}
	}

	stringbuffer_free(modes);
	stringbuffer_free(targets);
	// all pushed modes have been issued, clear dict
	dict_clear(pushmode_dict);
}
