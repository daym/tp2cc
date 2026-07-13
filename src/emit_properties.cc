#include "emit_properties.h"

#include <string>
#include <vector>

#include "emit_support.h"
namespace tp2cc {

using namespace ast;

EmitProperties::EmitProperties(EmitAnalysis& analysis,
                               EmitPropertyExprOps& expr_ops)
    : analysis_(analysis), expr_ops_(expr_ops) {}

std::optional<std::string> EmitProperties::maybe_property_read_text(
    const std::string& base_cxx, std::string_view base_access,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  // Properties are Pascal-side metadata only. Reads/writes rewrite to the
  // declared backing field/getter/setter so we do not invent extra C++
  // members whose names could collide in ways Pascal itself forbids.
  const std::string access(base_access);
  if (prop.read.kind == PropertyAccessorKind::FieldPath) {
    std::string text = base_cxx + access + prop.read.cxx_path;
    for (const auto* idx : indices) {
      text += "[" + expr_ops_.expr_to_cxx(*idx) + "]";
    }
    return {text};
  }
  if (prop.read.kind == PropertyAccessorKind::Method) {
    std::string text = base_cxx + access + mangle(prop.read.method_name) + "(";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i) text += ", ";
      text += expr_ops_.expr_to_cxx(*indices[i]);
    }
    text += ")";
    return {text};
  }
  return std::nullopt;
}

std::string EmitProperties::lower_property_read(
    Location where, const std::string& base_cxx,
    std::string_view base_access,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  if (auto text =
          maybe_property_read_text(base_cxx, base_access, prop, indices)) {
    return *text;
  }
  expr_ops_.report_error(
      where, "unsupported property read accessor '" + prop.read.display_name() +
                 "'");
  return base_cxx;
}

std::optional<std::string> EmitProperties::maybe_property_write_text(
    const std::string& base_cxx, std::string_view base_access,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  const std::string access(base_access);
  if (prop.write.empty()) {
    return std::nullopt;
  }
  std::string rhs =
      expr_ops_.const_value_to_cxx(value, prop.type.get(), false);
  if (prop.write.kind == PropertyAccessorKind::FieldPath) {
    std::string text = base_cxx + access + prop.write.cxx_path;
    for (const auto* idx : indices) {
      text += "[" + expr_ops_.expr_to_cxx(*idx) + "]";
    }
    return {text + " = " + rhs};
  }
  if (prop.write.kind == PropertyAccessorKind::Method) {
    std::string text = base_cxx + access + mangle(prop.write.method_name) + "(";
    bool first = true;
    for (const auto* idx : indices) {
      if (!first) text += ", ";
      text += expr_ops_.expr_to_cxx(*idx);
      first = false;
    }
    if (!first) text += ", ";
    text += rhs;
    text += ")";
    return {text};
  }
  return std::nullopt;
}

std::string EmitProperties::lower_property_write(
    Location where, const std::string& base_cxx,
    std::string_view base_access,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  if (auto text =
          maybe_property_write_text(base_cxx, base_access, prop, indices,
                                    value)) {
    return *text;
  }
  if (prop.write.empty()) {
    expr_ops_.report_error(where, "property is read-only");
  } else {
    expr_ops_.report_error(
        where, "unsupported property write accessor '" +
                   prop.write.display_name() + "'");
  }
  return base_cxx;
}

std::optional<ResolveResult> EmitProperties::maybe_resolve_implicit_property(
    std::string_view name) {
  auto found = analysis_.find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  if (auto text = maybe_property_read_text(found->base_cxx, found->base_access,
                                           *found->prop, no_indices)) {
    return ResolveResult(found->from_with ? ResolvedKind::WithProperty
                                          : ResolvedKind::ClassProperty,
                         *text);
  }
  return std::nullopt;
}

std::optional<std::string> EmitProperties::maybe_lower_implicit_property_write(
    Location where, std::string_view name, const Expr& value) {
  auto found = analysis_.find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  return lower_property_write(where, found->base_cxx, found->base_access,
                              *found->prop, no_indices, value);
}

}  // namespace tp2cc
