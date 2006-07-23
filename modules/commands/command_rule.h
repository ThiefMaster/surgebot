#ifndef COMMAND_RULE_H
#define COMMAND_RULE_H

#include "modules/parser/parser.h"

typedef struct parser_token_list command_rule;

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
unsigned int command_rule_validate(const char *rule);
command_rule *command_rule_compile(const char *rule);
enum command_rule_result command_rule_exec(const command_rule *rule, const struct irc_source *src, const struct irc_user *user);
void command_rule_free(command_rule *rule);

#endif
