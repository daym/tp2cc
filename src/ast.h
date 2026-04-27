#pragma once

// Pascal AST for tp2cc.
//
// Design:
//   * `Node` is the polymorphic base with a virtual destructor.
//   * Four subcategories -- Expr, Stmt, Decl, TypeExpr -- for the four big
//     syntactic categories. Each concrete node inherits from one of these.
//   * Every concrete node ends with a `Kind` enum value so we can dispatch
//     without RTTI if we want to later; for now `dynamic_cast`/`if constexpr`
//     is fine.
//   * Ownership: `std::shared_ptr<Node>` throughout -- TypeRegistry and
//     EmitCtx hold long-lived non-owning references into AST subtrees,
//     so shared ownership removes the implicit "AST must outlive its
//     consumers" invariant.
//
// We grow this file feature-by-feature. The current cut covers what's
// needed to parse a trivial program and a simple unit; parser tests pin
// each added AST node.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "source.h"

namespace tp2cc::ast {

enum class Kind : uint16_t {
  // Expressions
  Ident,
  IntLit,
  RealLit,
  StringLit,
  NilLit,
  BoolLit,
  Binary,
  Unary,
  Call,
  Index,
  Member,
  Deref,
  AddrOf,
  SetLit,
  Range,
  ArrayConst,
  RecordConst,
  // Statements
  Compound,
  Assign,
  ExprStmt,      // bare procedure call as a statement
  If,
  While,
  Repeat,
  For,
  CaseStmt,
  With,
  Goto,
  Labeled,
  BreakStmt,
  ContinueStmt,
  ExitStmt,
  EmptyStmt,
  AsmStmt,
  Try,
  Raise,
  // Declarations
  ConstDecl,
  TypeDecl,
  VarDecl,
  ProcDecl,
  LabelDecl,
  UsesClause,
  // Types
  TyName,
  TyArray,
  TyRecord,
  TyObject,
  TySet,
  TyFile,
  TyPointer,
  TyProcedural,
  TyEnum,
  TySubrange,
  TyString,
  TyMetaclass,
  TyDistinct,
  // Top-level
  UnitNode,
  ProgramNode,
};

struct Node {
  Kind kind;
  Location loc;
  virtual ~Node() = default;
 protected:
  explicit Node(Kind k) : kind(k) {}
};

struct Expr : Node { using Node::Node; };
struct Stmt : Node { using Node::Node; };
struct Decl : Node { using Node::Node; };
struct TypeExpr : Node { using Node::Node; };

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using DeclPtr = std::shared_ptr<Decl>;
using TypePtr = std::shared_ptr<TypeExpr>;

// ---------------------------------------------------------------------------
// Expressions

struct Ident : Expr {
  std::string name;   // lowercased
  Ident() : Expr(Kind::Ident) {}
};

struct IntLit : Expr {
  // Magnitude only.  Negative integer expressions are modelled as
  // `Unary(Neg, IntLit(magnitude))', so the sign is carried by the
  // parent node, not this one.  uint64_t so the full unsigned range
  // fits without bit-preserving casts.
  uint64_t value = 0;
  IntLit() : Expr(Kind::IntLit) {}
};

struct RealLit : Expr {
  std::string text;   // keep textual form; codegen emits as-is
  RealLit() : Expr(Kind::RealLit) {}
};

struct StringLit : Expr {
  std::string value;  // already unescaped
  StringLit() : Expr(Kind::StringLit) {}
};

struct NilLit : Expr { NilLit() : Expr(Kind::NilLit) {} };

struct BoolLit : Expr {
  bool value = false;
  BoolLit() : Expr(Kind::BoolLit) {}
};

enum class BinOp : uint8_t {
  Add, Sub, Mul, RealDiv, IntDiv, Mod, Shl, Shr,
  And, Or, Xor,
  Eq, NotEq, Lt, Gt, LtEq, GtEq,
  In, Is, As,
  SymDiff,
};

struct Binary : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  // Snapshot of `{$Q+}` / `{$overflowchecks+}` at the point this node
  // was parsed. The emitter routes integer arithmetic through checked
  // helpers when set; otherwise plain `+` / `-` / `*`.
  bool q_check = false;
  Binary() : Expr(Kind::Binary) {}
};

enum class UnOp : uint8_t { Neg, Plus, Not };

struct Unary : Expr {
  UnOp op;
  ExprPtr operand;
  bool q_check = false;
  Unary() : Expr(Kind::Unary) {}
};

struct Call : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
  // Optional write-formatter suffix for `x:width` or `x:width:prec`.
  // Attached to each arg independently in a parallel vector; unused slots
  // are null.
  std::vector<ExprPtr> width;
  std::vector<ExprPtr> precision;
  Call() : Expr(Kind::Call) {}
};

struct Index : Expr {
  ExprPtr base;
  std::vector<ExprPtr> indices;
  Index() : Expr(Kind::Index) {}
};

struct Member : Expr {
  ExprPtr base;
  std::string name;   // lowercased
  Member() : Expr(Kind::Member) {}
};

struct Deref : Expr {
  ExprPtr operand;
  Deref() : Expr(Kind::Deref) {}
};

struct AddrOf : Expr {
  ExprPtr operand;
  bool double_addr = false;   // @@
  AddrOf() : Expr(Kind::AddrOf) {}
};

struct SetLit : Expr {
  // Each element is either a plain expr or a range expr (Range).
  std::vector<ExprPtr> elements;
  SetLit() : Expr(Kind::SetLit) {}
};

struct Range : Expr {
  ExprPtr lo, hi;
  Range() : Expr(Kind::Range) {}
};

// Typed-constant initializers. Pascal distinguishes these from ordinary
// expressions: only legal on the RHS of `ident : type = ...`.

// `( a, b, c, ... )` -- may nest (2-D array constants).
struct ArrayConst : Expr {
  std::vector<ExprPtr> elements;
  ArrayConst() : Expr(Kind::ArrayConst) {}
};

// `( fld : val ; fld : val ; ... )`.
struct RecordConst : Expr {
  std::vector<std::pair<std::string, ExprPtr>> fields;
  RecordConst() : Expr(Kind::RecordConst) {}
};

// ---------------------------------------------------------------------------
// Statements

struct Compound : Stmt {
  std::vector<StmtPtr> body;
  Compound() : Stmt(Kind::Compound) {}
};

struct Assign : Stmt {
  ExprPtr target;
  ExprPtr value;
  // Snapshot of `{$R+}` / `{$rangechecks+}` at parse time. The
  // emitter inserts a runtime range check on narrowing conversions
  // (real -> int, wider-int -> narrower-int, ...) when set.
  bool r_check = false;
  Assign() : Stmt(Kind::Assign) {}
};

struct ExprStmt : Stmt {
  // For a bare `foo;` or `foo(x);` used as a statement.
  ExprPtr expr;
  ExprStmt() : Stmt(Kind::ExprStmt) {}
};

struct If : Stmt {
  ExprPtr cond;
  StmtPtr then_branch;
  StmtPtr else_branch;   // may be null
  If() : Stmt(Kind::If) {}
};

struct While : Stmt {
  ExprPtr cond;
  StmtPtr body;
  While() : Stmt(Kind::While) {}
};

struct Repeat : Stmt {
  std::vector<StmtPtr> body;
  ExprPtr cond;
  Repeat() : Stmt(Kind::Repeat) {}
};

struct For : Stmt {
  std::string var;
  ExprPtr from, to;
  bool downto = false;
  StmtPtr body;
  For() : Stmt(Kind::For) {}
};

struct CaseArm {
  // Each label is an expression; range labels use Range nodes.
  std::vector<ExprPtr> labels;
  StmtPtr body;
};

struct CaseStmt : Stmt {
  ExprPtr selector;
  std::vector<CaseArm> arms;
  StmtPtr else_branch;   // may be null; covers `else` / `otherwise`
  CaseStmt() : Stmt(Kind::CaseStmt) {}
};

struct With : Stmt {
  std::vector<ExprPtr> exprs;   // `with A, B do S` is modeled as one node
  StmtPtr body;
  With() : Stmt(Kind::With) {}
};

struct Goto : Stmt {
  std::string label;
  Goto() : Stmt(Kind::Goto) {}
};

struct Labeled : Stmt {
  std::string label;
  StmtPtr body;
  Labeled() : Stmt(Kind::Labeled) {}
};

struct BreakStmt : Stmt { BreakStmt() : Stmt(Kind::BreakStmt) {} };
struct ContinueStmt : Stmt { ContinueStmt() : Stmt(Kind::ContinueStmt) {} };
struct ExitStmt : Stmt {
  ExprPtr value;   // optional: exit(expr)
  ExitStmt() : Stmt(Kind::ExitStmt) {}
};
struct EmptyStmt : Stmt { EmptyStmt() : Stmt(Kind::EmptyStmt) {} };

struct AsmStmt : Stmt {
  // We don't parse asm bodies -- the only one in the compiler proper lives in
  // tpexcept.pas which we replace with a C++ shim. Just capture the raw text
  // for diagnostics.
  std::string raw;
  AsmStmt() : Stmt(Kind::AsmStmt) {}
};

// One `on <var>: <Class> do <stmt>' arm inside `try ... except ... end'.
// The variable name is optional (`on EFoo do ...' without bind).
struct ExceptHandler {
  std::string var_name;      // may be empty
  std::string class_name;    // lowercased; empty means catch-all (rare)
  StmtPtr body;
};

// Pascal `try ... except ... end' / `try ... finally ... end'.
// A single try-block is either an except or a finally, not both
// (nested try blocks handle the combined case -- see compiler/compiler.pas).
struct Try : Stmt {
  std::vector<StmtPtr> body;         // statements in the try-body
  bool is_finally = false;           // false = except, true = finally
  // except-form: per-class handlers, with optional catch-all via
  // `else <stmt>' after the `on' arms.
  std::vector<ExceptHandler> handlers;
  StmtPtr except_else;               // else-clause of except, may be null
  // finally-form: statements that always run on exit from body.
  std::vector<StmtPtr> finally_body;
  Try() : Stmt(Kind::Try) {}
};

// `raise' statement.  Two AST variants:
//   raise EFoo.Create('msg');  -- raise an instance
//   raise;                     -- bare, re-raise current exception
//                                 (only valid inside an except handler)
// Optional `at <address>' suffix (Delphi debug aid) is parsed and
// discarded.
struct Raise : Stmt {
  ExprPtr value;                     // null for bare `raise;'
  Raise() : Stmt(Kind::Raise) {}
};

// ---------------------------------------------------------------------------
// Declarations

struct ConstDecl : Decl {
  std::string name;
  TypePtr type;         // may be null (untyped const)
  ExprPtr value;        // required
  ConstDecl() : Decl(Kind::ConstDecl) {}
};

struct TypeDecl : Decl {
  std::string name;
  TypePtr type;
  TypeDecl() : Decl(Kind::TypeDecl) {}
};

struct VarDecl : Decl {
  std::vector<std::string> names;
  TypePtr type;
  ExprPtr init;          // optional `= expr` (typed const / global init)
  bool is_absolute = false;
  bool is_external = false;
  std::string absolute_target;   // when is_absolute; the aliased variable name
  ExprPtr external_name;         // `external 'name'` -- null if not external
  std::string external_lib;      // `external 'lib' name '...'`; may be empty
  VarDecl() : Decl(Kind::VarDecl) {}
};

struct LabelDecl : Decl {
  std::vector<std::string> labels;
  LabelDecl() : Decl(Kind::LabelDecl) {}
};

struct Param {
  enum Mode { Value, Var, Const, Out } mode = Value;
  std::vector<std::string> names;
  TypePtr type;        // may be null for untyped `var`/`const` params
  ExprPtr default_value;  // optional
};

enum class ProcKind : uint8_t { Procedure, Function, Constructor, Destructor };

struct ProcDecl : Decl {
  ProcKind pkind = ProcKind::Procedure;
  std::string name;
  // For methods: the object/record type this belongs to, if parsed as
  // `procedure TFoo.Bar(...)`. Empty otherwise.
  std::string of_type;
  bool is_class_method;
  std::vector<Param> params;
  TypePtr return_type;  // only for Function/Constructor; null otherwise
  // Modifiers (unordered list in source; we capture the important ones):
  bool is_virtual = false;
  bool is_abstract = false;
  bool is_override = false;
  bool is_forward = false;
  bool is_inline = false;
  bool is_cdecl = false;
  bool is_external = false;
  bool is_assembler = false;
  std::string external_lib;    // when is_external
  std::string external_name;   // when is_external; may be empty
  // Body (present only for the implementation-side definition):
  std::vector<DeclPtr> locals;
  StmtPtr body;                // null for forward / abstract / external
  explicit ProcDecl(bool class_method)
      : Decl(Kind::ProcDecl), is_class_method(class_method) {}
};

struct UsesClause : Decl {
  std::vector<std::string> units;
  UsesClause() : Decl(Kind::UsesClause) {}
};

// ---------------------------------------------------------------------------
// Types

struct TyName : TypeExpr {
  std::string name;
  // `^TFoo` style pointer-to-not-yet-defined named types become TyPointer
  // with inner=TyName.
  TyName() : TypeExpr(Kind::TyName) {}
};

enum class ArrayKind : uint8_t {
  Fixed,
  Open,
  Dynamic,
};

struct TyArray : TypeExpr {
  std::vector<TypePtr> dims;  // each is a TySubrange or TyName
  TypePtr element;
  bool is_packed = false;
  ArrayKind array_kind = ArrayKind::Fixed;
  TyArray() : TypeExpr(Kind::TyArray) {}
};

struct RecordField {
  std::vector<std::string> names;
  TypePtr type;
};

// Variant-record tail: `case Tag : T of lit,lit : (fields...); ...`
struct VariantCase {
  std::vector<ExprPtr> labels;
  std::vector<RecordField> fields;
};

struct TyRecord : TypeExpr {
  std::vector<RecordField> fields;
  // Optional variant part:
  bool has_variant = false;
  std::string variant_tag_name;    // may be empty when untagged: `case T of`
  TypePtr variant_tag_type;
  std::vector<VariantCase> variant_cases;
  bool is_packed = false;
  TyRecord() : TypeExpr(Kind::TyRecord) {}
};

enum class Visibility : uint8_t { Public, Private, Protected };

enum class ObjectMemberKind : uint8_t { Field, Method, Property };

struct PropertyDecl {
  std::string name;
  std::vector<Param> params;
  TypePtr type;
  std::string read_name;
  std::string write_name;
  bool is_default = false;
};

struct ObjectMember {
  // One of: field (names+type) | method (ProcDecl) | property (PropertyDecl)
  Visibility vis = Visibility::Public;
  ObjectMemberKind kind = ObjectMemberKind::Field;
  // field side
  std::vector<std::string> field_names;
  TypePtr field_type;
  // method side -- typed so the "it's a ProcDecl" invariant is
  // enforced at the type level rather than via documented faith.
  std::shared_ptr<ProcDecl> method;
  // property side
  PropertyDecl property;
};

struct TyObject : TypeExpr {
  std::string parent;              // base object type name; empty if none
  std::vector<ObjectMember> members;
  // `false`: TP-style `object` -- value type, stack-OK, `new(p, init)` to
  // heap-allocate, destructor via `dispose(p, done)`.
  // `true`: Delphi-style `class` -- reference type, always heap, implicit
  // TObject ancestor, constructor returns a pointer via `TFoo.Create(...)`,
  // destruction via `.Free` (null-safe at the call site).
  bool is_reference_type = false;
  // Delphi-style forward declaration `T = class;' -- the body comes
  // later in the same type block (or same unit section). A true forward
  // declaration carries no members and no parent; `T = class(Base);` is a
  // complete empty class declaration instead, not a forward.
  bool is_forward = false;
  TyObject() : TypeExpr(Kind::TyObject) {}
};

struct TySet : TypeExpr {
  TypePtr element;
  TySet() : TypeExpr(Kind::TySet) {}
};

struct TyFile : TypeExpr {
  TypePtr element;   // null for untyped `file`; special marker for `text`
  bool is_text = false;
  TyFile() : TypeExpr(Kind::TyFile) {}
};

struct TyPointer : TypeExpr {
  TypePtr target;
  TyPointer() : TypeExpr(Kind::TyPointer) {}
};

// Delphi distinct-type alias: `T = type <Underlying>'.  Creates a new
// type that is layout-compatible with its underlying but NOT
// assignment-compatible -- Pascal rejects `var x:T; var i:Integer;
// i:=x' without an explicit `Integer(x)' cast.  Emit-time target is a
// C++ wrapper struct with an `explicit' constructor and an `explicit
// operator Underlying()' so implicit conversions are rejected at
// compile time, preserving the type discipline the author intended.
struct TyDistinct : TypeExpr {
  TypePtr underlying;
  TyDistinct() : TypeExpr(Kind::TyDistinct) {}
};

// Delphi `class of T' -- a metaclass reference.  A value of this
// type names a class (as opposed to an instance of a class).  At
// runtime it's a pointer to a class descriptor carrying a virtual
// NewInstance etc.; `AClassVar.Create(args)' allocates an instance
// of whichever concrete class AClassVar currently names.
//
// For emit purposes this is NOT a `T*' (pointer to instance) -- it's
// a pointer to class-level metadata.  Kept syntactically minimal
// for now: we just record the name of the referenced class.
struct TyMetaclass : TypeExpr {
  std::string class_name;
  TyMetaclass() : TypeExpr(Kind::TyMetaclass) {}
};

struct TyProcedural : TypeExpr {
  bool is_function = false;
  std::vector<Param> params;
  TypePtr return_type;    // for function types
  bool is_cdecl = false;
  bool is_method = false; // `procedure/function ... of object`
  TyProcedural() : TypeExpr(Kind::TyProcedural) {}
};

struct EnumMember {
  std::string name;
  ExprPtr value;  // optional explicit ordinal value
};

struct TyEnum : TypeExpr {
  std::vector<EnumMember> members;
  TyEnum() : TypeExpr(Kind::TyEnum) {}
};

struct TySubrange : TypeExpr {
  ExprPtr lo, hi;
  TySubrange() : TypeExpr(Kind::TySubrange) {}
};

struct TyString : TypeExpr {
  ExprPtr max_length;   // null for unsized `string`
  TyString() : TypeExpr(Kind::TyString) {}
};

// ---------------------------------------------------------------------------
// Top level

// Shared AST node for `unit` and `program`.
struct UnitNode : Node {
  std::string name;
  // Interface section: uses + decls. For a program, interface is empty.
  std::vector<std::string> interface_uses;
  std::vector<DeclPtr> interface_decls;
  // Implementation section: uses + decls + optional body (`begin..end.`).
  std::vector<std::string> impl_uses;
  std::vector<DeclPtr> impl_decls;
  StmtPtr init_body;    // TP-7 style `begin..end.` at unit tail
  StmtPtr final_body;   // Delphi/FPC `finalization` block
  bool is_program = false;
  UnitNode() : Node(Kind::UnitNode) {}
};

}  // namespace tp2cc::ast
