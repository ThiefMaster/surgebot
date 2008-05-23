#include "global.h"
#include "command_rule.h"
#include "modules/parser/parser.h"
#include "group.h"
#include "chanuser.h"

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
PARSER_FUNC(inchannel);
PARSER_FUNC(channel);
PARSER_FUNC(oped);
PARSER_FUNC(voiced);
struct command_rule *command_rule_get(unsigned int rule_idx);


void command_rule_init()
{
	rules = command_rule_list_create();
	parser = parser_create();
	REG_COMMAND_RULE("group", group);
	REG_COMMAND_RULE("inchannel", inchannel);
	REG_COMMAND_RULE("channel", channel);
	REG_COMMAND_RULE("oped", oped);
	REG_COMMAND_RULE("voiced", voiced);
}

void command_rule_fini()
{
	command_rule_unreg("group");
	command_rule_unreg("inchannel");
	command_rule_unreg("channel");
	command_rule_unreg("oped");
	command_rule_unreg("voiced");
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

enum command_rule_result command_rule_exec(unsigned int rule_idx, const struct irc_source *src, const struct irc_user *user, const struct irc_channel *channel, const char *channelname)
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
	ctx.channel = channel;
	ctx.channelname = channelname;

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

PARSER_FUNC(inchannel)
{
	struct command_rule_context *cr_ctx = ctx;
	struct irc_channel *chan;

	if(!arg)
	{
		if(!cr_ctx->channelname)
			return RET_FALSE;
		else
			arg = cr_ctx->channelname;
	}

	if((chan = channel_find(arg)) && cr_ctx->user && channel_user_find(chan, (struct irc_user *)cr_ctx->user))
		return RET_TRUE;

	return RET_FALSE;
}

PARSER_FUNC(channel)
{
	struct command_rule_context *cr_ctx = ctx;

	if(!arg || !cr_ctx->channelname)
		return RET_FALSE;

	if(!strcasecmp(cr_ctx->channelname, arg))
		return RET_TRUE;

	return RET_FALSE;
}

PARSER_FUNC(oped)
{
	struct command_rule_context *cr_ctx = ctx;
	struct irc_channel *chan;
	struct irc_chanuser *chanuser;
	
	if(!arg)
	{
		if(!cr_ctx->channelname)
			return RET_FALSE;
		else
			arg = cr_ctx->channelname;
	}
	
	if((chan = channel_find(arg)) && cr_ctx->user && (chanuser = channel_user_find(chan, (struct irc_user *)cr_ctx->user)) && (chanuser->flags & MODE_OP))
		return RET_TRUE;
	
	return RET_FALSE;
}

PARSER_FUNC(voiced)
{
	struct command_rule_context *cr_ctx = ctx;
	struct irc_channel *chan;
	struct irc_chanuser *chanuser;
	
	if(!arg)
	{
		if(!cr_ctx->channelname)
			return RET_FALSE;
		else
			arg = cr_ctx->channelname;
	}
	
	if((chan = channel_find(arg)) && cr_ctx->user && (chanuser = channel_user_find(chan, (struct irc_user *)cr_ctx->user)) && (chanuser->flags & MODE_VOICE))
		return RET_TRUE;
	
	return RET_FALSE;
}
