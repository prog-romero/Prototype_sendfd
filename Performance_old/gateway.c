#line 1 "/home/tchiaze/Master2_ACS_SUPAERO_ISAE/Migration_TLS_2/Migration_TLS/gateway.c"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <http_parser.h>
#include "common.h"

int parse_sni(const unsigned char* data, int len, char* hostname) {
    if (len < 43) return -1;
    if (data[0] != 0x16) return -1; 
    int pos = 5 + 1 + 3 + 2 + 32;
    if (pos + 1 > len) return -1;
    pos += 1 + data[pos];
    if (pos + 2 > len) return -1;
    int cs_len = (data[pos] << 8) | data[pos+1];
    pos += 2 + cs_len;
    if (pos + 1 > len) return -1;
    pos += 1 + data[pos];
    if (pos + 2 > len) return -1;
    int ext_total_len = (data[pos] << 8) | data[pos+1];
    pos += 2;
    int ext_end = pos + ext_total_len;
    if (ext_end > len) ext_end = len;
    while (pos + 4 <= ext_end) {
        int ext_type = (data[pos] << 8) | data[pos+1];
        int ext_len = (data[pos+2] << 8) | data[pos+3];
        pos += 4;
        if (ext_type == 0x00) { 
            if (pos + 5 > ext_end) return -1;
            int type = data[pos+2];
            int name_len = (data[pos+3] << 8) | data[pos+4];
            pos += 5;
            if (type == 0 && pos + name_len <= ext_end) {
                strncpy(hostname, (char*)&data[pos], name_len);
                hostname[name_len] = '\0';
                return 0;
            }
        }
        pos += ext_len;
    }
    return -1;
}

typedef struct {
    char function[32];
} gw_parse_data_t;

int on_url_gw(http_parser* p, const char* at, size_t length) {
    gw_parse_data_t* data = (gw_parse_data_t*)p->data;
    if (length >= 4 && strncmp(at, "/sum", 4) == 0) strcpy(data->function, "sum");
    else if (length >= 8 && strncmp(at, "/product", 8) == 0) strcpy(data->function, "product");
    return 0;
}

int send_to_worker(int client_fd, migration_msg_t* msg) {
    const char* uds_path = (strcmp(msg->function_name, "sum") == 0) ? UDS_PATH_SUM : UDS_PATH_PRODUCT;
    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, uds_path, sizeof(addr.sun_path)-1);
    if (connect(uds_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { 
        fprintf(stderr, "[GW] Connect fail to %s: %s\n", uds_path, strerror(errno));
        close(uds_fd); return -1; 
    }
    
    struct msghdr msgh = {0};
    struct iovec iov = { .iov_base = msg, .iov_len = sizeof(*msg) };
    char control[CMSG_SPACE(sizeof(int))] = {0};
    msgh.msg_iov = &iov; msgh.msg_iovlen = 1;
    msgh.msg_control = control; msgh.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgh);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = client_fd;
    ssize_t sent = sendmsg(uds_fd, &msgh, 0);
    
    char ack; read(uds_fd, &ack, 1); // ACK du worker
    close(uds_fd);
    return (sent > 0) ? 0 : -1;
}

int main() {
    signal(SIGCHLD, SIG_IGN); // Simplifier la gestion des fils
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);

    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in tcp_addr = { .sin_family = AF_INET, .sin_port = htons(8443), .sin_addr.s_addr = INADDR_ANY };
    if (bind(tcp_fd, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) { perror("bind tcp"); exit(1); }
    listen(tcp_fd, 4096);

    printf("[GW] Gateway SNI/URL prêt (V6.3 Robust)\n");
    while (1) {
        int client_fd = accept(tcp_fd, NULL, NULL);
        if (client_fd >= 0) {
            if (fork() == 0) {
                close(tcp_fd);
                unsigned char buf[1024];
                // On attend un peu que les données arrivent si nécessaire
                struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
                if (poll(&pfd, 1, 1000) > 0) {
                    int n = recv(client_fd, buf, sizeof(buf)-1, MSG_PEEK);
                    if (n > 5) {
                        buf[n] = '\0';
                        migration_msg_t msg = {0};
                        char hostname[128] = {0};
                        if (buf[0] == 0x16) {
                            if (parse_sni(buf, n, hostname) == 0) {
                                if (strcasestr(hostname, "sum")) strcpy(msg.function_name, "sum");
                                else if (strcasestr(hostname, "prod")) strcpy(msg.function_name, "product");
                            }
                        } else {
                            gw_parse_data_t gdata = {0};
                            http_parser parser; http_parser_init(&parser, HTTP_REQUEST);
                            parser.data = &gdata;
                            http_parser_settings settings = {.on_url = on_url_gw};
                            http_parser_execute(&parser, &settings, (char*)buf, n);
                            if (gdata.function[0] != '\0') strcpy(msg.function_name, gdata.function);
                        }
                        if (msg.function_name[0] != '\0') {
                            send_to_worker(client_fd, &msg);
                        } else {
                            fprintf(stderr, "[GW] Unknown target (Peek[0]=%02x, SNI=[%s])\n", buf[0], hostname);
                        }
                    }
                }
                close(client_fd);
                exit(0);
            }
            close(client_fd);
        }
    }
    return 0;
}
