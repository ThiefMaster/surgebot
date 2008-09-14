/* ctype.h - Character type table manipulation
 * Copyright 2005 Zoot
 *
 * Taken from Srvx2
 */

#ifndef CTYPE_H
#define CTYPE_H

/** Mapping of characters to type bitmaps. */
struct ctype_map
{
	unsigned int m[256];
};

/* Basic ctype operators. */
#define ct_get(M, C)		((M).m[(unsigned char)(C)])
#define ct_set(M, C, F)		((M)->m[(unsigned char)(C)]) |= (F)
#define ct_unset(M, C, F)	((M)->m[(unsigned char)(C)]) &= ~(F)

/** Mark character in the range [start..end] with the given flags. */
void ctype_mark_range(struct ctype_map *map, unsigned int flags, char start, char end);
/** Mark all characters in the string with the given flags. */
void ctype_mark_string(struct ctype_map *map, unsigned int flags, char *s);
/** Mark characters as digits belonging to a certain class. */
void ctype_mark_digit(struct ctype_map *map, unsigned int flag, const char *s);

/** Mark characters matching the filter flags with the given flags. */
void ctype_mark_copy(struct ctype_map *map, unsigned int flags, struct ctype_map *src, unsigned int filter);

/** Remove the given flags from characters in the range [start..end]. */
void ctype_unmark_range(struct ctype_map *map, unsigned int flags, char start, char end);
/** Remove flags from characters in the string. */
void ctype_unmark_string(struct ctype_map *map, unsigned int flags, char *s);

extern struct ctype_map ctype;

#define CT_UPPER  0x010  /**< Character is upper-case letter. */
#define CT_LOWER  0x020  /**< Character is lower-case letter. */
#define CT_PUNCT  0x040  /**< Character is punctuation. */
#define CT_SPACE  0x080  /**< Character is whitespace. */
#define CT_ODIGIT 0x100  /**< Character is octal digit. */
#define CT_DIGIT  0x200  /**< Character is decimal digit. */
#define CT_XDIGIT 0x400  /**< Character is hexadecimal digit. */
#define CT_HTML   0x800  /**< Character is bad when used in html document. */
#define CT_ALPHA  (CT_UPPER|CT_LOWER) /**< Character is alphabetic. */
#define CT_ALNUM  (CT_LOWER|CT_ALPHA|CT_DIGIT) /**< Character is alphanumeric. */

#define ct_isupper(CH)            (ct_get(ctype, (CH)) & CT_UPPER)
#define ct_islower(CH)            (ct_get(ctype, (CH)) & CT_LOWER)
#define ct_ispunct(CH)            (ct_get(ctype, (CH)) & CT_PUNCT)
#define ct_isspace(CH)            (ct_get(ctype, (CH)) & CT_SPACE)
#define ct_isodigit(CH)           (ct_get(ctype, (CH)) & CT_ODIGIT)
#define ct_isdigit(CH)            (ct_get(ctype, (CH)) & CT_DIGIT)
#define ct_isxdigit(CH)           (ct_get(ctype, (CH)) & CT_XDIGIT)
#define ct_isalpha(CH)            (ct_get(ctype, (CH)) & CT_ALPHA)
#define ct_isalnum(CH)            (ct_get(ctype, (CH)) & CT_ALNUM)
#define ct_istoken(CH)            (ct_get(ctype, (CH)) & CT_TOKEN)

#define ct_xdigit_val(CH)         (ct_get(ctype, (CH)) & 15)

#endif
