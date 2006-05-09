#ifndef IRC_H
#define IRC_H

void irc_init();
void irc_fini();

int irc_connect();
int irc_send(const char *format, ...) PRINTF_LIKE(1, 2);

#endif
