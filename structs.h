#ifndef STRUCTS_H
#define STRUCTS_H

// various structs needed at multiple places

struct surgebot
{
	time_t		start;
	time_t		linked;

	unsigned long	lines_received;
	unsigned long	lines_sent;

	struct dict	*cfg;

	char		*local_host;

	char		*server_host;
	unsigned int	server_port;
	char		*server_pass;
	unsigned int	max_server_tries;
	unsigned int	server_ssl;

	unsigned int	server_tries;
	struct sock	*server_sock;

	char		*server_name;

	char		*nickname;
	char		*username;
	char		*hostname;
	char		*realname;

	char		trigger;
};

struct irc_source
{
	char	*nick;
	char	*ident;
	char	*host;
};

#endif
