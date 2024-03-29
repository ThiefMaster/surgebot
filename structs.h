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

	unsigned int	ready : 1;

	char		*server_name;

	char		*nickname;
	char		*username;
	char		*hostname;
	char		*realname;

	struct {
		// this will hold values such as "SILENCE=15" with "SILENCE" as key and "15" as value
		// capabilities without value, such as "USERIP" will be stored as key with a NULL value
		struct dict *capabilities;
	} server;
};

struct surgebot_conf
{
	char		*local_host;

	char		*server_host;
	unsigned int	server_port;
	char		*server_pass;
	unsigned int	max_server_tries;
	unsigned int	server_ssl;
	unsigned int	server_ipv6;

	unsigned int	throttle;

	char		*nickname;
	char		*username;
	char		*realname;

	char		*trigger;
};

struct user_account
{
	char	*name;
	char	pass[41]; // sha1 hash + \0
	struct stringlist	*login_masks;
	time_t	registered;

	struct dict	*users;
	struct dict	*groups;
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
	time_t		topic_ts;

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

	struct dict *channels; // irc_chanuser
	struct user_account	*account;
};

struct irc_chanuser
{
	struct irc_channel	*channel;
	struct irc_user		*user;
	int			flags;
	time_t			joined;
};

struct irc_ban
{
	struct irc_channel	*channel;
	char			*mask;
};

#endif
