/*
 * Phase 3, Stage 4: SSTable read path.
 * Contract under test: include/kv/sstable.h's kv_sstable_open/get/
 * entry_at/close -> src/sstable.c (you write this, added to Stage 3's
 * code).
 * See docs/stages/phase3-lsm-tree.md, Stage 4.
 */

#include <stdio.h>
#include <stdlib.h>

#include "kv/memtable.h"
#include "kv/sstable.h"
#include "test_framework.h"

#define TEST_SSTABLE_PATH "build/p3_stage04_test.sst"

static void test_get_hit_tombstone_and_miss(void) {
    remove(TEST_SSTABLE_PATH);

    kv_memtable_t *mt = kv_memtable_create();
    kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3);
    kv_memtable_delete(mt, (const uint8_t *)"deleted-key", 11);
    KV_ASSERT_EQ_INT(kv_sstable_write(TEST_SSTABLE_PATH, mt), 0);
    kv_memtable_destroy(mt);

    kv_sstable_t *sst = kv_sstable_open(TEST_SSTABLE_PATH);
    KV_ASSERT(sst != NULL);

    uint8_t *value = NULL;
    uint32_t value_len = 0;

    KV_ASSERT_EQ_INT(
        kv_sstable_get(sst, (const uint8_t *)"foo", 3, &value, &value_len),
        KV_LOOKUP_HIT);
    KV_ASSERT_EQ_BYTES(value, value_len, "bar", 3);
    free(value);

    KV_ASSERT_EQ_INT(
        kv_sstable_get(sst, (const uint8_t *)"deleted-key", 11, &value, &value_len),
        KV_LOOKUP_TOMBSTONE);

    KV_ASSERT_EQ_INT(
        kv_sstable_get(sst, (const uint8_t *)"never-here", 10, &value, &value_len),
        KV_LOOKUP_MISS);

    kv_sstable_close(sst);
    remove(TEST_SSTABLE_PATH);
}

static void test_entry_at_matches_ascending_key_order(void) {
    remove(TEST_SSTABLE_PATH);

    kv_memtable_t *mt = kv_memtable_create();
    kv_memtable_put(mt, (const uint8_t *)"zebra", 5, (const uint8_t *)"z", 1);
    kv_memtable_put(mt, (const uint8_t *)"apple", 5, (const uint8_t *)"a", 1);
    kv_sstable_write(TEST_SSTABLE_PATH, mt);
    kv_memtable_destroy(mt);

    kv_sstable_t *sst = kv_sstable_open(TEST_SSTABLE_PATH);
    KV_ASSERT_EQ_INT(kv_sstable_count(sst), 2);

    kv_entry_view_t view;
    KV_ASSERT_EQ_INT(kv_sstable_entry_at(sst, 0, &view), 0);
    KV_ASSERT_EQ_BYTES(view.key, view.key_len, "apple", 5);
    KV_ASSERT_EQ_INT(kv_sstable_entry_at(sst, 1, &view), 0);
    KV_ASSERT_EQ_BYTES(view.key, view.key_len, "zebra", 5);
    KV_ASSERT_EQ_INT(kv_sstable_entry_at(sst, 2, &view), -1); /* out of range */

    kv_sstable_close(sst);
    remove(TEST_SSTABLE_PATH);
}

int main(void) {
    KV_RUN(test_get_hit_tombstone_and_miss);
    KV_RUN(test_entry_at_matches_ascending_key_order);
    KV_REPORT_AND_EXIT();
}
