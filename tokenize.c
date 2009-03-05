#include "global.h"
#include "tokenize.h"

/**
 * @param str Input string; this string gets destroyed!
 * @param vec Output vector
 * @param vec_size Size of output vector array
 * @param token Token to use
 * @param allow_empty Allow empty elements
 *
 * @return Count of output vector items
 *
 * This function splits up a string using the given token
 */
unsigned int tokenize(char *str, char **vec, unsigned int vec_size, char token, unsigned char allow_empty)
{
	unsigned int count = 1;
	unsigned char inside_string = 0;
	char *ch = str;

	vec[0] = str;

	for(ch = str; *ch; ch++)
	{
		if((*ch == token) && (inside_string == 0))
			*ch = '\0';

		while((*ch == token) && (inside_string == 1))
		{
			*ch++ = '\0';
			if(!allow_empty && *ch == token)
				continue;

			if(allow_empty || *ch)
				vec[count++] = ch;

			if((count >= vec_size) || (*ch == '\0'))
				return count;
		}

		if(inside_string == 0)
			inside_string = 1;
	}

	return count;
}


/**
 * @param str Input string; this string gets destroyed
 * @param vec Output vector
 * @param vec_size Size of output vector
 * @param token Token to use
 * @param ltoken Token marking the last item
 *
 * @return Count of output vector items
 *
 * This function splits up a string using the given token and
 * the given last-item token
 */
unsigned int itokenize(char *str, char **vec, unsigned int vec_size, char token, char ltoken)
{
	char *ch = str;
	unsigned int count = 1;
	unsigned char inside_string = 0;

	vec[0] = ch;

	for(ch = str; *ch; ch++)
	{
		if((*ch == token) && (inside_string == 0))
			*ch = '\0';

		while((*ch == token) && (inside_string == 1))
		{
			*ch++ = '\0';

			if((*ch == ltoken))
			{
				*ch++ = '\0';
				vec[count++] = ch;
				return count;
			}

			vec[count++] = ch;

			if(((count + 1) >= vec_size) || (*ch == '\0'))
				return count;
		}

		if(inside_string == 0)
			inside_string = 1;
	}

	return count;
}

/**
 * @param num_items Number of vector items to use
 * @param vec Input vector
 * @param sep Char to insert between vector items
 *
 * @return String containing num_items elements from vec, separated by sep; must be free'd
 *
 * This function "unsplits" a vector using the given separator and
 * returns the string.
 */
char *untokenize(unsigned int num_items, char **vec, const char *sep)
{
	char *str;
	size_t len;
	unsigned int i;

	assert_return(num_items > 0, NULL);
	len = 1; // '\0'
	len += strlen(sep) * (num_items - 1); // separators between items
	len += strlen(vec[0]); // first item

	if(num_items > 1)
	{
		for(i = 1; i < num_items; i++)
		{
			len += strlen(vec[i]);
		}
	}

	str = malloc(len);
	memset(str, 0, len);

	strncpy(str, vec[0], len);
	if(num_items > 1)
	{
		for(i = 1; i < num_items; i++)
		{
			strncat(str, sep, len);
			strncat(str, vec[i], len);
		}
	}

	return str;
}
