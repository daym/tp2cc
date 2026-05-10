#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tp2cc::ast {
struct ProcDecl;
struct TypeExpr;
}  // namespace tp2cc::ast

namespace tp2cc {

// Result of Pascal identifier lookup. The emitter uses this both for spelling
// (`cxx`) and for semantic questions like "is this a zero-arg callable?".
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
  UnitType,     // named type exported from a visible Pascal unit
  EnumMember,   // enum value found through Pascal enum visibility
  RtBuiltin,    // runtime helper tracked without a Pascal AST decl
};

struct ResolveResult {
  ResolvedKind kind = ResolvedKind::Unknown;
  std::string cxx;              // fully spelled C++ access text
  bool is_parameterless = false;         // callable with zero explicit args
  bool is_callable = false;              // name denotes something invocable
  const ast::ProcDecl* proc = nullptr;   // declaration for call-site lowering
  bool accepts_zero_args = false;        // rt builtin or decl permits zero args
  std::string return_type_name;          // Pascal-facing return type alias/name
  std::string default_arg_unit;          // declaration scope for defaults
};

// Empty qualifier means ordinary lexical lookup. Non-empty qualifiers are
// Pascal unit names or class / record aliases.
enum class QualifierKind { None, Unit, Class };

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

// Pascal/FPC overload-resolution conversion ranks. Lower is better.
// `NotViable` means no implicit conversion exists.
//
//   rank | name                    | example
//   -----+-------------------------+----------------------------------------
//    1   | Exact                   | tidstring -> tidstring (same canonical)
//    2   | Equal                   | TSubrangeInt -> Integer (same underlying)
//    3   | SetCompatible           | set of 0..7 -> set of byte
//    4   | ClassHierarchy          | TButton -> TControl
//    5   | IntWideningSameSign     | byte -> word -> longint
//    6   | RealWidening            | single -> double -> extended
//    7   | StringSameTagWiden      | ShortString<N> -> ShortString<M>, M >= N
//    8   | StringToShortString     | Char/PChar/AnsiString -> ShortString
//    9   | StringToAnsiString      | Char/PChar/ShortString -> AnsiString;
//        |                         | ShortString/AnsiString -> PChar
//   10   | OrdinalSignChange       | longint -> longword (or back)
//   11   | IntNarrowing            | longint -> shortint, etc.
//   12   | Operator                | FPC operator := conversion
//   13   | Variant                 | anything <-> variant
//    -   | NotViable               | no implicit conversion exists
//
// Ranks 7 vs 8 stay split because the bootstrap compiler runs under `{$H-}`
// semantics, where `string` aliases ShortString and Pascal prefers
// ShortString-typed parameters over AnsiString-typed ones when both are
// otherwise tied.
enum class ConvRank : uint8_t {
  Exact = 1,
  Equal = 2,
  SetCompatible = 3,
  ClassHierarchy = 4,
  IntWideningSameSign = 5,
  RealWidening = 6,
  StringSameTagWiden = 7,
  StringToShortString = 8,
  StringToAnsiString = 9,
  OrdinalSignChange = 10,
  IntNarrowing = 11,
  Operator = 12,
  Variant = 13,
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
  // needs to rebuild member or unit-qualified call spellings.
  std::string member_name;
  // Default parameter expressions are lowered at the call site, but
  // unqualified names inside them resolve in the declaration's unit.
  std::string default_arg_unit;
  // True iff the resolver had to pick among multiple arity-viable candidates
  // by Pascal conversion ranking. The call site then wraps each value arg in
  // `static_cast<param_type>(...)` so C++ overload resolution lands on the
  // same overload Pascal picked.
  bool needs_arg_casts = false;
  // Set when two or more arity-viable candidates were mutually incomparable.
  // Call emission must surface a Pascal ambiguity instead of silently picking
  // one candidate.
  bool ambiguous = false;
};

class ResolutionTypeOps {
 public:
  virtual ~ResolutionTypeOps() = default;
  // Minimal type services the call resolver needs without depending on the
  // whole emitter implementation.
  virtual std::string type_to_cxx(const ast::TypeExpr& t) = 0;
  virtual bool type_is_pcharish(const ast::TypeExpr* t) = 0;
};

}  // namespace tp2cc
