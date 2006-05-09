#ifndef TOOLS_H
#define TOOLS_H

void split_mask(char *mask, char **nick, char **ident, char **host);
unsigned int aredigits(const char *text);
char *time2string(time_t time);
char *chanmodes2string(long modes, unsigned int limit, const char *key);
int IsChannelName(const char *name);
unsigned int validate_string(const char *str, const char *allowed, char *c);
int match(const char *mask, const char *name);

#define true_string(STR)	((STR) && (!strcasecmp((STR), "on") || !strcasecmp((STR), "true") || !strcmp((STR), "1") || !strcasecmp((STR), "yes")))
#define false_string(STR)	(!(STR) || !strcasecmp((STR), "off") || !strcasecmp((STR), "false") || !strcmp((STR), "0") || !strcasecmp((STR), "no"))

#endif
