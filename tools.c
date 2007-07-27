#include "global.h"
#include "tools.h"
#include "chanuser.h"

static char **str_tab;
static unsigned int str_tab_size;

void tools_init()
{
	str_tab_size = 1001;
	str_tab = calloc(str_tab_size, sizeof(char *));
}

void tools_fini()
{
	for(int i = 0; i < str_tab_size; i++)
	{
		if(str_tab[i])
			free(str_tab[i]);
	}

	free(str_tab);
}

void split_mask(char *mask, char **nick, char **ident, char **host)
{
	char *temp = mask;

	while(*mask != '!')
		mask++;

	*mask = '\0';
	*nick = strdup(temp);
	temp = ++mask;

	while(*mask != '@')
		mask++;

	*mask = '\0';

	if(ident)
		*ident = strdup(temp);

	if(host)
		*host = strdup(++mask);
}

unsigned int aredigits(const char *text)
{
	unsigned int i;

	for(i = 0; i < strlen(text); i++)
		if(!isdigit(text[i]))
			return 0;

	return 1;
}

// from feigbot source
char *duration2string(time_t time)
{
	unsigned int i, words, pos, count;
	static char buffer[MAXLEN];
	static const struct
	{
		const char *name;
		long length;
	} unit[] = {
		{ "year", 365 * 24 * 60 * 60 },
		{ "week",   7 * 24 * 60 * 60 },
		{ "day",        24 * 60 * 60 },
		{ "hour",            60 * 60 },
		{ "minute",               60 },
		{ "second",                1 }
	};

	memset(buffer, 0, sizeof(buffer));

	if(!time)
	{
		strcpy(buffer, "0 seconds");
		return buffer;
	}

	for(i = words = pos = 0; time && (words < 2) && (i < ArraySize(unit)); i++)
	{
		if(time < unit[i].length)
			continue;

		count = time / unit[i].length;
		time = time % unit[i].length;

		if(words == 1)
			pos += snprintf(buffer + pos, sizeof(buffer) - pos, " and %d %s", count, unit[i].name);
		else
			pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%d %s", count, unit[i].name);

		if(count != 1)
			buffer[pos++] = 's';

		words++;
	}

	buffer[pos] = '\0';
	return buffer;
}

char *time2string(time_t time)
{
	static char str[26];
	ctime_r(&time, str);
	str[strlen(str)-1] = '\0';
	return str;
}

const char *chanmodes2string(int modes, unsigned int limit, const char *key)
{
	static char str[MAXLEN];
	unsigned int pos = 0;

#define do_mode_char(MODE, CHAR) if(modes & (MODE_ ## MODE)) str[pos++] = (CHAR);
	do_mode_char(SECRET, 's');
	do_mode_char(PRIVATE, 'p');
	do_mode_char(MODERATED, 'm');
	do_mode_char(TOPICLIMIT, 't');
	do_mode_char(INVITEONLY, 'i');
	do_mode_char(NOPRIVMSGS, 'n');
	do_mode_char(REGONLY, 'r');
	do_mode_char(DELJOINS, 'D');
	do_mode_char(WASDELJOIN, 'd');
	do_mode_char(NOCOLOUR, 'c');
	do_mode_char(NOCTCP, 'C');
	do_mode_char(REGISTERED, 'z');
#undef do_mode_char

	switch(modes & (MODE_KEYED | MODE_LIMIT))
	{
		case MODE_KEYED | MODE_LIMIT:
			pos += snprintf(str + pos, MAXLEN, "lk %d %s", limit, key);
			break;
		case MODE_KEYED:
			pos += snprintf(str + pos, MAXLEN, "k %s", key);
			break;
		case MODE_LIMIT:
			pos += snprintf(str + pos, MAXLEN, "l %d", limit);
			break;
	}

	str[pos] = '\0';
	return str;
}

// from srvx source
int IsChannelName(const char *name)
{
	unsigned int i;

	if(*name != '#' && *name != '&')
		return 0;

	for(i = 1; name[i]; ++i)
	{
		if(name[i] > 0 && name[i] <= 32)
			return 0;
		if(name[i] == ',')
			return 0;
		if(name[i] == '\xa0')
			return 0;
	}

	return 1;
}

unsigned int validate_string(const char *str, const char *allowed, char *c)
{
	unsigned int i;

	for(i = 0; i < strlen(str); i++)
	{
		if(!strchr(allowed, str[i]))
		{
			*c = str[i];
			return 0;
		}
	}

	return 1;
}

// from ircu 2.10.12
/*
 * Compare if a given string (name) matches the given
 * mask (which can contain wild cards: '*' - match any
 * number of chars, '?' - match any single character.
 *
 * return  0, if match
 *         1, if no match
 *
 *  Originally by Douglas A Lewis (dalewis@acsu.buffalo.edu)
 *  Rewritten by Timothy Vogelsang (netski), net@astrolink.org
 */

/** Check a string against a mask.
 * This test checks using traditional IRC wildcards only: '*' means
 * match zero or more characters of any type; '?' means match exactly
 * one character of any type.  A backslash escapes the next character
 * so that a wildcard may be matched exactly.
 * @param[in] mask Wildcard-containing mask.
 * @param[in] name String to check against \a mask.
 * @return Zero if \a mask matches \a name, non-zero if no match.
 */
int match(const char *mask, const char *name)
{
	const char *m = mask, *n = name;
	const char *m_tmp = mask, *n_tmp = name;
	int star_p;

	for (;;) switch (*m) {
	case '\0':
		if (!*n)
			return 0;
	backtrack:
		if (m_tmp == mask)
			return 1;
		m = m_tmp;
		n = ++n_tmp;
		break;
	case '\\':
		m++;
		/* allow escaping to force capitalization */
		if (*m++ != *n++)
			return 1;
		break;
	case '*': case '?':
		for (star_p = 0; ; m++) {
			if (*m == '*')
				star_p = 1;
			else if (*m == '?') {
				if (!*n++)
					goto backtrack;
			} else break;
		}
		if (star_p) {
			if (!*m)
				return 0;
			else if (*m == '\\') {
				m_tmp = ++m;
				if (!*m)
					return 1;
				for (n_tmp = n; *n && *n != *m; n++) ;
			} else {
				m_tmp = m;
				for (n_tmp = n; *n && tolower(*n) != tolower(*m); n++) ;
			}
		}
		/* and fall through */
	default:
		if (!*n)
			return *m != '\0';
		if (tolower(*m) != tolower(*n))
			goto backtrack;
		m++;
		n++;
		break;
	}
}

/* Returns a string containing num. */
const char *strtab(unsigned int num)
{
	if(num > 65536)
		return "(overflow)";

	if(num > str_tab_size)
	{
		unsigned int old_size = str_tab_size;
		while (num >= str_tab_size)
			str_tab_size <<= 1;
		str_tab = realloc(str_tab, str_tab_size * sizeof(char *));
		memset(str_tab + old_size, 0, (str_tab_size - old_size) * sizeof(char *));
	}

	if(!str_tab[num])
	{
		str_tab[num] = malloc(12);
		sprintf(str_tab[num], "%u", num);
	}

	return str_tab[num];
}

size_t strlcpy(char *out, const char *in, size_t len)
{
	size_t in_len;

	in_len = strlen(in);
	if (in_len < --len)
		memcpy(out, in, in_len + 1);
	else
	{
		memcpy(out, in, len);
		out[len] = '\0';
	}
	return in_len;
}

size_t strlcat(char *out, const char *in, size_t len)
{
	size_t out_len, in_len;

	out_len = strlen(out);
	in_len = strlen(in);
	if (out_len > --len)
		out[len] = '\0';
	else if (out_len + in_len < len)
		memcpy(out + out_len, in, in_len + 1);
	else
	{
		memcpy(out + out_len, in, len - out_len);
		out[len] = '\0';
	}
	return out_len + in_len;
}

unsigned int is_valid_string(const char *str)
{
	const char *temp = str;
	do
	{
		unsigned int i = *temp;
		switch(i)
		{
			case 0:
				 return 1;
			case 2:
			case 3:
			case 9:
			case 15:
			case 22:
				continue;
			default:
				if(i >= 31 && i <= 255)
					continue;
				else
					return 0;
		}
	 } while(*temp++);

	 return 1;
}

void strtolower(char *str)
{
	for(char *ptr = str; *ptr; ptr++)
		*ptr = tolower(*ptr);
}

unsigned char check_date(int day, int month, int year)
{
	struct tm timeinfo, *tm;
	time_t timestamp;

	// First check the month, since it's the easiest, and the day and year for positivity
	if(month < 1 || month > 12 || day <= 0 || year < 1970)
		return 0;

	timeinfo.tm_year = year - 1900;
	timeinfo.tm_mon = month;
	timeinfo.tm_mday = 0;
	timeinfo.tm_hour = timeinfo.tm_min = timeinfo.tm_sec = 0;

	timestamp = mktime(&timeinfo);
	if(timestamp == -1)
		return 0;

	tm = localtime(&timestamp);
	return day <= tm->tm_mday;
}

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

char *strip_codes(char *str)
{
    int col, j;
    char *dup;

    col = j = 0;

    for(dup = str; *dup; dup++)
    {
        if(*dup == 2 || *dup == 15 || *dup == 22 || *dup == 31)
            continue;

        else if(*dup == 3)
            col = 1;

        else if((col == 2 || col == 3) && *dup == ',')
            col = 4;

        else if((col == 1 || col == 2 || col == 4 || col == 5) && *dup >= '0' && *dup <= '9')
            col++;

        else
        {
            str[j++] = *dup;
            col = 0;
        }
    }
    str[j]= '\0';
	return str;
}

