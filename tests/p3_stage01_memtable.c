/*
 * Phase 3, Stage 1: memtable.
 * Contract under test: include/kv/memtable.h -> src/memtable.c (you write this).
 * See docs/stages/phase3-lsm-tree.md, Stage 1.
 */

#include <stdlib.h>
#include <string.h>

#include "kv/memtable.h"
#include "test_framework.h"

static void test_put_then_get_roundtrips(void) {
    kv_memtable_t *mt = kv_memtable_create();
    KV_ASSERT(mt != NULL);

    KV_ASSERT_EQ_INT(
        kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3),
        0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    kv_lookup_result_t rc = kv_memtable_get(mt, (const uint8_t *)"foo", 3, &value,
                                             &value_len);
    KV_ASSERT_EQ_INT(rc, KV_LOOKUP_HIT);
    KV_ASSERT_EQ_BYTES(value, value_len, "bar", 3);
    free(value);

    kv_memtable_destroy(mt);
}

static void test_get_missing_key_is_miss(void) {
    kv_memtable_t *mt = kv_memtable_create();
    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(
        kv_memtable_get(mt, (const uint8_t *)"nope", 4, &value, &value_len),
        KV_LOOKUP_MISS);
    kv_memtable_destroy(mt);
}

static void test_delete_makes_a_tombstone_not_a_miss(void) {
    kv_memtable_t *mt = kv_memtable_create();
    KV_ASSERT_EQ_INT(
        kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3),
        0);
    KV_ASSERT_EQ_INT(kv_memtable_delete(mt, (const uint8_t *)"foo", 3), 0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    kv_lookup_result_t rc = kv_memtable_get(mt, (const uint8_t *)"foo", 3, &value,
                                             &value_len);
    /* Deliberately not MISS -- a tombstone is a distinct state from
     * "never existed", so a merge-read across layers (Stage 5) knows
     * to stop here instead of falling through to an older SSTable. */
    KV_ASSERT_EQ_INT(rc, KV_LOOKUP_TOMBSTONE);

    kv_memtable_destroy(mt);
}

static void test_delete_on_never_set_key_still_tombstones(void) {
    /* A memtable can't know whether the key exists in an older SSTable
     * -- it must record the tombstone regardless of whether it had
     * ever seen this key before. */
    kv_memtable_t *mt = kv_memtable_create();
    KV_ASSERT_EQ_INT(kv_memtable_delete(mt, (const uint8_t *)"ghost", 5), 0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(
        kv_memtable_get(mt, (const uint8_t *)"ghost", 5, &value, &value_len),
        KV_LOOKUP_TOMBSTONE);

    kv_memtable_destroy(mt);
}

static void test_put_after_delete_revives_the_key(void) {
    kv_memtable_t *mt = kv_memtable_create();
    KV_ASSERT_EQ_INT(
        kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"v1", 2),
        0);
    KV_ASSERT_EQ_INT(kv_memtable_delete(mt, (const uint8_t *)"foo", 3), 0);
    KV_ASSERT_EQ_INT(
        kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"v2", 2),
        0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(
        kv_memtable_get(mt, (const uint8_t *)"foo", 3, &value, &value_len),
        KV_LOOKUP_HIT);
    KV_ASSERT_EQ_BYTES(value, value_len, "v2", 2);
    free(value);

    kv_memtable_destroy(mt);
}

static void test_entry_at_visits_in_ascending_key_order(void) {
    kv_memtable_t *mt = kv_memtable_create();
    /* Inserted out of order on purpose. */
    kv_memtable_put(mt, (const uint8_t *)"charlie", 7, (const uint8_t *)"3", 1);
    kv_memtable_put(mt, (const uint8_t *)"alpha", 5, (const uint8_t *)"1", 1);
    kv_memtable_delete(mt, (const uint8_t *)"bravo", 5); /* a tombstone in the mix */

    KV_ASSERT_EQ_INT(kv_memtable_count(mt), 3);

    static const char *expected_keys[] = {"alpha", "bravo", "charlie"};
    static const size_t expected_lens[] = {5, 5, 7};
    for (size_t i = 0; i < 3; i++) {
        kv_entry_view_t view;
        KV_ASSERT_EQ_INT(kv_memtable_entry_at(mt, i, &view), 0);
        KV_ASSERT_EQ_BYTES(view.key, view.key_len, expected_keys[i],
                            expected_lens[i]);
    }
    /* "bravo" (index 1) is the tombstone. */
    kv_entry_view_t bravo_view;
    kv_memtable_entry_at(mt, 1, &bravo_view);
    KV_ASSERT_EQ_INT(bravo_view.is_tombstone, 1);

    KV_ASSERT_EQ_INT(kv_memtable_entry_at(mt, 3, &bravo_view), -1); /* out of range */

    kv_memtable_destroy(mt);
}

int main(void) {
    KV_RUN(test_put_then_get_roundtrips);
    KV_RUN(test_get_missing_key_is_miss);
    KV_RUN(test_delete_makes_a_tombstone_not_a_miss);
    KV_RUN(test_delete_on_never_set_key_still_tombstones);
    KV_RUN(test_put_after_delete_revives_the_key);
    KV_RUN(test_entry_at_visits_in_ascending_key_order);
    KV_REPORT_AND_EXIT();
}
