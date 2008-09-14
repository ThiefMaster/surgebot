#ifndef __HOOK_H__
#define __HOOK_H__

#define DECLARE_HOOKABLE(NAME, ARGS) \
	typedef void (*NAME##_f) ARGS; \
	void reg_##NAME##_hook(NAME##_f func); \
	void unreg_##NAME##_hook(NAME##_f func)

#define IMPLEMENT_HOOKABLE(NAME) \
	static NAME##_f *NAME##_hooks; \
	static unsigned int NAME##_hooks_used = 0, NAME##_hooks_size = 0; \
	void reg_##NAME##_hook(NAME##_f func) { \
		if (NAME##_hooks_used == NAME##_hooks_size) { \
			if (!NAME##_hooks_size) { \
				NAME##_hooks_size = 4; \
				NAME##_hooks = malloc(NAME##_hooks_size * sizeof(NAME##_hooks[0])); \
			} else { \
				NAME##_hooks_size <<= 1; \
				NAME##_hooks = realloc(NAME##_hooks, NAME##_hooks_size * sizeof(NAME##_hooks[0])); \
			} \
		} \
		NAME##_hooks[NAME##_hooks_used++] = func; \
	} \
	void unreg_##NAME##_hook(NAME##_f func) { \
		for (unsigned int i = 0; i < NAME##_hooks_used; i++) { \
			if (NAME##_hooks[i] == func) { \
				memmove(NAME##_hooks+i, NAME##_hooks+i+1, (NAME##_hooks_size-i-1)*sizeof(NAME##_hooks[0])); \
				NAME##_hooks_used--; \
				break; \
			} \
		} \
	} \
	static void clear_##NAME##_hooks() { \
		if (NAME##_hooks) free(NAME##_hooks); \
		NAME##_hooks = NULL; \
		NAME##_hooks_used = 0; \
		NAME##_hooks_size = 0; \
	}

#define CALL_HOOKS(NAME, ARGS) \
	for (unsigned int ii = 0; ii < NAME##_hooks_used; ii++) \
		NAME##_hooks[ii] ARGS

#endif /* __HOOK_H__ */
