#ifndef TLSMIGRATEKEEPALIVE_H
#define TLSMIGRATEKEEPALIVE_H

#include <stddef.h>
#include <stdint.h>

#include "tlsmigrate_keepalive.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tlsmigratekeepalive_ctx tlsmigratekeepalive_ctx_t;

tlsmigratekeepalive_ctx_t* tlsmigratekeepalive_new_ctx(const char* cert_file, const char* key_file);
void tlsmigratekeepalive_free_ctx(tlsmigratekeepalive_ctx_t* ctx);

int tlsmigratekeepalive_accept_peek_export(
    tlsmigratekeepalive_ctx_t* ctx,
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    tlsmigrate_keepalive_payload_t* payload_out);

int tlsmigratekeepalive_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const tlsmigrate_keepalive_payload_t* payload,
    size_t payload_len);

int tlsmigratekeepalive_unix_server(const char* sock_path);
int tlsmigratekeepalive_accept_recv(int listen_fd, tlsmigrate_keepalive_payload_t* payload_out);

#ifdef __cplusplus
}
#endif

#endif