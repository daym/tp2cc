// Runtime tests. These cover prelude helpers that the translated
// compiler depends on directly, so regressions show up in `make check`
// before we have to debug a broken bootstrap binary.

#include <cstdint>
#include <cstring>
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
}

void test_val_accepts_decimal_min_longint() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(ShortString<>("-2147483648"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());
}

void test_val_rejects_compiler_unsupported_integer_forms() {
  int32_t v = 123;
  int32_t code = -1;

  p_val(ShortString<>("&77"), v, code);
  CHECK_EQ(code, 1);
  CHECK_EQ(v, 0);

  p_val(ShortString<>("2147483648"), v, code);
  CHECK_EQ(code, 10);
  CHECK_EQ(v, 0);
}

void test_shortstring_char_concat_grows_capacity() {
  auto label = ShortString<2>(".L") + p_char_of('e') + p_char_of('0');
  CHECK_EQ(p_to_std_string(label), std::string(".Le0"));
}

void test_move_reads_from_const_shortstring_storage() {
  const ShortString<> text("hello");
  Array<p_char, 0, 8> buf;

  p_move(text[1], buf[0], p_length(text));
  buf[p_length(text)] = p_char_of('\0');

  CHECK_EQ(p_to_std_string(static_cast<p_char*>(buf)), std::string("hello"));
}

void test_str_formats_real_values() {
  ShortString<> s;

  p_str(100.0, s);
  CHECK_EQ(p_to_std_string(s), std::string(" 100"));

  p_str(0.01, s);
  CHECK_EQ(p_to_std_string(s), std::string(" 0.01"));
}

void test_reinterpret_bytes_copies_raw_object_bytes() {
  long double v = 10.0L;
  auto bytes = p_reinterpret_bytes<Array<uint8_t, 0, 10>>(v);
  uint8_t raw[sizeof(v)] = {};

  std::memcpy(raw, &v, sizeof(v));
  for (int i = 0; i < 10; ++i) {
    CHECK_EQ(bytes.data[i], raw[i]);
  }
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
  RUN_TEST(test_val_accepts_decimal_min_longint);
  RUN_TEST(test_val_rejects_compiler_unsupported_integer_forms);
  RUN_TEST(test_shortstring_char_concat_grows_capacity);
  RUN_TEST(test_move_reads_from_const_shortstring_storage);
  RUN_TEST(test_str_formats_real_values);
  RUN_TEST(test_reinterpret_bytes_copies_raw_object_bytes);
  RUN_TEST(test_exec_tracks_exit_status);
  RUN_TEST(test_exec_reports_spawn_failure);
  RUN_TEST(test_shell_tracks_exit_status);

  int n = p2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
