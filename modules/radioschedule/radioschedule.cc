extern "C" {
#include "global.h"
#include "irc.h"
#include "radioschedule.h"
#include "modules/tools/tools.h"
}

#include <mysql++.h>

static mysqlpp::Connection *db_connect()
{
	static mysqlpp::Connection con(true);
	try {
		con.connect(radioschedule_conf.mysql_db, radioschedule_conf.mysql_host, radioschedule_conf.mysql_user, radioschedule_conf.mysql_pass);
	}
	catch(mysqlpp::ConnectionFailed& e) {
		log_append(LOG_WARNING, "MySQL connection failed: %s", e.what());
		return NULL;
	}

	mysqlpp::Query query = con.query();
	query << "SET NAMES 'utf8'";
	query.execute();

	return &con;
}

static void db_close(mysqlpp::Connection *con)
{
	con->disconnect();
}

extern "C" void next_show(struct irc_source *src)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
	{
		reply("Could not connect to database.");
		return;
	}

	try {
		mysqlpp::Query query = con->query();
		query << "SELECT u.username, s.comment, s.starttime, s.endtime FROM schedule s LEFT JOIN users u ON (u.userid = s.userid) WHERE starttime >= " << now << " AND starttime <= " << (now + 86400*14) << " ORDER BY starttime ASC LIMIT 1";
		mysqlpp::UseQueryResult res = query.use();
		mysqlpp::Row row = res.fetch_row();
		if(!row)
		{
			reply("Keine Sendung eingetragen.");
			db_close(con);
			return;
		}
		const char *modname = strdup(row["username"]);
		const char *showtitle = strdup(row["comment"]);
		unsigned long starttime = (unsigned long)row["starttime"];
		char tmp[16], starttime_str[32];
		strftime(tmp, sizeof(tmp), "%d.%m.%Y", localtime((time_t*)&starttime));
		strftime(starttime_str, sizeof(starttime_str), "%d.%m.%Y", localtime((time_t*)&now));
		if(!strcmp(tmp, starttime_str)) // same date
			strftime(starttime_str, sizeof(starttime_str), "%H:%M", localtime((time_t*)&starttime));
		else
			strftime(starttime_str, sizeof(starttime_str), "%H:%M (%d.%m.)", localtime((time_t*)&starttime));
		reply("$b%s$b um $b%s$b: $b%s$b", modname, starttime_str, showtitle);
		free((void*)modname);
		free((void*)showtitle);
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		reply("Database query failed.");
		db_close(con);
		return;
	}

	db_close(con);
}

extern "C" void current_show(struct irc_source *src)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
	{
		reply("Could not connect to database.");
		return;
	}

	try {
		mysqlpp::Query query = con->query();
		query << "SELECT u.username, s.comment, s.starttime, s.endtime FROM schedule s LEFT JOIN users u ON (u.userid = s.userid) WHERE starttime <= " << now << " AND endtime > " << now << " ORDER BY starttime ASC LIMIT 1";
		mysqlpp::UseQueryResult res = query.use();
		mysqlpp::Row row = res.fetch_row();
		if(!row)
		{
			reply("Keine Sendung eingetragen.");
			db_close(con);
			return;
		}
		const char *modname = strdup(row["username"]);
		const char *showtitle = strdup(row["comment"]);
		unsigned long endtime = (unsigned long)row["endtime"];
		char tmp[16], endtime_str[32];
		strftime(tmp, sizeof(tmp), "%d.%m.%Y", localtime((time_t*)&endtime));
		strftime(endtime_str, sizeof(endtime_str), "%d.%m.%Y", localtime((time_t*)&now));
		if(!strcmp(tmp, endtime_str)) // same date
			strftime(endtime_str, sizeof(endtime_str), "%H:%M", localtime((time_t*)&endtime));
		else
			strftime(endtime_str, sizeof(endtime_str), "%H:%M (%d.%m.)", localtime((time_t*)&endtime));
		reply("$b%s$b bis $b%s$b: $b%s$b", modname, endtime_str, showtitle);
		free((void*)modname);
		free((void*)showtitle);
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		reply("Database query failed.");
		return;
	}

	db_close(con);
}

extern "C" int get_current_show_info(struct show_info *show_info, unsigned int grace_time)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
		return -1;

	memset(show_info, 0, sizeof(struct show_info));

	try {
		mysqlpp::Query query = con->query();
		query << "SELECT * FROM schedule WHERE starttime <= " << now << " AND endtime > " << (now - grace_time) << " ORDER BY starttime DESC LIMIT 1";
		mysqlpp::UseQueryResult res = query.use();
		mysqlpp::Row row = res.fetch_row();
		if(!row)
		{
			db_close(con);
			return 0;
		}
		show_info->entryid = (unsigned long)row["entryid"];
		show_info->userid = (unsigned long)row["userid"];
		show_info->userid2 = (unsigned long)row["userid2"];
		show_info->starttime = (unsigned long)row["starttime"];
		show_info->endtime = (unsigned long)row["endtime"];
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		db_close(con);
		return -1;
	}

	db_close(con);
	return 0;
}

extern "C" int get_conflicting_show_info(const struct show_info *show_info, struct show_info *conflict)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
		return -1;

	memset(conflict, 0, sizeof(struct show_info));

	try {
		mysqlpp::Query query = con->query();
		query << "SELECT * FROM schedule WHERE ( \
				(starttime >= " << show_info->starttime << " AND starttime < " << show_info->endtime << ") OR \
				(endtime > " << show_info->starttime << " AND endtime <= " << show_info->endtime <<  ") OR \
				(starttime <= " << show_info->starttime << " AND endtime >= " << show_info->endtime << ") \
			) AND entryid != " << show_info->entryid << " ORDER BY starttime ASC LIMIT 1";
		mysqlpp::UseQueryResult res = query.use();
		mysqlpp::Row row = res.fetch_row();
		if(!row)
		{
			db_close(con);
			return 0;
		}
		conflict->entryid = (unsigned long)row["entryid"];
		conflict->userid = (unsigned long)row["userid"];
		conflict->userid2 = (unsigned long)row["userid2"];
		conflict->starttime = (unsigned long)row["starttime"];
		conflict->endtime = (unsigned long)row["endtime"];
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		db_close(con);
		return -1;
	}

	db_close(con);
	return 0;
}

extern "C" int extend_show(const struct show_info *show_info)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
		return -1;

	try {
		mysqlpp::Query query = con->query();
		query << "UPDATE schedule SET endtime = " << show_info->endtime << " WHERE entryid = " << show_info->entryid;
		query.execute();
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		db_close(con);
		return -1;
	}

	db_close(con);
	return 0;
}

extern "C" int add_show(struct show_info *show_info, const char *show_title)
{
	mysqlpp::Connection *con = db_connect();
	if(!con)
		return -1;

	try {
		mysqlpp::Query query = con->query();
		query << "INSERT INTO schedule (userid, userid2, starttime, endtime, comment) VALUES (" << show_info->userid << ", " << show_info->userid2 << ", " << show_info->starttime << ", " << show_info->endtime << ", " << mysqlpp::quote << to_utf8(show_title) << ")";
		query.execute();
		show_info->entryid = (unsigned long)query.insert_id();
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		db_close(con);
		return -1;
	}

	db_close(con);
	return 0;
}

extern "C" unsigned long lookup_userid(const char *nick)
{
	unsigned long userid = 0;

	mysqlpp::Connection *con = db_connect();
	if(!con)
		return 0;

	try {
		mysqlpp::Query query = con->query();
		query << "SELECT userid FROM users WHERE username = " << mysqlpp::quote << nick << " AND (userflags & 1)";
		mysqlpp::UseQueryResult res = query.use();
		mysqlpp::Row row = res.fetch_row();
		if(!row)
		{
			db_close(con);
			return 0;
		}
		userid = (unsigned long)row["userid"];
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL error: %s", e.what());
		db_close(con);
		return 0;
	}

	db_close(con);
	return userid;
}
