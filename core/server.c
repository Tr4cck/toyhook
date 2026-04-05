#include "server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

static void handle_client(int fd, toyhook_server_ctx_t *ctx) {
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        respond(fd, "ERR fdopen failed\n");
        return;
    }

    char line[256];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;

        if (strcmp(line, "dump") == 0) {
            if (ctx->tracer)
                toy_tracer_dump(ctx->tracer, fp);
            fflush(fp);
            respond(fd, "OK\n");
        } else if (strcmp(line, "status") == 0) {
            toy_session_describe(ctx->session, fp);
            fflush(fp);
            respond(fd, "OK\n");
        } else if (strcmp(line, "count") == 0) {
            if (ctx->tracer) {
                dprintf(fd, "events=%zu dropped=%zu\n",
                        toy_tracer_count(ctx->tracer),
                        toy_tracer_dropped(ctx->tracer));
            } else {
                dprintf(fd, "events=0 dropped=0\n");
            }
            respond(fd, "OK\n");
        } else if (strcmp(line, "help") == 0) {
            respond(fd, "Commands: dump | status | count | help | quit\nOK\n");
        } else if (strcmp(line, "quit") == 0) {
            respond(fd, "bye\nOK\n");
            break;
        } else {
            respond(fd, "ERR unknown command\n");
        }
    }

    fclose(fp);
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
