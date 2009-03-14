#ifndef __TRIVIA_H__
#define __TRIVIA_H__

#define TRIVIA_FILE_FORMAT "trivia_%s.txt"

struct trivia_file
{
	char *name;
	struct dict *questions;
};

struct trivia_question
{
	char *question;
	struct stringlist *answers;
	struct stringlist *hints;
};

struct trivia_channel
{
	char *name;
	struct trivia_file *tf;
	struct dict *users;

	struct trivia_question *current_question;
	time_t last_question;
	unsigned int question_count;
	unsigned int hint_count;
};

struct trivia_user
{
	char *name;
	struct trivia_channel *channel;
	struct irc_user *user;

	unsigned int question_count;
};

extern time_t now;

static struct dict *trivia_dict;
static struct dict *trivia_channels;

//static void trivia_write();
//static void trivia_read();

static struct trivia_file *trivia_file_create(const char *);
struct trivia_file *trivia_file_find(const char *name);
static void trivia_file_free(struct trivia_file *);

static struct trivia_question *trivia_question_create(struct trivia_file *, const char *);
static void trivia_question_answer_add(struct trivia_question *, char *);
static void trivia_question_hint_add(struct trivia_question *, char *);
void trivia_question_free(struct trivia_question *);

struct trivia_channel *trivia_channel_create(char *name);
static void trivia_channel_zero(struct trivia_channel *);
static void trivia_channel_free(struct trivia_channel *);

void trivia_user_zero(struct trivia_user *);
void trivia_user_free(struct trivia_user *);
void trivia_answer_add(struct trivia_user *);

static int trivia_read_questions(struct trivia_file *);

void trivia_ask_question(struct trivia_channel *);
void trivia_show_hint(struct trivia_channel *);
void trivia_solve(struct trivia_channel *, struct trivia_user *);
static void trivia_end(struct trivia_channel *, struct trivia_user *);

#endif
