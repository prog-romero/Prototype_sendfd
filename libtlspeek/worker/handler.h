#ifndef HANDLER_H
#define HANDLER_H

/*
 * handler.h — HTTP request/response handler for the worker process.
 */

/**
 * handle_http_request() — Parse request and build an HTTP response.
 *
 * @param request   Null-terminated decrypted HTTP request.
 * @param len       Length of request buffer.
 * @param worker_id Worker index (used in X-Served-By header).
 * @return Heap-allocated response string. Caller must free().
 *         Returns NULL on allocation failure.
 */
char *handle_http_request(const char *request, int len, int worker_id);

#endif /* HANDLER_H */
