/*
 * Phase 5, Stage 2: kill -9 mid-write chaos test. See
 * docs/stages/phase5-persistence.md, Stage 2.
 *
 * Batch 1 is fully acked (every SET's OK response is read back before
 * the next is sent) -- every one of those keys MUST survive a kill
 * exactly as written, every single iteration.
 *
 * Batch 2 is pipelined -- several SETs are fired without waiting for
 * their acks, and SIGKILL lands partway through, at a different byte
 * offset in the client-to-server stream each iteration (by varying how
 * long the test sleeps before killing). For each batch-2 key, the only
 * two acceptable post-restart outcomes are "not found" (the record
 * never made it durably to the WAL) or "the exact value" (it did) --
 * anything else (a wrong/garbage value, or the server refusing to
 * start) is a bug.
 */

/* Exposes usleep() from <unistd.h> under -std=c11. Must precede all
 * includes. */
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

#define TEST_DATA_DIR "build/p5_stage02_data"
#define SERVER_BIN "build/kv_server"
#define TEST_PORT 18766
#define ITERATIONS 25
#define BATCH1_KEYS 5
#define BATCH2_KEYS 10

/* AB 82 00 00 00 00 00 -- OK */
static const uint8_t OK_FRAME[] = {0xAB, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00};

/* Same request-builder as Stage 1 -- kv_encode_response() is
 * opcode-agnostic, so it doubles as a request encoder here too. */
static size_t build_set_request(uint8_t *out_buf, size_t out_cap,
                                 const uint8_t *key, uint16_t key_len,
                                 const uint8_t *value, uint32_t value_len) {
    uint8_t payload[512];
    size_t off = 0;

    payload[off++] = (uint8_t)(key_len >> 8);
    payload[off++] = (uint8_t)key_len;
    memcpy(payload + off, key, key_len);
    off += key_len;

    payload[off++] = (uint8_t)(value_len >> 24);
    payload[off++] = (uint8_t)(value_len >> 16);
    payload[off++] = (uint8_t)(value_len >> 8);
    payload[off++] = (uint8_t)value_len;
    memcpy(payload + off, value, value_len);
    off += value_len;

    return kv_encode_response(KV_OP_SET, payload, (uint32_t)off, out_buf, out_cap);
}

static size_t build_get_request(uint8_t *out_buf, size_t out_cap,
                                 const uint8_t *key, uint16_t key_len) {
    uint8_t payload[512];
    payload[0] = (uint8_t)(key_len >> 8);
    payload[1] = (uint8_t)key_len;
    memcpy(payload + 2, key, key_len);
    return kv_encode_response(KV_OP_GET, payload, (uint32_t)(2 + key_len), out_buf, out_cap);
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
        _exit(127);
    }

    return pid;
}

/* Sends a SET and blocks until its OK is read back -- this is what
 * makes a batch-1 key's ack unambiguous. */
static void set_and_ack(int fd, const char *key, const char *value) {
    uint8_t req[512];
    size_t req_len = build_set_request(
        req, sizeof(req),
        (const uint8_t *)key, (uint16_t)strlen(key),
        (const uint8_t *)value, (uint32_t)strlen(value));

    KV_ASSERT(req_len > 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, req, req_len), 0);

    uint8_t resp[sizeof(OK_FRAME)];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, resp, sizeof(resp), 2000), 0);
    KV_ASSERT_EQ_BYTES(resp, sizeof(resp), OK_FRAME, sizeof(OK_FRAME));
}

/* Sends a SET without waiting for its response at all. */
static void set_fire_and_forget(int fd, const char *key, const char *value) {
    uint8_t req[512];
    size_t req_len = build_set_request(
        req, sizeof(req),
        (const uint8_t *)key, (uint16_t)strlen(key),
        (const uint8_t *)value, (uint32_t)strlen(value));

    KV_ASSERT(req_len > 0);
    /* Deliberately not checking the return value -- the connection may
     * already be dead by the time this send happens, and that's fine;
     * the point is only to get bytes in flight before the kill. */
    test_send_all(fd, req, req_len);
}

/* GETs a key on a fresh connection. Returns 1 if found (value copied
 * into out_value/out_value_len), 0 if NOT_FOUND, -1 on any other
 * (unexpected) response. */
static int get_after_restart(int fd, const char *key, uint8_t *out_value,
                              uint32_t *out_value_len) {
    uint8_t req[512];
    size_t req_len = build_get_request(req, sizeof(req), (const uint8_t *)key,
                                        (uint16_t)strlen(key));
    KV_ASSERT(req_len > 0);
    KV_ASSERT_EQ_INT(test_send_all(fd, req, req_len), 0);

    uint8_t header[KV_HEADER_LEN];
    KV_ASSERT_EQ_INT(test_recv_exact(fd, header, sizeof(header), 2000), 0);
    KV_ASSERT_EQ_INT(header[0], KV_MAGIC);

    if (header[1] == KV_OP_NOT_FOUND) {
        return 0;
    }

    if (header[1] != KV_OP_VALUE) {
        return -1;
    }

    uint32_t value_len = ((uint32_t)header[2] << 24) | ((uint32_t)header[3] << 16) |
                          ((uint32_t)header[4] << 8) | (uint32_t)header[5];
    KV_ASSERT(value_len < 256);
    KV_ASSERT_EQ_INT(test_recv_exact(fd, out_value, value_len, 2000), 0);
    *out_value_len = value_len;
    return 1;
}

static void run_one_iteration(int iter) {
    char data_dir[128];
    snprintf(data_dir, sizeof(data_dir), TEST_DATA_DIR "/iter_%02d", iter);

    char rm_cmd[256];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", data_dir);
    system(rm_cmd);
    mkdir(TEST_DATA_DIR, 0755);
    mkdir(data_dir, 0755);

    pid_t pid = spawn_server(data_dir, TEST_PORT);
    KV_ASSERT(pid > 0);

    int fd = connect_with_retry(TEST_PORT, 100);
    KV_ASSERT(fd >= 0);

    /* Batch 1: fully acked -- must survive unconditionally. */
    char batch1_keys[BATCH1_KEYS][32];
    char batch1_values[BATCH1_KEYS][32];
    for (int i = 0; i < BATCH1_KEYS; i++) {
        snprintf(batch1_keys[i], sizeof(batch1_keys[i]), "b1_k%d", i);
        snprintf(batch1_values[i], sizeof(batch1_values[i]), "b1_v%d_iter%d", i, iter);
        set_and_ack(fd, batch1_keys[i], batch1_values[i]);
    }

    /* Batch 2: pipelined, unacked -- kill lands somewhere in here. */
    char batch2_keys[BATCH2_KEYS][32];
    char batch2_values[BATCH2_KEYS][32];
    for (int i = 0; i < BATCH2_KEYS; i++) {
        snprintf(batch2_keys[i], sizeof(batch2_keys[i]), "b2_k%d", i);
        snprintf(batch2_values[i], sizeof(batch2_values[i]), "b2_v%d_iter%d", i, iter);
        set_fire_and_forget(fd, batch2_keys[i], batch2_values[i]);

        /* Vary the kill point: sleep a tiny, iteration-dependent amount
         * between sends so the kill lands at a different point in the
         * pipelined burst each time. */
        if (i == iter % BATCH2_KEYS) {
            break;
        }
    }

    /* A microsecond-scale sleep, pseudo-varied by iteration, so the
     * kill doesn't always land at exactly the same point relative to
     * the server's processing of the burst above. */
    usleep((useconds_t)(100 + (iter * 37) % 900));

    KV_ASSERT_EQ_INT(kill(pid, SIGKILL), 0);

    int status = -1;
    KV_ASSERT_EQ_INT(waitpid(pid, &status, 0), pid);
    KV_ASSERT(WIFSIGNALED(status));
    KV_ASSERT_EQ_INT(WTERMSIG(status), SIGKILL);

    close(fd);

    /* Restart against the same data_dir and verify. */
    pid_t pid2 = spawn_server(data_dir, TEST_PORT);
    KV_ASSERT(pid2 > 0);

    int fd2 = connect_with_retry(TEST_PORT, 100);
    KV_ASSERT(fd2 >= 0);

    /* Batch 1 must always be exactly there. */
    for (int i = 0; i < BATCH1_KEYS; i++) {
        uint8_t value[256];
        uint32_t value_len = 0;
        int rc = get_after_restart(fd2, batch1_keys[i], value, &value_len);
        KV_ASSERT_EQ_INT(rc, 1);
        if (rc == 1) {
            KV_ASSERT_EQ_BYTES(value, value_len, batch1_values[i], strlen(batch1_values[i]));
        }
    }

    /* Batch 2 must be either absent or exactly correct -- never torn. */
    for (int i = 0; i < BATCH2_KEYS; i++) {
        uint8_t value[256];
        uint32_t value_len = 0;
        int rc = get_after_restart(fd2, batch2_keys[i], value, &value_len);
        KV_ASSERT(rc == 0 || rc == 1);
        if (rc == 1) {
            KV_ASSERT_EQ_BYTES(value, value_len, batch2_values[i], strlen(batch2_values[i]));
        }
    }

    close(fd2);
    KV_ASSERT_EQ_INT(kill(pid2, SIGTERM), 0);

    int status2 = -1;
    KV_ASSERT_EQ_INT(waitpid(pid2, &status2, 0), pid2);
    KV_ASSERT(WIFEXITED(status2));
    KV_ASSERT_EQ_INT(WEXITSTATUS(status2), 0);

    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", data_dir);
    system(rm_cmd);
}

int main(void) {
    kv_test_current = "p5_stage02_kill_restart";

    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);

    for (int iter = 0; iter < ITERATIONS; iter++) {
        run_one_iteration(iter);
    }

    system("rm -rf " TEST_DATA_DIR);

    KV_REPORT_AND_EXIT();
}
