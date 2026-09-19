/*
 * Phase 1, Stage 1: TCP listener + accept.
 * Contract under test: include/kv/net.h -> src/net.c (you write this).
 * See docs/stages/phase1-tcp-server.md, Stage 1.
 */

#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kv/net.h"
#include "test_framework.h"
#include "helpers/test_client.h"

typedef struct {
    uint16_t port;
    int connected; /* 1 if the client thread's connect() succeeded */
} connector_arg_t;

static void *connector_thread(void *arg) {
    connector_arg_t *a = (connector_arg_t *)arg;
    int fd = test_client_connect(a->port);
    a->connected = (fd >= 0);
    if (fd >= 0) {
        /* Send one byte so the accept side has something it could read,
         * though this stage only asserts on accept() itself. */
        char c = 'x';
        (void)send(fd, &c, 1, 0);
        close(fd);
    }
    return NULL;
}

static void test_listen_returns_valid_fd(void) {
    int fd = net_listen(0 /* ephemeral port */, 1);
    KV_ASSERT(fd >= 0);
    uint16_t port = test_get_bound_port(fd);
    KV_ASSERT(port != 0);
    net_close(fd);
}

static void test_accept_returns_connected_client(void) {
    int listen_fd = net_listen(0, 1);
    KV_ASSERT(listen_fd >= 0);
    uint16_t port = test_get_bound_port(listen_fd);
    KV_ASSERT(port != 0);

    connector_arg_t arg;
    arg.port = port;
    arg.connected = 0;
    pthread_t th;
    KV_ASSERT_EQ_INT(pthread_create(&th, NULL, connector_thread, &arg), 0);

    struct sockaddr_in peer;
    int client_fd = net_accept(listen_fd, &peer);
    KV_ASSERT(client_fd >= 0);
    KV_ASSERT_EQ_INT(peer.sin_family, AF_INET);
    /* A loopback connect's peer address must be 127.0.0.1. */
    KV_ASSERT_EQ_INT(peer.sin_addr.s_addr, htonl(INADDR_LOOPBACK));

    pthread_join(th, NULL);
    KV_ASSERT_EQ_INT(arg.connected, 1);

    net_close(client_fd);
    net_close(listen_fd);
}

static void test_net_close_is_safe_on_bad_fd(void) {
    /* Must not crash. -1 is never a valid fd. */
    net_close(-1);
    KV_ASSERT(1);
}

int main(void) {
    KV_RUN(test_listen_returns_valid_fd);
    KV_RUN(test_accept_returns_connected_client);
    KV_RUN(test_net_close_is_safe_on_bad_fd);
    KV_REPORT_AND_EXIT();
}
