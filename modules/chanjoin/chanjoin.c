#include "global.h"
#include "module.h"
#include "chanjoin.h"
#include "timer.h"
#include "irc.h"
#include "irc_handler.h"
#include "chanuser.h"
#include "timer.h"
#include "conf.h"

MODULE_DEPENDS(NULL);

IMPLEMENT_LIST(cj_channel_ref_list, struct cj_channel_ref *)

static struct
{
	unsigned int max_tries;
	unsigned int rejoin_delay;
	const char *unban_command;
	const char *invite_command;
} chanjoin_conf;

static struct dict *cj_channels;

static void chanjoin_conf_reload();
static void chanjoin_join_timeout(struct cj_channel *chan, void *data);
static void chanjoin_addchan_tmr(struct cj_channel *chan, struct cj_channel_ref *ref);
static void chanjoin_ref_free(struct cj_channel_ref *ref);
static void chanjoin_free(struct cj_channel *chan);
static void module_unloaded(struct module *module);
IRC_HANDLER(join);
static void channel_complete_hook(struct irc_channel *irc_chan);
static void channel_del_hook(struct irc_channel *channel, const char *reason);
IRC_HANDLER(num_channelisfull);
IRC_HANDLER(num_inviteonlychan);
IRC_HANDLER(num_bannedfromchan);
IRC_HANDLER(num_badchannelkey);
static void chanjoin_rejoin_tmr(struct cj_channel *chan, void *data);


MODULE_INIT
{
	cj_channels = dict_create();
	dict_set_free_funcs(cj_channels, NULL, (dict_free_f*)chanjoin_free);

	reg_module_load_func(NULL, module_unloaded);
	chanuser_reg_channel_del_hook(channel_del_hook);
	chanuser_reg_channel_complete_hook(channel_complete_hook);
	reg_irc_handler("JOIN", join);
	reg_irc_handler("471", num_channelisfull);
	reg_irc_handler("473", num_inviteonlychan);
	reg_irc_handler("474", num_bannedfromchan);
	reg_irc_handler("475", num_badchannelkey);

	reg_conf_reload_func(chanjoin_conf_reload);
	chanjoin_conf_reload();
}

MODULE_FINI
{
	unreg_conf_reload_func(chanjoin_conf_reload);

	unreg_irc_handler("JOIN", join);
	unreg_irc_handler("471", num_channelisfull);
	unreg_irc_handler("473", num_inviteonlychan);
	unreg_irc_handler("474", num_bannedfromchan);
	unreg_irc_handler("475", num_badchannelkey);
	chanuser_unreg_channel_complete_hook(channel_complete_hook);
	chanuser_unreg_channel_del_hook(channel_del_hook);
	unreg_module_load_func(NULL, module_unloaded);

	dict_free(cj_channels);
}

static void chanjoin_conf_reload()
{
	char *str;
	chanjoin_conf.max_tries = ((str = conf_get("chanjoin/max_tries", DB_STRING)) ? atoi(str) : 3);
	chanjoin_conf.rejoin_delay = ((str = conf_get("chanjoin/rejoin_delay", DB_STRING)) ? atoi(str) : 10);
	chanjoin_conf.unban_command = conf_get("chanjoin/unban_command", DB_STRING);
	chanjoin_conf.invite_command = conf_get("chanjoin/invite_command", DB_STRING);

	if(chanjoin_conf.unban_command && (!strstr(chanjoin_conf.unban_command, "%s") ||
	   ((str = strstr(chanjoin_conf.unban_command, "%")) && *(str + 1) && strstr(str+1, "%"))))
	{
		log_append(LOG_WARNING, "chanjoin/unban_command must contain exactly one %%s and no other %% chars.");
		chanjoin_conf.unban_command = NULL;
	}

	if(chanjoin_conf.invite_command && (!strstr(chanjoin_conf.invite_command, "%s") ||
	   ((str = strstr(chanjoin_conf.invite_command, "%")) && *(str + 1) && strstr(str+1, "%"))))
	{
		log_append(LOG_WARNING, "chanjoin/invite_command must contain exactly one %%s and no other %% chars.");
		chanjoin_conf.invite_command = NULL;
	}
}

void chanjoin_addchan(const char *name, struct module *module, const char *key, chanjoin_success_f *success_func, chanjoin_error_f *error_func, void *ctx)
{
	struct cj_channel *chan;
	struct cj_channel_ref *ref;
	unsigned int found = 0;

	if(!(chan = chanjoin_find(name)))
	{
		chan = malloc(sizeof(struct cj_channel));
		memset(chan, 0, sizeof(struct cj_channel));
		chan->name = strdup(name);
		chan->channel = channel_find(name);
		chan->refs = cj_channel_ref_list_create();
		dict_insert(cj_channels, chan->name, chan);
	}

	assert(chanjoin_ref_find(chan, module, key) == NULL);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		ref = chan->refs->data[i];
		if(!ref->module && !strcasecmp(ref->module_name, module->name) &&
		   ((key == NULL && ref->key == key) || (key && ref->key && !strcasecmp(ref->key, key))))
		{
			debug("Found chanjoin record %s/%s with module %s", chan->name, ref->key, module->name);

			ref->success_func = success_func;
			ref->error_func = error_func;
			ref->ctx = ctx;
			ref->module_reloaded = 1;
			found = 1;
			break;
		}
	}

	if(!found)
	{
		ref = malloc(sizeof(struct cj_channel_ref));
		memset(ref, 0, sizeof(struct cj_channel_ref));
		ref->module = module;
		ref->module_name = strdup(module->name);
		ref->key = key ? strdup(key) : NULL;
		ref->success_func = success_func;
		ref->error_func = error_func;
		ref->ctx = ctx;
		cj_channel_ref_list_add(chan->refs, ref);
	}

	if(!chan->channel && chan->state != CJ_JOIN_PENDING && chan->state != CJ_REJOIN_PENDING)
	{
		chan->state = CJ_JOIN_PENDING;
		irc_send("JOIN %s", name);
		timer_del_boundname(chan, "chanjoin_join_timeout");
		timer_add(chan, "chanjoin_join_timeout", now + 30, (timer_f *)chanjoin_join_timeout, NULL, 0);
	}
	else if(chan->channel)
	{
		chan->state = CJ_JOINED;
		timer_add(chan, "chanjoin_joined", now, (timer_f *)chanjoin_addchan_tmr, ref, 0);
	}
}

static void chanjoin_join_timeout(struct cj_channel *chan, UNUSED_ARG(void *data))
{
	if(chan->state == CJ_JOIN_PENDING && !chan->channel)
	{
		log_append(LOG_WARNING, "Chanjoin for %s timed out.", chan->name);

		for(unsigned int i = 0; i < chan->refs->count; i++)
		{
			struct cj_channel_ref *ref = chan->refs->data[i];
			ref->error_func(chan, ref->key, ref->ctx, "timeout");
		}

		dict_delete(cj_channels, chan->name);
	}
}

static void chanjoin_addchan_tmr(struct cj_channel *chan, struct cj_channel_ref *ref)
{
	assert(chan->channel);
	ref->success_func(chan, ref->key, ref->ctx, !ref->module_reloaded);
	ref->module_reloaded = 0;
}

void chanjoin_delchan(const char *name, struct module *module, const char *key)
{
	struct cj_channel *chan;
	struct cj_channel_ref *ref;

	assert(chan = chanjoin_find(name));
	assert(ref = chanjoin_ref_find(chan, module, key));

	cj_channel_ref_list_del(chan->refs, ref);
	chanjoin_ref_free(ref);

	if(chan->refs->count == 0)
	{
		if(chan->state != CJ_INACTIVE)
			irc_send("PART %s :My work here is done.", chan->name);
		dict_delete(cj_channels, chan->name);
	}
}

struct cj_channel *chanjoin_find(const char *name)
{
	return dict_find(cj_channels, name);
}

struct cj_channel_ref *chanjoin_ref_find(struct cj_channel *chan, struct module *module, const char *key)
{
	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		// We search by module and key. If key is NULL we search for a NULL key.
		// A key should only be set if a module needs multiple refs on a single channel.
		if(ref->module == module && ((key == NULL && ref->key == key) || (key && ref->key && !strcasecmp(ref->key, key))))
			return ref;
	}

	return NULL;
}

static void chanjoin_ref_free(struct cj_channel_ref *ref)
{
	timer_del(NULL, "chanjoin_joined", 0, NULL, ref, TIMER_IGNORE_BOUND|TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC);
	free(ref->module_name);
	if(ref->key)
		free(ref->key);
	free(ref);
}

static void chanjoin_free(struct cj_channel *chan)
{
	timer_del(chan, NULL, 0, NULL, NULL, TIMER_IGNORE_NAME|TIMER_IGNORE_TIME|TIMER_IGNORE_FUNC|TIMER_IGNORE_DATA);
	for(unsigned int i = 0; i < chan->refs->count; i++)
		chanjoin_ref_free(chan->refs->data[i]);
	cj_channel_ref_list_free(chan->refs);
	free(chan->name);
	free(chan);
}

static void module_unloaded(struct module *module)
{
	// We only need to disable channel references if the module is being reloaded.
	// In other cases the module is supposed to delete the channel ref.
	if(!reloading_module)
		return;

	dict_iter(node, cj_channels)
	{
		struct cj_channel *chan = node->data;
		for(unsigned int i = 0; i < chan->refs->count; i++)
		{
			struct cj_channel_ref *ref = chan->refs->data[i];
			if(ref->module == module)
			{
				debug("Removing module %s for %s/%s", module->name, chan->name, ref->key);
				ref->module = NULL;
				ref->success_func = NULL;
				ref->error_func = NULL;
			}
		}
	}
}

IRC_HANDLER(join)
{
	struct cj_channel *chan;
	assert(argc > 1);
	if(!strcasecmp(src->nick, bot.nickname) && (chan = chanjoin_find(argv[1])))
		timer_del_boundname(chan, "chanjoin_join_timeout");
}

static void channel_complete_hook(struct irc_channel *irc_chan)
{
	struct cj_channel *chan;

	if(!(chan = chanjoin_find(irc_chan->name)))
		return;
	chan->channel = irc_chan;
	chan->state = CJ_JOINED;
	chan->tries = 0;

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->success_func(chan, ref->key, ref->ctx, !ref->module_reloaded);
		ref->module_reloaded = 0;
	}
}

static void channel_del_hook(struct irc_channel *irc_chan, const char *reason)
{
	struct cj_channel *chan;

	if(!(chan = chanjoin_find(irc_chan->name)))
		return;

	chan->channel = NULL;
	chan->tries++;
	if(chan->tries < chanjoin_conf.max_tries)
	{
		debug("Trying to rejoin %s", chan->name);
		timer_add(chan, "chanjoin_rejoin", now + chanjoin_conf.rejoin_delay, (timer_f *)chanjoin_rejoin_tmr, NULL, 0);
		return;
	}

	debug("Left channel %s (%s) and max. retries reached.", chan->name, reason);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->error_func(chan, ref->key, ref->ctx, reason);
	}

	dict_delete(cj_channels, chan->name);
}


IRC_HANDLER(num_channelisfull)
{
	struct cj_channel *chan;

	assert(argc > 2);
	if(!(chan = chanjoin_find(argv[2])))
		return;

	chan->tries++;
	if(chanjoin_conf.invite_command && chan->tries <= chanjoin_conf.max_tries)
	{
		chan->state = CJ_INACTIVE;
		irc_send(chanjoin_conf.invite_command, chan->name);
		timer_add(chan, "chanjoin_rejoin", now + chanjoin_conf.rejoin_delay, (timer_f *)chanjoin_rejoin_tmr, NULL, 0);
		return;
	}

	if(!chanjoin_conf.invite_command)
		debug("Userlimit for %s exceeded and no invite command defined.", chan->name);
	else if(chan->tries >= chanjoin_conf.max_tries)
		debug("Userlimit for %s exceeded and max. retries reached.", chan->name);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->error_func(chan, ref->key, ref->ctx, "limit");
	}

	dict_delete(cj_channels, chan->name);
}

IRC_HANDLER(num_inviteonlychan)
{
	struct cj_channel *chan;

	assert(argc > 2);
	if(!(chan = chanjoin_find(argv[2])))
		return;

	chan->tries++;
	if(chanjoin_conf.invite_command && chan->tries <= chanjoin_conf.max_tries)
	{
		chan->state = CJ_INACTIVE;
		irc_send(chanjoin_conf.invite_command, chan->name);
		timer_add(chan, "chanjoin_rejoin", now + chanjoin_conf.rejoin_delay, (timer_f *)chanjoin_rejoin_tmr, NULL, 0);
		return;
	}

	if(!chanjoin_conf.invite_command)
		debug("Needs invite for %s and no invite command defined.", chan->name);
	else if(chan->tries >= chanjoin_conf.max_tries)
		debug("Needs invite for %s and max. retries reached.", chan->name);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->error_func(chan, ref->key, ref->ctx, "inviteonly");
	}

	dict_delete(cj_channels, chan->name);
}

IRC_HANDLER(num_bannedfromchan)
{
	struct cj_channel *chan;

	assert(argc > 2);
	if(!(chan = chanjoin_find(argv[2])))
		return;

	chan->tries++;
	if(chanjoin_conf.unban_command && chan->tries <= chanjoin_conf.max_tries)
	{
		chan->state = CJ_INACTIVE;
		irc_send(chanjoin_conf.unban_command, chan->name);
		timer_add(chan, "chanjoin_rejoin", now + chanjoin_conf.rejoin_delay, (timer_f *)chanjoin_rejoin_tmr, NULL, 0);
		return;
	}

	if(!chanjoin_conf.unban_command)
		debug("Banned from %s and no unban command defined.", chan->name);
	else if(chan->tries >= chanjoin_conf.max_tries)
		debug("Banned from %s and max. retries reached.", chan->name);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->error_func(chan, ref->key, ref->ctx, "banned");
	}

	dict_delete(cj_channels, chan->name);
}

IRC_HANDLER(num_badchannelkey)
{
	struct cj_channel *chan;

	assert(argc > 2);
	if(!(chan = chanjoin_find(argv[2])))
		return;

	chan->tries++;
	if(chanjoin_conf.invite_command && chan->tries <= chanjoin_conf.max_tries)
	{
		chan->state = CJ_INACTIVE;
		irc_send(chanjoin_conf.invite_command, chan->name);
		timer_add(chan, "chanjoin_rejoin", now + chanjoin_conf.rejoin_delay, (timer_f *)chanjoin_rejoin_tmr, NULL, 0);
		return;
	}

	if(!chanjoin_conf.invite_command)
		debug("Needs key for %s and no invite command defined.", chan->name);
	else if(chan->tries >= chanjoin_conf.max_tries)
		debug("Needs key for %s and max. retries reached.", chan->name);

	for(unsigned int i = 0; i < chan->refs->count; i++)
	{
		struct cj_channel_ref *ref = chan->refs->data[i];
		ref->error_func(chan, ref->key, ref->ctx, "keyed");
	}

	dict_delete(cj_channels, chan->name);
}

static void chanjoin_rejoin_tmr(struct cj_channel *chan, UNUSED_ARG(void *data))
{
	chan->state = CJ_REJOIN_PENDING;
	irc_send("JOIN %s", chan->name);
}
