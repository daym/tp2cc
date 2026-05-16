#include "parser.h"

#include <initializer_list>
#include <iterator>
#include <optional>
#include <utility>

#include "diag.h"

namespace tp2cc {

using namespace ast;

namespace {

bool is_subrange_bound_intrinsic(const std::string& name) {
  return name == "low" || name == "high" || name == "pred" ||
         name == "succ" || name == "sizeof" || name == "ord";
}

bool is_constant_subrange_bound_expr(const Expr& e) {
  switch (e.kind) {
    case Kind::Ident:
    case Kind::IntLit:
    case Kind::StringLit:
    case Kind::BoolLit:
    case Kind::Member:
      return true;
    case Kind::Unary:
      return is_constant_subrange_bound_expr(
          *static_cast<const Unary&>(e).operand);
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      return is_constant_subrange_bound_expr(*b.lhs) &&
             is_constant_subrange_bound_expr(*b.rhs);
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      if (c.callee->kind != Kind::Ident || c.args.size() != 1) return false;
      const auto& callee = static_cast<const Ident&>(*c.callee);
      return is_subrange_bound_intrinsic(callee.name) &&
             is_constant_subrange_bound_expr(*c.args[0]);
    }
    default:
      return false;
  }
}

TypePtr make_type_name(Location loc, std::string name) {
  return std::make_shared<TyName>(loc, std::move(name));
}

std::optional<BinOp> relational_operator(Tok tok) {
  switch (tok) {
    case Tok::Eq: return BinOp::Eq;
    case Tok::NotEq: return BinOp::NotEq;
    case Tok::Lt: return BinOp::Lt;
    case Tok::Gt: return BinOp::Gt;
    case Tok::LtEq: return BinOp::LtEq;
    case Tok::GtEq: return BinOp::GtEq;
    case Tok::KwIn: return BinOp::In;
    case Tok::KwIs: return BinOp::Is;
    case Tok::KwAs: return BinOp::As;
    default: return std::nullopt;
  }
}

std::optional<BinOp> additive_operator(Tok tok) {
  switch (tok) {
    case Tok::Plus: return BinOp::Add;
    case Tok::Minus: return BinOp::Sub;
    case Tok::KwOr: return BinOp::Or;
    case Tok::KwXor: return BinOp::Xor;
    case Tok::SymDiff: return BinOp::SymDiff;
    default: return std::nullopt;
  }
}

std::optional<BinOp> multiplicative_operator(Tok tok) {
  switch (tok) {
    case Tok::Star: return BinOp::Mul;
    case Tok::Slash: return BinOp::RealDiv;
    case Tok::KwDiv: return BinOp::IntDiv;
    case Tok::KwMod: return BinOp::Mod;
    case Tok::KwAnd: return BinOp::And;
    case Tok::KwShl: return BinOp::Shl;
    case Tok::KwShr: return BinOp::Shr;
    default: return std::nullopt;
  }
}

std::string operator_decl_token_text(const Token& token) {
  switch (token.kind) {
    case Tok::Assign: return ":=";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Eq: return "=";
    case Tok::Lt: return "<";
    case Tok::Gt: return ">";
    case Tok::LtEq: return "<=";
    case Tok::GtEq: return ">=";
    case Tok::NotEq: return "<>";
    case Tok::KwDiv: return "div";
    case Tok::KwMod: return "mod";
    case Tok::KwAnd: return "and";
    case Tok::KwOr: return "or";
    case Tok::KwXor: return "xor";
    case Tok::KwShl: return "shl";
    case Tok::KwShr: return "shr";
    case Tok::Ident:
      if (token.text == "enumerator") return "enumerator";
      return {};
    default: return {};
  }
}

}  // namespace

ast::ProcModifiers Parser::combine_proc_modifiers(ProcModifiers base,
                                                     ProcModifiers delta) {
  return ProcModifiers{
      base.is_virtual || delta.is_virtual,
      base.is_abstract || delta.is_abstract,
      base.is_override || delta.is_override,
      base.is_final || delta.is_final,
      base.is_forward || delta.is_forward,
      base.is_inline || delta.is_inline,
      base.is_cdecl || delta.is_cdecl,
      base.is_noreturn || delta.is_noreturn,
      base.is_external || delta.is_external,
      base.is_assembler || delta.is_assembler,
      delta.external_lib.empty() ? std::move(base.external_lib)
                                 : std::move(delta.external_lib),
      delta.external_name.empty() ? std::move(base.external_name)
                                  : std::move(delta.external_name)};
}

ast::ProcModifiers Parser::proc_modifier(ProcModifierFlag flag) {
  switch (flag) {
    case ProcModifierFlag::Virtual:
      return ProcModifiers{true, false, false, false, false, false,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Abstract:
      return ProcModifiers{false, true, false, false, false, false,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Override:
      return ProcModifiers{false, false, true, false, false, false,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Final:
      return ProcModifiers{false, false, false, true, false, false,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Forward:
      return ProcModifiers{false, false, false, false, true, false,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Inline:
      return ProcModifiers{false, false, false, false, false, true,
                           false, false, false, false, "", ""};
    case ProcModifierFlag::Cdecl:
      return ProcModifiers{false, false, false, false, false, false,
                           true, false, false, false, "", ""};
    case ProcModifierFlag::Noreturn:
      return ProcModifiers{false, false, false, false, false, false,
                           false, true, false, false, "", ""};
    case ProcModifierFlag::Assembler:
      return ProcModifiers{false, false, false, false, false, false,
                           false, false, false, true, "", ""};
  }
  return ProcModifiers{};
}

ast::ProcModifiers Parser::external_proc_modifier(
    std::string external_lib, std::string external_name) {
  return ProcModifiers{false,
                       false,
                       false,
                       false,
                       false,
                       false,
                       false,
                       false,
                       true,
                       false,
                       std::move(external_lib),
                       std::move(external_name)};
}

// ---------------------------------------------------------------------------
// Token stream plumbing

Parser::Parser(Lexer& lex) : lex_(lex) {
  cur_ = lex_.next();
}

const Token& Parser::peek() {
  if (!have_peek_) {
    peek_ = lex_.next();
    have_peek_ = true;
  }
  return peek_;
}

void Parser::advance() {
  if (have_peek_) {
    cur_ = std::move(peek_);
    have_peek_ = false;
  } else {
    cur_ = lex_.next();
  }
}

bool Parser::accept(Tok t) {
  if (cur_.kind == t) { advance(); return true; }
  return false;
}

bool Parser::expect(Tok t, const char* ctx) {
  if (cur_.kind == t) { advance(); return true; }
  std::string msg = std::string("expected ");
  switch (t) {
    case Tok::Semi: msg += "';'"; break;
    case Tok::Dot: msg += "'.'"; break;
    case Tok::Colon: msg += "':'"; break;
    case Tok::Comma: msg += "','"; break;
    case Tok::Eq: msg += "'='"; break;
    case Tok::Assign: msg += "':='"; break;
    case Tok::LParen: msg += "'('"; break;
    case Tok::RParen: msg += "')'"; break;
    case Tok::LBrack: msg += "'['"; break;
    case Tok::RBrack: msg += "']'"; break;
    case Tok::KwBegin: msg += "'begin'"; break;
    case Tok::KwEnd: msg += "'end'"; break;
    case Tok::KwDo: msg += "'do'"; break;
    case Tok::KwThen: msg += "'then'"; break;
    case Tok::KwOf: msg += "'of'"; break;
    case Tok::KwTo: msg += "'to'"; break;
    case Tok::KwUntil: msg += "'until'"; break;
    case Tok::Ident: msg += "identifier"; break;
    default: msg += "token"; break;
  }
  if (ctx) { msg += " (in "; msg += ctx; msg += ")"; }
  if (!cur_.text.empty()) {
    msg += ", got '"; msg += cur_.text; msg += "'";
  }
  report_error(cur_.loc, msg);
  return false;
}

void Parser::sync_to(std::initializer_list<Tok> stops) {
  while (!at_end()) {
    for (Tok t : stops) if (cur_.kind == t) return;
    advance();
  }
}

std::string Parser::consume_ident(const char* ctx) {
  if (cur_.kind != Tok::Ident) {
    expect(Tok::Ident, ctx);
    return {};
  }
  std::string s = cur_.text;
  advance();
  return s;
}

bool Parser::is_directive(const char* name) const {
  return cur_.kind == Tok::Ident && cur_.text == name;
}

std::string Parser::consume_name_or_directive(const char* ctx) {
  if (cur_.kind == Tok::Ident) {
    std::string s = cur_.text;
    advance();
    return s;
  }
  expect(Tok::Ident, ctx);
  return {};
}

PropertyDecl::Accessor Parser::parse_property_accessor_path(const char* ctx) {
  std::vector<std::string> path;
  path.push_back(consume_name_or_directive(ctx));
  while (accept(Tok::Dot)) {
    path.push_back(consume_name_or_directive(ctx));
  }
  return PropertyDecl::Accessor(std::move(path));
}

// ---------------------------------------------------------------------------
// Entry

std::shared_ptr<UnitNode> Parser::parse() {
  if (cur_.kind == Tok::KwUnit) return parse_unit();
  if (cur_.kind == Tok::KwProgram) return parse_program();
  // Some sources are `.inc` files or program-body-only; treat as program
  // without a header.
  if (cur_.kind == Tok::KwBegin) {
    Location loc = cur_.loc;
    auto init_body = parse_compound_statement();
    if (check(Tok::Dot)) advance();
    return std::make_shared<UnitNode>(
        loc, "", std::vector<std::string>{}, std::vector<DeclPtr>{},
        std::vector<std::string>{}, std::vector<DeclPtr>{},
        std::move(init_body), nullptr, true);
  }
  report_error(cur_.loc,
               "expected 'program' or 'unit' at top of compilation unit");
  return nullptr;
}

std::shared_ptr<UnitNode> Parser::parse_program() {
  Location loc = cur_.loc;
  expect(Tok::KwProgram, "program header");
  std::string name = consume_name_or_directive("program name");
  // Optional program parameter list: program foo(input, output);
  if (accept(Tok::LParen)) {
    while (!at_end() && !check(Tok::RParen)) {
      consume_ident("program parameter");
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "program parameters");
  }
  expect(Tok::Semi, "program header");
  // Optional uses.
  std::vector<std::string> impl_uses;
  if (check(Tok::KwUses)) {
    advance();
    impl_uses = parse_uses_clause();
    expect(Tok::Semi, "uses clause");
  }
  std::vector<DeclPtr> impl_decls =
      parse_decl_block(/*in_interface=*/false);
  // Main body.
  StmtPtr init_body;
  if (check(Tok::KwBegin)) {
    init_body = parse_compound_statement();
  }
  expect(Tok::Dot, "program tail");
  return std::make_shared<UnitNode>(
      loc, std::move(name), std::vector<std::string>{}, std::vector<DeclPtr>{},
      std::move(impl_uses), std::move(impl_decls), std::move(init_body),
      nullptr, true);
}

std::shared_ptr<UnitNode> Parser::parse_unit() {
  Location loc = cur_.loc;
  expect(Tok::KwUnit, "unit header");
  std::string name = consume_name_or_directive("unit name");
  expect(Tok::Semi, "unit header");

  expect(Tok::KwInterface, "unit interface");
  std::vector<std::string> interface_uses;
  if (check(Tok::KwUses)) {
    advance();
    interface_uses = parse_uses_clause();
    expect(Tok::Semi, "interface uses");
  }
  std::vector<DeclPtr> interface_decls =
      parse_decl_block(/*in_interface=*/true);

  expect(Tok::KwImplementation, "unit implementation");
  std::vector<std::string> impl_uses;
  if (check(Tok::KwUses)) {
    advance();
    impl_uses = parse_uses_clause();
    expect(Tok::Semi, "implementation uses");
  }
  std::vector<DeclPtr> impl_decls =
      parse_decl_block(/*in_interface=*/false);

  // TP-7-style init body: optional `begin ... end.` or just `end.`
  StmtPtr init_body;
  StmtPtr final_body;
  if (check(Tok::KwBegin)) {
    init_body = parse_compound_statement();
    expect(Tok::Dot, "unit tail");
  } else if (check(Tok::KwEnd)) {
    advance();
    expect(Tok::Dot, "unit tail");
  } else if (check(Tok::KwInitialization) || check(Tok::KwFinalization)) {
    if (accept(Tok::KwInitialization)) {
      init_body = parse_statement_block_until({Tok::KwFinalization, Tok::KwEnd});
    }
    if (accept(Tok::KwFinalization)) {
      final_body = parse_statement_block_until({Tok::KwEnd});
    }
    expect(Tok::KwEnd, "unit tail");
    expect(Tok::Dot, "unit tail");
  } else {
    expect(Tok::KwEnd, "unit tail");
  }
  return std::make_shared<UnitNode>(
      loc, std::move(name), std::move(interface_uses),
      std::move(interface_decls), std::move(impl_uses),
      std::move(impl_decls), std::move(init_body), std::move(final_body),
      false);
}

std::vector<std::string> Parser::parse_uses_clause() {
  std::vector<std::string> out;
  while (!at_end()) {
    if (cur_.kind != Tok::Ident) {
      expect(Tok::Ident, "uses clause");
      break;
    }
    out.push_back(cur_.text);
    advance();
    // FPC supports `uses foo in 'foo.pas'` -- swallow that form.
    if (accept(Tok::KwIn)) {
      if (cur_.kind == Tok::StringLit) advance();
    }
    if (!accept(Tok::Comma)) break;
  }
  return out;
}

StmtPtr Parser::parse_statement_block_until(std::initializer_list<Tok> stops) {
  auto is_stop = [&]() {
    if (at_end()) return true;
    for (Tok stop : stops) {
      if (check(stop)) return true;
    }
    return false;
  };

  Location loc = cur_.loc;
  std::vector<StmtPtr> body;
  while (!is_stop()) {
    auto s = parse_statement();
    if (s) body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  return std::make_shared<Compound>(loc, std::move(body));
}

// ---------------------------------------------------------------------------
// Decl block

std::vector<DeclPtr> Parser::parse_decl_block(bool in_interface) {
  std::vector<DeclPtr> out;
  auto append_section = [&](std::vector<DeclPtr> section) {
    out.insert(out.end(), std::make_move_iterator(section.begin()),
               std::make_move_iterator(section.end()));
  };
  for (;;) {
    switch (cur_.kind) {
      case Tok::KwConst: append_section(parse_const_section()); break;
      case Tok::KwType:  append_section(parse_type_section()); break;
      case Tok::KwVar:   append_section(parse_var_section()); break;
      case Tok::KwLabel: append_section(parse_label_section()); break;
      case Tok::KwProcedure: {
        auto d = parse_proc_decl(ProcKind::Procedure, in_interface,
                                 /*is_class_method=*/false,
                                 /*in_type_member=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwFunction: {
        auto d = parse_proc_decl(ProcKind::Function, in_interface,
                                 /*is_class_method=*/false,
                                 /*in_type_member=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwOperator: {
        auto d = parse_operator_decl(in_interface);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwClass: {
        Tok next = peek().kind;
        if (next != Tok::KwProcedure && next != Tok::KwFunction &&
            next != Tok::KwConstructor && next != Tok::KwDestructor) {
          return out;
        }
        advance();
        ProcKind pk = ProcKind::Procedure;
        if (check(Tok::KwFunction)) pk = ProcKind::Function;
        else if (check(Tok::KwConstructor)) pk = ProcKind::Constructor;
        else if (check(Tok::KwDestructor)) pk = ProcKind::Destructor;
        auto d = parse_proc_decl(pk, in_interface, /*is_class_method=*/true,
                                 /*in_type_member=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwConstructor:
      case Tok::KwDestructor: {
        auto pk = check(Tok::KwConstructor) ? ProcKind::Constructor
                                            : ProcKind::Destructor;
        auto d = parse_proc_decl(pk, in_interface,
                                 /*is_class_method=*/false,
                                 /*in_type_member=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      default:
        return out;
    }
  }
}

// ---------------------------------------------------------------------------
// const/type/var/label sections

std::vector<DeclPtr> Parser::parse_const_section() {
  std::vector<DeclPtr> out;
  expect(Tok::KwConst, "const section");
  while (check(Tok::Ident)) {
    Location loc = cur_.loc;
    std::string name = cur_.text;
    advance();
    const bool typed = accept(Tok::Colon);
    TypePtr type = typed ? parse_type() : nullptr;
    expect(Tok::Eq, "const decl");
    ExprPtr value = typed ? parse_const_value(type.get()) : parse_expr();
    while (is_directive("deprecated") || is_directive("platform") ||
           is_directive("library") || is_directive("experimental")) {
      advance();
      if (cur_.kind == Tok::StringLit) advance();
    }
    expect(Tok::Semi, "const decl");
    out.push_back(std::make_shared<ConstDecl>(
        loc, std::move(name), std::move(type), std::move(value)));
  }
  return out;
}

// Typed-constant value. Allowed forms:
//   (a, b, c, ...)                 array constant (possibly nested)
//   (f1: v1; f2: v2; ...)          record constant
//   any scalar expression (including set literals)
// Disambiguate at `(` by looking one token past it.
ast::ExprPtr Parser::parse_const_value(const TypeExpr* target) {
  if (!check(Tok::LParen)) return parse_expr();
  Location loc = cur_.loc;
  std::shared_ptr<TyArray> nested_array_target;
  const TypeExpr* elem_target = nullptr;
  if (target && target->kind == Kind::TyArray) {
    const auto& arr = static_cast<const TyArray&>(*target);
    elem_target = arr.element.get();
    if (arr.dims.size() > 1) {
      std::vector<TypePtr> dims(arr.dims.begin() + 1, arr.dims.end());
      nested_array_target = std::make_shared<TyArray>(
          arr.loc, std::move(dims), arr.element, arr.is_packed,
          arr.array_kind);
      elem_target = nested_array_target.get();
    }
  }
  // Try to decide: advance past '(' and look at the first two inner tokens.
  // If we see `ident :` where `:` is the immediate next token, it's a record.
  advance();  // consume '('
  bool is_record = cur_.kind == Tok::Ident && peek().kind == Tok::Colon;

  if (is_record) {
    std::vector<std::pair<std::string, ExprPtr>> fields;
    while (!at_end() && !check(Tok::RParen)) {
      std::string fname = consume_name_or_directive("record-constant field");
      expect(Tok::Colon, "record-constant field");
      auto val = parse_const_value();
      fields.emplace_back(std::move(fname), std::move(val));
      // Fields separated by ';'. Allow a trailing ';'.
      if (!accept(Tok::Semi)) break;
    }
    expect(Tok::RParen, "record constant");
    return std::make_shared<RecordConst>(loc, std::move(fields));
  }

  // Array constant or a single parenthesised expression.
  if (check(Tok::RParen)) {
    // Empty `()` -- treat as empty array constant.
    advance();
    return std::make_shared<ArrayConst>(loc);
  }
  auto first = parse_const_value(elem_target);
  if (accept(Tok::Comma)) {
    std::vector<ExprPtr> elements;
    elements.push_back(std::move(first));
    elements.push_back(parse_const_value(elem_target));
    while (accept(Tok::Comma)) {
      elements.push_back(parse_const_value(elem_target));
    }
    expect(Tok::RParen, "array constant");
    return std::make_shared<ArrayConst>(loc, std::move(elements));
  }
  if (target && target->kind == Kind::TyArray) {
    std::vector<ExprPtr> elements;
    elements.push_back(std::move(first));
    expect(Tok::RParen, "array constant");
    return std::make_shared<ArrayConst>(loc, std::move(elements));
  }
  // Single item inside parens is just a parenthesised constant. Without the
  // enclosing type we cannot reliably distinguish `(x)` from a 1-element
  // array constant, but treating it as an array silently miscompiles nested
  // record constants like `((a: 1))`.
  expect(Tok::RParen, "parenthesised constant");
  return first;
}

std::vector<DeclPtr> Parser::parse_type_section() {
  std::vector<DeclPtr> out;
  expect(Tok::KwType, "type section");
  while (check(Tok::Ident)) {
    Location loc = cur_.loc;
    std::string name = cur_.text;
    advance();
    expect(Tok::Eq, "type decl");
    TypePtr type = parse_type();
    expect(Tok::Semi, "type decl");
    out.push_back(
        std::make_shared<TypeDecl>(loc, std::move(name), std::move(type)));
  }
  return out;
}

std::vector<DeclPtr> Parser::parse_var_section() {
  std::vector<DeclPtr> out;
  expect(Tok::KwVar, "var section");
  while (check(Tok::Ident)) {
    Location loc = cur_.loc;
    std::vector<std::string> names;
    names.push_back(cur_.text);
    advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      names.push_back(cur_.text);
      advance();
    }
    expect(Tok::Colon, "var decl");
    TypePtr type = parse_type();
    ExprPtr init = nullptr;
    bool is_absolute = false;
    bool is_external = false;
    std::string absolute_target;
    ExprPtr external_name;
    std::string external_lib;
    // absolute / external / initialiser tail
    if (is_directive("absolute")) {
      advance();
      is_absolute = true;
      // `absolute Identifier` -- other forms (address literals) not needed yet
      if (cur_.kind == Tok::Ident) {
        absolute_target = cur_.text;
        advance();
      } else {
        report_error(cur_.loc, "expected identifier after 'absolute'");
      }
    } else if (accept(Tok::Eq)) {
      init = parse_const_value(type.get());
    } else if (is_directive("external")) {
      advance();
      is_external = true;
      if (cur_.kind == Tok::StringLit) {
        external_lib = cur_.text;
        advance();
      }
      if (is_directive("name")) {
        advance();
        if (cur_.kind == Tok::StringLit) {
          external_name = std::make_shared<StringLit>(cur_.loc, cur_.text);
          advance();
        }
      }
    }
    expect(Tok::Semi, "var decl");
    out.push_back(std::make_shared<VarDecl>(
        loc, std::move(names), std::move(type), std::move(init), is_absolute,
        is_external, std::move(absolute_target), std::move(external_name),
        std::move(external_lib)));
  }
  return out;
}

std::vector<DeclPtr> Parser::parse_label_section() {
  std::vector<DeclPtr> out;
  expect(Tok::KwLabel, "label section");
  Location loc = cur_.loc;
  std::vector<std::string> labels;
  while (check(Tok::Ident) || check(Tok::IntLit)) {
    labels.push_back(cur_.text);
    advance();
    if (!accept(Tok::Comma)) break;
  }
  expect(Tok::Semi, "label section");
  out.push_back(std::make_shared<LabelDecl>(loc, std::move(labels)));
  return out;
}

// ---------------------------------------------------------------------------
// Procedure/function declarations

std::optional<ast::ProcModifiers> Parser::parse_proc_modifier(
    bool in_type_member) {
  if (in_type_member) {
    // After a method semicolon, tokens that start the next class/object member
    // are no longer routine modifiers. In this context `public' starts a
    // visibility section, and an identifier followed by ':' or ',' starts a
    // field declaration even when that identifier is also a routine directive
    // name such as `name' or `external'.
    if (is_directive("public")) return std::nullopt;
    if (cur_.kind == Tok::Ident &&
        (peek().kind == Tok::Colon || peek().kind == Tok::Comma)) {
      return std::nullopt;
    }
  }

  if (is_directive("virtual")) {
    advance();
    return proc_modifier(ProcModifierFlag::Virtual);
  }
  else if (is_directive("abstract")) {
    advance();
    return proc_modifier(ProcModifierFlag::Abstract);
  }
  else if (is_directive("override")) {
    advance();
    return proc_modifier(ProcModifierFlag::Override);
  }
  else if (is_directive("final")) {
    advance();
    return proc_modifier(ProcModifierFlag::Final);
  }
  else if (is_directive("dynamic")) {
    advance();
    return proc_modifier(ProcModifierFlag::Virtual);
  }
  else if (is_directive("message")) {
    advance();
    // integer constant or identifier for the message number/name.
    if (cur_.kind == Tok::IntLit || cur_.kind == Tok::Ident
        || cur_.kind == Tok::StringLit) advance();
    return proc_modifier(ProcModifierFlag::Virtual);
  }
  else if (is_directive("forward")) {
    advance();
    return proc_modifier(ProcModifierFlag::Forward);
  }
  else if (is_directive("inline")) {
    advance();
    return proc_modifier(ProcModifierFlag::Inline);
  }
  else if (is_directive("cdecl")) {
    advance();
    return proc_modifier(ProcModifierFlag::Cdecl);
  }
  else if (is_directive("noreturn")) {
    advance();
    return proc_modifier(ProcModifierFlag::Noreturn);
  }
  else if (is_directive("assembler")) {
    advance();
    return proc_modifier(ProcModifierFlag::Assembler);
  }
  else if (is_directive("far")) { advance(); }
  else if (is_directive("near")) { advance(); }
  else if (is_directive("pascal")) { advance(); }
  else if (is_directive("register")) { advance(); }
  else if (is_directive("stdcall")) { advance(); }
  else if (is_directive("safecall")) { advance(); }
  else if (is_directive("interrupt")) { advance(); }
  else if (is_directive("popstack")) { advance(); }
  else if (is_directive("export")) { advance(); }
  else if (is_directive("public")) { advance(); }
  else if (is_directive("static")) { advance(); }
  else if (is_directive("external")) {
    advance();
    std::string external_lib;
    std::string external_name;
    if (cur_.kind == Tok::StringLit) {
      external_lib = cur_.text;
      advance();
    }
    if (is_directive("name")) {
      advance();
      if (cur_.kind == Tok::StringLit) {
        external_name = cur_.text;
        advance();
      }
    }
    return external_proc_modifier(std::move(external_lib),
                                  std::move(external_name));
  }
  else if (is_directive("alias")) {
    advance();
    // `alias: 'FPC_FOO'` -- swallow the linkage name.
    if (accept(Tok::Colon)) {
      if (cur_.kind == Tok::StringLit) advance();
    }
  }
  else if (is_directive("name")) {
    // Standalone `name 'foo'` (rarely on its own; usually after external).
    advance();
    if (cur_.kind == Tok::StringLit) advance();
  }
  else if (is_directive("overload")) {
    // `overload` is declarative: it permits a same-name overload set, but the
    // chosen declaration is still selected later by Pascal overload ranking.
    advance();
  }
  else if (is_directive("reintroduce")) {
    // `reintroduce' hides an inherited virtual method at this
    // overload-resolution level without marking it `override'.
    advance();
  }
  else if (is_directive("deprecated")
           || is_directive("platform")
           || is_directive("library")
           || is_directive("experimental")) {
    advance();
    if (cur_.kind == Tok::StringLit) advance();
  }
  else {
    return std::nullopt;
  }
  return ProcModifiers{};
}

ast::ProcModifiers Parser::parse_proc_modifiers(ProcModifiers modifiers,
                                                   bool in_type_member) {
  auto delta = parse_proc_modifier(in_type_member);
  if (!delta) return modifiers;
  accept(Tok::Semi);
  return parse_proc_modifiers(
      combine_proc_modifiers(std::move(modifiers), std::move(*delta)),
      in_type_member);
}

ast::ProcModifiers Parser::parse_proc_header_tail(const char* ctx,
                                                     bool in_type_member) {
  // The semicolon between a routine header and the first routine directive may
  // be omitted only when that next token is actually a routine directive. Use
  // the same consumer as the modifier loop so this header exception cannot drift
  // from the directive list.
  if (accept(Tok::Semi)) {
    return parse_proc_modifiers(ProcModifiers{}, in_type_member);
  }
  auto first = parse_proc_modifier(in_type_member);
  if (!first) {
    expect(Tok::Semi, ctx);
    return ProcModifiers{};
  }
  accept(Tok::Semi);
  return parse_proc_modifiers(std::move(*first), in_type_member);
}

std::shared_ptr<ProcDecl> Parser::parse_proc_decl(
    ProcKind pk, bool in_interface, bool is_class_method,
    bool in_type_member) {
  Location loc = cur_.loc;
  advance();  // consume procedure/function/constructor/destructor
  // Name, possibly qualified `TFoo.Bar`. Allow directive words as names
  // (e.g., `function TCollection.At(...)`).
  std::string name = consume_name_or_directive("routine name");
  std::string of_type;
  if (accept(Tok::Dot)) {
    of_type = std::move(name);
    name = consume_name_or_directive("method name");
  }
  std::vector<Param> params;
  if (accept(Tok::LParen)) {
    params = parse_formal_param_list();
    expect(Tok::RParen, "parameter list");
  }
  TypePtr return_type;
  if (pk == ProcKind::Function || pk == ProcKind::Constructor) {
    if (pk == ProcKind::Function) {
      expect(Tok::Colon, "function return type");
      return_type = parse_type();
    } else if (accept(Tok::Colon)) {
      // Constructors typically have no return; accept if written.
      return_type = parse_type();
    }
  }
  if (is_class_method &&
      (pk == ProcKind::Constructor || pk == ProcKind::Destructor)) {
    size_t param_count = 0;
    for (const auto& p : params) {
      param_count += p.names.empty() ? 1 : p.names.size();
    }
    if (param_count != 0) {
      report_error(loc,
                   "class constructors and destructors cannot have parameters");
    }
    if (return_type) {
      report_error(loc, "class constructors cannot have a return type");
    }
  }
  ProcModifiers modifiers =
      parse_proc_header_tail("routine header", in_type_member);

  // Interface sections never have bodies. Nor do forward/external/abstract
  // declarations.
  std::vector<DeclPtr> locals;
  StmtPtr body = nullptr;
  if (!(in_interface || modifiers.is_forward || modifiers.is_external ||
        modifiers.is_abstract)) {
    // Implementation-side definition: parse locals and body.
    locals = parse_decl_block(/*in_interface=*/false);
    if (check(Tok::KwBegin)) {
      body = parse_compound_statement();
      expect(Tok::Semi, "routine body");
    } else if (check(Tok::KwAsm)) {
      body = parse_asm();
      expect(Tok::Semi, "routine body");
    }
  }
  return std::make_shared<ProcDecl>(
      loc, pk, std::move(name), false, "",
      ProcDecl::IntrinsicOperator::None, std::move(of_type), is_class_method,
      std::move(params), std::move(return_type), std::move(modifiers),
      std::move(locals), std::move(body));
}

std::shared_ptr<ProcDecl> Parser::parse_operator_decl(bool in_interface) {
  Location loc = cur_.loc;
  advance();  // consume operator

  std::string op_text = operator_decl_token_text(cur_);
  if (op_text.empty()) {
    report_error(cur_.loc, "expected operator token after 'operator'");
  } else {
    advance();
  }

  std::vector<Param> params;
  if (accept(Tok::LParen)) {
    params = parse_formal_param_list();
    expect(Tok::RParen, "operator parameter list");
  }
  expect(Tok::Colon, "operator return type");
  TypePtr return_type = parse_type();
  ProcModifiers modifiers =
      parse_proc_header_tail("operator header", /*in_type_member=*/false);

  std::vector<DeclPtr> locals;
  StmtPtr body = nullptr;
  if (!(in_interface || modifiers.is_forward || modifiers.is_external ||
        modifiers.is_abstract)) {
    locals = parse_decl_block(/*in_interface=*/false);
    if (check(Tok::KwBegin)) {
      body = parse_compound_statement();
      expect(Tok::Semi, "operator body");
    } else if (check(Tok::KwAsm)) {
      body = parse_asm();
      expect(Tok::Semi, "operator body");
    }
  }
  return std::make_shared<ProcDecl>(
      loc, ProcKind::Function, "operator_" + op_text, true, std::move(op_text),
      ProcDecl::IntrinsicOperator::None, "", false, std::move(params),
      std::move(return_type), std::move(modifiers),
      std::move(locals), std::move(body));
}

std::vector<Param> Parser::parse_param_list(Tok close) {
  std::vector<Param> out;
  while (!at_end() && !check(close)) {
    Param::Mode mode = Param::Value;
    if (accept(Tok::KwVar)) mode = Param::Var;
    else if (accept(Tok::KwConst)) mode = Param::Const;
    else if (is_directive("constref") && peek().kind == Tok::Ident) {
      mode = Param::ConstRef;
      advance();
    }
    else if (is_directive("out") && peek().kind == Tok::Ident) {
      // `out` is a soft keyword here: consume it as a modifier only when a
      // parameter name follows, so bare identifiers named `out` stay legal.
      mode = Param::Out;
      advance();
    }
    // (Open-array `array of ...` -- accepted as a type later.)
    std::vector<std::string> names;
    if (cur_.kind == Tok::Ident) {
      names.push_back(cur_.text);
      advance();
      while (accept(Tok::Comma)) {
        if (cur_.kind != Tok::Ident) break;
        names.push_back(cur_.text);
        advance();
      }
    }
    TypePtr type = nullptr;
    if (accept(Tok::Colon)) {
      // `array of T` open-array form
      if (check(Tok::KwArray)) {
        Location loc = cur_.loc;
        advance();
        expect(Tok::KwOf, "open array parameter");
        type = std::make_shared<TyArray>(
            loc, std::vector<TypePtr>{}, parse_type(), false, ArrayKind::Open);
      } else {
        type = parse_type();
      }
    }
    ExprPtr default_value;
    if (accept(Tok::Eq)) default_value = parse_expr();
    out.emplace_back(mode, std::move(names), std::move(type),
                     std::move(default_value));
    if (!accept(Tok::Semi)) break;
  }
  return out;
}

std::vector<Param> Parser::parse_formal_param_list() {
  return parse_param_list(Tok::RParen);
}

// ---------------------------------------------------------------------------
// Types

static bool starts_type_tok(Tok t) {
  switch (t) {
    case Tok::Ident:
    case Tok::Caret:
    case Tok::KwArray:
    case Tok::KwRecord:
    case Tok::KwObject:
    case Tok::KwClass:
    case Tok::KwSet:
    case Tok::KwFile:
    case Tok::KwString:
    case Tok::KwShortstring:
    case Tok::KwProcedure:
    case Tok::KwFunction:
    case Tok::KwPacked:
    case Tok::LParen:
    case Tok::Minus:
    case Tok::Plus:
    case Tok::IntLit:
    case Tok::StringLit:
      return true;
    default:
      return false;
  }
}

bool Parser::tok_starts_type() const { return starts_type_tok(cur_.kind); }

TypePtr Parser::parse_type() {
  Location loc = cur_.loc;
  // Pointer form: ^T
  if (accept(Tok::Caret)) {
    return std::make_shared<TyPointer>(loc, parse_type());
  }
  // Delphi distinct-type alias: `T = type <Underlying>;'.  Creates
  // a new type that is layout-compatible with its underlying but
  // NOT assignment-compatible without an explicit cast.  We keep a
  // TyDistinct wrapper node so emit-time can produce a C++ struct
  // with an explicit ctor and explicit conversion operator,
  // preserving Pascal's type discipline (an integer variable can
  // NOT silently receive a TSuperRegister value).
  if (accept(Tok::KwType)) {
    return std::make_shared<TyDistinct>(loc, parse_type());
  }
  bool packed = false;
  if (accept(Tok::KwPacked)) packed = true;

  switch (cur_.kind) {
    case Tok::KwArray:  return parse_array_type(packed);
    case Tok::KwRecord: return parse_record_type(packed);
    case Tok::KwObject:
      return parse_object_type();
    case Tok::KwInterface:
      return parse_interface_type();
    case Tok::KwClass: {
      // `class' at type position can start either a class declaration
      // (`class[(parent)] ... end') or a metaclass reference
      // (`class of T').  Disambiguate by 1-token lookahead.
      if (peek().kind == Tok::KwOf) {
        Location mloc = cur_.loc;
        advance();  // class
        advance();  // of
        return std::make_shared<TyMetaclass>(
            mloc, consume_ident("metaclass target"));
      }
      return parse_object_type();
    }
    case Tok::KwSet:    return parse_set_type();
    case Tok::KwFile:   return parse_file_type();
    case Tok::KwProcedure:
    case Tok::KwFunction:
      return parse_procedural_type();
    case Tok::KwString:
    case Tok::KwShortstring: {
      bool string_keyword = cur_.kind == Tok::KwString;
      advance();
      if (accept(Tok::LBrack)) {
        ExprPtr max_length = parse_expr();
        expect(Tok::RBrack, "string length");
        return std::make_shared<TyString>(loc, std::move(max_length));
      }
      if (string_keyword) {
        return make_type_name(loc,
                              lex_.long_strings_active() ? "ansistring"
                                                         : "shortstring");
      }
      return make_type_name(loc, "shortstring");
    }
    default:
      return parse_simple_type();
  }
}

TypePtr Parser::parse_simple_type() {
  Location loc = cur_.loc;
  auto finish_subrange = [&](ExprPtr lo) -> TypePtr {
    if (lo && !is_constant_subrange_bound_expr(*lo)) {
      report_error(lo->loc, "non-constant subrange bound");
    }
    expect(Tok::DotDot, "subrange");
    ExprPtr hi = parse_subrange_bound();
    if (hi && !is_constant_subrange_bound_expr(*hi)) {
      report_error(hi->loc, "non-constant subrange bound");
    }
    return std::make_shared<TySubrange>(loc, std::move(lo), std::move(hi));
  };

  // Enum: `( a, b, c )`
  if (accept(Tok::LParen)) {
    std::vector<EnumMember> members;
    while (cur_.kind == Tok::Ident) {
      std::string name = cur_.text;
      advance();
      // FPC accepts both `:=` and `=` here.
      ExprPtr value = nullptr;
      if (accept(Tok::Assign) || accept(Tok::Eq)) {
        value = parse_expr();
      }
      members.emplace_back(std::move(name), std::move(value));
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "enumeration");
    return std::make_shared<TyEnum>(
        loc, static_cast<uint8_t>(lex_.packenum_active()), std::move(members));
  }
  // Either a name (possibly qualified) or a subrange literal-start.
  // Pascal named types are simple identifiers.  A subrange whose lower bound
  // starts with an identifier is unambiguous when the next token is `..`.
  // FPC also uses constant ordinal intrinsics in type bounds, e.g.
  // `low(TEnum)..pred(EnumMember)` or `0..sizeof(T)-1`.  Accept only those
  // call expressions in bounds here; arbitrary `foo(...)` is not a type-level
  // constant.
  if (cur_.kind == Tok::Ident) {
    if (peek().kind == Tok::DotDot) {
      return finish_subrange(parse_subrange_bound());
    }
    if (peek().kind == Tok::LParen &&
        is_subrange_bound_intrinsic(cur_.text)) {
      return finish_subrange(parse_subrange_bound());
    }
    std::string name = cur_.text;
    advance();
    // Qualified: unit.Type.
    while (accept(Tok::Dot)) {
      if (cur_.kind == Tok::Ident) {
        name += ".";
        name += cur_.text;
        advance();
      } else {
        break;
      }
    }
    return std::make_shared<TyName>(loc, std::move(name));
  }
  // Otherwise, treat as subrange.
  return finish_subrange(parse_subrange_bound());
}

TypePtr Parser::parse_array_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwArray, "array");
  std::vector<TypePtr> dims;
  ArrayKind array_kind = ArrayKind::Fixed;
  if (accept(Tok::LBrack)) {
    while (!at_end() && !check(Tok::RBrack)) {
      dims.push_back(parse_simple_type());
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RBrack, "array dimensions");
  } else {
    array_kind = ArrayKind::Dynamic;
  }
  expect(Tok::KwOf, "array");
  return std::make_shared<TyArray>(loc, std::move(dims), parse_type(), packed,
                                   array_kind);
}

TypePtr Parser::parse_record_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwRecord, "record");
  std::vector<RecordField> fields;

  // Plain fields until `end` or `case`.
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwCase)) {
    if (cur_.kind != Tok::Ident) break;
    std::vector<std::string> names;
    names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      names.push_back(cur_.text); advance();
    }
    expect(Tok::Colon, "record field");
    fields.emplace_back(std::move(names), parse_type());
    if (!accept(Tok::Semi)) break;
  }

  auto parse_variants = [&](auto& self, std::shared_ptr<VariantPart>& vpart) -> void {
    if (!accept(Tok::KwCase)) return;
    std::string tag_name;
    if (cur_.kind == Tok::Ident && peek().kind == Tok::Colon) {
      tag_name = cur_.text;
      advance();
      advance();  // ':'
    }
    TypePtr tag_type = parse_type();
    expect(Tok::KwOf, "variant record");

    std::vector<VariantCase> cases;
    while (!at_end() && !check(Tok::KwEnd) && !check(Tok::RParen)) {
      std::vector<ExprPtr> labels;
      labels.push_back(parse_expr());
      while (accept(Tok::Comma)) labels.push_back(parse_expr());
      expect(Tok::Colon, "variant case");
      expect(Tok::LParen, "variant case");
      std::vector<RecordField> case_fields;
      while (!at_end() && !check(Tok::RParen) && !check(Tok::KwCase)) {
        if (cur_.kind != Tok::Ident) break;
        std::vector<std::string> names;
        names.push_back(cur_.text); advance();
        while (accept(Tok::Comma)) {
          if (cur_.kind != Tok::Ident) break;
          names.push_back(cur_.text); advance();
        }
        expect(Tok::Colon, "variant field");
        case_fields.emplace_back(std::move(names), parse_type());
        if (!accept(Tok::Semi)) break;
      }
      
      std::shared_ptr<VariantPart> nested_vpart;
      self(self, nested_vpart);

      // printf removed
      expect(Tok::RParen, "variant case");
      cases.emplace_back(std::move(labels), std::move(case_fields), std::move(nested_vpart));
      if (!accept(Tok::Semi)) break;
    }
    // printf removed
    vpart = std::make_shared<VariantPart>(VariantPart{std::move(tag_name), std::move(tag_type), std::move(cases)});
  };
  
  std::shared_ptr<VariantPart> vpart;
  parse_variants(parse_variants, vpart);

  expect(Tok::KwEnd, "record");
  return std::make_shared<TyRecord>(
      loc, std::move(fields), std::move(vpart), packed);
}

TypePtr Parser::parse_object_type() {
  Location loc = cur_.loc;
  // Accept either TP-style `object` or Delphi-style `class`.  The
  // grammar bodies are (almost) identical: inheritance via (parent),
  // public/private/protected sections, field and method declarations.
  // We record which keyword was used on the resulting TyObject so the
  // emitter can pick value-semantics (object) vs reference-semantics
  // (class).
  bool is_reference_type = false;
  bool is_abstract = false;
  if (check(Tok::KwClass)) {
    expect(Tok::KwClass, "class");
    is_reference_type = true;
    if (is_directive("abstract")) {
      advance();
      is_abstract = true;
    }
  } else {
    expect(Tok::KwObject, "object");
  }
  std::string parent;
  std::vector<std::string> interfaces;
  if (accept(Tok::LParen)) {
    parent = consume_ident("parent class");
    while (accept(Tok::Comma)) {
      interfaces.push_back(consume_ident("implemented interface"));
    }
    expect(Tok::RParen, "parent class");
  }
  // Delphi forward class declaration: `T = class;' (body follows in a
  // later type declaration within the same section). A parenthesized
  // ancestor changes the meaning: `T = class(Base);` is a complete empty
  // class declaration, equivalent to `class(Base) end`.
  if (is_reference_type && check(Tok::Semi)) {
    bool is_forward = parent.empty();
    return std::make_shared<TyObject>(
        loc, std::move(parent), std::move(interfaces),
        std::vector<ObjectMember>{}, is_reference_type, is_abstract,
        is_forward);
  }
  Visibility vis = Visibility::Public;
  // `class var' starts a static-field section; a new visibility section
  // starts ordinary instance fields again.
  bool class_var_section = false;
  std::vector<ObjectMember> members;
  while (!at_end() && !check(Tok::KwEnd)) {
    // visibility change
    if (is_directive("public")) {
      advance();
      vis = Visibility::Public;
      class_var_section = false;
      continue;
    }
    if (is_directive("private")) {
      advance();
      vis = Visibility::Private;
      class_var_section = false;
      continue;
    }
    if (is_directive("protected")) {
      advance();
      vis = Visibility::Protected;
      class_var_section = false;
      continue;
    }
    if (is_directive("strict")) {
      Location strict_loc = cur_.loc;
      advance();
      if (is_directive("private")) {
        advance();
        vis = Visibility::StrictPrivate;
      } else if (is_directive("protected")) {
        advance();
        vis = Visibility::StrictProtected;
      } else {
        report_error(strict_loc,
                     "expected `private' or `protected' after `strict'");
      }
      class_var_section = false;
      continue;
    }
    if (is_directive("published")) {
      advance();
      vis = Visibility::Public;
      class_var_section = false;
      continue;
    }

    bool is_class_method = false;
    if (check(Tok::KwClass)) {
      Location class_loc = cur_.loc;
      advance();
      if (check(Tok::KwVar)) {
        advance();
        if (!is_reference_type) {
          report_error(class_loc, "`class var' is only valid in class types");
          continue;
        }
        class_var_section = true;
        continue;
      }
      if (!(check(Tok::KwProcedure) || check(Tok::KwFunction) ||
            check(Tok::KwConstructor) || check(Tok::KwDestructor))) {
        report_error(cur_.loc,
                     "expected `procedure', `function', `constructor', "
                     "`destructor', or `var' after `class'");
        break;
      }
      is_class_method = true;
    }

    if (check(Tok::KwProcedure) || check(Tok::KwFunction) ||
        check(Tok::KwConstructor) || check(Tok::KwDestructor)) {
      ProcKind pk = ProcKind::Procedure;
      if (check(Tok::KwFunction)) pk = ProcKind::Function;
      else if (check(Tok::KwConstructor)) pk = ProcKind::Constructor;
      else if (check(Tok::KwDestructor)) pk = ProcKind::Destructor;
      Location member_loc = cur_.loc;
      // Method headers inside an object are always signatures only --
      // bodies live in the implementation section (TP 7.0 semantics).
      members.emplace_back(
          member_loc, vis,
          parse_proc_decl(pk, /*in_interface=*/true, is_class_method,
                          /*in_type_member=*/true));
      continue;
    }

    if (is_directive("property")) {
      advance();
      Location member_loc = cur_.loc;
      std::string name = consume_name_or_directive("property name");
      std::vector<Param> params;
      if (accept(Tok::LBrack)) {
        params = parse_param_list(Tok::RBrack);
        expect(Tok::RBrack, "property index list");
      }
      expect(Tok::Colon, "property");
      TypePtr property_type = parse_type();
      std::optional<PropertyDecl::Accessor> read_accessor;
      std::optional<PropertyDecl::Accessor> write_accessor;
      bool saw_accessor = false;
      while (true) {
        if (!read_accessor && is_directive("read")) {
          advance();
          read_accessor.emplace(
              parse_property_accessor_path("property read accessor"));
          saw_accessor = true;
          continue;
        }
        if (!write_accessor && is_directive("write")) {
          advance();
          write_accessor.emplace(
              parse_property_accessor_path("property write accessor"));
          saw_accessor = true;
          continue;
        }
        break;
      }
      if (!saw_accessor) {
        report_error(cur_.loc, "expected property accessor in declaration");
      }
      bool consumed_tail_semi = false;
      bool is_default = false;
      while (accept(Tok::Semi)) {
        consumed_tail_semi = true;
        if (is_directive("default")) {
          advance();
          is_default = true;
          continue;
        }
        break;
      }
      if (!consumed_tail_semi) expect(Tok::Semi, "property");
      members.emplace_back(
          member_loc, vis,
          PropertyDecl(std::move(name), std::move(params),
                       std::move(property_type),
                       read_accessor ? std::move(*read_accessor)
                                     : PropertyDecl::Accessor{},
                       write_accessor ? std::move(*write_accessor)
                                      : PropertyDecl::Accessor{},
                       is_default));
      continue;
    }

    // Field
    if (cur_.kind != Tok::Ident) break;
    Location member_loc = cur_.loc;
    std::vector<std::string> names;
    names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      names.push_back(cur_.text); advance();
    }
    expect(Tok::Colon, "object field");
    members.emplace_back(member_loc, vis, std::move(names), parse_type(),
                         class_var_section);
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwEnd, "object");
  return std::make_shared<TyObject>(
      loc, std::move(parent), std::move(interfaces), std::move(members),
      is_reference_type, is_abstract, false);
}

TypePtr Parser::parse_interface_type() {
  Location loc = cur_.loc;
  expect(Tok::KwInterface, "interface");

  // Bare FPC interfaces default to COM/IUnknown semantics, which means
  // refcounting and QueryInterface support. p2cc only lowers the non-
  // refcounted CORBA subset, so require an explicit `{$interfaces corba}`.
  if (lex_.interface_mode_active() != InterfaceMode::Corba) {
    report_error(loc,
                 "COM/refcounted interfaces are unsupported; use "
                 "{$interfaces corba}");
  }

  // The optional bracketed interface string is metadata only for p2cc's C++
  // lowering:
  //   IFoo = interface ['{...}'] ... end;
  std::string metadata_string;
  if (accept(Tok::LBrack)) {
    if (cur_.kind == Tok::StringLit) {
      metadata_string = cur_.text;
      advance();
    } else {
      report_error(cur_.loc, "expected interface metadata string");
    }
    expect(Tok::RBrack, "interface metadata");
  }

  std::vector<ObjectMember> members;
  while (!at_end() && !check(Tok::KwEnd)) {
    if (check(Tok::KwProcedure) || check(Tok::KwFunction)) {
      ProcKind pk = check(Tok::KwFunction) ? ProcKind::Function
                                           : ProcKind::Procedure;
      Location member_loc = cur_.loc;
      members.emplace_back(
          member_loc, Visibility::Public,
          parse_proc_decl(pk, /*in_interface=*/true,
                          /*is_class_method=*/false,
                          /*in_type_member=*/true));
      continue;
    }
    report_error(cur_.loc, "expected interface method declaration");
    break;
  }
  expect(Tok::KwEnd, "interface");
  return std::make_shared<TyInterface>(loc, std::move(metadata_string),
                                       std::move(members));
}

TypePtr Parser::parse_set_type() {
  Location loc = cur_.loc;
  expect(Tok::KwSet, "set");
  expect(Tok::KwOf, "set");
  return std::make_shared<TySet>(loc, parse_simple_type());
}

TypePtr Parser::parse_file_type() {
  Location loc = cur_.loc;
  expect(Tok::KwFile, "file");
  TypePtr element = nullptr;
  if (accept(Tok::KwOf)) element = parse_type();
  return std::make_shared<TyFile>(loc, std::move(element));
}

TypePtr Parser::parse_procedural_type() {
  Location loc = cur_.loc;
  bool is_function = check(Tok::KwFunction);
  advance();  // procedure/function
  std::vector<Param> params;
  if (accept(Tok::LParen)) {
    params = parse_formal_param_list();
    expect(Tok::RParen, "procedural type");
  }
  TypePtr return_type;
  if (is_function) {
    expect(Tok::Colon, "function type");
    return_type = parse_type();
  }
  bool is_method = false;
  if (accept(Tok::KwOf)) {
    expect(Tok::KwObject, "procedural type");
    is_method = true;
  }
  // Optional calling-convention modifier (no `;` required in type position).
  bool is_cdecl = false;
  while (is_directive("cdecl") || is_directive("pascal") ||
         is_directive("stdcall")) {
    if (is_directive("cdecl")) is_cdecl = true;
    advance();
  }
  return std::make_shared<TyProcedural>(
      loc, is_function, std::move(params), std::move(return_type), is_cdecl,
      is_method);
}

// ---------------------------------------------------------------------------
// Statements

StmtPtr Parser::parse_compound_statement() {
  Location loc = cur_.loc;
  expect(Tok::KwBegin, "compound statement");
  std::vector<StmtPtr> body;
  while (!at_end() && !check(Tok::KwEnd)) {
    auto s = parse_statement();
    if (s) body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwEnd, "compound statement");
  return std::make_shared<Compound>(loc, std::move(body));
}

StmtPtr Parser::parse_statement() {
  // In statement position, `Ident:` and `123:` start labeled statements.
  if (cur_.kind == Tok::Ident && peek().kind == Tok::Colon) {
    std::string lab = cur_.text;
    Location loc = cur_.loc;
    advance();
    advance();  // ':'
    return std::make_shared<Labeled>(loc, std::move(lab), parse_statement());
  }
  if (cur_.kind == Tok::IntLit && peek().kind == Tok::Colon) {
    std::string lab = cur_.text;
    Location loc = cur_.loc;
    advance(); advance();
    return std::make_shared<Labeled>(loc, std::move(lab), parse_statement());
  }

  switch (cur_.kind) {
    case Tok::KwBegin:  return parse_compound_statement();
    case Tok::KwIf:     return parse_if();
    case Tok::KwWhile:  return parse_while();
    case Tok::KwRepeat: return parse_repeat();
    case Tok::KwFor:    return parse_for();
    case Tok::KwCase:   return parse_case();
    case Tok::KwWith:   return parse_with();
    case Tok::KwGoto: {
      Location loc = cur_.loc;
      advance();
      std::string label;
      if (cur_.kind == Tok::Ident || cur_.kind == Tok::IntLit) {
        label = cur_.text; advance();
      } else {
        expect(Tok::Ident, "goto target");
      }
      return std::make_shared<Goto>(loc, std::move(label));
    }
    // `break`, `continue`, `exit`, `fail`, `halt`, `new`, `dispose` are
    // builtin procedures -- not grammar keywords -- so they come through as
    // identifiers and are parsed as ordinary calls. Codegen recognises the
    // names.
    case Tok::KwAsm:    return parse_asm();
    case Tok::KwTry:    return parse_try();
    case Tok::KwRaise:  return parse_raise();
    case Tok::Semi:
    case Tok::KwEnd:
    case Tok::KwUntil:
    case Tok::KwElse: {
      // Empty statement.
      return std::make_shared<EmptyStmt>(cur_.loc);
    }
    default:
      return parse_labeled_or_simple();
  }
}

// Assignment or bare-call -- lhs := rhs OR lhs (as expr statement).
StmtPtr Parser::parse_labeled_or_simple() {
  Location loc = cur_.loc;
  auto lhs = parse_expr();
  if (accept(Tok::Assign)) {
    return std::make_shared<Assign>(loc, std::move(lhs), parse_expr(),
                                    lex_.range_check_active());
  }
  return std::make_shared<ExprStmt>(loc, std::move(lhs));
}

StmtPtr Parser::parse_if() {
  Location loc = cur_.loc;
  expect(Tok::KwIf, "if");
  ExprPtr cond = parse_expr();
  expect(Tok::KwThen, "if");
  StmtPtr then_branch = parse_statement();
  StmtPtr else_branch;
  if (accept(Tok::KwElse)) else_branch = parse_statement();
  return std::make_shared<If>(loc, std::move(cond), std::move(then_branch),
                              std::move(else_branch));
}

StmtPtr Parser::parse_while() {
  Location loc = cur_.loc;
  expect(Tok::KwWhile, "while");
  ExprPtr cond = parse_expr();
  expect(Tok::KwDo, "while");
  return std::make_shared<While>(loc, std::move(cond), parse_statement());
}

StmtPtr Parser::parse_repeat() {
  Location loc = cur_.loc;
  expect(Tok::KwRepeat, "repeat");
  std::vector<StmtPtr> body;
  while (!at_end() && !check(Tok::KwUntil)) {
    auto s = parse_statement();
    if (s) body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwUntil, "repeat");
  return std::make_shared<Repeat>(loc, std::move(body), parse_expr());
}

StmtPtr Parser::parse_for() {
  Location loc = cur_.loc;
  expect(Tok::KwFor, "for");
  std::string var = consume_ident("for variable");
  if (accept(Tok::KwIn)) {
    ExprPtr in_expr = parse_expr();
    expect(Tok::KwDo, "for");
    return std::make_shared<For>(loc, std::move(var), std::move(in_expr),
                                 parse_statement());
  }
  expect(Tok::Assign, "for");
  ExprPtr from = parse_expr();
  bool downto = false;
  if (accept(Tok::KwTo)) downto = false;
  else if (accept(Tok::KwDownto)) downto = true;
  else { expect(Tok::KwTo, "for"); }
  ExprPtr to = parse_expr();
  expect(Tok::KwDo, "for");
  return std::make_shared<For>(loc, std::move(var), std::move(from),
                               std::move(to), downto, parse_statement());
}

StmtPtr Parser::parse_case() {
  Location loc = cur_.loc;
  expect(Tok::KwCase, "case");
  ExprPtr selector = parse_expr();
  expect(Tok::KwOf, "case");
  std::vector<CaseArm> arms;
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwElse) &&
         !check(Tok::KwOtherwise)) {
    std::vector<ExprPtr> labels;
    labels.push_back(parse_expr());
    if (accept(Tok::DotDot)) {
      Location range_loc = labels.back()->loc;
      auto lo = std::move(labels.back());
      labels.pop_back();
      auto r = std::make_shared<Range>(range_loc, std::move(lo), parse_expr());
      labels.push_back(std::move(r));
    }
    while (accept(Tok::Comma)) {
      auto lab = parse_expr();
      if (accept(Tok::DotDot)) {
        Location range_loc = lab->loc;
        auto r =
            std::make_shared<Range>(range_loc, std::move(lab), parse_expr());
        lab = std::move(r);
      }
      labels.push_back(std::move(lab));
    }
    expect(Tok::Colon, "case arm");
    arms.emplace_back(std::move(labels), parse_statement());
    if (!accept(Tok::Semi)) break;
  }
  StmtPtr else_branch;
  if (accept(Tok::KwElse) || accept(Tok::KwOtherwise)) {
    // Body is a statement sequence up to end.
    Location else_loc = cur_.loc;
    std::vector<StmtPtr> body;
    while (!at_end() && !check(Tok::KwEnd)) {
      auto s = parse_statement();
      if (s) body.push_back(std::move(s));
      if (!accept(Tok::Semi)) break;
    }
    else_branch = std::make_shared<Compound>(else_loc, std::move(body));
  }
  expect(Tok::KwEnd, "case");
  return std::make_shared<CaseStmt>(loc, std::move(selector), std::move(arms),
                                    std::move(else_branch));
}

StmtPtr Parser::parse_with() {
  Location loc = cur_.loc;
  expect(Tok::KwWith, "with");
  std::vector<ExprPtr> exprs;
  exprs.push_back(parse_expr());
  while (accept(Tok::Comma)) exprs.push_back(parse_expr());
  expect(Tok::KwDo, "with");
  return std::make_shared<With>(loc, std::move(exprs), parse_statement());
}

// Pascal `try ... except/finally ... end'.
//
// Grammar:
//   TryStmt := 'try' StatementList ('except' ExceptPart | 'finally' StatementList) 'end'
//   ExceptPart := { 'on' [Ident ':'] ClassName 'do' Statement ';' } [ 'else' StatementList ]
//              |  StatementList       { catch-all-with-no-on, legal shortform }
//
// fpc's compiler.pas nests `try try ... except ... end except ... end'
// to combine finally + except.  That's not a new construct -- just the
// outer is finally and the inner is except (or vice versa).  Our AST
// models a single try as one or the other; nesting is composed by the
// caller.
StmtPtr Parser::parse_try() {
  Location loc = cur_.loc;
  expect(Tok::KwTry, "try");
  std::vector<StmtPtr> body;
  while (!at_end() && !check(Tok::KwExcept) && !check(Tok::KwFinally)) {
    body.push_back(parse_statement());
    if (!accept(Tok::Semi)) break;
  }
  bool is_finally = false;
  std::vector<ExceptHandler> handlers;
  StmtPtr except_else;
  std::vector<StmtPtr> finally_body;
  if (accept(Tok::KwFinally)) {
    is_finally = true;
    while (!at_end() && !check(Tok::KwEnd)) {
      finally_body.push_back(parse_statement());
      if (!accept(Tok::Semi)) break;
    }
  } else if (accept(Tok::KwExcept)) {
    // Zero or more `on ClassName [: Ident?] do Stmt;' arms.  If the
    // first token isn't `on', the body is a catch-all statement list
    // that runs on any exception (Delphi shorthand).
    //
    // `on' is a soft keyword -- used only in except arms, not in the
    // lexer's reserved-words table -- so it arrives as Tok::Ident and
    // we match it via is_directive.
    if (is_directive("on")) {
      while (is_directive("on")) {
        advance();
        // `on Ident : ClassName do' OR `on ClassName do' (bind omitted).
        // Disambiguate by 1-token lookahead for `:'.
        std::string first = consume_ident("exception handler");
        std::string var_name;
        std::string class_name;
        if (accept(Tok::Colon)) {
          var_name = std::move(first);
          class_name = consume_ident("exception class");
        } else {
          class_name = std::move(first);
        }
        expect(Tok::KwDo, "exception handler");
        handlers.emplace_back(std::move(var_name), std::move(class_name),
                              parse_statement());
        accept(Tok::Semi);
      }
      // Optional else-branch after `on' arms: catch-all.
      if (accept(Tok::KwElse)) {
        Location else_loc = cur_.loc;
        std::vector<StmtPtr> else_body;
        while (!at_end() && !check(Tok::KwEnd)) {
          else_body.push_back(parse_statement());
          if (!accept(Tok::Semi)) break;
        }
        except_else = std::make_shared<Compound>(else_loc, std::move(else_body));
      }
    } else {
      // Catch-all shorthand: any statement list here runs on any
      // exception.  Model as the else-branch of an handlerless except.
      Location else_loc = cur_.loc;
      std::vector<StmtPtr> else_body;
      while (!at_end() && !check(Tok::KwEnd)) {
        else_body.push_back(parse_statement());
        if (!accept(Tok::Semi)) break;
      }
      except_else = std::make_shared<Compound>(else_loc, std::move(else_body));
    }
  } else {
    expect(Tok::KwExcept, "try");  // reports a useful error
  }
  expect(Tok::KwEnd, "try");
  return std::make_shared<Try>(
      loc, std::move(body), is_finally, std::move(handlers),
      std::move(except_else), std::move(finally_body));
}

// `raise [Expr] [at Expr [, Expr]]' -- raise or re-raise an exception.
// Bare `raise;' inside an except handler re-raises the current one.
StmtPtr Parser::parse_raise() {
  Location loc = cur_.loc;
  expect(Tok::KwRaise, "raise");
  ExprPtr value = nullptr;
  // Optional expression -- the instance being raised.  Absent iff the
  // next token ends the statement (`;', `end', `else', ...).
  if (!check(Tok::Semi) && !check(Tok::KwEnd) && !check(Tok::KwElse)
      && !check(Tok::KwUntil)) {
    value = parse_expr();
  }
  // Optional `at <address>[, <frame>]' is a raise-statement suffix.
  if (is_directive("at")) {
    advance();
    parse_expr();
    if (accept(Tok::Comma)) parse_expr();
  }
  return std::make_shared<Raise>(loc, std::move(value));
}

StmtPtr Parser::parse_asm() {
  Location loc = cur_.loc;
  expect(Tok::KwAsm, "asm");
  // We do not tokenize asm bodies. Drain lexer tokens until matching `end`.
  // Because the lexer doesn't know we're in asm, this strategy only works
  // for small/well-formed asm blocks -- but the only one in the compiler
  // proper is in tpexcept.pas which we replace wholesale. We still aim not
  // to crash.
  int depth = 1;
  while (!at_end() && depth > 0) {
    if (cur_.kind == Tok::KwEnd) { --depth; }
    else if (cur_.kind == Tok::KwAsm) { ++depth; }
    if (depth == 0) break;
    advance();
  }
  expect(Tok::KwEnd, "asm");
  return std::make_shared<AsmStmt>(loc);
}

// ---------------------------------------------------------------------------
// Expressions (precedence from lowest to highest):
//
//   expression   := simple_expr ( relop simple_expr )?
//   simple_expr  := term ( addop term )*
//   term         := factor ( mulop factor )*
//   factor       := ( + | - | NOT ) factor | unary-primary
//
// relop: =  <>  <  >  <=  >=  in  is
// addop: +  -  or  xor
// mulop: *  /  div  mod  and  shl  shr

ExprPtr Parser::parse_expr() {
  auto lhs = parse_simple_expr();
  for (;;) {
    const auto op = relational_operator(cur_.kind);
    if (!op) return lhs;
    Location loc = cur_.loc; advance();
    auto rhs = parse_simple_expr();
    auto b = std::make_shared<Binary>(loc, *op, std::move(lhs), std::move(rhs),
                                      lex_.overflow_check_active());
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_subrange_bound() {
  // Subrange bounds are constant ordinal expressions, not full relational
  // expressions. Parsing them as simple_expr keeps `=` out of the bound and
  // avoids swallowing the typed-const `=` that follows declarations like
  // `array[0..1] of 0..15 = (...)`.
  return parse_simple_expr();
}

ExprPtr Parser::parse_simple_expr() {
  auto lhs = parse_term();
  for (;;) {
    const auto op = additive_operator(cur_.kind);
    if (!op) return lhs;
    Location loc = cur_.loc; advance();
    auto rhs = parse_term();
    auto b = std::make_shared<Binary>(loc, *op, std::move(lhs), std::move(rhs),
                                      lex_.overflow_check_active());
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_term() {
  auto lhs = parse_factor();
  for (;;) {
    const auto op = multiplicative_operator(cur_.kind);
    if (!op) return lhs;
    Location loc = cur_.loc; advance();
    auto rhs = parse_factor();
    auto b = std::make_shared<Binary>(loc, *op, std::move(lhs), std::move(rhs),
                                      lex_.overflow_check_active());
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_factor() {
  if (check(Tok::Plus) || check(Tok::Minus)) {
    UnOp op = check(Tok::Minus) ? UnOp::Neg : UnOp::Plus;
    Location loc = cur_.loc; advance();
    return std::make_shared<Unary>(loc, op, parse_factor(),
                                   lex_.overflow_check_active());
  }
  if (check(Tok::KwNot)) {
    Location loc = cur_.loc; advance();
    return std::make_shared<Unary>(loc, UnOp::Not, parse_factor());
  }
  if (check(Tok::At) || check(Tok::AtAt)) {
    Location loc = cur_.loc;
    bool dbl = (cur_.kind == Tok::AtAt);
    advance();
    return std::make_shared<AddrOf>(loc, dbl, parse_factor());
  }
  return parse_primary();
}

ExprPtr Parser::parse_primary() {
  Location loc = cur_.loc;
  switch (cur_.kind) {
    case Tok::IntLit: {
      auto n = std::make_shared<IntLit>(loc, cur_.int_value);
      advance();
      return parse_postfix(std::move(n));
    }
    case Tok::RealLit: {
      auto n = std::make_shared<RealLit>(loc, cur_.text);
      advance();
      return parse_postfix(std::move(n));
    }
    case Tok::StringLit: {
      auto n = std::make_shared<StringLit>(loc, cur_.text);
      advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwNil: {
      auto n = std::make_shared<NilLit>(loc);
      advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      auto n = std::make_shared<BoolLit>(loc, cur_.kind == Tok::KwTrue);
      advance();
      return parse_postfix(std::move(n));
    }
    case Tok::LParen: {
      advance();
      auto e = parse_expr();
      expect(Tok::RParen, "parenthesised expression");
      return parse_postfix(std::move(e));
    }
    case Tok::LBrack:
      return parse_postfix(parse_set_literal());
    case Tok::KwInherited: {
      // `inherited Method[(...)]` -> Member(Ident("inherited"), "method").
      // Bare `inherited;` stays as Ident("inherited").
      advance();
      auto base = std::make_shared<Ident>(loc, "inherited");
      if (cur_.kind == Tok::Ident) {
        auto m = std::make_shared<Member>(loc, std::move(base), cur_.text);
        advance();
        return parse_postfix(std::move(m));
      }
      return parse_postfix(std::move(base));
    }
    case Tok::KwString:
    case Tok::KwShortstring: {
      // In expression position, `string(x)` / `shortstring(x)` is a typecast.
      // `string` follows the current H-mode at parse time.
      auto id = std::make_shared<Ident>(
          loc,
          cur_.kind == Tok::KwString && lex_.long_strings_active()
              ? "ansistring"
              : "shortstring");
      advance();
      return parse_postfix(std::move(id));
    }
    case Tok::Ident:
    case Tok::KwSelf: {
      auto id = std::make_shared<Ident>(loc, cur_.text);
      advance();
      return parse_postfix(std::move(id));
    }
    default: {
      report_error(loc,
                   "expected expression, got '" + cur_.text + "'");
      auto n = std::make_shared<IntLit>(loc, 0);
      advance();
      return n;
    }
  }
}

ExprPtr Parser::parse_postfix(ExprPtr lhs) {
  for (;;) {
    Location loc = cur_.loc;
    if (accept(Tok::Dot)) {
      auto m = std::make_shared<Member>(
          loc, std::move(lhs), consume_name_or_directive("member name"));
      lhs = std::move(m);
      continue;
    }
    if (accept(Tok::Caret)) {
      lhs = std::make_shared<Deref>(loc, std::move(lhs));
      continue;
    }
    if (accept(Tok::LBrack)) {
      std::vector<ExprPtr> indices;
      indices.push_back(parse_expr());
      while (accept(Tok::Comma)) indices.push_back(parse_expr());
      expect(Tok::RBrack, "index");
      lhs = std::make_shared<Index>(loc, std::move(lhs), std::move(indices));
      continue;
    }
    if (accept(Tok::LParen)) {
      std::vector<ExprPtr> args;
      std::vector<ExprPtr> width;
      std::vector<ExprPtr> precision;
      while (!at_end() && !check(Tok::RParen)) {
        auto arg = parse_expr();
        ExprPtr w, p;
        if (accept(Tok::Colon)) {
          w = parse_expr();
          if (accept(Tok::Colon)) p = parse_expr();
        }
        args.push_back(std::move(arg));
        width.push_back(std::move(w));
        precision.push_back(std::move(p));
        if (!accept(Tok::Comma)) break;
      }
      expect(Tok::RParen, "call");
      lhs = std::make_shared<Call>(loc, std::move(lhs), std::move(args),
                                   std::move(width), std::move(precision));
      continue;
    }
    return lhs;
  }
}

ExprPtr Parser::parse_set_literal() {
  Location loc = cur_.loc;
  expect(Tok::LBrack, "set literal");
  std::vector<ExprPtr> elements;
  while (!at_end() && !check(Tok::RBrack)) {
    auto lo = parse_expr();
    if (accept(Tok::DotDot)) {
      Location range_loc = lo->loc;
      auto r = std::make_shared<Range>(range_loc, std::move(lo), parse_expr());
      elements.push_back(std::move(r));
    } else {
      elements.push_back(std::move(lo));
    }
    if (!accept(Tok::Comma)) break;
  }
  expect(Tok::RBrack, "set literal");
  return std::make_shared<SetLit>(loc, std::move(elements));
}

}  // namespace tp2cc
