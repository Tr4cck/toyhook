#include "inline_hook.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "arch/arm64/asm.h"
#include "arch/arm64/emit.h"
#include "utils/mem.h"
#include "utils/log.h"

#define STOLEN_COUNT 4
#define STOLEN_BYTES (STOLEN_COUNT * 4)
#define HEADER_SIZE  32

struct trampoline_header {
    void *target;
    uint32_t orig_insns[STOLEN_COUNT];
    size_t mapped_size;
};

int hook_inline(void *target, void *replacement, void **original) {
    if ((uintptr_t)target % 4 != 0) {
        LOGE("target %p is not 4-byte aligned", target);
        return -1;
    }

    long ps = toy_pagesize();
    void *trampoline = alloc_rwx_near(ps, (unsigned long)target);
    if (!trampoline) {
        LOGD("near alloc failed, falling back to alloc_rwx");
        trampoline = alloc_rwx(ps);
    }
    if (!trampoline) return -1;

    struct trampoline_header *hdr = (struct trampoline_header *)trampoline;
    hdr->target = target;
    hdr->mapped_size = ps;
    for (int i = 0; i < STOLEN_COUNT; i++)
        hdr->orig_insns[i] = ((uint32_t *)target)[i];

    void *code_start = (void *)((uint8_t *)trampoline + HEADER_SIZE);
    uint32_t *code_ptr = (uint32_t *)code_start;

    for (int i = 0; i < STOLEN_COUNT; i++) {
        uint32_t insn = hdr->orig_insns[i];
        int slots_used = relocate_instruction(code_ptr, insn,
                                              (uintptr_t)target + i * 4,
                                              (uintptr_t)code_ptr);
        if (slots_used < 0) {
            LOGE("failed to relocate instruction at %p", (uint8_t *)target + i * 4);
            munmap(trampoline, ps);
            return -1;
        }
        code_ptr += slots_used;
    }

    code_ptr = emit_jump(code_ptr, (void *)((uintptr_t)target + STOLEN_BYTES));

    int orig_prot = make_page_rw((uintptr_t)target, ps);
    if (orig_prot < 0) {
        munmap(trampoline, ps);
        return -1;
    }

    emit_jump((uint32_t *)target, replacement);
    __builtin___clear_cache(target, (char *)target + STOLEN_BYTES);
    __builtin___clear_cache(code_start, (char *)code_start + (code_ptr - (uint32_t *)code_start) * 4);
    restore_page_perms((uintptr_t)target, ps, orig_prot);

    *original = code_start;
    return 0;
}

int unhook_inline(void *target, void *trampoline) {
    struct trampoline_header *hdr = (struct trampoline_header *)((uint8_t *)trampoline - HEADER_SIZE);
    if (hdr->target != target) {
        LOGE("trampoline target mismatch: expected %p, got %p", target, hdr->target);
        return -1;
    }

    long ps = toy_pagesize();
    int orig_prot = make_page_rw((uintptr_t)target, ps);
    if (orig_prot < 0) return -1;

    for (int i = 0; i < STOLEN_COUNT; i++)
        ((uint32_t *)target)[i] = hdr->orig_insns[i];

    __builtin___clear_cache(target, (char *)target + STOLEN_BYTES);
    restore_page_perms((uintptr_t)target, ps, orig_prot);
    munmap(hdr, hdr->mapped_size);
    return 0;
}

int hook_inline_get_insns(void *original, uint32_t out[4]) {
    if (!original || !out) return -1;

    struct trampoline_header *hdr =
        (struct trampoline_header *)((uint8_t *)original - HEADER_SIZE);
    if (!hdr->target) return -1;

    for (int i = 0; i < STOLEN_COUNT; i++)
        out[i] = hdr->orig_insns[i];

    return STOLEN_COUNT;
}
