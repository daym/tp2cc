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

namespace tp2cc {

// Upper bound on the length of a TyName -> TyName alias chain that
// `canonicalize' is willing to follow.  A well-formed Pascal program
// has short chains (typically 1--3 hops: alias -> concrete type);
// hitting this limit means either the registry has a cycle (bug) or the
// source declared a genuinely pathological set of aliases. Either way,
// silently returning an intermediate would hide the fault, so
// callers are expected to treat exceeding this limit as fatal.
constexpr int kMaxAliasChainHops = 32;

enum class SymKind : uint8_t {
  Unknown,
  Field,
  Method,           // procedure or function with/without params
  ClassMethod,
  Constructor,
  Destructor,
};

struct MethodSig {
  SymKind kind = SymKind::Method;
  size_t param_count = 0;
  bool accepts_zero_args = false;
  bool is_function = false;       // returns a value
  bool is_virtual = false;
  std::shared_ptr<const ast::ProcDecl> decl;
};

struct FieldInfo {
  std::shared_ptr<const ast::TypeExpr> type;   // declared field type
};

struct PropertyInfo {
  std::shared_ptr<const ast::TypeExpr> type;
  std::vector<ast::Param> params;
  std::string read_name;
  std::string write_name;
  bool is_default = false;
};

struct ClassInfo {
  std::string name;
  std::string parent;                    // empty if none
  std::string defining_unit;
  // TP-style `object' is a value type: lives on the stack by default,
  // heap-allocated with `new(p, init(...))', destroyed via
  // `dispose(p, done)'.  Delphi-style `class' is a reference type:
  // variables of class type always hold pointers, instances are
  // always heap-allocated, `TFoo.Create(...)' returns a pointer,
  // destruction via `.Free'.  Emit decisions fork on this flag.
  bool is_reference_type = false;
  std::unordered_map<std::string, FieldInfo> fields;
  std::unordered_map<std::string, MethodSig> methods;
  std::unordered_map<std::string, PropertyInfo> properties;
  std::string default_property_name;
};

struct RecordInfo {
  std::string name;
  std::string defining_unit;
  bool is_packed = false;
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
  std::shared_ptr<const ast::TypeExpr> target; // may itself be a TyName (chain)
};

struct ProcInfo {
  std::string defining_unit;
  std::shared_ptr<const ast::ProcDecl> decl;
  size_t param_count = 0;
  bool is_function = false;
  // For rt builtins that accept `foo;` with zero args (writeln,
  // readln, halt, etc.) regardless of declared arity.
  bool accepts_zero_args = false;
  // Return type name (lowercased Pascal type identifier) when known
  // -- used for `is_bool` / auto-call decisions on rt builtins
  // whose full AST we don't have.
  std::string return_type_name;
};

struct VarInfo {
  std::string defining_unit;
  std::shared_ptr<const ast::TypeExpr> type;
};

struct ConstInfo {
  std::string defining_unit;
  std::shared_ptr<const ast::TypeExpr> type;   // nullptr if untyped
  std::shared_ptr<const ast::Expr> value;      // constant expression AST
};

struct UnitInfo {
  std::string name;
  std::vector<std::string> uses;         // interface + impl (order)
  // Per-unit symbol tables -- split interface vs impl. Only
  // interface-exported symbols are visible to other units; both are
  // visible within this unit's own procs/method bodies.
  std::unordered_map<std::string, VarInfo> iface_vars;
  std::unordered_map<std::string, ConstInfo> iface_consts;
  std::unordered_map<std::string, ProcInfo> iface_procs;
  std::unordered_set<std::string> iface_types;
  std::unordered_set<std::string> iface_enum_members;
  std::unordered_map<std::string, VarInfo> impl_vars;
  std::unordered_map<std::string, ConstInfo> impl_consts;
  std::unordered_map<std::string, ProcInfo> impl_procs;
  std::unordered_set<std::string> impl_types;
  std::unordered_set<std::string> impl_enum_members;

  // Union views over iface + impl (used for own-unit lookup where
  // both sections are in scope).
  template <typename M>
  static const typename M::mapped_type* find(const M& a, const M& b,
                                              const std::string& n) {
    auto it = a.find(n);
    if (it != a.end()) return &it->second;
    auto jt = b.find(n);
    if (jt != b.end()) return &jt->second;
    return nullptr;
  }
  const VarInfo* find_var(const std::string& n) const {
    return find(iface_vars, impl_vars, n);
  }
  const ConstInfo* find_const(const std::string& n) const {
    return find(iface_consts, impl_consts, n);
  }
  const ProcInfo* find_proc(const std::string& n) const {
    return find(iface_procs, impl_procs, n);
  }
  bool has_type(const std::string& n) const {
    return iface_types.count(n) || impl_types.count(n);
  }
  bool has_enum_member(const std::string& n) const {
    return iface_enum_members.count(n) || impl_enum_members.count(n);
  }
  bool has(const std::string& n) const {
    return find_var(n) || find_const(n) || find_proc(n) ||
           has_type(n) || has_enum_member(n);
  }
  // Interface-exports view: what other units see when they `uses`
  // this unit.
  const VarInfo* find_export_var(const std::string& n) const {
    auto it = iface_vars.find(n);
    return it == iface_vars.end() ? nullptr : &it->second;
  }
  const ConstInfo* find_export_const(const std::string& n) const {
    auto it = iface_consts.find(n);
    return it == iface_consts.end() ? nullptr : &it->second;
  }
  const ProcInfo* find_export_proc(const std::string& n) const {
    auto it = iface_procs.find(n);
    return it == iface_procs.end() ? nullptr : &it->second;
  }
  bool has_export_type(const std::string& n) const {
    return iface_types.count(n) > 0;
  }
  bool has_export_enum_member(const std::string& n) const {
    return iface_enum_members.count(n) > 0;
  }
};

struct TypeRegistry {
  std::unordered_map<std::string, UnitInfo> units;

  // Indexed by unqualified lowercased type-alias name.
  std::unordered_map<std::string, ClassInfo> classes;
  std::unordered_map<std::string, RecordInfo> records;
  std::unordered_map<std::string, EnumInfoReg> enums;
  std::unordered_map<std::string, AliasInfo> aliases;   // includes pointer aliases

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

  const FieldInfo* lookup_class_field(
      const std::string& class_name, const std::string& member) const;

  const MethodSig* lookup_class_method(
      const std::string& class_name, const std::string& member) const;

  const PropertyInfo* lookup_class_property(
      const std::string& class_name, const std::string& member) const;

  const PropertyInfo* lookup_default_property(
      const std::string& class_name) const;

  const FieldInfo* lookup_record_field(
      const std::string& record_name, const std::string& member) const;
};

}  // namespace tp2cc
