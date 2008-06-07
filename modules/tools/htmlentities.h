#ifndef __HTMLENTITIES_H__
#define __HTMLENTITIES_H__

static const struct
{
	char *entity;
	char character;
}
entities[] =
{
	{ "auml",	'�' },
	{ "ouml",	'�' },
	{ "uuml",	'�' },
	{ "szlig",	'�' },
	{ "quot",	'"' },
	{ "amp",	'&' },
	{ "lt",		'<' },
	{ "gt",		'>' }
};

//struct entity decode_entity(const char *str);

#endif /* __HTMLENTITIES_H__ */
