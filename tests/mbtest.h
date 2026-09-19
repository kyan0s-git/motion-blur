/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A test harness small enough not to need a dependency. */
#ifndef MBTEST_H
#define MBTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mbtest_failures;
static const char *mbtest_current;

#define MBT_CASE(name)                          \
    do {                                        \
        mbtest_current = name;                  \
        printf("  %-46s", name);                \
        fflush(stdout);                         \
    } while (0)

#define MBT_DONE()                                       \
    do {                                                 \
        printf("%s\n", mbtest_failed ? "FAIL" : "ok");   \
        mbtest_failed = 0;                               \
    } while (0)

static int mbtest_failed;

#define MBT_CHECK(cond, ...)                                    \
    do {                                                        \
        if (!(cond)) {                                          \
            if (!mbtest_failed)                                 \
                printf("FAIL\n");                               \
            mbtest_failed = 1;                                  \
            mbtest_failures++;                                  \
            printf("    %s:%d: ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define MBT_CLOSE(a, b, eps) \
    MBT_CHECK(fabs((double)(a) - (double)(b)) <= (eps), \
              #a " = %.9f, expected %.9f", (double)(a), (double)(b))

static int mbtest_report(const char *suite)
{
    if (mbtest_failures) {
        printf("%s: %d failure(s)\n", suite, mbtest_failures);
        return 1;
    }
    printf("%s: all passed\n", suite);
    return 0;
}

/* Deterministic PRNG so a failure is reproducible from the seed alone. */
static uint32_t mbtest_rng_state = 0x12345678u;
static inline uint32_t mbtest_rand(void)
{
    uint32_t x = mbtest_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mbtest_rng_state = x;
    return x;
}
static inline void mbtest_fill_random(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(mbtest_rand() >> 13);
}

#endif /* MBTEST_H */
