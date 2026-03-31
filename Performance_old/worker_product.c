#line 1 "/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Migration_TLS_2/Migration_TLS/worker_product.c"
#define _GNU_SOURCE
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <http_parser.h>
#include "common.h"

#define DIRECT_TCP_PORT 8446

typedef struct {
    double a;
    double b;
    char function[32];
} http_data_t;

int on_url(http_parser* p, const char* at, size_t length) {
    http_data_t* data = (http_data_t*)p->data;
    if (length > 0) {
        if (length >= 4 && strncmp(at, "/sum", 4) == 0) strcpy(data->function, "sum");
        else if (length >= 8 && strncmp(at, "/product", 8) == 0) strcpy(data->function, "product");

        const char* q = memchr(at, '?', length);
        if (q) {
            size_t rem = length - (q - at);
            const char* pa = memmem(q, rem, "a=", 2);
            if (pa) data->a = atof(pa + 2);
            const char* pb = memmem(q, rem, "b=", 2);
            if (pb) data->b = atof(pb + 2);
        }
    }
    return 0;
}

int receive_handoff_v3(int uds_conn, int* client_fd, migration_msg_t* msg, char* raw_buf, size_t raw_sz) {
    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};
    msgh.msg_iov = &iov; msgh.msg_iovlen = 1;
    msgh.msg_control = control; msgh.msg_controllen = sizeof(control);
    
    ssize_t n = recvmsg(uds_conn, &msgh, 0);
    if (n <= 0) return -1;
    
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        *client_fd = *((int*)CMSG_DATA(cmsg));
        return 1;
    }
    memcpy(raw_buf, msg, (n > (ssize_t)raw_sz) ? raw_sz : (size_t)n);
    return 0;
}

void handle_client(WOLFSSL* ssl, int fd, int is_tls, char* initial_data, int initial_sz) {
    http_parser_settings settings = {0};
    settings.on_url = on_url;
    int first_iter = (initial_data && initial_sz > 0);

    while (1) {
        char req[MAX_REQUEST_SZ] = {0};
        int n;
        
        if (first_iter) {
            memcpy(req, initial_data, (initial_sz >= MAX_REQUEST_SZ) ? MAX_REQUEST_SZ-1 : initial_sz);
            n = (initial_sz >= MAX_REQUEST_SZ) ? MAX_REQUEST_SZ-1 : initial_sz;
            first_iter = 0;
        } else {
            if (is_tls) n = wolfSSL_read(ssl, req, sizeof(req)-1);
            else n = read(fd, req, sizeof(req)-1);
        }
        
        if (n <= 0) break;

        http_data_t hdata = {0};
        http_parser parser;
        http_parser_init(&parser, HTTP_REQUEST);
        parser.data = &hdata;
        
        size_t nparsed = http_parser_execute(&parser, &settings, req, n);
        if (nparsed > 0 && (strcmp(hdata.function, "sum") == 0 || strcmp(hdata.function, "product") == 0)) {
            char body[128]; int blen;
            if (strcmp(hdata.function, "sum") == 0) blen = snprintf(body, sizeof(body), "{\"result\": %.2f}\n", hdata.a + hdata.b);
            else blen = snprintf(body, sizeof(body), "{\"result\": %.2f}\n", hdata.a * hdata.b);
            
            char resp[512]; int rlen = snprintf(resp, sizeof(resp), 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %d\r\n"
                "Connection: keep-alive\r\n\r\n%s", 
                blen, body);
            if (is_tls) wolfSSL_write(ssl, resp, rlen);
            else write(fd, resp, rlen);
        } else break;
    }
}

void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main() {
    signal(SIGCHLD, sigchld_handler);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);

    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un uds_addr = { .sun_family = AF_UNIX };
    strcpy(uds_addr.sun_path, UDS_PATH_PRODUCT);
    unlink(UDS_PATH_PRODUCT);
    if (bind(uds_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) < 0) { perror("bind uds"); exit(1); }
    listen(uds_fd, 4096);

    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in tcp_addr = { .sin_family = AF_INET, .sin_port = htons(DIRECT_TCP_PORT), .sin_addr.s_addr = INADDR_ANY };
    if (bind(tcp_fd, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) { perror("bind tcp"); exit(1); }
    listen(tcp_fd, 4096);

    printf("[PROD] Worker PRODUCT prêt (V6.2 Robust)\n");
    struct pollfd p_fds[2];
    p_fds[0].fd = uds_fd; p_fds[0].events = POLLIN;
    p_fds[1].fd = tcp_fd; p_fds[1].events = POLLIN;

    while (1) {
        if (poll(p_fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p_fds[0].revents & POLLIN) {
            int uds_conn = accept(uds_fd, NULL, NULL);
            if (uds_conn >= 0) {
                if (fork() == 0) {
                    close(uds_fd); close(tcp_fd);
                    int client_fd = -1;
                    migration_msg_t msg = {0};
                    char raw_buf[MAX_REQUEST_SZ] = {0};
                    int mode = receive_handoff_v3(uds_conn, &client_fd, &msg, raw_buf, sizeof(raw_buf));
                    
                    if (mode == -1) {
                        fprintf(stderr, "[PROD] ERR: receive_handoff fail\n");
                    } else if (mode == 1) {
                        WOLFSSL* ssl = wolfSSL_new(ctx);
                        wolfSSL_set_fd(ssl, client_fd);
                        handle_client(ssl, client_fd, 1, msg.buffered_data, msg.buffered_sz);
                        wolfSSL_free(ssl); close(client_fd);
                    } else if (mode == 0) {
                        handle_client(NULL, uds_conn, 0, raw_buf, MAX_REQUEST_SZ);
                    }
                    char ack = 1; write(uds_conn, &ack, 1);
                    close(uds_conn);
                    exit(0);
                }
                close(uds_conn);
            }
        }
        if (p_fds[1].revents & POLLIN) {
            int client_fd = accept(tcp_fd, NULL, NULL);
            if (client_fd >= 0) {
                if (fork() == 0) {
                    close(uds_fd); close(tcp_fd);
                    WOLFSSL* ssl = wolfSSL_new(ctx);
                    wolfSSL_set_fd(ssl, client_fd);
                    handle_client(ssl, client_fd, 1, NULL, 0);
                    wolfSSL_free(ssl); close(client_fd);
                    exit(0);
                }
                close(client_fd);
            }
        }
    }
    return 0;
}
