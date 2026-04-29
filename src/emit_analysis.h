#pragma once

#include <optional>
#include <string>
#include <unordered_set>

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

class EmitAnalysis {
 public:
  EmitAnalysis(const TypeRegistry* registry, ScopeStateView& scope,
               ResolveNameProvider& resolve_name_provider);

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
  bool type_is_value_object(const ast::TypeExpr* t);

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

  // Expression/type/name analysis used by emit-time semantic decisions such as
  // member access, overload picking, and `with` lowering.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);
  std::string deduce_class_alias(const ast::Expr& e);
  const ast::TypeExpr* lookup_record_field_type_in_type(
      const ast::TypeExpr* type, std::string_view field_name);
  const ast::TypeExpr* lookup_record_field_type_in_with(
      const ScopeStateView::WithBind& wb, std::string_view field_name);

  // Visibility helpers used by semantic lowering. These obey Pascal unit/use
  // visibility rules rather than the registry's global last-wins maps.
  bool with_bind_has_visible_member(const ScopeStateView::WithBind& wb,
                                    std::string_view name);
  const VarInfo* find_visible_unit_var(const std::string& name);
  const ConstInfo* find_visible_unit_const(const std::string& name);
  const EnumInfoReg* find_visible_enum_info_for_member(const std::string& name);

  // Find an implicit property visible from the current method / `with` scope.
  // This keeps property lookup in one place instead of scattering custom
  // "maybe self / maybe with" rewrites through the printer.
  std::optional<ImplicitPropertyLookup> find_implicit_class_property(
      std::string_view name);

 private:
  std::string implicit_self_cxx();

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  ResolveNameProvider& resolve_name_provider_;
};

}  // namespace tp2cc
