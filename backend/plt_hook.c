#include "plt_hook.h"


#include <string.h>
#include "utils/elf.h"
#include "utils/mem.h"
#include "utils/log.h"

static int resolve_got_slot(const char *target_module,
                            const char *symbol_name,
                            void ***out_slot,
                            int *out_prot) {
    unsigned long base = find_module_base("/proc/self/maps", target_module);
    if (!base) {
        LOGE("module not found: %s", target_module);
        return -1;
    }

    Elf64_Dyn *dyn = find_dynamic_section(base);
    if (!dyn) {
        LOGE("PT_DYNAMIC not found in %s", target_module);
        return -1;
    }

    if (find_got_slot(base, dyn, symbol_name, out_slot) != 0) {
        LOGE("symbol not found in PLT: %s", symbol_name);
        return -1;
    }

    long ps = toy_pagesize();
    *out_prot = make_page_rw((unsigned long)*out_slot, ps);
    if (*out_prot < 0)
        return -1;

    return 0;
}

int hook_plt_symbol(const char *target_module,
                    const char *symbol_name,
                    void *replacement,
                    void **original) {
    void **got_slot;
    int orig_prot;
    if (resolve_got_slot(target_module, symbol_name, &got_slot, &orig_prot) != 0)
        return -1;

    long ps = toy_pagesize();
    *original = *got_slot;
    *got_slot = replacement;
    restore_page_perms((unsigned long)got_slot, ps, orig_prot);
    LOGD("patched: old=%p -> new=%p", *original, replacement);
    return 0;
}

int unhook_plt_symbol(const char *target_module,
                      const char *symbol_name,
                      void *original) {
    void **got_slot;
    int orig_prot;
    if (resolve_got_slot(target_module, symbol_name, &got_slot, &orig_prot) != 0)
        return -1;

    long ps = toy_pagesize();
    *got_slot = original;
    restore_page_perms((unsigned long)got_slot, ps, orig_prot);
    LOGD("restored: new=%p -> old=%p", original, *got_slot);
    return 0;
}
