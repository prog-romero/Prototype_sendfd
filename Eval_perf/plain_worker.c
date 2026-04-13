#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/wait.h>

#define UDS_PATH "nginx_worker.sock"

void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        while (waitpid(-1, NULL, WNOHANG) > 0);
    } else {
        unlink(UDS_PATH);
        exit(0);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    unlink(UDS_PATH);
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, UDS_PATH, sizeof(addr.sun_path)-1);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }

    listen(sockfd, 4096);
#ifndef QUIET
    printf("Plain Worker (Pre-forked) listening on %s\n", UDS_PATH);
#endif

    /* Pre-fork 4 workers to match Nginx load */
    for (int i = 0; i < 4; i++) {
        if (fork() == 0) {
            close(sockfd);
            // In a real pre-forked model, they should all accept on the same socket
            // But for UDS it's easier to just have one accept and fork, 
            // however let's fix it to be consistent.
            break; 
        }
    }

    /* This simplified version just forks on accept for the UDS worker to keep it robust */
    while (1) {
        int client_fd = accept(sockfd, NULL, NULL);
        if (client_fd < 0) continue;

        if (fork() == 0) {
            while (1) {
                char buffer[4096];
                int n = read(client_fd, buffer, sizeof(buffer)-1);
                if (n <= 0) break;

                buffer[n] = '\0';
                double a = 0, b = 0;
                
                const char *pa = strstr(buffer, "a=");
                if (pa) a = atof(pa + 2);
                const char *pb = strstr(buffer, "b=");
                if (pb) b = atof(pb + 2);

                char body[256];
                int body_len = snprintf(body, sizeof(body), "{\"mode\":\"proxy\",\"result\":%.2f}\n", (strstr(buffer, "/sum") ? (a+b) : (a*b)));
                
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
            exit(0);
        }
        close(client_fd);
    }

    return 0;
}
