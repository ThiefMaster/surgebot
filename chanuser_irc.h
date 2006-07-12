#ifndef HAVE_CHANUSER_IRC_H
#define HAVE_CHANUSER_IRC_H

void chanuser_irc_init();
void chanuser_irc_fini();
int chanuser_irc_handler(int argc, char **argv, struct irc_source *src, const char *raw_line);

#endif
