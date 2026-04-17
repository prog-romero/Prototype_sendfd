/*
 * sendfd.c — SCM_RIGHTS file descriptor transfer over Unix domain sockets.
 *
 * Implements sendfd_with_state() and recvfd_with_state(), which transfer
 * both a file descriptor (via ancillary SCM_RIGHTS data) and a structured
 * payload (the TLS session state) in a single sendmsg()/recvmsg() call.
 *
 * Reference: POSIX.1-2017 sendmsg(2), CMSG_SPACE(3).
 */

#include "sendfd.h"
#include "tlspeek_log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

/* ─────────────────────────────────────────────────────────────────────────── */

int sendfd_with_state(
    int         unix_sock,
    int         fd_to_send,
    const void *payload,
    size_t      payload_len)
{
    /* ── ancillary buffer: exactly one int (the fd) ── */
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    /* ── iovec: the TLS serial struct is the regular data ── */
    struct iovec iov = {
        .iov_base = (void *)payload,   /* cast away const for iovec */
        .iov_len  = payload_len,
    };

    struct msghdr msg = {
        .msg_name       = NULL,
        .msg_namelen    = 0,
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
        .msg_flags      = 0,
    };

    /* ── fill ancillary data ── */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    TLSPEEK_VLOG("[sendfd] Sending fd=%d with %zu bytes of TLS state over unix_sock=%d\n",
                 fd_to_send, payload_len, unix_sock);

    ssize_t sent = sendmsg(unix_sock, &msg, 0);
    if (sent < 0) {
        perror("[sendfd] sendmsg failed");
        return -1;
    }

    /*
     * Close gateway's copy of the fd.
     * The kernel reference count drops from 2 → 1.
     * The worker now exclusively owns the connection.
     */
    close(fd_to_send);
    TLSPEEK_VLOG("[sendfd] Closed gateway copy of fd=%d — worker now owns it\n",
                 fd_to_send);

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */

int recvfd_with_state(
    int    unix_sock,
    int   *received_fd,
    void  *payload,
    size_t payload_len)
{
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct iovec iov = {
        .iov_base = payload,
        .iov_len  = payload_len,
    };

    struct msghdr msg = {
        .msg_name       = NULL,
        .msg_namelen    = 0,
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
        .msg_flags      = 0,
    };

    TLSPEEK_VLOG("[recvfd] Waiting for fd + TLS state on unix_sock=%d\n",
                 unix_sock);

    ssize_t rcvd = recvmsg(unix_sock, &msg, 0);
    if (rcvd < 0) {
        perror("[recvfd] recvmsg failed");
        return -1;
    }
    if (rcvd == 0) {
        fprintf(stderr, "[recvfd] recvmsg: peer closed connection\n");
        return -1;
    }

    /* ── check for ancillary data truncation ── */
    if (msg.msg_flags & MSG_CTRUNC) {
        fprintf(stderr,
                "[recvfd] ERROR: ancillary data was truncated (MSG_CTRUNC) — "
                "fd was lost!\n");
        return -1;
    }

    /* ── extract the file descriptor ── */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg
        || cmsg->cmsg_level != SOL_SOCKET
        || cmsg->cmsg_type  != SCM_RIGHTS
        || cmsg->cmsg_len   != CMSG_LEN(sizeof(int)))
    {
        fprintf(stderr,
                "[recvfd] ERROR: missing or malformed SCM_RIGHTS ancillary data\n");
        return -1;
    }

    memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));

    TLSPEEK_VLOG("[recvfd] Received fd=%d with %zd bytes of TLS state\n",
                 *received_fd, rcvd);

    return 0;
}
