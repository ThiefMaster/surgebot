#ifndef RADIOSCHEDULE_H
#define RADIOSCHEDULE_H

struct pgsql;
struct
{
	const char *db_conn_string;
	struct pgsql *pg_conn;
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
int add_show(struct show_info *show_info, const char *show_title);
unsigned long lookup_userid(const char *nick);
void next_show(struct irc_source *src);
void current_show(struct irc_source *src);

#endif
