# toyhook Usage Guide

## Conceptual Model

```
session  →  manages the lifecycle of hooks
hook     →  an interception point (PLT or inline)
handler  →  callback after interception (attached to BEFORE / REPLACE / AFTER phases)
```

## Quick Start

### 1. Create a session

```c
#include "toyhook.h"

toy_session_t *sess = toy_session_create();
```

A session is the container for all hooks. Destroying it automatically cleans up every hook.

### 2. Define a target

The target decides "where to hook" and "how to hook":

```c
// PLT hook — intercept calls to an external symbol from a specific .so
toy_target_t t = {
    .backend = TOY_BACKEND_PLT,
    .by_symbol = {
        .module = "libtarget.so",
        .symbol = "open",
    },
};

// inline hook — patch the function prologue directly
toy_target_t t = {
    .backend = TOY_BACKEND_INLINE,
    .by_addr = { .addr = some_function_pointer },
};
```

**Difference:**
- `TOY_BACKEND_PLT`: rewrites a GOT entry, only affects calls to that symbol from the specified .so
- `TOY_BACKEND_INLINE`: rewrites function entry instructions, affects all callers

### 3. Add a hook

```c
toy_hook_t *hook = toy_hook_add(sess, &t);
```

### 4. Register a handler

```c
static int my_handler(toy_callctx_t *ctx, void *user_data) {
    // your logic
    return 0;
}

toy_handler_t h = {
    .phases   = TOY_PHASE_BEFORE,
    .fn       = my_handler,
    .name     = "my_handler",     // used later for toy_hook_off
    .priority = 0,                // lower runs first, default 0
};
toy_hook_on(hook, &h);
```

### 5. Enable the hook

```c
toy_hook_enable(hook);
// or batch-enable all hooks with handlers:
toy_commit(sess);
```

### 6. Cleanup

```c
toy_hook_disable(hook);       // single hook
toy_session_destroy(sess);    // all hooks (auto-disables everything)
```

## Handler Phases

### BEFORE — runs before the original function

Use for: observing/modifying arguments

```c
static int before_open(toy_callctx_t *ctx, void *ud) {
    const char *path = (const char *)ctx->args[0];
    int flags = (int)ctx->args[1];
    LOG("open(\"%s\", 0x%x)", path, flags);

    // you can modify arguments
    // ctx->args[0] = (unsigned long)"/dev/null";

    return 0;  // continue
}
```

### REPLACE — replaces the original function

Use for: full control over function behavior. **You must set `skip_original = 1` or call the original function yourself via `ctx->original_addr`.**

```c
// Pattern A: call original, modify return value
static int replace_open(toy_callctx_t *ctx, void *ud) {
    typedef int (*orig_fn)(const char *, int, ...);
    orig_fn orig = (orig_fn)ctx->original_addr;

    int fd = orig((const char *)ctx->args[0], (int)ctx->args[1]);
    LOG("original open returned %d", fd);

    ctx->ret_val = (unsigned long)fd;
    ctx->skip_original = 1;  // tell dispatch not to call original again
    return 0;
}

// Pattern B: full replacement, skip original entirely
static int replace_getpid(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 1337;
    ctx->skip_original = 1;
    return 0;
}
```

### AFTER — runs after the original function

Use for: observing/modifying return value

```c
static int after_open(toy_callctx_t *ctx, void *ud) {
    LOG("open returned fd=%d", (int)ctx->ret_val);
    // ctx->ret_val = -1;  // you can tamper with the return value
    return 0;
}
```

## Handler Execution Order

Multiple handlers can be registered on the same hook. They execute sorted by `priority`:

```c
toy_handler_t h1 = { .priority = 10, .phases = TOY_PHASE_BEFORE, .fn = log_args,  .name = "log" };
toy_handler_t h2 = { .priority = 20, .phases = TOY_PHASE_BEFORE, .fn = mod_args,  .name = "mod" };
toy_hook_on(hook, &h1);
toy_hook_on(hook, &h2);
// execution order: h1(10) -> h2(20)
```

Any handler returning non-zero aborts subsequent handlers in the current phase.

## toy_callctx_t Field Reference

```c
struct toy_callctx {
    toy_hook_t *hook;          // the hook that fired
    void *target_addr;         // address of the hooked function
    void *original_addr;       // original function address (for calling original)

    unsigned long args[8];     // function arguments (x0-x7)
    unsigned argc;             // argument count
    unsigned long ret_val;     // return value

    int skip_original;         // set to 1 to skip original function call
};
```

## Common Mistakes

### Forgetting skip_original

```c
// Wrong! REPLACE handler doesn't set skip_original,
// so dispatch will call the original function again
static int bad_handler(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 42;
    // missing: ctx->skip_original = 1;
    return 0;
}
```

### Calling a NULL original function pointer

```c
// Wrong! Don't save the original function pointer yourself
static int (*orig_fn)(void) = NULL;  // always NULL

static int handler(toy_callctx_t *ctx, void *ud) {
    orig_fn();  // crash!
}

// Correct: call through ctx->original_addr
static int handler(toy_callctx_t *ctx, void *ud) {
    typedef int (*fn_t)(void);
    fn_t orig = (fn_t)ctx->original_addr;
    orig();
    ctx->skip_original = 1;
    return 0;
}
```

## Full Example: Injected Payload

```c
#include <android/log.h>
#include <dlfcn.h>
#include "toyhook.h"

#define TAG "demo"

static int on_prop_get(toy_callctx_t *ctx, void *ud) {
    typedef int (*fn_t)(const char *, char *);
    fn_t orig = (fn_t)ctx->original_addr;

    const char *name = (const char *)ctx->args[0];
    char *value = (char *)ctx->args[1];
    int res = orig(name, value);

    __android_log_print(ANDROID_LOG_INFO, TAG,
                        "__system_property_get(\"%s\") = \"%s\" (%d)",
                        name, value, res);

    ctx->ret_val = (unsigned long)res;
    ctx->skip_original = 1;
    return 0;
}

__attribute__((constructor))
static void on_load(void) {
    toy_session_t *sess = toy_session_create();

    toy_target_t tgt = {
        .backend = TOY_BACKEND_PLT,
        .by_symbol = {
            .module = "libtarget.so",
            .symbol = "__system_property_get",
        },
    };

    toy_hook_t *hook = toy_hook_add(sess, &tgt);
    if (hook) {
        toy_handler_t h = {
            .phases = TOY_PHASE_REPLACE,
            .fn = on_prop_get,
            .name = "log_prop",
        };
        toy_hook_on(hook, &h);
        toy_hook_enable(hook);
    }
}

__attribute__((destructor))
static void on_unload(void) {
    // toy_session_destroy auto-disables all hooks
}
```

Inject into a target process with the injector:

```bash
./toyhook inject <pid> /path/to/libyour_payload.so
```
