#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "server.h"

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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: toyhookctl <command>\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, TOYHOOK_SOCK_NAME, sizeof(addr.sun_path) - 2);
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(TOYHOOK_SOCK_NAME);

    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    dprintf(fd, "%s\n", argv[1]);

    char line[4096];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) break;
        printf("%s\n", line);
        if (strcmp(line, "OK") == 0 || strncmp(line, "ERR", 3) == 0)
            break;
    }

    close(fd);
    return 0;
}
