#pragma once

#include <optional>
#include <string>

#include "emit_context.h"
#include "emit_resolution_types.h"

namespace tp2cc {

class EmitAnalysis;
class EmitProperties;
struct TypeRegistry;

// Pascal identifier lookup. This module owns the full unqualified and
// qualified name-resolution order used while emitting expressions and
// statements:
//
//   - unqualified: `with` -> locals -> nested procs -> current class chain ->
//                  current unit -> uses chain -> implicit runtime unit
//   - `Unit.name`: symbols exported by a visible Pascal unit
//   - `Class.name` / `obj.name`: class members walking ancestors
//
// Keeping that lookup tree here prevents emit paths from duplicating
// "is this a method? a unit export? a with-bound field?" checks.
class EmitLookup {
 public:
  EmitLookup(const TypeRegistry& registry, ScopeStateView& scope,
             EmitAnalysis& analysis, EmitProperties& properties);

  ResolveResult resolve_name(const std::string& name,
                             QualifierKind qk = QualifierKind::None,
                             const std::string& qualifier = {});

 private:
  std::optional<ResolveResult> resolve_exported_unit_name(
      const std::string& unit_name, const std::string& name);

  const TypeRegistry& registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitProperties& properties_;
};

}  // namespace tp2cc
