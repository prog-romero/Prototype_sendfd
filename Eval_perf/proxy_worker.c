#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define BACKLOG 4096
#define MAX_EVENTS 2048

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void handle_client(int client_fd, int id) {
    while (1) {
        char buffer[8192];
        int n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;

        buffer[n] = '\0';
        double a = 0, b = 0;
        
        const char *pa = strstr(buffer, "a=");
        if (pa) a = atof(pa + 2);
        const char *pb = strstr(buffer, "b=");
        if (pb) b = atof(pb + 2);

        char body[256];
        int body_len = snprintf(body, sizeof(body), 
            "{\"mode\":\"proxy\",\"worker\":%d,\"result\":%.2f}\n", 
            id, (strstr(buffer, "/sum") ? (a+b) : (a*b)));
        
        char response[1024];
        int resp_len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "%s",
            body_len, body);
        
        if (write(client_fd, response, resp_len) != resp_len) break;
    }
    close(client_fd);
}

int main(int argc, char *argv[]) {
    int id = (argc > 1) ? atoi(argv[1]) : 0;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "worker_%d_proxy.sock", id);
    char *sock_path = addr.sun_path;

    unlink(sock_path);
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Note: SO_REUSEPORT on Unix sockets is supported in recent kernels to load balance across processes */
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    chmod(sock_path, 0777);
    listen(listen_fd, BACKLOG);

    #include <signal.h>
    signal(SIGCHLD, SIG_IGN);
#ifndef QUIET
    printf("[proxy-worker-%d] Listening on %s (Epoll Event Loop)\n", id, sock_path);
#endif

    set_nonblocking(listen_fd);
    int epfd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (1) {
                    int client_fd = accept(listen_fd, NULL, NULL);
                    if (client_fd < 0) break; // EWOULDBLOCK
                    set_nonblocking(client_fd);
                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
                }
            } else {
                char buffer[8192];
                int n = read(fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    buffer[n] = '\0';
                    double a = 0, b = 0;
                    const char *pa = strstr(buffer, "a=");
                    if (pa) a = atof(pa + 2);
                    const char *pb = strstr(buffer, "b=");
                    if (pb) b = atof(pb + 2);

                    char body[256];
                    int body_len = snprintf(body, sizeof(body), 
                        "{\"mode\":\"proxy\",\"worker\":%d,\"result\":%.2f}\n", 
                        id, (strstr(buffer, "/sum") ? (a+b) : (a*b)));
                    
                    char response[1024];
                    int resp_len = snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n"
                        "%s",
                        body_len, body);
                    
                    int written = write(fd, response, resp_len);
                    (void)written;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                } else {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }
    return 0;
}
