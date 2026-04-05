#ifndef TOYHOOK_PLT_HOOK_H
#define TOYHOOK_PLT_HOOK_H

/*
 * hook_plt_symbol - hook a PLT/GOT entry in a loaded module.
 *
 * @target_module  substring to match in /proc/self/maps (e.g. "libfoo.so")
 * @symbol_name    the imported symbol to intercept (e.g. "open")
 * @replacement    your hook function
 * @original       receives the original function pointer (for calling through)
 *
 * Returns 0 on success, -1 on failure.
 */
int hook_plt_symbol(const char *target_module,
                    const char *symbol_name,
                    void *replacement,
                    void **original);

/*
 * unhook_plt_symbol - restore the original function pointer in the GOT.
 * 
 * @target_module  substring to match in /proc/self/maps (e.g. "libfoo.so")
 * @symbol_name    the imported symbol to restore (e.g. "open")
 * @original       the original function pointer to restore
 *
 * Returns 0 on success, -1 on failure.
 */
int unhook_plt_symbol(const char *target_module,
                      const char *symbol_name,
                      void *original);

#endif
