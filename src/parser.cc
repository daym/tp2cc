#include "parser.h"

#include <initializer_list>
#include <utility>

#include "diag.h"

namespace p2cc {

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

// Keywords that are directives in Pascal -- they have keyword status in
// certain positions but can still be used as ordinary identifiers (e.g. as
// field or member names). Consulted only by consume_name_or_directive.
bool Parser::tok_is_directive_kw() const {
  switch (cur_.kind) {
    case Tok::KwAbsolute:
    case Tok::KwAbstract:
    case Tok::KwAssembler:
    case Tok::KwCdecl:
    case Tok::KwDynamic:
    case Tok::KwExport:
    case Tok::KwExternal:
    case Tok::KwFar:
    case Tok::KwForward:
    case Tok::KwInline:
    case Tok::KwInterrupt:
    case Tok::KwNear:
    case Tok::KwOverride:
    case Tok::KwPascal:
    case Tok::KwPopstack:
    case Tok::KwPrivate:
    case Tok::KwProtected:
    case Tok::KwPublic:
    case Tok::KwPublished:
    case Tok::KwRegister:
    case Tok::KwResident:
    case Tok::KwSafecall:
    case Tok::KwStatic:
    case Tok::KwStdcall:
    case Tok::KwVirtual:
      return true;
    default:
      return false;
  }
}

std::string Parser::consume_name_or_directive(const char* ctx) {
  if (cur_.kind == Tok::Ident || tok_is_directive_kw()) {
    std::string s = cur_.text;
    advance();
    return s;
  }
  expect(Tok::Ident, ctx);
  return {};
}

// ---------------------------------------------------------------------------
// Entry

std::unique_ptr<UnitNode> Parser::parse() {
  if (cur_.kind == Tok::KwUnit) return parse_unit();
  if (cur_.kind == Tok::KwProgram) return parse_program();
  // Some sources are `.inc` files or program-body-only; treat as program
  // without a header.
  if (cur_.kind == Tok::KwBegin) {
    auto u = std::make_unique<UnitNode>();
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

std::unique_ptr<UnitNode> Parser::parse_program() {
  auto u = std::make_unique<UnitNode>();
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

std::unique_ptr<UnitNode> Parser::parse_unit() {
  auto u = std::make_unique<UnitNode>();
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
  } else if (check(Tok::KwInitialization)) {
    // Treat initialization/finalization sections as a compound init body.
    advance();
    auto c = std::make_unique<Compound>();
    c->loc = cur_.loc;
    while (!at_end() && !check(Tok::KwFinalization) && !check(Tok::KwEnd)) {
      auto s = parse_statement();
      if (s) c->body.push_back(std::move(s));
      if (!accept(Tok::Semi)) break;
    }
    if (accept(Tok::KwFinalization)) {
      // Discard for now.
      while (!at_end() && !check(Tok::KwEnd)) advance();
    }
    u->init_body = std::move(c);
    expect(Tok::KwEnd, "unit tail");
    expect(Tok::Dot, "unit tail");
  } else {
    expect(Tok::KwEnd, "unit tail");
  }
  return u;
}

void Parser::parse_uses_into(std::vector<std::string>& out) {
  while (!at_end()) {
    if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) {
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
        auto d = parse_proc_decl(ProcKind::Procedure, in_interface);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwFunction: {
        auto d = parse_proc_decl(ProcKind::Function, in_interface);
        if (d) out.push_back(std::move(d));
        break;
      }
      case Tok::KwConstructor:
      case Tok::KwDestructor: {
        auto pk = check(Tok::KwConstructor) ? ProcKind::Constructor
                                            : ProcKind::Destructor;
        auto d = parse_proc_decl(pk, in_interface);
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
    auto cd = std::make_unique<ConstDecl>();
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
  bool is_record =
      (cur_.kind == Tok::Ident || tok_is_directive_kw()) &&
      peek().kind == Tok::Colon;

  if (is_record) {
    auto rc = std::make_unique<RecordConst>();
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
    auto ac = std::make_unique<ArrayConst>();
    ac->loc = loc;
    advance();
    return ac;
  }
  auto first = parse_const_value();
  if (accept(Tok::Comma)) {
    auto ac = std::make_unique<ArrayConst>();
    ac->loc = loc;
    ac->elements.push_back(std::move(first));
    ac->elements.push_back(parse_const_value());
    while (accept(Tok::Comma)) ac->elements.push_back(parse_const_value());
    expect(Tok::RParen, "array constant");
    return ac;
  }
  // Single item inside parens -- treat as a 1-element array constant to
  // preserve the array shape (typed context), unless it's clearly just a
  // parenthesised scalar. Pascal treats `(x)` as both; we conservatively
  // wrap it as ArrayConst only if the outer type was declared as array-like,
  // which we can't check here without the type. Wrap as single-element
  // ArrayConst when the caller is clearly expecting one.
  expect(Tok::RParen, "parenthesised constant");
  auto ac = std::make_unique<ArrayConst>();
  ac->loc = loc;
  ac->elements.push_back(std::move(first));
  return ac;
}

void Parser::parse_type_section(std::vector<DeclPtr>& out) {
  expect(Tok::KwType, "type section");
  while (check(Tok::Ident)) {
    auto td = std::make_unique<TypeDecl>();
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
    auto vd = std::make_unique<VarDecl>();
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
    if (accept(Tok::KwAbsolute)) {
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
    } else if (accept(Tok::KwExternal)) {
      vd->external_name = nullptr;
      if (cur_.kind == Tok::StringLit) {
        vd->external_lib = cur_.text;
        advance();
      }
      if (is_directive("name")) {
        advance();
        if (cur_.kind == Tok::StringLit) {
          auto s = std::make_unique<StringLit>();
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
  auto ld = std::make_unique<LabelDecl>();
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
  // Pascal "directives" -- position-dependent keywords that follow a routine
  // header, separated by `;`. Some are true reserved words; others are
  // directives spelled as ordinary identifiers (name/alias/...).
  for (;;) {
    if (cur_.kind == Tok::KwVirtual) { pd.is_virtual = true; advance(); }
    else if (cur_.kind == Tok::KwAbstract) { pd.is_abstract = true; advance(); }
    else if (cur_.kind == Tok::KwOverride) { pd.is_override = true; advance(); }
    else if (cur_.kind == Tok::KwForward) { pd.is_forward = true; advance(); }
    else if (cur_.kind == Tok::KwInline) { pd.is_inline = true; advance(); }
    else if (cur_.kind == Tok::KwCdecl) { pd.is_cdecl = true; advance(); }
    else if (cur_.kind == Tok::KwAssembler) { pd.is_assembler = true; advance(); }
    else if (cur_.kind == Tok::KwFar) { advance(); }
    else if (cur_.kind == Tok::KwNear) { advance(); }
    else if (cur_.kind == Tok::KwPascal) { advance(); }
    else if (cur_.kind == Tok::KwRegister) { advance(); }
    else if (cur_.kind == Tok::KwStdcall) { advance(); }
    else if (cur_.kind == Tok::KwSafecall) { advance(); }
    else if (cur_.kind == Tok::KwInterrupt) { advance(); }
    else if (cur_.kind == Tok::KwPopstack) { advance(); }
    else if (cur_.kind == Tok::KwExport) { advance(); }
    else if (cur_.kind == Tok::KwPublic) { advance(); }
    else if (cur_.kind == Tok::KwStatic) { advance(); }
    else if (cur_.kind == Tok::KwExternal) {
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
    else {
      return;
    }
    // Semicolon between directives is conventional but optional in fpc --
    // e.g. `: THandle;stdcall external 'x' name 'y';`. Consume one if
    // present and continue.
    accept(Tok::Semi);
  }
}

DeclPtr Parser::parse_proc_decl(ProcKind pk, bool in_interface) {
  auto pd = std::make_unique<ProcDecl>();
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

std::vector<Param> Parser::parse_formal_param_list() {
  std::vector<Param> out;
  while (!at_end() && !check(Tok::RParen)) {
    Param p;
    if (accept(Tok::KwVar)) p.mode = Param::Var;
    else if (accept(Tok::KwConst)) p.mode = Param::Const;
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
        auto ta = std::make_unique<TyArray>();
        ta->loc = cur_.loc;
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

// ---------------------------------------------------------------------------
// Types

static bool starts_type_tok(Tok t) {
  switch (t) {
    case Tok::Ident:
    case Tok::Caret:
    case Tok::KwArray:
    case Tok::KwRecord:
    case Tok::KwObject:
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
    auto tp = std::make_unique<TyPointer>();
    tp->loc = loc;
    tp->target = parse_type();
    return tp;
  }
  bool packed = false;
  if (accept(Tok::KwPacked)) packed = true;

  switch (cur_.kind) {
    case Tok::KwArray:  return parse_array_type(packed);
    case Tok::KwRecord: return parse_record_type(packed);
    case Tok::KwObject: return parse_object_type();
    case Tok::KwSet:    return parse_set_type();
    case Tok::KwFile:   return parse_file_type();
    case Tok::KwProcedure:
    case Tok::KwFunction:
      return parse_procedural_type();
    case Tok::KwString:
    case Tok::KwShortstring: {
      advance();
      auto ts = std::make_unique<TyString>();
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
    auto te = std::make_unique<TyEnum>();
    te->loc = loc;
    while (cur_.kind == Tok::Ident) {
      te->members.push_back(cur_.text);
      advance();
      // Enums with explicit values: `a = 1` -- swallow the value if present.
      if (accept(Tok::Eq)) (void)parse_expr();
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "enumeration");
    return te;
  }
  // Either a name (possibly qualified) or a subrange literal-start.
  // Attempt subrange first by trying to parse an expression and looking for
  // `..`. But Pascal named types are simple identifiers, so:
  //   TypeName | lo .. hi
  // If the first token is an Ident and the next is NOT `..`, it's a name.
  if (cur_.kind == Tok::Ident) {
    // Could still be subrange like `a .. b` where a is an ident.
    // Look-ahead.
    if (peek().kind == Tok::DotDot) {
      auto sr = std::make_unique<TySubrange>();
      sr->loc = loc;
      sr->lo = parse_expr();     // will read the ident as primary
      expect(Tok::DotDot, "subrange");
      sr->hi = parse_expr();
      return sr;
    }
    auto tn = std::make_unique<TyName>();
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
  auto sr = std::make_unique<TySubrange>();
  sr->loc = loc;
  sr->lo = parse_expr();
  expect(Tok::DotDot, "subrange");
  sr->hi = parse_expr();
  return sr;
}

TypePtr Parser::parse_array_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwArray, "array");
  auto ta = std::make_unique<TyArray>();
  ta->loc = loc;
  ta->is_packed = packed;
  if (accept(Tok::LBrack)) {
    while (!at_end() && !check(Tok::RBrack)) {
      ta->dims.push_back(parse_simple_type());
      if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RBrack, "array dimensions");
  }
  expect(Tok::KwOf, "array");
  ta->element = parse_type();
  return ta;
}

TypePtr Parser::parse_record_type(bool packed) {
  Location loc = cur_.loc;
  expect(Tok::KwRecord, "record");
  auto tr = std::make_unique<TyRecord>();
  tr->loc = loc;
  tr->is_packed = packed;

  // Plain fields until `end` or `case`.
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwCase)) {
    RecordField f;
    if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
    f.names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
      f.names.push_back(cur_.text); advance();
    }
    expect(Tok::Colon, "record field");
    f.type = parse_type();
    tr->fields.push_back(std::move(f));
    if (!accept(Tok::Semi)) break;
  }

  if (accept(Tok::KwCase)) {
    tr->has_variant = true;
    if ((cur_.kind == Tok::Ident || tok_is_directive_kw()) &&
        peek().kind == Tok::Colon) {
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
        if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
        f.names.push_back(cur_.text); advance();
        while (accept(Tok::Comma)) {
          if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
          f.names.push_back(cur_.text); advance();
        }
        expect(Tok::Colon, "variant field");
        f.type = parse_type();
        vc.fields.push_back(std::move(f));
        if (!accept(Tok::Semi)) break;
      }
      // Nested variant part inside this case body: `case [tag:] T of ...`
      if (accept(Tok::KwCase)) {
        // We model nested variants structurally by parsing through them and
        // ignoring their sub-cases (they don't affect the shape we need for
        // codegen at this stage -- they're rare and appear only in the
        // compiler's own cpu-base records). Parse them to advance the
        // token stream correctly.
        if ((cur_.kind == Tok::Ident || tok_is_directive_kw()) &&
            peek().kind == Tok::Colon) {
          advance(); advance();
        }
        (void)parse_type();
        expect(Tok::KwOf, "nested variant");
        while (!at_end() && !check(Tok::RParen)) {
          (void)parse_expr();
          while (accept(Tok::Comma)) (void)parse_expr();
          expect(Tok::Colon, "nested variant case");
          expect(Tok::LParen, "nested variant case");
          while (!at_end() && !check(Tok::RParen)) {
            if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
            advance();
            while (accept(Tok::Comma)) {
              if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
              advance();
            }
            expect(Tok::Colon, "nested variant field");
            (void)parse_type();
            if (!accept(Tok::Semi)) break;
          }
          expect(Tok::RParen, "nested variant case");
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
  expect(Tok::KwObject, "object");
  auto to = std::make_unique<TyObject>();
  to->loc = loc;
  if (accept(Tok::LParen)) {
    to->parent = consume_ident("parent class");
    expect(Tok::RParen, "parent class");
  }
  Visibility vis = Visibility::Public;
  while (!at_end() && !check(Tok::KwEnd)) {
    // visibility change
    if (accept(Tok::KwPublic)) { vis = Visibility::Public; continue; }
    if (accept(Tok::KwPrivate)) { vis = Visibility::Private; continue; }
    if (accept(Tok::KwProtected)) { vis = Visibility::Protected; continue; }

    if (check(Tok::KwProcedure) || check(Tok::KwFunction) ||
        check(Tok::KwConstructor) || check(Tok::KwDestructor)) {
      ProcKind pk = ProcKind::Procedure;
      if (check(Tok::KwFunction)) pk = ProcKind::Function;
      else if (check(Tok::KwConstructor)) pk = ProcKind::Constructor;
      else if (check(Tok::KwDestructor)) pk = ProcKind::Destructor;
      ObjectMember m;
      m.vis = vis;
      m.is_field = false;
      // Method headers inside an object are always signatures only --
      // bodies live in the implementation section (TP 7.0 semantics).
      m.method = parse_proc_decl(pk, /*in_interface=*/true);
      to->members.push_back(std::move(m));
      continue;
    }

    // Field
    if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
    ObjectMember m;
    m.vis = vis;
    m.is_field = true;
    m.field_names.push_back(cur_.text); advance();
    while (accept(Tok::Comma)) {
      if (cur_.kind != Tok::Ident && !tok_is_directive_kw()) break;
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
  auto ts = std::make_unique<TySet>();
  ts->loc = loc;
  ts->element = parse_simple_type();
  return ts;
}

TypePtr Parser::parse_file_type() {
  Location loc = cur_.loc;
  expect(Tok::KwFile, "file");
  auto tf = std::make_unique<TyFile>();
  tf->loc = loc;
  if (accept(Tok::KwOf)) tf->element = parse_type();
  return tf;
}

TypePtr Parser::parse_procedural_type() {
  Location loc = cur_.loc;
  auto tp = std::make_unique<TyProcedural>();
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
  // Optional calling-convention modifier (no `;` required in type position).
  while (check(Tok::KwCdecl) || check(Tok::KwPascal) || check(Tok::KwStdcall)) {
    if (check(Tok::KwCdecl)) tp->is_cdecl = true;
    advance();
  }
  return tp;
}

// ---------------------------------------------------------------------------
// Statements

StmtPtr Parser::parse_compound_statement() {
  Location loc = cur_.loc;
  expect(Tok::KwBegin, "compound statement");
  auto c = std::make_unique<Compound>();
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
    auto lbl = std::make_unique<Labeled>();
    lbl->loc = loc;
    lbl->label = lab;
    lbl->body = parse_statement();
    return lbl;
  }
  if (cur_.kind == Tok::IntLit && peek().kind == Tok::Colon) {
    std::string lab = cur_.text;
    Location loc = cur_.loc;
    advance(); advance();
    auto lbl = std::make_unique<Labeled>();
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
      auto g = std::make_unique<Goto>(); g->loc = cur_.loc; advance();
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
    case Tok::Semi:
    case Tok::KwEnd:
    case Tok::KwUntil:
    case Tok::KwElse: {
      // Empty statement.
      auto e = std::make_unique<EmptyStmt>(); e->loc = cur_.loc;
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
    auto a = std::make_unique<Assign>();
    a->loc = loc;
    a->target = std::move(lhs);
    a->value = parse_expr();
    return a;
  }
  auto es = std::make_unique<ExprStmt>();
  es->loc = loc;
  es->expr = std::move(lhs);
  return es;
}

StmtPtr Parser::parse_if() {
  Location loc = cur_.loc;
  expect(Tok::KwIf, "if");
  auto n = std::make_unique<If>();
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
  auto n = std::make_unique<While>();
  n->loc = loc;
  n->cond = parse_expr();
  expect(Tok::KwDo, "while");
  n->body = parse_statement();
  return n;
}

StmtPtr Parser::parse_repeat() {
  Location loc = cur_.loc;
  expect(Tok::KwRepeat, "repeat");
  auto n = std::make_unique<Repeat>();
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
  auto n = std::make_unique<For>();
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
  auto n = std::make_unique<CaseStmt>();
  n->loc = loc;
  n->selector = parse_expr();
  expect(Tok::KwOf, "case");
  while (!at_end() && !check(Tok::KwEnd) && !check(Tok::KwElse) &&
         !check(Tok::KwOtherwise)) {
    CaseArm arm;
    arm.labels.push_back(parse_expr());
    if (accept(Tok::DotDot)) {
      // Rewrite as Range on last label.
      auto r = std::make_unique<Range>();
      r->loc = arm.labels.back()->loc;
      r->lo = std::move(arm.labels.back());
      arm.labels.pop_back();
      r->hi = parse_expr();
      arm.labels.push_back(std::move(r));
    }
    while (accept(Tok::Comma)) {
      auto lab = parse_expr();
      if (accept(Tok::DotDot)) {
        auto r = std::make_unique<Range>();
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
    auto c = std::make_unique<Compound>();
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
  auto n = std::make_unique<With>();
  n->loc = loc;
  n->exprs.push_back(parse_expr());
  while (accept(Tok::Comma)) n->exprs.push_back(parse_expr());
  expect(Tok::KwDo, "with");
  n->body = parse_statement();
  return n;
}

StmtPtr Parser::parse_asm() {
  Location loc = cur_.loc;
  expect(Tok::KwAsm, "asm");
  auto n = std::make_unique<AsmStmt>();
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
//   simple_expr  := sign? term ( addop term )*
//   term         := factor ( mulop factor )*
//   factor       := NOT factor | unary-primary
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
    auto b = std::make_unique<Binary>();
    b->loc = loc; b->op = op;
    b->lhs = std::move(lhs); b->rhs = std::move(rhs);
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_simple_expr() {
  ExprPtr lhs;
  // Optional sign
  if (check(Tok::Plus) || check(Tok::Minus)) {
    UnOp op = check(Tok::Minus) ? UnOp::Neg : UnOp::Plus;
    Location loc = cur_.loc; advance();
    auto u = std::make_unique<Unary>();
    u->loc = loc; u->op = op;
    u->operand = parse_term();
    lhs = std::move(u);
  } else {
    lhs = parse_term();
  }
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
    auto b = std::make_unique<Binary>();
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
    auto b = std::make_unique<Binary>();
    b->loc = loc; b->op = op;
    b->lhs = std::move(lhs); b->rhs = std::move(rhs);
    lhs = std::move(b);
  }
}

ExprPtr Parser::parse_factor() {
  if (check(Tok::KwNot)) {
    Location loc = cur_.loc; advance();
    auto u = std::make_unique<Unary>();
    u->loc = loc; u->op = UnOp::Not;
    u->operand = parse_factor();
    return u;
  }
  if (check(Tok::At) || check(Tok::AtAt)) {
    Location loc = cur_.loc;
    bool dbl = (cur_.kind == Tok::AtAt);
    advance();
    auto a = std::make_unique<AddrOf>();
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
      auto n = std::make_unique<IntLit>();
      n->loc = loc; n->value = cur_.int_value; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::RealLit: {
      auto n = std::make_unique<RealLit>();
      n->loc = loc; n->text = cur_.text; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::StringLit: {
      auto n = std::make_unique<StringLit>();
      n->loc = loc; n->value = cur_.text; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwNil: {
      auto n = std::make_unique<NilLit>();
      n->loc = loc; advance();
      return parse_postfix(std::move(n));
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      auto n = std::make_unique<BoolLit>();
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
      auto base = std::make_unique<Ident>();
      base->loc = loc; base->name = "inherited";
      if (cur_.kind == Tok::Ident || tok_is_directive_kw()) {
        auto m = std::make_unique<Member>();
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
      auto id = std::make_unique<Ident>();
      id->loc = loc; id->name = cur_.text; advance();
      return parse_postfix(std::move(id));
    }
    default: {
      // Directives (virtual, register, abstract, ...) can appear as bare
      // identifiers in expression/statement position -- e.g. `abstract;`
      // as a stub method body.
      if (tok_is_directive_kw()) {
        auto id = std::make_unique<Ident>();
        id->loc = loc; id->name = cur_.text; advance();
        return parse_postfix(std::move(id));
      }
      report_error(loc,
                   "expected expression, got '" + cur_.text + "'");
      auto n = std::make_unique<IntLit>();
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
      auto m = std::make_unique<Member>();
      m->loc = loc;
      m->base = std::move(lhs);
      m->name = consume_name_or_directive("member name");
      lhs = std::move(m);
      continue;
    }
    if (accept(Tok::Caret)) {
      auto d = std::make_unique<Deref>();
      d->loc = loc; d->operand = std::move(lhs);
      lhs = std::move(d);
      continue;
    }
    if (accept(Tok::LBrack)) {
      auto ix = std::make_unique<Index>();
      ix->loc = loc; ix->base = std::move(lhs);
      ix->indices.push_back(parse_expr());
      while (accept(Tok::Comma)) ix->indices.push_back(parse_expr());
      expect(Tok::RBrack, "index");
      lhs = std::move(ix);
      continue;
    }
    if (accept(Tok::LParen)) {
      auto c = std::make_unique<Call>();
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
  auto s = std::make_unique<SetLit>();
  s->loc = loc;
  while (!at_end() && !check(Tok::RBrack)) {
    auto lo = parse_expr();
    if (accept(Tok::DotDot)) {
      auto r = std::make_unique<Range>();
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

}  // namespace p2cc
