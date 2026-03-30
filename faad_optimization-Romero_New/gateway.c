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
#include <errno.h>
#include "common.h"

/* ------------------------------------------------------------------ */
/* Extraction du chemin et des paramètres a/b depuis HTTP GET */
int parse_request(const char* http_request, migration_msg_t* msg) {
    char line[256];
    strncpy(line, http_request, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char* eol = strchr(line, '\n');
    if (eol) *eol = '\0';
    eol = strchr(line, '\r');
    if (eol) *eol = '\0';

    char* path_start = strchr(line, ' ');
    if (!path_start) return -1;
    path_start++;

    char* path_end = strchr(path_start, ' ');
    if (path_end) *path_end = '\0';

    char path[64] = {0};
    char query[128] = {0};
    char* q = strchr(path_start, '?');
    if (q) {
        int plen = (int)(q - path_start);
        if (plen >= (int)sizeof(path)) plen = (int)sizeof(path) - 1;
        strncpy(path, path_start, plen);
        strncpy(query, q + 1, sizeof(query) - 1);
    } else {
        strncpy(path, path_start, sizeof(path) - 1);
    }

    if (strcmp(path, "/sum") == 0) {
        strcpy(msg->function_name, "sum");
    } else if (strcmp(path, "/product") == 0) {
        strcpy(msg->function_name, "product");
    } else {
        return -1;
    }

    msg->param_a = 0.0;
    msg->param_b = 0.0;
    char* pa = strstr(query, "a=");
    char* pb = strstr(query, "b=");
    if (pa) msg->param_a = atof(pa + 2);
    if (pb) msg->param_b = atof(pb + 2);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Envoi du FD + Msg au worker désigné */
int send_to_worker(int client_fd, migration_msg_t* msg) {
    const char* uds_path = (strcmp(msg->function_name, "sum") == 0) ? UDS_PATH_SUM : UDS_PATH_PRODUCT;
    
    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path)-1);

    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(uds_fd);
        return -1;
    }

    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = control;
    msgh.msg_controllen = sizeof(control);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(cmsg)) = client_fd;

    ssize_t sent = sendmsg(uds_fd, &msgh, 0);
    close(uds_fd);
    return (sent > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Réception du FD + État TLS depuis un worker (Migration de retour) */
int receive_back(int uds_conn, int* client_fd, migration_msg_t* msg) {
    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control = control;
    msgh.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(uds_conn, &msgh, 0);
    if (n <= 0) return -1;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        *client_fd = *((int*)CMSG_DATA(cmsg));
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
int main() {
    wolfSSL_Init();
    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
    wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM);
    wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM);

    /* 1. Serveur TCP 8443 */
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in tcp_addr = { .sin_family = AF_INET, .sin_port = htons(8443), .sin_addr.s_addr = INADDR_ANY };
    bind(tcp_fd, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr));
    listen(tcp_fd, 10);

    /* 2. Serveur UDS de retour */
    int back_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un back_addr = { .sun_family = AF_UNIX };
    strcpy(back_addr.sun_path, UDS_PATH_BACK);
    unlink(UDS_PATH_BACK);
    bind(back_fd, (struct sockaddr*)&back_addr, sizeof(back_addr));
    listen(back_fd, 10);

    printf("[GW] Hot Potato pret (8443) et retour UDS (%s)\n", UDS_PATH_BACK);

    struct pollfd fds[2];
    fds[0].fd = tcp_fd;  fds[0].events = POLLIN;
    fds[1].fd = back_fd; fds[1].events = POLLIN;

    while (1) {
        if (poll(fds, 2, -1) < 0) continue;

        /* CAS A : Nouvelle connexion client */
        if (fds[0].revents & POLLIN) {
            int client_fd = accept(tcp_fd, NULL, NULL);
            WOLFSSL* ssl = wolfSSL_new(ctx);
            wolfSSL_set_fd(ssl, client_fd);

            if (wolfSSL_accept(ssl) == SSL_SUCCESS) {
                migration_msg_t msg = {0};
                msg.request_len = wolfSSL_read(ssl, msg.http_request, MAX_REQUEST_SZ-1);
                if (msg.request_len > 0 && parse_request(msg.http_request, &msg) == 0) {
                    msg.blob_sz = MAX_EXPORT_SZ;
                    wolfSSL_tls_export(ssl, msg.tls_blob, &msg.blob_sz);
                    
                    printf("[GW] Routing initial : /%s vers worker\n", msg.function_name);
                    wolfSSL_set_quiet_shutdown(ssl, 1);
                    wolfSSL_free(ssl);

                    if (send_to_worker(client_fd, &msg) < 0) {
                        printf("[GW] Erreur d'envoi au worker !\n");
                        close(client_fd);
                    }
                } else {
                    wolfSSL_free(ssl); close(client_fd);
                }
            } else {
                wolfSSL_free(ssl); close(client_fd);
            }
        }

        /* CAS B : Retour d'un worker (Changement de route Keep-Alive) */
        if (fds[1].revents & POLLIN) {
            int uds_conn = accept(back_fd, NULL, NULL);
            int client_fd = -1;
            migration_msg_t msg = {0};

            if (receive_back(uds_conn, &client_fd, &msg) == 0) {
                printf("[GW] Recu retour Keep-Alive ! Nouvelle route : /%s (FD=%d)\n", msg.function_name, client_fd);
                
                /* On parse déjà la requête reçue du worker précédent */
                if (parse_request(msg.http_request, &msg) == 0) {
                    if (send_to_worker(client_fd, &msg) < 0) {
                        printf("[GW] Echec redirection vers nouveau worker\n");
                    }
                    /* On ferme TOUJOURS notre copie locale après l'envoi au worker, 
                       que l'envoi ait réussi ou échoué. */
                    close(client_fd);
                } else {
                    printf("[GW] Requete de retour invalide\n");
                    close(client_fd);
                }
            }
            close(uds_conn);
        }
    }

    return 0;
}