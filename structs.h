#ifndef STRUCTS_H
#define STRUCTS_H

// various structs needed at multiple places

struct surgebot
{
	time_t		start;
	time_t		linked;

	unsigned long	lines_received;
	unsigned long	lines_sent;

	unsigned int	server_tries;
	struct sock	*server_sock;

	struct stringlist	*sendq;
	time_t		last_msg;

	struct stringlist	*burst_lines;
	unsigned int	burst_count;

	char		*server_name;

	char		*nickname;
	char		*username;
	char		*hostname;
	char		*realname;
};

struct surgebot_conf
{
	char		*local_host;

	char		*server_host;
	unsigned int	server_port;
	char		*server_pass;
	unsigned int	max_server_tries;
	unsigned int	server_ssl;

	unsigned int	throttle;

	char		*nickname;
	char		*username;
	char		*realname;

	char		trigger;
};

struct irc_source
{
	char	*nick;
	char	*ident;
	char	*host;
};


struct irc_channel
{
	char		*name;
	int		modes;
	char		*key;
	unsigned int	limit;
	char		*topic;

	unsigned int	burst_state;
	struct stringlist	*burst_lines;

	struct dict	*users;
	struct dict	*bans;
};

struct irc_user
{
	char		*nick;
	char		*ident;
	char		*host;
	char		*info;

	struct dict	*channels;
};

struct irc_chanuser
{
	struct irc_channel	*channel;
	struct irc_user		*user;
	int			flags;
};

struct irc_ban
{
	struct irc_channel	*channel;
	char			*mask;
};

#endif
