#ifndef DATABASE_H
#define DATABASE_H

#include "ptrlist.h"
#include "stringlist.h"

struct database;

typedef void (db_read_f)(struct database *);
typedef int (db_write_f)(struct database *);

enum db_source
{
	SRC_FILE,
	SRC_MMAP
};

struct database
{
	char *name;
	char *filename;
	char *tmp_filename;

	db_read_f *read_func;
	db_write_f *write_func;

	time_t last_write;
	time_t write_interval;

	unsigned int indent;
	unsigned int line;
	unsigned int line_pos;
	unsigned int map_pos;

	enum db_source source;
	size_t length;
	FILE *fp;
	char *map; // for mmap()

	jmp_buf jbuf;
	struct ptrlist *free_on_error;

	struct dict *nodes;
};

enum db_type
{
	DB_EMPTY,
	DB_OBJECT,
	DB_STRING,
	DB_STRINGLIST
};

struct db_node
{
	enum db_type type;
	union
	{
		void *ptr; // since data is an union, ptr always points to the appropiate data storage
		char *string;
		struct dict *object;
		struct stringlist *slist;
	} data;
};

enum error_codes
{
	UNTERMINATED_STRING = 1,
	EXPECTED_OPEN_QUOTE,
	EXPECTED_OPEN_BRACE,
	EXPECTED_OPEN_PAREN,
	EXPECTED_COMMA,
	EXPECTED_START_DATA,
	EXPECTED_SEMICOLON,
	EXPECTED_RECORD_DATA,
	EXPECTED_COMMENT_END
};

enum ptr_types // for free_on_error ptrlist
{
	PTR_STRING,
	PTR_DICT,
	PTR_DB_ENTRY
};

void database_init();
void database_fini();

struct dict *database_load(const char *filename); // load database from file

struct database *database_create(const char *name, db_read_f *read_func, db_write_f *write_func); // register database
void database_delete(struct database *db); // unregister and free database

void database_set_write_interval(struct database *db, time_t interval);

struct db_node *database_fetch_path(struct dict *db_nodes, const char *node_path);
void *database_fetch(struct dict *db_nodes, const char *path, enum db_type type);

int database_read(struct database *db, unsigned int free_nodes_after_read);
int database_write(struct database *db);

void database_dump(struct dict *db_nodes);

void database_begin_object(struct database *db, const char *key);
void database_end_object(struct database *db);
void database_write_long(struct database *db, const char *key, long value);
void database_write_string(struct database *db, const char *key, const char *value);
void database_write_stringlist(struct database *db, const char *key, struct stringlist *slist);

#endif
