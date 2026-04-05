# toyhook 使用指南

## 概念模型

```
session  →  管理 hook 的生命周期
hook     →  一个拦截点（用 PLT 或 inline 方式）
handler  →  拦截后的回调（可以挂在 BEFORE / REPLACE / AFTER 三个阶段）
tracer   →  环形缓冲区事件记录器，挂载到 hook 上自动记录
```

## 快速开始

### 1. 创建 session

```c
#include "toyhook.h"

toy_session_t *sess = toy_session_create();
```

session 是所有 hook 的容器，销毁时自动清理所有 hook。

### 2. 定义 target

target 决定"hook 哪里"以及"用什么方式 hook"：

```c
// PLT hook — 拦截某个 .so 对外部符号的调用
toy_target_t t = {
    .backend = TOY_BACKEND_PLT,
    .by_symbol = {
        .module = "libtarget.so",
        .symbol = "open",
    },
};

// inline hook — 直接 patch 函数入口的机器码
toy_target_t t = {
    .backend = TOY_BACKEND_INLINE,
    .by_addr = { .addr = some_function_pointer },
};
```

**区别：**
- `TOY_BACKEND_PLT`：改写 GOT 表项，只影响特定 .so 对该符号的调用
- `TOY_BACKEND_INLINE`：改写函数入口指令，影响所有调用者

### 3. 添加 hook

```c
toy_hook_t *hook = toy_hook_add(sess, &t);
```

### 4. 注册 handler

```c
static int my_handler(toy_callctx_t *ctx, void *user_data) {
    // 你的逻辑
    return 0;
}

toy_handler_t h = {
    .phases  = TOY_PHASE_BEFORE,
    .fn      = my_handler,
    .name    = "my_handler",     // 用于后续 toy_hook_off 删除
    .priority = 0,               // 数字越小越先执行，默认 0
};
toy_hook_on(hook, &h);
```

### 5. 启用 hook

```c
toy_hook_enable(hook);
// 或批量启用所有有 handler 的 hook：
toy_commit(sess);
```

### 6. 清理

```c
toy_hook_disable(hook);       // 单个
toy_session_destroy(sess);    // 全部（自动 disable 所有 hook）
```

## Handler 三阶段

### BEFORE — 在原始函数调用前执行

用于：观察/修改参数

```c
static int before_open(toy_callctx_t *ctx, void *ud) {
    const char *path = (const char *)ctx->args[0];
    int flags = (int)ctx->args[1];
    LOG("open(\"%s\", 0x%x)", path, flags);

    // 可以修改参数
    // ctx->args[0] = (unsigned long)"/dev/null";

    return 0;  // 继续执行
}
```

### REPLACE — 替换原始函数

用于：完全控制函数行为。**你必须设置 `skip_original = 1` 或者自己通过 `ctx->original_addr` 调用原始函数。**

```c
// 模式 A：调用原始函数并修改返回值
static int replace_open(toy_callctx_t *ctx, void *ud) {
    typedef int (*orig_fn)(const char *, int, ...);
    orig_fn orig = (orig_fn)ctx->original_addr;

    int fd = orig((const char *)ctx->args[0], (int)ctx->args[1]);
    LOG("original open returned %d", fd);

    ctx->ret_val = (unsigned long)fd;
    ctx->skip_original = 1;  // 告诉 dispatch 不要再调原始函数了
    return 0;
}

// 模式 B：完全替换，不调用原始函数
static int replace_getpid(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 1337;
    ctx->skip_original = 1;
    return 0;
}
```

### AFTER — 在原始函数调用后执行

用于：观察/修改返回值

```c
static int after_open(toy_callctx_t *ctx, void *ud) {
    LOG("open returned fd=%d", (int)ctx->ret_val);
    // ctx->ret_val = -1;  // 可以篡改返回值
    return 0;
}
```

## Handler 执行顺序

同一个 hook 可以注册多个 handler，按 `priority` 排序执行：

```c
toy_handler_t h1 = { .priority = 10, .phases = TOY_PHASE_BEFORE, .fn = log_args,  .name = "log" };
toy_handler_t h2 = { .priority = 20, .phases = TOY_PHASE_BEFORE, .fn = mod_args,  .name = "mod" };
toy_hook_on(hook, &h1);
toy_hook_on(hook, &h2);
// 执行顺序：h1(10) → h2(20)
```

任何 handler 返回非零值会中断当前阶段的后续 handler。

## 重入保护

如果你的 handler（或原始函数）又触发了同一个 hook——比如 BEFORE handler 里调用 `LOGI()`，而 `LOGI` 内部又走了被 hook 的 `__system_property_get`——框架会自动处理：

- `toy_dispatch` 使用线程局部的深度计数器
- 嵌套调用时跳过所有 handler，直接调用原始函数
- 用户不需要写任何 guard 变量

## toy_callctx_t 字段说明

```c
struct toy_callctx {
    toy_hook_t *hook;          // 触发的 hook
    void *target_addr;         // 被 hook 的地址
    void *original_addr;       // 原始函数地址（用于调用原函数）

    unsigned long args[8];     // 函数参数（x0-x7）
    unsigned argc;             // 参数数量
    unsigned long ret_val;     // 返回值

    int skip_original;         // 设为 1 跳过原始函数调用
};
```

## Tracer（追踪器）

tracer 将 hook 的进入/离开事件记录到无锁环形缓冲区。

### 创建和挂载

```c
toy_tracer_t *tracer = toy_tracer_create(1024);  // 容量必须是 2 的幂
toy_tracer_attach(tracer, hook);
```

挂载时会自动注册 BEFORE + AFTER handler，记录 `trace_event_t`：

```c
typedef struct {
    unsigned long timestamp;
    unsigned long hook_id;
    unsigned long thread_id;
    unsigned long args[TOY_MAX_ARGS];
    unsigned argc;
    unsigned long ret_val;
    unsigned kind;       // TOY_TRACE_ENTER 或 TOY_TRACE_LEAVE
} trace_event_t;
```

### 查询

```c
size_t count   = toy_tracer_count(tracer);
size_t dropped = toy_tracer_dropped(tracer);
```

### 导出

```c
toy_tracer_dump(tracer, stderr);   // 导出到任意 FILE*
```

### 启停

```c
toy_tracer_disable(tracer);   // 停止记录
toy_tracer_enable(tracer);    // 恢复记录
```

## 控制通道 (toyhookctl)

payload 启动后会在抽象命名空间 UDS 上启动服务线程，可通过 `adb shell` 实时查询 hook 状态和追踪数据。

### 启动服务

在 payload 的 `on_load` 中：

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

### 使用 toyhookctl

```bash
adb shell toyhookctl status   # 打印所有 hook 的状态
adb shell toyhookctl count    # 显示事件数和丢弃数
adb shell toyhookctl dump     # 导出所有追踪事件
adb shell toyhookctl help     # 列出命令
adb shell toyhookctl quit     # 断开连接
```

### 协议

基于文本行协议，通过抽象 UDS 通信：

```
> dump
ENTER  hook=1 tid=12345 args=[0x7f...] @ 1234567890
LEAVE  hook=1 tid=12345 ret=0x42 @ 1234567900
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
```

## 常见错误

### 忘记设置 skip_original

```c
// 错误！REPLACE handler 没设置 skip_original，
// dispatch 还会再调一次原始函数
static int bad_handler(toy_callctx_t *ctx, void *ud) {
    ctx->ret_val = 42;
    // 缺少 ctx->skip_original = 1;
    return 0;
}
```

### 错误调用 NULL 的原始函数指针

```c
// 错误！不要自己保存原始函数指针
static int (*orig_fn)(void) = NULL;  // 永远是 NULL

static int handler(toy_callctx_t *ctx, void *ud) {
    orig_fn();  // crash!
}

// 正确：通过 ctx->original_addr 调用
static int handler(toy_callctx_t *ctx, void *ud) {
    typedef int (*fn_t)(void);
    fn_t orig = (fn_t)ctx->original_addr;
    orig();
    ctx->skip_original = 1;
    return 0;
}
```

## 完整示例：注入 payload

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

    ctx->ret_val = (unsigned long)res;
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
    toy_session_describe(g_sess, stderr);

    static toyhook_server_ctx_t server_ctx = {0};
    server_ctx.session = g_sess;
    server_ctx.tracer  = g_tracer;
    pthread_t tid;
    pthread_create(&tid, NULL, toyhook_server_run, &server_ctx);
    pthread_detach(tid);
}
```

注入并查询：

```bash
./toyhook inject <pid> /data/local/tmp/libyour_payload.so
toyhookctl status
toyhookctl dump
```
