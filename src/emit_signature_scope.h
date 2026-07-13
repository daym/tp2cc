#pragma once

#include <string>
#include <string_view>

#include "emit_context.h"
#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

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

inline std::string class_info_pascal_path(const ClassInfo& info) {
  std::string out;
  for (const auto& owner : info.owner_path) {
    if (!out.empty()) out += ".";
    out += owner;
  }
  if (!out.empty()) out += ".";
  out += info.name;
  return out;
}

inline std::string type_symbol_unit_pascal_path(const TypeSymbol& symbol) {
  std::string out = symbol.defining_unit;
  const std::string path = type_symbol_pascal_path(symbol);
  if (!out.empty() && !path.empty()) out += ".";
  out += path;
  return out;
}

class ScopedSignatureLookupUnit {
 public:
  // Temporarily restore the unit and nested type frames that were in force at
  // the declaration site of a method/property/constructor signature.
  ScopedSignatureLookupUnit(ScopeStateView& scope, const TypeRegistry& registry,
                            const MethodSig* sig)
      : scope_(scope),
        saved_unit_(scope.current_unit_name),
        saved_lookup_emission_unit_(scope.lookup_emission_unit_name),
        saved_type_scope_(scope.type_scope) {
    const TypeLookupContext* context =
        sig && sig->decl ? registry.lookup_proc_signature_context(sig->decl.get())
                         : nullptr;
    std::string_view defining_unit =
        sig ? sig->defining_unit : std::string_view{};
    if (!context && !defining_unit.empty()) {
      context = registry.lookup_unit_context(pascal_key(defining_unit),
                                             /*implementation=*/false);
      if (!context) {
        context = registry.lookup_unit_context(pascal_key(defining_unit),
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
  }

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

}  // namespace tp2cc
