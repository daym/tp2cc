#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "emit_analysis.h"
#include "emit_resolution_types.h"

namespace tp2cc {

struct PropertyInfo;

class EmitPropertyExprOps {
 public:
  virtual ~EmitPropertyExprOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  virtual std::string const_value_to_cxx(
      const ast::Expr& e, const ast::TypeExpr* target,
      bool explicit_conversion) = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

// Property lowering. This module rewrites Pascal property reads and writes to
// the declared field/getter/setter access path, and it reuses `EmitAnalysis`'s
// implicit-property lookup so method bodies and `with` scopes keep one source
// of truth for bare-property resolution.
class EmitProperties {
 public:
  EmitProperties(EmitAnalysis& analysis, EmitPropertyExprOps& expr_ops);

  std::string lower_property_read(Location where, const std::string& base_cxx,
                                  std::string_view base_access,
                                  const PropertyInfo& prop,
                                  const std::vector<const ast::Expr*>& indices);
  std::string lower_property_write(
      Location where, const std::string& base_cxx,
      std::string_view base_access, const PropertyInfo& prop,
      const std::vector<const ast::Expr*>& indices, const ast::Expr& value);

  std::optional<ResolveResult> maybe_resolve_implicit_property(
      std::string_view name);
  std::optional<std::string> maybe_lower_implicit_property_write(
      Location where, std::string_view name, const ast::Expr& value);

 private:
  std::optional<std::string> maybe_property_read_text(
      const std::string& base_cxx, std::string_view base_access,
      const PropertyInfo& prop, const std::vector<const ast::Expr*>& indices);
  std::optional<std::string> maybe_property_write_text(
      const std::string& base_cxx, std::string_view base_access,
      const PropertyInfo& prop, const std::vector<const ast::Expr*>& indices,
      const ast::Expr& value);

  EmitAnalysis& analysis_;
  EmitPropertyExprOps& expr_ops_;
};

}  // namespace tp2cc
