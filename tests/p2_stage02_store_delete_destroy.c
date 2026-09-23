/*
 * Phase 2, Stage 2: kv_store_delete / kv_store_destroy.
 * Contract under test: include/kv/store.h -> src/store.c (you write this,
 * added to Stage 1's code).
 * See docs/stages/phase2-storage.md, Stage 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "kv/store.h"
#include "test_framework.h"

#define TEST_DATA_DIR "build/p2_stage02_data"

/* Phase 3, Stage 5: see the matching helper in
 * tests/p2_stage01_store_set_get.c for why this wipes+reopens rather
 * than relying on the old "no init needed" contract. */
static void reset_and_open(void) {
    kv_store_destroy();
    system("rm -rf " TEST_DATA_DIR);
    mkdir(TEST_DATA_DIR, 0755);
    if (kv_store_open(TEST_DATA_DIR) != 0) {
        fprintf(stderr, "kv_store_open failed\n");
        exit(1);
    }
}

static void test_delete_existing_key(void) {
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"foo", 3, (const uint8_t *)"bar", 3), 0);

    KV_ASSERT_EQ_INT(kv_store_delete((const uint8_t *)"foo", 3), 1);

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"foo", 3, &value, &value_len),
                      0);

    kv_store_destroy();
}

static void test_delete_missing_key_is_a_harmless_no_op(void) {
    KV_ASSERT_EQ_INT(kv_store_delete((const uint8_t *)"never-set", 9), 0);

    kv_store_destroy();
}

/*
 * Populate the store, exercise every free path (a plain set, an
 * overwrite, a delete), then destroy it. Correctness of the operations
 * themselves is asserted here; whether every path actually freed its
 * memory is what `make valgrind-p2` checks on this same test binary --
 * a leak in any one of set's overwrite-free, delete's free, or
 * destroy's free-everything would show up there, not here.
 */
static void test_destroy_after_mixed_operations_leaves_it_reusable(void) {
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"a", 1, (const uint8_t *)"1", 1), 0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"b", 1, (const uint8_t *)"2", 1), 0);
    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"a", 1, (const uint8_t *)"1-updated", 9),
        0); /* overwrite */
    KV_ASSERT_EQ_INT(kv_store_delete((const uint8_t *)"b", 1), 1);

    kv_store_destroy();

    /* Phase 3, Stage 5: destroy() no longer leaves the store
     * immediately reusable on its own -- it now also closes the WAL
     * and every open SSTable (see store.h's doc comment), so it has to
     * be reopened before use, same as at process start. Wipe the
     * directory too, so "usable again" means "reset to empty," not
     * "recovers what was just destroyed" via WAL replay. */
    reset_and_open();

    uint8_t *value = NULL;
    uint32_t value_len = 0;
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"a", 1, &value, &value_len),
                      0); /* destroy cleared it -- "a" is gone too */

    KV_ASSERT_EQ_INT(
        kv_store_set((const uint8_t *)"c", 1, (const uint8_t *)"3", 1), 0);
    KV_ASSERT_EQ_INT(kv_store_get((const uint8_t *)"c", 1, &value, &value_len),
                      1);
    KV_ASSERT_EQ_BYTES(value, value_len, "3", 1);
    free(value);

    kv_store_destroy();
}

static void test_destroy_on_already_empty_store_is_safe(void) {
    kv_store_destroy();
    kv_store_destroy(); /* twice in a row, still safe */
}

int main(void) {
    reset_and_open();
    KV_RUN(test_delete_existing_key);
    reset_and_open();
    KV_RUN(test_delete_missing_key_is_a_harmless_no_op);
    reset_and_open();
    KV_RUN(test_destroy_after_mixed_operations_leaves_it_reusable);
    reset_and_open();
    KV_RUN(test_destroy_on_already_empty_store_is_safe);
    kv_store_destroy();
    KV_REPORT_AND_EXIT();
}
