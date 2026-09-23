/*
 * Phase 2, Stage 3: the store wired into kv_handle_client's dispatch,
 * exercised over a real socket -- GET/SET/DELETE now mean something,
 * not the Phase 1 stub's fixed responses.
 * Contract under test: src/server.c's dispatch (you edit this; the
 * read/parse/decode/encode/send loop itself doesn't change shape).
 * See docs/stages/phase2-storage.md, Stage 3.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv/net.h"
#include "kv/server.h"
#include "kv/store.h"
#include "test_framework.h"
#include "helpers/test_client.h"

#define TEST_DATA_DIR "build/p2_stage03_data"

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
/* AB 02 00 00 00 05 00 | GET foo */
static const uint8_t GET_FOO_FRAME[] = {0xAB, 0x02, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'f',  'o',  'o'};
/* AB 02 00 00 00 05 00 | GET zzz -- never set */
static const uint8_t GET_ZZZ_FRAME[] = {0xAB, 0x02, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'z',  'z',  'z'};
/* AB 04 00 00 00 05 00 | DELETE foo */
static const uint8_t DEL_FOO_FRAME[] = {0xAB, 0x04, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'f',  'o',  'o'};
/* AB 04 00 00 00 05 00 | DELETE www -- never set */
static const uint8_t DEL_WWW_FRAME[] = {0xAB, 0x04, 0x00, 0x00, 0x00, 0x05,
                                         0x00, 0x00, 0x03, 'w',  'w',  'w'};
/* AB 83 00 00 00 03 00 | VALUE "bar" -- the expected GET-hit response */
static const uint8_t VALUE_BAR_FRAME[] = {0xAB, 0x83, 0x00, 0x00, 0x00,
                                           0x03, 0x00, 'b',  'a',  'r'};

typedef struct {
    int listen_fd;
    int handle_rc;
} server_thread_arg_t;

static void *serve_one_client(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    int client_fd = net_accept(a->listen_fd, NULL);
    a->handle_rc = kv_handle_client(client_fd);
    net_close(client_fd);
    return NULL;
}

static void with_server(void (*scenario)(int client_fd)) {
    /* Clean slate -- store is process-global/shared across scenarios.
     * Phase 3, Stage 5: the store persists to disk now, so "clean
     * slate" means wiping the data directory and reopening, not just
     * destroy() (which would leave the store unopened and every
     * kv_store_*() call from server.c's dispatch would crash on a NULL
     * memtable/WAL). See docs/stages/phase3-lsm-tree.md, Stage 5. */
    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);

    int listen_fd = net_listen(0, 1);
    KV_ASSERT(listen_fd >= 0);
    uint16_t port = test_get_bound_port(listen_fd);

    server_thread_arg_t arg;
    arg.listen_fd = listen_fd;
    arg.handle_rc = -2;
    pthread_t th;
    KV_ASSERT_EQ_INT(pthread_create(&th, NULL, serve_one_client, &arg), 0);

    int client_fd = test_client_connect(port);
    KV_ASSERT(client_fd >= 0);

    scenario(client_fd);

    close(client_fd);
    pthread_join(th, NULL);
    KV_ASSERT_EQ_INT(arg.handle_rc, 0);

    net_close(listen_fd);
    kv_store_destroy();
}

static void scenario_set_then_get_returns_real_value(int client_fd) {
    uint8_t resp[sizeof(VALUE_BAR_FRAME)];

    KV_ASSERT_EQ_INT(
        test_send_all(client_fd, SET_FOO_BAR_FRAME, sizeof(SET_FOO_BAR_FRAME)),
        0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(OK_FRAME), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(OK_FRAME), OK_FRAME, sizeof(OK_FRAME));

    KV_ASSERT_EQ_INT(test_send_all(client_fd, GET_FOO_FRAME, sizeof(GET_FOO_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), VALUE_BAR_FRAME, sizeof(VALUE_BAR_FRAME));
}

static void scenario_get_never_set_key_is_not_found(int client_fd) {
    uint8_t resp[sizeof(NOT_FOUND_FRAME)];
    KV_ASSERT_EQ_INT(test_send_all(client_fd, GET_ZZZ_FRAME, sizeof(GET_ZZZ_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), NOT_FOUND_FRAME, sizeof(NOT_FOUND_FRAME));
}

static void scenario_delete_existing_key_then_get_misses(int client_fd) {
    uint8_t resp[sizeof(VALUE_BAR_FRAME)];

    KV_ASSERT_EQ_INT(
        test_send_all(client_fd, SET_FOO_BAR_FRAME, sizeof(SET_FOO_BAR_FRAME)),
        0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(OK_FRAME), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(OK_FRAME), OK_FRAME, sizeof(OK_FRAME));

    KV_ASSERT_EQ_INT(test_send_all(client_fd, DEL_FOO_FRAME, sizeof(DEL_FOO_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(OK_FRAME), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(OK_FRAME), OK_FRAME, sizeof(OK_FRAME));

    KV_ASSERT_EQ_INT(test_send_all(client_fd, GET_FOO_FRAME, sizeof(GET_FOO_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(NOT_FOUND_FRAME), 2000),
                      0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(NOT_FOUND_FRAME), NOT_FOUND_FRAME,
                        sizeof(NOT_FOUND_FRAME));
}

/* Deliberate Phase 2 behavior change from Phase 1's stub: DELETE on a
 * key that was never there answers NOT_FOUND now that the store can
 * actually tell -- the stub always answered OK because it had no way
 * to know better. See docs/stages/phase2-storage.md, Stage 3. */
static void scenario_delete_missing_key_is_not_found(int client_fd) {
    uint8_t resp[sizeof(NOT_FOUND_FRAME)];
    KV_ASSERT_EQ_INT(test_send_all(client_fd, DEL_WWW_FRAME, sizeof(DEL_WWW_FRAME)),
                      0);
    KV_ASSERT_EQ_INT(test_recv_exact(client_fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), NOT_FOUND_FRAME, sizeof(NOT_FOUND_FRAME));
}

int main(void) {
    kv_test_current = "scenario_set_then_get_returns_real_value";
    with_server(scenario_set_then_get_returns_real_value);
    kv_test_current = "scenario_get_never_set_key_is_not_found";
    with_server(scenario_get_never_set_key_is_not_found);
    kv_test_current = "scenario_delete_existing_key_then_get_misses";
    with_server(scenario_delete_existing_key_then_get_misses);
    kv_test_current = "scenario_delete_missing_key_is_not_found";
    with_server(scenario_delete_missing_key_is_not_found);
    KV_REPORT_AND_EXIT();
}
