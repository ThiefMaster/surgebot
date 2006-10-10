#ifndef PERFORM_H
#define PERFORM_H

typedef void (perform_f)();

void perform_func_reg(char *name, perform_f *func);
void perform_func_unreg(const char *name);

#endif

