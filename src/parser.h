#pragma once

#include <memory>

#include "ast.h"
#include "lexer.h"
#include "token.h"

namespace tp2cc {

class Parser {
 public:
  explicit Parser(Lexer& lex);

  // Parse one compilation unit. Returns null on fatal errors (errors are
  // also reported via diag.h).
  std::unique_ptr<ast::UnitNode> parse();

 private:
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
  bool at_end() const { return cur_.kind == Tok::Eof; }
  // Recover by skipping tokens until we see one of the stop tokens.
  void sync_to(std::initializer_list<Tok> stops);

  // ---- top level ----
  std::unique_ptr<ast::UnitNode> parse_program();
  std::unique_ptr<ast::UnitNode> parse_unit();
  void parse_uses_into(std::vector<std::string>& out);
  // in_interface:  suppress parsing of proc bodies (interface signatures only)
  void parse_decl_block(std::vector<ast::DeclPtr>& out, bool in_interface);

  // ---- declarations ----
  void parse_const_section(std::vector<ast::DeclPtr>& out);
  void parse_type_section(std::vector<ast::DeclPtr>& out);
  void parse_var_section(std::vector<ast::DeclPtr>& out);
  void parse_label_section(std::vector<ast::DeclPtr>& out);
  ast::DeclPtr parse_proc_decl(ast::ProcKind pk, bool in_interface);
  void parse_proc_modifiers(ast::ProcDecl& pd);

  // Pascal "directives" are position-dependent keywords spelled as ordinary
  // identifiers (e.g. `name`, `alias`, `read`, `write`, `stored`,
  // `default`, `message`, `index`, `result`, `operator`, `cvar`, `on`).
  // The lexer delivers them as Tok::Ident; we recognize them by text at
  // the points where they're meaningful.
  bool is_directive(const char* name) const;

  // ---- types ----
  ast::TypePtr parse_type();
  ast::TypePtr parse_simple_type();  // enum, subrange, name, string[N]
  ast::TypePtr parse_record_type(bool is_packed);
  ast::TypePtr parse_object_type();
  ast::TypePtr parse_array_type(bool is_packed);
  ast::TypePtr parse_set_type();
  ast::TypePtr parse_file_type();
  ast::TypePtr parse_procedural_type();
  std::vector<ast::Param> parse_formal_param_list();

  // ---- statements ----
  ast::StmtPtr parse_statement();
  ast::StmtPtr parse_compound_statement();
  ast::StmtPtr parse_if();
  ast::StmtPtr parse_while();
  ast::StmtPtr parse_repeat();
  ast::StmtPtr parse_for();
  ast::StmtPtr parse_case();
  ast::StmtPtr parse_with();
  ast::StmtPtr parse_asm();
  ast::StmtPtr parse_labeled_or_simple();

  // ---- expressions (precedence-climbing) ----
  ast::ExprPtr parse_expr();
  ast::ExprPtr parse_simple_expr();   // +/-/or/xor
  ast::ExprPtr parse_term();          // * / div mod and shl shr
  ast::ExprPtr parse_factor();        // unary, primary
  ast::ExprPtr parse_primary();
  ast::ExprPtr parse_postfix(ast::ExprPtr lhs);
  ast::ExprPtr parse_set_literal();

  // Helpers
  std::string consume_ident(const char* ctx);
  // Accepts Tok::Ident OR any reserved word that Pascal treats as a
  // directive (virtual, register, near, far, name, ...). Used where an
  // identifier is required but a directive-word happens to be used (e.g.
  // `p^.register` as a field name).
  std::string consume_name_or_directive(const char* ctx);
  bool tok_is_directive_kw() const;
  bool tok_starts_type() const;

  // Typed-constant value (on the RHS of `ident : type = ...`). Handles
  // scalar expressions, array constants `(a,b,c)`, and record constants
  // `(f:v;f:v)` including nesting.
  ast::ExprPtr parse_const_value();
};

}  // namespace tp2cc
