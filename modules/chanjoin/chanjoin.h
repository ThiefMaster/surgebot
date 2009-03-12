#ifndef CHANJOIN_H
#define CHANJOIN_H

struct cj_channel;
typedef void (chanjoin_success_f)(struct cj_channel *chan, const char *key, void *ctx, unsigned int first_time);
typedef void (chanjoin_error_f)(struct cj_channel *chan, const char *key, void *ctx, const char *reason);
typedef void (chanjoin_free_ctx_f)(void *);

enum cj_state
{
	CJ_NONE,
	CJ_JOIN_PENDING,
	CJ_JOINED,
	CJ_REJOIN_PENDING,
	CJ_INACTIVE,
	CJ_JOIN_LATER // Join when our perform hook got called
};

DECLARE_LIST(cj_channel_ref_list, struct cj_channel_ref *)

// Channel-wide data
struct cj_channel
{
	char *name;
	struct irc_channel *channel;
	enum cj_state state;
	unsigned int tries;

	struct cj_channel_ref_list *refs;
};

// Channel data related to a single channel reference
struct cj_channel_ref
{
	struct module *module;
	char *module_name;
	char *key;

	chanjoin_success_f *success_func;
	chanjoin_error_f *error_func;
	chanjoin_free_ctx_f *ctx_free_func;

	unsigned int module_reloaded;
	void *ctx;
};

void chanjoin_addchan(const char *name, struct module *module, const char *key, chanjoin_success_f *success_func, chanjoin_error_f *error_func, void *ctx, chanjoin_free_ctx_f *ctx_free_func);
void chanjoin_delchan(const char *name, struct module *module, const char *key);
struct cj_channel *chanjoin_find(const char *name);
struct cj_channel_ref *chanjoin_ref_find(struct cj_channel *chan, struct module *module, const char *key);

#endif

