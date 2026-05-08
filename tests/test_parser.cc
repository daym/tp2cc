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

std::shared_ptr<UnitNode> parse_snippet(std::string text) {
  auto sf = std::make_shared<SourceFile>();
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

void test_unit_initialization_and_finalization() {
  int before = error_count();
  auto u = parse_snippet(
      "unit foo;\n"
      "interface\n"
      "implementation\n"
      "initialization\n"
      "  x := 1;\n"
      "finalization\n"
      "  while x < 3 do begin\n"
      "    x := x + 1;\n"
      "  end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) {
    CHECK(u->init_body != nullptr);
    CHECK(u->final_body != nullptr);
    auto* fini = dynamic_cast<Compound*>(u->final_body.get());
    CHECK(fini != nullptr);
    if (fini) CHECK_EQ(fini->body.size(), size_t{1});
  }
}

void test_unit_standalone_finalization() {
  int before = error_count();
  auto u = parse_snippet(
      "unit foo;\n"
      "interface\n"
      "implementation\n"
      "finalization\n"
      "  x := 1;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u) {
    CHECK(u->init_body == nullptr);
    CHECK(u->final_body != nullptr);
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

void test_enum_explicit_values() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  treg = (lo := low(longint), hi := high(longint));\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td != nullptr);
    if (td) {
      auto* te = dynamic_cast<TyEnum*>(td->type.get());
      CHECK(te != nullptr);
      if (te) {
        CHECK_EQ(te->members.size(), size_t{2});
        CHECK_EQ(te->members[0].name, std::string("lo"));
        CHECK(te->members[0].value != nullptr);
        CHECK_EQ(te->members[1].name, std::string("hi"));
        CHECK(te->members[1].value != nullptr);
      }
    }
  }
}

void test_enum_captures_packenum_directives() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "{$packenum 1}\n"
      "type\n"
      "  tone = (a, b, c);\n"
      "{$minenumsize 2}\n"
      "  ttwo = (d, e, f);\n"
      "{$z4}\n"
      "  tfour = (g, h, i);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  if (u) {
    CHECK_EQ(u->interface_decls.size(), size_t{3});
    auto* t1 = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    auto* t2 = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    auto* t4 = dynamic_cast<TypeDecl*>(u->interface_decls[2].get());
    CHECK(t1 && t2 && t4);
    if (t1 && t2 && t4) {
      auto* e1 = dynamic_cast<TyEnum*>(t1->type.get());
      auto* e2 = dynamic_cast<TyEnum*>(t2->type.get());
      auto* e4 = dynamic_cast<TyEnum*>(t4->type.get());
      CHECK(e1 && e2 && e4);
      if (e1 && e2 && e4) {
        CHECK_EQ(e1->packenum, uint8_t{1});
        CHECK_EQ(e2->packenum, uint8_t{2});
        CHECK_EQ(e4->packenum, uint8_t{4});
      }
    }
  }
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
      if (to) {
        CHECK_EQ(to->parent, std::string("tbase"));
        // TP-style object: value type, not a reference.
        CHECK(!to->is_reference_type);
      }
    }
  }
}

// Delphi `T = type <Underlying>' distinct-type alias.  Must preserve
// the wrapping as a distinct node so emit-time can produce a C++
// struct that blocks implicit conversion to the underlying.
void test_distinct_type() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TSuperRegister = type word;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    if (td) {
      auto* dt = dynamic_cast<TyDistinct*>(td->type.get());
      CHECK(dt);  // must be a wrapper, not collapsed to TyName(word)
      if (dt) {
        CHECK(dt->underlying != nullptr);
        auto* inner = dynamic_cast<TyName*>(dt->underlying.get());
        CHECK(inner);
      }
    }
  }
}

// Pascal try/except/finally + raise.
//
// Exercises parser support for try/except/finally/raise forms:
//   - try ... except on E: TClass do ... end
//   - try ... except on TClass do ... else ... end  (no bind)
//   - try ... finally ... end
//   - raise EFoo.Create(...)
//   - raise EFoo.Create(...) at Addr, Frame
//   - bare `raise;' inside except arms
//   - nested try (finally-wrapping-except)
void test_try_except_finally_raise() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "begin\n"
      "  try\n"
      "    try\n"
      "      doit\n"
      "    except\n"
      "      on e: efoo do raise;\n"
      "      on ebar do writeln('bar');\n"
      "    else\n"
      "      writeln('other');\n"
      "    end;\n"
      "  finally\n"
      "    cleanup;\n"
      "  end;\n"
      "  raise efoo.create('msg') at get_caller_addr(get_frame), "
      "get_caller_frame(get_frame);\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

// `class of T' metaclass reference -- a value of this type names a
// class rather than an instance.  Different grammar from `class (...)
// ... end': `of' disambiguates.
void test_metaclass_type() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TBase = class\n"
      "  end;\n"
      "  TBaseClass = class of TBase;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* td_meta = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    CHECK(td_meta);
    if (td_meta) {
      auto* tm = dynamic_cast<TyMetaclass*>(td_meta->type.get());
      CHECK(tm);
      // Lexer lowercases identifiers; Pascal is case-insensitive.
      if (tm) CHECK_EQ(tm->class_name, std::string("tbase"));
    }
  }
}

// Class methods parse as distinct declarations, while regular method
// directives like `dynamic` and `reintroduce` still stay soft.
void test_class_directives() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  TFoo = class\n"
      "    procedure A; dynamic;\n"
      "    procedure B(x:integer); overload;\n"
      "    procedure B(x:string);  overload;\n"
      "    procedure C; reintroduce;\n"
      "    class procedure Classy;\n"
      "    class function  ClassyF: integer;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    if (td) {
      auto* to = dynamic_cast<TyObject*>(td->type.get());
      CHECK(to);
      if (to) {
        // 2 overloads of B count as 2 distinct method entries; A, C,
        // Classy, ClassyF are one each -- total 6.
        CHECK_EQ(to->members.size(), size_t{6});
        CHECK(to->members[4].method->is_class_method);
        CHECK(to->members[5].method->is_class_method);
      }
    }
  }
}

void test_class_method_impl_decl() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tfoo = class\n"
      "    class procedure classy;\n"
      "  end;\n"
      "implementation\n"
      "class procedure tfoo.classy;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->impl_decls.empty()) {
    auto* pd = dynamic_cast<ProcDecl*>(u->impl_decls[0].get());
    CHECK(pd);
    if (pd) {
      CHECK(pd->is_class_method);
      CHECK_EQ(pd->of_type, std::string("tfoo"));
      CHECK_EQ(pd->name, std::string("classy"));
    }
  }
}

void test_virtual_class_method_modifiers_are_recorded() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tfoo = class\n"
      "    class procedure basehook; virtual; abstract;\n"
      "    class procedure classy; override;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(u != nullptr);
  CHECK_EQ(error_count() - before, 0);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    auto* to = td ? dynamic_cast<TyObject*>(td->type.get()) : nullptr;
    CHECK(to);
    if (to && to->members.size() >= 2) {
      auto* base = to->members[0].method.get();
      auto* classy = to->members[1].method.get();
      CHECK(base && base->is_class_method);
      CHECK(base && base->is_virtual);
      CHECK(base && base->is_abstract);
      CHECK(classy && classy->is_class_method);
      CHECK(classy && classy->is_override);
    }
  }
}

// Delphi-style `class' reuses the object-member parser, then records
// reference-type semantics so the emitter can pick pointer storage and
// heap allocation.
void test_class_declaration() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    constructor Create;\n"
      "    destructor Destroy; override;\n"
      "    procedure m; virtual;\n"
      "  end;\n"
      "  tderived = class(tbase)\n"
      "    procedure m; override;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* td_base = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    auto* td_der  = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    CHECK(td_base && td_der);
    if (td_base && td_der) {
      auto* to_base = dynamic_cast<TyObject*>(td_base->type.get());
      auto* to_der  = dynamic_cast<TyObject*>(td_der->type.get());
      CHECK(to_base && to_der);
      if (to_base && to_der) {
        // Both are `class', so both carry the reference-type marker.
        CHECK(to_base->is_reference_type);
        CHECK(to_der->is_reference_type);
        CHECK(to_base->parent.empty());
        CHECK_EQ(to_der->parent, std::string("tbase"));
        // tbase has ctor, dtor, and one virtual method.
        CHECK_EQ(to_base->members.size(), size_t{3});
      }
    }
  }
}

void test_empty_inherited_class_decl() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  ebase = class end;\n"
      "  echild = class(ebase);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    CHECK(td);
    if (td) {
      auto* to = dynamic_cast<TyObject*>(td->type.get());
      CHECK(to);
      if (to) {
        CHECK(to->is_reference_type);
        CHECK(!to->is_forward);
        CHECK_EQ(to->parent, std::string("ebase"));
        CHECK_EQ(to->members.size(), size_t{0});
      }
    }
  }
}

void test_corba_interface_decl_and_class_implementation() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "{$interfaces corba}\n"
      "type\n"
      "  ireader = interface ['{11111111-1111-1111-1111-111111111111}']\n"
      "    function next(out s : string) : boolean;\n"
      "  end;\n"
      "  twriter = interface\n"
      "    procedure putline(const s : string);\n"
      "  end;\n"
      "  treader = class(tobject, ireader, twriter)\n"
      "    function next(out s : string) : boolean;\n"
      "    procedure putline(const s : string);\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 3) {
    auto* ireader_td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    auto* treader_td = dynamic_cast<TypeDecl*>(u->interface_decls[2].get());
    CHECK(ireader_td && treader_td);
    if (ireader_td) {
      auto* ti = dynamic_cast<TyInterface*>(ireader_td->type.get());
      CHECK(ti != nullptr);
      if (ti) {
        CHECK_EQ(ti->metadata_string,
                 std::string("{11111111-1111-1111-1111-111111111111}"));
        CHECK_EQ(ti->members.size(), size_t{1});
        CHECK_EQ(ti->members[0].method->name, std::string("next"));
      }
    }
    if (treader_td) {
      auto* to = dynamic_cast<TyObject*>(treader_td->type.get());
      CHECK(to != nullptr);
      if (to) {
        CHECK(to->is_reference_type);
        CHECK_EQ(to->parent, std::string("tobject"));
        CHECK_EQ(to->interfaces.size(), size_t{2});
        CHECK_EQ(to->interfaces[0], std::string("ireader"));
        CHECK_EQ(to->interfaces[1], std::string("twriter"));
      }
    }
  }
}

void test_com_interface_is_rejected() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  irefcounted = interface\n"
      "    procedure addref;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(u != nullptr);
  CHECK(error_count() - before > 0);
}

void test_explicit_com_interface_is_rejected() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "{$interfaces com}\n"
      "type\n"
      "  irefcounted = interface\n"
      "    procedure addref;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(u != nullptr);
  CHECK(error_count() - before > 0);
}

void test_interfaces_default_restores_com_rejection() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "{$interfaces corba}\n"
      "{$interfaces default}\n"
      "type\n"
      "  irefcounted = interface\n"
      "    procedure addref;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK(u != nullptr);
  CHECK(error_count() - before > 0);
}

void test_class_properties() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tlist = class\n"
      "  private\n"
      "    fcount : integer;\n"
      "    function get(index : integer) : pointer;\n"
      "    procedure put(index : integer; value : pointer);\n"
      "  public\n"
      "    property Count : integer read fcount;\n"
      "    property Items[index : integer] : pointer read get write put; default;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    if (td) {
      auto* to = dynamic_cast<TyObject*>(td->type.get());
      CHECK(to);
      if (to) {
        CHECK_EQ(to->members.size(), size_t{5});
        CHECK_EQ(to->members[3].kind, ObjectMemberKind::Property);
        CHECK_EQ(to->members[3].property.name, std::string("count"));
        CHECK_EQ(to->members[3].property.read_name, std::string("fcount"));
        CHECK_EQ(to->members[3].property.params.size(), size_t{0});
        CHECK_EQ(to->members[4].kind, ObjectMemberKind::Property);
        CHECK_EQ(to->members[4].property.name, std::string("items"));
        CHECK_EQ(to->members[4].property.params.size(), size_t{1});
        CHECK_EQ(to->members[4].property.read_name, std::string("get"));
        CHECK_EQ(to->members[4].property.write_name, std::string("put"));
        CHECK(to->members[4].property.is_default);
      }
    }
  }
}

void test_write_only_property() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tflag = class\n"
      "  private\n"
      "    fvalue : boolean;\n"
      "  public\n"
      "    property value : boolean write fvalue;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->interface_decls.empty()) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    CHECK(td);
    if (td) {
      auto* to = dynamic_cast<TyObject*>(td->type.get());
      CHECK(to);
      if (to) {
        CHECK_EQ(to->members.size(), size_t{2});
        CHECK_EQ(to->members[1].kind, ObjectMemberKind::Property);
        CHECK_EQ(to->members[1].property.write_name, std::string("fvalue"));
        CHECK_EQ(to->members[1].property.read_name, std::string(""));
      }
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

void test_c_style_shift_exprs_parse_like_shl_shr() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var x : integer;\n"
      "begin\n"
      "  x := (1 << 3) + (32 >> 2);\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_unary_plus_minus_parse_in_factor_position() {
  int before = error_count();
  auto u = parse_snippet(
      "program p;\n"
      "var x, a, b, mask : integer;\n"
      "begin\n"
      "  x := 5 * -3;\n"
      "  x := 5 * -a;\n"
      "  x := a or -mask;\n"
      "  x := not -b;\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
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
      "  tob = procedure(a : integer) of object;\n"
      "  tfn = function(a : integer) : integer;\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[1].get());
    CHECK(td != nullptr);
    if (td) {
      auto* tp = dynamic_cast<TyProcedural*>(td->type.get());
      CHECK(tp != nullptr);
      if (tp) CHECK(tp->is_method);
    }
  }
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

void test_typed_array_constant_with_inline_subrange_element_type() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "const\n"
      "  reverse_nible : array[0..1] of 0..15 = (%0001,%0010);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && !u->interface_decls.empty()) {
    auto* cd = dynamic_cast<ConstDecl*>(u->interface_decls[0].get());
    CHECK(cd);
    if (cd) {
      auto* ac = dynamic_cast<ArrayConst*>(cd->value.get());
      CHECK(ac);
      if (ac) CHECK_EQ(ac->elements.size(), size_t{2});
    }
  }
}

void test_subrange_bound_constant_intrinsic_calls() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tcgloc = (loc_invalid, loc_void, loc_creference, loc_reference);\n"
      "  tcgnonrefloc = low(tcgloc)..pred(loc_creference);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
}

void test_subrange_bound_rejects_nonconstant_calls() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tbad = low(foo())..1;\n"
      "implementation\n"
      "end.\n");
  CHECK(error_count() - before > 0);
  CHECK(u != nullptr);
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

void test_out_parameter_is_soft_and_parsed_in_formal_params() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "procedure fill(out r : integer);\n"
      "procedure keep(out : integer);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* fill = dynamic_cast<ProcDecl*>(u->interface_decls[0].get());
    auto* keep = dynamic_cast<ProcDecl*>(u->interface_decls[1].get());
    CHECK(fill != nullptr);
    CHECK(keep != nullptr);
    if (fill && !fill->params.empty()) {
      CHECK(fill->params[0].mode == Param::Out);
      CHECK_EQ(fill->params[0].names.size(), size_t{1});
      CHECK_EQ(fill->params[0].names[0], std::string("r"));
    }
    if (keep && !keep->params.empty()) {
      CHECK(keep->params[0].mode == Param::Value);
      CHECK_EQ(keep->params[0].names.size(), size_t{1});
      CHECK_EQ(keep->params[0].names[0], std::string("out"));
    }
  }
}

void test_open_and_dynamic_array_forms_stay_distinct() {
  int before = error_count();
  auto u = parse_snippet(
      "unit u;\n"
      "interface\n"
      "type\n"
      "  tints = array of integer;\n"
      "procedure demo(const xs : array of integer);\n"
      "implementation\n"
      "end.\n");
  CHECK_EQ(error_count() - before, 0);
  CHECK(u != nullptr);
  if (u && u->interface_decls.size() >= 2) {
    auto* td = dynamic_cast<TypeDecl*>(u->interface_decls[0].get());
    auto* pd = dynamic_cast<ProcDecl*>(u->interface_decls[1].get());
    CHECK(td != nullptr);
    CHECK(pd != nullptr);
    if (td) {
      auto* arr = dynamic_cast<TyArray*>(td->type.get());
      CHECK(arr != nullptr);
      if (arr) CHECK(arr->array_kind == ArrayKind::Dynamic);
    }
    if (pd && !pd->params.empty()) {
      auto* arr = dynamic_cast<TyArray*>(pd->params[0].type.get());
      CHECK(arr != nullptr);
      if (arr) CHECK(arr->array_kind == ArrayKind::Open);
    }
  }
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
  RUN_TEST(test_unit_initialization_and_finalization);
  RUN_TEST(test_unit_standalone_finalization);
  RUN_TEST(test_const_decls);
  RUN_TEST(test_type_decls_named_and_enum);
  RUN_TEST(test_enum_explicit_values);
  RUN_TEST(test_enum_captures_packenum_directives);
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
  RUN_TEST(test_c_style_shift_exprs_parse_like_shl_shr);
  RUN_TEST(test_unary_plus_minus_parse_in_factor_position);
  RUN_TEST(test_set_literal_in_expr);
  RUN_TEST(test_call_with_write_formatter);
  RUN_TEST(test_procedural_type_decl);
  RUN_TEST(test_directives_as_identifiers);
  RUN_TEST(test_typed_array_constant);
  RUN_TEST(test_typed_array_constant_with_inline_subrange_element_type);
  RUN_TEST(test_subrange_bound_constant_intrinsic_calls);
  RUN_TEST(test_subrange_bound_rejects_nonconstant_calls);
  RUN_TEST(test_typed_record_constant);
  RUN_TEST(test_inherited_method_call);
  RUN_TEST(test_directive_as_method_name);
  RUN_TEST(test_unit_named_like_directive);
  RUN_TEST(test_out_parameter_is_soft_and_parsed_in_formal_params);
  RUN_TEST(test_open_and_dynamic_array_forms_stay_distinct);
  RUN_TEST(test_error_recovery_basic);
  RUN_TEST(test_class_declaration);
  RUN_TEST(test_empty_inherited_class_decl);
  RUN_TEST(test_corba_interface_decl_and_class_implementation);
  RUN_TEST(test_com_interface_is_rejected);
  RUN_TEST(test_explicit_com_interface_is_rejected);
  RUN_TEST(test_interfaces_default_restores_com_rejection);
  RUN_TEST(test_class_properties);
  RUN_TEST(test_write_only_property);
  RUN_TEST(test_class_directives);
  RUN_TEST(test_class_method_impl_decl);
  RUN_TEST(test_virtual_class_method_modifiers_are_recorded);
  RUN_TEST(test_metaclass_type);
  RUN_TEST(test_try_except_finally_raise);
  RUN_TEST(test_distinct_type);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
