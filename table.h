#ifndef TABLE_H
#define TABLE_H

struct table
{
	unsigned int	cols;
	unsigned int	rows;
	unsigned long	bold_cols;
	const char	**header;
	const char	***data;
};

struct table *table_create(unsigned int cols, unsigned int rows);
void table_set_header(struct table *table, const char *str, ...);
void table_bold_column(struct table *table, unsigned int col, unsigned char enable);
void table_free(struct table *table);
void table_send(struct table *table, const char *target);

#endif
