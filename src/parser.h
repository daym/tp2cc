#pragma once

#include <initializer_list>
#include <memory>
#include <optional>

#include "ast.h"
#include "lexer.h"
#include "token.h"

namespace tp2cc {

class Parser {
 public:
  explicit Parser(Lexer& lex);

  // Parse one compilation unit. Returns null on fatal errors (errors are
  // also reported via diag.h).
  std::shared_ptr<ast::UnitNode> parse();

 private:
  enum class ProcModifierFlag {
    Virtual,
    Abstract,
    Override,
    Final,
    Forward,
    Inline,
    Cdecl,
    Noreturn,
    Assembler,
    Overload,
  };

  // ---- token stream ----
  Lexer& lex_;
  Token cur_;
  Token peek_;  // 1-token lookahead
  bool have_peek_ = false;

  const Token& cur() const { return cur_; }
  const Token& peek();
  void advance();
  bool accept(Tok t);
  bool expect(Tok t, const char* ctx);
  bool check(Tok t) const { return cur_.kind == t; }
  bool check_any(std::initializer_list<Tok> tokens) const;
  bool at_end() const { return cur_.kind == Tok::Eof; }
  // Recover by skipping tokens until we see one of the stop tokens.
  void sync_to(std::initializer_list<Tok> stops);

  // ---- top level ----
  std::shared_ptr<ast::UnitNode> parse_program();
  std::shared_ptr<ast::UnitNode> parse_unit();
  std::vector<std::string> parse_uses_clause();
  // in_interface:  suppress parsing of proc bodies (interface signatures only)
  std::vector<ast::DeclPtr> parse_decl_block(bool in_interface);

  // ---- declarations ----
  std::vector<ast::DeclPtr> parse_const_section();
  std::vector<ast::DeclPtr> parse_type_section();
  std::shared_ptr<ast::TypeDecl> parse_type_decl_from_current_ident(
      const char* ctx);
  std::vector<ast::DeclPtr> parse_var_section();
  std::vector<ast::DeclPtr> parse_label_section();
  std::shared_ptr<ast::ProcDecl> parse_proc_decl(ast::ProcKind pk,
                                                 bool in_interface,
                                                 bool is_class_method,
                                                 bool in_type_member);
  std::shared_ptr<ast::ProcDecl> parse_operator_decl(bool in_interface);
  // Routine directives are parsed as one ProcModifiers value before the ProcDecl
  // is built, so method flags, calling convention, external name, and lifecycle
  // markers cross the parser/emitter boundary as one complete fact.
  std::optional<ast::ProcModifiers> parse_proc_modifier(bool in_type_member);
  ast::ProcModifiers parse_proc_modifiers(ast::ProcModifiers modifiers,
                                          bool in_type_member);
  ast::ProcModifiers parse_proc_header_tail(const char* ctx, bool in_type_member);
  static ast::ProcModifiers combine_proc_modifiers(ast::ProcModifiers base,
                                                   ast::ProcModifiers delta);
  static ast::ProcModifiers proc_modifier(ProcModifierFlag flag);
  static ast::ProcModifiers external_proc_modifier(std::string external_lib,
                                                   std::string external_name);

  // Pascal "directives" are position-dependent words spelled as identifiers
  // (e.g. `name`, `alias`, `read`, `write`, `stored`, `default`, `message`,
  // `index`, `result`, `operator`, `cvar`, `on`).
  // The lexer delivers them as Tok::Ident; we recognize them by text at
  // the points where they're meaningful.
  bool is_directive(const char* name) const;
  bool identifier_ends_nested_type_block() const;

  // ---- types ----
  ast::TypePtr parse_type();
  ast::TypePtr parse_simple_type();  // enum, subrange, name, string[N]
  ast::TypePtr parse_subrange_type(Location loc, ast::ExprPtr lo);
  ast::TypePtr parse_record_type(bool is_packed);
  std::shared_ptr<ast::VariantPart> parse_variant_part();
  ast::TypePtr parse_object_type();
  ast::TypePtr parse_interface_type();
  ast::TypePtr parse_array_type(bool is_packed);
  ast::TypePtr parse_set_type();
  ast::TypePtr parse_file_type();
  ast::TypePtr parse_procedural_type();
  std::vector<ast::Param> parse_param_list(Tok close);
  std::vector<ast::Param> parse_formal_param_list();

  // ---- statements ----
  ast::StmtPtr parse_statement();
  ast::StmtPtr parse_statement_block_until(std::initializer_list<Tok> stops);
  ast::StmtPtr parse_compound_statement();
  ast::StmtPtr parse_if();
  ast::StmtPtr parse_while();
  ast::StmtPtr parse_repeat();
  ast::StmtPtr parse_for();
  ast::StmtPtr parse_case();
  ast::StmtPtr parse_with();
  ast::StmtPtr parse_asm();
  ast::StmtPtr parse_try();
  ast::StmtPtr parse_raise();
  ast::StmtPtr parse_labeled_or_simple();

  // ---- expressions (precedence-climbing) ----
  ast::ExprPtr parse_expr();
  ast::ExprPtr parse_subrange_bound();  // expression without relational ops
  ast::ExprPtr parse_simple_expr();   // +/-/or/xor
  ast::ExprPtr parse_term();          // * / div mod and shl shr
  ast::ExprPtr parse_factor();        // unary, primary
  ast::ExprPtr parse_primary();
  ast::ExprPtr parse_postfix(ast::ExprPtr lhs);
  ast::ExprPtr parse_set_literal();

  // Helpers
  std::string consume_ident(const char* ctx);
  // Accepts Tok::Ident OR any reserved word that Pascal treats as a
  // directive-like word. Those arrive from the lexer as Tok::Ident too, so
  // this helper just documents the intent at those call sites.
  std::string consume_name_or_directive(const char* ctx);
  ast::PropertyDecl::Accessor parse_property_accessor_path(const char* ctx);
  bool tok_starts_type() const;

  // Typed-constant value (on the RHS of `ident : type = ...`). Handles
  // scalar expressions, array constants `(a,b,c)`, and record constants
  // `(f:v;f:v)` including nesting.
  ast::ExprPtr parse_const_value(const ast::TypeExpr* target = nullptr);
};

}  // namespace tp2cc
