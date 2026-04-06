# toyhook Usage Guide

## Conceptual Model

```
session  →  manages the lifecycle of hooks
hook     →  an interception point (PLT or inline)
handler  →  callback after interception (attached to BEFORE / REPLACE / AFTER phases)
tracer   →  ring-buffer event recorder attached to hooks
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
    // ctx->args[0] = (uint64_t)"/dev/null";

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

    ctx->ret_val = (uint64_t)fd;
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

## Re-entrant Dispatch

If a handler (or the original function) triggers the same hook again — e.g. your BEFORE handler calls `LOGI()` which internally calls the hooked `__system_property_get` — the framework handles this automatically:

- `toy_dispatch` uses a per-thread depth counter
- Nested invocations skip all handlers and call the original function directly
- No user code or guard variables are needed

## toy_callctx_t Field Reference

```c
struct toy_callctx {
    toy_hook_t *hook;          // the hook that fired
    void *target_addr;         // address of the hooked function
    void *original_addr;       // original function address (for calling original)

    uint64_t args[8];          // function arguments (x0-x7)
    unsigned argc;             // argument count
    uint64_t ret_val;          // return value

    int skip_original;         // set to 1 to skip original function call
};
```

## Tracer

The tracer records hook enter/leave events to a lock-free ring buffer.

### Create and attach

```c
toy_tracer_t *tracer = toy_tracer_create(1024);  // capacity must be power of 2
toy_tracer_attach(tracer, hook);
```

Attaching registers BEFORE + AFTER handlers that record `trace_event_t` entries:

```c
typedef struct {
    uint64_t timestamp;     // event timestamp (ns)
    uint64_t duration;      // elapsed ns (0 for ENTER, filled for LEAVE)
    uint64_t hook_id;
    uint64_t thread_id;
    uint64_t args[TOY_MAX_ARGS];
    uint32_t argc;
    uint64_t ret_val;
    uint32_t kind;          // TOY_TRACE_ENTER or TOY_TRACE_LEAVE
} trace_event_t;
```

### Query

```c
size_t count   = toy_tracer_count(tracer);
size_t dropped = toy_tracer_dropped(tracer);
```

### Dump

```c
toy_tracer_dump(tracer, fileno(stderr));   // dump events to any fd
```

### Enable / disable recording

```c
toy_tracer_disable(tracer);   // stop recording events
toy_tracer_enable(tracer);    // resume
```

## Control Channel (toyhookctl)

The payload starts a UDS server thread that listens on an abstract namespace socket. You can query hook state and trace data at runtime from `adb shell`.

### Start the server

In your payload's `on_load`:

```c
#include <pthread.h>
#include "server.h"

static toyhook_server_ctx_t server_ctx = {0};
server_ctx.session = sess;
server_ctx.tracer  = tracer;
pthread_t tid;
pthread_create(&tid, NULL, toyhook_server_run, &server_ctx);
pthread_detach(tid);
```

### Use toyhookctl

One-shot mode (for scripting):

```bash
adb shell toyhookctl status   # print all hooks and their state
adb shell toyhookctl count    # show event count and dropped count
adb shell toyhookctl dump     # dump all recorded trace events
adb shell toyhookctl help     # list commands
adb shell toyhookctl quit     # disconnect
```

Interactive REPL mode:

```bash
adb shell toyhookctl
toyhook> status
Hook #1
    target       : libmyapplication.so!__system_property_get
    backend      : plt
    ...
OK
toyhook> watch hook=1
WATCHING
ENTER  hook=1 tid=12345 args=[0x7f...] dur=0 @ 1234567890
LEAVE  hook=1 tid=12345 ret=0x42 dur=1200ns @ 1234567900
^C
STOPPED
toyhook> quit
bye
OK
```

### Protocol

Text-based line protocol over abstract UDS:

```
> dump
ENTER  hook=1 tid=12345 args=[0x7f...] dur=0 @ 1234567890
LEAVE  hook=1 tid=12345 ret=0x42 dur=1200ns @ 1234567900
OK

> status
Hook #1
    target       : libmyapplication.so!__system_property_get
    backend      : plt
    ...
OK

> count
events=256 dropped=3
OK

> watch hook=1 tid=12345
WATCHING
ENTER  hook=1 tid=12345 args=[0x7f...] dur=0 @ 1234567890
LEAVE  hook=1 tid=12345 ret=0x42 dur=1200ns @ 1234567900
> stop
STOPPED
OK
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
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <pthread.h>
#include "toyhook.h"
#include "trace.h"
#include "server.h"
#include "utils/log.h"

static toy_session_t *g_sess;
static toy_tracer_t  *g_tracer;

static int on_prop_get(toy_callctx_t *ctx, void *ud) {
    typedef int (*fn_t)(const char *, char *);
    fn_t orig = (fn_t)ctx->original_addr;

    const char *name = (const char *)ctx->args[0];
    char *value = (char *)ctx->args[1];
    int res = orig(name, value);

    LOGI("__system_property_get(\"%s\") = \"%s\" (%d)", name, value, res);

    ctx->ret_val = (uint64_t)res;
    ctx->skip_original = 1;
    return 0;
}

__attribute__((constructor))
static void on_load(void) {
    g_sess = toy_session_create();

    g_tracer = toy_tracer_create(1024);

    toy_target_t tgt = {
        .backend = TOY_BACKEND_PLT,
        .by_symbol = {
            .module = "libtarget.so",
            .symbol = "__system_property_get",
        },
    };

    toy_hook_t *hook = toy_hook_add(g_sess, &tgt);
    if (hook) {
        toy_handler_t h = {
            .phases = TOY_PHASE_REPLACE,
            .fn = on_prop_get,
            .name = "log_prop",
        };
        toy_hook_on(hook, &h);
    }

    if (g_tracer && hook)
        toy_tracer_attach(g_tracer, hook);

    toy_commit(g_sess);
    toy_session_describe(g_sess, fileno(stderr));

    static toyhook_server_ctx_t server_ctx = {0};
    server_ctx.session = g_sess;
    server_ctx.tracer  = g_tracer;
    pthread_t tid;
    pthread_create(&tid, NULL, toyhook_server_run, &server_ctx);
    pthread_detach(tid);
}
```

Inject and query:

```bash
./toyhook inject <pid> /data/local/tmp/libyour_payload.so
toyhookctl status
toyhookctl dump
toyhookctl watch
```
