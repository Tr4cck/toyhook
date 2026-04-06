# toyhook

[中文](README_CN.md)

A teaching-oriented function hooking framework for ARM64 Android. It exposes internals rather than hiding them — every component (trampoline allocation, instruction relocation, GOT patching) is readable and hackable.

**What it does:**
- **Inline hook** — patches function prologues with a branch to a replacement, relocates stolen instructions into a trampoline
- **PLT/GOT hook** — rewrites GOT entries to intercept dynamic symbol calls
- **Unified dispatch** — a single pipeline routes all hooked calls through before/replace/after handler chains
- **Ring-buffer tracer** — records enter/leave events with args, return values, and call duration, queryable on-demand
- **UDS control channel** — interactive `toyhookctl` REPL for querying hook status, streaming trace events, and live watch with filtering
- **Re-entrant safe** — recursive hook invocation (e.g. handler calls a function that triggers the same hook) is transparently handled by the dispatch pipeline

**What it doesn't do:**
- Multi-arch (ARM64 only)
- Thread safety during enable/disable
- Production hardening (no locks, no signal safety)

## Quick Start

### Prerequisites

- Android NDK (set `ANDROID_NDK_ROOT`)
- `adb` in PATH (for device deployment)
- CMake 3.10+

### Build for Android

```bash
bash scripts/build.sh --ndk $ANDROID_NDK_ROOT --push
```

### Inject into a target process

```bash
adb shell toyhook inject <pid> /data/local/tmp/libtoyhook_payload.so
```

### Query hooks at runtime

```bash
# One-shot commands (for scripting)
adb shell toyhookctl status
adb shell toyhookctl count
adb shell toyhookctl dump
adb shell toyhookctl "watch hook=1"

# Interactive REPL
adb shell toyhookctl
toyhook> status
toyhook> watch hook=1
toyhook> dump
toyhook> quit
```

### Run host tests

```bash
bash scripts/test.sh
```

## Architecture

```
┌──────────────────────────────────────────────────┐
│                  user payload                     │
│         toy_hook_on / toy_commit / ...            │
├──────────────────────────────────────────────────┤
│              core/toyhook.c  (unified API)        │
│    session → hook → handler → dispatch pipeline   │
│    (includes re-entrant depth guard)              │
├────────────────────┬─────────────────────────────┤
│  inline_hook.c     │       plt_hook.c             │
│  (prologue patch)  │       (GOT rewrite)          │
├────────────────────┴─────────────────────────────┤
│           arch/arm64/  (backend primitives)        │
│   emit.c — instruction encoding                   │
│   asm.c  — instruction relocation                 │
│   dispatch.c — per-hook assembly stub             │
├──────────────────────────────────────────────────┤
│   core/trace.c   (ring-buffer event recorder)     │
├──────────────────────────────────────────────────┤
│   core/server.c  (UDS control server)             │
│   client/toyhookctl.c  (CLI control client)       │
├──────────────────────────────────────────────────┤
│   utils/  mem.c (RWX allocation, near alloc)      │
│           elf.c (ELF section parsing)             │
├──────────────────────────────────────────────────┤
│            injector/  (ptrace dlopen injector)     │
└──────────────────────────────────────────────────┘
```

### Project layout

```
.
├── arch/arm64/          ARM64 low-level primitives
│   ├── emit.c/h         Instruction encoding (STP, LDP_post, B, BL, LDR literal, ...)
│   ├── asm.c/h          Instruction relocation (PC-relative fixup)
│   └── dispatch.c/h     Per-hook assembly stub (save x0-x7, call toy_dispatch)
├── backend/
│   ├── inline_hook.c    Inline hook backend
│   └── plt_hook.c       PLT/GOT hook backend
├── core/
│   ├── toyhook.c        Unified API + dispatch pipeline (re-entrant guard)
│   ├── trace.c          Ring-buffer trace recorder
│   └── server.c         UDS control server (abstract namespace)
├── client/
│   └── toyhookctl.c     CLI client for querying hooks at runtime
├── include/
│   ├── toyhook.h        Public API header
│   ├── trace.h          Tracer API header
│   ├── server.h         Server context + socket name
│   ├── inline_hook.h
│   └── plt_hook.h
├── injector/
│   └── injector.c       ptrace-based remote dlopen injector
├── payload/
│   └── payload.c        Example SO (PLT hook + inline hook + tracer + server)
├── utils/
│   ├── elf.c/h          ELF .dynamic parsing (DT_JMPREL, DT_SYMTAB, ...)
│   ├── mem.c/h          RWX page allocation, near allocation
│   └── log.h            Logging macros
├── tests/               119 host unit tests
│   ├── test_inline_hook.c
│   ├── test_plt_hook.c
│   ├── test_toyhook.c
│   ├── test_trace.c
│   └── android/log.h    Mock for __android_log_print
├── docs/
│   ├── usage-guide.md       Usage guide (Chinese)
│   └── usage-guide.en.md    Usage guide (English)
└── scripts/
    ├── build.sh         Cross-compile for Android + optional push
    ├── test.sh          Build and run host tests
    └── debug.sh         Attach logcat for payload debugging
```

## API

### Session lifecycle

```c
toy_session_t *sess = toy_session_create();
// ... add hooks, attach handlers, commit ...
toy_session_destroy(sess);
```

### Hook a function by address (inline)

```c
toy_target_t target = {
    .backend = TOY_BACKEND_INLINE,
    .by_addr = { .addr = my_function },
};
toy_hook_t *hook = toy_hook_add(sess, &target);
```

### Hook a function by symbol (PLT/GOT)

```c
toy_target_t target = {
    .backend = TOY_BACKEND_PLT,
    .by_symbol = { .module = "libc.so", .symbol = "open" },
};
toy_hook_t *hook = toy_hook_add(sess, &target);
```

### Attach handlers

```c
static int log_args(toy_callctx_t *ctx, void *ud) {
    printf("arg0 = 0x%lx\n", ctx->args[0]);
    return 0;
}

static int deny_access(toy_callctx_t *ctx, void *ud) {
    const char *path = (const char *)ctx->args[0];
    if (path && strstr(path, "secret")) {
        ctx->ret_val = (uint64_t)-1;
        ctx->skip_original = 1;
    }
    return 0;
}

toy_handler_t h1 = {
    .phases = TOY_PHASE_BEFORE,
    .priority = 10,
    .fn = log_args,
    .name = "log_args",
};
toy_handler_t h2 = {
    .phases = TOY_PHASE_BEFORE,
    .priority = 20,
    .fn = deny_access,
    .name = "deny_access",
};

toy_hook_on(hook, &h1);
toy_hook_on(hook, &h2);
toy_hook_enable(hook);
```

### Dispatch pipeline

When a hooked function is called:

1. Assembly stub saves `x0`-`x7`, calls `toy_dispatch(hook, args, 8)`
2. Re-entrant check: if already dispatching on this thread, skip handlers and call original directly
3. **BEFORE** handlers run (can inspect/modify arguments)
4. **REPLACE** handlers run (can set `skip_original` and `ret_val`)
5. If `skip_original` is not set, original function is called
6. **AFTER** handlers run (can inspect/modify return value)
7. Return value goes back to caller

### Tracer

```c
toy_tracer_t *tracer = toy_tracer_create(1024);
toy_tracer_attach(tracer, hook);
```

Events are recorded to a lock-free ring buffer. Each LEAVE event carries a `duration` (elapsed ns from the matching ENTER). Query at runtime:

```bash
toyhookctl dump    # dump all recorded events
toyhookctl count   # show event count and dropped count
```

### Control channel

The payload starts a UDS server on an abstract namespace socket. Connect with `toyhookctl`:

```
Commands: dump | status | count | watch [hook=N] [tid=N] | help | quit
```

- `status` — print all hooks and their state
- `count`  — show tracer event/dropped counts
- `dump`   — dump all recorded trace events
- `watch [hook=N] [tid=N]` — live-stream trace events, optional filter by hook id or thread id
- `quit`   — disconnect

`toyhookctl` can run as a one-shot command (`toyhookctl dump`) or as an interactive REPL (`toyhookctl` with no args).

### Query and control

```c
uint64_t hits = toy_hook_get_hit_count(hook);
toy_hook_disable(hook);
toy_hook_remove(sess, hook);
toy_commit(sess);   // enable all hooks with handlers
```

## Key Design Decisions

### Near allocation (`alloc_rwx_near`)

Inline hook trampolines must be within ±128MB of the target function for ARM64 `B`/`BL` instructions. `alloc_rwx_near()` parses `/proc/self/maps`, sweeps outward from the target address, and uses `mmap` with `MAP_FIXED_NOREPLACE` to claim gaps between existing mappings.

### Instruction relocation (`relocate_instruction`)

Stolen instructions are not just `memcpy`'d to the trampoline. PC-relative instructions (ADRP, B, BL, CBZ, CBNZ, TBZ, TBNZ, B.cond, LDR literal) are decoded and their offsets rewritten for the new location.

### Emit helpers (`emit_*`)

ARM64 instructions are generated via small encoding functions (`emit_stp_pre`, `emit_ldr_literal`, `emit_blr`, etc.) instead of hardcoded hex. This makes the dispatch stub and trampoline code readable and maintainable.

### Dispatch stub (per-hook)

Each hook gets its own small assembly page that saves argument registers (x0-x7) and frame registers (FP, LR), loads the hook pointer and `toy_dispatch` address via LDR literal, and calls into C. This avoids a global indirect branch table.

### Re-entrant dispatch

`toy_dispatch` uses a thread-local depth counter. If a handler (or the original function) triggers the same hook recursively, the nested invocation skips all handlers and calls the original function directly. Users never need to worry about recursive dispatch protection.

### Abstract UDS control channel

On Android, untrusted apps can't create TCP sockets (seccomp) or filesystem sockets (SELinux cross-UID). The control server uses abstract namespace UDS (`AF_UNIX` with `sun_path[0] = '\0'`) which bypasses both restrictions, allowing `adb shell` to connect.

## Testing

119 host-side unit tests covering:
- Instruction encoding and decoding
- PC-relative instruction relocation (all branch types, ADRP, LDR literal)
- Trampoline generation
- Dispatch pipeline (before/replace/after handlers, skip_original, hit count)
- Tracer (enter/leave events, duration tracking, ring buffer)
- PLT/GOT hooking (ELF parsing, GOT patching, roundtrip)
- Near allocation

Tests use a mock-based pattern: `.c` files are `#include`'d directly with stubs replacing arch-specific and platform-specific functions.

## License

Educational / research use.
