/*
 * Phase 3, Stage 2: write-ahead log.
 * Contract under test: include/kv/wal.h -> src/wal.c (you write this).
 * See docs/stages/phase3-lsm-tree.md, Stage 2.
 */

#include <stdio.h>
#include <string.h>

#include "kv/wal.h"
#include "test_framework.h"

#define TEST_WAL_PATH "build/p3_stage02_test.wal"

typedef struct {
    int call_count;
    /* Records what happened, in order, as a tiny sequence of tags so
     * the test can check both content AND ordering in one assertion
     * pass: 'S' for a replayed SET, 'D' for a replayed DELETE. */
    char tags[8];
    uint8_t last_key[64];
    uint16_t last_key_len;
    uint8_t last_value[64];
    uint32_t last_value_len;
} replay_ctx_t;

static void on_set(const uint8_t *key, uint16_t key_len, const uint8_t *value,
                    uint32_t value_len, void *ctx_) {
    replay_ctx_t *ctx = (replay_ctx_t *)ctx_;
    ctx->tags[ctx->call_count] = 'S';
    memcpy(ctx->last_key, key, key_len);
    ctx->last_key_len = key_len;
    memcpy(ctx->last_value, value, value_len);
    ctx->last_value_len = value_len;
    ctx->call_count++;
}

static void on_delete(const uint8_t *key, uint16_t key_len, void *ctx_) {
    replay_ctx_t *ctx = (replay_ctx_t *)ctx_;
    ctx->tags[ctx->call_count] = 'D';
    memcpy(ctx->last_key, key, key_len);
    ctx->last_key_len = key_len;
    ctx->call_count++;
}

static void test_append_then_replay_sees_both_records_in_order(void) {
    remove(TEST_WAL_PATH);

    kv_wal_t *wal = kv_wal_open(TEST_WAL_PATH);
    KV_ASSERT(wal != NULL);
    KV_ASSERT_EQ_INT(
        kv_wal_append_set(wal, (const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3),
        0);
    KV_ASSERT_EQ_INT(kv_wal_append_delete(wal, (const uint8_t *)"baz", 3), 0);
    kv_wal_close(wal);

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    KV_ASSERT_EQ_INT(kv_wal_replay(TEST_WAL_PATH, on_set, on_delete, &ctx), 0);

    KV_ASSERT_EQ_INT(ctx.call_count, 2);
    KV_ASSERT_EQ_INT(ctx.tags[0], 'S');
    KV_ASSERT_EQ_INT(ctx.tags[1], 'D');

    remove(TEST_WAL_PATH);
}

static void test_replay_on_missing_file_is_a_fresh_start_not_an_error(void) {
    remove(TEST_WAL_PATH); /* make sure it really doesn't exist */

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = kv_wal_replay(TEST_WAL_PATH, on_set, on_delete, &ctx);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT_EQ_INT(ctx.call_count, 0);
}

static void test_truncate_then_replay_sees_nothing(void) {
    remove(TEST_WAL_PATH);

    kv_wal_t *wal = kv_wal_open(TEST_WAL_PATH);
    KV_ASSERT_EQ_INT(
        kv_wal_append_set(wal, (const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3),
        0);
    KV_ASSERT_EQ_INT(kv_wal_truncate(wal), 0);
    kv_wal_close(wal);

    replay_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    KV_ASSERT_EQ_INT(kv_wal_replay(TEST_WAL_PATH, on_set, on_delete, &ctx), 0);
    KV_ASSERT_EQ_INT(ctx.call_count, 0);

    remove(TEST_WAL_PATH);
}

int main(void) {
    KV_RUN(test_append_then_replay_sees_both_records_in_order);
    KV_RUN(test_replay_on_missing_file_is_a_fresh_start_not_an_error);
    KV_RUN(test_truncate_then_replay_sees_nothing);
    KV_REPORT_AND_EXIT();
}
