/*
 * Phase 3, Stage 5: engine integration -- src/store.c rewritten to
 * orchestrate memtable + WAL + SSTables (replacing Phase 2's flat
 * array wholesale).
 * Contract under test: include/kv/store.h (now including
 * kv_store_open()) -> src/store.c (you rewrite this).
 * See docs/stages/phase3-lsm-tree.md, Stage 5.
 *
 * Deliberately doesn't assume an exact flush threshold -- it writes
 * generously many keys (50) to force at least one flush regardless of
 * whatever small number you logged in docs/decisions.md, and tests
 * WAL-replay recovery separately with just 1-2 keys (well under any
 * reasonable threshold) so that assertion doesn't depend on the
 * threshold either.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p3_stage05_data"

static void reset_data_dir(void) {
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
}

static int count_files_in_dir(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        count++;
    }
    closedir(d);
    return count;
}

static void make_key(char *buf, size_t buf_len, int i) {
    snprintf(buf, buf_len, "key-%04d", i);
}
static void make_value(char *buf, size_t buf_len, int i) {
    snprintf(buf, buf_len, "value-%04d", i);
}

static void test_enough_sets_force_a_flush_to_disk(void) {
    reset_data_dir();
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);

    int before = count_files_in_dir(TEST_DATA_DIR);

    for (int i = 0; i < 50; i++) {
        char key[32], value[32];
        make_key(key, sizeof(key), i);
        make_value(value, sizeof(value), i);
        KV_ASSERT_EQ_INT(
            kv_store_set((const uint8_t *)key, (uint16_t)strlen(key),
                         (const uint8_t *)value, (uint32_t)strlen(value)),
            0);
    }

    int after = count_files_in_dir(TEST_DATA_DIR);
    /* Something beyond just the WAL must now exist -- at least one
     * SSTable got written out. */
    KV_ASSERT(after > before);

    kv_store_destroy();
}

static void test_get_after_flush_reads_through_to_the_sstable(void) {
    reset_data_dir();
    kv_store_open(TEST_DATA_DIR);

    for (int i = 0; i < 50; i++) {
        char key[32], value[32];
        make_key(key, sizeof(key), i);
        make_value(value, sizeof(value), i);
        kv_store_set((const uint8_t *)key, (uint16_t)strlen(key),
                     (const uint8_t *)value, (uint32_t)strlen(value));
    }

    /* key-0000 was set first -- with 50 sets total and any reasonably
     * small flush threshold, it's almost certainly been flushed out of
     * the current memtable by now, so this GET has to merge-read
     * through to an SSTable to find it. */
    uint8_t *out_value = NULL;
    uint32_t out_value_len = 0;
    int rc = kv_store_get((const uint8_t *)"key-0000", 8, &out_value, &out_value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_BYTES(out_value, out_value_len, "value-0000", 10);
    free(out_value);

    kv_store_destroy();
}

static void test_delete_shadows_a_value_already_flushed_to_an_sstable(void) {
    reset_data_dir();
    kv_store_open(TEST_DATA_DIR);

    KV_ASSERT_EQ_INT(kv_store_set((const uint8_t *)"shadow-key", 10,
                                   (const uint8_t *)"original", 8),
                      0);
    /* Force a flush so "shadow-key" ends up sitting in an SSTable. */
    for (int i = 0; i < 50; i++) {
        char key[32], value[32];
        make_key(key, sizeof(key), i);
        make_value(value, sizeof(value), i);
        kv_store_set((const uint8_t *)key, (uint16_t)strlen(key),
                     (const uint8_t *)value, (uint32_t)strlen(value));
    }

    KV_ASSERT_EQ_INT(kv_store_delete((const uint8_t *)"shadow-key", 10), 1);

    uint8_t *out_value = NULL;
    uint32_t out_value_len = 0;
    /* Must be a miss even though the SSTable underneath still
     * physically contains "original" -- the newer tombstone has to
     * shadow it. */
    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"shadow-key", 10, &out_value, &out_value_len),
        0);

    kv_store_destroy();
}

static void test_data_survives_close_and_reopen_via_wal_replay(void) {
    reset_data_dir();
    kv_store_open(TEST_DATA_DIR);

    /* Just one key -- well under any reasonable flush threshold, so
     * this is specifically testing WAL-replay recovery of unflushed
     * memtable data, not SSTable loading. */
    KV_ASSERT_EQ_INT(kv_store_set((const uint8_t *)"unflushed", 9,
                                   (const uint8_t *)"still-here", 10),
                      0);

    kv_store_destroy();
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);

    uint8_t *out_value = NULL;
    uint32_t out_value_len = 0;
    int rc =
        kv_store_get((const uint8_t *)"unflushed", 9, &out_value, &out_value_len);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_BYTES(out_value, out_value_len, "still-here", 10);
    free(out_value);

    kv_store_destroy();
}

int main(void) {
    KV_RUN(test_enough_sets_force_a_flush_to_disk);
    KV_RUN(test_get_after_flush_reads_through_to_the_sstable);
    KV_RUN(test_delete_shadows_a_value_already_flushed_to_an_sstable);
    KV_RUN(test_data_survives_close_and_reopen_via_wal_replay);
    KV_REPORT_AND_EXIT();
}
