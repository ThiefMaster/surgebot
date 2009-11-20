#ifndef CHANSERV_ACCESS_H
#define CHANSERV_ACCESS_H

typedef void (chanserv_access_f)(const char *channel, const char *nick, int access, void *ctx);
void chanserv_get_access_callback(const char *channel, const char *nick, chanserv_access_f *, void *ctx);

#endif
