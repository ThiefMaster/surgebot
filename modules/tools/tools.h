#ifndef __TOOLS_H__
#define __TOOLS_H__

char *html_decode(char *str);
char *str_replace(const char *str, const char *search, const char *replace, unsigned char case_sensitive);
char *strip_html_tags(char *str);
int remdir(const char *path, unsigned char exists);
char *urlencode(const char *s);

#endif // __TOOLS_H__
