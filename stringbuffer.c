#include "global.h"
#include "stringbuffer.h"
#include "tools.h"

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
	if(sbuf->len >= sbuf->size - 1) // sbuf is full, we need to allocate more memory
	{
		sbuf->size <<= 1; // double size
		sbuf->string = realloc(sbuf->string, sbuf->size + 1);
	}

	sbuf->string[sbuf->len++] = c;
	sbuf->string[sbuf->len] = '\0';
}

void stringbuffer_append_string_n(struct stringbuffer *sbuf, const char *str, size_t len)
{
	if(!len)
		return;

	while(sbuf->len + len + 1 > sbuf->size)
	{
		sbuf->size <<= 1;
		sbuf->string = realloc(sbuf->string, sbuf->size + 1);
	}

	memcpy(sbuf->string + sbuf->len, str, len);
	sbuf->len += len;
	sbuf->string[sbuf->len] = 0;
}

void stringbuffer_append_string(struct stringbuffer *sbuf, const char *str)
{
	stringbuffer_append_string_n(sbuf, str, strlen(str));
}

char *stringbuffer_shift(struct stringbuffer *sbuf, const char *delim, unsigned char require_token)
{
	char *tmp;
	int delim_len = strlen(delim);

	// Delimiter substring?
	if((tmp = strcasestr(sbuf->string, delim)))
	{
		int len = tmp - sbuf->string;
		// Duplicate substring
		tmp = strndup(sbuf->string, len);
		// Flush substring and delimiter
		free(stringbuffer_flush_return(sbuf, len + delim_len));
		return tmp;
	}
	else
	{
		// No delimiter was found, could the end of the line be the beginning of the delimiter?
		int i;
		for(i = 0, tmp = sbuf->string + sbuf->len - 1; i < delim_len; i++, tmp--)
			if(!strncasecmp(tmp, delim, i))
				return NULL;

		// return whole string (even if empty) in case the token is not required
		if(!require_token)
		{
			tmp = strdup(sbuf->string);
			stringbuffer_flush(sbuf);
			return tmp;
		}

		// No token but since it is required, we got nothing to return
		return NULL;
	}
}

char *stringbuffer_shiftspn(struct stringbuffer *sbuf, const char *delim_list, unsigned char require_token)
{
	int i = strcspn(sbuf->string, delim_list);
	char *tmp;

	// Not found
	if(i == sbuf->len)
	{
		if(require_token)
			return NULL;

		tmp = strdup(sbuf->string);
		stringbuffer_flush(sbuf);
		return tmp;
	}

	// Token was found
	tmp = strndup(sbuf->string, i);
	free(stringbuffer_flush_return(sbuf, i + strspn(sbuf->string + i, delim_list)));
	return tmp;

	// todo: What to do if line ends with delimiters from the list? (The next input could start with delimiters too)
}

void stringbuffer_flush(struct stringbuffer *sbuf)
{
	free(sbuf->string);
	sbuf->len = 0;
	sbuf->size = 8;
	sbuf->string = malloc(sbuf->size + 1);
	sbuf->string[0] = '\0';
}

char *stringbuffer_flush_return(struct stringbuffer *sbuf, size_t len)
{
	char *buf;

	len = min(len, sbuf->len);
	buf = malloc(len + 1);

	strncpy(buf, sbuf->string, len);
	buf[len] = 0;
	sbuf->len -= len;
	memmove(sbuf->string, sbuf->string + len, sbuf->len + 1);

	return buf;
}
