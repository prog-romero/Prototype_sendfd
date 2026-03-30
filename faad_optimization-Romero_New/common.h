#ifndef COMMON_H
#define COMMON_H

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#define MAX_EXPORT_SZ   16384
#define MAX_REQUEST_SZ   8192
#define UDS_PATH_SUM     "/tmp/faas_sum.sock"
#define UDS_PATH_PRODUCT "/tmp/faas_product.sock"
#define UDS_PATH_BACK    "/tmp/faas_back.sock"

typedef struct {
    char          function_name[32];  /* "sum" ou "product"   */
    unsigned char tls_blob[MAX_EXPORT_SZ];
    unsigned int  blob_sz;
    char          http_request[MAX_REQUEST_SZ];
    int           request_len;
    double        param_a;            /* premier  opérande    */
    double        param_b;            /* deuxième opérande    */
} migration_msg_t;

#endif