/*
 * Phase 1, Stage 4: single-connection request/response loop over a real
 * socket, against the storage stub (GET always NOT_FOUND, SET/DELETE
 * always OK, PING -> PONG -- real storage doesn't exist until Phase 2).
 *
 * Contract under test: include/kv/server.h's kv_handle_client() ->
 * src/server.c (you write this). See docs/stages/phase1-tcp-server.md,
 * Stage 4.
 *
 * This deliberately does NOT use kv_run_server()/kv_stop_server() --
 * those are Stage 5. This stage tests kv_handle_client() directly
 * against a listener the test manages itself, so Stage 4 doesn't
 * secretly depend on Stage 5 code existing yet.
 */

/* Exposes usleep() from <unistd.h> under -std=c11 (strict ISO C hides
 * it -- glibc only declares usleep under _DEFAULT_SOURCE, since it was
 * dropped from POSIX.1-2008). Must come before any system header. */
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv/net.h"
#include "kv/server.h"
#include "kv/store.h"
#include "test_framework.h"
#include "helpers/test_client.h"

/* AB 01 00 00 00 00 00 -- PING */
static const uint8_t PING_FRAME[] = {0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
/* AB 81 00 00 00 00 00 -- PONG */
static const uint8_t PONG_FRAME[] = {0xAB, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00};
/* AB 82 00 00 00 00 00 -- OK */
static const uint8_t OK_FRAME[] = {0xAB, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00};
/* AB 84 00 00 00 00 00 -- NOT_FOUND */
static const uint8_t NOT_FOUND_FRAME[] = {0xAB, 0x84, 0x00, 0x00,
                                           0x00, 0x00, 0x00};
/* AB 03 00 00 00 0C 00 | SET foo=bar */
static const uint8_t SET_FOO_BAR_FRAME[] = {
    0xAB, 0x03, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x00, 0x03, 'f',  'o',  'o',
    0x00, 0x00, 0x00, 0x03, 'b', 'a', 'r'};
/* AB 02 00 00 00 05 00 | GET qux -- a key never set anywhere in this file */
static const uint8_t GET_QUX_FRAME[] = {0xAB, 0x02, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'q',  'u',  'x'};
/* AB 04 00 00 00 05 00 | DELETE foo */
static const uint8_t DEL_FOO_FRAME[] = {0xAB, 0x04, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'f',  'o',  'o'};

typedef struct {
    int listen_fd;
    int handle_rc; /* kv_handle_client()'s return value, for the caller to check */
} server_thread_arg_t;

static void *serve_one_client(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    int client_fd = net_accept(a->listen_fd, NULL);
    a->handle_rc = kv_handle_client(client_fd);
    net_close(client_fd);
    return NULL;
}

/* Runs the full request/response scenario against a fresh server thread,
 * so each test case gets its own listener/connection. */
static void with_server(void (*scenario)(int client_fd)) {
    int listen_fd = net_listen(0, 1);
    KV_ASSERT(listen_fd >= 0);
    uint16_t port = test_get_bound_port(listen_fd);

    server_thread_arg_t arg;
    arg.listen_fd = listen_fd;
    arg.handle_rc = -2; /* sentinel: thread didn't run */
    pthread_t th;
    KV_ASSERT_EQ_INT(pthread_create(&th, NULL, serve_one_client, &arg), 0);

    int client_fd = test_client_connect(port);
    KV_ASSERT(client_fd >= 0);

    scenario(client_fd);

    close(client_fd); /* triggers a clean disconnect on the server side */
    pthread_join(th, NULL);
    KV_ASSERT_EQ_INT(arg.handle_rc, 0);

    net_close(listen_fd);
}

static void scenario_ping_pong(int client_fd) {
    KV_ASSERT_EQ_INT(test_send_all(client_fd, PING_FRAME, sizeof(PING_FRAME)), 0);
    uint8_t resp[sizeof(PONG_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), PONG_FRAME, sizeof(PONG_FRAME));
}

static void scenario_set_get_delete_get(int client_fd) {
    uint8_t resp[7];

    KV_ASSERT_EQ_INT(
        test_send_all(client_fd, SET_FOO_BAR_FRAME, sizeof(SET_FOO_BAR_FRAME)),
        0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), OK_FRAME, sizeof(OK_FRAME));

    /* GET a DIFFERENT key that was never set -- NOT_FOUND here is a
     * real invariant (an untouched key is always a miss) rather than
     * an artifact of Phase 1's storage-free stub, so this assertion
     * keeps holding once Phase 2 wires in real storage. Deliberately
     * not GET-ing "foo" here: under the Phase 1 stub GET always
     * answers NOT_FOUND regardless of any prior SET, but once Phase 2
     * lands, GET-ing a key you just SET is supposed to find it -- that
     * would flip this assertion from correct to wrong. */
    KV_ASSERT_EQ_INT(test_send_all(client_fd, GET_QUX_FRAME, sizeof(GET_QUX_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), NOT_FOUND_FRAME, sizeof(NOT_FOUND_FRAME));

    KV_ASSERT_EQ_INT(test_send_all(client_fd, DEL_FOO_FRAME, sizeof(DEL_FOO_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), OK_FRAME, sizeof(OK_FRAME));
}

static void scenario_pipelined_requests_get_ordered_responses(int client_fd) {
    /* Two PING frames sent as ONE write() call -- the server's read
     * loop must pull both frames out of a single recv() and answer
     * them in order. This is Stage 2's "multiple frames in one buffer"
     * case, now exercised over a live socket instead of a byte array. */
    uint8_t two_pings[sizeof(PING_FRAME) * 2];
    memcpy(two_pings, PING_FRAME, sizeof(PING_FRAME));
    memcpy(two_pings + sizeof(PING_FRAME), PING_FRAME, sizeof(PING_FRAME));
    KV_ASSERT_EQ_INT(test_send_all(client_fd, two_pings, sizeof(two_pings)), 0);

    uint8_t resp[sizeof(PONG_FRAME) * 2];
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(PONG_FRAME), PONG_FRAME, sizeof(PONG_FRAME));
    KV_ASSERT_EQ_BYTES(resp + sizeof(PONG_FRAME), sizeof(PONG_FRAME), PONG_FRAME,
                        sizeof(PONG_FRAME));
}

static void scenario_frame_split_across_two_writes(int client_fd) {
    /* Send the PING frame's 7 header bytes one at a time in two
     * separate write() calls, forcing the server to see it across (at
     * least) two recv()s -- proves Stage 2's incomplete-frame handling
     * is actually wired into the live read loop, not just correct in
     * isolation. */
    KV_ASSERT_EQ_INT(test_send_all(client_fd, PING_FRAME, 3), 0);
    usleep(50 * 1000); /* let the server's first recv() land with only 3 bytes */
    KV_ASSERT_EQ_INT(
        test_send_all(client_fd, PING_FRAME + 3, sizeof(PING_FRAME) - 3), 0);

    uint8_t resp[sizeof(PONG_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), PONG_FRAME, sizeof(PONG_FRAME));
}

#define TEST_DATA_DIR "build/p1_stage04_data"

int main(void) {
    /* Phase 3, Stage 5: the store persists to disk now, so it needs a
     * directory to open before kv_handle_client's dispatch can call
     * into it at all (see docs/stages/phase3-lsm-tree.md, Stage 5). */
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    if (kv_store_open(TEST_DATA_DIR) != 0) {
        fprintf(stderr, "kv_store_open failed\n");
        return 1;
    }

    kv_test_current = "scenario_ping_pong";
    with_server(scenario_ping_pong);
    kv_test_current = "scenario_set_get_delete_get";
    with_server(scenario_set_get_delete_get);
    kv_test_current = "scenario_pipelined_requests_get_ordered_responses";
    with_server(scenario_pipelined_requests_get_ordered_responses);
    kv_test_current = "scenario_frame_split_across_two_writes";
    with_server(scenario_frame_split_across_two_writes);

    /* This test calls kv_handle_client() directly (see the file-level
     * comment above) rather than through kv_run_server(), which is the
     * only place that calls kv_store_destroy() on shutdown. Since
     * Phase 2 Stage 3 wired the store into kv_handle_client's dispatch,
     * the SET in scenario_set_get_delete_get leaves an entry allocated
     * that nothing would otherwise free before this process exits --
     * clean it up explicitly instead of leaving it for valgrind to
     * (correctly) flag as still-reachable. */
    kv_store_destroy();

    KV_REPORT_AND_EXIT();
}
