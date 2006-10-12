#include "global.h"
#include "table.h"
#include "stringbuffer.h"
#include "irc.h"

struct table *table_create(unsigned int cols, unsigned int rows)
{
	struct table *table = malloc(sizeof(struct table));

	table->cols = cols;
	table->rows = rows;
	table->header = NULL;

	table->data = calloc(table->rows , sizeof(char **));
	for(int i = 0; i < table->rows; i++)
		table->data[i] = calloc(table->cols, sizeof(char *));

	return table;
}

void table_set_header(struct table *table, const char *str, ...)
{
	va_list args;

	assert(str);
	table->header = calloc(table->cols, sizeof(char *));

	va_start(args, str);
	table->header[0] = str;
	for(int i = 1; i < table->cols; i++)
		table->header[i] = va_arg(args, const char *);
	va_end(args);
}

void table_free(struct table *table)
{
	for(int i = 0; i < table->rows; i++)
		free(table->data[i]);

	if(table->header)
		free(table->header);
	free(table->data);
	free(table);
}

void table_send(struct table *table, const char *target)
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
			len = strlen(table->data[row][col]);
			if(len > maxlens[col])
				maxlens[col] = len;
		}
	}

	line = stringbuffer_create();
	for(int row = (table->header ? -1 : 0); row < (int)table->rows; row++)
	{
		const char **ptr = ((row == -1) ? table->header : table->data[row]);

		for(unsigned int col = 0; col < table->cols; col++)
		{
			if(row == -1)
			{
				stringbuffer_append_string(line, "$u");
				stringbuffer_append_string(line, ptr[col]);
				stringbuffer_append_string(line, "$u");
				spaces = maxlens[col] - strlen(ptr[col]);
			}
			else
			{
				stringbuffer_append_string(line, ptr[col]);
				spaces = maxlens[col] - strlen(ptr[col]);
			}

			if(col < table->cols - 1)
			{
				while(spaces--)
					stringbuffer_append_char(line, ' ');

				stringbuffer_append_string(line, "  ");
			}
		}

		irc_send_msg(target, "NOTICE", "%s", line->string);
		line->len = 0;
	}

	stringbuffer_free(line);
	free(maxlens);
}
