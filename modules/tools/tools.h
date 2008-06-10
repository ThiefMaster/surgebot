#ifndef __TOOLS_H__
#define __TOOLS_H__

unsigned char hexchars[] = "0123456789ABCDEF";

char *html_decode(char *str);
char *str_replace(const char *str, const char *search, const char *replace, unsigned char case_sensitive);
char *strip_html_tags(char * const str);
int remdir(const char *path, unsigned char exists);
char *trim(char * const str);
char *urlencode(const char *s);

#endif // __TOOLS_H__
