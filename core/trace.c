#include "trace.h"
#include "toyhook.h"
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if !defined(__BIONIC__)
#include <sys/syscall.h>
#define gettid() ((pid_t)syscall(SYS_gettid))
#endif

#define NSEC_PER_SEC 1000000000UL

typedef struct trace_ctx {
    struct toy_tracer *tracer;
    unsigned long hook_id;
} trace_ctx_t;

struct toy_tracer {
    trace_event_t *events;
    size_t capacity;
    size_t mask;
    size_t head;
    size_t dropped;
    int enabled;
};

toy_tracer_t *toy_tracer_create(size_t capacity) {
    toy_tracer_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    size_t cap = 1;
    while (cap < capacity) cap <<= 1;

    t->events = calloc(cap, sizeof(trace_event_t));
    if (!t->events) {
        free(t);
        return NULL;
    }

    t->capacity = cap;
    t->mask = cap - 1;
    t->enabled = 1;
    return t;
}

void toy_tracer_destroy(toy_tracer_t *t) {
    if (!t) return;
    free(t->events);
    free(t);
}

/* ── ring buffer record ─────────────────────────────────── */

static void tracer_record(toy_tracer_t *t, const trace_event_t *ev) {
    if (!t->enabled) return;

    size_t pos = __atomic_fetch_add(&t->head, 1, __ATOMIC_RELAXED);
    size_t total = pos + 1;
    if (total > t->capacity) {
        __atomic_fetch_add(&t->dropped, 1, __ATOMIC_RELAXED);
    }
    t->events[pos & t->mask] = *ev;
}

static unsigned long tracer_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * NSEC_PER_SEC + (unsigned long)ts.tv_nsec;
}

/* ── handler pair ──────────────────────────────────────── */

static int trace_on_enter(toy_callctx_t *ctx, void *ud) {
    trace_ctx_t *tc = ud;
    if (!tc->tracer->enabled) return 0;

    trace_event_t ev = {0};
    ev.kind      = TOY_TRACE_ENTER;
    ev.hook_id    = tc->hook_id;
    ev.thread_id  = (unsigned long)gettid();
    ev.timestamp  = tracer_now_ns();
    ev.argc       = ctx->argc;
    memcpy(ev.args, ctx->args, sizeof(ev.args));
    tracer_record(tc->tracer, &ev);
    return 0;
}

static int trace_on_leave(toy_callctx_t *ctx, void *ud) {
    trace_ctx_t *tc = ud;
    if (!tc->tracer->enabled) return 0;

    trace_event_t ev = {0};
    ev.kind      = TOY_TRACE_LEAVE;
    ev.hook_id    = tc->hook_id;
    ev.thread_id  = (unsigned long)gettid();
    ev.timestamp  = tracer_now_ns();
    ev.argc       = ctx->argc;
    ev.ret_val    = ctx->ret_val;
    memcpy(ev.args, ctx->args, sizeof(ev.args));
    tracer_record(tc->tracer, &ev);
    return 0;
}

/* ── attach / detach ─────────────────────────────────── */

int toy_tracer_attach(toy_tracer_t *t, toy_hook_t *h) {
    if (!t || !h) return -1;

    trace_ctx_t *tc = malloc(sizeof(*tc));
    if (!tc) return -1;
    tc->tracer = t;
    tc->hook_id = toy_hook_get_id(h);
    toy_handler_t hd_enter = {
        .phases   = TOY_PHASE_BEFORE,
        .priority = INT_MIN,
        .fn       = trace_on_enter,
        .user_data = tc,
        .name     = "trace_enter"
    };
    toy_handler_t hd_leave = {
        .phases   = TOY_PHASE_AFTER,
        .priority = INT_MAX,
        .fn       = trace_on_leave,
        .user_data = tc,
        .name     = "trace_leave"
    };
    if (toy_hook_on(h, &hd_enter) != 0) {
        free(tc);
        return -1;
    }
    if (toy_hook_on(h, &hd_leave) != 0) {
        toy_hook_off(h, "trace_enter");
        free(tc);
        return -1;
    }
    return 0;
}

void toy_tracer_detach(toy_tracer_t *t, toy_hook_t *h) {
    void *ud = toy_hook_off(h, "trace_enter");
    toy_hook_off(h, "trace_leave");
    free(ud);
}

/* ── control ────────────────────────────────────────── */

void toy_tracer_enable(toy_tracer_t *t)  { if (t) t->enabled = 1; }
void toy_tracer_disable(toy_tracer_t *t) { if (t) t->enabled = 0; }

size_t toy_tracer_count(const toy_tracer_t *t) {
    if (!t) return 0;
    size_t head = __atomic_load_n(&t->head, __ATOMIC_RELAXED);
    size_t n = head;
    return n > t->capacity ? t->capacity : n;
}

size_t toy_tracer_dropped(const toy_tracer_t *t) {
    return t ? __atomic_load_n(&t->dropped, __ATOMIC_RELAXED) : 0;
}

/* ── dump ─────────────────────────────────────────────── */

/*
 * Iterate ring buffer from oldest to newest.
 * head is total writes ever. Start index = (head > capacity) ? head - capacity : 0.
 * Read events[(start + i) & mask] for i = 0..count-1.
 *
 * Format per event:
 *   ENTER  hook=1 tid=12345 args=[0x1 0x2] @ 1234567890ns
 *   LEAVE  hook=1 tid=12345 ret=0x42 @ 1234567900ns
 *
 * Show dropped count if > 0.
 */
void toy_tracer_dump(const toy_tracer_t *t, FILE *fp) {
    if (!t || !fp) return;

    size_t head = __atomic_load_n(&t->head, __ATOMIC_RELAXED);
    size_t start = (head > t->capacity) ? (head - t->capacity) : 0;
    size_t count = toy_tracer_count(t);
    size_t dropped = toy_tracer_dropped(t);
    for (size_t i = 0; i < count; i++) {
        trace_event_t ev = t->events[(start + i) & t->mask];
        if (ev.kind == TOY_TRACE_ENTER) {
            fprintf(fp, "ENTER  hook=%lu tid=%lu args=[", ev.hook_id, ev.thread_id);
            for (unsigned j = 0; j < ev.argc; j++) {
                fprintf(fp, "0x%lx", ev.args[j]);
                if (j < ev.argc - 1) fprintf(fp, " ");
            }
            fprintf(fp, "] @ %lu ns\n", ev.timestamp);
        } else if (ev.kind == TOY_TRACE_LEAVE) {
            fprintf(fp, "LEAVE  hook=%lu tid=%lu ret=0x%lx @ %lu ns\n", ev.hook_id, ev.thread_id, ev.ret_val, ev.timestamp);
        }
    }
    if (dropped > 0) {
        fprintf(fp, "Dropped %lu events\n", dropped);
    }
}