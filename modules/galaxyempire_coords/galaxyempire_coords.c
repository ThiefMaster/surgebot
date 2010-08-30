#include "global.h"
#include "module.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"
#include "irc.h"
#include "chanuser.h"
#include "database.h"
#include "stringlist.h"

MODULE_DEPENDS("commands", "chanreg", "help", NULL);

COMMAND(coords_add);
COMMAND(coords_del);
COMMAND(coords_list);
COMMAND(coords_search);
static void ge_coords_db_read(struct dict *db_nodes, struct chanreg *reg);
static int ge_coords_db_write(struct database_object *dbo, struct chanreg *reg);
static int ge_coords_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason);
static void ge_coords_moved(struct chanreg *reg, const char *from, const char *to);
static int chanstatus_validator(struct chanreg *reg, struct irc_source *src, const char *value);
static const char* chanstatus_formatter(struct chanreg *reg, const char *value);
static const char* chanstatus_encoder(struct chanreg *reg, const char *old_value, const char *value);


static struct module *this;
static struct chanreg_module *cmod;
static struct dict *global_coords;

MODULE_INIT
{
	this = self;

	help_load(this, "galaxyempire_coords.help");

	global_coords = dict_create();
	dict_set_free_funcs(global_coords, free, (dict_free_f *)dict_free);

	cmod = chanreg_module_reg("GECoords", 0, ge_coords_db_read, ge_coords_db_write, NULL, ge_coords_disabled, ge_coords_moved);
	chanreg_module_setting_reg(cmod, "ViewPermission", "*", chanstatus_validator, chanstatus_formatter, chanstatus_encoder);
	chanreg_module_setting_reg(cmod, "EditPermission", "*", chanstatus_validator, chanstatus_formatter, chanstatus_encoder);
	chanreg_module_readdb(cmod);

	DEFINE_COMMAND(self, "coords add",	coords_add,	2, CMD_LAZY_ACCEPT_CHANNEL, "inchannel() || chanuser(200)");
	DEFINE_COMMAND(self, "coords del",	coords_del,	2, CMD_LAZY_ACCEPT_CHANNEL, "inchannel() || chanuser(250)");
	DEFINE_COMMAND(self, "coords search",	coords_search,	1, CMD_LAZY_ACCEPT_CHANNEL, "inchannel() || chanuser(200)");
	DEFINE_COMMAND(self, "coords list",	coords_list,	0, CMD_LAZY_ACCEPT_CHANNEL, "inchannel() || chanuser(225)");
}

MODULE_FINI
{
	chanreg_module_writedb(cmod);
	chanreg_module_unreg(cmod);

	dict_free(global_coords);
}

static int chanstatus_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	if(!strcasestr(value, "voice") && !strcasestr(value, "op") && strcasecmp(value, "public") &&
	   strcasecmp(value, "everyone") && strcmp(value, "*") && strcmp(value, "+") && strcmp(value, "@"))
	{
		reply("Permission must be one of $bEveryone$b, $bVoiced$b, $bOpped$b.");
		return 0;
	}

	return 1;
}

static const char* chanstatus_formatter(struct chanreg *reg, const char *value)
{
	if(*value == 'v')
		return "Voice (+) or Op (@)";
	else if(*value == 'o')
		return "Op (@)";
	else
		return "Everyone";
}

static const char* chanstatus_encoder(struct chanreg *reg, const char *old_value, const char *value)
{
	if(strcasestr(value, "voice") || !strcmp(value, "+"))
		return "v";
	else if(strcasestr(value, "op") || !strcmp(value, "@"))
		return "o";
	else /* if(!strcasecmp(value, "public") || !strcasecmp(value, "everyone") || !strcmp(value, "*")) */
		return "*";
}

static unsigned int check_perm(struct chanreg *reg, struct irc_source *src, struct irc_user *user, unsigned int edit)
{
	const char *perm = chanreg_setting_get(reg, cmod, (edit ? "EditPermission" : "ViewPermission"));
	struct irc_channel *channel;
	struct irc_chanuser *chanuser;

	if(!(channel = channel_find(reg->channel)) || !(chanuser = dict_find(channel->users, user->nick)))
	{
		reply("You must be in $b%s$b.", reg->channel);
		return 0;
	}

	if(*perm == 'v' && !(chanuser->flags & (MODE_VOICE|MODE_OP)))
	{
		reply("You must be voiced or opped in $b%s$b.", reg->channel);
		return 0;
	}
	else if(*perm == 'o' && !(chanuser->flags & MODE_OP))
	{
		reply("You must be opped in $b%s$b.", reg->channel);
		return 0;
	}

	return 1;
}

static void ge_coords_db_read(struct dict *db_nodes, struct chanreg *reg)
{
	struct dict *obj;

	if((obj = database_fetch(db_nodes, "coords", DB_OBJECT)))
	{
		struct dict *channel_coords = dict_create();
		dict_set_free_funcs(channel_coords, free, (dict_free_f *)stringlist_free);
		dict_insert(global_coords, strdup(reg->channel), channel_coords);
		dict_iter(rec, obj)
		{
			struct stringlist *list = ((struct db_node *)rec->data)->data.slist;
			dict_insert(channel_coords, strdup(rec->key), stringlist_copy(list));
		}
	}
}

static int ge_coords_db_write(struct database_object *dbo, struct chanreg *reg)
{
	struct dict *channel_coords;
	database_obj_begin_object(dbo, "coords");
	if((channel_coords = dict_find(global_coords, reg->channel)) && dict_size(channel_coords))
	{
		dict_iter(node, channel_coords)
			database_obj_write_stringlist(dbo, node->key, node->data);
	}
	database_obj_end_object(dbo);
	return 0;
}

static int ge_coords_disabled(struct chanreg *reg, unsigned int delete_data, enum cmod_disable_reason reason)
{
	if(delete_data)
		dict_delete(global_coords, reg->channel);
	return 0;
}

static void ge_coords_moved(struct chanreg *reg, const char *from, const char *to)
{
	struct dict_node *node = dict_find_node(global_coords, from);
	if(!node)
		return;
	free(node->key);
	node->key = strdup(to);
}

COMMAND(coords_add)
{
	const char *username, *coords_str;
	char *coords_dup, *coords[12];
	unsigned int coords_count;
	unsigned char success = 0;
	struct dict *channel_coords;
	struct stringlist *user_coords;

	CHANREG_MODULE_COMMAND(cmod)

	if(!check_perm(reg, src, user, 1))
		return 0;

	coords_str = argv[1];
	coords_dup = strdup(coords_str);
	username = argline + (argv[2] - argv[0]);

	if(!(channel_coords = dict_find(global_coords, reg->channel)))
	{
		channel_coords = dict_create();
		dict_set_free_funcs(channel_coords, free, (dict_free_f *)stringlist_free);
		dict_insert(global_coords, strdup(reg->channel), channel_coords);
	}

	if(!(user_coords = dict_find(channel_coords, username)))
	{
		user_coords = stringlist_create();
		dict_insert(channel_coords, strdup(username), user_coords);
	}

	coords_count = tokenize(coords_dup, coords, 12, ',', 0);
	if(coords_count > 11)
		reply("Es können max. 11 Koordinaten auf einmal eingetragen werden.");

	for(unsigned int i = 0; i < coords_count && i < 11; i++)
	{
		if(match("?*:?*:?*", coords[i]))
		{
			reply("Ungültige Koordinaten: %s", coords[i]);
			continue;
		}

		if(stringlist_find(user_coords, coords[i]) != -1)
		{
			reply("Die Koordinaten $b%s$b sind bereits eingetragen.", coords[i]);
			continue;
		}

		dict_iter(node, channel_coords)
		{
			struct stringlist *list = node->data;
			int pos;
			if(list == user_coords)
				continue;
			if((pos = stringlist_find(list, coords[i])) != -1)
			{
				reply("Die Koordinaten $b%s$b waren bei $b%s$b eingetragen und wurden dort gelöscht.", coords[i], node->key);
				stringlist_del(list, pos);
				if(list->count == 0)
					dict_delete(channel_coords, node->key);
				break;
			}
		}

		stringlist_add(user_coords, strdup(coords[i]));
		reply("Koordinaten $b%s$b für Spieler $b%s$b hinzugefügt.", coords[i], username);
		success = 1;
	}

	if(user_coords->count > 10)
		reply("Warnung, es sind mehr als $b10$b Koordinaten für diesen Spieler eingetragen.");

	free(coords_dup);
	return success;
}

COMMAND(coords_del)
{
	const char *coords, *username;
	struct dict *channel_coords;
	struct stringlist *user_coords;
	int pos;

	CHANREG_MODULE_COMMAND(cmod)

	if(!check_perm(reg, src, user, 1))
		return 0;

	coords = argv[1];
	username = argline + (argv[2] - argv[0]);

	if(!(channel_coords = dict_find(global_coords, reg->channel)) || !(user_coords = dict_find(channel_coords, username)))
	{
		reply("Für den Spieler $b%s$b sind keine Koordinaten eingetragen.", username);
		return 0;
	}

	if(strcmp(coords, "*") && match("?*:?*:?*", coords))
	{
		reply("Ungültige Koordinaten ($b*$b löscht alle Koordinaten des Spielers).");
		return 0;
	}

	if(*coords == '*')
	{
		dict_delete(channel_coords, username);
		reply("Alle Koordinaten von $b%s$b wurden gelöscht.", username);
		return 1;
	}
	else if((pos = stringlist_find(user_coords, coords)) == -1)
	{
		reply("Diese Koordinaten sind bei $b%s$b nicht eingetragen.", username);
		return 0;
	}

	stringlist_del(user_coords, pos);
	if(user_coords->count == 0)
		dict_delete(channel_coords, username);
	if(dict_size(channel_coords) == 0)
		dict_delete(global_coords, reg->channel);
	reply("Koordinaten $b%s$b von $b%s$b wurden gelöscht.", coords, username);
	return 1;
}

COMMAND(coords_search)
{
	const char *username;
	struct dict *channel_coords;
	struct stringlist *user_coords;

	CHANREG_MODULE_COMMAND(cmod)

	if(!check_perm(reg, src, user, 0))
		return 0;

	username = argline + (argv[1] - argv[0]);

	if(!(channel_coords = dict_find(global_coords, reg->channel)) || !(user_coords = dict_find(channel_coords, username)))
	{
		reply("Für den Spieler $b%s$b sind keine Koordinaten eingetragen.", username);
		return 0;
	}

	reply("Koordinaten von $b%s$b:", username);
	for(unsigned int i = 0; i < user_coords->count; i++)
		reply("  %s", user_coords->data[i]);
	reply("Insgesamt $b%d$b Koordinaten gefunden.", user_coords->count);
	return 1;
}

COMMAND(coords_list)
{
	struct dict *channel_coords;

	CHANREG_MODULE_COMMAND(cmod)

	if(!check_perm(reg, src, user, 0))
		return 0;

	if(!(channel_coords = dict_find(global_coords, reg->channel)) || !dict_size(channel_coords))
	{
		reply("Es sind noch keine Koordinaten eingetragen.");
		return 0;
	}

	dict_iter(node, channel_coords)
	{
		struct stringlist *user_coords = node->data;
		reply("Koordinaten von $b%s$b (insgesamt %d):", node->key, user_coords->count);
		for(unsigned int i = 0; i < user_coords->count; i++)
			reply("  %s", user_coords->data[i]);
	}

	return 1;
}
