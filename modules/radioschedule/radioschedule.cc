extern "C" {
#include "global.h"
#include "irc.h"
#include "radioschedule.h"
}

#include <mysql++.h>

static mysqlpp::Connection *db_connect()
{
	static mysqlpp::Connection con(true);
	try {
		con.connect(radioschedule_conf.mysql_db, radioschedule_conf.mysql_host, radioschedule_conf.mysql_user, radioschedule_conf.mysql_pass);
	}
	catch(mysqlpp::BadQuery& e) {
		log_append(LOG_WARNING, "MySQL connection failed: %s", e.what());
		return NULL;
	}

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
