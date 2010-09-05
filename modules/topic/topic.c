#include "global.h"
#include "dict.h"
#include "module.h"
#include "irc.h"
#include "modules/commands/commands.h"
#include "modules/chanreg/chanreg.h"
#include "modules/help/help.h"

/*
 * Topicmask module:
 *
 * Allows you to set a topicmask with different parts to be replaced.
 * Parts that you want to be replaced shall be surrounded by %, like this:
 *
 * 		News: #news#, Weather: #weather#
 *
 * To escape the #, use ##.
 */

MODULE_DEPENDS("commands", "chanreg", "help", NULL);

static struct dict *channel_topics;
static struct chanreg_module *cmod;
static struct database *topics_db;

static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason);
static int topicmask_validator(struct chanreg *reg, struct irc_source *src, const char *value);
static void topics_db_read(struct database *db);
static int topics_db_write(struct database *db);
static void topicmask_chanreg_del_f(struct chanreg *reg);

static struct dict *topicmask_get_replacements(const char *channel, unsigned char force);
static void topicmask_parse(struct chanreg *reg, struct irc_source *src, const char *value);
static void topicmask_set_topic(struct chanreg *reg);
static int topicmask_set(struct chanreg *reg, const char *param, const char *value);

COMMAND(topicmask);

MODULE_INIT
{
	help_load(self, "topic.help");

	// This dict has channelnames as keys and dicts with topic-param -> value as data
	channel_topics = dict_create();
	dict_set_free_funcs(channel_topics, free, (dict_free_f*)dict_free);

	cmod = chanreg_module_reg("Topicmask", 0, NULL, NULL, cmod_enabled, NULL, NULL);
	chanreg_module_setting_reg(cmod, "Topicmask", "*", topicmask_validator, NULL, NULL);
	chanreg_module_setting_reg(cmod, "ForceTopic", "No", NULL, boolean_formatter_onoff, boolean_encoder);

	DEFINE_COMMAND(self, "topicmask", topicmask, 0, 0, "true");

	topics_db = database_create("topics", topics_db_read, topics_db_write);
	database_read(topics_db, 1);
	database_set_write_interval(topics_db, 300);

	reg_chanreg_del_hook(topicmask_chanreg_del_f);
}

MODULE_FINI
{
	unreg_chanreg_del_hook(topicmask_chanreg_del_f);

	database_write(topics_db);
	database_delete(topics_db);
	chanreg_module_unreg(cmod);
	dict_free(channel_topics);
}

COMMAND(topicmask)
{
	CHANREG_MODULE_COMMAND(cmod);

	// Make sure this channel has a topicmask
	struct dict *replacements = dict_find(channel_topics, reg->channel);
	if(replacements == NULL || dict_size(replacements) == 0)
	{
		reply("Your topicmask has no patterns to replace, please set up a topicmask first.");
		return 0;
	}

	if(argc == 1)
	{
		// list all settings
		dict_iter(node, replacements)
		{
			reply("$b%s$b: %s", node->key, node->data != NULL ? (char*)node->data : "");
		}
		return 0;
	}
	if(argc == 2)
	{
		// first argument given, print its value if any
		char *value = dict_find(replacements, argv[1]);
		if(value != NULL)
			reply("$b%s$b: %s", argv[1], value);
		else
			reply("Your topicmask has no pattern called $b%s$b that could be replaced.", argv[1]);
		return 0;
	}
	assert_return(argc > 2, 0);
	char *param = untokenize(argc - 2, argv + 2, " ");
	if(topicmask_set(reg, argv[1], param) == 1)
	{
		reply("Your topicmask has no pattern called $b%s$b that could be replaced.", argv[1]);
		return 0;
	}
	reply("$b%s$b: %s", argv[1], param);
	free(param);
	return 1;
}

static void topicmask_chanreg_del_f(struct chanreg *reg)
{
	dict_delete(channel_topics, reg->channel);
}

static void topics_db_read(struct database *db)
{
	dict_iter(node, db->nodes)
	{
		struct db_node *dbnode = node->data;
		// this should be a list of db_nodes itself
		if(dbnode->type != DB_OBJECT)
		{
			log_append(LOG_ERROR, "topics.db: Path %s is not an object.", node->key);
			continue;
		}

		struct dict *replacements;
		if(dict_size(dbnode->data.object))
		{
			// find belonging chanreg
			struct chanreg *reg = chanreg_find(node->key);
			if(reg == NULL)
				continue;

			replacements = topicmask_get_replacements(node->key, 1);

			dict_iter(topic, dbnode->data.object)
			{
				struct db_node *topic_node = topic->data;
				// this should be a string
				if(topic_node->type != DB_STRING)
				{
					log_append(LOG_ERROR, "topics.db: Path %s/%s is not a string.", node->key, topic->key);
					continue;
				}

				char *str = strlen(topic_node->data.string) > 0 ? strdup(topic_node->data.string) : NULL;
				dict_insert(replacements, strdup(topic->key), str);
			}
		}
	}
}

static int topics_db_write(struct database *db)
{
	dict_iter(node, channel_topics)
	{
		database_begin_object(db, node->key);
			dict_iter(subnode, (struct dict*)node->data)
			{
				if(subnode->data != NULL)
					database_write_string(db, subnode->key, (char*)subnode->data);
			}
		database_end_object(db);
	}
	return 0;
}

static int cmod_enabled(struct chanreg *reg, enum cmod_enable_reason reason)
{
	const char *forcetopic = chanreg_setting_get(reg, cmod, "ForceTopic");
	topicmask_parse(reg, NULL, NULL);

	if(forcetopic != NULL && !strcasecmp(forcetopic, "On"))
		topicmask_set_topic(reg);

	return 0;
}

static void topicmask_set_topic(struct chanreg *reg)
{
	const char *topicmask, *pos;
	struct stringbuffer *sbuf;

	assert((topicmask = chanreg_setting_get(reg, cmod, "Topicmask")) != NULL);
	// get replacements dict
	struct dict *replacements = topicmask_get_replacements(reg->channel, 0);
	if(replacements == NULL)
	{
		// Nothing that needs replacing
		irc_send("TOPIC %s :%s", reg->channel, topicmask);
		return;
	}

	sbuf = stringbuffer_create();
	// inlining str_replace
	pos = topicmask;
	while(*pos != '\0')
	{
		// Position and value of next item to be replaced
		const char *minpos = NULL;
		struct dict_node *minnode;
		// Find next item
		dict_iter(node, replacements)
		{
			unsigned char found = 0;
			const char *nextpos = pos;
			while(found == 0 && (nextpos = strcasestr(nextpos, node->key)) != NULL)
			{
				// Valid pattern?
				if(nextpos > topicmask && *(nextpos - 1) == '#' && *(nextpos + strlen(node->key)) == '#'
					&& (minpos == NULL || (nextpos - 1) < minpos))
				{
					minpos = nextpos - 1;
					minnode = node;
					// only find first match as the following ones are not going to precede this one
					found = 1;
				}
				nextpos += strlen(node->key) + 1;
			}
		}

		if(minpos != NULL)
		{
			// we found a replacement to be done, append everything up to here
			stringbuffer_append_string_n(sbuf, pos, minpos - pos);
			// next comes the key, append its value
			if(minnode->data != NULL)
				stringbuffer_append_string(sbuf, (char*)minnode->data);
			else
				stringbuffer_append_char(sbuf, ' ');
			// move pos forward by the length of the key + 2 * '#'
			pos = minpos + strlen(minnode->key) + 2;
		}
		else
		{
			// No more replacements to be done, simply append anything that remained in the original string
			stringbuffer_append_string(sbuf, pos);
			break;
		}
	}

	irc_send("TOPIC %s :%s", reg->channel, sbuf->string);
	// Free memory
	stringbuffer_free(sbuf);
}

static int topicmask_set(struct chanreg *reg, const char *param, const char *value)
{
	const char *topicmask = chanreg_setting_get(reg, cmod, "Topicmask");
	if(topicmask == NULL)
		return 1;

	// Find replacement dict
	struct dict *replacements = dict_find(channel_topics, reg->channel);
	if(replacements == NULL)
		return 1;

	// See if a node with this name already exists
	struct dict_node *node = dict_find_node(replacements, param);
	if(node == NULL)
		return 1;

	if(node->data != NULL)
		free(node->data);
	node->data = strdup(value);

	topicmask_set_topic(reg);
	return 0;
}

static struct dict *topicmask_get_replacements(const char *channel, unsigned char force)
{
	struct dict *replacements = dict_find(channel_topics, channel);
	if(replacements == NULL && force != 0)
	{
		replacements = dict_create();
		dict_set_free_funcs(replacements, free, free);
		dict_insert(channel_topics, strdup(channel), replacements);
	}
	return replacements;
}

static int topicmask_validator(struct chanreg *reg, struct irc_source *src, const char *value)
{
	topicmask_parse(reg, src, value);
	return 1;
}

static void topicmask_parse(struct chanreg *reg, struct irc_source *src, const char *value)
{
	const char *pos;

	if(value == NULL)
	{
		value = chanreg_setting_get(reg, cmod, "Topicmask");
		if(value == NULL)
		{
			log_append(LOG_INFO, "No topicmask found for channel %s in topicmask_parse()", reg->channel);
			return;
		}
	}

	// Make sure there is a replacement dict for this channel
	struct dict *replacements = NULL;
	struct dict_node *replacements_node = dict_find_node(channel_topics, reg->channel);
	if(replacements_node != NULL)
		replacements = replacements_node->data;

	// To make sure nobody can flood the replacements dict with too many entries, I'm only keeping the
	// entries that currently matter, i.e. those that are present in the topic.
	// So let's create a new dict here and only copy existing entries.
	struct dict *new = dict_create();
	dict_set_free_funcs(new, free, free);

	// Search for placeholders
	pos = value;
	while(*pos != '\0' && (pos = strchr(pos, '#')) != NULL)
	{
		++pos;
		if(*pos == '#') // empty pattern
		{
			++pos;
			continue;
		}
		// Find ending #
		char *end = strchr(pos, '#');
		if(end == NULL) // no more delimiters found
		{
			if(src != NULL)
				reply("WARNING: Pattern delimiter at position %td does not seem to be closed.", (pos - 1) - value);
			break;
		}

		// Now, the pattern we want is between pos and end, let's separate it
		char *pattern = strndup(pos, end - pos);
		// Make sure patterns don't get created multiple times when they appear more than once in the same mask
		if(dict_find(new, pattern) == NULL)
		{
			// If we recorded this pattern before, keep it by saving a duplicate
			struct dict_node *node = (replacements == NULL) ? NULL : dict_find_node(replacements, pattern);
			char *replacement_value = (node == NULL || node->data == NULL) ? NULL : strdup(node->data);
			dict_insert(new, strdup(pattern), replacement_value);
		}
		pos = end + 1;
		free(pattern);
	}

	if(replacements_node != NULL)
	{
		replacements_node->data = new;
		assert(replacements != NULL);
		dict_free(replacements);
	}
	else
		dict_insert(channel_topics, strdup(reg->channel), new);
}
