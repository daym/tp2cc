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

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "ast.h"

namespace tp2cc {

struct StringViewHash {
  using is_transparent = void;

  size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  size_t operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
  }

  size_t operator()(const char* value) const noexcept {
    return (*this)(std::string_view(value));
  }
};

struct StringViewEqual {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

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
  std::string defining_unit;
  std::string declaring_type;
  size_t param_count = 0;
  bool accepts_zero_args = false;
  bool is_function = false;       // returns a value
  bool is_virtual = false;
  bool is_final = false;
  bool is_overload = false;       // Pascal `overload` directive
  std::string return_type_name;    // Pascal-facing result type, if any
  std::shared_ptr<const ast::ProcDecl> decl;
};

struct FieldInfo {
  std::shared_ptr<const ast::TypeExpr> type;   // declared field type
  bool is_class_var = false;
  bool is_variant = false;
};

enum class PropertyAccessorKind : uint8_t {
  None,
  FieldPath,
  Method,
  Unsupported,
};

struct PropertyAccessorInfo {
  // Set after class member registration. A Pascal property accessor is a
  // field designator or a method name, and later emit code must use that
  // resolved kind instead of inferring it from the original token text.
  PropertyAccessorKind kind = PropertyAccessorKind::None;
  std::vector<std::string> path;
  std::string cxx_path;
  std::string method_name;

  bool empty() const;
  std::string display_name() const;
};

struct PropertyInfo {
  std::shared_ptr<const ast::TypeExpr> type;
  std::vector<ast::Param> params;
  PropertyAccessorInfo read;
  PropertyAccessorInfo write;
  bool is_default = false;
};

struct TypeSymbol;

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
  bool is_abstract = false;
  bool is_forward = false;
  std::vector<std::string> interfaces;  // lowercased direct implements list
  std::unordered_map<std::string, FieldInfo> fields;
  // Pascal allows overloaded methods (multiple `procedure foo(...)`
  // declarations on the same class), so the registry tracks the full set
  // per name. Single-method consumers may pick any overload via the
  // shim accessor on `TypeRegistry`; overload-aware call sites should
  // use `lookup_class_methods`.
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  std::unordered_map<std::string, PropertyInfo> properties;
  // Nested Pascal types are lexical children of the containing type. Keep
  // them on the owner symbol, not in a parallel flat table, so `Outer.Inner'
  // lookup and unqualified lookup inside Outer follow the same tree.
  std::unordered_map<std::string, std::shared_ptr<TypeSymbol>> nested_types;
  // Bare enum labels from class field types are visible while emitting member
  // bodies. Keep the labels here so class and inherited-class lookup can
  // resolve them before ordinary unit lookup.
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
  // Same lexical nested-type model as ClassInfo; modern Object Pascal records
  // can own types, and those names are resolved through the record type.
  std::unordered_map<std::string, std::shared_ptr<TypeSymbol>> nested_types;
};

struct EnumInfoReg {
  std::string name;
  std::string defining_unit;
  const ast::TyEnum* type = nullptr;
  std::string cxx_name;
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

enum class TypeSymbolKind : uint8_t {
  Alias,
  Class,
  Record,
  Interface,
  Enum,
};

using TypeSymbolPayload =
    std::variant<AliasInfo, ClassInfo, RecordInfo, InterfaceInfo, EnumInfoReg>;

struct TypeSymbol {
  std::string name;
  std::string defining_unit;
  // Containing type names, outermost first. Empty for unit-level types.
  // Emission uses this to lower Pascal `Outer.Inner' to C++ `Outer::Inner'
  // after lookup has proven that the dot is type nesting rather than a unit.
  std::vector<std::string> owner_path;
  // For aliases this is the alias target. For named concrete types this is the
  // declaration AST node. Synthetic runtime types keep ownership in
  // `owned_type` so every TypeSymbol exposes the same raw AST pointer API.
  std::shared_ptr<const ast::TypeExpr> owned_type;
  const ast::TypeExpr* type = nullptr;
  TypeSymbolPayload payload;

  TypeSymbol() = delete;
  TypeSymbol(std::string name_in, std::string defining_unit_in,
             const ast::TypeExpr* type_in, TypeSymbolPayload payload_in)
      : name(std::move(name_in)),
        defining_unit(std::move(defining_unit_in)),
        type(type_in),
        payload(std::move(payload_in)) {}
  TypeSymbol(std::string name_in, std::string defining_unit_in,
             std::shared_ptr<const ast::TypeExpr> owned_type_in,
             TypeSymbolPayload payload_in)
      : name(std::move(name_in)),
        defining_unit(std::move(defining_unit_in)),
        owned_type(std::move(owned_type_in)),
        type(owned_type.get()),
        payload(std::move(payload_in)) {}

  TypeSymbolKind kind() const {
    if (std::holds_alternative<AliasInfo>(payload)) {
      return TypeSymbolKind::Alias;
    }
    if (std::holds_alternative<ClassInfo>(payload)) {
      return TypeSymbolKind::Class;
    }
    if (std::holds_alternative<RecordInfo>(payload)) {
      return TypeSymbolKind::Record;
    }
    if (std::holds_alternative<InterfaceInfo>(payload)) {
      return TypeSymbolKind::Interface;
    }
    if (std::holds_alternative<EnumInfoReg>(payload)) {
      return TypeSymbolKind::Enum;
    }
    throw std::logic_error("unreachable TypeSymbol payload");
  }
  const AliasInfo* alias_info() const {
    return std::get_if<AliasInfo>(&payload);
  }
  AliasInfo* mutable_alias_info() {
    return std::get_if<AliasInfo>(&payload);
  }
  const ClassInfo* class_info() const {
    return std::get_if<ClassInfo>(&payload);
  }
  ClassInfo* mutable_class_info() {
    return std::get_if<ClassInfo>(&payload);
  }
  const InterfaceInfo* interface_info() const {
    return std::get_if<InterfaceInfo>(&payload);
  }
  InterfaceInfo* mutable_interface_info() {
    return std::get_if<InterfaceInfo>(&payload);
  }
  const RecordInfo* record_info() const {
    return std::get_if<RecordInfo>(&payload);
  }
  RecordInfo* mutable_record_info() {
    return std::get_if<RecordInfo>(&payload);
  }
  const EnumInfoReg* enum_info() const {
    return std::get_if<EnumInfoReg>(&payload);
  }
  EnumInfoReg* mutable_enum_info() {
    return std::get_if<EnumInfoReg>(&payload);
  }
};

using TypeSymbolScopeMap =
    std::unordered_map<std::string, TypeSymbol, StringViewHash,
                       StringViewEqual>;
using TypeSymbolRefScopeMap =
    std::unordered_map<std::string, const TypeSymbol*, StringViewHash,
                       StringViewEqual>;

// Lexical type lookup context for a TypeExpr as written in Pascal source.
// Entries point at registry-owned TypeSymbols; they are not copied into this
// tree. This lets a field/member/signature type keep the unit and enclosing
// type scope that declared it even when rendered or canonicalized elsewhere.
struct TypeLookupContext {
  std::string unit;
  const TypeLookupContext* parent = nullptr;
  TypeSymbolRefScopeMap type_symbols;
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
  std::unordered_map<std::string, TypeSymbol*> iface_types;
  std::unordered_set<std::string> iface_enum_members;
  std::unordered_map<std::string, VarInfo> impl_vars;
  std::unordered_map<std::string, ConstInfo> impl_consts;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_procs;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_operators;
  std::unordered_map<std::string, TypeSymbol*> impl_types;
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
  const std::vector<ProcInfo>* find_procs(const std::string& n) const {
    return find(iface_procs, impl_procs, n);
  }
  const std::vector<ProcInfo>* find_operators(const std::string& n) const {
    return find(iface_operators, impl_operators, n);
  }
  const TypeSymbol* find_type(const std::string& n) const {
    auto it = iface_types.find(n);
    if (it != iface_types.end()) return it->second;
    auto jt = impl_types.find(n);
    return jt == impl_types.end() ? nullptr : jt->second;
  }
  TypeSymbol* find_type_mut(const std::string& n) {
    auto it = iface_types.find(n);
    if (it != iface_types.end()) return it->second;
    auto jt = impl_types.find(n);
    return jt == impl_types.end() ? nullptr : jt->second;
  }
  bool has_type(const std::string& n) const {
    return find_type(n) != nullptr;
  }
  bool has_enum_member(const std::string& n) const {
    return iface_enum_members.count(n) || impl_enum_members.count(n);
  }
  bool has_proc(const std::string& n) const {
    if (auto* procs = find_procs(n)) return !procs->empty();
    return false;
  }
  bool has(const std::string& n) const {
    return find_var(n) || find_const(n) || has_proc(n) ||
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
  const std::vector<ProcInfo>* find_export_procs(const std::string& n) const {
    auto it = iface_procs.find(n);
    return it == iface_procs.end() ? nullptr : &it->second;
  }
  const std::vector<ProcInfo>* find_export_operators(
      const std::string& n) const {
    auto it = iface_operators.find(n);
    return it == iface_operators.end() ? nullptr : &it->second;
  }
  const TypeSymbol* find_export_type(const std::string& n) const {
    auto it = iface_types.find(n);
    return it == iface_types.end() ? nullptr : it->second;
  }
  bool has_export_type(const std::string& n) const {
    return find_export_type(n) != nullptr;
  }
  bool has_export_enum_member(const std::string& n) const {
    return iface_enum_members.count(n) > 0;
  }
};

struct TypeRegistry {
  std::unordered_map<std::string, UnitInfo> units;

  // Unit-level Pascal type declarations. UnitInfo maps point into this deque;
  // deque references stay stable as new symbols are registered.
  std::deque<TypeSymbol> type_symbols;
  // Nested Pascal type declarations have the same stable identity requirement
  // as unit-level declarations. Owner nested-type maps are lookup indexes into
  // this store, not the owner of child symbol lifetime.
  std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>
      nested_type_symbols;
  // rt-side reference classes (tobject, exception, ...). Method lookups
  // (`lookup_class_method[s]`) consult this when a translated class chain
  // reaches a runtime parent, so a source class that inherits Create from
  // Exception still resolves to the synthesized constructor signature.
  // Code-gen never touches this table.
  std::unordered_map<std::string, ClassInfo> rt_classes;
  // Anonymous/nested enum carriers have no TypeSymbol of their own, but raw
  // TyEnum* values still need owner metadata after type inference.
  std::deque<EnumInfoReg> anonymous_enum_infos;
  std::unordered_map<const ast::TyEnum*, const EnumInfoReg*> enum_type_info;
  // (unit, member) -> owning enum's AST node, populated from EnumInfoReg as
  // each enum is registered. `EmitAnalysis::deduce_type` asks "is this Ident
  // an enum member?" for nearly every identifier in the source; the answer
  // is `no` for almost all of them. A direct hash lookup answers the common
  // negative case in one probe instead of walking every enum's member list.
  std::unordered_map<std::string,
                     std::unordered_map<std::string, const ast::TyEnum*>>
      enum_members_by_unit;
  // TypeExpr nodes that require more than source-file unit lookup are indexed
  // to their lexical declaration context. The context tree models unit scope
  // plus enclosing class/object/record type scopes using TypeSymbol refs.
  std::deque<TypeLookupContext> type_lookup_context_storage;
  std::unordered_map<const ast::TypeExpr*, const TypeLookupContext*>
      type_lookup_contexts;
  std::unordered_map<std::string, const TypeLookupContext*>
      unit_interface_type_contexts;
  std::unordered_map<std::string, const TypeLookupContext*>
      unit_implementation_type_contexts;
  std::unordered_map<const ast::ProcDecl*, const TypeLookupContext*>
      proc_signature_type_contexts;
  std::unordered_map<const ast::ProcDecl*, const TypeLookupContext*>
      proc_body_type_contexts;
  // Type syntax can be rendered after its declaring unit has finished
  // emitting. `Location::file` identifies the parsed source buffer that
  // contributed that syntax, including include-file buffers, so this map keeps
  // bound-name lookup in the unit that owned the source text without storing
  // an entry for every TypeExpr node.
  std::unordered_map<const SourceFile*, std::string> source_file_units;

  // Canonical descriptors for built-in type literals (atoms). Pascal is
  // nominal: a type's identity is its declaration identity. Atoms like
  // `integer`, `char`, `ansistring` are one declaration per atom, so they
  // have one identity independent of where they appear in source. Each atom
  // is interned once at build() time as a stable TypeSymbol whose `type`
  // pointer is that declaration's canonical representative. `canonicalize`
  // promotes terminal TyName occurrences of these names to that canonical
  // pointer; the emitter then tests type identity as pointer equality on
  // `const ast::TypeExpr*`, which is exactly "same declaration".
  std::unordered_map<std::string, const TypeSymbol*, StringViewHash,
                     StringViewEqual>
      builtin_literal_descriptors;

  // Merged method overload sets across the class hierarchy, keyed by
  // (current_unit, class_name, member_lowered) packed with NUL separators.
  // Populated on demand by `lookup_class_methods` during emission; the
  // registry is fully built before emission starts so this cache is safe
  // without invalidation.  Mutable so the const lookup can populate it.
  mutable std::unordered_map<std::string, std::vector<MethodSig>>
      merged_method_cache;

  // Fill from all parsed UnitNodes.
  void build(const std::vector<const ast::UnitNode*>& units);
  const TypeLookupContext* lookup_context_for_type(
      const ast::TypeExpr* type) const;
  const TypeLookupContext* lookup_unit_context(
      std::string_view unit, bool implementation) const;
  const TypeLookupContext* lookup_proc_signature_context(
      const ast::ProcDecl* proc) const;
  const TypeLookupContext* lookup_proc_body_context(
      const ast::ProcDecl* proc) const;
  std::string_view declaration_unit_for_type(
      const ast::TypeExpr* type) const;
  const TypeSymbol* lookup_type_symbol_in_context(
      std::string_view name, const TypeLookupContext* context) const;
  const TypeSymbol* lookup_type_symbol_in_scope_chain(
      std::string_view name, const TypeLookupContext* context) const;
  const TypeSymbol* lookup_type_symbol_exact(std::string_view unit,
                                             std::string_view name) const;
  TypeSymbol* lookup_type_symbol_exact_mut(std::string_view unit,
                                           std::string_view name);
  const TypeSymbol* lookup_type_symbol(std::string_view name,
                                       std::string_view current_unit) const;
  const EnumInfoReg* enum_info_for_type(const ast::TyEnum* type) const;
  // Constant-time "is `member` an enum member of an enum declared in
  // `unit`?" Returns the owning TyEnum* or nullptr.
  const ast::TyEnum* lookup_enum_member_in_unit(
      std::string_view unit, std::string_view member) const;

  const ClassInfo* lookup_class(std::string_view name,
                                std::string_view current_unit) const;
  const ClassInfo* lookup_class_exact(std::string_view unit,
                                      std::string_view name) const;
  // Pascal class ancestry for semantic lookup/conversion. Root reference
  // classes implicitly inherit runtime TObject, matching emitted C++ bases.
  const ClassInfo* lookup_parent_class(const ClassInfo& class_info) const;
  bool class_implements_interface(std::string_view class_name,
                                  const InterfaceInfo& interface_info,
                                  std::string_view current_unit) const;
  const InterfaceInfo* interface_info_for_type(
      const ast::TypeExpr* type, std::string_view current_unit) const;
  const InterfaceInfo* lookup_interface(std::string_view name,
                                        std::string_view current_unit) const;
  const InterfaceInfo* lookup_interface_exact(std::string_view unit,
                                              std::string_view name) const;
  const RecordInfo* lookup_record(std::string_view name,
                                  std::string_view current_unit) const;
  const RecordInfo* lookup_record_exact(std::string_view unit,
                                        std::string_view name) const;
  bool has_class(std::string_view name,
                 std::string_view current_unit = {}) const {
    return lookup_class(name, current_unit) != nullptr;
  }

  // Chase TyName aliases through the scoped registry to the first non-TyName
  // type expression.
  const ast::TypeExpr* canonicalize(
      const ast::TypeExpr* te,
      std::string_view current_unit = {}) const;
  const ast::TypeExpr* canonicalize(
      const ast::TypeExpr* te,
      const TypeLookupContext* context) const;

  // Returns the canonical TypeSymbol for a built-in type literal (lowercased
  // Pascal name), or nullptr if `name` is not a builtin atom. The symbol's
  // `type` field is the canonical AST representative; pointer equality on it
  // is type identity.
  const TypeSymbol* builtin_literal(std::string_view name) const;

  // If `te` canonicalizes to a pointer to a class/record, return its
  // type-alias name (lowercased). Otherwise empty string.
  std::string pointer_target_type_name(
      const ast::TypeExpr* te,
      std::string_view current_unit = {}) const;

  // If `te` canonicalizes to a class/record, return its type-alias name.
  std::string direct_type_name(
      const ast::TypeExpr* te,
      std::string_view current_unit = {}) const;

  // Pascal resolves type names and value names in different contexts. C++
  // class/struct scopes do not, so fields use value identifiers (`p_*`) while
  // named Pascal types use type identifiers (`t_*`).
  std::string field_cxx_name(std::string_view name) const;

  const FieldInfo* lookup_class_field(
      const std::string& class_name, const std::string& member,
      std::string_view current_unit) const;

  // Class member bodies may resolve inherited field-type enum labels before
  // unit-level lookup; this cache follows the class inheritance chain.
  bool class_has_enum_member(
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
      const std::string& record_name, const std::string& member,
      std::string_view current_unit = {}) const;
};

// A Pascal enum type contributes both labels for name lookup and a C++ carrier
// type. The enum syntax can be nested inside set, array, record, and object
// types, so registry and emitter setup walk the whole TypeExpr instead of
// checking only the top-level type node.
std::vector<const ast::TyEnum*> collect_enum_types(const ast::TypeExpr& t);
void register_enum_types_for_owner(
    std::unordered_map<std::string, const ast::TyEnum*>& out,
    const ast::TypeExpr* type, std::string_view owner_name,
    const ast::TyEnum* named_top_level = nullptr);
TypeSymbol make_type_symbol_for_type(
    std::string_view unit, std::string_view name,
    std::shared_ptr<const ast::TypeExpr> type);
void register_type_symbols_for_owner(
    TypeSymbolScopeMap& out, std::shared_ptr<const ast::TypeExpr> type,
    std::string_view owner_name,
    const ast::TyEnum* named_top_level = nullptr);

}  // namespace tp2cc
