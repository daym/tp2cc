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
#include <string_view>
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
  bool is_final = false;
  std::shared_ptr<const ast::ProcDecl> decl;
};

struct FieldInfo {
  std::shared_ptr<const ast::TypeExpr> type;   // declared field type
  bool is_class_var = false;
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
  bool is_forward = false;
  std::unordered_map<std::string, FieldInfo> fields;
  // Pascal allows overloaded methods (multiple `procedure foo(...)`
  // declarations on the same class), so the registry tracks the full set
  // per name. Single-method consumers may pick any overload via the
  // shim accessor on `TypeRegistry`; overload-aware call sites should
  // use `lookup_class_methods`.
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  std::unordered_map<std::string, PropertyInfo> properties;
  // Names of enum constants contributed by inline anonymous enum types
  // used as class field types (e.g. `libctype : (libc5, glibc2, ...);`).
  // Pascal exposes those constants in the enclosing class scope; member
  // bodies that reference them by bare identifier resolve through this
  // set so the emitter does not fall through to an `::rt::p_<name>`
  // unknown-fallback.
  std::unordered_set<std::string> enum_members;
  std::string default_property_name;
};

struct InterfaceInfo {
  std::string name;
  std::string defining_unit;
  std::string metadata_string;
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
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
  // Pascal allows multiple `function foo(...)` declarations under the same
  // name (`overload`); the registry keeps the full set so the emitter can
  // do Pascal-style overload resolution at call sites.
  std::unordered_map<std::string, std::vector<ProcInfo>> iface_procs;
  std::unordered_map<std::string, std::vector<ProcInfo>> iface_operators;
  std::unordered_set<std::string> iface_types;
  std::unordered_set<std::string> iface_enum_members;
  std::unordered_map<std::string, VarInfo> impl_vars;
  std::unordered_map<std::string, ConstInfo> impl_consts;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_procs;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_operators;
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
  // Returns the first-registered overload. WRONG ANSWER for overloaded
  // names -- `accepts_zero_args`, `param_count`, and `return_type` are all
  // per-overload, and the right value depends on which overload the call
  // resolves to. This stays only as a transitional shim for callers that
  // haven't been switched to the overload-aware path yet; it works in
  // practice today because every overload set in the bootstrap source
  // (`upper`, `lower`, `tostr`, `maybequoted`) happens to share arity and
  // return type across its overloads. Use `find_procs` and pick by
  // arg types whenever a real call site is involved.
  const ProcInfo* find_proc(const std::string& n) const {
    if (auto* v = find(iface_procs, impl_procs, n); v && !v->empty()) {
      return &(*v)[0];
    }
    return nullptr;
  }
  const std::vector<ProcInfo>* find_procs(const std::string& n) const {
    return find(iface_procs, impl_procs, n);
  }
  const std::vector<ProcInfo>* find_operators(const std::string& n) const {
    return find(iface_operators, impl_operators, n);
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
  // Same caveat as `find_proc`: returns one arbitrary overload. Overloaded
  // call sites must use `find_export_procs` and pick by arg types.
  const ProcInfo* find_export_proc(const std::string& n) const {
    auto it = iface_procs.find(n);
    if (it == iface_procs.end() || it->second.empty()) return nullptr;
    return &it->second[0];
  }
  const std::vector<ProcInfo>* find_export_procs(const std::string& n) const {
    auto it = iface_procs.find(n);
    return it == iface_procs.end() ? nullptr : &it->second;
  }
  const std::vector<ProcInfo>* find_export_operators(
      const std::string& n) const {
    auto it = iface_operators.find(n);
    return it == iface_operators.end() ? nullptr : &it->second;
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

  // User-source classes only, grouped by Pascal type name. Pascal units are
  // real namespaces, so two units may both export `TFoo`; storing all entries
  // under the existing name key prevents one unit from overwriting another.
  // Code that needs one class must use lookup_class* so unit visibility picks
  // the right entry.
  std::unordered_map<std::string, std::vector<ClassInfo>> classes;
  // rt-side reference classes (tobject, exception, ...). Method lookups
  // (`lookup_class_method[s]`) consult this when a translated class chain
  // reaches a runtime parent, so a source class that inherits Create from
  // Exception still resolves to the synthesized constructor signature.
  // Code-gen never touches this table.
  std::unordered_map<std::string, ClassInfo> rt_classes;
  std::unordered_map<std::string, InterfaceInfo> interfaces;
  std::unordered_map<std::string, RecordInfo> records;
  std::unordered_map<std::string, EnumInfoReg> enums;
  std::unordered_map<std::string, AliasInfo> aliases;   // includes pointer aliases

  // Fill from all parsed UnitNodes.
  void build(const std::vector<const ast::UnitNode*>& units);

  const ClassInfo* lookup_class(std::string_view name,
                                std::string_view current_unit) const;
  const ClassInfo* lookup_class_exact(std::string_view unit,
                                      std::string_view name) const;
  bool has_class(std::string_view name,
                 std::string_view current_unit = {}) const {
    return lookup_class(name, current_unit) != nullptr;
  }

  // Chase TyName aliases through the registry to the first non-TyName
  // type expression. `unit_ctx` is unused for now (aliases are global).
  const ast::TypeExpr* canonicalize(const ast::TypeExpr* te) const;

  // If `te` canonicalizes to a pointer to a class/record, return its
  // type-alias name (lowercased). Otherwise empty string.
  std::string pointer_target_type_name(const ast::TypeExpr* te) const;

  // If `te` canonicalizes to a class/record, return its type-alias name.
  std::string direct_type_name(const ast::TypeExpr* te) const;

  // Pascal resolves type names and value names in different contexts. C++
  // class/struct scopes do not, so all field declarations and references go
  // through value spelling, while named Pascal types use type spelling.
  std::string field_cxx_name(std::string_view name) const;

  const FieldInfo* lookup_class_field(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;

  // True if `member` is a member of an inline anonymous enum used as
  // a field type on `class_name` (or any of its ancestors).
  bool class_has_enum_member(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;

  // Single-method shim: returns the first overload (the one declared
  // earliest in source order). Wrong answer for overloaded names whose
  // overloads differ in arity / return type / `accepts_zero_args` --
  // those queries depend on which overload the call actually resolves to.
  // Use `lookup_class_methods` and pick by argument types instead.
  const MethodSig* lookup_class_method(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;
  // Full overload set, walking up the inheritance chain.
  const std::vector<MethodSig>* lookup_class_methods(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;

  const PropertyInfo* lookup_class_property(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;

  const PropertyInfo* lookup_default_property(
      const std::string& class_name, std::string_view current_unit) const;

  const FieldInfo* lookup_record_field(
      const std::string& record_name, const std::string& member) const;
};

}  // namespace tp2cc
