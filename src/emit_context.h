#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tp2cc::ast {
struct TypeExpr;
struct ConstDecl;
struct ProcDecl;
struct TyEnum;
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
  };

  // `with X do` contributes an already-lowered receiver expression plus the
  // Pascal type/class information needed to resolve bare members through it.
  struct WithBind {
    std::string cxx_text;
    const ast::TypeExpr* type = nullptr;
    std::string class_name;
    std::string access_op;
  };

  std::string& current_class_name;
  std::string& current_unit_name;
  // Non-empty only while lowering an omitted default argument. Name lookup
  // uses `current_unit_name` set to the declaration unit, but generated C++
  // is inserted in this caller unit. Own-unit symbols found through that
  // declaration-unit lookup therefore need explicit namespace qualification.
  std::string& default_arg_emission_unit_name;

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
  std::unordered_map<std::string, const ast::TypeExpr*>& local_types;
  std::unordered_map<std::string, const ast::ConstDecl*>& local_consts;
  std::unordered_set<std::string>& local_untyped_params;
  std::unordered_map<std::string, NestedFn>& local_nested_fns;
  std::unordered_set<std::string>& local_nested_forwards;
  std::unordered_map<std::string, const ast::TyEnum*>& local_enums;
  std::unordered_set<std::string>& local_const_params;
  std::unordered_map<std::string, const ast::TypeExpr*>&
      local_type_aliases_scoped;
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

}  // namespace tp2cc
