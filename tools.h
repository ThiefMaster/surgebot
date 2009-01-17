#ifndef TOOLS_H
#define TOOLS_H

#include "x_ctype.h"

void tools_init();
void tools_fini();
void split_mask(char *mask, char **nick, char **ident, char **host);
unsigned char aredigits(const char *text);
char *duration2string(time_t time);
char *time2string(time_t time);
const char *chanmodes2string(int modes, unsigned int limit, const char *key);
int IsChannelName(const char *name);
unsigned int validate_string(const char *str, const char *allowed, char *c);
int match(const char *mask, const char *name);
const char *strtab(unsigned int num);
size_t strlcpy(char *out, const char *in, size_t len);
size_t strlcat(char *out, const char *in, size_t len);
unsigned int is_valid_string(const char *str);
char *strtolower(char *str);
char *strtoupper(char *str);
unsigned char check_date(int day, int month, int year);
char *strip_codes(char *str);
unsigned char file_exists(const char *file);

static inline long min(long a, long b)
{
	return (a < b) ? a : b;
}

static inline long max(long a, long b)
{
	return (a > b) ? a : b;
}

#define true_string(STR)	((STR) && (!strcasecmp((STR), "on") || !strcasecmp((STR), "true") || !strcmp((STR), "1") || !strcasecmp((STR), "yes")))
#define false_string(STR)	(!(STR) || !strcasecmp((STR), "off") || !strcasecmp((STR), "false") || !strcmp((STR), "0") || !strcasecmp((STR), "no"))

#endif
