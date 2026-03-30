#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

/*
 * router.h — Routing decision based on HTTP headers.
 *
 * The router receives the plaintext HTTP headers (from tls_read_peek)
 * and returns the index of the target worker process.
 */

/**
 * route_request() — Inspect HTTP headers and select a worker.
 *
 * @param headers  Null-terminated or length-bounded plaintext HTTP headers.
 * @param len      Length of the headers buffer.
 * @return worker index (0 or 1), always >= 0.
 */
int route_request(const char *headers, int len);

#endif /* ROUTER_H */
