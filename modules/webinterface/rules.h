#ifndef RULES_H
#define RULES_H

#include "modules/parser/parser.h"

#define REG_RULE(NAME, FUNC)	rule_reg(NAME, __parser_func_ ## FUNC)

enum rule_result
{
	R_DENY,
	R_ALLOW,
	R_ERROR
};

struct session;
struct rule_context
{
	const struct session *session;
};

void rules_init();
void rules_fini();
void rule_reg(const char *name, parser_func_f *func);
void rule_unreg(const char *name);
unsigned int rule_validate(const char *rule);
unsigned int rule_compile(const char *rule);
unsigned int rule_executable(unsigned int rule_idx);
enum rule_result rule_exec(unsigned int rule_idx, const struct session *session);
void rule_free(unsigned int rule_idx);

#endif
