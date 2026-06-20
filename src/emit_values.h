#pragma once

#include <optional>
#include <string>

#include "ast.h"
#include "emit_context.h"

namespace tp2cc {

class EmitAnalysis;
class EmitResolution;
class EmitStorage;
class EmitTypes;
class OverloadTypeProvider;
struct ConvertedConstInt;
struct ConstIntExprInfo;
struct TypeRegistry;

class EmitValueExprOps {
 public:
  virtual ~EmitValueExprOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  virtual std::string expr_to_cxx_no_autocall(const ast::Expr& e) = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
  virtual bool in_block_scope() const = 0;
};

// Pascal value lowering owns the rules for emitting set literals, typed
// aggregate constants, explicit integer constant conversions, procedural
// values, and metaclass values.
class EmitValues {
 public:
  EmitValues(const TypeRegistry* registry, ScopeStateView& scope,
             EmitAnalysis& analysis, EmitTypes& types, EmitStorage& storage,
             EmitResolution& resolution, OverloadTypeProvider& overload_types,
             EmitValueExprOps& expr_ops);

  std::string set_literal_to_cxx(const ast::SetLit& s,
                                 const ast::TypeExpr* target = nullptr);
  // Lower a constant expression in contexts that may still apply their own
  // target conversion phase afterward, such as assignment RHSs and call
  // arguments.
  std::string const_value_to_cxx(const ast::Expr& e,
                                 const ast::TypeExpr* target = nullptr,
                                 bool explicit_conversion = false);
  // Lower a full typed-const initializer. There is no later assignment/call
  // conversion phase for `const x: T = ...`, so aggregate fields/elements keep
  // this mode while passing their own target types down to initializer leaves.
  std::string typed_const_value_to_cxx(const ast::Expr& e,
                                       const ast::TypeExpr* target,
                                       bool explicit_conversion = false);
  std::optional<std::string> maybe_convert_const_int_expr(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion);

 private:
  std::string const_value_to_cxx_impl(const ast::Expr& e,
                                      const ast::TypeExpr* target,
                                      bool explicit_conversion,
                                      bool typed_const_initializer);
  std::optional<std::string> maybe_convert_proc_value(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion);
  std::optional<std::string> reject_method_pointer_record_cast(
      const ast::Expr& e, const ast::TypeExpr* target,
      const ast::TyProcedural& proc, const ast::TypeExpr* source_type,
      bool explicit_conversion);
  std::optional<std::string> maybe_convert_plain_proc_value(
      const ast::Expr& e, const ast::TyProcedural& proc,
      bool explicit_conversion);
  std::string plain_proc_callee_text(const ast::Expr& e);
  std::string plain_proc_adapter_value(const ast::Expr& e,
                                       const ast::TyProcedural& proc,
                                       const ast::ProcDecl& decl);
  std::optional<std::string> maybe_lower_metaclass_value(
      const ast::Expr& e, const ast::TypeExpr* target);
  bool source_is_const_untyped_storage_arg(const ast::Expr& e) const;
  std::string apply_target_pointer_conversion(const ast::Expr& e,
                                              const ast::TypeExpr* target,
                                              const ast::TypeExpr* source_type,
                                              std::string out,
                                              bool explicit_conversion);
  std::optional<std::string> maybe_lower_target_pointer_arithmetic(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion, bool typed_const_initializer);
  std::string pchar_string_literal_to_cxx(const ast::StringLit& lit);
  const ast::TypeExpr* set_literal_member_source_type(const ast::Expr& e);
  const ast::TypeExpr* metaclass_value_base_type(const ast::Expr& e);
  bool same_cxx_type(const ast::TypeExpr* a, const ast::TypeExpr* b);
  bool reject_metaclass_member_as_plain_proc_value(const ast::Expr& value);
  std::string concrete_class_name_for_metaclass_value(const ast::Expr& src);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  EmitResolution& resolution_;
  OverloadTypeProvider& overload_types_;
  EmitValueExprOps& expr_ops_;
};

}  // namespace tp2cc
