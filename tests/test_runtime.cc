// Runtime tests. These cover prelude helpers that the translated
// compiler depends on directly, so regressions show up in `make check`
// before we have to debug a broken bootstrap binary.

#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <string>
#include <type_traits>
#include <unistd.h>

#include "tp2cc_rt/prelude.h"
#include "test_util.h"

using namespace rt;
using namespace tp2cc_test;

namespace {

struct MethodPtrCounter {
  int value = 0;
};

struct DestroyProbe : t_tobject {
  int* destroys = nullptr;

  explicit DestroyProbe(int* count) : destroys(count) {}

  void p_destroy() override {
    ++(*destroys);
  }
};

struct FreeInstanceProbe : t_tobject {
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

struct DisposeProbe {
  static inline int destroys = 0;
  int32_t value = 0;

  ~DisposeProbe() { ++destroys; }
};

void test_val_accepts_prefixed_integers() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(tp2cc_shortstring_of<>("$7fffffff"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 2147483647);

  p_val(tp2cc_shortstring_of<>("$80000000"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());

  p_val(tp2cc_shortstring_of<>("$D7B0"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 55216);

  p_val(tp2cc_shortstring_of<>("%1010"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 10);
}

void test_val_accepts_decimal_min_longint() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(tp2cc_shortstring_of<>("-2147483648"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());
}

void test_val_handles_bootstrap_integer_forms() {
  int32_t v = 123;
  int32_t code = -1;

  p_val(tp2cc_shortstring_of<>("&77"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 63);

  p_val(tp2cc_shortstring_of<>("2147483648"), v, code);
  CHECK_EQ(code, 10);
  CHECK_EQ(v, 0);
}

void test_val_keeps_leading_zero_decimals_decimal() {
  uint32_t v = 0;
  int32_t code = -1;

  p_val(tp2cc_shortstring_of<>("01012"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 1012u);
}

void test_pascal_shift_helpers_mask_count_and_shr_logically() {
  CHECK_EQ(p_shl<int32_t>(1, 33), 2);
  CHECK_EQ(p_shr<int32_t>(0xFFFF, 33), 32767);
  CHECK_EQ(p_shr<int32_t>(-1, 15), 131071);
  CHECK_EQ(p_shl<int64_t>(int64_t{1}, 65), int64_t{2});
  CHECK_EQ(p_shr<int64_t>(int64_t{0xFFFF}, 65), int64_t{32767});
  CHECK_EQ(p_shl<uint32_t>(static_cast<uint8_t>(255), 8), 65280u);
}

void test_signed_wrap_helpers_avoid_ub() {
  CHECK_EQ(tp2cc_wrap_negate(std::numeric_limits<int64_t>::min()),
           std::numeric_limits<int64_t>::min());
  CHECK_EQ(tp2cc_wrap_add<int32_t>(std::numeric_limits<int32_t>::max(),
                                    int32_t{1}),
           std::numeric_limits<int32_t>::min());
  CHECK_EQ(tp2cc_wrap_sub<int32_t>(std::numeric_limits<int32_t>::min(),
                                    int32_t{1}),
           std::numeric_limits<int32_t>::max());
  CHECK_EQ(tp2cc_wrap_mul<int32_t>(int32_t{1} << 30, int32_t{4}),
           int32_t{0});
  CHECK_EQ(tp2cc_add_checked<uint32_t>(
               std::numeric_limits<uint32_t>::max(), uint32_t{1}),
           uint32_t{0});
  CHECK_EQ((p_abs<int64_t, false>(std::numeric_limits<int64_t>::min())),
           std::numeric_limits<int64_t>::min());
  CHECK_EQ((p_sqr<int32_t, false>(int32_t{1} << 30)), int32_t{0});
  bool saw_abs_overflow = false;
  try {
    (void)p_abs<int64_t, true>(std::numeric_limits<int64_t>::min());
  } catch (t_tobject* e) {
    saw_abs_overflow = dynamic_cast<t_eintoverflow*>(e) != nullptr;
    delete e;
  }
  CHECK(saw_abs_overflow);
  bool saw_sqr_overflow = false;
  try {
    (void)p_sqr<int32_t, true>(int32_t{1} << 30);
  } catch (t_tobject* e) {
    saw_sqr_overflow = dynamic_cast<t_eintoverflow*>(e) != nullptr;
    delete e;
  }
  CHECK(saw_sqr_overflow);
  CHECK_EQ((tp2cc_ordinal_add<int32_t, int32_t, false, false>(
               std::numeric_limits<int32_t>::max(), 1,
               std::numeric_limits<int32_t>::min(),
               std::numeric_limits<int32_t>::max())),
           std::numeric_limits<int32_t>::min());
  CHECK_EQ((tp2cc_ordinal_sub<int32_t, int32_t, false, false>(
               std::numeric_limits<int32_t>::min(), 1,
               std::numeric_limits<int32_t>::min(),
               std::numeric_limits<int32_t>::max())),
           std::numeric_limits<int32_t>::max());
  bool saw_ordinal_range = false;
  try {
    (void)tp2cc_ordinal_add<uint8_t, int32_t, false, true>(
        uint8_t{255}, 1, 0, 255);
  } catch (t_tobject* e) {
    saw_ordinal_range = dynamic_cast<t_erangeerror*>(e) != nullptr;
    delete e;
  }
  CHECK(saw_ordinal_range);
  int32_t inc_v = std::numeric_limits<int32_t>::max();
  inc_v = tp2cc_ordinal_add<int32_t, int32_t, false, false>(
      inc_v, 1, std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max());
  CHECK_EQ(inc_v, std::numeric_limits<int32_t>::min());
  int32_t dec_v = std::numeric_limits<int32_t>::min();
  dec_v = tp2cc_ordinal_sub<int32_t, int32_t, false, false>(
      dec_v, 1, std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max());
  CHECK_EQ(dec_v, std::numeric_limits<int32_t>::max());
  CHECK_EQ(tp2cc_int_mod<int32_t>(std::numeric_limits<int32_t>::min(),
                                  int32_t{-1}),
           int32_t{0});
  bool saw_div_zero = false;
  try {
    (void)tp2cc_int_div<int32_t>(int32_t{1}, int32_t{0});
  } catch (t_tobject* e) {
    saw_div_zero = dynamic_cast<t_edivbyzero*>(e) != nullptr;
    delete e;
  }
  CHECK(saw_div_zero);
  bool saw_div_overflow = false;
  try {
    (void)tp2cc_int_div<int32_t, true>(
        std::numeric_limits<int32_t>::min(), int32_t{-1});
  } catch (t_tobject* e) {
    saw_div_overflow = dynamic_cast<t_eintoverflow*>(e) != nullptr;
    delete e;
  }
  CHECK(saw_div_overflow);
}

void test_pointer_arithmetic_helpers_use_pascal_address_semantics() {
  uint16_t values[3] = {};
  uint16_t* middle = &values[1];
  CHECK_EQ((tp2cc_pointer_add<uint16_t*, std::uintptr_t>(middle, 1)),
           &values[2]);
  CHECK_EQ((tp2cc_pointer_sub<uint16_t*, std::uintptr_t>(middle, 1)),
           &values[0]);
  CHECK_EQ((tp2cc_pointer_difference<std::intptr_t, uint16_t*>(
               &values[2], &values[0])),
           2);
  void* small_address = reinterpret_cast<void*>(std::uintptr_t{0x1b6});
  void* wrapped = tp2cc_pointer_sub<void*, std::intptr_t>(
      small_address, std::intptr_t{1000});
  CHECK_EQ(reinterpret_cast<std::uintptr_t>(wrapped),
           std::uintptr_t{0x1b6} - std::uintptr_t{1000});
}

void test_runtime_path_helpers_match_compiler_expectations() {
  const auto path = tp2cc_ansistring_of("/tmp/archive.tar.gz");

  CHECK_EQ(tp2cc_to_std_string(p_extractfilepath(path)), std::string("/tmp/"));
  CHECK_EQ(tp2cc_to_std_string(p_extractfiledir(path)), std::string("/tmp"));
  CHECK_EQ(tp2cc_to_std_string(p_extractfiledir(tp2cc_ansistring_of("/foo.txt"))),
           std::string("/"));
  CHECK_EQ(tp2cc_to_std_string(p_extractfiledir(tp2cc_ansistring_of("foo.txt"))),
           std::string(""));
  CHECK_EQ(tp2cc_to_std_string(p_extractfilename(path)), std::string("archive.tar.gz"));
  CHECK_EQ(tp2cc_to_std_string(p_extractfileext(path)), std::string(".gz"));
  CHECK_EQ(tp2cc_to_std_string(p_changefileext(path, tp2cc_ansistring_of(".o"))),
           std::string("/tmp/archive.tar.o"));
  CHECK_EQ(tp2cc_to_std_string(p_setdirseparators(
                tp2cc_ansistring_of("foo\\bar/baz"))),
           std::string("foo/bar/baz"));

  // IncludeTrailingPathDelimiter appends `/` only when missing. Empty
  // input round-trips to empty per Pascal's sysutils contract.
  CHECK_EQ(tp2cc_to_std_string(p_includetrailingpathdelimiter(
                tp2cc_ansistring_of("/tmp"))),
           std::string("/tmp/"));
  CHECK_EQ(tp2cc_to_std_string(p_includetrailingpathdelimiter(
                tp2cc_ansistring_of("/tmp/"))),
           std::string("/tmp/"));
  CHECK_EQ(tp2cc_to_std_string(p_includetrailingpathdelimiter(
                tp2cc_ansistring_of(""))),
           std::string(""));
}

void test_runtime_tdatetime_decodes_current_and_dos_times() {
  const t_tdatetime midnight =
      tp2cc_make_tdatetime(2024, 2, 3, 0, 0, 0, 0);
  const t_tdatetime stamped =
      tp2cc_make_tdatetime(2024, 2, 3, 4, 5, 6, 0);

  uint16_t year = 0, month = 0, day = 0;
  uint16_t hour = 0, minute = 0, second = 0, msec = 0;
  p_decodedate(midnight, year, month, day);
  CHECK_EQ(year, 2024);
  CHECK_EQ(month, 2);
  CHECK_EQ(day, 3);

  p_decodetime(stamped, hour, minute, second, msec);
  CHECK_EQ(hour, 4);
  CHECK_EQ(minute, 5);
  CHECK_EQ(second, 6);
  CHECK_EQ(msec, 0);

  t_datetime dos{};
  dos.p_year = 2024;
  dos.p_month = 2;
  dos.p_day = 3;
  dos.p_hour = 4;
  dos.p_min = 5;
  dos.p_sec = 6;
  int32_t filedate = 0;
  p_packtime(dos, filedate);
  const t_tdatetime converted = p_filedatetodatetime(filedate);
  p_decodedate(converted, year, month, day);
  p_decodetime(converted, hour, minute, second, msec);
  CHECK_EQ(year, 2024);
  CHECK_EQ(month, 2);
  CHECK_EQ(day, 3);
  CHECK_EQ(hour, 4);
  CHECK_EQ(minute, 5);
  CHECK_EQ(second, 6);
}

void test_runtime_file_helpers_expose_real_sysutils_surface() {
  char path[] = "/tmp/tp2cc-file-XXXXXX";
  int fd = ::mkstemp(path);
  CHECK(fd >= 0);
  std::FILE* f = ::fdopen(fd, "w+");
  CHECK(f != nullptr);
  std::fputs("hello", f);
  std::fflush(f);

  tp2cc_TextFile tf{};
  tf.f = f;
  tf.name = tp2cc_shortstring_of<>(path);

  CHECK(p_fileexists(tp2cc_ansistring_of(path)));
  CHECK_EQ(p_getfilehandle(tf), fd);
  const int32_t age = p_filegetdate(fd);
  CHECK(age != -1);
  CHECK_EQ(p_filesetdate(fd, age), 0);
  CHECK_EQ(p_fileage(tp2cc_ansistring_of(path)), age);

  int32_t getftime_age = 0;
  p_getftime(tf, getftime_age);
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(getftime_age, age);
  p_setftime(tf, getftime_age);
  CHECK_EQ(p_doserror, 0);

  t_searchrec rec{};
  CHECK_EQ(p_findfirst(tp2cc_shortstring_of<>(path), 0, rec), 0);
  CHECK_EQ(rec.p_time, age);
  p_findclose(rec);

  std::fclose(f);
  CHECK(p_deletefile(tp2cc_ansistring_of(path)));
  CHECK(!p_fileexists(tp2cc_ansistring_of(path)));
}

void test_runtime_swap_fill_and_compare_helpers() {
  CHECK_EQ(p_swapendian<uint16_t>(0x1234u), 0x3412u);
  CHECK_EQ(p_swapendian<uint32_t>(0x12345678u), 0x78563412u);
  CHECK_EQ(p_ntobe<uint32_t>(0x12345678u), p_beton<uint32_t>(0x12345678u));
  CHECK_EQ(p_ntole<uint32_t>(0x12345678u), p_leton<uint32_t>(0x12345678u));
  if constexpr (std::endian::native == std::endian::little) {
    CHECK_EQ(p_leton<uint32_t>(0x12345678u), 0x12345678u);
    CHECK_EQ(p_ntobe<uint32_t>(0x12345678u), 0x78563412u);
  } else {
    CHECK_EQ(p_beton<uint32_t>(0x12345678u), 0x12345678u);
    CHECK_EQ(p_ntole<uint32_t>(0x12345678u), 0x78563412u);
  }

  uint32_t words[4]{};
  p_filldword(words, 4, 0xaabbccddu);
  for (uint32_t word : words) CHECK_EQ(word, 0xaabbccddu);

  const auto s1 = tp2cc_shortstring_of<>("abc");
  const auto s2 = tp2cc_shortstring_of<>("abd");
  const p_char bc[] = {tp2cc_char_of('b'), tp2cc_char_of('c')};
  CHECK_EQ(p_comparechar(static_cast<const void*>(s1.data),
                         static_cast<const void*>(s2.data), 2), 0);
  CHECK(p_comparechar(static_cast<const void*>(s1.data + 1),
                      static_cast<const void*>(s2.data + 1), 2) < 0);
  CHECK_EQ(p_comparechar(static_cast<const void*>(s1.data + 1),
                         static_cast<const void*>(bc), 2), 0);
  auto a1 = tp2cc_ansistring_of("abc");
  CHECK_EQ(p_comparechar(static_cast<const void*>(a1.bytes() + 1),
                         static_cast<const void*>(bc), 2), 0);
  CHECK_EQ(p_indexbyte("abc\0", 4, static_cast<uint8_t>('c')), 2);
  CHECK_EQ(p_indexbyte(static_cast<const void*>(s1.data), 3, static_cast<uint8_t>('c')), 2);
  CHECK_EQ(tp2cc_to_std_string(p_stringofchar(tp2cc_char_of('x'), 3)), std::string("xxx"));
  CHECK_EQ(p_comparetext(tp2cc_ansistring_of("Alpha"), tp2cc_ansistring_of("alpha")), 0);
  CHECK(p_comparetext(tp2cc_ansistring_of("alpha"), tp2cc_ansistring_of("beta")) < 0);
  CHECK(p_comparetext(tp2cc_ansistring_of("beta"), tp2cc_ansistring_of("alpha")) > 0);
  CHECK(p_comparetext(tp2cc_ansistring_of("alpha"), tp2cc_ansistring_of("alphabet")) < 0);
  CHECK_EQ(p_ansicomparefilename("/tmp/a", "/tmp/a"), 0);
}

void test_runtime_rotate_helpers_mask_distance() {
  CHECK_EQ(p_rorbyte(static_cast<uint8_t>(0x81u)), static_cast<uint8_t>(0xc0u));
  CHECK_EQ(p_rolbyte(static_cast<uint8_t>(0x81u)), static_cast<uint8_t>(0x03u));
  CHECK_EQ(p_rorword(static_cast<uint16_t>(0x8001u), 4), static_cast<uint16_t>(0x1800u));
  CHECK_EQ(p_rolword(static_cast<uint16_t>(0x8001u), 4), static_cast<uint16_t>(0x0018u));
  CHECK_EQ(p_rordword(0x80000001u, 8), 0x01800000u);
  CHECK_EQ(p_roldword(0x80000001u, 8), 0x00000180u);
  CHECK_EQ(p_rorqword(0x8000000000000001ull, 16), 0x0001800000000000ull);
  CHECK_EQ(p_rolqword(0x8000000000000001ull, 16), 0x0000000000018000ull);
  CHECK_EQ(p_rordword(0x12345678u, 32), 0x12345678u);
  CHECK_EQ(p_roldword(0x12345678u, 32), 0x12345678u);
}

void test_runtime_bitscan_helpers_match_fpc_results() {
  CHECK_EQ(p_bsfbyte(static_cast<uint8_t>(0)), static_cast<uint8_t>(0xff));
  CHECK_EQ(p_bsrbyte(static_cast<uint8_t>(0)), static_cast<uint8_t>(0xff));
  CHECK_EQ(p_bsfword(static_cast<uint16_t>(0)), uint32_t{0xff});
  CHECK_EQ(p_bsrword(static_cast<uint16_t>(0)), uint32_t{0xff});
  CHECK_EQ(p_bsfdword(uint32_t{0}), uint32_t{0xff});
  CHECK_EQ(p_bsrdword(uint32_t{0}), uint32_t{0xff});
  CHECK_EQ(p_bsfqword(uint64_t{0}), uint32_t{0xff});
  CHECK_EQ(p_bsrqword(uint64_t{0}), uint32_t{0xff});

  CHECK_EQ(p_bsfbyte(static_cast<uint8_t>(0x28u)), static_cast<uint8_t>(3));
  CHECK_EQ(p_bsrbyte(static_cast<uint8_t>(0x25u)), static_cast<uint8_t>(5));
  CHECK_EQ(p_bsfword(static_cast<uint16_t>(0x0400u)), uint32_t{10});
  CHECK_EQ(p_bsrword(static_cast<uint16_t>(0x8400u)), uint32_t{15});
  CHECK_EQ(p_bsfdword(uint32_t{0x00100000u}), uint32_t{20});
  CHECK_EQ(p_bsrdword(uint32_t{0x80100000u}), uint32_t{31});
  CHECK_EQ(p_bsfqword(uint64_t{1} << 47), uint32_t{47});
  CHECK_EQ(p_bsrqword((uint64_t{1} << 63) | 7), uint32_t{63});
}

void test_runtime_sarlongint_matches_fpc_results() {
  CHECK_EQ(p_sarlongint(int32_t{-0x3fffffff}, 4), int32_t{-0x04000000});
  CHECK_EQ(p_sarlongint(int32_t{0x3fffffff}, 4), int32_t{0x03ffffff});
  CHECK_EQ(p_sarlongint(int32_t{-0x3ffffff0}, 4), int32_t{-0x03ffffff});
  CHECK_EQ(p_sarlongint(int32_t{0x3ffffff0}, 4), int32_t{0x03ffffff});
  CHECK_EQ(p_sarlongint(int32_t{-0x3fffffff}, 0), int32_t{-0x3fffffff});
  CHECK_EQ(p_sarlongint(int32_t{0x3fffffff}, 0), int32_t{0x3fffffff});
  CHECK_EQ(p_sarlongint(int32_t{-0x3fffffff}, 31), int32_t{-1});
  CHECK_EQ(p_sarlongint(int32_t{0x3fffffff}, 31), int32_t{0});
  CHECK_EQ(p_sarlongint(int32_t{-0x3fffffff}), int32_t{-0x20000000});
  CHECK_EQ(p_sarlongint(int32_t{0x3fffffff}), int32_t{0x1fffffff});
  CHECK_EQ(p_sarlongint(int32_t{0x3fffffff}, 36), int32_t{0x03ffffff});
}

void test_getmem_typed_pointer_keeps_requested_prefix_size() {
  using HugeSymIndex = tp2cc_Array<void*, 0, 536870911>;

  HugeSymIndex* p = nullptr;
  p_getmem(p, static_cast<int>(4 * sizeof(void*)));

  CHECK(p != nullptr);
  p_freemem(p);
  CHECK(p != nullptr);
}

void test_shortstring_pointer_deref_uses_live_prefix_storage() {
  tp2cc_ShortString<>* p = nullptr;
  const auto hello = tp2cc_shortstring_of<>("hello");
  const auto one = tp2cc_shortstring_of<>("A");

  p_getmem(p, p_length(hello) + 1);
  CHECK(p != nullptr);

  tp2cc_shortstring_assign(tp2cc_deref(p), hello);
  CHECK_EQ(p_length(tp2cc_deref(p)), 5);
  CHECK(tp2cc_deref(p) == hello);

  tp2cc_deref(p) = one;
  CHECK_EQ(p_length(tp2cc_deref(p)), 1);
  CHECK(static_cast<p_char>(tp2cc_deref(p)[1]) == tp2cc_char_of('A'));

  tp2cc_deref(p)[1] = tp2cc_char_of('Z');
  CHECK(static_cast<p_char>(tp2cc_deref(p)[1]) == tp2cc_char_of('Z'));

  p_freemem(p, p_length(tp2cc_deref(p)) + 1);
  CHECK(p != nullptr);
}

void test_shortstring_pointer_deref_interoperates_with_string_ops() {
  tp2cc_ShortString<>* lhs = nullptr;
  tp2cc_ShortString<>* rhs = nullptr;
  const auto foo = tp2cc_shortstring_of<>("foo");
  const auto bar = tp2cc_shortstring_of<>("bar");
  const auto zoo = tp2cc_shortstring_of<>("zoo");
  tp2cc_TextFile f{};

  p_getmem(lhs, p_length(foo) + 1);
  p_getmem(rhs, p_length(bar) + 1);
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);

  tp2cc_shortstring_assign(tp2cc_deref(lhs), foo);
  tp2cc_shortstring_assign(tp2cc_deref(rhs), bar);

  CHECK_EQ(tp2cc_to_std_string(foo + tp2cc_deref(rhs)), std::string("foobar"));
  CHECK_EQ(tp2cc_to_std_string(tp2cc_deref(lhs) + tp2cc_deref(rhs)),
           std::string("foobar"));
  CHECK(tp2cc_deref(lhs) < zoo);
  CHECK(zoo > tp2cc_deref(rhs));
  CHECK_EQ(tp2cc_to_std_string(p_copy(tp2cc_deref(lhs), 2, 2)), std::string("oo"));
  CHECK_EQ(p_pos(tp2cc_shortstring_of<>("ar"), tp2cc_deref(rhs)), 2);

  p_assign(f, tp2cc_deref(lhs));
  CHECK_EQ(tp2cc_to_std_string(f.name), std::string("foo"));

  p_freemem(lhs, p_length(tp2cc_deref(lhs)) + 1);
  p_freemem(rhs, p_length(tp2cc_deref(rhs)) + 1);
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);
}

void test_exception_mask_roundtrips() {
#if defined(__linux__)
  const auto original = p_getexceptionmask();
  const auto masked = t_tfpuexceptionmask::from_list(
      {p_exinvalidop, p_exdenormalized, p_exzerodivide,
       p_exoverflow, p_exunderflow, p_exprecision});

  CHECK_EQ(p_setexceptionmask(masked), original);
  CHECK_EQ(p_getexceptionmask(), masked);

  CHECK_EQ(p_setexceptionmask(original), masked);
  CHECK_EQ(p_getexceptionmask(), original);
#endif
}

void test_8087cw_compatibility_tracks_mask_bits() {
#if defined(__linux__)
  const uint16_t original = p_get8087cw();
  const uint16_t updated = static_cast<uint16_t>((original & 0xFFC0u) | 0x25u);

  p_set8087cw(updated);
  CHECK_EQ(static_cast<uint8_t>(p_get8087cw() & 0x3Fu), 0x25u);

  p_set8087cw(original);
  CHECK_EQ(static_cast<uint8_t>(p_get8087cw() & 0x3Fu),
           static_cast<uint8_t>(original & 0x3Fu));
#endif
}

void test_ansistring_copy_on_write_preserves_original() {
  tp2cc_AnsiString original = tp2cc_ansistring_of("abc");
  tp2cc_AnsiString copy = tp2cc_ansistring_of(original);

  copy[1] = tp2cc_char_of('z');

  CHECK_EQ(tp2cc_to_std_string(original), std::string("abc"));
  CHECK_EQ(tp2cc_to_std_string(copy), std::string("zbc"));
}

void test_ansistring_index_proxy_has_byte_cast_like_shortstring() {
  // Pascal `byte(s[i])` lowers as `(uint8_t)(p_s[p_i])` for both
  // ShortString and AnsiString. ShortStringCharRef's `explicit
  // operator uint8_t()` makes that work; AnsiStringCharRef has to
  // expose the same conversion or the same Pascal source builds
  // for ShortString-typed variables and breaks for AnsiString.
  // Pascal-style 1-based indexing on both proxies: s[1] is the first
  // character.
  tp2cc_AnsiString s = tp2cc_ansistring_of("Az");
  // Pre-fix this static_cast did not compile.
  uint8_t a = static_cast<uint8_t>(s[1]);
  uint8_t z = static_cast<uint8_t>(s[2]);
  CHECK_EQ(a, static_cast<uint8_t>('A'));
  CHECK_EQ(z, static_cast<uint8_t>('z'));
  // AnsiString indexes may use any Pascal integer carrier. Verify that the
  // runtime accepts signed and unsigned 64-bit carriers without narrowing them.
  int64_t first = 1;
  uint64_t second = 2;
  CHECK_EQ(static_cast<uint8_t>(s[first]), static_cast<uint8_t>('A'));
  CHECK_EQ(static_cast<uint8_t>(s[second]), static_cast<uint8_t>('z'));
  // Mirror the ShortString side so the symmetry is locked.
  tp2cc_ShortString<> ss = tp2cc_shortstring_of<>("Az");
  CHECK_EQ(static_cast<uint8_t>(ss[1]), static_cast<uint8_t>('A'));
  CHECK_EQ(static_cast<uint8_t>(ss[2]), static_cast<uint8_t>('z'));
}

void test_ansistring_storage_slot_holds_payload_pointer() {
  tp2cc_AnsiString s = tp2cc_ansistring_of("hello");

  void* slot = tp2cc_reinterpret_load<void*>(&s);

  CHECK(slot == static_cast<void*>(static_cast<p_char*>(s)));
  CHECK_EQ(tp2cc_deref(slot), 'h');
}

void test_new_and_dispose_share_malloc_storage_family() {
  // ISO Pascal / Turbo Pascal / FPC leave the pointer value undefined
  // after `Dispose(p)`. The runtime no longer nils the slot defensively;
  // this test only asserts the destructor ran.
  DisposeProbe::destroys = 0;

  DisposeProbe* p = nullptr;
  p_new(p);
  CHECK(p != nullptr);
  p->value = 7;

  p_dispose(p);
  CHECK_EQ(DisposeProbe::destroys, 1);
}

void test_dispose_releases_plain_storage_grown_with_reallocmem() {
  // Smoke test that `dispose` accepts storage previously grown with
  // `reallocmem`. The pointer's post-dispose value is undefined per
  // Pascal, so we don't assert anything about `p` after the call --
  // only that the call returns without aborting and the address-sanitizer
  // sees a clean free.
  struct MoveListLike {
    int32_t count;
    void* data[1];
  };

  MoveListLike* p = nullptr;
  p_getmem(p, static_cast<int>(sizeof(int32_t) + 4 * sizeof(void*)));
  p->count = 4;
  p_reallocmem(p, static_cast<int>(sizeof(int32_t) + 8 * sizeof(void*)));
  p->count = 8;

  p_dispose(p);
}

void test_reallocmem_returns_updated_pointer_slot() {
  void* raw = nullptr;
  void* raw_result = p_reallocmem(raw, 16);
  CHECK(raw != nullptr);
  CHECK(raw_result == raw);

  int32_t* typed = nullptr;
  int32_t* typed_result =
      p_reallocmem(typed, static_cast<int>(2 * sizeof(int32_t)));
  CHECK(typed != nullptr);
  CHECK(typed_result == typed);
  typed[0] = 11;
  typed[1] = 22;

  int32_t* grown =
      p_reallocmem(typed, static_cast<int>(4 * sizeof(int32_t)));
  CHECK(grown == typed);
  CHECK_EQ(typed[0], int32_t{11});
  CHECK_EQ(typed[1], int32_t{22});

  int32_t* cleared = p_reallocmem(typed, 0);
  CHECK(cleared == nullptr);
  CHECK(typed == nullptr);

  std::free(raw);
}

void test_pointer_slot_helpers_update_byte_storage() {
  alignas(void*) unsigned char slot[sizeof(int32_t*)] = {};

  p_new(tp2cc_storage_ref<int32_t*>(slot));
  int32_t* p = tp2cc_reinterpret_load<int32_t*>(slot);
  CHECK(p != nullptr);
  CHECK_EQ(*p, int32_t{0});
  *p = 123;
  p_dispose(p);

  p_getmem(tp2cc_storage_ref<int32_t*>(slot),
           static_cast<int>(2 * sizeof(int32_t)));
  p = tp2cc_reinterpret_load<int32_t*>(slot);
  CHECK(p != nullptr);
  p[0] = 11;
  p[1] = 22;

  int32_t* grown =
      p_reallocmem(tp2cc_storage_ref<int32_t*>(slot),
                   static_cast<int>(4 * sizeof(int32_t)));
  CHECK(grown == tp2cc_reinterpret_load<int32_t*>(slot));
  CHECK_EQ(grown[0], int32_t{11});
  CHECK_EQ(grown[1], int32_t{22});

  int32_t* cleared = p_reallocmem(tp2cc_storage_ref<int32_t*>(slot), 0);
  CHECK(cleared == nullptr);
  CHECK(tp2cc_reinterpret_load<int32_t*>(slot) == nullptr);
}

void test_strdispose_slot_clears_byte_storage() {
  alignas(void*) unsigned char slot[sizeof(p_char*)] = {};
  p_char* text = p_strnew("slot");
  tp2cc_reinterpret_store<p_char*>(slot, text);

  p_strdispose(tp2cc_storage_ref<p_char*>(slot));

  CHECK(tp2cc_reinterpret_load<p_char*>(slot) == nullptr);
}

void test_storage_ref_preserves_pascal_address_identity() {
  struct View {
    int32_t value;
  };
  alignas(View) unsigned char slot[sizeof(View)] = {};

  View& first = tp2cc_storage_ref<View>(slot);
  View& second = tp2cc_storage_ref<View>(slot);
  CHECK(&first == reinterpret_cast<View*>(slot));
  CHECK(&second == &first);
  first.value = 42;
  CHECK_EQ(second.value, int32_t{42});
}

void test_storage_ref_does_not_run_default_member_initializers() {
  struct View {
    int32_t first = 101;
    int32_t second = 202;
  };
  static_assert(std::is_trivially_copyable_v<View>);

  alignas(View) unsigned char slot[sizeof(View)];
  const int32_t words[2] = {7, 9};
  std::memcpy(slot, words, sizeof(words));
  View& view = tp2cc_storage_ref<View>(slot);
  CHECK_EQ(view.first, int32_t{7});
  CHECK_EQ(view.second, int32_t{9});
}

void test_scoped_storage_view_restores_backing_lifetime() {
  struct Backing {
    int32_t bits;
  };
  struct View {
    int32_t value;
  };
  static_assert(sizeof(Backing) == sizeof(View));
  static_assert(alignof(Backing) == alignof(View));

  Backing backing{7};
  {
    tp2cc_ScopedStorageView<View, Backing> view(&backing, &backing);
    CHECK(&*view == reinterpret_cast<View*>(&backing));
    view->value = 23;
  }
  CHECK_EQ(backing.bits, int32_t{23});
}

void test_ansistring_setlength_and_insert_delete_keep_bytes_stable() {
  tp2cc_AnsiString s = tp2cc_ansistring_of("ab");

  p_setlength(s, 4);
  void* slot = tp2cc_reinterpret_load<void*>(&s);
  static_cast<p_char*>(slot)[2] = tp2cc_char_of('c');
  static_cast<p_char*>(slot)[3] = tp2cc_char_of('d');

  CHECK_EQ(p_length(s), 4);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("abcd"));

  p_delete(s, 2, 2);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("ad"));

  p_insert(tp2cc_shortstring_of<>("bc"), s, 2);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("abcd"));
}

void test_ansistring_compares_equal_to_single_char_pascal_style() {
  // Pascal lifts a Char to a one-character string when comparing
  // against a string. The AnsiString-vs-Char overloads here implement
  // that lift directly: equal iff length 1 and bytes[0] matches.
  auto one = tp2cc_ansistring_of("c");
  auto two = tp2cc_ansistring_of("ab");
  auto empty = tp2cc_ansistring_of("");
  CHECK(one == tp2cc_char_of('c'));
  CHECK(tp2cc_char_of('c') == one);
  CHECK(!(one == tp2cc_char_of('d')));
  CHECK(!(two == tp2cc_char_of('a')));
  CHECK(!(empty == tp2cc_char_of(' ')));
  CHECK(one != tp2cc_char_of('d'));
  CHECK(tp2cc_char_of('d') != one);
  CHECK(!(one != tp2cc_char_of('c')));
}

void test_ansistring_converts_to_shortstring_with_pascal_truncation() {
  tp2cc_AnsiString s = tp2cc_ansistring_of("abcdef");
  auto shorty = static_cast<tp2cc_ShortString<4>>(s);

  CHECK_EQ(tp2cc_to_std_string(shorty), std::string("abcd"));
}

void test_shortstring_char_concat_grows_capacity() {
  auto label = tp2cc_shortstring_of<2>(".L") + tp2cc_char_of('e') + tp2cc_char_of('0');
  CHECK_EQ(tp2cc_to_std_string(label), std::string(".Le0"));
}

void test_shortstring_single_nul_char_keeps_length_one() {
  tp2cc_ShortString<> s = tp2cc_shortstring_of<>(tp2cc_char_of('\0'));

  CHECK_EQ(p_length(s), 1);
  CHECK_EQ(tp2cc_char_byte(s.data[0]), 0);
}

void test_shortstring_nul_char_concat_preserves_embedded_zero() {
  auto s = tp2cc_shortstring_of<>(tp2cc_char_of('\0')) + tp2cc_char_of('A');

  CHECK_EQ(p_length(s), 2);
  CHECK_EQ(tp2cc_char_byte(s.data[0]), 0);
  CHECK_EQ(tp2cc_char_byte(s.data[1]), static_cast<uint8_t>('A'));
}

void test_shortstring_literal_helper_preserves_embedded_nuls() {
  auto s = tp2cc_shortstring_literal<255>(tp2cc_char_of('\x8d'), tp2cc_char_of('\xb4'),
                                      tp2cc_char_of('&'), tp2cc_char_of('\0'),
                                      tp2cc_char_of('\0'), tp2cc_char_of('\0'),
                                      tp2cc_char_of('\0'));

  CHECK_EQ(p_length(s), 7);
  CHECK_EQ(tp2cc_char_byte(s.data[0]), 0x8d);
  CHECK_EQ(tp2cc_char_byte(s.data[1]), 0xb4);
  CHECK_EQ(tp2cc_char_byte(s.data[2]), static_cast<uint8_t>('&'));
  CHECK_EQ(tp2cc_char_byte(s.data[3]), 0);
  CHECK_EQ(tp2cc_char_byte(s.data[4]), 0);
  CHECK_EQ(tp2cc_char_byte(s.data[5]), 0);
  CHECK_EQ(tp2cc_char_byte(s.data[6]), 0);
}

void test_shortstring_implicitly_converts_between_capacities() {
  tp2cc_ShortString<4> small = tp2cc_shortstring_of<4>("abcdef");
  tp2cc_ShortString<> wide = small;
  tp2cc_ShortString<2> narrow = wide;

  CHECK_EQ(tp2cc_to_std_string(wide), std::string("abcd"));
  CHECK_EQ(tp2cc_to_std_string(narrow), std::string("ab"));
}

void test_shortstring_ref_mutating_helpers_write_through_storage() {
  tp2cc_ShortString<8> s = tp2cc_shortstring_of<8>("abc");
  auto ref = tp2cc_shortstring_ref<8>(s);

  p_setlength(ref, 5);
  CHECK_EQ(p_length(s), 5);
  CHECK_EQ(tp2cc_char_byte(s.data[3]), 0);
  CHECK_EQ(tp2cc_char_byte(s.data[4]), 0);

  s = tp2cc_shortstring_of<8>("abc");
  p_insert(tp2cc_shortstring_of<>("XY"), ref, 2);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("aXYbc"));

  p_delete(ref, 2, 2);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("abc"));

  p_str(int32_t{42}, ref);
  CHECK_EQ(tp2cc_to_std_string(s), std::string("42"));
}

void test_shortstring_pointer_value_reads_only_live_prefix() {
  uint8_t storage[] = {3, 'a', 'b', 'c'};
  tp2cc_ShortStringPtrValue<255> value{storage};
  tp2cc_ShortString<> copied = value;
  CHECK_EQ(copied.size(), 3);
  CHECK_EQ(static_cast<char>(copied.data[0]), 'a');
  CHECK_EQ(static_cast<char>(copied.data[1]), 'b');
  CHECK_EQ(static_cast<char>(copied.data[2]), 'c');
}

void test_shortstring_implicitly_converts_to_ansistring() {
  tp2cc_ShortString<> shorty = tp2cc_shortstring_of<>("abc");
  tp2cc_AnsiString text = shorty;

  CHECK_EQ(text.length(), 3);
  CHECK_EQ(tp2cc_char_byte(text.bytes()[0]), static_cast<uint8_t>('a'));
  CHECK_EQ(tp2cc_char_byte(text.bytes()[1]), static_cast<uint8_t>('b'));
  CHECK_EQ(tp2cc_char_byte(text.bytes()[2]), static_cast<uint8_t>('c'));
  CHECK_EQ(tp2cc_char_byte(text.bytes()[3]), 0);
}

void test_shortstring_assign_from_char_creates_one_character_string() {
  tp2cc_ShortString<> s{};
  s = tp2cc_char_of('.');

  CHECK_EQ(tp2cc_to_std_string(s), std::string("."));
}

void test_strpas_returns_shortstring_up_to_first_nul() {
  const p_char raw[] = {tp2cc_char_of('A'), tp2cc_char_of('B'), tp2cc_char_of('\0'),
                        tp2cc_char_of('C'), tp2cc_char_of('\0')};
  tp2cc_ShortString<> s = p_strpas(raw);

  CHECK_EQ(tp2cc_to_std_string(s), std::string("AB"));
}

void test_ansistring_from_shortstring_keeps_trailing_nul_storage() {
  tp2cc_AnsiString s = tp2cc_ansistring_of(
      tp2cc_shortstring_literal<255>(tp2cc_char_of('A'), tp2cc_char_of('\0'),
                                 tp2cc_char_of('B')));

  CHECK_EQ(s.length(), 3);
  CHECK_EQ(tp2cc_char_byte(s.bytes()[0]), static_cast<uint8_t>('A'));
  CHECK_EQ(tp2cc_char_byte(s.bytes()[1]), 0);
  CHECK_EQ(tp2cc_char_byte(s.bytes()[2]), static_cast<uint8_t>('B'));
  CHECK_EQ(tp2cc_char_byte(s.bytes()[3]), 0);
}

void test_shortstring_charref_inc_and_dec_update_length_slot_storage() {
  tp2cc_ShortString<> s = tp2cc_shortstring_of<>("A");

  s[1] = tp2cc_ordinal_add<p_char, uint8_t, false, false>(
      s[1], 1, 0, std::numeric_limits<uint8_t>::max());
  CHECK_EQ(tp2cc_to_std_string(s), std::string("B"));

  s[1] = tp2cc_ordinal_sub<p_char, uint8_t, false, false>(
      s[1], 1, 0, std::numeric_limits<uint8_t>::max());
  CHECK_EQ(tp2cc_to_std_string(s), std::string("A"));
}

void test_shortstring_index_address_names_layout_bytes() {
  tp2cc_ShortString<8> s = tp2cc_shortstring_of<8>("AB");

  auto* length_byte =
      static_cast<uint8_t*>(tp2cc_shortstring_index_address<8>(&s, 0));
  auto* first_char =
      static_cast<p_char*>(tp2cc_shortstring_index_address<8>(&s, 1));
  auto* second_char =
      static_cast<p_char*>(tp2cc_shortstring_index_address<8>(&s, 2));

  CHECK_EQ(*length_byte, 2);
  CHECK_EQ(*first_char, tp2cc_char_of('A'));
  CHECK_EQ(*second_char, tp2cc_char_of('B'));
}

void test_octstr_formats_octal_with_zero_padding() {
  CHECK_EQ(tp2cc_to_std_string(p_octstr(9, 4)), std::string("0011"));
}

void test_move_reads_from_const_shortstring_storage() {
  const tp2cc_ShortString<> text = tp2cc_shortstring_of<>("hello");
  tp2cc_Array<p_char, 0, 8> buf;

  p_move(static_cast<const void*>(text.data), static_cast<void*>(buf.data),
         p_length(text));
  buf[p_length(text)] = tp2cc_char_of('\0');

  CHECK_EQ(tp2cc_to_std_string(static_cast<p_char*>(buf)), std::string("hello"));
}

void test_shortstring_compares_equal_to_pchar_buffer() {
  const p_char text[] = {
      tp2cc_char_of('h'), tp2cc_char_of('e'), tp2cc_char_of('l'),
      tp2cc_char_of('l'), tp2cc_char_of('o'), tp2cc_char_of('\0')};
  const tp2cc_ShortString<> shorty = tp2cc_shortstring_of<>("hello");

  CHECK(text == shorty);
  CHECK(shorty == text);
}

void test_insert_accepts_shortstring_pointer_proxy_source() {
  tp2cc_ShortString<> src = tp2cc_shortstring_of<>("ab");
  tp2cc_ShortString<> dest = tp2cc_shortstring_of<>("XY");
  tp2cc_ShortString<>* ptr = &src;

  p_insert(tp2cc_deref(ptr), dest, 2);

  CHECK_EQ(tp2cc_to_std_string(dest), std::string("XabY"));
}

void test_char_array_compares_equal_to_shortstring_by_live_prefix() {
  tp2cc_Array<p_char, 0, 8> text{};
  text.data[0] = tp2cc_char_of('h');
  text.data[1] = tp2cc_char_of('i');
  text.data[2] = tp2cc_char_of('\0');

  CHECK(text == tp2cc_shortstring_of<>("hi"));
  CHECK(tp2cc_shortstring_of<>("hi") == text);
}

void test_char_array_assignment_from_shortstring_matches_fpc() {
  tp2cc_Array<p_char, 0, 4> short_dest{};
  short_dest = tp2cc_shortstring_of<8>("ab");
  CHECK_EQ(tp2cc_char_to_c(short_dest.data[0]), 'a');
  CHECK_EQ(tp2cc_char_to_c(short_dest.data[1]), 'b');
  CHECK_EQ(tp2cc_char_to_c(short_dest.data[2]), '\0');
  CHECK_EQ(tp2cc_char_to_c(short_dest.data[3]), '\0');

  tp2cc_Array<p_char, 0, 4> exact_dest{};
  exact_dest = tp2cc_shortstring_of<8>("abcd");
  CHECK_EQ(tp2cc_char_to_c(exact_dest.data[0]), 'a');
  CHECK_EQ(tp2cc_char_to_c(exact_dest.data[1]), 'b');
  CHECK_EQ(tp2cc_char_to_c(exact_dest.data[2]), 'c');
  CHECK_EQ(tp2cc_char_to_c(exact_dest.data[3]), 'd');

  tp2cc_Array<p_char, 0, 4> long_dest{};
  long_dest = tp2cc_shortstring_of<8>("abcdef");
  CHECK_EQ(tp2cc_char_to_c(long_dest.data[0]), 'a');
  CHECK_EQ(tp2cc_char_to_c(long_dest.data[1]), 'b');
  CHECK_EQ(tp2cc_char_to_c(long_dest.data[2]), 'c');
  CHECK_EQ(tp2cc_char_to_c(long_dest.data[3]), 'd');
}

void test_shortstring_assignment_from_char_array_matches_fpc() {
  tp2cc_Array<p_char, 0, 5> zero_based{};
  zero_based.data[0] = tp2cc_char_of('a');
  zero_based.data[1] = tp2cc_char_of('b');
  zero_based.data[2] = tp2cc_char_of('\0');
  zero_based.data[3] = tp2cc_char_of('c');
  zero_based.data[4] = tp2cc_char_of('d');

  tp2cc_ShortString<5> stop_at_nul{};
  stop_at_nul = zero_based;
  CHECK_EQ(stop_at_nul.length, 2);
  CHECK_EQ(tp2cc_to_std_string(stop_at_nul), std::string("ab"));

  tp2cc_Array<p_char, 0, 5> no_nul{};
  no_nul.data[0] = tp2cc_char_of('a');
  no_nul.data[1] = tp2cc_char_of('b');
  no_nul.data[2] = tp2cc_char_of('c');
  no_nul.data[3] = tp2cc_char_of('d');
  no_nul.data[4] = tp2cc_char_of('e');

  tp2cc_ShortString<3> truncated{};
  truncated = no_nul;
  CHECK_EQ(truncated.length, 3);
  CHECK_EQ(tp2cc_to_std_string(truncated), std::string("abc"));

  tp2cc_Array<p_char, 1, 4> one_based{};
  one_based.data[0] = tp2cc_char_of('x');
  one_based.data[1] = tp2cc_char_of('\0');
  one_based.data[2] = tp2cc_char_of('y');
  one_based.data[3] = tp2cc_char_of('z');

  tp2cc_ShortString<4> keep_embedded_nul{};
  keep_embedded_nul = one_based;
  CHECK_EQ(keep_embedded_nul.length, 4);
  CHECK_EQ(tp2cc_char_to_c(keep_embedded_nul.data[0]), 'x');
  CHECK_EQ(tp2cc_char_to_c(keep_embedded_nul.data[1]), '\0');
  CHECK_EQ(tp2cc_char_to_c(keep_embedded_nul.data[2]), 'y');
  CHECK_EQ(tp2cc_char_to_c(keep_embedded_nul.data[3]), 'z');
}

void test_str_formats_real_values() {
  tp2cc_ShortString<> s;

  p_str(100.0, s);
  CHECK_EQ(tp2cc_to_std_string(s), std::string(" 100"));

  p_str(0.01, s);
  CHECK_EQ(tp2cc_to_std_string(s), std::string(" 0.01"));
}

void test_inttostr_formats_integer_values() {
  CHECK_EQ(tp2cc_to_std_string(p_inttostr(int32_t{-42})), std::string("-42"));
  CHECK_EQ(tp2cc_to_std_string(p_inttostr(uint32_t{42})), std::string("42"));
}

void test_setstring_copies_counted_bytes() {
  const p_char raw[] = {tp2cc_char_of('A'), tp2cc_char_of('\0'),
                        tp2cc_char_of('B'), tp2cc_char_of('C')};

  tp2cc_ShortString<3> fixed{};
  p_setstring(fixed, raw, 4);
  CHECK_EQ(static_cast<int>(fixed.length), 3);
  CHECK_EQ(tp2cc_char_to_c(fixed.data[0]), 'A');
  CHECK_EQ(tp2cc_char_to_c(fixed.data[1]), '\0');
  CHECK_EQ(tp2cc_char_to_c(fixed.data[2]), 'B');

  tp2cc_AnsiString dynamic{};
  p_setstring(dynamic, raw, 4);
  CHECK_EQ(dynamic.length(), 4);
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[0]), 'A');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[1]), '\0');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[2]), 'B');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[3]), 'C');

  p_setstring(dynamic, "xy\0z", 4);
  CHECK_EQ(dynamic.length(), 4);
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[0]), 'x');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[1]), 'y');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[2]), '\0');
  CHECK_EQ(tp2cc_char_to_c(dynamic.bytes()[3]), 'z');
}

void test_reinterpret_bytes_copies_raw_object_bytes() {
  long double v = 10.0L;
  auto bytes = tp2cc_reinterpret_bytes<tp2cc_Array<uint8_t, 0, sizeof(v)>>(v);
  uint8_t raw[sizeof(v)] = {};

  std::memcpy(raw, &v, sizeof(v));
  for (size_t i = 0; i < sizeof(v); ++i) {
    CHECK_EQ(bytes.data[i], raw[i]);
  }
}

void test_reinterpret_copy_preserves_scalar_bit_pattern() {
  tp2cc_Array<uint8_t, 0, 8> bits{};
  bits.data[0] = 0;
  bits.data[1] = 0;
  bits.data[2] = 0;
  bits.data[3] = 0;
  bits.data[4] = 0;
  bits.data[5] = 0;
  bits.data[6] = 240;
  bits.data[7] = 127;

  double value = tp2cc_reinterpret_copy<double>(bits);
  uint8_t raw[sizeof(value)] = {};
  std::memcpy(raw, &value, sizeof(value));

  for (int i = 0; i < 8; ++i) {
    CHECK_EQ(raw[i], bits.data[i]);
  }
}

void test_reinterpret_load_store_and_inc_handle_misaligned_bytes() {
  uint8_t raw[8] = {};
  void* p = raw + 1;
  int32_t value = 0x12345678;
  uint8_t expected[sizeof(value)] = {};

  std::memcpy(expected, &value, sizeof(value));
  tp2cc_reinterpret_store<int32_t>(p, value);

  for (size_t i = 0; i < sizeof(value); ++i) {
    CHECK_EQ(raw[1 + i], expected[i]);
  }
  CHECK_EQ(tp2cc_reinterpret_load<int32_t>(p), value);

  tp2cc_reinterpret_update<int32_t>(p, [](int32_t current) {
    return tp2cc_ordinal_add<int32_t, int32_t, false, false>(
        current, 1, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max());
  });
  CHECK_EQ(tp2cc_reinterpret_load<int32_t>(p), value + 1);
}

void test_unaligned_load_store_handle_misaligned_bytes() {
  uint8_t raw[8] = {};
  void* p = raw + 1;
  int32_t value = 0x12345678;
  uint8_t expected[sizeof(value)] = {};

  std::memcpy(expected, &value, sizeof(value));
  tp2cc_unaligned_store<int32_t>(p, value);

  for (size_t i = 0; i < sizeof(value); ++i) {
    CHECK_EQ(raw[1 + i], expected[i]);
  }
  CHECK_EQ(tp2cc_unaligned_load<int32_t>(p), value);

  tp2cc_unaligned_update<int32_t>(p, [](int32_t current) {
    return tp2cc_ordinal_add<int32_t, int32_t, false, false>(
        current, 1, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max());
  });
  CHECK_EQ(tp2cc_unaligned_load<int32_t>(p), value + 1);

  tp2cc_unaligned_update<int32_t>(p, [](int32_t current) {
    return tp2cc_ordinal_sub<int32_t, int32_t, false, false>(
        current, 2, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max());
  });
  CHECK_EQ(tp2cc_unaligned_load<int32_t>(p), value - 1);
}

void test_scope_exit_runs_on_exception_unwind() {
  int value = 0;
  try {
    auto guard = tp2cc_make_scope_exit([&]() { value = 7; });
    throw 1;
  } catch (int) {
  }
  CHECK_EQ(value, 7);
}

void test_open_array_helper_owns_temporary_storage() {
  auto holder = tp2cc_open_array_of<int32_t>(1, 2, 3);
  tp2cc_OpenArray<int32_t> view = holder;

  CHECK_EQ(view.count, 3);
  CHECK_EQ(view[0], 1);
  CHECK_EQ(view[1], 2);
  CHECK_EQ(view[2], 3);
}

void test_dynamic_array_setlength_detaches_and_zeroes_tail() {
  tp2cc_DynArray<int32_t> nil_values = nullptr;
  CHECK(nil_values == nullptr);

  tp2cc_DynArray<int32_t> values;
  p_setlength(values, 2);
  values[0] = 7;
  values[1] = 9;

  tp2cc_DynArray<int32_t> alias = values;
  p_setlength(values, 4);

  CHECK_EQ(p_length(values), 4);
  CHECK_EQ(values[0], 7);
  CHECK_EQ(values[1], 9);
  CHECK_EQ(values[2], 0);
  CHECK_EQ(values[3], 0);

  CHECK_EQ(p_length(alias), 2);
  CHECK_EQ(alias[0], 7);
  CHECK_EQ(alias[1], 9);

  values[0] = 11;
  CHECK_EQ(alias[0], 7);

  values = nullptr;
  CHECK(values == nullptr);
}

void test_dynamic_array_copy_makes_independent_storage() {
  tp2cc_DynArray<int32_t> values;
  p_setlength(values, 2);
  values[0] = 7;
  values[1] = 9;

  tp2cc_DynArray<int32_t> copy = p_copy(values);
  CHECK_EQ(p_length(copy), 2);
  CHECK_EQ(copy[0], 7);
  CHECK_EQ(copy[1], 9);
  CHECK(copy.ptr() != values.ptr());

  values[0] = 11;
  CHECK_EQ(copy[0], 7);
}

void test_dynamic_array_assignment_from_fixed_array_copies_values() {
  tp2cc_Array<uint16_t, 1, 4> fixed{{3, 5, 7, 11}};
  tp2cc_DynArray<uint16_t> dyn;

  CHECK_EQ(p_length(fixed), 4);
  dyn = fixed;

  CHECK_EQ(p_length(dyn), 4);
  CHECK_EQ(dyn[0], 3);
  CHECK_EQ(dyn[1], 5);
  CHECK_EQ(dyn[2], 7);
  CHECK_EQ(dyn[3], 11);

  fixed[1] = 99;
  CHECK_EQ(dyn[1], 5);
}

void test_open_array_view_uses_dynamic_array_storage() {
  tp2cc_DynArray<int32_t> values;
  p_setlength(values, 3);
  values[0] = 1;
  values[1] = 2;
  values[2] = 3;

  tp2cc_OpenArray<int32_t> view = tp2cc_open_array<int32_t>(values);
  CHECK_EQ(view.count, 3);
  CHECK(view.data == values.ptr());

  view[1] = 8;
  CHECK_EQ(values[1], 8);
}

void test_dos_pack_unpack_time_matches_bit_layout() {
  t_datetime in{};
  in.p_year = 2004;
  in.p_month = 5;
  in.p_day = 6;
  in.p_hour = 7;
  in.p_min = 8;
  in.p_sec = 10;

  int32_t packed = 0;
  p_packtime(in, packed);
  CHECK_EQ(packed,
           (((2004 - tp2cc_dos_filetime_year_base)
             << tp2cc_dos_filetime_year_shift) |
            (5 << tp2cc_dos_filetime_month_shift) |
            (6 << tp2cc_dos_filetime_day_shift) |
            (7 << tp2cc_dos_filetime_hour_shift) |
            (8 << tp2cc_dos_filetime_minute_shift) |
            (10 / tp2cc_dos_filetime_second_quantum)));

  t_datetime out{};
  p_unpacktime(packed, out);
  CHECK_EQ(out.p_year, in.p_year);
  CHECK_EQ(out.p_month, in.p_month);
  CHECK_EQ(out.p_day, in.p_day);
  CHECK_EQ(out.p_hour, in.p_hour);
  CHECK_EQ(out.p_min, in.p_min);
  CHECK_EQ(out.p_sec, in.p_sec);
}

void test_getfattr_reports_directory_bit() {
  char dir_template[] = "/tmp/tp2cc-dir-XXXXXX";
  char file_template[] = "/tmp/tp2cc-file-XXXXXX";
  char* dir = ::mkdtemp(dir_template);
  CHECK(dir != nullptr);
  int fd = ::mkstemp(file_template);
  CHECK(fd >= 0);
  ::close(fd);

  tp2cc_TypedFile<uint8_t> dir_file{};
  p_assign(dir_file, tp2cc_shortstring_of<>(dir));
  uint16_t dir_attr = 0;
  p_getfattr(dir_file, dir_attr);
  CHECK_EQ(p_doserror, 0);
  CHECK((dir_attr & p_directory) == p_directory);

  tp2cc_TypedFile<uint8_t> plain_file{};
  p_assign(plain_file, tp2cc_shortstring_of<>(file_template));
  uint16_t plain_attr = 0;
  p_getfattr(plain_file, plain_attr);
  CHECK_EQ(p_doserror, 0);
  CHECK((plain_attr & p_directory) == 0);

  ::rmdir(dir);
  ::unlink(file_template);
}

void test_set_superset_operator_matches_pascal() {
  auto bigger = tp2cc_Set<int32_t>::from_list({1, 2});
  auto smaller = tp2cc_Set<int32_t>::from_list({1});

  CHECK(bigger >= smaller);
  CHECK(!(smaller >= bigger));
}

void test_empty_set_membership_is_always_false() {
  CHECK(!EmptySet{}.contains(static_cast<int32_t>(1)));
  CHECK(!EmptySet{}.contains(tp2cc_char_of('x')));
  CHECK(!EmptySet{}.contains(true));
}

void test_explicit_set_cast_copies_bits() {
  auto src = tp2cc_Set<int32_t>::from_list({1, 7});
  auto dst = tp2cc_set_cast<tp2cc_Set<uint8_t>>(src);

  CHECK(dst.contains(static_cast<uint8_t>(1)));
  CHECK(dst.contains(static_cast<uint8_t>(7)));
  CHECK(!dst.contains(static_cast<uint8_t>(2)));
}

void test_method_ptr_calls_bound_thunk() {
  MethodPtrCounter counter;
  auto cb = tp2cc_method_ptr<void(int32_t)>(tp2cc_method_code<&method_ptr_add>(), &counter);

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
  tp2cc_MethodPtr<void(int32_t)> cb{};
  Slots slots = tp2cc_reinterpret_load<Slots>(&cb);

  slots.procpointer = tp2cc_method_code<&method_ptr_add>();
  slots.self = &counter;
  tp2cc_reinterpret_store<Slots>(&cb, slots);

  CHECK(cb != nullptr);
  cb(9);
  CHECK_EQ(counter.value, 9);
}

void test_tmethod_storage_matches_two_pointer_slots() {
  MethodPtrCounter counter;
  tp2cc_MethodPtr<void(int32_t)> cb{};
  t_tmethod raw = tp2cc_reinterpret_load<t_tmethod>(&cb);

  raw.p_code = tp2cc_method_code<&method_ptr_add>();
  raw.p_data = &counter;
  tp2cc_reinterpret_store<t_tmethod>(&cb, raw);

  CHECK(cb != nullptr);
  cb(5);
  CHECK_EQ(counter.value, 5);
}

void test_method_ptr_constructs_from_tmethod_carrier() {
  MethodPtrCounter counter;
  t_tmethod raw{};

  raw.p_code = tp2cc_method_code<&method_ptr_add>();
  raw.p_data = &counter;

  auto cb =
      tp2cc_method_ptr_from_tmethod<tp2cc_MethodPtr<void(int32_t)>>(raw);

  CHECK(cb != nullptr);
  cb(6);
  CHECK_EQ(counter.value, 6);
}

void test_ppointer_alias_updates_pointer_slot() {
  void* slot = nullptr;
  int value = 0;

  tp2cc_deref(t_ppointer(&slot)) = &value;
  CHECK(slot == static_cast<void*>(&value));
}

void test_class_free_accepts_null_pointer() {
  DestroyProbe* p = nullptr;

  t_tobject::p_free(p);
  CHECK(p == nullptr);
}

void test_class_free_dispatches_virtual_destroy() {
  int destroys = 0;
  auto* p = new DestroyProbe(&destroys);

  t_tobject::p_free(p);
  CHECK_EQ(destroys, 1);
}

void test_class_free_dispatches_virtual_freeinstance() {
  int destroys = 0;
  int frees = 0;
  auto* p = new FreeInstanceProbe(&destroys, &frees);

  t_tobject::p_free(p);
  CHECK_EQ(destroys, 1);
  CHECK_EQ(frees, 1);
}

void test_tobject_metaclass_exists_and_constructs_root_instance() {
  const auto* meta = tp2cc_metaclass_value_t_tobject();

  CHECK(meta != nullptr);
  CHECK(meta->p_create != nullptr);

  t_tobject* instance = meta->p_create();
  CHECK(instance != nullptr);
  CHECK(instance->p_classtype() == meta);

  t_tobject::p_free(instance);
}

void test_exception_metaclass_exists_and_constructs_exception_instance() {
  const auto* meta = tp2cc_metaclass_value_t_exception();

  CHECK(meta != nullptr);
  CHECK(meta->p_create != nullptr);

  t_exception* instance = meta->p_create(tp2cc_shortstring_of<>("boom"));
  CHECK(instance != nullptr);
  CHECK(instance->p_classtype() == meta);
  CHECK_EQ(tp2cc_to_std_string(instance->p_message), std::string("boom"));

  t_tobject::p_free(instance);

  const auto* root_view =
      static_cast<const tp2cc_metaclass_t_tobject*>(meta);
  CHECK(root_view->p_create != nullptr);
  t_tobject* root_instance = root_view->p_create();
  CHECK(root_instance != nullptr);
  CHECK(root_instance->p_classtype() == meta);
  CHECK(dynamic_cast<t_exception*>(root_instance) != nullptr);

  t_tobject::p_free(root_instance);
}

void test_classname_comes_from_metaclass_descriptor() {
  t_tobject root;
  t_exception exc;

  CHECK(tp2cc_metaclass_value_t_tobject()->p_classname() ==
        tp2cc_shortstring_of<>("tobject"));
  CHECK(root.p_classname() == tp2cc_shortstring_of<>("tobject"));
  CHECK(tp2cc_metaclass_value_t_exception()->p_classname() ==
        tp2cc_shortstring_of<>("exception"));
  CHECK(exc.p_classname() == tp2cc_shortstring_of<>("exception"));
}

void test_exception_create_stores_message_for_pascal_message_property() {
  // Pascal `Exception.Message` stores the string passed to `Create`.
  // Catch sites read `e.message` (e.g. comprsrc.pas:394
  // `'Error processing resource file: ' + ... + E.Message`), so the
  // runtime stub has to actually retain the constructor argument.
  t_exception e;
  e.p_create(tp2cc_shortstring_of<>("disk full"));
  CHECK_EQ(tp2cc_to_std_string(e.p_message), std::string("disk full"));

  e.p_create(tp2cc_ansistring_of("permission denied"));
  CHECK_EQ(tp2cc_to_std_string(e.p_message), std::string("permission denied"));
}

void test_sysutils_exception_runtime_classes() {
  t_eoutofmemory oom;
  t_einouterror io;
  CHECK(dynamic_cast<t_exception*>(&oom) != nullptr);
  CHECK(dynamic_cast<t_eheapmemoryerror*>(&oom) != nullptr);
  CHECK(dynamic_cast<t_exception*>(&io) != nullptr);
  io.p_errorcode = 5;
  CHECK_EQ(io.p_errorcode, 5);
}

void test_exception_metaclass_accepts_concrete_root_create_thunk() {
  tp2cc_metaclass_t_exception meta(tp2cc_metaclass_t_tobject(+[]() -> t_tobject* {
    auto* instance = new t_exception{};
    instance->p_create();
    return instance;
  }));

  auto* root_view = static_cast<tp2cc_metaclass_t_tobject*>(&meta);
  CHECK(root_view->p_create != nullptr);
  CHECK(meta.tp2cc_parentclass() == tp2cc_metaclass_value_t_tobject());

  t_tobject* instance = root_view->p_create();
  CHECK(instance != nullptr);
  CHECK(dynamic_cast<t_exception*>(instance) != nullptr);

  t_tobject::p_free(instance);
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

  p_fillword(static_cast<void*>(words), 4, 0x1234);
  CHECK_EQ(std::memcmp(words, same, sizeof(words)), 0);
  CHECK_EQ(p_compareword(static_cast<const void*>(words),
                         static_cast<const void*>(same), 4), 0);
  CHECK(p_compareword(static_cast<const void*>(words),
                      static_cast<const void*>(different), 4) < 0);
}

void test_fillbyte_initialize_trim_and_strrscan_helpers() {
  uint8_t bytes[4] = {0, 0, 0, 0};
  p_fillbyte(static_cast<void*>(bytes), 4, 0x5a);
  CHECK_EQ(bytes[0], static_cast<uint8_t>(0x5a));
  CHECK_EQ(bytes[3], static_cast<uint8_t>(0x5a));
  p_fillchar(static_cast<void*>(bytes), 4, tp2cc_char_of('A'));
  CHECK_EQ(bytes[0], static_cast<uint8_t>('A'));
  CHECK_EQ(bytes[3], static_cast<uint8_t>('A'));

  int32_t value = 17;
  p_initialize(value);
  CHECK_EQ(value, 0);

  auto trimmed = p_trim(tp2cc_shortstring_of<>(" \tabc \n"));
  CHECK_EQ(tp2cc_to_std_string(trimmed), std::string("abc"));

  p_char text[] = {
      tp2cc_char_of('a'), tp2cc_char_of('b'), tp2cc_char_of('a'),
      tp2cc_char_of('\0')};
  CHECK(p_strrscan(text, tp2cc_char_of('a')) == &text[2]);
  CHECK(p_strrscan(text, tp2cc_char_of('z')) == nullptr);
}

void test_indexword_searches_prefix_only() {
  tp2cc_Array<uint16_t, 0, 5> words{};
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

  CHECK_EQ(p_comparebyte(static_cast<const void*>(a),
                         static_cast<const void*>(b), 4), 0);
  CHECK(p_comparebyte(static_cast<const void*>(a),
                      static_cast<const void*>(c), 4) < 0);
}

void test_blockread_writes_to_void_buffer() {
  tp2cc_TypedFile<uint8_t> f;
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

void test_blockwrite_uses_underlying_shortstring_char_storage() {
  tp2cc_TextFile f{};
  tp2cc_ShortString<> text = tp2cc_shortstring_of<>("hello");
  int32_t transferred = -1;
  char got[6] = {};

  f.f = std::tmpfile();
  CHECK(f.f != nullptr);

  p_blockwrite(f, static_cast<const void*>(text.data), p_length(text), transferred);
  CHECK_EQ(transferred, 5);
  std::rewind(f.f);
  CHECK_EQ(std::fread(got, 1, 5, f.f), static_cast<std::size_t>(5));
  CHECK_EQ(std::string(got, 5), std::string("hello"));

  const tp2cc_ShortString<> const_text = tp2cc_shortstring_of<>("ok");
  p_seek(f, 0);
  p_blockwrite(f, static_cast<const void*>(const_text.data), p_length(const_text),
               transferred);
  CHECK_EQ(transferred, 2);
  std::rewind(f.f);
  CHECK_EQ(std::fread(got, 1, 2, f.f), static_cast<std::size_t>(2));
  CHECK_EQ(std::string(got, 2), std::string("ok"));

  std::fclose(f.f);
  f.f = nullptr;
}

void test_blockread_uses_underlying_shortstring_char_storage() {
  tp2cc_TextFile f{};
  tp2cc_ShortString<> text{};
  int32_t transferred = -1;

  f.f = std::tmpfile();
  CHECK(f.f != nullptr);
  const char* src = "abc";
  std::fwrite(src, 1, 3, f.f);
  std::rewind(f.f);

  p_blockread(f, static_cast<void*>(text.data), 3, transferred);
  CHECK_EQ(transferred, 3);
  text.length = 3;
  CHECK_EQ(tp2cc_to_std_string(text), std::string("abc"));

  std::fclose(f.f);
  f.f = nullptr;
}

void test_strnew_allocates_and_disposes_pchar() {
  p_char* text = p_strnew("hello");
  CHECK(text != nullptr);
  CHECK_EQ(tp2cc_to_std_string(text), std::string("hello"));
  p_strdispose(text);
  CHECK(text == nullptr);
}

void test_textfile_reset_closes_previous_handle() {
  char path[] = "/tmp/tp2cc-reset-XXXXXX";
  int fd = ::mkstemp(path);
  CHECK(fd >= 0);
  ::close(fd);

  tp2cc_TextFile f;
  p_assign(f, tp2cc_shortstring_of<>(path));
  p_rewrite(f);
  CHECK(f.f != nullptr);
  p_write(f, tp2cc_shortstring_of<>("hello"));

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
  p_exec(tp2cc_shortstring_of<>("/bin/sh"), tp2cc_shortstring_of<>("-c 'exit 9'"));
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(p_dosexitcode(), 9);
}

void test_exec_reports_spawn_failure() {
  p_doserror = 0;
  p_exec(tp2cc_shortstring_of<>("/definitely/not/a/real/command"),
         tp2cc_shortstring_of<>(""));
  CHECK(p_doserror != 0);
  CHECK_EQ(p_dosexitcode(), 0);
}

void test_shell_tracks_exit_status() {
  p_doserror = -1;
  int32_t rc = p_shell(tp2cc_shortstring_of<>("exit 7"));
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(rc, 7);
  CHECK_EQ(p_dosexitcode(), 7);
}

}  // namespace

int main() {
  RUN_TEST(test_val_accepts_prefixed_integers);
  RUN_TEST(test_val_accepts_decimal_min_longint);
  RUN_TEST(test_val_handles_bootstrap_integer_forms);
  RUN_TEST(test_val_keeps_leading_zero_decimals_decimal);
  RUN_TEST(test_pascal_shift_helpers_mask_count_and_shr_logically);
  RUN_TEST(test_signed_wrap_helpers_avoid_ub);
  RUN_TEST(test_pointer_arithmetic_helpers_use_pascal_address_semantics);
  RUN_TEST(test_runtime_path_helpers_match_compiler_expectations);
  RUN_TEST(test_runtime_tdatetime_decodes_current_and_dos_times);
  RUN_TEST(test_runtime_file_helpers_expose_real_sysutils_surface);
  RUN_TEST(test_runtime_swap_fill_and_compare_helpers);
  RUN_TEST(test_runtime_rotate_helpers_mask_distance);
  RUN_TEST(test_runtime_bitscan_helpers_match_fpc_results);
  RUN_TEST(test_runtime_sarlongint_matches_fpc_results);
  RUN_TEST(test_getmem_typed_pointer_keeps_requested_prefix_size);
  RUN_TEST(test_shortstring_pointer_deref_uses_live_prefix_storage);
  RUN_TEST(test_shortstring_pointer_deref_interoperates_with_string_ops);
  RUN_TEST(test_exception_mask_roundtrips);
  RUN_TEST(test_8087cw_compatibility_tracks_mask_bits);
  RUN_TEST(test_ansistring_copy_on_write_preserves_original);
  RUN_TEST(test_ansistring_index_proxy_has_byte_cast_like_shortstring);
  RUN_TEST(test_ansistring_storage_slot_holds_payload_pointer);
  RUN_TEST(test_new_and_dispose_share_malloc_storage_family);
  RUN_TEST(test_dispose_releases_plain_storage_grown_with_reallocmem);
  RUN_TEST(test_reallocmem_returns_updated_pointer_slot);
  RUN_TEST(test_pointer_slot_helpers_update_byte_storage);
  RUN_TEST(test_strdispose_slot_clears_byte_storage);
  RUN_TEST(test_storage_ref_preserves_pascal_address_identity);
  RUN_TEST(test_storage_ref_does_not_run_default_member_initializers);
  RUN_TEST(test_scoped_storage_view_restores_backing_lifetime);
  RUN_TEST(test_ansistring_setlength_and_insert_delete_keep_bytes_stable);
  RUN_TEST(test_ansistring_compares_equal_to_single_char_pascal_style);
  RUN_TEST(test_ansistring_converts_to_shortstring_with_pascal_truncation);
  RUN_TEST(test_shortstring_char_concat_grows_capacity);
  RUN_TEST(test_shortstring_single_nul_char_keeps_length_one);
  RUN_TEST(test_shortstring_nul_char_concat_preserves_embedded_zero);
  RUN_TEST(test_shortstring_literal_helper_preserves_embedded_nuls);
  RUN_TEST(test_shortstring_implicitly_converts_between_capacities);
  RUN_TEST(test_shortstring_ref_mutating_helpers_write_through_storage);
  RUN_TEST(test_shortstring_pointer_value_reads_only_live_prefix);
  RUN_TEST(test_shortstring_implicitly_converts_to_ansistring);
  RUN_TEST(test_shortstring_assign_from_char_creates_one_character_string);
  RUN_TEST(test_strpas_returns_shortstring_up_to_first_nul);
  RUN_TEST(test_ansistring_from_shortstring_keeps_trailing_nul_storage);
  RUN_TEST(test_shortstring_charref_inc_and_dec_update_length_slot_storage);
  RUN_TEST(test_shortstring_index_address_names_layout_bytes);
  RUN_TEST(test_octstr_formats_octal_with_zero_padding);
  RUN_TEST(test_move_reads_from_const_shortstring_storage);
  RUN_TEST(test_shortstring_compares_equal_to_pchar_buffer);
  RUN_TEST(test_insert_accepts_shortstring_pointer_proxy_source);
  RUN_TEST(test_char_array_compares_equal_to_shortstring_by_live_prefix);
  RUN_TEST(test_char_array_assignment_from_shortstring_matches_fpc);
  RUN_TEST(test_shortstring_assignment_from_char_array_matches_fpc);
  RUN_TEST(test_str_formats_real_values);
  RUN_TEST(test_inttostr_formats_integer_values);
  RUN_TEST(test_setstring_copies_counted_bytes);
  RUN_TEST(test_reinterpret_bytes_copies_raw_object_bytes);
  RUN_TEST(test_reinterpret_copy_preserves_scalar_bit_pattern);
  RUN_TEST(test_reinterpret_load_store_and_inc_handle_misaligned_bytes);
  RUN_TEST(test_unaligned_load_store_handle_misaligned_bytes);
  RUN_TEST(test_scope_exit_runs_on_exception_unwind);
  RUN_TEST(test_open_array_helper_owns_temporary_storage);
  RUN_TEST(test_dynamic_array_setlength_detaches_and_zeroes_tail);
  RUN_TEST(test_dynamic_array_copy_makes_independent_storage);
  RUN_TEST(test_dynamic_array_assignment_from_fixed_array_copies_values);
  RUN_TEST(test_open_array_view_uses_dynamic_array_storage);
  RUN_TEST(test_dos_pack_unpack_time_matches_bit_layout);
  RUN_TEST(test_getfattr_reports_directory_bit);
  RUN_TEST(test_set_superset_operator_matches_pascal);
  RUN_TEST(test_empty_set_membership_is_always_false);
  RUN_TEST(test_explicit_set_cast_copies_bits);
  RUN_TEST(test_method_ptr_calls_bound_thunk);
  RUN_TEST(test_method_ptr_storage_matches_two_pointer_slots);
  RUN_TEST(test_tmethod_storage_matches_two_pointer_slots);
  RUN_TEST(test_method_ptr_constructs_from_tmethod_carrier);
  RUN_TEST(test_ppointer_alias_updates_pointer_slot);
  RUN_TEST(test_class_free_accepts_null_pointer);
  RUN_TEST(test_class_free_dispatches_virtual_destroy);
  RUN_TEST(test_class_free_dispatches_virtual_freeinstance);
  RUN_TEST(test_tobject_metaclass_exists_and_constructs_root_instance);
  RUN_TEST(test_exception_metaclass_exists_and_constructs_exception_instance);
  RUN_TEST(test_classname_comes_from_metaclass_descriptor);
  RUN_TEST(test_exception_create_stores_message_for_pascal_message_property);
  RUN_TEST(test_sysutils_exception_runtime_classes);
  RUN_TEST(test_exception_metaclass_accepts_concrete_root_create_thunk);
  RUN_TEST(test_hi_lo_split_ordinal_halves);
  RUN_TEST(test_fillword_and_compareword_operate_on_word_counts);
  RUN_TEST(test_fillbyte_initialize_trim_and_strrscan_helpers);
  RUN_TEST(test_indexword_searches_prefix_only);
  RUN_TEST(test_comparebyte_operates_on_byte_counts);
  RUN_TEST(test_blockread_writes_to_void_buffer);
  RUN_TEST(test_blockwrite_uses_underlying_shortstring_char_storage);
  RUN_TEST(test_blockread_uses_underlying_shortstring_char_storage);
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
