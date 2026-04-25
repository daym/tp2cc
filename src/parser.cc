#include "parser.h"

#include <initializer_list>
#include <utility>

#include "diag.h"

namespace tp2cc {

using namespace ast;

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

// ---------------------------------------------------------------------------
// Entry

std::shared_ptr<UnitNode> Parser::parse() {
  if (cur_.kind == Tok::KwUnit) return parse_unit();
  if (cur_.kind == Tok::KwProgram) return parse_program();
  // Some sources are `.inc` files or program-body-only; treat as program
  // without a header.
  if (cur_.kind == Tok::KwBegin) {
    auto u = std::make_shared<UnitNode>();
    u->loc = cur_.loc;
    u->is_program = true;
    u->init_body = parse_compound_statement();
    if (check(Tok::Dot)) advance();
    return u;
  }
  report_error(cur_.loc,
               "expected 'program' or 'unit' at top of compilation unit");
  return nullptr;
}

std::shared_ptr<UnitNode> Parser::parse_program() {
  auto u = std::make_shared<UnitNode>();
  u->loc = cur_.loc;
  u->is_program = true;
  expect(Tok::KwProgram, "program header");
  u->name = consume_name_or_directive("program name");
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
  if (check(Tok::KwUses)) {
    advance();
    parse_uses_into(u->impl_uses);
    expect(Tok::Semi, "uses clause");
  }
  parse_decl_block(u->impl_decls, /*in_interface=*/false);
  // Main body.
  if (check(Tok::KwBegin)) {
    u->init_body = parse_compound_statement();
  }
  expect(Tok::Dot, "program tail");
  return u;
}

std::shared_ptr<UnitNode> Parser::parse_unit() {
  auto u = std::make_shared<UnitNode>();
  u->loc = cur_.loc;
  u->is_program = false;
  expect(Tok::KwUnit, "unit header");
  u->name = consume_name_or_directive("unit name");
  expect(Tok::Semi, "unit header");

  expect(Tok::KwInterface, "unit interface");
  if (check(Tok::KwUses)) {
    advance();
    parse_uses_into(u->interface_uses);
    expect(Tok::Semi, "interface uses");
  }
  parse_decl_block(u->interface_decls, /*in_interface=*/true);

  expect(Tok::KwImplementation, "unit implementation");
  if (check(Tok::KwUses)) {
    advance();
    parse_uses_into(u->impl_uses);
    expect(Tok::Semi, "implementation uses");
  }
  parse_decl_block(u->impl_decls, /*in_interface=*/false);

  // TP-7-style init body: optional `begin ... end.` or just `end.`
  if (check(Tok::KwBegin)) {
    u->init_body = parse_compound_statement();
    expect(Tok::Dot, "unit tail");
  } else if (check(Tok::KwEnd)) {
    advance();
    expect(Tok::Dot, "unit tail");
  } else if (check(Tok::KwInitialization) || check(Tok::KwFinalization)) {
    if (accept(Tok::KwInitialization)) {
      u->init_body = parse_statement_block_until(
          {Tok::KwFinalization, Tok::KwEnd});
    }
    if (accept(Tok::KwFinalization)) {
      u->final_body = parse_statement_block_until({Tok::KwEnd});
    }
    expect(Tok::KwEnd, "unit tail");
    expect(Tok::Dot, "unit tail");
  } else {
    expect(Tok::KwEnd, "unit tail");
  }
  return u;
}

void Parser::parse_uses_into(std::vector<std::string>& out) {
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
}

StmtPtr Parser::parse_statement_block_until(std::initializer_list<Tok> stops) {
  auto is_stop = [&]() {
    if (at_end()) return true;
    for (Tok stop : stops) {
      if (check(stop)) return true;
    }
    return false;
  };

  auto c = std::make_shared<Compound>();
  c->loc = cur_.loc;
  while (!is_stop()) {
    auto s = parse_statement();
    if (s) c->body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  return c;
}

// ---------------------------------------------------------------------------
// Decl block

void Parser::parse_decl_block(std::vector<DeclPtr>& out, bool in_interface) {
  for (;;) {
    switch (cur_.kind) {
      case Tok::KwConst: parse_const_section(out); break;
      case Tok::KwType:  parse_type_section(out); break;
      case Tok::KwVar:   parse_var_section(out); break;
      case Tok::KwLabel: parse_label_section(out); break;
      case Tok::KwProcedure: {
        auto d = parse_proc_decl(ProcKind::Procedure, in_interface,
                                 /*is_class_method=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwFunction: {
        auto d = parse_proc_decl(ProcKind::Function, in_interface,
                                 /*is_class_method=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwClass: {
        Tok next = peek().kind;
        if (next != Tok::KwProcedure && next != Tok::KwFunction) return;
        advance();
        auto pk = check(Tok::KwFunction) ? ProcKind::Function
                                         : ProcKind::Procedure;
        auto d = parse_proc_decl(pk, in_interface, /*is_class_method=*/true);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwConstructor:
      case Tok::KwDestructor: {
        auto pk = check(Tok::KwConstructor) ? ProcKind::Constructor
                                            : ProcKind::Destructor;
        auto d = parse_proc_decl(pk, in_interface,
                                 /*is_class_method=*/false);
        if (d) out.push_back(std::move(d));
        break;
      }
      default:
        return;
    }
  }
}

// ---------------------------------------------------------------------------
// const/type/var/label sections

void Parser::parse_const_section(std::vector<DeclPtr>& out) {
  expect(Tok::KwConst, "const section");
  while (check(Tok::Ident)) {
    auto cd = std::make_shared<ConstDecl>();
    cd->loc = cur_.loc;
    cd->name = cur_.text;
    advance();
    bool typed = false;
    if (accept(Tok::Colon)) {
      cd->type = parse_type();
      typed = true;
    }
    expect(Tok::Eq, "const decl");
    cd->value = typed ? parse_const_value() : parse_expr();
    expect(Tok::Semi, "const decl");
    out.push_back(std::move(cd));
  }
}

// Typed-constant value. Allowed forms:
//   (a, b, c, ...)                 array constant (possibly nested)
//   (f1: v1; f2: v2; ...)          record constant
//   any scalar expression (including set literals)
// Disambiguate at `(` by looking one token past it.
ast::ExprPtr Parser::parse_const_value() {
  if (!check(Tok::LParen)) return parse_expr();
  Location loc = cur_.loc;
  // Try to decide: advance past '(' and look at the first two inner tokens.
  // If we see `ident :` where `:` is the immediate next token, it's a record.
  advance();  // consume '('
  bool is_record = cur_.kind == Tok::Ident && peek().kind == Tok::Colon;

  if (is_record) {
    auto rc = std::make_shared<RecordConst>();
    rc->loc = loc;
    while (!at_end() && !check(Tok::RParen)) {
      std::string fname = consume_name_or_directive("record-constant field");
      expect(Tok::Colon, "record-constant field");
      auto val = parse_const_value();
      rc->fields.emplace_back(std::move(fname), std::move(val));
      // Fields separated by ';'. Allow a trailing ';'.
      if (!accept(Tok::Semi)) break;
    }
    expect(Tok::RParen, "record constant");
    return rc;
  }

  // Array constant or a single parenthesised expression.
  if (check(Tok::RParen)) {
    // Empty `()` -- treat as empty array constant.
    auto ac = std::make_shared<ArrayConst>();
    ac->loc = loc;
    advance();
    return ac;
  }
  auto first = parse_const_value();
  if (accept(Tok::Comma)) {
    auto ac = std::make_shared<ArrayConst>();
    ac->loc = loc;
    ac->elements.push_back(std::move(first));
    ac->elements.push_back(parse_const_value());
    while (accept(Tok::Comma)) ac->elements.push_back(parse_const_value());
    expect(Tok::RParen, "array constant");
    return ac;
  }
  // Single item inside parens is just a parenthesised constant. Without the
  // enclosing type we cannot reliably distinguish `(x)` from a 1-element
  // array constant, but treating it as an array silently miscompiles nested
  // record constants like `((a: 1))`.
  expect(Tok::RParen, "parenthesised constant");
  return first;
}

void Parser::parse_type_section(std::vector<DeclPtr>& out) {
  expect(Tok::KwType, "type section");
  while (check(Tok::Ident)) {
    auto td = std::make_shared<TypeDecl>();
    td->loc = cur_.loc;
    td->name = cur_.text;
    advance();
    expect(Tok::Eq, "type decl");
    td->type = parse_type();
    expect(Tok::Semi, "type decl");
    out.push_back(std::move(td));
  }
}

void Parser::parse_var_section(std::vector<DeclPtr>& out) {
  expect(Tok::KwVar, "var section");
  while (check(Tok::Ident)) {
    auto vd = std::make_shared<VarDecl>();
    vd->loc = cur_.loc;
    vd->names.push_back(cur_.text);
    advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      vd->names.push_back(cur_.text);
      advance();
    }
    expect(Tok::Colon, "var decl");
    vd->type = parse_type();
    // absolute / external / initialiser tail
    if (is_directive("absolute")) {
      advance();
      vd->is_absolute = true;
      // `absolute Identifier` -- other forms (address literals) not needed yet
      if (cur_.kind == Tok::Ident) {
        vd->absolute_target = cur_.text;
        advance();
      } else {
        report_error(cur_.loc, "expected identifier after 'absolute'");
      }
    } else if (accept(Tok::Eq)) {
      vd->init = parse_const_value();
    } else if (is_directive("external")) {
      advance();
      vd->is_external = true;
      vd->external_name = nullptr;
      if (cur_.kind == Tok::StringLit) {
        vd->external_lib = cur_.text;
        advance();
      }
      if (is_directive("name")) {
        advance();
        if (cur_.kind == Tok::StringLit) {
          auto s = std::make_shared<StringLit>();
          s->loc = cur_.loc;
          s->value = cur_.text;
          vd->external_name = std::move(s);
          advance();
        }
      }
    }
    expect(Tok::Semi, "var decl");
    out.push_back(std::move(vd));
  }
}

void Parser::parse_label_section(std::vector<DeclPtr>& out) {
  expect(Tok::KwLabel, "label section");
  auto ld = std::make_shared<LabelDecl>();
  ld->loc = cur_.loc;
  while (check(Tok::Ident) || check(Tok::IntLit)) {
    ld->labels.push_back(cur_.text);
    advance();
    if (!accept(Tok::Comma)) break;
  }
  expect(Tok::Semi, "label section");
  out.push_back(std::move(ld));
}

// ---------------------------------------------------------------------------
// Procedure/function declarations

void Parser::parse_proc_modifiers(ProcDecl& pd) {
  // Pascal "directives" -- position-dependent modifiers that follow a
  // routine header, separated by `;`.
  // Class methods currently lower only to the non-virtual/static subset.
  // Reject modifiers that would require metaclass dispatch instead of
  // silently pretending they mean the same thing in C++.
  auto reject_class_modifier = [&](const char* name) {
    report_error(cur_.loc,
                 std::string("class methods with `") + name +
                     "` are unsupported");
  };
  for (;;) {
    if (is_directive("virtual")) {
      if (pd.is_class_method) reject_class_modifier("virtual");
      else pd.is_virtual = true;
      advance();
    }
    else if (is_directive("abstract")) {
      if (pd.is_class_method) reject_class_modifier("abstract");
      else pd.is_abstract = true;
      advance();
    }
    else if (is_directive("override")) {
      if (pd.is_class_method) reject_class_modifier("override");
      else pd.is_override = true;
      advance();
    }
    else if (is_directive("dynamic")) {
      if (pd.is_class_method) reject_class_modifier("dynamic");
      else pd.is_virtual = true;
      advance();
    }
    else if (is_directive("message")) {
      if (pd.is_class_method) reject_class_modifier("message");
      advance();
      // integer constant or identifier for the message number/name.
      if (cur_.kind == Tok::IntLit || cur_.kind == Tok::Ident
          || cur_.kind == Tok::StringLit) advance();
      if (!pd.is_class_method) pd.is_virtual = true;
    }
    else if (is_directive("forward")) { pd.is_forward = true; advance(); }
    else if (is_directive("inline")) { pd.is_inline = true; advance(); }
    else if (is_directive("cdecl")) { pd.is_cdecl = true; advance(); }
    else if (is_directive("assembler")) { pd.is_assembler = true; advance(); }
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
      pd.is_external = true;
      advance();
      if (cur_.kind == Tok::StringLit) { pd.external_lib = cur_.text; advance(); }
      if (is_directive("name")) {
        advance();
        if (cur_.kind == Tok::StringLit) { pd.external_name = cur_.text; advance(); }
      }
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
      // Delphi-style method overloading.  Emit-time handling: drop the
      // directive; C++ overload resolution picks the right signature
      // from the emitted `p_<Method>(...)' candidates automatically.
      advance();
    }
    else if (is_directive("reintroduce")) {
      // `reintroduce' hides an inherited virtual method at this
      // overload-resolution level without marking it `override'.
      // Emit-time: can be translated to a fresh `virtual' declaration
      // on the derived class.  For now, recognise and skip.
      advance();
    }
    else if (is_directive("deprecated")
             || is_directive("platform")
             || is_directive("library")
             || is_directive("experimental")) {
      // Purely advisory; swallow and keep going.
      advance();
      if (cur_.kind == Tok::StringLit) advance();
    }
    else {
      return;
    }
    // Semicolon between directives is conventional but optional in fpc --
    // e.g. `: THandle;stdcall external 'x' name 'y';`. Consume one if
    // present and continue.
    accept(Tok::Semi);
  }
}

std::shared_ptr<ProcDecl> Parser::parse_proc_decl(
    ProcKind pk, bool in_interface, bool is_class_method) {
  auto pd = std::make_shared<ProcDecl>(is_class_method);
  pd->loc = cur_.loc;
  pd->pkind = pk;
  advance();  // consume procedure/function/constructor/destructor
  // Name, possibly qualified `TFoo.Bar`. Allow directive words as names
  // (e.g., `function TCollection.At(...)`).
  pd->name = consume_name_or_directive("routine name");
  if (accept(Tok::Dot)) {
    pd->of_type = pd->name;
    pd->name = consume_name_or_directive("method name");
  }
  if (accept(Tok::LParen)) {
    pd->params = parse_formal_param_list();
    expect(Tok::RParen, "parameter list");
  }
  if (pk == ProcKind::Function || pk == ProcKind::Constructor) {
    if (pk == ProcKind::Function) {
      expect(Tok::Colon, "function return type");
      pd->return_type = parse_type();
    } else if (accept(Tok::Colon)) {
      // Constructors typically have no return; accept if written.
      pd->return_type = parse_type();
    }
  }
  expect(Tok::Semi, "routine header");
  parse_proc_modifiers(*pd);

  // Interface sections never have bodies. Nor do forward/external/abstract
  // declarations.
  if (in_interface || pd->is_forward || pd->is_external || pd->is_abstract) {
    return pd;
  }
  // Implementation-side definition: parse locals and body.
  parse_decl_block(pd->locals, /*in_interface=*/false);
  if (check(Tok::KwBegin)) {
    pd->body = parse_compound_statement();
    expect(Tok::Semi, "routine body");
  } else if (check(Tok::KwAsm)) {
    pd->body = parse_asm();
    expect(Tok::Semi, "routine body");
  }
  return pd;
}

std::vector<Param> Parser::parse_param_list(Tok close) {
  std::vector<Param> out;
  while (!at_end() && !check(close)) {
    Param p;
    if (accept(Tok::KwVar)) p.mode = Param::Var;
    else if (accept(Tok::KwConst)) p.mode = Param::Const;
    else if (is_directive("out") && peek().kind == Tok::Ident) {
      // `out` is a soft keyword here: consume it as a modifier only when a
      // parameter name follows, so bare identifiers named `out` stay legal.
      p.mode = Param::Out;
      advance();
    }
    // (Open-array `array of ...` -- accepted as a type later.)
    if (cur_.kind == Tok::Ident) {
      p.names.push_back(cur_.text);
      advance();
      while (accept(Tok::Comma)) {
        if (cur_.kind != Tok::Ident) break;
        p.names.push_back(cur_.text);
        advance();
      }
    }
    if (accept(Tok::Colon)) {
      // `array of T` open-array form
      if (accept(Tok::KwArray)) {
        expect(Tok::KwOf, "open array parameter");
        auto ta = std::make_shared<TyArray>();
        ta->loc = cur_.loc;
        ta->array_kind = ArrayKind::Open;
        ta->element = parse_type();
        p.type = std::move(ta);
      } else {
        p.type = parse_type();
      }
    }
    if (accept(Tok::Eq)) p.default_value = parse_expr();
    out.push_back(std::move(p));
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
    auto tp = std::make_shared<TyPointer>();
    tp->loc = loc;
    tp->target = parse_type();
    return tp;
  }
  // Delphi distinct-type alias: `T = type <Underlying>;'.  Creates
  // a new type that is layout-compatible with its underlying but
  // NOT assignment-compatible without an explicit cast.  We keep a
  // TyDistinct wrapper node so emit-time can produce a C++ struct
  // with an explicit ctor and explicit conversion operator,
  // preserving Pascal's type discipline (an integer variable can
  // NOT silently receive a TSuperRegister value).
  if (accept(Tok::KwType)) {
    auto td = std::make_shared<TyDistinct>();
    td->loc = loc;
    td->underlying = parse_type();
    return td;
  }
  bool packed = false;
  if (accept(Tok::KwPacked)) packed = true;

  switch (cur_.kind) {
    case Tok::KwArray:  return parse_array_type(packed);
    case Tok::KwRecord: return parse_record_type(packed);
    case Tok::KwObject:
      return parse_object_type();
    case Tok::KwClass: {
      // `class' at type position can start either a class declaration
      // (`class[(parent)] ... end') or a metaclass reference
      // (`class of T').  Disambiguate by 1-token lookahead.
      if (peek().kind == Tok::KwOf) {
        Location mloc = cur_.loc;
        advance();  // class
        advance();  // of
        auto tm = std::make_shared<TyMetaclass>();
        tm->loc = mloc;
        tm->class_name = consume_ident("metaclass target");
        return tm;
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
      advance();
      auto ts = std::make_shared<TyString>();
      ts->loc = loc;
      if (accept(Tok::LBrack)) {
        ts->max_length = parse_expr();
        expect(Tok::RBrack, "string length");
      }
      return ts;
    }
    default:
      return parse_simple_type();
  }
}

TypePtr Parser::parse_simple_type() {
  Location loc = cur_.loc;
  // Enum: `( a, b, c )`
  if (accept(Tok::LParen)) {
    auto te = std::make_shared<TyEnum>();
    te->loc = loc;
    while (cur_.kind == Tok::Ident) {
      EnumMember member;
      member.name = cur_.text;
      advance();
      // FPC accepts both `:=` and `=` here.
      if (accept(Tok::Assign) || accept(Tok::Eq)) {
        member.value = parse_expr();
      }
      te->members.push_back(std::move(member));
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "enumeration");
    return te;
  }
  // Either a name (possibly qualified) or a subrange literal-start.
  // Attempt subrange first by trying to parse a bound expression and looking for
  // `..`. But Pascal named types are simple identifiers, so:
  //   TypeName | lo .. hi
  // If the first token is an Ident and the next is NOT `..`, it's a name.
  if (cur_.kind == Tok::Ident) {
    // Could still be subrange like `a .. b` where a is an ident.
    // Look-ahead.
    if (peek().kind == Tok::DotDot) {
      auto sr = std::make_shared<TySubrange>();
      sr->loc = loc;
      sr->lo = parse_subrange_bound();     // will read the ident as primary
      expect(Tok::DotDot, "subrange");
      sr->hi = parse_subrange_bound();
      return sr;
    }
    auto tn = std::make_shared<TyName>();
    tn->loc = loc;
    tn->name = cur_.text;
    advance();
    // Qualified: unit.Type.
    while (accept(Tok::Dot)) {
      if (cur_.kind == Tok::Ident) {
        tn->name += ".";
        tn->name += cur_.text;
        advance();
      } else {
        break;
      }
    }
    return tn;
  }
  // Otherwise, treat as subrange.
  auto sr = std::make_shared<TySubrange>();
  sr->loc = loc;
  sr->lo = parse_subrange_bound();
  expect(Tok::DotDot, "subrange");
  sr->hi = parse_subrange_bound();
  return sr;
}

TypePtr Parser::parse_array_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwArray, "array");
  auto ta = std::make_shared<TyArray>();
  ta->loc = loc;
  ta->is_packed = packed;
  if (accept(Tok::LBrack)) {
    while (!at_end() && !check(Tok::RBrack)) {
      ta->dims.push_back(parse_simple_type());
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RBrack, "array dimensions");
  } else {
    ta->array_kind = ArrayKind::Dynamic;
  }
  expect(Tok::KwOf, "array");
  ta->element = parse_type();
  return ta;
}

TypePtr Parser::parse_record_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwRecord, "record");
  auto tr = std::make_shared<TyRecord>();
  tr->loc = loc;
  tr->is_packed = packed;

  // Plain fields until `end` or `case`.
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwCase)) {
    RecordField f;
    if (cur_.kind != Tok::Ident) break;
    f.names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      f.names.push_back(cur_.text); advance();
    }
    expect(Tok::Colon, "record field");
    f.type = parse_type();
    tr->fields.push_back(std::move(f));
    if (!accept(Tok::Semi)) break;
  }

  if (accept(Tok::KwCase)) {
    tr->has_variant = true;
    if (cur_.kind == Tok::Ident && peek().kind == Tok::Colon) {
      tr->variant_tag_name = cur_.text;
      advance();
      advance();  // ':'
    }
    tr->variant_tag_type = parse_type();
    expect(Tok::KwOf, "variant record");

    while (!at_end() && !check(Tok::KwEnd) && !check(Tok::RParen)) {
      VariantCase vc;
      vc.labels.push_back(parse_expr());
      while (accept(Tok::Comma)) vc.labels.push_back(parse_expr());
      expect(Tok::Colon, "variant case");
      expect(Tok::LParen, "variant case");
      while (!at_end() && !check(Tok::RParen) && !check(Tok::KwCase)) {
        RecordField f;
        if (cur_.kind != Tok::Ident) break;
        f.names.push_back(cur_.text); advance();
        while (accept(Tok::Comma)) {
          if (cur_.kind != Tok::Ident) break;
          f.names.push_back(cur_.text); advance();
        }
        expect(Tok::Colon, "variant field");
        f.type = parse_type();
        vc.fields.push_back(std::move(f));
        if (!accept(Tok::Semi)) break;
      }
      // Nested variant part inside this case body: `case [tag:] T of ...`
      // We flatten nested variant sub-cases: each sub-case's fields become
      // a standalone VariantCase on the outer record. Pascal lets you name
      // the fields directly on the outer record regardless of which case
      // is active, and the emitter already emits one struct-in-union per
      // case -- so flattening preserves access semantics.
      if (accept(Tok::KwCase)) {
        if (cur_.kind == Tok::Ident && peek().kind == Tok::Colon) {
          advance(); advance();
        }
        (void)parse_type();
        expect(Tok::KwOf, "nested variant");
        while (!at_end() && !check(Tok::RParen)) {
          VariantCase sub;
          sub.labels.push_back(parse_expr());
          while (accept(Tok::Comma)) sub.labels.push_back(parse_expr());
          expect(Tok::Colon, "nested variant case");
          expect(Tok::LParen, "nested variant case");
          while (!at_end() && !check(Tok::RParen)) {
            RecordField nf;
            if (cur_.kind != Tok::Ident) break;
            nf.names.push_back(cur_.text); advance();
            while (accept(Tok::Comma)) {
              if (cur_.kind != Tok::Ident) break;
              nf.names.push_back(cur_.text); advance();
            }
            expect(Tok::Colon, "nested variant field");
            nf.type = parse_type();
            sub.fields.push_back(std::move(nf));
            if (!accept(Tok::Semi)) break;
          }
          expect(Tok::RParen, "nested variant case");
          tr->variant_cases.push_back(std::move(sub));
          if (!accept(Tok::Semi)) break;
        }
      }
      expect(Tok::RParen, "variant case");
      tr->variant_cases.push_back(std::move(vc));
      if (!accept(Tok::Semi)) break;
    }
  }

  expect(Tok::KwEnd, "record");
  return tr;
}

TypePtr Parser::parse_object_type() {
  Location loc = cur_.loc;
  // Accept either TP-style `object` or Delphi-style `class`.  The
  // grammar bodies are (almost) identical: inheritance via (parent),
  // public/private/protected sections, field and method declarations.
  // We record which keyword was used on the resulting TyObject so the
  // emitter can pick value-semantics (object) vs reference-semantics
  // (class).
  auto to = std::make_shared<TyObject>();
  if (check(Tok::KwClass)) {
    expect(Tok::KwClass, "class");
    to->is_reference_type = true;
  } else {
    expect(Tok::KwObject, "object");
  }
  to->loc = loc;
  if (accept(Tok::LParen)) {
    to->parent = consume_ident("parent class");
    expect(Tok::RParen, "parent class");
  }
  // Delphi forward class declaration: `T = class;' (body follows in a
  // later type declaration within the same section). A parenthesized
  // ancestor changes the meaning: `T = class(Base);` is a complete empty
  // class declaration, equivalent to `class(Base) end`.
  if (to->is_reference_type && check(Tok::Semi)) {
    if (to->parent.empty()) to->is_forward = true;
    return to;
  }
  Visibility vis = Visibility::Public;
  while (!at_end() && !check(Tok::KwEnd)) {
    // visibility change
    if (is_directive("public")) { advance(); vis = Visibility::Public; continue; }
    if (is_directive("private")) { advance(); vis = Visibility::Private; continue; }
    if (is_directive("protected")) { advance(); vis = Visibility::Protected; continue; }
    if (is_directive("published")) { advance(); vis = Visibility::Public; continue; }

    bool is_class_method = false;
    if (check(Tok::KwClass)) {
      advance();
      if (!(check(Tok::KwProcedure) || check(Tok::KwFunction))) {
        report_error(cur_.loc,
                     "expected `procedure' or `function' after `class'");
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
      ObjectMember m;
      m.vis = vis;
      m.kind = ObjectMemberKind::Method;
      // Method headers inside an object are always signatures only --
      // bodies live in the implementation section (TP 7.0 semantics).
      m.method = parse_proc_decl(pk, /*in_interface=*/true, is_class_method);
      to->members.push_back(std::move(m));
      continue;
    }

    if (is_directive("property")) {
      advance();
      ObjectMember m;
      m.vis = vis;
      m.kind = ObjectMemberKind::Property;
      m.property.name = consume_name_or_directive("property name");
      if (accept(Tok::LBrack)) {
        m.property.params = parse_param_list(Tok::RBrack);
        expect(Tok::RBrack, "property index list");
      }
      expect(Tok::Colon, "property");
      m.property.type = parse_type();
      bool saw_accessor = false;
      while (true) {
        if (m.property.read_name.empty() && is_directive("read")) {
          advance();
          m.property.read_name =
              consume_name_or_directive("property read accessor");
          saw_accessor = true;
          continue;
        }
        if (m.property.write_name.empty() && is_directive("write")) {
          advance();
          m.property.write_name =
              consume_name_or_directive("property write accessor");
          saw_accessor = true;
          continue;
        }
        break;
      }
      if (!saw_accessor) {
        report_error(cur_.loc, "expected property accessor in declaration");
      }
      bool consumed_tail_semi = false;
      while (accept(Tok::Semi)) {
        consumed_tail_semi = true;
        if (is_directive("default")) {
          advance();
          m.property.is_default = true;
          continue;
        }
        break;
      }
      if (!consumed_tail_semi) expect(Tok::Semi, "property");
      to->members.push_back(std::move(m));
      continue;
    }

    // Field
    if (cur_.kind != Tok::Ident) break;
    ObjectMember m;
    m.vis = vis;
    m.kind = ObjectMemberKind::Field;
    m.field_names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident) break;
      m.field_names.push_back(cur_.text); advance();
    }
    expect(Tok::Colon, "object field");
    m.field_type = parse_type();
    to->members.push_back(std::move(m));
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwEnd, "object");
  return to;
}

TypePtr Parser::parse_set_type() {
  Location loc = cur_.loc;
  expect(Tok::KwSet, "set");
  expect(Tok::KwOf, "set");
  auto ts = std::make_shared<TySet>();
  ts->loc = loc;
  ts->element = parse_simple_type();
  return ts;
}

TypePtr Parser::parse_file_type() {
  Location loc = cur_.loc;
  expect(Tok::KwFile, "file");
  auto tf = std::make_shared<TyFile>();
  tf->loc = loc;
  if (accept(Tok::KwOf)) tf->element = parse_type();
  return tf;
}

TypePtr Parser::parse_procedural_type() {
  Location loc = cur_.loc;
  auto tp = std::make_shared<TyProcedural>();
  tp->loc = loc;
  tp->is_function = check(Tok::KwFunction);
  advance();  // procedure/function
  if (accept(Tok::LParen)) {
    tp->params = parse_formal_param_list();
    expect(Tok::RParen, "procedural type");
  }
  if (tp->is_function) {
    expect(Tok::Colon, "function type");
    tp->return_type = parse_type();
  }
  if (accept(Tok::KwOf)) {
    expect(Tok::KwObject, "procedural type");
    tp->is_method = true;
  }
  // Optional calling-convention modifier (no `;` required in type position).
  while (is_directive("cdecl") || is_directive("pascal") ||
         is_directive("stdcall")) {
    if (is_directive("cdecl")) tp->is_cdecl = true;
    advance();
  }
  return tp;
}

// ---------------------------------------------------------------------------
// Statements

StmtPtr Parser::parse_compound_statement() {
  Location loc = cur_.loc;
  expect(Tok::KwBegin, "compound statement");
  auto c = std::make_shared<Compound>();
  c->loc = loc;
  while (!at_end() && !check(Tok::KwEnd)) {
    auto s = parse_statement();
    if (s) c->body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwEnd, "compound statement");
  return c;
}

StmtPtr Parser::parse_statement() {
  // Labeled statement: LabelId `:` Stmt
  if (cur_.kind == Tok::Ident && peek().kind == Tok::Colon) {
    // But only if the identifier was declared as a label -- we can't know
    // here. Heuristic: if the next-next token looks like a statement start,
    // treat as labeled. Otherwise fall through (it'll be an assignment or
    // call with an ident LHS).
    // Fpc sources use labels like `exit_label:` before a statement -- safe
    // to always treat ident-colon as label when the identifier is
    // short/uppercased; but simpler: always treat as label here, then parse
    // another statement.
    // This introduces ambiguity only in declarations which don't reach us.
    // Keep it permissive: treat as labeled.
    std::string lab = cur_.text;
    Location loc = cur_.loc;
    // Peek further: a labeled statement in Pascal must be followed by a
    // full statement, not be a type ascription. In statement position we
    // are never ascribing, so the following heuristic is safe.
    advance();
    advance();  // ':'
    auto lbl = std::make_shared<Labeled>();
    lbl->loc = loc;
    lbl->label = lab;
    lbl->body = parse_statement();
    return lbl;
  }
  if (cur_.kind == Tok::IntLit && peek().kind == Tok::Colon) {
    std::string lab = cur_.text;
    Location loc = cur_.loc;
    advance(); advance();
    auto lbl = std::make_shared<Labeled>();
    lbl->loc = loc;
    lbl->label = lab;
    lbl->body = parse_statement();
    return lbl;
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
      auto g = std::make_shared<Goto>(); g->loc = cur_.loc; advance();
      if (cur_.kind == Tok::Ident || cur_.kind == Tok::IntLit) {
        g->label = cur_.text; advance();
      } else {
        expect(Tok::Ident, "goto target");
      }
      return g;
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
      auto e = std::make_shared<EmptyStmt>(); e->loc = cur_.loc;
      return e;
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
    auto a = std::make_shared<Assign>();
    a->loc = loc;
    a->target = std::move(lhs);
    a->value = parse_expr();
    return a;
  }
  auto es = std::make_shared<ExprStmt>();
  es->loc = loc;
  es->expr = std::move(lhs);
  return es;
}

StmtPtr Parser::parse_if() {
  Location loc = cur_.loc;
  expect(Tok::KwIf, "if");
  auto n = std::make_shared<If>();
  n->loc = loc;
  n->cond = parse_expr();
  expect(Tok::KwThen, "if");
  n->then_branch = parse_statement();
  if (accept(Tok::KwElse)) n->else_branch = parse_statement();
  return n;
}

StmtPtr Parser::parse_while() {
  Location loc = cur_.loc;
  expect(Tok::KwWhile, "while");
  auto n = std::make_shared<While>();
  n->loc = loc;
  n->cond = parse_expr();
  expect(Tok::KwDo, "while");
  n->body = parse_statement();
  return n;
}

StmtPtr Parser::parse_repeat() {
  Location loc = cur_.loc;
  expect(Tok::KwRepeat, "repeat");
  auto n = std::make_shared<Repeat>();
  n->loc = loc;
  while (!at_end() && !check(Tok::KwUntil)) {
    auto s = parse_statement();
    if (s) n->body.push_back(std::move(s));
    if (!accept(Tok::Semi)) break;
  }
  expect(Tok::KwUntil, "repeat");
  n->cond = parse_expr();
  return n;
}

StmtPtr Parser::parse_for() {
  Location loc = cur_.loc;
  expect(Tok::KwFor, "for");
  auto n = std::make_shared<For>();
  n->loc = loc;
  n->var = consume_ident("for variable");
  expect(Tok::Assign, "for");
  n->from = parse_expr();
  if (accept(Tok::KwTo)) n->downto = false;
  else if (accept(Tok::KwDownto)) n->downto = true;
  else { expect(Tok::KwTo, "for"); }
  n->to = parse_expr();
  expect(Tok::KwDo, "for");
  n->body = parse_statement();
  return n;
}

StmtPtr Parser::parse_case() {
  Location loc = cur_.loc;
  expect(Tok::KwCase, "case");
  auto n = std::make_shared<CaseStmt>();
  n->loc = loc;
  n->selector = parse_expr();
  expect(Tok::KwOf, "case");
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwElse) &&
         !check(Tok::KwOtherwise)) {
    CaseArm arm;
    arm.labels.push_back(parse_expr());
    if (accept(Tok::DotDot)) {
      // Rewrite as Range on last label.
      auto r = std::make_shared<Range>();
      r->loc = arm.labels.back()->loc;
      r->lo = std::move(arm.labels.back());
      arm.labels.pop_back();
      r->hi = parse_expr();
      arm.labels.push_back(std::move(r));
    }
    while (accept(Tok::Comma)) {
      auto lab = parse_expr();
      if (accept(Tok::DotDot)) {
        auto r = std::make_shared<Range>();
        r->loc = lab->loc;
        r->lo = std::move(lab);
        r->hi = parse_expr();
        lab = std::move(r);
      }
      arm.labels.push_back(std::move(lab));
    }
    expect(Tok::Colon, "case arm");
    arm.body = parse_statement();
    n->arms.push_back(std::move(arm));
    if (!accept(Tok::Semi)) break;
  }
  if (accept(Tok::KwElse) || accept(Tok::KwOtherwise)) {
    // Body is a statement sequence up to end.
    auto c = std::make_shared<Compound>();
    c->loc = cur_.loc;
    while (!at_end() && !check(Tok::KwEnd)) {
      auto s = parse_statement();
      if (s) c->body.push_back(std::move(s));
      if (!accept(Tok::Semi)) break;
    }
    n->else_branch = std::move(c);
  }
  expect(Tok::KwEnd, "case");
  return n;
}

StmtPtr Parser::parse_with() {
  Location loc = cur_.loc;
  expect(Tok::KwWith, "with");
  auto n = std::make_shared<With>();
  n->loc = loc;
  n->exprs.push_back(parse_expr());
  while (accept(Tok::Comma)) n->exprs.push_back(parse_expr());
  expect(Tok::KwDo, "with");
  n->body = parse_statement();
  return n;
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
  auto n = std::make_shared<Try>();
  n->loc = loc;
  while (!at_end() && !check(Tok::KwExcept) && !check(Tok::KwFinally)) {
    n->body.push_back(parse_statement());
    if (!accept(Tok::Semi)) break;
  }
  if (accept(Tok::KwFinally)) {
    n->is_finally = true;
    while (!at_end() && !check(Tok::KwEnd)) {
      n->finally_body.push_back(parse_statement());
      if (!accept(Tok::Semi)) break;
    }
  } else if (accept(Tok::KwExcept)) {
    n->is_finally = false;
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
        ExceptHandler h;
        // `on Ident : ClassName do' OR `on ClassName do' (bind omitted).
        // Disambiguate by 1-token lookahead for `:'.
        std::string first = consume_ident("exception handler");
        if (accept(Tok::Colon)) {
          h.var_name = first;
          h.class_name = consume_ident("exception class");
        } else {
          h.class_name = first;
        }
        expect(Tok::KwDo, "exception handler");
        h.body = parse_statement();
        n->handlers.push_back(std::move(h));
        accept(Tok::Semi);
      }
      // Optional else-branch after `on' arms: catch-all.
      if (accept(Tok::KwElse)) {
        auto compound = std::make_shared<Compound>();
        compound->loc = cur_.loc;
        while (!at_end() && !check(Tok::KwEnd)) {
          compound->body.push_back(parse_statement());
          if (!accept(Tok::Semi)) break;
        }
        n->except_else = std::move(compound);
      }
    } else {
      // Catch-all shorthand: any statement list here runs on any
      // exception.  Model as the else-branch of an handlerless except.
      auto compound = std::make_shared<Compound>();
      compound->loc = cur_.loc;
      while (!at_end() && !check(Tok::KwEnd)) {
        compound->body.push_back(parse_statement());
        if (!accept(Tok::Semi)) break;
      }
      n->except_else = std::move(compound);
    }
  } else {
    expect(Tok::KwExcept, "try");  // reports a useful error
  }
  expect(Tok::KwEnd, "try");
  return n;
}

// `raise [Expr] [at Expr]' -- raise or re-raise an exception.
// Bare `raise;' inside an except handler re-raises the current one.
StmtPtr Parser::parse_raise() {
  Location loc = cur_.loc;
  expect(Tok::KwRaise, "raise");
  auto n = std::make_shared<Raise>();
  n->loc = loc;
  // Optional expression -- the instance being raised.  Absent iff the
  // next token ends the statement (`;', `end', `else', ...).
  if (!check(Tok::Semi) && !check(Tok::KwEnd) && !check(Tok::KwElse)
      && !check(Tok::KwUntil)) {
    n->value = parse_expr();
  }
  // Optional `at <Expr>' suffix (Delphi debug aid); parsed and discarded.
  if (is_directive("at")) {
    advance();
    parse_expr();
  }
  return n;
}

StmtPtr Parser::parse_asm() {
  Location loc = cur_.loc;
  expect(Tok::KwAsm, "asm");
  auto n = std::make_shared<AsmStmt>();
  n->loc = loc;
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
  return n;
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
    BinOp op;
    switch (cur_.kind) {
      case Tok::Eq:    op = BinOp::Eq; break;
      case Tok::NotEq: op = BinOp::NotEq; break;
      case Tok::Lt:    op = BinOp::Lt; break;
      case Tok::Gt:    op = BinOp::Gt; break;
      case Tok::LtEq:  op = BinOp::LtEq; break;
      case Tok::GtEq:  op = BinOp::GtEq; break;
      case Tok::KwIn:  op = BinOp::In; break;
      case Tok::KwIs:  op = BinOp::Is; break;
      case Tok::KwAs:  op = BinOp::As; break;
      default: return lhs;
    }
    Location loc = cur_.loc; advance();
    auto rhs = parse_simple_expr();
    auto b = std::make_shared<Binary>();
    b->loc = loc; b->op = op;
    b->lhs = std::move(lhs); b->rhs = std::move(rhs);
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
    BinOp op;
    switch (cur_.kind) {
      case Tok::Plus:  op = BinOp::Add; break;
      case Tok::Minus: op = BinOp::Sub; break;
      case Tok::KwOr:  op = BinOp::Or;  break;
      case Tok::KwXor: op = BinOp::Xor; break;
      case Tok::SymDiff: op = BinOp::SymDiff; break;
      default: return lhs;
    }
    Location loc = cur_.loc; advance();
    auto rhs = parse_term();
    auto b = std::make_shared<Binary>();
    b->loc = loc; b->op = op;
    b->lhs = std::move(lhs); b->rhs = std::move(rhs);
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_term() {
  auto lhs = parse_factor();
  for (;;) {
    BinOp op;
    switch (cur_.kind) {
      case Tok::Star:  op = BinOp::Mul; break;
      case Tok::Slash: op = BinOp::RealDiv; break;
      case Tok::KwDiv: op = BinOp::IntDiv; break;
      case Tok::KwMod: op = BinOp::Mod; break;
      case Tok::KwAnd: op = BinOp::And; break;
      case Tok::KwShl: op = BinOp::Shl; break;
      case Tok::KwShr: op = BinOp::Shr; break;
      default: return lhs;
    }
    Location loc = cur_.loc; advance();
    auto rhs = parse_factor();
    auto b = std::make_shared<Binary>();
    b->loc = loc; b->op = op;
    b->lhs = std::move(lhs); b->rhs = std::move(rhs);
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_factor() {
  if (check(Tok::Plus) || check(Tok::Minus)) {
    UnOp op = check(Tok::Minus) ? UnOp::Neg : UnOp::Plus;
    Location loc = cur_.loc; advance();
    auto u = std::make_shared<Unary>();
    u->loc = loc; u->op = op;
    u->operand = parse_factor();
    return u;
  }
  if (check(Tok::KwNot)) {
    Location loc = cur_.loc; advance();
    auto u = std::make_shared<Unary>();
    u->loc = loc; u->op = UnOp::Not;
    u->operand = parse_factor();
    return u;
  }
  if (check(Tok::At) || check(Tok::AtAt)) {
    Location loc = cur_.loc;
    bool dbl = (cur_.kind == Tok::AtAt);
    advance();
    auto a = std::make_shared<AddrOf>();
    a->loc = loc; a->double_addr = dbl;
    a->operand = parse_factor();
    return a;
  }
  return parse_primary();
}

ExprPtr Parser::parse_primary() {
  Location loc = cur_.loc;
  switch (cur_.kind) {
    case Tok::IntLit: {
      auto n = std::make_shared<IntLit>();
      n->loc = loc; n->value = cur_.int_value; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::RealLit: {
      auto n = std::make_shared<RealLit>();
      n->loc = loc; n->text = cur_.text; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::StringLit: {
      auto n = std::make_shared<StringLit>();
      n->loc = loc; n->value = cur_.text; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwNil: {
      auto n = std::make_shared<NilLit>();
      n->loc = loc; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      auto n = std::make_shared<BoolLit>();
      n->loc = loc; n->value = (cur_.kind == Tok::KwTrue); advance();
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
      auto base = std::make_shared<Ident>();
      base->loc = loc; base->name = "inherited";
      if (cur_.kind == Tok::Ident) {
        auto m = std::make_shared<Member>();
        m->loc = loc;
        m->base = std::move(base);
        m->name = cur_.text;
        advance();
        return parse_postfix(std::move(m));
      }
      return parse_postfix(std::move(base));
    }
    case Tok::Ident:
    case Tok::KwSelf: {
      auto id = std::make_shared<Ident>();
      id->loc = loc; id->name = cur_.text; advance();
      return parse_postfix(std::move(id));
    }
    default: {
      report_error(loc,
                   "expected expression, got '" + cur_.text + "'");
      auto n = std::make_shared<IntLit>();
      n->loc = loc;
      advance();
      return n;
    }
  }
}

ExprPtr Parser::parse_postfix(ExprPtr lhs) {
  for (;;) {
    Location loc = cur_.loc;
    if (accept(Tok::Dot)) {
      auto m = std::make_shared<Member>();
      m->loc = loc;
      m->base = std::move(lhs);
      m->name = consume_name_or_directive("member name");
      lhs = std::move(m);
      continue;
    }
    if (accept(Tok::Caret)) {
      auto d = std::make_shared<Deref>();
      d->loc = loc; d->operand = std::move(lhs);
      lhs = std::move(d);
      continue;
    }
    if (accept(Tok::LBrack)) {
      auto ix = std::make_shared<Index>();
      ix->loc = loc; ix->base = std::move(lhs);
      ix->indices.push_back(parse_expr());
      while (accept(Tok::Comma)) ix->indices.push_back(parse_expr());
      expect(Tok::RBrack, "index");
      lhs = std::move(ix);
      continue;
    }
    if (accept(Tok::LParen)) {
      auto c = std::make_shared<Call>();
      c->loc = loc; c->callee = std::move(lhs);
      while (!at_end() && !check(Tok::RParen)) {
        auto arg = parse_expr();
        ExprPtr w, p;
        if (accept(Tok::Colon)) {
          w = parse_expr();
          if (accept(Tok::Colon)) p = parse_expr();
        }
        c->args.push_back(std::move(arg));
        c->width.push_back(std::move(w));
        c->precision.push_back(std::move(p));
        if (!accept(Tok::Comma)) break;
      }
      expect(Tok::RParen, "call");
      lhs = std::move(c);
      continue;
    }
    return lhs;
  }
}

ExprPtr Parser::parse_set_literal() {
  Location loc = cur_.loc;
  expect(Tok::LBrack, "set literal");
  auto s = std::make_shared<SetLit>();
  s->loc = loc;
  while (!at_end() && !check(Tok::RBrack)) {
    auto lo = parse_expr();
    if (accept(Tok::DotDot)) {
      auto r = std::make_shared<Range>();
      r->loc = lo->loc;
      r->lo = std::move(lo);
      r->hi = parse_expr();
      s->elements.push_back(std::move(r));
    } else {
      s->elements.push_back(std::move(lo));
    }
    if (!accept(Tok::Comma)) break;
  }
  expect(Tok::RBrack, "set literal");
  return s;
}

}  // namespace tp2cc
