#include "global.h"
#include "module.h"
#include "modules/chanreg/chanreg.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "chanuser.h"
MODULE_DEPENDS("chanreg", NULL);

COMMAND(inactive);
COMMAND(unreg_dead);

MODULE_INIT
{
	DEFINE_COMMAND(self, "inactive", inactive, 0, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "unreg_dead", unreg_dead, 0, CMD_REQUIRE_AUTHED, "group(admins)");
}

MODULE_FINI
{
}

COMMAND(inactive)
{
	const struct dict *chanregs = chanreg_dict();
	dict_iter(node, chanregs)
	{
		struct irc_channel *chan;
		struct chanreg *reg = node->data;

		if(!(chan = channel_find(node->key)))
			reply("NOTJOINED: %s", node->key);
		else
		{
			if(!(chan->modes & MODE_REGISTERED))
			{
				if(dict_size(chan->users) == 1)
					reply("ONLYBOT: %s", node->key);
				else
					reply("NOCHANSERV: %s", node->key);
			}
		}

		if(!dict_size(reg->users))
			reply("NOUSERS: %s", node->key);
	}
	return 1;
}

COMMAND(unreg_dead)
{
	unsigned int nuke = 0;
	struct dict *chanregs = (struct dict *)chanreg_dict();
	dict_iter(node, chanregs)
	{
		struct irc_channel *chan;
		struct chanreg *reg = node->data;

		if(!(chan = channel_find(node->key)))
			continue;
		if(chan->modes & MODE_REGISTERED)
			continue;
		if(dict_size(chan->users) > 1)
			continue;

		reply("UNREG: %s", node->key);
		nuke++;

		dict_iter(node, reg->db_data)
		{
			struct chanreg_module *cmod = chanreg_module_find(node->key);
			if(cmod)
				chanreg_module_disable(reg, cmod, 1, CDR_UNREG);
		}

		dict_delete(chanregs, node->key);
	}

	reply("NUKED: %u channels", nuke);
	return 1;
}
