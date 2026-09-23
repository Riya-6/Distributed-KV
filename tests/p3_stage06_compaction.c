/*
 * Phase 3, Stage 6: compaction.
 * Contract under test: include/kv/sstable.h's kv_sstable_compact() ->
 * src/sstable.c or src/compaction.c (your call -- you write this).
 * See docs/stages/phase3-lsm-tree.md, Stage 6.
 */

#include <stdio.h>
#include <stdlib.h>

#include "kv/memtable.h"
#include "kv/sstable.h"
#include "test_framework.h"

#define OLDER_PATH "build/p3_stage06_older.sst"
#define NEWER_PATH "build/p3_stage06_newer.sst"
#define OUT_PATH "build/p3_stage06_compacted.sst"

static void write_sstable(const char *path, void (*fill)(kv_memtable_t *mt)) {
    remove(path);
    kv_memtable_t *mt = kv_memtable_create();
    fill(mt);
    kv_sstable_write(path, mt);
    kv_memtable_destroy(mt);
}

static void fill_older(kv_memtable_t *mt) {
    kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"old", 3);
    kv_memtable_put(mt, (const uint8_t *)"bar", 3, (const uint8_t *)"b", 1);
    kv_memtable_put(mt, (const uint8_t *)"shadow", 6, (const uint8_t *)"was-here",
                     8);
}

static void fill_newer(kv_memtable_t *mt) {
    kv_memtable_put(mt, (const uint8_t *)"foo", 3, (const uint8_t *)"new", 3);
    kv_memtable_delete(mt, (const uint8_t *)"shadow", 6); /* tombstone */
}

static void test_compact_merges_overwrites_drops_shadowed_tombstone(void) {
    write_sstable(OLDER_PATH, fill_older);
    write_sstable(NEWER_PATH, fill_newer);
    remove(OUT_PATH);

    const char *paths[] = {OLDER_PATH, NEWER_PATH}; /* oldest first */
    KV_ASSERT_EQ_INT(kv_sstable_compact(paths, 2, OUT_PATH), 0);

    kv_sstable_t *out = kv_sstable_open(OUT_PATH);
    KV_ASSERT(out != NULL);

    uint8_t *value = NULL;
    uint32_t value_len = 0;

    /* Newer value wins over older's for the same key. */
    KV_ASSERT_EQ_INT(kv_sstable_get(out, (const uint8_t *)"foo", 3, &value, &value_len),
                      KV_LOOKUP_HIT);
    KV_ASSERT_EQ_BYTES(value, value_len, "new", 3);
    free(value);

    /* Untouched-in-newer key from older survives, unchanged. */
    KV_ASSERT_EQ_INT(kv_sstable_get(out, (const uint8_t *)"bar", 3, &value, &value_len),
                      KV_LOOKUP_HIT);
    KV_ASSERT_EQ_BYTES(value, value_len, "b", 1);
    free(value);

    /* A tombstone in the newest layer, with every older layer included
     * in this compaction, means the key is gone entirely -- not even
     * present as a tombstone anymore. */
    KV_ASSERT_EQ_INT(
        kv_sstable_get(out, (const uint8_t *)"shadow", 6, &value, &value_len),
        KV_LOOKUP_MISS);

    /* Exactly 2 entries survive: foo and bar. */
    KV_ASSERT_EQ_INT(kv_sstable_count(out), 2);

    kv_sstable_close(out);
    remove(OLDER_PATH);
    remove(NEWER_PATH);
    remove(OUT_PATH);
}

int main(void) {
    KV_RUN(test_compact_merges_overwrites_drops_shadowed_tombstone);
    KV_REPORT_AND_EXIT();
}
