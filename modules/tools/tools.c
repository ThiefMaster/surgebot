#include "global.h"
#include "module.h"
#include <string.h> // strlen
#include <stdlib.h> // malloc

// Module header
#include "tools.h"

MODULE_DEPENDS(NULL);

MODULE_INIT {}
MODULE_FINI {}

static unsigned char hexchars[] = "0123456789ABCDEF";

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
