#include <stdlib.h>
#include <string.h>
#include "policer.h"
#include "global.h"

extern time_t now;

struct policer_params *policer_params_create(double bucket_size, double drain_rate)
{
	struct policer_params *params = malloc(sizeof(struct policer_params));
	memset(params, 0, sizeof(struct policer_params));
	params->bucket_size = bucket_size;
	params->drain_rate = drain_rate;
	return params;
}

void policer_params_free(struct policer_params *params)
{
	if(params)
		free(params);
}

struct policer *policer_create(struct policer_params *params)
{
	struct policer *pol = malloc(sizeof(struct policer));
	memset(pol, 0, sizeof(struct policer));
	pol->params = params;
	pol->last_req = now;
	return pol;
}

void policer_free(struct policer* policer)
{
	free(policer);
}

unsigned char policer_conforms(struct policer *pol, time_t reqtime, double weight)
{
	int ret;
	pol->level -= pol->params->drain_rate * (reqtime - pol->last_req);
	if(pol->level < 0.0) pol->level = 0.0;
	ret = pol->level < pol->params->bucket_size;
	pol->level += weight;
	pol->last_req = reqtime;
	return ret;
}
