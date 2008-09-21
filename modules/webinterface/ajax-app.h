#ifndef AJAXAPP_H
#define AJAXAPP_H

void ajaxapp_init();
void ajaxapp_fini();

unsigned int menu_add(const char *id, const char *title, const char *rule);
void menu_del(const char *id);

#endif

