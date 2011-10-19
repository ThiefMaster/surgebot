#ifndef TOOLS_MODULE_H
#define TOOLS_MODULE_H

char *html_decode(char *str);
int remdir(const char *path, unsigned char exists);
char *str_replace(const char *str, unsigned char case_sensitive, ...) NULL_SENTINEL;
char *strip_html_tags(char * const str);
char *strip_duplicate_whitespace(char *str);
size_t substr_count(const char *haystack, const char *needle, unsigned char case_sensitive);
char *ltrim(char * const str);
char *rtrim(char * const str);
char *urlencode(const char *s);
char *urldecode(char *uri);
char *html_encode(const char *str);
int is_utf8(const char *buf);
void make_utf8(const char *str, char *buf, size_t bufsize);
unsigned char channel_mode_changes_state(struct irc_channel *channel, const char *mode, const char *arg);
time_t strtotime(const char *str);

static inline const char *to_utf8(const char *str)
{
	static char buf[1024];
	if(!is_utf8(str))
		make_utf8(str, buf, sizeof(buf));
	else
		strlcpy(buf, str, sizeof(buf));
	return buf;
}


static inline char *trim(char * const str)
{
	return ltrim(rtrim(str));
}

#endif
