#ifndef RADIOSCHEDULE_H
#define RADIOSCHEDULE_H

struct
{
	const char *mysql_host;
	const char *mysql_user;
	const char *mysql_pass;
	const char *mysql_db;
} radioschedule_conf;

void next_show(struct irc_source *src);
void current_show(struct irc_source *src);

#endif
