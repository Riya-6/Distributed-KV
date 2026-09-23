/*
 * Phase 4, Stage 3: concurrent read during compaction. Adds no source
 * code -- this locks in an invariant Phase 3's design already
 * provides (kv_sstable_open() copies the whole file into its own
 * private memory, and kv_sstable_compact() never mutates an existing
 * SSTable file, only ever reads inputs and writes a brand-new output).
 * See docs/stages/phase4-concurrency.md, Stage 3.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv/memtable.h"
#include "kv/sstable.h"
#include "test_framework.h"

#define OLDER_PATH "build/p4_stage03_older.sst"
#define NEWER_PATH "build/p4_stage03_newer.sst"
#define OUT_PATH "build/p4_stage03_compacted.sst"

#define NUM_READER_THREADS 4
#define READS_PER_THREAD 200

typedef struct {
    const char *path;
    const char *expected_key;
    size_t expected_key_len;
    const char *expected_value;
    size_t expected_value_len;
} reader_arg_t;

static void *reader_thread(void *arg_) {
    reader_arg_t *arg = (reader_arg_t *)arg_;
    for (int i = 0; i < READS_PER_THREAD; i++) {
        /* Deliberately opens its own independent handle every
         * iteration rather than sharing one across threads -- this is
         * exactly the "no shared mutable state" property being tested. */
        kv_sstable_t *sst = kv_sstable_open(arg->path);
        if (sst == NULL) continue; /* compaction never deletes inputs, but be defensive */

        uint8_t *value = NULL;
        uint32_t value_len = 0;
        kv_lookup_result_t rc =
            kv_sstable_get(sst, (const uint8_t *)arg->expected_key,
                            (uint16_t)arg->expected_key_len, &value, &value_len);
        KV_ASSERT_EQ_INT(rc, KV_LOOKUP_HIT);
        KV_ASSERT_EQ_BYTES(value, value_len, arg->expected_value,
                            arg->expected_value_len);
        free(value);
        kv_sstable_close(sst);
    }
    return NULL;
}

static void *compactor_thread(void *arg) {
    (void)arg;
    const char *paths[] = {OLDER_PATH, NEWER_PATH};
    kv_sstable_compact(paths, 2, OUT_PATH);
    return NULL;
}

static void write_sstable(const char *path, const char *key, const char *value) {
    remove(path);
    kv_memtable_t *mt = kv_memtable_create();
    kv_memtable_put(mt, (const uint8_t *)key, (uint16_t)strlen(key),
                     (const uint8_t *)value, (uint32_t)strlen(value));
    kv_sstable_write(path, mt);
    kv_memtable_destroy(mt);
}

static void test_readers_see_consistent_data_throughout_a_concurrent_compaction(void) {
    remove(OUT_PATH);
    write_sstable(OLDER_PATH, "older-key", "older-value");
    write_sstable(NEWER_PATH, "newer-key", "newer-value");

    reader_arg_t older_arg = {OLDER_PATH, "older-key", 9, "older-value", 11};
    reader_arg_t newer_arg = {NEWER_PATH, "newer-key", 9, "newer-value", 11};

    pthread_t readers[NUM_READER_THREADS];
    pthread_t compactor;

    KV_ASSERT_EQ_INT(pthread_create(&compactor, NULL, compactor_thread, NULL), 0);
    for (int t = 0; t < NUM_READER_THREADS; t++) {
        reader_arg_t *arg = (t % 2 == 0) ? &older_arg : &newer_arg;
        KV_ASSERT_EQ_INT(pthread_create(&readers[t], NULL, reader_thread, arg), 0);
    }

    for (int t = 0; t < NUM_READER_THREADS; t++) pthread_join(readers[t], NULL);
    pthread_join(compactor, NULL);

    /* Sanity check the compaction itself actually produced a correct
     * result while all that reading was happening concurrently. */
    kv_sstable_t *out = kv_sstable_open(OUT_PATH);
    KV_ASSERT(out != NULL);
    KV_ASSERT_EQ_INT(kv_sstable_count(out), 2);
    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(
        kv_sstable_get(out, (const uint8_t *)"older-key", 9, &value, &value_len),
        KV_LOOKUP_HIT);
    free(value);
    kv_sstable_close(out);

    remove(OLDER_PATH);
    remove(NEWER_PATH);
    remove(OUT_PATH);
}

int main(void) {
    KV_RUN(test_readers_see_consistent_data_throughout_a_concurrent_compaction);
    KV_REPORT_AND_EXIT();
}
