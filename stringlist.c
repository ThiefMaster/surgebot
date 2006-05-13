#include "global.h"
#include "stringlist.h"
#include "strnatcmp.h"

// stringlists...
// ...DO NOT store duplicates of strings
// ...free the stored strings

struct stringlist *stringlist_create()
{
	struct stringlist *list = malloc(sizeof(struct stringlist));
	memset(list, 0, sizeof(struct stringlist));
	list->count = 0;
	list->size = 2;
	list->data = calloc(list->size, sizeof(char *));
	return list;
}

void stringlist_free(struct stringlist *list)
{
	unsigned int i;
	for(i = 0; i < list->count; i++)
		free(list->data[i]);
	free(list->data);
	free(list);
}

struct stringlist *stringlist_copy(const struct stringlist *slist)
{
	unsigned int i;
	struct stringlist *new = malloc(sizeof(struct stringlist));
	new->count = slist->count;
	new->size = slist->size;
	new->data = calloc(new->size, sizeof(char *));
	for(i = 0; i < slist->count; i++) // copy entries
		new->data[i] = strdup(slist->data[i]);

	return new;
}

void stringlist_add(struct stringlist *list, char *string)
{
	if(list->count == list->size) // list is full, we need to allocate more memory
	{
		list->size <<= 1; // double size
		list->data = realloc(list->data, list->size * sizeof(char *));
	}

	list->data[list->count++] = string;
}

void stringlist_del(struct stringlist *list, int pos)
{
	assert(pos < list->count);
	free(list->data[pos]);
	list->data[pos] = list->data[--list->count]; // copy last element into empty position
}

int stringlist_cmp(const void *a, const void *b)
{
	return strnatcasecmp(*(const char **)a, *(const char **)b);
}

int stringlist_cmp_irc(const void *a, const void *b)
{
	return ircnatcasecmp(*(const char **)a, *(const char **)b);
}

void stringlist_sort(struct stringlist *list)
{
	qsort(list->data, list->count, sizeof(list->data[0]), stringlist_cmp);
}

void stringlist_sort_irc(struct stringlist *list)
{
	qsort(list->data, list->count, sizeof(list->data[0]), stringlist_cmp_irc);
}

struct stringlist *stringlist_to_irclines(const char *target, struct stringlist *list)
{
	unsigned int max_len;
	if(target) // if we have a target nick, we assume it's IRC
		max_len = MAXLEN - strlen(bot.nickname) - strlen(bot.username) - strlen(bot.hostname) - strlen(target) - 20;
	else // use MAXLEN if it does not look like IRC
		max_len = MAXLEN;

	struct stringlist *lines = stringlist_create();

	char buf[MAXLEN];
	unsigned int i, total_len = 0, len = 0;
	for(i = 0; i < list->count; i++)
	{
		char *str = list->data[i];
		len = strlen(str);
		if(total_len + len + 4 > max_len)
		{
			buf[total_len] = '\0';
			stringlist_add(lines, strdup(buf));
			total_len = 0;
		}
		memcpy(buf + total_len, str, len);
		total_len += len;
		buf[total_len++] = ' ';
	}

	if(total_len) // still something in the temp. buffer
	{
		buf[total_len] = '\0';
		stringlist_add(lines, strdup(buf));
	}

	return lines;
}
