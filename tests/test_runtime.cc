// Runtime tests. These cover prelude helpers that the translated
// compiler depends on directly, so regressions show up in `make check`
// before we have to debug a broken bootstrap binary.

#include <cstdint>
#include <limits>

#include "../p2cc_rt/prelude.h"
#include "test_util.h"

using namespace rt;
using namespace p2cc_test;

namespace {

void test_val_accepts_prefixed_integers() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(ShortString<>("$7fffffff"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 2147483647);

  p_val(ShortString<>("$80000000"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());

  p_val(ShortString<>("$D7B0"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 55216);

  p_val(ShortString<>("%1010"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 10);

  p_val(ShortString<>("&77"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 63);
}

void test_val_wraps_decimal_longint() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(ShortString<>("2147483648"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());
}

void test_exec_tracks_exit_status() {
  p_doserror = -1;
  p_exec(ShortString<>("/bin/sh"), ShortString<>("-c 'exit 9'"));
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(p_dosexitcode(), 9);
}

void test_exec_reports_spawn_failure() {
  p_doserror = 0;
  p_exec(ShortString<>("/definitely/not/a/real/command"), ShortString<>(""));
  CHECK(p_doserror != 0);
  CHECK_EQ(p_dosexitcode(), 0);
}

void test_shell_tracks_exit_status() {
  p_doserror = -1;
  int32_t rc = p_shell(ShortString<>("exit 7"));
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(rc, 7);
  CHECK_EQ(p_dosexitcode(), 7);
}

}  // namespace

int main() {
  RUN_TEST(test_val_accepts_prefixed_integers);
  RUN_TEST(test_val_wraps_decimal_longint);
  RUN_TEST(test_exec_tracks_exit_status);
  RUN_TEST(test_exec_reports_spawn_failure);
  RUN_TEST(test_shell_tracks_exit_status);

  int n = p2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
