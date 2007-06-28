#ifndef POLICER_H
#define POLICER_H

#include <time.h>

struct policer_params
{
	double bucket_size;
	double drain_rate;
};

struct policer_params *policer_params_create(double bucket_size, double drain_rate);
void policer_params_free(struct policer_params *params);

struct policer
{
	double level;
	time_t last_req;

	struct policer_params *params;
};

struct policer *policer_create(struct policer_params *params);
unsigned char policer_conforms(struct policer *pol, time_t reqtime, double weight);
void policer_free(struct policer* pol);

#endif
