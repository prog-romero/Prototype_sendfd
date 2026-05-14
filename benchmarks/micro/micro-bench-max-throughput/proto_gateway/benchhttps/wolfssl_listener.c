// wolfssl_listener.c
#include "wolfssl_listener.h"
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
struct wolfssl_listener {
    int listen_fd;
    WOLFSSL_CTX* ctx;
};

struct wolfssl_conn {
    int client_fd;
    WOLFSSL* ssl;
};

static int wait_for_io(int fd, int want_write) {
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = (short)(want_write ? POLLOUT : POLLIN);

    for (;;) {
        int rc = poll(&pfd, 1, -1);
        if (rc > 0) {
            return 0;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}

static int parse_port(const char* addr) {
    if (addr == NULL) {
        return -1;
    }

    const char* colon = strrchr(addr, ':');
    if (colon == NULL || colon[1] == '\0') {
        return -1;
    }

    char* end = NULL;
    long port = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535) {
        return -1;
    }

    return (int)port;
}

wolfssl_listener_t* wolfssl_listener_new(const char* addr, const char* cert_file, const char* key_file) {
    int port = parse_port(addr);
    if (port < 0) {
        return NULL;
    }

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        return NULL;
    }

    wolfssl_listener_t* listener = (wolfssl_listener_t*)calloc(1, sizeof(*listener));
    if (listener == NULL) {
        wolfSSL_Cleanup();
        return NULL;
    }
    listener->listen_fd = -1;

    listener->ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (listener->ctx == NULL) {
        wolfssl_listener_close(listener);
        return NULL;
    }

    if (wolfSSL_CTX_use_certificate_file(listener->ctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfssl_listener_close(listener);
        return NULL;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(listener->ctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        wolfssl_listener_close(listener);
        return NULL;
    }

    listener->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener->listen_fd < 0) {
        wolfssl_listener_close(listener);
        return NULL;
    }

    int opt = 1;
    (void)setsockopt(listener->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons((uint16_t)port);

    if (bind(listener->listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        wolfssl_listener_close(listener);
        return NULL;
    }
    if (listen(listener->listen_fd, 4096) < 0) {
        wolfssl_listener_close(listener);
        return NULL;
    }

    return listener;
}

wolfssl_conn_t* wolfssl_listener_accept(wolfssl_listener_t* listener) {
    if (listener == NULL || listener->listen_fd < 0 || listener->ctx == NULL) {
        return NULL;
    }

    /* Loop: retry on per-connection TLS errors so the listener never dies. */
    for (;;) {
        int client_fd = accept(listener->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            /* Transient resource exhaustion (EMFILE, ENFILE, ENOMEM, ENOBUFS):
             * the listening socket is still valid — yield briefly and retry.
             * Fatal errors (EBADF, EINVAL, ENOTSOCK, EOPNOTSUPP) fall through
             * and return NULL to the Go caller. */
            if (errno == EMFILE || errno == ENFILE ||
                errno == ENOMEM || errno == ENOBUFS) {
                usleep(1000); /* 1 ms — let other threads close fds */
                continue;
            }
            return NULL;
        }

        WOLFSSL* ssl = wolfSSL_new(listener->ctx);
        if (ssl == NULL) {
            close(client_fd);
            /* Transient allocation failure — try next connection. */
            continue;
        }

        wolfSSL_set_fd(ssl, client_fd);
        if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
            /* Per-connection TLS handshake failure (bad client hello, etc.).
             * This must NOT kill the listener goroutine — just drop this
             * connection and accept the next one. */
            wolfSSL_free(ssl);
            close(client_fd);
            continue;
        }

        wolfssl_conn_t* conn = (wolfssl_conn_t*)calloc(1, sizeof(*conn));
        if (conn == NULL) {
            wolfSSL_free(ssl);
            close(client_fd);
            continue;
        }

        conn->client_fd = client_fd;
        conn->ssl = ssl;
        return conn;
    }
}

int wolfssl_listener_fd(const wolfssl_listener_t* listener) {
    if (listener == NULL) {
        return -1;
    }
    return listener->listen_fd;
}

int wolfssl_conn_read(wolfssl_conn_t* conn, void* buf, int len) {
    if (conn == NULL || conn->ssl == NULL || buf == NULL || len < 0) {
        return SSL_FATAL_ERROR;
    }

    for (;;) {
        int ret = wolfSSL_read(conn->ssl, buf, len);
        if (ret > 0) {
            return ret;
        }

        int err = wolfSSL_get_error(conn->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            if (wait_for_io(conn->client_fd, 0) == 0) {
                continue;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (wait_for_io(conn->client_fd, 1) == 0) {
                continue;
            }
        }

        return ret;
    }
}

int wolfssl_conn_write(wolfssl_conn_t* conn, const void* buf, int len) {
    if (conn == NULL || conn->ssl == NULL || buf == NULL || len < 0) {
        return SSL_FATAL_ERROR;
    }

    for (;;) {
        int ret = wolfSSL_write(conn->ssl, buf, len);
        if (ret > 0) {
            return ret;
        }

        int err = wolfSSL_get_error(conn->ssl, ret);
        if (err == SSL_ERROR_WANT_READ) {
            if (wait_for_io(conn->client_fd, 0) == 0) {
                continue;
            }
        } else if (err == SSL_ERROR_WANT_WRITE) {
            if (wait_for_io(conn->client_fd, 1) == 0) {
                continue;
            }
        }

        return ret;
    }
}

int wolfssl_conn_get_error(wolfssl_conn_t* conn, int ret) {
    if (conn == NULL || conn->ssl == NULL) {
        return BAD_FUNC_ARG;
    }
    return wolfSSL_get_error(conn->ssl, ret);
}

int wolfssl_conn_fd(const wolfssl_conn_t* conn) {
    if (conn == NULL) {
        return -1;
    }
    return conn->client_fd;
}

int wolfssl_conn_pending(const wolfssl_conn_t* conn) {
    if (conn == NULL || conn->ssl == NULL) {
        return 0;
    }
    return wolfSSL_pending(conn->ssl);
}

void wolfssl_conn_close(wolfssl_conn_t* conn) {
    if (conn == NULL) {
        return;
    }

    if (conn->ssl != NULL) {
        (void)wolfSSL_shutdown(conn->ssl);
        wolfSSL_free(conn->ssl);
        conn->ssl = NULL;
    }

    if (conn->client_fd >= 0) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }

    free(conn);
}

void wolfssl_listener_close(wolfssl_listener_t* listener) {
    if (listener == NULL) {
        return;
    }

    if (listener->listen_fd >= 0) {
        close(listener->listen_fd);
        listener->listen_fd = -1;
    }
    if (listener->ctx != NULL) {
        wolfSSL_CTX_free(listener->ctx);
        listener->ctx = NULL;
    }

    free(listener);
    wolfSSL_Cleanup();
}
