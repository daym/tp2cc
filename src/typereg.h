#pragma once

// Cross-unit Pascal type/symbol registry.
//
// Built from all parsed units up-front so the emitter can answer
// questions like "does class C have a field named X, or a method?",
// "what's the class of this variable?", "what does alias A resolve
// to?" -- without heuristics.
//
// Only the minimum information the emitter needs is captured: classes
// with their member kinds + parent, records with field lists, enums
// with their members, type aliases, and unit-level procs/vars with
// their signatures/types. Expression resolution lives on top of this.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"

namespace p2cc {

enum class SymKind : uint8_t {
  Unknown,
  Field,
  Method,           // procedure or function with/without params
  ClassMethod,      // (future) class method
  Constructor,
  Destructor,
};

struct MethodSig {
  SymKind kind = SymKind::Method;
  size_t param_count = 0;
  bool is_function = false;       // returns a value
  bool is_virtual = false;
  const ast::ProcDecl* decl = nullptr;
};

struct FieldInfo {
  const ast::TypeExpr* type = nullptr;   // declared field type
};

struct ClassInfo {
  std::string name;
  std::string parent;                    // empty if none
  std::string defining_unit;
  // member_name -> (is_method, field_info | method_sig)
  struct Member {
    bool is_method = false;
    FieldInfo field;
    MethodSig method;
  };
  std::unordered_map<std::string, Member> members;
};

struct RecordInfo {
  std::string name;
  std::string defining_unit;
  // Flat list of all field names including variant-case fields.
  std::unordered_map<std::string, FieldInfo> fields;
};

struct EnumInfoReg {
  std::string name;
  std::string defining_unit;
  std::vector<std::string> members;      // lowercased
};

struct AliasInfo {
  std::string defining_unit;
  const ast::TypeExpr* target = nullptr; // may itself be a TyName (chain)
};

struct ProcInfo {
  std::string defining_unit;
  const ast::ProcDecl* decl = nullptr;
  size_t param_count = 0;
  bool is_function = false;
};

struct VarInfo {
  std::string defining_unit;
  const ast::TypeExpr* type = nullptr;
};

struct ConstInfo {
  std::string defining_unit;
  const ast::TypeExpr* type = nullptr;   // nullptr if untyped
};

struct UnitInfo {
  std::string name;
  std::vector<std::string> uses;         // interface + impl (order)
  // Names declared in THIS unit (interface + impl) -- used to
  // shadow same-named symbols from `uses` without extra lookup
  // cost.
  std::unordered_set<std::string> own_consts;
  std::unordered_set<std::string> own_vars;
  std::unordered_set<std::string> own_procs;
  std::unordered_set<std::string> own_types;
};

struct TypeRegistry {
  std::unordered_map<std::string, UnitInfo> units;

  // Indexed by unqualified lowercased type-alias name.
  std::unordered_map<std::string, ClassInfo> classes;
  std::unordered_map<std::string, RecordInfo> records;
  std::unordered_map<std::string, EnumInfoReg> enums;
  std::unordered_map<std::string, AliasInfo> aliases;   // includes pointer aliases

  // Unqualified lowercased name -> info. Ambiguity across units is rare in
  // the fpc compiler sources; last-wins is acceptable for now, and the
  // translator's job isn't "bit-perfect name resolution" but "emit code
  // that compiles".
  std::unordered_map<std::string, ProcInfo> procs;
  std::unordered_map<std::string, VarInfo> vars;
  std::unordered_map<std::string, ConstInfo> consts;
  // Enum member -> defining unit. Enum members are emitted at unit-
  // namespace scope (Pascal's unscoped enums leak members into the
  // enclosing namespace); using-namespaces can cause ambiguity when
  // two units' enums share a member name.
  std::unordered_map<std::string, std::string> enum_members;

  // Fill from all parsed UnitNodes.
  void build(const std::vector<const ast::UnitNode*>& units);

  // Chase TyName aliases through the registry to the first non-TyName
  // type expression. `unit_ctx` is unused for now (aliases are global).
  const ast::TypeExpr* canonicalize(const ast::TypeExpr* te) const;

  // If `te` canonicalizes to a pointer to a class/record, return its
  // type-alias name (lowercased). Otherwise empty string.
  std::string pointer_target_type_name(const ast::TypeExpr* te) const;

  // If `te` canonicalizes to a class/record, return its type-alias name.
  std::string direct_type_name(const ast::TypeExpr* te) const;

  // Look up a member in `class_name` (or any ancestor). Returns nullptr
  // if not found. Ancestor chain stops on the first hit.
  const ClassInfo::Member* lookup_class_member(
      const std::string& class_name, const std::string& member) const;

  const FieldInfo* lookup_record_field(
      const std::string& record_name, const std::string& member) const;
};

}  // namespace p2cc
