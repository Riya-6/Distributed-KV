/*
 * Phase 1, Stage 5: sequential multi-client lifecycle + graceful
 * shutdown. Contract under test: include/kv/server.h's kv_run_server(),
 * kv_stop_server(), kv_server_port() -> src/server.c (you write this).
 * See docs/stages/phase1-tcp-server.md, Stage 5.
 */

/* Exposes usleep() from <unistd.h> under -std=c11 (strict ISO C hides
 * it -- glibc only declares usleep under _DEFAULT_SOURCE, since it was
 * dropped from POSIX.1-2008). Must come before any system header. */
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kv/server.h"
#include "test_framework.h"
#include "helpers/test_client.h"

static const uint8_t PING_FRAME[] = {0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PONG_FRAME[] = {0xAB, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00};

static void *run_server_thread(void *arg) {
    int *run_rc = (int *)arg;
    *run_rc = kv_run_server(0 /* ephemeral port */);
    return NULL;
}

/* Guards against a buggy kv_stop_server()/kv_run_server() hanging this
 * test binary forever -- fail loudly and exit instead of hanging CI (or
 * you, watching a terminal) indefinitely. */
static void watchdog_handler(int sig) {
    (void)sig;
    fprintf(stderr,
            "FAIL: server did not shut down within the watchdog timeout "
            "(kv_run_server() likely didn't return after kv_stop_server())\n");
    _exit(1);
}

static uint16_t wait_for_server_port(void) {
    for (int i = 0; i < 200; i++) { /* up to ~2s */
        uint16_t port = kv_server_port();
        if (port != 0) return port;
        usleep(10 * 1000);
    }
    return 0;
}

static void ping_pong_over_new_connection(uint16_t port) {
    int fd = test_client_connect(port);
    KV_ASSERT(fd >= 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, PING_FRAME, sizeof(PING_FRAME)), 0);
    uint8_t resp[sizeof(PONG_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), PONG_FRAME, sizeof(PONG_FRAME));
    close(fd);
}

static void test_sequential_clients_and_graceful_shutdown(void) {
    signal(SIGALRM, watchdog_handler);
    alarm(10);

    int run_rc = -2; /* sentinel: thread didn't run */
    pthread_t server_th;
    KV_ASSERT_EQ_INT(pthread_create(&server_th, NULL, run_server_thread, &run_rc), 0);

    uint16_t port = wait_for_server_port();
    KV_ASSERT(port != 0);

    /* Client A connects, does one request, disconnects. */
    ping_pong_over_new_connection(port);
    /* Client B connects AFTER A is gone -- proves the accept loop kept
     * running and the listener/fd state wasn't left in a broken state
     * by A's disconnect. */
    ping_pong_over_new_connection(port);

    kv_stop_server();
    pthread_join(server_th, NULL); /* watchdog above bounds how long this can hang */
    KV_ASSERT_EQ_INT(run_rc, 0);

    KV_ASSERT_EQ_INT(kv_server_port(), 0);

    alarm(0);
}

int main(void) {
    KV_RUN(test_sequential_clients_and_graceful_shutdown);
    KV_REPORT_AND_EXIT();
}
