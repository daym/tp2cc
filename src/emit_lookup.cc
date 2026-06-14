#include "emit_lookup.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "emit_analysis.h"
#include "emit_properties.h"
#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

static const ProcInfo* unique_zero_arg_proc(
    const std::vector<ProcInfo>* procs) {
  if (!procs) return nullptr;
  const ProcInfo* found = nullptr;
  for (const auto& proc : *procs) {
    if (!proc.accepts_zero_args) continue;
    if (found) return nullptr;
    found = &proc;
  }
  return found;
}

static const MethodSig* unique_zero_arg_method(
    const std::vector<MethodSig>* methods) {
  if (!methods) return nullptr;
  const MethodSig* found = nullptr;
  for (const auto& method : *methods) {
    if (!method.accepts_zero_args) continue;
    if (found) return nullptr;
    found = &method;
  }
  return found;
}

static ResolveResult resolved_value(ResolvedKind kind, std::string cxx) {
  return ResolveResult(kind, std::move(cxx));
}

static ResolveResult zero_arg_callable(ResolvedKind kind, std::string cxx) {
  return ResolveResult::callable(kind, std::move(cxx),
                                 /*is_parameterless=*/true, nullptr,
                                 /*accepts_zero_args=*/true, {}, {});
}

static std::optional<ResolveResult> resolve_proc_overloads(
    const std::vector<ProcInfo>* procs, ResolvedKind kind, std::string cxx) {
  if (!procs || procs->empty()) return std::nullopt;
  if (const ProcInfo* proc = unique_zero_arg_proc(procs)) {
    return ResolveResult::callable(kind, std::move(cxx),
                                   proc->param_count == 0, proc->decl.get(),
                                   /*accepts_zero_args=*/true,
                                   proc->return_type_name,
                                   proc->defining_unit);
  }
  return ResolveResult::callable(kind, std::move(cxx),
                                 /*is_parameterless=*/false, nullptr,
                                 /*accepts_zero_args=*/false, {}, {});
}

static std::optional<ResolveResult> resolve_method_overloads(
    const std::vector<MethodSig>* methods, ResolvedKind kind, std::string cxx) {
  if (!methods || methods->empty()) return std::nullopt;
  if (const MethodSig* method = unique_zero_arg_method(methods)) {
    return ResolveResult::callable(kind, std::move(cxx),
                                   method->param_count == 0,
                                   method->decl.get(),
                                   /*accepts_zero_args=*/true, {},
                                   method->defining_unit,
                                   method->declaring_type);
  }
  return ResolveResult::callable(kind, std::move(cxx),
                                 /*is_parameterless=*/false, nullptr,
                                 /*accepts_zero_args=*/false, {}, {});
}

static std::optional<ResolveResult> resolve_nested_overloads(
    const std::vector<ScopeStateView::NestedFn>& overloads,
    std::string_view current_unit_name) {
  if (overloads.empty()) return std::nullopt;
  const ScopeStateView::NestedFn* zero_arg = nullptr;
  for (const auto& overload : overloads) {
    if (!overload.accepts_zero_args) continue;
    if (zero_arg) {
      return ResolveResult::callable(
          ResolvedKind::NestedFn, overloads.front().cxx_name,
          /*is_parameterless=*/false, nullptr,
          /*accepts_zero_args=*/false, {}, {});
    }
    zero_arg = &overload;
  }
  if (zero_arg) {
    return ResolveResult::callable(
        ResolvedKind::NestedFn, zero_arg->cxx_name, zero_arg->param_count == 0,
        zero_arg->decl, zero_arg->accepts_zero_args, {},
        std::string(current_unit_name));
  }
  return ResolveResult::callable(
      ResolvedKind::NestedFn, overloads.front().cxx_name,
      /*is_parameterless=*/false, nullptr,
      /*accepts_zero_args=*/false, {}, {});
}

EmitLookup::EmitLookup(const TypeRegistry* registry, ScopeStateView& scope,
                       EmitAnalysis& analysis, EmitProperties& properties)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      properties_(properties) {}

std::optional<ResolveResult> EmitLookup::resolve_exported_unit_name(
    const std::string& unit_name, const std::string& name) {
  auto it = registry_->units.find(unit_name);
  if (it == registry_->units.end()) return std::nullopt;
  const UnitInfo& unit = it->second;
  // Synthetic `__rt__` unit holds runtime builtins. Emit them fully qualified
  // so translated units do not depend on `using namespace ::rt;`.
  const std::string prefix = unit_namespace_prefix(unit_name);
  if (auto result = resolve_proc_overloads(
          unit.find_export_procs(name),
          (unit_name == "__rt__") ? ResolvedKind::RtBuiltin
                                  : ResolvedKind::UnitProc,
          prefix + mangle(name))) {
    return result;
  }
  if (unit.find_export_var(name)) {
    return resolved_value(ResolvedKind::UnitVar, prefix + mangle(name));
  }
  if (unit.find_export_const(name)) {
    return resolved_value(ResolvedKind::UnitConst, prefix + mangle(name));
  }
  if (unit.has_export_enum_member(name)) {
    return resolved_value(ResolvedKind::EnumMember, prefix + mangle(name));
  }
  if (unit.has_export_type(name)) {
    return resolved_value(ResolvedKind::UnitType, prefix + mangle(name));
  }
  return std::nullopt;
}

ResolveResult EmitLookup::resolve_name(const std::string& name,
                                       QualifierKind qk,
                                       const std::string& qualifier) {
  // ----- Qualified lookups first: `Unit.name` / `Class.name`. -----
  if (qk == QualifierKind::Unit) {
    const std::string unit_cxx = unit_namespace_prefix(qualifier) + mangle(name);
    if (registry_) {
      auto uit = registry_->units.find(qualifier);
      if (uit != registry_->units.end()) {
        const UnitInfo& u = uit->second;
        const bool own_unit = qualifier == scope_.current_unit_name;
        const std::vector<ProcInfo>* procs =
            own_unit ? u.find_procs(name) : u.find_export_procs(name);
        if (auto result =
                resolve_proc_overloads(procs, ResolvedKind::UnitProc, unit_cxx)) {
          return *result;
        }
        if (own_unit ? u.find_var(name) : u.find_export_var(name)) {
          return resolved_value(ResolvedKind::UnitVar, unit_cxx);
        }
        if (own_unit ? u.find_const(name) : u.find_export_const(name)) {
          return resolved_value(ResolvedKind::UnitConst, unit_cxx);
        }
        if (own_unit ? u.has_enum_member(name) : u.has_export_enum_member(name)) {
          return resolved_value(ResolvedKind::EnumMember, unit_cxx);
        }
        if (own_unit ? u.has_type(name) : u.has_export_type(name)) {
          return resolved_value(ResolvedKind::UnitType, unit_cxx);
        }
      }
    }
    return resolved_value(ResolvedKind::Unknown, {});
  }
  if (qk == QualifierKind::Class) {
    const std::string member_cxx = mangle(name);
    if (registry_) {
      if (auto result = resolve_method_overloads(
              registry_->lookup_class_methods(qualifier, name,
                                              scope_.current_unit_name),
              ResolvedKind::ClassMethod, member_cxx)) {
        return *result;
      }
      if (registry_->lookup_class_field(
              qualifier, name, scope_.current_unit_name)) {
        return resolved_value(ResolvedKind::ClassField,
                              registry_->field_cxx_name(name));
      }
    }
    return resolved_value(ResolvedKind::Unknown, member_cxx);
  }

  // ----- Unqualified lookup. -----

  // 1. Function-name-as-read inside its own body -> the implicit Pascal
  //    result variable.
  if (scope_.current_fn_is_function && scope_.current_fn_result_type &&
      !scope_.current_fn_name.empty() && name == scope_.current_fn_name) {
    return resolved_value(ResolvedKind::ResultSlot,
                          scope_.current_result_slot_name);
  }
  if (scope_.bare_result_type && is_pascal_result_ident(name)) {
    return resolved_value(ResolvedKind::ResultSlot, scope_.bare_result_slot_name);
  }
  if (scope_.outer_result_type && !scope_.outer_result_name.empty() &&
      name == scope_.outer_result_name) {
    return resolved_value(ResolvedKind::ResultSlot,
                          scope_.outer_result_slot_name);
  }

  // 2. `with X do` bindings (inside-out). Fields and methods of X's
  //    class (walking ancestors) shadow outer scopes.
  if (registry_) {
    for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
         ++it) {
      const std::string& cls = it->class_name;
      const std::string& access = it->access_op;
      if (const auto* ci = analysis_.class_info_for_type_name(cls);
          ci && ci->is_reference_type &&
          (name == "classtype" || name == "instancesize")) {
        return zero_arg_callable(ResolvedKind::WithMethod,
                                 it->cxx_text + access + mangle(name));
      }
      // `with obj do ... Free;` is the bare-identifier form of `obj.Free`.
      // Resolve it through the active `with` expression so inherited TObject
      // methods are still available even though TObject is a runtime class
      // rather than an entry in `registry->classes`.
      if (const auto* ci = analysis_.class_info_for_type_name(cls);
          ci && ci->is_reference_type && name == "free") {
        // The expression is already a complete call; no implicit-zero-arg
        // wrap is wanted at the use site.
        return resolved_value(ResolvedKind::WithMethod,
                              "::rt::t_tobject::p_free(" + it->cxx_text + ")");
      }
      if (!cls.empty()) {
        if (auto result = resolve_method_overloads(
                registry_->lookup_class_methods(cls, name,
                                                scope_.current_unit_name),
                ResolvedKind::WithMethod,
                it->cxx_text + access + mangle(name))) {
          return *result;
        }
        if (registry_->lookup_class_field(
                cls, name, scope_.current_unit_name)) {
          return resolved_value(ResolvedKind::WithField,
                                it->cxx_text + access +
                                    registry_->field_cxx_name(name));
        }
      }
      if (const ast::TypeExpr* field_type =
              analysis_.lookup_record_field_type_in_with(*it, name)) {
        if (it->bytewise_storage &&
            (it->bytewise_storage->field_selection ==
                 ScopeStateView::WithBind::BytewiseStorage::FieldSelection::
                     AllFields ||
             analysis_.record_field_is_variant_in_type(it->type, name))) {
          const auto& bytewise = *it->bytewise_storage;
          return ResolveResult::bytewise_with_field_result(
              field_type, bytewise.ptr_cxx, bytewise.type_cxx,
              registry_->field_cxx_name(name), bytewise.unaligned);
        }
        return resolved_value(ResolvedKind::WithField,
                              it->cxx_text + access +
                                  registry_->field_cxx_name(name));
      }
    }
  }

  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = scope_.local_nested_fns.find(name);
    if (nit != scope_.local_nested_fns.end()) {
      if (auto resolved =
              resolve_nested_overloads(nit->second, scope_.current_unit_name)) {
        return *resolved;
      }
    }
  }

  // 4. Procedure-local (param, var, typed const, nested-proc-name).
  if (scope_.local_scope.count(name)) {
    return resolved_value(ResolvedKind::Local, mangle(name));
  }
  if (scope_.local_consts.count(name)) {
    return resolved_value(ResolvedKind::Local, mangle(name));
  }
  const std::string low_name = ascii_lower(name);
  for (const TypeScopeFrame* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    for (const auto& [symbol_name, symbol] : frame->symbols) {
      (void)symbol_name;
      const EnumInfoReg* info = symbol.enum_info();
      if (!info || !info->type) continue;
      for (const auto& member : info->type->members) {
        if (ascii_lower(member.name) == low_name) {
          return resolved_value(ResolvedKind::EnumMember, mangle(name));
        }
      }
    }
  }
  if (auto prop = properties_.maybe_resolve_implicit_property(name)) {
    return *prop;
  }

  // 5. Current class's members (chain).
  if (!scope_.current_class_name.empty() && registry_) {
    if (const auto* ci =
            analysis_.class_info_for_type_name(scope_.current_class_name);
        ci && ci->is_reference_type &&
        (name == "classtype" || name == "instancesize")) {
      return zero_arg_callable(ResolvedKind::ClassMethod, mangle(name));
    }
    if (auto result = resolve_method_overloads(
            registry_->lookup_class_methods(scope_.current_class_name, name,
                                            scope_.current_unit_name),
            ResolvedKind::ClassMethod, mangle(name))) {
      return *result;
    }
    if (registry_->lookup_class_field(scope_.current_class_name, name,
                                      scope_.current_unit_name)) {
      return resolved_value(ResolvedKind::ClassField,
                            registry_->field_cxx_name(name));
    }
    if (registry_->class_has_enum_member(scope_.current_class_name, name,
                                         scope_.current_unit_name)) {
      return resolved_value(ResolvedKind::EnumMember, mangle(name));
    }
  }

  // 6. Unit-level -- own unit first, then cross-unit (`uses` chain).
  if (registry_) {
    auto uit = registry_->units.find(scope_.current_unit_name);
    const UnitInfo* ui = (uit != registry_->units.end()) ? &uit->second
                                                          : nullptr;
    const std::string own_prefix =
        (!scope_.lookup_emission_unit_name.empty() &&
         scope_.lookup_emission_unit_name != scope_.current_unit_name)
            ? unit_namespace_prefix(scope_.current_unit_name)
            : std::string{};
    // Current unit's own symbols shadow everything from `uses`. Normal
    // emission leaves them bare. When lookup is redirected to a declaration
    // unit but the generated C++ is inserted in another unit, those
    // declaration-unit symbols need an explicit namespace prefix there.
    if (ui) {
      if (auto result = resolve_proc_overloads(
              ui->find_procs(name), ResolvedKind::UnitProc,
              own_prefix + mangle(name))) {
        return *result;
      }
      if (ui->find_var(name)) {
        return resolved_value(ResolvedKind::UnitVar, own_prefix + mangle(name));
      }
      if (ui->find_const(name)) {
        return resolved_value(ResolvedKind::UnitConst, own_prefix + mangle(name));
      }
      if (ui->has_enum_member(name)) {
        return resolved_value(ResolvedKind::EnumMember,
                              own_prefix + mangle(name));
      }
      if (ui->has_type(name)) {
        return resolved_value(ResolvedKind::UnitType, own_prefix + mangle(name));
      }
    }

    if (ui) {
      // Right-to-left is Pascal's uses resolution order. Keep the synthetic
      // `__rt__` unit as the last resort so real imported units can shadow
      // runtime builtin names such as FPU exception enum members.
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (*it == "__rt__") continue;
        if (auto result = resolve_exported_unit_name(*it, name)) return *result;
      }
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (*it != "__rt__") continue;
        if (auto result = resolve_exported_unit_name(*it, name)) return *result;
      }
    }
  }

  // 7. Unresolved free name: keep it in Pascal identifier space.
  // Known runtime helpers already resolve through the synthetic `__rt__`
  // unit on every unit's uses-chain; reaching this branch therefore means
  // "lookup failed", not "implicit runtime builtin".
  return resolved_value(ResolvedKind::Unknown, mangle(name));
}

}  // namespace tp2cc
