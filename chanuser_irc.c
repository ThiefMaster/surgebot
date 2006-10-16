#include "global.h"
#include "chanuser_irc.h"
#include "chanuser.h"
#include "irc_handler.h"
#include "irc.h"
#include "stringlist.h"

#define CHANUSER_IRC_HANDLER(NAME)	static int __chanuser_irc_handler_ ## NAME(int argc, char **argv, struct irc_source *src, const char *raw_line)

typedef int (chanuser_irc_handler_f)(int argc, char **argv, struct irc_source *src, const char *raw_line);


static struct dict* chanuser_irc_handlers = NULL;

int check_burst(struct irc_channel *channel, const char *line);
void parse_channel_modes(struct irc_channel *channel, int argc, char **argv);
static void setup_handlers();


void chanuser_irc_init()
{
	chanuser_irc_handlers = dict_create();
	setup_handlers();
}

void chanuser_irc_fini()
{
	dict_free(chanuser_irc_handlers);
	chanuser_irc_handlers = NULL;
}

int chanuser_irc_handler(int argc, char **argv, struct irc_source *src, const char *raw_line)
{
	chanuser_irc_handler_f *func;
	assert_return(chanuser_irc_handlers, 0);

	if((func = dict_find(chanuser_irc_handlers, argv[0])) == NULL)
		return 0;

	if(func(argc, argv, src, raw_line) == -1)
	{
		debug("\033[1;33mDelaying message:\033[0m %s", raw_line);
		return -1;
	}

	return 0;
}

int check_burst(struct irc_channel *channel, const char *line)
{
	if(channel && channel->burst_state != BURST_FINISHED)
	{
		stringlist_add(channel->burst_lines, strdup(line));
		return 1;
	}
	else if(channel == NULL && bot.burst_count)
	{
		stringlist_add(bot.burst_lines, strdup(line));
		return 1;
	}

	return 0;
}

void parse_channel_modes(struct irc_channel *channel, int argc, char **argv)
{
	struct irc_user *user;
	struct irc_chanuser *chanuser;
	int flags, add = 1, arg_pos = 1;
	char *arg;
	char *modes = argv[0];

#define do_chan_mode(MODE)	do { if(add) channel->modes |= (MODE_ ## MODE); else channel->modes &= ~(MODE_ ## MODE); } while(0)
	while(*modes)
	{
		switch(*modes)
		{
			case '+':
				add = 1;
				break;
			case '-':
				add = 0;
				break;

			case 'k':
				assert_break(arg_pos < argc);
				arg = argv[arg_pos++];
				do_chan_mode(KEYED);
				channel_set_key(channel, add ? arg : NULL);
				break;

			case 'l':
				if(add)
					assert_break(arg_pos < argc);
				do_chan_mode(LIMIT);
				channel_set_limit(channel, add ? atoi(argv[arg_pos++]) : 0);
				break;

			// modes not needing any additonal code
			case 'i': do_chan_mode(INVITEONLY); break;
			case 'p': do_chan_mode(PRIVATE); break;
			case 's': do_chan_mode(SECRET); break;
			case 'm': do_chan_mode(MODERATED); break;
			case 't': do_chan_mode(TOPICLIMIT); break;
			case 'n': do_chan_mode(NOPRIVMSGS); break;
			case 'D': do_chan_mode(DELJOINS); break;
			case 'd': do_chan_mode(WASDELJOIN); break;
			case 'r': do_chan_mode(REGONLY); break;
			case 'c': do_chan_mode(NOCOLOUR); break;
			case 'C': do_chan_mode(NOCTCP); break;
			case 'z': do_chan_mode(REGISTERED); break;

			// modes affecting users
			case 'o':
			case 'v':
				assert_break(arg_pos < argc);
				assert_break(user = user_find(argv[arg_pos++]));
				assert_break(chanuser = channel_user_find(channel, user));

				flags = 0;
				if(*modes == 'o')
					flags = MODE_OP;
				else if(*modes == 'v')
					flags = MODE_VOICE;

				if(add)
					chanuser->flags |= flags;
				else
					chanuser->flags &= ~flags;

				debug("Mode change in %s for %s: %c%c", channel->name, user->nick, add ? '+' : '-', *modes);
				break;

			case 'b':
				assert_break(arg_pos < argc);
				arg = argv[arg_pos++];

				if(add)
					channel_ban_add(channel, arg);
				else
					channel_ban_del(channel, arg);

				debug("Ban in %s for %s %s", channel->name, arg, add ? "added" : "removed");
				break;
		}
		modes++;
	}
#undef do_chan_mode
}


CHANUSER_IRC_HANDLER(join)
{
	struct irc_channel *channel;
	struct irc_user *user;
	assert_return(argc > 1, 0);

	if(!strcasecmp(src->nick, bot.nickname))
	{
		debug("We joined %s, adding channel", argv[1]);
		channel = channel_add(argv[1], 1);
	}
	else
	{
		channel = channel_find(argv[1]);
		debug("%s joined %s", src->nick, argv[1]);
		assert_return(channel, 0);

		if(check_burst(channel, raw_line))
			return -1;
	}

	if((user = user_find(src->nick)) == NULL)
		user = user_add(src->nick, src->ident, src->host);
	else
		user_complete(user, src->ident, src->host);

	channel_user_add(channel, user, 0);
	return 0;
}

CHANUSER_IRC_HANDLER(part)
{
	struct irc_channel *channel;
	struct irc_user *user;
	assert_return(argc > 1, 0);
	assert_return(channel = channel_find(argv[1]), 0);

	if(check_burst(channel, raw_line))
		return -1;

	assert_return(user = user_find(src->nick), 0);
	user_complete(user, src->ident, src->host);

	if(!strcasecmp(src->nick, bot.nickname))
	{
		debug("We left %s, deleting channel", argv[1]);
		channel_del(channel);
	}
	else
	{
		debug("%s left %s", src->nick, argv[1]);
		channel_user_del(channel, user, 1);
	}
	return 0;
}

CHANUSER_IRC_HANDLER(kick)
{
	struct irc_channel *channel;
	struct irc_user *user, *victim;
	assert_return(argc > 2, 0);
	assert_return(channel = channel_find(argv[1]), 0);

	if(check_burst(channel, raw_line))
		return -1;

	assert_return(user = user_find(src->nick), 0);
	user_complete(user, src->ident, src->host);
	assert_return(victim = user_find(argv[2]), 0);

	if(!strcasecmp(argv[2], bot.nickname))
	{
		debug("We were kicked from %s by %s, deleting channel", argv[1], src->nick);
		channel_del(channel);
	}
	else
	{
		debug("%s was kicked from %s by %s", argv[2], argv[1], src->nick);
		channel_user_del(channel, user, 1);
	}
	return 0;
}

CHANUSER_IRC_HANDLER(nick)
{
	struct irc_user *user;
	assert_return(argc > 1, 0);
	assert_return(user = user_find(src->nick), 0);
	user_complete(user, src->ident, src->host);

	if(!strcasecmp(src->nick, bot.nickname))
	{
		debug("Our nick changed from %s to %s", src->nick, argv[1]);
		free(bot.nickname);
		bot.nickname = strdup(argv[1]);
	}
	else
	{
		debug("%s changed his nick to %s", src->nick, argv[1]);
	}

	user_rename(user, argv[1]);
	return 0;
}

CHANUSER_IRC_HANDLER(quit)
{
	struct irc_user *user;
	assert_return(user = user_find(src->nick), 0);

	debug("%s has quit: %s", src->nick, (argc > 1 ? argv[1] : "(no message)"));
	user_del(user);
	return 0;
}

CHANUSER_IRC_HANDLER(mode)
{
	struct irc_channel *channel;
	char *modestr;
	assert_return(argc > 2, 0);

	if(!IsChannelName(argv[1])) // usermode
		return 0;

	assert_return(channel = channel_find(argv[1]), 0);
	if(check_burst(channel, raw_line))
		return -1;

	modestr = untokenize(argc - 2, argv + 2, " ");
	debug("Mode change in %s: %s", argv[1], modestr);
	free(modestr);

	parse_channel_modes(channel, argc - 2, argv + 2);
	return 0;
}

CHANUSER_IRC_HANDLER(topic)
{
	struct irc_channel *channel;
	assert_return(argc > 2, 0);
	assert_return(channel = channel_find(argv[1]), 0);
	if(check_burst(channel, raw_line))
		return -1;

	debug("Topic of %s changed to %s", argv[1], argv[2]);
	channel_set_topic(channel, argv[2]);
	return 0;
}

/*
 * Handler for PRIVMSG/NOTICE to complete a user if his data is missing for some
 * reason so we can be sure in privmsg handlers that user->ident/host are known
 */
CHANUSER_IRC_HANDLER(msg)
{
	struct irc_user *user;

	if(src == NULL || src->ident == NULL || src->host == NULL)
		return 0;

	if((user = user_find(src->nick)))
		user_complete(user, src->ident, src->host);

	return 0;
}

CHANUSER_IRC_HANDLER(num_endofwho)
{
	struct irc_channel *channel;
	assert_return(argc > 2, 0);

	if(IsChannelName(argv[2]) && (channel = channel_find(argv[2])) && channel->burst_state == BURST_WHO)
	{
		channel->burst_state = BURST_FINISHED;
		bot.burst_count--;
		debug("Bursting %s finished", argv[2]);

		if(channel->burst_lines->count)
		{
			for(int i = 0; i < channel->burst_lines->count; i++)
			{
				debug("Parsing delayed line from channel[%s]->burst_lines: %s", channel->name, channel->burst_lines->data[i]);
				irc_parse_line(channel->burst_lines->data[i]);
			}

			stringlist_free(channel->burst_lines);
			channel->burst_lines = stringlist_create();
		}

		if(bot.burst_count == 0 && bot.burst_lines->count)
		{
			for(int i = 0; i < bot.burst_lines->count; i++)
			{
				debug("Parsing delayed line from bot.burst_lines: %s", bot.burst_lines->data[i]);
				irc_parse_line(bot.burst_lines->data[i]);
			}

			stringlist_free(bot.burst_lines);
			bot.burst_lines = stringlist_create();
		}
	}
	return 0;
}

CHANUSER_IRC_HANDLER(num_channelmodeis)
{
	struct irc_channel *channel;
	char *modestr;
	assert_return(argc > 3, 0);
	assert_return(IsChannelName(argv[2]), 0);
	assert_return(channel = channel_find(argv[2]), 0);

	modestr = untokenize(argc - 3, argv + 3, " ");
	debug("Modes of %s are %s", argv[2], modestr);
	free(modestr);

	parse_channel_modes(channel, argc - 3, argv + 3);

	if(channel->burst_state == BURST_MODES)
	{
		irc_send("MODE %s b", channel->name);
		channel->burst_state = BURST_BANS;
	}
	return 0;
}

CHANUSER_IRC_HANDLER(num_topic)
{
	struct irc_channel *channel;
	assert_return(argc > 3, 0);
	assert_return(channel = channel_find(argv[2]), 0);

	debug("Topic of %s is %s", argv[2], argv[3]);
	channel_set_topic(channel, argv[3]);
	return 0;
}

CHANUSER_IRC_HANDLER(num_namereply)
{
	struct irc_channel *channel;
	int namec;
	char *namev[MAXARG];
	char *names_dup;
	assert_return(argc > 4, 0);
	assert_return(channel = channel_find(argv[3]), 0);

	names_dup = strdup(argv[4]);
	namec = tokenize(names_dup, namev, MAXARG, ' ', 0);

	for(int i = 0; i < namec; i++)
	{
		struct irc_user *user;
		struct irc_chanuser *chanuser;
		int flags = 0;

		if(*namev[i] == '@')
			flags = MODE_OP;
		else if(*namev[i] == '+')
			flags = MODE_VOICE;

		if(flags)
			namev[i]++;

		if((user = user_find(namev[i])) == NULL)
			user = user_add_nick(namev[i]);

		if((chanuser = channel_user_find(channel, user)))
			chanuser->flags = flags;
		else
			channel_user_add(channel, user, flags);
	}

	free(names_dup);
	return 0;
}

CHANUSER_IRC_HANDLER(num_whospecial)
{
	struct irc_user *user;
	assert_return(argc > 6, 0);

	if(strcmp(argv[2], "1")) // query type must be 1
		return 0;

	assert_return(user = user_find(argv[5]), 0);
	user_complete(user, argv[3], argv[4]);
	user_set_info(user, argv[6]);
	debug("Complete info for %s: %s@%s / %s", user->nick, user->ident, user->host, user->info);
	return 0;
}

CHANUSER_IRC_HANDLER(num_endofnames)
{
	struct irc_channel *channel;
	assert_return(argc > 2, 0);
	assert_return(channel = channel_find(argv[2]), 0);

	if(channel->burst_state == BURST_NAMES)
	{
		irc_send("MODE %s", channel->name);
		channel->burst_state = BURST_MODES;
	}
	return 0;
}

CHANUSER_IRC_HANDLER(num_banlist)
{
	struct irc_channel *channel;
	assert_return(argc > 3, 0);
	assert_return(channel = channel_find(argv[2]), 0);

	channel_ban_add(channel, argv[3]);
	debug("Banned in %s: %s", argv[2], argv[3]);
	return 0;
}

CHANUSER_IRC_HANDLER(num_endofbanlist)
{
	struct irc_channel *channel;
	assert_return(argc > 2, 0);
	assert_return(channel = channel_find(argv[2]), 0);

	if(channel->burst_state == BURST_BANS)
	{
		// TODO: We should write a module for WHOing that ensures unique query types, generates the WHO
		//       request and parses the results
		irc_send("WHO %s %%tuhnr,1", channel->name);
		channel->burst_state = BURST_WHO;
	}

	return 0;
}

CHANUSER_IRC_HANDLER(num_hosthidden)
{
	struct irc_user *user;
	assert_return(argc > 2, 0);
	assert_return(!strcasecmp(argv[1], bot.nickname), 0);

	if((user = user_find(argv[1])))
	{
		if(user->host)
			free(user->host);
		user->host = strdup(argv[2]);
	}

	return 0;
}


static void setup_handlers()
{
#define set_chanuser_irc_handler(CMD, NAME)	dict_insert(chanuser_irc_handlers, CMD, __chanuser_irc_handler_ ## NAME);
	set_chanuser_irc_handler("JOIN", join);
	set_chanuser_irc_handler("PART", part);
	set_chanuser_irc_handler("KICK", kick);
	set_chanuser_irc_handler("NICK", nick);
	set_chanuser_irc_handler("QUIT", quit);
	set_chanuser_irc_handler("MODE", mode);
	set_chanuser_irc_handler("TOPIC", topic);
	set_chanuser_irc_handler("PRIVMSG", msg);
	set_chanuser_irc_handler("NOTICE", msg);
	set_chanuser_irc_handler("315", num_endofwho);
	set_chanuser_irc_handler("324", num_channelmodeis);
	set_chanuser_irc_handler("332", num_topic);
	set_chanuser_irc_handler("353", num_namereply);
	set_chanuser_irc_handler("354", num_whospecial);
	set_chanuser_irc_handler("366", num_endofnames);
	set_chanuser_irc_handler("367", num_banlist);
	set_chanuser_irc_handler("368", num_endofbanlist);
	set_chanuser_irc_handler("396", num_hosthidden);
#undef set_chanuser_irc_handler
}
