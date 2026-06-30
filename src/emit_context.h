#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "emit_support.h"
#include "typereg.h"

namespace tp2cc::ast {
struct TypeExpr;
struct ConstDecl;
struct ProcDecl;
}  // namespace tp2cc::ast

namespace tp2cc {

// Shared view of the current Pascal semantic environment while emitting one
// routine or unit. This is intentionally a view over Emitter-owned state, not
// a second source of truth.
struct ScopeStateView {
  // Parameterless nested functions auto-call in value context, so resolution
  // needs both the declaration and enough signature facts to know whether the
  // name denotes a value-producing callable.
  struct NestedFn {
    size_t param_count = 0;
    bool accepts_zero_args = false;
    bool is_function = false;
    const ast::TypeExpr* return_type = nullptr;
    const ast::ProcDecl* decl = nullptr;
    std::string cxx_name;
  };

  // `with X do` contributes an already-lowered receiver expression plus the
  // Pascal type/class information needed to resolve bare members through it.
  struct WithBind {
    struct BytewiseStorage {
      enum class FieldSelection {
        AllFields,
        VariantPayloadFieldsOnly,
      };

      std::string ptr_cxx;
      std::string type_cxx;
      bool unaligned;
      FieldSelection field_selection;

      BytewiseStorage(std::string ptr_cxx_in, std::string type_cxx_in,
                      bool unaligned_in, FieldSelection field_selection_in)
          : ptr_cxx(std::move(ptr_cxx_in)),
            type_cxx(std::move(type_cxx_in)),
            unaligned(unaligned_in),
            field_selection(field_selection_in) {}
    };

    WithBind(std::string cxx_text_in, const ast::TypeExpr* type_in,
             std::string class_name_in, std::string access_op_in)
        : cxx_text(std::move(cxx_text_in)),
          type(type_in),
          class_name(std::move(class_name_in)),
          access_op(std::move(access_op_in)) {}

    WithBind(std::string cxx_text_in, const ast::TypeExpr* type_in,
             std::string class_name_in, std::string access_op_in,
             BytewiseStorage bytewise_storage_in)
        : cxx_text(std::move(cxx_text_in)),
          type(type_in),
          class_name(std::move(class_name_in)),
          access_op(std::move(access_op_in)),
          bytewise_storage(std::move(bytewise_storage_in)) {}

    std::string cxx_text;
    const ast::TypeExpr* type = nullptr;
    std::string class_name;
    std::string access_op;
    // Base storage available to bare fields inside `with`.
    //
    // If the `with` target itself is byte-addressed, every field selection must
    // compose from this address. If the target is ordinary record storage, only
    // variant payload fields use it; ordinary fields stay on direct C++ member
    // access through `cxx_text`.
    std::optional<BytewiseStorage> bytewise_storage;
  };

  std::string& current_class_name;
  std::string& current_unit_name;
  // Non-empty when Pascal name lookup is intentionally performed with
  // `current_unit_name` set to a declaration unit, while the generated C++
  // text is inserted in another unit's namespace. Own-unit symbols found
  // through that declaration-unit lookup therefore still need explicit
  // namespace qualification for the insertion site.
  std::string& lookup_emission_unit_name;

  // LHS-only rewrites for Pascal's implicit result variables.
  std::string& lhs_fn_rewrite;
  std::string& lhs_fn_rewrite_slot;
  std::string& lhs_outer_result_rewrite;
  std::string& lhs_outer_result_rewrite_slot;
  // Value reads from scalar fields nested inside packed aggregate fields can
  // be lowered safely via byte offsets. Lvalue/address contexts must suppress
  // that path so the existing packed-aggregate guard still rejects references.
  bool& suppress_packed_scalar_value_load;
  // Pascal typecasts are context-sensitive. The enclosing construct decides
  // whether `T(x)` is a value expression or a variable designator. In ordinary
  // expression context, `TArray(x)` produces an array value; Pascal arrays are
  // first-class values and do not decay to pointers. In storage contexts, such
  // as assignment targets, `@x`, var/out/untyped-var actual arguments, and
  // mutation helpers like `Inc(T(x))`, the same source expression denotes a
  // typed view of the original storage.
  bool& storage_view_context;

  // Current lexical scope.
  std::unordered_set<std::string>& local_scope;
  std::unordered_map<std::string, const ast::TypeExpr*>& local_value_types;
  std::unordered_map<std::string, const ast::ConstDecl*>& local_consts;
  std::unordered_set<std::string>& local_untyped_params;
  std::unordered_map<std::string, std::vector<NestedFn>>& local_nested_fns;
  std::unordered_set<std::string>& local_nested_forwards;
  const TypeLookupContext*& type_scope;
  std::unordered_set<std::string>& local_const_params;
  std::vector<WithBind>& with_stack;

  // Current-function / result-slot state. `Result` semantics differ between
  // nested procedures and nested functions, so this has to stay explicit.
  std::string& current_fn_name;
  std::vector<std::string>& current_fn_param_names;
  bool& current_fn_is_function;
  bool& current_fn_is_ctor;
  const ast::TypeExpr*& current_fn_result_type;
  std::string& current_result_slot_name;
  std::string& bare_result_slot_name;
  const ast::TypeExpr*& bare_result_type;
  std::string& outer_result_name;
  std::string& outer_result_slot_name;
  const ast::TypeExpr*& outer_result_type;
};

// Temporarily evaluate declaration-owned syntax in the lexical environment
// that owned that declaration. This is for type bounds, enum ordinals, and
// default arguments that are lowered later from a different call/type site.
class ScopedDeclarationLookup {
 public:
  ScopedDeclarationLookup(ScopeStateView& scope,
                          const TypeLookupContext* declaration_context,
                          std::string_view declaration_unit)
      : scope_(scope),
        saved_current_unit_(scope.current_unit_name),
        saved_lookup_emission_unit_(scope.lookup_emission_unit_name),
        saved_type_scope_(scope.type_scope) {
    if (!declaration_context && declaration_unit.empty()) return;

    std::string_view unit = declaration_context ? declaration_context->unit
                                                : declaration_unit;
    if (declaration_context == scope.type_scope &&
        (unit.empty() || unit == scope.current_unit_name)) {
      return;
    }

    saved_local_scope_.swap(scope.local_scope);
    saved_local_value_types_.swap(scope.local_value_types);
    saved_local_consts_.swap(scope.local_consts);
    saved_local_untyped_params_.swap(scope.local_untyped_params);
    saved_local_nested_fns_.swap(scope.local_nested_fns);
    saved_local_nested_forwards_.swap(scope.local_nested_forwards);
    saved_local_const_params_.swap(scope.local_const_params);
    saved_with_stack_.swap(scope.with_stack);

    if (!unit.empty() && unit != scope_.current_unit_name) {
      scope_.lookup_emission_unit_name =
          saved_lookup_emission_unit_.empty() ? saved_current_unit_
                                              : saved_lookup_emission_unit_;
      scope_.current_unit_name = std::string(unit);
    }
    scope_.type_scope = declaration_context;
    active_ = true;
  }

  ScopedDeclarationLookup(const ScopedDeclarationLookup&) = delete;
  ScopedDeclarationLookup& operator=(const ScopedDeclarationLookup&) = delete;

  ~ScopedDeclarationLookup() {
    if (!active_) return;
    scope_.current_unit_name = saved_current_unit_;
    scope_.lookup_emission_unit_name = saved_lookup_emission_unit_;
    scope_.local_scope.swap(saved_local_scope_);
    scope_.local_value_types.swap(saved_local_value_types_);
    scope_.local_consts.swap(saved_local_consts_);
    scope_.local_untyped_params.swap(saved_local_untyped_params_);
    scope_.local_nested_fns.swap(saved_local_nested_fns_);
    scope_.local_nested_forwards.swap(saved_local_nested_forwards_);
    scope_.local_const_params.swap(saved_local_const_params_);
    scope_.with_stack.swap(saved_with_stack_);
    scope_.type_scope = saved_type_scope_;
  }

 private:
  ScopeStateView& scope_;
  std::string saved_current_unit_;
  std::string saved_lookup_emission_unit_;
  std::unordered_set<std::string> saved_local_scope_;
  std::unordered_map<std::string, const ast::TypeExpr*>
      saved_local_value_types_;
  std::unordered_map<std::string, const ast::ConstDecl*> saved_local_consts_;
  std::unordered_set<std::string> saved_local_untyped_params_;
  std::unordered_map<std::string, std::vector<ScopeStateView::NestedFn>>
      saved_local_nested_fns_;
  std::unordered_set<std::string> saved_local_nested_forwards_;
  std::unordered_set<std::string> saved_local_const_params_;
  std::vector<ScopeStateView::WithBind> saved_with_stack_;
  const TypeLookupContext* saved_type_scope_ = nullptr;
  bool active_ = false;
};

// MIGRATION_NAME_LOOKUP_FALLBACK: every call to a migration_fallback_* type
// lookup function is a remaining spelling-based type lookup site. These are
// temporary because the parser does not yet bind constructs such as `sizeof(T)`
// or callee syntax `T(expr)` to a TypeExpr. Consumers that already have a
// TypeExpr must use resolved_type_symbol_in_context(),
// resolved_symbol_for_type(), or canonical_symbol_for_type() instead. The
// final parser-driven migration should delete these functions and all call
// sites.
inline const TypeSymbol* migration_fallback_type_symbol_by_name(
    const TypeRegistry& registry, const ScopeStateView& scope,
    std::string_view name) {
  if (scope.type_scope) {
    if (const TypeSymbol* symbol =
            registry.lookup_type_symbol_in_context(name, scope.type_scope)) {
      return symbol;
    }
  }
  return nullptr;
}

inline const TypeSymbol* migration_fallback_lexical_type_symbol_by_name(
    const TypeRegistry& registry, const ScopeStateView& scope,
    std::string_view name) {
  if (!scope.type_scope) return nullptr;
  return registry.lookup_type_symbol_in_scope_chain(name, scope.type_scope);
}

inline const EnumInfoReg* local_enum_info_for_member(
    const ScopeStateView& scope, std::string_view name) {
  const std::string member = ascii_lower(name);
  for (const TypeLookupContext* frame = scope.type_scope; frame;
       frame = frame->parent) {
    for (const auto& [_, symbol] : frame->type_symbols) {
      if (!symbol) continue;
      const EnumInfoReg* info = symbol->enum_info();
      if (!info) continue;
      for (const auto& enum_member : info->members) {
        if (enum_member == member) return info;
      }
    }
  }
  return nullptr;
}

inline const TypeSymbol* resolved_type_symbol_in_context(
    const TypeRegistry& registry, const ScopeStateView& scope,
    const ast::TypeExpr* type, const TypeLookupContext* context = nullptr) {
  const TypeSymbol* symbol = registry.canonical_symbol_for_type(type);
  if (symbol && symbol->defining_unit != "__builtin__") return symbol;
  if (!type || type->kind != ast::Kind::TyName) return symbol;
  const auto& name = static_cast<const ast::TyName&>(*type).name;
  if (const TypeSymbol* visible =
          context ? registry.lookup_type_symbol_in_context(name, context)
                  : migration_fallback_type_symbol_by_name(registry, scope, name)) {
    return visible;
  }
  return symbol;
}

}  // namespace tp2cc
