#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tp2cc::ast {
struct Call;
struct Expr;
struct Param;
struct ProcDecl;
struct TypeExpr;
}  // namespace tp2cc::ast

namespace tp2cc {

struct TypeLookupContext;
struct ProcInfo;
struct TypeSymbol;

// Result of Pascal identifier lookup. The emitter uses this both for C++ access
// text (`cxx`) and for semantic questions like "is this a zero-arg callable?".
enum class ResolvedKind {
  Unknown,      // emit the mangled name; let later lowering decide
  ResultSlot,   // Pascal function name used as its implicit result variable
  Local,        // local/parameter/typed-const binding
  NestedFn,     // parameterless nested function value
  WithField,    // field visible through a `with` binding
  WithMethod,   // method visible through a `with` binding
  WithProperty, // property visible through a `with` binding
  ClassField,   // field of current class / ancestor
  ClassMethod,  // method of current class / ancestor
  ClassProperty,// property of current class / ancestor
  UnitVar,      // variable exported from a visible Pascal unit
  UnitConst,    // constant exported from a visible Pascal unit
  UnitProc,     // procedure/function exported from a visible Pascal unit
  EnumMember,   // enum value found through Pascal enum visibility
  RtBuiltin,    // runtime helper tracked without a Pascal AST decl
};

struct ResolveResult {
  struct BytewiseWithField {
    const ast::TypeExpr* field_type;
    std::string base_ptr_cxx;
    std::string base_type_cxx;
    std::string field_cxx;
    bool unaligned;

    BytewiseWithField(const ast::TypeExpr* field_type_in,
                      std::string base_ptr_cxx_in,
                      std::string base_type_cxx_in, std::string field_cxx_in,
                      bool unaligned_in)
        : field_type(field_type_in),
          base_ptr_cxx(std::move(base_ptr_cxx_in)),
          base_type_cxx(std::move(base_type_cxx_in)),
          field_cxx(std::move(field_cxx_in)),
          unaligned(unaligned_in) {}
  };

  ResolvedKind kind = ResolvedKind::Unknown;
  std::string cxx;              // complete C++ access expression text
  bool is_parameterless = false;         // callable with zero explicit args
  bool is_callable = false;              // name denotes something invocable
  const ast::ProcDecl* proc = nullptr;   // declaration for call-site lowering
  bool accepts_zero_args = false;        // rt builtin or decl permits zero args
  std::string default_arg_unit;          // declaration scope for defaults
  // Metadata for a bare field resolved through `with` where the `with`
  // receiver is byte-addressed storage. The ordinary `cxx` expression cannot
  // name such a field without first manufacturing a C++ aggregate reference.
  std::optional<BytewiseWithField> bytewise_with_field;

  ResolveResult() = default;
  ResolveResult(ResolvedKind kind_in, std::string cxx_in)
      : kind(kind_in),
        cxx(std::move(cxx_in)) {}

  static ResolveResult callable(ResolvedKind kind, std::string cxx,
                                bool is_parameterless,
                                const ast::ProcDecl* proc,
                                bool accepts_zero_args,
                                std::string default_arg_unit) {
    return ResolveResult(kind, std::move(cxx), is_parameterless, true, proc,
                         accepts_zero_args, std::move(default_arg_unit),
                         std::nullopt);
  }

  static ResolveResult bytewise_with_field_result(
      const ast::TypeExpr* field_type, std::string base_ptr_cxx,
      std::string base_type_cxx, std::string field_cxx, bool unaligned) {
    return ResolveResult(
        ResolvedKind::WithField, {}, false, false, nullptr, false, {},
        BytewiseWithField(field_type, std::move(base_ptr_cxx),
                          std::move(base_type_cxx), std::move(field_cxx),
                          unaligned));
  }

 private:
  ResolveResult(ResolvedKind kind_in, std::string cxx_in,
                bool is_parameterless_in, bool is_callable_in,
                const ast::ProcDecl* proc_in, bool accepts_zero_args_in,
                std::string default_arg_unit_in,
                std::optional<BytewiseWithField> bytewise_with_field_in)
      : kind(kind_in),
        cxx(std::move(cxx_in)),
        is_parameterless(is_parameterless_in),
        is_callable(is_callable_in),
        proc(proc_in),
        accepts_zero_args(accepts_zero_args_in),
        default_arg_unit(std::move(default_arg_unit_in)),
        bytewise_with_field(std::move(bytewise_with_field_in)) {}
};

// Empty qualifier means ordinary lexical lookup. Non-empty qualifiers are
// Pascal unit names. Type-qualified member access is emitted from the
// bound TypeSymbol/ClassInfo instead of resolving a source qualifier
// here.
enum class QualifierKind { None, Unit };

class ResolveNameProvider {
 public:
  virtual ~ResolveNameProvider() = default;
  // One Pascal name-resolution entry point. Every emit path that needs symbol
  // meaning should come through this instead of open-coding its own lookup
  // order.
  virtual ResolveResult resolve_name(const std::string& name,
                                     QualifierKind qk = QualifierKind::None,
                                     const std::string& qualifier = {}) = 0;
};

class OverloadTypeProvider {
 public:
  virtual ~OverloadTypeProvider() = default;
  // Overload ranking may need the selected result type of a nested
  // call/operator expression. That information belongs to expression lowering;
  // structural type deduction must not choose overloads.
  virtual const ast::TypeExpr* type_for_overload(const ast::Expr& e) = 0;
};

class CallTypeProvider {
 public:
  virtual ~CallTypeProvider() = default;
  virtual const ast::TypeExpr* type_for_resolved_call(
      const ast::Call& call) = 0;
};

// Pascal/FPC overload-resolution conversion ranks. Lower is better.
// `NotViable` means no implicit conversion exists.
//
//   rank | name                    | example
//   -----+-------------------------+----------------------------------------
//    1   | Exact                   | tidstring -> tidstring (same canonical)
//    2   | Equal                   | TSubrangeInt -> Integer (same underlying)
//    3   | SetCompatible           | set of 0..7 -> set of byte
//    4   | ClassHierarchy          | TButton -> TControl
//    5   | IntDomainCompatible     | 0..255 -> byte/cardinal/longint
//    6   | IntWideningSameSign     | byte -> word -> qword
//    7   | RealWidening            | single -> double -> extended
//    8   | RealNarrowing           | extended -> double -> single
//    9   | StringSameTagWiden      | ShortString<N> -> ShortString<M>, M >= N
//   10   | StringToShortString     | Char/PChar/AnsiString -> ShortString
//   11   | StringToAnsiString      | Char/PChar/ShortString -> AnsiString;
//        |                         | ShortString/AnsiString -> PChar
//   12   | OrdinalSignChange       | longint -> longword (or back)
//   13   | IntNarrowing            | longint -> shortint, etc.
//   14   | IntegerToReal           | longint -> real/double/extended
//   15   | PointerValueConversion  | ^T -> Pointer, fixed char array -> PChar
//   16   | ClassValueConversion    | TChild identifier -> TClass/class of TBase
//   17   | Operator                | FPC operator := conversion
//   18   | Variant                 | anything <-> variant
//    -   | NotViable               | no implicit conversion exists
//
// The ShortString and AnsiString ranks stay split because the bootstrap compiler
// runs under `{$H-}` semantics, where `string` aliases ShortString and Pascal
// prefers ShortString-typed parameters over AnsiString-typed ones when both are
// otherwise tied.
enum class ConvRank : uint8_t {
  Exact = 1,
  Equal = 2,
  SetCompatible = 3,
  ClassHierarchy = 4,
  IntDomainCompatible = 5,
  IntWideningSameSign = 6,
  RealWidening = 7,
  RealNarrowing = 8,
  StringSameTagWiden = 9,
  StringToShortString = 10,
  StringToAnsiString = 11,
  OrdinalSignChange = 12,
  IntNarrowing = 13,
  IntegerToReal = 14,
  // Pascal value conversions among concrete pointer-compatible types. This is
  // viable for calls, but worse than an exact declared pointer type match.
  PointerValueConversion = 15,
  // Class identifiers/class aliases as values passed to TClass/class-of slots.
  ClassValueConversion = 16,
  Operator = 17,
  Variant = 18,
  NotViable = 255,
};

struct ConvScore {
  ConvRank rank = ConvRank::NotViable;
  // Tie-breaker within one rank. Smaller means a closer fit, e.g. `byte`
  // prefers `cardinal` over `qword` among widening candidates.
  int distance = 0;
  bool viable() const { return rank != ConvRank::NotViable; }
};

// Result of overload picking. `ambiguous=true` means the candidates were
// mutually incomparable, so the caller must diagnose a Pascal-level error.
struct PickResult {
  const ast::ProcDecl* decl = nullptr;
  bool ambiguous = false;
};

// Flattened formal parameter slot used by overload ranking and default
// argument expansion. A Pascal declaration with repeated names contributes one
// row per call-site argument position.
struct FlatCallParamInfo {
  const ast::TypeExpr* type = nullptr;
  std::shared_ptr<const ast::TypeExpr> owned_type;
  const TypeLookupContext* type_context = nullptr;
  bool untyped = false;
  bool mutable_ref = false;
  const ast::Expr* default_value = nullptr;
  std::string param_unit;
  std::string param_declaring_type;
  FlatCallParamInfo(const ast::TypeExpr* type_in, bool untyped_in,
                    bool mutable_ref_in, const ast::Expr* default_value_in,
                    std::string param_unit_in = {},
                    std::string param_declaring_type_in = {},
                    std::shared_ptr<const ast::TypeExpr> owned_type_in = {},
                    const TypeLookupContext* type_context_in = nullptr)
      : type(type_in),
        owned_type(std::move(owned_type_in)),
        type_context(type_context_in),
        untyped(untyped_in),
        mutable_ref(mutable_ref_in),
        default_value(default_value_in),
        param_unit(std::move(param_unit_in)),
        param_declaring_type(std::move(param_declaring_type_in)) {
    if (owned_type) type = owned_type.get();
  }
};

enum class ResolvedCalleeKind {
  Default,            // emitter uses ordinary expr/callee logic
  FreeFunctionInUnit, // emitter uses <unit_ns>::<mangled_name>
};

struct ResolvedCall {
  const ast::ProcDecl* decl = nullptr;
  ResolvedCalleeKind callee_kind = ResolvedCalleeKind::Default;
  // For `FreeFunctionInUnit`: the unit that owns `decl`. This drives the
  // namespace prefix in the final emitted C++ callee text.
  std::string defining_unit;
  // Pascal-side unmangled callee/member name. The printer uses this when it
  // needs to rebuild member or unit-qualified call text.
  std::string member_name;
  // Default parameter expressions are lowered at the call site, but
  // unqualified names inside them resolve in the declaration's unit.
  std::string default_arg_unit;
  // Metadata-only runtime helpers have no ProcDecl, so build carries the
  // resolved result symbol here for type analysis.
  const TypeSymbol* return_type_symbol = nullptr;
  // The selected callable record. This is needed for metadata-only runtime
  // procedures because their writable/untyped parameter contracts have no
  // ProcDecl params to inspect.
  const ProcInfo* proc_info = nullptr;
  // True iff the resolver had to pick among multiple arity-viable candidates
  // by Pascal conversion ranking. The call site then wraps each value arg in
  // `static_cast<param_type>(...)` so C++ overload resolution lands on the
  // same overload Pascal picked.
  bool needs_arg_casts = false;
  // True for a single arity-matching declaration that did not go through
  // overload ranking. Arity alone is not a Pascal call, so call emission still
  // validates each actual against the chosen formal conversion rules.
  bool needs_arg_validation = false;
  // Set when two or more arity-viable candidates were mutually incomparable.
  // Call emission must report a Pascal ambiguity instead of silently picking one
  // candidate.
  bool ambiguous = false;
  // Set when a callable name was found but no arity-matching declaration accepts
  // the actual argument types.
  bool no_match = false;
};

class ResolutionTypeOps {
 public:
  virtual ~ResolutionTypeOps() = default;
  // Minimal type services the call resolver needs without depending on the
  // whole emitter implementation.
  virtual std::string type_to_cxx(const ast::TypeExpr& t) = 0;
  virtual bool type_is_pcharish(const ast::TypeExpr* t) = 0;
  virtual bool type_is_stringish(const ast::TypeExpr* t) = 0;
  // True for any type that lowers to a C++ pointer (typed pointers,
  // procedural variables, reference-class instances, the pchar family,
  // etc.). The picker uses it to recognize Pascal nil-compatible slots
  // without re-listing the cases at the call site.
  virtual bool type_is_pointerish(const ast::TypeExpr* t) = 0;
  virtual bool expr_is_storage_lvalue(const ast::Expr& e) = 0;
  virtual bool fixed_char_array_value_can_decay_to_pchar(
      const ast::TypeExpr* src_type, const ast::TypeExpr* dst_type) = 0;
  virtual bool pointer_value_conversion_is_valid(
      const ast::TypeExpr* dst_type, const ast::TypeExpr* src_type,
      bool explicit_pascal_cast) = 0;
  // Method binding compares the real Pascal formal surface. Procvar carrier
  // signatures may intentionally normalize pointer-like value parameters for
  // safe indirect C++ calls, but overload/method identity must not collapse
  // `TObject` and `Pointer` into the same signature.
  virtual std::string formal_param_types_to_cxx(
      const std::vector<ast::Param>& params) = 0;
  virtual bool procedural_param_uses_pointer_carrier(
      const ast::Param& param) = 0;
  virtual std::string procedural_param_type_to_cxx(
      const ast::Param& param) = 0;
};

}  // namespace tp2cc
