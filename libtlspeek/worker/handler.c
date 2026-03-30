/*
 * handler.c — HTTP request handler for the worker process.
 *
 * Parses the decrypted HTTP request and returns a dynamically allocated
 * response string.  Caller is responsible for free().
 */

#include "handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────── */

char *handle_http_request(const char *request, int len, int worker_id)
{
    char *response = malloc(4096);
    if (!response) return NULL;

    fprintf(stderr,
            "[worker-%d] Handling request (%d bytes):\n%.200s\n...\n",
            worker_id, len, request);

    if (strstr(request, "GET /function/hello") || strstr(request, "HEAD /function/hello")) {

        char body[64];
        int body_len = snprintf(body, sizeof(body),
                                "Hello from worker %d!\n", worker_id);
        snprintf(response, 4096,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "X-Served-By: worker-%d\r\n"
            "X-Mechanism: tls-read-peek\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            worker_id, body_len, body);

    } else if (strstr(request, "GET /function/compute") || strstr(request, "HEAD /function/compute")) {

        const char *body = "result=42\n";
        int body_len = (int)strlen(body);
        snprintf(response, 4096,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "X-Served-By: worker-%d\r\n"
            "X-Mechanism: tls-read-peek\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            worker_id, body_len, body);

    } else if (strstr(request, "POST /function/echo")) {

        /* Find body after the blank line (\r\n\r\n) */
        const char *body    = strstr(request, "\r\n\r\n");
        int         body_len = 0;
        if (body) {
            body    += 4;  /* skip \r\n\r\n */
            body_len = len - (int)(body - request);
            if (body_len < 0) body_len = 0;
        }

        snprintf(response, 4096,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "X-Served-By: worker-%d\r\n"
            "X-Mechanism: tls-read-peek\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%.*s",
            worker_id, body_len,
            body_len, body ? body : "");

    } else if (strstr(request, "GET /function/status") || strstr(request, "HEAD /function/status")) {

        char body[128];
        int body_len = snprintf(body, sizeof(body),
                                "{\"worker\":%d,\"status\":\"ok\"}\n",
                                worker_id);
        snprintf(response, 4096,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "X-Served-By: worker-%d\r\n"
            "X-Mechanism: tls-read-peek\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            worker_id, body_len, body);

    } else {

        const char *body = "Not Found\n";
        int body_len = (int)strlen(body);
        snprintf(response, 4096,
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "X-Served-By: worker-%d\r\n"
            "X-Mechanism: tls-read-peek\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            worker_id, body_len, body);
    }

    fprintf(stderr, "[worker-%d] Response built (%zu bytes)\n",
            worker_id, strlen(response));
    return response;
}
