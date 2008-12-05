#ifndef TOOLS_MODULE_H
#define TOOLS_MODULE_H

char *html_decode(char *str);
int remdir(const char *path, unsigned char exists);
char *str_replace(const char *str, const char *search, const char *replace, unsigned char case_sensitive);
char *strip_html_tags(char * const str);
char *strip_duplicate_whitespace(char *str);
size_t substr_count(const char *haystack, const char *needle, unsigned char case_sensitive);
char *ltrim(char * const str);
char *rtrim(char * const str);
inline char *trim(char * const str);
char *urlencode(const char *s);
char *urldecode(char *uri);
char *html_encode(const char *str);

#endif
