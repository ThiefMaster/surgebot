#ifndef COMMAND_RULE_H
#define COMMAND_RULE_H

#include "modules/parser/parser.h"

typedef struct parser_token_list command_rule;

#define REG_COMMAND_RULE(NAME, FUNC)	command_rule_reg(NAME, __parser_func_ ## FUNC)

enum command_rule_result
{
	CR_DENY,
	CR_ALLOW,
	CR_ERROR
};

struct command_rule_context
{
	const struct irc_source		*src;
	const struct irc_user		*user;
};

void command_rule_init();
void command_rule_fini();
void command_rule_reg(const char *name, parser_func_f *func);
void command_rule_unreg(const char *name);
unsigned int command_rule_validate(const char *rule);
unsigned int command_rule_compile(const char *rule);
unsigned int command_rule_executable(unsigned int rule_idx);
enum command_rule_result command_rule_exec(unsigned int rule_idx, const struct irc_source *src, const struct irc_user *user);
void command_rule_free(unsigned int rule_idx);

#endif
