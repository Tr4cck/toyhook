#include "server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include "trace.h"
#include "utils/log.h"

static int read_line(int fd, char *buf, size_t bufsz) {
    size_t i = 0;
    while (i < bufsz - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static void respond(int fd, const char *s) {
    write(fd, s, strlen(s));
}

typedef void (*cmd_fn)(int fd, const char *args, toyhook_server_ctx_t *ctx);

typedef struct {
    const char *name;
    cmd_fn fn;
    int prefix;
} cmd_entry_t;

static void cmd_dump(int fd, const char *args, toyhook_server_ctx_t *ctx) {
    if (ctx->tracer)
        toy_tracer_dump(ctx->tracer, fd);
    respond(fd, "OK\n");
}

static void cmd_status(int fd, const char *args, toyhook_server_ctx_t *ctx) {
    toy_session_describe(ctx->session, fd);
    respond(fd, "OK\n");
}

static void cmd_count(int fd, const char *args, toyhook_server_ctx_t *ctx) {
    if (ctx->tracer) {
        dprintf(fd, "events=%zu dropped=%zu\n",
                toy_tracer_count(ctx->tracer),
                toy_tracer_dropped(ctx->tracer));
    } else {
        dprintf(fd, "events=0 dropped=0\n");
    }
    respond(fd, "OK\n");
}

static void handle_watch(int fd, toyhook_server_ctx_t *ctx, uint64_t hook_id, uint64_t thread_id) {
    toy_tracer_t *t = ctx->tracer;
    if (!t) {
        respond(fd, "ERR no tracer available\n");
        return;
    }

    respond(fd, "WATCHING\n");
    size_t last_head = __atomic_load_n(&t->head, __ATOMIC_RELAXED);
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            LOGE("poll failed");
            break;
        } else if (ret == 0) {
            size_t cur = __atomic_load_n(&t->head, __ATOMIC_RELAXED);
            if (cur != last_head) {
                for (size_t i = last_head; i < cur; i++) {
                    trace_event_t ev = t->events[i & t->mask];
                    if (hook_id && ev.hook_id != hook_id) continue;
                    if (thread_id && ev.thread_id != thread_id) continue;
                    if (ev.kind == TOY_TRACE_ENTER) {
                        dprintf(fd, "ENTER  hook=%lu tid=%lu args=[",
                                ev.hook_id, ev.thread_id);
                        for (unsigned j = 0; j < ev.argc; j++) {
                            dprintf(fd, "0x%lx", ev.args[j]);
                            if (j < ev.argc - 1) dprintf(fd, " ");
                        }
                        dprintf(fd, "] @ %lu\n", ev.timestamp);
                    } else if (ev.kind == TOY_TRACE_LEAVE) {
                        dprintf(fd, "LEAVE  hook=%lu tid=%lu ret=0x%lx dur=%lu @ %lu\n",
                                ev.hook_id, ev.thread_id, ev.ret_val, ev.duration, ev.timestamp);
                    }
                }
                last_head = cur;
            }
        } else {
            if (pfd.revents & POLLIN) {
                char line[256];
                int n = read_line(fd, line, sizeof(line));
                if (n <= 0) break;
                if (strcmp(line, "stop") == 0) {
                    respond(fd, "STOPPED\n");
                    break;
                } else {
                    respond(fd, "ERR unknown command during watch\n");
                }
            }
        }
    }
}

static void cmd_watch(int fd, const char *args, toyhook_server_ctx_t *ctx) {
    uint64_t hook_id   = 0;
    uint64_t thread_id = 0;
    const char *p = args;
    while (*p == ' ') p++;
    if (strncmp(p, "hook=", 5) == 0) {
        hook_id = strtoull(p + 5, NULL, 10);
    }
    p = strchr(p, ' ');
    if (p) {
        while (*p == ' ') p++;
        if (strncmp(p, "tid=", 4) == 0) {
            thread_id = strtoull(p + 4, NULL, 10);
        }
    }
    handle_watch(fd, ctx, hook_id, thread_id);
}

static void cmd_help(int fd, const char *args, toyhook_server_ctx_t *ctx);

static const cmd_entry_t g_cmds[] = {
    { "dump",   cmd_dump,   0 },
    { "status", cmd_status, 0 },
    { "count",  cmd_count,  0 },
    { "watch",  cmd_watch,  1 },
    { "help",   cmd_help,   0 },
    { NULL, NULL, 0 },
};

static void cmd_help(int fd, const char *args, toyhook_server_ctx_t *ctx) {
    dprintf(fd, "Commands:");
    for (const cmd_entry_t *c = g_cmds; c->name; c++) {
        dprintf(fd, " %s", c->name);
        if (strcmp(c->name, "watch") == 0)
            dprintf(fd, " [hook=N] [tid=N]");
    }
    dprintf(fd, " | quit\nOK\n");
}

static const cmd_entry_t *find_cmd(const char *line, const char **out_args) {
    for (const cmd_entry_t *c = g_cmds; c->name; c++) {
        size_t nlen = strlen(c->name);
        if (c->prefix) {
            if (strncmp(line, c->name, nlen) == 0) {
                *out_args = line[nlen] ? line + nlen : "";
                return c;
            }
        } else {
            if (strncmp(line, c->name, nlen) == 0
                && (line[nlen] == '\0' || line[nlen] == ' ')) {
                *out_args = line[nlen] ? line + nlen : "";
                return c;
            }
        }
    }
    return NULL;
}

static void handle_client(int fd, toyhook_server_ctx_t *ctx) {
    char line[256];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;

        if (strcmp(line, "quit") == 0) {
            respond(fd, "bye\nOK\n");
            break;
        }

        const char *args = NULL;
        const cmd_entry_t *cmd = find_cmd(line, &args);
        if (cmd) {
            cmd->fn(fd, args, ctx);
        } else {
            respond(fd, "ERR unknown command\n");
        }
    }
}

void *toyhook_server_run(void *arg) {
    toyhook_server_ctx_t *ctx = arg;

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        LOGE("server socket() failed");
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, TOYHOOK_SOCK_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(TOYHOOK_SOCK_NAME);

    if (bind(sfd, (struct sockaddr *)&addr, addrlen) < 0) {
        LOGE("server bind failed");
        close(sfd);
        return NULL;
    }

    listen(sfd, 1);
    LOGI("server listening on abstract:%s", TOYHOOK_SOCK_NAME);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;
        handle_client(cfd, ctx);
        close(cfd);
    }

    return NULL;
}
