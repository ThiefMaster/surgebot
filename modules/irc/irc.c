#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "chanuser.h"
#include "irc.h"

MODULE_DEPENDS("commands", "help", NULL);

COMMAND(whois);
COMMAND(chaninfo);
COMMAND(say);
COMMAND(emote);

MODULE_INIT
{
	help_load(self, "irc.help");

	DEFINE_COMMAND(self, "whois",		whois,		2, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "chaninfo",	chaninfo,	1, CMD_REQUIRE_AUTHED | CMD_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "say",		say,		2, CMD_REQUIRE_AUTHED | CMD_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "emote",		emote,		2, CMD_REQUIRE_AUTHED | CMD_ACCEPT_CHANNEL, "group(admins)");
}

MODULE_FINI
{

}


COMMAND(whois)
{
	struct irc_user *target;
	struct stringlist *channel_list;
	struct stringlist *lines;

	if(!(target = user_find(argv[1])))
	{
		reply("User with nick $b%s$b does not exist or is unknown.", argv[1]);
		return 0;
	}

	reply("Information about $b%s$b!%s@%s", target->nick, target->ident, target->host);
	if(target->info)
		reply(" Info: %s", target->info);
	reply(" Account: %s", (target->account ? target->account->name : "(not authed)"));

	reply(" Channels (%d):", dict_size(target->channels));
	channel_list = stringlist_create();
	dict_iter(node, target->channels)
	{
		struct irc_chanuser *chanuser = node->data;
		unsigned int len = 0, bufsize;
		bufsize = strlen(chanuser->channel->name) + 2; // chanlen + modechar + '\0'
		char *buf = malloc(bufsize);

		if(chanuser->flags & MODE_OP)
			buf[len++] = '@';
		else if(chanuser->flags & MODE_VOICE)
			buf[len++] = '+';
		safestrncpy(buf + len, chanuser->channel->name, bufsize - 1);
		len += strlen(chanuser->channel->name);
		buf[len] = '\0';
		stringlist_add(channel_list, buf);
	}

	stringlist_sort_irc(channel_list);
	lines = stringlist_to_irclines(src->nick, channel_list);
	for(unsigned int i = 0; i < lines->count; i++)
		reply("  %s", lines->data[i]);
	stringlist_free(channel_list);
	stringlist_free(lines);
	return 1;
}

COMMAND(chaninfo)
{
	struct stringlist *user_list;
	struct stringlist *lines;

	if(!channel)
	{
		reply("No/Invalid channel specified.");
		return 0;
	}

	reply("Information about $b%s$b:", channel->name);
	reply(" Modes: +%s", chanmodes2string(channel->modes, channel->limit, channel->key));
	reply(" Topic: %s", channel->topic ? channel->topic : "(none)");

	reply(" Users (%d):", dict_size(channel->users));
	user_list = stringlist_create();
	dict_iter(node, channel->users)
	{
		struct irc_chanuser *chanuser = node->data;
		unsigned int len = 0, bufsize;
		bufsize = strlen(chanuser->user->nick) + 2; // nicklen + modechar + '\0'
		char *buf = malloc(bufsize);

		if(chanuser->flags & MODE_OP)
			buf[len++] = '@';
		else if(chanuser->flags & MODE_VOICE)
			buf[len++] = '+';
		safestrncpy(buf + len, chanuser->user->nick, bufsize - 1);
		len += strlen(chanuser->user->nick);
		buf[len] = '\0';
		stringlist_add(user_list, buf);
	}

	stringlist_sort_irc(user_list);
	lines = stringlist_to_irclines(src->nick, user_list);
	for(unsigned int i = 0; i < lines->count; i++)
		reply("  %s", lines->data[i]);
	stringlist_free(user_list);
	stringlist_free(lines);

	if(dict_size(channel->bans))
	{
		reply(" Bans (%d):", dict_size(channel->bans));
		dict_iter(node, channel->bans)
		{
			struct irc_ban *ban = node->data;
			reply("  %s", ban->mask);
		}
	}

	return 1;
}

COMMAND(say)
{
	char *target, *str;
	unsigned int offset;

	if(!channel && argc < 3)
		return -1;

	if(channel)
	{
		target = channel->name;
		offset = 1;
	}
	else
	{
		target = argv[1];
		offset = 2;
	}

	str = untokenize(argc - offset, argv + offset, " ");
	irc_send("PRIVMSG %s :%s", target, str);
	free(str);
	return 1;
}

COMMAND(emote)
{
	char *target, *str;
	unsigned int offset;

	if(!channel && argc < 3)
		return -1;

	if(channel)
	{
		target = channel->name;
		offset = 1;
	}
	else
	{
		target = argv[1];
		offset = 2;
	}

	str = untokenize(argc - offset, argv + offset, " ");
	irc_send("PRIVMSG %s :\001ACTION %s\001", target, str);
	free(str);
	return 1;
}

