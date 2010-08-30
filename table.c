#include "global.h"
#include "table.h"
#include "stringbuffer.h"
#include "irc.h"

size_t table_strlen(const struct table *t, const char *str, unsigned int col)
{
	size_t len = 0;

	if(t->col_flags[col] & TABLE_CELL_COLORS_ANSI)
	{
		if(!strchr(str, '\033'))
			return strlen(str);

		for(const char *c = str; *c; c++)
		{
			if(*c == '\033')
			{
				while(*c && *c != 'm')
					c++;
				continue;
			}

			len++;
		}
	}
	else if(t->col_flags[col] & TABLE_CELL_COLORS_IRC)
	{
		const char *pos = str;
		int digits;
		for(const char *pos = str; *pos != '\0'; pos++)
		{
			if(*pos == '$')
			{
				switch(*pos)
				{
					case 'b':
					case 'u':
					case 'r':
						pos++;
						continue;
					case 'c':
						// eat up up to two digits
						digits = 0;
						while(*(++pos) != '\0' && isdigit(*pos) && ++digits < 2)
							;

						// background color?
						if(*pos == ',')
						{
							digits = 0;
							// again, two digits are possible
							while(*(++pos) != '\0' && isdigit(*pos) && ++digits < 2)
								;
						}
						continue;

					default:
						break;
				}
			}
			++len;
		}
	}
	else
	{
		len = strlen(str);
	}

	return len;
}

struct table *table_create(unsigned int cols, unsigned int rows)
{
	struct table *table = malloc(sizeof(struct table));

	table->cols = cols;
	table->rows = rows;
	table->header = NULL;
	table->prefix = NULL;
	table->col_flags = malloc(table->cols * sizeof(table->col_flags[0]));
	// irc colors are always parsed during output, strip them by default
	for(unsigned int i = 0; i < table->cols; ++i)
		table->col_flags[i] = TABLE_CELL_COLORS_IRC;

	table->data = calloc(table->rows , sizeof(char **));
	for(unsigned int i = 0; i < table->rows; ++i)
		table->data[i] = calloc(table->cols, sizeof(char *));

	return table;
}

void table_set_header(struct table *table, char *str, ...)
{
	va_list args;

	assert(str);
	table->header = calloc(table->cols, sizeof(char *));

	va_start(args, str);
	table->header[0] = str;
	for(unsigned int i = 1; i < table->cols; ++i)
		table->header[i] = va_arg(args, char *);
	va_end(args);
}

void table_free(struct table *table)
{
	for(unsigned int i = 0; i < table->rows; ++i)
	{
		for(unsigned int j = 0; j < table->cols; ++j)
		{
			if(table->col_flags[j] & TABLE_CELL_FREE)
				free(table->data[i][j]);
		}

		free(table->data[i]);
	}

	if(table->header)
		free(table->header);
	if(table->prefix)
		free(table->prefix);
	free(table->col_flags);
	free(table->data);
	free(table);
}

void table_send(struct table *table, const char *target)
{
	do_table_send(table, target, "NOTICE");
}

void table_send_pm(struct table *table, const char *target)
{
	do_table_send(table, target, "PRIVMSG");
}

void do_table_send(struct table *table, const char *target, const char *msgtype)
{
	unsigned int len, spaces, *maxlens;
	struct stringbuffer *line;

	maxlens = calloc(table->cols, sizeof(unsigned int));

	if(table->header)
	{
		for(unsigned int col = 0; col < table->cols; col++)
			maxlens[col] = strlen(table->header[col]);
	}

	for(unsigned int col = 0; col < table->cols; col++)
	{
		for(unsigned int row = 0; row < table->rows; row++)
		{
			len = table_strlen(table, table->data[row][col], col);
			if(len > maxlens[col])
				maxlens[col] = len;
		}
	}

	line = stringbuffer_create();
	for(int row = (table->header ? -1 : 0); row < (int)table->rows; row++)
	{
		char **ptr = ((row == -1) ? table->header : table->data[row]);

		for(unsigned int col = 0; col < table->cols; col++)
		{
			if(row == -1)
			{
				stringbuffer_append_printf(line, "$u%s$u", ptr[col]);
				spaces = maxlens[col] - table_strlen(table, ptr[col], col);
			}
			else
			{
				if(table->prefix != NULL)
					stringbuffer_append_string(line, table->prefix);

				if(table->col_flags[col] & TABLE_CELL_BOLD)
					stringbuffer_append_string(line, "$b");

				len = table_strlen(table, ptr[col], col);
				spaces = maxlens[col] - len;
				if(table->col_flags[col] & TABLE_CELL_ALIGN_CENTER)
				{
					if(spaces > 0)
					{
						int indent = spaces / 2; // round down
						stringbuffer_append_printf(line, "%*s", indent, " ");
						spaces -= indent;
					}
				}
				else if(table->col_flags[col] & TABLE_CELL_ALIGN_RIGHT)
				{
					if(spaces > 0)
					{
						stringbuffer_append_printf(line, "%*s", spaces, " ");
						spaces = 0;
					}
				}
				stringbuffer_append_string(line, ptr[col]);
			}

			if(col < table->cols - 1)
			{
				// Add spaces
				if(spaces > 0)
					stringbuffer_append_printf(line, "%*s", spaces, " ");
				if(row > -1 && table->col_flags[col] & TABLE_CELL_BOLD)
					stringbuffer_append_string(line, "$b");

				stringbuffer_append_string(line, "  ");
			}
			else if(row > -1 && table->col_flags[col] & TABLE_CELL_BOLD)
			{
				stringbuffer_append_string(line, "$b");
			}
		}

		irc_send_msg(target, msgtype, "%s", line->string);
		line->len = 0;
	}

	stringbuffer_free(line);
	free(maxlens);
}

void table_col_num(struct table *table, unsigned int row, unsigned int col, unsigned int val)
{
	assert(col < table->cols);
	assert(row < table->rows);
	assert(table->col_flags[col] & TABLE_CELL_FREE);

	asprintf(&table->data[row][col], "%u", val);
}

void table_col_str(struct table *table, unsigned int row, unsigned int col, char *val)
{
	assert(col < table->cols);
	assert(row < table->rows);

	table->data[row][col] = val;
}

void table_col_fmt(struct table *table, unsigned int row, unsigned int col, const char *fmt, ...)
{
	va_list args;

	assert(col < table->cols);
	assert(row < table->rows);
	assert(table->col_flags[col] & TABLE_CELL_FREE);

	va_start(args, fmt);
	vasprintf(&table->data[row][col], fmt, args);
	va_end(args);
}
