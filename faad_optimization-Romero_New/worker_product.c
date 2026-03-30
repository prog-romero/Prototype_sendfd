#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"

/* Helper pour envoyer le FD de retour au Gateway */
int send_back_to_gateway(int client_fd, migration_msg_t* msg) {
    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, UDS_PATH_BACK);
    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(uds_fd); return -1;
    }
    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};
    msgh.msg_iov = &iov; msgh.msg_iovlen = 1;
    msgh.msg_control = control; msgh.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(cmsg)) = client_fd;
    ssize_t sent = sendmsg(uds_fd, &msgh, 0);
    close(uds_fd); return (sent > 0) ? 0 : -1;
}

int parse_request_local(const char* http_request, migration_msg_t* msg) {
    char line[256]; strncpy(line, http_request, sizeof(line) - 1); line[sizeof(line) - 1] = '\0';
    char* eol = strchr(line, '\n'); if (eol) *eol = '\0'; eol = strchr(line, '\r'); if (eol) *eol = '\0';
    char* path_start = strchr(line, ' '); if (!path_start) return -1; path_start++;
    char* path_end = strchr(path_start, ' '); if (path_end) *path_end = '\0';
    char path[64] = {0}; char query[128] = {0};
    char* q = strchr(path_start, '?');
    if (q) {
        int plen = (int)(q - path_start); if (plen >= (int)sizeof(path)) plen = (int)sizeof(path) - 1;
        strncpy(path, path_start, plen); strncpy(query, q + 1, sizeof(query) - 1);
    } else { strncpy(path, path_start, sizeof(path) - 1); }
    if (strcmp(path, "/sum") == 0) strcpy(msg->function_name, "sum");
    else if (strcmp(path, "/product") == 0) strcpy(msg->function_name, "product");
    else return -1;
    msg->param_a = 0.0; msg->param_b = 0.0;
    char* pa = strstr(query, "a="); char* pb = strstr(query, "b=");
    if (pa) msg->param_a = atof(pa + 2);
    if (pb) msg->param_b = atof(pb + 2);
    return 0;
}

int receive_handoff(int uds_conn, int* client_fd, migration_msg_t* msg) {
    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};
    msgh.msg_iov = &iov; msgh.msg_iovlen = 1;
    msgh.msg_control = control; msgh.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(uds_conn, &msgh, 0);
    if (n <= 0) return -1;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) { *client_fd = *((int*)CMSG_DATA(cmsg)); return 0; }
    return -1;
}

int main() {
    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);
    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, UDS_PATH_PRODUCT);
    unlink(UDS_PATH_PRODUCT);
    bind(uds_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(uds_fd, 10);
    printf("[PRODUCT] Worker PRODUCT pret sur %s\n", UDS_PATH_PRODUCT);
    while (1) {
        int uds_conn = accept(uds_fd, NULL, NULL);
        int client_fd = -1; migration_msg_t msg = {0};
        if (receive_handoff(uds_conn, &client_fd, &msg) == 0) {
            WOLFSSL* ssl = wolfSSL_new(ctx);
            wolfSSL_set_fd(ssl, client_fd);
            wolfSSL_tls_import(ssl, msg.tls_blob, msg.blob_sz);
            wolfSSL_set_fd(ssl, client_fd);
            while (1) {
                printf("[PRODUCT] Traitement de /%s (a=%.2f, b=%.2f)\n", msg.function_name, msg.param_a, msg.param_b);
                double res = msg.param_a * msg.param_b;
                char body[256]; int blen = snprintf(body, sizeof(body), "PRODUCT Result: %.2f\n", res);
                char resp[512]; int rlen = snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s", blen, body);
                wolfSSL_write(ssl, resp, rlen);
                printf("[PRODUCT] Attente requête suivante...\n");
                memset(msg.http_request, 0, MAX_REQUEST_SZ);
                msg.request_len = wolfSSL_read(ssl, msg.http_request, MAX_REQUEST_SZ - 1);
                if (msg.request_len <= 0) {
                    wolfSSL_free(ssl); close(client_fd); break;
                }
                if (parse_request_local(msg.http_request, &msg) == 0) {
                    if (strcmp(msg.function_name, "product") == 0) continue;
                    else {
                        printf("[PRODUCT] Route /%s detectee. Retour Gateway...\n", msg.function_name);
                        msg.blob_sz = MAX_EXPORT_SZ;
                        wolfSSL_tls_export(ssl, msg.tls_blob, &msg.blob_sz);
                        wolfSSL_set_quiet_shutdown(ssl, 1);
                        wolfSSL_free(ssl);
                        if (send_back_to_gateway(client_fd, &msg) < 0) close(client_fd);
                        break;
                    }
                } else {
                    wolfSSL_free(ssl); close(client_fd); break;
                }
            }
        }
        close(uds_conn);
    }
    return 0;
}
