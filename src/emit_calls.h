#pragma once

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
struct TypeRegistry;

enum class UntypedArgKind : uint8_t { None, Const, Mutable };

class EmitCallExprOps {
 public:
  virtual ~EmitCallExprOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  virtual std::string const_value_to_cxx(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion) = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

// Call-site lowering. This module owns parameter metadata flattening,
// default-argument expansion, untyped/var/out adaptation, and special Pascal
// call forms that are semantically more than "print callee(args)".
class EmitCalls {
 public:
  EmitCalls(const TypeRegistry* registry, ScopeStateView& scope,
            EmitAnalysis& analysis, EmitTypes& types, EmitStorage& storage,
            EmitResolution& resolution, EmitCallExprOps& expr_ops);

  bool proc_accepts_zero_args(const ast::ProcDecl& decl);

  // Populate call-site metadata in argument order. This is the single place
  // that answers "is slot i untyped storage?", "does slot i alias mutable
  // caller state?", and "what Pascal type was chosen for slot i?" so explicit
  // calls and implicit zero-arg calls stay aligned.
  void mark_call_param_info(const ast::ProcDecl* decl,
                            std::vector<UntypedArgKind>& untyped_arg,
                            std::vector<bool>& mutable_ref_arg,
                            std::vector<const ast::TypeExpr*>& param_types);
  void collect_builtin_helper_param_info(
      const ast::Expr& callee, std::vector<UntypedArgKind>& untyped_arg,
      std::vector<bool>& mutable_ref_arg,
      std::vector<const ast::TypeExpr*>& param_types);
  void collect_call_param_info(const ast::Expr& callee,
                               std::vector<UntypedArgKind>& untyped_arg,
                               std::vector<bool>& mutable_ref_arg,
                               std::vector<const ast::TypeExpr*>& param_types);

  // Pascal trailing default parameters are compile-time sugar: before the call
  // reaches C++ overload resolution, omitted suffix actuals must be expanded
  // into an ordinary full argument list.
  void append_defaulted_trailing_call_args(
      const ast::ProcDecl* decl, std::vector<const ast::Expr*>& args);

  // Lower one Pascal actual argument into the C++ form required by the
  // chosen formal parameter slot: open-array constructors, typed/mutable
  // storage rebinds, string promotion, and untyped const/var adaptation all
  // flow through this one choke point.
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             UntypedArgKind untyped_arg,
                             bool mutable_ref_arg);

  // Bare Pascal `foo;` / `obj.meth;` can still mean a call when omitted
  // trailing actuals all come from defaults. Rebuild that call here so those
  // implicit sites share the exact same lowering as explicit `Call`.
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl);

  // Pascal `obj.Free` is the null-safe TObject cleanup entrypoint, not a
  // normal instance call, and `TClass.Create` on a metaclass value allocates a
  // new instance before running the constructor body. Keep those special
  // call-site forms here with the rest of the call lowering.
  std::optional<std::string> maybe_lower_class_free_member(
      const ast::Expr& base, std::string_view member_name);
  std::optional<std::string> maybe_lower_class_constructor_call(
      Location where, std::string_view class_name, std::string_view member_name,
      const std::vector<const ast::Expr*>& args,
      const std::vector<const ast::TypeExpr*>& param_types,
      const std::vector<UntypedArgKind>& untyped_arg,
      const std::vector<bool>& mutable_ref_arg);

 private:
  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  EmitResolution& resolution_;
  EmitCallExprOps& expr_ops_;
};

}  // namespace tp2cc
