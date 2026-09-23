/*
 * Phase 4, Stage 2: race-condition tests against the storage layer,
 * plus the lock upgrade (pthread_mutex_t -> pthread_rwlock_t).
 * Contract under test: kv_store_* -> src/store.c (you edit the lock
 * type/usage; signatures and behavior are unchanged).
 * See docs/stages/phase4-concurrency.md, Stage 2.
 *
 * A clean pass here does NOT by itself prove there's no race -- a
 * buggy implementation can still pass by luck on a given scheduler.
 * `make helgrind-p4-s2` (valgrind --tool=helgrind on this same binary)
 * is the check that actually matters; this file's assertions catch
 * functional wrongness (a corrupted/torn value), helgrind catches the
 * race itself.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p4_stage02_data"

#define NUM_WRITER_THREADS 4
#define ITERS_PER_THREAD 50
#define NUM_READER_THREADS 2

/*
 * -- Scenario 1: concurrent writers on the SAME key --
 * Every writer thread repeatedly SETs one shared key to its own
 * distinct, identifiable value. Readers hammer GET on that same key
 * throughout. After everyone joins, the final value must be exactly
 * one thread's actual last-written value -- never a mix of two, never
 * garbage.
 */
static char g_last_value[NUM_WRITER_THREADS][32];

static void *same_key_writer(void *arg) {
    int tid = *(int *)arg;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        char value[32];
        int len = snprintf(value, sizeof(value), "t%d-i%d", tid, i);
        kv_store_set((const uint8_t *)"shared", 6, (const uint8_t *)value,
                     (uint32_t)len);
        if (i == ITERS_PER_THREAD - 1) {
            memcpy(g_last_value[tid], value, (size_t)len + 1);
        }
    }
    return NULL;
}

static void *same_key_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS_PER_THREAD * NUM_WRITER_THREADS; i++) {
        uint8_t *value = NULL;
        uint32_t value_len = 0;
        int rc = kv_store_get((const uint8_t *)"shared", 6, &value, &value_len);
        if (rc == 1) {
            /* Every value this test ever writes fits comfortably under
             * this -- a longer or garbage length would indicate a torn
             * read, not just an unlucky timing snapshot. */
            KV_ASSERT(value_len < sizeof(g_last_value[0]));
            free(value);
        }
    }
    return NULL;
}

static void test_concurrent_writers_on_same_key_never_produce_a_torn_value(void) {
    pthread_t writers[NUM_WRITER_THREADS];
    pthread_t readers[NUM_READER_THREADS];
    int tids[NUM_WRITER_THREADS];

    for (int t = 0; t < NUM_WRITER_THREADS; t++) {
        tids[t] = t;
        KV_ASSERT_EQ_INT(pthread_create(&writers[t], NULL, same_key_writer, &tids[t]),
                          0);
    }
    for (int t = 0; t < NUM_READER_THREADS; t++) {
        KV_ASSERT_EQ_INT(pthread_create(&readers[t], NULL, same_key_reader, NULL), 0);
    }
    for (int t = 0; t < NUM_WRITER_THREADS; t++) pthread_join(writers[t], NULL);
    for (int t = 0; t < NUM_READER_THREADS; t++) pthread_join(readers[t], NULL);

    uint8_t *final_value = NULL;
    uint32_t final_len = 0;
    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"shared", 6, &final_value, &final_len), 1);

    int matched_some_thread = 0;
    for (int t = 0; t < NUM_WRITER_THREADS; t++) {
        size_t expected_len = strlen(g_last_value[t]);
        if (final_len == expected_len &&
            memcmp(final_value, g_last_value[t], expected_len) == 0) {
            matched_some_thread = 1;
            break;
        }
    }
    KV_ASSERT(matched_some_thread);
    free(final_value);
}

/*
 * -- Scenario 2: concurrent writers on DISTINCT keys --
 * Each thread owns a private set of keys no other thread ever touches,
 * so there's no ambiguity about the expected final value -- this
 * scenario is about proving the flush path (crossing FLUSH_THRESHOLD
 * while writers are still active on other keys) doesn't corrupt
 * anything, not about resolving a genuine race.
 */
#define KEYS_PER_THREAD 10

static void *distinct_key_writer(void *arg) {
    int tid = *(int *)arg;
    for (int i = 0; i < KEYS_PER_THREAD; i++) {
        char key[32], value[32];
        int key_len = snprintf(key, sizeof(key), "dk-%d-%d", tid, i);
        int value_len = snprintf(value, sizeof(value), "val-%d-%d", tid, i);
        kv_store_set((const uint8_t *)key, (uint16_t)key_len,
                     (const uint8_t *)value, (uint32_t)value_len);
    }
    return NULL;
}

static void test_concurrent_writers_on_distinct_keys_all_land_correctly(void) {
    pthread_t writers[NUM_WRITER_THREADS];
    int tids[NUM_WRITER_THREADS];

    for (int t = 0; t < NUM_WRITER_THREADS; t++) {
        tids[t] = t;
        KV_ASSERT_EQ_INT(
            pthread_create(&writers[t], NULL, distinct_key_writer, &tids[t]), 0);
    }
    for (int t = 0; t < NUM_WRITER_THREADS; t++) pthread_join(writers[t], NULL);

    for (int t = 0; t < NUM_WRITER_THREADS; t++) {
        for (int i = 0; i < KEYS_PER_THREAD; i++) {
            char key[32], expected_value[32];
            int key_len = snprintf(key, sizeof(key), "dk-%d-%d", t, i);
            int expected_len = snprintf(expected_value, sizeof(expected_value),
                                         "val-%d-%d", t, i);

            uint8_t *value = NULL;
            uint32_t value_len = 0;
            int rc = kv_store_get((const uint8_t *)key, (uint16_t)key_len, &value,
                                   &value_len);
            KV_ASSERT_EQ_INT(rc, 1);
            KV_ASSERT_EQ_BYTES(value, value_len, expected_value, (size_t)expected_len);
            free(value);
        }
    }
}

int main(void) {
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    if (kv_store_open(TEST_DATA_DIR) != 0) {
        fprintf(stderr, "kv_store_open failed\n");
        return 1;
    }

    KV_RUN(test_concurrent_writers_on_same_key_never_produce_a_torn_value);
    KV_RUN(test_concurrent_writers_on_distinct_keys_all_land_correctly);

    kv_store_destroy();
    KV_REPORT_AND_EXIT();
}
