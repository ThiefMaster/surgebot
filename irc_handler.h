#ifndef IRC_HANDLER_H
#define IRC_HANDLER_H

#include "list.h"

#define IRC_HANDLER(NAME)	static void __irc_handler_ ## NAME(int argc, char **argv, struct irc_source *src)

typedef void (irc_handler_f)(int argc, char **argv, struct irc_source *src);
typedef void (connected_f)();

void irc_handler_init();
void irc_handler_fini();

void _reg_irc_handler(const char *cmd, irc_handler_f *func);
void _unreg_irc_handler(const char *cmd, irc_handler_f *func);
#define reg_irc_handler(CMD, NAME)	_reg_irc_handler(CMD, __irc_handler_ ## NAME)
#define unreg_irc_handler(CMD, NAME)	_unreg_irc_handler(CMD, __irc_handler_ ## NAME)
void reg_connected_func(connected_f *func);
void unreg_connected_func(connected_f *func);

void irc_handle_msg(int argc, char **argv, struct irc_source *src, const char *raw_line);

DECLARE_LIST(irc_handler_list, irc_handler_f *)
DECLARE_LIST(connected_func_list, connected_f *)

#endif
