/*
 * Phase 6, Stage 1: TTL on SET + lazy expiry on GET.
 * Contract under test: include/kv/store.h's kv_store_set_ttl() ->
 * src/store.c, plus the same expiry threaded through
 * memtable/wal/sstable. See docs/stages/phase6-ttl.md, Stage 1.
 */

#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p6_stage01_data"

static void reset_and_open(void) {
    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);
}

static void test_set_ttl_then_get_immediately_is_a_hit(void) {
    reset_and_open();

    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3, 5),
        0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"foo", 3, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_BYTES(value, value_len, "bar", 3);
    free(value);
}

static void test_get_after_ttl_elapses_is_not_found(void) {
    reset_and_open();

    /* A 1-second TTL is short enough to keep this test fast but long
     * enough not to be flaky on a loaded box. */
    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3, 1),
        0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"foo", 3, &value, &value_len), 1);
    free(value);

    sleep(2);

    value = NULL;
    value_len = 0;
    int rc = kv_store_get((const uint8_t *)"foo", 3, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT(value == NULL);
}

static void test_plain_set_never_expires(void) {
    reset_and_open();

    /* No TTL at all -- must still be a HIT well past where a short
     * TTL would have expired, proving "no TTL" isn't secretly TTL=0. */
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3), 0);

    sleep(2);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"foo", 3, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_BYTES(value, value_len, "bar", 3);
    free(value);
}

static void test_overwrite_with_plain_set_clears_ttl(void) {
    reset_and_open();

    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3, 1),
        0);
    /* Overwrite before it expires, with a plain (no-TTL) SET. */
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foo", 3, (const uint8_t *)"baz", 3), 0);

    sleep(2);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"foo", 3, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_BYTES(value, value_len, "baz", 3);
    free(value);
}

static void test_expiry_survives_a_flush_to_sstable(void) {
    reset_and_open();

    /* One short-TTL key, then enough plain SETs on other keys to force
     * a flush (FLUSH_THRESHOLD is 8, per docs/decisions.md). */
    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"expiring", 8, (const uint8_t *)"bar", 3, 1),
        0);

    char key[16];
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof(key), "filler%d", i);
        KV_ASSERT_EQ_INT(
            kv_store_set((const uint8_t *)key, (uint16_t)strlen(key),
                         (const uint8_t *)"x", 1),
            0);
    }

    sleep(2);

    /* The key is very likely flushed into an SSTable by now (it's the
     * oldest entry and the memtable has been swapped at least once) --
     * either way, GET must still say NOT_FOUND. */
    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"expiring", 8, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT(value == NULL);
}

int main(void) {
    KV_RUN(test_set_ttl_then_get_immediately_is_a_hit);
    KV_RUN(test_get_after_ttl_elapses_is_not_found);
    KV_RUN(test_plain_set_never_expires);
    KV_RUN(test_overwrite_with_plain_set_clears_ttl);
    KV_RUN(test_expiry_survives_a_flush_to_sstable);

    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);

    KV_REPORT_AND_EXIT();
}
