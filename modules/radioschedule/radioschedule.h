#ifndef RADIOSCHEDULE_H
#define RADIOSCHEDULE_H

struct
{
	const char *mysql_host;
	const char *mysql_user;
	const char *mysql_pass;
	const char *mysql_db;
} radioschedule_conf;

struct show_info
{
	unsigned long entryid;
	unsigned long userid;
	unsigned long userid2;
	time_t starttime;
	time_t endtime;
};

int get_current_show_info(struct show_info *show_info, unsigned int grace_time);
int get_conflicting_show_info(const struct show_info *show_info, struct show_info *conflict);
int extend_show(const struct show_info *show_info);
void next_show(struct irc_source *src);
void current_show(struct irc_source *src);

#endif
