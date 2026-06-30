#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "emit_context.h"
#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

inline const std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>*
signature_nested_types_for(const TypeSymbol* symbol) {
  if (!symbol) return nullptr;
  if (const ClassInfo* ci = symbol->class_info()) return &ci->nested_types;
  if (const RecordInfo* ri = symbol->record_info()) return &ri->nested_types;
  return nullptr;
}

inline std::string type_symbol_pascal_path(const TypeSymbol& symbol) {
  std::string out;
  for (const auto& owner : symbol.owner_path) {
    if (!out.empty()) out += ".";
    out += owner;
  }
  if (!out.empty()) out += ".";
  out += symbol.name;
  return out;
}

inline std::string type_symbol_unit_pascal_path(const TypeSymbol& symbol) {
  std::string out = symbol.defining_unit;
  const std::string path = type_symbol_pascal_path(symbol);
  if (!out.empty() && !path.empty()) out += ".";
  out += path;
  return out;
}

inline const TypeSymbol* signature_type_symbol_for(
    const TypeRegistry& registry, const ScopeStateView& scope,
    std::string_view type_name) {
  // Signature syntax is lexical: a type name in `function f: TNested` belongs
  // to the unit and declaring type where that signature was declared. Later
  // emitters may render the signature from another unit or helper context, so
  // resolve through the saved nested-type scope before falling back to ordinary
  // unit visibility.
  return migration_fallback_type_symbol_by_name(registry, scope, type_name);
}

class ScopedSignatureLookupUnit {
 public:
  // Temporarily restore the unit and nested type frames that were in force at
  // the declaration site of a method/property/constructor signature.
  ScopedSignatureLookupUnit(ScopeStateView& scope, const TypeRegistry& registry,
                            std::string_view defining_unit,
                            std::string_view declaring_type,
                            const TypeLookupContext* declaration_context = nullptr)
      : scope_(scope),
        saved_unit_(scope.current_unit_name),
        saved_lookup_emission_unit_(scope.lookup_emission_unit_name),
        saved_type_scope_(scope.type_scope) {
    const TypeLookupContext* context = declaration_context;
    if (!context && !defining_unit.empty()) {
      context = registry.lookup_unit_context(defining_unit,
                                             /*implementation=*/false);
      if (!context) {
        context = registry.lookup_unit_context(defining_unit,
                                               /*implementation=*/true);
      }
    }
    if (context || (!defining_unit.empty() &&
                    defining_unit != scope_.current_unit_name)) {
      changed_ = true;
      std::string_view unit = context ? context->unit : defining_unit;
      if (!unit.empty() && unit != scope_.current_unit_name) {
        scope_.lookup_emission_unit_name =
            saved_lookup_emission_unit_.empty() ? saved_unit_
                                                : saved_lookup_emission_unit_;
        scope_.current_unit_name = std::string(unit);
      }
      scope_.type_scope = context;
    }
    (void)declaring_type;
  }

  explicit ScopedSignatureLookupUnit(ScopeStateView& scope,
                                     const TypeRegistry& registry,
                                     const MethodSig* sig)
      : ScopedSignatureLookupUnit(scope, registry,
                                  sig ? sig->defining_unit : std::string_view{},
                                  sig ? sig->declaring_type
                                      : std::string_view{},
                                  sig && sig->decl
                                      ? registry.lookup_proc_signature_context(
                                            sig->decl.get())
                                      : nullptr) {}

  ScopedSignatureLookupUnit(const ScopedSignatureLookupUnit&) = delete;
  ScopedSignatureLookupUnit& operator=(const ScopedSignatureLookupUnit&) =
      delete;

  ~ScopedSignatureLookupUnit() {
    if (!changed_) return;
    scope_.current_unit_name = saved_unit_;
    scope_.lookup_emission_unit_name = saved_lookup_emission_unit_;
    scope_.type_scope = saved_type_scope_;
  }

 private:
  ScopeStateView& scope_;
  std::string saved_unit_;
  std::string saved_lookup_emission_unit_;
  const TypeLookupContext* saved_type_scope_ = nullptr;
  bool changed_ = false;
};

inline std::shared_ptr<ast::TyName> qualified_signature_type_name(
    const TypeRegistry& registry, ScopeStateView& scope,
    const ast::TypeExpr* param_type, std::string_view param_unit,
    std::string_view param_declaring_type) {
  if (!param_type || param_type->kind != ast::Kind::TyName) return nullptr;
  const auto& tn = static_cast<const ast::TyName&>(*param_type);
  if (tn.name == "nil" || is_primitive_type(tn.name) ||
      !runtime_named_type_cxx(tn.name).empty()) {
    return nullptr;
  }
  if (registry.resolved_symbol_for_type(param_type) ||
      registry.lookup_context_for_type(param_type)) {
    return nullptr;
  }
  ScopedSignatureLookupUnit signature_scope(scope, registry, param_unit,
                                            param_declaring_type);
  const TypeLookupContext* saved_context = scope.type_scope;
  if (const TypeLookupContext* context =
          registry.lookup_context_for_type(param_type)) {
    scope.type_scope = context;
  }
  const TypeSymbol* symbol =
      signature_type_symbol_for(registry, scope, tn.name);
  scope.type_scope = saved_context;
  if (!symbol || symbol->defining_unit.empty() ||
      symbol->defining_unit == "__rt__") {
    return nullptr;
  }
  auto qualified = std::make_shared<ast::TyName>(tn);
  qualified->name = type_symbol_unit_pascal_path(*symbol);
  return qualified;
}

}  // namespace tp2cc
