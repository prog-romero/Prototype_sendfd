#include "handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same logic as Performance_old/worker_sum.c */
char *handle_http_request(const char *request, int len, int worker_id)
{
    char *response = malloc(4096);
    if (!response) return NULL;

    double a = 0, b = 0;
    char function[32] = {0};

    /* Basic URL parameter parsing */
    if (strstr(request, "/sum")) strcpy(function, "sum");
    else if (strstr(request, "/product")) strcpy(function, "product");
    else if (strstr(request, "/hello")) strcpy(function, "hello");

    const char *pa = strstr(request, "a=");
    if (pa) a = atof(pa + 2);
    const char *pb = strstr(request, "b=");
    if (pb) b = atof(pb + 2);

    char body[256];
    int body_len = 0;

    if (strcmp(function, "sum") == 0) {
        body_len = snprintf(body, sizeof(body), "{\"worker\":%d,\"result\":%.2f,\"op\":\"sum\"}\n", worker_id, a + b);
    } else if (strcmp(function, "product") == 0) {
        body_len = snprintf(body, sizeof(body), "{\"worker\":%d,\"result\":%.2f,\"op\":\"product\"}\n", worker_id, a * b);
    } else {
        body_len = snprintf(body, sizeof(body), "{\"worker\":%d,\"message\":\"Hello from Hot Potato Worker\"}\n", worker_id);
    }

    snprintf(response, 4096,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "X-Served-By: worker-%d\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        worker_id, body_len, body);

    return response;
}
