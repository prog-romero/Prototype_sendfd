#ifndef HTTPMIGRATE_H
#define HTTPMIGRATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTPMIGRATE_MAGIC   0x484D4754U /* 'HMGT' */
#define HTTPMIGRATE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t top1_rdtsc;
    uint64_t cntfrq;
    uint8_t top1_set;
    uint8_t _pad[7];
} httpmigrate_payload_t;

/* Starts a simple HTTP listener on the given FD that peeks and routes via sendfd */
int httpmigrate_accept_peek(
    int listen_fd,
    unsigned char* headers_buf,
    size_t headers_buf_sz,
    int* headers_len_out,
    httpmigrate_payload_t* payload_out);

int httpmigrate_send_fd(
    const char* unix_sock_path,
    int client_fd,
    const httpmigrate_payload_t* payload,
    size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* HTTPMIGRATE_H */
