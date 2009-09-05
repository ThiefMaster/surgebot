#ifndef CHANDICT_H
#define CHANDICT_H

struct dict *chandict_get_entries(const char *channel);
void chandict_add_entry(const char *channel, const char *entry, const char *data);
void chandict_del_entry(const char *channel, const char *entry);

struct dict *chandict_get_aliases(const char *channel);
int chandict_add_alias(const char *channel, const char *alias, const char *entry);
void chandict_del_alias(const char *channel, const char *alias);

#endif
