// Emitter tests. Each test parses a small Pascal snippet, emits C++, and
// checks the generated text for the expected lowering. String matches are
// kept loose so formatting-only changes do not churn the tests.

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "emit.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "test_util.h"
#include "typereg.h"

using namespace p2cc;
using namespace p2cc::ast;
using namespace p2cc_test;

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

bool contains(const std::string& s, std::string_view needle) {
  return s.find(needle) != std::string::npos;
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

void test_proc_signature_in_header() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure foo(x : longint; var y : longint);\n"
      "function bar(a, b : longint) : longint;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "void p_foo(int32_t p_x, int32_t& p_y);"));
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
  CHECK(contains(out.header, "#pragma pack(push, 1)"));
  CHECK(contains(out.header, "struct p_hdr {"));
  CHECK(contains(out.header, "#pragma pack(pop)"));
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
                 "::rt::p_reinterpret_ref<p_t80bitarray>(p_e)[p_i]"));
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
                 "::rt::p_reinterpret_ref<p_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "p_t80bitarray(p_e)[p_i]"));
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
  RUN_TEST(test_uses_become_includes_and_usings);
  RUN_TEST(test_scalar_const);
  RUN_TEST(test_typed_scalar_const);
  RUN_TEST(test_enum_type);
  RUN_TEST(test_named_type_alias);
  RUN_TEST(test_set_type_alias);
  RUN_TEST(test_var_extern_in_header_and_def_in_impl);
  RUN_TEST(test_proc_signature_in_header);
  RUN_TEST(test_typed_array_const);
  RUN_TEST(test_singleton_typed_array_const);
  RUN_TEST(test_nested_array_type);
  RUN_TEST(test_named_subrange_array_type);
  RUN_TEST(test_packed_record_keeps_packed_layout);
  RUN_TEST(test_parenthesized_record_const);
  RUN_TEST(test_char_plus_cast_uses_string_concat);
  RUN_TEST(test_integer_and_or_stays_bitwise);
  RUN_TEST(test_nested_boolean_function_and_short_circuits);
  RUN_TEST(test_nested_untyped_var_forwarding_stays_pointer_value);
  RUN_TEST(test_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_local_byte_array_typecast_reinterprets_storage);
  RUN_TEST(test_cxx_reserved_word_identifiers);

  int n = p2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
