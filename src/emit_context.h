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
             const TypeSymbol* class_symbol_in, std::string access_op_in)
        : cxx_text(std::move(cxx_text_in)),
          type(type_in),
          class_symbol(class_symbol_in),
          access_op(std::move(access_op_in)) {}

    WithBind(std::string cxx_text_in, const ast::TypeExpr* type_in,
             const TypeSymbol* class_symbol_in, std::string access_op_in,
             BytewiseStorage bytewise_storage_in)
        : cxx_text(std::move(cxx_text_in)),
          type(type_in),
          class_symbol(class_symbol_in),
          access_op(std::move(access_op_in)),
          bytewise_storage(std::move(bytewise_storage_in)) {}

    std::string cxx_text;
    const ast::TypeExpr* type = nullptr;
    const TypeSymbol* class_symbol = nullptr;
    std::string access_op;
    // Base storage available to bare fields inside `with`.
    //
    // If the `with` target itself is byte-addressed, every field selection must
    // compose from this address. If the target is ordinary record storage, only
    // variant payload fields use it; ordinary fields stay on direct C++ member
    // access through `cxx_text`.
    std::optional<BytewiseStorage> bytewise_storage;
  };

  const TypeSymbol*& current_class_symbol;
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
  // Pascal `var view: T absolute other` binds `view` as a T-typed view
  // of the bytes at `other`. There is no C++ variable emitted for
  // `view`: emitting a persistent `T&` reference over storage whose
  // dynamic type is not T would be strict-aliasing UB. Instead, name
  // lookup for `view` synthesises a bytewise storage designator whose
  // ptr expression is `&other`; the existing bytewise machinery then
  // handles reads/writes via memcpy helpers, and method calls / var-out
  // args go through the bounded `tp2cc_TypedView` wrapper.
  struct AbsoluteAlias {
    std::string target_cxx;             // C++ ident of the source variable
    const ast::TypeExpr* type = nullptr; // alias's declared Pascal type (T)
    bool target_is_pointerish = false;   // target value already is an address
    bool target_is_const = false;        // Pascal `const` source parameter
  };
  std::unordered_map<std::string, AbsoluteAlias>& local_absolute_targets;
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
        saved_current_class_symbol_(scope.current_class_symbol),
        saved_current_fn_name_(scope.current_fn_name),
        saved_current_fn_param_names_(scope.current_fn_param_names),
        saved_current_fn_is_function_(scope.current_fn_is_function),
        saved_current_fn_is_ctor_(scope.current_fn_is_ctor),
        saved_current_fn_result_type_(scope.current_fn_result_type),
        saved_current_result_slot_name_(scope.current_result_slot_name),
        saved_bare_result_slot_name_(scope.bare_result_slot_name),
        saved_bare_result_type_(scope.bare_result_type),
        saved_outer_result_name_(scope.outer_result_name),
        saved_outer_result_slot_name_(scope.outer_result_slot_name),
        saved_outer_result_type_(scope.outer_result_type),
        saved_type_scope_(scope.type_scope) {
    if (!declaration_context && declaration_unit.empty()) return;

    std::string_view unit = declaration_context ? declaration_context->unit
                                                : declaration_unit;
    preserve_local_value_scope_ =
        declaration_context && declaration_context->preserve_local_value_scope &&
        (unit.empty() || unit == scope_.current_unit_name);
    // Unit emission preloads later const declarations into local value maps for
    // ordinary output, so an already-active type context still needs the local
    // value maps cleared when rendering declaration-owned bounds/ordinals.
    if (declaration_context == scope.type_scope &&
        (unit.empty() || unit == scope.current_unit_name) &&
        preserve_local_value_scope_) {
      return;
    }
    if (!preserve_local_value_scope_) {
      saved_local_scope_.swap(scope.local_scope);
      saved_local_value_types_.swap(scope.local_value_types);
      saved_local_consts_.swap(scope.local_consts);
      saved_local_untyped_params_.swap(scope.local_untyped_params);
      saved_local_nested_fns_.swap(scope.local_nested_fns);
      saved_local_nested_forwards_.swap(scope.local_nested_forwards);
      saved_local_const_params_.swap(scope.local_const_params);
      saved_local_absolute_targets_.swap(scope.local_absolute_targets);
      saved_with_stack_.swap(scope.with_stack);
    }

    // Declaration-owned expressions are not evaluated as part of the caller's
    // routine body. Even when local value names are preserved for a local
    // declaration context, caller-only bindings such as `Result`, the current
    // function name, `Self`, and `inherited` must not leak into default
    // arguments, enum ordinals, or type bounds.
    scope_.current_class_symbol = nullptr;
    scope_.current_fn_name.clear();
    scope_.current_fn_param_names.clear();
    scope_.current_fn_is_function = false;
    scope_.current_fn_is_ctor = false;
    scope_.current_fn_result_type = nullptr;
    scope_.current_result_slot_name.clear();
    scope_.bare_result_slot_name.clear();
    scope_.bare_result_type = nullptr;
    scope_.outer_result_name.clear();
    scope_.outer_result_slot_name.clear();
    scope_.outer_result_type = nullptr;

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
    scope_.current_class_symbol = saved_current_class_symbol_;
    scope_.current_fn_name = saved_current_fn_name_;
    scope_.current_fn_param_names = saved_current_fn_param_names_;
    scope_.current_fn_is_function = saved_current_fn_is_function_;
    scope_.current_fn_is_ctor = saved_current_fn_is_ctor_;
    scope_.current_fn_result_type = saved_current_fn_result_type_;
    scope_.current_result_slot_name = saved_current_result_slot_name_;
    scope_.bare_result_slot_name = saved_bare_result_slot_name_;
    scope_.bare_result_type = saved_bare_result_type_;
    scope_.outer_result_name = saved_outer_result_name_;
    scope_.outer_result_slot_name = saved_outer_result_slot_name_;
    scope_.outer_result_type = saved_outer_result_type_;
    if (!preserve_local_value_scope_) {
      scope_.local_scope.swap(saved_local_scope_);
      scope_.local_value_types.swap(saved_local_value_types_);
      scope_.local_consts.swap(saved_local_consts_);
      scope_.local_untyped_params.swap(saved_local_untyped_params_);
      scope_.local_nested_fns.swap(saved_local_nested_fns_);
      scope_.local_nested_forwards.swap(saved_local_nested_forwards_);
      scope_.local_const_params.swap(saved_local_const_params_);
      scope_.local_absolute_targets.swap(saved_local_absolute_targets_);
      scope_.with_stack.swap(saved_with_stack_);
    }
    scope_.type_scope = saved_type_scope_;
  }

 private:
  ScopeStateView& scope_;
  std::string saved_current_unit_;
  std::string saved_lookup_emission_unit_;
  const TypeSymbol* saved_current_class_symbol_ = nullptr;
  std::string saved_current_fn_name_;
  std::vector<std::string> saved_current_fn_param_names_;
  bool saved_current_fn_is_function_ = false;
  bool saved_current_fn_is_ctor_ = false;
  const ast::TypeExpr* saved_current_fn_result_type_ = nullptr;
  std::string saved_current_result_slot_name_;
  std::string saved_bare_result_slot_name_;
  const ast::TypeExpr* saved_bare_result_type_ = nullptr;
  std::string saved_outer_result_name_;
  std::string saved_outer_result_slot_name_;
  const ast::TypeExpr* saved_outer_result_type_ = nullptr;
  std::unordered_set<std::string> saved_local_scope_;
  std::unordered_map<std::string, const ast::TypeExpr*>
      saved_local_value_types_;
  std::unordered_map<std::string, const ast::ConstDecl*> saved_local_consts_;
  std::unordered_set<std::string> saved_local_untyped_params_;
  std::unordered_map<std::string, std::vector<ScopeStateView::NestedFn>>
      saved_local_nested_fns_;
  std::unordered_set<std::string> saved_local_nested_forwards_;
  std::unordered_set<std::string> saved_local_const_params_;
  std::unordered_map<std::string, ScopeStateView::AbsoluteAlias>
      saved_local_absolute_targets_;
  std::vector<ScopeStateView::WithBind> saved_with_stack_;
  const TypeLookupContext* saved_type_scope_ = nullptr;
  bool active_ = false;
  bool preserve_local_value_scope_ = false;
};

inline const EnumInfoReg* local_enum_info_for_member(
    const ScopeStateView& scope, std::string_view name) {
  assert(pascal_key_is_canonical(name));
  for (const TypeLookupContext* frame = scope.type_scope; frame;
       frame = frame->parent) {
    if (frame->kind != ScopeFrameKind::Local) continue;
    if (const EnumInfoReg* info =
            scope_frame_find_local_enum_info_for_member(*frame, name)) {
      return info;
    }
  }
  return nullptr;
}

}  // namespace tp2cc
