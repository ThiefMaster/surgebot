#ifndef STRINGLIST_H
#define STRINGLIST_H

struct stringlist
{
	unsigned int count;
	unsigned int size;

	char **data;
};

struct stringlist *stringlist_create();
void stringlist_free(struct stringlist *list);
struct stringlist *stringlist_copy(const struct stringlist *slist);

void stringlist_add(struct stringlist *list, char *string);
void stringlist_del(struct stringlist *list, int pos);
void stringlist_sort(struct stringlist *list);
void stringlist_sort_irc(struct stringlist *list);
struct stringlist *stringlist_to_irclines(const char *target, struct stringlist *list);

#endif
