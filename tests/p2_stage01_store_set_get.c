/*
 * Phase 2, Stage 1: kv_store_set / kv_store_get.
 * Contract under test: include/kv/store.h -> src/store.c (you write this).
 * See docs/stages/phase2-storage.md, Stage 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p2_stage01_data"

/*
 * Phase 3, Stage 5: the store persists to disk now (docs/stages/
 * phase3-lsm-tree.md), so "no init needed, zero value = empty" no
 * longer holds -- each test needs a fresh, wiped directory opened
 * before it can call kv_store_*() at all. Wipes+reopens rather than
 * just destroy()-then-reuse-the-same-dir: a prior test's WAL entries
 * are still durably on disk after its own kv_store_destroy() call, and
 * reopening the same directory would replay them right back in,
 * breaking test isolation.
 */
static void reset_and_open(void) {
    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    if (kv_store_open(TEST_DATA_DIR) != 0) {
        fprintf(stderr, "kv_store_open failed\n");
        exit(1);
    }
}

static void test_set_then_get_roundtrips(void) {
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3), 0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"foo", 3, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(value_len, 3);
    KV_ASSERT_EQ_BYTES(value, value_len, "bar", 3);
    free(value);

    kv_store_destroy();
}

static void test_get_missing_key_returns_zero(void) {
    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"nope", 4, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 0);

    kv_store_destroy();
}

static void test_overwrite_existing_key(void) {
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"k", 1, (const uint8_t *)"v1", 2), 0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"k", 1, (const uint8_t *)"v2-longer", 9),
        0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    int rc = kv_store_get((const uint8_t *)"k", 1, &value, &value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(value_len, 9);
    KV_ASSERT_EQ_BYTES(value, value_len, "v2-longer", 9);
    free(value);

    /* If the first value ("v1") wasn't freed on overwrite, this test
     * won't fail here -- valgrind (make valgrind-p2) is what actually
     * catches that leak. */
    kv_store_destroy();
}

static void test_distinct_keys_do_not_collide(void) {
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"a", 1, (const uint8_t *)"1", 1), 0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"b", 1, (const uint8_t *)"2", 1), 0);

    uint8_t *va = NULL, *vb = NULL;
    uint32_t la = 0, lb = 0;
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"a", 1, &va, &la), 1);
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"b", 1, &vb, &lb), 1);
    KV_ASSERT_EQ_BYTES(va, la, "1", 1);
    KV_ASSERT_EQ_BYTES(vb, lb, "2", 1);
    free(va);
    free(vb);

    kv_store_destroy();
}

static void test_prefix_keys_of_different_lengths_do_not_collide(void) {
    /* "foo" and "foobar" share a byte prefix -- a comparison that only
     * checks bytes up to the shorter key's length (instead of also
     * checking key_len equality) would wrongly treat these as the same
     * key. */
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foo", 3, (const uint8_t *)"short", 5),
        0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foobar", 6, (const uint8_t *)"long", 4),
        0);

    uint8_t *v_foo = NULL, *v_foobar = NULL;
    uint32_t l_foo = 0, l_foobar = 0;
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"foo", 3, &v_foo, &l_foo), 1);
    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"foobar", 6, &v_foobar, &l_foobar), 1);
    KV_ASSERT_EQ_BYTES(v_foo, l_foo, "short", 5);
    KV_ASSERT_EQ_BYTES(v_foobar, l_foobar, "long", 4);
    free(v_foo);
    free(v_foobar);

    kv_store_destroy();
}

int main(void) {
    reset_and_open();
    KV_RUN(test_set_then_get_roundtrips);
    reset_and_open();
    KV_RUN(test_get_missing_key_returns_zero);
    reset_and_open();
    KV_RUN(test_overwrite_existing_key);
    reset_and_open();
    KV_RUN(test_distinct_keys_do_not_collide);
    reset_and_open();
    KV_RUN(test_prefix_keys_of_different_lengths_do_not_collide);
    kv_store_destroy();
    KV_REPORT_AND_EXIT();
}
