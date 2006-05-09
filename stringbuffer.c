#include "global.h"
#include "stringbuffer.h"

struct stringbuffer *stringbuffer_create()
{
	struct stringbuffer *sbuf = malloc(sizeof(struct stringbuffer));
	sbuf->len = 0;
	sbuf->size = 8; // size available for chars - \0 is not included
	sbuf->string = malloc(sbuf->size + 1); // size + \0
	sbuf->string[0] = '\0';
	return sbuf;
}

void stringbuffer_free(struct stringbuffer *sbuf)
{
	free(sbuf->string);
	free(sbuf);
}

void stringbuffer_append_char(struct stringbuffer *sbuf, char c)
{
	if(sbuf->len == sbuf->size) // sbuf is full, we need to allocate more memory
	{
		sbuf->size <<= 1; // double size
		sbuf->string = realloc(sbuf->string, sbuf->size + 1);
	}

	sbuf->string[sbuf->len++] = c;
	sbuf->string[sbuf->len + 1] = '\0';
}

void stringbuffer_append_string(struct stringbuffer *sbuf, const char *str)
{
	unsigned int len = strlen(str);
	if(len == 0)
		return;

	while(sbuf->len + len + 1 >= sbuf->size) // sbuf will be full or is full, we need to allocate more memory
	{
		sbuf->size <<= 1; // double size
		sbuf->string = realloc(sbuf->string, sbuf->size + 1);
	}

	memcpy(sbuf->string + sbuf->len, str, len + 1); // len+1 will get the \0, too
	sbuf->len += len;
}

void stringbuffer_flush(struct stringbuffer *sbuf)
{
	free(sbuf->string);
	sbuf->len = 0;
	sbuf->size = 8;
	sbuf->string = malloc(sbuf->size + 1);
	sbuf->string[0] = '\0';
}
