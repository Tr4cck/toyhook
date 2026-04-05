#ifndef TOYHOOK_TEST_FRAMEWORK_H
#define TOYHOOK_TEST_FRAMEWORK_H

#include <stdint.h>

static int __tf_total, __tf_pass, __tf_fail;

#define TEST(name) static int test_##name(void)

#define ASSERT_INT(a, b) do {                                       \
    long _a = (long)(a), _b = (long)(b);                            \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    FAIL line %d: expected %ld, got %ld\n",\
                __LINE__, (long)(b), (long)(a));                     \
        return 1;                                                   \
    }                                                               \
} while (0)

#define ASSERT_PTR(a, b) do {                                       \
    const void *_a = (a), *_b = (b);                                \
    if (_a != _b) {                                                 \
        fprintf(stderr, "    FAIL line %d: expected %p, got %p\n",  \
                __LINE__, (void*)(b), (void*)(a));                   \
        return 1;                                                   \
    }                                                               \
} while (0)

#define ASSERT_HEX(a, b) do {                                        \
    uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);                 \
    if (_a != _b) {                                                  \
        fprintf(stderr, "    FAIL line %d: expected 0x%lx, got 0x%lx\n",\
                __LINE__, (unsigned long)(b), (unsigned long)(a));    \
        return 1;                                                    \
    }                                                                \
} while (0)

#define ASSERT_TRUE(cond) do {                                      \
    if (!(cond)) {                                                  \
        fprintf(stderr, "    FAIL line %d: %s\n", __LINE__, #cond); \
        return 1;                                                   \
    }                                                               \
} while (0)

#define RUN_TEST(name) do {                                          \
    printf("  %-55s", #name);                                       \
    __tf_total++;                                                    \
    if (test_##name() == 0) { printf("PASS\n"); __tf_pass++; }      \
    else { printf("FAIL\n"); __tf_fail++; }                          \
} while (0)

#endif
