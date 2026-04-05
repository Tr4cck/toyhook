# toyhook

[English](README.md)

一个面向教学的 ARM64 Android 函数 hook 框架。不隐藏细节，而是把每个组件（trampoline 分配、指令重定位、GOT 改写）做成可读、可改的样子。

**能做什么：**
- **Inline hook** — 改写函数入口，把被覆盖的指令搬到 trampoline
- **PLT/GOT hook** — 改写 GOT 表项，拦截动态符号调用
- **统一 dispatch** — 所有被 hook 的调用经过同一个 before/replace/after handler 链
- **环形缓冲区追踪器** — 记录 enter/leave 事件及参数和返回值，支持按需查询
- **UDS 控制通道** — 通过 `toyhookctl` CLI 实时查询 hook 状态和追踪数据
- **重入安全** — handler 中递归触发同一 hook 时（如 handler 调用的函数又走了被 hook 的符号），dispatch 管线自动跳过 handler、直接调用原函数，用户无需关心

**不能做什么：**
- 多架构支持（仅 ARM64）
- enable/disable 期间的线程安全
- 生产级加固（无锁、无信号安全）

## 快速开始

### 前置条件

- Android NDK（设置 `ANDROID_NDK_ROOT`）
- `adb` 在 PATH 中
- CMake 3.10+

### 交叉编译

```bash
bash scripts/build.sh --ndk $ANDROID_NDK_ROOT --push
```

### 注入目标进程

```bash
adb shell toyhook inject <pid> /data/local/tmp/libtoyhook_payload.so
```

### 运行时查询

```bash
adb shell toyhookctl status
adb shell toyhookctl count
adb shell toyhookctl dump
```

### 运行本地测试

```bash
bash scripts/test.sh
```

## 架构

```
┌──────────────────────────────────────────────────┐
│                  用户 payload                      │
│         toy_hook_on / toy_commit / ...            │
├──────────────────────────────────────────────────┤
│              core/toyhook.c  (统一 API)            │
│    session → hook → handler → dispatch 管线       │
│    （内置线程局部重入深度保护）                      │
├────────────────────┬─────────────────────────────┤
│  inline_hook.c     │       plt_hook.c             │
│  (入口指令改写)     │       (GOT 改写)             │
├────────────────────┴─────────────────────────────┤
│           arch/arm64/  (底层原语)                   │
│   emit.c — 指令编码                                │
│   asm.c  — 指令重定位                              │
│   dispatch.c — 每个 hook 的汇编 stub               │
├──────────────────────────────────────────────────┤
│   core/trace.c       (环形缓冲区事件记录器)         │
├──────────────────────────────────────────────────┤
│   core/server.c      (UDS 控制服务端)              │
│   client/toyhookctl.c (CLI 控制客户端)             │
├──────────────────────────────────────────────────┤
│   utils/  mem.c (RWX 分配、就近分配)               │
│           elf.c (ELF 段解析)                       │
├──────────────────────────────────────────────────┤
│            injector/  (ptrace dlopen 注入器)        │
└──────────────────────────────────────────────────┘
```

### 目录结构

```
.
├── arch/arm64/          ARM64 底层原语
│   ├── emit.c/h         指令编码 (STP, LDP, B, BL, LDR literal, ...)
│   ├── asm.c/h          指令重定位 (PC-relative 修正)
│   └── dispatch.c/h     每个 hook 的汇编 stub (保存 x0-x7, 调用 toy_dispatch)
├── backend/
│   ├── inline_hook.c    Inline hook 后端
│   └── plt_hook.c       PLT/GOT hook 后端
├── core/
│   ├── toyhook.c        统一 API + dispatch 管线（含重入保护）
│   ├── trace.c          环形缓冲区追踪器
│   └── server.c         UDS 控制服务端（抽象命名空间）
├── client/
│   └── toyhookctl.c     CLI 客户端，运行时查询 hook 状态
├── include/
│   ├── toyhook.h        公共 API 头文件
│   ├── trace.h          追踪器 API 头文件
│   ├── server.h         服务端上下文 + socket 名称
│   ├── inline_hook.h
│   └── plt_hook.h
├── injector/
│   └── injector.c       基于 ptrace 的远程 dlopen 注入器
├── payload/
│   └── payload.c        示例 SO (PLT hook + inline hook + tracer + server)
├── utils/
│   ├── elf.c/h          ELF .dynamic 解析 (DT_JMPREL, DT_SYMTAB, ...)
│   ├── mem.c/h          RWX 页分配、就近分配
│   └── log.h            日志宏
├── tests/               102 个本地单元测试
│   ├── test_inline_hook.c
│   ├── test_plt_hook.c
│   ├── test_toyhook.c
│   ├── test_trace.c
│   └── android/log.h    __android_log_print 的 mock
├── docs/
│   ├── usage-guide.md       使用指南（中文）
│   ├── usage-guide.en.md    使用指南（英文）
│   └── plan.md              功能路线图
└── scripts/
    ├── build.sh         Android 交叉编译 + 可选推送到设备
    ├── test.sh          构建并运行本地测试
    └── debug.sh         attach logcat 调试 payload
```

## API

### Session 生命周期

```c
toy_session_t *sess = toy_session_create();
// ... 添加 hook、挂载 handler、提交 ...
toy_session_destroy(sess);
```

### 按地址 hook（inline）

```c
toy_target_t target = {
    .backend = TOY_BACKEND_INLINE,
    .by_addr = { .addr = my_function },
};
toy_hook_t *hook = toy_hook_add(sess, &target);
```

### 按符号 hook（PLT/GOT）

```c
toy_target_t target = {
    .backend = TOY_BACKEND_PLT,
    .by_symbol = { .module = "libc.so", .symbol = "open" },
};
toy_hook_t *hook = toy_hook_add(sess, &target);
```

### 挂载 handler

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

### Dispatch 管线

当被 hook 的函数被调用时：

1. 汇编 stub 保存 `x0`-`x7`，调用 `toy_dispatch(hook, args, 8)`
2. 重入检查：如果当前线程已在 dispatch 中，跳过所有 handler，直接调用原函数
3. **BEFORE** handler 执行（可检查/修改参数）
4. **REPLACE** handler 执行（可设置 `skip_original` 和 `ret_val`）
5. 如果 `skip_original` 未设置，调用原函数
6. **AFTER** handler 执行（可检查/修改返回值）
7. 返回值传回调用者

### 追踪器

```c
toy_tracer_t *tracer = toy_tracer_create(1024);
toy_tracer_attach(tracer, hook);
```

事件记录到无锁环形缓冲区，运行时查询：

```bash
toyhookctl dump    # 导出所有记录的事件
toyhookctl count   # 显示事件数和丢弃数
```

### 控制通道

payload 启动后会在抽象命名空间 UDS 上监听，用 `toyhookctl` 连接：

```
Commands: dump | status | count | help | quit
```

- `status` — 打印所有 hook 及其状态
- `count`  — 显示追踪器事件数 / 丢弃数
- `dump`   — 导出所有追踪事件
- `quit`   — 断开连接

### 查询与控制

```c
unsigned long hits = toy_hook_get_hit_count(hook);
toy_hook_disable(hook);
toy_hook_remove(sess, hook);
toy_commit(sess);   // 启用所有挂载了 handler 的 hook
```

## 关键设计

### 就近分配（`alloc_rwx_near`）

Inline hook 的 trampoline 必须在目标函数 ±128MB 范围内，否则 ARM64 的 `B`/`BL` 指令跳不过去。`alloc_rwx_near()` 解析 `/proc/self/maps`，从目标地址向外逐页搜索，用 `mmap` + `MAP_FIXED_NOREPLACE` 占据已有映射之间的空洞。

### 指令重定位（`relocate_instruction`）

被覆盖的指令不能直接 `memcpy` 到 trampoline。PC-relative 指令（ADRP、B、BL、CBZ、CBNZ、TBZ、TBNZ、B.cond、LDR literal）需要解码并重写偏移量。

### Emit 辅助函数（`emit_*`）

ARM64 指令通过小型编码函数（`emit_stp_pre`、`emit_ldr_literal`、`emit_blr` 等）生成，而非硬编码十六进制。dispatch stub 和 trampoline 代码因此可读且可维护。

### Dispatch stub（每个 hook 一个）

每个 hook 分配独立的汇编页，保存 callee-saved 寄存器，通过 LDR literal 加载 hook 指针和 `toy_dispatch` 地址，然后进入 C 代码。避免了全局间接跳转表。

### 重入 dispatch

`toy_dispatch` 使用线程局部深度计数器。如果 handler（或原函数）递归触发了同一个 hook，嵌套调用会跳过所有 handler、直接调用原函数。用户完全不需要关心递归保护。

### 抽象命名空间 UDS 控制通道

Android 上，untrusted_app 无法创建 TCP socket（seccomp）和文件系统 socket（SELinux 跨 UID 限制）。控制服务端使用抽象命名空间 UDS（`AF_UNIX` + `sun_path[0] = '\0'`），绕过两种限制，让 `adb shell` 可以直接连接。

## 测试

102 个本地单元测试，覆盖：
- 指令编码与解码
- PC-relative 指令重定位（所有分支类型、ADRP、LDR literal）
- Trampoline 生成
- Dispatch 管线（before/replace/after handler、skip_original、hit count）
- PLT/GOT hook（ELF 解析、GOT 改写、往返测试）
- 就近分配

测试采用 mock 模式：直接 `#include` `.c` 文件，用 stub 替换架构相关和平台相关的函数。

## 许可

教育 / 研究用途。
