#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

// Tiny test framework. Each test binary declares `int run_tests()` which is
// invoked by main(). Use CHECK / CHECK_EQ; failures print and increment a
// counter. `run_tests` returns the failure count.

namespace p2cc_test {

inline int& failures() { static int n = 0; return n; }
inline const char*& current_test() { static const char* name = ""; return name; }

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s: %s:%d  CHECK(%s)\n",                  \
                   ::p2cc_test::current_test(), __FILE__, __LINE__, #cond);  \
      ++::p2cc_test::failures();                                             \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    auto _va = (a);                                                          \
    auto _vb = (b);                                                          \
    if (!(_va == _vb)) {                                                     \
      std::fprintf(stderr, "  FAIL %s: %s:%d  CHECK_EQ(%s, %s)\n",           \
                   ::p2cc_test::current_test(), __FILE__, __LINE__, #a, #b); \
      ++::p2cc_test::failures();                                             \
    }                                                                        \
  } while (0)

#define RUN_TEST(fn)                                                         \
  do {                                                                       \
    ::p2cc_test::current_test() = #fn;                                       \
    int _before = ::p2cc_test::failures();                                   \
    fn();                                                                    \
    int _after = ::p2cc_test::failures();                                    \
    std::printf("  %s  %s\n", (_after == _before ? "ok  " : "FAIL"), #fn);   \
  } while (0)

}  // namespace p2cc_test
