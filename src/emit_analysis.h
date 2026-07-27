#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "emit_context.h"
#include "emit_resolution_types.h"
#include "emit_support.h"
#include "target_info.h"

namespace tp2cc {

struct ClassInfo;
struct ConstInfo;
struct EnumMemberInfo;
struct EnumInfoReg;
struct InterfaceInfo;
struct PropertyInfo;
struct TypeRegistry;
struct TypeSymbol;
struct UnitInfo;
struct VarInfo;

struct ConstIntExprInfo {
  int64_t value = 0;
  uint64_t bits = 0;
  const PrimitiveInfo* type = nullptr;

  ConstIntExprInfo() = default;
  ConstIntExprInfo(int64_t value_in, const PrimitiveInfo* type_in)
      : value(value_in), bits(static_cast<uint64_t>(value_in)), type(type_in) {}
  ConstIntExprInfo(int64_t value_in, uint64_t bits_in,
                   const PrimitiveInfo* type_in)
      : value(value_in), bits(bits_in), type(type_in) {}
};

// Value produced by converting a Pascal integer expression into a specific
// destination type. `value` is the signed interpretation, `bits` preserves the
// exact destination bit pattern for unsigned and wraparound-sensitive cases.
struct ConvertedConstInt {
  int64_t value = 0;
  uint64_t bits = 0;
  const PrimitiveInfo* type = nullptr;
};

// Result of implicit class-property lookup in the current Pascal scope.
// `base_cxx` is the already-resolved receiver expression (`self`, a `with`
// binding, etc.), so callers can lower reads/writes without rediscovering the
// access path.
struct ImplicitPropertyLookup {
  const PropertyInfo* prop = nullptr;
  std::string base_cxx;
  std::string base_access;
  bool from_with = false;
};

struct UnitQualifiedMemberLookup {
  std::string unit_name;
  std::string member_name;
  ResolveResult resolved;
};

enum class SetConversionKind : uint8_t {
  Incompatible,
  Exact,
  Compatible,
};

enum class OrdinalFamily : uint8_t {
  Invalid,
  Integer,
  Boolean,
  Char,
  WideChar,
  Enum,
};

struct OrdinalDomain {
  OrdinalFamily family = OrdinalFamily::Invalid;
  int64_t low = 0;
  int64_t high = 0;
  const TypeDescriptor* enum_key = nullptr;
};

class EmitAnalysis {
 public:
  EmitAnalysis(const TypeRegistry& registry, ScopeStateView& scope,
               ResolveNameProvider& resolve_name_provider,
               CallTypeProvider& call_type_provider, TargetInfo target);

  std::string_view current_unit_name() const { return scope_.current_unit_name; }

  // Resolve a primitive integer type's bit width.  For pointer-sized
  // primitives (ptrint/ptruint/sizeint/sizeuint), returns the target
  // pointer width; for everything else, returns the fixed table width.
  [[nodiscard]] uint8_t resolved_primitive_bits(const PrimitiveInfo& info) const;

  // Descriptor-first semantic shape. Use this for predicates that need the
  // resolved type body (array, record, set, pointer, primitive, class/interface
  // kind). Do not use it to choose emitted C++ carrier spelling.
  const ast::TypeExpr* semantic_shape_type(const ast::TypeExpr* t);
  const ast::TypeExpr* semantic_shape_type_in_context(
      const ast::TypeExpr* t, const TypeLookupContext* context);

  // Parameter ABI policy: decide when Pascal `const` aggregates still need
  // reference lowering so C++ preserves aliasing/mutability semantics.
  bool const_param_needs_mutable_ref(const ast::TypeExpr* t);
  bool const_param_needs_const_ref(const ast::TypeExpr* t);
  // Descriptor-backed TypeExpr queries. Consumers must not re-query a name.
  const ClassInfo* class_info_for_type(const ast::TypeExpr* t);
  const TypeSymbol* class_symbol_for_type(const ast::TypeExpr* t);
  const InterfaceInfo* interface_info_for_type(const ast::TypeExpr* t);
  std::string direct_type_name(const ast::TypeExpr* t);
  const TypeSymbol* metaclass_target_symbol(const ast::TypeExpr* t);
  bool type_accepts_class_value(const ast::TypeExpr* t);
  bool type_is_reference_class(const ast::TypeExpr* t);
  bool type_is_interface(const ast::TypeExpr* t);
  bool type_is_value_object(const ast::TypeExpr* t);

  // Constant folding for Pascal integer expressions. These routines answer in
  // Pascal's type/value model, not C++'s promotion rules, so later emit-time
  // conversions can stay faithful.
  const ast::TypeExpr* deduce_const_decl_type(const ast::ConstDecl& cd);
  const ast::TypeExpr* deduce_const_info_type(const ConstInfo& c);
  std::optional<ConvertedConstInt> convert_const_int_value(
      Location where, const ConstIntExprInfo& value,
      const ast::TypeExpr* target,
      bool explicit_conversion, bool diagnose);
  std::optional<ConstIntExprInfo> eval_const_int_expr(const ast::Expr& e);
  std::optional<ConstIntExprInfo> eval_const_int_expr(
      const ast::Expr& e, const TypeLookupContext* context);
  bool required_const_int_expr_overflows(const ast::Expr& e);
  std::optional<int64_t> enum_member_ordinal(
      const EnumMemberInfo& member);
  const ast::TypeExpr* const_intrinsic_type_arg(const ast::Expr& arg);
  const ast::TypeExpr* deduce_set_literal_type(
      const ast::SetLit& s, const ast::TypeExpr* target = nullptr);
  SetConversionKind classify_set_conversion(const ast::TypeExpr* source,
                                            const ast::TypeExpr* target);
  std::optional<OrdinalDomain> ordinal_domain_for_type(
      const ast::TypeExpr* t);

  // Best-effort Pascal static type for an expression. A null result means this
  // analysis layer does not have enough context to type the expression; it is
  // not by itself a diagnostic, and callers that need an error must own that
  // language rule explicitly.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);
  // Pascal type identity after alias/distinct canonicalization. Overload
  // ranking needs this source-language identity before it falls back to emitted
  // C++ carrier text, because unrelated Pascal pointer aliases may lower to
  // the same C++ representation.
  bool same_type_ast(const ast::TypeExpr* a, const ast::TypeExpr* b);
  bool same_type_ast_in_context(const ast::TypeExpr* a,
                                const TypeLookupContext* a_context,
                                const ast::TypeExpr* b,
                                const TypeLookupContext* b_context);
  // Pascal explicit typecasts are call-shaped in the AST. This returns the
  // source-language result type for `T(x)` / `unit.T(x)` when the callee is a
  // visible type name; null means the call is not a typecast.
  const ast::TypeExpr* explicit_typecast_result_type(const ast::Expr& e);
  // Same parser-binding debt as explicit_typecast_result_type(), but for emit
  // sites that need the declaration symbol to preserve named C++ carriers.
  const TypeSymbol* explicit_typecast_target_symbol(const ast::Expr& e);
  // `Ord` is lowered by the compiler, not by a runtime helper. Its result type
  // follows the operand's ordinal domain so `Ord(Char)` stays byte-sized while
  // enums and integer subranges keep their own storage width.
  const ast::TypeExpr* ord_result_type_for_operand(const ast::Expr& operand);
  const ast::TypeExpr* ord_result_type_for_type(const ast::TypeExpr* t);
  const TypeSymbol* deduce_class_symbol(const ast::Expr& e);
  const TypeSymbol* class_or_record_type_value_symbol(const ast::Expr& e);
  // Class identifiers and class aliases are values when passed to TClass or a
  // `class of ...` formal. Preserve the resolved class symbol so emission does
  // not have to re-look up the same source spelling.
  const TypeSymbol* concrete_class_symbol_for_metaclass_value(
      const ast::Expr& e);
  // For `TChild` used where `class of TBase` or `TClass` is expected, return
  // the concrete class symbol only when bound ancestry proves the
  // Pascal class value is assignment-compatible with the target metaclass.
  const TypeSymbol* concrete_class_symbol_for_metaclass_target(
      const ast::Expr& e, const ast::TypeExpr* target);
  const ast::TypeExpr* lookup_record_field_type_in_type(
      const ast::TypeExpr* type, std::string_view field_name);
  bool record_field_is_variant_in_type(const ast::TypeExpr* type,
                                       std::string_view field_name);
  const ast::TypeExpr* lookup_record_field_type_in_with(
      const ScopeStateView::WithBind& wb, std::string_view field_name);

  // Visibility helpers used by semantic lowering. These obey Pascal unit/use
  // visibility rules rather than the registry's global last-wins maps.
  bool with_bind_has_visible_member(const ScopeStateView::WithBind& wb,
                                    std::string_view name);
  // A value in the current lexical scope blocks interpreting the same
  // identifier as a unit or type qualifier in `name.member`.
  bool identifier_is_shadowed_value(std::string_view name);
  // `Unit.name` and `record.field` share the same Member AST node. Resolve
  // the unit-qualified form once, after Pascal lexical shadowing rules have
  // ruled out a nearer value named `Unit`, so value/call/storage emitters do
  // not each recurse into the qualifier as if it were an expression.
  std::optional<UnitQualifiedMemberLookup> resolve_unit_qualified_member(
      const ast::Member& mem);
  // A compiler intrinsic can be spelled either as a bare call (`low(t)`,
  // `sizeof(x)`) or as a system-unit-qualified call (`system.low(t)`). Both
  // forms denote the same intrinsic; this helper returns the intrinsic name
  // for either spelling so dispatch tables in deduce_type and the emitter do
  // not need a parallel Ident-callee / Member-callee copy of every intrinsic.
  // Returns nullopt when the callee is neither form (e.g. `self.foo(...)`,
  // `rec.field(...)`, or a plain function-pointer call).
  std::optional<std::string> intrinsic_call_name(const ast::Expr& callee);
  const VarInfo* find_visible_unit_var(const std::string& name);
  const ConstInfo* find_visible_unit_const(const std::string& name);
  const EnumMemberInfo* find_visible_enum_member(const std::string& name);
  const EnumInfoReg* find_visible_enum_info_for_member(const std::string& name);
  const EnumInfoReg* find_enum_info_in_unit(std::string_view unit_name,
                                            std::string_view member_name);

  // Find an implicit property visible from the current method / `with` scope.
  // This keeps property lookup in one place instead of scattering custom
  // "maybe self / maybe with" rewrites through the printer.
  std::optional<ImplicitPropertyLookup> find_implicit_class_property(
      std::string_view name);

  // Type-identity predicates consume the bound descriptor and its metadata.
  // Pascal is nominal: aliases share one descriptor; unrelated declarations
  // remain different even when their C++ carriers match.
  bool type_is_string_like(const ast::TypeExpr* t);
  bool type_is_long_string(const ast::TypeExpr* t);

  // Primitive metadata is attached to the registry descriptor. Consumers do
  // not recover a builtin declaration from its Pascal spelling.
  const PrimitiveInfo* primitive_info_for_type(const ast::TypeExpr* t);
  TargetInfo target() const { return target_; }

  // Resolve TySubrange to its host integer primitive type for arithmetic
  // contexts. Uses the same ordinal-domain-to-primitive mapping as
  // EmitResolution::integer_actual_domain_for_type so type deduction and
  // overload ranking stay consistent. Returns the input unchanged for
  // non-subrange types.
  const ast::TypeExpr* canonicalize_for_arithmetic(const ast::TypeExpr* t);

 private:
  struct SetLiteralOrdinalSummary {
    OrdinalFamily family;
    const TypeDescriptor* enum_key;
    const ast::TypeExpr* element_type;
    int64_t low;
    int64_t high;
  };

  struct OrdinalExprValue {
    int64_t value;
    OrdinalFamily family;
    const TypeDescriptor* enum_key;
  };

  std::string implicit_self_cxx();
  std::string implicit_self_access();
  bool is_visible_unit_qualifier(std::string_view name);
  const ast::TySet* synthesize_set_type(
      const ast::TypeExpr* element,
      std::optional<std::pair<int64_t, int64_t>> explicit_bounds);
  const ast::TyPointer* synthesize_pointer_type(
      const ast::TypeExpr* target);
  const ast::TypeExpr* semantic_shape_type(
      const ast::TypeExpr* t, const TypeLookupContext*& context);
  std::optional<OrdinalDomain> ordinal_domain_for_type(
      const ast::TypeExpr* t, const TypeLookupContext* context);
  std::optional<OrdinalDomain> ordinal_domain_for_set_type(
      const ast::TypeExpr* t);
  std::optional<OrdinalExprValue> eval_ordinal_expr(const ast::Expr& e);
  std::optional<OrdinalExprValue> eval_ordinal_expr(
      const ast::Expr& e, const TypeLookupContext* context);
  std::optional<ConstIntExprInfo> eval_const_low_high(
      const ast::TypeExpr* type, bool want_low);
  void evaluate_enum_ordinals(const EnumInfoReg& info);
  std::optional<SetLiteralOrdinalSummary>
  extend_set_literal_ordinal_summary(
      std::optional<SetLiteralOrdinalSummary> summary, const ast::Expr& e);
  std::optional<SetLiteralOrdinalSummary> summarize_set_literal_ordinals(
      const ast::SetLit& s);
  const ast::TypeExpr* canonical_set_type(const ast::TypeExpr* t);
  const ast::TypeExpr* integer_type(const PrimitiveInfo* info) const;
  const ast::TypeExpr* ordinal_integer_type(PrimitiveIntKind kind,
                                            uint8_t bits) const;
  bool type_is_numeric_primitive(const ast::TypeExpr* t);
  static bool binop_is_comparison(ast::BinOp op);
  static bool binop_is_arithmetic_like(ast::BinOp op);
  const ast::TypeExpr* deduce_binary_expr_type(const ast::Binary& b);
  const ast::TypeExpr* deduce_low_high_result_type(const ast::TypeExpr* t);
  std::optional<ConstIntExprInfo> eval_const_int_expr_impl(
      const ast::Expr& e,
      std::unordered_set<std::string>* visiting_const_names,
      bool* overflow);
  std::optional<ConstIntExprInfo> eval_const_int_cast(
      const ast::Call& c,
      std::unordered_set<std::string>* visiting_const_names,
      bool* overflow);
  std::optional<ConstIntExprInfo> fold_untyped_const_decl(
      const ast::ConstDecl& cd,
      std::unordered_set<std::string>* visiting_const_names,
      bool* overflow);
  std::optional<ConstIntExprInfo> fold_untyped_const_info(
      const ConstInfo& c,
      std::unordered_set<std::string>* visiting_const_names,
      bool* overflow);
  const ClassInfo* current_class_info() const;

  const TypeRegistry& registry_;
  ScopeStateView& scope_;
  ResolveNameProvider& resolve_name_provider_;
  CallTypeProvider& call_type_provider_;
  TargetInfo target_;
  // Enum expressions are evaluated through the existing Pascal value
  // resolver. Cache only the resulting ordinals so EmitTypes, Ord, Low/High,
  // and subrange analysis consume one answer instead of maintaining parallel
  // enum evaluators.
  std::unordered_map<const EnumInfoReg*,
                     std::vector<std::optional<int64_t>>>
      enum_ordinal_cache_;
  std::unordered_set<const EnumInfoReg*> enums_being_evaluated_;
};

}  // namespace tp2cc
