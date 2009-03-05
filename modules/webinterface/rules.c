#include "global.h"
#include "rules.h"
#include "modules/parser/parser.h"
#include "group.h"
#include "chanuser.h"
#include "session.h"

DECLARE_LIST(rule_list, struct rule *)
IMPLEMENT_LIST(rule_list, struct rule *)

struct rule
{
	unsigned int idx;
	char *rule_str;
	struct parser_token_list *rule;
};

static struct parser *parser = NULL;
static struct rule_list *rules;
static unsigned int next_rule_idx = 1;

PARSER_FUNC(group);
PARSER_FUNC(loggedin);
struct rule *rule_get(unsigned int rule_idx);

void rules_init()
{
	rules = rule_list_create();
	parser = parser_create();
	REG_RULE("group", group);
	REG_RULE("loggedin", loggedin);
}

void rules_fini()
{
	rule_unreg("group");
	rule_unreg("loggedin");
	parser_free(parser);
	rule_list_free(rules);
}

void rule_reg(const char *name, parser_func_f *func)
{
	assert(dict_find(parser->funcs, name) == NULL);
	debug("Adding new rule function '%s'", name);
	parser_add_func(parser, name, func);
	// Adding new rules is pretty safe since we try to compile uncompiled rules when needed.
}

void rule_unreg(const char *name)
{
	// By default the parser module has no function list modifiable after parsing so we
	// must delete the function manually and then make sure no previously parsed rule is
	// evaluated after that since it would contain a pointer to a function that might not
	// exist/work anymore.

	debug("Deleting command rule function '%s'", name);
	for(unsigned int i = 0; i < rules->count; i++)
	{
		if(rules->data[i]->rule)
			parser_free_tokens(rules->data[i]->rule);
		rules->data[i]->rule = NULL;
	}

	dict_delete(parser->funcs, name);
}

unsigned int rule_validate(const char *rule)
{
	struct parser_token_list *ptl;

	if(rule == NULL)
		return 0;

	if((ptl = parser_tokenize(parser, rule)) == NULL)
		return 0;

	parser_free_tokens(ptl);
	return 1;
}

unsigned int rule_compile(const char *rule_str)
{
	struct rule *rule;

	assert_return(rule_str, 0);
	rule = malloc(sizeof(struct rule));
	rule->idx = next_rule_idx++;
	rule->rule_str = strdup(rule_str);
	rule->rule = parser_tokenize(parser, rule_str);
	rule_list_add(rules, rule);

	return rule->idx;
}

struct rule *rule_get(unsigned int rule_idx)
{
	assert_return(rule_idx < next_rule_idx, NULL);
	for(unsigned int i = 0; i < rules->count; i++)
	{
		if(rules->data[i]->idx == rule_idx)
			return rules->data[i];
	}

	return NULL;
}

unsigned int rule_executable(unsigned int rule_idx)
{
	struct rule *rule;
	assert_return(rule = rule_get(rule_idx), 0);

	// Rule is not compiled and compilation failed
	if(!rule->rule && !(rule->rule = parser_tokenize(parser, rule->rule_str)))
	{
		log_append(LOG_WARNING, "Rule '%s' is not executable: compilation failed.", rule->rule_str);
		return 0;
	}

	return 1;
}

enum rule_result rule_exec(unsigned int rule_idx, const struct session *session)
{
	struct rule_context ctx;
	struct rule *rule;
	int res;

	assert_return(rule = rule_get(rule_idx), R_ERROR);

	// Rule is not compiled and compilation failed
	if(!rule->rule && !(rule->rule = parser_tokenize(parser, rule->rule_str)))
	{
		log_append(LOG_WARNING, "Could not execute rule '%s'; compilation failed.", rule->rule_str);
		return R_ERROR;
	}

	ctx.session = session;
	res = parser_execute(parser, rule->rule, &ctx);

	if(res == 1)
		return R_ALLOW;
	else if(res == 0)
		return R_DENY;
	else
		return R_ERROR;
}

void rule_free(unsigned int rule_idx)
{
	struct rule *rule;

	assert(rule = rule_get(rule_idx));

	rule_list_del(rules, rule);
	free(rule->rule_str);
	if(rule->rule)
		parser_free_tokens(rule->rule);
	free(rule);
}

PARSER_FUNC(group)
{
	int res;
	struct rule_context *r_ctx = ctx;

	if(arg == NULL)
		return RET_NONE;

	if(r_ctx->session == NULL || r_ctx->session->account == NULL)
		return RET_FALSE;

	res = group_has_member(arg, r_ctx->session->account);

	if(res == 1)
		return RET_TRUE;
	else if(res == 0)
		return RET_FALSE;
	else
		return RET_NONE;
}

PARSER_FUNC(loggedin)
{
	struct rule_context *r_ctx = ctx;

	if(r_ctx->session && r_ctx->session->account)
		return RET_TRUE;
	return RET_FALSE;
}
