/* ctype.c - Character type table manipulation
 * Copyright 2005 Zoot <zoot@gamesurge.net>
 *
 * Taken from Srvx2
 */

#include "global.h"
#include "x_ctype.h"

void ctype_mark_range(struct ctype_map *map, unsigned int flags, char st, char end)
{
	for( ; (unsigned char)st < (unsigned char)end; st++)
		ct_set(map, st, flags);
	ct_set(map, end, flags);
}

void ctype_mark_string(struct ctype_map *map, unsigned int flags, char *s)
{
	for(unsigned int i = 0; s[i]; )
		ct_set(map, s[i++], flags);
}

void ctype_mark_digit(struct ctype_map *map, unsigned int flag, const char *s)
{
	for(unsigned int i = 0; s[i]; ++i)
		ct_set(map, s[i], i | flag);
}

void ctype_mark_copy(struct ctype_map *map, unsigned int flags, struct ctype_map *src, unsigned int filter)
{
	for(unsigned int c = 0; c < ArraySize(map->m); ++c)
		if(ct_get(*src, c) & filter)
			ct_set(map, c, flags);
}

void ctype_unmark_range(struct ctype_map *map, unsigned int flags, char st, char end)
{
	for( ; st <= end; st++)
		ct_unset(map, st, flags);
}

void ctype_unmark_string(struct ctype_map *map, unsigned int flags, char *s)
{
	for(unsigned int i = 0; s[i]; )
		ct_unset(map, s[i++], flags);
}
