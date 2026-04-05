#include "dispatch.h"

#include <stdint.h>
#include <sys/mman.h>
#include "arch/arm64/emit.h"
#include "arch/arm64/arm64.h"
#include "utils/mem.h"
#include "utils/log.h"

/*
 * alloc_dispatch_stub — generate a per-hook trampoline that bridges
 * the patched target function to toy_dispatch().
 *
 * Layout (each hook gets one RWX page):
 *
 *   stp fp, lr, [sp, #-80]!        save frame, alloc 80 bytes on stack
 *   mov fp, sp
 *   stp x0,x1, [sp, #16]           save arg registers x0-x7
 *   stp x2,x3, [sp, #32]             into a contiguous 64-byte region
 *   stp x4,x5, [sp, #48]             at sp+16..sp+80
 *   stp x6,x7, [sp, #64]
 *
 *   ldr x16, [pc, #off1]           load hook pointer from literal pool
 *   mov x0, x16                    arg1 = hook
 *   add x1, sp, #16                arg2 = &saved_args (contiguous array)
 *   movz x2, #8                    arg3 = argc = 8
 *
 *   ldr x16, [pc, #off2]           load &toy_dispatch from literal pool
 *   blr x16                        toy_dispatch(hook, &args, 8)
 *
 *   ldp fp, lr, [sp], #80          restore frame, free stack
 *   ret                            return (x0 already has ret_val)
 *
 *   .quad hook_ptr                 literal pool entry 1
 *   .quad &toy_dispatch            literal pool entry 2
 *
 * The ldr-literal offsets are patched in after code generation,
 * once the literal pool positions are known.
 */
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
    p += 2; /* advance past 8-byte literal (p is uint32_t*, 4 bytes per slot) */

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
