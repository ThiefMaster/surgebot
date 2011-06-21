#include "global.h"
#include "module.h"
#include "modules/tools/tools.h"
#include "modules/commands/commands.h"
#include "modules/help/help.h"
#include "irc.h"
#include "irc_handler.h"
#include "time.h"
#include "chanuser.h"
MODULE_DEPENDS("commands", "tools", NULL);

IRC_HANDLER(privmsg);
IRC_HANDLER(lmgtfy); //PRIVMSG
IRC_HANDLER(invite);
IRC_HANDLER(kick);
IRC_HANDLER(chan_is_invite_only);
IRC_HANDLER(banned);
COMMAND(time_info);
COMMAND(care_police);
COMMAND(wtf_truck);
COMMAND(germany);
COMMAND(france);
COMMAND(switzerland);
COMMAND(argentina);
COMMAND(spain);
COMMAND(slap);
COMMAND(bday);
COMMAND(loadavg);
COMMAND(blah);
COMMAND(ducks);
COMMAND(apple);
COMMAND(windows);
COMMAND(love);
COMMAND(troll);
COMMAND(spiderpig);
COMMAND(pastebin);
COMMAND(repeat);
COMMAND(sex);
COMMAND(horse);
COMMAND(banana);
COMMAND(grammar_police);

MODULE_INIT
{
	reg_irc_handler("PRIVMSG", privmsg);
	reg_irc_handler("PRIVMSG", lmgtfy);
	reg_irc_handler("INVITE", invite);
	reg_irc_handler("KICK", kick);
	reg_irc_handler("473", chan_is_invite_only);
	reg_irc_handler("474", banned);
	DEFINE_COMMAND(self, "time",	time_info,	0, 0, "group(admins)");
	DEFINE_COMMAND(self, "care",	care_police,	0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "truck",	wtf_truck,	0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "germany",	germany,	0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "france",	france,		0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "switzerland", switzerland, 0, CMD_LAZY_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "argentina", argentina, 0, CMD_LAZY_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "spain",	spain,		0, CMD_LAZY_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "slap",	slap,		0, CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "bday",	bday,		2, CMD_REQUIRE_AUTHED , "group(admins)");
	DEFINE_COMMAND(self, "loadavg", loadavg,	0, 0, "group(admins)");
	DEFINE_COMMAND(self, "blah",	blah,		0, CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "ducks",	ducks,		0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "apple",	apple,		0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "windows",	windows,	0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "love",	love,		0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "troll",	troll,		0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "spiderpig",spiderpig,	0, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "pastebin",pastebin,	0, CMD_REQUIRE_CHANNEL, "true");
	DEFINE_COMMAND(self, "repeat",	repeat,		3, CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(self, "sex",		sex,		0, 0, "group(admins)");
	DEFINE_COMMAND(self, "horse",	horse,		0, CMD_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "banana",	banana,		0, CMD_ACCEPT_CHANNEL, "group(admins)");
	DEFINE_COMMAND(self, "grammarpolice", grammar_police, 0, CMD_LAZY_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL, "group(admins)");
}

MODULE_FINI
{
	unreg_irc_handler("PRIVMSG", lmgtfy);
	unreg_irc_handler("PRIVMSG", privmsg);
	unreg_irc_handler("INVITE", invite);
	unreg_irc_handler("KICK", kick);
	unreg_irc_handler("473", chan_is_invite_only);
	unreg_irc_handler("474", banned);
}

COMMAND(loadavg)
{
	FILE *f = fopen("/proc/loadavg", "r");
	char buf[256], *vec[10], *color_start = "", *color_end = "";
	int tokcount;
	if(!f)
	{
		reply("Could not read load average");
		return 0;
	}
	fgets(buf, 256, f);
	fclose(f);
	tokcount = tokenize(buf, vec, 10, ' ', 0);
	assert_return(tokcount > 2, 0);
	if(atof(vec[0]) >= 1.5)
	{
		color_start = "$c04";
		color_end = "$c";
	}
	else if(atof(vec[0]) >= 1)
	{
		color_start = "$c07";
		color_end = "$c";
	}
	reply("Load average: %s%s%s, %s, %s", color_start, vec[0], color_end, vec[1], vec[2]);
	return 0;
}

IRC_HANDLER(lmgtfy)
{
	unsigned int count;
	char *nick, *msgdup, *vec[3]; // Nick: lmgtfy <keyword(s)>
	const char *msg, *dst = argv[1];
	struct irc_user *user;
	struct irc_channel *channel;

	if(!IsChannelName(dst))
		return;

	assert((channel = channel_find(dst)));

	msgdup = strdup(argv[2]);

	count = tokenize(msgdup, vec, ArraySize(vec), ' ', 0);

	if(count <= 2 || strcasecmp(vec[1], "lmgtfy"))
	{
		free(msgdup);
		return;
	}

	nick = strndup(vec[0], strlen(vec[0]) - 1);
	if((user = user_find(nick)) && channel_user_find(channel, user))
		irc_send("PRIVMSG %s :\002%s\002: http://lmgtfy.com/?q=%s", dst, nick, urlencode(vec[2]));

	free(nick);
	free(msgdup);
}

IRC_HANDLER(privmsg)
{
	const char *dst = argv[1];
	const char *msg = argv[2];
	//char *tmp;

	if(IsChannelName(dst) && src && src->nick)
	{
		if(!strcasecmp(dst, "#help") || !strcasecmp(dst, "#surgebot"))
		{
			struct irc_channel *irc_channel;
			struct irc_user *irc_user;
			const time_t min_duration = 60;
			static time_t last_msg = 0;

			if((irc_channel = channel_find(dst)) && (irc_user = user_find(src->nick)))
			{
				struct irc_chanuser *chanuser = channel_user_find(irc_channel, irc_user);
				if(chanuser && (chanuser->flags & (MODE_OP | MODE_VOICE)))
					return;
			}

			if(last_msg > 0 && (last_msg + min_duration >= now))
				return;

			if(
				   !strcasecmp(msg, "help")
				|| !strcasecmp(msg, "!next")
				|| (strcasestr(msg, "I need help") && strlen(msg) < 30)
				|| (strcasestr(msg, "help me") && strlen(msg) < 30)
				|| !strncasecmp(msg, "need help", 9)
				|| !strcasecmp(msg, "I need help!")
				|| !strcasecmp(msg, "hello?")
				|| !strcasecmp(msg, "anyone here")
				|| !strcasecmp(msg, "anyone here?")
				|| !strcasecmp(msg, ".help")
				|| !strcasecmp(msg, "anyone here that can help me?")
			)
			{
				//irc_send("PRIVMSG %s :%s: Please just ask your question and somebody will get back to you!", dst, src->nick);
				irc_send("PRIVMSG %s :$b%s$b: %s", dst, src->nick,
					/*(irc_channel->topic[0] ?
					irc_channel->topic :
						"Welcome to the GameSurge Help Channel. "
						"If you need help, simply ask and someone active in this channel will assist you. "
						"If you require GameSurge Staff interaction (such as channel registration), visit our #support channel. "
						"Have a great day!")*/
					 "Welcome. Ask. In here, not in a PM. We won't PM you either.");
				last_msg = now;
				return;
			}
		}
		
		if(IsChannelName(dst) && strcasecmp(dst, "#help") != 0 && strcasecmp(dst, "#gamesurge") != 0) {
			size_t len = strlen(msg);
			const time_t min_duration = 60;
			static time_t last_msg = 0;

			if(len >= 2 && len == strspn(msg, ".") && last_msg <= (now - min_duration)) {
				irc_send("PRIVMSG %s :\001ACTION evolves into pacman and eats up all of %s's dots\001", dst, src->nick);
				last_msg = now;
				return;
			}
		}
	}
}

COMMAND(slap)
{
	const char *nick;
	if(argc > 1)
		nick = argv[1];
	else
		nick = src->nick;

	irc_send("PRIVMSG %s :\001ACTION slaps %s around a bit with a large trout!\001", channel->name, nick);
	return 1;
}

COMMAND(france)
{
	for(int i = 0; i < 6; i++)
	{
		if(i == 2)
			irc_send("PRIVMSG %s :\00312,12----------\0030,0---\0031DOOFE\0030--\0034,4----------", channel->name);
		else if(i == 4)
			irc_send("PRIVMSG %s :\00312,12----------\0030,0---\0031OHREN\0030--\0034,4----------", channel->name);
		else
			irc_send("PRIVMSG %s :\00312,12----------\0030,0----------\0034,4----------", channel->name);
	}
	return 1;
}

COMMAND(germany)
{
	int values[6] = {1, 1, 4, 4, 8, 8};
	for(unsigned int i = 0; i < (sizeof(values) / sizeof(values[0])); i++)
	{
		irc_send("PRIVMSG %s :\003%d,%d------------------------------", channel->name, values[i], values[i]);
	}
	return 1;
}

COMMAND(care_police)
{
	irc_send("PRIVMSG %s :............. _@@@__", channel->name);
	irc_send("PRIVMSG %s :......_____//____?__\\________", channel->name);
	irc_send("PRIVMSG %s :- ---o--------CARE-POLICE----@)", channel->name);
	irc_send("PRIVMSG %s :-----` --(@)======+====(@)--", channel->name);
	return 1;
}

COMMAND(wtf_truck)
{
	irc_send("PRIVMSG %s :|^^^^^^^^^^^^^^||____", channel->name);
	irc_send("PRIVMSG %s :|.The WHO GIVES A | ||'""|""\\_ _", channel->name);
	irc_send("PRIVMSG %s :|__ FUCK TRUCK __ l ||__|__|__|)", channel->name);
	irc_send("PRIVMSG %s :|(@)(@)****(@)(@)****|(@)**(@)", channel->name);
	return 1;
}

IRC_HANDLER(chan_is_invite_only)
{
	irc_send("PRIVMSG #surgebot.debug :Could not join $b%s$b (+i)", argv[2] );
}

IRC_HANDLER(banned)
{
	irc_send("PRIVMSG #surgebot.debug :I'm banned from $b%s$b", argv[2]);
}

IRC_HANDLER(invite)
{
	irc_send( "PRIVMSG #surgebot.debug :$b%s$b has invited me to join $b%s$b", src->nick, argv[2] );
}

IRC_HANDLER(kick)
{
	if( !strcasecmp( argv[2], bot.nickname ) )
		irc_send( "PRIVMSG #surgebot.intern :$b%s$b has kicked me from $b%s$b (%s)", src->nick, argv[1], argv[3] );
}

COMMAND( time_info )
{
	long timestamp;
	if( argc > 1 )
	{
		if( ( timestamp = atol( argv[1] ) ) > 0 )
			reply( "The timestamp $b%ld$b is the following date/time: %s", timestamp, asctime( localtime( &timestamp ) ) );
		else
			reply( "$b%s$b is no valid timestamp", argv[1] );
	}
	else
		reply( "The current timestamp is $b%ld$b.", time( NULL ) );
	return 0;
}

COMMAND(bday)
{
	char *text = "HAPPY BIRTHDAY!!!11einseinself";
	char *c;
	int pos;
	int count = argc > 2 ? atoi(argv[2]) : 2;

	assert_return((strlen(text) / 4) < MAXLEN, 0);

	if(count < 1 || count > 10)
	{
		reply("You can send 1-10 messages - not less and not more.");
		return 0;
	}

	for(int i = 0; i < count; i++)
	{
		char msg[MAXLEN + 1];
		pos = 0;
		for(c = text; *c; c++)
			pos += snprintf(msg + pos, MAXLEN - pos, "\003%d%c", (int) (15.0 * (rand() / (RAND_MAX + 1.0))), *c);
		msg[pos] = '\0';
		irc_send("PRIVMSG %s :%s", argv[1], msg);
	}

	return 1;
}

COMMAND(blah)
{
	return 1;
}

COMMAND(ducks)
{
	irc_send("PRIVMSG %s :  _          _          _          _          _", channel->name);
	irc_send("PRIVMSG %s :>(')____,  >(')____,  >(')____,  >(')____,  >(') ___,", channel->name);
	irc_send("PRIVMSG %s :  (` =~~/    (` =~~/    (` =~~/    (` =~~/    (` =~~/", channel->name);
	irc_send("PRIVMSG %s :^~^`---'~^~^~^`---'~^~^~^`---'~^~^~^`---'~^~^~^`---'~^~^~", channel->name);
	return 1;
}

COMMAND(apple)
{
	irc_send("PRIVMSG %s :                           .", channel->name);
	irc_send("PRIVMSG %s :                         .OO", channel->name);
	irc_send("PRIVMSG %s :                       .OOOO", channel->name);
	irc_send("PRIVMSG %s :                      .OOOO'", channel->name);
	irc_send("PRIVMSG %s :                      OOOO'          .-~~~~-.", channel->name);
	irc_send("PRIVMSG %s :                      OOO'          /   (o)(o)", channel->name);
	irc_send("PRIVMSG %s :              .OOOOOO `O .OOOOOOO. /      .. |", channel->name);
	irc_send("PRIVMSG %s :          .OOOOOOOOOOOO OOOOOOOOOO/\\    \\____/", channel->name);
	irc_send("PRIVMSG %s :        .OOOOOOOOOOOOOOOOOOOOOOOO/ \\\\   ,\\_/", channel->name);
	irc_send("PRIVMSG %s :       .OOOOOOO%%%%OOOOOOOOOOOOO(#/\\     /.", channel->name);
	irc_send("PRIVMSG %s :      .OOOOOO%%%%%%OOOOOOOOOOOOOOO\\ \\\\  \\/OO.", channel->name);
	irc_send("PRIVMSG %s :     .OOOOO%%%%%%%%OOOOOOOOOOOOOOOOO\\   \\/OOOO.", channel->name);
	irc_send("PRIVMSG %s :     OOOOO%%%%%%%%OOOOOOOOOOOOOOOOOOO\\_\\/\\OOOOO", channel->name);
	irc_send("PRIVMSG %s :     OOOOO%%%%%%OOOOOOOOOOOOOOOOOOOOO\\###)OOOO", channel->name);
	irc_send("PRIVMSG %s :     OOOOOO%%%%OOOOOOOOOOOOOOOOOOOOOOOOOOOOOO", channel->name);
	irc_send("PRIVMSG %s :     OOOOOOO%%OOOOOOOOOOOOOOOOOOOOOOOOOOOOOO", channel->name);
	irc_send("PRIVMSG %s :     `OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :   .-~~\\OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :  / _/  `\\(#\\OOOOOOOOOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s : / / \\  / `~~\\OOOOOOOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :|/'  `\\//  \\\\ \\OOOOOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :       `-.__\\_,\\OOOOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :           `OO\\#)OOOOOOOOOOOOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :             `OOOOOOOOO''OOOOOOOOO'", channel->name);
	irc_send("PRIVMSG %s :               `\"\"\"\"\"\"'  `\"\"\"\"\"\"'", channel->name);
	return 1;
}

COMMAND(windows)
{
	irc_send("PRIVMSG %s :$c4,1        _..?++?II7$$._                   ", channel->name);
	irc_send("PRIVMSG %s :$c4,1       7I??++?I77$ZZOO                  ", channel->name);
	irc_send("PRIVMSG %s :$c4,1      .7I?++??I77$ZZO8.$c9.87.         _.$.", channel->name);
	irc_send("PRIVMSG %s :$c4,1      =I??++?II7$$ZZO.$c9.$8OOZ$Z, _:I77$O. ", channel->name);
	irc_send("PRIVMSG %s :$c4,1      7I?++??I7$$ZZOO.$c9.8OOZZ$$7III77$ZD   ", channel->name);
	irc_send("PRIVMSG %s :$c4,1     +I??++?II7$$ZOO~ $c9$8OOZZ$77III7$$Z.   ", channel->name);
	irc_send("PRIVMSG %s :$c4,1    .7I?++??I77$ZZOO.$c9.8OOZZ$$7IIII7$ZD   ", channel->name);
	irc_send("PRIVMSG %s :$c4,1    .II?++?II7$$ZZO8.$c9Z8OOZZ$77III77$Z.   ", channel->name);
	irc_send("PRIVMSG %s :$c4,1   .7I?´  $c12_  $c4``O7OO.$c9 8OOZZ$$77III7$ZD    ", channel->name);
	irc_send("PRIVMSG %s :$c12,1     .,~===++?+  $c4`` $c9O8OOZZ$77III77$Z.   ", channel->name);
	irc_send("PRIVMSG %s :$c12,1  .I?+=~===+??I7$$. $c9?OOZZ$$77III7$ZD      ", channel->name);
	irc_send("PRIVMSG %s :$c12,1  .?+=~~==++?I77$Z.$c8,.  $c9.8$$7III7´´       ", channel->name);
	irc_send("PRIVMSG %s :$c12,1  I?+=~~==+??I7$Z.$c8 7Z$$.   $c9´´ $c8 .,IZ      ", channel->name);
	irc_send("PRIVMSG %s :$c12,1 .?+=~~==++?I77$Z$c8 7ZZ$$77I??++??II?      ", channel->name);
	irc_send("PRIVMSG %s :$c12,1.7?+=~~==++?I7$Z.$c8.7Z$$77II?+++??I$       ", channel->name);
	irc_send("PRIVMSG %s :$c12,1.?+=~~==++?II7$Z$c8 ,ZZ$$77II?++??IIO       ", channel->name);
	irc_send("PRIVMSG %s :$c12,1+?+=~~==++?I7$Z.$c8 ?Z$$77II??++??I$.       ", channel->name);
	irc_send("PRIVMSG %s :$c12,1I?=~~==++?I77$Z$c8 .ZZ$$77II?++??IIO.       ", channel->name);
	irc_send("PRIVMSG %s :$c12,1I´         `+I.$c8.7Z$$77II??++??II.        ", channel->name);
	irc_send("PRIVMSG %s :$c12,1             ` $c8.ZZ$$77II?+++??IO.        ", channel->name);
	irc_send("PRIVMSG %s :$c8,1                `.Z777I??++?IZZ´        ", channel->name);
	irc_send("PRIVMSG %s :$c8,1                    `7I?++?´            ", channel->name);
	return 1;
}

COMMAND(love)
{
	char line1[18], line12[16], line2[14];
	const char *text1 = NULL, *text2 = NULL;

	for(int i = 0; i < 18; i++)
	{
		line1[i] = ' ';
		if(i < 16)
			line12[i] = ' ';
		if(i < 14)
			line2[i] = ' ';
	}

	line1[17] = line12[15] = line2[13] = '\0';

	if(argc > 1)
		text1 = argv[1];
	if(argc > 2)
		text2 = argv[2];

	if((text1 && strlen(text1) > 17) || (text2 && strlen(text2) > 13))
	{
		reply("Line 1 may have max. 17 chars and line 2 may have max. 13 chars.");
		return 0;
	}

	if(text1)
	{
		int offset = (17 - strlen(text1)) / 2;
		memcpy(line1 + offset, text1, strlen(text1));
	}

	if(text2)
	{
		line12[7] = '+';
		int offset = (13 - strlen(text2)) / 2;
		memcpy(line2 + offset, text2, strlen(text2));
	}


	irc_send("PRIVMSG %s :$c13,1                               $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1    .:oOOOOo:.   .:oOOOOo:.    $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1  .:oOO:'':Oo:. .:oO:'':OOo:.  $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1 .:oO:      'Oo:oO'      :Oo:. $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1 :oO:         'o'         :Oo: $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1 :oO:  $c4%s$c13  :Oo: $c",                channel->name, line1);
	irc_send("PRIVMSG %s :$c13,1 ':oO:  $c8%s$c13  :Oo:' $c",              channel->name, line12);
	irc_send("PRIVMSG %s :$c13,1  ':oO:  $c4%s$c13  :Oo:'  $c",            channel->name, line2);
	irc_send("PRIVMSG %s :$c13,1    ':oO.             .Oo:'    $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1      ':oO.         .Oo:'      $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1        ':oO.     .Oo:'        $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1          ':oO. .Oo:'          $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1            'oO:Oo'            $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1             'oOo'             $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1              'o'              $c", channel->name);
	irc_send("PRIVMSG %s :$c13,1                               $c", channel->name);
	return 1;
}

COMMAND(troll)
{
	irc_send("PRIVMSG %s :        \\|||/", channel->name);
	irc_send("PRIVMSG %s :        (o o)", channel->name);
	irc_send("PRIVMSG %s :,~~~ooO~~(_)~~~~~~~~~,", channel->name);
	irc_send("PRIVMSG %s :|       Please       |", channel->name);
	irc_send("PRIVMSG %s :|   don't feed the   |", channel->name);
	irc_send("PRIVMSG %s :|       TROLL!       |", channel->name);
	irc_send("PRIVMSG %s :'~~~~~~~~~~~~~~ooO~~~'", channel->name);
	irc_send("PRIVMSG %s :       |__|__|", channel->name);
	irc_send("PRIVMSG %s :        || ||", channel->name);
	irc_send("PRIVMSG %s :       ooO Ooo", channel->name);
	return 1;
}

COMMAND(spiderpig)
{
	irc_send("PRIVMSG %s :____________________________________", channel->name);
	irc_send("PRIVMSG %s :----------.__.--------._.-----------", channel->name);
	irc_send("PRIVMSG %s :---------,-|--|==__|-|--------------", channel->name);
	irc_send("PRIVMSG %s :---_-_,'--|--|--------|--\\----------", channel->name);
	irc_send("PRIVMSG %s :---|@|---------------|---\\----------", channel->name);
	irc_send("PRIVMSG %s :----|o--o-------------'.---|--------", channel->name);
	irc_send("PRIVMSG %s :----\\----------------------/~~~-----", channel->name);
	irc_send("PRIVMSG %s :-----|-__--======='-----------------", channel->name);
	irc_send("PRIVMSG %s :-----\\/--\\/-------------------------", channel->name);
	irc_send("PRIVMSG %s :------------------------------------", channel->name);
	irc_send("PRIVMSG %s :-------------$bSPIDERPIG$b-------------", channel->name);
	irc_send("PRIVMSG %s :------------------------------------", channel->name);
	return 1;
}

COMMAND(pastebin)
{
	irc_send("PRIVMSG %s :Here is a pastebin you can use: $uhttp://www.privatepaste.com/%s$u", channel->name, (argc > 1 ? argv[1] : ""));
	return 0;
}

COMMAND(repeat)
{
        int count = atoi(argv[2]);
	char *text = untokenize(argc - 3, argv + 3, " ");

        for(int i = 0; i < count; i++)
                irc_send("PRIVMSG %s :%s", argv[1], text);

	free(text);
        return 1;
}

COMMAND(sex)
{
	irc_send("PRIVMSG %s :I don't want to be part of your fertile imagination!", channel ? channel->name : src->nick);
	return 0;
}

COMMAND(horse)
{
	const char *target = channel ? channel->name : src->nick;

	irc_send("PRIVMSG %s :            ,%%,_", target);
	irc_send("PRIVMSG %s :           %%%%%%/,\\", target);
	irc_send("PRIVMSG %s :        _.-\"%%%%|//%%", target);
	irc_send("PRIVMSG %s :     _.' _.-\"  /%%%%%%", target);
	irc_send("PRIVMSG %s : _.-'_.-\" O)    \\%%%%%%", target);
	irc_send("PRIVMSG %s :/.\\.'            \\%%%%%%", target);
	irc_send("PRIVMSG %s :\\ /        _,     |%%%%%%", target);
	irc_send("PRIVMSG %s : `\"-----\"~`\\   _,*'\\%%%%'   _,--\"\"\"\"-,%%%%,", target);
	irc_send("PRIVMSG %s :            )*^     `\"\"~~`          \\%%%%%%,", target);
	irc_send("PRIVMSG %s :          _/                         \\%%%%%%", target);
	irc_send("PRIVMSG %s :      _.-`/                           |%%%%,___", target);
	irc_send("PRIVMSG %s :  _.-\"   /      ,           ,        ,|%%%%   .`\\", target);
	irc_send("PRIVMSG %s : /\\     /      /             `\\       \\%%'   \\ /", target);
	irc_send("PRIVMSG %s : \\ \\ _,/      /`~-._         _,`\\      \\`\"\"~~`", target);
	irc_send("PRIVMSG %s :  `\"` /-.,_ /'      `~\"----\"~    `\\     \\", target);
	irc_send("PRIVMSG %s :      \\___,'                       \\.-\"`/", target);
	irc_send("PRIVMSG %s :                                    `--'", target);

	return 1;
}

COMMAND(banana)
{
	const char *target = channel ? channel->name : src->nick;

	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxxxx\0031,1xxxx\0030,0xxxxxxxxxxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xxxxxxxx\0030,0xxxxxxxxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0038,8xxxx\0031,1xx\0030,0xxxxxxxxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0038,8xxxx\0031,1xxxx\0030,0xxxxxxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0038,8xxxxxx\0031,1xxxx\0030,0xxxxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0038,8xxxxxxxx\0031,1xxxx\0030,0xxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0038,8xxxxxxxxxx\0031,1xx\0030,0xxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxx\0031,1xxxxxxxxxx\0038,8xxxxxx\0031,1xxxx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxx\0031,1xx\0030,0xx\0031,1xx\0030,0xxxxxx\0031,1xx\0038,8xxxxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxx\0031,1xxxxxxxx\0030,0xxxx\0031,1xx\0038,8xxxxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxx\0031,1xx\0030,0xx\0031,1xx\0030,0xxxxxx\0031,1xx\0038,8xxxxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxx\0031,1xxxxxxxxxx\0038,8xxxx\0031,1xx\0038,8xx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxx\0031,1xx\0038,8xxxxxx\0031,1xxxxxx\0038,8xx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxx\0031,1xxxxxxxx\0034,4xxxx\0031,1xx\0038,8xx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0034,4xxxxxx\0031,1xx\0038,8xxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxxxx\0031,1xx\0034,4xx\0031,1xxxx\0038,8xxxxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxxxxxxxxxx\0031,1xxxxxx\0038,8xxxxxxxxxx\0031,1xx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xx\0031,1xxxxxx\0030,0xxxxxx\0031,1xx\0038,8xxxxxxxxxxxx\0031,1xxxx\0030,0xx\0031,1xxxxxx\0030,0xxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xx\0030,0xxxx\0031,1xxxx\0030,0xx\0031,1xxxx\0038,8xxxxxxxxxxxx\0031,1xx\0030,0xx\0031,1xxxx\0030,0xxxx\0031,1xx\0030,0x", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xx\0030,0xxxxxx\0031,1xx\0030,0xx\0031,1xx\0038,8xxxxxxxxxxxx\0031,1xxxx\0030,0xx\0031,1xx\0030,0xxxxxx\0031,1xx\0030,0x", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xx\0030,0xxxx\0031,1xxxx\0030,0xx\0031,1xx\0038,8xxxxxxxxxxxx\0031,1xx\0030,0xxxx\0031,1xxxx\0030,0xxxx\0031,1xx\0030,0x", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xx\0031,1xxxxxx\0030,0xx\0031,1xxxx\0038,8xxxxxxxxxx\0031,1xxxx\0030,0xxxx\0033,3xx\0031,1xxxxxx\0030,0xxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxx\0033,3xx\0030,0xx\0031,1xx\0038,8xxxxxxxxxx\0031,1xxxx\0033,3xx\0030,0xx\0033,3xxxx\0030,0xxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxxxx\0033,3xxxx\0031,1xxxx\0038,8xxxx\0031,1xxxxxx\0030,0xx\0033,3xxxxxx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xxxx\0033,3xxxxxx\0031,1xxxxxxxxxx\0033,3xxxxxxxxxx\0030,0xxxxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0030,0xx\0031,1xxxxxx\0030,0xxxxxxxxxxxxxxxx\0033,3xx\0031,1xxxxxx\0030,0xxxxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xx\0030,0xxxxxx\0031,1xxxxxx\0030,0xxxxxx\0031,1xxxxxx\0030,0xxxxxx\0031,1xx\0030,0xxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xx\0030,0xxxxxxxxxx\0031,1xx\0030,0xxxxxx\0031,1xx\0030,0xxxxxxxxxx\0031,1xx\0030,0xxxxxxxxx", target);
	irc_send("PRIVMSG %s :\0030,0x\0031,1xxxxxxxxxx\0031,1xx\0030,0xxxxxxxxxx\0031,1xxxxxxxxxx\0031,1xx\0030,0xxxxxxxxx", target);

	return 0;
}

COMMAND(grammar_police)
{
	irc_send("PRIVMSG %s :............. _@@@__", channel->name);
	irc_send("PRIVMSG %s :......_____//____?__\\________", channel->name);
	irc_send("PRIVMSG %s :- ---o-----GRAMMAR-POLICE----@)", channel->name);
	irc_send("PRIVMSG %s :-----` --(@)======+====(@)--", channel->name);
	return 1;
}

COMMAND(switzerland)
{
	const char *target = channel ? channel->name : src->nick;

	irc_send("PRIVMSG %s :\0034,4-------------------------------", target);
	irc_send("PRIVMSG %s :\0034,4-------------\0030,0:::\0034,4---------------", target);
	irc_send("PRIVMSG %s :\0034,4-------------\0030,0:::\0034,4---------------", target);
	irc_send("PRIVMSG %s :\0034,4----------\0030,0:::::::::\0034,4------------", target);
	irc_send("PRIVMSG %s :\0034,4----------\0030,0:::::::::\0034,4------------", target);
	irc_send("PRIVMSG %s :\0034,4-------------\0030,0:::\0034,4---------------", target);
	irc_send("PRIVMSG %s :\0034,4-------------\0030,0:::\0034,4---------------", target);
	irc_send("PRIVMSG %s :\0034,4-------------------------------", target);

	return 1;
}

COMMAND(argentina)
{
	const char *target = channel ? channel->name : src->nick;

	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c7,7@@@@@@@$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@$c7,7@@@@@@@@@@@@@$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@$c7,7@@@@@@@@@@@@@$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c7,7@@@@@@@$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c0,0@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);
	irc_send("PRIVMSG %s :$c10,10@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@$c", target);

	return 1;
}

COMMAND(spain)
{
	const char *target = channel ? channel->name : src->nick;

	int values[] = {4, 4, 7, 7, 7, 7, 4, 4};
	for(unsigned int i = 0; i < ArraySize(values); i++)
	{
		irc_send("PRIVMSG %s :$c%u,%u------------------------------", target, values[i], values[i]);
	}

	return 1;
}

