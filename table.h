#ifndef TABLE_H
#define TABLE_H

#define TABLE_CELL_ALIGN_CENTER	0x1
#define TABLE_CELL_ALIGN_RIGHT	0x2
#define TABLE_CELL_BOLD			0x4
#define TABLE_CELL_FREE			0x8
#define TABLE_CELL_COLORS_ANSI	0x10
#define TABLE_CELL_COLORS_IRC	0x20

struct table
{
	unsigned int	cols;
	unsigned int	rows;
	unsigned int	*col_flags;
	char		**header;
	char		***data;
	char		*prefix;
};

size_t table_strlen(const struct table *t, const char *str, unsigned int col);

struct table *table_create(unsigned int cols, unsigned int rows);
void table_set_header(struct table *table, char *str, ...);
void table_free(struct table *table);
void table_send(struct table *table, const char *target);
void table_send_pm(struct table *table, const char *target);
void do_table_send(struct table *table, const char *target, const char *msgtype);
void table_col_num(struct table *table, unsigned int row, unsigned int col, unsigned int val);
void table_col_str(struct table *table, unsigned int row, unsigned int col, char *val);
void table_col_fmt(struct table *table, unsigned int row, unsigned int col, const char *fmt, ...) PRINTF_LIKE(4,5);


#endif
