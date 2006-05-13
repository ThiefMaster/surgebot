#ifndef SURGEBOT_H
#define SURGEBOT_H

#include "list.h"

typedef void (loop_func)();

void reg_loop_func(loop_func *func);
void unreg_loop_func(loop_func *func);

DECLARE_LIST(loop_func_list, loop_func *)

#endif

