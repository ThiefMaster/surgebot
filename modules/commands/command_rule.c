#include "global.h"
#include "command_rule.h"
#include "modules/parser/parser.h"
#include "group.h"

static struct parser *parser = NULL;

PARSER_FUNC(group);


void command_rule_init()
{
	parser = parser_create();
	ADD_PARSER_FUNC(parser, "group", group);
}

void command_rule_fini()
{
	parser_free(parser);
}

unsigned int command_rule_validate(const char *rule)
{
	struct parser_token_list *ptl;

	if(rule == NULL)
		return 0;

	if((ptl = parser_tokenize(parser, rule)) == NULL)
		return 0;

	parser_free_tokens(ptl);
	return 1;
}

command_rule *command_rule_compile(const char *rule)
{
	assert_return(rule, NULL);
	return parser_tokenize(parser, rule);
}

enum command_rule_result command_rule_exec(const command_rule *rule, const struct irc_source *src, const struct irc_user *user)
{
	struct command_rule_context ctx;
	int res;

	assert_return(rule, CR_ERROR);

	ctx.src     = src;
	ctx.user    = user;

	res = parser_execute(parser, rule, &ctx);

	if(res == 1)
		return CR_ALLOW;
	else if(res == 0)
		return CR_DENY;
	else
		return CR_ERROR;
}

void command_rule_free(command_rule *rule)
{
	parser_free_tokens(rule);
}

PARSER_FUNC(group)
{
	int res;
	struct command_rule_context *cr_ctx = ctx;

	if(arg == NULL)
		return RET_NONE;

	if(cr_ctx->user == NULL || cr_ctx->user->account == NULL)
		return RET_FALSE;

	res = group_has_member(arg, cr_ctx->user->account);

	if(res == 1)
		return RET_TRUE;
	else if(res == 0)
		return RET_FALSE;
	else
		return RET_NONE;
}
