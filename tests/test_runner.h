/*
 * test_runner.h — Minimal test framework
 * =============================================================================
 * Provides a simple ASSERT macro and helpers for reporting results.
 *
 * Usage in a test file:
 *
 *   #include "test_runner.h"
 *
 *   TEST(my_test) {
 *       ASSERT(1 + 1 == 2, "basic arithmetic");
 *   }
 *
 *   int main(void) {
 *       printf("=== my_tests ===\n");
 *       RUN(my_test);
 *       TEST_SUMMARY();
 *   }
 *
 * Each test file compiles to its own binary and returns exit code 0 on
 * success, 1 if any assertion failed.  `make test` runs them all.
 * =============================================================================
 */

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>

/* Global counters — defined as static so each .c file gets its own copy. */
static int tests_passed = 0;
static int tests_failed = 0;

/*
 * ASSERT — check a condition.
 *
 * Prints a failure message (with file and line number) if `cond` is false.
 * Always increments either tests_passed or tests_failed.
 */
#define ASSERT(cond, msg)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            tests_passed++;                                                 \
        } else {                                                            \
            fprintf(stderr, "  FAIL: %s\n        at %s:%d\n",              \
                    (msg), __FILE__, __LINE__);                             \
            tests_failed++;                                                 \
        }                                                                   \
    } while (0)

/*
 * TEST — declare a test function.
 * Just a readable alias for `static void`.
 */
#define TEST(name) static void name(void)

/*
 * RUN — call a test function and print its name.
 */
#define RUN(name)                                                           \
    do {                                                                    \
        printf("  " #name "\n");                                            \
        name();                                                             \
    } while (0)

/*
 * TEST_SUMMARY — print totals and return the right exit code.
 * Put this at the end of main().
 */
#define TEST_SUMMARY()                                                      \
    do {                                                                    \
        if (tests_failed == 0) {                                            \
            printf("  All %d tests passed.\n\n", tests_passed);            \
        } else {                                                            \
            printf("  %d passed, %d FAILED.\n\n",                          \
                   tests_passed, tests_failed);                             \
        }                                                                   \
        return tests_failed > 0 ? 1 : 0;                                   \
    } while (0)

#endif /* TEST_RUNNER_H */
