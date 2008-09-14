#ifndef __TOOLS_H__
#define __TOOLS_H__

const struct
{
	char *entity;
	char character;
}
entities[] =
{
	// "Default" entities
	{ "auml",	'ä' },
	{ "ouml",	'ö' },
	{ "uuml",	'ü' },
	{ "szlig",	'ß' },
	{ "quot",	'"' },
	{ "amp",	'&' },
	{ "lt",		'<' },
	{ "gt",		'>' }
};

unsigned char hexchars[] = "0123456789ABCDEF";

char *html_decode(char *str);
int remdir(const char *path, unsigned char exists);
char *str_replace(const char *str, const char *search, const char *replace, unsigned char case_sensitive);
char *strip_html_tags(char * const str);
char *strip_duplicate_whitespace(char *str);
size_t substr_count(const char *haystack, const char *needle, unsigned char case_sensitive);
char *trim(char * const str);
char *urlencode(const char *s);
char *urldecode(char *uri);
char *html_encode(const char *str);

#endif // __TOOLS_H__
