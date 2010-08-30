#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "irc.h"
#include "conf.h"
#include "table.h"
#include <mysql/mysql.h>

MODULE_DEPENDS("commands", NULL);

COMMAND(query);
COMMAND(pubquery);
static void ge_admin_conf_reload();
static MYSQL *mysql = NULL;

static struct
{
	const char *mysql_host;
	const char *mysql_user;
	const char *mysql_pass;
	const char *mysql_db;
} ge_admin_conf;

static struct module *this;

MODULE_INIT
{
	this = self;

	reg_conf_reload_func(ge_admin_conf_reload);
	ge_admin_conf_reload();

	mysql_library_init(-1, NULL, NULL);

	DEFINE_COMMAND(this, "query",		query,		1, CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED, "group(admins)");
	DEFINE_COMMAND(this, "pubquery",	pubquery,	1, CMD_ACCEPT_CHANNEL | CMD_REQUIRE_CHANNEL | CMD_LOG_HOSTMASK | CMD_REQUIRE_AUTHED, "group(admins)");
}

MODULE_FINI
{
	mysql_library_end();
	unreg_conf_reload_func(ge_admin_conf_reload);
}

static void ge_admin_conf_reload()
{
	char *str;

	str = conf_get("galaxyempire_admin/mysql_host", DB_STRING);
	ge_admin_conf.mysql_host = str ? str : "localhost";

	str = conf_get("galaxyempire_admin/mysql_user", DB_STRING);
	ge_admin_conf.mysql_user = str ? str : "root";

	str = conf_get("galaxyempire_admin/mysql_pass", DB_STRING);
	ge_admin_conf.mysql_pass = str ? str : "";

	str = conf_get("galaxyempire_admin/mysql_db", DB_STRING);
	ge_admin_conf.mysql_db = str ? str : "test";
}

static int db_connect(struct irc_source *src)
{
	mysql = mysql_init(NULL);
	if(!mysql_real_connect(mysql, ge_admin_conf.mysql_host, ge_admin_conf.mysql_user, ge_admin_conf.mysql_pass, ge_admin_conf.mysql_db, 0, NULL, 0))
	{
		reply("Database connection failed: %s", mysql_error(mysql));
		return 0;
	}

	return 1;
}

static void db_disconnect()
{
	mysql_close(mysql);
	mysql = NULL;
}

COMMAND(query)
{
	unsigned int ret = 1;
	if(!db_connect(src))
		return 0;

	char *query = untokenize(argc - 1, argv + 1, " ");

	if(mysql_query(mysql, query))
	{
		reply("Database query failed: %s", mysql_error(mysql));
		ret = 0;
		goto out;
	}

	MYSQL_RES *result = mysql_store_result(mysql);
	if(!result) // No results: error or command not returning results
	{
		if(mysql_field_count(mysql) == 0) // simply no results
		{
			unsigned long long affected_rows = mysql_affected_rows(mysql);
			reply("Query successful; %llu row%s affected", affected_rows, (affected_rows == 1 ? "" : "s"));
		}
		else // error
		{
			reply("Could not fetch query results: %s", mysql_error(mysql));
			ret = 0;
		}

		goto out;
	}

	// We have results
	MYSQL_FIELD *fields;
	MYSQL_ROW row;
	unsigned long long num_fields = mysql_num_fields(result);
	unsigned long long num_rows = mysql_num_rows(result);
	reply("Query returned %llu row%s", num_rows, (num_rows == 1 ? "" : "s"));

	struct table *table = table_create(num_fields, num_rows);
	table->header = calloc(table->cols, sizeof(char *));
	fields = mysql_fetch_fields(result);
	for(unsigned long long i = 0; i < num_fields; i++)
		table->header[i] = fields[i].name;

	for(unsigned long long i = 0; i < num_rows; i++)
	{
		row = mysql_fetch_row(result);
		for(unsigned long long j = 0; j < num_fields; j++)
			table->data[i][j] = row[j] ? row[j] : "NULL";
	}

	table_send(table, src->nick);
	table_free(table);
	mysql_free_result(result);

out:
	free(query);
	db_disconnect();
	return ret;
}

COMMAND(pubquery)
{
	unsigned int ret = 1;
	if(!db_connect(src))
		return 0;

	char *query = untokenize(argc - 1, argv + 1, " ");

	if(mysql_query(mysql, query))
	{
		reply("Database query failed: %s", mysql_error(mysql));
		ret = 0;
		goto out;
	}

	MYSQL_RES *result = mysql_store_result(mysql);
	if(!result) // No results: error or command not returning results
	{
		if(mysql_field_count(mysql) == 0) // simply no results
		{
			unsigned long long affected_rows = mysql_affected_rows(mysql);
			irc_send("PRIVMSG %s :Query successful; %llu row%s affected", channel->name, affected_rows, (affected_rows == 1 ? "" : "s"));
		}
		else // error
		{
			reply("Could not fetch query results: %s", mysql_error(mysql));
			ret = 0;
		}

		goto out;
	}

	// We have results
	MYSQL_FIELD *fields;
	MYSQL_ROW row;
	unsigned long long num_fields = mysql_num_fields(result);
	unsigned long long num_rows = mysql_num_rows(result);
	irc_send("PRIVMSG %s :Query returned %llu row%s", channel->name, num_rows, (num_rows == 1 ? "" : "s"));

	struct table *table = table_create(num_fields, num_rows);
	table->header = calloc(table->cols, sizeof(char *));
	fields = mysql_fetch_fields(result);
	for(unsigned long long i = 0; i < num_fields; i++)
		table->header[i] = fields[i].name;

	for(unsigned long long i = 0; i < num_rows; i++)
	{
		row = mysql_fetch_row(result);
		for(unsigned long long j = 0; j < num_fields; j++)
			table->data[i][j] = row[j] ? row[j] : "NULL";
	}

	table_send_pm(table, channel->name);
	table_free(table);
	mysql_free_result(result);

out:
	free(query);
	db_disconnect();
	return ret;
}
