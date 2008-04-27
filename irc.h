#ifndef IRC_H
#define IRC_H

#include "list.h"

typedef void (disconnected_f)();

void irc_init();
void irc_fini();

int irc_connect();
void irc_watchdog_reset();
void irc_parse_line(const char *line);
void irc_send(const char *format, ...) PRINTF_LIKE(1, 2);
void irc_send_raw(const char *format, ...) PRINTF_LIKE(1, 2);
void irc_send_fast(const char *format, ...) PRINTF_LIKE(1, 2);
void irc_send_msg(const char *target, const char *cmd, const char *format, ...) PRINTF_LIKE(3, 4);
char *irc_format_line(const char *msg);

void reg_disconnected_func(disconnected_f *func);
void unreg_disconnected_func(disconnected_f *func);

#define reply(FMT, ...)		irc_send_msg(src->nick, "NOTICE", FMT, ##__VA_ARGS__)

DECLARE_LIST(disconnected_func_list, disconnected_f *)

#endif
