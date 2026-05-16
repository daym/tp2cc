#pragma once

#include <optional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "emit_context.h"
#include "emit_resolution_types.h"
#include "emit_support.h"

namespace tp2cc {

struct ClassInfo;
struct ConstInfo;
struct EnumInfoReg;
struct PropertyInfo;
struct TypeRegistry;
struct VarInfo;

struct ConstIntExprInfo {
  int64_t value = 0;
  const PrimitiveInfo* type = nullptr;
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
// access path ad hoc.
struct ImplicitPropertyLookup {
  const PropertyInfo* prop = nullptr;
  std::string class_name;
  std::string base_cxx;
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

class EmitAnalysis {
 public:
  EmitAnalysis(const TypeRegistry* registry, ScopeStateView& scope,
               ResolveNameProvider& resolve_name_provider,
               CallTypeProvider& call_type_provider);

  // Canonicalize Pascal type aliases/distinct wrappers to the underlying type
  // view the emitter should reason about for layout and conversion questions.
  const ast::TypeExpr* canonicalize_type(const ast::TypeExpr* t);

  // Parameter ABI policy: decide when Pascal `const` aggregates still need
  // reference lowering so C++ preserves aliasing/mutability semantics.
  bool const_param_needs_mutable_ref(const ast::TypeExpr* t);
  bool const_param_needs_const_ref(const ast::TypeExpr* t);
  const ClassInfo* class_info_for_type_name(std::string_view name);
  const ast::TypeExpr* lookup_named_type_expr(std::string_view name);
  bool is_builtin_reference_class_name(std::string_view name) const;
  std::string metaclass_target_name(const ast::TypeExpr* t);
  bool type_is_reference_class(const ast::TypeExpr* t);
  bool type_is_interface(const ast::TypeExpr* t);
  bool type_is_value_object(const ast::TypeExpr* t);
  std::string_view current_unit_name() const { return scope_.current_unit_name; }

  // Constant folding for Pascal integer expressions. These routines answer in
  // Pascal's type/value model, not C++'s promotion rules, so later emit-time
  // conversions can stay faithful.
  const ast::TypeExpr* deduce_const_decl_type(const ast::ConstDecl& cd);
  const ast::TypeExpr* deduce_const_info_type(const ConstInfo& c);
  std::optional<ConvertedConstInt> convert_const_int_value(
      Location where, int64_t value, const ast::TypeExpr* target,
      bool explicit_conversion, bool diagnose);
  std::optional<ConstIntExprInfo> eval_const_int_cast(
      const ast::Call& c,
      std::unordered_set<std::string>* visiting_const_names);
  std::optional<ConstIntExprInfo> eval_const_int_expr(
      const ast::Expr& e,
      std::unordered_set<std::string>* visiting_const_names = nullptr);
  const ast::TypeExpr* deduce_set_literal_type(
      const ast::SetLit& s, const ast::TypeExpr* target = nullptr);
  SetConversionKind classify_set_conversion(const ast::TypeExpr* source,
                                            const ast::TypeExpr* target);

  // Expression/type/name analysis used by emit-time semantic decisions such as
  // member access, overload picking, and `with` lowering.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);
  const ast::TypeExpr* ord_result_type_for_operand(const ast::Expr& operand);
  std::string deduce_class_alias(const ast::Expr& e);
  std::string canonical_method_owner_type_name(std::string_view owner);
  const ast::TypeExpr* lookup_record_field_type_in_type(
      const ast::TypeExpr* type, std::string_view field_name);
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
  const VarInfo* find_visible_unit_var(const std::string& name);
  const ConstInfo* find_visible_unit_const(const std::string& name);
  const EnumInfoReg* find_visible_enum_info_for_member(const std::string& name);

  // Find an implicit property visible from the current method / `with` scope.
  // This keeps property lookup in one place instead of scattering custom
  // "maybe self / maybe with" rewrites through the printer.
  std::optional<ImplicitPropertyLookup> find_implicit_class_property(
      std::string_view name);

 private:
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
    // Enum identity for `OrdinalFamily::Enum`. The TyEnum AST node IS the
    // enum's identity: there's one per Pascal `type T = (...)` declaration,
    // and the registry stores it (EnumInfoReg::type). Using the pointer
    // directly removes the "what spelling did this come from" question that
    // a string key invites - every path to the same enum (raw TyEnum from
    // deduce_type of an enum-member ident, or TyName canonicalised through
    // the registry) ends at the same pointer.
    const ast::TyEnum* enum_key = nullptr;
  };

  std::string implicit_self_cxx();
  bool is_visible_unit_qualifier(std::string_view name);
  const ast::TySet* synthesize_set_type(
      const ast::TypeExpr* element,
      std::optional<std::pair<int64_t, int64_t>> explicit_bounds);
  bool same_type_ast(const ast::TypeExpr* a, const ast::TypeExpr* b);
  const ast::TypeExpr* ord_result_type_for_type(const ast::TypeExpr* t);
  std::optional<OrdinalDomain> ordinal_domain_for_type(const ast::TypeExpr* t);
  std::optional<OrdinalDomain> ordinal_domain_for_set_type(
      const ast::TypeExpr* t);
  bool try_eval_ordinal_expr(const ast::Expr& e, int64_t* value,
                             OrdinalFamily* family,
                             const ast::TyEnum** enum_key);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  ResolveNameProvider& resolve_name_provider_;
  CallTypeProvider& call_type_provider_;
  std::vector<std::shared_ptr<ast::TypeExpr>> synthesized_types_;
};

}  // namespace tp2cc
