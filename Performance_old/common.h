#ifndef COMMON_H
#define COMMON_H

#define MAX_REQUEST_SZ   8192
#define UDS_PATH_SUM     "/tmp/faas_sum.sock"
#define UDS_PATH_PRODUCT "/tmp/faas_product.sock"
#define UDS_PATH_BACK    "/tmp/faas_back.sock"

typedef struct {
    char          function_name[32];  /* "sum" ou "product"   */
    char          http_request[MAX_REQUEST_SZ]; /* Utilisé pour le re-routage Keep-Alive */
    int           request_len;
    char          buffered_data[1024]; /* Données lues via PEEK pour le re-routage */
    int           buffered_sz;
    double        param_a;            /* premier  opérande    */
    double        param_b;            /* deuxième opérande    */
} migration_msg_t;

#endif
