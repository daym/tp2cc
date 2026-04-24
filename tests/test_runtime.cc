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

struct DisposeProbe {
  static inline int destroys = 0;
  int32_t value = 0;

  ~DisposeProbe() { ++destroys; }
};

void test_val_accepts_prefixed_integers() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(p_shortstring_of<>("$7fffffff"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 2147483647);

  p_val(p_shortstring_of<>("$80000000"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());

  p_val(p_shortstring_of<>("$D7B0"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 55216);

  p_val(p_shortstring_of<>("%1010"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 10);
}

void test_val_accepts_decimal_min_longint() {
  int32_t v = 0;
  int32_t code = -1;

  p_val(p_shortstring_of<>("-2147483648"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, std::numeric_limits<int32_t>::min());
}

void test_val_handles_bootstrap_integer_forms() {
  int32_t v = 123;
  int32_t code = -1;

  p_val(p_shortstring_of<>("&77"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 63);

  p_val(p_shortstring_of<>("2147483648"), v, code);
  CHECK_EQ(code, 10);
  CHECK_EQ(v, 0);
}

void test_val_keeps_leading_zero_decimals_decimal() {
  uint32_t v = 0;
  int32_t code = -1;

  p_val(p_shortstring_of<>("01012"), v, code);
  CHECK_EQ(code, 0);
  CHECK_EQ(v, 1012u);
}

void test_bootstrap_pointer_sized_aliases_are_32bit() {
  static_assert(std::is_same_v<p_sizeint, int32_t>);
  static_assert(std::is_same_v<p_sizeuint, uint32_t>);
  static_assert(std::is_same_v<p_ptrint, int32_t>);
  static_assert(std::is_same_v<p_ptruint, uint32_t>);

  CHECK_EQ(p_maxint, std::numeric_limits<int32_t>::max());
}

void test_getmem_typed_pointer_keeps_requested_prefix_size() {
  using HugeSymIndex = Array<void*, 0, 536870911>;

  HugeSymIndex* p = nullptr;
  p_getmem(p, static_cast<int>(4 * sizeof(void*)));

  CHECK(p != nullptr);
  p_freemem(p);
  CHECK(p == nullptr);
}

void test_shortstring_pointer_deref_uses_live_prefix_storage() {
  ShortString<>* p = nullptr;
  const auto hello = p_shortstring_of<>("hello");
  const auto one = p_shortstring_of<>("A");

  p_getmem(p, p_length(hello) + 1);
  CHECK(p != nullptr);

  p_shortstring_assign(p_deref(p), hello);
  CHECK_EQ(p_length(p_deref(p)), 5);
  CHECK(p_deref(p) == hello);

  p_shortstring_assign(p_deref(p), one);
  CHECK_EQ(p_length(p_deref(p)), 1);
  CHECK(static_cast<p_char>(p_deref(p)[1]) == p_char_of('A'));

  p_deref(p)[1] = p_char_of('Z');
  CHECK(static_cast<p_char>(p_deref(p)[1]) == p_char_of('Z'));

  p_freemem(p, p_length(p_deref(p)) + 1);
  CHECK(p == nullptr);
}

void test_shortstring_pointer_deref_interoperates_with_string_ops() {
  ShortString<>* lhs = nullptr;
  ShortString<>* rhs = nullptr;
  const auto foo = p_shortstring_of<>("foo");
  const auto bar = p_shortstring_of<>("bar");
  const auto zoo = p_shortstring_of<>("zoo");
  TextFile f{};

  p_getmem(lhs, p_length(foo) + 1);
  p_getmem(rhs, p_length(bar) + 1);
  CHECK(lhs != nullptr);
  CHECK(rhs != nullptr);

  p_shortstring_assign(p_deref(lhs), foo);
  p_shortstring_assign(p_deref(rhs), bar);

  CHECK_EQ(p_to_std_string(foo + p_deref(rhs)), std::string("foobar"));
  CHECK_EQ(p_to_std_string(p_deref(lhs) + p_deref(rhs)),
           std::string("foobar"));
  CHECK(p_deref(lhs) < zoo);
  CHECK(zoo > p_deref(rhs));
  CHECK_EQ(p_to_std_string(p_copy(p_deref(lhs), 2, 2)), std::string("oo"));
  CHECK_EQ(p_pos(p_shortstring_of<>("ar"), p_deref(rhs)), 2);

  p_assign(f, p_deref(lhs));
  CHECK_EQ(p_to_std_string(f.name), std::string("foo"));

  p_freemem(lhs, p_length(p_deref(lhs)) + 1);
  p_freemem(rhs, p_length(p_deref(rhs)) + 1);
  CHECK(lhs == nullptr);
  CHECK(rhs == nullptr);
}

void test_exception_mask_roundtrips() {
#if defined(__linux__)
  enum TestFPUException : uint8_t {
    ExInvalidOp,
    ExDenormalized,
    ExZeroDivide,
    ExOverflow,
    ExUnderflow,
    ExPrecision
  };

  const auto original = p_getexceptionmask<TestFPUException>();
  const auto masked = Set<TestFPUException>::from_list(
      {ExInvalidOp, ExDenormalized, ExZeroDivide, ExOverflow, ExUnderflow, ExPrecision});

  CHECK_EQ(p_setexceptionmask(masked), original);
  CHECK_EQ(p_getexceptionmask<TestFPUException>(), masked);

  CHECK_EQ(p_setexceptionmask(original), masked);
  CHECK_EQ(p_getexceptionmask<TestFPUException>(), original);
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
  AnsiString original = p_ansistring_of("abc");
  AnsiString copy = p_ansistring_of(original);

  copy[1] = p_char_of('z');

  CHECK_EQ(p_to_std_string(original), std::string("abc"));
  CHECK_EQ(p_to_std_string(copy), std::string("zbc"));
}

void test_ansistring_storage_slot_holds_payload_pointer() {
  AnsiString s = p_ansistring_of("hello");

  auto& slot = p_reinterpret_storage_ref<void*>(s);

  CHECK(slot == static_cast<void*>(static_cast<p_char*>(s)));
  CHECK_EQ(p_deref(slot), 'h');
}

void test_new_and_dispose_share_malloc_storage_family() {
  DisposeProbe::destroys = 0;

  DisposeProbe* p = nullptr;
  p_new(p);
  CHECK(p != nullptr);
  p->value = 7;

  p_dispose(p);
  CHECK(p == nullptr);
  CHECK_EQ(DisposeProbe::destroys, 1);
}

void test_dispose_releases_plain_storage_grown_with_reallocmem() {
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
  CHECK(p == nullptr);
}

void test_ansistring_setlength_and_insert_delete_keep_bytes_stable() {
  AnsiString s = p_ansistring_of("ab");

  p_setlength(s, 4);
  auto& slot = p_reinterpret_storage_ref<void*>(s);
  static_cast<p_char*>(slot)[2] = p_char_of('c');
  static_cast<p_char*>(slot)[3] = p_char_of('d');

  CHECK_EQ(p_length(s), 4);
  CHECK_EQ(p_to_std_string(s), std::string("abcd"));

  p_delete(s, 2, 2);
  CHECK_EQ(p_to_std_string(s), std::string("ad"));

  p_insert(p_shortstring_of<>("bc"), s, 2);
  CHECK_EQ(p_to_std_string(s), std::string("abcd"));
}

void test_ansistring_converts_to_shortstring_with_pascal_truncation() {
  AnsiString s = p_ansistring_of("abcdef");
  auto shorty = static_cast<ShortString<4>>(s);

  CHECK_EQ(p_to_std_string(shorty), std::string("abcd"));
}

void test_shortstring_char_concat_grows_capacity() {
  auto label = p_shortstring_of<2>(".L") + p_char_of('e') + p_char_of('0');
  CHECK_EQ(p_to_std_string(label), std::string(".Le0"));
}

void test_shortstring_single_nul_char_keeps_length_one() {
  ShortString<> s = p_shortstring_of<>(p_char_of('\0'));

  CHECK_EQ(p_length(s), 1);
  CHECK_EQ(p_char_byte(s.data[0]), 0);
}

void test_shortstring_nul_char_concat_preserves_embedded_zero() {
  auto s = p_shortstring_of<>(p_char_of('\0')) + p_char_of('A');

  CHECK_EQ(p_length(s), 2);
  CHECK_EQ(p_char_byte(s.data[0]), 0);
  CHECK_EQ(p_char_byte(s.data[1]), static_cast<uint8_t>('A'));
}

void test_shortstring_literal_helper_preserves_embedded_nuls() {
  auto s = p_shortstring_literal<255>(p_char_of('\x8d'), p_char_of('\xb4'),
                                      p_char_of('&'), p_char_of('\0'),
                                      p_char_of('\0'), p_char_of('\0'),
                                      p_char_of('\0'));

  CHECK_EQ(p_length(s), 7);
  CHECK_EQ(p_char_byte(s.data[0]), 0x8d);
  CHECK_EQ(p_char_byte(s.data[1]), 0xb4);
  CHECK_EQ(p_char_byte(s.data[2]), static_cast<uint8_t>('&'));
  CHECK_EQ(p_char_byte(s.data[3]), 0);
  CHECK_EQ(p_char_byte(s.data[4]), 0);
  CHECK_EQ(p_char_byte(s.data[5]), 0);
  CHECK_EQ(p_char_byte(s.data[6]), 0);
}

void test_shortstring_implicitly_converts_between_capacities() {
  ShortString<4> small = p_shortstring_of<4>("abcdef");
  ShortString<> wide = small;
  ShortString<2> narrow = wide;

  CHECK_EQ(p_to_std_string(wide), std::string("abcd"));
  CHECK_EQ(p_to_std_string(narrow), std::string("ab"));
}

void test_ansistring_from_shortstring_keeps_trailing_nul_storage() {
  AnsiString s = p_ansistring_of(
      p_shortstring_literal<255>(p_char_of('A'), p_char_of('\0'),
                                 p_char_of('B')));

  CHECK_EQ(s.length(), 3);
  CHECK_EQ(p_char_byte(s.bytes()[0]), static_cast<uint8_t>('A'));
  CHECK_EQ(p_char_byte(s.bytes()[1]), 0);
  CHECK_EQ(p_char_byte(s.bytes()[2]), static_cast<uint8_t>('B'));
  CHECK_EQ(p_char_byte(s.bytes()[3]), 0);
}

void test_shortstring_charref_inc_and_dec_update_length_slot_storage() {
  ShortString<> s = p_shortstring_of<>("A");

  p_inc(s[1]);
  CHECK_EQ(p_to_std_string(s), std::string("B"));

  p_dec(s[1], 1);
  CHECK_EQ(p_to_std_string(s), std::string("A"));
}

void test_octstr_formats_octal_with_zero_padding() {
  CHECK_EQ(p_to_std_string(p_octstr(9, 4)), std::string("0011"));
}

void test_move_reads_from_const_shortstring_storage() {
  const ShortString<> text = p_shortstring_of<>("hello");
  Array<p_char, 0, 8> buf;

  p_move(text[1], buf[0], p_length(text));
  buf[p_length(text)] = p_char_of('\0');

  CHECK_EQ(p_to_std_string(static_cast<p_char*>(buf)), std::string("hello"));
}

void test_shortstring_compares_equal_to_pchar_buffer() {
  const p_char text[] = {
      p_char_of('h'), p_char_of('e'), p_char_of('l'),
      p_char_of('l'), p_char_of('o'), p_char_of('\0')};
  const ShortString<> shorty = p_shortstring_of<>("hello");

  CHECK(text == shorty);
  CHECK(shorty == text);
}

void test_char_array_compares_equal_to_shortstring_by_live_prefix() {
  Array<p_char, 0, 8> text{};
  text.data[0] = p_char_of('h');
  text.data[1] = p_char_of('i');
  text.data[2] = p_char_of('\0');

  CHECK(text == p_shortstring_of<>("hi"));
  CHECK(p_shortstring_of<>("hi") == text);
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

void test_reinterpret_copy_preserves_scalar_bit_pattern() {
  Array<uint8_t, 0, 8> bits{};
  bits.data[0] = 0;
  bits.data[1] = 0;
  bits.data[2] = 0;
  bits.data[3] = 0;
  bits.data[4] = 0;
  bits.data[5] = 0;
  bits.data[6] = 240;
  bits.data[7] = 127;

  double value = p_reinterpret_copy<double>(bits);
  uint8_t raw[sizeof(value)] = {};
  std::memcpy(raw, &value, sizeof(value));

  for (int i = 0; i < 8; ++i) {
    CHECK_EQ(raw[i], bits.data[i]);
  }
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

void test_open_array_helper_owns_temporary_storage() {
  auto holder = p_open_array_of<int32_t>(1, 2, 3);
  OpenArray<int32_t> view = holder;

  CHECK_EQ(view.count, 3);
  CHECK_EQ(view[0], 1);
  CHECK_EQ(view[1], 2);
  CHECK_EQ(view[2], 3);
}

void test_dynamic_array_setlength_detaches_and_zeroes_tail() {
  DynArray<int32_t> values;
  p_setlength(values, 2);
  values[0] = 7;
  values[1] = 9;

  DynArray<int32_t> alias = values;
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

void test_open_array_view_uses_dynamic_array_storage() {
  DynArray<int32_t> values;
  p_setlength(values, 3);
  values[0] = 1;
  values[1] = 2;
  values[2] = 3;

  OpenArray<int32_t> view = p_open_array<int32_t>(values);
  CHECK_EQ(view.count, 3);
  CHECK(view.data == values.ptr());

  view[1] = 8;
  CHECK_EQ(values[1], 8);
}

void test_dos_pack_unpack_time_matches_bit_layout() {
  DateTime in{};
  in.p_year = 2004;
  in.p_month = 5;
  in.p_day = 6;
  in.p_hour = 7;
  in.p_min = 8;
  in.p_sec = 10;

  int32_t packed = 0;
  p_packtime(in, packed);
  CHECK_EQ(packed,
           (((2004 - 1980) << 25) | (5 << 21) | (6 << 16) |
            (7 << 11) | (8 << 5) | (10 / 2)));

  DateTime out{};
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

  TypedFile<uint8_t> dir_file{};
  p_assign(dir_file, p_shortstring_of<>(dir));
  uint16_t dir_attr = 0;
  p_getfattr(dir_file, dir_attr);
  CHECK_EQ(p_doserror, 0);
  CHECK((dir_attr & p_directory) == p_directory);

  TypedFile<uint8_t> plain_file{};
  p_assign(plain_file, p_shortstring_of<>(file_template));
  uint16_t plain_attr = 0;
  p_getfattr(plain_file, plain_attr);
  CHECK_EQ(p_doserror, 0);
  CHECK((plain_attr & p_directory) == 0);

  ::rmdir(dir);
  ::unlink(file_template);
}

void test_set_superset_operator_matches_pascal() {
  auto bigger = set_of<int32_t>({1, 2});
  auto smaller = set_of<int32_t>({1});

  CHECK(bigger >= smaller);
  CHECK(!(smaller >= bigger));
}

void test_explicit_set_cast_copies_bits() {
  auto src = set_of<int32_t>({1, 7});
  auto dst = p_set_cast<Set<uint8_t>>(src);

  CHECK(dst.contains(static_cast<uint8_t>(1)));
  CHECK(dst.contains(static_cast<uint8_t>(7)));
  CHECK(!dst.contains(static_cast<uint8_t>(2)));
}

void test_method_ptr_calls_bound_thunk() {
  MethodPtrCounter counter;
  auto cb = p_method_ptr<void(int32_t)>(p_method_code<&method_ptr_add>(), &counter);

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

void test_tmethod_storage_matches_two_pointer_slots() {
  MethodPtrCounter counter;
  MethodPtr<void(int32_t)> cb{};
  auto& raw = p_reinterpret_storage_ref<p_tmethod>(cb);

  raw.p_code = p_method_code<&method_ptr_add>();
  raw.p_data = &counter;

  CHECK(cb != nullptr);
  cb(5);
  CHECK_EQ(counter.value, 5);
}

void test_ppointer_alias_updates_pointer_slot() {
  void* slot = nullptr;
  int value = 0;

  p_deref(p_ppointer(&slot)) = &value;
  CHECK(slot == static_cast<void*>(&value));
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

void test_tobject_metaclass_exists_and_constructs_root_instance() {
  const auto* meta = tp2cc_metaclass_value_p_tobject();

  CHECK(meta != nullptr);
  CHECK(meta->p_create != nullptr);

  p_tobject* instance = meta->p_create();
  CHECK(instance != nullptr);
  CHECK(instance->p_classtype() == meta);

  p_tobject::p_free(instance);
}

void test_exception_metaclass_exists_and_constructs_exception_instance() {
  const auto* meta = tp2cc_metaclass_value_p_exception();

  CHECK(meta != nullptr);
  CHECK(meta->p_create != nullptr);

  p_tobject* instance = meta->p_create();
  CHECK(instance != nullptr);
  CHECK(instance->p_classtype() == meta);
  CHECK(dynamic_cast<p_exception*>(instance) != nullptr);

  p_tobject::p_free(instance);
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

void test_blockwrite_uses_underlying_shortstring_char_storage() {
  TextFile f{};
  ShortString<> text = p_shortstring_of<>("hello");
  int32_t transferred = -1;
  char got[6] = {};

  f.f = std::tmpfile();
  CHECK(f.f != nullptr);

  p_blockwrite(f, text[1], p_length(text), transferred);
  CHECK_EQ(transferred, 5);
  std::rewind(f.f);
  CHECK_EQ(std::fread(got, 1, 5, f.f), static_cast<std::size_t>(5));
  CHECK_EQ(std::string(got, 5), std::string("hello"));

  const ShortString<> const_text = p_shortstring_of<>("ok");
  p_seek(f, 0);
  p_blockwrite(f, const_text[1], p_length(const_text), transferred);
  CHECK_EQ(transferred, 2);
  std::rewind(f.f);
  CHECK_EQ(std::fread(got, 1, 2, f.f), static_cast<std::size_t>(2));
  CHECK_EQ(std::string(got, 2), std::string("ok"));

  std::fclose(f.f);
  f.f = nullptr;
}

void test_blockread_uses_underlying_shortstring_char_storage() {
  TextFile f{};
  ShortString<> text{};
  int32_t transferred = -1;

  f.f = std::tmpfile();
  CHECK(f.f != nullptr);
  const char* src = "abc";
  std::fwrite(src, 1, 3, f.f);
  std::rewind(f.f);

  p_blockread(f, text[1], 3, transferred);
  CHECK_EQ(transferred, 3);
  text.length = 3;
  CHECK_EQ(p_to_std_string(text), std::string("abc"));

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
  p_assign(f, p_shortstring_of<>(path));
  p_rewrite(f);
  CHECK(f.f != nullptr);
  p_write(f, p_shortstring_of<>("hello"));

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
  p_exec(p_shortstring_of<>("sh"), p_shortstring_of<>("-c 'exit 9'"));
  CHECK_EQ(p_doserror, 0);
  CHECK_EQ(p_dosexitcode(), 9);
}

void test_exec_reports_spawn_failure() {
  p_doserror = 0;
  p_exec(p_shortstring_of<>("/definitely/not/a/real/command"),
         p_shortstring_of<>(""));
  CHECK(p_doserror != 0);
  CHECK_EQ(p_dosexitcode(), 0);
}

void test_shell_tracks_exit_status() {
  p_doserror = -1;
  int32_t rc = p_shell(p_shortstring_of<>("exit 7"));
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
  RUN_TEST(test_bootstrap_pointer_sized_aliases_are_32bit);
  RUN_TEST(test_getmem_typed_pointer_keeps_requested_prefix_size);
  RUN_TEST(test_shortstring_pointer_deref_uses_live_prefix_storage);
  RUN_TEST(test_shortstring_pointer_deref_interoperates_with_string_ops);
  RUN_TEST(test_exception_mask_roundtrips);
  RUN_TEST(test_8087cw_compatibility_tracks_mask_bits);
  RUN_TEST(test_ansistring_copy_on_write_preserves_original);
  RUN_TEST(test_ansistring_storage_slot_holds_payload_pointer);
  RUN_TEST(test_new_and_dispose_share_malloc_storage_family);
  RUN_TEST(test_dispose_releases_plain_storage_grown_with_reallocmem);
  RUN_TEST(test_ansistring_setlength_and_insert_delete_keep_bytes_stable);
  RUN_TEST(test_ansistring_converts_to_shortstring_with_pascal_truncation);
  RUN_TEST(test_shortstring_char_concat_grows_capacity);
  RUN_TEST(test_shortstring_single_nul_char_keeps_length_one);
  RUN_TEST(test_shortstring_nul_char_concat_preserves_embedded_zero);
  RUN_TEST(test_shortstring_literal_helper_preserves_embedded_nuls);
  RUN_TEST(test_shortstring_implicitly_converts_between_capacities);
  RUN_TEST(test_ansistring_from_shortstring_keeps_trailing_nul_storage);
  RUN_TEST(test_shortstring_charref_inc_and_dec_update_length_slot_storage);
  RUN_TEST(test_octstr_formats_octal_with_zero_padding);
  RUN_TEST(test_move_reads_from_const_shortstring_storage);
  RUN_TEST(test_shortstring_compares_equal_to_pchar_buffer);
  RUN_TEST(test_char_array_compares_equal_to_shortstring_by_live_prefix);
  RUN_TEST(test_str_formats_real_values);
  RUN_TEST(test_reinterpret_bytes_copies_raw_object_bytes);
  RUN_TEST(test_reinterpret_copy_preserves_scalar_bit_pattern);
  RUN_TEST(test_scope_exit_runs_on_exception_unwind);
  RUN_TEST(test_reinterpret_storage_ref_views_pointer_variable_bytes);
  RUN_TEST(test_reinterpret_ref_views_pointee_bytes_of_pointer_value);
  RUN_TEST(test_open_array_helper_owns_temporary_storage);
  RUN_TEST(test_dynamic_array_setlength_detaches_and_zeroes_tail);
  RUN_TEST(test_open_array_view_uses_dynamic_array_storage);
  RUN_TEST(test_dos_pack_unpack_time_matches_bit_layout);
  RUN_TEST(test_getfattr_reports_directory_bit);
  RUN_TEST(test_set_superset_operator_matches_pascal);
  RUN_TEST(test_explicit_set_cast_copies_bits);
  RUN_TEST(test_method_ptr_calls_bound_thunk);
  RUN_TEST(test_method_ptr_storage_matches_two_pointer_slots);
  RUN_TEST(test_tmethod_storage_matches_two_pointer_slots);
  RUN_TEST(test_ppointer_alias_updates_pointer_slot);
  RUN_TEST(test_class_free_accepts_null_pointer);
  RUN_TEST(test_class_free_dispatches_virtual_destroy);
  RUN_TEST(test_class_free_dispatches_virtual_freeinstance);
  RUN_TEST(test_tobject_metaclass_exists_and_constructs_root_instance);
  RUN_TEST(test_exception_metaclass_exists_and_constructs_exception_instance);
  RUN_TEST(test_hi_lo_split_ordinal_halves);
  RUN_TEST(test_fillword_and_compareword_operate_on_word_counts);
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
