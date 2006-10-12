#ifndef TABLE_H
#define TABLE_H

struct table
{
	unsigned int	cols;
	unsigned int	rows;
	const char	**header;
	const char	***data;
};

struct table *table_create(unsigned int cols, unsigned int rows);
void table_set_header(struct table *table, const char *str, ...);
void table_free(struct table *table);
void table_send(struct table *table, const char *target);

#endif
