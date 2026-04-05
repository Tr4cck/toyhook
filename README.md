# toyhook

[中文](docs/README_CN.md)

A teaching-oriented function hooking framework for ARM64 Android. It exposes internals rather than hiding them — every component (trampoline allocation, instruction relocation, GOT patching) is readable and hackable.

**What it does:**
- **Inline hook** — patches function prologues with a branch to a replacement, relocates stolen instructions into a trampoline
- **PLT/GOT hook** — rewrites GOT entries to intercept dynamic symbol calls
- **Unified dispatch** — a single pipeline routes all hooked calls through before/replace/after handler chains

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
├────────────────────┬─────────────────────────────┤
│  inline_hook.c     │       plt_hook.c             │
│  (prologue patch)  │       (GOT rewrite)          │
├────────────────────┴─────────────────────────────┤
│           arch/arm64/  (backend primitives)        │
│   emit.c — instruction encoding                   │
│   asm.c  — instruction relocation                 │
│   dispatch.c — per-hook assembly stub             │
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
│   ├── emit.c/h         Instruction encoding (STP, LDP, B, BL, LDR literal, ...)
│   ├── asm.c/h          Instruction relocation (PC-relative fixup)
│   └── dispatch.c/h     Per-hook assembly stub (save x0-x7, call toy_dispatch)
├── backend/
│   ├── inline_hook.c    Inline hook backend
│   └── plt_hook.c       PLT/GOT hook backend
├── core/
│   └── toyhook.c        Unified API + dispatch pipeline
├── include/
│   ├── toyhook.h        Public API header
│   ├── inline_hook.h
│   └── plt_hook.h
├── injector/
│   └── injector.c       ptrace-based remote dlopen injector
├── payload/
│   └── payload.c        Example SO (hooks getpid + __system_property_get)
├── utils/
│   ├── elf.c/h          ELF .dynamic parsing (DT_JMPREL, DT_SYMTAB, ...)
│   ├── mem.c/h          RWX page allocation, near allocation
│   └── log.h            Logging macros
├── tests/               102 host unit tests
│   ├── test_inline_hook.c
│   ├── test_plt_hook.c
│   ├── test_toyhook.c
│   └── android/log.h    Mock for __android_log_print
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
        ctx->ret_val = (unsigned long)-1;
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
2. **BEFORE** handlers run (can inspect/modify arguments)
3. **REPLACE** handlers run (can set `skip_original` and `ret_val`)
4. If `skip_original` is not set, original function is called
5. **AFTER** handlers run (can inspect/modify return value)
6. Return value goes back to caller

### Query and control

```c
unsigned long hits = toy_hook_get_hit_count(hook);
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

Each hook gets its own small assembly page that saves callee-saved registers, loads the hook pointer and `toy_dispatch` address via LDR literal, and calls into C. This avoids a global indirect branch table.

## Testing

102 host-side unit tests covering:
- Instruction encoding and decoding
- PC-relative instruction relocation (all branch types, ADRP, LDR literal)
- Trampoline generation
- Dispatch pipeline (before/replace/after handlers, skip_original, hit count)
- PLT/GOT hooking (ELF parsing, GOT patching, roundtrip)
- Near allocation

Tests use a mock-based pattern: `.c` files are `#include`'d directly with stubs replacing arch-specific and platform-specific functions.

## License

Educational / research use.
