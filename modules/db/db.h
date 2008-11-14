#ifndef DB_H
#define DB_H

#include "simplelist.h"

typedef int db_serial_t;
typedef void (db_free_ctx_f)(void *);

enum db_type {
	DBTYPE_INTEGER,  /* int */
	DBTYPE_DATETIME, /* time_t */
	DBTYPE_STRING,   /* char* */
	DBTYPE_SERIAL,   /* int, read-only by db users, 0 means invalid or unknown */
	DBTYPE_NUM_TYPES
};

struct column_desc {
	char *name;
	enum db_type type;
};

struct db_named_value {
	const char *name;
	union {
		int integer;
		time_t datetime;
		char *string;
		db_serial_t serial;
	} u;
};

DECLARE_SLIST(db_nv_list, struct db_named_value)

struct db_table {
	const char *name;
	//struct database *parent;
	struct dict *columns;
};

#define DB_SELECT_CB(NAME) static int NAME(UNUSED_ARG(void *ctx), struct db_nv_list *values, unsigned int rownum, unsigned int rowcount, unsigned int error)
typedef int (db_select_cb)(void *ctx, struct db_nv_list *values, unsigned int rownum, unsigned int rowcount, unsigned int error);
#define DB_EMPTY_RESULT() (rowcount == 0)

const char *db_type_to_name(enum db_type type);
enum db_type db_type_from_name(const char *name);

struct db_table *db_table_open(const char *name, const struct column_desc *cols);
void db_table_close(struct db_table *table);

int db_row_insert(struct db_table *table, ...);
int db_row_update(struct db_table *table, ...);
int db_row_drop(struct db_table *table, ...);
int db_row_get(struct db_table *table, ...);
int db_vsync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, va_list *args);
int db_sync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, ...);
int db_vasync_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, va_list *args);
int db_async_select(struct db_table *table, db_select_cb cb, void *ctx, db_free_ctx_f *free_ctx_func, ...);

#endif
