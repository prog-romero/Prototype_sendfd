// wolfssl_listener.h
#ifndef WOLFSSL_LISTENER_H
#define WOLFSSL_LISTENER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolfssl_listener wolfssl_listener_t;
typedef struct wolfssl_conn wolfssl_conn_t;

wolfssl_listener_t* wolfssl_listener_new(const char* addr, const char* cert_file, const char* key_file);
wolfssl_conn_t* wolfssl_listener_accept(wolfssl_listener_t* listener);
int wolfssl_listener_fd(const wolfssl_listener_t* listener);
void wolfssl_listener_close(wolfssl_listener_t* listener);

int wolfssl_conn_read(wolfssl_conn_t* conn, void* buf, int len);
int wolfssl_conn_write(wolfssl_conn_t* conn, const void* buf, int len);
int wolfssl_conn_get_error(wolfssl_conn_t* conn, int ret);
int wolfssl_conn_fd(const wolfssl_conn_t* conn);
int wolfssl_conn_pending(const wolfssl_conn_t* conn);
void wolfssl_conn_close(wolfssl_conn_t* conn);

#ifdef __cplusplus
}
#endif

#endif // WOLFSSL_LISTENER_H
