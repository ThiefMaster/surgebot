#include "global.h"
#include "module.h"

// Module header
#include "tools.h"

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
	unsigned char white = 0;
	char *tmp = str, *tmp2 = NULL, *end;
	size_t len;
	
	trim(str);
	
	len = strlen(str);
	end = str + len;
	
	while(tmp < end)
	{
		if(isspace(*tmp))
		{
			if(white && !tmp2)
				tmp2 = tmp;
			
			white = 1;
			tmp++;
			continue;
		}
		else
		{
			if(tmp2)
			{
				memmove(tmp2, tmp, len - (tmp - str));
				end -= (tmp - tmp2);
				
				tmp = tmp2;
				tmp2 = NULL;
				continue;
			}
			white = 0;
			tmp++;
		}
	}
	
	*end = 0;
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

char *trim(char * const str)
{
	int len = strlen(str);
	char *tmp = str;

	while(len > 0 && isspace(str[len - 1])) len--;
		str[len] = '\0';
	
	len++;
	tmp = str + strspn(str, " \n\r\t\v\f");
	memmove(str, tmp, len - (tmp - str));
	
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
