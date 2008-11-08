#include "global.h"
#include "module.h"
#include "sock.h"
#include "conf.h"
#include "timer.h"
#include "stringbuffer.h"
#include "db.h"
#include <libpq-fe.h>

MODULE_DEPENDS(NULL);

IMPLEMENT_SLIST(db_nv_list, struct db_named_value)

static const char *db_type_names[] = {
	"integer",
	"datetime",
	"string",
	"serial",
	NULL
};

struct pgsql_async
{
	struct db_table *table;
	PGconn *conn;
	struct sock *sock;
	struct pgsql_async *next;
	enum {
		PGSQL_ASYNC_CONNECTING,
		PGSQL_ASYNC_QUERY,
		PGSQL_ASYNC_BUSY,
		PGSQL_ASYNC_ABORTING,
		PGSQL_ASYNC_IDLE,
		PGSQL_ASYNC_DEAD
	} state;
	db_select_cb *cb;
	void *ctx;
	db_free_ctx_f *free_ctx_func;
	struct db_nv_list *values;
	struct db_nv_list *filter;
};


struct pgsql_database
{
	PGconn *conn;
	struct pgsql_async *async;
} db_;

static const char *pgsql_select_table_oid = "SELECT oid FROM pg_class WHERE relname=$1";
static const char *pgsql_select_column_typname = "SELECT typname FROM pg_type INNER JOIN pg_attribute ON pg_type.oid=pg_attribute.atttypid WHERE pg_attribute.attname=$2 AND pg_attribute.attrelid=$1;";
static const char *pgsql_type_name[DBTYPE_NUM_TYPES] = {
	"int4 not null default 0",
	"timestamp",
	"text not null default ''",
	"serial"
};

static struct {
	const char *connect_string;
} db_conf = { 0 };

static void db_conf_reload();
static db_serial_t pgsql_get_serial(struct db_table *table, struct column_desc *desc, struct stringlist *values);
static int pgsql_send_query(struct pgsql_async *async);
static void pgsql_async_event(struct sock *sock, enum sock_event event, int err);
static struct pgsql_async *pgsql_get_async(struct db_table *table);
static void db_table_free(struct db_table *table);
static int db_vget_values(struct db_table *table, va_list *args, struct db_nv_list *values, db_serial_t **pserial, int *pserial_idx);
static int db_vget_names(struct db_table *table, va_list *args, struct db_nv_list *values);
static int db_get_values(struct db_table *table, struct stringbuffer *cv, struct stringlist *values, struct db_nv_list *nvv, const char *sep, int reject_write_only);
static int db_put_values(struct db_table *table, PGresult *res, int row, struct db_nv_list *values, int dup);
static int db_vput_values_in(struct db_table *table, va_list *args, struct db_nv_list *values);
static int db_vput_values_out(struct db_table *table, va_list *args, struct db_nv_list *values);
static PGresult *do_PQexecParams(PGconn **conn, const char *command, int nParams, char **paramValues);
static PGresult *do_PQexec(PGconn **conn, const char *command);
static int do_row_insert(struct db_table *table, struct db_nv_list *values);
static int do_row_update(struct db_table *table, struct db_nv_list *filter, struct db_nv_list *updates);
static int do_row_drop(struct db_table *table, struct db_nv_list *filter);
static int do_row_get(struct db_table *table, struct db_nv_list *filter, struct db_nv_list *values);
static int do_async_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, struct db_nv_list *filter, struct db_nv_list *values);
static int do_sync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, struct db_nv_list *filter, struct db_nv_list *values);
#ifdef DB_TEST
static void run_test();
struct db_table *test_table;
#endif

static struct module *this;
struct pgsql_database *db = &db_;

MODULE_INIT
{
	this = self;

	reg_conf_reload_func(db_conf_reload);
	db_conf_reload();

	db->conn = PQconnectdb(db_conf.connect_string);
	if(PQstatus(db->conn) == CONNECTION_BAD)
		log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(db->conn));
	db->async = NULL;

#ifdef DB_TEST
	debug("DB TEST\n\n\n");
	run_test();
	printf("\n\n\n");
	debug("DB TEST END");
#endif
}

MODULE_FINI
{
	while(db->async)
	{
		struct pgsql_async *async = db->async;
		db->async = async->next;
		if(async->conn)
			PQfinish(async->conn);
		if(async->sock)
			sock_close(async->sock);
		if(async->values)
		{
			db_nv_list_clear(async->values);
			free(async->values);
		}
		free(async);
	}

#ifdef DB_TEST
	if(test_table)
		db_table_close(test_table);
#endif
	PQfinish(db->conn);
	unreg_conf_reload_func(db_conf_reload);
}

static void db_conf_reload()
{
	char *str;
	const char *old_cs;

	old_cs = db_conf.connect_string;
	db_conf.connect_string = ((str = conf_get("db/connect_string", DB_STRING)) ? str : NULL);

	if(!db_conf.connect_string || !*db_conf.connect_string)
	{
		log_append(LOG_ERROR, "/db/connect_string must be set and not empty");
		db_conf.connect_string = old_cs ? old_cs : "";
	}

	if(old_cs && strcasecmp(db_conf.connect_string, old_cs))
	{
		debug("Changing connect string from '%s' to '%s'", old_cs, db_conf.connect_string);
		if(db->conn)
			PQfinish(db->conn);

		db->conn = PQconnectdb(db_conf.connect_string);
		if(PQstatus(db->conn) == CONNECTION_BAD)
			log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(db->conn));

		for(struct pgsql_async *async = db->async; async; async = async->next)
		{
			if(async->state == PGSQL_ASYNC_IDLE)
			{
				debug("Killing idle async pgsql connection");
				PQfinish(async->conn);
				async->conn = NULL;
				async->state = PGSQL_ASYNC_DEAD;
			}
		}
	}
}


static int pgsql_send_query(struct pgsql_async *async)
{
	struct stringbuffer *query;
	struct stringlist *params;
	int rval;

	debug("sending async query; state: %d; PQstatus = %d", async->state, PQstatus(async->conn));

	query = stringbuffer_create();
	params = stringlist_create();
	stringbuffer_append_printf(query, "SELECT * FROM \"%s\" ", async->table->name);
	if(async->filter->count)
	{
		stringbuffer_append_string(query, "WHERE ");
		rval = db_get_values(async->table, query, params, async->filter, " AND ", 0);
		if(rval)
			goto out;
	}

	if(!PQsendQueryParams(async->conn, query->string, params->count, NULL, (const char*const*)params->data, NULL, NULL, 0))
	{
		debug("PQsendQueryParams failed: %s", PQerrorMessage(async->conn));
		rval = 1;
		goto out;
	}

	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < params->count; ++ii)
		debug("   $%u = %s", ii+1, params->data[ii]);
	rval = 0;
out:
	debug("rval = %d", rval);
	if(rval == 0)
	{
		if(PQflush(async->conn) == 0)
		{
			async->sock->want_write = 0;
			async->sock->want_read = 1;
		}
		else
		{
			async->sock->want_write = 1;
			async->sock->want_read = 0;
		}
	}

	async->state = rval ? PGSQL_ASYNC_IDLE : PGSQL_ASYNC_BUSY;
	db_nv_list_clear(async->filter);
	free(async->filter);
	async->filter = NULL;
	if(rval != 0)
	{
		db_nv_list_clear(async->values);
		free(async->values);
		async->values = NULL;
		async->cb(async->ctx, NULL, 1);
		if(rval == 1)
		{
			PQfinish(async->conn);
			async->conn = NULL;
			async->state = PGSQL_ASYNC_DEAD;
		}
	}
	stringlist_free(params);
	stringbuffer_free(query);
	return rval;
}

static void pgsql_async_event(struct sock *sock, enum sock_event event, int err)
{
	struct pgsql_async *async = (struct pgsql_async *)sock->ctx;

	debug("pg async event: %d", event);
	if(async->state == PGSQL_ASYNC_CONNECTING)
	{
		debug("replacing async event with EV_CONNECT");
		event = EV_CONNECT;
	}

	switch(event)
	{
		case EV_WRITE:
			if(PQflush(async->conn) == 0)
			{
				sock->want_write = 0;
				sock->want_read = 1;
			}
			break;

		case EV_CONNECT:{
			unsigned int ready = 0;
			switch(PQconnectPoll(async->conn))
			{
				case PGRES_POLLING_READING:
					sock->want_read = 1;
					sock->want_write = 0;
					break;

				case PGRES_POLLING_WRITING:
					sock->want_read = 0;
					sock->want_write = 1;
					break;

				case PGRES_POLLING_FAILED:
					if(async->cb)
						async->cb(async->ctx, NULL, 1);
					if(async->values)
					{
						db_nv_list_clear(async->values);
						free(async->values);
						async->values = NULL;
					}
					PQfinish(async->conn);
					async->conn = NULL;
					async->state = PGSQL_ASYNC_DEAD;
					log_append(LOG_ERROR, "Async connection failed to database.");
					break;

				case PGRES_POLLING_OK:
					ready = 1;
					async->state = async->filter ? PGSQL_ASYNC_QUERY : PGSQL_ASYNC_IDLE;
					break;

				default: /* needed to squash gcc warning for the deprecated PGRES_POLLING_ACTIVE enum */
					break;
			}

			if(async->state == PGSQL_ASYNC_QUERY && ready)
				pgsql_send_query(async);

			break;
		}

		case EV_READ:
			if(!PQconsumeInput(async->conn))
			{
				log_append(LOG_ERROR, "PQconsumeInput failed: %s", PQerrorMessage(async->conn));
				if(async->values)
				{
					db_nv_list_clear(async->values);
					free(async->values);
					async->values = NULL;
				}
				PQfinish(async->conn);
				async->conn = NULL;
				async->state = PGSQL_ASYNC_DEAD;
				break;
			}

			/* If it is still busy, we cannot do anything further. */
			while(!PQisBusy(async->conn))
			{
				PGresult *res;
				unsigned int ii, count;

				debug("entered PQisBusy loop");
				/* Get result row; if done, release async and break out. */
				res = PQgetResult(async->conn);
				if(!res)
				{
					debug("PQgetResult == NULL");
					async->sock->want_read = 0;
					if(async->values)
					{
						db_nv_list_clear(async->values);
						free(async->values);
						async->values = NULL;
					}
					if(async->free_ctx_func)
						async->free_ctx_func(async->ctx);
					async->cb = NULL;
					async->state = PGSQL_ASYNC_IDLE;
					debug("state is now %d", async->state);
					break;
				}

				debug("PQntuples = %d", PQntuples(res));
				for(ii = 0, count = PQntuples(res); ii < count; ++ii)
				{
					/* Usable row: unpack into async->values. */
					if(db_put_values(async->table, res, ii, async->values, 0))
						continue;
					/* Make callback, maybe asking to stop. */
					if((async->state == PGSQL_ASYNC_BUSY) && async->cb(async->ctx, async->values, 0))
					{
						async->state = PGSQL_ASYNC_ABORTING;
						PQrequestCancel(async->conn);
					}
				}
				/* Release the result object. */
				PQclear(res);
			}

			if(async->state == PGSQL_ASYNC_QUERY)
				pgsql_send_query(async);
			break;

		default:
			break;
	}

	debug("event func complete");
}

static struct pgsql_async *pgsql_get_async(struct db_table *table)
{
	struct pgsql_async *async;

	/* Search for a currently idle request. */
	for(async = db->async; async; async = async->next)
		if(async->state == PGSQL_ASYNC_IDLE)
			return async;

	unsigned int revive = 0;
	/* Search for a currently idle request. */
	for(async = db->async; async; async = async->next)
	{
		if(async->state == PGSQL_ASYNC_DEAD)
		{
			debug("Reviving dead async struct");
			revive = 1;
			break;
		}
	}

	/* Make sure we still have the connection argument. */
	/*
	args = conf_get_child(db->cfg, "args", CONF_STRING);
	if (!args) {
		log_append(LOG_ERROR, "'args' config field disappeared for database %s.", db->base.name);
		return NULL;
	}
	*/

	/* Allocate and link in a new connection (so we do not leak it in
	 * the event of a later failure).
	 */
	if(!revive)
	{
		debug("Creating new async struct");
		async = malloc(sizeof(struct pgsql_async));
		memset(async, 0, sizeof(struct pgsql_async));
		async->next = db->async;
		db->async = async;
	}

	async->table = table;

	/* Start the connection. */
	async->conn = PQconnectStart(db_conf.connect_string);
	if(!async->conn)
	{
		log_append(LOG_ERROR, "Unable to PQconnectStart().");
		return NULL;
	}

	PQsetnonblocking(async->conn, 1);
	async->state = PGSQL_ASYNC_CONNECTING;
	int status = PQstatus(async->conn);
	debug("PQstatus after connect = %d", status);
	switch(status)
	{
		case CONNECTION_BAD:
			log_append(LOG_ERROR, "Async connection failed to database.");
			return NULL;

		default:
			async->sock = sock_create(SOCK_NOSOCK, pgsql_async_event, NULL);
			async->sock->config_poll = 1;
			async->sock->want_write = 1;
			async->sock->ctx = async;
			async->sock->flags |= SOCK_CONNECT;
			sock_set_fd(async->sock, PQsocket(async->conn));
			debug("add socket with SOCK_CONNECT");
			break;
	}
	return async;
}

const char *db_type_to_name(enum db_type type)
{
	if(type < ArraySize(db_type_names))
		return db_type_names[type];
	else
		return NULL;
}

enum db_type db_type_from_name(const char *name)
{
	enum db_type type;
	for(type = 0; type < DBTYPE_NUM_TYPES; ++type)
		if(!strcmp(db_type_names[type], name))
			break;
	return type;
}

struct db_table *db_table_open(const char *name, const struct column_desc *cols)
{
	struct db_table *table;
	const char *params[3], *value;
	char oid[32];
	PGresult *res;
	unsigned int valid;

	/* Does the table exist? */
	params[0] = name;
	res = do_PQexecParams(&db->conn, pgsql_select_table_oid, 1, (char **)params);
	if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		log_append(LOG_ERROR, "Error trying to find table %s: %s", name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		return NULL;
	}

	if(!PQntuples(res)) /* If not, create it. */
	{
		struct stringbuffer *cv;
		PQclear(res);
		cv = stringbuffer_create();
		stringbuffer_append_printf(cv, "CREATE TABLE \"%s\" (", name);
		for(unsigned int ii = 0; cols[ii].name; ++ii)
		{
			if(ii)
				stringbuffer_append_string(cv, ", ");
			stringbuffer_append_printf(cv, "\"%s\" %s", cols[ii].name, pgsql_type_name[cols[ii].type]);
		}
		stringbuffer_append_string(cv, ") WITHOUT OIDS");
		res = PQexec(db->conn, cv->string);
		if(!res || (PQresultStatus(res) != PGRES_COMMAND_OK))
		{
			log_append(LOG_ERROR, "Error trying to create table %s: %s", name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
			PQclear(res);
			stringbuffer_free(cv);
			return NULL;
		}
		stringbuffer_free(cv);
		PQclear(res);
	}
	else
	{
		/* Otherwise make sure all columns are compatible. */
		/* Copy the table's OID. */
		strlcpy(oid, PQgetvalue(res, 0, 0), sizeof(oid));
		params[0] = oid;
		PQclear(res);
		/* Check each column we want in the table. */
		for(unsigned int ii = 0; cols[ii].name; ++ii)
		{
			/* Query the column's type. */
			params[1] = cols[ii].name;
			res = do_PQexecParams(&db->conn, pgsql_select_column_typname, 2, (char **)params);
			if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK))
			{
				log_append(LOG_ERROR, "Error trying to find type for column %s in table %s: %s", params[1], name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
				PQclear(res);
				return NULL;
			}

			/* If column is missing, try to add it. */
			if(!PQntuples(res))
			{
				struct stringbuffer *cv;
				PGresult *res2;

				log_append(LOG_INFO, "Table %s missing column %s, attempting to create it.", name, params[1]);
				cv = stringbuffer_create();
				stringbuffer_append_printf(cv, "ALTER TABLE \"%s\" ADD COLUMN \"%s\" %s", name, params[1], pgsql_type_name[cols[ii].type]);
				res2 = do_PQexec(&db->conn, cv->string);
				stringbuffer_free(cv);
				if(!res2 || (PQresultStatus(res2) != PGRES_COMMAND_OK))
				{
					log_append(LOG_ERROR, "Unable to add missing column %s to table %s: %s", params[1], name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
					PQclear(res2);
					return NULL;
				}
				PQclear(res2);
				continue;
			}

			/* See if the type is known to be compatible. */
			value = PQgetvalue(res, 0, 0);
			switch(cols[ii].type)
			{
				case DBTYPE_INTEGER:
					valid = !strcmp(value, "int4") || !strcmp(value, "int8");
					break;

				case DBTYPE_DATETIME:
					valid = !strcmp(value, "timestamp");
					break;

				case DBTYPE_STRING:
					valid = !strcmp(value, "varchar") || !strcmp(value, "text");
					break;

				case DBTYPE_SERIAL:
					valid = !strcmp(value, "int4") || !strcmp(value, "int8") || !strcmp(value, "serial");
					break;

				default:
					valid = 0;
					break;
			}
			PQclear(res);
			if(!valid)
			{
				log_append(LOG_ERROR, "Column %s in table %s has incompatible type %s.", params[1], name, value);
				return NULL;
			}
		}
	}

	table = malloc(sizeof(struct db_table));
	memset(table, 0, sizeof(struct db_table));

	table->name = name;
	table->columns = dict_create();
	dict_set_free_funcs(table->columns, free, free);
	for(int i = 0; cols[i].name; ++i)
	{
		struct column_desc *cd = malloc(sizeof(struct column_desc));
		cd->name = strdup(cols[i].name);
		cd->type = cols[i].type;
		dict_insert(table->columns, cd->name, cd);
	}

	return table;
}

static void db_table_free(struct db_table *table)
{
	dict_free(table->columns);
	free(table);
}

void db_table_close(struct db_table *table)
{
	db_table_free(table);
}

static int db_vget_values(struct db_table *table, va_list *args, struct db_nv_list *values, db_serial_t **pserial, int *pserial_idx)
{
	struct column_desc *cd;
	struct db_named_value dvv;

	values->count = 0;
	if(pserial)
		*pserial = 0;

	while((dvv.name = va_arg(*args, const char*)) != NULL)
	{
		if(!(cd = dict_find(table->columns, dvv.name)))
		{
			log_append(LOG_ERROR, "Attempt to use non-existent column %s in table %s.", dvv.name, table->name);
			db_nv_list_clear(values);
			return 1;
		}

		dvv.name = cd->name; /* use canonical case, just in case */
		switch(cd->type)
		{
			case DBTYPE_INTEGER:
				dvv.u.integer = va_arg(*args, int);
				break;
			case DBTYPE_DATETIME:
				dvv.u.datetime = va_arg(*args, time_t);
				break;
			case DBTYPE_STRING:
				dvv.u.string = va_arg(*args, char *);
				break;
			case DBTYPE_SERIAL:
				if(pserial)
				{
					*pserial = va_arg(*args, db_serial_t*);
					*pserial_idx = values->count;
					dvv.u.serial = 0;
				}
				else
					dvv.u.serial = va_arg(*args, db_serial_t);
				break;
			default:
				log_append(LOG_ERROR, "Attempt to use column %s in table %s with unhandled type %d.", dvv.name, table->name, cd->type);
				db_nv_list_clear(values);
				return 2;
		}
		db_nv_list_add(values, dvv);
	}
	return 0;
}

static int db_vget_names(struct db_table *table, va_list *args, struct db_nv_list *values)
{
	struct column_desc *cd;
	struct db_named_value dvv;

	values->count = 0;
	while((dvv.name = va_arg(*args, const char*)) != NULL)
	{
		if(!(cd = dict_find(table->columns, dvv.name)))
		{
			log_append(LOG_ERROR, "Attempt to use non-existent column %s in table %s.", dvv.name, table->name);
			db_nv_list_clear(values);
			return 1;
		}

		dvv.name = cd->name; /* use canonical version of name */
		db_nv_list_add(values, dvv);
	}

	return 0;
}

static int db_get_values(struct db_table *table, struct stringbuffer *cv, struct stringlist *values, struct db_nv_list *nvv, const char *sep, int reject_write_only)
{
	struct column_desc *desc;
	int rval;
	char value[64];

	for(unsigned int ii = 0; ii < nvv->count; ++ii)
	{
		const struct db_named_value *dnv;

		/* Find the column or bail out. */
		dnv = &nvv->data[ii];
		desc = dict_find(table->columns, dnv->name);
		if(!desc)
		{
			log_append(LOG_ERROR, "Tried to operate on %s with non-existent column %s.", table->name, dnv->name);
			rval = 1;
			goto out;
		}

		/* Append element to query string */
		if(ii)
			stringbuffer_append_string(cv, sep);
		stringbuffer_append_printf(cv, "\"%s\"=$%u", dnv->name, values->count + 1);

		/* Figure out what type it is and append that to query value list. */
		switch(desc->type)
		{
			case DBTYPE_INTEGER:
				snprintf(value, sizeof(value), "%d", dnv->u.integer);
				stringlist_add(values, strdup(value));
				break;

			case DBTYPE_DATETIME: {
				struct tm tm;

				memset(&tm, 0, sizeof(tm));
				if (!gmtime_r(&dnv->u.datetime, &tm)) {
					log_append(LOG_ERROR, "Could not gmtime_r(%ld).", dnv->u.datetime);
					rval = 2;
					goto out;
				}

				if (!strftime(value, sizeof(value), "%Y-%m-%d %H:%M:%S-00", &tm)) {
					log_append(LOG_ERROR, "Could not strftime(%ld).", dnv->u.datetime);
					rval = 3;
					goto out;
				}

				stringlist_add(values, strdup(value));
				break;
			}

			case DBTYPE_STRING:
				stringlist_add(values, strdup(dnv->u.string));
				break;

			case DBTYPE_SERIAL:
				if(reject_write_only)
				{
					log_append(LOG_ERROR, "Must not write to serial column %s in table %s.", desc->name, table->name);
					rval = 5;
					goto out;
				}
				snprintf(value, sizeof(value), "%d", dnv->u.serial);
				stringlist_add(values, strdup(value));
				break;

			default:
				log_append(LOG_ERROR, "Unhandled type %d for column %s in table %s.", desc->type, desc->name, table->name);
				rval = 4;
				goto out;
		}
	}
	rval = 0;

out:
	return rval;
}

static int db_put_values(struct db_table *table, PGresult *res, int row, struct db_nv_list *values, int dup)
{
	struct column_desc *desc;
	const char *value;
	int field;

	for(unsigned int ii = 0; ii < values->count; ++ii)
	{
		struct db_named_value *dnv;

		dnv = &values->data[ii];
		field = PQfnumber(res, dnv->name);
		if(field < 0)
		{
			log_append(LOG_ERROR, "Tried to get a non-existent field %s from query results.", dnv->name);
			return 6;
		}
		desc = dict_find(table->columns, dnv->name);
		assert_return(desc != NULL, 7);
		value = PQgetvalue(res, row, field);
		switch(desc->type)
		{
			case DBTYPE_INTEGER:
				dnv->u.integer = value ? atoi(value) : 0;
				break;

			case DBTYPE_DATETIME:
				if(value)
				{
					struct tm tm;
					memset(&tm, 0, sizeof(tm));
					if(strptime(value, "%Y-%m-%d %H:%M:%S", &tm))
						dnv->u.datetime = mktime(&tm);
					else
						dnv->u.datetime = 0;
				}
				else
				{
					dnv->u.datetime = 0;
				}
				break;

			case DBTYPE_STRING:
				dnv->u.string = (value && dup) ? strdup(value) : (char *)value;
				break;

			case DBTYPE_SERIAL:
				dnv->u.serial = value ? atoi(value) : 0;
				break;

			default:
				log_append(LOG_ERROR, "Unhandled type %d for column %s in table %s.", desc->type, desc->name, table->name);
				return 4;
		}
	}

	return 0;
}

static int db_vput_values_in(struct db_table *table, va_list *args, struct db_nv_list *values)
{
	struct column_desc *cd;
	struct db_named_value dvv;

	values->count = 0;
	while((dvv.name = va_arg(*args, const char*)) != NULL)
	{
		if(!(cd = dict_find(table->columns, dvv.name)))
		{
			log_append(LOG_ERROR, "Attempt to use non-existent column %s in table %s.", dvv.name, table->name);
			db_nv_list_clear(values);
			return 1;
		}

		dvv.name = cd->name; /* use canonical case, just in case */
		switch(cd->type)
		{
			case DBTYPE_INTEGER:
				(void)va_arg(*args, int*);
				break;

			case DBTYPE_DATETIME:
				(void)va_arg(*args, time_t*);
				break;

			case DBTYPE_STRING:
				(void)va_arg(*args, char **);
				break;

			case DBTYPE_SERIAL:
				(void)va_arg(*args, db_serial_t*);
				break;

			default:
				log_append(LOG_ERROR, "Attempt to use column %s in table %s with unhandled type %d.", dvv.name, table->name, cd->type);
				db_nv_list_clear(values);
				return 2;
		}
		db_nv_list_add(values, dvv);
	}
	return 0;
}

static int db_vput_values_out(struct db_table *table, va_list *args, struct db_nv_list *values)
{
	struct column_desc *cd;

	for(unsigned int ii = 0; ii < values->count; ++ii)
	{
		const char *name = va_arg(*args, const char*);
		struct db_named_value *dvv = &values->data[ii];

		assert_return(!strcasecmp(name, dvv->name), 3);
		if(!(cd = dict_find(table->columns, dvv->name)))
		{
			log_append(LOG_ERROR, "Attempt to use non-existent column %s in table %s.", dvv->name, table->name);
			db_nv_list_clear(values);
			return 1;
		}

		switch(cd->type)
		{
			case DBTYPE_INTEGER: {
				int *res = va_arg(*args, int*);
				*res = dvv->u.integer;
				break;
			}

			case DBTYPE_DATETIME: {
				time_t *res = va_arg(*args, time_t*);
				*res = dvv->u.datetime;
				break;
			}

			case DBTYPE_STRING: {
				char **res = va_arg(*args, char **);
				*res = dvv->u.string;
				break;
			}

			case DBTYPE_SERIAL: {
				int *res = va_arg(*args, db_serial_t*);
				*res = dvv->u.serial;
				break;
			}

			default:
				log_append(LOG_ERROR, "Attempt to use column %s in table %s with unhandled type %d.", dvv->name, table->name, cd->type);
				db_nv_list_clear(values);
				return 2;
		}
	}
	return 0;
}

static db_serial_t pgsql_get_serial(struct db_table *table, struct column_desc *desc, struct stringlist *values)
{
	struct stringbuffer *cv;
	PGresult *res;
	char *value;

	cv = stringbuffer_create();
	stringbuffer_append_printf(cv, "SELECT nextval('%s_%s_seq')", table->name, desc->name);
	res = do_PQexec(&db->conn, cv->string);
	if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		log_append(LOG_ERROR, "Error trying to get serial number from %s.%s: %s", table->name, desc->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		stringbuffer_free(cv);
		return 0;
	}
	stringbuffer_free(cv);
	value = strdup(PQgetvalue(res, 0, 0));
	stringlist_add(values, value);
	PQclear(res);
	return atoi(value);
}

int db_row_insert(struct db_table *table, ...)
{
	struct db_nv_list values;
	va_list args;
	db_serial_t *serial;
	int serial_idx, res;

	memset(&values, 0, sizeof(values));
	va_start(args, table);
	res = db_vget_values(table, &args, &values, &serial, &serial_idx);
	va_end(args);
	if(res)
		return res;
	res = do_row_insert(table, &values);
	if(serial && !res)
		*serial = values.data[serial_idx].u.serial;
	db_nv_list_clear(&values);
	return res;
}

int db_row_update(struct db_table *table, ...)
{
	struct db_nv_list filter, values;
	va_list args;
	int res;

	memset(&filter, 0, sizeof(filter));
	memset(&values, 0, sizeof(values));
	va_start(args, table);
	res = db_vget_values(table, &args, &filter, NULL, NULL);
	if(res)
	{
		va_end(args);
		return res;
	}

	res = db_vget_values(table, &args, &values, NULL, NULL);
	va_end(args);
	if(res)
	{
		db_nv_list_clear(&filter);
		return res;
	}
	res = do_row_update(table, &filter, &values);
	db_nv_list_clear(&filter);
	db_nv_list_clear(&values);
	return res;
}

int db_row_drop(struct db_table *table, ...)
{
	struct db_nv_list values;
	va_list args;
	int res;

	memset(&values, 0, sizeof(values));
	va_start(args, table);
	res = db_vget_values(table, &args, &values, NULL, NULL);
	va_end(args);
	if(res)
		return res;
	res = do_row_drop(table, &values);
	db_nv_list_clear(&values);
	return res;
}

int db_row_get(struct db_table *table, ...)
{
	struct db_nv_list filter, values;
	va_list args, midway;
	int res;

	memset(&filter, 0, sizeof(filter));
	memset(&values, 0, sizeof(values));
	va_start(args, table);
	res = db_vget_values(table, &args, &filter, NULL, NULL);
	if(res)
	{
		va_end(args);
		return res;
	}
	va_copy(midway, args);
	res = db_vput_values_in(table, &args, &values);
	va_end(args);
	if(res)
	{
		db_nv_list_clear(&filter);
		return res;
	}
	res = do_row_get(table, &filter, &values);
	if(!res)
		res = db_vput_values_out(table, &midway, &values);
	va_end(midway);
	db_nv_list_clear(&filter);
	db_nv_list_clear(&values);
	return res;
}

int db_vsync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, va_list *args)
{
	struct db_nv_list filter, values;
	int res;

	memset(&filter, 0, sizeof(filter));
	memset(&values, 0, sizeof(values));
	res = db_vget_values(table, args, &filter, NULL, NULL);
	if(res)
		return res;

	res = db_vget_names(table, args, &values);
	if(res)
	{
		db_nv_list_clear(&filter);
		return res;
	}
	res = do_sync_select(table, cb, ctx, free_ctx_func, &filter, &values);
	db_nv_list_clear(&filter);
	db_nv_list_clear(&values);
	return res;
}

int db_sync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, ...)
{
	va_list args;
	int res;

	va_start(args, free_ctx_func);
	res = db_vsync_select(table, cb, ctx, free_ctx_func, &args);
	va_end(args);
	return res;
}

int db_vasync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, va_list *args)
{
	struct db_nv_list *filter, *values;
	int res;

	filter = malloc(sizeof(struct db_nv_list));
	values = malloc(sizeof(struct db_nv_list));
	memset(filter, 0, sizeof(struct db_nv_list));
	memset(values, 0, sizeof(struct db_nv_list));

	res = db_vget_values(table, args, filter, NULL, NULL);
	if(res)
		return res;
	res = db_vget_names(table, args, values);
	if(res)
		return res;
	res = do_async_select(table, cb, ctx, free_ctx_func, filter, values);
	return res;
}

int db_async_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, ...)
{
	va_list args;
	int res;

	va_start(args, free_ctx_func);
	res = db_vasync_select(table, cb, ctx, free_ctx_func, &args);
	va_end(args);
	return res;
}

// actual db access functions
static PGresult *do_PQexecParams(PGconn **conn, const char *command, int nParams, char **paramValues)
{
	PGresult *res;

	if(PQstatus(*conn) == CONNECTION_BAD)
	{
		debug("reconnecting - before query");
		PQfinish(*conn);
		*conn = PQconnectdb(db_conf.connect_string);
		if(PQstatus(*conn) == CONNECTION_BAD)
			log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(*conn));
	}
	res = PQexecParams(*conn, command, nParams, NULL, (const char*const*)paramValues, NULL, NULL, 0);
	debug("conn status after query: %d bad: %d", PQstatus(*conn), PQstatus(*conn) == CONNECTION_BAD);
	if(PQstatus(*conn) == CONNECTION_BAD)
	{
		debug("reconnecting - after query");
		if(res)
			PQclear(res);
		PQfinish(*conn);
		*conn = PQconnectdb(db_conf.connect_string);
		if(PQstatus(*conn) == CONNECTION_BAD)
			log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(*conn));
		res = PQexecParams(*conn, command, nParams, NULL, (const char*const*)paramValues, NULL, NULL, 0);
	}

	return res;
}

static PGresult *do_PQexec(PGconn **conn, const char *command)
{
	PGresult *res;

	if(PQstatus(*conn) == CONNECTION_BAD)
	{
		debug("reconnecting - before query");
		*conn = PQconnectdb(db_conf.connect_string);
		if(PQstatus(*conn) == CONNECTION_BAD)
			log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(*conn));
	}
	res = PQexec(*conn, command);
	if(PQstatus(*conn) == CONNECTION_BAD)
	{
		debug("reconnecting - after query");
		if(res)
			PQclear(res);
		*conn = PQconnectdb(db_conf.connect_string);
		if(PQstatus(*conn) == CONNECTION_BAD)
			log_append(LOG_ERROR, "PQconnectdb failed: %s", PQerrorMessage(*conn));
		res = PQexec(*conn, command);
	}

	return res;
}

static int do_row_insert(struct db_table *table, struct db_nv_list *in_values)
{
	struct stringbuffer *query, *value_fmt;
	struct stringlist *values;
	struct column_desc *desc;
	char value[64];
	PGresult *res;
	int rval;

	query = stringbuffer_create();
	value_fmt = stringbuffer_create();
	values = stringlist_create();
	stringbuffer_append_printf(query, "INSERT INTO \"%s\" (", table->name);

	for(unsigned int ii = 0; ii < in_values->count; ++ii)
	{
		struct db_named_value *dnv;

		dnv = &in_values->data[ii];
		/* Find the column or bail out. */
		desc = dict_find(table->columns, dnv->name);
		if(!desc)
		{
			log_append(LOG_ERROR, "Tried to db_row_insert('%s', %d values) with non-existent column %s.", table->name, in_values->count, dnv->name);
			rval = 1;
			goto out;
		}

		/* Append elements to query strings. */
		if(ii)
		{
			stringbuffer_append_string(query, ", ");
			stringbuffer_append_string(value_fmt, ", ");
		}

		stringbuffer_append_char(query, '"');
		stringbuffer_append_string(query, dnv->name);
		stringbuffer_append_char(query, '"');
		stringbuffer_append_printf(value_fmt, "$%u", ii+1);

		/* Figure out what type it is and append that to query value list. */
		switch(desc->type)
		{
			case DBTYPE_INTEGER:
				snprintf(value, sizeof(value), "%d", dnv->u.integer);
				stringlist_add(values, strdup(value));
				break;

			case DBTYPE_DATETIME: {
				struct tm tm;

				memset(&tm, 0, sizeof(tm));
				if(!gmtime_r(&dnv->u.datetime, &tm)) {
					log_append(LOG_ERROR, "Could not gmtime_r(%ld).", dnv->u.datetime);
					rval = 2;
					goto out;
				}

				if(!strftime(value, sizeof(value), "%Y-%m-%d %H:%M:%S-00", &tm)) {
					log_append(LOG_ERROR, "Could not strftime(%ld).", dnv->u.datetime);
					rval = 3;
					goto out;
				}

				stringlist_add(values, strdup(value));
				break;
			}

			case DBTYPE_STRING:
				stringlist_add(values, strdup(dnv->u.string ? dnv->u.string : ""));
				break;

			case DBTYPE_SERIAL:
				/* Special behavior for special column type! */
				dnv->u.serial = pgsql_get_serial(table, desc, values);
				break;

			default:
				log_append(LOG_ERROR, "Unhandled type %d for column %s in table %s.", desc->type, desc->name, table->name);
				rval = 4;
				goto out;
		}
	}

	/* Now put together the rest of the query string. */
	stringbuffer_append_printf(query, ") VALUES (%s)", value_fmt->string);
	res = do_PQexecParams(&db->conn, query->string, values->count, values->data);
	if(!res || (PQresultStatus(res) != PGRES_COMMAND_OK))
	{
		log_append(LOG_ERROR, "Failure INSERTing to %s: %s", table->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		rval = 5;
		goto out;
	}
	PQclear(res);
	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < values->count; ++ii)
		debug("   $%u = %s", ii+1, values->data[ii]);
	rval = 0;
out:
	stringlist_free(values);
	stringbuffer_free(query);
	stringbuffer_free(value_fmt);
	return rval;
}

static int do_row_update(struct db_table *table, struct db_nv_list *filter, struct db_nv_list *updates)
{
	struct stringbuffer *query;
	struct stringlist *params;
	PGresult *res;
	unsigned int rval;

	query = stringbuffer_create();
	params = stringlist_create();
	stringbuffer_append_printf(query, "UPDATE \"%s\" SET ", table->name);
	rval = db_get_values(table, query, params, updates, ", ", 1);
	if(rval)
		goto out;
	stringbuffer_append_string(query, " WHERE ");
	rval = db_get_values(table, query, params, filter, " AND ", 0);
	if(rval)
		goto out;
	res = do_PQexecParams(&db->conn, query->string, params->count, params->data);
	if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK)) {
		log_append(LOG_ERROR, "Failure UPDATEing %s: %s", table->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		rval = 5;
		goto out;
	}
	PQclear(res);
	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < params->count; ++ii)
		debug("   $%u = %s", ii+1, params->data[ii]);
	rval = 0;
out:
	stringbuffer_free(query);
	stringlist_free(params);
	return rval;
}

static int do_row_drop(struct db_table *table, struct db_nv_list *filter)
{
	struct stringbuffer *query;
	struct stringlist *params;
	PGresult *res;
	unsigned int rval;

	query = stringbuffer_create();
	params = stringlist_create();
	stringbuffer_append_printf(query, "DELETE FROM \"%s\" WHERE ", table->name);
	rval = db_get_values(table, query, params, filter, " AND ", 0);
	if(rval)
		goto out;

	res = do_PQexecParams(&db->conn, query->string, params->count, params->data);
	if(!res || (PQresultStatus(res) != PGRES_COMMAND_OK)) {
		log_append(LOG_ERROR, "Failure UPDATEing %s: %s", table->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		rval = 5;
		goto out;
	}
	PQclear(res);

	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < params->count; ++ii)
		debug("   $%u = %s", ii+1, params->data[ii]);
	rval = 0;
out:
	stringbuffer_free(query);
	stringlist_free(params);
	return rval;
}

static int do_row_get(struct db_table *table, struct db_nv_list *filter, struct db_nv_list *values)
{
	struct stringbuffer *query;
	struct stringlist *params;
	PGresult *res;
	unsigned int rval;

	query = stringbuffer_create();
	params = stringlist_create();
	stringbuffer_append_printf(query, "SELECT * FROM \"%s\" WHERE ", table->name);
	rval = db_get_values(table, query, params, filter, " AND ", 0);
	if(rval)
		goto out;
	res = do_PQexecParams(&db->conn, query->string, params->count, params->data);
	if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		log_append(LOG_ERROR, "Failure SELECTing %s: %s", table->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		rval = 5;
		goto out;
	}

	if(PQntuples(res) > 0)
		rval = db_put_values(table, res, 0, values, 1);
	else
		rval = 6;
	PQclear(res);

	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < params->count; ++ii)
		debug("   $%u = %s", ii+1, params->data[ii]);
out:
	stringbuffer_free(query);
	stringlist_free(params);
	return rval;
}

static int do_sync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, struct db_nv_list *filter, struct db_nv_list *values)
{
	struct stringbuffer *query;
	struct stringlist *params;
	PGresult *res;
	unsigned int ii, count, rval;

	query = stringbuffer_create();
	params = stringlist_create();
	stringbuffer_append_printf(query, "SELECT * FROM \"%s\" ", table->name);
	if(filter->count)
	{
		stringbuffer_append_string(query, "WHERE ");
		rval = db_get_values(table, query, params, filter, " AND ", 0);
		if(rval)
			goto out;
	}

	res = do_PQexecParams(&db->conn, query->string, params->count, params->data);
	if(!res || (PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		log_append(LOG_ERROR, "Failure SELECTing %s: %s", table->name, (res ? PQresultErrorMessage(res) : PQerrorMessage(db->conn)));
		PQclear(res);
		rval = 5;
		goto out;
	}

	for(ii = 0, count = PQntuples(res); ii < count; ++ii)
	{
		rval = db_put_values(table, res, ii, values, 0);
		if(rval)
		{
			PQclear(res);
			goto out;
		}

		if(cb(ctx, values, 0))
			break;
	}

	PQclear(res);
	debug("Query: %s", query->string);
	debug("Params:");
	for(unsigned int ii = 0; ii < params->count; ++ii)
		debug("   $%u = %s", ii+1, params->data[ii]);
	rval = 0;
out:
	if(free_ctx_func)
		free_ctx_func(ctx);
	stringbuffer_free(query);
	stringlist_free(params);
	return rval;
}

static int do_async_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, struct db_nv_list *filter, struct db_nv_list *values)
{
	struct pgsql_async *async;

	if(!(async = pgsql_get_async(table)))
	{
		db_nv_list_clear(filter);
		db_nv_list_clear(values);
		free(filter);
		free(values);
		return 1;
	}
	async->cb = cb;
	async->ctx = ctx;
	async->free_ctx_func = free_ctx_func;
	async->filter = filter;
	if(async->values)
		log_append(LOG_WARNING, "MEMLEAK: async struct still had values != 0!");
	async->values = values;
	debug("async->state = %d", async->state);
	if(async->state == PGSQL_ASYNC_IDLE)
		return pgsql_send_query(async);
	return 0;
}


/* testing */
#ifdef DB_TEST
static const struct column_desc test_cols[] = {
	{ "id", DBTYPE_SERIAL },
	{ "chartest", DBTYPE_STRING },
	{ "datetest", DBTYPE_DATETIME },
	{ NULL, DBTYPE_NUM_TYPES }
};

DB_SELECT_CB(test_cb)
{
	unsigned int ctx_num = (int)ctx;

	if(error)
	{
		log_append(LOG_WARNING, "error flag set in DB_SELECT_DB");
		return 0;
	}

	debug("%d columns in resultset [ctx arg: %d]:", values->count, ctx_num);
	debug("  * %d", values->data[0].u.serial);
	debug("  * %s (@%p)", values->data[1].u.string, values->data[1].u.string);
	debug("  * %lu", values->data[2].u.datetime);
	return 0;
}

void second_query(void *bound, void *data)
{
	debug("sync select 2");
	db_sync_select((struct db_table *)data, test_cb, (void*)0, NULL, NULL, "id", "chartest", "datetest", NULL);
	timer_add(this, "2nd_query", now + 10, second_query, test_table, 0, 1);
}

static void run_test()
{
	test_table = db_table_open("test", test_cols);
	assert(test_table);

	db_serial_t serial = 0;
	debug("insert");
	db_row_insert(test_table, "id", &serial, "chartest", "moep", "datetest", now, NULL);
	debug("serial is now %d", serial);

	debug("update");
	db_row_update(test_table, "id", serial, NULL, "chartest", "foo", NULL);

	char *str = NULL;
	debug("getrow");
	db_row_get(test_table, "id", serial, NULL, "chartest", &str);
	debug("fetched string: %s", str);
	free(str);

	debug("sync select");
	db_sync_select(test_table, test_cb, (void*)1, NULL, "id", serial, NULL, "id", "chartest", "datetest", NULL);

	debug("async select");
	db_async_select(test_table, test_cb, (void*)0, NULL, NULL, "id", "chartest", "datetest", NULL);

	timer_add(this, "2nd_query", now + 5, second_query, test_table, 0, 1);

	debug("delete");
	db_row_drop(test_table, "id", serial-1, NULL);
}
#endif
