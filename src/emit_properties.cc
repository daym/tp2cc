#include "emit_properties.h"

#include <string>
#include <vector>

#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitProperties::EmitProperties(const TypeRegistry* registry,
                               EmitAnalysis& analysis,
                               EmitPropertyExprOps& expr_ops)
    : registry_(registry), analysis_(analysis), expr_ops_(expr_ops) {}

std::optional<std::string> EmitProperties::maybe_property_read_text(
    const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  // Properties are Pascal-side metadata only. Reads/writes rewrite to the
  // declared backing field/getter/setter so we do not invent extra C++
  // members whose names could collide in ways Pascal itself forbids.
  if (!registry_) return std::nullopt;
  const std::string access =
      (registry_->classes.count(class_name) &&
       registry_->classes.at(class_name).is_reference_type)
          ? "->"
          : ".";
  if (const auto* field =
          registry_->lookup_class_field(class_name, prop.read_name)) {
    (void)field;
    std::string text = base_cxx + access + mangle(prop.read_name);
    for (const auto* idx : indices) {
      text += "[" + expr_ops_.expr_to_cxx(*idx) + "]";
    }
    return {text};
  }
  if (const auto* method =
          registry_->lookup_class_method(class_name, prop.read_name)) {
    (void)method;
    std::string text = base_cxx + access + mangle(prop.read_name) + "(";
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
    Location where, const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  if (auto text = maybe_property_read_text(base_cxx, class_name, prop, indices)) {
    return *text;
  }
  expr_ops_.report_error(
      where, "unsupported property read accessor '" + prop.read_name + "'");
  return base_cxx + "." + mangle(prop.read_name);
}

std::optional<std::string> EmitProperties::maybe_property_write_text(
    const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  if (!registry_) return std::nullopt;
  const std::string access =
      (registry_->classes.count(class_name) &&
       registry_->classes.at(class_name).is_reference_type)
          ? "->"
          : ".";
  if (prop.write_name.empty()) {
    return std::nullopt;
  }
  std::string rhs =
      expr_ops_.const_value_to_cxx(value, prop.type.get(), false);
  if (const auto* field =
          registry_->lookup_class_field(class_name, prop.write_name)) {
    (void)field;
    std::string text = base_cxx + access + mangle(prop.write_name);
    for (const auto* idx : indices) {
      text += "[" + expr_ops_.expr_to_cxx(*idx) + "]";
    }
    return {text + " = " + rhs};
  }
  if (const auto* method =
          registry_->lookup_class_method(class_name, prop.write_name)) {
    (void)method;
    std::string text = base_cxx + access + mangle(prop.write_name) + "(";
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
    Location where, const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  if (auto text =
          maybe_property_write_text(base_cxx, class_name, prop, indices, value)) {
    return *text;
  }
  if (prop.write_name.empty()) {
    expr_ops_.report_error(where, "property is read-only");
  } else {
    expr_ops_.report_error(
        where, "unsupported property write accessor '" + prop.write_name + "'");
  }
  return base_cxx;
}

std::optional<ResolveResult> EmitProperties::maybe_resolve_implicit_property(
    std::string_view name) {
  auto found = analysis_.find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  if (auto text = maybe_property_read_text(found->base_cxx, found->class_name,
                                           *found->prop, no_indices)) {
    ResolveResult r;
    r.kind = found->from_with ? ResolvedKind::WithProperty
                              : ResolvedKind::ClassProperty;
    r.cxx = *text;
    return r;
  }
  return std::nullopt;
}

std::optional<std::string> EmitProperties::maybe_lower_implicit_property_write(
    Location where, std::string_view name, const Expr& value) {
  auto found = analysis_.find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  return lower_property_write(where, found->base_cxx, found->class_name,
                              *found->prop, no_indices, value);
}

}  // namespace tp2cc
