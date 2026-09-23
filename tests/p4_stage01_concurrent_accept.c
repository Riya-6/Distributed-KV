/*
 * Phase 4, Stage 1: concurrent accept loop.
 * Contract under test: kv_run_server()'s internals -> src/server.c
 * (you edit this; the signature and shutdown contract don't change).
 * See docs/stages/phase4-concurrency.md, Stage 1.
 */

/* Exposes usleep() under -std=c11 -- see Phase 1's stage 4/5 tests for
 * why this is needed. Must precede all includes. */
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv/server.h"
#include "kv/store.h"
#include "test_framework.h"
#include "helpers/test_client.h"

#define TEST_DATA_DIR "build/p4_stage01_data"

/* AB 01 00 00 00 00 00 -- PING */
static const uint8_t PING_FRAME[] = {0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
/* AB 81 00 00 00 00 00 -- PONG */
static const uint8_t PONG_FRAME[] = {0xAB, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00};

static void *run_server_thread(void *arg) {
    (void)arg;
    kv_run_server(0 /* ephemeral port */);
    return NULL;
}

/* Same watchdog pattern as Phase 1 Stage 5 -- fail loudly instead of
 * hanging forever if kv_stop_server()/kv_run_server() is ever broken. */
static void watchdog_handler(int sig) {
    (void)sig;
    fprintf(stderr, "FAIL: test did not complete within the watchdog timeout\n");
    _exit(1);
}

static uint16_t wait_for_server_port(void) {
    for (int i = 0; i < 200; i++) {
        uint16_t port = kv_server_port();
        if (port != 0) return port;
        usleep(10 * 1000);
    }
    return 0;
}

/*
 * NOTE on why this trickles bytes rather than just stalling A forever:
 * kv_run_server() sets SO_RCVTIMEO on the *listening* socket (Phase 1
 * Stage 5), and that timeout is inherited by every socket accept()
 * hands back from it (confirmed empirically -- this is real Linux
 * behavior, not assumed). A client that simply stops sending gets its
 * connection dropped by the server after ~100ms regardless of whether
 * the accept loop is sequential or concurrent -- that's a "self-heal"
 * via timeout, not evidence of real concurrency, and a naive stalled-
 * client test can't tell the two apart.
 *
 * Feeding A's frame one byte at a time, with each gap kept under
 * 100ms, keeps every individual recv() fed before its own inherited
 * timeout fires -- so A's connection never times out, and the accept
 * loop is genuinely occupied with A for the whole ~350ms this takes.
 * B's request has to complete well inside that window to prove real
 * concurrency; a sequential server (even a self-healing one) can't
 * reach B until A is done.
 */
typedef struct {
    int fd;
} slow_sender_arg_t;

static void *slow_sender_thread(void *arg_) {
    slow_sender_arg_t *arg = (slow_sender_arg_t *)arg_;
    for (size_t i = 0; i < sizeof(PING_FRAME); i++) {
        send(arg->fd, PING_FRAME + i, 1, 0);
        usleep(50 * 1000); /* 50ms < the inherited 100ms SO_RCVTIMEO */
    }
    return NULL;
}

static void test_stalled_client_does_not_block_a_fresh_client(void) {
    signal(SIGALRM, watchdog_handler);
    alarm(10);

    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);

    pthread_t server_th;
    KV_ASSERT_EQ_INT(pthread_create(&server_th, NULL, run_server_thread, NULL), 0);
    uint16_t port = wait_for_server_port();
    KV_ASSERT(port != 0);

    /* Client A: connect, then start trickling its 7-byte frame in
     * over ~350ms (see the note above) on its own thread so the main
     * thread is free to test B concurrently. */
    int fd_a = test_client_connect(port);
    KV_ASSERT(fd_a >= 0);
    slow_sender_arg_t sender_arg = {fd_a};
    pthread_t sender_th;
    KV_ASSERT_EQ_INT(pthread_create(&sender_th, NULL, slow_sender_thread, &sender_arg),
                      0);

    /* Give A a head start so it's definitely accepted and mid-frame
     * before B shows up. */
    usleep(75 * 1000);

    /* Client B: a full, fresh PING/PONG round trip, required to
     * complete well inside A's ~350ms trickle window -- only possible
     * if the accept loop can serve B while A's handler is still busy,
     * i.e. real per-connection concurrency, not sequential handling
     * that merely recovers quickly once a slow client happens to time
     * out. */
    int fd_b = test_client_connect(port);
    KV_ASSERT(fd_b >= 0);
    KV_ASSERT_EQ_INT(test_send_all(fd_b, PING_FRAME, sizeof(PING_FRAME)), 0);
    uint8_t resp_b[sizeof(PONG_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd_b, resp_b, sizeof(resp_b), 200), 0);
    KV_ASSERT_EQ_BYTES(resp_b, sizeof(resp_b), PONG_FRAME, sizeof(PONG_FRAME));
    close(fd_b);

    /* A should still complete correctly once its trickle finishes. */
    pthread_join(sender_th, NULL);
    uint8_t resp_a[sizeof(PONG_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd_a, resp_a, sizeof(resp_a), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp_a, sizeof(resp_a), PONG_FRAME, sizeof(PONG_FRAME));
    close(fd_a);

    kv_stop_server();
    pthread_join(server_th, NULL);

    alarm(0);
}

int main(void) {
    KV_RUN(test_stalled_client_does_not_block_a_fresh_client);
    KV_REPORT_AND_EXIT();
}
