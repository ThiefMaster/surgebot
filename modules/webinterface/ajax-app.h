#ifndef AJAXAPP_H
#define AJAXAPP_H

void ajaxapp_init();
void ajaxapp_fini();

void menu_add(const char *id, const char *title, unsigned int guest);
void menu_del(const char *id);

#endif

