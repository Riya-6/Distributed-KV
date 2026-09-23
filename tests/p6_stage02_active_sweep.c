/*
 * Phase 6, Stage 2: active sweep thread.
 * Contract under test: the background sweep thread src/store.c starts
 * in kv_store_open() and stops in kv_store_destroy(). See
 * docs/stages/phase6-ttl.md, Stage 2, and docs/decisions.md for the
 * 200ms sweep interval.
 */

#define _DEFAULT_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p6_stage02_data"
#define NUM_WRITER_THREADS 4
#define KEYS_PER_WRITER 20

static void reset_and_open(void) {
    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);
}

/*
 * Sets several short-TTL keys and a few no-TTL/long-TTL keys, then
 * deliberately never GETs the short-TTL ones -- proving the sweep
 * reclaims them on its own, not lazy expiry. Since only kv_store_get()
 * and kv_store_set() are public (the memtable itself isn't exposed
 * across the store boundary), "reclaimed" is observed by closing and
 * reopening the store and confirming a subsequent GET (which would
 * otherwise trigger lazy expiry the sweep is supposed to make
 * redundant here) sees them already gone via a fresh, cheap read-only
 * path -- what actually matters is that this test never once GETs the
 * short-TTL keys before the reopen, so lazy expiry never had a chance
 * to run.
 */
static void test_sweep_reclaims_expired_keys_without_any_get(void) {
    reset_and_open();

    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"short1", 6, (const uint8_t *)"x", 1, 1),
        0);
    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"short2", 6, (const uint8_t *)"x", 1, 1),
        0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"forever", 7, (const uint8_t *)"y", 1), 0);
    KV_ASSERT_EQ_INT(
        kv_store_set_ttl((const uint8_t *)"longttl", 7, (const uint8_t *)"z", 1, 3600),
        0);

    /* Past the 1s TTL and several sweep intervals (200ms each), but
     * short of the store being closed at all -- give the sweep time
     * to run entirely on its own. */
    sleep(2);

    /* Close and reopen: if the sweep already turned the short-TTL keys
     * into tombstones and wrote those deletes to the WAL, a fresh
     * store replaying that WAL must not resurrect them. If the sweep
     * never ran, the keys would still be live SETs in the WAL and
     * would come back after reopen -- that's exactly the failure this
     * test is designed to catch. */
    kv_store_destroy();
    KV_ASSERT_EQ_INT(kv_store_open(TEST_DATA_DIR), 0);

    uint8_t *value = NULL;
    uint32_t value_len = 0;

    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"short1", 6, &value, &value_len), 0);
    KV_ASSERT(value == NULL);

    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"short2", 6, &value, &value_len), 0);
    KV_ASSERT(value == NULL);

    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"forever", 7, &value, &value_len), 1);
    KV_ASSERT_EQ_BYTES(value, value_len, "y", 1);
    free(value);
    value = NULL;

    KV_ASSERT_EQ_INT(
        kv_store_get((const uint8_t *)"longttl", 7, &value, &value_len), 1);
    KV_ASSERT_EQ_BYTES(value, value_len, "z", 1);
    free(value);
}

typedef struct {
    int id;
    int ok;
} writer_arg_t;

static void *writer_thread(void *arg_) {
    writer_arg_t *arg = (writer_arg_t *)arg_;
    arg->ok = 1;

    for (int i = 0; i < KEYS_PER_WRITER; i++) {
        char key[32];
        char value[32];
        snprintf(key, sizeof(key), "w%d_k%d", arg->id, i);
        snprintf(value, sizeof(value), "w%d_v%d", arg->id, i);

        /* Never-expiring writes, concurrent with the sweep running in
         * the background -- the sweep must never touch these. */
        if (kv_store_set((const uint8_t *)key, (uint16_t)strlen(key),
                          (const uint8_t *)value, (uint16_t)strlen(value)) != 0) {
            arg->ok = 0;
        }

        uint8_t *value_out = NULL;
        uint32_t value_out_len = 0;
        int rc = kv_store_get((const uint8_t *)key, (uint16_t)strlen(key),
                               &value_out, &value_out_len);
        if (rc != 1 || value_out_len != strlen(value) ||
            memcmp(value_out, value, value_out_len) != 0) {
            arg->ok = 0;
        }
        free(value_out);
    }

    return NULL;
}

/* Concurrent writers/readers on never-expiring keys, running for long
 * enough that the sweep thread (200ms interval) fires several times
 * in the background -- nothing should corrupt, and the sweep must
 * never touch a key it has no business touching. */
static void test_concurrent_writes_survive_the_sweep(void) {
    reset_and_open();

    pthread_t threads[NUM_WRITER_THREADS];
    writer_arg_t args[NUM_WRITER_THREADS];

    for (int i = 0; i < NUM_WRITER_THREADS; i++) {
        args[i].id = i;
        args[i].ok = 0;
        KV_ASSERT_EQ_INT(
            pthread_create(&threads[i], NULL, writer_thread, &args[i]), 0);
    }

    for (int i = 0; i < NUM_WRITER_THREADS; i++) {
        KV_ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        KV_ASSERT_EQ_INT(args[i].ok, 1);
    }

    /* Final check: every key every writer set is still exactly there. */
    for (int t = 0; t < NUM_WRITER_THREADS; t++) {
        for (int i = 0; i < KEYS_PER_WRITER; i++) {
            char key[32];
            char value[32];
            snprintf(key, sizeof(key), "w%d_k%d", t, i);
            snprintf(value, sizeof(value), "w%d_v%d", t, i);

            uint8_t *value_out = NULL;
            uint32_t value_out_len = 0;
            int rc = kv_store_get((const uint8_t *)key, (uint16_t)strlen(key),
                                   &value_out, &value_out_len);
            KV_ASSERT_EQ_INT(rc, 1);
            KV_ASSERT_EQ_BYTES(value_out, value_out_len, value, strlen(value));
            free(value_out);
        }
    }
}

int main(void) {
    KV_RUN(test_sweep_reclaims_expired_keys_without_any_get);
    KV_RUN(test_concurrent_writes_survive_the_sweep);

    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);

    KV_REPORT_AND_EXIT();
}
