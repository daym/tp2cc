#include "emit_lookup.h"

#include <string>
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

static bool adopt_proc_overloads(ResolveResult& r,
                                 const std::vector<ProcInfo>* procs,
                                 ResolvedKind kind,
                                 const std::string& cxx) {
  if (!procs || procs->empty()) return false;
  r.kind = kind;
  r.cxx = cxx;
  r.is_callable = true;
  if (const ProcInfo* proc = unique_zero_arg_proc(procs)) {
    r.proc = proc->decl.get();
    r.is_parameterless = (proc->param_count == 0);
    r.accepts_zero_args = true;
    r.return_type_name = proc->return_type_name;
    r.default_arg_unit = proc->defining_unit;
  }
  return true;
}

static bool adopt_method_overloads(ResolveResult& r,
                                   const std::vector<MethodSig>* methods,
                                   ResolvedKind kind,
                                   const std::string& cxx) {
  if (!methods || methods->empty()) return false;
  r.kind = kind;
  r.cxx = cxx;
  r.is_callable = true;
  if (const MethodSig* method = unique_zero_arg_method(methods)) {
    r.proc = method->decl.get();
    r.is_parameterless = (method->param_count == 0);
    r.accepts_zero_args = true;
    r.default_arg_unit = method->defining_unit;
  }
  return true;
}

EmitLookup::EmitLookup(const TypeRegistry* registry, ScopeStateView& scope,
                       EmitAnalysis& analysis, EmitProperties& properties)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      properties_(properties) {}

ResolveResult EmitLookup::resolve_name(const std::string& name,
                                       QualifierKind qk,
                                       const std::string& qualifier) {
  ResolveResult r;

  // ----- Qualified lookups first: `Unit.name` / `Class.name`. -----
  if (qk == QualifierKind::Unit) {
    r.cxx = unit_namespace_prefix(qualifier) + mangle(name);
    if (registry_) {
      auto uit = registry_->units.find(qualifier);
      if (uit != registry_->units.end()) {
        const UnitInfo& u = uit->second;
        const bool own_unit = qualifier == scope_.current_unit_name;
        const std::vector<ProcInfo>* procs =
            own_unit ? u.find_procs(name) : u.find_export_procs(name);
        if (adopt_proc_overloads(r, procs, ResolvedKind::UnitProc, r.cxx)) {
          return r;
        }
        if (own_unit ? u.find_var(name) : u.find_export_var(name)) {
          r.kind = ResolvedKind::UnitVar;
          return r;
        }
        if (own_unit ? u.find_const(name) : u.find_export_const(name)) {
          r.kind = ResolvedKind::UnitConst;
          return r;
        }
        if (own_unit ? u.has_enum_member(name) : u.has_export_enum_member(name)) {
          r.kind = ResolvedKind::EnumMember;
          return r;
        }
        if (own_unit ? u.has_type(name) : u.has_export_type(name)) {
          r.kind = ResolvedKind::UnitType;
          return r;
        }
      }
    }
    // RTL unit unavailable to translation. Keep the Pascal unit qualifier in
    // the emitted text and let the runtime's stub namespace alias own that
    // lookup.
    r.kind = ResolvedKind::Unknown;
    return r;
  }
  if (qk == QualifierKind::Class) {
    if (registry_) {
      if (adopt_method_overloads(
              r,
              registry_->lookup_class_methods(qualifier, name,
                                              scope_.current_unit_name),
              ResolvedKind::ClassMethod,
              mangle(name))) {
        return r;
      }
      if (registry_->lookup_class_field(
              qualifier, name, scope_.current_unit_name)) {
        r.kind = ResolvedKind::ClassField;
        r.cxx = registry_->field_cxx_name(name);
        return r;
      }
    }
    r.cxx = mangle(name);
    r.kind = ResolvedKind::Unknown;
    return r;
  }

  // ----- Unqualified lookup. -----

  // 1. Function-name-as-read inside its own body -> the implicit Pascal
  //    result variable.
  if (scope_.current_fn_is_function && scope_.current_fn_result_type &&
      !scope_.current_fn_name.empty() && name == scope_.current_fn_name) {
    r.cxx = scope_.current_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (scope_.bare_result_type && is_pascal_result_ident(name)) {
    r.cxx = scope_.bare_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (scope_.outer_result_type && !scope_.outer_result_name.empty() &&
      name == scope_.outer_result_name) {
    r.cxx = scope_.outer_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
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
        r.cxx = it->cxx_text + access + mangle(name);
        r.kind = ResolvedKind::WithMethod;
        r.is_callable = true;
        r.is_parameterless = true;
        r.accepts_zero_args = true;
        return r;
      }
      // `with obj do ... Free;` is the bare-identifier form of `obj.Free`.
      // Resolve it through the active `with` expression so inherited TObject
      // methods are still available even though TObject is a runtime class
      // rather than an entry in `registry->classes`.
      if (const auto* ci = analysis_.class_info_for_type_name(cls);
          ci && ci->is_reference_type && name == "free") {
        r.cxx = "::rt::t_tobject::p_free(" + it->cxx_text + ")";
        r.kind = ResolvedKind::WithMethod;
        // The expression is already a complete call; no implicit-zero-arg
        // wrap is wanted at the use site.
        r.is_callable = false;
        return r;
      }
      if (!cls.empty()) {
        if (adopt_method_overloads(
                r,
                registry_->lookup_class_methods(cls, name,
                                                scope_.current_unit_name),
                ResolvedKind::WithMethod,
                it->cxx_text + access + mangle(name))) {
          return r;
        }
        if (registry_->lookup_class_field(
                cls, name, scope_.current_unit_name)) {
          r.cxx = it->cxx_text + access + registry_->field_cxx_name(name);
          r.kind = ResolvedKind::WithField;
          return r;
        }
      }
      if (analysis_.lookup_record_field_type_in_with(*it, name)) {
        r.cxx = it->cxx_text + access + registry_->field_cxx_name(name);
        r.kind = ResolvedKind::WithField;
        return r;
      }
    }
  }

  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = scope_.local_nested_fns.find(name);
    if (nit != scope_.local_nested_fns.end()) {
      r.kind = ResolvedKind::NestedFn;
      r.cxx = mangle(name);
      r.proc = nit->second.decl;
      r.is_callable = true;
      r.is_parameterless = (nit->second.param_count == 0);
      r.accepts_zero_args = nit->second.accepts_zero_args;
      r.default_arg_unit = scope_.current_unit_name;
      return r;
    }
  }

  // 4. Procedure-local (param, var, typed const, nested-proc-name).
  if (scope_.local_scope.count(name)) {
    r.kind = ResolvedKind::Local;
    r.cxx = mangle(name);
    return r;
  }
  if (scope_.local_consts.count(name)) {
    r.kind = ResolvedKind::Local;
    r.cxx = mangle(name);
    return r;
  }
  for (const auto& [_, en] : scope_.local_enums) {
    if (!en) continue;
    for (const auto& member : en->members) {
      if (ascii_lower(member.name) == ascii_lower(name)) {
        r.kind = ResolvedKind::EnumMember;
        r.cxx = mangle(name);
        return r;
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
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassMethod;
      r.is_callable = true;
      r.is_parameterless = true;
      r.accepts_zero_args = true;
      return r;
    }
    if (adopt_method_overloads(
            r,
            registry_->lookup_class_methods(scope_.current_class_name, name,
                                            scope_.current_unit_name),
            ResolvedKind::ClassMethod,
            mangle(name))) {
      return r;
    }
    if (registry_->lookup_class_field(scope_.current_class_name, name,
                                      scope_.current_unit_name)) {
      r.cxx = registry_->field_cxx_name(name);
      r.kind = ResolvedKind::ClassField;
      return r;
    }
    if (registry_->class_has_enum_member(scope_.current_class_name, name,
                                         scope_.current_unit_name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::EnumMember;
      return r;
    }
  }

  // 6. Unit-level -- own unit first, then cross-unit (`uses` chain).
  if (registry_) {
    auto uit = registry_->units.find(scope_.current_unit_name);
    const UnitInfo* ui = (uit != registry_->units.end()) ? &uit->second
                                                          : nullptr;
    const std::string own_prefix =
        (!scope_.default_arg_emission_unit_name.empty() &&
         scope_.default_arg_emission_unit_name != scope_.current_unit_name)
            ? unit_namespace_prefix(scope_.current_unit_name)
            : std::string{};
    // Current unit's own symbols shadow everything from `uses`. Normal
    // emission leaves them bare. Default-argument lowering is different:
    // lookup is intentionally in the declaration unit, but the C++ argument is
    // inserted at a caller in another unit, so declaration-unit symbols need
    // an explicit namespace prefix there.
    if (ui) {
      if (adopt_proc_overloads(r, ui->find_procs(name),
                               ResolvedKind::UnitProc,
                               own_prefix + mangle(name))) {
        return r;
      }
      if (ui->find_var(name)) {
        r.cxx = own_prefix + mangle(name);
        r.kind = ResolvedKind::UnitVar;
        return r;
      }
      if (ui->find_const(name)) {
        r.cxx = own_prefix + mangle(name);
        r.kind = ResolvedKind::UnitConst;
        return r;
      }
      if (ui->has_enum_member(name)) {
        r.cxx = own_prefix + mangle(name);
        r.kind = ResolvedKind::EnumMember;
        return r;
      }
      if (ui->has_type(name)) {
        r.cxx = own_prefix + mangle(name);
        r.kind = ResolvedKind::UnitType;
        return r;
      }
    }

    // Cross-unit lookup: walk the current unit's `uses` list and pick
    // the first match in a unit that actually exports this name.
    // Ambiguity between same-named symbols in two `using namespace`'d
    // units is resolved by emitting the fully-qualified form.
    auto check_unit = [&](const std::string& un) -> bool {
      auto it = registry_->units.find(un);
      if (it == registry_->units.end()) return false;
      const UnitInfo& u = it->second;
      // Synthetic `__rt__` unit holds runtime builtins. Emit them fully
      // qualified so translated units do not depend on `using namespace
      // ::rt;` for correctness.
      const std::string prefix = unit_namespace_prefix(un);
      // Other units contribute only their interface-exported names.
      if (adopt_proc_overloads(r, u.find_export_procs(name),
                               (un == "__rt__") ? ResolvedKind::RtBuiltin
                                                : ResolvedKind::UnitProc,
                               prefix + mangle(name))) {
        return true;
      }
      if (u.find_export_var(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitVar;
        return true;
      }
      if (u.find_export_const(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitConst;
        return true;
      }
      if (u.has_export_enum_member(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::EnumMember;
        return true;
      }
      if (u.has_export_type(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitType;
        return true;
      }
      return false;
    };
    if (ui) {
      // Right-to-left is Pascal's uses resolution order. Keep the synthetic
      // `__rt__` unit as the last resort so real imported units can shadow
      // runtime builtin names such as FPU exception enum members.
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (*it == "__rt__") continue;
        if (check_unit(*it)) return r;
      }
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (*it != "__rt__") continue;
        if (check_unit(*it)) return r;
      }
    }
  }

  // 7. Fallback: keep unresolved free names in Pascal identifier space.
  // Known runtime helpers already resolve through the synthetic `__rt__`
  // unit on every unit's uses-chain; reaching this fallback therefore means
  // "lookup failed", not "implicit runtime builtin".
  r.cxx = mangle(name);
  r.kind = ResolvedKind::Unknown;
  return r;
}

}  // namespace tp2cc
