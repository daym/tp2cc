// Unit tests for the lexer.
//
// Each test function drives the Lexer over a small Pascal snippet and asserts
// expected token kinds / text / numeric values. Keep snippets tight and
// independent.

#include <memory>
#include <string>
#include <vector>

#include "diag.h"
#include "lexer.h"
#include "source.h"
#include "test_util.h"
#include "token.h"

using namespace tp2cc;
using namespace tp2cc_test;

namespace {

std::unique_ptr<SourceFile> make_src(std::string text, std::string name = "<mem>") {
  auto sf = std::make_unique<SourceFile>();
  sf->path = std::move(name);
  sf->contents = std::move(text);
  return sf;
}

// Drain tokens into a vector; drop the trailing Eof.
std::vector<Token> lex_all(std::string text,
                           const std::vector<std::string>& defines = {}) {
  Lexer lx(make_src(std::move(text)));
  for (auto& d : defines) lx.define(d);
  std::vector<Token> out;
  for (;;) {
    Token t = lx.next();
    if (t.kind == Tok::Eof) break;
    out.push_back(std::move(t));
  }
  return out;
}

void test_empty() {
  auto ts = lex_all("");
  CHECK_EQ(ts.size(), size_t{0});
}

void test_whitespace_only() {
  auto ts = lex_all("   \t\n\r  ");
  CHECK_EQ(ts.size(), size_t{0});
}

void test_trivial_program() {
  auto ts = lex_all("begin end.");
  CHECK_EQ(ts.size(), size_t{3});
  CHECK(ts[0].kind == Tok::KwBegin);
  CHECK(ts[1].kind == Tok::KwEnd);
  CHECK(ts[2].kind == Tok::Dot);
}

void test_hello_program() {
  // Exercises: program/unit name, uses form not here; string lit; writeln id.
  auto ts = lex_all(
      "program hello;\n"
      "begin\n"
      "  writeln('hi');\n"
      "end.\n");
  // program hello ; begin writeln ( 'hi' ) ; end .
  CHECK_EQ(ts.size(), size_t{11});
  CHECK(ts[0].kind == Tok::KwProgram);
  CHECK(ts[1].kind == Tok::Ident);
  CHECK_EQ(ts[1].text, std::string("hello"));
  CHECK(ts[2].kind == Tok::Semi);
  CHECK(ts[3].kind == Tok::KwBegin);
  CHECK(ts[4].kind == Tok::Ident);
  CHECK_EQ(ts[4].text, std::string("writeln"));
  CHECK(ts[5].kind == Tok::LParen);
  CHECK(ts[6].kind == Tok::StringLit);
  CHECK_EQ(ts[6].text, std::string("hi"));
  CHECK(ts[7].kind == Tok::RParen);
  CHECK(ts[8].kind == Tok::Semi);
  CHECK(ts[9].kind == Tok::KwEnd);
  CHECK(ts[10].kind == Tok::Dot);
}

void test_case_insensitive_keywords() {
  auto ts = lex_all("BEGIN Begin bEgIn end");
  CHECK_EQ(ts.size(), size_t{4});
  CHECK(ts[0].kind == Tok::KwBegin);
  CHECK(ts[1].kind == Tok::KwBegin);
  CHECK(ts[2].kind == Tok::KwBegin);
  CHECK(ts[3].kind == Tok::KwEnd);
}

void test_identifiers_lowercased() {
  auto ts = lex_all("Foo BAR baz_Qux");
  CHECK_EQ(ts.size(), size_t{3});
  CHECK_EQ(ts[0].text, std::string("foo"));
  CHECK_EQ(ts[1].text, std::string("bar"));
  CHECK_EQ(ts[2].text, std::string("baz_qux"));
}

void test_operators_and_punctuation() {
  auto ts = lex_all(":= <> <= >= .. ^ @ @@ + - * / = < > ; , ( ) [ ]");
  std::vector<Tok> expected = {
      Tok::Assign, Tok::NotEq,  Tok::LtEq,   Tok::GtEq, Tok::DotDot,
      Tok::Caret,  Tok::At,     Tok::AtAt,   Tok::Plus, Tok::Minus,
      Tok::Star,   Tok::Slash,  Tok::Eq,     Tok::Lt,   Tok::Gt,
      Tok::Semi,   Tok::Comma,  Tok::LParen, Tok::RParen,
      Tok::LBrack, Tok::RBrack,
  };
  CHECK_EQ(ts.size(), expected.size());
  for (size_t i = 0; i < ts.size() && i < expected.size(); ++i) {
    CHECK(ts[i].kind == expected[i]);
  }
}

void test_numbers_decimal() {
  auto ts = lex_all("0 1 42 12345");
  CHECK_EQ(ts.size(), size_t{4});
  CHECK_EQ(ts[0].int_value, int64_t{0});
  CHECK_EQ(ts[1].int_value, int64_t{1});
  CHECK_EQ(ts[2].int_value, int64_t{42});
  CHECK_EQ(ts[3].int_value, int64_t{12345});
}

void test_numbers_bases() {
  auto ts = lex_all("$ff $1A %1010 &17");
  CHECK_EQ(ts.size(), size_t{4});
  CHECK_EQ(ts[0].int_value, int64_t{0xff});
  CHECK_EQ(ts[1].int_value, int64_t{0x1A});
  CHECK_EQ(ts[2].int_value, int64_t{10});
  CHECK_EQ(ts[3].int_value, int64_t{15});
}

void test_real_numbers() {
  auto ts = lex_all("3.14  1.0  2.5e3  1E-2");
  CHECK_EQ(ts.size(), size_t{4});
  for (auto& t : ts) CHECK(t.kind == Tok::RealLit);
  CHECK_EQ(ts[0].text, std::string("3.14"));
  CHECK_EQ(ts[2].text, std::string("2.5e3"));
  CHECK_EQ(ts[3].text, std::string("1E-2"));
}

void test_dotdot_vs_real() {
  // `1..10` must lex as IntLit DotDot IntLit, not a real.
  auto ts = lex_all("1..10");
  CHECK_EQ(ts.size(), size_t{3});
  CHECK(ts[0].kind == Tok::IntLit);
  CHECK_EQ(ts[0].int_value, int64_t{1});
  CHECK(ts[1].kind == Tok::DotDot);
  CHECK(ts[2].kind == Tok::IntLit);
  CHECK_EQ(ts[2].int_value, int64_t{10});
}

void test_strings_basic() {
  auto ts = lex_all("'hello' 'it''s'");
  CHECK_EQ(ts.size(), size_t{2});
  CHECK(ts[0].kind == Tok::StringLit);
  CHECK_EQ(ts[0].text, std::string("hello"));
  CHECK(ts[1].kind == Tok::StringLit);
  CHECK_EQ(ts[1].text, std::string("it's"));
}

void test_strings_char_codes() {
  auto ts = lex_all("#13#10 'x'#0'y' #$41");
  CHECK_EQ(ts.size(), size_t{3});
  CHECK_EQ(ts[0].text, std::string("\r\n"));
  CHECK_EQ(ts[1].text, std::string("x\0y", 3));
  CHECK_EQ(ts[2].text, std::string("A"));
}

void test_comments_all_styles() {
  auto ts = lex_all(
      "begin { brace comment } (* paren comment *) // line comment\n"
      "end");
  CHECK_EQ(ts.size(), size_t{2});
  CHECK(ts[0].kind == Tok::KwBegin);
  CHECK(ts[1].kind == Tok::KwEnd);
}

void test_brace_comments_not_nested() {
  // Turbo Pascal `{ ... }` does NOT nest -- the first `}` closes the comment.
  // So `{ a { b } c` produces: end-of-comment at the first `}`, then `c`
  // is a plain identifier.
  auto ts = lex_all("a { comment { still comment } c");
  CHECK_EQ(ts.size(), size_t{2});
  CHECK(ts[0].kind == Tok::Ident);
  CHECK_EQ(ts[0].text, std::string("a"));
  CHECK(ts[1].kind == Tok::Ident);
  CHECK_EQ(ts[1].text, std::string("c"));
}

void test_directive_ifdef_taken() {
  auto ts = lex_all(
      "{$ifdef FOO}\n"
      "inside\n"
      "{$endif}\n",
      {"foo"});  // case-insensitive
  CHECK_EQ(ts.size(), size_t{1});
  CHECK(ts[0].kind == Tok::Ident);
  CHECK_EQ(ts[0].text, std::string("inside"));
}

void test_directive_ifdef_not_taken() {
  auto ts = lex_all(
      "{$ifdef FOO}\n"
      "inside\n"
      "{$endif}\n",
      {/*no defines*/});
  CHECK_EQ(ts.size(), size_t{0});
}

void test_directive_ifndef() {
  auto ts = lex_all(
      "{$ifndef FOO}\n"
      "taken\n"
      "{$endif}\n",
      {});
  CHECK_EQ(ts.size(), size_t{1});
  CHECK_EQ(ts[0].text, std::string("taken"));
}

void test_directive_else() {
  auto ts1 = lex_all(
      "{$ifdef FOO}\n"
      "yes\n"
      "{$else}\n"
      "no\n"
      "{$endif}\n",
      {"foo"});
  CHECK_EQ(ts1.size(), size_t{1});
  CHECK_EQ(ts1[0].text, std::string("yes"));

  auto ts2 = lex_all(
      "{$ifdef FOO}\n"
      "yes\n"
      "{$else}\n"
      "no\n"
      "{$endif}\n",
      {});
  CHECK_EQ(ts2.size(), size_t{1});
  CHECK_EQ(ts2[0].text, std::string("no"));
}

void test_directive_nested_ifdef() {
  auto ts = lex_all(
      "{$ifdef A}\n"
      "  a_before\n"
      "  {$ifdef B}b{$else}notb{$endif}\n"
      "  a_after\n"
      "{$endif}\n",
      {"a"});
  // "a_before", "notb", "a_after"
  CHECK_EQ(ts.size(), size_t{3});
  CHECK_EQ(ts[0].text, std::string("a_before"));
  CHECK_EQ(ts[1].text, std::string("notb"));
  CHECK_EQ(ts[2].text, std::string("a_after"));
}

void test_directive_define_undef() {
  auto ts = lex_all(
      "{$define X}\n"
      "{$ifdef X} one {$endif}\n"
      "{$undef X}\n"
      "{$ifdef X} two {$endif}\n"
      "{$ifndef X} three {$endif}\n");
  CHECK_EQ(ts.size(), size_t{2});
  CHECK_EQ(ts[0].text, std::string("one"));
  CHECK_EQ(ts[1].text, std::string("three"));
}

void test_directive_builtin_macro_expands_deterministically() {
  int errs_before = tp2cc::error_count();
  auto ts = lex_all("const d = {$I %DATE%};");
  int errs = tp2cc::error_count() - errs_before;
  CHECK_EQ(errs, 0);
  CHECK_EQ(ts.size(), size_t{5});
  CHECK(ts[0].kind == Tok::KwConst);
  CHECK(ts[1].kind == Tok::Ident);
  CHECK(ts[2].kind == Tok::Eq);
  CHECK(ts[3].kind == Tok::StringLit);
  CHECK_EQ(ts[3].text, std::string("1970-01-01"));
  CHECK(ts[4].kind == Tok::Semi);
}

void test_directive_ignored_configs() {
  // These directives must be silently accepted (not emit tokens / errors).
  auto ts = lex_all(
      "{$I+} {$R-} {$H-} {$S-} {$mode fpc} {$ASMMODE intel}\n"
      "x\n");
  CHECK_EQ(ts.size(), size_t{1});
  CHECK_EQ(ts[0].text, std::string("x"));
}

void test_enum_and_set_of_type() {
  auto ts = lex_all(
      "type\n"
      "  TColor = (red, green, blue);\n"
      "  TColors = set of TColor;\n");
  std::vector<Tok> expected = {
      Tok::KwType,
      Tok::Ident,    // tcolor
      Tok::Eq,
      Tok::LParen,
      Tok::Ident,    // red
      Tok::Comma,
      Tok::Ident,    // green
      Tok::Comma,
      Tok::Ident,    // blue
      Tok::RParen,
      Tok::Semi,
      Tok::Ident,    // tcolors
      Tok::Eq,
      Tok::KwSet,
      Tok::KwOf,
      Tok::Ident,    // tcolor
      Tok::Semi,
  };
  CHECK_EQ(ts.size(), expected.size());
  for (size_t i = 0; i < ts.size() && i < expected.size(); ++i) {
    CHECK(ts[i].kind == expected[i]);
  }
  // Spot-check a couple of ident texts.
  if (ts.size() == expected.size()) {
    CHECK_EQ(ts[1].text, std::string("tcolor"));
    CHECK_EQ(ts[4].text, std::string("red"));
    CHECK_EQ(ts[11].text, std::string("tcolors"));
    CHECK_EQ(ts[15].text, std::string("tcolor"));
  }
}

void test_set_literal_and_in() {
  auto ts = lex_all("if c in [red, green] then x := 1;");
  // if c in [ red , green ] then x := 1 ;
  CHECK_EQ(ts.size(), size_t{13});
  CHECK(ts[0].kind == Tok::KwIf);
  CHECK(ts[2].kind == Tok::KwIn);
  CHECK(ts[3].kind == Tok::LBrack);
  CHECK(ts[7].kind == Tok::RBrack);
  CHECK(ts[8].kind == Tok::KwThen);
  CHECK(ts[10].kind == Tok::Assign);
  CHECK(ts[11].kind == Tok::IntLit);
  CHECK_EQ(ts[11].int_value, int64_t{1});
}

void test_object_declaration_syntax() {
  // Style used across cobjects.pas
  auto ts = lex_all(
      "type\n"
      "  tlist = object\n"
      "    first : plistitem;\n"
      "    constructor init;\n"
      "    destructor done; virtual;\n"
      "  end;\n");
  // Spot-check key keywords.
  bool saw_object = false, saw_constructor = false, saw_destructor = false,
       saw_virtual = false;
  for (auto& t : ts) {
    if (t.kind == Tok::KwObject) saw_object = true;
    if (t.kind == Tok::KwConstructor) saw_constructor = true;
    if (t.kind == Tok::KwDestructor) saw_destructor = true;
    if (t.kind == Tok::KwVirtual) saw_virtual = true;
  }
  CHECK(saw_object);
  CHECK(saw_constructor);
  CHECK(saw_destructor);
  CHECK(saw_virtual);
}

void test_location_tracking() {
  auto ts = lex_all(
      "a\n"       // line 1
      "  b\n"     // line 2, col 3
      "c");       // line 3
  CHECK_EQ(ts.size(), size_t{3});
  CHECK_EQ(ts[0].loc.line, 1u);
  CHECK_EQ(ts[0].loc.col, 1u);
  CHECK_EQ(ts[1].loc.line, 2u);
  CHECK_EQ(ts[1].loc.col, 3u);
  CHECK_EQ(ts[2].loc.line, 3u);
  CHECK_EQ(ts[2].loc.col, 1u);
}

}  // namespace

int main() {
  RUN_TEST(test_empty);
  RUN_TEST(test_whitespace_only);
  RUN_TEST(test_trivial_program);
  RUN_TEST(test_hello_program);
  RUN_TEST(test_case_insensitive_keywords);
  RUN_TEST(test_identifiers_lowercased);
  RUN_TEST(test_operators_and_punctuation);
  RUN_TEST(test_numbers_decimal);
  RUN_TEST(test_numbers_bases);
  RUN_TEST(test_real_numbers);
  RUN_TEST(test_dotdot_vs_real);
  RUN_TEST(test_strings_basic);
  RUN_TEST(test_strings_char_codes);
  RUN_TEST(test_comments_all_styles);
  RUN_TEST(test_brace_comments_not_nested);
  RUN_TEST(test_directive_ifdef_taken);
  RUN_TEST(test_directive_ifdef_not_taken);
  RUN_TEST(test_directive_ifndef);
  RUN_TEST(test_directive_else);
  RUN_TEST(test_directive_nested_ifdef);
  RUN_TEST(test_directive_define_undef);
  RUN_TEST(test_directive_builtin_macro_expands_deterministically);
  RUN_TEST(test_directive_ignored_configs);
  RUN_TEST(test_enum_and_set_of_type);
  RUN_TEST(test_set_literal_and_in);
  RUN_TEST(test_object_declaration_syntax);
  RUN_TEST(test_location_tracking);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
