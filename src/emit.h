#pragma once

// Pascal-to-C++ emitter.
//
// Output conventions (see plan):
//   * Every Pascal unit becomes `namespace p_<unitname> { ... }` in both
//     the `.h` and the `.cc`.
//   * Every user-defined Pascal identifier is prefixed `p_` to avoid
//     collisions with C++ reserved words.
//   * Each `uses X` becomes `#include "X.h"`; cross-unit references are
//     emitted explicitly instead of relying on open namespaces.
//   * Interface decls land in the `.h`; implementation decls in the `.cc`.
//   * Primitive types are mapped with a fixed table (see emit.cc).
//
// This is a growing emitter -- each Pascal feature is added one at a time,
// driven by tests.

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ast.h"

namespace tp2cc {

struct TypeRegistry;

struct EmittedUnit {
  std::string header;  // contents of UNIT.h
  std::string impl;    // contents of UNIT.cc
};

EmittedUnit emit_unit(const ast::UnitNode& u,
                      const TypeRegistry* registry = nullptr,
                      // When emitting a `program`, the ordered list of
                      // unit names (excluding the program itself) whose
                      // unit lifecycle hooks should run before the
                      // program body.
                      const std::vector<std::string>* unit_init_order = nullptr);

}  // namespace tp2cc
