#include "global.h"
#include "irc.h"
#include "stringlist.h"
#include "radioschedule.h"
#include "modules/tools/tools.h"
#include "modules/pgsql/pgsql.h"


static char *extract_html_arg(const char *str, const char *arg_name)
{
	static char buf[MAXLEN];
	char *start, *end;
	char quote;
	size_t arg_len;

	arg_len = strlen(arg_name);

	strlcpy(buf, str, sizeof(buf));
	if(!(start = strcasestr(buf, arg_name)))
		return NULL;
	if(strlen(start) < (arg_len + 2))
		return NULL;

	start += arg_len + 1; // skip over "arg="

	quote = *start;
	if(quote == '"' || quote == '\'')
		start++; // skip over quote
	else
		quote = '\0'; // no quote char

	end = start;
	if(quote)
	{
		while(*end && *end != quote)
			end++;
	}
	else
	{
		while(*end && *end != '>' && !isspace(*end))
			end++;
	}

	*end = '\0';
	return start;
}

static const char *dehtmlify_showtitle(const char *orig)
{
	char *arg;

	if(!strcasestr(orig, "<img"))
		return orig;

	if((arg = extract_html_arg(orig, "alt")) && *arg)
		return arg;
	else if((arg = extract_html_arg(orig, "src")) && *arg)
	{
		arg = basename(arg); // get rid of path
		char *dot = strrchr(arg, '.');
		if(dot)
			*dot = '\0';
		return arg;
	}

	return orig;
}

void next_show(struct irc_source *src)
{
	PGresult *res = pgsql_query(radioschedule_conf.pg_conn,
		"SELECT u.username, u2.username AS username2, s.title, extract(epoch from s.start_time at time zone 'UTC') AS start_ts \
		 FROM shows s \
		 LEFT JOIN users u ON (u.id = s.user_id) \
		 LEFT JOIN users u2 ON (u2.id = s.user_id_2) \
		 WHERE start_time >= (now() at time zone 'UTC') AND start_time <= (now() at time zone 'UTC') + interval '2 weeks' \
		 ORDER BY start_time ASC \
		 LIMIT 1", 1, NULL);
	if(!res || !pgsql_num_rows(res))
	{
		if(res)
			pgsql_free(res);
		reply("Keine Sendung eingetragen.");
		return;
	}
	const char *modname = pgsql_nvalue(res, 0, "username");
	const char *modname2 = pgsql_nvalue(res, 0, "username2");
	const char *showtitle = strdup(pgsql_nvalue(res, 0, "title"));
	unsigned long starttime = strtoul(pgsql_nvalue(res, 0, "start_ts"), NULL, 10);
	char tmp[16], starttime_str[32];
	strftime(tmp, sizeof(tmp), "%d.%m.%Y", localtime((time_t*)&starttime));
	strftime(starttime_str, sizeof(starttime_str), "%d.%m.%Y", localtime((time_t*)&now));
	if(!strcmp(tmp, starttime_str)) // same date
		strftime(starttime_str, sizeof(starttime_str), "%H:%M", localtime((time_t*)&starttime));
	else
		strftime(starttime_str, sizeof(starttime_str), "%H:%M (%d.%m.)", localtime((time_t*)&starttime));
	if(modname2)
		reply("$b%s$b und $b%s$b um $b%s$b: $b%s$b", modname, modname2, starttime_str, showtitle);
	else
		reply("$b%s$b um $b%s$b: $b%s$b", modname, starttime_str, showtitle);
	pgsql_free(res);
}

void current_show(struct irc_source *src)
{
	PGresult *res = pgsql_query(radioschedule_conf.pg_conn,
		"SELECT u.username, u2.username AS username2, s.title, extract(epoch from s.end_time at time zone 'UTC') AS end_ts \
		 FROM shows s \
		 LEFT JOIN users u ON (u.id = s.user_id) \
		 LEFT JOIN users u2 ON (u2.id = s.user_id_2) \
		 WHERE start_time <= (now() at time zone 'UTC') AND end_time >= (now() at time zone 'UTC') \
		 ORDER BY start_time ASC \
		 LIMIT 1", 1, NULL);
	if(!res || !pgsql_num_rows(res)) {
		if(res) {
			pgsql_free(res);
		}
		reply("Keine Sendung eingetragen.");
		return;
	}
	const char *modname = pgsql_nvalue(res, 0, "username");
	const char *modname2 = pgsql_nvalue(res, 0, "username2");
	const char *showtitle = strdup(pgsql_nvalue(res, 0, "title"));
	unsigned long endtime = strtoul(pgsql_nvalue(res, 0, "end_ts"), NULL, 10);
	char tmp[16], endtime_str[32];
	strftime(tmp, sizeof(tmp), "%d.%m.%Y", localtime((time_t*)&endtime));
	strftime(endtime_str, sizeof(endtime_str), "%d.%m.%Y", localtime((time_t*)&now));
	if(!strcmp(tmp, endtime_str)) { // same date
		strftime(endtime_str, sizeof(endtime_str), "%H:%M", localtime((time_t*)&endtime));
	}
	else {
		strftime(endtime_str, sizeof(endtime_str), "%H:%M (%d.%m.)", localtime((time_t*)&endtime));
	}
	if(modname2) {
		reply("$b%s$b und $b%s$b bis $b%s$b: $b%s$b", modname, modname2, endtime_str, dehtmlify_showtitle(showtitle));
	}
	else {
		reply("$b%s$b bis $b%s$b: $b%s$b", modname, endtime_str, dehtmlify_showtitle(showtitle));
	}
	pgsql_free(res);
}

int get_current_show_info(struct show_info *show_info, unsigned int grace_time)
{
	PGresult *res;
	char query[1024];
	const char *tmp;
	memset(show_info, 0, sizeof(struct show_info));

	snprintf(query, sizeof(query), "SELECT id, user_id, user_id_2, extract(epoch from start_time at time zone 'UTC') AS start_ts, extract(epoch from end_time at time zone 'UTC') AS end_ts \
			FROM shows \
			WHERE start_time <= (now() at time zone 'UTC') AND end_time > ((now() at time zone 'UTC') - interval '%u seconds') \
			ORDER BY start_time DESC \
			LIMIT 1", grace_time);
	res = pgsql_query(radioschedule_conf.pg_conn, query, 1, NULL);
	if(!res) {
		return -1;
	}
	else if(!pgsql_num_rows(res)) {
		pgsql_free(res);
		return 0;
	}
	show_info->entryid = strtoul(pgsql_nvalue(res, 0, "id"), NULL, 10);
	show_info->userid = strtoul(pgsql_nvalue(res, 0, "user_id"), NULL, 10);
	show_info->userid2 = (tmp = pgsql_nvalue(res, 0, "user_id_2")) ? strtoul(pgsql_nvalue(res, 0, "user_id_2"), NULL, 10) : 0;
	show_info->starttime = strtoul(pgsql_nvalue(res, 0, "start_ts"), NULL, 10);
	show_info->endtime = strtoul(pgsql_nvalue(res, 0, "end_ts"), NULL, 10);
	pgsql_free(res);
	return 0;
}

int get_conflicting_show_info(const struct show_info *show_info, struct show_info *conflict)
{
	PGresult *res;
	char buf[16];
	const char *tmp;
	struct stringlist *params = stringlist_create();
	memset(conflict, 0, sizeof(struct show_info));

	snprintf(buf, sizeof(buf), "%lu", show_info->starttime);
	stringlist_add(params, strdup(buf));
	snprintf(buf, sizeof(buf), "%lu", show_info->endtime);
	stringlist_add(params, strdup(buf));
	snprintf(buf, sizeof(buf), "%lu", show_info->entryid);
	stringlist_add(params, strdup(buf));

	res = pgsql_query(radioschedule_conf.pg_conn, "SELECT id, user_id, user_id_2, extract(epoch from start_time at time zone 'UTC') AS start_ts, extract(epoch from end_time at time zone 'UTC') AS end_ts \
			FROM shows WHERE ( \
			(start_time >= (to_timestamp($1) at time zone 'UTC') AND start_time < (to_timestamp($2) at time zone 'UTC')) OR \
			(end_time > (to_timestamp($1) at time zone 'UTC') AND end_time <= (to_timestamp($2) at time zone 'UTC')) OR \
			(start_time <= (to_timestamp($1) at time zone 'UTC') AND end_time >= (to_timestamp($2) at time zone 'UTC')) \
		) AND id != $3 ORDER BY start_time ASC LIMIT 1", 1, params);
	if(!res) {
		return -1;
	}
	else if(!pgsql_num_rows(res)) {
		pgsql_free(res);
		return 0;
	}
	conflict->entryid = strtoul(pgsql_nvalue(res, 0, "id"), NULL, 10);
	conflict->userid = strtoul(pgsql_nvalue(res, 0, "user_id"), NULL, 10);
	conflict->userid2 = (tmp = pgsql_nvalue(res, 0, "user_id_2")) ? strtoul(pgsql_nvalue(res, 0, "user_id_2"), NULL, 10) : 0;
	conflict->starttime = strtoul(pgsql_nvalue(res, 0, "start_ts"), NULL, 10);
	conflict->endtime = strtoul(pgsql_nvalue(res, 0, "end_ts"), NULL, 10);
	pgsql_free(res);
	return 0;
}

int extend_show(const struct show_info *show_info)
{
	PGresult *res;
	char buf[16];
	struct stringlist *params = stringlist_create();

	snprintf(buf, sizeof(buf), "%lu", show_info->endtime);
	stringlist_add(params, strdup(buf));
	snprintf(buf, sizeof(buf), "%lu", show_info->entryid);
	stringlist_add(params, strdup(buf));

	res = pgsql_query(radioschedule_conf.pg_conn, "UPDATE shows \
			SET end_time = to_timestamp($1) at time zone 'UTC' \
			WHERE id = $2", 1, params);
	if(!res || !pgsql_num_affected(res)) {
		if(res) {
			pgsql_free(res);
		}
		return -1;
	}
	pgsql_free(res);
	return 0;
}

int add_show(struct show_info *show_info, const char *show_title)
{
	PGresult *res;
	char buf[16];
	struct stringlist *params = stringlist_create();

	snprintf(buf, sizeof(buf), "%lu", show_info->userid);
	stringlist_add(params, strdup(buf));
	snprintf(buf, sizeof(buf), "%lu", show_info->userid2);
	stringlist_add(params, show_info->userid2 ? strdup(buf) : NULL);
	snprintf(buf, sizeof(buf), "%lu", show_info->starttime);
	stringlist_add(params, strdup(buf));
	snprintf(buf, sizeof(buf), "%lu", show_info->endtime);
	stringlist_add(params, strdup(buf));
	stringlist_add(params, strdup(show_title));

	res = pgsql_query(radioschedule_conf.pg_conn, "INSERT INTO shows \
			(user_id, user_id_2, create_date, start_time, end_time, title) VALUES \
			($1, $2, now()::date, (to_timestamp($3) at time zone 'UTC'), (to_timestamp($4) at time zone 'UTC'), $5)", 1, params);
	if(!res || !pgsql_num_affected(res)) {
		if(res) {
			pgsql_free(res);
		}
		return -1;
	}
	pgsql_free(res);
	return 0;
}

unsigned long lookup_userid(const char *nick)
{
	// 24576 = schedule.*
	return pgsql_query_int(radioschedule_conf.pg_conn, "SELECT id \
			FROM users \
			WHERE lower(username) = lower($1) AND (superuser OR (privs & 24576) != 0)",
			stringlist_build_n(1, nick));
}
