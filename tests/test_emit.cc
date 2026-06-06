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
#include "emit_makefile.h"
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

void test_emitted_makefile_tracks_sources_headers_and_program() {
  EmittedBuildManifest manifest;
  manifest.cc_sources = {"p_pp.cc", "p_globals.cc"};
  manifest.headers = {"p_pp.h", "p_globals.h", "p_sysutils.h"};
  manifest.pas_sources = {"../compiler/globals.pas", "../compiler/pp.pas"};
  manifest.tp2cc_program = "../bin/tp2cc";
  manifest.include_dirs = {"../include", "../../include"};
  manifest.tp2cc_args = {"emit-all", "-dCPU86", "-Fu../compiler", "--",
                         "../compiler/pp.pas", "."};
  manifest.program_name = "pp";
  std::string mk = emit_makefile(manifest);
  CHECK(contains(mk, "TP2CC = ../bin/tp2cc"));
  CHECK(contains(mk,
                 "CPPFLAGS = -I. -I../include -I../../include"));
  CHECK(contains(mk, "CC_SRCS := \\\n  p_globals.cc \\\n  p_pp.cc\n"));
  CHECK(contains(mk,
                 "HDRS := \\\n  p_globals.h \\\n  p_pp.h \\\n  p_sysutils.h\n"));
  CHECK(contains(mk,
                 "PAS_SRCS := \\\n  ../compiler/globals.pas \\\n  ../compiler/pp.pas\n"));
  CHECK(contains(mk, "PROGRAM = pp"));
  CHECK(contains(mk, "all: $(PROGRAM)"));
  CHECK(contains(mk, "$(CC_SRCS) $(HDRS): $(PAS_SRCS)"));
  CHECK(contains(mk,
                 "$(TP2CC) 'emit-all' '-dCPU86' '-Fu../compiler' '--' '../compiler/pp.pas' '.'"));
  CHECK(contains(mk, "$(PROGRAM): $(CC_OBJS) $(EXTRA_OBJS)"));
  CHECK(contains(mk, "p_%.o: p_%.cc $(HDRS)"));
}

void test_program_registers_unit_finalizers() {
  auto out = compile_snippet_with_init_order(
      "program demo;\n"
      "begin\n"
      "end.\n",
      {"sysutils", "classes"});
  CHECK(contains(out.impl, "::p_sysutils::tp2cc_unit_init();"));
  CHECK(contains(out.impl,
                 "std::atexit(::p_sysutils::tp2cc_unit_fini)"));
  CHECK(contains(out.impl, "::p_classes::tp2cc_unit_init();"));
  CHECK(contains(out.impl,
                 "std::atexit(::p_classes::tp2cc_unit_fini)"));
  CHECK(out.impl.find("::p_sysutils::tp2cc_unit_init();") <
        out.impl.find("::p_classes::tp2cc_unit_init();"));
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

void test_string_literal_pchar_context_uses_static_c_literal_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnames = array[0..1] of pchar;\n"
      "const\n"
      "  Names : tnames = ('tc_none', '');\n"
      "function miss : pchar;\n"
      "implementation\n"
      "function miss : pchar;\n"
      "begin\n"
      "  miss := '';\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"tc_none\"))"));
  CHECK(contains(out.header,
                 "const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"\"))"));
  CHECK(contains(out.impl,
                 "p_result = const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"\"));"));
  CHECK(!contains(out.header, "tp2cc_shortstring_literal"));
  CHECK(!contains(out.impl, "tp2cc_shortstring_literal"));
}

void test_string_literal_pchar_assignment_and_call_contexts() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure take(p: pchar);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure take(p: pchar);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var p: pchar;\n"
      "begin\n"
      "  p := 'abc';\n"
      "  take('def');\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_p = const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"abc\"));"));
  CHECK(contains(out.impl,
                 "p_take(const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"def\")));"));
  CHECK(!contains(out.impl, "tp2cc_shortstring_literal"));
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
  CHECK(contains(out.header, "enum t_tcolor : uint32_t"));
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
  CHECK(contains(out.header, "enum t_treg : int32_t {"));
  CHECK(contains(out.header,
                 "p_lo = ::std::numeric_limits<int32_t>::min(),"));
  CHECK(contains(out.header,
                 "p_hi = ::std::numeric_limits<int32_t>::max()"));
}

void test_packed_record_uses_byte_sized_enum_fields() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "{$packenum 1}\n"
      "type\n"
      "  tsmall = (a, b, c);\n"
      "  trec = packed record\n"
      "    hi : word;\n"
      "    lo : tsmall;\n"
      "    kind : tsmall;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tsmall : uint8_t {"));
  CHECK(contains(out.header, "struct [[gnu::packed]] t_trec {"));
  CHECK(contains(out.header, "t_tsmall p_lo;"));
  CHECK(contains(out.header, "t_tsmall p_kind;"));
}

void test_packenum_two_uses_word_sized_enum() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "{$packenum 2}\n"
      "type TColor = (red, green, blue);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tcolor : uint16_t"));
}

void test_minenumsize_alias_uses_packenum_rules() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "{$minenumsize 1}\n"
      "type TColor = (red, green, blue);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tcolor : uint8_t"));
}

void test_mode_tp_switches_default_enum_size_to_byte() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "{$mode tp}\n"
      "type TColor = (red, green, blue);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tcolor : uint8_t"));
}

void test_mode_objfpc_restores_default_enum_size_to_longword() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "{$mode tp}\n"
      "{$mode objfpc}\n"
      "type TColor = (red, green, blue);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tcolor : uint32_t"));
}

void test_subrange_type_uses_minimal_ordinal_storage() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbyte = 0..255;\n"
      "  tsigned = -1..1;\n"
      "  tsmall = 1..2;\n"
      "  tword = 0..65535;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using t_tbyte = uint8_t;"));
  CHECK(contains(out.header, "using t_tsigned = int8_t;"));
  CHECK(contains(out.header, "using t_tsmall = uint8_t;"));
  CHECK(contains(out.header, "using t_tword = uint16_t;"));
}

void test_subrange_type_accepts_constant_ordinal_intrinsics() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcgloc = (loc_invalid, loc_void, loc_creference, loc_reference);\n"
      "  tcgnonrefloc = low(tcgloc)..pred(loc_creference);\n"
      "  tlocation = record\n"
      "    loc: tcgloc;\n"
      "  end;\n"
      "procedure reset(var l: tlocation; lt: tcgnonrefloc);\n"
      "implementation\n"
      "procedure reset(var l: tlocation; lt: tcgnonrefloc);\n"
      "begin\n"
      "  l.loc := lt;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tcgloc"));
  CHECK(contains(out.header, "using t_tcgnonrefloc = t_tcgloc;"));
  CHECK(contains(out.impl, "p_l.p_loc = p_lt;"));
}

void test_subrange_bound_folds_ord_high_enum_type() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tdefoption = (do_one, do_two, do_three);\n"
      "  tindex = 1..ord(high(tdefoption));\n"
      "  tmap = array[tindex] of byte;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using t_tindex = uint8_t;"));
  CHECK(contains(out.header,
                 "using t_tmap = ::rt::tp2cc_Array<uint8_t, 1, "
                 "((static_cast<int32_t>(tp2cc_enum_high_tdefoption)) - (1) + 1)>;"));
  CHECK(!contains(out.header, "::rt::p_ord("));
}

void test_char_subrange_preserves_char_storage() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tletter = 'a'..'z';\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "using t_tletter = ::rt::p_char;"));
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
  CHECK(contains(out.header, "static_assert(offsetof(t_trec, p_name) == 0"));
  CHECK(contains(out.header,
                 "static_assert(sizeof(t_trec) == sizeof(::rt::tp2cc_ShortString<30>)"));
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
  CHECK(contains(out.header, "static_assert(offsetof(t_trec, p_future) == 0"));
  CHECK(contains(out.header,
                 "static_assert(sizeof(t_trec) == sizeof(::rt::tp2cc_Array<int32_t, 0, ((2) - (0) + 1)>)"));
}

void test_nested_variant_record_preserves_inner_tag_and_recursively_emits_union() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    case outertag: integer of\n"
      "      1: (\n"
      "        case innertag: integer of\n"
      "          0: (val: integer);\n"
      "          1: (str: pansichar);\n"
      "      );\n"
      "      2: (other: integer);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "int32_t p_outertag;"));
  CHECK(contains(out.header, "int32_t p_innertag;"));
  CHECK(contains(out.header, "  union {\n"
                             "    struct {\n"
                             "      int32_t p_innertag;\n"
                             "      union {\n"
                             "        struct {\n"
                             "          int32_t p_val;\n"
                             "        };\n"
                             "        struct {\n"
                             "          ::rt::p_char* p_str;\n"
                             "        };\n"
                             "      };\n"
                             "    };\n"
                             "    struct {\n"
                             "      int32_t p_other;\n"
                             "    };\n"
                             "  };\n"));
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
  CHECK(contains(out.header, "static_assert(offsetof(t_trec, p_tag) == 0"));
  CHECK(contains(out.header,
                 "static_assert(offsetof(t_trec, p_a) == sizeof(uint8_t)"));
  CHECK(contains(out.header,
                 "static_assert(offsetof(t_trec, p_b) == (sizeof(uint8_t) + sizeof(uint16_t))"));
  CHECK(contains(out.header, "static_assert(sizeof(t_trec) == (sizeof(uint8_t) + ::std::max({"));
  CHECK(!contains(out.header, " ? "));
}

void test_packed_variant_record_many_cases_uses_linear_size_max() {
  std::string src =
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = packed record\n"
      "    case byte of\n";
  for (int i = 0; i < 96; ++i) {
    src += "      " + std::to_string(i) + " : (f" + std::to_string(i) +
           " : longint);\n";
  }
  src +=
      "  end;\n"
      "implementation\n"
      "end.\n";
  auto out = compile_snippet(std::move(src));
  CHECK(contains(out.header, "static_assert(sizeof(t_trec) == ::std::max({"));
  CHECK_EQ(count_substring(out.header, "::std::max({"), size_t{1});
  CHECK(!contains(out.header, " ? "));
  CHECK(out.header.size() < 50000);
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

void test_packed_record_nested_scalar_value_uses_unaligned_load() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
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
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_load<int32_t>(::rt::tp2cc_byte_offset("
                 "static_cast<const void*>(&(p_r)), "
                 "offsetof(t_trec, p_sub) + offsetof(t_tsub, p_x)))"));
  CHECK(!contains(out.impl, "p_r.p_sub.p_x"));
}

void test_packed_record_nested_scalar_assignment_reports_error() {
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
      "procedure run;\n"
      "begin\n"
      "  r.sub.x := 1;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_packed_record_nested_scalar_var_arg_reports_error() {
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
      "procedure take(var x : longint);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "begin\n"
      "  take(r.sub.x);\n"
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
                 "using t_tmap = ::rt::tp2cc_Array<uint8_t, p_lo, ((::rt::tp2cc_ordinal_value(p_hi)) - (::rt::tp2cc_ordinal_value(p_lo)) + 1)>;"));
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
                 "using t_tmap = ::rt::tp2cc_Array<uint8_t, 0, 65536>;"));
}

void test_boolean_family_array_bounds_use_boolean_domain() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tkind = array[longbool] of pchar;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(
      out.header,
      "using t_tkind = ::rt::tp2cc_Array<::rt::p_char*, false, 2>;"));
}

void test_signed_ordinal_array_bounds_preserve_negative_low() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tmap = array[smallint] of byte;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(
      out.header,
      "using t_tmap = ::rt::tp2cc_Array<uint8_t, ::std::numeric_limits<int16_t>::min(), 65536>;"));
}

void test_imported_const_array_bounds_are_folded() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses cpubase;\n"
      "type\n"
      "  poper = ^longint;\n"
      "  tinst = record\n"
      "    oper : array[0..max_operands-1] of poper;\n"
      "  end;\n"
      "implementation\n"
      "end.\n",
      {{"cpubase.pas",
        "unit cpubase;\n"
        "interface\n"
        "const max_operands = 3;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(error_count() == before);
  CHECK(contains(out.header,
                 "::rt::tp2cc_Array<t_poper, 0, ((2) - (0) + 1)> p_oper;"));
  CHECK(!contains(out.header, "max_operands"));
}

void test_imported_const_array_bounds_use_declaring_unit_scope() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit usearr;\n"
      "interface\n"
      "uses arrunit;\n"
      "procedure setoper(var r : trec; i : longint);\n"
      "implementation\n"
      "procedure setoper(var r : trec; i : longint);\n"
      "begin\n"
      "  r.oper[i] := 1;\n"
      "end;\n"
      "end.\n",
      {{"cpubase.pas",
        "unit cpubase;\n"
        "interface\n"
        "const max_operands = 3;\n"
        "implementation\n"
        "end.\n"},
       {"arrunit.pas",
        "unit arrunit;\n"
        "interface\n"
        "uses cpubase;\n"
        "type\n"
        "  trec = record\n"
        "    oper : array[0..max_operands-1] of longint;\n"
        "  end;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_r.p_oper[p_i] = 1;"));
  CHECK(!contains(out.impl, "max_operands"));
}

void test_unsupported_fixed_array_index_reports_error_and_stays_array_typed() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tmap = array[qword] of byte;\n"
      "implementation\n"
      "end.\n");
  CHECK(error_count() > before);
  CHECK(contains(out.header,
                 "using t_tmap = ::rt::tp2cc_Array<uint8_t, 0, 1>;"));
  CHECK(!contains(out.header, "using t_tmap = uint8_t*;"));
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
  CHECK(contains(out.header, "using t_tindex = uint16_t;"));
  CHECK(contains(out.header,
                 "const auto p_maxindex = ::std::numeric_limits<uint16_t>::max();"));
  CHECK(contains(out.impl,
                 "if ((p_a == ::std::numeric_limits<uint16_t>::max()))"));
  // Array `low`/`high` lower to the dim's literal bounds (recursing
  // through `low_high_expr_for_type` -> TyArray -> dims[0] subrange).
  CHECK(contains(out.impl, "while ((0 <= 2))"));
  CHECK(!contains(out.impl, "p_high(p_a)"));
  CHECK(!contains(out.impl, "p_low(p_arr)"));
  CHECK(!contains(out.impl, "p_high(p_arr)"));
}

void test_low_high_on_set_type_uses_element_bounds() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tflag = (fa, fb, fc);\n"
      "  tflags = set of tflag;\n"
      "  tsmall = set of 2..5;\n"
      "const\n"
      "  lastflag = ord(high(tflags));\n"
      "  firstsmall = low(tsmall);\n"
      "  lastsmall = high(tsmall);\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  if ord(high(tflags)) > 31 then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "inline const int32_t p_lastflag = static_cast<int32_t>(tp2cc_enum_high_tflag);"));
  CHECK(contains(out.header, "const auto p_firstsmall = 2;"));
  CHECK(contains(out.header, "const auto p_lastsmall = 5;"));
  CHECK(contains(out.impl,
                 "if ((static_cast<int32_t>(tp2cc_enum_high_tflag) > 31))"));
  CHECK(!contains(out.header, "::rt::p_high("));
  CHECK(!contains(out.impl, "::rt::p_high("));
  CHECK(!contains(out.header, "::rt::p_ord("));
  CHECK(!contains(out.impl, "::rt::p_ord("));
}

void test_low_high_on_local_array_type_lowers_to_index_bounds() {
  // `low(arrtype)` / `high(arrtype)` where the type is a function-local
  // array alias. The arg is a type name (no value to deduce_type), so
  // the dispatch must recurse through `low_high_expr_for_type` ->
  // TyArray -> dims[0] subrange to recover the literal bounds.
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "type localarr = array[0..7] of byte;\n"
      "var i : longint;\n"
      "begin\n"
      "  for i := low(localarr) to high(localarr) do begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto tp2cc_from = (0);"));
  CHECK(contains(out.impl, "auto tp2cc_to = (7);"));
  CHECK(!contains(out.impl, "::rt::p_low("));
  CHECK(!contains(out.impl, "::rt::p_high("));
}

void test_system_qualified_low_high_lowers_like_unqualified() {
  // `system.low(int64)` / `system.high(int64)` -- Pascal allows the
  // explicit System-unit qualification on the intrinsic. Treat as
  // identical to bare `low`/`high` for emission.
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "function run(p : int64) : int64;\n"
      "implementation\n"
      "function run(p : int64) : int64;\n"
      "begin\n"
      "  if p > system.high(int64) div 2 then run := 0;\n"
      "  if p < system.low(int64) div 2 then run := 0;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::std::numeric_limits<int64_t>::max()"));
  CHECK(contains(out.impl, "::std::numeric_limits<int64_t>::min()"));
  CHECK(!contains(out.impl, "::rt::p_low("));
  CHECK(!contains(out.impl, "::rt::p_high("));
}

void test_system_qualified_runtime_exports_use_implicit_unit() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "var\n"
      "  f : text;\n"
      "  n, code : longint;\n"
      "procedure run;\n"
      "begin\n"
      "  n := system.heapsize;\n"
      "  system.val('12', n, code);\n"
      "  system.close(f);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_n = ::rt::p_heapsize;"));
  CHECK(contains(out.impl, "::rt::p_val("));
  CHECK(contains(out.impl, "::rt::p_close(p_f);"));
  CHECK(!contains(out.impl, "p_system"));
}

void test_system_member_access_respects_value_shadowing() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    heapsize : longint;\n"
      "  end;\n"
      "procedure run(system : trec; var n : longint);\n"
      "implementation\n"
      "procedure run(system : trec; var n : longint);\n"
      "begin\n"
      "  n := system.heapsize;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_n = p_system.p_heapsize;"));
  CHECK(!contains(out.impl, "::rt::p_heapsize"));
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
                 "inline t_tbuf p_magic = ::rt::tp2cc_array_literal<::rt::p_char, 1, ((4) - (1) + 1)>(::rt::tp2cc_shortstring_literal<255>("));
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

void test_nested_array_typed_const_initializes_each_array_data_member() {
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
                 "p_table = {.data = {{.data = {{.data = {1, 2, 3, 4}}, "
                 "{.data = {5, 6, 7, 8}}}}, {.data = {{.data = {9, 10, "
                 "11, 12}}, {.data = {13, 14, 15, 16}}}}}};"));
}

void test_single_record_array_typed_const_wraps_array_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tkey = (none);\n"
      "  trec = record\n"
      "    name : string[20];\n"
      "    size : longint;\n"
      "  end;\n"
      "const\n"
      "  items : array[tkey] of trec = ((name:''; size:0));\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "p_items = {.data = {{.p_name = "
                 "::rt::tp2cc_shortstring_literal<20>(), .p_size = 0}}};"));
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
  CHECK(contains(out.header, "using t_myint = int32_t;"));
  CHECK(contains(out.header, "using t_pint = int32_t*;"));
  CHECK(contains(out.header, "using t_mystr = ::rt::tp2cc_ShortString<32>;"));
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
  CHECK(contains(out.header, "using t_tname = ::rt::tp2cc_AnsiString;"));
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
  CHECK(contains(out.header, "using t_twide = uint16_t;"));
  CHECK(contains(out.header, "extern uint16_t p_w;"));
  CHECK(contains(out.header, "void p_take(uint16_t p_x);"));
  CHECK(!contains(out.header, "p_widechar"));
}

void test_ansichar_and_pansichar_builtin_maps_to_char_carriers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  ta = ansichar;\n"
      "  tpa = pansichar;\n"
      "  tbuf = array[1..3] of ansichar;\n"
      "var c : ansichar; p : pansichar; buf : tbuf;\n"
      "procedure take(c : ansichar; p : pansichar);\n"
      "implementation\n"
      "procedure take(c : ansichar; p : pansichar);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  c := ansichar(65);\n"
      "  p := pansichar(@c);\n"
      "  c := p^;\n"
      "  buf := 'ab';\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "using t_ta = ::rt::p_char;"));
  CHECK(contains(out.header, "using t_tpa = ::rt::p_char*;"));
  CHECK(contains(out.header, "extern ::rt::p_char p_c;"));
  CHECK(contains(out.header, "extern ::rt::p_char* p_p;"));
  CHECK(contains(out.header, "void p_take(::rt::p_char p_c, ::rt::p_char* p_p);"));
  CHECK(contains(out.impl, "p_c = ::rt::p_chr(65);"));
  CHECK(contains(out.impl, "p_p = ((::rt::p_char*)((&p_c)));"));
  CHECK(contains(out.impl, "p_c = ::rt::tp2cc_deref(p_p);"));
  CHECK(contains(out.impl, "p_buf = ::rt::tp2cc_array_literal<::rt::p_char"));
  CHECK(!contains(out.header, "t_ansichar"));
  CHECK(!contains(out.header, "t_pansichar"));
  CHECK(!contains(out.impl, "t_ansichar"));
  CHECK(!contains(out.impl, "t_pansichar"));
}

void test_ord_storage_view_for_char_assignment_inc_and_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type pbyte = ^byte;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var c : char; p : pbyte;\n"
      "begin\n"
      "  c := 'A';\n"
      "  inc(ord(c));\n"
      "  ord(c) := 66;\n"
      "  p := @ord(c);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_inc(::rt::tp2cc_reinterpret_storage_ref<uint8_t>(p_c))"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<uint8_t>(p_c) = 66;"));
  CHECK(contains(out.impl, "reinterpret_cast<uint8_t*>((&p_c))"));
}

void test_ord_storage_view_for_shortstring_length_byte() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var s : string[10];\n"
      "begin\n"
      "  s := 'abc';\n"
      "  dec(ord(s[0]));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_dec(::rt::tp2cc_reinterpret_storage_ref<uint8_t>(p_s[0]))"));
}

void test_chr_storage_view_for_byte_assignment_inc_and_var_arg() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure take(var c : char);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "var b : byte;\n"
      "begin\n"
      "  chr(b) := 'B';\n"
      "  inc(chr(b));\n"
      "  take(chr(b));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(
      out.impl,
      "::rt::tp2cc_reinterpret_storage_ref<::rt::p_char>(p_b) = ::rt::tp2cc_char_of('B');"));
  CHECK(contains(
      out.impl,
      "::rt::p_inc(::rt::tp2cc_reinterpret_storage_ref<::rt::p_char>(p_b))"));
  CHECK(contains(
      out.impl,
      "p_take(::rt::tp2cc_reinterpret_storage_ref<::rt::p_char>(p_b));"));
}

void test_ord_char_value_has_byte_result_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(c : char);\n"
      "implementation\n"
      "function pick(x : byte) : byte;\n"
      "begin\n"
      "  pick := x;\n"
      "end;\n"
      "function pick(x : longint) : byte;\n"
      "begin\n"
      "  pick := 0;\n"
      "end;\n"
      "procedure run(c : char);\n"
      "var b : byte;\n"
      "begin\n"
      "  b := pick(ord(c));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick(static_cast<uint8_t>(::rt::tp2cc_char_byte(p_c)));"));
  CHECK(!contains(out.impl, "::rt::p_ord("));
}

void test_ord_pchar_offset_deref_uses_char_byte_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(p : pchar);\n"
      "implementation\n"
      "procedure run(p : pchar);\n"
      "begin\n"
      "  if (ord((p+1)^)=187) and (ord((p+2)^)=191) then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_char_byte(::rt::tp2cc_deref((p_p + 1))) == 187"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_char_byte(::rt::tp2cc_deref((p_p + 2))) == 191"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref((p_p + 1)) == 187"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref((p_p + 2)) == 191"));
  CHECK(!contains(out.impl, "::rt::p_ord("));
}

void test_ord_value_result_type_follows_ordinal_operand() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tcolor = (red, green);\n"
      "procedure run(w : word; wc : widechar; bb : bytebool; wb : wordbool; c : tcolor);\n"
      "implementation\n"
      "function pick_word(x : word) : byte;\n"
      "begin\n"
      "  pick_word := 1;\n"
      "end;\n"
      "function pick_word(x : longint) : byte;\n"
      "begin\n"
      "  pick_word := 2;\n"
      "end;\n"
      "function pick_shortint(x : shortint) : byte;\n"
      "begin\n"
      "  pick_shortint := 1;\n"
      "end;\n"
      "function pick_shortint(x : byte) : byte;\n"
      "begin\n"
      "  pick_shortint := 2;\n"
      "end;\n"
      "function pick_smallint(x : smallint) : byte;\n"
      "begin\n"
      "  pick_smallint := 1;\n"
      "end;\n"
      "function pick_smallint(x : word) : byte;\n"
      "begin\n"
      "  pick_smallint := 2;\n"
      "end;\n"
      "function pick_longint(x : longint) : byte;\n"
      "begin\n"
      "  pick_longint := 1;\n"
      "end;\n"
      "function pick_longint(x : byte) : byte;\n"
      "begin\n"
      "  pick_longint := 2;\n"
      "end;\n"
      "procedure run(w : word; wc : widechar; bb : bytebool; wb : wordbool; c : tcolor);\n"
      "var b : byte;\n"
      "begin\n"
      "  b := pick_word(ord(w));\n"
      "  b := pick_word(ord(wc));\n"
      "  b := pick_shortint(ord(bb));\n"
      "  b := pick_smallint(ord(wb));\n"
      "  b := pick_longint(ord(c));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick_word(static_cast<uint16_t>(p_w));"));
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick_word(static_cast<uint16_t>(p_wc));"));
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick_shortint(static_cast<int8_t>(p_bb));"));
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick_smallint(static_cast<int16_t>(p_wb));"));
  CHECK(contains(out.impl,
                 "p_b = ::p_u::p_pick_longint(static_cast<int32_t>(p_c));"));
  CHECK(!contains(out.impl, "::rt::p_ord("));
}

void test_ord_boolean_expression_lowers_to_numeric_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tflag = (a, b);\n"
      "type tflags = set of tflag;\n"
      "procedure run;\n"
      "implementation\n"
      "var s : tflags;\n"
      "procedure take(x : longint);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "begin\n"
      "  take(ord(a in s));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_take(static_cast<uint8_t>("));
  CHECK(contains(out.impl, "(p_s).contains(p_a)"));
  CHECK(!contains(out.impl, "p_take((p_s).contains(p_a));"));
  CHECK(!contains(out.impl, "::rt::p_ord("));
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
  CHECK(contains(out.header, "using t_tcolors = ::rt::tp2cc_Set<t_tcolor>;"));
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

void test_constref_record_parameter_emits_const_reference() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    x : longint;\n"
      "  end;\n"
      "procedure take(constref r : trec);\n"
      "function make : trec;\n"
      "implementation\n"
      "procedure take(constref r : trec);\n"
      "begin\n"
      "  if r.x <> 0 then ;\n"
      "end;\n"
      "function make : trec;\n"
      "begin\n"
      "  make.x := 1;\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  take(make);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "void p_take(const t_trec& p_r);"));
  CHECK(contains(out.impl, "void p_take(const t_trec& p_r) {"));
  CHECK(contains(out.impl, "if ((p_r.p_x != 0))"));
  CHECK(contains(out.impl, "p_take(p_make());"));
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
  CHECK(contains(out.header, "void p_take(t_tarr p_a);"));
  CHECK(contains(out.impl, "void p_take(t_tarr p_a) {"));
  CHECK(contains(out.impl, "p_p = ::rt::tp2cc_array_addr(p_a);"));
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
  CHECK(contains(out.header, "void p_take(t_tarr p_a);"));
  CHECK(contains(out.impl, "p_p = ::rt::tp2cc_array_addr(p_a);"));
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
  CHECK(contains(out.header, "void p_take(t_tarr p_a);"));
  CHECK(contains(out.impl, "p_p = ::rt::tp2cc_array_addr(p_a);"));
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

void test_noreturn_directive_emits_cxx_attribute() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure fatal(code : longint); noreturn;\n"
      "implementation\n"
      "procedure fatal(code : longint); noreturn;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "[[noreturn]] void p_fatal(int32_t p_code);"));
  CHECK(contains(out.impl, "[[noreturn]] void p_fatal(int32_t p_code) {"));
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

void test_unit_qualified_trailing_default_argument_is_lowered() {
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
      "  u.note(1);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::p_u::p_note(1, 7);"));
}

void test_imported_default_argument_resolves_in_declaring_unit() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "uses api;\n"
      "procedure run;\n"
      "begin\n"
      "  note(1);\n"
      "end;\n"
      "end.\n",
      {{"base.pas",
        "unit base;\n"
        "interface\n"
        "type\n"
        "  tsymkind = (at_none, at_data);\n"
        "implementation\n"
        "end.\n"},
       {"api.pas",
        "unit api;\n"
        "interface\n"
        "uses base;\n"
        "procedure note(w : integer; kind : tsymkind = at_none);\n"
        "implementation\n"
        "procedure note(w : integer; kind : tsymkind);\n"
        "begin\n"
        "end;\n"
        "end.\n"}});
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl, "::p_api::p_note(1, ::p_base::p_at_none);"));
}

void test_imported_nil_default_argument_qualifies_procedural_type() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "uses api;\n"
      "procedure run;\n"
      "begin\n"
      "  note;\n"
      "end;\n"
      "end.\n",
      {{"api.pas",
        "unit api;\n"
        "interface\n"
        "type\n"
        "  tcallback = procedure(v : longint) of object;\n"
        "procedure note(cb : tcallback = nil);\n"
        "implementation\n"
        "procedure note(cb : tcallback);\n"
        "begin\n"
        "end;\n"
        "end.\n"}});
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl, "::p_api::p_note(::p_api::t_tcallback{});"));
}

void test_imported_default_argument_qualifies_declaring_unit_const() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "uses api;\n"
      "procedure run;\n"
      "begin\n"
      "  handle;\n"
      "end;\n"
      "end.\n",
      {{"api.pas",
        "unit api;\n"
        "interface\n"
        "type\n"
        "  thccflag = (hcc_check);\n"
        "  thccflags = set of thccflag;\n"
        "const\n"
        "  hcc_all = [hcc_check];\n"
        "procedure handle(flags : thccflags = hcc_all);\n"
        "implementation\n"
        "procedure handle(flags : thccflags);\n"
        "begin\n"
        "end;\n"
        "end.\n"}});
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl, "::p_api::p_handle(::p_api::p_hcc_all);"));
}

void test_unit_qualified_variable_assignment_is_storage_designator() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure internalerror(i : longint);\n"
      "implementation\n"
      "uses dep;\n"
      "procedure internalerror(i : longint);\n"
      "begin\n"
      "end;\n"
      "initialization\n"
      "  dep.internalerror := @internalerror;\n"
      "end.\n",
      {{"dep.pas",
        "unit dep;\n"
        "interface\n"
        "var\n"
        "  internalerror : procedure(i : longint);\n"
        "implementation\n"
        "end.\n"}});
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl, "::p_dep::p_internalerror = (&p_internalerror);"));
}

void test_external_used_unit_qualified_call_keeps_namespace_cxx_name() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "uses dos;\n"
      "procedure run;\n"
      "begin\n"
      "  dos.getenv('PATH');\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl, "::p_dos::p_getenv("));
}

void test_method_pointer_trailing_default_nil_is_lowered_as_empty_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tqueue = procedure(msg : integer) of object;\n"
      "procedure note(onqueue : tqueue = nil);\n"
      "procedure run;\n"
      "implementation\n"
      "procedure note(onqueue : tqueue);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "begin\n"
      "  note;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_note(t_tqueue{});"));
}

void test_singleton_typed_array_const() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  only : array[1..1] of longint = (7);\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "p_only = {.data = {7}};"));
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
                 "using t_arr = ::rt::tp2cc_Array<int32_t, 1, ((10) - (1) + 1)>;"));
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
  CHECK(contains(out.header, "struct [[gnu::packed]] t_hdr {"));
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
  CHECK(contains(out.header, "inline t_r p_x = {.p_a = 1};"));
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
  CHECK(contains(out.impl,
                 "p_nextlabelnr = ::rt::tp2cc_wrap_add(p_nextlabelnr, 1);"));
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
  CHECK(contains(out.impl, "throw ([&]{ auto tp2cc_ptr = new t_efoo{};"));
  CHECK(contains(out.impl, "} catch (::rt::t_exception* tp2cc_exc_1) {"));
  CHECK(contains(out.impl,
                 "dynamic_cast<t_efoo*>(tp2cc_exc_1); tp2cc_match_1_0) {"));
  CHECK(contains(out.impl, "auto p_e = tp2cc_match_1_0;"));
}

void test_raise_at_address_and_frame_metadata_is_accepted_and_discarded() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  efoo = class(exception)\n"
      "  end;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  raise efoo.create at get_caller_addr(get_frame), "
      "get_caller_frame(get_frame);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "throw ([&]{ auto tp2cc_ptr = new t_efoo{};"));
  CHECK(!contains(out.impl, "get_caller_addr"));
  CHECK(!contains(out.impl, "get_caller_frame"));
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
  CHECK(contains(out.impl, "if (auto tp2cc_match_1_0 = dynamic_cast<t_efoo*>(tp2cc_exc_1); tp2cc_match_1_0) {"));
  CHECK(contains(out.impl, "else if (auto tp2cc_match_1_1 = dynamic_cast<::rt::t_exception*>(tp2cc_exc_1); tp2cc_match_1_1) {"));
  CHECK(!contains(out.impl, "bool tp2cc_handled_1 = false;\n      else if"));
}

void test_sysutils_exception_handlers_are_qualified_and_pointer_bound() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses sysutils;\n"
      "function demo : boolean;\n"
      "implementation\n"
      "function demo : boolean;\n"
      "begin\n"
      "  try\n"
      "    raise exception.create('x');\n"
      "  except\n"
      "    on EOutOfMemory do\n"
      "      Result := true;\n"
      "    on e : EInOutError do\n"
      "      Result := e.message <> '';\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "dynamic_cast<::p_sysutils::t_eoutofmemory*>("));
  CHECK(contains(out.impl,
                 "dynamic_cast<::p_sysutils::t_einouterror*>("));
  CHECK(contains(out.impl, "auto p_e = tp2cc_match_1_1;"));
  CHECK(contains(out.impl, "p_e->p_message"));
  CHECK(!contains(out.impl, "dynamic_cast<t_eoutofmemory*>"));
  CHECK(!contains(out.impl, "dynamic_cast<t_einouterror*>"));
  CHECK(!contains(out.impl, "p_e.p_message"));
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

void test_string_typecast_from_pchar_lowers_to_shortstring() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "{$H-}\n"
      "function demo(p : pchar; i : longint) : string;\n"
      "implementation\n"
      "function demo(p : pchar; i : longint) : string;\n"
      "begin\n"
      "  demo := string(PChar(@p[i]));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, ::rt::tp2cc_shortstring_of<>(((::rt::p_char*)((&p_p[p_i])))));"));
  CHECK(!contains(out.impl, "tp2cc_ansistring_of"));
}

void test_h_plus_string_typecast_from_pchar_lowers_to_ansistring() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "{$H+}\n"
      "function demo(p : pchar; i : longint) : string;\n"
      "implementation\n"
      "function demo(p : pchar; i : longint) : string;\n"
      "begin\n"
      "  demo := string(PChar(@p[i]));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = ::rt::tp2cc_ansistring_of(((::rt::p_char*)((&p_p[p_i]))));"));
  CHECK(!contains(out.impl, "tp2cc_shortstring_of"));
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
                 "::rt::tp2cc_ShortString<14> p_emptytok = "
                 "::rt::tp2cc_shortstring_literal<14>();"));
  CHECK(contains(out.header,
                 "::rt::tp2cc_ShortString<14> p_plustok = "
                 "::rt::tp2cc_shortstring_of<14>(::rt::tp2cc_char_of('+'));"));
  CHECK(!contains(out.header,
                  "::rt::tp2cc_ShortString<14> p_plustok = "
                  "::rt::tp2cc_char_of('+');"));
}

void test_shortstring_length_literal_capacity_is_constant_folded() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "type tsection = (sec_code, sec_data);\n"
      "const secnames : array[tsection] of "
      "string[length('__DATA, __datacoal_nt,coalesced')] = "
      "('__TEXT', '__DATA, __datacoal_nt,coalesced');\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "::rt::tp2cc_Array<::rt::tp2cc_ShortString<31>"));
  CHECK(!contains(out.header, "::rt::p_length("));
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
  CHECK(contains(out.impl,
                 "p_replace(::rt::tp2cc_shortstring_ref<255>(p_s));"));
  CHECK(!contains(out.impl, "p_replace(::rt::tp2cc_shortstring_of"));
  CHECK(!contains(out.impl, "p_replace(::rt::tp2cc_ansistring_of"));
}

void test_var_shortstring_capacity_mismatch_uses_storage_ref() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure replace(out s : string);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure replace(out s : string);\n"
      "begin\n"
      "  s := 'x';\n"
      "end;\n"
      "procedure demo;\n"
      "var small : string[7];\n"
      "begin\n"
      "  replace(small);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "void p_replace(::rt::tp2cc_ShortStringPtrRef<> p_s);"));
  CHECK(contains(out.impl,
                 "p_replace(::rt::tp2cc_shortstring_ref<255>(p_small));"));
  CHECK(!contains(out.impl, "::rt::tp2cc_ShortString<> tp2cc_"));
}

void test_var_runtime_shortstring_alias_uses_storage_ref() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses dos;\n"
      "procedure replace(var s : string);\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure replace(var s : string);\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var d : dirstr;\n"
      "begin\n"
      "  replace(d);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_replace(::rt::tp2cc_shortstring_ref<255>(p_d));"));
}

void test_procvar_var_shortstring_call_uses_storage_ref() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tpred = function(var s : string) : boolean;\n"
      "procedure demo(p : tpred);\n"
      "implementation\n"
      "procedure demo(p : tpred);\n"
      "var value : string; b : boolean;\n"
      "begin\n"
      "  b := p(value);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "using t_tpred = bool (*)(::rt::tp2cc_ShortStringPtrRef<>);"));
  CHECK(contains(out.impl,
                 "p_b = p_p(::rt::tp2cc_shortstring_ref<255>(p_value));"));
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

void test_custom_operator_declarations_emit_cxx_operators_and_assignment_helpers() {
  auto out = compile_snippet_with_registry(
      "unit ops;\n"
      "interface\n"
      "type\n"
      "  tbox = record\n"
      "    v : longint;\n"
      "  end;\n"
      "operator + (const a,b : tbox) : tbox;\n"
      "operator - (const a,b : tbox) : tbox;\n"
      "operator = (const a,b : tbox) : boolean;\n"
      "operator div (const a,b : tbox) : tbox;\n"
      "operator / (const a,b : tbox) : real;\n"
      "operator := (const n : longint) : tbox;\n"
      "operator := (const n : qword) : tbox;\n"
      "operator := (const b : tbox) : longint;\n"
      "operator := (const b : tbox) : int64;\n"
      "type tarr = array[0..(high(qword) div 4)-1] of longint;\n"
      "type tbase = class\n"
      "end;\n"
      "type tnode = class(tbase)\n"
      "  value : tbox;\n"
      "end;\n"
      "procedure test;\n"
      "implementation\n"
      "operator + (const a,b : tbox) : tbox;\n"
      "begin\n"
      "  result.v := a.v + b.v;\n"
      "end;\n"
      "operator - (const a,b : tbox) : tbox;\n"
      "begin\n"
      "  result.v := a.v - b.v;\n"
      "end;\n"
      "operator = (const a,b : tbox) : boolean;\n"
      "begin\n"
      "  result := a.v = b.v;\n"
      "end;\n"
      "operator div (const a,b : tbox) : tbox;\n"
      "begin\n"
      "  result.v := a.v div b.v;\n"
      "end;\n"
      "operator / (const a,b : tbox) : real;\n"
      "begin\n"
      "  result := a.v / b.v;\n"
      "end;\n"
      "operator := (const n : longint) : tbox;\n"
      "begin\n"
      "  result.v := n;\n"
      "end;\n"
      "operator := (const n : qword) : tbox;\n"
      "begin\n"
      "  result.v := longint(n);\n"
      "end;\n"
      "operator := (const b : tbox) : longint;\n"
      "begin\n"
      "  result := b.v;\n"
      "end;\n"
      "operator := (const b : tbox) : int64;\n"
      "begin\n"
      "  result := b.v;\n"
      "end;\n"
      "procedure test;\n"
      "const limit = high(longint);\n"
      "var a,b,c : tbox; r : real; i : longint; base : tbase;\n"
      "begin\n"
      "  a := 1;\n"
      "  b := a + a;\n"
      "  c := a div b;\n"
      "  c := 1 div b;\n"
      "  c := a div sizeof(tbox);\n"
      "  c := high(qword) div b;\n"
      "  c := limit div b;\n"
      "  c := tnode(base).value div 0;\n"
      "  r := a / b;\n"
      "  r := 1 / b;\n"
      "  if a <> 0 then i := 1;\n"
      "  if tnode(base).value <> 0 then i := 2;\n"
      "  i := longint(b);\n"
      "end;\n"
      "end.\n");

  CHECK(contains(out.header, "t_tbox operator+(t_tbox p_a, t_tbox p_b);"));
  CHECK(contains(out.header, "t_tbox operator-(t_tbox p_a, t_tbox p_b);"));
  CHECK(contains(out.header, "bool operator==(t_tbox p_a, t_tbox p_b);"));
  CHECK(contains(out.header, "t_tbox tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(t_tbox p_a, t_tbox p_b);"));
  CHECK(contains(out.header, "double operator/(t_tbox p_a, t_tbox p_b);"));
  CHECK(contains(out.header, "t_tbox tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(int32_t p_n);"));
  CHECK(contains(out.header, "t_tbox tp2cc_operator_assign_params_const_name_qword_ret_name_tbox(uint64_t p_n);"));
  CHECK(contains(out.header, "int32_t tp2cc_operator_assign_params_const_name_tbox_ret_name_longint(t_tbox p_b);"));
  CHECK(contains(out.header, "int64_t tp2cc_operator_assign_params_const_name_tbox_ret_name_int64(t_tbox p_b);"));
  CHECK(!contains(out.header, "tp2cc_operator_assign_params_const_name_tbox_ret_name_int64((::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox"));
  CHECK(contains(out.impl, "t_tbox operator+(t_tbox p_a, t_tbox p_b) {"));
  CHECK(contains(out.impl, "t_tbox operator-(t_tbox p_a, t_tbox p_b) {"));
  CHECK(contains(out.impl, "bool operator==(t_tbox p_a, t_tbox p_b) {"));
  CHECK(contains(out.impl, "t_tbox tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(t_tbox p_a, t_tbox p_b) {"));
  CHECK(contains(out.impl, "double operator/(t_tbox p_a, t_tbox p_b) {"));
  CHECK(contains(out.impl, "p_a = ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(1);"));
  CHECK(contains(out.impl, "p_b = (p_a + p_a);"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(p_a, p_b);"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(1), p_b);"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(p_a, ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(static_cast<int32_t>(sizeof(t_tbox))));"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(::p_ops::tp2cc_operator_assign_params_const_name_qword_ret_name_tbox(::std::numeric_limits<uint64_t>::max()), p_b);"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(p_limit), p_b);"));
  CHECK(contains(out.impl, "p_c = ::p_ops::tp2cc_operator_div_params_const_name_tbox_const_name_tbox_ret_name_tbox(static_cast<t_tnode*>(p_base)->p_value, ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(0));"));
  CHECK(contains(out.impl, "p_r = (p_a / p_b);"));
  CHECK(contains(out.impl, "p_r = (::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(1) / p_b);"));
  CHECK(contains(out.impl, "if (::rt::p_not((p_a == ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(0))))"));
  CHECK(contains(out.impl, "if (::rt::p_not((static_cast<t_tnode*>(p_base)->p_value == ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(0))))"));
  CHECK(contains(out.impl, "p_i = ::p_ops::tp2cc_operator_assign_params_const_name_tbox_ret_name_longint(p_b);"));
}

void test_overloaded_call_result_type_uses_selected_decl() {
  auto out = compile_snippet_with_registry(
      "unit ops;\n"
      "interface\n"
      "type\n"
      "  tbox = record\n"
      "    v : longint;\n"
      "  end;\n"
      "operator * (const a,b : tbox) : tbox;\n"
      "operator := (const n : longint) : tbox;\n"
      "function pick(n : longint) : longint; overload;\n"
      "function pick(b : tbox) : tbox; overload;\n"
      "procedure test;\n"
      "implementation\n"
      "operator * (const a,b : tbox) : tbox;\n"
      "begin\n"
      "  result.v := a.v * b.v;\n"
      "end;\n"
      "operator := (const n : longint) : tbox;\n"
      "begin\n"
      "  result.v := n;\n"
      "end;\n"
      "function pick(n : longint) : longint;\n"
      "begin\n"
      "  result := n;\n"
      "end;\n"
      "function pick(b : tbox) : tbox;\n"
      "begin\n"
      "  result := b;\n"
      "end;\n"
      "procedure test;\n"
      "var a,b : tbox;\n"
      "begin\n"
      "  a := pick(b);\n"
      "  a := pick(b) * 2;\n"
      "end;\n"
      "end.\n");

  CHECK(contains(out.impl, "p_a = ::p_ops::p_pick(static_cast<t_tbox>(p_b));"));
  CHECK(contains(out.impl, "p_a = (::p_ops::p_pick(static_cast<t_tbox>(p_b)) * ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(2));"));
  CHECK(!contains(out.impl, "::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(p_pick(p_b))"));
  CHECK(!contains(out.impl, "::rt::tp2cc_wrap_mul(p_pick(p_b), 2)"));
}

void test_nested_overload_result_type_is_used_for_outer_overload() {
  auto out = compile_snippet_with_registry(
      "unit ops;\n"
      "interface\n"
      "type\n"
      "  tbox = record\n"
      "    v : longint;\n"
      "  end;\n"
      "operator + (const a,b : tbox) : tbox;\n"
      "operator := (const n : longint) : tbox;\n"
      "function pick(n : longint) : longint; overload;\n"
      "function pick(b : tbox) : tbox; overload;\n"
      "procedure take(n : longint); overload;\n"
      "procedure take(b : tbox); overload;\n"
      "procedure test;\n"
      "implementation\n"
      "operator + (const a,b : tbox) : tbox;\n"
      "begin\n"
      "  result.v := a.v + b.v;\n"
      "end;\n"
      "operator := (const n : longint) : tbox;\n"
      "begin\n"
      "  result.v := n;\n"
      "end;\n"
      "function pick(n : longint) : longint;\n"
      "begin\n"
      "  result := n;\n"
      "end;\n"
      "function pick(b : tbox) : tbox;\n"
      "begin\n"
      "  result := b;\n"
      "end;\n"
      "procedure take(n : longint);\n"
      "begin\n"
      "end;\n"
      "procedure take(b : tbox);\n"
      "begin\n"
      "end;\n"
      "procedure test;\n"
      "var b : tbox;\n"
      "begin\n"
      "  take(pick(b));\n"
      "  take(pick(b) + 1);\n"
      "end;\n"
      "end.\n");

  CHECK(contains(out.impl,
                 "p_take(static_cast<t_tbox>(::p_ops::p_pick(static_cast<t_tbox>(p_b))));"));
  CHECK(contains(out.impl,
                 "p_take(static_cast<t_tbox>((::p_ops::p_pick(static_cast<t_tbox>(p_b)) + ::p_ops::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(1))));"));
  CHECK(!contains(out.impl, "p_take(static_cast<int32_t>(::p_ops::p_pick"));
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

void test_untyped_boolean_const_and_short_circuits() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TSys = (s1, s2);\n"
      "  TSystems = set of TSys;\n"
      "const\n"
      "  ControllerSupport = true;\n"
      "  SystemsEmbedded : TSystems = [s1];\n"
      "procedure demo(system : TSys; c : longint; s : string);\n"
      "implementation\n"
      "procedure demo(system : TSys; c : longint; s : string);\n"
      "begin\n"
      "  if ControllerSupport and (system in SystemsEmbedded) and\n"
      "     (c <> 0) and (s <> '') then writeln(c);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "if ((((p_controllersupport && "
                 "(p_systemsembedded).contains(p_system)) && (p_c != 0)) &&"));
  CHECK(!contains(out.impl, "p_controllersupport & "));
}

void test_overloaded_boolean_call_result_short_circuits() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TOp = (op_a, op_b);\n"
      "  TOps = set of TOp;\n"
      "function matches(op : TOp) : boolean; overload;\n"
      "function matches(ops : TOps) : boolean; overload;\n"
      "procedure demo(op : TOp; ready : boolean);\n"
      "implementation\n"
      "function matches(op : TOp) : boolean;\n"
      "begin\n"
      "  matches := op = op_a;\n"
      "end;\n"
      "function matches(ops : TOps) : boolean;\n"
      "begin\n"
      "  matches := op_a in ops;\n"
      "end;\n"
      "procedure demo(op : TOp; ready : boolean);\n"
      "begin\n"
      "  if ready and matches(op) and matches([op]) then writeln('yes');\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_ready && ::p_u::p_matches("));
  CHECK(contains(out.impl, "&& ::p_u::p_matches(static_cast<t_tops>("));
  CHECK(!contains(out.impl, "p_ready & "));
  CHECK(!contains(out.impl, " & ::p_u::p_matches(static_cast<t_tops>("));
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

void test_nested_untyped_const_forwarding_stays_pointer_value() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure inner(const x; len : longint);\n"
      "procedure outer(const y; len : longint);\n"
      "implementation\n"
      "procedure inner(const x; len : longint);\n"
      "begin\n"
      "end;\n"
      "procedure outer(const y; len : longint);\n"
      "begin\n"
      "  inner(y, len);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_inner(p_y, p_len);"));
  CHECK(!contains(out.impl, "p_inner(::rt::tp2cc_const_untyped_ptr(p_y), p_len);"));
  CHECK(!contains(out.impl, "p_inner(((void*)&(p_y)), p_len);"));
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
  CHECK(contains(out.impl, "p_result = p_source.p_read(((void*)((&p_buffer))), p_count);"));
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
  CHECK(contains(out.impl, "p_fillchar(((void*)((&p_list[p_capacity])))"));
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
  CHECK(contains(out.impl,
                 "::rt::p_move(((const void*)((&p_list[::rt::tp2cc_wrap_add(p_index, 1)])))"));
  CHECK(contains(out.impl, "((void*)((&p_list[p_index])))"));
  CHECK(!contains(out.impl, "::rt::p_move(p_list[(p_index + 1)],"));
}

void test_indexword_nil_pointer_deref_uses_pointer_actual() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "type\n"
      "  psuper = ^word;\n"
      "var\n"
      "  buf : psuper;\n"
      "  len : longint;\n"
      "  s : word;\n"
      "begin\n"
      "  if indexword(buf^, len, s) = -1 then ;\n"
      "end;\n"
      "end.\n");
  // `indexword(buf^, len, s)` is a raw-memory helper call. Native FPC accepts
  // `buf=nil, len=0`; lowering through `tp2cc_deref(buf)` would bind a null
  // C++ reference before the helper can observe the zero count.
  CHECK(contains(out.impl, "::rt::p_indexword(((const void*)(p_buf)), p_len, p_s)"));
  CHECK(!contains(out.impl, "::rt::p_indexword(::rt::tp2cc_deref(p_buf),"));
}

void test_move_pointer_derefs_use_pointer_actuals() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(count : longint);\n"
      "implementation\n"
      "procedure run(count : longint);\n"
      "type\n"
      "  pbyte = ^byte;\n"
      "var\n"
      "  src, dst : pbyte;\n"
      "begin\n"
      "  move(src^, dst^, count);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_move(((const void*)(p_src)), ((void*)(p_dst)), p_count)"));
  CHECK(!contains(out.impl, "::rt::p_move(((void*)&(::rt::tp2cc_deref(p_src)))"));
  CHECK(!contains(out.impl, "::rt::p_move(((void*)&(::rt::tp2cc_deref(p_dst)))"));
}

void test_string_index_buffer_helpers_use_storage_addresses() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(i, count : longint);\n"
      "implementation\n"
      "procedure run(i, count : longint);\n"
      "var\n"
      "  s1, s2 : string;\n"
      "begin\n"
      "  if comparechar(s1[i], s2[i], count) = 0 then ;\n"
      "  if indexbyte(s1[i], count, byte('c')) >= 0 then ;\n"
      "end;\n"
      "end.\n");
  // Untyped buffer helpers receive the address of the Pascal storage denoted
  // by s[i], not the C++ proxy object used to model string indexing.
  CHECK(contains(out.impl,
                 "::rt::p_comparechar(((const void*)((&p_s1[p_i]))), ((const void*)((&p_s2[p_i]))), p_count)"));
  CHECK(contains(out.impl, "::rt::p_indexbyte(((const void*)((&p_s1[p_i]))), p_count,"));
  CHECK(!contains(out.impl, "::rt::p_comparechar(p_s1[p_i],"));
  CHECK(!contains(out.impl, "::rt::p_indexbyte(p_s1[p_i],"));
}

void test_string_index_char_coerces_to_shortstring_formal() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure take(const s : string);\n"
      "procedure run(i : longint);\n"
      "implementation\n"
      "procedure take(const s : string);\n"
      "begin\n"
      "end;\n"
      "procedure run(i : longint);\n"
      "var\n"
      "  s : string;\n"
      "begin\n"
      "  take(s[i]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_take(::rt::tp2cc_shortstring_of<255>(p_s[p_i]))"));
}

void test_block_io_string_index_uses_storage_addresses() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(i, count : longint);\n"
      "implementation\n"
      "procedure run(i, count : longint);\n"
      "var\n"
      "  f : file;\n"
      "  s : string;\n"
      "  transferred : longint;\n"
      "begin\n"
      "  blockwrite(f, s[i], count, transferred);\n"
      "  blockread(f, s[i], count, transferred);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_blockwrite(p_f, ((const void*)((&p_s[p_i]))), p_count, p_transferred)"));
  CHECK(contains(out.impl,
                 "::rt::p_blockread(p_f, ((void*)((&p_s[p_i]))), p_count, p_transferred)"));
  CHECK(!contains(out.impl, "::rt::p_blockwrite(p_f, p_s[p_i],"));
  CHECK(!contains(out.impl, "::rt::p_blockread(p_f, p_s[p_i],"));
}

void test_blockwrite_fixed_array_uses_const_storage_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run(count : longint);\n"
      "implementation\n"
      "procedure run(count : longint);\n"
      "var\n"
      "  f : file;\n"
      "  buf : array[0..15] of char;\n"
      "begin\n"
      "  blockwrite(f, buf, count);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_blockwrite(p_f, ((const void*)((&p_buf))), p_count)"));
  CHECK(!contains(out.impl, "::rt::p_blockwrite(p_f, p_buf, p_count)"));
}

void test_byte_array_typecast_index_read_builds_value() {
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
                 "::rt::tp2cc_reinterpret_bytes<t_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "t_t80bitarray(p_e)[p_i]"));
}

void test_local_byte_array_typecast_index_read_builds_value() {
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
                 "::rt::tp2cc_reinterpret_bytes<t_t80bitarray>(p_e)[p_i]"));
  CHECK(!contains(out.impl, "t_t80bitarray(p_e)[p_i]"));
}

void test_array_typecast_index_assignment_uses_storage_view() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..7] of longint;\n"
      "procedure poke(var b; i : longint; v : longint);\n"
      "implementation\n"
      "procedure poke(var b; i : longint; v : longint);\n"
      "begin\n"
      "  tarr(b)[i] := v;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<int32_t>(::rt::tp2cc_byte_offset(p_b, ((p_i) - (0)) * sizeof(int32_t)), p_v);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_bytes<t_tarr>(p_b)[p_i]"));
}

void test_untyped_array_value_cast_copies_caller_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..7] of byte;\n"
      "procedure copy(var b);\n"
      "implementation\n"
      "procedure copy(var b);\n"
      "var\n"
      "  a : tarr;\n"
      "begin\n"
      "  a := tarr(b);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_a = ::rt::tp2cc_reinterpret_load<t_tarr>(p_b);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_bytes<t_tarr>(p_b)"));
}

void test_untyped_record_value_cast_copies_caller_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    value : longint;\n"
      "  end;\n"
      "procedure copy(var b);\n"
      "implementation\n"
      "procedure copy(var b);\n"
      "var\n"
      "  r : trec;\n"
      "begin\n"
      "  r := trec(b);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_r = ::rt::tp2cc_reinterpret_load<t_trec>(p_b);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_copy<t_trec>(p_b)"));
}

void test_text_typecast_over_pointer_deref_keeps_file_lvalue() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure dump(arg : pointer);\n"
      "implementation\n"
      "procedure dump(arg : pointer);\n"
      "begin\n"
      "  write(text(arg^), 'x');\n"
      "  writeln(text(arg^));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_write(::rt::tp2cc_reinterpret_storage_ref<::rt::tp2cc_TextFile>(::rt::tp2cc_deref(p_arg))"));
  CHECK(contains(out.impl,
                 "::rt::p_writeln(::rt::tp2cc_reinterpret_storage_ref<::rt::tp2cc_TextFile>(::rt::tp2cc_deref(p_arg)))"));
  CHECK(!contains(out.impl, "((::rt::tp2cc_TextFile)(::rt::tp2cc_deref(p_arg)))"));
}

void test_visible_pointer_alias_cast_uses_qualified_type_name() {
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
                 "static_cast<::p_widestr::t_pcompilerwidestring>(p_raw)"));
  CHECK(!contains(out.impl,
                  "((p_pcompilerwidestring)(p_raw))"));
}

void test_local_pointer_alias_cast_uses_local_type_name() {
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
                 "::rt::tp2cc_deref(static_cast<t_psetbytes>(p_raw))[0]"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref(::rt::tp2cc_reinterpret_storage_ref<t_psetbytes>(p_raw))[0]"));
}

void test_implicit_pointer_call_argument_gets_explicit_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pint = ^longint;\n"
      "procedure take(p : pint);\n"
      "procedure demo(raw : pointer);\n"
      "implementation\n"
      "procedure take(p : pint);\n"
      "begin\n"
      "end;\n"
      "procedure demo(raw : pointer);\n"
      "begin\n"
      "  take(raw);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_take(static_cast<t_pint>(p_raw));"));
}

void test_pointer_assignment_from_pointer_result_gets_explicit_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  buf : pchar;\n"
      "begin\n"
      "  buf := allocmem(4);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_buf = static_cast<::rt::p_char*>(::rt::p_allocmem(4));"));
}

void test_pointer_builtin_cast_still_coerces_to_typed_pointer_slot() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  raw : pointer;\n"
      "  p : ppchar;\n"
      "begin\n"
      "  p := pointer(raw);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_p = static_cast<::rt::p_char**>(((void*)(p_raw)));"));
}

void test_addr_of_untyped_param_keeps_pointer_slot_semantics() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(var data);\n"
      "implementation\n"
      "procedure demo(var data);\n"
      "var\n"
      "  p : pchar;\n"
      "begin\n"
      "  p := @data;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = static_cast<::rt::p_char*>((p_data));"));
}

void test_addr_of_member_assignment_to_typed_pointer_slot_gets_explicit_cast() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "  end;\n"
      "  tcallparanode = class(tnode)\n"
      "    right : tnode;\n"
      "  end;\n"
      "  pcallparanode = ^tcallparanode;\n"
      "procedure demo(pt : tcallparanode);\n"
      "implementation\n"
      "procedure demo(pt : tcallparanode);\n"
      "var\n"
      "  oldppt : pcallparanode;\n"
      "begin\n"
      "  oldppt := @pt.right;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_oldppt = reinterpret_cast<"));
  CHECK(contains(out.impl, "&p_pt->p_right"));
}

void test_pointer_function_slot_assignment_uses_funptr_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "var\n"
      "  oldexit : pointer;\n"
      "procedure myexit;\n"
      "begin\n"
      "  exitproc := oldexit;\n"
      "end;\n"
      "procedure demo;\n"
      "begin\n"
      "  oldexit := exitproc;\n"
      "  exitproc := @myexit;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_oldexit = ::rt::tp2cc_funptr_bits(::rt::p_exitproc);"));
  CHECK(contains(
      out.impl,
      "::rt::p_exitproc = ::rt::tp2cc_funptr_from_bits<void (*)()>(p_oldexit);"));
  CHECK(contains(out.impl, "::rt::p_exitproc = (&p_myexit);"));
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
  CHECK(contains(out.header, "extern ::rt::t_dirstr p_d;"));
  CHECK(!contains(out.header, "extern p_dirstr p_d;"));
  CHECK(contains(out.impl, "::rt::t_datetime p_dt{};"));
}

void test_tdatetime_and_runtime_date_time_lower_through_rt() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  dt : tdatetime;\n"
      "begin\n"
      "  dt := time;\n"
      "  dt := date;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::t_tdatetime p_dt{};"));
  CHECK(contains(out.impl, "p_dt = ::rt::p_time();"));
  CHECK(contains(out.impl, "p_dt = ::rt::p_date();"));
}

void test_runtime_aliases_cover_currency_systemtime_and_pansistring() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  c : currency;\n"
      "  ef : texecuteflags;\n"
      "  hr : hresult;\n"
      "  pd : pdword;\n"
      "  pl : plongword;\n"
      "  sc : tsyscharset;\n"
      "  st : tsystemtime;\n"
      "  ps : pansistring;\n"
      "  pq : pqword;\n"
      "  pss : pshortstring;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::t_currency p_c{};"));
  CHECK(contains(out.impl, "::rt::t_texecuteflags p_ef{};"));
  CHECK(contains(out.impl, "::rt::t_hresult p_hr{};"));
  CHECK(contains(out.impl, "::rt::t_pdword p_pd{};"));
  CHECK(contains(out.impl, "::rt::t_plongword p_pl{};"));
  CHECK(contains(out.impl, "::rt::t_tsyscharset p_sc{};"));
  CHECK(contains(out.impl, "::rt::t_tsystemtime p_st{};"));
  CHECK(contains(out.impl, "::rt::t_pansistring p_ps{};"));
  CHECK(contains(out.impl, "::rt::t_pqword p_pq{};"));
  CHECK(contains(out.impl, "::rt::t_pshortstring p_pss{};"));
}

void test_string_comparison_uses_runtime_operator_resolution() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  s : ansistring;\n"
      "  ps : pshortstring;\n"
      "begin\n"
      "  if s <> ps^ then ;\n"
      "  if ps^ = s then ;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_string_compare(p_s, ::rt::tp2cc_deref(p_ps)) != 0"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_string_compare(::rt::tp2cc_deref(p_ps), p_s) == 0"));
  CHECK(!contains(out.impl, "p_s != ::rt::tp2cc_deref(p_ps)"));
  CHECK(!contains(out.impl, "::rt::tp2cc_deref(p_ps) == p_s"));
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
  CHECK(contains(out.impl, "::rt::t_tmethod p_m{};"));
  CHECK(!contains(out.impl, "\n  t_tmethod p_m{};"));
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

void test_sizeof_visible_type_uses_type_name_not_identifier_lookup() {
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
  CHECK(contains(out.impl, "sizeof(t_aint)"));
  CHECK(!contains(out.impl, "sizeof(::rt::p_aint)"));
}

void test_sizeof_qualified_type_uses_type_name_not_value_namespace() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses macho;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "begin\n"
      "  writeln(sizeof(macho.section));\n"
      "  writeln(sizeof(macho.counter));\n"
      "end;\n"
      "end.\n",
      {{"macho.pas",
        "unit macho;\n"
        "interface\n"
        "type\n"
        "  section = record\n"
        "    x : longint;\n"
        "  end;\n"
        "var\n"
        "  counter : longint;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "sizeof(::p_macho::t_section)"));
  CHECK(!contains(out.impl, "sizeof(::p_macho::p_section)"));
  CHECK(contains(out.impl, "sizeof(::p_macho::p_counter)"));
}

void test_unit_type_value_duplicates_across_sections_report_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  foo = longint;\n"
      "implementation\n"
      "var\n"
      "  foo : byte;\n"
      "end.\n");
  CHECK(error_count() > before);

  before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  foo : byte;\n"
      "implementation\n"
      "type\n"
      "  foo = longint;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_sizeof_own_implementation_private_qualified_names() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "type\n"
      "  foo = record\n"
      "    a : longint;\n"
      "    b : longint;\n"
      "    c : longint;\n"
      "  end;\n"
      "var\n"
      "  bar : word;\n"
      "procedure run;\n"
      "begin\n"
      "  writeln(sizeof(u.foo));\n"
      "  writeln(sizeof(u.bar));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "sizeof(t_foo)"));
  CHECK(contains(out.impl, "sizeof(::p_u::p_bar)"));
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

void test_addr_of_primitive_cast_returns_typed_pointer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  plongint = ^longint;\n"
      "function addr(var b) : plongint;\n"
      "implementation\n"
      "function addr(var b) : plongint;\n"
      "begin\n"
      "  addr := @longint(b);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = reinterpret_cast<int32_t*>(p_b);"));
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

void test_nested_aggregate_to_primitive_cast_reinterprets_source_bytes() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure fetch;\n"
      "implementation\n"
      "procedure fetch;\n"
      "type\n"
      "  tdummyarray = packed array[0..7] of byte;\n"
      "const\n"
      "  dummy1 : int64 = $4330000080000000;\n"
      "var\n"
      "  a : tdummyarray;\n"
      "  d : double;\n"
      "begin\n"
      "  a := tdummyarray(dummy1);\n"
      "  d := double(tdummyarray(dummy1));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_a = ::rt::tp2cc_reinterpret_bytes<t_tdummyarray>(p_dummy1);"));
  CHECK(contains(out.impl,
                 "p_d = ::rt::tp2cc_reinterpret_copy<double>(::rt::tp2cc_reinterpret_bytes<t_tdummyarray>(p_dummy1));"));
  CHECK(!contains(out.impl,
                  "((double)(::rt::tp2cc_reinterpret_storage_ref<t_tdummyarray>(p_dummy1)))"));
  CHECK(!contains(out.impl,
                  "::rt::tp2cc_reinterpret_storage_ref<t_tdummyarray>(p_dummy1)"));
}

void test_nested_untyped_aggregate_to_primitive_cast_reads_caller_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure fetch(var b);\n"
      "implementation\n"
      "procedure fetch(var b);\n"
      "type\n"
      "  tdummyarray = packed array[0..7] of byte;\n"
      "var\n"
      "  d : double;\n"
      "begin\n"
      "  d := double(tdummyarray(b));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_d = ::rt::tp2cc_reinterpret_copy<double>(::rt::tp2cc_reinterpret_load<t_tdummyarray>(p_b));"));
  CHECK(!contains(out.impl,
                  "::rt::tp2cc_reinterpret_bytes<t_tdummyarray>(p_b)"));
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
                 "t_titem &p_item = ::rt::tp2cc_reinterpret_ref<t_titem>(p_raw);"));
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
                 "t_pint &p_value = ::rt::tp2cc_reinterpret_storage_ref<t_pint>(p_raw);"));
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
  CHECK(contains(out.impl, "reinterpret_cast<t_plongint>("));
  CHECK(!contains(out.impl, "::rt::t_plongint("));
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
                 "t_parr &p_view = ::rt::tp2cc_reinterpret_storage_ref<t_parr>(p_raw);"));
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
  CHECK(contains(out.impl,
                 "this->p_fcount = ::rt::tp2cc_wrap_add(this->p_fcount, 1);"));
  CHECK(contains(out.impl,
                 "if ((::rt::tp2cc_string_compare(this->p_getname(), ::rt::tp2cc_shortstring_literal<255>()) != 0))"));
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
  CHECK(contains(out.impl, "int32_t t_tx::p_bar() {"));
  CHECK(contains(out.impl, "p_result = p_bar();"));
  CHECK(contains(out.impl, "p_x->p_bar();"));
  CHECK(contains(out.impl, "t_tx::p_bar();"));
}

void test_class_var_static_emission_and_access() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  public\n"
      "    y : integer;\n"
      "    class var x : integer;\n"
      "    function getx : integer;\n"
      "  end;\n"
      "  tne = class(tbase)\n"
      "  public\n"
      "    class var z : integer;\n"
      "  end;\n"
      "procedure run(b : tbase; n : tne);\n"
      "implementation\n"
      "function tbase.getx : integer;\n"
      "begin\n"
      "  getx := x;\n"
      "end;\n"
      "procedure run(b : tbase; n : tne);\n"
      "begin\n"
      "  tbase.x := 1;\n"
      "  tne.x := 2;\n"
      "  b.x := 3;\n"
      "  b.y := 4;\n"
      "  n.z := 5;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "inline static int32_t p_x{};"));
  CHECK(contains(out.header, "int32_t p_y;"));
  CHECK(contains(out.header, "inline static int32_t p_z{};"));
  CHECK(contains(out.impl, "p_result = p_x;"));
  CHECK(contains(out.impl, "t_tbase::p_x = 1;"));
  CHECK(contains(out.impl, "t_tne::p_x = 2;"));
  CHECK(contains(out.impl, "p_b->p_x = 3;"));
  CHECK(contains(out.impl, "p_b->p_y = 4;"));
  CHECK(contains(out.impl, "p_n->p_z = 5;"));
}

void test_strict_visibility_lowers_to_cxx_access_sections() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tfoo = class\n"
      "  protected\n"
      "    plain : integer;\n"
      "  strict protected\n"
      "    procedure hook;\n"
      "  strict private\n"
      "    secret : integer;\n"
      "  public\n"
      "    value : integer;\n"
      "  end;\n"
      "implementation\n"
      "procedure tfoo.hook;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "int32_t p_plain;"));
  CHECK(contains(out.header, "protected:\n  void p_hook();"));
  CHECK(contains(out.header, "private:\n  int32_t p_secret;"));
  CHECK(contains(out.header, "public:\n  int32_t p_value;"));
}

void test_metaclass_support_can_call_strict_protected_class_methods() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tfoo = class\n"
      "  strict protected\n"
      "    class procedure hidden;\n"
      "    class procedure hook; virtual;\n"
      "  public\n"
      "    class procedure run;\n"
      "  end;\n"
      "implementation\n"
      "class procedure tfoo.hidden;\n"
      "begin\n"
      "end;\n"
      "class procedure tfoo.hook;\n"
      "begin\n"
      "end;\n"
      "class procedure tfoo.run;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "friend struct tp2cc_metaclass_t_tfoo;"));
  CHECK(contains(out.header,
                 "friend tp2cc_metaclass_t_tfoo* "
                 "tp2cc_metaclass_value_t_tfoo();"));
  CHECK(contains(out.header, "protected:\n  static void p_hidden();"));
  CHECK(contains(out.header,
                 "virtual void p_hook() const { t_tfoo::p_hook(); }"));
  CHECK(contains(out.header,
                 "+[]() -> void { t_tfoo::p_hidden(); }"));
}

void test_class_var_inherited_duplicate_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  public\n"
      "    class var x : integer;\n"
      "  end;\n"
      "  tne = class(tbase)\n"
      "  public\n"
      "    class var x : integer;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(error_count() > before);
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
  CHECK(contains(out.header, "virtual ::rt::t_tclass p_classtype() const override;"));
  CHECK(contains(out.header, "virtual int32_t p_instancesize() const override;"));
  CHECK(contains(out.header, "inline ::rt::t_tclass t_titem::p_classtype() const {"));
  CHECK(contains(out.header, "inline int32_t t_titem::p_instancesize() const {"));
  CHECK(contains(out.impl, "p_result = p_classtype();"));
  CHECK(contains(out.impl, "p_result = p_instancesize();"));
}

void test_classname_uses_metaclass_descriptor_slot() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class\n"
      "  end;\n"
      "  titemclass = class of titem;\n"
      "function inst_name(x : titem) : shortstring;\n"
      "function meta_name(c : titemclass) : shortstring;\n"
      "function direct_name : shortstring;\n"
      "implementation\n"
      "function inst_name(x : titem) : shortstring;\n"
      "begin\n"
      "  inst_name := x.classname;\n"
      "end;\n"
      "function meta_name(c : titemclass) : shortstring;\n"
      "begin\n"
      "  meta_name := c.classname;\n"
      "end;\n"
      "function direct_name : shortstring;\n"
      "begin\n"
      "  direct_name := titem.classname;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "::rt::tp2cc_ShortString<> p_classname() const override { "
                 "return ::rt::tp2cc_shortstring_of<>(\"titem\"); }"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, "
                 "p_x->p_classname());"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, "
                 "p_c->p_classname());"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_shortstring_assign(p_result, "
                 "tp2cc_metaclass_value_t_titem()->p_classname());"));
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
  CHECK(contains(out.impl,
                 "::rt::t_tobject::p_free(static_cast<::rt::t_tobject*>(p_p));"));
  CHECK(!contains(out.impl, "::rt::t_tobject(p_p)"));
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
  CHECK(contains(out.header, "using t_tcb = ::rt::tp2cc_MethodPtr<void(int32_t)>;"));
  CHECK(contains(out.header,
                 "static void tp2cc_methodptr_fire_value_name_integer_ret_void"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_method_code<&t_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
  CHECK(contains(out.impl, "p_cb = t_tcb("));
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
                 "::rt::tp2cc_reinterpret_storage_ref<t_trec>(p_p).p_procpointer = p_addr;"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<t_trec>(p_p).p_s = p_selfp;"));
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
                 "p_addr = ::rt::tp2cc_method_code<&t_tobj::tp2cc_methodptr_fire_value_name_integer_ret_void"));
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
  CHECK(contains(out.impl, "::p_globals::p_token = tp2cc_from;"));
  CHECK(contains(out.impl, "if (::p_globals::p_token == tp2cc_to) break;"));
  CHECK(contains(out.impl, "::rt::p_inc(::p_globals::p_token);"));
}

void test_case_statement_lowers_to_if_chain() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(var i : longint);\n"
      "implementation\n"
      "procedure demo(var i : longint);\n"
      "begin\n"
      "  case i of\n"
      "    1 : i := 10;\n"
      "    2, 3 : i := 20;\n"
      "    4..6 : i := 30;\n"
      "  else\n"
      "    i := 99;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "switch ("));
  CHECK(contains(out.impl, "auto p_tp2cc_case_1 = p_i;"));
  CHECK(contains(out.impl, "if ((p_tp2cc_case_1 == 1)) {"));
  CHECK(contains(out.impl,
                 "else if ((p_tp2cc_case_1 == 2) || (p_tp2cc_case_1 == 3)) {"));
  CHECK(contains(out.impl,
                 "else if (((p_tp2cc_case_1 >= 4) && (p_tp2cc_case_1 <= 6))) {"));
  CHECK(contains(out.impl, "else {"));
}

void test_string_case_statement_lowers_to_if_chain() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(s : string; var i : longint);\n"
      "implementation\n"
      "procedure demo(s : string; var i : longint);\n"
      "begin\n"
      "  case s of\n"
      "    'one' : i := 1;\n"
      "    'two', 'dos' : i := 2;\n"
      "  else\n"
      "    i := 0;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "switch ("));
  CHECK(contains(out.impl, "auto p_tp2cc_case_1 = p_s;"));
  CHECK(!contains(out.impl, "p_ord("));
  CHECK(contains(
      out.impl,
      "if ((::rt::tp2cc_string_compare(p_tp2cc_case_1, ::rt::tp2cc_shortstring_literal<255>("));
  CHECK(contains(out.impl,
                 "== 0)) {"));
  CHECK(contains(
      out.impl,
      "else if ((::rt::tp2cc_string_compare(p_tp2cc_case_1, ::rt::tp2cc_shortstring_literal<255>("));
  CHECK(contains(out.impl, "|| (::rt::tp2cc_string_compare(p_tp2cc_case_1,"));
}

void test_string_case_statement_with_char_label_uses_string_compare() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(s : string; var i : longint);\n"
      "implementation\n"
      "procedure demo(s : string; var i : longint);\n"
      "begin\n"
      "  case s of\n"
      "    'a' : i := 1;\n"
      "  else\n"
      "    i := 0;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto p_tp2cc_case_1 = p_s;"));
  CHECK(!contains(out.impl, "p_ord("));
  CHECK(contains(out.impl,
                 "if ((::rt::tp2cc_string_compare(p_tp2cc_case_1, ::rt::tp2cc_shortstring_of<255>(::rt::tp2cc_char_of('a'))) == 0)) {"));
}

void test_string_case_statement_with_upcase_selector() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(s : string; var i : longint);\n"
      "implementation\n"
      "procedure demo(s : string; var i : longint);\n"
      "function to_upper(s: string): string;\n"
      "begin\n"
      "  to_upper := s;\n"
      "end;\n"
      "begin\n"
      "  case to_upper(s) of\n"
      "    'CS': i := 1;\n"
      "    'DS', 'ES': i := 2;\n"
      "  else\n"
      "    i := 0;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "switch ("));
  CHECK(!contains(out.impl, "p_ord("));
  CHECK(contains(out.impl, "::rt::tp2cc_string_compare"));
  CHECK(contains(
      out.impl,
      "if ((::rt::tp2cc_string_compare(p_tp2cc_case_1,"));
  CHECK(contains(
      out.impl,
      "else if ((::rt::tp2cc_string_compare(p_tp2cc_case_1,"));
}

void test_string_case_statement_with_builtin_upcase_selector() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(s : string; var i : longint);\n"
      "implementation\n"
      "procedure demo(s : string; var i : longint);\n"
      "begin\n"
      "  case UpCase(s) of\n"
      "    'CS': i := 1;\n"
      "    'DS', 'ES': i := 2;\n"
      "  else\n"
      "    i := 0;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "switch ("));
  CHECK(!contains(out.impl, "p_ord("));
  CHECK(contains(out.impl, "auto p_tp2cc_case_1 = ::rt::p_upcase(p_s);"));
  CHECK(contains(
      out.impl,
      "if ((::rt::tp2cc_string_compare(p_tp2cc_case_1,"));
  CHECK(contains(
      out.impl,
      "else if ((::rt::tp2cc_string_compare(p_tp2cc_case_1,"));
}

void test_char_case_statement_uses_direct_comparison() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tchar = char;\n"
      "  tcallback = procedure(p: tchar; arg: pointer) of object;\n"
      "  tlist = class\n"
      "    procedure foreachcall(cb : tcallback; arg : pointer);\n"
      "  end;\n"
      "  thost = class\n"
      "    ch : tchar;\n"
      "    list : tlist;\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "procedure tlist.foreachcall(cb : tcallback; arg : pointer); begin end;\n"
      "procedure thost.run;\n"
      "var c : tchar;\n"
      "begin\n"
      "  c := 'a';\n"
      "  case ch of\n"
      "    'a' : c := 'b';\n"
      "    'b', 'c' : c := 'd';\n"
      "  else\n"
      "    c := 'z';\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "p_ord("));
  CHECK(contains(out.impl, "auto p_tp2cc_case_1 = p_ch;"));
  CHECK(contains(out.impl,
                 "if ((p_tp2cc_case_1 == ::rt::tp2cc_char_of('a'))) {"));
  CHECK(contains(out.impl, "else if ((p_tp2cc_case_1 == ::rt::tp2cc_char_of('b')) || (p_tp2cc_case_1 == ::rt::tp2cc_char_of('c'))) {"));
}

void test_method_value_typecast_base_uses_method_code_binding() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobject = class end;\n"
      "  tcallback = procedure(p:tobject; arg:pointer) of object;\n"
      "  tlist = class\n"
      "    procedure foreachcall(cb : tcallback; arg : pointer);\n"
      "  end;\n"
      "  tbase = class\n"
      "  end;\n"
      "  tcasted = class(tbase)\n"
      "    procedure handler(p:tobject; arg:pointer);\n"
      "  end;\n"
      "  thost = class\n"
      "    list : tlist;\n"
      "    base : tbase;\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "procedure tlist.foreachcall(cb : tcallback; arg : pointer); begin end;\n"
      "procedure tcasted.handler(p:tobject; arg:pointer); begin end;\n"
      "procedure thost.run;\n"
      "begin\n"
      "  list.foreachcall(@tcasted(base).handler, nil);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "tp2cc_method_code"));
  CHECK(!contains(out.impl, "tp2cc_byte_offset"));
}

void test_method_value_cast_base_field_expression() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobject = class end;\n"
      "  tcallback = procedure(p:tobject; arg:pointer) of object;\n"
      "  tstoredsymtable = class\n"
      "    procedure testfordefaultproperty(p:tobject; arg:pointer);\n"
      "  end;\n"
      "  tlist = class\n"
      "    procedure foreachcall(cb : tcallback; arg : pointer);\n"
      "  end;\n"
      "  thelp = class\n"
      "    symtable : tstoredsymtable;\n"
      "  end;\n"
      "var\n"
      "  helperpd : thelp;\n"
      "implementation\n"
      "procedure tlist.foreachcall(cb : tcallback; arg : pointer); begin end;\n"
      "procedure tstoredsymtable.testfordefaultproperty(p:tobject; arg:pointer); begin end;\n"
      "procedure demo;\n"
      "var\n"
      "  list : tlist;\n"
      "  host : thelp;\n"
      "begin\n"
      "  list.foreachcall(@tstoredsymtable(host.symtable).testfordefaultproperty, nil);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_foreachcall(t_tcallback(::rt::tp2cc_method_code"));
  CHECK(contains(out.impl, "tp2cc_method_code"));
  CHECK(contains(out.impl, "(void*)(p_host->p_symtable)"));
  CHECK(!contains(out.impl, "tp2cc_byte_offset"));
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
  CHECK(contains(out.header, "void p_take(t_tobj &p_x);"));
  CHECK(!contains(out.header, "void p_take(t_tobj const &p_x);"));
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
                 "static ::rt::tp2cc_ShortString<> tp2cc_methodptr_get_value_name_integer_const_openarr_name_shortstring_ret_name_shortstring(void* tp2cc_self, int32_t p_nr, ::rt::tp2cc_OpenArray<::rt::tp2cc_ShortString<>> p_args)"));
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
                 "using t_tcb = void (*)(::rt::tp2cc_OpenArray<::rt::tp2cc_ShortString<>>);"));
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
                 "using t_tints = ::rt::tp2cc_DynArray<int32_t>;"));
  CHECK(contains(out.header,
                 "void p_demo(::rt::tp2cc_OpenArray<int32_t> p_xs, t_tints p_ys);"));
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
                 "::rt::p_getmem(::rt::tp2cc_reinterpret_storage_ref<t_pdata>(p_raw), 4);"));
  CHECK(contains(out.impl,
                 "::rt::p_freemem(static_cast<t_pdata>(p_raw), 4);"));
  CHECK(contains(out.impl,
                 "::rt::p_dispose(static_cast<t_pdata>(p_raw));"));
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
  CHECK(contains(out.impl, "::rt::tp2cc_Set<uint8_t>::from_list({3})"));
}

void test_cross_unit_enum_set_literal_keeps_exported_enum_type() {
  auto out = compile_snippet_with_registry(
      "unit pp;\n"
      "interface\n"
      "uses globals;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  SetFPUExceptionMask([exInvalidOp, exDenormalized, exZeroDivide,\n"
      "                       exOverflow, exUnderflow, exPrecision]);\n"
      "end;\n"
      "end.\n",
      {{"globals.pas",
        "unit globals;\n"
        "interface\n"
        "type\n"
        "  TFPUException = (exInvalidOp, exDenormalized, exZeroDivide,\n"
        "                   exOverflow, exUnderflow, exPrecision);\n"
        "  TFPUExceptionMask = set of TFPUException;\n"
        "procedure SetFPUExceptionMask(const Mask: TFPUExceptionMask);\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl,
                 "::rt::tp2cc_Set<::p_globals::t_tfpuexception>::from_list"));
  CHECK(!contains(out.impl,
                  "::rt::tp2cc_Set<::rt::t_tfpuexception>::from_list"));
}

void test_duplicate_enum_type_names_keep_set_literal_member_unit() {
  auto out = compile_snippet_with_registry(
      "unit rax86att;\n"
      "interface\n"
      "uses rax86int, raatt;\n"
      "procedure run(act : raatt.tasmtoken);\n"
      "implementation\n"
      "procedure run(act : raatt.tasmtoken);\n"
      "begin\n"
      "  if act in [as_comma, as_separator, as_end] then begin end;\n"
      "end;\n"
      "end.\n",
      {{"raatt.pas",
        "unit raatt;\n"
        "interface\n"
        "type\n"
        "  tasmtoken = (as_comma, as_separator, as_end);\n"
        "implementation\n"
        "end.\n"},
       {"rax86int.pas",
        "unit rax86int;\n"
        "interface\n"
        "type\n"
        "  tasmtoken = (as_comma, as_lbracket, as_end);\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "::rt::tp2cc_Set<::p_raatt::t_tasmtoken>"));
  CHECK(!contains(out.impl, "::rt::tp2cc_Set<::p_rax86int::t_tasmtoken>"));
}

void test_duplicate_record_type_names_keep_visible_unit_owner() {
  auto out = compile_snippet_with_registry(
      "unit main;\n"
      "interface\n"
      "uses ua, ub;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  x : tshared;\n"
      "begin\n"
      "  x.b := 1;\n"
      "end;\n"
      "end.\n",
      {{"ua.pas",
        "unit ua;\n"
        "interface\n"
        "type\n"
        "  tshared = record\n"
        "    a : longint;\n"
        "  end;\n"
        "implementation\n"
        "end.\n"},
       {"ub.pas",
        "unit ub;\n"
        "interface\n"
        "type\n"
        "  tshared = record\n"
        "    b : longint;\n"
        "  end;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "::p_ub::t_tshared p_x"));
  CHECK(contains(out.impl, "p_x.p_b = 1;"));
  CHECK(!contains(out.impl, "::p_ua::t_tshared p_x"));
}

void test_duplicate_alias_type_names_keep_visible_unit_owner() {
  auto out = compile_snippet_with_registry(
      "unit main;\n"
      "interface\n"
      "uses ua, ub;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  x : talias;\n"
      "begin\n"
      "  x := 300;\n"
      "end;\n"
      "end.\n",
      {{"ua.pas",
        "unit ua;\n"
        "interface\n"
        "type\n"
        "  talias = byte;\n"
        "implementation\n"
        "end.\n"},
       {"ub.pas",
        "unit ub;\n"
        "interface\n"
        "type\n"
        "  talias = word;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "::p_ub::t_talias p_x"));
  CHECK(!contains(out.impl, "::p_ua::t_talias p_x"));
}

void test_runtime_enum_members_resolve_explicitly() {
  auto out = compile_snippet_with_registry(
      "unit compiler;\n"
      "interface\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  SetExceptionMask([exInvalidOp, exDenormalized, exZeroDivide,\n"
      "                    exOverflow, exUnderflow, exPrecision]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_setexceptionmask("));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_Set<::rt::t_tfpuexception>::from_list"));
  CHECK(contains(out.impl, "::rt::p_exinvalidop"));
  CHECK(contains(out.impl, "::rt::p_exprecision"));
  CHECK(!contains(out.impl, "tp2cc_Set<enum"));
}

void test_sysutils_executeprocess_accepts_execute_flags() {
  auto out = compile_snippet_with_registry(
      "unit cfileutl;\n"
      "interface\n"
      "uses sysutils;\n"
      "function RunIt(const path: ansistring; flags: TExecuteFlags = []): longint;\n"
      "implementation\n"
      "function RunIt(const path: ansistring; flags: TExecuteFlags): longint;\n"
      "begin\n"
      "  RunIt := SysUtils.ExecuteProcess(path, 'arg', flags);\n"
      "end;\n"
      "procedure Demo;\n"
      "begin\n"
      "  RunIt('tool');\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "int32_t p_runit(::rt::tp2cc_AnsiString p_path, "
                 "::rt::t_texecuteflags p_flags);"));
  CHECK(contains(out.impl, "::p_sysutils::p_executeprocess("));
  CHECK(contains(out.impl, "::rt::tp2cc_Set<::rt::t_texecuteflag>{}"));
}

void test_ansicomparefilename_resolves_explicitly() {
  auto out = compile_snippet_with_registry(
      "unit comprsrc;\n"
      "interface\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  i : longint;\n"
      "begin\n"
      "  i := AnsiCompareFileName('a', 'b');\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::p_ansicomparefilename("));
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

void test_named_set_const_assigns_to_compatible_set_via_runtime_helper() {
  auto out = compile_snippet_with_registry(
      "unit cpupara;\n"
      "interface\n"
      "type\n"
      "  tsmall = 0..15;\n"
      "  tsmallset = set of tsmall;\n"
      "  tbyteset = set of byte;\n"
      "const\n"
      "  rs_r0 = 0;\n"
      "  rs_r3 = 3;\n"
      "  rs_r12 = 12;\n"
      "  rs_r15 = 15;\n"
      "  volatile_intregisters = [rs_r0..rs_r3, rs_r12..rs_r15];\n"
      "function take : tbyteset;\n"
      "implementation\n"
      "function take : tbyteset;\n"
      "begin\n"
      "  take := volatile_intregisters;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "::rt::tp2cc_Set<int32_t> tp2cc_set{};"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_set_cast<t_tbyteset>(p_volatile_intregisters)"));
}

void test_compatible_set_actual_stays_viable_in_overload_resolution() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsmall = set of 0..7;\n"
      "  tbytes = set of byte;\n"
      "procedure take(xs : tbytes); overload;\n"
      "procedure take(n : longint); overload;\n"
      "procedure run(s : tsmall);\n"
      "implementation\n"
      "procedure take(xs : tbytes); begin end;\n"
      "procedure take(n : longint); begin end;\n"
      "procedure run(s : tsmall);\n"
      "begin\n"
      "  take(s);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(contains(out.impl, "p_u::p_take(static_cast<t_tbytes>(p_s))"));
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

void test_local_var_inline_anon_enum_resolves_members() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var mode : (lookup, search);\n"
      "begin\n"
      "  mode := lookup;\n"
      "  if mode = lookup then begin end;\n"
      "end;\n"
      "end.\n");
  CHECK(!contains(out.impl, "::rt::p_lookup"));
  CHECK(contains(out.impl, "p_mode = p_lookup"));
}

void test_set_of_inline_enum_uses_named_carrier() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tflags = set of (red, green, blue);\n"
      "var flags : tflags;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  flags := [red, blue];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tflags_enum0 :"));
  CHECK(contains(out.header,
                 "using t_tflags = ::rt::tp2cc_Set<t_tflags_enum0>;"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_Set<t_tflags_enum0>::from_list({p_red, p_blue})"));
  CHECK(!contains(out.header, "::rt::tp2cc_Set<enum"));
}

void test_record_field_inline_enum_uses_unit_carrier() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    state : (idle, busy);\n"
      "  end;\n"
      "var r : trec;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  r.state := busy;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_trec_enum0 :"));
  CHECK(contains(out.header, "t_trec_enum0 p_state;"));
  CHECK(contains(out.impl, "p_r.p_state = p_busy;"));
  CHECK(!contains(out.impl, "::rt::p_busy"));
}

void test_unresolved_free_identifier_reports_error_without_rt_fallback() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var x : longint;\n"
      "begin\n"
      "  x := missing;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
  CHECK(contains(out.impl, "p_x = p_missing;"));
  CHECK(!contains(out.impl, "::rt::p_missing"));
}

void test_runtime_math_surface_resolves_explicitly() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var r : double; l : longint;\n"
      "begin\n"
      "  r := abs(r) + int(pi) + sqrt(exp(ln(1.0)));\n"
      "  l := round(sqr(r)) + trunc(arctan(r));\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::p_abs("));
  CHECK(contains(out.impl, "::rt::p_int(::rt::p_pi)"));
  CHECK(contains(out.impl, "::rt::p_sqrt(::rt::p_exp(::rt::p_ln("));
  CHECK(contains(out.impl, "::rt::p_round(::rt::p_sqr("));
  CHECK(contains(out.impl, "::rt::p_trunc(::rt::p_arctan("));
}

void test_runtime_endian_helpers_resolve_explicitly() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var l : longint; w : word;\n"
      "begin\n"
      "  l := NtoBE(l) + BEtoN(l) + NtoLE(l) + LEtoN(l);\n"
      "  w := NtoBE(w);\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::p_ntobe("));
  CHECK(contains(out.impl, "::rt::p_beton("));
  CHECK(contains(out.impl, "::rt::p_ntole("));
  CHECK(contains(out.impl, "::rt::p_leton("));
}

void test_runtime_rotate_helpers_resolve_explicitly() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var b : byte; w : word; d : dword; q : qword;\n"
      "begin\n"
      "  b := RorByte(b) + RolByte(b, 3);\n"
      "  w := RorWord(w) + RolWord(w, 3);\n"
      "  d := RorDWord(d) + RolDWord(d, 3);\n"
      "  q := RorQWord(q) + RolQWord(q, 3);\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::p_rorbyte("));
  CHECK(contains(out.impl, "::rt::p_rolbyte("));
  CHECK(contains(out.impl, "::rt::p_rorword("));
  CHECK(contains(out.impl, "::rt::p_rolword("));
  CHECK(contains(out.impl, "::rt::p_rordword("));
  CHECK(contains(out.impl, "::rt::p_roldword("));
  CHECK(contains(out.impl, "::rt::p_rorqword("));
  CHECK(contains(out.impl, "::rt::p_rolqword("));
}

void test_runtime_string_and_memory_helpers_resolve_explicitly() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var s : string; p : pchar; c : char; b : boolean; x : longint;\n"
      "begin\n"
      "  fillbyte(s, sizeof(s), 0);\n"
      "  initialize(x);\n"
      "  b := directoryexists(s);\n"
      "  s := inttostr(x);\n"
      "  setstring(s, p, x);\n"
      "  s := trim(s);\n"
      "  p := reallocmem(p, x);\n"
      "  p := strrscan(p, c);\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::p_fillbyte("));
  CHECK(contains(out.impl, "::rt::p_initialize("));
  CHECK(contains(out.impl, "::rt::p_directoryexists("));
  CHECK(contains(out.impl, "::rt::p_inttostr("));
  CHECK(contains(out.impl, "::rt::p_setstring("));
  CHECK(contains(out.impl, "::rt::p_trim("));
  CHECK(contains(out.impl,
                 "p_p = static_cast<::rt::p_char*>(::rt::p_reallocmem(p_p, p_x));"));
  CHECK(contains(out.impl, "::rt::p_strrscan("));
}

void test_sysutils_setdirseparators_resolves_qualified_and_unqualified() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses sysutils;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var s : string;\n"
      "begin\n"
      "  s := SetDirSeparators(s);\n"
      "  s := SysUtils.SetDirSeparators(s);\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "::rt::p_setdirseparators("));
  CHECK(contains(out.impl, "::p_sysutils::p_setdirseparators("));
}

void test_prefetch_intrinsic_statement_is_noop() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var x : longint; p : ^longint;\n"
      "begin\n"
      "  prefetch(x);\n"
      "  system.prefetch(p^);\n"
      "  x := 1;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_x = 1;"));
  CHECK(!contains(out.impl, "prefetch("));
  CHECK(!contains(out.impl, "p_prefetch("));
}

void test_and_with_not_of_xor_short_circuits() {
  // Three chained Pascal `and`s where the third's RHS is `not(bool xor
  // bool)`. Without recognising xor-of-bools as bool, the last `and`
  // falls back to bitwise `&`, evaluating both sides unconditionally
  // -- and an earlier clause may have downcast a node to a derived
  // type, which is UB when the runtime type doesn't match. Both `and`s
  // must emit `&&` so short-circuit guards the downcast.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function f(a, b : longint) : boolean;\n"
      "implementation\n"
      "function f(a, b : longint) : boolean;\n"
      "begin\n"
      "  f := (a = 1) and (b = 2) and not ((a = 3) xor (b = 4));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "&& ::rt::p_not("));
  CHECK(!contains(out.impl, ") & ::rt::p_not("));
}

void test_r_plus_routes_narrowing_assignment_through_range_check() {
  // `{$R+}` raises ERangeError on narrowing assignment. The bootstrap
  // path that hits this: `value_currency : currency := bestreal` in
  // ncon.pas's trealconstnode.create. Real -> integer assignments
  // under R+ must route through the range-check helper; under R-
  // they still need a truncation helper because plain `(int)real`
  // is UB in C++ when out of range (i386 silently returns the
  // "indefinite integer" value, which the helper reproduces).
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure narrow_r(v : extended);\n"
      "procedure narrow_n(v : extended);\n"
      "implementation\n"
      "{$R+}\n"
      "procedure narrow_r(v : extended);\n"
      "var c : int64;\n"
      "begin\n"
      "  c := v;\n"
      "end;\n"
      "{$R-}\n"
      "procedure narrow_n(v : extended);\n"
      "var c : int64;\n"
      "begin\n"
      "  c := v;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_range_check_assign<int64_t>(p_v)"));
  CHECK(contains(out.impl, "::rt::tp2cc_real_to_int_trunc<int64_t>(p_v)"));
}

void test_q_minus_routes_signed_negate_through_wrap_helper() {
  // Plain `-x` on a signed integer is UB in C++ when x is the type's
  // minimum. nmat.pas's tunaryminusnode.simplify hits this on
  // `-value_currency` after a real->int truncation has produced
  // INT64_MIN; fpc native gets away with it because i386's `neg`
  // wraps. Match that without UB by routing Q- signed negation
  // through tp2cc_wrap_negate.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function neg_q(v : int64) : int64;\n"
      "function neg_n(v : int64) : int64;\n"
      "implementation\n"
      "{$Q+}\n"
      "function neg_q(v : int64) : int64;\n"
      "begin\n"
      "  neg_q := -v;\n"
      "end;\n"
      "{$Q-}\n"
      "function neg_n(v : int64) : int64;\n"
      "begin\n"
      "  neg_n := -v;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_negate_checked(p_v)"));
  CHECK(contains(out.impl, "::rt::tp2cc_wrap_negate(p_v)"));
}

void test_q_plus_routes_integer_arith_through_checked_helpers() {
  // `{$Q+}` enables Pascal's runtime overflow check on integer
  // arithmetic. Translated code under Q+ must route through helpers that
  // raise `p_eintoverflow`; Q- routes through wrapping helpers to avoid C++
  // signed-overflow UB.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function add_q(a, b : longint) : longint;\n"
      "function add_n(a, b : longint) : longint;\n"
      "implementation\n"
      "{$Q+}\n"
      "function add_q(a, b : longint) : longint;\n"
      "begin\n"
      "  add_q := a + b;\n"
      "end;\n"
      "{$Q-}\n"
      "function add_n(a, b : longint) : longint;\n"
      "begin\n"
      "  add_n := a + b;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_add_checked(p_a, p_b)"));
  CHECK(contains(out.impl, "::rt::tp2cc_wrap_add(p_a, p_b)"));
}

void test_q_minus_routes_signed_binary_arith_through_wrap_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function hash_step(result : longword; ch : byte) : longword;\n"
      "implementation\n"
      "{$Q-}\n"
      "function hash_step(result : longword; ch : byte) : longword;\n"
      "begin\n"
      "  hash_step := longword(longint(result shl 5) - longint(result)) xor ch;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_wrap_sub("));
  CHECK(!contains(out.impl, ") - ((int32_t)(p_result))"));
}

void test_qword_const_cast_produces_64bit_literal_for_shifts() {
  // n386mat.pas computes `qword(1) shl (32+l) div d` -- the explicit
  // qword cast tells fpc to do the shift in 64 bits. A folded literal
  // without a width tag stays at C++ `int`, and `1 << (32+l)` with
  // l>0 hits 32-bit shift-count UB.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function shl_test(l : longint; d : longint) : qword;\n"
      "implementation\n"
      "function shl_test(l : longint; d : longint) : qword;\n"
      "begin\n"
      "  shl_test := qword(1) shl (32 + l) div d;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "((uint64_t)(1))"));
  CHECK(!contains(out.impl, " (1 << "));
}

void test_shift_ops_lower_through_pascal_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function rotl(d : dword; b : byte) : dword;\n"
      "function widen(v : shortint; n : byte) : longint;\n"
      "implementation\n"
      "function rotl(d : dword; b : byte) : dword;\n"
      "begin\n"
      "  rotl := (d shr (32-b)) or (d shl b);\n"
      "end;\n"
      "function widen(v : shortint; n : byte) : longint;\n"
      "begin\n"
      "  widen := v shl n;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::p_shr<uint32_t>(p_d, ::rt::tp2cc_wrap_sub(32, p_b))"));
  CHECK(contains(out.impl, "::rt::p_shl<uint32_t>(p_d, p_b)"));
  CHECK(contains(out.impl, "::rt::p_shl<int32_t>(p_v, p_n)"));
  CHECK(!contains(out.impl, ">> (32 - p_b)"));
  CHECK(!contains(out.impl, "<< p_b"));
}

void test_integer_div_mod_lower_through_pascal_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure demo(a, b : longint; var q, r : longint);\n"
      "implementation\n"
      "procedure demo(a, b : longint; var q, r : longint);\n"
      "begin\n"
      "  q := a div b;\n"
      "  r := a mod b;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_q = ::rt::tp2cc_int_div(p_a, p_b);"));
  CHECK(contains(out.impl, "p_r = ::rt::tp2cc_int_mod(p_a, p_b);"));
  CHECK(!contains(out.impl, "p_q = (p_a / p_b);"));
  CHECK(!contains(out.impl, "p_r = (p_a % p_b);"));
}

void test_addr_of_pointer_deref_field_uses_offsetof_arithmetic() {
  // `ptrint(@p^.field) - ptrint(p)` computes a field offset even when `p`
  // is nil. Lowering through `&deref(p).field` would bind a C++ reference
  // to `*p`; use integer arithmetic plus `offsetof` so the pointer value is
  // not dereferenced.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    a : longint;\n"
      "    b : longint;\n"
      "  end;\n"
      "  prec = ^trec;\n"
      "function offset(p : prec) : longint;\n"
      "implementation\n"
      "function offset(p : prec) : longint;\n"
      "begin\n"
      "  offset := ptrint(@p^.b) - ptrint(p);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "reinterpret_cast<int32_t*>(::rt::tp2cc_pointer_byte_offset(p_p, offsetof("));
  CHECK(!contains(out.impl, "&::rt::tp2cc_deref(p_p).p_b"));
}

void test_addr_of_array_value_uses_context_selecting_proxy() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbuf = array[0..3] of char;\n"
      "  pbuf = ^tbuf;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure demo;\n"
      "var\n"
      "  buf : tbuf;\n"
      "  pc : pchar;\n"
      "  pa : pbuf;\n"
      "begin\n"
      "  pc := @buf;\n"
      "  pa := @buf;\n"
      "end;\n"
      "end.\n");
  // Native FPC accepts the same raw `@buf` in both pointer-to-element and
  // pointer-to-array contexts. Preserve that as one proxy value instead of
  // hardwiring `@buf` to `@buf[0]` or `^tbuf`.
  CHECK_EQ(count_substring(out.impl, "::rt::tp2cc_array_addr(p_buf)"),
           static_cast<size_t>(2));
  CHECK(!contains(out.impl, "((::rt::p_char*)(p_buf))"));
  CHECK(!contains(out.impl, "(&p_buf)"));
}

void test_addr_of_pointer_deref_array_field_uses_offsetof_proxy() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcode = array[0..3] of char;\n"
      "  pcode = ^tcode;\n"
      "  trec = record\n"
      "    code : tcode;\n"
      "  end;\n"
      "  prec = ^trec;\n"
      "procedure demo(p : prec; var pc : pchar; var pa : pcode);\n"
      "implementation\n"
      "procedure demo(p : prec; var pc : pchar; var pa : pcode);\n"
      "begin\n"
      "  pc := @p^.code;\n"
      "  pa := @p^.code;\n"
      "end;\n"
      "end.\n");
  // `@p^.code` must keep the nil-safe `uintptr_t + offsetof` lowering, but
  // it also has to stay usable as both `pchar` and `^tcode`. The runtime
  // proxy keeps that address ambiguity until the use site converts it.
  CHECK(contains(out.impl,
                 "::rt::tp2cc_array_addr(reinterpret_cast<t_tcode*>(::rt::tp2cc_pointer_byte_offset(p_p, offsetof("));
  CHECK(!contains(out.impl, "&::rt::tp2cc_deref(p_p).p_code"));
  CHECK(!contains(out.impl, "((::rt::p_char*)("));
}

void test_addr_of_pointer_deref_array_index_uses_pointer_offset() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[0..9] of char;\n"
      "  ptarr = ^tarr;\n"
      "procedure run(p : ptarr; i : longint; var outp : pchar);\n"
      "implementation\n"
      "procedure run(p : ptarr; i : longint; var outp : pchar);\n"
      "begin\n"
      "  outp := @p^[i];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_outp = reinterpret_cast<::rt::p_char*>(::rt::tp2cc_pointer_byte_offset(p_p, ((p_i) - (0)) * sizeof(::rt::p_char)));"));
  CHECK(!contains(out.impl, "&::rt::tp2cc_deref(p_p)[p_i]"));
}

void test_addr_of_dynamic_array_targets_array_handle_not_data_proxy() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbytes = array of byte;\n"
      "  pbytes = ^tbytes;\n"
      "  trec = record\n"
      "    use : tbytes;\n"
      "  end;\n"
      "  prec = ^trec;\n"
      "procedure demo(var a : tbytes; r : prec; var pa, pr : pbytes);\n"
      "implementation\n"
      "procedure demo(var a : tbytes; r : prec; var pa, pr : pbytes);\n"
      "begin\n"
      "  pa := @a;\n"
      "  pr := @r^.use;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_pa = (&p_a);"));
  CHECK(contains(out.impl,
                 "p_pr = reinterpret_cast<t_tbytes*>(::rt::tp2cc_pointer_byte_offset(p_r, offsetof("));
  CHECK(!contains(out.impl, "tp2cc_array_addr(p_a)"));
  CHECK(!contains(out.impl, "tp2cc_array_addr(reinterpret_cast<t_tbytes*"));
}

void test_set_to_int_cast_uses_endian_safe_helper() {
  // ncgrtti.pas computes interface-flag words as `longint([flagA, flagB])`.
  // A plain C-style cast would invoke a non-existent
  // `tp2cc_Set::operator int`, and a memcpy would tie the result to
  // host endianness. The runtime helper packs element i into bit i
  // explicitly, regardless of host byte order.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tflag = (fa, fb, fc, fd);\n"
      "function pack : longint;\n"
      "implementation\n"
      "function pack : longint;\n"
      "begin\n"
      "  pack := longint([fa, fc]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_set_to_int<int32_t>("));
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
  // `const void*&` in thunks or procvar signatures. But the parameter itself
  // is read-only storage, so the generated C++ surface should say `const
  // void*`.
  CHECK(contains(out.header,
                 "int32_t p_write(const void* p_buffer, int32_t p_count);"));
  CHECK(contains(out.header,
                 "static int32_t tp2cc_methodptr_write_const_untyped_value_name_longint_ret_name_longint(void* tp2cc_self, const void* p_buffer, int32_t p_count)"));
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
  // locations, and byte-buffer writers rely on that distinction. The pointed
  // bytes now lower as the raw pointer value rather than `&tp2cc_deref(p)`, so
  // the zero-count/nil case stays out of C++ UB.
  CHECK(contains(out.impl, "p_sink(((const void*)((&p_p))), 1);"));
  CHECK(contains(out.impl, "p_sink(((const void*)(p_p)), 1);"));
  CHECK(!contains(out.impl, "p_sink(((void*)&(::rt::tp2cc_deref(p_p))), 1);"));
}

void test_untyped_const_pointer_assignment_drops_qualifier_explicitly() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure load(const b);\n"
      "implementation\n"
      "procedure load(const b);\n"
      "var\n"
      "  p : pchar;\n"
      "begin\n"
      "  p := b;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_p = reinterpret_cast<::rt::p_char*>(const_cast<void*>("));
}

void test_addr_of_untyped_const_pointer_assignment_drops_qualifier_explicitly() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure load(const b);\n"
      "implementation\n"
      "procedure load(const b);\n"
      "var\n"
      "  p : pchar;\n"
      "begin\n"
      "  p := @b;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_p = reinterpret_cast<::rt::p_char*>(const_cast<void*>("));
}

void test_untyped_const_pointer_cast_drops_qualifier_explicitly() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tobj = object\n"
      "    procedure load(const b);\n"
      "  end;\n"
      "implementation\n"
      "procedure tobj.load(const b);\n"
      "var\n"
      "  p : pchar;\n"
      "begin\n"
      "  p := pchar(@b);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_p = reinterpret_cast<::rt::p_char*>(const_cast<void*>("));
}

void test_untyped_const_temporary_uses_addressable_helper() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    a, b : longint;\n"
      "  end;\n"
      "  twriter = object\n"
      "    procedure sink(const b; len : longint);\n"
      "  end;\n"
      "function buildrec(x : longint) : trec;\n"
      "procedure demo;\n"
      "implementation\n"
      "procedure twriter.sink(const b; len : longint);\n"
      "begin\n"
      "end;\n"
      "function buildrec(x : longint) : trec;\n"
      "begin\n"
      "end;\n"
      "procedure demo;\n"
      "var\n"
      "  w : twriter;\n"
      "begin\n"
      "  w.sink(buildrec(42), sizeof(trec));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_const_untyped_ptr(::p_u::p_buildrec(42))"));
  CHECK(!contains(out.impl, "&(::p_u::p_buildrec(42))"));
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
  CHECK(contains(out.header, "struct t_tnode : public ::rt::t_tobject {"));
  CHECK(contains(out.header, "using inherited = ::rt::t_tobject;"));
  CHECK(contains(out.header, "t_tnode* p_next;"));
  CHECK(contains(out.header, "extern t_tnode* p_head;"));
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
  CHECK_EQ(count_substring(out.header, "struct t_tnode : public ::rt::t_tobject {"),
           1u);
  CHECK(contains(out.header, "t_tnode* p_next;"));
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
  CHECK(contains(out.header, "struct t_echild : public t_ebase {"));
  CHECK(contains(out.header, "using inherited = t_ebase;"));
  CHECK(contains(out.header, "extern t_echild* p_child;"));
}

void test_abstract_method_emits_fail_fast_virtual_body() {
  // `virtual; abstract;` Pascal methods have no source-side body, so
  // the emitter must provide one inline -- otherwise the vtable slot
  // refers to an undefined symbol and the link fails. Pure-virtual
  // (`= 0`) is not used because some Pascal classes are still
  // instantiated despite carrying placeholder abstract methods; the
  // fail-fast `abort()` body keeps the class constructible while
  // turning any actual call into an immediate stop.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit; virtual; abstract;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header,
                 "virtual void p_doit() { ::std::abort(); }"));
  CHECK(!contains(out.header, "virtual void p_doit() = 0;"));
  CHECK(!contains(out.header, "virtual void p_doit();"));
}

void test_final_virtual_method_emits_cxx_final() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure seal; virtual; final;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure childseal; virtual; final;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.seal; begin end;\n"
      "procedure tchild.childseal; begin end;\n"
      "end.\n");
  CHECK(contains(out.header, "virtual void p_seal() final;"));
  CHECK(contains(out.header, "virtual void p_childseal() final;"));
}

void test_final_override_method_emits_override_final() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure seal; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure seal; override; final;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.seal; begin end;\n"
      "procedure tchild.seal; begin end;\n"
      "end.\n");
  CHECK(contains(out.header, "virtual void p_seal() override final;"));
}

void test_final_nonvirtual_method_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure seal; final;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.seal; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_override_final_method_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure seal; virtual; final;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure seal; override;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.seal; begin end;\n"
      "procedure tchild.seal; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_reintroduce_same_signature_inherited_virtual_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit(n : longint); virtual;\n"
      "  end;\n"
      "  tmid = class(tbase)\n"
      "  end;\n"
      "  tchild = class(tmid)\n"
      "    procedure doit(n : longint); reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit(n : longint); begin end;\n"
      "procedure tchild.doit(n : longint); begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_reintroduce_same_signature_inherited_virtual_constructor_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create; reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.create; begin end;\n"
      "constructor tchild.create; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_reintroduce_different_signature_is_still_accepted() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit(n : longint); virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure doit(s : shortstring); reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit(n : longint); begin end;\n"
      "procedure tchild.doit(s : shortstring); begin end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
}

void test_plain_different_signature_inherited_virtual_is_accepted() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit(n : longint); virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure doit(s : shortstring);\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit(n : longint); begin end;\n"
      "procedure tchild.doit(s : shortstring); begin end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
}

void test_explicit_reintroduce_same_signature_inherited_virtual_method_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    function getcopy : tbase; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    function getcopy : tbase; virtual; reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "function tbase.getcopy : tbase; begin getcopy := nil; end;\n"
      "function tchild.getcopy : tbase; begin getcopy := nil; end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_explicit_reintroduce_same_signature_inherited_virtual_constructor_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create; virtual; reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.create; begin end;\n"
      "constructor tchild.create; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_explicit_reintroduce_nonvirtual_same_signature_method_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure doit; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure doit; reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit; begin end;\n"
      "procedure tchild.doit; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_explicit_reintroduce_nonvirtual_same_signature_constructor_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create; reintroduce;\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.create; begin end;\n"
      "constructor tchild.create; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_plain_same_signature_inherited_virtual_method_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    function getcopy : tbase; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    function getcopy : tbase; virtual;\n"
      "  end;\n"
      "implementation\n"
      "function tbase.getcopy : tbase; begin getcopy := nil; end;\n"
      "function tchild.getcopy : tbase; begin getcopy := nil; end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_plain_same_signature_inherited_virtual_constructor_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create;\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.create; begin end;\n"
      "constructor tchild.create; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_override_same_signature_inherited_virtual_method_is_accepted() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    function getcopy : tbase; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    function getcopy : tbase; override;\n"
      "  end;\n"
      "implementation\n"
      "function tbase.getcopy : tbase; begin getcopy := nil; end;\n"
      "function tchild.getcopy : tbase; begin getcopy := nil; end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.header, "virtual t_tbase* p_getcopy() override;"));
}

void test_override_same_signature_inherited_virtual_constructor_is_accepted() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create; override;\n"
      "  end;\n"
      "implementation\n"
      "constructor tbase.create; begin end;\n"
      "constructor tchild.create; begin end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.header, "bool p_create() override;"));
  CHECK(contains(out.impl, "bool t_tchild::p_create()"));
}

void test_object_virtual_same_signature_inherited_virtual_is_accepted() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = object\n"
      "    procedure doit; virtual;\n"
      "  end;\n"
      "  tchild = object(tbase)\n"
      "    procedure doit; virtual;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit; begin end;\n"
      "procedure tchild.doit; begin end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.header, "virtual void p_doit() override;"));
}

void test_object_plain_same_signature_inherited_virtual_reports_error() {
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = object\n"
      "    procedure doit; virtual;\n"
      "  end;\n"
      "  tchild = object(tbase)\n"
      "    procedure doit;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.doit; begin end;\n"
      "procedure tchild.doit; begin end;\n"
      "end.\n");
  CHECK(error_count() > before);
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
  CHECK(contains(out.header, "extern ::rt::t_ptrint p_a;"));
  CHECK(contains(out.header, "extern ::rt::t_ptruint p_b;"));
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
  CHECK(contains(out.header, "bool p_sameclass(::rt::t_tclass p_c);"));
}

void test_corba_interface_emits_pure_virtual_base_and_pointer_calls() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "{$interfaces corba}\n"
      "type\n"
      "  ireader = interface ['{11111111-1111-1111-1111-111111111111}']\n"
      "    function next(out s : string) : boolean;\n"
      "  end;\n"
      "  treader = class(tobject, ireader)\n"
      "    function next(out s : string) : boolean;\n"
      "  end;\n"
      "procedure use(reader : ireader);\n"
      "implementation\n"
      "function treader.next(out s : string) : boolean;\n"
      "begin\n"
      "  next := false;\n"
      "end;\n"
      "procedure use(reader : ireader);\n"
      "var s : string;\n"
      "begin\n"
      "  reader.next(s);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "struct t_ireader {"));
  CHECK(contains(out.header, "virtual ~t_ireader() = default;"));
  CHECK(contains(out.header, "virtual bool p_next("));
  CHECK(contains(out.header, ") = 0;"));
  CHECK(contains(out.header,
                 "struct t_treader : public ::rt::t_tobject, public t_ireader {"));
  CHECK(contains(out.header, "void p_use(t_ireader* p_reader);"));
  CHECK(contains(out.impl,
                 "p_reader->p_next(::rt::tp2cc_shortstring_ref<255>(p_s));"));
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
  CHECK(contains(out.impl, "auto tp2cc_ptr = new t_tnode{};"));
  CHECK(contains(out.impl, "tp2cc_ptr->p_create();"));
}

void test_nested_record_type_emits_inside_owner_scope() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    type\n"
      "      tinner = record\n"
      "        value : integer;\n"
      "      end;\n"
      "    var\n"
      "      f : tinner;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(contains(out.header, "struct t_trec {"));
  CHECK(contains(out.header, "struct t_tinner {"));
  CHECK(contains(out.header, "int32_t p_value;"));
  CHECK(contains(out.header, "t_tinner p_f;"));
}

void test_nested_class_type_emits_qualified_owner_and_method_scope() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  touter = class\n"
      "  public\n"
      "    type\n"
      "      tinner = class\n"
      "        procedure doit;\n"
      "      end;\n"
      "    procedure use(v : tinner);\n"
      "  end;\n"
      "implementation\n"
      "procedure touter.tinner.doit;\n"
      "begin\n"
      "end;\n"
      "procedure touter.use(v : tinner);\n"
      "begin\n"
      "  v.doit;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "struct t_touter : public ::rt::t_tobject {"));
  CHECK(contains(out.header, "struct t_tinner : public ::rt::t_tobject {"));
  CHECK(contains(out.header, "void p_use(t_tinner* p_v);"));
  CHECK(contains(out.header,
                 "inline ::rt::t_tclass t_touter::t_tinner::p_classtype() const"));
  CHECK(contains(out.impl, "void t_touter::t_tinner::p_doit()"));
  CHECK(contains(out.impl, "void t_touter::p_use(t_tinner* p_v)"));
  CHECK(contains(out.impl, "p_v->p_doit();"));
}

void test_class_lifecycle_methods_run_from_unit_hooks() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    class constructor init;\n"
      "    class destructor done;\n"
      "  end;\n"
      "implementation\n"
      "class constructor tnode.init;\n"
      "begin\n"
      "end;\n"
      "class destructor tnode.done;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "static void p_init();"));
  CHECK(contains(out.header, "static void p_done();"));
  CHECK(contains(out.impl, "void t_tnode::p_init() {"));
  CHECK(contains(out.impl, "void t_tnode::p_done() {"));
  const size_t init_hook = out.impl.find("void tp2cc_unit_init() {");
  const size_t init_call = out.impl.find("t_tnode::p_init();");
  const size_t fini_hook = out.impl.find("void tp2cc_unit_fini() {");
  const size_t fini_call = out.impl.find("t_tnode::p_done();");
  CHECK(init_hook != std::string::npos && init_call != std::string::npos &&
        init_hook < init_call);
  CHECK(fini_hook != std::string::npos && fini_call != std::string::npos &&
        fini_hook < fini_call);
}

void test_abstract_class_constructor_call_warns() {
  int before = warning_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class abstract\n"
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
  CHECK_EQ(warning_count() - before, 1);
  CHECK(contains(out.impl, "auto tp2cc_ptr = new t_tnode{};"));
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
  CHECK(contains(out.impl, "t_tbase::p_init(p_n);"));
  CHECK(!contains(out.impl, "new t_tbase{};"));
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
  CHECK(contains(out.impl, "::rt::t_tobject::p_free(this->p_next);"));
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
                 "using t_tbaseclass = tp2cc_metaclass_t_tbase*;"));
  CHECK(contains(out.header,
                 "struct tp2cc_metaclass_t_tchild : public tp2cc_metaclass_t_tbase {"));
  CHECK(contains(out.header,
                 "inline tp2cc_metaclass_t_tchild* tp2cc_metaclass_value_t_tchild() {"));
  CHECK(contains(out.impl, "p_cls = tp2cc_metaclass_value_t_tchild();"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create(1);"));
  CHECK(contains(out.impl, "if (::rt::p_assigned(p_cls))"));
}

void test_metaclass_virtual_class_method_dispatch() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    class function kind : integer; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    class function kind : integer; override;\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "procedure demo(cls : tbaseclass; var i : integer);\n"
      "implementation\n"
      "class function tbase.kind : integer;\n"
      "begin\n"
      "  kind := 1;\n"
      "end;\n"
      "class function tchild.kind : integer;\n"
      "begin\n"
      "  kind := inherited kind + 1;\n"
      "end;\n"
      "procedure demo(cls : tbaseclass; var i : integer);\n"
      "begin\n"
      "  i := cls.kind;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "virtual int32_t p_kind() const { return t_tbase::p_kind(); }"));
  CHECK(contains(out.header,
                 "virtual int32_t p_kind() const override { return t_tchild::p_kind(); }"));
  CHECK(contains(out.impl, "inherited::p_kind()"));
  CHECK(contains(out.impl, "p_i = p_cls->p_kind();"));
}

void test_bare_inherited_in_function_value_context_calls_current_parent_method() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    function getit : pointer; virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    function getit : pointer; override;\n"
      "  end;\n"
      "implementation\n"
      "function tbase.getit : pointer;\n"
      "begin\n"
      "  getit := nil;\n"
      "end;\n"
      "function tchild.getit : pointer;\n"
      "begin\n"
      "  result := inherited;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_result = inherited::p_getit();"));
  CHECK(!contains(out.impl, "p_result = inherited{};"));
}

void test_bare_inherited_statement_forwards_current_method_params() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure touch(const name: string); virtual;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    procedure touch(const name: string); override;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.touch(const name: string);\n"
      "begin\n"
      "end;\n"
      "procedure tchild.touch(const name: string);\n"
      "var\n"
      "  local: string;\n"
      "begin\n"
      "  local := name;\n"
      "  inherited;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "inherited::p_touch(p_name);"));
  CHECK(!contains(out.impl, "inherited::p_touch();"));
}

void test_static_class_method_address_keeps_plain_function_pointer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tproc = function : integer;\n"
      "  tbase = class\n"
      "    class function kind : integer; virtual;\n"
      "  end;\n"
      "procedure bind(var p : tproc);\n"
      "implementation\n"
      "class function tbase.kind : integer;\n"
      "begin\n"
      "  kind := 1;\n"
      "end;\n"
      "procedure bind(var p : tproc);\n"
      "begin\n"
      "  p := @tbase.kind;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = (&t_tbase::p_kind);"));
  CHECK(!contains(out.impl, "tp2cc_method_code"));
}

void test_metaclass_class_method_proc_value_reports_error() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tproc = function : integer;\n"
      "  tbase = class\n"
      "    class function kind : integer; virtual;\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "procedure bind(cls : tbaseclass; var p : tproc; var q : pointer);\n"
      "implementation\n"
      "class function tbase.kind : integer;\n"
      "begin\n"
      "  kind := 1;\n"
      "end;\n"
      "procedure bind(cls : tbaseclass; var p : tproc; var q : pointer);\n"
      "begin\n"
      "  p := @cls.kind;\n"
      "  p := cls.kind;\n"
      "  q := @cls.kind;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() >= before + 3);
  CHECK(!contains(out.impl, "(&p_cls->p_kind)"));
  CHECK(!contains(out.impl, "p_p = p_cls->p_kind;"));
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
  CHECK(contains(out.impl, "p_basecls = tp2cc_metaclass_value_t_tchild();"));
  CHECK(contains(out.impl,
                 "p_childcls = reinterpret_cast<t_tchildclass>(p_basecls);"));
}

void test_metaclass_value_can_flow_through_pointer_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "  tbaseclass = class of tbase;\n"
      "procedure put(p : pointer);\n"
      "procedure demo(cls : tbaseclass);\n"
      "implementation\n"
      "procedure put(p : pointer);\n"
      "begin\n"
      "end;\n"
      "procedure demo(cls : tbaseclass);\n"
      "begin\n"
      "  put(cls);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_put(static_cast<void*>(p_cls));"));
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
                 "p_result = (p_x->p_classtype() == tp2cc_metaclass_value_t_tchild());"));
}

void test_inline_anonymous_enum_class_field_resolves_members() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlinker = class\n"
      "    libctype : (libc5, glibc2, glibc21, uclibc);\n"
      "    procedure setup;\n"
      "  end;\n"
      "implementation\n"
      "procedure tlinker.setup;\n"
      "begin\n"
      "  libctype := glibc21;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "enum t_tlinker_enum0 :"));
  CHECK(contains(out.header, "t_tlinker_enum0 p_libctype;"));
  CHECK(contains(out.header, "p_glibc21"));
  CHECK(!contains(out.header, "int32_t p_libctype;"));
  CHECK(contains(out.impl, "p_libctype = p_glibc21;"));
  CHECK(!contains(out.impl, "::rt::p_glibc21"));
}

void test_inline_anonymous_packed_record_var_lowers_to_struct() {
  // Inline anonymous packed record bound to a local var: emits a real
  // C++ anonymous struct so field accesses resolve, and emits the same
  // offsetof/sizeof layout asserts the named-record path uses, this
  // time anchored on `decltype(varname)`.
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  rec : packed record\n"
      "    a, b, c : byte;\n"
      "    payload : array[0..3] of byte;\n"
      "  end;\n"
      "begin\n"
      "  rec.a := 1;\n"
      "end;\n"
      "end.\n");
  // Struct lowered properly, NOT stubbed as int32_t.
  CHECK(contains(out.impl, "struct [[gnu::packed]] {"));
  CHECK(!contains(out.impl, "/* inline-record */ int32_t p_rec"));
  CHECK(contains(out.impl, "uint8_t p_a;"));
  CHECK(contains(out.impl, "uint8_t p_b;"));
  CHECK(contains(out.impl, "uint8_t p_c;"));
  CHECK(contains(out.impl, "::rt::tp2cc_Array<uint8_t, 0, ((3) - (0) + 1)> p_payload;"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_store<uint8_t>(::rt::tp2cc_byte_offset((&p_rec), offsetof(::std::remove_reference_t<decltype(p_rec)>, p_a)), 1);"));
  // Layout asserts use `decltype(p_rec)` since there's no typedef name.
  CHECK(contains(out.impl,
                 "static_assert(offsetof(decltype(p_rec), p_a) == 0"));
  CHECK(contains(out.impl,
                 "static_assert(sizeof(decltype(p_rec)) =="));
}

void test_inline_anonymous_variant_record_lowers_to_union() {
  auto out = compile_snippet(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  n : record\n"
      "    n_un : record\n"
      "      case longint of\n"
      "        0 : (n_name : pchar);\n"
      "        1 : (n_strx : longint);\n"
      "    end;\n"
      "  end;\n"
      "begin\n"
      "  n.n_un.n_strx := 7;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "union { struct { ::rt::p_char* p_n_name; }; struct { int32_t p_n_strx; }; };"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<int32_t>(::rt::tp2cc_byte_offset((&p_n.p_n_un), offsetof(::std::remove_reference_t<decltype(p_n.p_n_un)>, p_n_strx)), 7);"));
  CHECK(!contains(out.impl, "p_n.p_n_un.p_n_strx = 7;"));
  CHECK(!contains(out.impl, "/* inline-variant-record */ int32_t"));
}

void test_variant_record_payload_fields_use_byte_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tintbits = record\n"
      "    overflow : boolean;\n"
      "    case signed : boolean of\n"
      "      false : (uvalue : qword);\n"
      "      true : (svalue : int64);\n"
      "  end;\n"
      "function readu(var r : tintbits) : qword;\n"
      "procedure writeu(var r : tintbits; v : qword);\n"
      "implementation\n"
      "function readu(var r : tintbits) : qword;\n"
      "begin\n"
      "  readu := r.uvalue;\n"
      "end;\n"
      "procedure writeu(var r : tintbits; v : qword);\n"
      "begin\n"
      "  r.uvalue := v;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_load<uint64_t>(::rt::tp2cc_byte_offset((&p_r), offsetof(t_tintbits, p_uvalue)))"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<uint64_t>(::rt::tp2cc_byte_offset((&p_r), offsetof(t_tintbits, p_uvalue)), p_v);"));
  CHECK(!contains(out.impl, "p_r.p_uvalue"));
}

void test_variant_record_payload_storage_composes_through_members() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treference = record\n"
      "    offset : longint;\n"
      "    base : longint;\n"
      "  end;\n"
      "  tlocation = record\n"
      "    case tag : longint of\n"
      "      0 : (reference : treference);\n"
      "      1 : (reg : longint);\n"
      "  end;\n"
      "procedure touch(var r : treference);\n"
      "procedure run(var loc : tlocation; delta : longint);\n"
      "implementation\n"
      "procedure touch(var r : treference);\n"
      "begin\n"
      "end;\n"
      "procedure run(var loc : tlocation; delta : longint);\n"
      "begin\n"
      "  loc.reference.offset := 4;\n"
      "  inc(loc.reference.offset, delta);\n"
      "  touch(loc.reference);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<int32_t>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference)), offsetof(t_treference, p_offset)), 4);"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_inc<int32_t>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference)), offsetof(t_treference, p_offset)), p_delta);"));
  CHECK(contains(out.impl,
                 "p_touch(::rt::tp2cc_reinterpret_ref<t_treference>(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference))));"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_treference>("));
}

void test_with_variant_record_payload_keeps_field_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tconstexprint = record\n"
      "    case signed : boolean of\n"
      "      false : (uvalue : qword);\n"
      "      true : (svalue : int64);\n"
      "  end;\n"
      "  tconstvalue = record\n"
      "    case tag : longint of\n"
      "      0 : (valueord : tconstexprint);\n"
      "      1 : (valueptr : pointer);\n"
      "  end;\n"
      "procedure run(var value : tconstvalue; v : qword; var outv : qword);\n"
      "implementation\n"
      "procedure run(var value : tconstvalue; v : qword; var outv : qword);\n"
      "begin\n"
      "  with value.valueord do begin\n"
      "    signed := false;\n"
      "    uvalue := v;\n"
      "    outv := uvalue;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "auto tp2cc_with_0 = ::rt::tp2cc_byte_offset((&p_value), offsetof(t_tconstvalue, p_valueord));"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<bool>(::rt::tp2cc_byte_offset(tp2cc_with_0, offsetof(t_tconstexprint, p_signed)), false);"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<uint64_t>(::rt::tp2cc_byte_offset(tp2cc_with_0, offsetof(t_tconstexprint, p_uvalue)), p_v);"));
  CHECK(contains(out.impl,
                 "p_outv = ::rt::tp2cc_reinterpret_load<uint64_t>(::rt::tp2cc_byte_offset(tp2cc_with_0, offsetof(t_tconstexprint, p_uvalue)));"));
  CHECK(!contains(out.impl,
                  "auto& tp2cc_with_0 = ::rt::tp2cc_reinterpret_load<t_tconstexprint>"));
  CHECK(!contains(out.impl, "tp2cc_with_0.p_uvalue"));
}

void test_with_variant_payload_object_field_method_keeps_payload_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tderef = object\n"
      "    dataidx : longint;\n"
      "    procedure reset;\n"
      "    function resolve : pointer;\n"
      "  end;\n"
      "  titem = record\n"
      "    case byte of\n"
      "      0 : (symderef : tderef);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "function run(var item : titem) : pointer;\n"
      "implementation\n"
      "procedure tderef.reset;\n"
      "begin\n"
      "  dataidx := 0;\n"
      "end;\n"
      "function tderef.resolve : pointer;\n"
      "begin\n"
      "  resolve := nil;\n"
      "end;\n"
      "function run(var item : titem) : pointer;\n"
      "begin\n"
      "  with item do begin\n"
      "    symderef.reset;\n"
      "    run := symderef.resolve;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  // A bare field inside `with` still names the variant payload slot. Calling an
  // object method on it must bind `Self` to that slot, not to a copied value.
  const std::string slot =
      "::rt::tp2cc_byte_offset((&p_item), offsetof(t_titem, p_symderef))";
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_ref<t_tderef>(" + slot +
                     ").p_reset();"));
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_ref<t_tderef>(" + slot +
                               ").p_resolve()"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tderef>(" + slot +
                                ").p_reset()"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tderef>(" + slot +
                                ").p_resolve()"));
}

void test_variant_record_payload_member_read_address_and_untyped_actual() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  plongint = ^longint;\n"
      "  treference = record\n"
      "    offset : longint;\n"
      "    base : longint;\n"
      "  end;\n"
      "  tlocation = record\n"
      "    case tag : longint of\n"
      "      0 : (reference : treference);\n"
      "      1 : (reg : longint);\n"
      "  end;\n"
      "procedure raw(var x);\n"
      "function readoffset(var loc : tlocation) : longint;\n"
      "function addressoffset(var loc : tlocation) : plongint;\n"
      "procedure passoffset(var loc : tlocation);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "function readoffset(var loc : tlocation) : longint;\n"
      "begin\n"
      "  readoffset := loc.reference.offset;\n"
      "end;\n"
      "function addressoffset(var loc : tlocation) : plongint;\n"
      "begin\n"
      "  addressoffset := @loc.reference.offset;\n"
      "end;\n"
      "procedure passoffset(var loc : tlocation);\n"
      "begin\n"
      "  raw(loc.reference.offset);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = ::rt::tp2cc_reinterpret_load<int32_t>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference)), offsetof(t_treference, p_offset)));"));
  CHECK(contains(out.impl,
                 "p_result = reinterpret_cast<int32_t*>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference)), offsetof(t_treference, p_offset)));"));
  CHECK(contains(out.impl,
                 "p_raw(((void*)(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_loc), offsetof(t_tlocation, p_reference)), offsetof(t_treference, p_offset)))));"));
  CHECK(!contains(out.impl, "&::rt::tp2cc_reinterpret_load<int32_t>"));
}

void test_variant_record_payload_storage_composes_through_indexes() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[1..3] of longint;\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (items : tarr);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure run(var view : tview; i : longint; value : longint);\n"
      "implementation\n"
      "procedure run(var view : tview; i : longint; value : longint);\n"
      "begin\n"
      "  view.items[i] := value;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<int32_t>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items)), ((p_i) - (1)) * sizeof(int32_t)), p_value);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tarr>("));
}

void test_variant_record_payload_index_read_address_and_untyped_actual() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pint = ^longint;\n"
      "  tarr = array[1..3] of longint;\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (items : tarr);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure raw(var x);\n"
      "function readitem(var view : tview; i : longint) : longint;\n"
      "function addritem(var view : tview; i : longint) : pint;\n"
      "procedure passitem(var view : tview; i : longint);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "function readitem(var view : tview; i : longint) : longint;\n"
      "begin\n"
      "  readitem := view.items[i];\n"
      "end;\n"
      "function addritem(var view : tview; i : longint) : pint;\n"
      "begin\n"
      "  addritem := @view.items[i];\n"
      "end;\n"
      "procedure passitem(var view : tview; i : longint);\n"
      "begin\n"
      "  raw(view.items[i]);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = ::rt::tp2cc_reinterpret_load<int32_t>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items)), ((p_i) - (1)) * sizeof(int32_t)));"));
  CHECK(contains(out.impl,
                 "p_result = reinterpret_cast<int32_t*>(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items)), ((p_i) - (1)) * sizeof(int32_t)));"));
  CHECK(contains(out.impl,
                 "p_raw(((void*)(::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items)), ((p_i) - (1)) * sizeof(int32_t)))));"));
  CHECK(!contains(out.impl, "&::rt::tp2cc_reinterpret_load<int32_t>"));
}

void test_variant_payload_object_array_index_method_keeps_payload_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tderef = object\n"
      "    dataidx : longint;\n"
      "    procedure reset;\n"
      "  end;\n"
      "  tderefs = array[1..3] of tderef;\n"
      "  tview = record\n"
      "    case byte of\n"
      "      0 : (items : tderefs);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure run(var view : tview; i : longint);\n"
      "implementation\n"
      "procedure tderef.reset;\n"
      "begin\n"
      "  dataidx := 0;\n"
      "end;\n"
      "procedure run(var view : tview; i : longint);\n"
      "begin\n"
      "  view.items[i].reset;\n"
      "end;\n"
      "end.\n");
  // Indexing composes another byte offset onto the variant payload address.
  // The method receiver must be the addressed array element itself.
  const std::string slot =
      "::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items)), ((p_i) - (1)) * sizeof(t_tderef))";
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_ref<t_tderef>(" + slot +
                               ").p_reset();"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tderef>(" + slot +
                                ").p_reset()"));
}

void test_variant_record_payload_array_address_uses_payload_address_proxy() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tarr = array[1..3] of char;\n"
      "  ptarr = ^tarr;\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (items : tarr);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure run(var view : tview; var pc : pchar; var pa : ptarr);\n"
      "implementation\n"
      "procedure run(var view : tview; var pc : pchar; var pa : ptarr);\n"
      "begin\n"
      "  pc := @view.items;\n"
      "  pa := @view.items;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(count_substring(out.impl,
                           "::rt::tp2cc_array_addr(reinterpret_cast<t_tarr*>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_items))))"),
           static_cast<size_t>(2));
  CHECK(!contains(out.impl, "::rt::tp2cc_array_addr()"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tarr>"));
}

void test_variant_record_payload_shortstring_index_stays_on_storage() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (text : string[10]);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure raw(var x);\n"
      "procedure take(var ch : char);\n"
      "procedure run(var view : tview; i : longint; ch : char; var p : pchar);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "procedure take(var ch : char);\n"
      "begin\n"
      "end;\n"
      "procedure run(var view : tview; i : longint; ch : char; var p : pchar);\n"
      "begin\n"
      "  ch := view.text[i];\n"
      "  view.text[i] := ch;\n"
      "  inc(view.text[i]);\n"
      "  p := @view.text[i];\n"
      "  raw(view.text[i]);\n"
      "  take(view.text[i]);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  const std::string slot =
      "::rt::tp2cc_byte_offset(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_text)), ::rt::tp2cc_shortstring_index_offset<10>(p_i))";
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_load<::rt::p_char>(" + slot + ")"));
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_store<::rt::p_char>(" + slot + ", p_ch);"));
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_inc<::rt::p_char>(" + slot + ");"));
  CHECK(contains(out.impl, "p_p = reinterpret_cast<::rt::p_char*>(" + slot + ");"));
  CHECK(contains(out.impl, "p_raw(((void*)(" + slot + ")));"));
  CHECK(contains(out.impl,
                 "p_take(::rt::tp2cc_reinterpret_ref<::rt::p_char>(" + slot + "));"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<::rt::tp2cc_ShortString<10>>"));
  CHECK(!contains(out.impl, "&(::rt::tp2cc_reinterpret_load"));
}

void test_variant_record_pointer_payload_passes_slot_to_allocation_builtins() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pint = ^longint;\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (item : pint);\n"
      "      1 : (text : pchar);\n"
      "  end;\n"
      "procedure run(var view : tview; size : longint);\n"
      "implementation\n"
      "procedure run(var view : tview; size : longint);\n"
      "begin\n"
      "  new(view.item);\n"
      "  getmem(view.text, size);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(contains(out.impl,
                 "::rt::p_new_slot<t_pint>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_item)));"));
  CHECK(contains(out.impl,
                 "::rt::p_getmem_slot<::rt::p_char*>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_text)), p_size);"));
  CHECK(!contains(out.impl, "tp2cc_reinterpret_load<t_pint>"));
  CHECK(!contains(out.impl, "tp2cc_reinterpret_load<::rt::p_char*>"));
}

void test_shadowed_getmem_does_not_use_runtime_slot_helper() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (text : pchar);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "procedure run(var view : tview; size : longint);\n"
      "implementation\n"
      "procedure getmem(var p : pchar; size : longint);\n"
      "begin\n"
      "end;\n"
      "procedure run(var view : tview; size : longint);\n"
      "begin\n"
      "  getmem(view.text, size);\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count(), before);
  CHECK(!contains(out.impl, "::rt::p_getmem_slot"));
  CHECK(contains(out.impl,
                 "p_getmem(::rt::tp2cc_reinterpret_ref<::rt::p_char*>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_text))), p_size);"));
}

void test_variant_record_pointer_payload_distinguishes_slot_from_pointee() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pint = ^longint;\n"
      "  tview = record\n"
      "    case tag : longint of\n"
      "      0 : (item : pint);\n"
      "      1 : (value : longint);\n"
      "  end;\n"
      "procedure touch(var p : pint);\n"
      "procedure setslot(var view : tview; p : pint);\n"
      "procedure setpointed(var view : tview; v : longint);\n"
      "function addrpointed(var view : tview) : pint;\n"
      "implementation\n"
      "procedure touch(var p : pint);\n"
      "begin\n"
      "end;\n"
      "procedure setslot(var view : tview; p : pint);\n"
      "begin\n"
      "  view.item := p;\n"
      "  touch(view.item);\n"
      "end;\n"
      "procedure setpointed(var view : tview; v : longint);\n"
      "begin\n"
      "  view.item^ := v;\n"
      "end;\n"
      "function addrpointed(var view : tview) : pint;\n"
      "begin\n"
      "  addrpointed := @view.item^;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<t_pint>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_item)), p_p);"));
  CHECK(contains(out.impl,
                 "p_touch(::rt::tp2cc_reinterpret_ref<t_pint>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_item))));"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_deref(::rt::tp2cc_reinterpret_load<t_pint>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_item)))) = p_v;"));
  CHECK(contains(out.impl,
                 "p_result = ::rt::tp2cc_reinterpret_load<t_pint>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_item)));"));
  CHECK(!contains(out.impl, "&::rt::tp2cc_deref"));
}

void test_variant_record_pointer_payload_typecast_keeps_slot_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pwide = ^longint;\n"
      "  tconstvalue = record\n"
      "    case integer of\n"
      "      0 : (valueptr : pointer; len : longint);\n"
      "      1 : (valueord : longint);\n"
      "  end;\n"
      "procedure touch(var p : pwide);\n"
      "procedure run(var value : tconstvalue; pw : pwide);\n"
      "implementation\n"
      "procedure touch(var p : pwide);\n"
      "begin\n"
      "end;\n"
      "procedure run(var value : tconstvalue; pw : pwide);\n"
      "begin\n"
      "  pwide(value.valueptr) := pw;\n"
      "  touch(pwide(value.valueptr));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_store<t_pwide>(::rt::tp2cc_byte_offset((&p_value), offsetof(t_tconstvalue, p_valueptr)), p_pw);"));
  CHECK(contains(out.impl,
                 "p_touch(::rt::tp2cc_reinterpret_ref<t_pwide>(::rt::tp2cc_byte_offset((&p_value), offsetof(t_tconstvalue, p_valueptr))));"));
  CHECK(!contains(out.impl,
                  "tp2cc_reinterpret_storage_ref<t_pwide>(::rt::tp2cc_reinterpret_load"));
}

void test_variant_class_payload_member_loads_pointer_before_field_offset() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  psymtable = ^tsymtable;\n"
      "  tsymtable = record\n"
      "    marker : longint;\n"
      "  end;\n"
      "  tsym = class\n"
      "  public\n"
      "    owner : psymtable;\n"
      "  end;\n"
      "  pitem = ^titem;\n"
      "  titem = record\n"
      "    case byte of\n"
      "      0 : (sym : tsym);\n"
      "      1 : (value : longint);\n"
      "  end;\n"
      "function read_owner(plist : pitem) : psymtable;\n"
      "procedure write_owner(plist : pitem; st : psymtable);\n"
      "implementation\n"
      "function read_owner(plist : pitem) : psymtable;\n"
      "begin\n"
      "  read_owner := plist^.sym.owner;\n"
      "end;\n"
      "procedure write_owner(plist : pitem; st : psymtable);\n"
      "begin\n"
      "  plist^.sym.owner := st;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "tp2cc_pointer_byte_offset(::rt::tp2cc_reinterpret_load<t_tsym*>(::rt::tp2cc_pointer_byte_offset(p_plist, offsetof(t_titem, p_sym))), offsetof(t_tsym, p_owner))"));
  CHECK(!contains(out.impl,
                  "tp2cc_byte_offset(::rt::tp2cc_byte_offset(p_plist, offsetof(t_titem, p_sym)), offsetof(t_tsym, p_owner))"));
}

void test_variant_payload_after_reference_field_loads_reference_before_offset() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlocation = record\n"
      "    case byte of\n"
      "      0 : (reg : longint);\n"
      "      1 : (other : longint);\n"
      "  end;\n"
      "  tnode = class\n"
      "    location : tlocation;\n"
      "  end;\n"
      "  tcall = class(tnode)\n"
      "    left : tnode;\n"
      "    right : tnode;\n"
      "  end;\n"
      "function read_reg(n : tnode) : longint;\n"
      "implementation\n"
      "function read_reg(n : tnode) : longint;\n"
      "begin\n"
      "  read_reg := tcall(tcall(n).right).left.location.reg;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "tp2cc_pointer_byte_offset(static_cast<t_tcall*>(static_cast<t_tcall*>(p_n)->p_right)->p_left, offsetof(t_tnode, p_location))"));
  CHECK(!contains(out.impl,
                  "tp2cc_byte_offset(::rt::tp2cc_pointer_byte_offset(static_cast<t_tcall*>(static_cast<t_tcall*>(p_n)->p_right), offsetof(t_tcall, p_left)), offsetof(t_tnode, p_location))"));
}

void test_variant_payload_object_field_method_keeps_payload_storage() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tderef = object\n"
      "    dataidx : longint;\n"
      "    procedure reset;\n"
      "    function resolve : pointer;\n"
      "  end;\n"
      "  pitem = ^titem;\n"
      "  titem = record\n"
      "    case byte of\n"
      "      0 : (symderef : tderef);\n"
      "      1 : (value : longint);\n"
      "  end;\n"
      "function run(hp : pitem) : pointer;\n"
      "implementation\n"
      "procedure tderef.reset;\n"
      "begin\n"
      "  dataidx := 0;\n"
      "end;\n"
      "function tderef.resolve : pointer;\n"
      "begin\n"
      "  resolve := nil;\n"
      "end;\n"
      "function run(hp : pitem) : pointer;\n"
      "begin\n"
      "  hp^.symderef.reset;\n"
      "  run := hp^.symderef.resolve;\n"
      "end;\n"
      "end.\n");
  // `symderef` is an object field in a variant-record payload. Method calls
  // need Pascal `Self` to name that payload slot, not a temporary value loaded
  // from the slot.
  const std::string slot =
      "::rt::tp2cc_pointer_byte_offset(p_hp, offsetof(t_titem, p_symderef))";
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_ref<t_tderef>(" + slot +
                               ").p_reset();"));
  CHECK(contains(out.impl, "::rt::tp2cc_reinterpret_ref<t_tderef>(" + slot +
                               ").p_resolve()"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tderef>(" + slot +
                                ").p_reset()"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_load<t_tderef>(" + slot +
                                ").p_resolve()"));
  CHECK(!contains(out.impl, "offsetof(t_tderef, p_resolve)"));
}

void test_packed_field_typed_cast_assignment_uses_memcpy_store() {
  // `longint(p.d1) := X` where `d1` is in a `packed record`. Forming a
  // `T&` to a packed field is UB; the emitter routes the assignment
  // through `tp2cc_unaligned_store` (memcpy) instead.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tg = packed record d1 : longword; end;\n"
      "procedure run(var p : tg; v : longint);\n"
      "implementation\n"
      "procedure run(var p : tg; v : longint);\n"
      "begin\n"
      "  longint(p.d1) := v;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_store<int32_t>(::rt::tp2cc_byte_offset((&p_p), offsetof(t_tg, p_d1)), p_v);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_storage_ref<int32_t>"));
}

void test_packed_record_typecast_field_assignment_uses_storage_view() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tkind = (ka, kb);\n"
      "  tview = packed record\n"
      "    lo : word;\n"
      "    sub : byte;\n"
      "    kind : tkind;\n"
      "  end;\n"
      "procedure run(var r : longint; k : tkind);\n"
      "implementation\n"
      "procedure run(var r : longint; k : tkind);\n"
      "begin\n"
      "  tview(r).kind := k;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_store<t_tkind>(::rt::tp2cc_byte_offset((&p_r), offsetof(t_tview, p_kind)), p_k);"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_copy<t_tview>(p_r).p_kind"));
}

void test_record_typecast_field_read_uses_storage_view() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  twoword = record\n"
      "    lo : longword;\n"
      "    hi : longword;\n"
      "  end;\n"
      "function run(x : double) : longword;\n"
      "implementation\n"
      "function run(x : double) : longword;\n"
      "begin\n"
      "  run := twoword(x).hi;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<t_twoword>(p_x).p_hi"));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_copy<t_twoword>(p_x).p_hi"));
}

void test_local_record_typecast_field_read_uses_storage_view() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function run(x : double) : longword;\n"
      "implementation\n"
      "function run(x : double) : longword;\n"
      "type\n"
      "  twoword = record\n"
      "    lo : longword;\n"
      "    hi : longword;\n"
      "  end;\n"
      "begin\n"
      "  run := twoword(x).hi;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<t_twoword>(p_x).p_hi"));
  CHECK(!contains(out.impl, "((t_twoword)(p_x)).p_hi"));
}

void test_inc_packed_field_routes_through_memcpy_inc() {
  // `Inc(p.f, n)` where `f` is in a `packed record` -- direct member
  // access, no outer typed cast. The emitter uses the field's own
  // declared type as the operand and routes through
  // `tp2cc_unaligned_inc`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tdir = packed record name_ord : word; end;\n"
      "procedure run(var p : tdir; n : word);\n"
      "implementation\n"
      "procedure run(var p : tdir; n : word);\n"
      "begin\n"
      "  inc(p.name_ord, n);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_inc<uint16_t>(::rt::tp2cc_byte_offset((&p_p), offsetof(t_tdir, p_name_ord)), p_n);"));
}

void test_inc_local_packed_pointee_field_routes_through_memcpy_inc() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "type\n"
      "  tdir = packed record name_ord : word; end;\n"
      "  pdir = ^tdir;\n"
      "var\n"
      "  p : pdir;\n"
      "  n : word;\n"
      "begin\n"
      "  inc(p^.name_ord, n);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_inc<uint16_t>("));
}

void test_unaligned_typed_deref_read_uses_bytewise_load() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type pword = ^word;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  b : array[0..15] of byte;\n"
      "  i : longint;\n"
      "  w : word;\n"
      "begin\n"
      "  w := unaligned(pword(@b[i])^);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_load<uint16_t>("));
  CHECK(!contains(out.impl, "::rt::p_unaligned("));
}

void test_unaligned_typed_deref_write_uses_bytewise_store() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type pword = ^word;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  b : array[0..15] of byte;\n"
      "  i : longint;\n"
      "  w : word;\n"
      "begin\n"
      "  unaligned(pword(@b[i])^) := w;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_store<uint16_t>("));
  CHECK(!contains(out.impl, "::rt::p_unaligned("));
}

void test_unaligned_typed_deref_inc_dec_use_unaligned_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type pword = ^word;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  b : array[0..15] of byte;\n"
      "  i : longint;\n"
      "  n : word;\n"
      "begin\n"
      "  inc(unaligned(pword(@b[i])^), n);\n"
      "  dec(unaligned(pword(@b[i])^));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_inc<uint16_t>("));
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_dec<uint16_t>("));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_inc<uint16_t>("));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_dec<uint16_t>("));
}

void test_unaligned_variant_payload_uses_payload_storage_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tview = record\n"
      "    case byte of\n"
      "      0 : (w : word);\n"
      "      1 : (i : longint);\n"
      "  end;\n"
      "function readw(var view : tview) : word;\n"
      "implementation\n"
      "function readw(var view : tview) : word;\n"
      "begin\n"
      "  readw := unaligned(view.w);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_load<uint16_t>(::rt::tp2cc_byte_offset((&p_view), offsetof(t_tview, p_w)))"));
  CHECK(!contains(out.impl, "&(::rt::tp2cc_reinterpret_load<uint16_t>"));
}

void test_unaligned_storage_address_and_untyped_actual_use_raw_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbuf = array[0..15] of byte;\n"
      "  pword = ^word;\n"
      "procedure raw(var x);\n"
      "procedure run(var b : tbuf; i : longint; var p : pword);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "procedure run(var b : tbuf; i : longint; var p : pword);\n"
      "begin\n"
      "  p := @unaligned(pword(@b[i])^);\n"
      "  raw(unaligned(pword(@b[i])^));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_p = reinterpret_cast<uint16_t*>("));
  CHECK(contains(out.impl, "p_raw(((void*)("));
  CHECK(contains(out.impl, "(&p_b[p_i])"));
  CHECK(!contains(out.impl, "&(::rt::tp2cc_unaligned_load<uint16_t>"));
}

void test_typecast_over_unaligned_storage_preserves_unaligned_helpers() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbuf = array[0..15] of byte;\n"
      "  plongint = ^longint;\n"
      "procedure raw(var x);\n"
      "function readl(var b : tbuf; i : longint) : longint;\n"
      "procedure run(var b : tbuf; i : longint; v : longint; var p : plongint);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "function readl(var b : tbuf; i : longint) : longint;\n"
      "begin\n"
      "  readl := longint(unaligned(plongint(@b[i])^));\n"
      "end;\n"
      "procedure run(var b : tbuf; i : longint; v : longint; var p : plongint);\n"
      "begin\n"
      "  longint(unaligned(plongint(@b[i])^)) := v;\n"
      "  inc(longint(unaligned(plongint(@b[i])^)), v);\n"
      "  p := @longint(unaligned(plongint(@b[i])^));\n"
      "  raw(longint(unaligned(plongint(@b[i])^)));\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = ((int32_t)(::rt::tp2cc_unaligned_load<int32_t>("));
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_store<int32_t>("));
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_inc<int32_t>("));
  CHECK(contains(out.impl, "p_p = reinterpret_cast<int32_t*>("));
  CHECK(contains(out.impl, "p_raw(((void*)("));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_store<int32_t>("));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_inc<int32_t>("));
  CHECK(!contains(out.impl, "&(::rt::tp2cc_unaligned_load<int32_t>"));
}

void test_packed_record_field_through_pointer_slot_stays_bytewise() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tpacked = packed record\n"
      "    w : word;\n"
      "  end;\n"
      "  ppacked = ^tpacked;\n"
      "  pslot = ^ppacked;\n"
      "  pword = ^word;\n"
      "procedure raw(var x);\n"
      "function readw(slot : pslot) : word;\n"
      "procedure run(slot : pslot; n : word; var p : pword);\n"
      "implementation\n"
      "procedure raw(var x);\n"
      "begin\n"
      "end;\n"
      "function readw(slot : pslot) : word;\n"
      "begin\n"
      "  readw := slot^.w;\n"
      "end;\n"
      "procedure run(slot : pslot; n : word; var p : pword);\n"
      "begin\n"
      "  slot^.w := n;\n"
      "  inc(slot^.w, n);\n"
      "  p := @slot^.w;\n"
      "  raw(slot^.w);\n"
      "end;\n"
      "end.\n");
  const std::string slot =
      "::rt::tp2cc_pointer_byte_offset(::rt::tp2cc_deref(p_slot), offsetof(t_tpacked, p_w))";
  CHECK(contains(out.impl,
                 "p_result = ::rt::tp2cc_unaligned_load<uint16_t>(" +
                     slot + ");"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_store<uint16_t>(" + slot +
                     ", p_n);"));
  CHECK(contains(out.impl,
                 "::rt::tp2cc_unaligned_inc<uint16_t>(" + slot + ", p_n);"));
  CHECK(contains(out.impl, "p_p = reinterpret_cast<uint16_t*>(" + slot + ");"));
  CHECK(contains(out.impl, "p_raw(((void*)(" + slot + ")));"));
  CHECK(!contains(out.impl, "&::rt::tp2cc_unaligned_load<uint16_t>"));
  CHECK(!contains(out.impl, "::rt::p_inc(::rt::tp2cc_deref(p_slot)->p_w"));
}

void test_unaligned_pointer_field_read_uses_bytewise_load() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type trec = packed record name : pchar; end;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  r : trec;\n"
      "  p : pchar;\n"
      "begin\n"
      "  p := unaligned(r.name);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "::rt::tp2cc_unaligned_load<::rt::p_char*>("));
  CHECK(!contains(out.impl, "::rt::p_unaligned("));
}

void test_unaligned_storage_var_arg_is_rejected_instead_of_ref_bound() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  pword = ^word;\n"
      "  plongint = ^longint;\n"
      "procedure take(var w : word);\n"
      "procedure takel(var l : longint);\n"
      "procedure run;\n"
      "implementation\n"
      "procedure take(var w : word);\n"
      "begin\n"
      "end;\n"
      "procedure takel(var l : longint);\n"
      "begin\n"
      "end;\n"
      "procedure run;\n"
      "var\n"
      "  b : array[0..15] of byte;\n"
      "  i : longint;\n"
      "begin\n"
      "  take(unaligned(pword(@b[i])^));\n"
      "  takel(longint(unaligned(plongint(@b[i])^)));\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 2);
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_ref<uint16_t>("));
  CHECK(!contains(out.impl, "::rt::tp2cc_reinterpret_ref<int32_t>("));
}

void test_overload_picks_shortstring_target_for_short_string_arg() {
  // When a callee has both `f(string)` and `f(ansistring)` overloads,
  // a `string[N]` argument (N < 255) should resolve to the `string`
  // overload. C++ overload resolution alone treats both as equally
  // ranked single user-defined conversions; the emitter inserts a
  // disambiguating `static_cast<ShortString<255>>(...)` so the C++
  // compiler lands on the same overload Pascal would.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tidstring = string[127];\n"
      "function upper(const s : string) : string;\n"
      "function upper(const s : ansistring) : ansistring;\n"
      "procedure run(const id : tidstring);\n"
      "implementation\n"
      "function upper(const s : string) : string;\n"
      "begin\n"
      "  upper := s;\n"
      "end;\n"
      "function upper(const s : ansistring) : ansistring;\n"
      "begin\n"
      "  upper := s;\n"
      "end;\n"
      "procedure run(const id : tidstring);\n"
      "begin\n"
      "  upper(id);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_upper(static_cast<::rt::tp2cc_ShortString<>>(p_id))"));
}

void test_overload_picks_pchar_to_shortstring_over_ansistring() {
  // PChar -> ShortString is rank 7; PChar -> AnsiString is rank 8 in the
  // emitter's Pascal-overload table. The split exists because Pascal
  // under `{$H-}` (the bootstrap compiler's mode) prefers ShortString
  // parameters when both string-family overloads would otherwise tie.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function upper(const c : char) : char;\n"
      "function upper(const s : string) : string;\n"
      "function upper(const s : ansistring) : ansistring;\n"
      "procedure run(p : pchar);\n"
      "implementation\n"
      "function upper(const c : char) : char;\n"
      "begin\n"
      "  upper := c;\n"
      "end;\n"
      "function upper(const s : string) : string;\n"
      "begin\n"
      "  upper := s;\n"
      "end;\n"
      "function upper(const s : ansistring) : ansistring;\n"
      "begin\n"
      "  upper := s;\n"
      "end;\n"
      "procedure run(p : pchar);\n"
      "begin\n"
      "  upper(p);\n"
      "end;\n"
      "end.\n");
  // The arg goes through `tp2cc_shortstring_of<255>(p_p)` because
  // `lower_call_arg` already pre-wraps PChar values for stringish params;
  // what matters is that the outer cast targets ShortString (the picked
  // overload) rather than AnsiString or `p_char`.
  CHECK(contains(out.impl,
                 "p_upper(static_cast<::rt::tp2cc_ShortString<>>("));
  CHECK(!contains(out.impl, "p_upper(static_cast<::rt::tp2cc_AnsiString>"));
  CHECK(!contains(out.impl, "p_upper(static_cast<::rt::p_char>"));
}

void test_sizeof_lowers_to_int32_to_match_pascal_longint_semantics() {
  // Pascal `sizeof` returns `longint`; emitting raw C++ `sizeof(...)`
  // would yield `size_t` (unsigned), which then bombs against an
  // overload set that has signed/unsigned variants of the same width
  // (qword / int64 / longint) -- everything is one standard conversion
  // away, equally ranked, ambiguous.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type myrec = record x : longint; end;\n"
      "function note(n : longint) : longint;\n"
      "implementation\n"
      "function note(n : longint) : longint;\n"
      "begin\n"
      "  note := sizeof(myrec);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "static_cast<int32_t>(sizeof(t_myrec))"));
}

void test_overload_picks_unsigned_widening_over_sign_change() {
  // For an unsigned arg, `f(uint64)` (IntWideningSameSign, rank 4)
  // beats both `f(int64)` and `f(int32)` (OrdinalSignChange, rank 9).
  // C++ would otherwise call all three candidates ambiguous because
  // every conversion is a single standard conversion.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function tostr(i : qword) : string;\n"
      "function tostr(i : int64) : string;\n"
      "function tostr(i : longint) : string;\n"
      "procedure run(v : cardinal);\n"
      "implementation\n"
      "function tostr(i : qword) : string;\n"
      "begin\n"
      "  tostr := '';\n"
      "end;\n"
      "function tostr(i : int64) : string;\n"
      "begin\n"
      "  tostr := '';\n"
      "end;\n"
      "function tostr(i : longint) : string;\n"
      "begin\n"
      "  tostr := '';\n"
      "end;\n"
      "procedure run(v : cardinal);\n"
      "begin\n"
      "  tostr(v);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_tostr(static_cast<uint64_t>(p_v))"));
}

void test_overload_aggregates_candidates_across_uses_chain() {
  // When the SAME callable name is declared in multiple visible units
  // with different arities (e.g. `FileExists(name)` in sysutils/rt and
  // `FileExists(name, allowcache)` in cfileutils), the picker must see
  // both candidates so arity filtering can pick correctly. The bug was
  // returning on the first non-empty match, so __rt__'s 1-arg
  // `fileexists` shadowed cfileutils' 2-arg version and 2-arg call
  // sites failed to match.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses\n"
      "  cfileutils;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  if fileexists('a.txt') then ;\n"
      "  if fileexists('b.txt', false) then ;\n"
      "end;\n"
      "end.\n",
      {{"cfileutils.pas",
        "unit cfileutils;\n"
        "interface\n"
        "function FileExists(const F : string; allowcache : boolean) : boolean;\n"
        "implementation\n"
        "function FileExists(const F : string; allowcache : boolean) : boolean;\n"
        "begin\n"
        "  fileexists := false;\n"
        "end;\n"
        "end.\n"}});
  // 1-arg call resolves to rt's sysutils-side fileexists.
  CHECK(contains(out.impl, "::rt::p_fileexists("));
  // 2-arg call resolves to cfileutils' overload, not the rt one.
  CHECK(contains(out.impl, "p_cfileutils::p_fileexists("));
}

void test_overload_ambiguous_default_arg_vs_no_default_reports_error() {
  // `f(x : longint)` and `f(x : longint = 5)` are both viable for `f(7)`:
  // the first by exact match, the second also by exact match (default
  // unused). Neither dominates on the conversion-rank vector. The picker
  // must report ambiguity instead of silently picking one.
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure f(x : longint); overload;\n"
      "procedure f(x : longint = 5); overload;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure f(x : longint); begin end;\n"
      "procedure f(x : longint = 5); begin end;\n"
      "procedure run;\n"
      "begin\n"
      "  f(7);\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_overload_ambiguous_two_default_arg_overloads_reports_error() {
  // `f(a : longint = 1; b : shortstring = 'x')` and
  // `f(a : longint;     b : shortstring = 'x')`: a 2-arg call `f(7,'y')`
  // matches both with identical conversion ranks. Pascal-level
  // ambiguous; must error.
  int before = error_count();
  (void)compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure f(a : longint = 1; b : shortstring = 'x'); overload;\n"
      "procedure f(a : longint;     b : shortstring = 'x'); overload;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure f(a : longint = 1; b : shortstring = 'x'); begin end;\n"
      "procedure f(a : longint;     b : shortstring = 'x'); begin end;\n"
      "procedure run;\n"
      "begin\n"
      "  f(7, 'y');\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() > before);
}

void test_overload_picks_method_callback_for_current_method_address() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class end;\n"
      "  tobjectcallback = procedure(p : titem; arg : pointer) of object;\n"
      "  tstaticcallback = procedure(p : titem; arg : pointer);\n"
      "  tlist = class\n"
      "    procedure foreachcall(cb : tobjectcallback; arg : pointer); overload;\n"
      "    procedure foreachcall(cb : tstaticcallback; arg : pointer); overload;\n"
      "  end;\n"
      "  tholder = class\n"
      "    list : tlist;\n"
      "    procedure visit(p : titem; arg : pointer);\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "procedure tlist.foreachcall(cb : tobjectcallback; arg : pointer); begin end;\n"
      "procedure tlist.foreachcall(cb : tstaticcallback; arg : pointer); begin end;\n"
      "procedure tholder.visit(p : titem; arg : pointer); begin end;\n"
      "procedure tholder.run;\n"
      "begin\n"
      "  list.foreachcall(@visit, nil);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_list->p_foreachcall(t_tobjectcallback("));
  CHECK(contains(out.impl, "tp2cc_method_code"));
  CHECK(!contains(out.impl, "p_foreachcall((&p_visit)"));
}

void test_class_field_shadows_unit_name_in_member_call() {
  // When a class field name happens to match a unit name visible
  // through `uses` (here: `symtable` is both a unit and a field on
  // `tabstractrec`), Pascal lexical scope says the field wins for
  // `symtable.foreach(...)` -- the call lowers to a method call on
  // the field's instance, not a unit-qualified free-function call.
  // Without this, the resolver mistakes the receiver for a unit and
  // returns no decl, leaving method-pointer args (`@handler`) lowered
  // as raw `&p_handler` member-pointers that won't convert to the
  // callback's `tp2cc_MethodPtr` type.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses symtable;\n"
      "type\n"
      "  titem = class end;\n"
      "  tcb = procedure(p:titem; arg:pointer) of object;\n"
      "  tcontainer = class\n"
      "    procedure foreach(cb : tcb; arg : pointer);\n"
      "  end;\n"
      "  tabstractrec = class\n"
      "    symtable : tcontainer;\n"
      "    procedure handler(p:titem; arg:pointer);\n"
      "  end;\n"
      "  tderived = class(tabstractrec)\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "procedure tcontainer.foreach(cb : tcb; arg : pointer); begin end;\n"
      "procedure tabstractrec.handler(p:titem; arg:pointer); begin end;\n"
      "procedure tderived.run;\n"
      "begin\n"
      "  symtable.foreach(@handler, nil);\n"
      "end;\n"
      "end.\n",
      {{"symtable.pas",
        "unit symtable;\n"
        "interface\n"
        "implementation\n"
        "end.\n"}});
  // Method-pointer construction must land on the field's foreach,
  // wrapping the @handler reference as a (code, self) pair.
  CHECK(contains(out.impl, "p_symtable->p_foreach"));
  CHECK(contains(out.impl, "tp2cc_method_code"));
  CHECK(!contains(out.impl, "p_symtable::p_foreach"));
}

void test_record_field_named_like_type_keeps_pascal_type_lookup() {
  // Pascal resolves the RHS of `fvmlib: fvmlib` in type context and the LHS
  // as a field/value symbol. Generated C++ uses `t_` for the type and `p_` for
  // the field.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  fvmlib = record\n"
      "    x : longint;\n"
      "  end;\n"
      "  tcmd = record\n"
      "    fvmlib : fvmlib;\n"
      "  end;\n"
      "procedure run(var c : tcmd);\n"
      "implementation\n"
      "procedure run(var c : tcmd);\n"
      "begin\n"
      "  c.fvmlib.x := 7;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "struct t_fvmlib"));
  CHECK(contains(out.header, "t_fvmlib p_fvmlib;"));
  CHECK(!contains(out.header, "p_fvmlib p_fvmlib;"));
  CHECK(contains(out.impl, "p_c.p_fvmlib.p_x = 7"));
}

void test_member_base_local_record_shadows_same_named_type() {
  // `section.sectname` is value-member access when `section` is a parameter,
  // even if a visible record type has the same Pascal name. Static type
  // qualification is only valid when the base identifier is not a nearer
  // value in Pascal scope.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  section = record\n"
      "    sectname : longint;\n"
      "  end;\n"
      "procedure run(var section : section);\n"
      "implementation\n"
      "procedure run(var section : section);\n"
      "begin\n"
      "  section.sectname := 7;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_section.p_sectname = 7"));
  CHECK(!contains(out.impl, "t_section::p_sectname"));
}

void test_member_base_local_class_shadows_same_named_type() {
  // Same rule for classes: `tsym.typedef` uses the local variable `tsym`.
  // Otherwise a class/type named `tsym` would be misread as a static
  // qualifier and emit `t_tsym::p_typedef`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsym = class\n"
      "  end;\n"
      "  ttypesym = class(tsym)\n"
      "    typedef : longint;\n"
      "  end;\n"
      "procedure run(tsym : ttypesym);\n"
      "implementation\n"
      "procedure run(tsym : ttypesym);\n"
      "begin\n"
      "  tsym.typedef := 3;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_tsym->p_typedef = 3"));
  CHECK(!contains(out.impl, "t_tsym::p_typedef"));
}

void test_forward_decl_does_not_create_duplicate_overload_candidate() {
  // `function f : T; forward;` followed later by the actual
  // `function f : T; begin ... end;` body. Both decls flow through
  // typereg; if the forward stub were registered as a separate
  // ProcInfo the picker would see two identically-typed candidates
  // and (correctly) flag the call ambiguous. Only the implementation body
  // should land in the registry.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "function statement : longint; forward;\n"
      "procedure run;\n"
      "var i : longint;\n"
      "begin\n"
      "  i := statement();\n"
      "end;\n"
      "function statement : longint;\n"
      "begin\n"
      "  statement := 0;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_statement()"));
}

void test_overload_local_unit_shadows_uses_chain_overloads() {
  // Pascal scope: a same-named decl in the current unit hides
  // overloads imported through `uses`. Without this rule, a local
  // non-overload and an imported overload with the same signature would
  // tie at Exact and produce a spurious ambiguity.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses cutils;\n"
      "function tostr(i : longint) : shortstring;\n"
      "procedure run;\n"
      "implementation\n"
      "function tostr(i : longint) : shortstring; begin tostr := ''; end;\n"
      "procedure run;\n"
      "var x : longint;\n"
      "    s : shortstring;\n"
      "begin\n"
      "  s := tostr(x);\n"
      "end;\n"
      "end.\n",
      {{"cutils.pas",
        "unit cutils;\n"
        "interface\n"
        "function tostr(i : qword) : shortstring;    overload;\n"
        "function tostr(i : int64) : shortstring;    overload;\n"
        "function tostr(i : cardinal) : shortstring; overload;\n"
        "function tostr(i : longint) : shortstring;  overload;\n"
        "implementation\n"
        "function tostr(i : qword) : shortstring;    begin tostr := ''; end;\n"
        "function tostr(i : int64) : shortstring;    begin tostr := ''; end;\n"
        "function tostr(i : cardinal) : shortstring; begin tostr := ''; end;\n"
        "function tostr(i : longint) : shortstring;  begin tostr := ''; end;\n"
        "end.\n"}});
  // Local tostr wins -- spelled with the current unit's namespace, not
  // p_cutils's.
  CHECK(contains(out.impl, "p_u::p_tostr") || contains(out.impl, "p_tostr(p_x)"));
  CHECK(!contains(out.impl, "p_cutils::p_tostr("));
}

void test_overload_exact_match_dominates_widening_alternatives() {
  // The fpc bootstrap calls `tostr(status.currentline)` where
  // `currentline` is `longint`. With four overloads (qword, int64,
  // cardinal, longint) only `tostr(longint)` is Exact; the others are
  // widenings or sign changes. Exact must dominate -- no ambiguity.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type ts = record line : longint; end;\n"
      "function tostr(i : qword) : shortstring;    overload;\n"
      "function tostr(i : int64) : shortstring;    overload;\n"
      "function tostr(i : cardinal) : shortstring; overload;\n"
      "function tostr(i : longint) : shortstring;  overload;\n"
      "procedure run;\n"
      "implementation\n"
      "function tostr(i : qword) : shortstring;    begin tostr := ''; end;\n"
      "function tostr(i : int64) : shortstring;    begin tostr := ''; end;\n"
      "function tostr(i : cardinal) : shortstring; begin tostr := ''; end;\n"
      "function tostr(i : longint) : shortstring;  begin tostr := ''; end;\n"
      "var s : ts;\n"
      "    hs : shortstring;\n"
      "procedure run;\n"
      "begin\n"
      "  hs := tostr(s.line);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "static_cast<int32_t>"));
  CHECK(!contains(out.impl, "static_cast<int64_t>"));
  CHECK(!contains(out.impl, "static_cast<uint32_t>"));
  CHECK(!contains(out.impl, "static_cast<uint64_t>"));
}

void test_overload_picks_narrowest_widening_target() {
  // `tostr(qword)` and `tostr(cardinal)` both widen `byte` (uint8) but
  // `cardinal` (uint32) is the narrower target. Without a width-distance
  // tiebreaker the two would tie at `IntWideningSameSign` and the call
  // would be flagged ambiguous.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function tostr(i : qword) : shortstring;    overload;\n"
      "function tostr(i : int64) : shortstring;    overload;\n"
      "function tostr(i : cardinal) : shortstring; overload;\n"
      "function tostr(i : longint) : shortstring;  overload;\n"
      "procedure run;\n"
      "implementation\n"
      "function tostr(i : qword) : shortstring;    begin tostr := ''; end;\n"
      "function tostr(i : int64) : shortstring;    begin tostr := ''; end;\n"
      "function tostr(i : cardinal) : shortstring; begin tostr := ''; end;\n"
      "function tostr(i : longint) : shortstring;  begin tostr := ''; end;\n"
      "procedure run;\n"
      "var b : byte;\n"
      "    s : shortstring;\n"
      "begin\n"
      "  s := tostr(b);\n"
      "end;\n"
      "end.\n");
  // Cast wraps the picked overload's param type (cardinal -> uint32_t)
  // because four candidates remain after arity narrowing.
  CHECK(contains(out.impl, "static_cast<uint32_t>"));
  CHECK(!contains(out.impl, "static_cast<uint64_t>"));
}

void test_overload_int_narrowing_to_shortint_param_is_viable() {
  // Pascal lets a longint argument bind to a shortint value parameter
  // (with a runtime range check). The picker must rank narrowing as
  // viable -- otherwise the only matching overload of a function whose
  // formal parameter is `shortint` falls out as NotViable, the picker
  // returns null, and the call site silently drops the defaulted
  // trailing args. fpc's ogcoff.pas calls
  // `createsection(name, current_settings.alignment.procalign, ...)`
  // where `procalign : longint` and the formal is `aalign : shortint`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tobj = class\n"
      "  function take(name : shortstring; align : shortint;\n"
      "                discard : boolean = true) : longint; overload;\n"
      "  function take(kind : longint; name : shortstring = '') : longint; overload;\n"
      "end;\n"
      "procedure run(o : tobj);\n"
      "implementation\n"
      "function tobj.take(name : shortstring; align : shortint;\n"
      "                   discard : boolean) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.take(kind : longint; name : shortstring) : longint;\n"
      "begin take := 0; end;\n"
      "procedure run(o : tobj);\n"
      "var n : longint;\n"
      "    s : shortstring;\n"
      "    r : longint;\n"
      "begin\n"
      "  r := o.take(s, n);\n"
      "end;\n"
      "end.\n");
  // Picker chose the (string, shortint, ...) overload -- the longint
  // arg got narrowed to int8_t. Cast wrapping forces C++ overload
  // resolution onto the same overload; default-fill adds the
  // boolean=true trailing arg.
  CHECK(contains(out.impl, "static_cast<int8_t>"));
  CHECK(contains(out.impl, "p_take("));
  CHECK(contains(out.impl, "static_cast<bool>(true)"));  // defaulted DiscardDuplicate
}

void test_overload_picks_string_concat_arg_against_string_param() {
  // `'*' + name` is a Pascal string-concat Binary expression. Without
  // deduce_type returning a string type for it, the picker filters
  // every string-typed overload as NotViable and the wrong overload
  // gets selected. fpc's ogbase.pas does this with
  // `createsection('*'+aname, 0, [])`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type tobj = class\n"
      "  function take(kind : longint;\n"
      "                name : shortstring = '') : longint; overload;\n"
      "  function take(name : shortstring;\n"
      "                count : longint = 0) : longint; overload;\n"
      "end;\n"
      "procedure run(o : tobj; suffix : shortstring);\n"
      "implementation\n"
      "function tobj.take(kind : longint; name : shortstring) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.take(name : shortstring; count : longint) : longint;\n"
      "begin take := 0; end;\n"
      "procedure run(o : tobj; suffix : shortstring);\n"
      "var r : longint;\n"
      "begin\n"
      "  r := o.take('*' + suffix);\n"
      "end;\n"
      "end.\n");
  // Picker chose the string-typed overload; the deduced concat type
  // satisfies the rank-Exact match against the string param. The
  // longint default (0) was filled in.
  CHECK(contains(out.impl, "p_take("));
  CHECK(contains(out.impl, "static_cast<int32_t>(0)"));  // default-filled
}

void test_overload_picks_empty_set_literal_against_typed_set_param() {
  // Pascal's `[]` empty set literal is context-typed: it adopts the
  // target param's set element type. Without the picker treating an
  // empty SetLit as Exact-rank against a TySet param, every set-taking
  // overload gets filtered as NotViable and the wrong overload is
  // selected (or default-fill never runs). fpc's ogbase.pas hits this
  // with `createsection('*__image_base__', 0, [])`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  topt = (a, b);\n"
      "  topts = set of topt;\n"
      "  tobj = class\n"
      "    function take(kind : longint) : longint; overload;\n"
      "    function take(name : shortstring; align : shortint;\n"
      "                  opts : topts;\n"
      "                  discard : boolean = true) : longint; overload;\n"
      "  end;\n"
      "procedure run(o : tobj);\n"
      "implementation\n"
      "function tobj.take(kind : longint) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.take(name : shortstring; align : shortint;\n"
      "                   opts : topts; discard : boolean) : longint;\n"
      "begin take := 0; end;\n"
      "procedure run(o : tobj);\n"
      "var r : longint;\n"
      "begin\n"
      "  r := o.take('hello', 0, []);\n"
      "end;\n"
      "end.\n");
  // Picker chose the (string, shortint, topts, bool=default) overload.
  // The empty `[]` lowered as a typed `tp2cc_Set<topt>{}`, and the
  // boolean default was filled in.
  CHECK(contains(out.impl, "tp2cc_Set<t_topt>{}") ||
        contains(out.impl, "t_topts>{}"));
  // Default-fill produced the trailing `true` arg (raw or cast-wrapped
  // depending on whether type-rank picking ran).
  CHECK(contains(out.impl, ", true)") ||
        contains(out.impl, "static_cast<bool>(true)"));
}

void test_overload_default_arg_through_inherited_field_receiver() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "uses base;\n"
      "type\n"
      "  tchild = class(texeoutput)\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "procedure tchild.run;\n"
      "var s : tsection;\n"
      "begin\n"
      "  s := internaldata.createsection('.reloc', 0, [oso_data, oso_keep]);\n"
      "end;\n"
      "end.\n",
      {{"base.pas",
        "unit base;\n"
        "interface\n"
        "type\n"
        "  topt = (oso_data, oso_keep);\n"
        "  topts = set of topt;\n"
        "  tkind = (sec_code, sec_data);\n"
        "  tsection = class end;\n"
        "  tobjdata = class\n"
        "    function createsection(kind : tkind; name : shortstring = '') : tsection; overload;\n"
        "    function createsection(name : shortstring; align : shortint;\n"
        "      opts : topts; discard : boolean = true) : tsection; overload;\n"
        "  end;\n"
        "  texeoutput = class\n"
        "  protected\n"
        "    internaldata : tobjdata;\n"
        "  end;\n"
        "implementation\n"
        "function tobjdata.createsection(kind : tkind; name : shortstring) : tsection;\n"
        "begin createsection := nil; end;\n"
        "function tobjdata.createsection(name : shortstring; align : shortint;\n"
        "  opts : topts; discard : boolean) : tsection;\n"
        "begin createsection := nil; end;\n"
        "end.\n"}});
  CHECK(contains(out.impl, "p_internaldata->p_createsection("));
  CHECK(contains(out.impl, "::rt::tp2cc_Set<::p_base::t_topt>::from_list"));
  CHECK(contains(out.impl, ", true)"));
}

void test_membership_in_empty_set_uses_shared_set_api() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "function keep(code : byte) : boolean;\n"
      "implementation\n"
      "function keep(code : byte) : boolean;\n"
      "begin\n"
      "  keep := not(code in []);\n"
      "end;\n"
      "end.\n");
  // Keep the generic `set.contains(elem)` lowering here; the runtime's
  // EmptySet sentinel must satisfy that same API instead of requiring a
  // dedicated emitter branch for `x in []`.
  CHECK(contains(out.impl, "(::rt::EmptySet{}).contains(p_code)"));
}

void test_set_for_in_lowers_to_ordered_membership_scan() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treg = 0..7;\n"
      "  tregs = set of treg;\n"
      "procedure p;\n"
      "implementation\n"
      "procedure p;\n"
      "var regs : tregs; j : treg; total : integer;\n"
      "begin\n"
      "  total := 0;\n"
      "  for j in regs do\n"
      "    total := total + j;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto tp2cc_set_"));
  CHECK(contains(out.impl, "t_treg tp2cc_item_"));
  CHECK(contains(out.impl, " = 0;"));
  CHECK(contains(out.impl, ".contains(tp2cc_item_"));
  CHECK(contains(out.impl, "p_j = tp2cc_item_"));
  CHECK(contains(out.impl, "if (tp2cc_item_"));
  CHECK(contains(out.impl, " == 7) break;"));
  CHECK(contains(out.impl, "::rt::p_inc(tp2cc_item_"));
}

void test_type_for_in_lowers_to_ordinal_bounds_loop() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tkind = (ka, kb, kc);\n"
      "procedure p;\n"
      "implementation\n"
      "procedure p;\n"
      "var k : tkind; total : integer;\n"
      "begin\n"
      "  total := 0;\n"
      "  for k in tkind do\n"
      "    total := total + 1;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "tp2cc_enum_low_tkind"));
  CHECK(contains(out.impl, "tp2cc_enum_high_tkind"));
  CHECK(contains(out.impl, "p_k = tp2cc_from;"));
  CHECK(contains(out.impl, "::rt::p_inc(p_k);"));
  CHECK(!contains(out.impl, "tp2cc_set_"));
}

void test_for_in_operator_enumerator_precedes_builtin_set() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tregs = set of 0..3;\n"
      "  tenumerator = class\n"
      "    fcurrent : integer;\n"
      "    function MoveNext : boolean;\n"
      "    property Current : integer read fcurrent;\n"
      "  end;\n"
      "procedure p;\n"
      "implementation\n"
      "operator enumerator(s : tregs) : tenumerator;\n"
      "begin\n"
      "  Result := nil;\n"
      "end;\n"
      "function tenumerator.MoveNext : boolean;\n"
      "begin\n"
      "  Result := false;\n"
      "end;\n"
      "procedure p;\n"
      "var regs : tregs; j : integer;\n"
      "begin\n"
      "  for j in regs do\n"
      "    j := j + 1;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "tp2cc_operator_enumerator"));
  CHECK(contains(out.impl, "p_j = tp2cc_enum_"));
  CHECK(contains(out.impl, "->p_fcurrent"));
  CHECK(contains(out.impl, "->p_movenext()"));
  CHECK(!contains(out.impl, "auto tp2cc_set_"));
}

void test_for_in_own_getenumerator_uses_movenext_and_current() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tenumerator = class\n"
      "    fcurrent : integer;\n"
      "    function MoveNext : boolean;\n"
      "    property Current : integer read fcurrent;\n"
      "  end;\n"
      "  tbox = class\n"
      "    function GetEnumerator : tenumerator;\n"
      "  end;\n"
      "procedure p;\n"
      "implementation\n"
      "function tenumerator.MoveNext : boolean;\n"
      "begin\n"
      "  Result := false;\n"
      "end;\n"
      "function tbox.GetEnumerator : tenumerator;\n"
      "begin\n"
      "  Result := nil;\n"
      "end;\n"
      "procedure p;\n"
      "var box : tbox; i : integer;\n"
      "begin\n"
      "  for i in box do\n"
      "    i := i + 1;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_box->p_getenumerator()"));
  CHECK(contains(out.impl, "p_i = tp2cc_enum_"));
  CHECK(contains(out.impl, "->p_fcurrent"));
  CHECK(contains(out.impl, "->p_movenext()"));
}

void test_for_in_open_array_uses_value_length_bounds() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tasmop = (a_none, a_add);\n"
      "function p(const ops : array of tasmop) : boolean;\n"
      "implementation\n"
      "function p(const ops : array of tasmop) : boolean;\n"
      "var op : tasmop;\n"
      "begin\n"
      "  Result := false;\n"
      "  for op in ops do\n"
      "    Result := true;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto&& tp2cc_array_"));
  CHECK(contains(out.impl, " = (p_ops);"));
  CHECK(contains(out.impl, "auto tp2cc_index_"));
  CHECK(contains(out.impl, " = (0);"));
  CHECK(contains(out.impl, "::rt::p_length(tp2cc_array_"));
  CHECK(contains(out.impl, ") - 1);"));
  CHECK(contains(out.impl, "p_op = tp2cc_array_"));
}

void test_for_in_dynamic_array_property_uses_value_length_bounds() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsegmentlist = array of integer;\n"
      "  tgroup = class\n"
      "    fsegmentlist : tsegmentlist;\n"
      "    property SegmentList : tsegmentlist read fsegmentlist;\n"
      "  end;\n"
      "procedure p(g : tgroup);\n"
      "implementation\n"
      "procedure p(g : tgroup);\n"
      "var segment : integer; total : integer;\n"
      "begin\n"
      "  total := 0;\n"
      "  for segment in g.SegmentList do\n"
      "    total := total + segment;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto&& tp2cc_array_"));
  CHECK(contains(out.impl, "p_g->p_fsegmentlist"));
  CHECK(contains(out.impl, "::rt::p_length(tp2cc_array_"));
  CHECK(contains(out.impl, "p_segment = tp2cc_array_"));
}

void test_for_in_nested_getenumerator_return_type_resolves_in_owner() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class end;\n"
      "  taggregate = class\n"
      "  public\n"
      "    type tenumerator = class\n"
      "      fcurrent : titem;\n"
      "      function MoveNext : boolean;\n"
      "      property Current : titem read fcurrent;\n"
      "    end;\n"
      "    function GetEnumerator : tenumerator;\n"
      "  end;\n"
      "procedure p(agg : taggregate);\n"
      "implementation\n"
      "function taggregate.tenumerator.MoveNext : boolean;\n"
      "begin\n"
      "  Result := false;\n"
      "end;\n"
      "function taggregate.GetEnumerator : tenumerator;\n"
      "begin\n"
      "  Result := nil;\n"
      "end;\n"
      "procedure p(agg : taggregate);\n"
      "var item : titem;\n"
      "begin\n"
      "  for item in agg do\n"
      "    item := nil;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_agg->p_getenumerator()"));
  CHECK(contains(out.impl, "->p_movenext()"));
  CHECK(contains(out.impl, "p_item = tp2cc_enum_"));
  CHECK(contains(out.impl, "->p_fcurrent"));
}

void test_for_in_self_uses_current_class_getenumerator() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class end;\n"
      "  taggregate = class\n"
      "  public\n"
      "    type tenumerator = class\n"
      "      fcurrent : titem;\n"
      "      function MoveNext : boolean;\n"
      "      property Current : titem read fcurrent;\n"
      "    end;\n"
      "    function GetEnumerator : tenumerator;\n"
      "    procedure Convert;\n"
      "  end;\n"
      "implementation\n"
      "function taggregate.tenumerator.MoveNext : boolean;\n"
      "begin\n"
      "  Result := false;\n"
      "end;\n"
      "function taggregate.GetEnumerator : tenumerator;\n"
      "begin\n"
      "  Result := nil;\n"
      "end;\n"
      "procedure taggregate.Convert;\n"
      "var item : titem;\n"
      "begin\n"
      "  for item in self do\n"
      "    item := nil;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "this->p_getenumerator()"));
  CHECK(contains(out.impl, "->p_movenext()"));
  CHECK(contains(out.impl, "p_item = tp2cc_enum_"));
  CHECK(contains(out.impl, "->p_fcurrent"));
}

void test_for_in_set_literal_assigns_to_distinct_ordinal_loop_var() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tsuperregister = type word;\n"
      "const\n"
      "  rs_s1 = $20;\n"
      "  rs_s3 = $22;\n"
      "procedure p;\n"
      "implementation\n"
      "procedure p;\n"
      "var i : tsuperregister; total : longint;\n"
      "begin\n"
      "  total := 0;\n"
      "  for i in [rs_s1, rs_s3] do\n"
      "    total := total + ord(i);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "auto tp2cc_set_"));
  CHECK(contains(out.impl, "p_i = tp2cc_item_"));
  CHECK(contains(out.impl, ".contains(tp2cc_item_"));
}

void test_overload_picks_set_difference_arg_against_typed_set_param() {
  // Pascal's `set - set` (set difference) returns the same set type as
  // its operands. Without typing the binary expression here, the
  // picker can't rank a `setvar - [literal]` argument against a
  // typed-set parameter; both overloads fall through as NotViable.
  // fpc's ogcoff.pas hits this with
  // `createsection(name, align, sectiontype2options(...) - [oso_keep])`.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  topt = (a, b);\n"
      "  topts = set of topt;\n"
      "  tobj = class\n"
      "    function take(kind : longint) : longint; overload;\n"
      "    function take(name : shortstring; opts : topts;\n"
      "                  discard : boolean = true) : longint; overload;\n"
      "    function options : topts;\n"
      "  end;\n"
      "procedure run(o : tobj);\n"
      "implementation\n"
      "function tobj.take(kind : longint) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.take(name : shortstring; opts : topts;\n"
      "                   discard : boolean) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.options : topts;\n"
      "begin options := []; end;\n"
      "procedure run(o : tobj);\n"
      "var r : longint;\n"
      "begin\n"
      "  r := o.take('hello', o.options - [a]);\n"
      "end;\n"
      "end.\n");
  // Picker chose the (string, topts, bool=default) overload. The set
  // difference was typed and matched the topts param.
  CHECK(contains(out.impl, "p_take("));
  // Default-fill produced the trailing boolean.
  CHECK(contains(out.impl, ", true)") ||
        contains(out.impl, "static_cast<bool>(true)"));
}

void test_overload_resolves_through_with_block_bare_ident_call() {
  // `with X do begin foo(...) end` -- the bare Ident `foo` resolves
  // against X's class methods, not against the surrounding scope.
  // ogcoff.pas does this with `createsection(...)` inside
  // `with internalobjdata do ...`. If the picker doesn't walk the
  // with-stack, it sees zero candidates, falls back to single-decl
  // resolution, and picks the wrong overload by declaration order.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tkind = (ka, kb);\n"
      "  torder = (oa, ob);\n"
      "  topt = (a, b);\n"
      "  topts = set of topt;\n"
      "  tobj = class\n"
      "    function take(kind : tkind; name : shortstring = '';\n"
      "                  order : torder = oa) : longint; overload;\n"
      "    function take(name : shortstring; align : shortint;\n"
      "                  opts : topts;\n"
      "                  discard : boolean = true) : longint; overload;\n"
      "  end;\n"
      "  thost = class\n"
      "    inner : tobj;\n"
      "    procedure run;\n"
      "  end;\n"
      "implementation\n"
      "function tobj.take(kind : tkind; name : shortstring;\n"
      "                   order : torder) : longint;\n"
      "begin take := 0; end;\n"
      "function tobj.take(name : shortstring; align : shortint;\n"
      "                   opts : topts; discard : boolean) : longint;\n"
      "begin take := 0; end;\n"
      "procedure thost.run;\n"
      "var s : shortstring;\n"
      "    n : longint;\n"
      "    e : topts;\n"
      "    r : longint;\n"
      "begin\n"
      "  with inner do\n"
      "    r := take(s, n, e);\n"
      "end;\n"
      "end.\n");
  // Picker found overload 2 through the with-binding. Default-fill
  // adds the boolean (true), and longint -> shortint narrows.
  CHECK(contains(out.impl, "static_cast<int8_t>"));
  CHECK(contains(out.impl, "static_cast<bool>(true)"));
}

void test_overload_default_arg_extends_arity_disambiguates_cleanly() {
  // Sanity check the converse: `f(x : longint)` and `f(x : longint;
  // y : shortstring)`. A 2-arg call must select the second overload and
  // must NOT be flagged as ambiguous.
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure f(x : longint); overload;\n"
      "procedure f(x : longint; y : shortstring); overload;\n"
      "procedure run;\n"
      "implementation\n"
      "procedure f(x : longint); begin end;\n"
      "procedure f(x : longint; y : shortstring); begin end;\n"
      "procedure run;\n"
      "begin\n"
      "  f(7, 'y');\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_f("));
}

void test_property_read_lowers_to_getter_call() {
  // `obj.x` where `x` is a non-indexed property with a `read getter`
  // accessor must lower the read to the getter call -- emitting a raw
  // member access would skip Pascal's read semantics.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = class\n"
      "  private\n"
      "    function getval : longint;\n"
      "  public\n"
      "    property val : longint read getval;\n"
      "  end;\n"
      "function read_it(b : tbox) : longint;\n"
      "implementation\n"
      "function tbox.getval : longint;\n"
      "begin\n"
      "  getval := 0;\n"
      "end;\n"
      "function read_it(b : tbox) : longint;\n"
      "begin\n"
      "  read_it := b.val;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_b->p_getval()"));
  CHECK(!contains(out.impl, "p_b->p_val"));
}

void test_property_write_lowers_to_setter_call() {
  // Assignment to a property must lower to its `write setter` call.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = class\n"
      "  private\n"
      "    procedure setval(v : longint);\n"
      "  public\n"
      "    property val : longint write setval;\n"
      "  end;\n"
      "procedure write_it(b : tbox);\n"
      "implementation\n"
      "procedure tbox.setval(v : longint);\n"
      "begin\n"
      "end;\n"
      "procedure write_it(b : tbox);\n"
      "begin\n"
      "  b.val := 42;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_b->p_setval("));
  CHECK(!contains(out.impl, "p_b->p_val ="));
}

void test_implicit_write_only_property_assignment_lowers_to_setter_call() {
  int before = error_count();
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = class\n"
      "  private\n"
      "    procedure setval(v : longint);\n"
      "    procedure putslot(i : longint; v : longint);\n"
      "  public\n"
      "    property val : longint write setval;\n"
      "    property slots[i : longint] : longint write putslot;\n"
      "    procedure write_it;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbox.setval(v : longint);\n"
      "begin\n"
      "end;\n"
      "procedure tbox.putslot(i : longint; v : longint);\n"
      "begin\n"
      "end;\n"
      "procedure tbox.write_it;\n"
      "begin\n"
      "  val := 42;\n"
      "  slots[3] := 9;\n"
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "this->p_setval(42);"));
  CHECK(contains(out.impl, "this->p_putslot(3, 9);"));
  CHECK(!contains(out.impl, "p_val ="));
  CHECK(!contains(out.impl, "p_slots"));
}

void test_typecast_property_write_lowers_to_setter_call() {
  // Property assignment must run before raw storage lowering. Even with a
  // class typecast on the base, `T(x).Prop := y` must call the Pascal setter
  // instead of emitting an assignment to a nonexistent C++ property field.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class end;\n"
      "  tbox = class(tbase)\n"
      "  private\n"
      "    procedure setval(v : longint);\n"
      "  public\n"
      "    property val : longint write setval;\n"
      "  end;\n"
      "procedure write_it(b : tbase);\n"
      "implementation\n"
      "procedure tbox.setval(v : longint);\n"
      "begin\n"
      "end;\n"
      "procedure write_it(b : tbase);\n"
      "begin\n"
      "  tbox(b).val := 42;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, ")->p_setval(42);"));
  CHECK(!contains(out.impl, "p_val = 42"));
}

void test_property_read_through_field_lowers_to_field_access() {
  // `read fieldname` (no getter) must lower to the underlying field
  // access, not a phantom `p_fieldname()` method call.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = class\n"
      "  private\n"
      "    fval : longint;\n"
      "  public\n"
      "    property val : longint read fval write fval;\n"
      "  end;\n"
      "function read_it(b : tbox) : longint;\n"
      "implementation\n"
      "function read_it(b : tbox) : longint;\n"
      "begin\n"
      "  read_it := b.val;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_b->p_fval"));
}

void test_property_dotted_field_accessor_lowers_to_field_path() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tdata = record\n"
      "    typ : longint;\n"
      "  end;\n"
      "  tbox = class\n"
      "    data : tdata;\n"
      "    property datatype : longint read data.typ write data.typ;\n"
      "  end;\n"
      "function read_it(b : tbox) : longint;\n"
      "procedure write_it(b : tbox);\n"
      "implementation\n"
      "function read_it(b : tbox) : longint;\n"
      "begin\n"
      "  read_it := b.datatype;\n"
      "end;\n"
      "procedure write_it(b : tbox);\n"
      "begin\n"
      "  b.datatype := 9;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_b->p_data.p_typ"));
  CHECK(!contains(out.impl, "p_datatype"));
}

void test_typecast_property_read_field_ref_uses_backing_field() {
  // A record `const` formal is passed by value here, but lowering the actual
  // still has to spell the source expression correctly. When `Prop` reads a
  // backing field, the value expression must use that field name; the Pascal
  // property name is not a C++ member.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tslot = record\n"
      "    value : longint;\n"
      "  end;\n"
      "  tbase = class end;\n"
      "  tbox = class(tbase)\n"
      "  private\n"
      "    fslot : tslot;\n"
      "  public\n"
      "    property slot : tslot read fslot;\n"
      "  end;\n"
      "procedure take(const s : tslot);\n"
      "procedure run(b : tbase);\n"
      "implementation\n"
      "procedure take(const s : tslot);\n"
      "begin\n"
      "end;\n"
      "procedure run(b : tbase);\n"
      "begin\n"
      "  take(tbox(b).slot);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, ")->p_fslot"));
  CHECK(!contains(out.impl, "p_slot"));
}

void test_default_indexed_property_obj_brackets_calls_getter() {
  // `obj[i]` where `obj` is of a class with a `default` indexed
  // property must lower to the property's read accessor with the
  // index passed in, not to raw subscripting.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class end;\n"
      "  tlist = class\n"
      "  private\n"
      "    function getitem(i : longint) : titem;\n"
      "  public\n"
      "    property items[i : longint] : titem read getitem; default;\n"
      "  end;\n"
      "function pick(l : tlist; i : longint) : titem;\n"
      "implementation\n"
      "function tlist.getitem(i : longint) : titem;\n"
      "begin\n"
      "  getitem := nil;\n"
      "end;\n"
      "function pick(l : tlist; i : longint) : titem;\n"
      "begin\n"
      "  pick := l[i];\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_l->p_getitem(p_i)"));
  CHECK(!contains(out.impl, "p_l[p_i]"));
}

void test_property_read_returning_class_then_default_index_chains_through_getter() {
  // Non-indexed property read followed by a default indexed property.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  titem = class end;\n"
      "  tinner = class\n"
      "  private\n"
      "    function getitem(i : longint) : titem;\n"
      "  public\n"
      "    property items[i : longint] : titem read getitem; default;\n"
      "  end;\n"
      "  touter = class\n"
      "  private\n"
      "    function getinner : tinner;\n"
      "  public\n"
      "    property inner : tinner read getinner;\n"
      "  end;\n"
      "function pick(o : touter; i : longint) : titem;\n"
      "implementation\n"
      "function tinner.getitem(i : longint) : titem;\n"
      "begin\n"
      "  getitem := nil;\n"
      "end;\n"
      "function touter.getinner : tinner;\n"
      "begin\n"
      "  getinner := nil;\n"
      "end;\n"
      "function pick(o : touter; i : longint) : titem;\n"
      "begin\n"
      "  pick := o.inner[i];\n"
      "end;\n"
      "end.\n");
  // First the outer property's getter is called, then the inner class's
  // default-indexed-property getter on the result.
  CHECK(contains(out.impl, "p_o->p_getinner()"));
  CHECK(contains(out.impl, "p_getitem(p_i)"));
  CHECK(!contains(out.impl, "p_o->p_inner["));
}

void test_inline_anon_enum_in_var_decl_members_resolve_in_unit_scope() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "var x : (alpha, beta, gamma);\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "begin\n"
      "  x := alpha;\n"
      "  if x = beta then ;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "p_x = p_alpha"));
  CHECK(contains(out.impl, "(p_x == p_beta)"));
  CHECK(!contains(out.impl, "::rt::p_alpha"));
  CHECK(!contains(out.impl, "::rt::p_beta"));
}

void test_private_enum_type_name_prefers_current_unit() {
  auto out = compile_snippet_with_registry(
      "unit b;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "uses a;\n"
      "type tasmtoken = (b0, b1);\n"
      "var current : tasmtoken;\n"
      "procedure run;\n"
      "begin\n"
      "  current := b0;\n"
      "end;\n"
      "end.\n",
      {{"a.pas",
        "unit a;\n"
        "interface\n"
        "procedure touch;\n"
        "implementation\n"
        "type tasmtoken = (a0, a1);\n"
        "var current : tasmtoken;\n"
        "procedure touch;\n"
        "begin\n"
        "  current := a0;\n"
        "end;\n"
        "end.\n"}});
  CHECK(contains(out.impl, "t_tasmtoken p_current;"));
  CHECK(contains(out.impl, "p_current = p_b0;"));
  CHECK(!contains(out.impl, "::p_a::t_tasmtoken"));
}

void test_inline_enum_labels_export_from_interface_field_types() {
  auto out = compile_snippet_with_registry(
      "program b;\n"
      "uses a;\n"
      "var r : TRec;\n"
      "    x : TFoo;\n"
      "begin\n"
      "  r.state := busy;\n"
      "  x := nil;\n"
      "  x.f := cb;\n"
      "end.\n",
      {{"a.pas",
        "unit a;\n"
        "interface\n"
        "type\n"
        "  TRec = record\n"
        "    state : (idle, busy);\n"
        "  end;\n"
        "  TFoo = class\n"
        "    f : (ca, cb);\n"
        "  end;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.impl, "p_r.p_state = ::p_a::p_busy;"));
  CHECK(contains(out.impl, "p_x->p_f = ::p_a::p_cb;"));
  CHECK(!contains(out.impl, "::rt::p_busy"));
  CHECK(!contains(out.impl, "::rt::p_cb"));
}

void test_inherited_call_routes_through_pascal_picker() {
  // `inherited Foo(args)` looks up Foo in the parent chain. With two
  // ShortString-vs-AnsiString overloads on the parent, Pascal picks
  // the ShortString one for a ShortString argument; the resolver must
  // do the picking and force a static_cast on the arg so C++ overload
  // resolution lands on the same parent overload.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    procedure foo(s : shortstring); overload; virtual;\n"
      "    procedure foo(s : ansistring); overload; virtual;\n"
      "  end;\n"
      "  tderived = class(tbase)\n"
      "    procedure foo(s : shortstring); override;\n"
      "  end;\n"
      "implementation\n"
      "procedure tbase.foo(s : shortstring); begin end;\n"
      "procedure tbase.foo(s : ansistring); begin end;\n"
      "procedure tderived.foo(s : shortstring);\n"
      "var hs : shortstring;\n"
      "begin\n"
      "  inherited foo(hs);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "inherited::p_foo("));
  // Picker disambiguated to the ShortString-typed overload, so the
  // arg is wrapped in static_cast<ShortString<>>(...) -- without that,
  // C++ might pick the AnsiString version when the source is
  // implicitly convertible to both.
  CHECK(contains(out.impl, "static_cast<::rt::tp2cc_ShortString<>"));
}

void test_inherited_exception_create_lowers_as_constructor_call() {
  // `EFoo = class(Exception)` inherits `Create(const Msg: string)` from
  // sysutils' Exception (which is an rt builtin). A call site of the
  // form `EFoo.Create('msg')` must lower as a Pascal constructor call:
  // allocate a fresh instance, then dispatch to `p_create(msg)` on it.
  // Before tobject/exception were in the registry, the constructor
  // lookup found nothing for `EFoo`, the lowering fell back to a plain
  // method call, and the C++ compiler refused
  // `p_efoo::p_create('msg')` (non-static method called without an
  // object).
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  efoo = class(exception);\n"
      "procedure boom;\n"
      "implementation\n"
      "procedure boom;\n"
      "begin\n"
      "  raise efoo.create('bad');\n"
      "end;\n"
      "end.\n");
  // Constructor lowering: allocate, then call p_create on the new
  // instance.
  CHECK(contains(out.impl, "new t_efoo"));
  CHECK(contains(out.impl, "tp2cc_ptr->p_create("));
  // Must NOT be a static-style call.
  CHECK(!contains(out.impl, "p_efoo::p_create("));
}

void test_recursive_call_var_param_gets_param_info_for_reinterpret_ref() {
  // A recursive call to the current function: `resolve_name` rewrites the
  // bare function name to its result slot for assignments like `f := f(...)`,
  // while call lowering must still use the selected function declaration for
  // parameter modes. Without that, a var-T pointer parameter receiving a
  // subclass-field expression misses the reinterpret_storage_ref lowering and
  // C++ rejects the pointer-base mismatch.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TNode = class\n"
      "  end;\n"
      "  TBlock = class(TNode)\n"
      "  end;\n"
      "  TCall = class(TNode)\n"
      "    body : TBlock;\n"
      "  end;\n"
      "function visit(var n : TNode) : boolean;\n"
      "implementation\n"
      "function visit(var n : TNode) : boolean;\n"
      "begin\n"
      "  result := false;\n"
      "  if n is TCall then\n"
      "    result := visit(TCall(n).body) or result;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<t_tnode*>"));
}

void test_with_block_bare_free_lowers_through_static_helper() {
  // `with obj do ... Free;` -- the bare Ident `Free` should resolve to
  // the same null-safe TObject-static helper as the Member form `obj.Free`,
  // anchored on the with-bound expression. Without this, `Free` falls
  // through to the rt-fallback as `::rt::p_free` which doesn't exist.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TFoo = class\n"
      "  end;\n"
      "procedure run(obj : TFoo);\n"
      "implementation\n"
      "procedure run(obj : TFoo);\n"
      "begin\n"
      "  with obj do begin\n"
      "    Free;\n"
      "  end;\n"
      "end;\n"
      "end.\n");
  // The `with` binding is materialised as a `tp2cc_with_<N>` reference;
  // what matters is that the bare `Free` lowered through the static
  // helper anchored on that binding rather than the rt-fallback.
  CHECK(contains(out.impl, "::rt::t_tobject::p_free(tp2cc_with_0);"));
  CHECK(!contains(out.impl, "::rt::p_free"));
}

void test_metaclass_member_base_emits_with_implicit_zero_arg_call() {
  // `TBaseClass(classtype).Create(...)` -- the cast result has metaclass
  // type, so the Member emit goes through the metaclass-member-access
  // branch. That branch must lower the cast's inner `classtype` with
  // its implicit zero-arg call parens; otherwise C++ rejects the result
  // as "invalid use of member function 'classtype' (did you forget the
  // '&' ?)". The base expression must be emitted outside callee context so
  // implicit parameterless calls still get their `()` suffix.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TList = class\n"
      "  end;\n"
      "  TBase = class(TList)\n"
      "    function clone(L : TList) : TBase; virtual;\n"
      "  end;\n"
      "  TBaseClass = class of TBase;\n"
      "implementation\n"
      "function TBase.clone(L : TList) : TBase;\n"
      "begin\n"
      "  result := TBaseClass(classtype).Create(L);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl, "(p_classtype())"));
  CHECK(!contains(out.impl, "(p_classtype))"));
}

void test_class_alias_in_value_position_lowers_to_underlying_metaclass() {
  // `texportalias = texportbase;` is a Pascal type alias (not a new class).
  // In value position the alias name still means the underlying class's
  // metaclass, so passing the alias as a value must lower to the aliased
  // class's metaclass-value function.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  texportbase = class\n"
      "  end;\n"
      "  texportalias = texportbase;\n"
      "procedure registerit(c : tclass);\n"
      "implementation\n"
      "procedure registerit(c : tclass);\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  registerit(texportalias);\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_registerit(tp2cc_metaclass_value_t_texportbase());"));
}

void test_method_definition_on_class_alias_uses_canonical_owner() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treal = class\n"
      "    procedure ping;\n"
      "  end;\n"
      "  talias = treal;\n"
      "implementation\n"
      "procedure talias.ping;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.header, "using t_talias = t_treal*;"));
  CHECK(contains(out.impl, "void t_treal::p_ping()"));
  CHECK(!contains(out.impl, "t_talias::p_ping"));
}

void test_duplicate_class_names_across_units_keep_metaclass_owner_unit() {
  auto out = compile_snippet_with_registry(
      "unit agppcvasm;\n"
      "interface\n"
      "uses aggas, agppcgas;\n"
      "type\n"
      "  tppcinstrwriter = class(tcpuinstrwriter)\n"
      "  end;\n"
      "implementation\n"
      "end.\n",
      {{"aggas.pas",
        "unit aggas;\n"
        "interface\n"
        "type\n"
        "  tcpuinstrwriter = class\n"
        "    constructor create;\n"
        "  end;\n"
        "implementation\n"
        "constructor tcpuinstrwriter.create;\n"
        "begin\n"
        "end;\n"
        "end.\n"},
       {"agppcgas.pas",
        "unit agppcgas;\n"
        "interface\n"
        "uses aggas;\n"
        "type\n"
        "  tppcinstrwriter = class(tcpuinstrwriter)\n"
        "  end;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.header,
                 "struct tp2cc_metaclass_t_tppcinstrwriter : public ::p_aggas::tp2cc_metaclass_t_tcpuinstrwriter {"));
  CHECK(contains(out.header,
                 "static tp2cc_metaclass_t_tppcinstrwriter value = tp2cc_metaclass_t_tppcinstrwriter("));
  CHECK(!contains(out.header,
                  "::p_agppcgas::tp2cc_metaclass_t_tppcinstrwriter"));
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
                 "struct tp2cc_metaclass_t_tchild : public tp2cc_metaclass_t_tbase {"));
  CHECK(contains(out.header,
                 "t_tchild* (*p_create)(int32_t);"));
  CHECK(contains(out.header,
                 "tp2cc_metaclass_t_tchild(tp2cc_metaclass_t_tbase tp2cc_parent, t_tchild* (*tp2cc_p_create)(int32_t))"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create(7);"));
}

void test_metaclass_named_constructor_with_args_lowers_to_descriptor_slot() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tnode = class\n"
      "    constructor create_internal(n : integer);\n"
      "  end;\n"
      "  tnodeclass = class of tnode;\n"
      "var\n"
      "  cls : tnodeclass;\n"
      "  inst : tnode;\n"
      "implementation\n"
      "constructor tnode.create_internal(n : integer);\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  inst := cls.create_internal(7);\n"
      "end.\n");
  CHECK(contains(out.header, "t_tnode* (*p_create_internal)(int32_t);"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create_internal(7);"));
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
                 "static_cast<t_tbase*>(tp2cc_ptr)->p_create();"));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create();"));
}

void test_metaclass_same_signature_constructor_keeps_derived_return_type() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor create;\n"
      "  end;\n"
      "  tchild = class(tbase)\n"
      "    constructor create;\n"
      "  end;\n"
      "  tchildclass = class of tchild;\n"
      "var\n"
      "  cls : tchildclass;\n"
      "  inst : tchild;\n"
      "implementation\n"
      "constructor tbase.create;\n"
      "begin\n"
      "end;\n"
      "constructor tchild.create;\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "  inst := cls.create;\n"
      "end.\n");
  CHECK(contains(out.header,
                 "struct tp2cc_metaclass_t_tchild : public tp2cc_metaclass_t_tbase {"));
  CHECK(contains(out.header, "t_tchild* (*p_create)();"));
  CHECK(contains(out.header,
                 "static tp2cc_metaclass_t_tchild value = "
                 "tp2cc_metaclass_t_tchild(tp2cc_metaclass_t_tbase("));
  CHECK(contains(out.impl, "p_inst = p_cls->p_create();"));
}

void test_inherited_metaclass_constructor_uses_declaring_unit_type_lookup() {
  auto out = compile_snippet_with_registry(
      "unit nx86con;\n"
      "interface\n"
      "uses ncon, ncgcon;\n"
      "type\n"
      "  tx86realconstnode = class(tcgrealconstnode)\n"
      "  end;\n"
      "implementation\n"
      "end.\n",
      {{"cpuinfo.pas",
        "unit cpuinfo;\n"
        "interface\n"
        "type\n"
        "  bestreal = double;\n"
        "implementation\n"
        "end.\n"},
       {"ncon.pas",
        "unit ncon;\n"
        "interface\n"
        "uses cpuinfo, node;\n"
        "type\n"
        "  trealconstnode = class(tnode)\n"
        "    constructor create(v : bestreal);\n"
        "  end;\n"
        "implementation\n"
        "end.\n"},
       {"node.pas",
        "unit node;\n"
        "interface\n"
        "type\n"
        "  tnodetype = (nt);\n"
        "  tnode = class\n"
        "    constructor create(t : tnodetype);\n"
        "  end;\n"
        "implementation\n"
        "end.\n"},
       {"ncgcon.pas",
        "unit ncgcon;\n"
        "interface\n"
        "uses ncon;\n"
        "type\n"
        "  tcgrealconstnode = class(trealconstnode)\n"
        "  end;\n"
        "implementation\n"
        "end.\n"}});
  CHECK(contains(out.header,
                 "t_tx86realconstnode* (*p_create)(::p_cpuinfo::t_bestreal);"));
  CHECK(contains(out.header, "+[](::p_node::t_tnodetype p_t)"));
  CHECK(!contains(out.header, "t_tx86realconstnode* (*p_create)(t_bestreal);"));
  CHECK(!contains(out.header, "+[](t_tnodetype p_t)"));
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
  CHECK(contains(out.header, "bool p_isbase(t_tbase* p_x, ::rt::t_tclass p_c);"));
  CHECK(contains(out.impl, "p_result = p_x->p_inheritsfrom(p_c);"));
}

void test_inheritsfrom_is_boolean_for_short_circuit_and() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "  end;\n"
      "function isbase(x : tbase; c : tclass) : boolean;\n"
      "implementation\n"
      "function isbase(x : tbase; c : tclass) : boolean;\n"
      "begin\n"
      "  if x.inheritsfrom(c) and assigned(x) then\n"
      "    isbase := true\n"
      "  else\n"
      "    isbase := false;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "if ((p_x->p_inheritsfrom(p_c) && ::rt::p_assigned(p_x)))"));
  CHECK(!contains(out.impl, "p_x->p_inheritsfrom(p_c) & ::rt::p_assigned(p_x)"));
}

void test_indexed_property_result_classtype_autocalls() {
  int before = error_count();
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
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_result = (p_b->p_getitem(p_i)->p_classtype() == p_c);"));
}

void test_implicit_indexed_property_result_classtype_autocalls() {
  int before = error_count();
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
      "end;\n"
      "end.\n");
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_result = (this->p_getitem(p_i)->p_classtype() == p_c);"));
}

void test_indexed_implicit_property_in_method_body() {
  int before = error_count();
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
  CHECK(error_count() == before);
  CHECK(contains(out.impl, "p_result = this->p_get(0);"));
  CHECK(!contains(out.impl, "p_items[0]"));
}

void test_indexed_implicit_property_result_write_in_method_body() {
  int before = error_count();
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
  CHECK(error_count() == before);
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
  CHECK(contains(out.impl, "auto tp2cc_with_0 = static_cast<t_tnode*>(p_p);"));
  CHECK(!contains(out.impl, "auto& tp2cc_with_0 = static_cast<t_tnode*>(p_p);"));
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

void test_val_var_arg_typecast_reinterprets_storage() {
  // Pascal's `Val(S; var V; var Code)` writes to caller storage, so a
  // call-site typecast on the var slot -- `Val(s, aword(result), code)`
  // -- must reinterpret `result`'s storage as the cast target. Without
  // marking Val's 2nd slot as mutable_ref in the builtin-helper info
  // table, the cast lowers as a value rvalue and the unsigned `p_val`
  // overload (which takes `UInt&`) fails to match.
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "var\n"
      "  s : shortstring;\n"
      "  result : longint;\n"
      "  code : integer;\n"
      "begin\n"
      "  val(s, cardinal(result), code);\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "::rt::tp2cc_reinterpret_storage_ref<uint32_t>(p_result)"));
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
                 "p_take(::rt::tp2cc_reinterpret_storage_ref<t_tsym*>(p_l));"));
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
                 "p_take(::rt::tp2cc_reinterpret_storage_ref<t_tbase*>(p_c));"));
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
                 "p_touch(::rt::tp2cc_reinterpret_storage_ref<t_tsym*>(p_l));"));
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
                 "p_result = dynamic_cast<t_tprocdef*>(p_pd)->p_inlininginfo->p_flags;"));
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
  CHECK(!contains(out.impl, "new ::std::remove_pointer_t<t_pbox>{}"));
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
      "using t_titems = ::rt::tp2cc_Array<t_titem, 0, 65536>;");
  size_t class_pos = out.header.find("struct t_tbox : public ::rt::t_tobject");
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
  CHECK(contains(out.impl, "p_result = static_cast<t_tchild*>(p_p);"));
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
  CHECK(contains(out.impl, "p_result = static_cast<t_tchild*>(p_p)->p_next;"));
}

void test_addr_of_reference_class_typecast_field_uses_object_pointer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  plongint = ^longint;\n"
      "  tbase = class end;\n"
      "  tchild = class(tbase)\n"
      "    value : longint;\n"
      "  end;\n"
      "function field_addr(p : tbase) : plongint;\n"
      "implementation\n"
      "function field_addr(p : tbase) : plongint;\n"
      "begin\n"
      "  field_addr := @tchild(p).value;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = reinterpret_cast<int32_t*>("
                 "::rt::tp2cc_pointer_byte_offset(static_cast<t_tchild*>(p_p), "
                 "offsetof(t_tchild, p_value)));"));
  CHECK(!contains(out.impl, "::rt::tp2cc_byte_offset((&p_p), "
                            "offsetof(t_tchild, p_value))"));
}

void test_addr_of_object_pointer_field_returns_typed_pointer() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tinfo = record\n"
      "    line : longint;\n"
      "  end;\n"
      "  pinfo = ^tinfo;\n"
      "  pai = ^tai;\n"
      "  tai = object\n"
      "    fileinfo : tinfo;\n"
      "  end;\n"
      "function last_fileinfo(last : pai) : pinfo;\n"
      "implementation\n"
      "function last_fileinfo(last : pai) : pinfo;\n"
      "begin\n"
      "  last_fileinfo := @last^.fileinfo;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "p_result = reinterpret_cast<t_tinfo*>("
                 "::rt::tp2cc_pointer_byte_offset(p_last, "
                 "offsetof(t_tai, p_fileinfo)));"));
}

void test_reference_class_typecast_field_assignment_uses_assignment_operator() {
  auto out = compile_snippet_with_registry(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbox = record\n"
      "    n : longint;\n"
      "  end;\n"
      "  tbase = class end;\n"
      "  tchild = class(tbase)\n"
      "    value : tbox;\n"
      "  end;\n"
      "operator :=(const n : longint) : tbox;\n"
      "procedure store(p : tbase; n : longint);\n"
      "implementation\n"
      "operator :=(const n : longint) : tbox;\n"
      "begin\n"
      "  result.n := n;\n"
      "end;\n"
      "procedure store(p : tbase; n : longint);\n"
      "begin\n"
      "  tchild(p).value := n;\n"
      "end;\n"
      "end.\n");
  CHECK(contains(out.impl,
                 "static_cast<t_tchild*>(p_p)->p_value = "
                 "::p_u::tp2cc_operator_assign_params_const_name_longint_ret_name_tbox(p_n);"));
  CHECK(!contains(out.impl, "static_cast<t_tchild*>(p_p)->p_value = p_n;"));
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
  CHECK(contains(out.impl, "dynamic_cast<t_tchild*>(p_p) != nullptr"));
  CHECK(contains(out.impl, "p_result = dynamic_cast<t_tchild*>(p_p);"));
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

void test_multiple_case_blocks_in_record_errors_out() {
  int before = error_count();
  (void)compile_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    case b: integer of\n"
      "      0: (x: integer);\n"
      "    case c: integer of\n"
      "      1: (y: integer);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(error_count() > before);
}

}  // namespace

int main() {
  RUN_TEST(test_empty_unit_skeleton);
  RUN_TEST(test_emitted_makefile_tracks_sources_headers_and_program);
  RUN_TEST(test_program_registers_unit_finalizers);
  RUN_TEST(test_uses_become_includes_without_open_namespaces);
  RUN_TEST(test_scalar_const);
  RUN_TEST(test_typed_scalar_const);
  RUN_TEST(test_string_literal_pchar_context_uses_static_c_literal_storage);
  RUN_TEST(test_string_literal_pchar_assignment_and_call_contexts);
  RUN_TEST(test_typed_scalar_const_wraps_to_destination_value);
  RUN_TEST(test_enum_type);
  RUN_TEST(test_enum_type_with_explicit_values);
  RUN_TEST(test_packed_record_uses_byte_sized_enum_fields);
  RUN_TEST(test_packenum_two_uses_word_sized_enum);
  RUN_TEST(test_minenumsize_alias_uses_packenum_rules);
  RUN_TEST(test_mode_tp_switches_default_enum_size_to_byte);
  RUN_TEST(test_mode_objfpc_restores_default_enum_size_to_longword);
  RUN_TEST(test_subrange_type_uses_minimal_ordinal_storage);
  RUN_TEST(test_subrange_type_accepts_constant_ordinal_intrinsics);
  RUN_TEST(test_subrange_bound_folds_ord_high_enum_type);
  RUN_TEST(test_char_subrange_preserves_char_storage);
  RUN_TEST(test_packed_record_shortstring_field_emits_exact_layout_asserts);
  RUN_TEST(test_packed_record_array_field_keeps_array_wrapper_with_exact_layout_asserts);
  RUN_TEST(test_nested_variant_record_preserves_inner_tag_and_recursively_emits_union);
  RUN_TEST(test_packed_variant_record_emits_packed_case_layout_asserts);
  RUN_TEST(test_packed_variant_record_many_cases_uses_linear_size_max);
  RUN_TEST(test_multiple_case_blocks_in_record_errors_out);
  RUN_TEST(test_packed_record_array_index_reports_error);
  RUN_TEST(test_packed_record_nested_scalar_value_uses_unaligned_load);
  RUN_TEST(test_packed_record_nested_scalar_assignment_reports_error);
  RUN_TEST(test_packed_record_nested_scalar_var_arg_reports_error);
  RUN_TEST(test_packed_record_method_call_reports_error);
  RUN_TEST(test_packed_record_char_array_index_is_allowed);
  RUN_TEST(test_packed_record_shortstring_array_index_is_allowed);
  RUN_TEST(test_explicit_enum_array_bounds_use_ordinal_range);
  RUN_TEST(test_distinct_ordinal_array_bounds_use_underlying_range);
  RUN_TEST(test_boolean_family_array_bounds_use_boolean_domain);
  RUN_TEST(test_signed_ordinal_array_bounds_preserve_negative_low);
  RUN_TEST(test_imported_const_array_bounds_are_folded);
  RUN_TEST(test_imported_const_array_bounds_use_declaring_unit_scope);
  RUN_TEST(test_unsupported_fixed_array_index_reports_error_and_stays_array_typed);
  RUN_TEST(test_low_high_use_resolved_pascal_type);
  RUN_TEST(test_low_high_on_set_type_uses_element_bounds);
  RUN_TEST(test_low_high_on_local_array_type_lowers_to_index_bounds);
  RUN_TEST(test_system_qualified_low_high_lowers_like_unqualified);
  RUN_TEST(test_system_qualified_runtime_exports_use_implicit_unit);
  RUN_TEST(test_system_member_access_respects_value_shadowing);
  RUN_TEST(test_char_array_typed_const_uses_explicit_array_literal_helper);
  RUN_TEST(test_char_array_assignment_uses_explicit_array_literal_helper);
  RUN_TEST(test_nested_array_typed_const_initializes_each_array_data_member);
  RUN_TEST(test_single_record_array_typed_const_wraps_array_storage);
  RUN_TEST(test_typed_const_shortstring_literals_use_target_capacity);
  RUN_TEST(test_shortstring_length_literal_capacity_is_constant_folded);
  RUN_TEST(test_named_type_alias);
  RUN_TEST(test_ansistring_builtin_maps_to_runtime_type);
  RUN_TEST(test_widechar_builtin_maps_to_16bit_ordinal);
  RUN_TEST(test_ansichar_and_pansichar_builtin_maps_to_char_carriers);
  RUN_TEST(test_ord_storage_view_for_char_assignment_inc_and_address);
  RUN_TEST(test_ord_storage_view_for_shortstring_length_byte);
  RUN_TEST(test_chr_storage_view_for_byte_assignment_inc_and_var_arg);
  RUN_TEST(test_ord_char_value_has_byte_result_type);
  RUN_TEST(test_ord_pchar_offset_deref_uses_char_byte_value);
  RUN_TEST(test_ord_value_result_type_follows_ordinal_operand);
  RUN_TEST(test_ord_boolean_expression_lowers_to_numeric_value);
  RUN_TEST(test_set_type_alias);
  RUN_TEST(test_var_extern_in_header_and_def_in_impl);
  RUN_TEST(test_out_parameter_emits_like_var_reference);
  RUN_TEST(test_const_pointer_parameter_stays_value_abi);
  RUN_TEST(test_constref_record_parameter_emits_const_reference);
  RUN_TEST(test_const_fixed_array_parameter_stays_value_abi);
  RUN_TEST(test_const_fixed_record_array_parameter_stays_value_abi);
  RUN_TEST(test_const_fixed_classref_array_parameter_stays_value_abi);
  RUN_TEST(test_proc_signature_in_header);
  RUN_TEST(test_noreturn_directive_emits_cxx_attribute);
  RUN_TEST(test_typed_array_const);
  RUN_TEST(test_typed_array_const_with_inline_subrange_element_type);
  RUN_TEST(test_free_function_trailing_default_argument_is_lowered);
  RUN_TEST(test_method_trailing_default_argument_is_lowered);
  RUN_TEST(test_unit_qualified_trailing_default_argument_is_lowered);
  RUN_TEST(test_imported_default_argument_resolves_in_declaring_unit);
  RUN_TEST(test_imported_nil_default_argument_qualifies_procedural_type);
  RUN_TEST(test_imported_default_argument_qualifies_declaring_unit_const);
  RUN_TEST(test_unit_qualified_variable_assignment_is_storage_designator);
  RUN_TEST(test_external_used_unit_qualified_call_keeps_namespace_cxx_name);
  RUN_TEST(test_method_pointer_trailing_default_nil_is_lowered_as_empty_value);
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
  RUN_TEST(test_raise_at_address_and_frame_metadata_is_accepted_and_discarded);
  RUN_TEST(test_try_except_multiple_handlers_start_with_if_and_base_pointer_cast);
  RUN_TEST(test_sysutils_exception_handlers_are_qualified_and_pointer_bound);
  RUN_TEST(test_char_plus_cast_uses_string_concat);
  RUN_TEST(test_nul_char_plus_cast_uses_string_concat);
  RUN_TEST(test_embedded_nul_string_literal_uses_explicit_length_builder);
  RUN_TEST(test_shortstring_assignment_uses_pascal_string_helper);
  RUN_TEST(test_string_typecast_from_pchar_lowers_to_shortstring);
  RUN_TEST(test_h_plus_string_typecast_from_pchar_lowers_to_ansistring);
  RUN_TEST(test_var_shortstring_call_keeps_lvalue_storage);
  RUN_TEST(test_var_shortstring_capacity_mismatch_uses_storage_ref);
  RUN_TEST(test_var_runtime_shortstring_alias_uses_storage_ref);
  RUN_TEST(test_procvar_var_shortstring_call_uses_storage_ref);
  RUN_TEST(test_var_ansistring_call_keeps_lvalue_storage);
  RUN_TEST(test_overloaded_string_and_bool_call_keeps_boolean_argument_raw);
  RUN_TEST(test_custom_operator_declarations_emit_cxx_operators_and_assignment_helpers);
  RUN_TEST(test_overloaded_call_result_type_uses_selected_decl);
  RUN_TEST(test_nested_overload_result_type_is_used_for_outer_overload);
  RUN_TEST(test_pchar_cast_argument_converts_to_string_value);
  RUN_TEST(test_integer_and_or_stays_bitwise);
  RUN_TEST(test_nested_boolean_function_and_short_circuits);
  RUN_TEST(test_untyped_boolean_const_and_short_circuits);
  RUN_TEST(test_overloaded_boolean_call_result_short_circuits);
  RUN_TEST(test_nested_untyped_var_forwarding_stays_pointer_value);
  RUN_TEST(test_nested_untyped_const_forwarding_stays_pointer_value);
  RUN_TEST(test_untyped_method_call_on_variable_uses_storage_address);
  RUN_TEST(test_fillchar_uses_storage_address_for_pointer_slots);
  RUN_TEST(test_move_uses_storage_addresses_for_source_and_destination_slots);
  RUN_TEST(test_indexword_nil_pointer_deref_uses_pointer_actual);
  RUN_TEST(test_move_pointer_derefs_use_pointer_actuals);
  RUN_TEST(test_string_index_buffer_helpers_use_storage_addresses);
  RUN_TEST(test_string_index_char_coerces_to_shortstring_formal);
  RUN_TEST(test_block_io_string_index_uses_storage_addresses);
  RUN_TEST(test_blockwrite_fixed_array_uses_const_storage_address);
  RUN_TEST(test_byte_array_typecast_index_read_builds_value);
  RUN_TEST(test_local_byte_array_typecast_index_read_builds_value);
  RUN_TEST(test_array_typecast_index_assignment_uses_storage_view);
  RUN_TEST(test_untyped_array_value_cast_copies_caller_storage);
  RUN_TEST(test_untyped_record_value_cast_copies_caller_storage);
  RUN_TEST(test_text_typecast_over_pointer_deref_keeps_file_lvalue);
  RUN_TEST(test_visible_pointer_alias_cast_uses_qualified_type_name);
  RUN_TEST(test_local_pointer_alias_cast_uses_local_type_name);
  RUN_TEST(test_runtime_alias_type_names_are_explicitly_qualified);
  RUN_TEST(test_tdatetime_and_runtime_date_time_lower_through_rt);
  RUN_TEST(test_runtime_aliases_cover_currency_systemtime_and_pansistring);
  RUN_TEST(test_string_comparison_uses_runtime_operator_resolution);
  RUN_TEST(test_tmethod_type_name_is_explicitly_qualified);
  RUN_TEST(test_local_enum_members_do_not_fall_back_to_runtime);
  RUN_TEST(test_sizeof_visible_type_uses_type_name_not_identifier_lookup);
  RUN_TEST(test_sizeof_qualified_type_uses_type_name_not_value_namespace);
  RUN_TEST(test_unit_type_value_duplicates_across_sections_report_error);
  RUN_TEST(test_sizeof_own_implementation_private_qualified_names);
  RUN_TEST(test_primitive_cast_assign_reinterprets_storage);
  RUN_TEST(test_primitive_cast_read_reinterprets_storage);
  RUN_TEST(test_addr_of_primitive_cast_returns_typed_pointer);
  RUN_TEST(test_inc_untyped_primitive_cast_reinterprets_storage_by_byte_copy);
  RUN_TEST(test_inc_primitive_cast_reinterprets_storage);
  RUN_TEST(test_untyped_array_view_index_uses_byte_load_store);
  RUN_TEST(test_aggregate_to_primitive_cast_reinterprets_bytes);
  RUN_TEST(test_nested_aggregate_to_primitive_cast_reinterprets_source_bytes);
  RUN_TEST(test_nested_untyped_aggregate_to_primitive_cast_reads_caller_storage);
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
  RUN_TEST(test_class_var_static_emission_and_access);
  RUN_TEST(test_strict_visibility_lowers_to_cxx_access_sections);
  RUN_TEST(test_metaclass_support_can_call_strict_protected_class_methods);
  RUN_TEST(test_class_var_inherited_duplicate_reports_error);
  RUN_TEST(test_tobject_runtime_helpers_lower_in_method_body);
  RUN_TEST(test_classname_uses_metaclass_descriptor_slot);
  RUN_TEST(test_tobject_cast_preserves_pointer_semantics_for_free);
  RUN_TEST(test_parameterless_proc_assignment_keeps_designator);
  RUN_TEST(test_method_pointer_type_and_bound_assignment_emit);
  RUN_TEST(test_method_pointer_record_cast_reinterprets_same_storage);
  RUN_TEST(test_unbound_method_address_uses_thunk_code);
  RUN_TEST(test_internal_helpers_avoid_double_underscores);
  RUN_TEST(test_for_loop_uses_resolved_global_control_var);
  RUN_TEST(test_case_statement_lowers_to_if_chain);
  RUN_TEST(test_string_case_statement_lowers_to_if_chain);
  RUN_TEST(test_string_case_statement_with_char_label_uses_string_compare);
  RUN_TEST(test_string_case_statement_with_upcase_selector);
  RUN_TEST(test_string_case_statement_with_builtin_upcase_selector);
  RUN_TEST(test_char_case_statement_uses_direct_comparison);
  RUN_TEST(test_const_object_param_uses_mutable_ref);
  RUN_TEST(test_parameterless_procvar_stmt_autocalls);
  RUN_TEST(test_direct_procvar_var_decl_uses_named_function_pointer_syntax);
  RUN_TEST(test_runtime_builtin_stmt_autocalls);
  RUN_TEST(test_prefetch_intrinsic_statement_is_noop);
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
  RUN_TEST(test_cross_unit_enum_set_literal_keeps_exported_enum_type);
  RUN_TEST(test_duplicate_enum_type_names_keep_set_literal_member_unit);
  RUN_TEST(test_duplicate_record_type_names_keep_visible_unit_owner);
  RUN_TEST(test_duplicate_alias_type_names_keep_visible_unit_owner);
  RUN_TEST(test_runtime_enum_members_resolve_explicitly);
  RUN_TEST(test_sysutils_executeprocess_accepts_execute_flags);
  RUN_TEST(test_ansicomparefilename_resolves_explicitly);
  RUN_TEST(test_explicit_set_cast_uses_runtime_helper);
  RUN_TEST(test_named_set_const_assigns_to_compatible_set_via_runtime_helper);
  RUN_TEST(test_compatible_set_actual_stays_viable_in_overload_resolution);
  RUN_TEST(test_set_range_literal_uses_integer_ordinal_loop);
  RUN_TEST(test_local_var_inline_anon_enum_resolves_members);
  RUN_TEST(test_set_of_inline_enum_uses_named_carrier);
  RUN_TEST(test_record_field_inline_enum_uses_unit_carrier);
  RUN_TEST(test_unresolved_free_identifier_reports_error_without_rt_fallback);
  RUN_TEST(test_runtime_math_surface_resolves_explicitly);
  RUN_TEST(test_runtime_endian_helpers_resolve_explicitly);
  RUN_TEST(test_runtime_rotate_helpers_resolve_explicitly);
  RUN_TEST(test_runtime_string_and_memory_helpers_resolve_explicitly);
  RUN_TEST(test_sysutils_setdirseparators_resolves_qualified_and_unqualified);
  RUN_TEST(test_and_with_not_of_xor_short_circuits);
  RUN_TEST(test_r_plus_routes_narrowing_assignment_through_range_check);
  RUN_TEST(test_q_minus_routes_signed_negate_through_wrap_helper);
  RUN_TEST(test_q_plus_routes_integer_arith_through_checked_helpers);
  RUN_TEST(test_q_minus_routes_signed_binary_arith_through_wrap_helpers);
  RUN_TEST(test_qword_const_cast_produces_64bit_literal_for_shifts);
  RUN_TEST(test_shift_ops_lower_through_pascal_helpers);
  RUN_TEST(test_integer_div_mod_lower_through_pascal_helpers);
  RUN_TEST(test_addr_of_pointer_deref_field_uses_offsetof_arithmetic);
  RUN_TEST(test_addr_of_array_value_uses_context_selecting_proxy);
  RUN_TEST(test_addr_of_pointer_deref_array_field_uses_offsetof_proxy);
  RUN_TEST(test_addr_of_pointer_deref_array_index_uses_pointer_offset);
  RUN_TEST(test_addr_of_dynamic_array_targets_array_handle_not_data_proxy);
  RUN_TEST(test_set_to_int_cast_uses_endian_safe_helper);
  RUN_TEST(test_untyped_const_method_thunk_keeps_raw_storage_pointer);
  RUN_TEST(test_untyped_const_distinguishes_pointer_slot_from_pointed_bytes);
  RUN_TEST(test_untyped_const_pointer_assignment_drops_qualifier_explicitly);
  RUN_TEST(test_addr_of_untyped_const_pointer_assignment_drops_qualifier_explicitly);
  RUN_TEST(test_untyped_const_pointer_cast_drops_qualifier_explicitly);
  RUN_TEST(test_untyped_const_temporary_uses_addressable_helper);
  RUN_TEST(test_class_types_lower_to_pointers_and_implicit_tobject);
  RUN_TEST(test_forward_class_decl_only_emits_one_struct_body);
  RUN_TEST(test_empty_inherited_class_decl_emits_real_struct);
  RUN_TEST(test_abstract_method_emits_fail_fast_virtual_body);
  RUN_TEST(test_final_virtual_method_emits_cxx_final);
  RUN_TEST(test_final_override_method_emits_override_final);
  RUN_TEST(test_final_nonvirtual_method_reports_error);
  RUN_TEST(test_override_final_method_reports_error);
  RUN_TEST(test_reintroduce_same_signature_inherited_virtual_reports_error);
  RUN_TEST(test_reintroduce_same_signature_inherited_virtual_constructor_reports_error);
  RUN_TEST(test_reintroduce_different_signature_is_still_accepted);
  RUN_TEST(test_plain_different_signature_inherited_virtual_is_accepted);
  RUN_TEST(test_explicit_reintroduce_same_signature_inherited_virtual_method_reports_error);
  RUN_TEST(test_explicit_reintroduce_same_signature_inherited_virtual_constructor_reports_error);
  RUN_TEST(test_explicit_reintroduce_nonvirtual_same_signature_method_reports_error);
  RUN_TEST(test_explicit_reintroduce_nonvirtual_same_signature_constructor_reports_error);
  RUN_TEST(test_plain_same_signature_inherited_virtual_method_reports_error);
  RUN_TEST(test_plain_same_signature_inherited_virtual_constructor_reports_error);
  RUN_TEST(test_override_same_signature_inherited_virtual_method_is_accepted);
  RUN_TEST(test_override_same_signature_inherited_virtual_constructor_is_accepted);
  RUN_TEST(test_object_virtual_same_signature_inherited_virtual_is_accepted);
  RUN_TEST(test_object_plain_same_signature_inherited_virtual_reports_error);
  RUN_TEST(test_pointer_sized_integer_aliases_lower_through_rt);
  RUN_TEST(test_tclass_alias_lowers_through_rt);
  RUN_TEST(test_corba_interface_emits_pure_virtual_base_and_pointer_calls);
  RUN_TEST(test_class_constructor_call_allocates_instance);
  RUN_TEST(test_nested_record_type_emits_inside_owner_scope);
  RUN_TEST(test_nested_class_type_emits_qualified_owner_and_method_scope);
  RUN_TEST(test_class_lifecycle_methods_run_from_unit_hooks);
  RUN_TEST(test_abstract_class_constructor_call_warns);
  RUN_TEST(test_class_constructor_trailing_default_argument_is_lowered);
  RUN_TEST(test_object_constructor_call_uses_base_method_on_self);
  RUN_TEST(test_implicit_tobject_inherited_constructor_autocalls);
  RUN_TEST(test_inherited_destroy_autocalls_through_non_overriding_parent);
  RUN_TEST(test_class_self_and_free_use_pointer_semantics);
  RUN_TEST(test_metaclass_alias_and_concrete_class_value_lowering);
  RUN_TEST(test_metaclass_virtual_class_method_dispatch);
  RUN_TEST(test_bare_inherited_in_function_value_context_calls_current_parent_method);
  RUN_TEST(test_bare_inherited_statement_forwards_current_method_params);
  RUN_TEST(test_static_class_method_address_keeps_plain_function_pointer);
  RUN_TEST(test_metaclass_class_method_proc_value_reports_error);
  RUN_TEST(test_metaclass_cast_keeps_concrete_descriptor);
  RUN_TEST(test_metaclass_value_can_flow_through_pointer_storage);
  RUN_TEST(test_class_identifier_value_lowers_to_metaclass_descriptor);
  RUN_TEST(test_inline_anonymous_enum_class_field_resolves_members);
  RUN_TEST(test_inline_anonymous_packed_record_var_lowers_to_struct);
  RUN_TEST(test_inline_anonymous_variant_record_lowers_to_union);
  RUN_TEST(test_variant_record_payload_fields_use_byte_storage);
  RUN_TEST(test_variant_record_payload_storage_composes_through_members);
  RUN_TEST(test_with_variant_record_payload_keeps_field_storage);
  RUN_TEST(test_with_variant_payload_object_field_method_keeps_payload_storage);
  RUN_TEST(test_variant_record_payload_member_read_address_and_untyped_actual);
  RUN_TEST(test_variant_record_payload_storage_composes_through_indexes);
  RUN_TEST(test_variant_record_payload_index_read_address_and_untyped_actual);
  RUN_TEST(test_variant_payload_object_array_index_method_keeps_payload_storage);
  RUN_TEST(test_variant_record_payload_array_address_uses_payload_address_proxy);
  RUN_TEST(test_variant_record_payload_shortstring_index_stays_on_storage);
  RUN_TEST(test_variant_record_pointer_payload_passes_slot_to_allocation_builtins);
  RUN_TEST(test_shadowed_getmem_does_not_use_runtime_slot_helper);
  RUN_TEST(test_variant_record_pointer_payload_distinguishes_slot_from_pointee);
  RUN_TEST(test_variant_record_pointer_payload_typecast_keeps_slot_storage);
  RUN_TEST(test_variant_class_payload_member_loads_pointer_before_field_offset);
  RUN_TEST(test_variant_payload_after_reference_field_loads_reference_before_offset);
  RUN_TEST(test_variant_payload_object_field_method_keeps_payload_storage);
  RUN_TEST(test_packed_field_typed_cast_assignment_uses_memcpy_store);
  RUN_TEST(test_packed_record_typecast_field_assignment_uses_storage_view);
  RUN_TEST(test_record_typecast_field_read_uses_storage_view);
  RUN_TEST(test_local_record_typecast_field_read_uses_storage_view);
  RUN_TEST(test_inc_packed_field_routes_through_memcpy_inc);
  RUN_TEST(test_inc_local_packed_pointee_field_routes_through_memcpy_inc);
  RUN_TEST(test_unaligned_typed_deref_read_uses_bytewise_load);
  RUN_TEST(test_unaligned_typed_deref_write_uses_bytewise_store);
  RUN_TEST(test_unaligned_typed_deref_inc_dec_use_unaligned_helpers);
  RUN_TEST(test_unaligned_variant_payload_uses_payload_storage_address);
  RUN_TEST(test_unaligned_storage_address_and_untyped_actual_use_raw_address);
  RUN_TEST(test_typecast_over_unaligned_storage_preserves_unaligned_helpers);
  RUN_TEST(test_packed_record_field_through_pointer_slot_stays_bytewise);
  RUN_TEST(test_unaligned_pointer_field_read_uses_bytewise_load);
  RUN_TEST(test_unaligned_storage_var_arg_is_rejected_instead_of_ref_bound);
  RUN_TEST(test_overload_picks_shortstring_target_for_short_string_arg);
  RUN_TEST(test_overload_picks_pchar_to_shortstring_over_ansistring);
  RUN_TEST(test_sizeof_lowers_to_int32_to_match_pascal_longint_semantics);
  RUN_TEST(test_overload_picks_unsigned_widening_over_sign_change);
  RUN_TEST(test_overload_aggregates_candidates_across_uses_chain);
  RUN_TEST(test_overload_ambiguous_default_arg_vs_no_default_reports_error);
  RUN_TEST(test_overload_ambiguous_two_default_arg_overloads_reports_error);
  RUN_TEST(test_overload_default_arg_extends_arity_disambiguates_cleanly);
  RUN_TEST(test_overload_picks_method_callback_for_current_method_address);
  RUN_TEST(test_class_field_shadows_unit_name_in_member_call);
  RUN_TEST(test_method_value_typecast_base_uses_method_code_binding);
  RUN_TEST(test_method_value_cast_base_field_expression);
  RUN_TEST(test_record_field_named_like_type_keeps_pascal_type_lookup);
  RUN_TEST(test_member_base_local_record_shadows_same_named_type);
  RUN_TEST(test_member_base_local_class_shadows_same_named_type);
  RUN_TEST(test_forward_decl_does_not_create_duplicate_overload_candidate);
  RUN_TEST(test_overload_local_unit_shadows_uses_chain_overloads);
  RUN_TEST(test_overload_exact_match_dominates_widening_alternatives);
  RUN_TEST(test_overload_picks_narrowest_widening_target);
  RUN_TEST(test_overload_int_narrowing_to_shortint_param_is_viable);
  RUN_TEST(test_overload_resolves_through_with_block_bare_ident_call);
  RUN_TEST(test_overload_picks_string_concat_arg_against_string_param);
  RUN_TEST(test_overload_picks_empty_set_literal_against_typed_set_param);
  RUN_TEST(test_overload_default_arg_through_inherited_field_receiver);
  RUN_TEST(test_membership_in_empty_set_uses_shared_set_api);
  RUN_TEST(test_set_for_in_lowers_to_ordered_membership_scan);
  RUN_TEST(test_type_for_in_lowers_to_ordinal_bounds_loop);
  RUN_TEST(test_for_in_operator_enumerator_precedes_builtin_set);
  RUN_TEST(test_for_in_own_getenumerator_uses_movenext_and_current);
  RUN_TEST(test_for_in_open_array_uses_value_length_bounds);
  RUN_TEST(test_for_in_dynamic_array_property_uses_value_length_bounds);
  RUN_TEST(test_for_in_nested_getenumerator_return_type_resolves_in_owner);
  RUN_TEST(test_for_in_self_uses_current_class_getenumerator);
  RUN_TEST(test_for_in_set_literal_assigns_to_distinct_ordinal_loop_var);
  RUN_TEST(test_overload_picks_set_difference_arg_against_typed_set_param);
  RUN_TEST(test_property_read_lowers_to_getter_call);
  RUN_TEST(test_property_write_lowers_to_setter_call);
  RUN_TEST(test_implicit_write_only_property_assignment_lowers_to_setter_call);
  RUN_TEST(test_typecast_property_write_lowers_to_setter_call);
  RUN_TEST(test_property_read_through_field_lowers_to_field_access);
  RUN_TEST(test_property_dotted_field_accessor_lowers_to_field_path);
  RUN_TEST(test_typecast_property_read_field_ref_uses_backing_field);
  RUN_TEST(test_default_indexed_property_obj_brackets_calls_getter);
  RUN_TEST(test_property_read_returning_class_then_default_index_chains_through_getter);
  RUN_TEST(test_implicit_pointer_call_argument_gets_explicit_cast);
  RUN_TEST(test_pointer_assignment_from_pointer_result_gets_explicit_cast);
  RUN_TEST(test_pointer_builtin_cast_still_coerces_to_typed_pointer_slot);
  RUN_TEST(test_addr_of_untyped_param_keeps_pointer_slot_semantics);
  RUN_TEST(test_addr_of_member_assignment_to_typed_pointer_slot_gets_explicit_cast);
  RUN_TEST(test_pointer_function_slot_assignment_uses_funptr_helpers);
  RUN_TEST(test_inline_anon_enum_in_var_decl_members_resolve_in_unit_scope);
  RUN_TEST(test_private_enum_type_name_prefers_current_unit);
  RUN_TEST(test_inline_enum_labels_export_from_interface_field_types);
  RUN_TEST(test_inherited_call_routes_through_pascal_picker);
  RUN_TEST(test_inherited_exception_create_lowers_as_constructor_call);
  RUN_TEST(test_recursive_call_var_param_gets_param_info_for_reinterpret_ref);
  RUN_TEST(test_with_block_bare_free_lowers_through_static_helper);
  RUN_TEST(test_metaclass_member_base_emits_with_implicit_zero_arg_call);
  RUN_TEST(test_class_alias_in_value_position_lowers_to_underlying_metaclass);
  RUN_TEST(test_method_definition_on_class_alias_uses_canonical_owner);
  RUN_TEST(test_duplicate_class_names_across_units_keep_metaclass_owner_unit);
  RUN_TEST(test_metaclass_derived_constructor_surface_stays_visible);
  RUN_TEST(test_metaclass_named_constructor_with_args_lowers_to_descriptor_slot);
  RUN_TEST(test_metaclass_base_constructor_slot_survives_hidden_child_create);
  RUN_TEST(test_metaclass_same_signature_constructor_keeps_derived_return_type);
  RUN_TEST(test_inherited_metaclass_constructor_uses_declaring_unit_type_lookup);
  RUN_TEST(test_inheritsfrom_uses_runtime_tclass_and_method_call);
  RUN_TEST(test_inheritsfrom_is_boolean_for_short_circuit_and);
  RUN_TEST(test_indexed_property_result_classtype_autocalls);
  RUN_TEST(test_implicit_indexed_property_result_classtype_autocalls);
  RUN_TEST(test_indexed_implicit_property_in_method_body);
  RUN_TEST(test_indexed_implicit_property_result_write_in_method_body);
  RUN_TEST(test_function_result_member_access_uses_pointer_semantics);
  RUN_TEST(test_pointer_typed_field_chain_keeps_arrow_access);
  RUN_TEST(test_with_cast_binds_pointer_rvalue_by_value);
  RUN_TEST(test_statement_context_member_destroy_autocalls);
  RUN_TEST(test_val_var_arg_typecast_reinterprets_storage);
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
  RUN_TEST(test_addr_of_reference_class_typecast_field_uses_object_pointer);
  RUN_TEST(test_addr_of_object_pointer_field_returns_typed_pointer);
  RUN_TEST(test_reference_class_typecast_field_assignment_uses_assignment_operator);
  RUN_TEST(test_is_as_use_pointer_target_types);
  RUN_TEST(test_cxx_reserved_word_identifiers);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
