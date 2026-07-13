#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"
#include "emit_context.h"

namespace tp2cc {

class EmitAnalysis;
class EmitResolution;
class EmitStorage;
class EmitTypes;
class OverloadTypeProvider;
struct TypeRegistry;
struct TypeLookupContext;

enum class UntypedArgKind : uint8_t { None, Const, Mutable };

struct CallArgumentSlot {
  const ast::Expr* expr = nullptr;
  const ast::TypeExpr* param_type = nullptr;
  const TypeLookupContext* param_context = nullptr;
  const TypeLookupContext* default_arg_context = nullptr;
  UntypedArgKind untyped_arg = UntypedArgKind::None;
  bool mutable_ref_arg = false;
  bool defaulted = false;
};

struct CallArgumentPlan {
  std::vector<CallArgumentSlot> slots;
  std::string default_arg_unit;
};

class EmitCallExprOps {
 public:
  virtual ~EmitCallExprOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  virtual std::string const_value_to_cxx(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion) = 0;
  virtual bool can_convert_value_to_type(const ast::Expr& e,
                                         const ast::TypeExpr* target,
                                         bool explicit_conversion) = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

// Call-site lowering. This module owns parameter metadata flattening,
// default-argument expansion, untyped/var/out adaptation, and special Pascal
// call forms that are semantically more than "print callee(args)".
class EmitCalls {
 public:
  EmitCalls(const TypeRegistry& registry, ScopeStateView& scope,
            EmitAnalysis& analysis, EmitTypes& types, EmitStorage& storage,
            EmitResolution& resolution, OverloadTypeProvider& overload_types,
            EmitCallExprOps& expr_ops);

  bool proc_accepts_zero_args(const ast::ProcDecl& decl);

  // Build the complete call-site slot table after overload resolution. The plan
  // is short-lived and contains only AST/type pointers plus lowering flags.
  CallArgumentPlan plan_call_arguments(
      const ast::ProcDecl* decl, const ast::Expr* callee,
      const std::vector<const ast::Expr*>& explicit_args,
      std::string_view default_arg_unit = {},
      const ProcInfo* selected_proc = nullptr);

  // Validate an already-planned call against the chosen formal slots. This is
  // the single-candidate counterpart to overload picking: arity alone is not a
  // Pascal call, and the accepted conversions must be the same conversions
  // lower_call_arg will emit.
  bool validate_call_arguments(const CallArgumentPlan& plan);

  // Lower one Pascal actual argument into the C++ form required by the
  // chosen formal parameter slot: open-array constructors, typed/mutable
  // storage rebinds, string promotion, and untyped const/var adaptation all
  // flow through this one choke point.
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             UntypedArgKind untyped_arg,
                             bool mutable_ref_arg,
                             std::string_view default_arg_unit = {},
                             const TypeLookupContext* default_arg_context = nullptr,
                             const TypeLookupContext* param_context = nullptr);
  std::string lower_call_arg(const CallArgumentSlot& slot,
                             std::string_view default_arg_unit = {});

  // Bare Pascal `foo;` / `obj.meth;` can still mean a call when omitted
  // trailing actuals all come from defaults. Rebuild that call here so those
  // implicit sites share the exact same lowering as explicit `Call`.
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl,
                                           std::string_view default_arg_unit);

  // Pascal `obj.Free` is the null-safe TObject cleanup entrypoint, not a
  // normal instance call, and `TClass.Create` on a metaclass value allocates a
  // new instance before running the constructor body. Keep those special
  // call-site forms here with the rest of the call lowering.
  std::optional<std::string> maybe_lower_class_free_member(
      const ast::Expr& base, std::string_view member_name);
  std::optional<std::string> maybe_lower_class_constructor_call(
      Location where, const TypeSymbol& class_symbol,
      std::string_view member_name,
      const CallArgumentPlan& plan, const ast::ProcDecl* selected_decl);

 private:
  std::vector<CallArgumentSlot> append_default_call_slots(
      const ast::ProcDecl* decl, std::vector<CallArgumentSlot> slots);
  std::vector<CallArgumentSlot> call_slots_with_proc_info(
      const ProcInfo* proc_info, std::vector<CallArgumentSlot> slots);
  std::vector<CallArgumentSlot> call_slots_with_decl_param_info(
      const ast::ProcDecl* decl, std::vector<CallArgumentSlot> slots);
  bool slot_accepts_argument(const CallArgumentSlot& slot,
                             std::string_view default_arg_unit);
  std::vector<CallArgumentSlot> call_slots_with_procedural_callee_param_info(
      const ast::Expr& callee, std::vector<CallArgumentSlot> slots);
  const ast::TypeExpr* procedural_callee_type(const ast::Expr& callee);

  const TypeRegistry& registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  EmitResolution& resolution_;
  OverloadTypeProvider& overload_types_;
  EmitCallExprOps& expr_ops_;
};

}  // namespace tp2cc
