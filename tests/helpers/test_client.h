#ifndef KV_TEST_CLIENT_H
#define KV_TEST_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Test-only socket helpers (scaffolded test infra, not part of the
 * project's src/ contract). Used by the Phase 1 stage 4/5 integration
 * tests to act as a raw TCP client against the server under test.
 */

/* Connect a TCP client socket to 127.0.0.1:port. Returns fd, or -1. */
int test_client_connect(uint16_t port);

/*
 * Get the port a listening socket was actually bound to (for
 * net_listen(0, ...) ephemeral-port binds). Returns the port in host
 * byte order, or 0 on failure.
 */
uint16_t test_get_bound_port(int listen_fd);

/* Write all len bytes of buf to fd. Returns 0 on success, -1 on error
 * (short write due to a closed connection counts as an error here). */
int test_send_all(int fd, const void *buf, size_t len);

/*
 * Read exactly len bytes from fd into buf, waiting up to timeout_ms for
 * data to become available on each read. Returns 0 on success, -1 on
 * timeout, EOF, or error before len bytes were read.
 */
int test_recv_exact(int fd, void *buf, size_t len, int timeout_ms);

#endif /* KV_TEST_CLIENT_H */
