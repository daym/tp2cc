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

EmittedUnit compile_snippet(std::string text) {
  auto sf = std::make_unique<SourceFile>();
  sf->path = "<mem>";
  sf->contents = std::move(text);
  Lexer lx(std::move(sf));
  Parser p(lx);
  auto u = p.parse();
  if (!u) return {};
  return emit_unit(*u);
}

EmittedUnit compile_snippet_with_registry(std::string text) {
  auto sf = std::make_unique<SourceFile>();
  sf->path = "<mem>";
  sf->contents = std::move(text);
  Lexer lx(std::move(sf));
  Parser p(lx);
  auto u = p.parse();
  if (!u) return {};
  TypeRegistry reg;
  std::vector<const UnitNode*> units = {u.get()};
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

void test_uses_become_includes_and_usings() {
  auto out = compile_snippet(
      "unit foo;\n"
      "interface\n"
      "uses bar, baz;\n"
      "implementation\n"
      "uses qux;\n"
      "end.\n");
  CHECK(contains(out.header, "#include \"p_bar.h\""));
  CHECK(contains(out.header, "#include \"p_baz.h\""));
  CHECK(contains(out.header, "using namespace p_bar;"));
  CHECK(contains(out.header, "using namespace p_baz;"));
  CHECK(contains(out.impl, "#include \"p_qux.h\""));
  CHECK(contains(out.impl, "using namespace p_qux;"));
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
  // Multi-character string literal is wrapped in ShortString so `+` is
  // concatenation, not pointer arithmetic.
  CHECK(contains(out.header, "p_s = ::rt::ShortString<>(\"hello\")"));
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
  CHECK(contains(out.header, "enum p_tcolor"));
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
                 "using p_tmap = ::rt::Array<uint8_t, p_lo, ((::rt::p_ordinal_value(p_hi)) - (::rt::p_ordinal_value(p_lo)) + 1)>;"));
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
                 "using p_tmap = ::rt::Array<uint8_t, 0, 65536>;"));
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
  CHECK(contains(out.header, "using p_mystr = ::rt::ShortString<32>;"));
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
  CHECK(contains(out.header, "using p_tname = ::rt::AnsiString;"));
  CHECK(contains(out.header, "extern ::rt::AnsiString p_s;"));
  CHECK(contains(out.header, "void p_take(::rt::AnsiString p_x);"));
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
  CHECK(contains(out.header, "using p_tcolors = ::rt::Set<p_tcolor>;"));
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
  CHECK(contains(out.header, "::rt::Array<::rt::Array<int32_t"));
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
                 "using p_arr = ::rt::Array<int32_t, 1, ((10) - (1) + 1)>;"));
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

void test_char_plus_cast_uses_string_concat() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  s = #1 + char(byte(66));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::ShortString<>(::rt::p_char_of('\\x01'))"));
  CHECK(!contains(out.header, "'\\x01' + (("));
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
                 "::rt::p_reinterpret_storage_ref<p_t80bitarray>(p_e)[p_i]"));
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
                 "::rt::p_reinterpret_storage_ref<p_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "p_t80bitarray(p_e)[p_i]"));
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
  CHECK(contains(out.impl, "::rt::p_reinterpret_ref<int32_t>(p_b) = p_l;"));
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
  CHECK(contains(out.impl, "p_l = ::rt::p_reinterpret_ref<int32_t>(p_b);"));
  CHECK(!contains(out.impl, "p_l = ((int32_t)(p_b));"));
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
                 "::rt::p_inc(::rt::p_reinterpret_storage_ref<int32_t>(p_p))"));
  CHECK(!contains(out.impl, "p_p = ((int32_t)(p_p) + 1)"));
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
  CHECK(contains(out.impl, "p_d = ::rt::p_reinterpret_copy<double>(p_bits);"));
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
                 "p_titem &p_item = ::rt::p_reinterpret_ref<p_titem>(p_raw);"));
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
                 "p_pint &p_value = ::rt::p_reinterpret_storage_ref<p_pint>(p_raw);"));
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
                 "p_parr &p_view = ::rt::p_reinterpret_storage_ref<p_parr>(p_raw);"));
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
                 "const ::rt::ShortString<>& p_view = "
                 "::rt::p_reinterpret_storage_ref<::rt::ShortString<>>(p_s);"));
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
  CHECK(contains(out.impl, "if ((this->p_getname() != ::rt::ShortString<>(\"\")))"));
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
  CHECK(contains(out.header, "virtual const void* p_classtype() override;"));
  CHECK(contains(out.header, "virtual int32_t p_instancesize() override;"));
  CHECK(contains(out.header, "inline const void* p_titem::p_classtype() {"));
  CHECK(contains(out.header, "inline int32_t p_titem::p_instancesize() {"));
  CHECK(contains(out.impl, "p_result = p_classtype();"));
  CHECK(contains(out.impl, "p_result = p_instancesize();"));
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
  CHECK(contains(out.header, "using p_tcb = ::rt::MethodPtr<void(int32_t)>;"));
  CHECK(contains(out.header,
                 "static void tp2cc_methodptr_fire_value_name_integer_ret_void"));
  CHECK(contains(out.impl,
                 "::rt::p_method_code<&p_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
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
                 "::rt::p_reinterpret_storage_ref<p_trec>(p_p).p_procpointer = p_addr;"));
  CHECK(contains(out.impl,
                 "::rt::p_reinterpret_storage_ref<p_trec>(p_p).p_s = p_selfp;"));
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
                 "p_addr = ::rt::p_method_code<&p_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
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
  // `OpenArray<T>` for a parameter, it must keep that exact C++ carrier
  // type instead of falling back to the generic `array of T -> T*` path.
  CHECK(contains(out.header,
                 "::rt::ShortString<> p_get(int32_t p_nr, ::rt::OpenArray<::rt::ShortString<>> p_args);"));
  CHECK(contains(out.header,
                 "static ::rt::ShortString<> tp2cc_methodptr_get_value_name_integer_const_openarr_string_ret_string(void* tp2cc_self, int32_t p_nr, ::rt::OpenArray<::rt::ShortString<>> p_args)"));
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
                 "using p_tcb = void (*)(::rt::OpenArray<::rt::ShortString<>>);"));
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
  CHECK(contains(out.impl, "p_log(::rt::p_open_array_of<uint16_t>(1, 2, 3))"));
  CHECK(contains(out.impl, "p_log(::rt::OpenArray<uint16_t>())"));
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
  CHECK(contains(out.impl, "::rt::Set<int32_t>::from_list({3})"));
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
  CHECK(contains(out.impl, "::rt::p_set_cast<::rt::Set<uint8_t>>(p_small)"));
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
  CHECK(contains(out.impl, "for (int64_t tp2cc_value = (int64_t)(::rt::p_char_of('a'))"));
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
  CHECK(contains(out.impl, "if (p_assigned(p_cls))"));
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
  CHECK(contains(out.impl, "p_result = p_b->p_sym->p_name;"));
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
                 "p_take(::rt::p_reinterpret_storage_ref<p_tsym*>(p_l));"));
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
                 "p_take(::rt::p_reinterpret_storage_ref<p_tbase*>(p_c));"));
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
                 "p_touch(::rt::p_reinterpret_storage_ref<p_tsym*>(p_l));"));
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
  CHECK(contains(out.impl, "p_result = p_ps->p_first_procdef()->p_mangledname;"));
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
                 "p_result = tp2cc_with_0->p_first_procdef()->p_mangledname;"));
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
  CHECK(contains(out.impl, "p_result = p_m[0].p_u->p_modulename;"));
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
  CHECK(contains(out.impl, "auto& tp2cc_with_0 = ::rt::p_deref(p_d);"));
  CHECK(contains(out.impl, "tp2cc_with_0.p_x = 1;"));
  CHECK(contains(out.impl, "tp2cc_with_0.p_y = tp2cc_with_0.p_x;"));
  CHECK(!contains(out.impl, "\n        p_x = 1;"));
  CHECK(!contains(out.impl, "\n        p_y = p_x;"));
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
      "using p_titems = ::rt::Array<p_titem, 0, 65536>;");
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
  RUN_TEST(test_uses_become_includes_and_usings);
  RUN_TEST(test_scalar_const);
  RUN_TEST(test_typed_scalar_const);
  RUN_TEST(test_typed_scalar_const_wraps_to_destination_value);
  RUN_TEST(test_enum_type);
  RUN_TEST(test_enum_type_with_explicit_values);
  RUN_TEST(test_explicit_enum_array_bounds_use_ordinal_range);
  RUN_TEST(test_distinct_ordinal_array_bounds_use_underlying_range);
  RUN_TEST(test_low_high_use_resolved_pascal_type);
  RUN_TEST(test_named_type_alias);
  RUN_TEST(test_ansistring_builtin_maps_to_runtime_type);
  RUN_TEST(test_widechar_builtin_maps_to_16bit_ordinal);
  RUN_TEST(test_set_type_alias);
  RUN_TEST(test_var_extern_in_header_and_def_in_impl);
  RUN_TEST(test_out_parameter_emits_like_var_reference);
  RUN_TEST(test_const_pointer_parameter_stays_value_abi);
  RUN_TEST(test_proc_signature_in_header);
  RUN_TEST(test_typed_array_const);
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
  RUN_TEST(test_char_plus_cast_uses_string_concat);
  RUN_TEST(test_integer_and_or_stays_bitwise);
  RUN_TEST(test_nested_boolean_function_and_short_circuits);
  RUN_TEST(test_nested_untyped_var_forwarding_stays_pointer_value);
  RUN_TEST(test_untyped_method_call_on_variable_uses_storage_address);
  RUN_TEST(test_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_local_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_primitive_cast_assign_reinterprets_storage);
  RUN_TEST(test_primitive_cast_read_reinterprets_storage);
  RUN_TEST(test_inc_primitive_cast_reinterprets_storage);
  RUN_TEST(test_aggregate_to_primitive_cast_reinterprets_bytes);
  RUN_TEST(test_absolute_pointer_target_reinterprets_pointee_storage);
  RUN_TEST(test_absolute_pointer_alias_reinterprets_pointer_storage);
  RUN_TEST(test_absolute_typed_const_alias_reinterprets_same_storage);
  RUN_TEST(test_absolute_const_param_alias_stays_const_reference);
  RUN_TEST(test_property_getter_setter_lowering);
  RUN_TEST(test_property_field_and_default_index_lowering);
  RUN_TEST(test_implicit_property_lookup_in_method_body);
  RUN_TEST(test_procvar_property_stmt_and_value_context);
  RUN_TEST(test_default_indexed_procvar_property_stmt_autocalls);
  RUN_TEST(test_class_method_static_emission_and_calls);
  RUN_TEST(test_tobject_runtime_helpers_lower_in_method_body);
  RUN_TEST(test_parameterless_proc_assignment_keeps_designator);
  RUN_TEST(test_method_pointer_type_and_bound_assignment_emit);
  RUN_TEST(test_method_pointer_record_cast_reinterprets_same_storage);
  RUN_TEST(test_unbound_method_address_uses_thunk_code);
  RUN_TEST(test_internal_helpers_avoid_double_underscores);
  RUN_TEST(test_const_object_param_uses_mutable_ref);
  RUN_TEST(test_parameterless_procvar_stmt_autocalls);
  RUN_TEST(test_direct_procvar_var_decl_uses_named_function_pointer_syntax);
  RUN_TEST(test_runtime_builtin_stmt_autocalls);
  RUN_TEST(test_bool_procvar_call_uses_logical_and);
  RUN_TEST(test_open_array_method_signature_keeps_wrapper_type);
  RUN_TEST(test_open_array_procvar_signature_keeps_wrapper_type);
  RUN_TEST(test_open_array_call_uses_owning_temporary_wrapper);
  RUN_TEST(test_high_low_on_open_array_use_runtime_length);
  RUN_TEST(test_typed_set_literal_uses_surrounding_set_type);
  RUN_TEST(test_explicit_set_cast_uses_runtime_helper);
  RUN_TEST(test_set_range_literal_uses_integer_ordinal_loop);
  RUN_TEST(test_untyped_const_method_thunk_keeps_raw_storage_pointer);
  RUN_TEST(test_class_types_lower_to_pointers_and_implicit_tobject);
  RUN_TEST(test_forward_class_decl_only_emits_one_struct_body);
  RUN_TEST(test_class_constructor_call_allocates_instance);
  RUN_TEST(test_implicit_tobject_inherited_constructor_autocalls);
  RUN_TEST(test_inherited_destroy_autocalls_through_non_overriding_parent);
  RUN_TEST(test_class_self_and_free_use_pointer_semantics);
  RUN_TEST(test_metaclass_alias_and_concrete_class_value_lowering);
  RUN_TEST(test_metaclass_cast_keeps_concrete_descriptor);
  RUN_TEST(test_metaclass_derived_constructor_surface_stays_visible);
  RUN_TEST(test_indexed_implicit_property_in_method_body);
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
