#ifndef IRC_H
#define IRC_H

void irc_init();
void irc_fini();

int irc_connect();
void irc_parse_line(const char *line);
void irc_send(const char *format, ...) PRINTF_LIKE(1, 2);
void irc_send_fast(const char *format, ...) PRINTF_LIKE(1, 2);

#endif
