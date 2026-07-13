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
                                 /*accepts_zero_args=*/true, {});
}

static std::optional<ResolveResult> resolve_proc_overloads(
    const std::vector<ProcInfo>* procs, ResolvedKind kind, std::string cxx) {
  if (!procs || procs->empty()) return std::nullopt;
  if (const ProcInfo* proc = unique_zero_arg_proc(procs)) {
    return ResolveResult::callable(kind, std::move(cxx),
                                   proc->param_count == 0, proc->decl.get(),
                                   /*accepts_zero_args=*/true,
                                   proc->defining_unit);
  }
  return ResolveResult::callable(kind, std::move(cxx),
                                 /*is_parameterless=*/false, nullptr,
                                 /*accepts_zero_args=*/false, {});
}

static std::optional<ResolveResult> resolve_method_overloads(
    const std::vector<MethodSig>* methods, ResolvedKind kind, std::string cxx) {
  if (!methods || methods->empty()) return std::nullopt;
  if (const MethodSig* method = unique_zero_arg_method(methods)) {
    return ResolveResult::callable(kind, std::move(cxx),
                                   method->param_count == 0,
                                   method->decl.get(),
                                   /*accepts_zero_args=*/true,
                                   method->defining_unit);
  }
  return ResolveResult::callable(kind, std::move(cxx),
                                 /*is_parameterless=*/false, nullptr,
                                 /*accepts_zero_args=*/false, {});
}

static std::optional<ResolveResult> resolve_nested_overloads(
    const std::vector<ScopeStateView::NestedFn>& overloads) {
  if (overloads.empty()) return std::nullopt;
  const ScopeStateView::NestedFn* zero_arg = nullptr;
  for (const auto& overload : overloads) {
    if (!overload.accepts_zero_args) continue;
    if (zero_arg) {
      return ResolveResult::callable(
          ResolvedKind::NestedFn, overloads.front().cxx_name,
          /*is_parameterless=*/false, nullptr,
          /*accepts_zero_args=*/false, {});
    }
    zero_arg = &overload;
  }
  if (zero_arg) {
    return ResolveResult::callable(
        ResolvedKind::NestedFn, zero_arg->cxx_name, zero_arg->param_count == 0,
        zero_arg->decl, zero_arg->accepts_zero_args, {});
  }
  return ResolveResult::callable(
      ResolvedKind::NestedFn, overloads.front().cxx_name,
      /*is_parameterless=*/false, nullptr,
      /*accepts_zero_args=*/false, {});
}

EmitLookup::EmitLookup(const TypeRegistry& registry, ScopeStateView& scope,
                       EmitAnalysis& analysis, EmitProperties& properties)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      properties_(properties) {}

ResolveResult EmitLookup::resolve_class_member(const ClassInfo& class_info,
                                               const std::string& name) {
  const std::string member_cxx = mangle(name);
  if (auto result = resolve_method_overloads(
          registry_.lookup_class_methods(class_info, name),
          ResolvedKind::ClassMethod, member_cxx)) {
    return *result;
  }
  if (registry_.lookup_class_field(class_info, name)) {
    return resolved_value(ResolvedKind::ClassField,
                          registry_.field_cxx_name(name));
  }
  return resolved_value(ResolvedKind::Unknown, member_cxx);
}

ResolveResult EmitLookup::resolve_name(const std::string& name,
                                       QualifierKind qk,
                                       const std::string& qualifier) {
  // ----- Qualified unit lookup: `Unit.name`. -----
  if (qk == QualifierKind::Unit) {
    const std::string unit_cxx = unit_namespace_prefix(qualifier) + mangle(name);
    auto uit = registry_.units.find(qualifier);
    if (uit != registry_.units.end()) {
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
    }
    return resolved_value(ResolvedKind::Unknown, {});
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
  for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
       ++it) {
      const std::string& access = it->access_op;
      const TypeSymbol* with_symbol = it->class_symbol;
      const ClassInfo* ci =
          with_symbol ? with_symbol->class_info() : nullptr;
      const InterfaceInfo* iface =
          with_symbol ? with_symbol->interface_info() : nullptr;
      if (ci && ci->is_reference_type &&
          (name == "classtype" || name == "instancesize")) {
        return zero_arg_callable(ResolvedKind::WithMethod,
                                 it->cxx_text + access + mangle(name));
      }
      // `with obj do ... Free;` is the bare-identifier form of `obj.Free`.
      // Resolve it through the active `with` expression so inherited TObject
      // methods are still available even though TObject is a runtime class
      // rather than an entry in `registry->classes`.
      if (ci && ci->is_reference_type && name == "free") {
        // The expression is already a complete call; no implicit-zero-arg
        // wrap is wanted at the use site.
        return resolved_value(ResolvedKind::WithMethod,
                              "::rt::t_tobject::p_free(" + it->cxx_text + ")");
      }
      if (ci || iface) {
        const std::vector<MethodSig>* methods =
            ci ? registry_.lookup_class_methods(*ci, name)
               : registry_.lookup_interface_methods(*iface, name);
        if (auto result = resolve_method_overloads(
                methods,
                ResolvedKind::WithMethod,
                it->cxx_text + access + mangle(name))) {
          return *result;
        }
        if (ci && registry_.lookup_class_field(*ci, name)) {
          return resolved_value(ResolvedKind::WithField,
                                it->cxx_text + access +
                                    registry_.field_cxx_name(name));
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
              registry_.field_cxx_name(name), bytewise.unaligned);
        }
        return resolved_value(ResolvedKind::WithField,
                              it->cxx_text + access +
                                  registry_.field_cxx_name(name));
      }
  }

  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = scope_.local_nested_fns.find(name);
    if (nit != scope_.local_nested_fns.end()) {
      if (auto resolved = resolve_nested_overloads(nit->second)) {
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
  if (local_enum_info_for_member(scope_, name)) {
    return resolved_value(ResolvedKind::EnumMember, mangle(name));
  }
  if (auto prop = properties_.maybe_resolve_implicit_property(name)) {
    return *prop;
  }

  // 5. Current class's members (chain).
  const TypeSymbol* current_symbol = scope_.current_class_symbol;
  const ClassInfo* current_class =
      current_symbol ? current_symbol->class_info() : nullptr;
  if (current_class) {
    if (current_class->is_reference_type &&
        (name == "classtype" || name == "instancesize")) {
      return zero_arg_callable(ResolvedKind::ClassMethod, mangle(name));
    }
    if (auto result = resolve_method_overloads(
            registry_.lookup_class_methods(*current_class, name),
            ResolvedKind::ClassMethod, mangle(name))) {
      return *result;
    }
    if (registry_.lookup_class_field(*current_class, name)) {
      return resolved_value(ResolvedKind::ClassField,
                            registry_.field_cxx_name(name));
    }
    if (registry_.class_has_enum_member(*current_class, name)) {
      return resolved_value(ResolvedKind::EnumMember, mangle(name));
    }
  }

  // 6. Unit-level frames: current implementation, current interface, imported
  // interface frames, then implicit runtime.
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (!frame->unit_info) continue;
    const bool imported = scope_frame_is_import(*frame);
    const std::string prefix =
        imported
            ? unit_namespace_prefix(frame->unit)
            : ((!scope_.lookup_emission_unit_name.empty() &&
                scope_.lookup_emission_unit_name != frame->unit)
                   ? unit_namespace_prefix(frame->unit)
                   : std::string{});
    const ResolvedKind proc_kind =
        scope_frame_is_runtime(*frame) ? ResolvedKind::RtBuiltin
                                       : ResolvedKind::UnitProc;
    if (auto result = resolve_proc_overloads(
            scope_frame_find_procs(*frame, name), proc_kind,
            prefix + mangle(name))) {
      return *result;
    }
    if (scope_frame_find_var(*frame, name)) {
      return resolved_value(ResolvedKind::UnitVar, prefix + mangle(name));
    }
    if (scope_frame_find_const(*frame, name)) {
      return resolved_value(ResolvedKind::UnitConst, prefix + mangle(name));
    }
    if (scope_frame_has_enum_member(*frame, name)) {
      return resolved_value(ResolvedKind::EnumMember, prefix + mangle(name));
    }
  }

  // 7. Unresolved free name: keep it in Pascal identifier space.
  // Known runtime helpers already resolve through the synthetic `__rt__`
  // unit on every unit's uses-chain; reaching this branch therefore means
  // "lookup failed", not "implicit runtime builtin".
  return resolved_value(ResolvedKind::Unknown, mangle(name));
}

}  // namespace tp2cc
