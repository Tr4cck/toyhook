/*
 * toyhook — unified hook framework (Phase 4)
 *
 * This layer sits above the raw inline_hook and plt_hook backends.
 * It provides:
 *   - toy_session_t  : a container that owns a set of hooks
 *   - toy_hook_t     : one hook point, bound to either inline or PLT backend
 *   - toy_handler_t  : callback attached to a hook, with phase/priority
 *
 * Call flow (user perspective):
 *
 *   sess = toy_session_create()
 *   hook = toy_hook_add(sess, &target)
 *   toy_hook_on(hook, &(toy_handler_t){ .phases = TOY_PHASE_BEFORE, .fn = my_cb })
 *   toy_hook_enable(hook)
 *   ...
 *   toy_hook_disable(hook)
 *   toy_session_destroy(sess)
 *
 * Phase 5 adds toy_dispatch(), which routes intercepted calls through
 * the handler chain (before → replace → original → after).
 */

#include "toyhook.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "inline_hook.h"
#include "plt_hook.h"
#include "utils/mem.h"
#include "utils/log.h"
#include "arch/arm64/dispatch.h"

#define INIT_HANDLER_CAP 4
#define INIT_HOOK_CAP    8

/*
 * toy_hook — represents one interception point.
 *
 * Lifecycle: created by toy_hook_add(), destroyed by toy_hook_remove()
 *            or toy_session_destroy().
 *
 *   target          what the user asked to hook (address or symbol)
 *   backend         which low-level engine to use (inline or PLT)
 *   resolved_addr   the actual memory address of the hook target,
 *                   filled by resolve_target()
 *   original_addr   after enable: points to the trampoline (inline) or
 *                   the original GOT value (PLT).  Calling through this
 *                   invokes the un-hooked function.
 *   handlers        dynamic array of toy_handler_t, sorted by priority.
 *                   Phase 5's dispatcher iterates this list.
 *   backend_data    opaque pointer owned by the backend.
 *                   For inline: the trampoline pointer (to free on disable).
 *                   For PLT: not needed currently (GOT slot is found by name).
 */
struct toy_hook {
    unsigned long id;
    toy_target_t target;

    void *resolved_addr;
    void *original_addr;

    toy_handler_t *handlers;
    size_t handler_count;
    size_t handler_cap;

    int enabled;
    unsigned long hit_count;

    void *dispatch_stub;
    void *backend_data;
};

/*
 * toy_session — a collection of hooks sharing the same lifecycle.
 *
 * Hooks are stored as a pointer array so that toy_hook_t pointers
 * remain stable across realloc.
 */
struct toy_session {
    toy_hook_t **hooks;
    size_t hook_count;
    size_t hook_cap;
    unsigned long next_id;
};

/* ── session lifecycle ────────────────────────────────────────── */

toy_session_t *toy_session_create(void) {
    toy_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->hook_cap = INIT_HOOK_CAP;
    s->hooks = calloc(s->hook_cap, sizeof(toy_hook_t *));
    if (!s->hooks) { free(s); return NULL; }
    s->next_id = 1;
    LOGD("session created %p", (void *)s);
    return s;
}

static void hook_cleanup(toy_hook_t *h) {
    if (h->enabled) {
        toy_hook_disable(h);
    }
    free(h->handlers);
    free(h);
}

void toy_session_destroy(toy_session_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->hook_count; i++) {
        hook_cleanup(s->hooks[i]);
    }
    free(s->hooks);
    free(s);
    LOGD("session destroyed %p", (void *)s);
}

/* ── target resolution ────────────────────────────────────────── */

/*
 * resolve_target — translate toy_target_t into resolved_addr.
 *
 * TOY_BACKEND_INLINE: use the address directly from by_addr.
 * TOY_BACKEND_PLT:    dlsym to find address from by_symbol.
 */
static int resolve_target(toy_hook_t *h) {
    if (!h) return -1;

    toy_target_t *t = &h->target;
    if (t->backend == TOY_BACKEND_INLINE) {
        h->resolved_addr = t->by_addr.addr;
        return 0;
    } else if (t->backend == TOY_BACKEND_PLT) {
        void *sym_addr = dlsym(RTLD_DEFAULT, t->by_symbol.symbol);
        if (!sym_addr) {
            LOGE("symbol '%s' not found", t->by_symbol.symbol);
            return -1;
        }
        h->resolved_addr = sym_addr;
        return 0;
    } else {
        LOGE("invalid target backend %d", t->backend);
        return -1;
    }
}

/* ── hook CRUD ────────────────────────────────────────────────── */

/*
 * toy_hook_add — create a new hook in the session.
 *
 * Allocates a toy_hook_t, copies the target descriptor, resolves it
 * (fills resolved_addr and backend), then appends to the session.
 *
 * Returns the hook pointer on success, NULL on failure (bad args,
 * resolution failed, OOM).
 */
toy_hook_t *toy_hook_add(toy_session_t *s, const toy_target_t *t) {
    if (!s || !t) return NULL;

    if (s->hook_count >= s->hook_cap) {
        size_t new_cap = s->hook_cap * 2;
        toy_hook_t **new_arr = realloc(s->hooks, new_cap * sizeof(toy_hook_t *));
        if (!new_arr) return NULL;
        s->hooks = new_arr;
        s->hook_cap = new_cap;
    }

    toy_hook_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->id = s->next_id++;
    h->target = *t;
    h->handler_cap = INIT_HANDLER_CAP;
    h->handlers = calloc(h->handler_cap, sizeof(toy_handler_t));
    if (!h->handlers) { free(h); return NULL; }

    if (resolve_target(h) != 0) {
        LOGE("failed to resolve target for hook %lu", h->id);
        free(h->handlers);
        free(h);
        return NULL;
    }

    s->hooks[s->hook_count++] = h;
    LOGD("hook added id=%lu backend=%d resolved=%p", h->id, h->target.backend, h->resolved_addr);
    return h;
}

/*
 * toy_hook_remove — detach and destroy a hook from the session.
 *
 * Swaps with the last element for O(1) removal (order doesn't matter).
 * Calls hook_cleanup() which disables the hook if active, then frees.
 */
int toy_hook_remove(toy_session_t *s, toy_hook_t *h)
{
    for (size_t i = 0; i < s->hook_count; i++) {
        if (s->hooks[i] == h) {
            s->hooks[i] = s->hooks[s->hook_count - 1];
            s->hooks[s->hook_count - 1] = NULL;
            s->hook_count--;
            hook_cleanup(h);
            return 0;
        }
    }
    LOGE("hook %p not found in session", (void *)h);
    return -1;
}

/* ── handler management ───────────────────────────────────────── */

/*
 * handler_priority_cmp — comparator for qsort on handlers array.
 * Sort by priority ascending (lower number = runs first).
 */
static int handler_priority_cmp(const void *a, const void *b) {
    const toy_handler_t *ha = (const toy_handler_t *)a;
    const toy_handler_t *hb = (const toy_handler_t *)b;
    if (ha->priority != hb->priority) {
        return (ha->priority > hb->priority) - (ha->priority < hb->priority);
    }

    return 0;
}

/*
 * toy_hook_on — attach a handler to a hook.
 *
 * Appends the handler, then re-sorts the array by priority.
 * A handler must have a non-NULL fn.  name is used by toy_hook_off()
 * to remove it later (can be NULL, but then you can't remove it by name).
 *
 * Returns 0 on success, -1 on failure.
 */
int toy_hook_on(toy_hook_t *h, const toy_handler_t *handler)
{
    if (!h || !handler || !handler->fn) return -1;

    if (h->handler_count >= h->handler_cap) {
        size_t new_cap = h->handler_cap * 2;
        toy_handler_t *new_arr = realloc(h->handlers, new_cap * sizeof(toy_handler_t));
        if (!new_arr) return -1;
        h->handlers = new_arr;
        h->handler_cap = new_cap;
    }

    h->handlers[h->handler_count++] = *handler;
    qsort(h->handlers, h->handler_count, sizeof(toy_handler_t), handler_priority_cmp);
    LOGD("handler '%s' added to hook %lu", handler->name ? handler->name : "?", h->id);
    return 0;
}

/*
 * toy_hook_off — remove a handler by name.
 *
 * Uses swap-with-last for O(1) removal, then re-sorts.
 * Returns 0 on success, -1 if not found.
 */
int toy_hook_off(toy_hook_t *h, const char *name) {
    if (!h || !name) return -1;

    for (size_t i = 0; i < h->handler_count; i++) {
        if (h->handlers[i].name && strcmp(h->handlers[i].name, name) == 0) {
            h->handlers[i] = h->handlers[h->handler_count - 1];
            h->handler_count--;
            qsort(h->handlers, h->handler_count, sizeof(toy_handler_t), handler_priority_cmp);
            return 0;
        }
    }
    LOGE("handler '%s' not found", name);
    return -1;
}

/* ── dispatch pipeline ─────────────────────────────────────────── */

/*
 * toy_call_original — invoke the original (un-hooked) function.
 *
 * ctx->original_addr is either a trampoline pointer (inline) or old GOT
 * value (PLT).  Cast to the appropriate function pointer type based on
 * argc and call with ctx->args[0..argc-1].
 */
static unsigned long toy_call_original(toy_callctx_t *ctx) {
    if (!ctx || !ctx->original_addr) return 0;

    void *orig = ctx->original_addr;
    switch (ctx->argc) {
        case 0: {
            typedef unsigned long (*fn0_t)(void);
            fn0_t fn = (fn0_t)orig;
            return fn();
        }
        case 1: {
            typedef unsigned long (*fn1_t)(unsigned long);
            fn1_t fn = (fn1_t)orig;
            return fn(ctx->args[0]);
        }
        case 2: {
            typedef unsigned long (*fn2_t)(unsigned long, unsigned long);
            fn2_t fn = (fn2_t)orig;
            return fn(ctx->args[0], ctx->args[1]);
        }
        case 3: {
            typedef unsigned long (*fn3_t)(unsigned long, unsigned long, unsigned long);
            fn3_t fn = (fn3_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2]);
        }
        case 4: {
            typedef unsigned long (*fn4_t)(unsigned long, unsigned long, unsigned long, unsigned long);
            fn4_t fn = (fn4_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2], ctx->args[3]);
        }
        case 5: {
            typedef unsigned long (*fn5_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
            fn5_t fn = (fn5_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2], ctx->args[3], ctx->args[4]);
        }
        case 6: {
            typedef unsigned long (*fn6_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
            fn6_t fn = (fn6_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2], ctx->args[3], ctx->args[4], ctx->args[5]);
        }
        case 7: {
            typedef unsigned long (*fn7_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
            fn7_t fn = (fn7_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2], ctx->args[3], ctx->args[4], ctx->args[5], ctx->args[6]);
        }
        case 8: {
            typedef unsigned long (*fn8_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
            fn8_t fn = (fn8_t)orig;
            return fn(ctx->args[0], ctx->args[1], ctx->args[2], ctx->args[3], ctx->args[4], ctx->args[5], ctx->args[6], ctx->args[7]);
        }
        default: {
            LOGE("too many arguments %u, max supported is %d", ctx->argc, TOY_MAX_ARGS);
            return 0;
        }
    }
}

/*
 * toy_run_handlers — execute all handlers matching the given phase.
 *
 * Iterates h->handlers[] (already sorted by priority).  For each handler
 * whose .phases bitmask includes `phase`, calls handler->fn(ctx, user_data).
 *
 * A handler returning non-zero causes early exit (the return value is
 * propagated upward).  All handlers returning 0 means "continue".
 */
static int toy_run_handlers(toy_callctx_t *ctx, unsigned phase) {
    toy_hook_t *h = ctx->hook;
    for (size_t i = 0; i < h->handler_count; i++) {
        toy_handler_t *hd = &h->handlers[i];
        if (hd->phases & phase) {
            int ret = hd->fn(ctx, hd->user_data);
            if (ret != 0) return ret;
        }
    }
    return 0;
}

/*
 * toy_dispatch — main entry point from the per-hook assembly stub.
 *
 * Called when a hooked function is invoked.  The assembly stub has already
 * saved x0-x7 into args[] and passes the hook pointer.
 *
 * Pipeline:
 *   1. Build toy_callctx_t from hook state + args
 *   2. Run TOY_PHASE_BEFORE handlers  (inspect/modify args)
 *   3. Run TOY_PHASE_REPLACE handlers (fully replace original)
 *   4. If no handler set skip_original, call the real function
 *   5. Run TOY_PHASE_AFTER handlers   (inspect/modify return value)
 *   6. Return ctx.ret_val to the assembly stub → caller
 */
unsigned long toy_dispatch(toy_hook_t *h, unsigned long *args, unsigned argc) {
    toy_callctx_t ctx = {0};
    ctx.hook = h;
    ctx.target_addr = h->resolved_addr;
    ctx.original_addr = h->original_addr;
    ctx.argc = argc;
    for (unsigned i = 0; i < argc && i < TOY_MAX_ARGS; i++)
        ctx.args[i] = args[i];

    __atomic_add_fetch(&h->hit_count, 1, __ATOMIC_RELAXED);

    toy_run_handlers(&ctx, TOY_PHASE_BEFORE);
    toy_run_handlers(&ctx, TOY_PHASE_REPLACE);

    if (!ctx.skip_original)
        ctx.ret_val = toy_call_original(&ctx);

    toy_run_handlers(&ctx, TOY_PHASE_AFTER);

    return ctx.ret_val;
}

/* ── enable / disable ─────────────────────────────────────────── */

/*
 * toy_hook_enable — activate the hook.
 *
 * Allocates a per-hook dispatch stub (arch-specific assembly that saves
 * args and calls toy_dispatch), then installs it via the chosen backend.
 * On failure, the stub is freed and the hook is left unchanged.
 */
int toy_hook_enable(toy_hook_t *h) {
    if (!h || h->enabled) return -1;

    void *stub = alloc_dispatch_stub(h);
    if (!stub) {
        LOGE("failed to allocate dispatch stub for hook %lu", h->id);
        return -1;
    }

    int ret;
    if (h->target.backend == TOY_BACKEND_INLINE) {
        ret = hook_inline(h->resolved_addr, stub, &h->original_addr);
    } else if (h->target.backend == TOY_BACKEND_PLT) {
        ret = hook_plt_symbol(h->target.by_symbol.module,
                              h->target.by_symbol.symbol,
                              stub, &h->original_addr);
    } else {
        LOGE("invalid backend %d for hook %lu", h->target.backend, h->id);
        free_dispatch_stub(stub, h);
        return -1;
    }

    if (ret != 0) {
        LOGE("backend install failed for hook %lu", h->id);
        free_dispatch_stub(stub, h);
        return -1;
    }

    h->dispatch_stub = stub;
    h->enabled = 1;
    LOGD("hook %lu enabled (stub=%p original=%p)", h->id, stub, h->original_addr);
    return 0;
}

/*
 * toy_hook_disable — deactivate the hook, restoring original behavior.
 *
 * Inline: unhook_inline restores the original instructions.
 * PLT: unhook_plt_symbol restores the GOT slot.
 *
 * After backend cleanup, frees the dispatch stub.
 */
int toy_hook_disable(toy_hook_t *h) {
    if (!h || !h->enabled) return -1;

    if (h->target.backend == TOY_BACKEND_INLINE) {
        unhook_inline(h->resolved_addr, h->original_addr);
    } else if (h->target.backend == TOY_BACKEND_PLT) {
        unhook_plt_symbol(h->target.by_symbol.module,
                          h->target.by_symbol.symbol,
                          h->original_addr);
    }

    free_dispatch_stub(h->dispatch_stub, h);
    h->dispatch_stub = NULL;
    h->original_addr = NULL;
    h->enabled = 0;
    LOGD("hook %lu disabled", h->id);
    return 0;
}

/* ── query ────────────────────────────────────────────────────── */

unsigned long toy_hook_get_hit_count(toy_hook_t *h) {
    return h ? __atomic_load_n(&h->hit_count, __ATOMIC_RELAXED) : 0;
}

/* ── batch commit ─────────────────────────────────────────────── */

/*
 * toy_commit — enable all hooks that have handlers attached.
 *
 * Equivalent to calling toy_hook_enable() on each hook where
 * handler_count > 0 and enabled == 0.
 */
int toy_commit(toy_session_t *s) {
    if (!s) return -1;

    for (size_t i = 0; i < s->hook_count; i++) {
        toy_hook_t *h = s->hooks[i];
        if (h->handler_count > 0 && !h->enabled) {
            if (toy_hook_enable(h) != 0) {
                LOGE("failed to enable hook %lu", h->id);
                return -1;
            }
        }
    }
    LOGD("commit on session %p", (void *)s);
    return 0;
}
