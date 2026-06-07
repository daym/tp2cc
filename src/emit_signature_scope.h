#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "emit_context.h"
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
    const TypeRegistry* registry, const ScopeStateView& scope,
    std::string_view type_name) {
  // Signature syntax is lexical: a type name in `function f: TNested` belongs
  // to the unit and declaring type where that signature was declared. Later
  // emitters may render the signature from another unit or helper context, so
  // resolve through the saved nested-type scope before falling back to ordinary
  // unit visibility.
  const std::string lower = ascii_lower(type_name);
  if (scope.type_scope) {
    if (const TypeSymbol* local = scope.type_scope->find_lower(lower)) {
      return local;
    }
  }
  return registry ? registry->lookup_type_symbol(lower, scope.current_unit_name)
                  : nullptr;
}

class ScopedSignatureLookupUnit {
 public:
  // Temporarily restore the unit and nested type frames that were in force at
  // the declaration site of a method/property/constructor signature.
  ScopedSignatureLookupUnit(ScopeStateView& scope, const TypeRegistry* registry,
                            std::string_view defining_unit,
                            std::string_view declaring_type)
      : scope_(scope),
        registry_(registry),
        saved_unit_(scope.current_unit_name),
        saved_lookup_emission_unit_(scope.lookup_emission_unit_name),
        saved_type_scope_(scope.type_scope) {
    if (!defining_unit.empty() && defining_unit != scope_.current_unit_name) {
      changed_ = true;
      scope_.lookup_emission_unit_name =
          saved_lookup_emission_unit_.empty() ? saved_unit_
                                              : saved_lookup_emission_unit_;
      scope_.current_unit_name = std::string(defining_unit);
      scope_.type_scope = nullptr;
    }
    if (!declaring_type.empty()) {
      seed_declaring_type_scope(declaring_type);
    }
  }

  explicit ScopedSignatureLookupUnit(ScopeStateView& scope,
                                     const TypeRegistry* registry,
                                     const MethodSig* sig)
      : ScopedSignatureLookupUnit(scope, registry,
                                  sig ? sig->defining_unit : std::string_view{},
                                  sig ? sig->declaring_type
                                      : std::string_view{}) {}

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
  void push_nested_scope_for(const TypeSymbol* symbol) {
    const auto* nested_types = signature_nested_types_for(symbol);
    if (!nested_types || nested_types->empty()) return;
    TypeScopeFrame* parent = frames_.empty() ? nullptr : frames_.back().get();
    auto frame = std::make_unique<TypeScopeFrame>(parent);
    for (const auto& [name, nested] : *nested_types) {
      (void)name;
      if (nested) frame->insert_or_assign(*nested);
    }
    frames_.push_back(std::move(frame));
  }

  void seed_declaring_type_scope(std::string_view declaring_type) {
    if (!registry_) return;
    const TypeSymbol* owner =
        registry_->lookup_type_symbol(declaring_type, scope_.current_unit_name);
    if (!owner) return;

    std::string path;
    for (const auto& segment : owner->owner_path) {
      if (!path.empty()) path += ".";
      path += segment;
      push_nested_scope_for(
          registry_->lookup_type_symbol(path, scope_.current_unit_name));
    }
    push_nested_scope_for(owner);
    if (!frames_.empty()) {
      scope_.type_scope = frames_.back().get();
      changed_ = true;
    }
  }

  ScopeStateView& scope_;
  const TypeRegistry* registry_ = nullptr;
  std::string saved_unit_;
  std::string saved_lookup_emission_unit_;
  TypeScopeFrame* saved_type_scope_ = nullptr;
  std::vector<std::unique_ptr<TypeScopeFrame>> frames_;
  bool changed_ = false;
};

}  // namespace tp2cc
