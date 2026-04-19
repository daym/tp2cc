#pragma once

// Pascal-to-C++ emitter.
//
// Output conventions (see plan):
//   * Every Pascal unit becomes `namespace p_<unitname> { ... }` in both
//     the `.h` and the `.cc`.
//   * Every user-defined Pascal identifier is prefixed `p_` to avoid
//     collisions with C++ reserved words.
//   * Each `uses X` becomes `#include "X.h"` + `using namespace p_X;` in
//     the implementation file.
//   * Interface decls land in the `.h`; implementation decls in the `.cc`.
//   * Primitive types are mapped with a fixed table (see emit.cc).
//
// This is a growing emitter -- each Pascal feature is added one at a time,
// driven by tests.

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ast.h"

namespace p2cc {

struct TypeRegistry;

struct EmittedUnit {
  std::string header;  // contents of UNIT.h
  std::string impl;    // contents of UNIT.cc
};

EmittedUnit emit_unit(const ast::UnitNode& u,
                      const TypeRegistry* registry = nullptr);

}  // namespace p2cc
