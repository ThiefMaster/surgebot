#ifndef STRINGBUFFER_H
#define STRINGBUFFER_H

struct stringbuffer
{
	unsigned int len;
	unsigned int size;

	char *string;
};

struct stringbuffer *stringbuffer_create();
void stringbuffer_free(struct stringbuffer *sbuf);

void stringbuffer_append_char(struct stringbuffer *sbuf, char c);
void stringbuffer_append_string(struct stringbuffer *sbuf, const char *str);
void stringbuffer_flush(struct stringbuffer *sbuf);

#endif
