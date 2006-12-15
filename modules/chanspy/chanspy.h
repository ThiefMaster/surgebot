#ifndef CHANSPY_H
#define CHANSPY_H

#define CSPY_PRIVMSG	0x001
#define CSPY_ACTION	0x002
#define CSPY_NOTICE	0x004
#define CSPY_QUERY	0x008
#define CSPY_MODE	0x010
#define CSPY_JOIN	0x020
#define CSPY_PART	0x040
#define CSPY_KICK	0x080
#define CSPY_QUIT	0x100
#define CSPY_NICK	0x200
#define CSPY_TOPIC	0x400
#define CSPY_ALL	(CSPY_PRIVMSG | CSPY_ACTION | CSPY_NOTICE | CSPY_QUERY | CSPY_MODE | CSPY_JOIN | CSPY_PART | CSPY_KICK | CSPY_QUIT | CSPY_NICK | CSPY_TOPIC)

struct chanspy
{
	char	*name;
	char	*channel;
	char	*target;
	char	*target_host;
	char	*target_pass;
	unsigned int target_port;
	int	flags;
	unsigned int active : 1;
	const char *last_error;
	struct sock *sock;
};

#define IsSpySourceChannelName(NAME)	(IsChannelName((NAME)) || !strcmp((NAME), "*"))

#endif
