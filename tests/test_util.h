/*
 * test_util.h - Minimal assertion harness.
 *
 * Deliberately dependency-free so the core can be tested with nothing but the
 * clang that ships with the Command Line Tools.
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_failed;
extern const char *g_current_suite;

#define TEST_SUITE(name)                                                      \
    do {                                                                      \
        g_current_suite = (name);                                             \
        printf("\n\033[1m%s\033[0m\n", (name));                               \
    } while (0)

#define CHECK(cond, fmt, ...)                                                 \
    do {                                                                      \
        g_tests_run++;                                                        \
        if (!(cond)) {                                                        \
            g_tests_failed++;                                                 \
            printf("  \033[31mFAIL\033[0m %s:%d: " fmt "\n",                  \
                   __FILE__, __LINE__, ##__VA_ARGS__);                        \
        } else {                                                              \
            printf("  \033[32mok\033[0m   " fmt "\n", ##__VA_ARGS__);         \
        }                                                                     \
    } while (0)

#define CHECK_EQ_U(actual, expected, what)                                    \
    do {                                                                      \
        uint64_t _a = (uint64_t)(actual), _e = (uint64_t)(expected);          \
        g_tests_run++;                                                        \
        if (_a != _e) {                                                       \
            g_tests_failed++;                                                 \
            printf("  \033[31mFAIL\033[0m %s:%d: %s = 0x%" PRIx64             \
                   ", expected 0x%" PRIx64 "\n",                              \
                   __FILE__, __LINE__, (what), _a, _e);                       \
        } else {                                                              \
            printf("  \033[32mok\033[0m   %s = 0x%" PRIx64 "\n", (what), _a); \
        }                                                                     \
    } while (0)

#define CHECK_STR(actual, expected, what)                                     \
    do {                                                                      \
        g_tests_run++;                                                        \
        if (strcmp((actual), (expected)) != 0) {                              \
            g_tests_failed++;                                                 \
            printf("  \033[31mFAIL\033[0m %s:%d: %s = \"%s\", expected \"%s\"\n", \
                   __FILE__, __LINE__, (what), (actual), (expected));         \
        } else {                                                              \
            printf("  \033[32mok\033[0m   %s = \"%s\"\n", (what), (actual));  \
        }                                                                     \
    } while (0)

void test_crc(void);
void test_selfid(void);
void test_configrom(void);
void test_packet(void);
void test_transaction(void);
void test_sbp2(void);

#endif /* TEST_UTIL_H */
