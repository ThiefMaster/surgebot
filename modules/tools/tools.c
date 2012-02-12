#include "global.h"
#include "module.h"
#include "stringbuffer.h"
#include "chanuser.h"

// Module header
#include "tools.h"

#include <iconv.h>

const struct
{
	char *entity;
	unsigned char character;
}
entities[] =
{
	// "Default" entities
	{ "auml",	U'ä' },
	{ "ouml",	U'ö' },
	{ "uuml",	U'ü' },
	{ "szlig",	U'ß' },
	{ "quot",	'"' },
	{ "amp",	'&' },
	{ "lt",		'<' },
	{ "gt",		'>' },
	{ "apos",	'\'' }
};

const unsigned char hexchars[] = "0123456789ABCDEF";
const char whitespace_chars[] = " \t\n\v\f\r";

MODULE_DEPENDS(NULL);

MODULE_INIT {}
MODULE_FINI {}

char *html_decode(char *str)
{
	unsigned int i;
	char *end, *pos = str, entity[11];
	size_t len = strlen(str);

	while((pos = strchr(pos, '&')))
	{
		// Entity ends in ; ...
		if((end = strchr(pos, ';')) == NULL)
		{
			pos++;
			continue;
		}

		// ... and has at most 10 characters (we only need some limit)
		if((end - pos) > 10)
		{
			debug("> 10");
			pos = end + 1;
			continue;
		}

		if(pos[1] == '#') // Numeric entity
		{
			strlcpy(entity, pos + 2, (end - pos) - 1);
			if((strspn(entity, "0123456789") < strlen(entity)) || !(i = atoi(entity)) || i < 32 || i > 255)
			{
				pos = end + 1;
				continue;
			}

			pos[0] = (char)i;
		}
		else // Non-numeric entity
		{
			for(i = 0; i < ArraySize(entities); i++)
			{
				if(strncasecmp(pos + 1, entities[i].entity, (end - pos) - 1) == 0)
				{
					pos[0] = entities[i].character;
					goto loop_continue;
				}
			}
			pos++;
			continue;
		}

loop_continue:

		pos++, end++;
		memmove(pos, end, (len - (end - str) + 1));
	}

	return str;
}
#undef MAX_ENTITY_LENGTH

int remdir(const char *path, unsigned char exists)
{
	DIR *dir;
	struct dirent *direntry;
	char new_path[PATH_MAX];
	struct stat attribut;

	if(!(dir = opendir(path)))
	{
		debug("Failed to open directory %s", path);
		return exists;
	}

	strncpy(new_path, path, sizeof(new_path));
	int len = strlen(new_path);
	if(new_path[len - 1] == '/') new_path[--len] = '\0';

	while((direntry = readdir(dir)))
	{
		if(!strcmp(direntry->d_name, ".") || !strcmp(direntry->d_name, ".."))
			continue;

		snprintf(new_path + len, sizeof(new_path) - len, "/%s", direntry->d_name);
		stat(new_path, &attribut);
		if(attribut.st_mode & S_IFDIR)
		{
			if(!remdir((const char*)new_path, 1))
			{
				debug("Failed removing directory: %s", new_path);
				return 2;
			}
		}

		else if(unlink(new_path))
		{
			debug("Failed to unlink %s", new_path);
			return 3;
		}
	}
	closedir(dir);

	return rmdir(path);
}

char *str_replace(const char *str, unsigned char case_sensitive, ...)
{
	char *search, *replace, *ret;
	const char *pos;
	struct dict *replacements = dict_create();
	size_t *strlen_cache, new_len = 0;
	struct stringbuffer *sbuf = stringbuffer_create();

	va_list vl;
	va_start(vl, case_sensitive);

	while((search = va_arg(vl, char *)) != NULL && ((replace = va_arg(vl, char *)) != NULL))
		dict_insert(replacements, search, replace);
	va_end(vl);

	char *(*strstrfunc)(const char *, const char *) = (case_sensitive ?  strstr : strcasestr);

	pos = str;
	while(*pos != '\0')
	{
		// Position and value of next item to be replaced
		char *minpos = NULL;
		struct dict_node *minnode;
		// Find next item
		dict_iter(node, replacements)
		{
			char *nextpos = strstrfunc(pos, node->key);
			if(minpos == NULL || (nextpos && nextpos < minpos))
			{
				minpos = nextpos;
				minnode = node;
			}
		}

		if(minpos != NULL)
		{
			// we found a replacement to be done, append everything up to here
			stringbuffer_append_string_n(sbuf, pos, minpos - pos);
			// next comes the key, append its value
			stringbuffer_append_string(sbuf, (char*)minnode->data);
			// move pos forward by the length of the key
			pos += (minpos - pos) + strlen(minnode->key);
		}
		else
		{
			// No more replacements to be done, simply append anything that remained in the original string
			stringbuffer_append_string(sbuf, pos);
			break;
		}
	}

	// Duplicate string to be returned
	ret = strdup(sbuf->string);
	// Free memory
	stringbuffer_free(sbuf);
	dict_free(replacements);

	return ret;
}

char *strip_html_tags(char *str)
{
	char *tmp, *tmp2;
	size_t len = strlen(str) + 1;

	tmp = str;
	while((tmp2 = strchr(tmp, '<')))
	{
		if(!(tmp = strchr(tmp2, '>')))
			break;

		tmp++;
		memmove(tmp2, tmp, len - (tmp - str));
		tmp = tmp2;
	}

	return str;
}

char *strip_duplicate_whitespace(char *str)
{
	char *tmp;
	size_t len, white;

	trim(str);
	// Make sure to copy null byte as well
	len = strlen(str) + 1;

	for(tmp = str; *tmp; tmp++)
	{
		if(!isspace(*tmp))
			continue;

		// Keep first space
		tmp++;
		white = strspn(tmp, whitespace_chars);
		memmove(tmp, tmp + white, (len - white) - (tmp - str));
	}
	return str;
}

size_t substr_count(const char *haystack, const char *needle, unsigned char case_sensitive)
{
	size_t count;
	const char *tmp = haystack;
	char *(*strstr_func)(const char *haystack, const char *needle) = case_sensitive ? strcasestr : strstr;

	while((tmp = strstr_func(tmp, needle)))
		tmp++, count++;

	return count + 1;
}

char *ltrim(char * const str)
{
	int len;
	for(len = strlen(str); len && isspace(str[len - 1]); len--);
	str[len] = '\0';
	return str;
}

char *rtrim(char * const str)
{
	char *tmp = str + strspn(str, whitespace_chars);
	memmove(str, tmp, strlen(str) - (tmp - str) + 1);
	return str;
}

char *urlencode(const char *s)
{
	register int x, y;
	int len = strlen(s);
	char *str;

	str = malloc(3 * len + 1);
	for (x = 0, y = 0; len--; x++, y++)
	{
		str[y] = (unsigned char) s[x];
		if ((str[y] < '0' && str[y] != '-' && str[y] != '.') ||
		    (str[y] < 'A' && str[y] > '9') ||
		    (str[y] > 'Z' && str[y] < 'a' && str[y] != '_') ||
		    (str[y] > 'z'))
		{
			str[y++] = '%';
			str[y++] = hexchars[(unsigned char) s[x] >> 4];
			str[y] = hexchars[(unsigned char) s[x] & 15];
		}
	}

	str[y] = '\0';
	return str;
}

char *urldecode(char *uri)
{
	char *start, *out;
	int h, g;

	/* Decode the URI in-place. */
	for(out = start = uri; *uri; uri++)
	{
		if(*uri == '+')
		{
			*out++ = ' ';
			continue;
		}

		if(*uri != '%')
		{
			*out++ = *uri;
			continue;
		}

		/* The current character is a '%', expect two hex characters.
		   Invalid escape sequences are passed through (non-RFC behavior). */
		h = ct_get(ctype, *++uri);
		if((h & CT_XDIGIT) == 0)
		{
			*out++ = '%';
			uri -= 1;
			continue;
		}

		g = ct_get(ctype, *++uri);
		if((g & CT_XDIGIT) == 0)
		{
			*out++ = '%';
			uri -= 2;
			continue;
		}

		/* Write the resulting character out. */
		*out++ = ((h & 15) << 4) + (g & 15);
	}
	*out = 0;

	return start;
}

char *html_encode(const char *str)
{
	struct stringbuffer *buf = stringbuffer_create();
	char *string;

	for(const char *ptr = str; *ptr; ptr++)
	{
		if(ct_get(ctype, *ptr) & CT_HTML)
			stringbuffer_append_printf(buf, "&#%d;", *ptr);
		else
			stringbuffer_append_char(buf, *ptr);
	}

	string = strdup(buf->string);
	stringbuffer_free(buf);
	return string;
}

/*
 * check if string is utf8
 * 0: invalid utf8, -1: plain ascii, 1: utf8
 */
int is_utf8(const char *buf)
{
	unsigned int i, j, gotone = 0;
	unsigned int len = strlen((const char *)buf);
	unsigned int ascii = 1;


	for(i = 0; i < len; i++) {
		if((buf[i] & 0x80) == 0) {  /* 0xxxxxxx is plain ASCII */

		} else if((buf[i] & 0x40) == 0) { /* 10xxxxxx never 1st byte */
			return 0;
		} else {         /* 11xxxxxx begins UTF-8 */
			ascii = 0;
			unsigned int following;

			if((buf[i] & 0x20) == 0) {    /* 110xxxxx */
				/* c = buf[i] & 0x1f; */
				following = 1;
			} else if((buf[i] & 0x10) == 0) {  /* 1110xxxx */
				/* c = buf[i] & 0x0f; */
				following = 2;
			} else if((buf[i] & 0x08) == 0) {  /* 11110xxx */
				/* c = buf[i] & 0x07; */
				following = 3;
			} else if((buf[i] & 0x04) == 0) {  /* 111110xx */
				/* c = buf[i] & 0x03; */
				following = 4;
			} else if((buf[i] & 0x02) == 0) {  /* 1111110x */
				/* c = buf[i] & 0x01; */
				following = 5;
			} else {
				return 0;
			}

			for(j = 0; j < following; j++) {
				if(++i >= len)
					return gotone ? 1 : (ascii ? -1 : 0);

				if((buf[i] & 0x80) == 0 || (buf[i] & 0x40))
					return 0;

				/* c = (c << 6) + (buf[i] & 0x3f); */
			}

			gotone = 1;
		}
	}

	return gotone ? 1 : (ascii ? -1 : 0);
}

/*
 * convert string to utf8
 */
void make_utf8(const char *str, char *buf, size_t bufsize)
{
	unsigned int i = 0;
	unsigned char *c;

	for (c = (unsigned char *)str; *c && i < bufsize - 1; c++) {
		if ((*c & 0x80) == 0) // plain ascii
			buf[i++] = *c;
		else if (*c == 128) { // euro sign
			if(i >= bufsize - 3)
				buf[i++] = '?';
			else
			{
				buf[i++] = '\xE2';
				buf[i++] = '\x82';
				buf[i++] = '\xAC';
			}
		}
		else if (*c == 133) { // "..." sign
			if(i >= bufsize - 3)
				buf[i++] = '?';
			else
			{
				buf[i++] = '\xE2';
				buf[i++] = '\x80';
				buf[i++] = '\xA6';
			}
		}
		else {
			if(i >= bufsize - 2)
				buf[i++] = '?';
			else
			{
				buf[i++] = (*c >> 6) | 192;
				buf[i++] = (*c & 63) | 128;
			}
		}
	}

	buf[i] = '\0';
}

char *iconv_str(const char *from_charset, const char *to_charset, const char *input)
{
	size_t inleft, outleft, converted = 0;
	char *output, *outbuf, *tmp;
	const char *inbuf;
	size_t outlen;
	iconv_t cd;

	if((cd = iconv_open(to_charset, from_charset)) == (iconv_t) -1)
		return NULL;

	inleft = strlen(input);
	inbuf = input;

	/* we'll start off allocating an output buffer which is the same size
	 * as our input buffer. */
	outlen = inleft;

	/* we allocate 4 bytes more than what we need for nul-termination... */
	if(!(output = malloc(outlen + 4))) {
		iconv_close(cd);
		return NULL;
	}

	while(1) {
		errno = 0;
		outbuf = output + converted;
		outleft = outlen - converted;

		converted = iconv(cd, (char **) &inbuf, &inleft, &outbuf, &outleft);
		if(converted != (size_t)-1 || errno == EINVAL) {
			/*
			 * EINVAL: An incomplete multibyte sequence has been encountered in the input.
			 * We'll just truncate it and ignore it.
			 */
			break;
		}

		if(errno != E2BIG) {
			printf("errno %d: %s\n", errno, strerror(errno));
			/*
			 * EILSEQ An invalid multibyte sequence has been encountered in the input.
			 * Bad input, we can't really recover from this.
			 */
			iconv_close(cd);
			free(output);
			return NULL;
		}

		/*
		 * E2BIG: There is not sufficient room at *outbuf.
		 * We just need to grow our outbuffer and try again.
		 */
		converted = outbuf - output;
		outlen += (inleft * 2) + 8;

		if(!(tmp = realloc(output, outlen + 4))) {
			iconv_close(cd);
			free(output);
			return NULL;
		}

		output = tmp;
		outbuf = output + converted;
	};

	/* flush the iconv conversion */
	iconv(cd, NULL, NULL, &outbuf, &outleft);
	iconv_close(cd);

	/* Note: not all charsets can be nul-terminated with a single
	 * nul byte. UCS2, for example, needs 2 nul bytes and UCS4
	 * needs 4. I hope that 4 nul bytes is enough to terminate all
	 * multibyte charsets? */

	/* nul-terminate the string */
	memset(outbuf, 0, 4);

	return output;
}

unsigned char channel_mode_changes_state(struct irc_channel *channel, const char *mode, const char *arg)
{
	char sign = '+';
	if(*mode == '+' || *mode == '-') {
		sign = *mode++;
	}
	// make sure there is only one mode character
	assert_return(strlen(mode) == 1, 0);
	unsigned char set = (sign == '+');
	int flags = channel->modes;
	int flag = 0;

	switch(*mode) {
		case 'v':
		case 'o':
			assert_return(arg != NULL, 0);
			struct irc_user *ircuser = user_find(arg);
			assert_return(ircuser != NULL, 0);
			struct irc_chanuser *chanuser = channel_user_find(channel, ircuser);
			assert_return(chanuser != NULL, 0);
			flag = (*mode == 'v') ? MODE_VOICE : MODE_OP;
			flags = chanuser->flags;
			break;
		case 'k':
			assert_return(arg != NULL, 0);
			if(flags & MODE_KEYED) {
				// if a key is already set, the only allowed state change is removing the key
				return !strcmp(channel->key, arg) && !set;
			}
			// otherwise, setting any key is a valid state change
			return set;
		case 'l':
			if(set) {
				assert_return(arg != NULL, 0);
			}
			flag = MODE_LIMIT;
			break;
		case 'i':
			flag = MODE_INVITEONLY;
			break;
		case 't':
			flag = MODE_TOPICLIMIT;
			break;
		case 'p':
			flag = MODE_PRIVATE;
			break;
		case 's':
			flag = MODE_SECRET;
			break;
		case 'm':
			flag = MODE_MODERATED;
			break;
		case 'n':
			flag = MODE_NOPRIVMSGS;
			break;
		case 'D':
			flag = MODE_DELJOINS;
			break;
		case 'd':
			return 0;
		case 'r':
			flag = MODE_REGONLY;
			break;
		case 'c':
			flag = MODE_NOCOLOUR;
			break;
		case 'C':
			flag = MODE_NOCTCP;
			break;
		case 'z':
			flag = MODE_REGISTERED;
			break;
		default:
			log_append(LOG_ERROR, "channel_mode_changes_state: Unsupported mode: %s", mode);
			return 0;
	}
	return ((flags & flag) != 0) != set;
}

time_t strtotime(const char *str)
{
	int hours, minutes;
	struct tm *tm;

	if(sscanf(str, "%2d:%2d", &hours, &minutes) != 2)
		return 0;

	tm = localtime(&now);
	if(hours < tm->tm_hour || (hours == tm->tm_hour && minutes < tm->tm_min))
		tm->tm_mday++;
	tm->tm_hour = hours;
	tm->tm_min = minutes;
	tm->tm_sec = 0;
	return mktime(tm);
}
