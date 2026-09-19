#ifndef KV_TEST_FRAMEWORK_H
#define KV_TEST_FRAMEWORK_H

/*
 * Minimal header-only test harness. Not Unity/Check on purpose — this
 * project doesn't need fixtures, mocking, or a runner beyond "run every
 * KV_TEST function in main() and report pass/fail", so a ~90-line header
 * is lower-friction than pulling in and vendoring an external framework.
 *
 * Usage in a stage's test file:
 *
 *   #include "test_framework.h"
 *
 *   static void test_something(void) {
 *       KV_ASSERT(1 + 1 == 2);
 *       KV_ASSERT_EQ_INT(net_listen(0, 1) >= 0, 1);
 *   }
 *
 *   int main(void) {
 *       KV_RUN(test_something);
 *       KV_REPORT_AND_EXIT();
 *   }
 */

#include <stdio.h>
#include <string.h>

static int kv_test_assertions = 0;
static int kv_test_failures = 0;
static const char *kv_test_current = "";

#define KV_ASSERT(cond)                                                    \
    do {                                                                   \
        kv_test_assertions++;                                              \
        if (!(cond)) {                                                     \
            kv_test_failures++;                                            \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", kv_test_current,    \
                    __FILE__, __LINE__, #cond);                            \
        }                                                                  \
    } while (0)

#define KV_ASSERT_EQ_INT(actual, expected)                                 \
    do {                                                                   \
        long long kv_a_ = (long long)(actual);                            \
        long long kv_e_ = (long long)(expected);                          \
        kv_test_assertions++;                                              \
        if (kv_a_ != kv_e_) {                                              \
            kv_test_failures++;                                            \
            fprintf(stderr,                                                \
                    "  FAIL [%s] %s:%d: %s == %s (got %lld, want %lld)\n", \
                    kv_test_current, __FILE__, __LINE__, #actual,          \
                    #expected, kv_a_, kv_e_);                              \
        }                                                                  \
    } while (0)

#define KV_ASSERT_EQ_BYTES(actual_ptr, actual_len, expected_ptr, expected_len) \
    do {                                                                   \
        kv_test_assertions++;                                              \
        size_t kv_al_ = (size_t)(actual_len);                             \
        size_t kv_el_ = (size_t)(expected_len);                           \
        if (kv_al_ != kv_el_ ||                                            \
            memcmp((actual_ptr), (expected_ptr), kv_al_) != 0) {           \
            kv_test_failures++;                                            \
            fprintf(stderr,                                                \
                    "  FAIL [%s] %s:%d: %s != %s (len %zu vs %zu)\n",      \
                    kv_test_current, __FILE__, __LINE__, #actual_ptr,      \
                    #expected_ptr, kv_al_, kv_el_);                        \
        }                                                                  \
    } while (0)

#define KV_RUN(test_fn)                                                    \
    do {                                                                   \
        kv_test_current = #test_fn;                                       \
        fprintf(stderr, "-- %s\n", kv_test_current);                      \
        test_fn();                                                         \
    } while (0)

#define KV_REPORT_AND_EXIT()                                               \
    do {                                                                   \
        if (kv_test_failures == 0) {                                       \
            printf("PASS (%d assertions)\n", kv_test_assertions);          \
            return 0;                                                      \
        } else {                                                           \
            printf("FAIL: %d/%d assertions failed\n", kv_test_failures,   \
                   kv_test_assertions);                                    \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#endif /* KV_TEST_FRAMEWORK_H */
