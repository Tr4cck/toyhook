#ifndef TOYHOOK_TRACE_H
#define TOYHOOK_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include "toyhook.h"

#define TOY_TRACE_ENTER 1
#define TOY_TRACE_LEAVE 2

typedef struct {
    uint64_t timestamp;
    uint64_t duration;
    uint64_t hook_id;
    uint64_t thread_id;
    uint64_t args[TOY_MAX_ARGS];
    uint32_t argc;
    uint64_t ret_val;
    uint32_t kind;
} trace_event_t;

typedef struct toy_tracer {
    trace_event_t *events;
    size_t capacity;
    size_t mask;
    size_t head;
    size_t dropped;
    int enabled;
} toy_tracer_t;

toy_tracer_t *toy_tracer_create(size_t capacity);
 /* must be power of 2 */
void toy_tracer_destroy(toy_tracer_t *t);


/* Attach tracer to a hook as a BEFORE+after handler pair */
int toy_tracer_attach(toy_tracer_t *t, toy_hook_t *h);

void toy_tracer_detach(toy_tracer_t *t, toy_hook_t *h);

/* control */
void toy_tracer_enable(toy_tracer_t *t);

void toy_tracer_disable(toy_tracer_t *t);

/* query */
size_t toy_tracer_count(const toy_tracer_t *t);
size_t toy_tracer_dropped(const toy_tracer_t *t);

/* dump all recorded events */
void toy_tracer_dump(const toy_tracer_t *t, int fd);



#endif
