#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

/*
 * unix_socket.h — Unix domain socket helpers.
 *
 * Provides server-side (bind/listen) and client-side (connect)
 * primitives for SOCK_SEQPACKET Unix sockets used between the
 * gateway and worker processes.
 */

/**
 * Create a Unix domain socket, bind to path, and start listening.
 *
 * @param path   Socket filesystem path (e.g. /tmp/worker_0.sock).
 * @param backlog listen() backlog.
 * @return listening fd on success, -1 on error.
 */
int unix_server_socket(const char *path, int backlog);

/**
 * Accept a connection on the listening Unix socket.
 * Blocks until a peer connects.
 *
 * @param listen_fd fd returned by unix_server_socket().
 * @return connected fd on success, -1 on error.
 */
int unix_accept(int listen_fd);

/**
 * Set O_NONBLOCK on an existing socket fd.
 *
 * @param fd socket descriptor.
 * @return 0 on success, -1 on error.
 */
int unix_set_nonblocking(int fd);

/**
 * Create a Unix domain socket and connect to the server at path.
 *
 * @param path  Socket filesystem path (e.g. /tmp/worker_0.sock).
 * @return connected fd on success, -1 on error.
 */
int unix_client_connect(const char *path);

#endif /* UNIX_SOCKET_H */
