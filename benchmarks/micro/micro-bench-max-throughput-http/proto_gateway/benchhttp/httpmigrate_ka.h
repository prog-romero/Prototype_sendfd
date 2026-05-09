#ifndef HTTPMIGRATE_KA_H
#define HTTPMIGRATE_KA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPMIGRATE_MAGIC   0x484D4B41U  /* 'HMKA' - KeepAlive variant */
#define HTTPMIGRATE_VERSION 2U
#define HTTPMIGRATE_TARGET_LEN 128

/*
 * Payload for Keep-Alive mode.
 * - target_function: the /function/<name> the next request is for
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    char     target_function[HTTPMIGRATE_TARGET_LEN];
} httpmigrate_ka_payload_t;

/* Accept a new TCP connection, peek headers, populate payload. */
int httpmigrate_ka_accept_peek(
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    httpmigrate_ka_payload_t* payload_out);

/* Send a client_fd + payload to a worker's Unix socket. */
int httpmigrate_ka_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const httpmigrate_ka_payload_t* payload,
    size_t payload_len);

/* Create a Unix domain listening socket at sock_path (for relay). */
int httpmigrate_ka_unix_server(const char* sock_path);

/*
 * Accept an incoming relay connection from a worker, receive the fd + payload.
 * Returns the client_fd on success, -1 on error.
 */
int httpmigrate_ka_accept_recv(int relay_listen_fd,
                               httpmigrate_ka_payload_t* payload_out);

#ifdef __cplusplus
}
#endif

#endif /* HTTPMIGRATE_KA_H */
