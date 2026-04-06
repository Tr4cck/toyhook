#include <unistd.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>
#include "toyhook.h"
#include "trace.h"
#include "server.h"
#include "utils/log.h"

static toy_session_t *g_sess;
static toy_hook_t *g_prop_hook;
static toy_hook_t *g_rand_hook;
static toy_tracer_t *g_tracer;

typedef struct {
    unsigned long count;
} call_state_t;

/* ── Hook 1: __system_property_get (PLT backend) ────────────
 *
 * Demonstrates: BEFORE + AFTER handlers on one hook,
 *               priority ordering, user_data, toy_hook_get_id.
 */

static int on_prop_enter(toy_callctx_t *ctx, void *ud) {
    call_state_t *st = ud;
    st->count++;
    LOGI("[hook#%lu] BEFORE prop_get(\"%s\") call=%lu",
        toy_hook_get_id(ctx->hook),
        (const char *)ctx->args[0],
        st->count);
    return 0;
}

static int on_prop_leave(toy_callctx_t *ctx, void *ud) {
    (void)ud;
    LOGI("[hook#%lu] AFTER  prop_get -> ret=%lu",
        toy_hook_get_id(ctx->hook),
        ctx->ret_val);
    return 0;
}

/* ── Hook 2: rand (INLINE backend) ────────────────────────────
 *
 * Demonstrates: REPLACE phase, skip_original, fake return value.
 */

static int on_rand(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 42;
    ctx->skip_original = 1;
    LOGI("[hook#%lu] REPLACE rand() -> %lu",
        toy_hook_get_id(ctx->hook), ctx->ret_val);
    return 0;
}

/* ── lifecycle ───────────────────────────────────────────── */

__attribute__((constructor))
static void on_load(void) {
    LOGI("toyhook loaded (real pid=%d)", getpid());

    g_sess = toy_session_create();
    if (!g_sess) {
        LOGE("session create failed");
        return;
    }

    g_tracer = toy_tracer_create(1024);
    if (!g_tracer) {
        LOGE("tracer create failed");
    }

    toy_target_t prop_tgt = {
        .backend = TOY_BACKEND_PLT,
        .by_symbol = {
            .module = "libmyapplication.so",
            .symbol = "__system_property_get",
        },
    };
    g_prop_hook = toy_hook_add(g_sess, &prop_tgt);
    if (g_prop_hook) {
        call_state_t *st = malloc(sizeof(*st));
        if (st) {
            st->count = 0;
            toy_handler_t h_enter = {
                .phases    = TOY_PHASE_BEFORE,
                .priority  = -100,
                .fn        = on_prop_enter,
                .user_data = st,
                .name      = "prop_enter",
            };
            toy_hook_on(g_prop_hook, &h_enter);
        }

        toy_handler_t h_leave = {
            .phases    = TOY_PHASE_AFTER,
            .priority  = 100,
            .fn        = on_prop_leave,
            .user_data = NULL,
            .name      = "prop_leave",
        };
        toy_hook_on(g_prop_hook, &h_leave);
    }

    void *rand_addr = dlsym(RTLD_DEFAULT, "rand");
    if (rand_addr) {
        toy_target_t rand_tgt = {
            .backend = TOY_BACKEND_INLINE,
            .by_addr = { .addr = rand_addr },
        };
        g_rand_hook = toy_hook_add(g_sess, &rand_tgt);
        if (g_rand_hook) {
            toy_handler_t h = {
                .phases = TOY_PHASE_REPLACE,
                .fn     = on_rand,
                .name   = "fake_rand",
            };
            toy_hook_on(g_rand_hook, &h);
        }
    }

    if (g_tracer) {
        if (g_prop_hook) toy_tracer_attach(g_tracer, g_prop_hook);
        if (g_rand_hook)  toy_tracer_attach(g_tracer, g_rand_hook);
    }

    toy_commit(g_sess);

    toy_session_describe(g_sess, stderr);

    static toyhook_server_ctx_t server_ctx = {0};
    server_ctx.session = g_sess;
    server_ctx.tracer  = g_tracer;
    pthread_t tid;
    pthread_create(&tid, NULL, toyhook_server_run, &server_ctx);
    pthread_detach(tid);
}
