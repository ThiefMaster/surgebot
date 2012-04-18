#ifndef IRC_HANDLER_H
#define IRC_HANDLER_H

#include "list.h"

#define IRC_HANDLER(NAME)	static void __irc_handler_ ## NAME(int argc, char **argv, struct irc_source *src, void *extra)

typedef void (irc_handler_f)(int argc, char **argv, struct irc_source *src, void *extra);
typedef int (irc_handler_extra_cmp_f)(void *a, void *b);
typedef void (connected_f)();

struct irc_handler {
	irc_handler_f *func;
	void *extra;
	irc_handler_extra_cmp_f* extra_cmp_func;
};

void irc_handler_init();
void irc_handler_fini();

void _reg_irc_handler(const char *cmd, irc_handler_f *func, void *extra, irc_handler_extra_cmp_f* extra_cmp_func);
void _unreg_irc_handler(const char *cmd, irc_handler_f *func, void *extra);
#define reg_irc_handler(CMD, NAME)	_reg_irc_handler(CMD, __irc_handler_ ## NAME, NULL, NULL)
#define unreg_irc_handler(CMD, NAME)	_unreg_irc_handler(CMD, __irc_handler_ ## NAME, NULL)
void reg_connected_func(connected_f *func);
void unreg_connected_func(connected_f *func);

void irc_handle_msg(int argc, char **argv, struct irc_source *src, const char *raw_line);

DECLARE_LIST(irc_handler_list, struct irc_handler *)
DECLARE_LIST(connected_func_list, connected_f *)

#endif
