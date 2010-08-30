#include <stdlib.h>
#include <ctype.h>
#include "global.h"
#include "modules/trivia/trivia.h"
#include "modules/commands/commands.h"
#include "dict.h"
#include "stringlist.h"
#include "module.h"
#include "irc.h"

#define min(a,b) ((a) < (b) ? (a) : (b))

MODULE_DEPENDS("commands", NULL);

COMMAND(readfile);
COMMAND(show_questions);

MODULE_INIT
{
	trivia_dict = dict_create();
	trivia_channels = dict_create();
	dict_set_free_funcs(trivia_dict, NULL, (dict_free_f*)trivia_file_free);
	dict_set_free_funcs(trivia_channels, NULL, (dict_free_f*)trivia_channel_free);
	DEFINE_COMMAND(self, "readfile", readfile, 1, 0, "group(admins)");
	DEFINE_COMMAND(self, "questions", show_questions, 0, 0, "group(admins)");
}

MODULE_FINI
{
	dict_free(trivia_channels);
	dict_free(trivia_dict);
}

COMMAND(readfile)
{
	struct trivia_file *tf = trivia_file_create(argv[1]);
	int count = trivia_read_questions(tf);
	reply("Read %d question(s).", count);
	return 0;
}

COMMAND(show_questions)
{
	if(!trivia_dict->count)
	{
		reply("No trivia files loaded.");
	}
	else
	{
		dict_iter(node, trivia_dict)
		{
			struct trivia_file *tf = node->data;
			reply("$uTrivia: $b%s$b$u", tf->name);
			dict_iter(tq_node, tf->questions)
			{
				struct trivia_question *tq = tq_node->data;
				if(tq->question)
				{
					reply("    Question: \"%s\"", tq->question);
					if(tq->answers->count)
					{
						for(unsigned int i = 0; i < tq->answers->count; i++)
							reply("      Answer: \"%s\"", tq->answers->data[i]);
					}
					else
						reply("This question does not seem to have any answers...");
				}
			}
		}
	}
	return 0;
}

struct trivia_file *trivia_file_create(const char *name)
{
	debug("Creating new trivia file '%s'", name);
	struct trivia_file *tf = malloc(sizeof(struct trivia_file));
	tf->name = strdup(name);
	tf->questions = dict_create();
	dict_insert(trivia_dict, tf->name, tf);
	return tf;
}

struct trivia_file *trivia_file_find(const char *name)
{
	return dict_find(trivia_dict, name);
}

void trivia_file_free(struct trivia_file *tf)
{
	assert(tf);
	dict_iter(node, tf->questions)
	{
		dict_delete_node(tf->questions, node);
		// Free questions
		struct trivia_question *tq = node->data;
		if(tq->answers)
			stringlist_free(tq->answers);
		if(tq->hints)
			stringlist_free(tq->hints);
		free(tq->question);
		free(tq);
	}
	dict_delete(trivia_dict, tf->name);
	free(tf->name);
	free(tf);
}

struct trivia_question *trivia_question_create(struct trivia_file *tf, const char *question)
{
	assert_return(tf && !dict_find(tf->questions, question), NULL);

	struct trivia_question *tq = malloc(sizeof(struct trivia_question));
	tq->question = strdup(question);
	tq->answers = stringlist_create();
	tq->hints = stringlist_create();
	dict_insert(tf->questions, tq->question, tq);
	return tq;
}

static void trivia_question_answer_add(struct trivia_question *tq, char *answer)
{
	stringlist_add(tq->answers, answer);
}

static void trivia_question_hint_add(struct trivia_question *tq, char *hint)
{
	stringlist_add(tq->hints, hint);
}

void trivia_question_free(struct trivia_question *tq)
{
	stringlist_free(tq->answers);
	stringlist_free(tq->hints);
	free(tq->question);
	free(tq);
}

struct trivia_channel *trivia_channel_create(char *name)
{
	struct trivia_channel *tc = malloc(sizeof(struct trivia_channel));

	tc->name = strdup(name);
	tc->tf = NULL;
	tc->users = dict_create();
	tc->current_question = NULL;
	tc->question_count = 0;
	tc->hint_count = 0;
	tc->last_question = 0;
	dict_set_free_funcs(tc->users, NULL, (dict_free_f*)trivia_channel_free);
	return tc;
}

static void trivia_channel_zero(struct trivia_channel *tc)
{
	// Delete all users from the channel
	dict_clear(tc->users);
	tc->question_count = tc->hint_count = 0;
}

static void trivia_channel_free(struct trivia_channel *tc)
{
	dict_free(tc->users);
	free(tc->name);
}

void trivia_user_zero(struct trivia_user *tu)
{
	tu->question_count = 0;
}

void trivia_user_free(struct trivia_user *tu)
{
	free(tu);
}

void trivia_answer_add(struct trivia_user *tu)
{
	tu->question_count++;
}

static int trivia_read_questions(struct trivia_file *tf)
{
	FILE *triFile;
	char *filename, fileBuffer[1024] = {0};
	int linecount = 0, len, question_count = 0;
	struct trivia_question *tq = NULL;

	filename = malloc(strlen(tf->name) + strlen(TRIVIA_FILE_FORMAT) - 1); // strlen("%s") - strlen("\0")
	sprintf(filename, TRIVIA_FILE_FORMAT, tf->name);

	triFile = fopen(filename, "r");
	while(fgets(fileBuffer, 1024, triFile))
	{
		linecount++;
		len = strlen(fileBuffer) - 1; // fgets() reads in a newline at the end
		if(len == 0)
		{
			if(tq)
			{
				tq = NULL;
			}
			continue;
		}

		if(fileBuffer[len] != '\n')
		{
			debug("%s: Line %d is too long (> 1024 chars), skipping.", filename, linecount);
			while(fgets(fileBuffer, 1024, triFile))
			{
				if(fileBuffer[strlen(fileBuffer) - 1] == '\n')
					break;
			}
			continue;
		}

		char *str = fileBuffer;
		// Strip trailing spaces
		while(isspace(*str))
			str++, len--;
		while(isspace(str[len]))
			len--;
		str[len + 1] = '\0';

		if(!len)
		{
			debug("%s: Empty line %d.", filename, linecount);
			continue;
		}

		if(!tq)
		{
			tq = trivia_question_create(tf, str);
			question_count++;
			continue;
		}
		if(!tq->question)
		{
			tq->question = strdup(str);
		}
		else if(!strncasecmp(str, "Answer:", 7))
		{
			if(len <= 8)
			{
				debug("%s: Empty answer on line %d, skipping.", filename, linecount);
				continue;
			}
			trivia_question_answer_add(tq, str + 8);
		}
		else if(!strncasecmp(str, "Hint:", 5))
		{
			if(len <= 6)
			{
				debug("%s: Empty hint on line %d, skipping.", filename, linecount);
				continue;
			}
			trivia_question_hint_add(tq, str + 6);
		}
		else
		{
			debug("%s: Line %d seems to be invalid.", filename, linecount);
			continue;
		}
	}
	fclose(triFile);
	MyFree(filename);
	return question_count;
}

void trivia_ask_question(struct trivia_channel *tc)
{
	tc->question_count++;
	tc->hint_count = 0;
	tc->last_question = now;
}

void trivia_show_hint(struct trivia_channel *tc)
{
	tc->hint_count++;
}

void trivia_solve(struct trivia_channel *tc, struct trivia_user *tu)
{
	if(!tu)
	{
		// This means no user knew the answer and the question is automatically solved
		irc_send("PRIVMSG %s :Question %d automatically solved after %s.", tc->name, tc->question_count, duration2string(now - tc->last_question));
	}
	else
	{
		// Somebody gave the right answer, let's solve it!
		tu->question_count++;
	}

	if(tu->question_count == min(30, tc->tf->questions->count))
		trivia_end(tc, tu);

	tc->hint_count = 0;
	tc->current_question = NULL;
}

static void trivia_end(struct trivia_channel *tc, struct trivia_user *tu)
{
	// Output stuff
	trivia_channel_zero(tc);
}
