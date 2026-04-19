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

struct EmittedUnit {
  std::string header;  // contents of UNIT.h
  std::string impl;    // contents of UNIT.cc
};

// Collect parameterless method names from a single parsed unit's AST.
// Caller is expected to drive this over every parsed unit before emission
// so cross-unit `obj.method` auto-parenthesising works.
void collect_parameterless_methods(const ast::UnitNode& u,
                                   std::unordered_set<std::string>& out);

// Collect all record/object field names from a parsed unit's AST. Used
// to block auto-call on `obj.name` when `name` appears as a field in
// any class (cross-unit safety).
void collect_field_names(const ast::UnitNode& u,
                         std::unordered_set<std::string>& out);

// Collect all type aliases (enum-like names are most useful) from a
// parsed unit. Enables cross-unit `array[tenum] of T` dim-size
// computation even when `tenum` is declared in another unit.
struct EnumInfo { int member_count = 0; };
void collect_enum_sizes(const ast::UnitNode& u,
                        std::unordered_map<std::string, EnumInfo>& out);

EmittedUnit emit_unit(const ast::UnitNode& u,
                      const std::unordered_set<std::string>& extra_parameterless = {},
                      const std::unordered_set<std::string>& extra_fields = {},
                      const std::unordered_map<std::string, EnumInfo>& extra_enums = {});

}  // namespace p2cc
