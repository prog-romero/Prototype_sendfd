/*
 * router.c — HTTP request routing based on plaintext headers.
 *
 * Receives the decrypted (but not consumed) HTTP headers from
 * tls_read_peek() and maps the request path to a worker ID.
 */

#include "router.h"

#include <string.h>
#include <stdio.h>

/* ─────────────────────────────────────────────────────────────────────────── */

int route_request(const char *headers, int len)
{
    if (!headers || len <= 0) {
        fprintf(stderr, "[router] empty headers — routing to default worker 0\n");
        return 0;
    }

    fprintf(stderr, "[router] Routing decision for:\n%.200s\n...\n", headers);

    /*
     * Simple prefix matching on the HTTP request line.
     * strstr works fine here because the buffer is null-terminated by
     * the tls_read_peek() caller (gateway.c zeroes the buffer first).
     */
    if (strstr(headers, "GET /function/hello"))    { fprintf(stderr, "[router] → worker 0 (/function/hello)\n");   return 0; }
    if (strstr(headers, "GET /function/compute"))  { fprintf(stderr, "[router] → worker 1 (/function/compute)\n"); return 1; }
    if (strstr(headers, "POST /function/echo"))    { fprintf(stderr, "[router] → worker 0 (/function/echo)\n");    return 0; }
    if (strstr(headers, "GET /function/status"))   { fprintf(stderr, "[router] → worker 1 (/function/status)\n");  return 1; }
    if (strstr(headers, "GET /function/"))         { fprintf(stderr, "[router] → worker 0 (generic /function/)\n"); return 0; }

    fprintf(stderr, "[router] → worker 0 (default)\n");
    return 0;
}
