#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"
#include "test_framework.h"

/* ── stubs for toyhook API ─────────────────────────────── */

static unsigned long stub_hook_id = 1;
unsigned long toy_hook_get_id(toy_hook_t *h) {
    (void)h;
    return stub_hook_id;
}

static int stub_hook_on_ret = 0;
int toy_hook_on(toy_hook_t *h, const toy_handler_t *handler) {
    (void)h;
    return stub_hook_on_ret;
}

static void *stub_hook_off_ud = NULL;
void *toy_hook_off(toy_hook_t *h, const char *name) {
    (void)h; (void)name;
    return stub_hook_off_ud;
}

static void reset_stubs(void) {
    stub_hook_id = 1;
    stub_hook_on_ret = 0;
    stub_hook_off_ud = NULL;
}

/* ── include code under test ──────────────────────────────── */

#include "../core/trace.c"

/* ── tests ─────────────────────────────────────────────────── */

TEST(tracer_create_destroy) {
    toy_tracer_t *t = toy_tracer_create(64);
    ASSERT_TRUE(t != NULL);
    ASSERT_TRUE(t->events != NULL);
    ASSERT_INT((int)t->capacity, 64);
    ASSERT_INT((int)t->mask, 63);
    ASSERT_INT(t->enabled, 1);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_create_round_up) {
    toy_tracer_t *t = toy_tracer_create(100);
    ASSERT_TRUE(t != NULL);
    size_t cap = 1;
    while (cap < 100) cap <<= 1;
    ASSERT_INT((int)t->capacity, (int)cap);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_create_null_safe) {
    toy_tracer_destroy(NULL);
    return 0;
}

TEST(tracer_record_single) {
    toy_tracer_t *t = toy_tracer_create(4);
    trace_event_t ev = {
        .kind = TOY_TRACE_ENTER,
        .hook_id = 1,
        .timestamp = 1000,
    };
    tracer_record(t, &ev);
    ASSERT_INT((int)t->head, 1);
    ASSERT_INT((int)t->events[0].kind, TOY_TRACE_ENTER);
    ASSERT_INT((int)t->events[0].hook_id, 1);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_record_wraps) {
    toy_tracer_t *t = toy_tracer_create(4);
    for (int i = 0; i < 6; i++) {
        trace_event_t ev = { .kind = TOY_TRACE_ENTER, .hook_id = (unsigned long)i };
        tracer_record(t, &ev);
    }
    ASSERT_INT((int)t->head, 6);
    ASSERT_INT((int)toy_tracer_count(t), 4);
    ASSERT_INT((int)toy_tracer_dropped(t), 2);

    size_t idx = t->head & t->mask;
    ASSERT_INT((int)t->events[idx].hook_id, 4);
    ASSERT_INT((int)t->events[(idx + 1) & t->mask].hook_id, 5);
    ASSERT_INT((int)t->events[(idx + 2) & t->mask].hook_id, 2);
    ASSERT_INT((int)t->events[(idx + 3) & t->mask].hook_id, 3);

    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_disabled_ignores_record) {
    toy_tracer_t *t = toy_tracer_create(4);
    toy_tracer_disable(t);
    trace_event_t ev = { .kind = TOY_TRACE_ENTER, .hook_id = 42 };
    tracer_record(t, &ev);
    ASSERT_INT((int)t->head, 0);
    toy_tracer_enable(t);
    tracer_record(t, &ev);
    ASSERT_INT((int)t->head, 1);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_count_empty) {
    toy_tracer_t *t = toy_tracer_create(4);
    ASSERT_INT((int)toy_tracer_count(t), 0);
    ASSERT_INT((int)toy_tracer_dropped(t), 0);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_count_null_safe) {
    ASSERT_INT((int)toy_tracer_count(NULL), 0);
    ASSERT_INT((int)toy_tracer_dropped(NULL), 0);
    return 0;
}

TEST(tracer_attach_success) {
    reset_stubs();
    toy_tracer_t *t = toy_tracer_create(16);
    int ret = toy_tracer_attach(t, (toy_hook_t *)1);
    ASSERT_INT(ret, 0);
    toy_tracer_detach(t, (toy_hook_t *)1);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_attach_null_args) {
    reset_stubs();
    ASSERT_INT(toy_tracer_attach(NULL, (toy_hook_t *)1), -1);
    ASSERT_INT(toy_tracer_attach((toy_tracer_t*)1, NULL), -1);
    return 0;
}

TEST(tracer_attach_hook_on_fail) {
    reset_stubs();
    stub_hook_on_ret = -1;
    toy_tracer_t *t = toy_tracer_create(16);
    int ret = toy_tracer_attach(t, (toy_hook_t *)1);
    ASSERT_INT(ret, -1);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_dump_output) {
    toy_tracer_t *t = toy_tracer_create(16);
    trace_event_t ev1 = {
        .kind = TOY_TRACE_ENTER,
        .hook_id = 1,
        .thread_id = 100,
        .argc = 2,
        .args = { 0xA, 0xB },
        .timestamp = 5000,
    };
    trace_event_t ev2 = {
        .kind = TOY_TRACE_LEAVE,
        .hook_id = 1,
        .thread_id = 100,
        .ret_val = 0x42,
        .timestamp = 6000,
    };
    tracer_record(t, &ev1);
    tracer_record(t, &ev2);

    char buf[1024] = {0};
    FILE *fp = fmemopen(buf, sizeof(buf), "w");
    toy_tracer_dump(t, fp);
    fclose(fp);

    ASSERT_TRUE(strstr(buf, "ENTER") != NULL);
    ASSERT_TRUE(strstr(buf, "LEAVE") != NULL);
    ASSERT_TRUE(strstr(buf, "hook=1") != NULL);
    ASSERT_TRUE(strstr(buf, "0x42") != NULL);
    ASSERT_TRUE(strstr(buf, "0xa") != NULL || strstr(buf, "0xA") != NULL);

    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_dump_dropped) {
    toy_tracer_t *t = toy_tracer_create(2);
    for (int i = 0; i < 5; i++) {
        trace_event_t ev = { .kind = TOY_TRACE_ENTER, .hook_id = (unsigned long)i };
        tracer_record(t, &ev);
    }

    char buf[1024] = {0};
    FILE *fp = fmemopen(buf, sizeof(buf), "w");
    toy_tracer_dump(t, fp);
    fclose(fp);

    ASSERT_TRUE(strstr(buf, "Dropped") != NULL);
    toy_tracer_destroy(t);
    return 0;
}

TEST(tracer_dump_null_safe) {
    toy_tracer_dump(NULL, stdout);
    toy_tracer_dump(NULL, NULL);
    return 0;
}

/* ── main ──────────────────────────────────────────────────── */

int main(void) {
    printf("trace tests:\n");

    RUN_TEST(tracer_create_destroy);
    RUN_TEST(tracer_create_round_up);
    RUN_TEST(tracer_create_null_safe);
    RUN_TEST(tracer_record_single);
    RUN_TEST(tracer_record_wraps);
    RUN_TEST(tracer_disabled_ignores_record);
    RUN_TEST(tracer_count_empty);
    RUN_TEST(tracer_count_null_safe);
    RUN_TEST(tracer_attach_success);
    RUN_TEST(tracer_attach_null_args);
    RUN_TEST(tracer_attach_hook_on_fail);
    RUN_TEST(tracer_dump_output);
    RUN_TEST(tracer_dump_dropped);
    RUN_TEST(tracer_dump_null_safe);

    printf("\n%d/%d passed, %d failed\n", __tf_pass, __tf_total, __tf_fail);
    return __tf_fail > 0 ? 1 : 0;
}
