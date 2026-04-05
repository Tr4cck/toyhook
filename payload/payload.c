#include <android/log.h>
#include <unistd.h>
#include <dlfcn.h>
#include "toyhook.h"


#define TAG "toyhook"

static toy_session_t *g_sess;

static int on_prop_call(toy_callctx_t *ctx, void *ud) {
    typedef int (*fn_t)(const char *, char *);
    fn_t orig = (fn_t)ctx->original_addr;
    const char *name = (const char *)ctx->args[0];
    char *value = (char *)ctx->args[1];
    int res = orig(name, value);
    ctx->ret_val = (unsigned long)res;
    ctx->skip_original = 1;
    return 0;
}

static int on_getpid_call(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 12345;
    ctx->skip_original = 1;
    return 0;
}

__attribute__((constructor))
static void on_load(void) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "injected (pid=%d)", getpid());

    g_sess = toy_session_create();
    if (!g_sess) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "session create failed");
        return;
    }

    toy_target_t prop_tgt = {
        .backend = TOY_BACKEND_PLT,
        .by_symbol = { .module = "libmyapplication.so", .symbol = "__system_property_get" },
    };
    toy_hook_t *prop_hook = toy_hook_add(g_sess, &prop_tgt);
    if (prop_hook) {
        toy_handler_t h = {
            .phases = TOY_PHASE_REPLACE,
            .fn = on_prop_call,
            .name = "log_prop",
        };
        toy_hook_on(prop_hook, &h);
        toy_hook_enable(prop_hook);
    }

    void *getpid_addr = dlsym(RTLD_DEFAULT, "getpid");
    if (getpid_addr) {
        toy_target_t pid_tgt = {
            .backend = TOY_BACKEND_INLINE,
            .by_addr = { .addr = getpid },
        };
        toy_hook_t *pid_hook = toy_hook_add(g_sess, &pid_tgt);
        if (pid_hook) {
            toy_handler_t h = {
                .phases = TOY_PHASE_REPLACE,
                .fn = on_getpid_call,
                .name = "fake_pid",
            };
            toy_hook_on(pid_hook, &h);
            toy_hook_enable(pid_hook);
        }
    }
}

__attribute__((destructor))
static void on_unload(void) {
    if (g_sess) {
        toy_session_destroy(g_sess);
        g_sess = NULL;
    }
}
