#ifndef __SHAREDMEM_H__
#define __SHAREDMEM_H__

typedef void(shared_memory_free_f)(void *ptr);

/*
 * Creates a new shared memory block that can be referenced through key, holding the given data.
 */
void shared_memory_set(struct module *module, const char *key, void *data);

/*
 * Looks up the key in the shared memory. If it found data with that given key, it returns the
 * associated data, else fallback will be returned.
 */
void *shared_memory_get(const char *module_name, const char *key, void *fallback);

/*
 * Set a free function to free data upon module's shared memory deletion
 */
void shared_memory_set_free_func(struct module *module, shared_memory_free_f *free_func);

#endif
