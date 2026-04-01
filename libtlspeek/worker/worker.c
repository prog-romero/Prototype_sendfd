/*
 * worker.c — Worker process: receives the TLS connection fd from the gateway
 * via SCM_RIGHTS, restores the TLS session state, reads the HTTP request
 * (the SAME data that tls_read_peek() saw, because the kernel buffer was
 * never consumed), and sends the HTTP response directly to the client.
 *
 * Usage:  ./worker <worker_id>
 * Example: ./worker 0
 *          ./worker 1
 *
 * The worker must start BEFORE the gateway so its Unix socket exists when
 * the gateway tries to connect.
 */

/* Ensure wolfSSL features are enabled */
#ifndef WOLFSSL_TLS13
#define WOLFSSL_TLS13
#endif
#ifndef HAVE_HKDF
#define HAVE_HKDF
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define MAX_EVENTS 2048
#define MAX_FDS 65536

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

typedef struct {
    int active;
    int is_migration;
    WOLFSSL *ssl;
} conn_state_t;

static conn_state_t clients[MAX_FDS] = {0};

static void close_client(int epfd, int fd) {
    if (!clients[fd].active) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    if (clients[fd].is_migration && clients[fd].ssl) {
        wolfSSL_shutdown(clients[fd].ssl);
        wolfSSL_free(clients[fd].ssl);
    }
    close(fd);
    memset(&clients[fd], 0, sizeof(conn_state_t));
}

// Wolfssl headers moved up

#include "tlspeek.h"
#include "tlspeek_serial.h"
#include "sendfd.h"
#include "unix_socket.h"
#include "handler.h"

#define UNIX_BACKLOG  4096
#define REQUEST_BUFSZ 65536

/* ─────────────────────────────────────────────────────────────────────────── */

// handle_incoming_fd removed, integrated entirely into epoll loop.

/* ─────────────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <worker_id> [cert.crt] [cert.key]\n"
                "Example: ./worker 0\n"
                "         ./worker 1 certs/server.crt certs/server.key\n",
                argv[0]);
        return 1;
    }

    int         worker_id  = atoi(argv[1]);
    /* Optional cert/key for worker-side wolfSSL context.
     * The worker needs a valid CTX to create WOLFSSL* objects.
     * For prototype simplicity the cert is not verified on the worker
     * because the worker is the server-side of an existing connection.
     * We only need the CTX to allocate the WOLFSSL* session object. */
    const char *cert_file  = (argc >= 4) ? argv[2] : "certs/server.crt";
    const char *key_file   = (argc >= 4) ? argv[3] : "certs/server.key";

    fprintf(stderr,
            "[worker-%d] Starting (cert=%s key=%s)\n",
            worker_id, cert_file, key_file);

    /* Ignore SIGPIPE and SIGCHLD */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* ── wolfSSL init ── */
    wolfSSL_Init();
    /* wolfSSL_Debugging_ON(); */

    /*
     * Use server method — the worker is acting as the server side of
     * an existing (already handshaked) TLS connection.
     * We won't call wolfSSL_accept() — instead tlspeek_restore() will
     * inject the session keys directly.
     */
    WOLFSSL_CTX *wctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!wctx) {
        fprintf(stderr, "[worker-%d] wolfSSL_CTX_new failed\n", worker_id);
        return 1;
    }

    /* Load the server certificate (needed to init a valid CTX) */
    if (wolfSSL_CTX_use_certificate_file(wctx, cert_file,
                                          SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr,
                "[worker-%d] Warning: could not load cert %s — "
                "continuing anyway (cert not needed for key injection)\n",
                worker_id, cert_file);
    }
    if (wolfSSL_CTX_use_PrivateKey_file(wctx, key_file,
                                         SSL_FILETYPE_PEM) != SSL_SUCCESS)
    {
        fprintf(stderr,
                "[worker-%d] Warning: could not load key %s\n",
                worker_id, key_file);
    }

    /* ── Create Unix socket ── */
    char sock_path[64];
    snprintf(sock_path, sizeof(sock_path), "worker_%d.sock", worker_id);

    int listen_fd = unix_server_socket(sock_path, UNIX_BACKLOG);
    if (listen_fd < 0) {
        wolfSSL_CTX_free(wctx);
        return 1;
    }

    fprintf(stderr,
            "[worker-%d] Ready. Waiting for connections on %s\n\n",
            worker_id, sock_path);

    /* ── Main loop: Epoll Multiplexing ── */
    signal(SIGCHLD, SIG_IGN);
    
    set_nonblocking(listen_fd);
    int epfd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    fprintf(stderr, "[worker-%d] Entering epoll loop...\n", worker_id);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0 && errno == EINTR) continue;

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (1) {
                    int conn_fd = unix_accept(listen_fd);
                    if (conn_fd < 0) break; // EWOULDBLOCK

                    char cmsg_buf[CMSG_SPACE(sizeof(int))];
                    memset(cmsg_buf, 0, sizeof(cmsg_buf));
                    tlspeek_serial_t serial;
                    memset(&serial, 0, sizeof(serial));
                    struct iovec iov = { .iov_base = &serial, .iov_len = sizeof(serial) };
                    struct msghdr msg = {
                        .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf)
                    };

                    ssize_t n = recvmsg(conn_fd, &msg, 0);
                    if (n <= 0) {
                        close(conn_fd);
                        continue;
                    }

                    int client_fd = -1;
                    int is_migration = 0;
                    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
                        client_fd = *((int *)CMSG_DATA(cmsg));
                        is_migration = 1;
                        close(conn_fd); 
                    } else {
                        client_fd = conn_fd; 
                    }

                    set_nonblocking(client_fd);

                    if (is_migration) {
                        WOLFSSL *ssl = wolfSSL_new(wctx);
                        if (!ssl) { close(client_fd); continue; }
                        wolfSSL_set_fd(ssl, client_fd);
                        if (tlspeek_restore(ssl, &serial) != 0) {
                            wolfSSL_free(ssl); close(client_fd); continue;
                        }
                        wolfSSL_set_fd(ssl, client_fd);
                        
                        clients[client_fd].active = 1;
                        clients[client_fd].is_migration = 1;
                        clients[client_fd].ssl = ssl;
                    } else {
                        clients[client_fd].active = 1;
                        clients[client_fd].is_migration = 0;
                        clients[client_fd].ssl = NULL;
                        
                        if (n > 0 && n <= (ssize_t)sizeof(serial)) {
                            char request[8192];
                            memcpy(request, &serial, n);
                            request[n] = '\0';
                            char *response = handle_http_request(request, n, worker_id);
                            if (response) {
                                int written = write(client_fd, response, strlen(response));
                                (void)written;
                                free(response);
                            }
                        }
                    }

                    struct epoll_event c_ev;
                    c_ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    c_ev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &c_ev);
                }
            } else {
                int c_fd = fd;
                if (!clients[c_fd].active) continue;
                
                char request[8192];
                int req_len = 0;
                
                if (clients[c_fd].is_migration) {
                    req_len = wolfSSL_read(clients[c_fd].ssl, request, sizeof(request) - 1);
                } else {
                    req_len = read(c_fd, request, sizeof(request) - 1);
                }

                if (req_len > 0) {
                    request[req_len] = '\0';
                    char *response = handle_http_request(request, req_len, worker_id);
                    if (response) {
                        int resp_len = strlen(response);
                        int total_written = 0;
                        if (clients[c_fd].is_migration) {
                            while(total_written < resp_len) {
                                int written = wolfSSL_write(clients[c_fd].ssl, response + total_written, resp_len - total_written);
                                if (written > 0) total_written += written;
                                else {
                                    int err = wolfSSL_get_error(clients[c_fd].ssl, written);
                                    if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) break; 
                                    free(response);
                                    goto close_client_now;
                                }
                            }
                        } else {
                            int written_plain = write(c_fd, response, resp_len);
                            (void)written_plain;
                        }
                        free(response);
                    } else {
                        goto close_client_now;
                    }
                } else if (req_len < 0) {
                    if (clients[c_fd].is_migration) {
                        int err = wolfSSL_get_error(clients[c_fd].ssl, req_len);
                        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) continue;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    }
                    goto close_client_now;
                } else {
                    goto close_client_now;
                }
                continue;
                
            close_client_now:
                close_client(epfd, c_fd);
            }
        }
    }

    /* Cleanup (unreachable in prototype) */
    close(listen_fd);
    unlink(sock_path);
    wolfSSL_CTX_free(wctx);
    wolfSSL_Cleanup();

    return 0;
}
