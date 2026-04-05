#ifndef TOYHOOK_H
#define TOYHOOK_H

#include <stddef.h>

typedef struct toy_session toy_session_t;
typedef struct toy_hook toy_hook_t;
typedef struct toy_callctx toy_callctx_t;

typedef enum {
    TOY_BACKEND_INLINE,
    TOY_BACKEND_PLT,
} toy_backend_t;

typedef struct {
    toy_backend_t backend;
    union {
        struct { void *addr; } by_addr;
        struct { const char *module; const char *symbol; } by_symbol;
    };
} toy_target_t;

typedef enum {
    TOY_PHASE_BEFORE  = 1 << 0,
    TOY_PHASE_AFTER   = 1 << 1,
    TOY_PHASE_REPLACE = 1 << 2,
} toy_phase_t;

typedef int (*toy_handler_fn)(toy_callctx_t *ctx, void *user_data);

/**
 * toy_handler_t - a hook handler.
 */
typedef struct {
    unsigned phases;
    int priority;
    toy_handler_fn fn;
    void *user_data;
    const char *name;
} toy_handler_t;

#define TOY_MAX_ARGS 8

/**
 * toy_callctx - context for a hook call.
 */
struct toy_callctx {
    toy_hook_t *hook;
    void *target_addr;
    void *original_addr;

    unsigned long args[TOY_MAX_ARGS];
    unsigned argc;
    unsigned long ret_val;

    int skip_original;
};

toy_session_t *toy_session_create(void);
void toy_session_destroy(toy_session_t *s);

toy_hook_t *toy_hook_add(toy_session_t *s, const toy_target_t *t);
int toy_hook_remove(toy_session_t *s, toy_hook_t *h);

int toy_hook_on(toy_hook_t *h, const toy_handler_t *handler);
int toy_hook_off(toy_hook_t *h, const char *name);

int toy_hook_enable(toy_hook_t *h);
int toy_hook_disable(toy_hook_t *h);
unsigned long toy_hook_get_hit_count(toy_hook_t *h);

int toy_commit(toy_session_t *s);

#endif
