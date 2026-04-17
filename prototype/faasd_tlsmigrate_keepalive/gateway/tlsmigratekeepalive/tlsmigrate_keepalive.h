#ifndef TLSMIGRATE_KEEPALIVE_H
#define TLSMIGRATE_KEEPALIVE_H

#include <stdint.h>
#include <stddef.h>

#include <tlspeek/tlspeek.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TLSMIGRATE_KEEPALIVE_MAGIC 0x544D4B41U
#define TLSMIGRATE_KEEPALIVE_VERSION 1U
#define TLSMIGRATE_KEEPALIVE_TARGET_LEN 128

typedef struct {
    uint32_t magic;
    uint32_t version;
    tlspeek_serial_t serial;
    char target_function[TLSMIGRATE_KEEPALIVE_TARGET_LEN];
} tlsmigrate_keepalive_payload_t;

#ifdef __cplusplus
}
#endif

#endif