#include "global.h"
#include "tools.h"
//#include "channel.h"

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
char *time2string(time_t time)
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

/*
char *chanmodes2string(long modes, unsigned int limit, const char *key)
{
	char *str = malloc(MAXLEN);

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
*/

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
