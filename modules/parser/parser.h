#ifndef PARSER_H
#define PARSER_H

#include "list.h"
#include "stringbuffer.h"

#define PARSER_FUNC(NAME)	static enum parser_func_retval __parser_func_ ## NAME(void *ctx, const char *name, const char *arg)
typedef enum parser_func_retval (parser_func_f)(void *ctx, const char *name, const char *arg);
#define ADD_PARSER_FUNC(PARSER, NAME, FUNC)	parser_add_func(PARSER, NAME, __parser_func_ ## FUNC);

enum token_type
{
	T_NONE,
	T_OPEN_PAREN,
	T_CLOSE_PAREN,
	T_AND,
	T_OR,
	T_NOT,
	T_FALSE,
	T_TRUE,
	T_FUNC
};

typedef enum parser_func_retval
{
	RET_FALSE,
	RET_TRUE,
	RET_NONE
} pf_retval;

struct parser
{
	char		error[MAXLEN];
	struct dict	*funcs;
};

struct parser_func_token
{
	char		*name;
	struct stringbuffer	*arg;
	parser_func_f	*func;
};

struct parser_token
{
	enum token_type	type;
	void		*data;
};

DECLARE_LIST(parser_token_list, struct parser_token *)

struct parser *parser_create();
void parser_free(struct parser *parser);
void parser_add_func(struct parser *parser, const char *name, parser_func_f *func);
struct parser_token_list *parser_tokenize(struct parser *parser, const char *string);
void parser_free_tokens(struct parser_token_list *parser_tokenize);
int parser_execute(struct parser *parser, const struct parser_token_list *tokens, void *ctx);

#endif
