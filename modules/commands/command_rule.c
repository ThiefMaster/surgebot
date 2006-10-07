#include "global.h"
#include "command_rule.h"
#include "modules/parser/parser.h"
#include "group.h"

DECLARE_LIST(command_rule_list, struct command_rule *)
IMPLEMENT_LIST(command_rule_list, struct command_rule *)

struct command_rule
{
	unsigned int	idx;
	char		*rule_str;
	struct parser_token_list	*rule;
};

static struct parser *parser = NULL;
static struct command_rule_list *rules;
static unsigned int next_rule_idx = 1;


PARSER_FUNC(group);
struct command_rule *command_rule_get(unsigned int rule_idx);


void command_rule_init()
{
	rules = command_rule_list_create();
	parser = parser_create();
	REG_COMMAND_RULE("group", group);
}

void command_rule_fini()
{
	command_rule_unreg("group");
	parser_free(parser);
	command_rule_list_free(rules);
}

void command_rule_reg(const char *name, parser_func_f *func)
{
	assert(dict_find(parser->funcs, name) == NULL);
	debug("Adding new command rule function '%s'", name);
	parser_add_func(parser, name, func);
	// Adding new rules is pretty safe since we try to compile uncompiled rules when needed.
}

void command_rule_unreg(const char *name)
{
	// By default the parser module has no function list modifiable after parsing so we
	// must delete the function manually and then make sure no previously parsed rule is
	// evaluated after that since it would contain a pointer to a function that might not
	// exist/work anymore.

	debug("Deleting command rule function '%s'", name);
	for(int i = 0; i < rules->count; i++)
	{
		if(rules->data[i]->rule)
			parser_free_tokens(rules->data[i]->rule);
		rules->data[i]->rule = NULL;
	}

	dict_delete(parser->funcs, name);
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

unsigned int command_rule_compile(const char *rule)
{
	struct command_rule *crule;

	assert_return(rule, 0);
	crule = malloc(sizeof(struct command_rule));
	crule->idx = next_rule_idx++;
	crule->rule_str = strdup(rule);
	crule->rule = parser_tokenize(parser, rule);
	command_rule_list_add(rules, crule);

	return crule->idx;
}

struct command_rule *command_rule_get(unsigned int rule_idx)
{
	assert_return(rule_idx < next_rule_idx, NULL);
	for(int i = 0; i < rules->count; i++)
	{
		if(rules->data[i]->idx == rule_idx)
			return rules->data[i];
	}

	return NULL;
}

unsigned int command_rule_executable(unsigned int rule_idx)
{
	struct command_rule *crule;
	assert_return(crule = command_rule_get(rule_idx), 0);

	// Rule is not compiled and compilation failed
	if(!crule->rule && !(crule->rule = parser_tokenize(parser, crule->rule_str)))
	{
		log_append(LOG_WARNING, "Rule '%s' is not executable: compilation failed.", crule->rule_str);
		return 0;
	}

	return 1;
}

enum command_rule_result command_rule_exec(unsigned int rule_idx, const struct irc_source *src, const struct irc_user *user)
{
	struct command_rule_context ctx;
	struct command_rule *crule;
	int res;

	assert_return(crule = command_rule_get(rule_idx), CR_ERROR);

	// Rule is not compiled and compilation failed
	if(!crule->rule && !(crule->rule = parser_tokenize(parser, crule->rule_str)))
	{
		log_append(LOG_WARNING, "Could not execute rule '%s'; compilation failed.", crule->rule_str);
		return CR_ERROR;
	}

	ctx.src     = src;
	ctx.user    = user;

	res = parser_execute(parser, crule->rule, &ctx);

	if(res == 1)
		return CR_ALLOW;
	else if(res == 0)
		return CR_DENY;
	else
		return CR_ERROR;
}

void command_rule_free(unsigned int rule_idx)
{
	struct command_rule *crule;

	assert(crule = command_rule_get(rule_idx));

	command_rule_list_del(rules, crule);
	free(crule->rule_str);
	if(crule->rule)
		parser_free_tokens(crule->rule);
	free(crule);
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
