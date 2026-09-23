/*
 * Phase 5, Stage 1: a real, independent server process (src/main.c,
 * built as build/kv_server) started, cleanly stopped, and restarted
 * against the same data_dir -- proving persistence survives an actual
 * process boundary, not just an in-process store close/reopen (which
 * Phase 3 Stage 5 already covered). See docs/stages/phase5-persistence.md.
 *
 * This test only links the client-side pieces (net/protocol/command +
 * test_client) -- it never links server.c/store.c itself. It drives
 * the server purely as a client over a real socket, fork()/exec()-ing
 * build/kv_server as a separate OS process.
 */

/* Exposes usleep() from <unistd.h> under -std=c11 -- see Phase 1's
 * stage 4/5 test files for why. Must precede all includes. */
#define _DEFAULT_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kv/command.h"
#include "kv/net.h"
#include "kv/protocol.h"
#include "test_framework.h"
#include "helpers/test_client.h"

#define TEST_DATA_DIR "build/p5_stage01_data"
#define SERVER_BIN "build/kv_server"
#define TEST_PORT 18765

/* AB 82 00 00 00 00 00 -- OK */
static const uint8_t OK_FRAME[] = {0xAB, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00};
/* AB 84 00 00 00 00 00 -- NOT_FOUND */
static const uint8_t NOT_FOUND_FRAME[] = {0xAB, 0x84, 0x00, 0x00,
                                           0x00, 0x00, 0x00};

/* Builds a request frame (PING/GET/SET/DELETE) into out_buf using the
 * same payload layout src/command.c's kv_decode_command() expects.
 * kv_encode_response() is opcode-agnostic -- it just writes a header
 * plus payload bytes -- so it works equally well to build a request. */
static size_t build_request(uint8_t *out_buf, size_t out_cap, uint8_t opcode,
                             const uint8_t *key, uint16_t key_len,
                             const uint8_t *value, uint32_t value_len) {
    uint8_t payload[512];
    size_t off = 0;

    if (opcode == KV_OP_PING) {
        return kv_encode_response(opcode, NULL, 0, out_buf, out_cap);
    }

    payload[off++] = (uint8_t)(key_len >> 8);
    payload[off++] = (uint8_t)key_len;
    memcpy(payload + off, key, key_len);
    off += key_len;

    if (opcode == KV_OP_SET) {
        payload[off++] = (uint8_t)(value_len >> 24);
        payload[off++] = (uint8_t)(value_len >> 16);
        payload[off++] = (uint8_t)(value_len >> 8);
        payload[off++] = (uint8_t)value_len;
        memcpy(payload + off, value, value_len);
        off += value_len;
    }

    return kv_encode_response(opcode, payload, (uint32_t)off, out_buf, out_cap);
}

static int connect_with_retry(uint16_t port, int max_attempts) {
    for (int i = 0; i < max_attempts; i++) {
        int fd = test_client_connect(port);
        if (fd >= 0) {
            return fd;
        }
        usleep(50 * 1000);
    }
    return -1;
}

static pid_t spawn_server(const char *data_dir, uint16_t port) {
    pid_t pid = fork();

    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);
        execl(SERVER_BIN, SERVER_BIN, data_dir, port_str, (char *)NULL);
        /* execl only returns on failure. */
        _exit(127);
    }

    return pid;
}

/* Sends a SET and waits for OK. */
static void set_and_expect_ok(int fd, const char *key, const char *value) {
    uint8_t req[512];
    size_t req_len = build_request(
        req, sizeof(req), KV_OP_SET,
        (const uint8_t *)key, (uint16_t)strlen(key),
        (const uint8_t *)value, (uint32_t)strlen(value));

    KV_ASSERT(req_len > 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, req, req_len), 0);

    uint8_t resp[sizeof(OK_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), OK_FRAME, sizeof(OK_FRAME));
}

/* Sends a GET and checks the response matches expected_value, or
 * checks NOT_FOUND if expected_value is NULL. */
static void get_and_expect(int fd, const char *key, const char *expected_value) {
    uint8_t req[512];
    size_t req_len = build_request(
        req, sizeof(req), KV_OP_GET,
        (const uint8_t *)key, (uint16_t)strlen(key), NULL, 0);

    KV_ASSERT(req_len > 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, req, req_len), 0);

    if (expected_value == NULL) {
        uint8_t resp[sizeof(NOT_FOUND_FRAME)];
        KV_ASSERT_EQ_INT(test_recv_exact(fd, resp, sizeof(resp), 2000), 0);
        KV_ASSERT_EQ_BYTES(resp, sizeof(resp), NOT_FOUND_FRAME, sizeof(NOT_FOUND_FRAME));
        return;
    }

    uint8_t header[KV_HEADER_LEN];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, header, sizeof(header), 2000), 0);
    KV_ASSERT_EQ_INT(header[0], KV_MAGIC);
    KV_ASSERT_EQ_INT(header[1], KV_OP_VALUE);

    uint32_t value_len = ((uint32_t)header[2] << 24) | ((uint32_t)header[3] << 16) |
                          ((uint32_t)header[4] << 8) | (uint32_t)header[5];
    KV_ASSERT_EQ_INT(value_len, strlen(expected_value));

    uint8_t value[256];
    KV_ASSERT(value_len < sizeof(value));
    KV_ASSERT_EQ_INT(test_recv_exact(fd, value, value_len, 2000), 0);
    KV_ASSERT_EQ_BYTES(value, value_len, expected_value, strlen(expected_value));
}

static void delete_and_expect_ok(int fd, const char *key) {
    uint8_t req[512];
    size_t req_len = build_request(
        req, sizeof(req), KV_OP_DELETE,
        (const uint8_t *)key, (uint16_t)strlen(key), NULL, 0);

    KV_ASSERT(req_len > 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, req, req_len), 0);

    uint8_t resp[sizeof(OK_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), OK_FRAME, sizeof(OK_FRAME));
}

int main(void) {
    kv_test_current = "p5_stage01_process_restart";

    /* Clean slate: wipe any leftovers from a previous run. */
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);

    /* --- Round 1: start the server, write and delete some data. --- */
    pid_t pid1 = spawn_server(TEST_DATA_DIR, TEST_PORT);
    KV_ASSERT(pid1 > 0);

    int fd1 = connect_with_retry(TEST_PORT, 100);
    KV_ASSERT(fd1 >= 0);

    set_and_expect_ok(fd1, "alpha", "one");
    set_and_expect_ok(fd1, "beta", "two");
    set_and_expect_ok(fd1, "gamma", "three");
    delete_and_expect_ok(fd1, "beta");

    close(fd1);

    /* Ask the server to stop cleanly (SIGTERM -> kv_stop_server()). */
    KV_ASSERT_EQ_INT(kill(pid1, SIGTERM), 0);

    int status1 = -1;
    KV_ASSERT_EQ_INT(waitpid(pid1, &status1, 0), pid1);
    KV_ASSERT(WIFEXITED(status1));
    KV_ASSERT_EQ_INT(WEXITSTATUS(status1), 0);

    /* --- Round 2: a fresh process, same data_dir -- must see round 1's
     * data exactly as it was left (gamma/alpha survive, beta stays
     * deleted, nothing resurrected by WAL replay). --- */
    pid_t pid2 = spawn_server(TEST_DATA_DIR, TEST_PORT);
    KV_ASSERT(pid2 > 0);

    int fd2 = connect_with_retry(TEST_PORT, 100);
    KV_ASSERT(fd2 >= 0);

    get_and_expect(fd2, "alpha", "one");
    get_and_expect(fd2, "gamma", "three");
    get_and_expect(fd2, "beta", NULL); /* deleted -- must stay gone */

    /* Prove round 2's process is itself a real, live server too. */
    set_and_expect_ok(fd2, "delta", "four");
    get_and_expect(fd2, "delta", "four");

    close(fd2);

    KV_ASSERT_EQ_INT(kill(pid2, SIGTERM), 0);

    int status2 = -1;
    KV_ASSERT_EQ_INT(waitpid(pid2, &status2, 0), pid2);
    KV_ASSERT(WIFEXITED(status2));
    KV_ASSERT_EQ_INT(WEXITSTATUS(status2), 0);

    KV_REPORT_AND_EXIT();
}
