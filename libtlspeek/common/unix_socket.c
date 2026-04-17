/*
 * unix_socket.c — Unix domain socket helpers.
 *
 * Uses SOCK_STREAM (not SOCK_SEQPACKET) so that the gateway can
 * persistently connect to each worker socket and send multiple fds
 * over the lifetime of the process.
 */

#include "unix_socket.h"
#include "tlspeek_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ─────────────────────────────────────────────────────────────────────────── */

int unix_server_socket(const char *path, int backlog)
{
    /* Remove stale socket file if it exists */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[unix] socket() failed");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[unix] bind() failed");
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        perror("[unix] listen() failed");
        close(fd);
        return -1;
    }

    TLSPEEK_VLOG("[unix] Server listening on %s (fd=%d)\n", path, fd);
    return fd;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int unix_accept(int listen_fd)
{
    int conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("[unix] accept() failed");
        return -1;
    }
    TLSPEEK_VLOG("[unix] Accepted connection on listen_fd=%d → conn_fd=%d\n",
                 listen_fd, conn_fd);
    return conn_fd;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int unix_client_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[unix] socket() failed");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[unix] connect() failed");
        close(fd);
        return -1;
    }

    TLSPEEK_VLOG("[unix] Connected to %s (fd=%d)\n", path, fd);
    return fd;
}
