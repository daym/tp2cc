// Runtime tests. These cover prelude helpers that the translated
// compiler depends on directly, so regressions show up in `make check`
// before we have to debug a broken bootstrap binary.

#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "../tp2cc_rt/prelude.h"
#include "test_util.h"

using namespace rt;
using namespace tp2cc_test;

namespace {

struct MethodPtrCounter {
  int value = 0;
};

struct DestroyProbe : p_tobject {
  int* destroys = nullptr;

  explicit DestroyProbe(int* count) : destroys(count) {}

  void p_destroy() override {
    ++(*destroys);
  }
};

struct FreeInstanceProbe : p_tobject {
  int* destroys = nullptr;
  int* frees = nullptr;

  FreeInstanceProbe(int* d, int* f) : destroys(d), frees(f) {}

  void p_destroy() override { ++(*destroys); }

  void p_freeinstance() override {
    ++(*frees);
    delete this;
  }
};

inline void method_ptr_add(void* self, int32_t delta) {
  static_cast<MethodPtrCounter*>(self)->value += delta;
}

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

void test_bootstrap_pointer_sized_aliases_are_32bit() {
  static_assert(std::is_same_v<p_sizeint, int32_t>);
  static_assert(std::is_same_v<p_sizeuint, uint32_t>);
  static_assert(std::is_same_v<p_ptrint, int32_t>);
  static_assert(std::is_same_v<p_ptruint, uint32_t>);

  CHECK_EQ(p_maxint, std::numeric_limits<int32_t>::max());
}

void test_ansistring_copy_on_write_preserves_original() {
  AnsiString original("abc");
  AnsiString copy = original;

  copy[1] = p_char_of('z');

  CHECK_EQ(p_to_std_string(original), std::string("abc"));
  CHECK_EQ(p_to_std_string(copy), std::string("zbc"));
}

void test_ansistring_storage_slot_holds_payload_pointer() {
  AnsiString s("hello");

  auto& slot = p_reinterpret_storage_ref<void*>(s);

  CHECK(slot == static_cast<void*>(static_cast<p_char*>(s)));
  CHECK_EQ(p_deref(slot), 'h');
}

void test_ansistring_setlength_and_insert_delete_keep_bytes_stable() {
  AnsiString s("ab");

  p_setlength(s, 4);
  auto& slot = p_reinterpret_storage_ref<void*>(s);
  static_cast<p_char*>(slot)[2] = p_char_of('c');
  static_cast<p_char*>(slot)[3] = p_char_of('d');

  CHECK_EQ(p_length(s), 4);
  CHECK_EQ(p_to_std_string(s), std::string("abcd"));

  p_delete(s, 2, 2);
  CHECK_EQ(p_to_std_string(s), std::string("ad"));

  p_insert(ShortString<>("bc"), s, 2);
  CHECK_EQ(p_to_std_string(s), std::string("abcd"));
}

void test_ansistring_converts_to_shortstring_with_pascal_truncation() {
  AnsiString s("abcdef");
  auto shorty = static_cast<ShortString<4>>(s);

  CHECK_EQ(p_to_std_string(shorty), std::string("abcd"));
}

void test_shortstring_char_concat_grows_capacity() {
  auto label = ShortString<2>(".L") + p_char_of('e') + p_char_of('0');
  CHECK_EQ(p_to_std_string(label), std::string(".Le0"));
}

void test_shortstring_charref_inc_and_dec_update_length_slot_storage() {
  ShortString<> s("A");

  p_inc(s[1]);
  CHECK_EQ(p_to_std_string(s), std::string("B"));

  p_dec(s[1], 1);
  CHECK_EQ(p_to_std_string(s), std::string("A"));
}

void test_octstr_formats_octal_with_zero_padding() {
  CHECK_EQ(p_to_std_string(p_octstr(9, 4)), std::string("0011"));
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

void test_reinterpret_storage_ref_views_pointer_variable_bytes() {
  int first = 11;
  int second = 22;
  void* p = &first;

  auto& alias = p_reinterpret_storage_ref<void*>(p);
  CHECK_EQ(alias, static_cast<void*>(&first));

  alias = &second;
  CHECK_EQ(p, static_cast<void*>(&second));
}

void test_reinterpret_ref_views_pointee_bytes_of_pointer_value() {
  struct Box {
    int value;
  };

  Box box{17};
  void* p = &box;

  auto& alias = p_reinterpret_ref<Box>(p);
  CHECK_EQ(alias.value, 17);

  alias.value = 29;
  CHECK_EQ(box.value, 29);
}

void test_method_ptr_calls_bound_thunk() {
  MethodPtrCounter counter;
  MethodPtr<void(int32_t)> cb(p_method_code<&method_ptr_add>(), &counter);

  CHECK(p_assigned(cb));
  cb(7);
  CHECK_EQ(counter.value, 7);
}

void test_method_ptr_storage_matches_two_pointer_slots() {
  struct Slots {
    void* procpointer;
    void* self;
  };

  MethodPtrCounter counter;
  MethodPtr<void(int32_t)> cb{};
  auto& slots = p_reinterpret_storage_ref<Slots>(cb);

  slots.procpointer = p_method_code<&method_ptr_add>();
  slots.self = &counter;

  CHECK(cb != nullptr);
  cb(9);
  CHECK_EQ(counter.value, 9);
}

void test_class_free_accepts_null_pointer() {
  DestroyProbe* p = nullptr;

  p_tobject::p_free(p);
  CHECK(p == nullptr);
}

void test_class_free_dispatches_virtual_destroy() {
  int destroys = 0;
  auto* p = new DestroyProbe(&destroys);

  p_tobject::p_free(p);
  CHECK_EQ(destroys, 1);
}

void test_class_free_dispatches_virtual_freeinstance() {
  int destroys = 0;
  int frees = 0;
  auto* p = new FreeInstanceProbe(&destroys, &frees);

  p_tobject::p_free(p);
  CHECK_EQ(destroys, 1);
  CHECK_EQ(frees, 1);
}

void test_hi_lo_split_ordinal_halves() {
  CHECK_EQ(p_lo(uint32_t{0x11223344}), static_cast<uint16_t>(0x3344));
  CHECK_EQ(p_hi(uint32_t{0x11223344}), static_cast<uint16_t>(0x1122));
  CHECK_EQ(p_lo(uint64_t{0x1122334455667788ull}), uint32_t{0x55667788u});
  CHECK_EQ(p_hi(uint64_t{0x1122334455667788ull}), uint32_t{0x11223344u});
}

void test_fillword_and_compareword_operate_on_word_counts() {
  uint16_t words[4] = {0, 0, 0, 0};
  uint16_t same[4] = {0x1234, 0x1234, 0x1234, 0x1234};
  uint16_t different[4] = {0x1234, 0x1234, 0x1235, 0x1234};

  p_fillword(words[0], 4, 0x1234);
  CHECK_EQ(std::memcmp(words, same, sizeof(words)), 0);
  CHECK_EQ(p_compareword(words[0], same[0], 4), 0);
  CHECK(p_compareword(words[0], different[0], 4) < 0);
}

void test_indexword_searches_prefix_only() {
  Array<uint16_t, 0, 5> words{};
  words[0] = 0x10;
  words[1] = 0x20;
  words[2] = 0x30;
  words[3] = 0x20;
  words[4] = 0x50;

  CHECK_EQ(p_indexword(words, 3, static_cast<uint16_t>(0x20)), 1);
  CHECK_EQ(p_indexword(words, 3, static_cast<uint16_t>(0x50)), -1);
}

void test_comparebyte_operates_on_byte_counts() {
  uint8_t a[4] = {1, 2, 3, 4};
  uint8_t b[4] = {1, 2, 3, 4};
  uint8_t c[4] = {1, 2, 4, 4};

  CHECK_EQ(p_comparebyte(a[0], b[0], 4), 0);
  CHECK(p_comparebyte(a[0], c[0], 4) < 0);
}

void test_blockread_writes_to_void_buffer() {
  TypedFile<uint8_t> f;
  uint8_t got[5] = {};
  int32_t transferred = -1;

  f.f = std::tmpfile();
  CHECK(f.f != nullptr);
  const char* text = "hello";
  std::fwrite(text, 1, 5, f.f);
  std::rewind(f.f);

  p_blockread(f, static_cast<void*>(got), 5, transferred);
  CHECK_EQ(transferred, 5);
  CHECK_EQ(std::memcmp(got, text, 5), 0);

  std::fclose(f.f);
  f.f = nullptr;
}

void test_strnew_allocates_and_disposes_pchar() {
  p_char* text = p_strnew("hello");
  CHECK(text != nullptr);
  CHECK_EQ(p_to_std_string(text), std::string("hello"));
  p_strdispose(text);
  CHECK(text == nullptr);
}

void test_textfile_reset_closes_previous_handle() {
  char path[] = "/tmp/tp2cc-reset-XXXXXX";
  int fd = ::mkstemp(path);
  CHECK(fd >= 0);
  ::close(fd);

  TextFile f;
  p_assign(f, ShortString<>(path));
  p_rewrite(f);
  CHECK(f.f != nullptr);
  p_write(f, ShortString<>("hello"));

  // The compiler reopens the same file variable in AsmClose before invoking
  // the assembler. That reopen must close+flush the write handle first.
  p_reset(f, 1);
  CHECK(f.f != nullptr);

  char buf[6] = {};
  std::size_t got = std::fread(buf, 1, 5, f.f);
  CHECK_EQ(got, static_cast<std::size_t>(5));
  CHECK_EQ(std::string(buf, 5), std::string("hello"));

  p_close(f);
  std::remove(path);
}

void test_exec_tracks_exit_status() {
  p_doserror = -1;
  p_exec(ShortString<>("sh"), ShortString<>("-c 'exit 9'"));
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
  RUN_TEST(test_bootstrap_pointer_sized_aliases_are_32bit);
  RUN_TEST(test_ansistring_copy_on_write_preserves_original);
  RUN_TEST(test_ansistring_storage_slot_holds_payload_pointer);
  RUN_TEST(test_ansistring_setlength_and_insert_delete_keep_bytes_stable);
  RUN_TEST(test_ansistring_converts_to_shortstring_with_pascal_truncation);
  RUN_TEST(test_shortstring_char_concat_grows_capacity);
  RUN_TEST(test_shortstring_charref_inc_and_dec_update_length_slot_storage);
  RUN_TEST(test_octstr_formats_octal_with_zero_padding);
  RUN_TEST(test_move_reads_from_const_shortstring_storage);
  RUN_TEST(test_str_formats_real_values);
  RUN_TEST(test_reinterpret_bytes_copies_raw_object_bytes);
  RUN_TEST(test_reinterpret_storage_ref_views_pointer_variable_bytes);
  RUN_TEST(test_reinterpret_ref_views_pointee_bytes_of_pointer_value);
  RUN_TEST(test_method_ptr_calls_bound_thunk);
  RUN_TEST(test_method_ptr_storage_matches_two_pointer_slots);
  RUN_TEST(test_class_free_accepts_null_pointer);
  RUN_TEST(test_class_free_dispatches_virtual_destroy);
  RUN_TEST(test_class_free_dispatches_virtual_freeinstance);
  RUN_TEST(test_hi_lo_split_ordinal_halves);
  RUN_TEST(test_fillword_and_compareword_operate_on_word_counts);
  RUN_TEST(test_indexword_searches_prefix_only);
  RUN_TEST(test_comparebyte_operates_on_byte_counts);
  RUN_TEST(test_blockread_writes_to_void_buffer);
  RUN_TEST(test_strnew_allocates_and_disposes_pchar);
  RUN_TEST(test_textfile_reset_closes_previous_handle);
  RUN_TEST(test_exec_tracks_exit_status);
  RUN_TEST(test_exec_reports_spawn_failure);
  RUN_TEST(test_shell_tracks_exit_status);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
