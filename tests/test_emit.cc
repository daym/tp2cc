// Emitter tests. Each test parses a small Pascal snippet, emits C++, and
// checks the generated text for the expected lowering. String matches are
// kept loose so formatting-only changes do not churn the tests.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag.h"
#include "emit.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "test_util.h"
#include "typereg.h"

using namespace tp2cc;
using namespace tp2cc::ast;
using namespace tp2cc_test;

namespace {

std::shared_ptr<UnitNode> parse_unit(std::string path, std::string text) {
  auto sf = std::make_unique<SourceFile>();
  sf->path = std::move(path);
  sf->contents = std::move(text);
  Lexer lx(std::move(sf));
  Parser p(lx);
  return p.parse();
}

EmittedUnit compile_snippet(std::string text) {
  auto u = parse_unit("<mem>", std::move(text));
  if (!u) return {};
  return emit_unit(*u);
}

EmittedUnit compile_snippet_with_registry(
    std::string text,
    std::vector<std::pair<std::string, std::string>> extra_units = {}) {
  auto u = parse_unit("<mem>", std::move(text));
  if (!u) return {};

  std::vector<std::shared_ptr<UnitNode>> parsed_units;
  parsed_units.push_back(u);
  for (auto& [path, unit_text] : extra_units) {
    auto dep = parse_unit(std::move(path), std::move(unit_text));
    if (!dep) return {};
    parsed_units.push_back(std::move(dep));
  }

  TypeRegistry reg;
  std::vector<const UnitNode*> units;
  units.reserve(parsed_units.size());
  for (const auto& unit : parsed_units) units.push_back(unit.get());
  reg.build(units);
  return emit_unit(*u, &reg);
}

EmittedUnit compile_snippet_with_init_order(
    std::string text, std::vector<std::string> init_order) {
  auto sf = std::make_unique<SourceFile>();
  sf->path = "<mem>";
  sf->contents = std::move(text);
  Lexer lx(std::move(sf));
  Parser p(lx);
  auto u = p.parse();
  if (!u) return {};
  return emit_unit(*u, nullptr, &init_order);
}

bool contains(const std::string& s, std::string_view needle) {
  return s.find(needle) != std::string::npos;
}

size_t count_substring(const std::string& s, std::string_view needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

void test_empty_unit_skeleton() {
  auto out = compile_snippet(
      "unit foo;\n"
      "interface\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "#pragma once"));
  CHECK(contains(out.header, "namespace p_foo {"));
  CHECK(contains(out.header, "}  // namespace p_foo"));
  // Header filenames are `p_<unit>.h` to avoid colliding with system
  // headers like <strings.h>.
  CHECK(contains(out.impl, "#include \"p_foo.h\""));
  CHECK(contains(out.impl, "namespace p_foo {"));
  CHECK(contains(out.header, "void tp2cc_unit_init();"));
  CHECK(contains(out.header, "void tp2cc_unit_fini();"));
  CHECK(contains(out.impl, "void tp2cc_unit_init() {"));
  CHECK(contains(out.impl, "void tp2cc_unit_fini() {"));
  CHECK(!contains(out.header, "__unit_init"));
  CHECK(!contains(out.impl, "__unit_init"));
}

void test_program_registers_unit_finalizers() {
  auto out = compile_snippet_with_init_order(
      "program demo;\n"
      "begin\n"
      "end.\n",
      {"sysutils", "classes"});
  CHECK(contains(out.impl, "p_sysutils::tp2cc_unit_init();"));
  CHECK(contains(out.impl,
                 "std::atexit(p_sysutils::tp2cc_unit_fini)"));
  CHECK(contains(out.impl, "p_classes::tp2cc_unit_init();"));
  CHECK(contains(out.impl,
                 "std::atexit(p_classes::tp2cc_unit_fini)"));
  CHECK(out.impl.find("p_sysutils::tp2cc_unit_init();") <
        out.impl.find("p_classes::tp2cc_unit_init();"));
}

void test_uses_become_includes_without_open_namespaces() {
  auto out = compile_snippet(
      "unit foo;\n"
      "interface\n"
      "uses bar, baz;\n"
      "implementation\n"
      "uses qux;\n"
      "end.\n");
  CHECK(contains(out.header, "#include \"p_bar.h\""));
  CHECK(contains(out.header, "#include \"p_baz.h\""));
  CHECK(contains(out.impl, "#include \"p_qux.h\""));
  CHECK(!contains(out.header, "using namespace p_bar;"));
  CHECK(!contains(out.header, "using namespace p_baz;"));
  CHECK(!contains(out.impl, "using namespace p_qux;"));
  CHECK(!contains(out.header, "using namespace ::rt;"));
  CHECK(!contains(out.impl, "using namespace ::rt;"));
}

void test_scalar_const() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  A = 5;\n"
      "  S = 'hello';\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_a = 5"));
  CHECK(contains(out.header,
                 "p_s = ::rt::tp2cc_shortstring_literal<255>(::rt::tp2cc_char_of('h'), "
                 "::rt::tp2cc_char_of('e'), ::rt::tp2cc_char_of('l'), "
                 "::rt::tp2cc_char_of('l'), ::rt::tp2cc_char_of('o'))"));
}

void test_typed_scalar_const() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  N : longint = 42;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "int32_t p_n = 42"));
}

void test_typed_scalar_const_wraps_to_destination_value() {
  int before = error_count();
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  N : longint = $FFFFFFFF;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.header, "int32_t p_n = -1;"));
  CHECK(!contains(out.header, "bit_cast"));
}

void test_enum_type() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type TColor = (red, green, blue);\n"
      "implementation\n"
      "end.\n");
  // Pascal enums are unscoped -- emit plain `enum` so members are visible
  // at namespace scope, matching Pascal's name lookup.
  CHECK(contains(out.header, "enum p_tcolor : uint8_t"));
  CHECK(contains(out.header, "p_red"));
  CHECK(contains(out.header, "p_green"));
  CHECK(contains(out.header, "p_blue"));
}

void test_enum_type_with_explicit_values() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treg = (lo := low(longint), hi := high(longint));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum p_treg : int32_t {"));
  CHECK(contains(out.header,
                 "p_lo = ::std::numeric_limits<int32_t>::min(),"));
  CHECK(contains(out.header,
                 "p_hi = ::std::numeric_limits<int32_t>::max()"));
}

void test_packed_record_uses_byte_sized_enum_fields() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsmall = (a, b, c);\n"
      "  trec = packed record\n"
      "    hi : word;\n"
      "    lo : tsmall;\n"
      "    kind : tsmall;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum p_tsmall : uint8_t {"));
  CHECK(contains(out.header, "struct [[gnu::packed]] p_trec {"));
  CHECK(contains(out.header, "p_tsmall p_lo;"));
  CHECK(contains(out.header, "p_tsmall p_kind;"));
}

void test_packed_record_shortstring_field_emits_exact_layout_asserts() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    name : string[30];\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_ShortString<30> p_name;"));
  CHECK(contains(out.header, "static_assert(offsetof(p_trec, p_name) == 0"));
  CHECK(contains(out.header,
                 "static_assert(sizeof(p_trec) == (0 + sizeof(::rt::tp2cc_ShortString<30>))"));
}

void test_packed_record_array_field_keeps_array_wrapper_with_exact_layout_asserts() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    future : array[0..2] of longint;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_Array<int32_t, 0, ((2) - (0) + 1)> p_future;"));
  CHECK(contains(out.header, "static_assert(offsetof(p_trec, p_future) == 0"));
  CHECK(contains(out.header,
                 "static_assert(sizeof(p_trec) == (0 + sizeof(::rt::tp2cc_Array<int32_t, 0, ((2) - (0) + 1)>))"));
}

void test_packed_variant_record_emits_packed_case_layout_asserts() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    case byte of\n"
      "      0 : (a : word; b : longint);\n"
      "      1 : (c : longint);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "struct [[gnu::packed]] {"));
  CHECK(contains(out.header, "static_assert(offsetof(p_trec, p_tag) == 0"));
  CHECK(contains(out.header,
                 "static_assert(offsetof(p_trec, p_a) == ((0 + sizeof(uint8_t)) + 0)"));
  CHECK(contains(out.header,
                 "static_assert(offsetof(p_trec, p_b) == ((0 + sizeof(uint8_t)) + (0 + sizeof(uint16_t)))"));
  CHECK(contains(out.header,
                 "static_assert(sizeof(p_trec) == ((0 + sizeof(uint8_t)) + ((((0 + sizeof(uint16_t)) + sizeof(int32_t))) < ((0 + sizeof(int32_t))) ? ((0 + sizeof(int32_t))) : (((0 + sizeof(uint16_t)) + sizeof(int32_t)))))"));
}

void test_packed_record_array_index_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    data : array[0..2] of longint;\n"
      "  end;\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  r : trec;\n"
      "  i : longint;\n"
      "procedure run;\n"
      "begin\n"
      "  i := r.data[0];\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_packed_record_nested_member_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsub = record\n"
      "    x : longint;\n"
      "  end;\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    sub : tsub;\n"
      "  end;\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  r : trec;\n"
      "  i : longint;\n"
      "procedure run;\n"
      "begin\n"
      "  i := r.sub.x;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_packed_record_method_call_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobj = object\n"
      "    procedure ping;\n"
      "  end;\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    child : tobj;\n"
      "  end;\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  r : trec;\n"
      "procedure tobj.ping;\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "begin\n"
      "  r.child.ping;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_packed_record_char_array_index_is_allowed() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    name : array[0..15] of char;\n"
      "  end;\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  r : trec;\n"
      "  c : char;\n"
      "procedure run;\n"
      "begin\n"
      "  c := r.name[0];\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_c = p_r.p_name[0];"));
}

void test_packed_record_shortstring_array_index_is_allowed() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    tag : byte;\n"
      "    names : array[0..1] of string[20];\n"
      "  end;\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  r : trec;\n"
      "  s : string[20];\n"
      "procedure run;\n"
      "begin\n"
      "  s := r.names[0];\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::tp2cc_shortstring_assign(p_s, p_r.p_names[0]);"));
}

void test_explicit_enum_array_bounds_use_ordinal_range() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treg = (lo := low(longint), hi := high(longint));\n"
      "  tmap = array[treg] of byte;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_tmap = ::rt::tp2cc_Array<uint8_t, p_lo, ((::rt::tp2cc_ordinal_value(p_hi)) - (::rt::tp2cc_ordinal_value(p_lo)) + 1)>;"));
}

void test_distinct_ordinal_array_bounds_use_underlying_range() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tindex = type word;\n"
      "  tmap = array[tindex] of byte;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_tmap = ::rt::tp2cc_Array<uint8_t, 0, 65536>;"));
}

void test_low_high_use_resolved_pascal_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tindex = type word;\n"
      "  tarr = array[0..2] of byte;\n"
      "const\n"
      "  maxindex = high(tindex);\n"
      "procedure take(a : tindex; const arr : tarr);\n"
      "implementation\n"
      "procedure take(a : tindex; const arr : tarr);\n"
      "begin\n"
      "  if a = high(a) then begin end;\n"
      "  while low(arr) <= high(arr) do break;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "using p_tindex = uint16_t;"));
  CHECK(contains(out.header,
                 "const auto p_maxindex = ::std::numeric_limits<uint16_t>::max();"));
  CHECK(contains(out.impl,
                 "if ((p_a == ::std::numeric_limits<uint16_t>::max()))"));
  CHECK(contains(out.impl, "while ((p_tarr::low() <= p_tarr::high()))"));
  CHECK(!contains(out.impl, "p_high(p_a)"));
  CHECK(!contains(out.impl, "p_low(p_arr)"));
  CHECK(!contains(out.impl, "p_high(p_arr)"));
}

void test_char_array_typed_const_uses_explicit_array_literal_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbuf = array[1..4] of char;\n"
      "const\n"
      "  magic : tbuf = 'ABC';\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "inline p_tbuf p_magic = ::rt::tp2cc_array_literal<::rt::p_char, 1, ((4) - (1) + 1)>(::rt::tp2cc_shortstring_literal<255>("));
}

void test_char_array_assignment_uses_explicit_array_literal_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure setbuf;\n"
      "implementation\n"
      "procedure setbuf;\n"
      "var\n"
      "  buf : array[1..4] of char;\n"
      "begin\n"
      "  buf := 'A';\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_buf = ::rt::tp2cc_array_literal<::rt::p_char, 1, ((4) - (1) + 1)>(::rt::tp2cc_char_of('A'));"));
}

void test_nested_array_typed_const_braces_each_array_wrapper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..1, 0..1, 0..3] of byte;\n"
      "const\n"
      "  table : tarr = (\n"
      "    ((1, 2, 3, 4), (5, 6, 7, 8)),\n"
      "    ((9, 10, 11, 12), (13, 14, 15, 16))\n"
      "  );\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "p_table = {{{{{{1, 2, 3, 4}}, {{5, 6, 7, 8}}}}, "
                 "{{{{9, 10, 11, 12}}, {{13, 14, 15, 16}}}}}};"));
}

void test_named_type_alias() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  MyInt = longint;\n"
      "  PInt  = ^longint;\n"
      "  MyStr = string[32];\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using p_myint = int32_t;"));
  CHECK(contains(out.header, "using p_pint = int32_t*;"));
  CHECK(contains(out.header, "using p_mystr = ::rt::tp2cc_ShortString<32>;"));
}

void test_ansistring_builtin_maps_to_runtime_type() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type tname = ansistring;\n"
      "var s : ansistring;\n"
      "procedure take(x : ansistring);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using p_tname = ::rt::tp2cc_AnsiString;"));
  CHECK(contains(out.header, "extern ::rt::tp2cc_AnsiString p_s;"));
  CHECK(contains(out.header, "void p_take(::rt::tp2cc_AnsiString p_x);"));
  CHECK(!contains(out.header, "p_ansistring"));
}

void test_widechar_builtin_maps_to_16bit_ordinal() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type twide = widechar;\n"
      "var w : widechar;\n"
      "procedure take(x : widechar);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using p_twide = uint16_t;"));
  CHECK(contains(out.header, "extern uint16_t p_w;"));
  CHECK(contains(out.header, "void p_take(uint16_t p_x);"));
  CHECK(!contains(out.header, "p_widechar"));
}

void test_set_type_alias() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcolor = (red, green, blue);\n"
      "  tcolors = set of tcolor;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using p_tcolors = ::rt::tp2cc_Set<p_tcolor>;"));
}

void test_var_extern_in_header_and_def_in_impl() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "var g : longint;\n"
      "implementation\n"
      "var h : longint;\n"
      "end.\n");
  CHECK(contains(out.header, "extern int32_t p_g;"));
  CHECK(contains(out.impl, "int32_t p_h;"));
}

void test_out_parameter_emits_like_var_reference() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure fill(out x : integer);\n"
      "implementation\n"
      "procedure fill(out x : integer);\n"
      "begin\n"
      "  x := 1;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_fill(int32_t &p_x);"));
  CHECK(contains(out.impl, "void p_fill(int32_t &p_x) {"));
  CHECK(contains(out.impl, "p_x = 1;"));
}

void test_const_pointer_parameter_stays_value_abi() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure take(const p : pointer);\n"
      "function pick : pointer;\n"
      "implementation\n"
      "procedure take(const p : pointer);\n"
      "begin\n"
      "end;\n"
      "function pick : pointer;\n"
      "begin\n"
      "  pick := nil;\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  take(pick);\n"
      "end;\n"
      "end.\n");
  // Pascal `const p: pointer` is a read-only pointer value, not an alias
  // to the caller's pointer slot. Emit a plain value parameter so calls may
  // pass function results, fields, and other non-lvalues.
  CHECK(contains(out.header, "void p_take(void* p_p);"));
  CHECK(contains(out.impl, "void p_take(void* p_p) {"));
  CHECK(contains(out.impl, "p_take(p_pick());"));
  CHECK(!contains(out.header, "const void* &p_p"));
  CHECK(!contains(out.header, "const void*& p_p"));
  CHECK(!contains(out.impl, "p_take(const "));
}

void test_const_fixed_array_parameter_stays_value_abi() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..3] of longint;\n"
      "  ptarr = ^tarr;\n"
      "procedure take(const a : tarr);\n"
      "implementation\n"
      "procedure take(const a : tarr);\n"
      "var\n"
      "  p : ptarr;\n"
      "begin\n"
      "  p := @a;\n"
      "  if a[0] <> 0 then ;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_take(p_tarr p_a);"));
  CHECK(contains(out.impl, "void p_take(p_tarr p_a) {"));
  CHECK(contains(out.impl, "p_p = (&p_a);"));
  CHECK(contains(out.impl, "if ((p_a[0] != 0))"));
}

void test_const_fixed_record_array_parameter_stays_value_abi() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    x : longint;\n"
      "  end;\n"
      "  tarr = array[0..3] of trec;\n"
      "  ptarr = ^tarr;\n"
      "procedure take(const a : tarr);\n"
      "implementation\n"
      "procedure take(const a : tarr);\n"
      "var\n"
      "  p : ptarr;\n"
      "begin\n"
      "  p := @a;\n"
      "  if a[0].x <> 0 then ;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_take(p_tarr p_a);"));
  CHECK(contains(out.impl, "p_p = (&p_a);"));
  CHECK(contains(out.impl, "if ((p_a[0].p_x != 0))"));
}

void test_const_fixed_classref_array_parameter_stays_value_abi() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class end;\n"
      "  tarr = array[0..3] of tnode;\n"
      "  ptarr = ^tarr;\n"
      "procedure take(const a : tarr);\n"
      "implementation\n"
      "procedure take(const a : tarr);\n"
      "var\n"
      "  p : ptarr;\n"
      "begin\n"
      "  p := @a;\n"
      "  if a[0] <> nil then ;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_take(p_tarr p_a);"));
  CHECK(contains(out.impl, "p_p = (&p_a);"));
  CHECK(contains(out.impl, "if ((p_a[0] != nullptr))"));
}

void test_proc_signature_in_header() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure foo(x : longint; var y : longint);\n"
      "function bar(a, b : longint) : longint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "void p_foo(int32_t p_x, int32_t &p_y);"));
  CHECK(contains(out.header, "int32_t p_bar(int32_t p_a, int32_t p_b);"));
}

void test_typed_array_const() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  sizes : array[0..3] of longint = (1, 2, 4, 8);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_sizes"));
  CHECK(contains(out.header, "{1, 2, 4, 8}"));
}

void test_typed_array_const_with_inline_subrange_element_type() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  reverse_nible : array[0..1] of 0..15 = (%0001, %0010);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_reverse_nible"));
  CHECK(contains(out.header, "{1, 2}"));
}

void test_free_function_trailing_default_argument_is_lowered() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure note(w : integer; code : integer = 7);\n"
      "procedure run;\n"
      "implementation\n"
      "procedure note(w : integer; code : integer);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "begin\n"
      "  note(1);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_note(1, 7);"));
}

void test_method_trailing_default_argument_is_lowered() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tverbose = class\n"
      "    procedure message(w : integer; onqueue : integer = 9);\n"
      "  end;\n"
      "procedure run(verbose : tverbose);\n"
      "implementation\n"
      "procedure tverbose.message(w : integer; onqueue : integer);\n"
      "begin\n"
      "end;\n"
      "procedure run(verbose : tverbose);\n"
      "begin\n"
      "  verbose.message(1);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "->p_message(1, 9);"));
}

void test_singleton_typed_array_const() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  only : array[1..1] of longint = (7);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_only = {7};"));
}

void test_nested_array_type() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  t2d = array[0..1, 0..2] of longint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_Array<::rt::tp2cc_Array<int32_t"));
}

void test_named_subrange_array_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  idx = 1..10;\n"
      "  arr = array[idx] of longint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_arr = ::rt::tp2cc_Array<int32_t, 1, ((10) - (1) + 1)>;"));
}

void test_packed_record_keeps_packed_layout() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  hdr = packed record\n"
      "    tag : byte;\n"
      "    size : longint;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "struct [[gnu::packed]] p_hdr {"));
}

void test_parenthesized_record_const() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  r = record a : longint; end;\n"
      "const\n"
      "  x : r = ((a : 1));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "inline p_r p_x = {.p_a = 1};"));
}

void test_call_arg_literal_is_lowered_to_parameter_value() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure take(x : longint);\n"
      "implementation\n"
      "procedure take(x : longint);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  take($FFFFFFFF);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_take(-1);"));
}

void test_explicit_integer_cast_literal_uses_converted_value() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  s : smallint;\n"
      "  b : byte;\n"
      "begin\n"
      "  s := smallint($8000);\n"
      "  b := byte(-1);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_s = -32768;"));
  CHECK(contains(out.impl, "p_b = 255;"));
}

void test_assignment_const_expr_is_lowered_to_target_value() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  b : byte;\n"
      "begin\n"
      "  b := $4000 - $3F01;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_b = 255;"));
}

void test_implicit_const_expr_wraps_without_emit_error() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  b : byte;\n"
      "begin\n"
      "  b := $4000;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_b = 0;"));
}

void test_explicit_integer_cast_const_expr_wraps_without_error() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  b : byte;\n"
      "begin\n"
      "  b := byte($4000);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_b = 0;"));
}

void test_untyped_integer_const_uses_pascal_initial_type() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  x = $4000 - $3F01;\n"
      "  y = $FFFFFFFF;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  b : byte;\n"
      "begin\n"
      "  b := x;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.header, "const uint8_t p_x = 255;"));
  CHECK(contains(out.header, "const uint32_t p_y = 4294967295;"));
  CHECK(contains(out.impl, "p_b = 255;"));
}

void test_typed_const_read_is_not_folded_through_initializer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  nextlabelnr : longint = 1;\n"
      "procedure bump;\n"
      "implementation\n"
      "procedure bump;\n"
      "begin\n"
      "  nextlabelnr := nextlabelnr + 1;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "int32_t p_nextlabelnr = 1;"));
  CHECK(contains(out.impl, "p_nextlabelnr = (p_nextlabelnr + 1);"));
  CHECK(!contains(out.impl, "p_nextlabelnr = 2;"));
}

void test_exit_literal_uses_function_result_type() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function f : int64;\n"
      "implementation\n"
      "function f : int64;\n"
      "begin\n"
      "  exit(-$8000000000000000);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.header, "#include <limits>"));
  CHECK(contains(out.impl,
                 "p_result = ::std::numeric_limits<int64_t>::min();"));
}

void test_local_result_name_is_rejected_in_function_body() {
  int before = error_count();
  (void)compile_snippet(
      "unit foo;\n"
      "interface\n"
      "implementation\n"
      "function f : integer;\n"
      "var\n"
      "  Result : integer;\n"
      "begin\n"
      "  Result := 3;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() - before > 0);
}

void test_nested_procedure_can_assign_enclosing_result() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function demo : integer;\n"
      "implementation\n"
      "function demo : integer;\n"
      "  procedure setit(v : integer);\n"
      "  begin\n"
      "    Result := v;\n"
      "  end;\n"
      "begin\n"
      "  setit(3);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_setit = [&](int32_t p_v) -> void {"));
  CHECK(contains(out.impl, "p_result = p_v;"));
}

void test_nested_function_uses_own_result_and_outer_name() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function outer : integer;\n"
      "implementation\n"
      "function outer : integer;\n"
      "  function inner : boolean;\n"
      "  begin\n"
      "    Result := false;\n"
      "    outer := 123;\n"
      "  end;\n"
      "begin\n"
      "  inner;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "bool tp2cc_result_p_inner{};"));
  CHECK(contains(out.impl, "tp2cc_result_p_inner = false;"));
  CHECK(contains(out.impl, "p_result = 123;"));
}

void test_try_finally_uses_scope_exit_guard() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "var x : integer;\n"
      "procedure demo;\n"
      "begin\n"
      "  try\n"
      "    x := 1;\n"
      "  finally\n"
      "    x := 2;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "auto tp2cc_finally_1 = ::rt::tp2cc_make_scope_exit([&]() {"));
  CHECK(contains(out.impl, "p_x = 1;"));
  CHECK(contains(out.impl, "p_x = 2;"));
}

void test_try_except_raises_and_matches_exception_class() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  efoo = class(exception)\n"
      "  end;\n"
      "function demo : boolean;\n"
      "implementation\n"
      "function demo : boolean;\n"
      "begin\n"
      "  try\n"
      "    raise efoo.create;\n"
      "  except\n"
      "    on e : efoo do\n"
      "      Result := e <> nil;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "throw ([&]{ auto tp2cc_ptr = new p_efoo{};"));
  CHECK(contains(out.impl, "} catch (::rt::p_exception* tp2cc_exc_1) {"));
  CHECK(contains(out.impl,
                 "dynamic_cast<p_efoo*>(tp2cc_exc_1); tp2cc_match_1_0) {"));
  CHECK(contains(out.impl, "auto p_e = tp2cc_match_1_0;"));
}

void test_try_except_multiple_handlers_start_with_if_and_base_pointer_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  efoo = class(exception)\n"
      "  end;\n"
      "  ebar = class(exception)\n"
      "  end;\n"
      "function demo : boolean;\n"
      "implementation\n"
      "function demo : boolean;\n"
      "begin\n"
      "  try\n"
      "    raise efoo.create;\n"
      "  except\n"
      "    on a : efoo do\n"
      "      Result := a <> nil;\n"
      "    on b : exception do\n"
      "      Result := b <> nil;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "if (auto tp2cc_match_1_0 = dynamic_cast<p_efoo*>(tp2cc_exc_1); tp2cc_match_1_0) {"));
  CHECK(contains(out.impl, "else if (auto tp2cc_match_1_1 = dynamic_cast<::rt::p_exception*>(tp2cc_exc_1); tp2cc_match_1_1) {"));
  CHECK(!contains(out.impl, "bool tp2cc_handled_1 = false;\n      else if"));
}

void test_char_plus_cast_uses_string_concat() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  s = #1 + char(byte(66));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_shortstring_of<>(::rt::tp2cc_char_of('\\x01'))"));
  CHECK(!contains(out.header, "'\\x01' + (("));
}

void test_nul_char_plus_cast_uses_string_concat() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  s = #0 + char(byte(66));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_shortstring_of<>(::rt::tp2cc_char_of('\\0'))"));
  CHECK(!contains(out.header, "::rt::tp2cc_shortstring_of<>(\"\\0\")"));
}

void test_embedded_nul_string_literal_uses_explicit_length_builder() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  S = #141#180#38#0#0#0#0;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_s = ::rt::tp2cc_shortstring_literal<255>("));
  CHECK(contains(out.header, "::rt::tp2cc_char_of('\\0')"));
  CHECK(!contains(out.header, "p_s = ::rt::tp2cc_ShortString<>(\""));
}

void test_shortstring_assignment_uses_pascal_string_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pstring = ^string;\n"
      "procedure store_string(const s : string; var p : pstring);\n"
      "implementation\n"
      "procedure store_string(const s : string; var p : pstring);\n"
      "begin\n"
      "  getmem(p, length(s) + 1);\n"
      "  p^ := s;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_shortstring_assign(::rt::tp2cc_deref(p_p), p_s);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref(p_p) = p_s;"));
}

void test_typed_const_shortstring_literals_use_target_capacity() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "const tokenlenmax = 14;\n"
      "const emptytok : string[tokenlenmax] = '';\n"
      "const plustok : string[tokenlenmax] = '+';\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "::rt::tp2cc_ShortString<::rt::p_tokenlenmax> p_emptytok = "
                 "::rt::tp2cc_shortstring_literal<::rt::p_tokenlenmax>();"));
  CHECK(contains(out.header,
                 "::rt::tp2cc_ShortString<::rt::p_tokenlenmax> p_plustok = "
                 "::rt::tp2cc_shortstring_of<::rt::p_tokenlenmax>(::rt::tp2cc_char_of('+'));"));
  CHECK(!contains(out.header,
                  "::rt::tp2cc_ShortString<::rt::p_tokenlenmax> p_plustok = "
                  "::rt::tp2cc_char_of('+');"));
}

void test_var_shortstring_call_keeps_lvalue_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure replace(var s : string);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure replace(var s : string);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var s : string;\n"
      "begin\n"
      "  replace(s);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_replace(p_s);"));
  CHECK(!contains(out.impl, "p_replace(::rt::tp2cc_shortstring_of"));
  CHECK(!contains(out.impl, "p_replace(::rt::tp2cc_ansistring_of"));
}

void test_var_ansistring_call_keeps_lvalue_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure replace(var s : ansistring);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure replace(var s : ansistring);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var s : ansistring;\n"
      "begin\n"
      "  replace(s);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_replace(p_s);"));
  CHECK(!contains(out.impl, "p_replace(::rt::tp2cc_ansistring_of"));
}

void test_overloaded_string_and_bool_call_keeps_boolean_argument_raw() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tpath = class\n"
      "    procedure addpath(s : string; addfirst : boolean); overload;\n"
      "    procedure addpath(srcpath, s : string; addfirst : boolean); overload;\n"
      "  end;\n"
      "procedure demo(p : tpath; s : string);\n"
      "implementation\n"
      "procedure tpath.addpath(s : string; addfirst : boolean);\n"
      "begin\n"
      "end;\n"
      "procedure tpath.addpath(srcpath, s : string; addfirst : boolean);\n"
      "begin\n"
      "end;\n"
      "procedure demo(p : tpath; s : string);\n"
      "begin\n"
      "  p.addpath(s, false);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p->p_addpath(p_s, false);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_shortstring_of<255>(false)"));
}

void test_pchar_cast_argument_converts_to_string_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure take(const s : string);\n"
      "procedure demo(p : pchar);\n"
      "implementation\n"
      "procedure take(const s : string);\n"
      "begin\n"
      "end;\n"
      "procedure demo(p : pchar);\n"
      "begin\n"
      "  take(pchar(p));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_take(::rt::tp2cc_shortstring_of<255>(((::rt::p_char*)(p_p))));"));
}

void test_integer_and_or_stays_bitwise() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  IF_SM = 1;\n"
      "  IF_SM2 = 2;\n"
      "procedure check(flags : longint);\n"
      "implementation\n"
      "procedure check(flags : longint);\n"
      "begin\n"
      "  if (flags and (IF_SM or IF_SM2)) <> 0 then writeln(flags);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "(p_flags & (p_if_sm | p_if_sm2)) != 0"));
  CHECK(!contains(out.impl, "(p_flags && (p_if_sm || p_if_sm2)) != 0"));
}

void test_nested_boolean_function_and_short_circuits() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure outer(x : longint);\n"
      "implementation\n"
      "procedure outer(x : longint);\n"
      "  function ready(v : longint) : boolean;\n"
      "  begin\n"
      "    ready := v > 0;\n"
      "  end;\n"
      "begin\n"
      "  if (x < 10) and ready(x) then writeln(x);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "((p_x < 10) && p_ready(p_x))"));
  CHECK(!contains(out.impl, "((p_x < 10) & p_ready(p_x))"));
}

void test_nested_untyped_var_forwarding_stays_pointer_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure inner(var x);\n"
      "procedure outer(var y);\n"
      "implementation\n"
      "procedure inner(var x);\n"
      "begin\n"
      "end;\n"
      "procedure outer(var y);\n"
      "begin\n"
      "  inner(y);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_inner(p_y);"));
  CHECK(!contains(out.impl, "p_inner(((void*)&(p_y)))"));
}

void test_untyped_method_call_on_variable_uses_storage_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tstream = object\n"
      "    function read(var buffer; count : longint) : longint;\n"
      "    function copyfrom(source : tstream; count : longint) : longint;\n"
      "  end;\n"
      "implementation\n"
      "function tstream.read(var buffer; count : longint) : longint;\n"
      "begin\n"
      "end;\n"
      "function tstream.copyfrom(source : tstream; count : longint) : longint;\n"
      "var\n"
      "  buffer : array[0..3] of byte;\n"
      "begin\n"
      "  copyfrom := source.read(buffer, count);\n"
      "end;\n"
      "end.\n");
  // `var buffer` passes the storage address of the local array value.
  // The receiver being a variable (`source.read`) must not lose that
  // metadata and accidentally pass the whole value expression instead.
  CHECK(contains(out.impl, "p_result = p_source.p_read(((void*)&(p_buffer)), p_count);"));
  CHECK(!contains(out.impl, "p_source.p_read(p_buffer, p_count);"));
}

void test_fillchar_uses_storage_address_for_pointer_slots() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure zero(capacity : longint);\n"
      "implementation\n"
      "procedure zero(capacity : longint);\n"
      "var\n"
      "  list : array[0..3] of pointer;\n"
      "begin\n"
      "  fillchar(list[capacity], (4 - capacity) * sizeof(pointer), 0);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_fillchar(((void*)&(p_list[p_capacity]))"));
  CHECK(!contains(out.impl, "p_fillchar(p_list[p_capacity],"));
}

void test_move_uses_storage_addresses_for_source_and_destination_slots() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure slide(index, count : longint);\n"
      "implementation\n"
      "procedure slide(index, count : longint);\n"
      "var\n"
      "  list : array[0..3] of pointer;\n"
      "begin\n"
      "  system.move(list[index + 1], list[index],\n"
      "              (count - index) * sizeof(pointer));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_move(((void*)&(p_list[(p_index + 1)]))"));
  CHECK(contains(out.impl, "((void*)&(p_list[p_index]))"));
  CHECK(!contains(out.impl, "::rt::p_move(p_list[(p_index + 1)],"));
}

void test_byte_array_typecast_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  t80bitarray = array[0..9] of byte;\n"
      "procedure dump(e : extended; i : longint);\n"
      "implementation\n"
      "procedure dump(e : extended; i : longint);\n"
      "begin\n"
      "  writeln(t80bitarray(e)[i]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<p_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "p_t80bitarray(p_e)[p_i]"));
}

void test_local_byte_array_typecast_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure dump(e : extended; i : longint);\n"
      "implementation\n"
      "procedure dump(e : extended; i : longint);\n"
      "type\n"
      "  t80bitarray = array[0..9] of byte;\n"
      "begin\n"
      "  writeln(t80bitarray(e)[i]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<p_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "p_t80bitarray(p_e)[p_i]"));
}

void test_visible_pointer_alias_cast_uses_qualified_type_spelling() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses widestr;\n"
      "procedure demo(raw : pointer);\n"
      "implementation\n"
      "procedure demo(raw : pointer);\n"
      "begin\n"
      "  widestr.copywidestring(widestr.pcompilerwidestring(raw),\n"
      "                         widestr.pcompilerwidestring(raw));\n"
      "end;\n"
      "end.\n",
      {{"widestr.pas",
        "unit widestr;\n"
        "interface\n"
        "type\n"
        "  compilerwidestring = array[0..3] of widechar;\n"
        "  pcompilerwidestring = ^compilerwidestring;\n"
        "procedure copywidestring(src, dst : pcompilerwidestring);\n"
        "implementation\n"
        "procedure copywidestring(src, dst : pcompilerwidestring);\n"
        "begin\n"
        "end;\n"
        "end.\n"}});
  CHECK(contains(out.impl,
                 "((p_widestr::p_pcompilerwidestring)(p_raw))"));
  CHECK(!contains(out.impl,
                  "((p_pcompilerwidestring)(p_raw))"));
}

void test_local_pointer_alias_cast_uses_local_type_spelling() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(raw : pointer);\n"
      "implementation\n"
      "procedure demo(raw : pointer);\n"
      "type\n"
      "  setbytes = array[0..31] of byte;\n"
      "  psetbytes = ^setbytes;\n"
      "begin\n"
      "  writeln(psetbytes(raw)^[0]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_deref(((p_psetbytes)(p_raw)))[0]"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref(::rt::tp2cc_reinterpret_storage_ref<p_psetbytes>(p_raw))[0]"));
}

void test_runtime_alias_type_names_are_explicitly_qualified() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses dos;\n"
      "var\n"
      "  d : dirstr;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  dt : datetime;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "extern ::rt::p_dirstr p_d;"));
  CHECK(!contains(out.header, "extern p_dirstr p_d;"));
  CHECK(contains(out.impl, "::rt::p_datetime p_dt{};"));
}

void test_charset_stub_type_names_are_explicitly_qualified() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses charset;\n"
      "var\n"
      "  m : punicodemap;\n"
      "  rec : tunicodemap;\n"
      "  item : tunicodecharmapping;\n"
      "  flag : tunicodecharmappingflag;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  flag := umf_noinfo;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "extern ::rt::p_punicodemap p_m;"));
  CHECK(contains(out.header, "extern ::rt::p_tunicodemap p_rec;"));
  CHECK(contains(out.header, "extern ::rt::p_tunicodecharmapping p_item;"));
  CHECK(contains(out.header, "extern ::rt::p_tunicodecharmappingflag p_flag;"));
  CHECK(contains(out.impl, "p_flag = ::rt::p_umf_noinfo;"));
}

void test_tmethod_type_name_is_explicitly_qualified() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  m : tmethod;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_tmethod p_m{};"));
  CHECK(!contains(out.impl, "\n  p_tmethod p_m{};"));
}

void test_local_enum_members_do_not_fall_back_to_runtime() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(i : longint);\n"
      "implementation\n"
      "procedure demo(i : longint);\n"
      "type\n"
      "  leftright = (left, right);\n"
      "var\n"
      "  lr : leftright;\n"
      "begin\n"
      "  if i = 0 then\n"
      "    lr := right\n"
      "  else\n"
      "    lr := left;\n"
      "  writeln(ord(lr));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_lr = p_right;"));
  CHECK(contains(out.impl, "p_lr = p_left;"));
  CHECK(!contains(out.impl, "::rt::p_right"));
  CHECK(!contains(out.impl, "::rt::p_left"));
}

void test_sizeof_visible_type_uses_type_spelling_not_identifier_lookup() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  aint = longint;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  writeln(sizeof(aint));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "sizeof(p_aint)"));
  CHECK(!contains(out.impl, "sizeof(::rt::p_aint)"));
}

void test_primitive_cast_assign_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure fill(var b);\n"
      "implementation\n"
      "procedure fill(var b);\n"
      "var\n"
      "  l : longint;\n"
      "begin\n"
      "  longint(b) := l;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_store<int32_t>(p_b, p_l);"));
  CHECK(!contains(out.impl, "p_b = ((int32_t)(p_l));"));
}

void test_primitive_cast_read_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure fetch(var b);\n"
      "implementation\n"
      "procedure fetch(var b);\n"
      "var\n"
      "  l : longint;\n"
      "begin\n"
      "  l := longint(b);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_l = ::rt::tp2cc_reinterpret_load<int32_t>(p_b);"));
  CHECK(!contains(out.impl, "p_l = ((int32_t)(p_b));"));
}

void test_inc_untyped_primitive_cast_reinterprets_storage_by_byte_copy() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure step(var b);\n"
      "implementation\n"
      "procedure step(var b);\n"
      "begin\n"
      "  inc(longint(b));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_inc<int32_t>(p_b)"));
}

void test_inc_primitive_cast_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure step(p : pchar);\n"
      "implementation\n"
      "procedure step(p : pchar);\n"
      "begin\n"
      "  inc(longint(p));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_inc(::rt::tp2cc_reinterpret_storage_ref<int32_t>(p_p))"));
  CHECK(!contains(out.impl, "p_p = ((int32_t)(p_p) + 1)"));
}

void test_untyped_array_view_index_uses_byte_load_store() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..7] of longint;\n"
      "procedure poke(var b; i : longint);\n"
      "implementation\n"
      "procedure poke(var b; i : longint);\n"
      "begin\n"
      "  tarr(b)[i] := tarr(b)[i];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_load<int32_t>(::rt::tp2cc_byte_offset(p_b, ((p_i) - (0)) * sizeof(int32_t)))"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<int32_t>(::rt::tp2cc_byte_offset(p_b, ((p_i) - (0)) * sizeof(int32_t)), "));
}

void test_aggregate_to_primitive_cast_reinterprets_bytes() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tdoublearray = array[0..7] of byte;\n"
      "const\n"
      "  bits : tdoublearray = (0,0,0,0,0,0,240,127);\n"
      "procedure fetch;\n"
      "implementation\n"
      "procedure fetch;\n"
      "var\n"
      "  d : double;\n"
      "begin\n"
      "  d := double(bits);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_d = ::rt::tp2cc_reinterpret_copy<double>(p_bits);"));
  CHECK(!contains(out.impl, "p_d = ((double)(p_bits));"));
}

void test_absolute_pointer_target_reinterprets_pointee_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = record\n"
      "    value : longint;\n"
      "  end;\n"
      "procedure demo(raw : pointer);\n"
      "implementation\n"
      "procedure demo(raw : pointer);\n"
      "var\n"
      "  item : titem absolute raw;\n"
      "begin\n"
      "  writeln(item.value);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_titem &p_item = ::rt::tp2cc_reinterpret_ref<p_titem>(p_raw);"));
}

void test_absolute_pointer_alias_reinterprets_pointer_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pint = ^longint;\n"
      "procedure demo(raw : pointer);\n"
      "implementation\n"
      "procedure demo(raw : pointer);\n"
      "var\n"
      "  value : pint absolute raw;\n"
      "begin\n"
      "  writeln(value^);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_pint &p_value = ::rt::tp2cc_reinterpret_storage_ref<p_pint>(p_raw);"));
}

void test_pointer_alias_cast_on_pointer_expression_uses_plain_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "type\n"
      "  plongint = ^longint;\n"
      "var\n"
      "  raw : array[0..7] of byte;\n"
      "  i : longint;\n"
      "  l : longint;\n"
      "begin\n"
      "  l := plongint(@raw[i*4])^;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "((p_plongint)("));
  CHECK(!contains(out.impl, "::rt::p_plongint("));
}

void test_absolute_typed_const_alias_reinterprets_same_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "type\n"
      "  pint = ^longint;\n"
      "  parr = array[0..1] of pint;\n"
      "const\n"
      "  raw : array[0..1] of pointer = (nil, nil);\n"
      "var\n"
      "  view : parr absolute raw;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_parr &p_view = ::rt::tp2cc_reinterpret_storage_ref<p_parr>(p_raw);"));
}

void test_absolute_const_param_alias_stays_const_reference() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(const s : string);\n"
      "implementation\n"
      "procedure demo(const s : string);\n"
      "var\n"
      "  view : string absolute s;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "const ::rt::tp2cc_ShortString<>& p_view = "
                 "::rt::tp2cc_reinterpret_storage_ref<::rt::tp2cc_ShortString<>>(p_s);"));
}

void test_property_getter_setter_lowering() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlist = class\n"
      "    function getcount : longint;\n"
      "    procedure setcount(value : longint);\n"
      "    property count : longint read getcount write setcount;\n"
      "  end;\n"
      "procedure demo(lst : tlist);\n"
      "implementation\n"
      "function tlist.getcount : longint;\n"
      "begin\n"
      "  getcount := 0;\n"
      "end;\n"
      "procedure tlist.setcount(value : longint);\n"
      "begin\n"
      "end;\n"
      "procedure demo(lst : tlist);\n"
      "var\n"
      "  n : longint;\n"
      "begin\n"
      "  n := lst.count;\n"
      "  lst.count := 3;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_n = p_lst->p_getcount();"));
  CHECK(contains(out.impl, "p_lst->p_setcount(3);"));
  CHECK(!contains(out.header, " p_count("));
  CHECK(!contains(out.header, " p_count;"));
}

void test_property_field_and_default_index_lowering() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlist = class\n"
      "  private\n"
      "    fsize : longint;\n"
      "    function get(index : longint) : pointer;\n"
      "    procedure put(index : longint; value : pointer);\n"
      "  public\n"
      "    property size : longint read fsize write fsize;\n"
      "    property items[index : longint] : pointer read get write put; default;\n"
      "  end;\n"
      "procedure demo(lst : tlist; p : pointer);\n"
      "implementation\n"
      "function tlist.get(index : longint) : pointer;\n"
      "begin\n"
      "  get := nil;\n"
      "end;\n"
      "procedure tlist.put(index : longint; value : pointer);\n"
      "begin\n"
      "end;\n"
      "procedure demo(lst : tlist; p : pointer);\n"
      "begin\n"
      "  lst.size := 1;\n"
      "  p := lst[1];\n"
      "  lst[2] := p;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_lst->p_fsize = 1;"));
  CHECK(contains(out.impl, "p_p = p_lst->p_get(1);"));
  CHECK(contains(out.impl, "p_lst->p_put(2, p_p);"));
}

void test_property_result_default_index_read_write_lowering() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titems = class\n"
      "    function get(index : longint) : pointer;\n"
      "    procedure put(index : longint; value : pointer);\n"
      "    property items[index : longint] : pointer read get write put; default;\n"
      "  end;\n"
      "  tbox = class\n"
      "  private\n"
      "    flist : titems;\n"
      "  public\n"
      "    property list : titems read flist;\n"
      "  end;\n"
      "procedure demo(box : tbox; p : pointer);\n"
      "implementation\n"
      "function titems.get(index : longint) : pointer;\n"
      "begin\n"
      "  get := nil;\n"
      "end;\n"
      "procedure titems.put(index : longint; value : pointer);\n"
      "begin\n"
      "end;\n"
      "procedure demo(box : tbox; p : pointer);\n"
      "begin\n"
      "  p := box.list[1];\n"
      "  box.list[2] := p;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = p_box->p_flist->p_get(1);"));
  CHECK(contains(out.impl, "p_box->p_flist->p_put(2, p_p);"));
  CHECK(!contains(out.impl, "unsupported property write accessor"));
}

void test_implicit_property_lookup_in_method_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  private\n"
      "    fcount : longint;\n"
      "    function getname : string;\n"
      "  public\n"
      "    property count : longint read fcount write fcount;\n"
      "    property name : string read getname;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure bump;\n"
      "  end;\n"
      "implementation\n"
      "function tbase.getname : string;\n"
      "begin\n"
      "  getname := '';\n"
      "end;\n"
      "procedure tchild.bump;\n"
      "begin\n"
      "  count := count + 1;\n"
      "  if name <> '' then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "this->p_fcount = (this->p_fcount + 1);"));
  CHECK(contains(out.impl, "if ((this->p_getname() != ::rt::tp2cc_shortstring_literal<255>()))"));
  CHECK(!contains(out.impl, "p_count ="));
  CHECK(!contains(out.impl, "p_name !="));
}

void test_procvar_property_stmt_and_value_context() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tproc = procedure;\n"
      "  tbox = class\n"
      "  private\n"
      "    ffoo : tproc;\n"
      "    function getfoo : tproc;\n"
      "  public\n"
      "    property foo : tproc read getfoo write ffoo;\n"
      "  end;\n"
      "procedure demo(box : tbox; p : tproc);\n"
      "implementation\n"
      "function tbox.getfoo : tproc;\n"
      "begin\n"
      "  getfoo := ffoo;\n"
      "end;\n"
      "procedure demo(box : tbox; p : tproc);\n"
      "begin\n"
      "  box.foo;\n"
      "  box.foo();\n"
      "  p := box.foo;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_box->p_getfoo()();"));
  CHECK(contains(out.impl, "p_p = p_box->p_getfoo();"));
  CHECK(!contains(out.impl, "p_p = p_box->p_getfoo()();"));
}

void test_default_indexed_procvar_property_stmt_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tproc = procedure;\n"
      "  tbox = class\n"
      "    function getitem(index : longint) : tproc;\n"
      "    property items[index : longint] : tproc read getitem; default;\n"
      "  end;\n"
      "procedure demo(box : tbox);\n"
      "implementation\n"
      "function tbox.getitem(index : longint) : tproc;\n"
      "begin\n"
      "  getitem := nil;\n"
      "end;\n"
      "procedure demo(box : tbox);\n"
      "begin\n"
      "  box[1];\n"
      "  box[2]();\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_box->p_getitem(1)();"));
  CHECK(contains(out.impl, "p_box->p_getitem(2)();"));
}

void test_class_method_static_emission_and_calls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tx = class\n"
      "    class function bar : integer;\n"
      "    function foo : integer;\n"
      "  end;\n"
      "procedure demo(x : tx);\n"
      "implementation\n"
      "class function tx.bar : integer;\n"
      "begin\n"
      "  bar := 7;\n"
      "end;\n"
      "function tx.foo : integer;\n"
      "begin\n"
      "  foo := bar;\n"
      "end;\n"
      "procedure demo(x : tx);\n"
      "begin\n"
      "  x.bar;\n"
      "  tx.bar;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "static int32_t p_bar();"));
  CHECK(contains(out.impl, "int32_t p_tx::p_bar() {"));
  CHECK(contains(out.impl, "p_result = p_bar();"));
  CHECK(contains(out.impl, "p_x->p_bar();"));
  CHECK(contains(out.impl, "p_tx::p_bar();"));
}

void test_tobject_runtime_helpers_lower_in_method_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class\n"
      "    function metaptr : pointer;\n"
      "    function bytes : integer;\n"
      "  end;\n"
      "implementation\n"
      "function titem.metaptr : pointer;\n"
      "begin\n"
      "  metaptr := classtype;\n"
      "end;\n"
      "function titem.bytes : integer;\n"
      "begin\n"
      "  bytes := instancesize;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "virtual ::rt::p_tclass p_classtype() const override;"));
  CHECK(contains(out.header, "virtual int32_t p_instancesize() const override;"));
  CHECK(contains(out.header, "inline ::rt::p_tclass p_titem::p_classtype() const {"));
  CHECK(contains(out.header, "inline int32_t p_titem::p_instancesize() const {"));
  CHECK(contains(out.impl, "p_result = p_classtype();"));
  CHECK(contains(out.impl, "p_result = p_instancesize();"));
}

void test_tobject_cast_preserves_pointer_semantics_for_free() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = class\n"
      "    p : pointer;\n"
      "    destructor destroy; override;\n"
      "  end;\n"
      "implementation\n"
      "destructor tbox.destroy;\n"
      "begin\n"
      "  if assigned(p) then\n"
      "    tobject(p).free;\n"
      "  inherited destroy;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_tobject::p_free(((::rt::p_tobject*)(p_p)));"));
  CHECK(!contains(out.impl, "::rt::p_tobject(p_p)"));
}

void test_parameterless_proc_assignment_keeps_designator() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tproc = procedure;\n"
      "var\n"
      "  p : tproc;\n"
      "procedure foo;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure foo;\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  p := foo;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = p_foo;"));
  CHECK(!contains(out.impl, "p_p = p_foo();"));
}

void test_method_pointer_type_and_bound_assignment_emit() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcb = procedure(x : integer) of object;\n"
      "  tobj = object\n"
      "    cb : tcb;\n"
      "    procedure fire(x : integer);\n"
      "    procedure setcb;\n"
      "  end;\n"
      "implementation\n"
      "procedure tobj.fire(x : integer);\n"
      "begin\n"
      "end;\n"
      "procedure tobj.setcb;\n"
      "begin\n"
      "  cb := fire;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "using p_tcb = ::rt::tp2cc_MethodPtr<void(int32_t)>;"));
  CHECK(contains(out.header,
                 "static void tp2cc_methodptr_fire_value_name_integer_ret_void"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_method_code<&p_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
  CHECK(contains(out.impl, "p_cb = p_tcb("));
}

void test_method_pointer_record_cast_reinterprets_same_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcb = procedure(x : integer) of object;\n"
      "  trec = record\n"
      "    procpointer : pointer;\n"
      "    s : pointer;\n"
      "  end;\n"
      "procedure setslots(var p : tcb; addr : pointer; selfp : pointer);\n"
      "implementation\n"
      "procedure setslots(var p : tcb; addr : pointer; selfp : pointer);\n"
      "begin\n"
      "  trec(p).procpointer := addr;\n"
      "  trec(p).s := selfp;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<p_trec>(p_p).p_procpointer = p_addr;"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<p_trec>(p_p).p_s = p_selfp;"));
}

void test_unbound_method_address_uses_thunk_code() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobj = object\n"
      "    procedure fire(x : integer);\n"
      "  end;\n"
      "const\n"
      "  addr : pointer = @tobj.fire;\n"
      "implementation\n"
      "procedure tobj.fire(x : integer);\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "p_addr = ::rt::tp2cc_method_code<&p_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
}

void test_internal_helpers_avoid_double_underscores() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcolor = (red, green);\n"
      "  tcb = procedure(x : integer) of object;\n"
      "  tobj = object\n"
      "    cb : tcb;\n"
      "    procedure fire(x : integer);\n"
      "    procedure run(var i : integer);\n"
      "  end;\n"
      "implementation\n"
      "procedure tobj.fire(x : integer);\n"
      "begin\n"
      "end;\n"
      "procedure tobj.run(var i : integer);\n"
      "begin\n"
      "  with self do cb := fire;\n"
      "  for i := low(tcolor) to high(tcolor) do begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.header, "__"));
  CHECK(!contains(out.impl, "__"));
}

void test_for_loop_uses_resolved_global_control_var() {
  auto out = compile_snippet_with_registry(
      "unit symtable;\n"
      "interface\n"
      "uses globals;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  for token := first_overloaded to last_overloaded do begin end;\n"
      "end;\n"
      "end.\n",
      {{"globals.pas",
        "unit globals;\n"
        "interface\n"
        "type\n"
        "  ttoken = (_plus, _assignment);\n"
        "var\n"
        "  token : ttoken;\n"
        "const\n"
        "  first_overloaded = _plus;\n"
        "  last_overloaded = _assignment;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "p_globals::p_token = tp2cc_from;"));
  CHECK(contains(out.impl, "if (p_globals::p_token == tp2cc_to) break;"));
  CHECK(contains(out.impl, "::rt::p_inc(p_globals::p_token);"));
}

void test_const_object_param_uses_mutable_ref() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobj = object\n"
      "    n : longint;\n"
      "  end;\n"
      "procedure take(const x : tobj);\n"
      "implementation\n"
      "procedure take(const x : tobj);\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_take(p_tobj &p_x);"));
  CHECK(!contains(out.header, "void p_take(p_tobj const &p_x);"));
}

void test_parameterless_procvar_stmt_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tstop = procedure;\n"
      "var\n"
      "  oldstop : tstop;\n"
      "procedure kick;\n"
      "implementation\n"
      "procedure kick;\n"
      "begin\n"
      "  oldstop;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_oldstop();"));
}

void test_direct_procvar_var_decl_uses_named_function_pointer_syntax() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  hook : procedure(i : longint);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "extern void (*p_hook)(int32_t);"));
}

void test_runtime_builtin_stmt_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  swapvectors;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_swapvectors();"));
}

void test_bool_procvar_call_uses_logical_and() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tpred = function : boolean;\n"
      "var\n"
      "  pred : tpred;\n"
      "function go : boolean;\n"
      "implementation\n"
      "function go : boolean;\n"
      "begin\n"
      "  go := pred() and true;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_pred() && true"));
  CHECK(!contains(out.impl, "p_pred() & true"));
}

void test_open_array_method_signature_keeps_wrapper_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tmsg = object\n"
      "    function get(nr : integer; const args : array of string) : string;\n"
      "  end;\n"
      "implementation\n"
      "function tmsg.get(nr : integer; const args : array of string) : string;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  // Open arrays carry pointer-plus-length state. Once the emitter picks
  // `tp2cc_OpenArray<T>` for a parameter, it must keep that exact C++ carrier
  // type instead of falling back to the generic `array of T -> T*` path.
  CHECK(contains(out.header,
                 "::rt::tp2cc_ShortString<> p_get(int32_t p_nr, ::rt::tp2cc_OpenArray<::rt::tp2cc_ShortString<>> p_args);"));
  CHECK(contains(out.header,
                 "static ::rt::tp2cc_ShortString<> tp2cc_methodptr_get_value_name_integer_const_openarr_string_ret_string(void* tp2cc_self, int32_t p_nr, ::rt::tp2cc_OpenArray<::rt::tp2cc_ShortString<>> p_args)"));
}

void test_open_array_procvar_signature_keeps_wrapper_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcb = procedure(const args : array of string);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_tcb = void (*)(::rt::tp2cc_OpenArray<::rt::tp2cc_ShortString<>>);"));
}

void test_open_array_call_uses_owning_temporary_wrapper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure log(const values : array of word);\n"
      "implementation\n"
      "procedure log(const values : array of word);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  log([1, 2, 3]);\n"
      "  log([]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_log(::rt::tp2cc_open_array_of<uint16_t>(1, 2, 3))"));
  CHECK(contains(out.impl, "p_log(::rt::tp2cc_open_array<uint16_t>())"));
}

void test_high_low_on_open_array_use_runtime_length() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(const xs : array of longint);\n"
      "implementation\n"
      "procedure demo(const xs : array of longint);\n"
      "begin\n"
      "  if high(xs) = 1 then begin end;\n"
      "  if low(xs) = 0 then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_length(p_xs) - 1"));
  CHECK(!contains(out.impl, "int32_t*::high()"));
}

void test_dynamic_array_type_uses_runtime_carrier() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tints = array of longint;\n"
      "procedure demo(const xs : array of longint; ys : tints);\n"
      "implementation\n"
      "procedure demo(const xs : array of longint; ys : tints);\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_tints = ::rt::tp2cc_DynArray<int32_t>;"));
  CHECK(contains(out.header,
                 "void p_demo(::rt::tp2cc_OpenArray<int32_t> p_xs, p_tints p_ys);"));
}

void test_dynamic_array_actual_converts_to_open_array_view() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tints = array of longint;\n"
      "procedure log(const xs : array of longint);\n"
      "implementation\n"
      "procedure log(const xs : array of longint);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var\n"
      "  ys : tints;\n"
      "begin\n"
      "  setlength(ys, 2);\n"
      "  log(ys);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_log(::rt::tp2cc_open_array<int32_t>(p_ys))"));
}

void test_memory_helpers_reinterpret_typecast_pointer_slots() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "type\n"
      "  pdata = ^byte;\n"
      "procedure demo;\n"
      "var\n"
      "  raw : pointer;\n"
      "begin\n"
      "  getmem(pdata(raw), 4);\n"
      "  freemem(pdata(raw), 4);\n"
      "  dispose(pdata(raw));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_getmem(::rt::tp2cc_reinterpret_storage_ref<p_pdata>(p_raw), 4);"));
  CHECK(contains(out.impl,
                 "::rt::p_freemem(::rt::tp2cc_reinterpret_storage_ref<p_pdata>(p_raw), 4);"));
  CHECK(contains(out.impl,
                 "::rt::p_dispose(::rt::tp2cc_reinterpret_storage_ref<p_pdata>(p_raw));"));
}

void test_unit_local_enum_array_bounds_win_over_unrelated_same_name_types() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tasmtoken = (as_none, as_xor);\n"
      "const\n"
      "  token2str : array[tasmtoken] of integer = (1, 2);\n"
      "implementation\n"
      "end.\n",
      {{"other.pas",
        "unit other;\n"
        "interface\n"
        "type\n"
        "  tasmtoken = (as_none, as_or, as_xor);\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.header,
                 "::rt::tp2cc_Array<int32_t, 0, 2> p_token2str"));
  CHECK(!contains(out.header, "p_other::p_as_none"));
}

void test_typed_set_literal_uses_surrounding_set_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tset = set of 0..7;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  s : tset;\n"
      "begin\n"
      "  s := s + [3];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_Set<int32_t>::from_list({3})"));
}

void test_explicit_set_cast_uses_runtime_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsmall = set of 0..7;\n"
      "  tbyte = set of byte;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  small : tsmall;\n"
      "begin\n"
      "  if tbyte(small) = [] then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_set_cast<::rt::tp2cc_Set<uint8_t>>(p_small)"));
}

void test_set_range_literal_uses_integer_ordinal_loop() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  if 'm' in ['a'..'z'] then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "for (int64_t tp2cc_value = (int64_t)(::rt::tp2cc_char_of('a'))"));
  CHECK(!contains(out.impl, "++tp2cc_i"));
}

void test_untyped_const_method_thunk_keeps_raw_storage_pointer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tstream = object\n"
      "    function write(const buffer; count : longint) : longint;\n"
      "  end;\n"
      "implementation\n"
      "function tstream.write(const buffer; count : longint) : longint;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  // Untyped Pascal params always mean "address of caller storage". Even
  // `const` keeps that raw-storage ABI; it must not turn into
  // `const void*&` in thunks or procvar signatures.
  CHECK(contains(out.header, "int32_t p_write(void* p_buffer, int32_t p_count);"));
  CHECK(contains(out.header,
                 "static int32_t tp2cc_methodptr_write_const_untyped_value_name_longint_ret_name_longint(void* tp2cc_self, void* p_buffer, int32_t p_count)"));
}

void test_untyped_const_distinguishes_pointer_slot_from_pointed_bytes() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure sink(const b; len : longint);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure sink(const b; len : longint);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var\n"
      "  p : pchar;\n"
      "begin\n"
      "  sink(p, 1);\n"
      "  sink(p^, 1);\n"
      "end;\n"
      "end.\n");
  // Untyped `const` receives the storage address of the actual argument. A
  // pointer variable and the bytes it points to are different storage
  // locations, and byte-buffer writers rely on that distinction.
  CHECK(contains(out.impl, "p_sink(((void*)&(p_p)), 1);"));
  CHECK(contains(out.impl, "p_sink(((void*)&(::rt::tp2cc_deref(p_p))), 1);"));
}

void test_class_types_lower_to_pointers_and_implicit_tobject() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "    destructor destroy; override;\n"
      "  end;\n"
      "var\n"
      "  head : tnode;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "struct p_tnode : public ::rt::p_tobject {"));
  CHECK(contains(out.header, "using inherited = ::rt::p_tobject;"));
  CHECK(contains(out.header, "p_tnode* p_next;"));
  CHECK(contains(out.header, "extern p_tnode* p_head;"));
}

void test_forward_class_decl_only_emits_one_struct_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class;\n"
      "  tbox = class\n"
      "    next : tnode;\n"
      "  end;\n"
      "  tnode = class\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(count_substring(out.header, "struct p_tnode : public ::rt::p_tobject {"),
           1u);
  CHECK(contains(out.header, "p_tnode* p_next;"));
}

void test_empty_inherited_class_decl_emits_real_struct() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  ebase = class\n"
      "  end;\n"
      "  echild = class(ebase);\n"
      "var\n"
      "  child : echild;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "struct p_echild : public p_ebase {"));
  CHECK(contains(out.header, "using inherited = p_ebase;"));
  CHECK(contains(out.header, "extern p_echild* p_child;"));
}

void test_abstract_method_emits_fail_fast_virtual_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit; virtual; abstract;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "virtual void p_doit();"));
  CHECK(!contains(out.header, "virtual void p_doit() = 0;"));
}

void test_pointer_sized_integer_aliases_lower_through_rt() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  a : ptrint;\n"
      "  b : ptruint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "extern ::rt::p_ptrint p_a;"));
  CHECK(contains(out.header, "extern ::rt::p_ptruint p_b;"));
}

void test_tclass_alias_lowers_through_rt() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "function sameclass(c : tclass) : boolean;\n"
      "implementation\n"
      "function sameclass(c : tclass) : boolean;\n"
      "begin\n"
      "  sameclass := c = nil;\n"
      "end.\n"
      "end.\n");
  CHECK(contains(out.header, "bool p_sameclass(::rt::p_tclass p_c);"));
}

void test_class_constructor_call_allocates_instance() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    constructor create;\n"
      "  end;\n"
      "function build : tnode;\n"
      "implementation\n"
      "constructor tnode.create;\n"
      "begin\n"
      "end;\n"
      "function build : tnode;\n"
      "begin\n"
      "  build := tnode.create;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto tp2cc_ptr = new p_tnode{};"));
  CHECK(contains(out.impl, "tp2cc_ptr->p_create();"));
}

void test_class_constructor_trailing_default_argument_is_lowered() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    constructor create(n : integer = 7);\n"
      "  end;\n"
      "function build : tnode;\n"
      "implementation\n"
      "constructor tnode.create(n : integer);\n"
      "begin\n"
      "end;\n"
      "function build : tnode;\n"
      "begin\n"
      "  build := tnode.create;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "tp2cc_ptr->p_create(7);"));
}

void test_object_constructor_call_uses_base_method_on_self() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = object\n"
      "    constructor init(n : integer);\n"
      "  end;\n"
      "  tchild = object(tbase)\n"
      "    constructor init(n : integer);\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.init(n : integer);\n"
      "begin\n"
      "end;\n"
      "constructor tchild.init(n : integer);\n"
      "begin\n"
      "  tbase.init(n);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_tbase::p_init(p_n);"));
  CHECK(!contains(out.impl, "new p_tbase{};"));
}

void test_implicit_tobject_inherited_constructor_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    constructor create;\n"
      "  end;\n"
      "implementation\n"
      "constructor tnode.create;\n"
      "begin\n"
      "  inherited create;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "inherited::p_create();"));
}

void test_inherited_destroy_autocalls_through_non_overriding_parent() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    destructor destroy; override;\n"
      "  end;\n"
      "  tmid = class(tbase)\n"
      "  end;\n"
      "  tleaf = class(tmid)\n"
      "    destructor destroy; override;\n"
      "  end;\n"
      "implementation\n"
      "destructor tbase.destroy;\n"
      "begin\n"
      "end;\n"
      "destructor tleaf.destroy;\n"
      "begin\n"
      "  inherited destroy;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "inherited::p_destroy();"));
}

void test_class_self_and_free_use_pointer_semantics() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "    procedure zap;\n"
      "  end;\n"
      "implementation\n"
      "procedure tnode.zap;\n"
      "begin\n"
      "  self.next.free;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_tobject::p_free(this->p_next);"));
  CHECK(!contains(out.impl, "this->p_next->p_free()"));
}

void test_metaclass_alias_and_concrete_class_value_lowering() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create(n : integer);\n"
      "    class function load(n : integer) : integer;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "var\n"
      "  cls : tbaseclass;\n"
      "  inst : tbase;\n"
      "implementation\n"
      "constructor tbase.create(n : integer);\n"
      "begin\n"
      "end;\n"
      "class function tbase.load(n : integer) : integer;\n"
      "begin\n"
      "  load := n;\n"
      "end;\n"
      "begin\n"
      "  cls := tchild;\n"
      "  inst := cls.create(1);\n"
      "  if assigned(cls) then\n"
      "    inst := cls.create(2);\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using p_tbaseclass = const tp2cc_metaclass_p_tbase*;"));
  CHECK(contains(out.header,
                 "struct tp2cc_metaclass_p_tchild : public tp2cc_metaclass_p_tbase {"));
  CHECK(contains(out.header,
                 "inline const tp2cc_metaclass_p_tchild* tp2cc_metaclass_value_p_tchild() {"));
  CHECK(contains(out.impl, "p_cls = tp2cc_metaclass_value_p_tchild();"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create(1);"));
  CHECK(contains(out.impl, "if (::rt::p_assigned(p_cls))"));
}

void test_metaclass_cast_keeps_concrete_descriptor() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "  tchildclass = class of tchild;\n"
      "var\n"
      "  basecls : tbaseclass;\n"
      "  childcls : tchildclass;\n"
      "implementation\n"
      "constructor tbase.create;\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  basecls := tchild;\n"
      "  childcls := tchildclass(basecls);\n"
      "end.\n");
  CHECK(contains(out.impl, "p_basecls = tp2cc_metaclass_value_p_tchild();"));
  CHECK(contains(out.impl, "p_childcls = ((p_tchildclass)(p_basecls));"));
}

void test_class_identifier_value_lowers_to_metaclass_descriptor() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "  end;\n"
      "function ischild(x : tbase) : boolean;\n"
      "implementation\n"
      "function ischild(x : tbase) : boolean;\n"
      "begin\n"
      "  ischild := x.classtype = tchild;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = (p_x->p_classtype() == tp2cc_metaclass_value_p_tchild());"));
}

void test_metaclass_derived_constructor_surface_stays_visible() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create(n : integer);\n"
      "  end;\n"
      "  tchildclass = class of tchild;\n"
      "var\n"
      "  cls : tchildclass;\n"
      "  inst : tchild;\n"
      "implementation\n"
      "constructor tchild.create(n : integer);\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  inst := cls.create(7);\n"
      "end.\n");
  CHECK(contains(out.header,
                 "struct tp2cc_metaclass_p_tchild : public tp2cc_metaclass_p_tbase {"));
  CHECK(contains(out.header,
                 "p_tchild* (*p_create)(int32_t);"));
  CHECK(contains(out.header,
                 "tp2cc_metaclass_p_tchild(tp2cc_metaclass_p_tbase tp2cc_parent, p_tchild* (*tp2cc_p_create)(int32_t))"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create(7);"));
}

void test_metaclass_base_constructor_slot_survives_hidden_child_create() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create(n : integer);\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "var\n"
      "  cls : tbaseclass;\n"
      "  inst : tbase;\n"
      "implementation\n"
      "constructor tbase.create;\n"
      "begin\n"
      "end;\n"
      "constructor tchild.create(n : integer);\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  cls := tchild;\n"
      "  inst := cls.create;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "static_cast<p_tbase*>(tp2cc_ptr)->p_create();"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create();"));
}

void test_inheritsfrom_uses_runtime_tclass_and_method_call() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "  end;\n"
      "function isbase(x : tbase; c : tclass) : boolean;\n"
      "implementation\n"
      "function isbase(x : tbase; c : tclass) : boolean;\n"
      "begin\n"
      "  isbase := x.inheritsfrom(c);\n"
      "end.\n");
  CHECK(contains(out.header, "bool p_isbase(p_tbase* p_x, ::rt::p_tclass p_c);"));
  CHECK(contains(out.impl, "p_result = p_x->p_inheritsfrom(p_c);"));
}

void test_indexed_property_result_classtype_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class\n"
      "  end;\n"
      "  tbox = class\n"
      "  private\n"
      "    function getitem(i : integer) : titem;\n"
      "  public\n"
      "    property items[i : integer] : titem read getitem; default;\n"
      "  end;\n"
      "function sameclass(b : tbox; i : integer; c : tclass) : boolean;\n"
      "implementation\n"
      "function tbox.getitem(i : integer) : titem;\n"
      "begin\n"
      "  getitem := nil;\n"
      "end;\n"
      "function sameclass(b : tbox; i : integer; c : tclass) : boolean;\n"
      "begin\n"
      "  sameclass := b.items[i].classtype = c;\n"
      "end.\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = (p_b->p_getitem(p_i)->p_classtype() == p_c);"));
}

void test_implicit_indexed_property_result_classtype_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class\n"
      "  end;\n"
      "  tbox = class\n"
      "  private\n"
      "    function getitem(i : integer) : titem;\n"
      "  public\n"
      "    property items[i : integer] : titem read getitem; default;\n"
      "    function sameclass(i : integer; c : tclass) : boolean;\n"
      "  end;\n"
      "implementation\n"
      "function tbox.getitem(i : integer) : titem;\n"
      "begin\n"
      "  getitem := nil;\n"
      "end;\n"
      "function tbox.sameclass(i : integer; c : tclass) : boolean;\n"
      "begin\n"
      "  sameclass := items[i].classtype = c;\n"
      "end.\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = (this->p_getitem(p_i)->p_classtype() == p_c);"));
}

void test_indexed_implicit_property_in_method_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlist = class\n"
      "  private\n"
      "    function get(i : integer) : integer;\n"
      "  public\n"
      "    function first : integer;\n"
      "    property items[i : integer] : integer read get; default;\n"
      "  end;\n"
      "implementation\n"
      "function tlist.get(i : integer) : integer;\n"
      "begin\n"
      "  get := i;\n"
      "end;\n"
      "function tlist.first : integer;\n"
      "begin\n"
      "  first := items[0];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = this->p_get(0);"));
  CHECK(!contains(out.impl, "p_items[0]"));
}

void test_indexed_implicit_property_result_write_in_method_body() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titems = class\n"
      "    function get(index : longint) : pointer;\n"
      "    procedure put(index : longint; value : pointer);\n"
      "    property items[index : longint] : pointer read get write put; default;\n"
      "  end;\n"
      "  tbox = class\n"
      "  private\n"
      "    flist : titems;\n"
      "  public\n"
      "    procedure demo(p : pointer);\n"
      "    property list : titems read flist;\n"
      "  end;\n"
      "implementation\n"
      "function titems.get(index : longint) : pointer;\n"
      "begin\n"
      "  get := nil;\n"
      "end;\n"
      "procedure titems.put(index : longint; value : pointer);\n"
      "begin\n"
      "end;\n"
      "procedure tbox.demo(p : pointer);\n"
      "begin\n"
      "  p := list[1];\n"
      "  list[2] := p;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = this->p_flist->p_get(1);"));
  CHECK(contains(out.impl, "this->p_flist->p_put(2, p_p);"));
  CHECK(!contains(out.impl, "property is read-only"));
}

void test_function_result_member_access_uses_pointer_semantics() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "  end;\n"
      "function clone(n : tnode) : tnode;\n"
      "implementation\n"
      "function clone(n : tnode) : tnode;\n"
      "begin\n"
      "  clone := n;\n"
      "  clone.next := nil;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = p_n;"));
  CHECK(contains(out.impl, "p_result->p_next = nullptr;"));
}

void test_pointer_typed_field_chain_keeps_arrow_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsym = class\n"
      "    name : string;\n"
      "  end;\n"
      "  tblock = class\n"
      "    sym : tsym;\n"
      "  end;\n"
      "function getname(b : tblock) : string;\n"
      "implementation\n"
      "function getname(b : tblock) : string;\n"
      "begin\n"
      "  getname := b.sym.name;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, p_b->p_sym->p_name);"));
}

void test_with_cast_binds_pointer_rvalue_by_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "  end;\n"
      "procedure clear(p : pointer);\n"
      "implementation\n"
      "procedure clear(p : pointer);\n"
      "begin\n"
      "  with tnode(p) do\n"
      "    next := nil;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto tp2cc_with_0 = ((p_tnode*)(p_p));"));
  CHECK(!contains(out.impl, "auto& tp2cc_with_0 = ((p_tnode*)(p_p));"));
  CHECK(contains(out.impl, "tp2cc_with_0->p_next = nullptr;"));
}

void test_statement_context_member_destroy_autocalls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlist = class\n"
      "  end;\n"
      "procedure zap(list : tlist);\n"
      "implementation\n"
      "procedure zap(list : tlist);\n"
      "begin\n"
      "  list.destroy;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_list->p_destroy();"));
}

void test_var_arg_class_cast_reinterprets_storage_slot() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsym = class\n"
      "  end;\n"
      "  tlabel = class(tsym)\n"
      "  end;\n"
      "procedure take(var s : tsym);\n"
      "procedure use(l : tlabel);\n"
      "implementation\n"
      "procedure take(var s : tsym);\n"
      "begin\n"
      "end;\n"
      "procedure use(l : tlabel);\n"
      "begin\n"
      "  take(tsym(l));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_take(::rt::tp2cc_reinterpret_storage_ref<p_tsym*>(p_l));"));
}

void test_var_arg_derived_pointer_slot_reinterprets_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "  end;\n"
      "procedure take(var b : tbase);\n"
      "procedure demo(c : tchild);\n"
      "implementation\n"
      "procedure take(var b : tbase);\n"
      "begin\n"
      "end;\n"
      "procedure demo(c : tchild);\n"
      "begin\n"
      "  take(c);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_take(::rt::tp2cc_reinterpret_storage_ref<p_tbase*>(p_c));"));
}

void test_nested_proc_var_arg_keeps_storage_semantics() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsym = class\n"
      "  end;\n"
      "  tlabel = class(tsym)\n"
      "  end;\n"
      "procedure demo(l : tlabel);\n"
      "implementation\n"
      "procedure demo(l : tlabel);\n"
      "  procedure touch(var s : tsym);\n"
      "  begin\n"
      "  end;\n"
      "begin\n"
      "  touch(tsym(l));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_touch(::rt::tp2cc_reinterpret_storage_ref<p_tsym*>(p_l));"));
}

void test_parameterless_method_result_keeps_arrow_member_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tprocdef = class\n"
      "    mangledname : string;\n"
      "  end;\n"
      "  tprocsym = class\n"
      "    function first_procdef : tprocdef;\n"
      "  end;\n"
      "function getname(ps : tprocsym) : string;\n"
      "implementation\n"
      "function tprocsym.first_procdef : tprocdef;\n"
      "begin\n"
      "  first_procdef := nil;\n"
      "end;\n"
      "function getname(ps : tprocsym) : string;\n"
      "begin\n"
      "  getname := ps.first_procdef.mangledname;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, "
                 "p_ps->p_first_procdef()->p_mangledname);"));
}

void test_parameterless_method_result_autocalls_in_outer_callee_context() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tprocdef = class\n"
      "    function flag(x : integer) : boolean;\n"
      "  end;\n"
      "  tprocsym = class\n"
      "    function first_procdef : tprocdef;\n"
      "  end;\n"
      "function ok(ps : tprocsym) : boolean;\n"
      "implementation\n"
      "function tprocdef.flag(x : integer) : boolean;\n"
      "begin\n"
      "  flag := false;\n"
      "end;\n"
      "function tprocsym.first_procdef : tprocdef;\n"
      "begin\n"
      "  first_procdef := nil;\n"
      "end;\n"
      "function ok(ps : tprocsym) : boolean;\n"
      "begin\n"
      "  ok := ps.first_procdef.flag(1);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = p_ps->p_first_procdef()->p_flag(1);"));
}

void test_with_parameterless_method_result_keeps_arrow_member_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tprocdef = class\n"
      "    mangledname : string;\n"
      "  end;\n"
      "  tprocsym = class\n"
      "    function first_procdef : tprocdef;\n"
      "  end;\n"
      "function getname(ps : tprocsym) : string;\n"
      "implementation\n"
      "function tprocsym.first_procdef : tprocdef;\n"
      "begin\n"
      "  first_procdef := nil;\n"
      "end;\n"
      "function getname(ps : tprocsym) : string;\n"
      "begin\n"
      "  with ps do\n"
      "    getname := first_procdef.mangledname;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign("
                 "p_result, tp2cc_with_0->p_first_procdef()->p_mangledname);"));
}

void test_pointer_indexed_class_field_chain_keeps_arrow_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tmodule = class\n"
      "    modulename : string;\n"
      "  end;\n"
      "  tderefmaprec = record\n"
      "    u : tmodule;\n"
      "  end;\n"
      "  pderefmap = ^tderefmaprec;\n"
      "function getname(m : pderefmap) : string;\n"
      "implementation\n"
      "function getname(m : pderefmap) : string;\n"
      "begin\n"
      "  getname := m[0].u.modulename;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, "
                 "p_m[0].p_u->p_modulename);"));
}

void test_as_cast_member_chain_keeps_arrow_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tinlininginfo = class\n"
      "    flags : integer;\n"
      "  end;\n"
      "  tabstractprocdef = class\n"
      "  end;\n"
      "  tprocdef = class(tabstractprocdef)\n"
      "    inlininginfo : tinlininginfo;\n"
      "  end;\n"
      "function getflags(pd : tabstractprocdef) : integer;\n"
      "implementation\n"
      "function getflags(pd : tabstractprocdef) : integer;\n"
      "begin\n"
      "  getflags := (pd as tprocdef).inlininginfo.flags;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = dynamic_cast<p_tprocdef*>(p_pd)->p_inlininginfo->p_flags;"));
}

void test_with_local_record_pointer_uses_bound_storage_for_fields() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "type\n"
      "  pdata = ^tdata;\n"
      "  tdata = record\n"
      "    x, y : integer;\n"
      "  end;\n"
      "var\n"
      "  d : pdata;\n"
      "begin\n"
      "  new(d);\n"
      "  with d^ do\n"
      "  begin\n"
      "    x := 1;\n"
      "    y := x;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto& tp2cc_with_0 = ::rt::tp2cc_deref(p_d);"));
  CHECK(contains(out.impl, "tp2cc_with_0.p_x = 1;"));
  CHECK(contains(out.impl, "tp2cc_with_0.p_y = tp2cc_with_0.p_x;"));
  CHECK(!contains(out.impl, "\n        p_x = 1;"));
  CHECK(!contains(out.impl, "\n        p_y = p_x;"));
}

void test_statement_new_and_dispose_use_runtime_storage_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "type\n"
      "  pdata = ^tdata;\n"
      "  tdata = record\n"
      "    value : integer;\n"
      "  end;\n"
      "procedure demo;\n"
      "var\n"
      "  d : pdata;\n"
      "begin\n"
      "  new(d);\n"
      "  dispose(d);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_new(p_d);"));
  CHECK(contains(out.impl, "::rt::p_dispose(p_d);"));
  CHECK(!contains(out.impl, "p_d = new "));
  CHECK(!contains(out.impl, "delete p_d;"));
}

void test_expression_new_uses_runtime_storage_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "type\n"
      "  pbox = ^tbox;\n"
      "  tbox = object\n"
      "    constructor init(n : integer);\n"
      "  end;\n"
      "procedure demo;\n"
      "var\n"
      "  b : pbox;\n"
      "begin\n"
      "  b := new(pbox, init(3));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_new(tp2cc_ptr);"));
  CHECK(!contains(out.impl, "new ::std::remove_pointer_t<p_pbox>{}"));
}

void test_type_order_sees_method_signature_dependencies() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = record\n"
      "    value : integer;\n"
      "  end;\n"
      "  titems = array[word] of titem;\n"
      "  tbox = class\n"
      "    procedure fill(var items : titems);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  size_t alias_pos = out.header.find(
      "using p_titems = ::rt::tp2cc_Array<p_titem, 0, 65536>;");
  size_t class_pos = out.header.find("struct p_tbox : public ::rt::p_tobject");
  CHECK(alias_pos != std::string::npos);
  CHECK(class_pos != std::string::npos);
  CHECK(alias_pos < class_pos);
}

void test_reference_class_typecast_is_pointer_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class end;\n"
      "  tchild = class(tbase) end;\n"
      "function cast_child(p : tbase) : tchild;\n"
      "implementation\n"
      "function cast_child(p : tbase) : tchild;\n"
      "begin\n"
      "  cast_child := tchild(p);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = ((p_tchild*)(p_p));"));
}

void test_reference_class_cast_keeps_pointer_member_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class end;\n"
      "  tchild = class(tbase)\n"
      "    next : tchild;\n"
      "  end;\n"
      "function fetch_next(p : tbase) : tchild;\n"
      "implementation\n"
      "function fetch_next(p : tbase) : tchild;\n"
      "begin\n"
      "  fetch_next := tchild(p).next;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = ((p_tchild*)(p_p))->p_next;"));
}

void test_is_as_use_pointer_target_types() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class end;\n"
      "  tchild = class(tbase) end;\n"
      "function is_child(p : tbase) : boolean;\n"
      "function as_child(p : tbase) : tchild;\n"
      "implementation\n"
      "function is_child(p : tbase) : boolean;\n"
      "begin\n"
      "  is_child := p is tchild;\n"
      "end;\n"
      "function as_child(p : tbase) : tchild;\n"
      "begin\n"
      "  as_child := p as tchild;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "dynamic_cast<p_tchild*>(p_p) != nullptr"));
  CHECK(contains(out.impl, "p_result = dynamic_cast<p_tchild*>(p_p);"));
}

// Pascal identifiers that happen to be C++ reserved words (but are NOT
// Pascal keywords) must survive translation thanks to the `p_` prefix.
void test_cxx_reserved_word_identifiers() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  this : longint;\n"
      "  namespace : longint;\n"
      "  template : longint;\n"
      "  typename : longint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_this"));
  CHECK(contains(out.header, "p_namespace"));
  CHECK(contains(out.header, "p_template"));
  CHECK(contains(out.header, "p_typename"));
  // And crucially: the raw C++ keywords must not appear bare.
  CHECK(!contains(out.header, " this "));
  CHECK(!contains(out.header, " template "));
}

}  // namespace

int main() {
  RUN_TEST(test_empty_unit_skeleton);
  RUN_TEST(test_program_registers_unit_finalizers);
  RUN_TEST(test_uses_become_includes_without_open_namespaces);
  RUN_TEST(test_scalar_const);
  RUN_TEST(test_typed_scalar_const);
  RUN_TEST(test_typed_scalar_const_wraps_to_destination_value);
  RUN_TEST(test_enum_type);
  RUN_TEST(test_enum_type_with_explicit_values);
  RUN_TEST(test_packed_record_uses_byte_sized_enum_fields);
  RUN_TEST(test_packed_record_shortstring_field_emits_exact_layout_asserts);
  RUN_TEST(test_packed_record_array_field_keeps_array_wrapper_with_exact_layout_asserts);
  RUN_TEST(test_packed_variant_record_emits_packed_case_layout_asserts);
  RUN_TEST(test_packed_record_array_index_reports_error);
  RUN_TEST(test_packed_record_nested_member_reports_error);
  RUN_TEST(test_packed_record_method_call_reports_error);
  RUN_TEST(test_packed_record_char_array_index_is_allowed);
  RUN_TEST(test_packed_record_shortstring_array_index_is_allowed);
  RUN_TEST(test_explicit_enum_array_bounds_use_ordinal_range);
  RUN_TEST(test_distinct_ordinal_array_bounds_use_underlying_range);
  RUN_TEST(test_low_high_use_resolved_pascal_type);
  RUN_TEST(test_char_array_typed_const_uses_explicit_array_literal_helper);
  RUN_TEST(test_char_array_assignment_uses_explicit_array_literal_helper);
  RUN_TEST(test_nested_array_typed_const_braces_each_array_wrapper);
  RUN_TEST(test_typed_const_shortstring_literals_use_target_capacity);
  RUN_TEST(test_named_type_alias);
  RUN_TEST(test_ansistring_builtin_maps_to_runtime_type);
  RUN_TEST(test_widechar_builtin_maps_to_16bit_ordinal);
  RUN_TEST(test_set_type_alias);
  RUN_TEST(test_var_extern_in_header_and_def_in_impl);
  RUN_TEST(test_out_parameter_emits_like_var_reference);
  RUN_TEST(test_const_pointer_parameter_stays_value_abi);
  RUN_TEST(test_const_fixed_array_parameter_stays_value_abi);
  RUN_TEST(test_const_fixed_record_array_parameter_stays_value_abi);
  RUN_TEST(test_const_fixed_classref_array_parameter_stays_value_abi);
  RUN_TEST(test_proc_signature_in_header);
  RUN_TEST(test_typed_array_const);
  RUN_TEST(test_typed_array_const_with_inline_subrange_element_type);
  RUN_TEST(test_free_function_trailing_default_argument_is_lowered);
  RUN_TEST(test_method_trailing_default_argument_is_lowered);
  RUN_TEST(test_singleton_typed_array_const);
  RUN_TEST(test_nested_array_type);
  RUN_TEST(test_named_subrange_array_type);
  RUN_TEST(test_packed_record_keeps_packed_layout);
  RUN_TEST(test_parenthesized_record_const);
  RUN_TEST(test_call_arg_literal_is_lowered_to_parameter_value);
  RUN_TEST(test_explicit_integer_cast_literal_uses_converted_value);
  RUN_TEST(test_assignment_const_expr_is_lowered_to_target_value);
  RUN_TEST(test_implicit_const_expr_wraps_without_emit_error);
  RUN_TEST(test_explicit_integer_cast_const_expr_wraps_without_error);
  RUN_TEST(test_untyped_integer_const_uses_pascal_initial_type);
  RUN_TEST(test_typed_const_read_is_not_folded_through_initializer);
  RUN_TEST(test_exit_literal_uses_function_result_type);
  RUN_TEST(test_local_result_name_is_rejected_in_function_body);
  RUN_TEST(test_nested_procedure_can_assign_enclosing_result);
  RUN_TEST(test_nested_function_uses_own_result_and_outer_name);
  RUN_TEST(test_try_finally_uses_scope_exit_guard);
  RUN_TEST(test_try_except_raises_and_matches_exception_class);
  RUN_TEST(test_try_except_multiple_handlers_start_with_if_and_base_pointer_cast);
  RUN_TEST(test_char_plus_cast_uses_string_concat);
  RUN_TEST(test_nul_char_plus_cast_uses_string_concat);
  RUN_TEST(test_embedded_nul_string_literal_uses_explicit_length_builder);
  RUN_TEST(test_shortstring_assignment_uses_pascal_string_helper);
  RUN_TEST(test_var_shortstring_call_keeps_lvalue_storage);
  RUN_TEST(test_var_ansistring_call_keeps_lvalue_storage);
  RUN_TEST(test_overloaded_string_and_bool_call_keeps_boolean_argument_raw);
  RUN_TEST(test_pchar_cast_argument_converts_to_string_value);
  RUN_TEST(test_integer_and_or_stays_bitwise);
  RUN_TEST(test_nested_boolean_function_and_short_circuits);
  RUN_TEST(test_nested_untyped_var_forwarding_stays_pointer_value);
  RUN_TEST(test_untyped_method_call_on_variable_uses_storage_address);
  RUN_TEST(test_fillchar_uses_storage_address_for_pointer_slots);
  RUN_TEST(test_move_uses_storage_addresses_for_source_and_destination_slots);
  RUN_TEST(test_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_local_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_visible_pointer_alias_cast_uses_qualified_type_spelling);
  RUN_TEST(test_local_pointer_alias_cast_uses_local_type_spelling);
  RUN_TEST(test_runtime_alias_type_names_are_explicitly_qualified);
  RUN_TEST(test_charset_stub_type_names_are_explicitly_qualified);
  RUN_TEST(test_tmethod_type_name_is_explicitly_qualified);
  RUN_TEST(test_local_enum_members_do_not_fall_back_to_runtime);
  RUN_TEST(test_sizeof_visible_type_uses_type_spelling_not_identifier_lookup);
  RUN_TEST(test_primitive_cast_assign_reinterprets_storage);
  RUN_TEST(test_primitive_cast_read_reinterprets_storage);
  RUN_TEST(test_inc_untyped_primitive_cast_reinterprets_storage_by_byte_copy);
  RUN_TEST(test_inc_primitive_cast_reinterprets_storage);
  RUN_TEST(test_untyped_array_view_index_uses_byte_load_store);
  RUN_TEST(test_aggregate_to_primitive_cast_reinterprets_bytes);
  RUN_TEST(test_absolute_pointer_target_reinterprets_pointee_storage);
  RUN_TEST(test_absolute_pointer_alias_reinterprets_pointer_storage);
  RUN_TEST(test_pointer_alias_cast_on_pointer_expression_uses_plain_cast);
  RUN_TEST(test_absolute_typed_const_alias_reinterprets_same_storage);
  RUN_TEST(test_absolute_const_param_alias_stays_const_reference);
  RUN_TEST(test_property_getter_setter_lowering);
  RUN_TEST(test_property_field_and_default_index_lowering);
  RUN_TEST(test_property_result_default_index_read_write_lowering);
  RUN_TEST(test_implicit_property_lookup_in_method_body);
  RUN_TEST(test_procvar_property_stmt_and_value_context);
  RUN_TEST(test_default_indexed_procvar_property_stmt_autocalls);
  RUN_TEST(test_class_method_static_emission_and_calls);
  RUN_TEST(test_tobject_runtime_helpers_lower_in_method_body);
  RUN_TEST(test_tobject_cast_preserves_pointer_semantics_for_free);
  RUN_TEST(test_parameterless_proc_assignment_keeps_designator);
  RUN_TEST(test_method_pointer_type_and_bound_assignment_emit);
  RUN_TEST(test_method_pointer_record_cast_reinterprets_same_storage);
  RUN_TEST(test_unbound_method_address_uses_thunk_code);
  RUN_TEST(test_internal_helpers_avoid_double_underscores);
  RUN_TEST(test_for_loop_uses_resolved_global_control_var);
  RUN_TEST(test_const_object_param_uses_mutable_ref);
  RUN_TEST(test_parameterless_procvar_stmt_autocalls);
  RUN_TEST(test_direct_procvar_var_decl_uses_named_function_pointer_syntax);
  RUN_TEST(test_runtime_builtin_stmt_autocalls);
  RUN_TEST(test_bool_procvar_call_uses_logical_and);
  RUN_TEST(test_open_array_method_signature_keeps_wrapper_type);
  RUN_TEST(test_open_array_procvar_signature_keeps_wrapper_type);
  RUN_TEST(test_open_array_call_uses_owning_temporary_wrapper);
  RUN_TEST(test_high_low_on_open_array_use_runtime_length);
  RUN_TEST(test_dynamic_array_type_uses_runtime_carrier);
  RUN_TEST(test_dynamic_array_actual_converts_to_open_array_view);
  RUN_TEST(test_memory_helpers_reinterpret_typecast_pointer_slots);
  RUN_TEST(test_unit_local_enum_array_bounds_win_over_unrelated_same_name_types);
  RUN_TEST(test_typed_set_literal_uses_surrounding_set_type);
  RUN_TEST(test_explicit_set_cast_uses_runtime_helper);
  RUN_TEST(test_set_range_literal_uses_integer_ordinal_loop);
  RUN_TEST(test_untyped_const_method_thunk_keeps_raw_storage_pointer);
  RUN_TEST(test_untyped_const_distinguishes_pointer_slot_from_pointed_bytes);
  RUN_TEST(test_class_types_lower_to_pointers_and_implicit_tobject);
  RUN_TEST(test_forward_class_decl_only_emits_one_struct_body);
  RUN_TEST(test_empty_inherited_class_decl_emits_real_struct);
  RUN_TEST(test_abstract_method_emits_fail_fast_virtual_body);
  RUN_TEST(test_pointer_sized_integer_aliases_lower_through_rt);
  RUN_TEST(test_tclass_alias_lowers_through_rt);
  RUN_TEST(test_class_constructor_call_allocates_instance);
  RUN_TEST(test_class_constructor_trailing_default_argument_is_lowered);
  RUN_TEST(test_object_constructor_call_uses_base_method_on_self);
  RUN_TEST(test_implicit_tobject_inherited_constructor_autocalls);
  RUN_TEST(test_inherited_destroy_autocalls_through_non_overriding_parent);
  RUN_TEST(test_class_self_and_free_use_pointer_semantics);
  RUN_TEST(test_metaclass_alias_and_concrete_class_value_lowering);
  RUN_TEST(test_metaclass_cast_keeps_concrete_descriptor);
  RUN_TEST(test_class_identifier_value_lowers_to_metaclass_descriptor);
  RUN_TEST(test_metaclass_derived_constructor_surface_stays_visible);
  RUN_TEST(test_metaclass_base_constructor_slot_survives_hidden_child_create);
  RUN_TEST(test_inheritsfrom_uses_runtime_tclass_and_method_call);
  RUN_TEST(test_indexed_property_result_classtype_autocalls);
  RUN_TEST(test_implicit_indexed_property_result_classtype_autocalls);
  RUN_TEST(test_indexed_implicit_property_in_method_body);
  RUN_TEST(test_indexed_implicit_property_result_write_in_method_body);
  RUN_TEST(test_function_result_member_access_uses_pointer_semantics);
  RUN_TEST(test_pointer_typed_field_chain_keeps_arrow_access);
  RUN_TEST(test_with_cast_binds_pointer_rvalue_by_value);
  RUN_TEST(test_statement_context_member_destroy_autocalls);
  RUN_TEST(test_var_arg_class_cast_reinterprets_storage_slot);
  RUN_TEST(test_var_arg_derived_pointer_slot_reinterprets_storage);
  RUN_TEST(test_nested_proc_var_arg_keeps_storage_semantics);
  RUN_TEST(test_parameterless_method_result_keeps_arrow_member_access);
  RUN_TEST(test_parameterless_method_result_autocalls_in_outer_callee_context);
  RUN_TEST(test_with_parameterless_method_result_keeps_arrow_member_access);
  RUN_TEST(test_pointer_indexed_class_field_chain_keeps_arrow_access);
  RUN_TEST(test_as_cast_member_chain_keeps_arrow_access);
  RUN_TEST(test_with_local_record_pointer_uses_bound_storage_for_fields);
  RUN_TEST(test_statement_new_and_dispose_use_runtime_storage_helpers);
  RUN_TEST(test_expression_new_uses_runtime_storage_helper);
  RUN_TEST(test_type_order_sees_method_signature_dependencies);
  RUN_TEST(test_reference_class_typecast_is_pointer_cast);
  RUN_TEST(test_reference_class_cast_keeps_pointer_member_access);
  RUN_TEST(test_is_as_use_pointer_target_types);
  RUN_TEST(test_cxx_reserved_word_identifiers);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
