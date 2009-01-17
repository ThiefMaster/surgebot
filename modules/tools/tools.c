#include "global.h"
#include "module.h"
#include "stringbuffer.h"

// Module header
#include "tools.h"

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
	{ "gt",		'>' },
	{ "apos",	'\'' }
};

unsigned char hexchars[] = "0123456789ABCDEF";
const char whitespace_chars[] = " \t\n\v\f\r";

MODULE_DEPENDS(NULL);

MODULE_INIT {}
MODULE_FINI {}

char *html_decode(char *str)
{
	int i;
	char *tmp2, *tmp = str, entity[11];
	size_t len = strlen(str);

	while((tmp = strchr(tmp, '&')))
	{
		// Entity if below 10 chars
		if(!(tmp2 = strchr(tmp, ';')))
			continue;

		if((tmp2 - tmp) > 10)
		{
			tmp = tmp2 + 1;
			continue;
		}

		if(tmp[1] == '#') // Numeric entity
		{
			strlcpy(entity, tmp + 2, (tmp2 - tmp) - 1);
			if((strspn(entity, "0123456789") < strlen(entity)) || !(i = atoi(entity)) || i < 32 || i > 255)
			{
				tmp = tmp2 + 1;
				continue;
			}

			tmp[0] = (char)i;
		}
		else // Non-numeric entity
		{
			for(i = 0; i < ArraySize(entities); i++)
			{
				if(!strncasecmp(tmp + 1, entities[i].entity, (tmp2 - tmp) - 1))
				{
					tmp[0] = entities[i].character;
					goto loop_continue;
				}
			}
			tmp++;
			continue;
		}
loop_continue:

		tmp++, tmp2++;
		memmove(tmp, tmp2, (len - (tmp2 - str) + 1));
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

char *str_replace(const char *str, const char *search, const char *replace, unsigned char case_sensitive)
{
	size_t replace_len = strlen(replace), search_len = strlen(search);
	char *(*strstrfunc)(const char *, const char *) = (case_sensitive ?  strstr : strcasestr);
	const char *tmp;
	char *tmp2, *ret;
	int i = 0;

	for(tmp = str; (tmp = strstrfunc(tmp, search)); tmp += search_len, i++);

	ret = malloc(strlen(str) + (replace_len - search_len) * i + 1);
	ret[0] = '\0';

	for(tmp = str; (tmp2 = strstrfunc(tmp, search)); tmp = (tmp2 + search_len))
	{
		// Append string up to found string
		strncat(ret, tmp, (tmp2 - tmp));
		// Append replace-string
		strcat(ret, replace);
	}

	// Append remaining string after last search occurence
	strcat(ret, tmp);
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

