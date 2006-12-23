#include "global.h"
#include "module.h"
#include "parser.h"

MODULE_DEPENDS(NULL);

IMPLEMENT_LIST(parser_token_list, struct parser_token *)

static void parser_error(struct parser *parser, const char *fmt, ...);
static enum token_type add_token(struct parser_token_list *tokens, enum token_type type, void *data);
static struct parser_func_token *make_func_token(const char *name, parser_func_f *func);
static pf_retval parser_eval_binary(struct parser *parser, pf_retval lhs, pf_retval rhs, enum token_type op);
static pf_retval parser_eval_func(struct parser *parser, struct parser_func_token *ftok, void *ctx);
static int parser_execute_recursive(struct parser *parser, const struct parser_token_list *tokens, void *ctx, int *pos_ptr);

static const char *token_names[] = {
	"T_NONE",
	"T_OPEN_PAREN",
	"T_CLOSE_PAREN",
	"T_AND",
	"T_OR",
	"T_NOT",
	"T_FALSE",
	"T_TRUE",
	"T_FUNC"
};


MODULE_INIT
{

}

MODULE_FINI
{

}

struct parser *parser_create()
{
	struct parser *parser = malloc(sizeof(struct parser));
	memset(parser, 0, sizeof(struct parser));
	parser->funcs = dict_create();
	dict_set_free_funcs(parser->funcs, free, NULL);

	return parser;
}

void parser_free(struct parser *parser)
{
	dict_free(parser->funcs);
	free(parser);
}

void parser_add_func(struct parser *parser, const char *name, parser_func_f *func)
{
	assert(strcasecmp(name, "true"));
	assert(strcasecmp(name, "false"));

	dict_insert(parser->funcs, strdup(name), func);
}

static void parser_error(struct parser *parser, const char *fmt, ...)
{
	va_list args;

	if(fmt == NULL) // Clear error
	{
		memset(parser->error, 0, sizeof(parser->error));
		return;
	}

	va_start(args, fmt);
	vsnprintf(parser->error, sizeof(parser->error), fmt, args);
	va_end(args);
	log_append(LOG_WARNING, "Parser error: %s", parser->error);
}

static enum token_type add_token(struct parser_token_list *tokens, enum token_type type, void *data)
{
	struct parser_token *tok = malloc(sizeof(struct parser_token));
	tok->type = type;
	tok->data = data;
	parser_token_list_add(tokens, tok);
	return tok->type;
}

static struct parser_func_token *make_func_token(const char *name, parser_func_f *func)
{
	struct parser_func_token *ftok = malloc(sizeof(struct parser_func_token));
	ftok->name = strdup(name);
	ftok->arg = stringbuffer_create();
	ftok->func = func;
	return ftok;
}

struct parser_token_list *parser_tokenize(struct parser *parser, const char *string)
{
	struct parser_token_list *tokens = parser_token_list_create();
	struct parser_func_token *func_token = NULL;
	parser_func_f *func;
	enum token_type last_token = T_NONE;
	unsigned int open_parens = 0, func_arg = 0;
	const char *c = string;
	char *str, *pos;

	parser_error(parser, NULL);

	while(*c)
	{
		if(*c == '(')
		{
			if(func_token && !func_arg) // Beginning of function argument
			{
				func_arg = 1;
			}
			else if(func_token && func_arg) // '(' is not allowed inside a function argument
			{
				parser_error(parser, "Unexpected opening parenthesis in function argument");
				goto error;
			}
			else
			{
				if(last_token == T_FUNC || last_token == T_TRUE || last_token == T_FALSE || last_token == T_CLOSE_PAREN)
				{
					parser_error(parser, "Unexpected token %s after %s", token_names[T_OPEN_PAREN], token_names[last_token]);
					goto error;
				}

				last_token = add_token(tokens, T_OPEN_PAREN, NULL);
			}

			open_parens++;
			c++;
		}
		else if(*c == ')')
		{
			if(!open_parens) // We can't close more parenthesis than we opened
			{
				parser_error(parser, "Unexpected closing parenthesis at: %s", c);
				goto error;
			}

			if(func_arg) // End of function argument
			{
				if(func_token->arg->len)
				{
					while(isspace(func_token->arg->string[func_token->arg->len - 1]))
						func_token->arg->string[--func_token->arg->len] = '\0';
				}

				func_arg = 0;
				func_token = NULL;
			}
			else
			{
				if(last_token == T_OPEN_PAREN || last_token == T_AND || last_token == T_OR || last_token == T_NOT)
				{
					parser_error(parser, "Unexpected token %s after %s", token_names[T_CLOSE_PAREN], token_names[last_token]);
					goto error;
				}

				last_token = add_token(tokens, T_CLOSE_PAREN, NULL);
			}

			open_parens--;
			c++;
		}
		else if(func_token && func_arg) // Inside function argument -> add whatever we get
		{
			if(func_token->arg->len || !isspace(*c)) // leading whitespace
				stringbuffer_append_char(func_token->arg, *c);
			c++;
		}
		else if(*c == '&' && *c == *(c+1)) // && -> AND
		{
			if(last_token != T_FUNC && last_token != T_FALSE && last_token != T_TRUE && last_token != T_CLOSE_PAREN)
			{
				parser_error(parser, "Unexpected token %s after %s", token_names[T_AND], token_names[last_token]);
				goto error;
			}

			last_token = add_token(tokens, T_AND, NULL);
			c += 2;
		}
		else if(*c == '|' && *c == *(c+1)) // || -> OR
		{
			if(last_token != T_FUNC && last_token != T_FALSE && last_token != T_TRUE && last_token != T_CLOSE_PAREN)
			{
				parser_error(parser, "Unexpected token %s after %s", token_names[T_OR], token_names[last_token]);
				goto error;
			}

			last_token = add_token(tokens, T_OR, NULL);
			c += 2;
		}
		else if(*c == '!') // NOT
		{
			if(last_token != T_NONE && last_token != T_AND && last_token != T_OR && last_token != T_OPEN_PAREN)
			{
				parser_error(parser, "Unexpected token %s after %s", token_names[T_NOT], token_names[last_token]);
				goto error;
			}

			last_token = add_token(tokens, T_NOT, NULL);
			c++;
		}
		else if(isspace(*c)) // Skip whitespace
		{
			c++;
		}
		else if(func_token == NULL)
		{
			if(last_token == T_FUNC || last_token == T_TRUE || last_token == T_FALSE || last_token == T_CLOSE_PAREN)
			{
				parser_error(parser, "Unexpected token T_FUNC/T_TRUE/T_FALSE after %s", token_names[last_token]);
				goto error;
			}

			str = strdup(c);
			if((pos = strpbrk(str, " ()&|\t")))
				*pos = '\0';

			if(!strcasecmp(str, "true")) // Literal true?
			{
				last_token = add_token(tokens, T_TRUE, NULL);
				c += 4;
				free(str);
			}
			else if(!strcasecmp(str, "false")) // Literal false?
			{
				last_token = add_token(tokens, T_FALSE, NULL);
				c += 5;
				free(str);
			}
			else if(pos && (func = dict_find(parser->funcs, str))) // Beginning of a function?
			{
				func_token = make_func_token(str, func);
				last_token = add_token(tokens, T_FUNC, func_token);
				c += strlen(str);
				free(str);
			}
			else // No? Then we have something we don't know about
			{
				parser_error(parser, "Unexpected: %s", c);
				free(str);
				goto error;
			}
		}
	}

	if(open_parens)
	{
		parser_error(parser, "%d unclosed parenthesises in: %s", open_parens, string);
		goto error;
	}
	else if(last_token == T_NOT || last_token == T_AND || last_token == T_OR)
	{
		parser_error(parser, "Unexpected last token %s", token_names[last_token]);
		goto error;
	}

	return tokens;

error:
	parser_free_tokens(tokens);
	return NULL;
}

void parser_free_tokens(struct parser_token_list *tokens)
{
	for(int i = 0; i < tokens->count; i++)
	{
		if(tokens->data[i]->type == T_FUNC)
		{
			struct parser_func_token *ftok = tokens->data[i]->data;
			free(ftok->name);
			stringbuffer_free(ftok->arg);
			free(ftok);
		}

		free(tokens->data[i]);
	}

	parser_token_list_free(tokens);
}

static pf_retval parser_eval_binary(struct parser *parser, pf_retval lhs, pf_retval rhs, enum token_type op)
{
	pf_retval res = RET_NONE;

	if(op == T_AND)
		res = ((lhs == RET_TRUE) && (rhs == RET_TRUE)) ? RET_TRUE : RET_FALSE;
	else if(op == T_OR)
		res = ((lhs == RET_TRUE) || (rhs == RET_TRUE)) ? RET_TRUE : RET_FALSE;
	else
		parser_error(parser, "Unexpected op token: %s", token_names[op]);

	return res;
}

static pf_retval parser_eval_func(struct parser *parser, struct parser_func_token *ftok, void *ctx)
{
	return ftok->func(ctx, ftok->name, (ftok->arg->len ? ftok->arg->string : NULL));
}

int parser_execute(struct parser *parser, const struct parser_token_list *tokens, void *ctx)
{
	int pos_ptr = 0;

	return parser_execute_recursive(parser, tokens, ctx, &pos_ptr);
}

static int parser_execute_recursive(struct parser *parser, const struct parser_token_list *tokens, void *ctx, int *pos_ptr)
{
	pf_retval tmp, lhs, rhs;
	unsigned char neg = 0;
	enum token_type op = T_NONE;
	tmp = lhs = rhs = RET_NONE;

	assert_return(tokens, -1);
	assert_return(tokens->count, -2);

	parser_error(parser, NULL);

#define eval_ready()	(lhs != RET_NONE && rhs != RET_NONE && op != T_NONE)
	while(*pos_ptr < tokens->count)
	{
		struct parser_token *token = tokens->data[*pos_ptr];
		(*pos_ptr)++;
		tmp = RET_NONE;

		// If we have all data to execute a binary expression, do it
		if(eval_ready())
		{
			lhs = parser_eval_binary(parser, lhs, rhs, op);
			rhs = RET_NONE;
			op = T_NONE;
		}

		// Check tokens
		if(token->type == T_OPEN_PAREN) // Opening parenthesis -> recursive parsing
		{
			tmp = parser_execute_recursive(parser, tokens, ctx, pos_ptr);
			if(tmp != RET_TRUE && tmp != RET_FALSE)
				return tmp;
		}
		else if(token->type == T_CLOSE_PAREN) // Closing parenthesis -> up a level
		{
			if(eval_ready())
				lhs = parser_eval_binary(parser, lhs, rhs, op);
			return lhs;
		}
		else if(token->type == T_TRUE) // Literal true
		{
			tmp = RET_TRUE;
		}
		else if(token->type == T_FALSE) // Literal false
		{
			tmp = RET_FALSE;
		}
		else if(token->type == T_FUNC)
		{
			// If the left side is false (true) and we are doing an AND (OR) comparison there is no need to check the right side
			if(lhs == RET_FALSE && op == T_AND)
				tmp = RET_FALSE;
			else if(lhs == RET_TRUE && op == T_OR)
				tmp = RET_TRUE;
			else // Call function
				tmp = parser_eval_func(parser, token->data, ctx);
		}
		else if(token->type == T_NOT) // Negation
		{
			neg = !neg;
			continue;
		}
		else if(token->type == T_AND || token->type == T_OR) // AND/OR operator
		{
			op = token->type;
			continue;
		}
		else // Something we shouldn't have...
		{
			parser_error(parser, "Unexpected token: %s", token_names[token->type]);
			return -3;
		}

		// At this position we have a temporary result

		if(tmp == RET_NONE) // Something failed
		{
			if(strlen(parser->error) == 0) // No error set -> set a default one
				parser_error(parser, "Invalid result after token %s", token_names[token->type]);
			return -4;
		}

		assert_return(tmp == RET_TRUE || tmp == RET_FALSE, -4);

		if(neg)
		{
			tmp = (tmp == RET_FALSE) ? RET_TRUE : RET_FALSE;
			neg = 0;
		}

		if(lhs == RET_NONE)
			lhs = tmp;
		else if(rhs == RET_NONE)
			rhs = tmp;
	}

	// We might have a complete binary expression here if it was completed with the last token
	if(eval_ready())
		lhs = parser_eval_binary(parser, lhs, rhs, op);

	return lhs;

#undef eval_ready
}

