#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "server.h"

static volatile int g_stop = 0;

static void on_sigint(int sig) {
    g_stop = 1;
}

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

static int do_watch(int fd) {
    struct sigaction sa = { .sa_handler = on_sigint };
    sigaction(SIGINT, &sa, NULL);
    g_stop = 0;

    for (;;) {
        if (g_stop) {
            dprintf(fd, "stop\n");
            char line[256];
            int n = read_line(fd, line, sizeof(line));
            if (n > 0) printf("%s\n", line);
            break;
        }
        char line[4096];
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;
        printf("%s\n", line);
        fflush(stdout);
    }

    signal(SIGINT, SIG_DFL);
    return 0;
}

static int do_cmd(int fd, const char *line) {
    if (strncmp(line, "watch", 5) == 0) {
        dprintf(fd, "%s\n", line);
        char resp[64];
        int n = read_line(fd, resp, sizeof(resp));
        if (n > 0) printf("%s\n", resp);
        if (strcmp(resp, "WATCHING") == 0)
            do_watch(fd);
        return 0;
    }

    dprintf(fd, "%s\n", line);
    char buf[4096];
    for (;;) {
        int n = read_line(fd, buf, sizeof(buf));
        if (n <= 0) break;
        printf("%s\n", buf);
        if (strcmp(buf, "OK") == 0 || strncmp(buf, "ERR", 3) == 0)
            break;
    }
    return 0;
}

static void repl(int fd) {
    char line[256];
    for (;;) {
        printf("toyhook> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        line[strcspn(line, "\n")] = '\0';

        if (line[0] == '\0') continue;

        do_cmd(fd, line);
    }
}

static int connect_server(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, TOYHOOK_SOCK_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(TOYHOOK_SOCK_NAME);

    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char *argv[]) {
    int fd = connect_server();
    if (fd < 0) return 1;

    if (argc >= 2) {
        do_cmd(fd, argv[1]);
    } else {
        repl(fd);
    }

    close(fd);
    return 0;
}
