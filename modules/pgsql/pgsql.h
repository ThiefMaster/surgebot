#ifndef PGSQL_H
#define PGSQL_H

#include <libpq-fe.h>

struct stringlist;

struct pgsql
{
	PGconn *conn;
	char *conn_info;
};

struct pgsql *pgsql_init(const char *conn_info);
void pgsql_fini(struct pgsql *conn);

void pgsql_free(PGresult *res);
int pgsql_num_rows(PGresult *res);
int pgsql_num_affected(PGresult *res);
const char *pgsql_value(PGresult *res, int row, int col);
const char *pgsql_nvalue(PGresult *res, int row, const char *col);
char *pgsql_nvalue_bytea(PGresult *res, int row, const char *col);
PGresult *pgsql_query(struct pgsql *conn, const char *query, int want_result, struct stringlist *params);
PGresult *pgsql_query_bin(struct pgsql *conn, const char *query, int want_result, struct stringlist *params, uint32_t binary_flags);
int pgsql_query_int(struct pgsql *conn, const char *query, struct stringlist *params);
int pgsql_query_bool(struct pgsql *conn, const char *query, struct stringlist *params);
char *pgsql_query_str(struct pgsql *conn, const char *query, struct stringlist *params);
void pgsql_begin(struct pgsql *conn);
void pgsql_commit(struct pgsql *conn);
void pgsql_rollback(struct pgsql *conn);

#endif
