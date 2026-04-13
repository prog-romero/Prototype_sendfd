#define _POSIX_C_SOURCE 200809L

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

static int wolfssl_write_all(WOLFSSL* ssl, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = len;
    while (left > 0) {
        int n = wolfSSL_write(ssl, p, (int)left);
        if (n <= 0) {
            int err = wolfSSL_get_error(ssl, n);
            if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return 0;
}

static ssize_t find_double_crlf(const char* buf, size_t len) {
    if (len < 4) {
        return -1;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)i;
        }
    }
    return -1;
}

static long parse_content_length(const char* headers, size_t headers_len) {
    size_t i = 0;
    while (i < headers_len) {
        size_t line_start = i;
        size_t line_end = i;

        while (line_end + 1 < headers_len) {
            if (headers[line_end] == '\r' && headers[line_end + 1] == '\n') {
                break;
            }
            line_end++;
        }

        if (line_end + 1 >= headers_len) {
            break;
        }

        size_t line_len = line_end - line_start;
        const char* line = headers + line_start;

        const char key[] = "content-length:";
        if (line_len >= sizeof(key) - 1) {
            bool match = true;
            for (size_t k = 0; k < sizeof(key) - 1; k++) {
                char c = line[k];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c - 'A' + 'a');
                }
                if (c != key[k]) {
                    match = false;
                    break;
                }
            }

            if (match) {
                const char* p = line + (sizeof(key) - 1);
                while ((size_t)(p - line) < line_len && (*p == ' ' || *p == '\t')) {
                    p++;
                }
                errno = 0;
                long v = strtol(p, NULL, 10);
                if (errno == 0 && v >= 0) {
                    return v;
                }
            }
        }

        i = line_end + 2;
        if (i < headers_len && headers[i - 2] == '\r' && headers[i - 1] == '\n') {
            if (i < headers_len && headers[i] == '\r') {
                break;
            }
        }
    }
    return -1;
}

static int connect_tcp(const char* host, const char* port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s,%s): %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int parse_host_port(const char* in, char** host_out, char** port_out) {
    const char* colon = strrchr(in, ':');
    if (!colon || colon == in || *(colon + 1) == '\0') {
        return -1;
    }

    size_t host_len = (size_t)(colon - in);
    size_t port_len = strlen(colon + 1);

    char* host = (char*)malloc(host_len + 1);
    char* port = (char*)malloc(port_len + 1);
    if (!host || !port) {
        free(host);
        free(port);
        return -1;
    }

    memcpy(host, in, host_len);
    host[host_len] = '\0';
    memcpy(port, colon + 1, port_len);
    port[port_len] = '\0';

    *host_out = host;
    *port_out = port;
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "Usage: %s --listen HOST:PORT --upstream HOST:PORT --cert CERT.pem --key KEY.pem\n",
            argv0);
}

static int handle_connection(int client_fd, WOLFSSL_CTX* ctx, const char* upstream_host,
                             const char* upstream_port) {
    int rc = -1;
    WOLFSSL* ssl = wolfSSL_new(ctx);
    if (!ssl) {
        fprintf(stderr, "wolfSSL_new failed\n");
        return -1;
    }

    wolfSSL_set_fd(ssl, client_fd);
    if (wolfSSL_accept(ssl) != SSL_SUCCESS) {
        wolfSSL_free(ssl);
        return -1;
    }

    const uint64_t t0_ns = now_monotonic_ns();
    char bench_header[128];
    const int bench_header_len = snprintf(bench_header, sizeof(bench_header),
                                          "X-Bench-T0-Ns: %llu\r\n",
                                          (unsigned long long)t0_ns);
    if (bench_header_len <= 0 || (size_t)bench_header_len >= sizeof(bench_header)) {
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        return -1;
    }

    const size_t max_header_bytes = 64 * 1024;
    size_t cap = 8192;
    size_t len = 0;
    char* req = (char*)malloc(cap);
    if (!req) {
        wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
        return -1;
    }

    ssize_t delim_pos = -1;
    while (delim_pos < 0) {
        char tmp[4096];
        int n = wolfSSL_read(ssl, tmp, (int)sizeof(tmp));
        if (n <= 0) {
            goto cleanup;
        }

        if (len + (size_t)n > cap) {
            size_t new_cap = cap;
            while (new_cap < len + (size_t)n) {
                new_cap *= 2;
            }
            char* new_req = (char*)realloc(req, new_cap);
            if (!new_req) {
                goto cleanup;
            }
            req = new_req;
            cap = new_cap;
        }

        memcpy(req + len, tmp, (size_t)n);
        len += (size_t)n;

        if (len > max_header_bytes) {
            fprintf(stderr, "Request headers too large (> %zu bytes)\n", max_header_bytes);
            goto cleanup;
        }

        delim_pos = find_double_crlf(req, len);
    }

    const size_t header_end = (size_t)delim_pos + 4;
    if (header_end > len) {
        goto cleanup;
    }

    const long content_len = parse_content_length(req, header_end);
    const size_t body_in_buf = len - header_end;

    if (content_len >= 0 && body_in_buf > (size_t)content_len) {
        fprintf(stderr,
                "Received more body bytes than Content-Length (%zu > %ld)\n",
                body_in_buf, content_len);
        goto cleanup;
    }

    int upstream_fd = connect_tcp(upstream_host, upstream_port);
    if (upstream_fd < 0) {
        fprintf(stderr, "Failed to connect upstream %s:%s\n", upstream_host, upstream_port);
        goto cleanup;
    }

    const size_t insert_pos = (size_t)delim_pos + 2;
    if (insert_pos > len) {
        close(upstream_fd);
        goto cleanup;
    }

    if (write_all(upstream_fd, req, insert_pos) != 0 ||
        write_all(upstream_fd, bench_header, (size_t)bench_header_len) != 0 ||
        write_all(upstream_fd, req + insert_pos, len - insert_pos) != 0) {
        close(upstream_fd);
        goto cleanup;
    }

    if (content_len > 0) {
        size_t remaining = (size_t)content_len - body_in_buf;
        while (remaining > 0) {
            char tmp[8192];
            const size_t to_read = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
            int n = wolfSSL_read(ssl, tmp, (int)to_read);
            if (n <= 0) {
                close(upstream_fd);
                goto cleanup;
            }
            if (write_all(upstream_fd, tmp, (size_t)n) != 0) {
                close(upstream_fd);
                goto cleanup;
            }
            remaining -= (size_t)n;
        }
    }

    char resp[8192];
    while (1) {
        ssize_t n = read(upstream_fd, resp, sizeof(resp));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (wolfssl_write_all(ssl, resp, (size_t)n) != 0) {
            break;
        }
    }

    close(upstream_fd);
    rc = 0;

cleanup:
    free(req);
    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
    return rc;
}

int main(int argc, char** argv) {
    const char* listen_arg = NULL;
    const char* upstream_arg = NULL;
    const char* cert_file = NULL;
    const char* key_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            listen_arg = argv[++i];
        } else if (strcmp(argv[i], "--upstream") == 0 && i + 1 < argc) {
            upstream_arg = argv[++i];
        } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            cert_file = argv[++i];
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_file = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!listen_arg || !upstream_arg || !cert_file || !key_file) {
        usage(argv[0]);
        return 2;
    }

    char* listen_host = NULL;
    char* listen_port = NULL;
    char* upstream_host = NULL;
    char* upstream_port = NULL;
    if (parse_host_port(listen_arg, &listen_host, &listen_port) != 0 ||
        parse_host_port(upstream_arg, &upstream_host, &upstream_port) != 0) {
        fprintf(stderr, "Invalid HOST:PORT argument\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!ctx) {
        fprintf(stderr, "wolfSSL_CTX_new failed\n");
        return 1;
    }

    if (wolfSSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Failed to load certificate: %s\n", cert_file);
        return 1;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "Failed to load private key: %s\n", key_file);
        return 1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = NULL;
    int gai = getaddrinfo(*listen_host ? listen_host : NULL, listen_port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo(listen): %s\n", gai_strerror(gai));
        return 1;
    }

    int srv_fd = -1;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        srv_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (srv_fd < 0) {
            continue;
        }
        int opt = 1;
        setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(srv_fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(srv_fd);
        srv_fd = -1;
    }

    freeaddrinfo(res);
    if (srv_fd < 0) {
        perror("bind");
        return 1;
    }

    if (listen(srv_fd, 128) != 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr,
            "[vanilla_proxy] TLS listening on %s, forwarding to http://%s:%s (inject X-Bench-T0-Ns)\n",
            listen_arg, upstream_host, upstream_port);

    while (1) {
        int client_fd = accept(srv_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            (void)handle_connection(client_fd, ctx, upstream_host, upstream_port);
            close(client_fd);
            _exit(0);
        }

        close(client_fd);
    }

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    free(listen_host);
    free(listen_port);
    free(upstream_host);
    free(upstream_port);
    return 0;
}
