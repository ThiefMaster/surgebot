#ifndef IRC_HANDLER_H
#define IRC_HANDLER_H

#include "list.h"

#define IRC_HANDLER(CMD)	static void __irc_handler_ ## CMD(int argc, char **argv, struct irc_source *src)

typedef void (irc_handler_f)(int argc, char **argv, struct irc_source *src);

void irc_handler_init();
void irc_handler_fini();

void _reg_irc_handler(const char *cmd, irc_handler_f *func);
void _unreg_irc_handler(const char *cmd, irc_handler_f *func);
#define reg_irc_handler(CMD)	_reg_irc_handler(#CMD, __irc_handler_ ## CMD)
#define unreg_irc_handler(CMD)	_unreg_irc_handler(#CMD, __irc_handler_ ## CMD)

void irc_handle_msg(int argc, char **argv, struct irc_source *src);

DECLARE_LIST(irc_handler_list, irc_handler_f *)

#endif
