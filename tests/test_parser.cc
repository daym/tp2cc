// Unit tests for the parser.
//
// Each test builds a Lexer over a snippet, constructs a Parser, and checks
// the produced AST. We deliberately test a wide variety of Pascal constructs
// so parse regressions show up immediately.

#include <memory>
#include <string>
#include <utility>

#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "test_util.h"
#include "token.h"

using namespace tp2cc;
using namespace tp2cc::ast;
using namespace tp2cc_test;

namespace {

std::unique_ptr<UnitNode> parse_snippet(std::string text) {
  auto sf = std::make_unique<SourceFile>();
  sf->path = "<mem>";
  sf->contents = std::move(text);
  Lexer lx(std::move(sf));
  Parser p(lx);
  return p.parse();
}


// A trivial program: program foo; begin end.
void test_trivial_program() {
  int before = error_count();
  auto u = parse_snippet("program foo; begin end.");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) {
    CHECK(u->is_program);
    CHECK_EQ(u->name, std::string("foo"));
    CHECK(u->init_body != nullptr);
  }
}

// Bare program body (no header) -- some .inc files look like this.
void test_bare_body() {
  int before = error_count();
  auto u = parse_snippet("begin end.");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) CHECK(u->is_program);
}

// Minimal unit.
void test_empty_unit() {
  int before = error_count();
  auto u = parse_snippet(
      "unit foo;\n"
      "interface\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) {
    CHECK(!u->is_program);
    CHECK_EQ(u->name, std::string("foo"));
  }
}

// Unit with uses + init body.
void test_unit_uses_and_init() {
  int before = error_count();
  auto u = parse_snippet(
      "unit foo;\n"
      "interface\n"
      "uses bar, baz;\n"
      "implementation\n"
      "uses qux;\n"
      "begin\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) {
    CHECK_EQ(u->interface_uses.size(), size_t{2});
    CHECK_EQ(u->interface_uses[0], std::string("bar"));
    CHECK_EQ(u->interface_uses[1], std::string("baz"));
    CHECK_EQ(u->impl_uses.size(), size_t{1});
    CHECK_EQ(u->impl_uses[0], std::string("qux"));
    CHECK(u->init_body != nullptr);
  }
}

// Constants.
void test_const_decls() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  A = 5;\n"
      "  B : integer = 42;\n"
      "  S = 'hello';\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u) CHECK_EQ(u->interface_decls.size(), size_t{3});
}

void test_type_decls_named_and_enum() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TColor = (red, green, blue);\n"
      "  TColors = set of TColor;\n"
      "  TByte = 0..255;\n"
      "  TStr = string[20];\n"
      "  PInt = ^integer;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u) CHECK_EQ(u->interface_decls.size(), size_t{5});
}

void test_record_type() {
  // Uses `name` as a record field; verifies the directive-vs-keyword fix.
  int b = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TPoint = record\n"
      "    x, y : integer;\n"
      "    name : string;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - b, 0);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td != nullptr);
    if (td) {
      auto* tr = dynamic_cast<TyRecord*>(td->type.get());
      CHECK(tr != nullptr);
      if (tr) CHECK_EQ(tr->fields.size(), size_t{2});
    }
  }
}

void test_variant_record() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TVal = record\n"
      "    tag : integer;\n"
      "    case k : integer of\n"
      "      1 : (i : integer);\n"
      "      2 : (r : real);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    if (td) {
      auto* tr = dynamic_cast<TyRecord*>(td->type.get());
      CHECK(tr);
      if (tr) {
        CHECK(tr->has_variant);
        CHECK_EQ(tr->variant_tag_name, std::string("k"));
        CHECK_EQ(tr->variant_cases.size(), size_t{2});
      }
    }
  }
}

void test_object_type() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  plist = ^tlist;\n"
      "  tlist = object\n"
      "    first : plist;\n"
      "    constructor init;\n"
      "    destructor done; virtual;\n"
      "    procedure add(item : integer); virtual;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u) CHECK_EQ(u->interface_decls.size(), size_t{2});
}

void test_object_inheritance() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = object\n"
      "    procedure m;\n"
      "  end;\n"
      "  tderived = object(tbase)\n"
      "    procedure m; virtual;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && u->interface_decls.size() >= 2) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    CHECK(td);
    if (td) {
      auto* to = dynamic_cast<TyObject*>(td->type.get());
      CHECK(to);
      if (to) CHECK_EQ(to->parent, std::string("tbase"));
    }
  }
}

void test_var_decls() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  a, b : integer;\n"
      "  c : string;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->interface_decls.empty()) {
    auto* vd = dynamic_cast<VarDecl*>(u->interface_decls[0].get());
    CHECK(vd);
    if (vd) CHECK_EQ(vd->names.size(), size_t{2});
  }
}

void test_var_absolute() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "var\n"
      "  x : integer;\n"
      "  y : integer absolute x;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && u->interface_decls.size() >= 2) {
    auto* vd = dynamic_cast<VarDecl*>(u->interface_decls[1].get());
    CHECK(vd);
    if (vd) {
      CHECK(vd->is_absolute);
      CHECK_EQ(vd->absolute_target, std::string("x"));
    }
  }
}

void test_proc_decl_interface() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "procedure foo(x : integer);\n"
      "function  bar(a, b : integer) : integer;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u) CHECK_EQ(u->interface_decls.size(), size_t{2});
}

void test_proc_impl_with_body() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "procedure foo;\n"
      "implementation\n"
      "procedure foo;\n"
      "var x : integer;\n"
      "begin\n"
      "  x := 1;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->impl_decls.empty()) {
    auto* pd = dynamic_cast<ProcDecl*>(u->impl_decls[0].get());
    CHECK(pd);
    if (pd) {
      CHECK(pd->body != nullptr);
      CHECK_EQ(pd->locals.size(), size_t{1});
    }
  }
}

void test_proc_method_qualified() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type tfoo = object procedure bar; end;\n"
      "implementation\n"
      "procedure tfoo.bar;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->impl_decls.empty()) {
    auto* pd = dynamic_cast<ProcDecl*>(u->impl_decls[0].get());
    CHECK(pd);
    if (pd) {
      CHECK_EQ(pd->of_type, std::string("tfoo"));
      CHECK_EQ(pd->name, std::string("bar"));
    }
  }
}

void test_proc_modifiers_forward_external() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "procedure a; forward;\n"
      "procedure b; cdecl; external 'libc' name 'b';\n"
      "implementation\n"
      "procedure a; begin end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && u->interface_decls.size() >= 2) {
    auto* pa = dynamic_cast<ProcDecl*>(u->interface_decls[0].get());
    auto* pb = dynamic_cast<ProcDecl*>(u->interface_decls[1].get());
    CHECK(pa && pa->is_forward);
    CHECK(pb && pb->is_external && pb->is_cdecl);
    if (pb) {
      CHECK_EQ(pb->external_lib, std::string("libc"));
      CHECK_EQ(pb->external_name, std::string("b"));
    }
  }
}

void test_nested_proc() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "procedure outer;\n"
      "implementation\n"
      "procedure outer;\n"
      "  procedure inner;\n"
      "  begin\n"
      "  end;\n"
      "begin\n"
      "  inner;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->impl_decls.empty()) {
    auto* pd = dynamic_cast<ProcDecl*>(u->impl_decls[0].get());
    CHECK(pd);
    if (pd) CHECK_EQ(pd->locals.size(), size_t{1});
  }
}

void test_statements_if_while() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var x : integer;\n"
      "begin\n"
      "  x := 0;\n"
      "  while x < 10 do\n"
      "    if x > 5 then x := x + 2 else x := x + 1;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_statement_repeat_for_case_with() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var i : integer;\n"
      "begin\n"
      "  repeat i := i + 1 until i > 5;\n"
      "  for i := 1 to 10 do i := i + 1;\n"
      "  for i := 10 downto 1 do i := i - 1;\n"
      "  case i of\n"
      "    1 : i := 0;\n"
      "    2, 3 : i := 1;\n"
      "    4..6 : i := 2;\n"
      "  else\n"
      "    i := 99;\n"
      "  end;\n"
      "  with i do begin end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_goto_and_label() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "label l1;\n"
      "begin\n"
      "  goto l1;\n"
      "  l1: ;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_expr_precedence() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var x : integer;\n"
      "begin\n"
      "  x := 1 + 2 * 3 - 4 div 5 mod 6 and 7 or 8 xor 9;\n"
      "  x := not (x = 0) and (x < 10);\n"
      "  x := -x + @x^.y[1,2].z;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
}

void test_set_literal_in_expr() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "type tcolor = (r,g,b);\n"
      "var c : tcolor;\n"
      "begin\n"
      "  if c in [r, g] then c := b;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_call_with_write_formatter() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var x : integer;\n"
      "begin\n"
      "  writeln(x:4, x:6:2, 'hello');\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
}

void test_procedural_type_decl() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcb = procedure(x : integer);\n"
      "  tfn = function(a : integer) : integer;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
}

// Pascal directives are position-dependent keywords; they must work as
// ordinary identifiers in other positions.
void test_directives_as_identifiers() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  trec = record\n"
      "    name : string;\n"
      "    index : integer;\n"
      "    read : longint;\n"
      "    write : longint;\n"
      "  end;\n"
      "var result : integer;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_typed_array_constant() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type tenum = (a, b, c);\n"
      "const\n"
      "  names : array[tenum] of string[8] = ('aa', 'bb', 'cc');\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* cd = dynamic_cast<ConstDecl*>(u->interface_decls[1].get());
    CHECK(cd);
    if (cd) {
      auto* ac = dynamic_cast<ArrayConst*>(cd->value.get());
      CHECK(ac);
      if (ac) CHECK_EQ(ac->elements.size(), size_t{3});
    }
  }
}

void test_typed_record_constant() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tpt = record x, y : integer; end;\n"
      "const\n"
      "  origin : tpt = (x: 0; y: 0);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && u->interface_decls.size() >= 2) {
    auto* cd = dynamic_cast<ConstDecl*>(u->interface_decls[1].get());
    CHECK(cd);
    if (cd) {
      auto* rc = dynamic_cast<RecordConst*>(cd->value.get());
      CHECK(rc);
      if (rc) CHECK_EQ(rc->fields.size(), size_t{2});
    }
  }
}

void test_inherited_method_call() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tfoo = object\n"
      "    constructor init;\n"
      "  end;\n"
      "implementation\n"
      "constructor tfoo.init;\n"
      "begin\n"
      "  inherited init;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_directive_as_method_name() {
  // `function Coll.At(...)` uses the directive `at` as a method name.
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcoll = object\n"
      "    function at(i : integer) : longint;\n"
      "  end;\n"
      "implementation\n"
      "function tcoll.at(i : integer) : longint;\n"
      "begin\n"
      "  at := i;\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
}

void test_unit_named_like_directive() {
  int before = error_count();
  auto u = parse_snippet(
      "unit export;\n"
      "interface\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u && u->name == "export");
}

void test_error_recovery_basic() {
  int before = error_count();
  // Missing semicolon after unit name.
  auto u = parse_snippet("unit foo interface implementation end.");
  CHECK(error_count() - before >= 1);
  // We still return a node.
  CHECK(u != nullptr);
}

}  // namespace

int main() {
  RUN_TEST(test_trivial_program);
  RUN_TEST(test_bare_body);
  RUN_TEST(test_empty_unit);
  RUN_TEST(test_unit_uses_and_init);
  RUN_TEST(test_const_decls);
  RUN_TEST(test_type_decls_named_and_enum);
  RUN_TEST(test_record_type);
  RUN_TEST(test_variant_record);
  RUN_TEST(test_object_type);
  RUN_TEST(test_object_inheritance);
  RUN_TEST(test_var_decls);
  RUN_TEST(test_var_absolute);
  RUN_TEST(test_proc_decl_interface);
  RUN_TEST(test_proc_impl_with_body);
  RUN_TEST(test_proc_method_qualified);
  RUN_TEST(test_proc_modifiers_forward_external);
  RUN_TEST(test_nested_proc);
  RUN_TEST(test_statements_if_while);
  RUN_TEST(test_statement_repeat_for_case_with);
  RUN_TEST(test_goto_and_label);
  RUN_TEST(test_expr_precedence);
  RUN_TEST(test_set_literal_in_expr);
  RUN_TEST(test_call_with_write_formatter);
  RUN_TEST(test_procedural_type_decl);
  RUN_TEST(test_directives_as_identifiers);
  RUN_TEST(test_typed_array_constant);
  RUN_TEST(test_typed_record_constant);
  RUN_TEST(test_inherited_method_call);
  RUN_TEST(test_directive_as_method_name);
  RUN_TEST(test_unit_named_like_directive);
  RUN_TEST(test_error_recovery_basic);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
