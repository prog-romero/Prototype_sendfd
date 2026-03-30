#ifndef SENDFD_H
#define SENDFD_H

#include <stddef.h>

/*
 * sendfd.h — SCM_RIGHTS fd transfer over Unix domain sockets.
 *
 * Combines a file descriptor transfer (ancillary data) with a regular
 * payload (the tlspeek_serial_t struct) in a single sendmsg() call.
 */

/**
 * Send a file descriptor AND a data payload over a Unix socket.
 *
 * @param unix_sock   Connected Unix domain socket to the worker.
 * @param fd_to_send  File descriptor to transfer via SCM_RIGHTS.
 * @param payload     Regular data payload (e.g. tlspeek_serial_t *).
 * @param payload_len Size of payload in bytes.
 * @return 0 on success, -1 on error (errno set).
 *
 * NOTE: After sendmsg() succeeds, fd_to_send is closed by this function.
 *       The kernel reference count drops from 2 to 1 (worker owns it).
 */
int sendfd_with_state(
    int         unix_sock,
    int         fd_to_send,
    const void *payload,
    size_t      payload_len
);

/**
 * Receive a file descriptor AND a data payload from a Unix socket.
 *
 * @param unix_sock   Listening or connected Unix domain socket.
 * @param received_fd Output: the transferred file descriptor.
 * @param payload     Output buffer for received data.
 * @param payload_len Size of output buffer.
 * @return 0 on success, -1 on error.
 *
 * Checks MSG_CTRUNC — returns -1 if ancillary data was truncated.
 */
int recvfd_with_state(
    int    unix_sock,
    int   *received_fd,
    void  *payload,
    size_t payload_len
);

#endif /* SENDFD_H */
