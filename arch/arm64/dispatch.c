#include "dispatch.h"

#include <stdint.h>
#include <sys/mman.h>
#include "arch/arm64/emit.h"
#include "arch/arm64/arm64.h"
#include "utils/mem.h"
#include "utils/log.h"

void *alloc_dispatch_stub(toy_hook_t *h) {
    long ps = toy_pagesize();
    void *page = alloc_rwx(ps);
    if (!page) {
        LOGE("alloc_dispatch_stub: alloc_rwx failed");
        return NULL;
    }

    uint32_t *p = page;

    p = emit_stp_pre(p, FP, LR, SP, -80);
    p = emit_mov_reg(p, FP, SP);
    p = emit_stp(p, X0, X1, SP, 16);
    p = emit_stp(p, X2, X3, SP, 32);
    p = emit_stp(p, X4, X5, SP, 48);
    p = emit_stp(p, X6, X7, SP, 64);

    uint32_t *hook_ldr = p;
    p = emit_ldr_literal(p, X16, 0);

    p = emit_mov_reg(p, X0, X16);
    p = emit_add_imm(p, X1, SP, 16);
    p = emit_movz_w(p, X2, 8);

    uint32_t *fn_ldr = p;
    p = emit_ldr_literal(p, X16, 0);

    p = emit_blr(p, X16);
    p = emit_ldp_post(p, FP, LR, SP, 80);
    p = emit_ret(p);

    emit_ldr_literal(hook_ldr, X16, (int64_t)(p - hook_ldr) * 4);
    *(uint64_t *)p = (uint64_t)h;
    p += 2;

    emit_ldr_literal(fn_ldr, X16, (int64_t)(p - fn_ldr) * 4);
    *(uint64_t *)p = (uint64_t)&toy_dispatch;

    __builtin___clear_cache(page, (char *)page + ps);
    LOGD("dispatch stub allocated at %p for hook %p", page, (void *)h);
    return page;
}

void free_dispatch_stub(void *stub, toy_hook_t *h) {
    if (!stub) return;
    long ps = toy_pagesize();
    munmap(stub, ps);
    LOGD("dispatch stub freed at %p for hook %p", stub, (void *)h);
}
