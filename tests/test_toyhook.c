#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <elf.h>
#include "toyhook.h"

#include "test_framework.h"

/* ── stubs for backend / ELF functions ─────────────────────── */

static int   stub_hook_inline_ret = 0;
static void *stub_hook_inline_target = NULL;
static void *stub_hook_inline_replacement = NULL;
static void *stub_hook_inline_original_val = (void*)0xDEAD;

int hook_inline(void *target, void *replacement, void **original) {
    stub_hook_inline_target = target;
    stub_hook_inline_replacement = replacement;
    if (original && stub_hook_inline_ret == 0)
        *original = stub_hook_inline_original_val;
    return stub_hook_inline_ret;
}

static int stub_unhook_inline_ret = 0;
int unhook_inline(void *target, void *trampoline) {
    (void)target; (void)trampoline;
    return stub_unhook_inline_ret;
}

static int stub_unhook_plt_ret = 0;
int unhook_plt_symbol(const char *mod, const char *sym, void *orig) {
    (void)mod; (void)sym; (void)orig;
    return stub_unhook_plt_ret;
}

static int   stub_hook_plt_ret = 0;
static void *stub_hook_plt_original_val = (void*)0xBEEF;
static const char *stub_hook_plt_last_module = NULL;
static const char *stub_hook_plt_last_symbol = NULL;

int hook_plt_symbol(const char *mod, const char *sym, void *repl, void **orig) {
    (void)repl;
    stub_hook_plt_last_module = mod;
    stub_hook_plt_last_symbol = sym;
    if (orig && stub_hook_plt_ret == 0)
        *orig = stub_hook_plt_original_val;
    return stub_hook_plt_ret;
}

static unsigned long stub_module_base = 0x400000;
unsigned long find_module_base(const char *path, const char *mod) {
    (void)path; (void)mod;
    return stub_module_base;
}

static Elf64_Dyn stub_dyn;
static Elf64_Dyn *stub_dynamic_section = &stub_dyn;
Elf64_Dyn *find_dynamic_section(unsigned long base) {
    (void)base;
    return stub_dynamic_section;
}

static int stub_got_slot_ret = -1;
static void *stub_got_slot_ptr;

int find_got_slot(unsigned long base, Elf64_Dyn *dyn, const char *sym, void ***out) {
    (void)base; (void)dyn; (void)sym;
    if (stub_got_slot_ret == 0 && out)
        *out = &stub_got_slot_ptr;
    return stub_got_slot_ret;
}

static void *stub_alloc_dispatch_val = (void*)0xCAFE;
void *alloc_dispatch_stub(toy_hook_t *h) {
    (void)h;
    return stub_alloc_dispatch_val;
}

void free_dispatch_stub(void *stub, toy_hook_t *h) {
    (void)stub; (void)h;
}

static int stub_get_insns_ret = -1;
static uint32_t stub_get_insns_out[4] = {0};

int hook_inline_get_insns(void *original, uint32_t out[4]) {
    (void)original;
    if (stub_get_insns_ret < 0) return -1;
    for (int i = 0; i < 4; i++) out[i] = stub_get_insns_out[i];
    return stub_get_insns_ret;
}

static void reset_stubs(void) {
    stub_hook_inline_ret = 0;
    stub_hook_inline_target = NULL;
    stub_hook_inline_replacement = NULL;
    stub_hook_inline_original_val = (void*)0xDEAD;
    stub_unhook_inline_ret = 0;
    stub_hook_plt_ret = 0;
    stub_hook_plt_original_val = (void*)0xBEEF;
    stub_hook_plt_last_module = NULL;
    stub_hook_plt_last_symbol = NULL;
    stub_module_base = 0x400000;
    stub_dynamic_section = &stub_dyn;
    stub_got_slot_ret = -1;
    stub_alloc_dispatch_val = (void*)0xCAFE;
    stub_get_insns_ret = -1;
    memset(stub_get_insns_out, 0, sizeof(stub_get_insns_out));
}

/* ── include code under test ──────────────────────────────── */
#include "../core/toyhook.c"

/* ── helpers ──────────────────────────────────────────────── */

static void dummy_fn(void) {}

static toy_target_t make_addr_target(void *addr) {
    toy_target_t t = {0};
    t.backend = TOY_BACKEND_INLINE;
    t.by_addr.addr = addr;
    return t;
}

static toy_target_t make_sym_target(const char *mod, const char *sym) {
    toy_target_t t = {0};
    t.backend = TOY_BACKEND_PLT;
    t.by_symbol.module = mod;
    t.by_symbol.symbol = sym;
    return t;
}

static int dummy_handler(toy_callctx_t *ctx, void *ud) {
    (void)ctx; (void)ud;
    return 0;
}

static int handler_b_called;
static int handler_a_called;
static int handler_c_called;

static int handler_b(toy_callctx_t *ctx, void *ud) { (void)ctx; (void)ud; return handler_b_called = 1; }
static int handler_a(toy_callctx_t *ctx, void *ud) { (void)ctx; (void)ud; return handler_a_called = 1; }
static int handler_c(toy_callctx_t *ctx, void *ud) { (void)ctx; (void)ud; return handler_c_called = 1; }

/* ── session tests ────────────────────────────────────────── */

TEST(session_create_basic) {
    toy_session_t *s = toy_session_create();
    ASSERT_TRUE(s != NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(session_destroy_null_safe) {
    toy_session_destroy(NULL);
    return 0;
}

/* ── hook add tests ───────────────────────────────────────── */

TEST(hook_add_addr_target) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_TRUE(h != NULL);
    ASSERT_PTR(h->resolved_addr, dummy_fn);
    ASSERT_INT(h->target.backend, TOY_BACKEND_INLINE);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_add_null_args) {
    ASSERT_PTR(toy_hook_add(NULL, &(toy_target_t){0}), NULL);
    toy_session_t *s = toy_session_create();
    ASSERT_PTR(toy_hook_add(s, NULL), NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_add_symbol_resolves_addr) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_sym_target("libc.so.6", "printf");
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(h->resolved_addr != NULL);
    ASSERT_INT(h->target.backend, TOY_BACKEND_PLT);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_add_symbol_not_found) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_sym_target("no_such_lib.so", "nonexistent_symbol_xyz_12345");
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_PTR(h, NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_add_ids_increment) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h1 = toy_hook_add(s, &t);
    toy_hook_t *h2 = toy_hook_add(s, &t);
    ASSERT_TRUE(h1 && h2);
    ASSERT_INT(h2->id, h1->id + 1);
    toy_session_destroy(s);
    return 0;
}

/* ── hook remove tests ────────────────────────────────────── */

TEST(hook_add_and_remove) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_remove(s, h), 0);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_remove_not_found) {
    reset_stubs();
    toy_session_t *s1 = toy_session_create();
    toy_session_t *s2 = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s1, &t);
    ASSERT_INT(toy_hook_remove(s2, h), -1);
    toy_session_destroy(s1);
    toy_session_destroy(s2);
    return 0;
}

/* ── handler tests ────────────────────────────────────────── */

TEST(hook_on_basic) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_handler_t hd = { .fn = dummy_handler, .name = "test" };
    ASSERT_INT(toy_hook_on(h, &hd), 0);
    ASSERT_INT(h->handler_count, 1);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_on_null_fn_rejected) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_handler_t hd = { .fn = NULL, .name = "bad" };
    ASSERT_INT(toy_hook_on(h, &hd), -1);
    ASSERT_INT(h->handler_count, 0);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_on_null_args_rejected) {
    reset_stubs();
    toy_handler_t hd = { .fn = dummy_handler, .name = "x" };
    ASSERT_INT(toy_hook_on(NULL, &hd), -1);
    ASSERT_INT(toy_hook_on((toy_hook_t*)1, NULL), -1);
    return 0;
}

TEST(hook_on_grow_array) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    for (int i = 0; i < INIT_HANDLER_CAP + 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "h%d", i);
        toy_handler_t hd = { .fn = dummy_handler, .name = name };
        ASSERT_INT(toy_hook_on(h, &hd), 0);
    }
    ASSERT_INT(h->handler_count, INIT_HANDLER_CAP + 4);
    ASSERT_TRUE(h->handler_cap > INIT_HANDLER_CAP);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_off_basic) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_handler_t hd = { .fn = dummy_handler, .name = "removeme" };
    toy_hook_on(h, &hd);
    ASSERT_INT(toy_hook_off(h, "removeme"), 0);
    ASSERT_INT(h->handler_count, 0);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_off_not_found) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_handler_t hd = { .fn = dummy_handler, .name = "real" };
    toy_hook_on(h, &hd);
    ASSERT_TRUE(toy_hook_off(h, "ghost") == NULL);
    ASSERT_INT(h->handler_count, 1);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_off_null_args_rejected) {
    ASSERT_TRUE(toy_hook_off(NULL, "x") == NULL);
    ASSERT_TRUE(toy_hook_off((toy_hook_t*)1, NULL) == NULL);
    return 0;
}

TEST(handler_priority_sorted) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);

    toy_handler_t hb = { .priority = 30, .fn = handler_b, .name = "b" };
    toy_handler_t ha = { .priority = 10, .fn = handler_a, .name = "a" };
    toy_handler_t hc = { .priority = 20, .fn = handler_c, .name = "c" };
    toy_hook_on(h, &hb);
    toy_hook_on(h, &ha);
    toy_hook_on(h, &hc);

    ASSERT_INT(h->handler_count, 3);
    ASSERT_INT(h->handlers[0].priority, 10);
    ASSERT_INT(h->handlers[1].priority, 20);
    ASSERT_INT(h->handlers[2].priority, 30);
    ASSERT_PTR(h->handlers[0].fn, handler_a);
    ASSERT_PTR(h->handlers[1].fn, handler_c);
    ASSERT_PTR(h->handlers[2].fn, handler_b);
    toy_session_destroy(s);
    return 0;
}

/* ── enable / disable tests ───────────────────────────────── */

TEST(hook_enable_inline) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_enable(h), 0);
    ASSERT_INT(h->enabled, 1);
    ASSERT_PTR(stub_hook_inline_target, dummy_fn);
    ASSERT_PTR(h->original_addr, (void*)0xDEAD);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_enable_plt) {
    reset_stubs();
    stub_got_slot_ret = 0;
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_sym_target("libc.so.6", "printf");
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(h->target.backend, TOY_BACKEND_PLT);

    ASSERT_INT(toy_hook_enable(h), 0);
    ASSERT_INT(h->enabled, 1);
    ASSERT_TRUE(stub_hook_plt_last_module != NULL);
    ASSERT_INT(strcmp(stub_hook_plt_last_module, "libc.so.6"), 0);
    ASSERT_INT(strcmp(stub_hook_plt_last_symbol, "printf"), 0);
    ASSERT_PTR(h->original_addr, (void*)0xBEEF);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_enable_double_fails) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_enable(h), 0);
    ASSERT_INT(toy_hook_enable(h), -1);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_enable_backend_fails) {
    reset_stubs();
    stub_hook_inline_ret = -1;
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_enable(h), -1);
    ASSERT_INT(h->enabled, 0);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_enable_null_rejected) {
    ASSERT_INT(toy_hook_enable(NULL), -1);
    return 0;
}

TEST(hook_disable_basic) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_hook_enable(h);
    ASSERT_INT(toy_hook_disable(h), 0);
    ASSERT_INT(h->enabled, 0);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_disable_not_enabled) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_disable(h), -1);
    toy_session_destroy(s);
    return 0;
}

TEST(hook_disable_null_rejected) {
    ASSERT_INT(toy_hook_disable(NULL), -1);
    return 0;
}

/* ── commit tests ─────────────────────────────────────────── */

TEST(commit_enables_hooks_with_handlers) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h1 = toy_hook_add(s, &t);
    toy_hook_t *h2 = toy_hook_add(s, &t);
    toy_handler_t hd = { .fn = dummy_handler, .name = "cb" };
    toy_hook_on(h1, &hd);
    toy_hook_on(h2, &hd);
    ASSERT_INT(toy_commit(s), 0);
    ASSERT_INT(h1->enabled, 1);
    ASSERT_INT(h2->enabled, 1);
    toy_session_destroy(s);
    return 0;
}

TEST(commit_skips_no_handlers) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_commit(s), 0);
    ASSERT_INT(h->enabled, 0);
    toy_session_destroy(s);
    return 0;
}

TEST(commit_null_safe) {
    ASSERT_INT(toy_commit(NULL), -1);
    return 0;
}

/* ── query tests ──────────────────────────────────────────── */

TEST(hit_count_default_zero) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(toy_hook_get_hit_count(h), 0);
    ASSERT_INT(toy_hook_get_hit_count(NULL), 0);
    toy_session_destroy(s);
    return 0;
}

/* ── hook array grow test ─────────────────────────────────── */

TEST(hook_add_many_grow) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    for (int i = 0; i < INIT_HOOK_CAP + 4; i++) {
        toy_hook_t *h = toy_hook_add(s, &t);
        ASSERT_TRUE(h != NULL);
    }
    ASSERT_INT(s->hook_count, INIT_HOOK_CAP + 4);
    ASSERT_TRUE(s->hook_cap > INIT_HOOK_CAP);
    toy_session_destroy(s);
    return 0;
}

/* ── dispatch pipeline helpers ──────────────────────────────── */

static unsigned long g_seen_args[TOY_MAX_ARGS];
static unsigned g_seen_argc;
static unsigned long g_seen_retval;
static int g_before_called, g_after_called, g_replace_called;
static void *g_seen_user_data;
static int g_handler_order[8];
static int g_handler_order_idx;

static unsigned long test_fn_0args(void) { return 42; }
static unsigned long test_fn_1arg(unsigned long a0) { return a0 + 1; }
static unsigned long test_fn_2args(unsigned long a0, unsigned long a1) { return a0 + a1; }

static int handler_record_before(toy_callctx_t *ctx, void *ud) {
    g_before_called = 1;
    g_seen_argc = ctx->argc;
    for (unsigned i = 0; i < ctx->argc; i++)
        g_seen_args[i] = ctx->args[i];
    g_seen_user_data = ud;
    return 0;
}

static int handler_record_after(toy_callctx_t *ctx, void *ud) {
    g_after_called = 1;
    g_seen_retval = ctx->ret_val;
    (void)ud;
    return 0;
}

static int handler_skip_and_set_ret(toy_callctx_t *ctx, void *ud) {
    ctx->skip_original = 1;
    ctx->ret_val = 999;
    (void)ud;
    return 0;
}

static int handler_modify_arg0(toy_callctx_t *ctx, void *ud) {
    ctx->args[0] = 100;
    (void)ud;
    return 0;
}

static int handler_modify_retval(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 777;
    (void)ud;
    return 0;
}

static int handler_record_order(toy_callctx_t *ctx, void *ud) {
    g_handler_order[g_handler_order_idx++] = (int)(long)ud;
    (void)ctx;
    return 0;
}

static int handler_return_error(toy_callctx_t *ctx, void *ud) {
    (void)ctx; (void)ud;
    return 42;
}

static int handler_record_replace(toy_callctx_t *ctx, void *ud) {
    g_replace_called = 1;
    (void)ctx; (void)ud;
    return 0;
}

static void reset_dispatch_globals(void) {
    memset(g_seen_args, 0, sizeof(g_seen_args));
    g_seen_argc = 0;
    g_seen_retval = 0;
    g_before_called = g_after_called = g_replace_called = 0;
    g_seen_user_data = NULL;
    memset(g_handler_order, 0, sizeof(g_handler_order));
    g_handler_order_idx = 0;
}

/* ── call_original tests ───────────────────────────────────── */

TEST(call_original_0_args) {
    toy_callctx_t ctx = {0};
    ctx.original_addr = (void*)test_fn_0args;
    ctx.argc = 0;
    ASSERT_INT(toy_call_original(&ctx), 42);
    return 0;
}

TEST(call_original_1_arg) {
    toy_callctx_t ctx = {0};
    ctx.original_addr = (void*)test_fn_1arg;
    ctx.argc = 1;
    ctx.args[0] = 99;
    ASSERT_INT(toy_call_original(&ctx), 100);
    return 0;
}

TEST(call_original_2_args) {
    toy_callctx_t ctx = {0};
    ctx.original_addr = (void*)test_fn_2args;
    ctx.argc = 2;
    ctx.args[0] = 30;
    ctx.args[1] = 12;
    ASSERT_INT(toy_call_original(&ctx), 42);
    return 0;
}

TEST(call_original_too_many_args) {
    toy_callctx_t ctx = {0};
    ctx.original_addr = NULL;
    ctx.argc = 9;
    ASSERT_INT(toy_call_original(&ctx), 0);
    return 0;
}

/* ── dispatch pipeline tests ───────────────────────────────── */

TEST(dispatch_before_sees_args) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t hd = { .phases = TOY_PHASE_BEFORE, .fn = handler_record_before, .name = "rec" };
    toy_hook_on(h, &hd);

    unsigned long args[] = { 10, 20, 30 };
    toy_dispatch(h, args, 3);

    ASSERT_TRUE(g_before_called);
    ASSERT_INT(g_seen_argc, 3);
    ASSERT_INT(g_seen_args[0], 10);
    ASSERT_INT(g_seen_args[1], 20);
    ASSERT_INT(g_seen_args[2], 30);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_before_modify_args) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_1arg;

    toy_handler_t hd = { .phases = TOY_PHASE_BEFORE, .fn = handler_modify_arg0, .name = "mod" };
    toy_hook_on(h, &hd);

    unsigned long args[] = { 5 };
    unsigned long ret = toy_dispatch(h, args, 1);

    ASSERT_INT(ret, 101);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_after_sees_retval) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t hd = { .phases = TOY_PHASE_AFTER, .fn = handler_record_after, .name = "rec" };
    toy_hook_on(h, &hd);

    unsigned long ret = toy_dispatch(h, NULL, 0);

    ASSERT_TRUE(g_after_called);
    ASSERT_INT(g_seen_retval, 42);
    ASSERT_INT(ret, 42);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_after_modify_retval) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t hd = { .phases = TOY_PHASE_AFTER, .fn = handler_modify_retval, .name = "mod" };
    toy_hook_on(h, &hd);

    unsigned long ret = toy_dispatch(h, NULL, 0);

    ASSERT_INT(ret, 777);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_skip_original) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t hd = { .phases = TOY_PHASE_REPLACE, .fn = handler_skip_and_set_ret, .name = "skip" };
    toy_hook_on(h, &hd);

    unsigned long ret = toy_dispatch(h, NULL, 0);

    ASSERT_INT(ret, 999);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_replace_phase_runs) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t hd = { .phases = TOY_PHASE_REPLACE, .fn = handler_record_replace, .name = "rep" };
    toy_hook_on(h, &hd);

    toy_dispatch(h, NULL, 0);

    ASSERT_TRUE(g_replace_called);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_hit_count_increments) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    ASSERT_INT(toy_hook_get_hit_count(h), 0);
    toy_dispatch(h, NULL, 0);
    ASSERT_INT(toy_hook_get_hit_count(h), 1);
    toy_dispatch(h, NULL, 0);
    ASSERT_INT(toy_hook_get_hit_count(h), 2);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_handler_user_data) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    void *my_ud = (void*)0x1234;
    toy_handler_t hd = { .phases = TOY_PHASE_BEFORE, .fn = handler_record_before, .user_data = my_ud, .name = "ud" };
    toy_hook_on(h, &hd);

    toy_dispatch(h, NULL, 0);

    ASSERT_PTR(g_seen_user_data, my_ud);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_handler_priority_order) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t h30 = { .priority = 30, .phases = TOY_PHASE_BEFORE, .fn = handler_record_order, .user_data = (void*)30, .name = "p30" };
    toy_handler_t h10 = { .priority = 10, .phases = TOY_PHASE_BEFORE, .fn = handler_record_order, .user_data = (void*)10, .name = "p10" };
    toy_handler_t h20 = { .priority = 20, .phases = TOY_PHASE_BEFORE, .fn = handler_record_order, .user_data = (void*)20, .name = "p20" };
    toy_hook_on(h, &h30);
    toy_hook_on(h, &h10);
    toy_hook_on(h, &h20);

    toy_dispatch(h, NULL, 0);

    ASSERT_INT(g_handler_order[0], 10);
    ASSERT_INT(g_handler_order[1], 20);
    ASSERT_INT(g_handler_order[2], 30);
    toy_session_destroy(s);
    return 0;
}

TEST(dispatch_handler_early_exit) {
    reset_stubs(); reset_dispatch_globals();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    h->original_addr = (void*)test_fn_0args;

    toy_handler_t herr = { .priority = 10, .phases = TOY_PHASE_BEFORE, .fn = handler_return_error, .name = "err" };
    toy_handler_t hok  = { .priority = 20, .phases = TOY_PHASE_BEFORE, .fn = handler_record_order, .name = "ok" };
    toy_hook_on(h, &herr);
    toy_hook_on(h, &hok);

    toy_dispatch(h, NULL, 0);

    ASSERT_INT(g_handler_order_idx, 0);
    toy_session_destroy(s);
    return 0;
}

/* ── enable/disable lifecycle tests ────────────────────────── */

TEST(enable_sets_dispatch_stub) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);

    ASSERT_PTR(h->dispatch_stub, NULL);
    ASSERT_INT(toy_hook_enable(h), 0);
    ASSERT_PTR(h->dispatch_stub, (void*)0xCAFE);
    toy_session_destroy(s);
    return 0;
}

TEST(disable_clears_dispatch_stub) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_hook_enable(h);

    ASSERT_TRUE(h->dispatch_stub != NULL);
    toy_hook_disable(h);

    ASSERT_PTR(h->dispatch_stub, NULL);
    ASSERT_PTR(h->original_addr, NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(enable_stub_alloc_fails) {
    reset_stubs();
    stub_alloc_dispatch_val = NULL;
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);

    ASSERT_INT(toy_hook_enable(h), -1);
    ASSERT_INT(h->enabled, 0);
    ASSERT_PTR(h->dispatch_stub, NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(disable_plt_calls_unhook) {
    reset_stubs();
    stub_got_slot_ret = 0;
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_sym_target("libc.so.6", "printf");
    toy_hook_t *h = toy_hook_add(s, &t);
    ASSERT_INT(h->target.backend, TOY_BACKEND_PLT);

    ASSERT_INT(toy_hook_enable(h), 0);
    ASSERT_TRUE(h->original_addr != NULL);

    ASSERT_INT(toy_hook_disable(h), 0);
    ASSERT_INT(h->enabled, 0);
    ASSERT_PTR(h->original_addr, NULL);
    ASSERT_PTR(h->dispatch_stub, NULL);
    toy_session_destroy(s);
    return 0;
}

TEST(session_destroy_cleans_enabled_hooks) {
    reset_stubs();
    toy_session_t *s = toy_session_create();
    toy_target_t t = make_addr_target(dummy_fn);
    toy_hook_t *h = toy_hook_add(s, &t);
    toy_hook_enable(h);
    ASSERT_INT(h->enabled, 1);

    toy_session_destroy(s);
    return 0;
}

/* ── describe tests ──────────────────────────────────────────── */

TEST(describe_null_safe) {
    toy_hook_describe(NULL, fileno(stderr));
    toy_session_describe(NULL, fileno(stderr));
    return 0;
}

/* ── main ─────────────────────────────────────────────────── */

int main(void) {
    printf("toyhook framework tests:\n");

    RUN_TEST(session_create_basic);
    RUN_TEST(session_destroy_null_safe);

    RUN_TEST(hook_add_addr_target);
    RUN_TEST(hook_add_null_args);
    RUN_TEST(hook_add_symbol_resolves_addr);
    RUN_TEST(hook_add_symbol_not_found);
    RUN_TEST(hook_add_ids_increment);
    RUN_TEST(hook_add_many_grow);

    RUN_TEST(hook_add_and_remove);
    RUN_TEST(hook_remove_not_found);

    RUN_TEST(hook_on_basic);
    RUN_TEST(hook_on_null_fn_rejected);
    RUN_TEST(hook_on_null_args_rejected);
    RUN_TEST(hook_on_grow_array);
    RUN_TEST(hook_off_basic);
    RUN_TEST(hook_off_not_found);
    RUN_TEST(hook_off_null_args_rejected);
    RUN_TEST(handler_priority_sorted);

    RUN_TEST(hook_enable_inline);
    RUN_TEST(hook_enable_plt);
    RUN_TEST(hook_enable_double_fails);
    RUN_TEST(hook_enable_backend_fails);
    RUN_TEST(hook_enable_null_rejected);
    RUN_TEST(hook_disable_basic);
    RUN_TEST(hook_disable_not_enabled);
    RUN_TEST(hook_disable_null_rejected);

    RUN_TEST(commit_enables_hooks_with_handlers);
    RUN_TEST(commit_skips_no_handlers);
    RUN_TEST(commit_null_safe);

    RUN_TEST(hit_count_default_zero);

    RUN_TEST(call_original_0_args);
    RUN_TEST(call_original_1_arg);
    RUN_TEST(call_original_2_args);
    RUN_TEST(call_original_too_many_args);

    RUN_TEST(dispatch_before_sees_args);
    RUN_TEST(dispatch_before_modify_args);
    RUN_TEST(dispatch_after_sees_retval);
    RUN_TEST(dispatch_after_modify_retval);
    RUN_TEST(dispatch_skip_original);
    RUN_TEST(dispatch_replace_phase_runs);
    RUN_TEST(dispatch_hit_count_increments);
    RUN_TEST(dispatch_handler_user_data);
    RUN_TEST(dispatch_handler_priority_order);
    RUN_TEST(dispatch_handler_early_exit);

    RUN_TEST(enable_sets_dispatch_stub);
    RUN_TEST(disable_clears_dispatch_stub);
    RUN_TEST(enable_stub_alloc_fails);
    RUN_TEST(disable_plt_calls_unhook);
    RUN_TEST(session_destroy_cleans_enabled_hooks);

    RUN_TEST(describe_null_safe);

    printf("\n%d/%d passed, %d failed\n", __tf_pass, __tf_total, __tf_fail);
    return __tf_fail > 0 ? 1 : 0;
}
