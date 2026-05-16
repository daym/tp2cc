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

// Pascal value lowering owns the rules for spelling set literals, typed
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
  std::string const_value_to_cxx(const ast::Expr& e,
                                 const ast::TypeExpr* target = nullptr,
                                 bool explicit_conversion = false);
  std::optional<std::string> maybe_convert_const_int_expr(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion);

 private:
  std::optional<std::string> maybe_convert_proc_value(
      const ast::Expr& e, const ast::TypeExpr* target);
  std::optional<std::string> maybe_lower_metaclass_value(
      const ast::Expr& e, const ast::TypeExpr* target);
  std::string pchar_string_literal_to_cxx(const ast::StringLit& lit);

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
