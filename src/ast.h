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
#include <utility>
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
  TyInterface,
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
  Node(Kind k, Location loc_in) : kind(k), loc(loc_in) {}
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
  explicit Ident(std::string name_in)
      : Expr(Kind::Ident), name(std::move(name_in)) {}
  Ident(Location loc_in, std::string name_in)
      : Expr(Kind::Ident, loc_in), name(std::move(name_in)) {}
};

struct IntLit : Expr {
  // Magnitude only.  Negative integer expressions are modelled as
  // `Unary(Neg, IntLit(magnitude))', so the sign is carried by the
  // parent node, not this one.  uint64_t so the full unsigned range
  // fits without bit-preserving casts.
  uint64_t value = 0;
  IntLit() : Expr(Kind::IntLit) {}
  explicit IntLit(uint64_t value_in) : Expr(Kind::IntLit), value(value_in) {}
  IntLit(Location loc_in, uint64_t value_in)
      : Expr(Kind::IntLit, loc_in), value(value_in) {}
};

struct RealLit : Expr {
  std::string text;   // keep textual form; codegen emits as-is
  RealLit() : Expr(Kind::RealLit) {}
  explicit RealLit(std::string text_in)
      : Expr(Kind::RealLit), text(std::move(text_in)) {}
  RealLit(Location loc_in, std::string text_in)
      : Expr(Kind::RealLit, loc_in), text(std::move(text_in)) {}
};

struct StringLit : Expr {
  std::string value;  // already unescaped
  StringLit() : Expr(Kind::StringLit) {}
  explicit StringLit(std::string value_in)
      : Expr(Kind::StringLit), value(std::move(value_in)) {}
  StringLit(Location loc_in, std::string value_in)
      : Expr(Kind::StringLit, loc_in), value(std::move(value_in)) {}
};

struct NilLit : Expr {
  NilLit() : Expr(Kind::NilLit) {}
  explicit NilLit(Location loc_in) : Expr(Kind::NilLit, loc_in) {}
};

struct BoolLit : Expr {
  bool value = false;
  BoolLit() : Expr(Kind::BoolLit) {}
  explicit BoolLit(bool value_in) : Expr(Kind::BoolLit), value(value_in) {}
  BoolLit(Location loc_in, bool value_in)
      : Expr(Kind::BoolLit, loc_in), value(value_in) {}
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
  Binary(BinOp op_in, ExprPtr lhs_in, ExprPtr rhs_in, bool q_check_in)
      : Expr(Kind::Binary),
        op(op_in),
        lhs(std::move(lhs_in)),
        rhs(std::move(rhs_in)),
        q_check(q_check_in) {}
  Binary(Location loc_in, BinOp op_in, ExprPtr lhs_in, ExprPtr rhs_in,
         bool q_check_in)
      : Expr(Kind::Binary, loc_in),
        op(op_in),
        lhs(std::move(lhs_in)),
        rhs(std::move(rhs_in)),
        q_check(q_check_in) {}
};

enum class UnOp : uint8_t { Neg, Plus, Not };

struct Unary : Expr {
  UnOp op;
  ExprPtr operand;
  bool q_check = false;
  Unary() : Expr(Kind::Unary) {}
  Unary(Location loc_in, UnOp op_in, ExprPtr operand_in,
        bool q_check_in = false)
      : Expr(Kind::Unary, loc_in),
        op(op_in),
        operand(std::move(operand_in)),
        q_check(q_check_in) {}
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
  Call(Location loc_in, ExprPtr callee_in, std::vector<ExprPtr> args_in,
       std::vector<ExprPtr> width_in, std::vector<ExprPtr> precision_in)
      : Expr(Kind::Call, loc_in),
        callee(std::move(callee_in)),
        args(std::move(args_in)),
        width(std::move(width_in)),
        precision(std::move(precision_in)) {}
};

struct Index : Expr {
  ExprPtr base;
  std::vector<ExprPtr> indices;
  Index() : Expr(Kind::Index) {}
  Index(Location loc_in, ExprPtr base_in, std::vector<ExprPtr> indices_in)
      : Expr(Kind::Index, loc_in),
        base(std::move(base_in)),
        indices(std::move(indices_in)) {}
};

struct Member : Expr {
  ExprPtr base;
  std::string name;   // lowercased
  Member() : Expr(Kind::Member) {}
  Member(ExprPtr base_in, std::string name_in)
      : Expr(Kind::Member),
        base(std::move(base_in)),
        name(std::move(name_in)) {}
  Member(Location loc_in, ExprPtr base_in, std::string name_in)
      : Expr(Kind::Member, loc_in),
        base(std::move(base_in)),
        name(std::move(name_in)) {}
};

struct Deref : Expr {
  ExprPtr operand;
  Deref() : Expr(Kind::Deref) {}
  Deref(Location loc_in, ExprPtr operand_in)
      : Expr(Kind::Deref, loc_in), operand(std::move(operand_in)) {}
};

struct AddrOf : Expr {
  ExprPtr operand;
  bool double_addr = false;   // @@
  AddrOf() : Expr(Kind::AddrOf) {}
  AddrOf(Location loc_in, bool double_addr_in, ExprPtr operand_in)
      : Expr(Kind::AddrOf, loc_in),
        operand(std::move(operand_in)),
        double_addr(double_addr_in) {}
};

struct SetLit : Expr {
  // Each element is either a plain expr or a range expr (Range).
  std::vector<ExprPtr> elements;
  SetLit() : Expr(Kind::SetLit) {}
  SetLit(Location loc_in, std::vector<ExprPtr> elements_in)
      : Expr(Kind::SetLit, loc_in), elements(std::move(elements_in)) {}
};

struct Range : Expr {
  ExprPtr lo, hi;
  Range() : Expr(Kind::Range) {}
  Range(ExprPtr lo_in, ExprPtr hi_in)
      : Expr(Kind::Range), lo(std::move(lo_in)), hi(std::move(hi_in)) {}
  Range(Location loc_in, ExprPtr lo_in, ExprPtr hi_in)
      : Expr(Kind::Range, loc_in),
        lo(std::move(lo_in)),
        hi(std::move(hi_in)) {}
};

// Typed-constant initializers. Pascal distinguishes these from ordinary
// expressions: only legal on the RHS of `ident : type = ...`.

// `( a, b, c, ... )` -- may nest (2-D array constants).
struct ArrayConst : Expr {
  std::vector<ExprPtr> elements;
  ArrayConst() : Expr(Kind::ArrayConst) {}
  ArrayConst(Location loc_in, std::vector<ExprPtr> elements_in = {})
      : Expr(Kind::ArrayConst, loc_in), elements(std::move(elements_in)) {}
};

// `( fld : val ; fld : val ; ... )`.
struct RecordConst : Expr {
  std::vector<std::pair<std::string, ExprPtr>> fields;
  RecordConst() : Expr(Kind::RecordConst) {}
  RecordConst(Location loc_in,
              std::vector<std::pair<std::string, ExprPtr>> fields_in)
      : Expr(Kind::RecordConst, loc_in), fields(std::move(fields_in)) {}
};

// ---------------------------------------------------------------------------
// Statements

struct Compound : Stmt {
  std::vector<StmtPtr> body;
  Compound() : Stmt(Kind::Compound) {}
  Compound(Location loc_in, std::vector<StmtPtr> body_in = {})
      : Stmt(Kind::Compound, loc_in), body(std::move(body_in)) {}
};

struct Assign : Stmt {
  ExprPtr target;
  ExprPtr value;
  // Snapshot of `{$R+}` / `{$rangechecks+}` at parse time. The
  // emitter inserts a runtime range check on narrowing conversions
  // (real -> int, wider-int -> narrower-int, ...) when set.
  bool r_check = false;
  Assign() : Stmt(Kind::Assign) {}
  Assign(Location loc_in, ExprPtr target_in, ExprPtr value_in,
         bool r_check_in)
      : Stmt(Kind::Assign, loc_in),
        target(std::move(target_in)),
        value(std::move(value_in)),
        r_check(r_check_in) {}
};

struct ExprStmt : Stmt {
  // For a bare `foo;` or `foo(x);` used as a statement.
  ExprPtr expr;
  ExprStmt() : Stmt(Kind::ExprStmt) {}
  ExprStmt(Location loc_in, ExprPtr expr_in)
      : Stmt(Kind::ExprStmt, loc_in), expr(std::move(expr_in)) {}
};

struct If : Stmt {
  ExprPtr cond;
  StmtPtr then_branch;
  StmtPtr else_branch;   // may be null
  If() : Stmt(Kind::If) {}
  If(Location loc_in, ExprPtr cond_in, StmtPtr then_branch_in,
     StmtPtr else_branch_in = nullptr)
      : Stmt(Kind::If, loc_in),
        cond(std::move(cond_in)),
        then_branch(std::move(then_branch_in)),
        else_branch(std::move(else_branch_in)) {}
};

struct While : Stmt {
  ExprPtr cond;
  StmtPtr body;
  While() : Stmt(Kind::While) {}
  While(Location loc_in, ExprPtr cond_in, StmtPtr body_in)
      : Stmt(Kind::While, loc_in),
        cond(std::move(cond_in)),
        body(std::move(body_in)) {}
};

struct Repeat : Stmt {
  std::vector<StmtPtr> body;
  ExprPtr cond;
  Repeat() : Stmt(Kind::Repeat) {}
  Repeat(Location loc_in, std::vector<StmtPtr> body_in, ExprPtr cond_in)
      : Stmt(Kind::Repeat, loc_in),
        body(std::move(body_in)),
        cond(std::move(cond_in)) {}
};

struct For : Stmt {
  std::string var;
  ExprPtr from, to;
  ExprPtr in_expr;
  bool downto = false;
  bool for_in = false;
  StmtPtr body;
  For() : Stmt(Kind::For) {}
  For(Location loc_in, std::string var_in, ExprPtr in_expr_in, StmtPtr body_in)
      : Stmt(Kind::For, loc_in),
        var(std::move(var_in)),
        in_expr(std::move(in_expr_in)),
        for_in(true),
        body(std::move(body_in)) {}
  For(Location loc_in, std::string var_in, ExprPtr from_in, ExprPtr to_in,
      bool downto_in, StmtPtr body_in)
      : Stmt(Kind::For, loc_in),
        var(std::move(var_in)),
        from(std::move(from_in)),
        to(std::move(to_in)),
        downto(downto_in),
        body(std::move(body_in)) {}
};

struct CaseArm {
  // Each label is an expression; range labels use Range nodes.
  std::vector<ExprPtr> labels;
  StmtPtr body;
  CaseArm() = default;
  CaseArm(std::vector<ExprPtr> labels_in, StmtPtr body_in)
      : labels(std::move(labels_in)), body(std::move(body_in)) {}
};

struct CaseStmt : Stmt {
  ExprPtr selector;
  std::vector<CaseArm> arms;
  StmtPtr else_branch;   // may be null; covers `else` / `otherwise`
  CaseStmt() : Stmt(Kind::CaseStmt) {}
  CaseStmt(Location loc_in, ExprPtr selector_in, std::vector<CaseArm> arms_in,
           StmtPtr else_branch_in = nullptr)
      : Stmt(Kind::CaseStmt, loc_in),
        selector(std::move(selector_in)),
        arms(std::move(arms_in)),
        else_branch(std::move(else_branch_in)) {}
};

struct With : Stmt {
  std::vector<ExprPtr> exprs;   // `with A, B do S` is modeled as one node
  StmtPtr body;
  With() : Stmt(Kind::With) {}
  With(Location loc_in, std::vector<ExprPtr> exprs_in, StmtPtr body_in)
      : Stmt(Kind::With, loc_in),
        exprs(std::move(exprs_in)),
        body(std::move(body_in)) {}
};

struct Goto : Stmt {
  std::string label;
  Goto() : Stmt(Kind::Goto) {}
  Goto(Location loc_in, std::string label_in)
      : Stmt(Kind::Goto, loc_in), label(std::move(label_in)) {}
};

struct Labeled : Stmt {
  std::string label;
  StmtPtr body;
  Labeled() : Stmt(Kind::Labeled) {}
  Labeled(Location loc_in, std::string label_in, StmtPtr body_in)
      : Stmt(Kind::Labeled, loc_in),
        label(std::move(label_in)),
        body(std::move(body_in)) {}
};

struct BreakStmt : Stmt {
  BreakStmt() : Stmt(Kind::BreakStmt) {}
  explicit BreakStmt(Location loc_in) : Stmt(Kind::BreakStmt, loc_in) {}
};
struct ContinueStmt : Stmt {
  ContinueStmt() : Stmt(Kind::ContinueStmt) {}
  explicit ContinueStmt(Location loc_in) : Stmt(Kind::ContinueStmt, loc_in) {}
};
struct ExitStmt : Stmt {
  ExprPtr value;   // optional: exit(expr)
  ExitStmt() : Stmt(Kind::ExitStmt) {}
  ExitStmt(Location loc_in, ExprPtr value_in = nullptr)
      : Stmt(Kind::ExitStmt, loc_in), value(std::move(value_in)) {}
};
struct EmptyStmt : Stmt {
  EmptyStmt() : Stmt(Kind::EmptyStmt) {}
  explicit EmptyStmt(Location loc_in) : Stmt(Kind::EmptyStmt, loc_in) {}
};

struct AsmStmt : Stmt {
  // We don't parse asm bodies -- the only one in the compiler proper lives in
  // tpexcept.pas which we replace with a C++ shim. Just capture the raw text
  // for diagnostics.
  std::string raw;
  AsmStmt() : Stmt(Kind::AsmStmt) {}
  AsmStmt(Location loc_in, std::string raw_in = {})
      : Stmt(Kind::AsmStmt, loc_in), raw(std::move(raw_in)) {}
};

// One `on <var>: <Class> do <stmt>' arm inside `try ... except ... end'.
// The variable name is optional (`on EFoo do ...' without bind).
struct ExceptHandler {
  std::string var_name;      // may be empty
  std::string class_name;    // lowercased; empty means catch-all (rare)
  StmtPtr body;
  ExceptHandler() = default;
  ExceptHandler(std::string var_name_in, std::string class_name_in,
                StmtPtr body_in)
      : var_name(std::move(var_name_in)),
        class_name(std::move(class_name_in)),
        body(std::move(body_in)) {}
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
  Try(Location loc_in, std::vector<StmtPtr> body_in, bool is_finally_in,
      std::vector<ExceptHandler> handlers_in, StmtPtr except_else_in,
      std::vector<StmtPtr> finally_body_in)
      : Stmt(Kind::Try, loc_in),
        body(std::move(body_in)),
        is_finally(is_finally_in),
        handlers(std::move(handlers_in)),
        except_else(std::move(except_else_in)),
        finally_body(std::move(finally_body_in)) {}
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
  Raise(Location loc_in, ExprPtr value_in = nullptr)
      : Stmt(Kind::Raise, loc_in), value(std::move(value_in)) {}
};

// ---------------------------------------------------------------------------
// Declarations

struct ConstDecl : Decl {
  std::string name;
  TypePtr type;         // may be null (untyped const)
  ExprPtr value;        // required
  ConstDecl() : Decl(Kind::ConstDecl) {}
  ConstDecl(Location loc_in, std::string name_in, TypePtr type_in,
            ExprPtr value_in)
      : Decl(Kind::ConstDecl, loc_in),
        name(std::move(name_in)),
        type(std::move(type_in)),
        value(std::move(value_in)) {}
};

struct TypeDecl : Decl {
  std::string name;
  TypePtr type;
  TypeDecl() : Decl(Kind::TypeDecl) {}
  TypeDecl(Location loc_in, std::string name_in, TypePtr type_in)
      : Decl(Kind::TypeDecl, loc_in),
        name(std::move(name_in)),
        type(std::move(type_in)) {}
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
  VarDecl(Location loc_in, std::vector<std::string> names_in, TypePtr type_in,
          ExprPtr init_in, bool is_absolute_in, bool is_external_in,
          std::string absolute_target_in, ExprPtr external_name_in,
          std::string external_lib_in)
      : Decl(Kind::VarDecl, loc_in),
        names(std::move(names_in)),
        type(std::move(type_in)),
        init(std::move(init_in)),
        is_absolute(is_absolute_in),
        is_external(is_external_in),
        absolute_target(std::move(absolute_target_in)),
        external_name(std::move(external_name_in)),
        external_lib(std::move(external_lib_in)) {}
};

struct LabelDecl : Decl {
  std::vector<std::string> labels;
  LabelDecl() : Decl(Kind::LabelDecl) {}
  LabelDecl(Location loc_in, std::vector<std::string> labels_in)
      : Decl(Kind::LabelDecl, loc_in), labels(std::move(labels_in)) {}
};

struct Param {
  enum Mode { Value, Var, Const, ConstRef, Out } mode = Value;
  std::vector<std::string> names;
  TypePtr type;        // may be null for untyped `var`/`const` params
  ExprPtr default_value;  // optional
  Param() = default;
  Param(Mode mode_in, std::vector<std::string> names_in, TypePtr type_in,
        ExprPtr default_value_in = nullptr)
      : mode(mode_in),
        names(std::move(names_in)),
        type(std::move(type_in)),
        default_value(std::move(default_value_in)) {}
};

enum class ProcKind : uint8_t { Procedure, Function, Constructor, Destructor };

struct ProcDecl : Decl {
  ProcKind pkind = ProcKind::Procedure;
  std::string name;
  bool is_operator = false;
  // Pascal operator token text, e.g. "+", "div", ":=".  Operator
  // declarations are represented as function-like ProcDecls so they can
  // reuse the existing parameter/body/result lowering.
  std::string operator_token;
  enum class IntrinsicOperator { None, StringCompare };
  IntrinsicOperator intrinsic_operator = IntrinsicOperator::None;
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
  bool is_final = false;
  bool is_forward = false;
  bool is_inline = false;
  bool is_cdecl = false;
  bool is_noreturn = false;
  bool is_external = false;
  bool is_assembler = false;
  std::string external_lib;    // when is_external
  std::string external_name;   // when is_external; may be empty
  // Body (present only for the implementation-side definition):
  std::vector<DeclPtr> locals;
  StmtPtr body;                // null for forward / abstract / external
  explicit ProcDecl(bool class_method)
      : Decl(Kind::ProcDecl), is_class_method(class_method) {}
  ProcDecl(Location loc_in, ProcKind pkind_in, std::string name_in,
           bool is_operator_in, std::string operator_token_in,
           IntrinsicOperator intrinsic_operator_in, std::string of_type_in,
           bool is_class_method_in, std::vector<Param> params_in,
           TypePtr return_type_in, bool is_virtual_in, bool is_abstract_in,
           bool is_override_in, bool is_final_in, bool is_forward_in,
           bool is_inline_in, bool is_cdecl_in, bool is_noreturn_in,
           bool is_external_in, bool is_assembler_in,
           std::string external_lib_in, std::string external_name_in,
           std::vector<DeclPtr> locals_in, StmtPtr body_in)
      : Decl(Kind::ProcDecl, loc_in),
        pkind(pkind_in),
        name(std::move(name_in)),
        is_operator(is_operator_in),
        operator_token(std::move(operator_token_in)),
        intrinsic_operator(intrinsic_operator_in),
        of_type(std::move(of_type_in)),
        is_class_method(is_class_method_in),
        params(std::move(params_in)),
        return_type(std::move(return_type_in)),
        is_virtual(is_virtual_in),
        is_abstract(is_abstract_in),
        is_override(is_override_in),
        is_final(is_final_in),
        is_forward(is_forward_in),
        is_inline(is_inline_in),
        is_cdecl(is_cdecl_in),
        is_noreturn(is_noreturn_in),
        is_external(is_external_in),
        is_assembler(is_assembler_in),
        external_lib(std::move(external_lib_in)),
        external_name(std::move(external_name_in)),
        locals(std::move(locals_in)),
        body(std::move(body_in)) {}
};

struct UsesClause : Decl {
  std::vector<std::string> units;
  UsesClause() : Decl(Kind::UsesClause) {}
  UsesClause(Location loc_in, std::vector<std::string> units_in)
      : Decl(Kind::UsesClause, loc_in), units(std::move(units_in)) {}
};

// ---------------------------------------------------------------------------
// Types

struct TyName : TypeExpr {
  std::string name;
  // `^TFoo` style pointer-to-not-yet-defined named types become TyPointer
  // with inner=TyName.
  TyName() : TypeExpr(Kind::TyName) {}
  explicit TyName(std::string name_in)
      : TypeExpr(Kind::TyName), name(std::move(name_in)) {}
  TyName(Location loc_in, std::string name_in)
      : TypeExpr(Kind::TyName, loc_in), name(std::move(name_in)) {}
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
  TyArray(Location loc_in, std::vector<TypePtr> dims_in, TypePtr element_in,
          bool is_packed_in, ArrayKind array_kind_in)
      : TypeExpr(Kind::TyArray, loc_in),
        dims(std::move(dims_in)),
        element(std::move(element_in)),
        is_packed(is_packed_in),
        array_kind(array_kind_in) {}
};

struct RecordField {
  std::vector<std::string> names;
  TypePtr type;
  RecordField() = default;
  RecordField(std::vector<std::string> names_in, TypePtr type_in)
      : names(std::move(names_in)), type(std::move(type_in)) {}
};

// Variant-record tail: `case Tag : T of lit,lit : (fields...); ...`
struct VariantCase;

struct VariantPart {
  std::string tag_name;
  TypePtr tag_type;
  std::vector<VariantCase> cases;
};

struct VariantCase {
  std::vector<ExprPtr> labels;
  std::vector<RecordField> fields;
  std::shared_ptr<VariantPart> variant_part;
  
  VariantCase() = default;
  VariantCase(std::vector<ExprPtr> labels_in,
              std::vector<RecordField> fields_in)
      : labels(std::move(labels_in)), fields(std::move(fields_in)) {}
  VariantCase(std::vector<ExprPtr> labels_in,
              std::vector<RecordField> fields_in,
              std::shared_ptr<VariantPart> variant_part_in)
      : labels(std::move(labels_in)), fields(std::move(fields_in)),
        variant_part(std::move(variant_part_in)) {}
};

struct TyRecord : TypeExpr {
  std::vector<RecordField> fields;
  // Optional variant part:
  std::shared_ptr<VariantPart> variant_part;
  bool is_packed = false;
  TyRecord() : TypeExpr(Kind::TyRecord) {}
  TyRecord(Location loc_in, std::vector<RecordField> fields_in,
           std::shared_ptr<VariantPart> variant_part_in, bool is_packed_in)
      : TypeExpr(Kind::TyRecord, loc_in),
        fields(std::move(fields_in)),
        variant_part(std::move(variant_part_in)),
        is_packed(is_packed_in) {}
};

enum class Visibility : uint8_t {
  Public,
  Private,
  Protected,
  StrictPrivate,
  StrictProtected,
};

enum class ObjectMemberKind : uint8_t { Field, Method, Property };

struct PropertyDecl {
  struct Accessor {
    std::vector<std::string> path;

    Accessor() = default;
    explicit Accessor(std::vector<std::string> path_in)
        : path(std::move(path_in)) {}
    bool empty() const { return path.empty(); }
  };

  std::string name;
  std::vector<Param> params;
  TypePtr type;
  Accessor read_accessor;
  Accessor write_accessor;
  bool is_default = false;
  PropertyDecl() = default;
  PropertyDecl(std::string name_in, std::vector<Param> params_in,
               TypePtr type_in, Accessor read_accessor_in,
               Accessor write_accessor_in, bool is_default_in)
      : name(std::move(name_in)),
        params(std::move(params_in)),
        type(std::move(type_in)),
        read_accessor(std::move(read_accessor_in)),
        write_accessor(std::move(write_accessor_in)),
        is_default(is_default_in) {}
};

struct ObjectMember {
  // One of: field (names+type) | method (ProcDecl) | property (PropertyDecl)
  Location loc;
  Visibility vis = Visibility::Public;
  ObjectMemberKind kind = ObjectMemberKind::Field;
  // field side
  std::vector<std::string> field_names;
  TypePtr field_type;
  bool is_class_var = false;
  // method side -- typed so the "it's a ProcDecl" invariant is
  // enforced at the type level rather than via documented faith.
  std::shared_ptr<ProcDecl> method;
  // property side
  PropertyDecl property;
  ObjectMember() = default;
  ObjectMember(Location loc_in, Visibility vis_in,
               std::vector<std::string> field_names_in, TypePtr field_type_in,
               bool is_class_var_in)
      : loc(loc_in),
        vis(vis_in),
        kind(ObjectMemberKind::Field),
        field_names(std::move(field_names_in)),
        field_type(std::move(field_type_in)),
        is_class_var(is_class_var_in) {}
  ObjectMember(Location loc_in, Visibility vis_in,
               std::shared_ptr<ProcDecl> method_in)
      : loc(loc_in),
        vis(vis_in),
        kind(ObjectMemberKind::Method),
        method(std::move(method_in)) {}
  ObjectMember(Location loc_in, Visibility vis_in, PropertyDecl property_in)
      : loc(loc_in),
        vis(vis_in),
        kind(ObjectMemberKind::Property),
        property(std::move(property_in)) {}
};

struct TyObject : TypeExpr {
  std::string parent;              // base object type name; empty if none
  std::vector<std::string> interfaces;
  std::vector<ObjectMember> members;
  // `false`: TP-style `object` -- value type, stack-OK, `new(p, init)` to
  // heap-allocate, destructor via `dispose(p, done)`.
  // `true`: Delphi-style `class` -- reference type, always heap, implicit
  // TObject ancestor, constructor returns a pointer via `TFoo.Create(...)`,
  // destruction via `.Free` (null-safe at the call site).
  bool is_reference_type = false;
  // Set by `class abstract`; direct constructor calls use it for diagnostics.
  bool is_abstract = false;
  // Delphi-style forward declaration `T = class;' -- the body comes
  // later in the same type block (or same unit section). A true forward
  // declaration carries no members and no parent; `T = class(Base);` is a
  // complete empty class declaration instead, not a forward.
  bool is_forward = false;
  TyObject() : TypeExpr(Kind::TyObject) {}
  TyObject(Location loc_in, std::string parent_in,
           std::vector<std::string> interfaces_in,
           std::vector<ObjectMember> members_in, bool is_reference_type_in,
           bool is_abstract_in, bool is_forward_in)
      : TypeExpr(Kind::TyObject, loc_in),
        parent(std::move(parent_in)),
        interfaces(std::move(interfaces_in)),
        members(std::move(members_in)),
        is_reference_type(is_reference_type_in),
        is_abstract(is_abstract_in),
        is_forward(is_forward_in) {}
};

struct TyInterface : TypeExpr {
  std::string metadata_string;
  std::vector<ObjectMember> members;
  TyInterface() : TypeExpr(Kind::TyInterface) {}
  TyInterface(Location loc_in, std::string metadata_string_in,
              std::vector<ObjectMember> members_in)
      : TypeExpr(Kind::TyInterface, loc_in),
        metadata_string(std::move(metadata_string_in)),
        members(std::move(members_in)) {}
};

struct TySet : TypeExpr {
  TypePtr element;
  // Declared Pascal set types take their bounds from the ordinal element
  // type (`set of byte` -> 0..255). Untyped set constants may carry a
  // narrower actual element range (`[rs_r0..rs_r3]` -> 0..3). Keep those
  // explicit bounds here so later compatibility checks can follow Pascal's
  // setbase/setmax rules instead of guessing from the emitted C++ element
  // type alone.
  bool has_explicit_bounds = false;
  int64_t explicit_low = 0;
  int64_t explicit_high = 0;
  TySet() : TypeExpr(Kind::TySet) {}
  TySet(Location loc_in, TypePtr element_in, bool has_explicit_bounds_in = false,
        int64_t explicit_low_in = 0, int64_t explicit_high_in = 0)
      : TypeExpr(Kind::TySet, loc_in),
        element(std::move(element_in)),
        has_explicit_bounds(has_explicit_bounds_in),
        explicit_low(explicit_low_in),
        explicit_high(explicit_high_in) {}
};

struct TyFile : TypeExpr {
  TypePtr element;   // null for untyped `file`; special marker for `text`
  bool is_text = false;
  TyFile() : TypeExpr(Kind::TyFile) {}
  TyFile(Location loc_in, TypePtr element_in, bool is_text_in = false)
      : TypeExpr(Kind::TyFile, loc_in),
        element(std::move(element_in)),
        is_text(is_text_in) {}
};

struct TyPointer : TypeExpr {
  TypePtr target;
  TyPointer() : TypeExpr(Kind::TyPointer) {}
  TyPointer(Location loc_in, TypePtr target_in)
      : TypeExpr(Kind::TyPointer, loc_in), target(std::move(target_in)) {}
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
  TyDistinct(Location loc_in, TypePtr underlying_in)
      : TypeExpr(Kind::TyDistinct, loc_in),
        underlying(std::move(underlying_in)) {}
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
  TyMetaclass(Location loc_in, std::string class_name_in)
      : TypeExpr(Kind::TyMetaclass, loc_in),
        class_name(std::move(class_name_in)) {}
};

struct TyProcedural : TypeExpr {
  bool is_function = false;
  std::vector<Param> params;
  TypePtr return_type;    // for function types
  bool is_cdecl = false;
  bool is_method = false; // `procedure/function ... of object`
  TyProcedural() : TypeExpr(Kind::TyProcedural) {}
  TyProcedural(Location loc_in, bool is_function_in,
               std::vector<Param> params_in, TypePtr return_type_in,
               bool is_cdecl_in, bool is_method_in)
      : TypeExpr(Kind::TyProcedural, loc_in),
        is_function(is_function_in),
        params(std::move(params_in)),
        return_type(std::move(return_type_in)),
        is_cdecl(is_cdecl_in),
        is_method(is_method_in) {}
};

struct EnumMember {
  std::string name;
  ExprPtr value;  // optional explicit ordinal value
  EnumMember() = default;
  explicit EnumMember(std::string name_in) : name(std::move(name_in)) {}
  EnumMember(std::string name_in, ExprPtr value_in)
      : name(std::move(name_in)), value(std::move(value_in)) {}
};

struct TyEnum : TypeExpr {
  // Active `{$PACKENUM n}` / `{$MINENUMSIZE n}` / `{$Zn}` at the point this
  // enum type was declared. FPC defaults to 4-byte enum storage outside TP
  // mode; smaller values request the minimum storage width, subject to the
  // enum's signed/unsigned ordinal range.
  uint8_t packenum = 4;
  std::vector<EnumMember> members;
  TyEnum() : TypeExpr(Kind::TyEnum) {}
  TyEnum(Location loc_in, uint8_t packenum_in,
         std::vector<EnumMember> members_in)
      : TypeExpr(Kind::TyEnum, loc_in),
        packenum(packenum_in),
        members(std::move(members_in)) {}
};

struct TySubrange : TypeExpr {
  ExprPtr lo, hi;
  TySubrange() : TypeExpr(Kind::TySubrange) {}
  TySubrange(Location loc_in, ExprPtr lo_in, ExprPtr hi_in)
      : TypeExpr(Kind::TySubrange, loc_in),
        lo(std::move(lo_in)),
        hi(std::move(hi_in)) {}
};

struct TyString : TypeExpr {
  ExprPtr max_length;   // null for unsized `string`
  TyString() : TypeExpr(Kind::TyString) {}
  TyString(Location loc_in, ExprPtr max_length_in = nullptr)
      : TypeExpr(Kind::TyString, loc_in),
        max_length(std::move(max_length_in)) {}
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
  UnitNode(Location loc_in, std::string name_in,
           std::vector<std::string> interface_uses_in,
           std::vector<DeclPtr> interface_decls_in,
           std::vector<std::string> impl_uses_in,
           std::vector<DeclPtr> impl_decls_in, StmtPtr init_body_in,
           StmtPtr final_body_in, bool is_program_in)
      : Node(Kind::UnitNode, loc_in),
        name(std::move(name_in)),
        interface_uses(std::move(interface_uses_in)),
        interface_decls(std::move(interface_decls_in)),
        impl_uses(std::move(impl_uses_in)),
        impl_decls(std::move(impl_decls_in)),
        init_body(std::move(init_body_in)),
        final_body(std::move(final_body_in)),
        is_program(is_program_in) {}
};

}  // namespace tp2cc::ast
