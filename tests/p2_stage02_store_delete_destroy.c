/*
 * Phase 2, Stage 2: kv_store_delete / kv_store_destroy.
 * Contract under test: include/kv/store.h -> src/store.c (you write this,
 * added to Stage 1's code).
 * See docs/stages/phase2-storage.md, Stage 2.
 */

#include <stdlib.h>

#include "kv/store.h"
#include "test_framework.h"

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

    /* The store must be usable again immediately -- this is a reset,
     * not a one-way teardown. */
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
    KV_RUN(test_delete_existing_key);
    KV_RUN(test_delete_missing_key_is_a_harmless_no_op);
    KV_RUN(test_destroy_after_mixed_operations_leaves_it_reusable);
    KV_RUN(test_destroy_on_already_empty_store_is_safe);
    KV_REPORT_AND_EXIT();
}
