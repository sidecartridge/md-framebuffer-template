/* The whole test framework: CHECK a condition, CHECK_EQ two integers, and
 * end main() with TEST_END(), which prints the tally and returns non-zero
 * when a check failed. */
#ifndef HOST_TEST_H
#define HOST_TEST_H

#include <stdio.h>

static int test_checks;
static int test_failures;

#define CHECK(cond)                                                     \
  do {                                                                  \
    test_checks++;                                                      \
    if (!(cond)) {                                                      \
      test_failures++;                                                  \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
              #cond);                                                   \
    }                                                                   \
  } while (0)

#define CHECK_EQ(actual, expected)                                      \
  do {                                                                  \
    long long test_a_ = (long long)(actual);                            \
    long long test_e_ = (long long)(expected);                          \
    test_checks++;                                                      \
    if (test_a_ != test_e_) {                                           \
      test_failures++;                                                  \
      fprintf(stderr, "%s:%d: %s is %lld, expected %lld\n", __FILE__,   \
              __LINE__, #actual, test_a_, test_e_);                     \
    }                                                                   \
  } while (0)

#define TEST_END()                                                      \
  do {                                                                  \
    printf("%s: %d checks, %d failed\n", __FILE__, test_checks,         \
           test_failures);                                              \
    return test_failures ? 1 : 0;                                       \
  } while (0)

#endif
