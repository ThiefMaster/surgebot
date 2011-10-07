#include "global.h"
#include "module.h"
#include "stringlist.h"
#include "pgsql.h"

#include <libpq-fe.h>

MODULE_DEPENDS(NULL);

MODULE_INIT
{

}

MODULE_FINI
{

}


struct pgsql *pgsql_init(const char *conn_info)
{
	struct pgsql *conn;
	PGconn *pg_conn = PQconnectdb(conn_info);
	if(PQstatus(pg_conn) != CONNECTION_OK)
	{
		log_append(LOG_WARNING, "connection to database failed: %s", PQerrorMessage(pg_conn));
		PQfinish(pg_conn);
		return NULL;
	}

	conn = malloc(sizeof(struct pgsql));
	conn->conn = pg_conn;
	conn->conn_info = strdup(conn_info);
	return conn;
}

void pgsql_fini(struct pgsql *conn)
{
	free(conn->conn_info);
	PQfinish(conn->conn);
	free(conn);
}

void pgsql_free(PGresult *res)
{
	if(res)
		PQclear(res);
}

int pgsql_num_rows(PGresult *res)
{
	return PQntuples(res);
}

int pgsql_num_affected(PGresult *res)
{
	const char *str = PQcmdTuples(res);
	assert_return(*str, 0);
	return atoi(str);
}

const char *pgsql_value(PGresult *res, int row, int col)
{
	if(PQgetisnull(res, row, col))
		return NULL;
	return PQgetvalue(res, row, col);
}

const char *pgsql_nvalue(PGresult *res, int row, const char *col)
{
	int fnum = PQfnumber(res, col);
	if(fnum == -1)
	{
		log_append(LOG_WARNING, "field '%s' does not exist", col);
		return NULL;
	}

	if(PQgetisnull(res, row, fnum))
		return NULL;
	return PQgetvalue(res, row, fnum);
}

char *pgsql_nvalue_bytea(PGresult *res, int row, const char *col)
{
	char *value, *dup;
	size_t len;
	int fnum = PQfnumber(res, col);
	if(fnum == -1)
	{
		log_append(LOG_WARNING, "field '%s' does not exist", col);
		return NULL;
	}

	if(PQgetisnull(res, row, fnum))
		return NULL;

	value = (char *)PQunescapeBytea((unsigned char *)PQgetvalue(res, row, fnum), &len);
	dup = strndup(value, len);
	PQfreemem(value);
	return dup;
}

PGresult *pgsql_query(struct pgsql *conn, const char *query, int want_result, struct stringlist *params)
{
	return pgsql_query_bin(conn, query, want_result, params, 0);
}

PGresult *pgsql_query_bin(struct pgsql *conn, const char *query, int want_result, struct stringlist *params, uint32_t binary_flags)
{
	PGresult *res = NULL;
	int *paramLengths, *paramFormats;

	if(PQstatus(conn->conn) == CONNECTION_BAD)
	{
		debug("reconnecting to database");
		PQfinish(conn->conn);
		conn->conn = PQconnectdb(conn->conn_info);

		if(PQstatus(conn->conn) != CONNECTION_OK)
		{
			log_append(LOG_WARNING, "connection to database failed: %s", PQerrorMessage(conn->conn));
			return NULL;
		}
	}

	if(!binary_flags || !params)
		paramLengths = paramFormats = NULL;
	else
	{
		paramLengths = calloc(params->count, sizeof(int));
		paramFormats = calloc(params->count, sizeof(int));

		for(unsigned int i = 0; i < params->count; i++)
		{
			paramLengths[i] = params->data[i] ? strlen(params->data[i]) : 0;
			paramFormats[i] = (binary_flags >> i) ? 1 : 0;
		}
	}

	res = PQexecParams(conn->conn, query, params ? params->count : 0, NULL, params ? (const char*const*)params->data : NULL, paramLengths, paramFormats, 0);

	// If the connection is bad now, our query was most likely not processed -> reconnect and retry
	if(PQstatus(conn->conn) == CONNECTION_BAD)
	{
		debug("reconnecting to database");
		if(res)
			PQclear(res);
		PQfinish(conn->conn);
		conn->conn = PQconnectdb(conn->conn_info);

		if(PQstatus(conn->conn) != CONNECTION_OK)
		{
			log_append(LOG_WARNING, "connection to database failed: %s", PQerrorMessage(conn->conn));
			MyFree(paramLengths);
			MyFree(paramFormats);
			return NULL;
		}

		res = PQexecParams(conn->conn, query, params ? params->count : 0, NULL, params ? (const char*const*)params->data : NULL, NULL, NULL, 0);
	}

	MyFree(paramLengths);
	MyFree(paramFormats);

	if(!res)
	{
		log_append(LOG_WARNING, "pgsql query failed: %s", PQerrorMessage(conn->conn));
		if(params)
			stringlist_free(params);

		return NULL;
	}

	switch(PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
			break;

		default:
			log_append(LOG_WARNING, "pgsql query failed (%s): %s", PQresStatus(PQresultStatus(res)), PQresultErrorMessage(res));
			if(params)
				stringlist_free(params);
			return NULL;
	}

	if(params)
		stringlist_free(params);

	if(!want_result)
	{
		PQclear(res);
		return NULL;
	}

	return res;
}

int pgsql_query_int(struct pgsql *conn, const char *query, struct stringlist *params)
{
	int val = 0;
	const char *tmp;
	PGresult *res = pgsql_query(conn, query, 1, params);
	if(res && pgsql_num_rows(res) > 0 && (tmp = pgsql_value(res, 0, 0)))
		val = atoi(tmp);
	pgsql_free(res);
	return val;
}

int pgsql_query_bool(struct pgsql *conn, const char *query, struct stringlist *params)
{
	return !strcasecmp(pgsql_query_str(conn, query, params), "t");
}

char *pgsql_query_str(struct pgsql *conn, const char *query, struct stringlist *params)
{
	static char buf[1024];
	const char *tmp;
	memset(buf, 0, sizeof(buf));
	PGresult *res = pgsql_query(conn, query, 1, params);
	if(res && pgsql_num_rows(res) > 0 && (tmp = pgsql_value(res, 0, 0)))
		strlcpy(buf, tmp, sizeof(buf));
	pgsql_free(res);
	return buf;
}

void pgsql_begin(struct pgsql *conn)
{
	pgsql_query(conn, "BEGIN TRANSACTION", 0, NULL);
}

void pgsql_commit(struct pgsql *conn)
{
	pgsql_query(conn, "COMMIT", 0, NULL);
}

void pgsql_rollback(struct pgsql *conn)
{
	pgsql_query(conn, "ROLLBACK", 0, NULL);
}

