#pragma once

// Cross-unit Pascal type/symbol registry.
//
// Driven as declarations are parsed so the emitter can consume already-bound
// type identity and descriptor-owned class/record/interface/enum metadata.
//
// General value/procedure binding is a later migration. The value/procedure
// tables here currently support the existing emitter resolver and declaration-
// time type binding where Pascal syntax is ambiguous.

#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "ast.h"
#include "emit_support.h"

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

inline bool pascal_key_is_canonical(std::string_view text) {
  for (char ch : text) {
    if (ch >= 'A' && ch <= 'Z') return false;
  }
  return true;
}

// Semantic tables are keyed by identifiers after Pascal case-folding. Keeping
// that key as its own type prevents registry lookups from silently repairing
// callers that skipped parser-side canonicalization.
class PascalKey {
 public:
  explicit PascalKey(std::string_view text_in) : text_(text_in) {
    assert(pascal_key_is_canonical(text_));
  }

  std::string_view view() const { return text_; }
  bool empty() const { return text_.empty(); }

 private:
  std::string_view text_;
};

inline PascalKey pascal_key(std::string_view text) {
  return PascalKey(text);
}

inline std::string pascal_key_string(PascalKey key) {
  return std::string(key.view());
}

enum class SymKind : uint8_t {
  Unknown,
  Field,
  Method,           // procedure or function with/without params
  ClassMethod,
  Constructor,
  Destructor,
};

struct TypeSymbol;
struct EnumInfoReg;

struct EnumMemberInfo {
  const EnumInfoReg* owner = nullptr;
  // Declaration position, not the Pascal ordinal. Explicit enum expressions
  // can assign a different ordinal; the constant evaluator computes that
  // value in the declaration's existing lexical scope.
  size_t member_index = 0;
};

struct MethodSig {
  SymKind kind = SymKind::Method;
  std::string defining_unit;
  std::string declaring_type;
  // Build binds class/interface method signatures to their owner symbol once;
  // emit-time method selection must not reconstruct that owner from text.
  const TypeSymbol* declaring_symbol = nullptr;
  size_t param_count = 0;
  bool accepts_zero_args = false;
  bool is_function = false;       // returns a value
  bool is_virtual = false;
  bool is_final = false;
  bool is_overload = false;       // Pascal `overload` directive
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

struct ClassInfo {
  const TypeDescriptor* descriptor = nullptr;
  std::string name;
  std::vector<std::string> owner_path;
  std::string parent;                    // empty if none
  const TypeSymbol* parent_symbol = nullptr;
  std::string defining_unit;
  // ClassInfo is copied into runtime helper views and passed around by
  // reference during emission. Keep the owning symbol with it so class and
  // metaclass emission use the semantically bound identity instead of reconstructing
  // a unit-qualified type name from text.
  const TypeSymbol* symbol = nullptr;
  // TP-style `object' is a value type: lives on the stack by default,
  // heap-allocated with `new(p, init(...))', destroyed via
  // `dispose(p, done)'.  Delphi-style `class' is a reference type:
  // variables of class type always hold pointers, instances are
  // always heap-allocated, `TFoo.Create(...)' returns a pointer,
  // destruction via `.Free'.  Emit decisions fork on this flag.
  bool is_reference_type = false;
  bool is_abstract = false;
  bool is_forward = false;
  // Resolved identities for `class(TBase, IFace, ...)` entries.
  // Semantic checks and emission use these symbols so ancestry and interface
  // checks follow the same semantic type binding as fields and parameters.
  std::vector<const TypeSymbol*> interface_symbols;
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
  // Bare enum labels from class field types are visible in member bodies.
  // Store the descriptor-owned enum and ordinal directly; a later consumer
  // must not recover either from the label spelling.
  std::unordered_map<std::string, EnumMemberInfo> enum_members;
  std::string default_property_name;
};

struct InterfaceInfo {
  const TypeDescriptor* descriptor = nullptr;
  std::string name;
  std::string defining_unit;
  // Interface compatibility is nominal. Keep the owning symbol here so a class
  // implementing OuterA.I does not satisfy a formal of same-leaf OuterB.I just
  // because both interfaces live in the same unit.
  const TypeSymbol* symbol = nullptr;
  std::string metadata_string;
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
};

struct RecordInfo {
  const TypeDescriptor* descriptor = nullptr;
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
  const TypeDescriptor* descriptor = nullptr;
  std::string name;
  std::string defining_unit;
  const ast::TyEnum* type = nullptr;
  std::string cxx_name;
  std::vector<std::string> members;      // lowercased
};

using TypeDescriptorPayload =
    std::variant<std::monostate, ClassInfo, RecordInfo, InterfaceInfo,
                 EnumInfoReg>;

struct TypeDescriptor {
  size_t id = 0;
  // The syntax that creates this Pascal type object. Alias syntax points to
  // another descriptor and therefore never becomes this field.
  const ast::TypeExpr* type = nullptr;
  // Optional declaration name for the object. Aliases are not owners.
  const TypeSymbol* symbol = nullptr;
  // `class of T` has target-keyed identity and no declaration owner.
  const TypeSymbol* metaclass_target = nullptr;
  // Result edges for intrinsic operations whose result is fixed by this type.
  // These point back into the same registry tree; emit-time consumers follow
  // the edge and never acquire the result through a Pascal spelling.
  const TypeDescriptor* ordinal_result = nullptr;
  const TypeDescriptor* element_result = nullptr;
  const TypeDescriptor* pointer_difference_result = nullptr;
  const TypeDescriptor* real_division_result = nullptr;
  const TypeDescriptor* string_concat_result = nullptr;
  const TypeDescriptor* lo_hi_result = nullptr;
  const TypeDescriptor* set_literal_element_result = nullptr;
  const TypeDescriptor* low_high_result = nullptr;
  // Backend properties of compiler-provided primitive types. Aliases share
  // this descriptor and therefore share this metadata without spelling tests.
  const PrimitiveInfo* primitive = nullptr;
  // Pascal's standard Code/Data record is the only record type that an
  // explicit cast may convert to a procedure-of-object carrier. Aliases share
  // this role through descriptor identity.
  bool is_method_carrier = false;
  TypeDescriptorPayload payload;

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

struct ProcInfo {
  enum class SlotStorage { Value, Mutable, UntypedConst, UntypedMutable };
  struct SlotInfo {
    SlotStorage storage = SlotStorage::Value;
  };

  std::string defining_unit;
  std::shared_ptr<const ast::ProcDecl> decl;
  size_t param_count = 0;
  bool is_function = false;
  bool is_overload = false;
  // For rt builtins that accept `foo;` with zero args (writeln,
  // readln, halt, etc.) regardless of declared arity.
  bool accepts_zero_args = false;
  // Runtime metadata procedures have no ProcDecl, so build stores their
  // resolved result symbol here instead of making emission rebuild it from
  // this diagnostic/source spelling.
  std::string return_type_name;
  const TypeSymbol* return_type_symbol = nullptr;
  // Metadata-only runtime helpers have no ProcDecl, so build stores their
  // caller-storage parameter contracts here. Parsed Pascal declarations use
  // Param::Var/Out/Const instead.
  std::vector<SlotInfo> slot_info;
};

struct VarInfo {
  std::string defining_unit;
  std::shared_ptr<const ast::TypeExpr> type;
  Location loc;
};

struct ConstInfo {
  std::string defining_unit;
  std::shared_ptr<const ast::TypeExpr> type;   // nullptr if untyped
  std::shared_ptr<const ast::Expr> value;      // constant expression AST
  Location loc;
};

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
  // Pascal semantic identity. `type Y = X` shares X's descriptor; `type X =
  // record end` owns the descriptor created by that record syntax. This is
  // populated by parser-driven semantic binding.
  //
  // `name` is only Pascal declaration/source spelling. It is not the type
  // object's identity and it is not inherently the emitted C++ spelling. The
  // emitter is allowed to apply the fixed backend naming rule for declared
  // Pascal types (`type X = ...` -> generated carrier `t_x`, with owner/unit
  // qualification as needed). Anonymous nominal types have no `type X =`
  // source name, so they need a descriptor-attached synthesized C++ name or an
  // inline C++ form when that is representable.
  const TypeDescriptor* descriptor = nullptr;
  // Kept after an explicit `T = class;` is completed so references between
  // the forward declaration and its later body remain visible.
  bool has_forward_declaration = false;
  TypeSymbol() = delete;
  TypeSymbol(std::string name_in, std::string defining_unit_in,
             const ast::TypeExpr* type_in)
      : name(std::move(name_in)),
        defining_unit(std::move(defining_unit_in)),
        type(type_in) {}
  TypeSymbol(std::string name_in, std::string defining_unit_in,
             std::shared_ptr<const ast::TypeExpr> owned_type_in)
      : name(std::move(name_in)),
        defining_unit(std::move(defining_unit_in)),
        owned_type(std::move(owned_type_in)),
        type(owned_type.get()) {}

  const ClassInfo* class_info() const {
    return descriptor ? descriptor->class_info() : nullptr;
  }
  ClassInfo* mutable_class_info() {
    return descriptor
               ? const_cast<TypeDescriptor*>(descriptor)->mutable_class_info()
               : nullptr;
  }
  const InterfaceInfo* interface_info() const {
    return descriptor ? descriptor->interface_info() : nullptr;
  }
  InterfaceInfo* mutable_interface_info() {
    return descriptor
               ? const_cast<TypeDescriptor*>(descriptor)
                     ->mutable_interface_info()
               : nullptr;
  }
  const RecordInfo* record_info() const {
    return descriptor ? descriptor->record_info() : nullptr;
  }
  RecordInfo* mutable_record_info() {
    return descriptor
               ? const_cast<TypeDescriptor*>(descriptor)->mutable_record_info()
               : nullptr;
  }
  const EnumInfoReg* enum_info() const {
    return descriptor ? descriptor->enum_info() : nullptr;
  }
  EnumInfoReg* mutable_enum_info() {
    return descriptor
               ? const_cast<TypeDescriptor*>(descriptor)->mutable_enum_info()
               : nullptr;
  }
};

using TypeSymbolScopeMap =
    std::unordered_map<std::string, TypeSymbol, StringViewHash,
                       StringViewEqual>;
using TypeSymbolRefScopeMap =
    std::unordered_map<std::string, const TypeSymbol*, StringViewHash,
                       StringViewEqual>;

struct UnitInfo;

enum class ScopeFrameKind : uint8_t {
  Local,
  UnitInterface,
  UnitImplementation,
  ImportedUnitInterface,
};

// Lexical type lookup context for a TypeExpr as written in Pascal source.
// Entries point at registry-owned TypeSymbols; they are not copied into this
// tree. This lets a field/member/signature type keep the unit and enclosing
// type scope that declared it even when rendered after parsing.
struct TypeLookupContext {
  std::string unit;
  const TypeLookupContext* parent = nullptr;
  ScopeFrameKind kind = ScopeFrameKind::Local;
  const UnitInfo* unit_info = nullptr;
  // Ordered semantic frames keep the owning UnitInfo for value/proc visibility and
  // unit-qualified lookup, but type lookup must use only declarations that
  // were visible at this source point. The full UnitInfo maps remain ownership
  // and export indexes, not a substitute for Pascal declaration order.
  bool restrict_unit_type_lookup = false;
  // Procedure-local declarations still use the emitter's existing local value
  // state during the transition. A declaration-owned type context inside that
  // procedure must not clear those local const/proc/var maps when evaluating
  // bounds or defaults later in emission.
  bool preserve_local_value_scope = false;
  TypeSymbolRefScopeMap type_symbols;
  // Enum labels are values, not type declarations. Point directly at the
  // descriptor payload so lookup is constant-time and anonymous enum syntax
  // does not require a synthetic TypeSymbol.
  std::unordered_map<std::string, EnumMemberInfo, StringViewHash,
                     StringViewEqual>
      enum_members;
  std::unordered_map<std::string, ConstInfo, StringViewHash, StringViewEqual>
      const_symbols;
};

inline const EnumMemberInfo* scope_frame_find_local_enum_member(
    const TypeLookupContext& frame, std::string_view n) {
  auto it = frame.enum_members.find(n);
  return it == frame.enum_members.end() ? nullptr : &it->second;
}

inline const EnumInfoReg* scope_frame_find_local_enum_info_for_member(
    const TypeLookupContext& frame, std::string_view n) {
  const EnumMemberInfo* member = scope_frame_find_local_enum_member(frame, n);
  return member ? member->owner : nullptr;
}

struct UnitInfo {
  std::string name;
  // Direct unit names from the corresponding source-section `uses` clauses.
  // Implementation lookup builds a frame chain from both vectors; imported
  // units contribute only their interface symbols.
  std::vector<std::string> interface_uses;
  std::vector<std::string> implementation_uses;
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
  std::unordered_map<std::string, EnumMemberInfo> iface_enum_members;
  std::unordered_map<std::string, VarInfo> impl_vars;
  std::unordered_map<std::string, ConstInfo> impl_consts;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_procs;
  std::unordered_map<std::string, std::vector<ProcInfo>> impl_operators;
  std::unordered_map<std::string, TypeSymbol*> impl_types;
  std::unordered_map<std::string, EnumMemberInfo> impl_enum_members;

  // Union views over iface + impl (used for own-unit lookup where
  // both sections are in scope).
  template <typename M>
  static const typename M::mapped_type* find(const M& a, const M& b,
                                              const std::string& n) {
    assert(pascal_key_is_canonical(n));
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
    assert(pascal_key_is_canonical(n));
    auto it = iface_types.find(n);
    if (it != iface_types.end()) return it->second;
    auto jt = impl_types.find(n);
    return jt == impl_types.end() ? nullptr : jt->second;
  }
  TypeSymbol* find_type_mut(const std::string& n) {
    assert(pascal_key_is_canonical(n));
    auto it = iface_types.find(n);
    if (it != iface_types.end()) return it->second;
    auto jt = impl_types.find(n);
    return jt == impl_types.end() ? nullptr : jt->second;
  }
  bool has_type(const std::string& n) const {
    return find_type(n) != nullptr;
  }
  bool has_enum_member(const std::string& n) const {
    assert(pascal_key_is_canonical(n));
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
    assert(pascal_key_is_canonical(n));
    auto it = iface_vars.find(n);
    return it == iface_vars.end() ? nullptr : &it->second;
  }
  const ConstInfo* find_export_const(const std::string& n) const {
    assert(pascal_key_is_canonical(n));
    auto it = iface_consts.find(n);
    return it == iface_consts.end() ? nullptr : &it->second;
  }
  const std::vector<ProcInfo>* find_export_procs(const std::string& n) const {
    assert(pascal_key_is_canonical(n));
    auto it = iface_procs.find(n);
    return it == iface_procs.end() ? nullptr : &it->second;
  }
  const std::vector<ProcInfo>* find_export_operators(
      const std::string& n) const {
    assert(pascal_key_is_canonical(n));
    auto it = iface_operators.find(n);
    return it == iface_operators.end() ? nullptr : &it->second;
  }
  const TypeSymbol* find_export_type(const std::string& n) const {
    assert(pascal_key_is_canonical(n));
    auto it = iface_types.find(n);
    return it == iface_types.end() ? nullptr : it->second;
  }
  bool has_export_type(const std::string& n) const {
    return find_export_type(n) != nullptr;
  }
  bool has_export_enum_member(const std::string& n) const {
    assert(pascal_key_is_canonical(n));
    return iface_enum_members.count(n) > 0;
  }
};

inline bool scope_frame_is_import(const TypeLookupContext& frame) {
  return frame.kind == ScopeFrameKind::ImportedUnitInterface;
}

inline bool scope_frame_is_runtime(const TypeLookupContext& frame) {
  return frame.unit == "__rt__";
}

inline const VarInfo* scope_frame_find_var(const TypeLookupContext& frame,
                                           const std::string& n) {
  if (!frame.unit_info) return nullptr;
  assert(pascal_key_is_canonical(n));
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_vars.find(n);
      return it == frame.unit_info->impl_vars.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_vars.find(n);
      return it == frame.unit_info->iface_vars.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

inline const ConstInfo* scope_frame_find_const(const TypeLookupContext& frame,
                                               const std::string& n) {
  assert(pascal_key_is_canonical(n));
  if (auto local = frame.const_symbols.find(n);
      local != frame.const_symbols.end()) {
    return &local->second;
  }
  if (frame.restrict_unit_type_lookup || !frame.unit_info) return nullptr;
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_consts.find(n);
      return it == frame.unit_info->impl_consts.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_consts.find(n);
      return it == frame.unit_info->iface_consts.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

inline const std::vector<ProcInfo>* scope_frame_find_procs(
    const TypeLookupContext& frame, const std::string& n) {
  if (!frame.unit_info) return nullptr;
  assert(pascal_key_is_canonical(n));
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_procs.find(n);
      return it == frame.unit_info->impl_procs.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_procs.find(n);
      return it == frame.unit_info->iface_procs.end() ? nullptr : &it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

inline const std::vector<ProcInfo>* scope_frame_find_operators(
    const TypeLookupContext& frame, const std::string& n) {
  if (!frame.unit_info) return nullptr;
  assert(pascal_key_is_canonical(n));
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_operators.find(n);
      return it == frame.unit_info->impl_operators.end() ? nullptr
                                                        : &it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_operators.find(n);
      return it == frame.unit_info->iface_operators.end() ? nullptr
                                                         : &it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

inline const EnumMemberInfo* scope_frame_find_enum_member(
    const TypeLookupContext& frame, const std::string& n) {
  assert(pascal_key_is_canonical(n));
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      if (!frame.unit_info) return nullptr;
      auto it = frame.unit_info->impl_enum_members.find(n);
      return it == frame.unit_info->impl_enum_members.end()
                 ? nullptr
                 : &it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      if (!frame.unit_info) return nullptr;
      auto it = frame.unit_info->iface_enum_members.find(n);
      return it == frame.unit_info->iface_enum_members.end()
                 ? nullptr
                 : &it->second;
    }
    case ScopeFrameKind::Local:
      return scope_frame_find_local_enum_member(frame, n);
  }
  return nullptr;
}

inline bool scope_frame_has_enum_member(const TypeLookupContext& frame,
                                        const std::string& n) {
  return scope_frame_find_enum_member(frame, n) != nullptr;
}

struct TypeRegistry {
  struct InferredSetKey {
    const TypeDescriptor* element = nullptr;
    bool has_explicit_bounds = false;
    int64_t low = 0;
    int64_t high = 0;
    bool operator==(const InferredSetKey& other) const {
      return element == other.element &&
             has_explicit_bounds == other.has_explicit_bounds &&
             low == other.low && high == other.high;
    }
  };

  struct InferredSetKeyHash {
    std::size_t operator()(const InferredSetKey& key) const {
      std::size_t h = std::hash<const TypeDescriptor*>{}(key.element);
      h ^= std::hash<bool>{}(key.has_explicit_bounds) + 0x9e3779b9 +
           (h << 6) + (h >> 2);
      h ^= std::hash<int64_t>{}(key.low) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<int64_t>{}(key.high) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct ArrayTailKey {
    const TypeDescriptor* source = nullptr;
    std::size_t first_dimension = 0;
    bool operator==(const ArrayTailKey& other) const {
      return source == other.source &&
             first_dimension == other.first_dimension;
    }
  };

  struct ArrayTailKeyHash {
    std::size_t operator()(const ArrayTailKey& key) const {
      std::size_t h = std::hash<const TypeDescriptor*>{}(key.source);
      h ^= std::hash<std::size_t>{}(key.first_dimension) + 0x9e3779b9 +
           (h << 6) + (h >> 2);
      return h;
    }
  };

  struct MethodCacheKey {
    const TypeDescriptor* owner = nullptr;
    std::string member;

    bool operator==(const MethodCacheKey& other) const {
      return owner == other.owner && member == other.member;
    }
  };

  struct MethodCacheKeyHash {
    std::size_t operator()(const MethodCacheKey& key) const {
      std::size_t h = std::hash<const TypeDescriptor*>{}(key.owner);
      h ^= std::hash<std::string>{}(key.member) + 0x9e3779b9 +
           (h << 6) + (h >> 2);
      return h;
    }
  };

  struct ParseUnitTypeState {
    const TypeLookupContext* interface_current = nullptr;
    const TypeLookupContext* implementation_current = nullptr;
    std::unordered_map<std::string, const ast::TypeDecl*> interface_types;
    std::unordered_map<std::string, const ast::TypeDecl*> implementation_types;
  };

  std::unordered_map<std::string, UnitInfo> units;

  // Unit-level Pascal type declarations. UnitInfo maps point into this deque;
  // deque references stay stable as new symbols are registered.
  std::deque<TypeSymbol> type_symbols;
  // Nested Pascal type declarations have the same stable identity requirement
  // as unit-level declarations. Owner nested-type maps are lookup indexes into
  // this store, not the owner of child symbol lifetime.
  std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>
      nested_type_symbols;
  // Measured negative-query index for enum labels. Values point into the
  // descriptor payload and include the ordinal, so this is not a second enum
  // identity or metadata authority.
  std::unordered_map<std::string,
                     std::unordered_map<std::string, EnumMemberInfo>>
      enum_members_by_unit;
  // TypeExpr nodes that require more than source-file unit lookup are indexed
  // to their lexical declaration context. The context tree models unit scope
  // plus enclosing class/object/record type scopes using TypeSymbol refs.
  std::deque<TypeLookupContext> type_lookup_context_storage;
  // Stable storage for semantic type identities. TypeExpr and TypeSymbol nodes
  // point directly into this deque.
  mutable std::deque<TypeDescriptor> type_descriptor_storage;
  // Per-registry primitive metadata. Static declaration data is copied here so
  // each entry can point directly back to this registry's descriptor.
  std::deque<PrimitiveInfo> primitive_info_storage;
  mutable std::unordered_map<InferredSetKey, std::shared_ptr<ast::TySet>,
                             InferredSetKeyHash>
      inferred_set_types;
  mutable std::unordered_map<const TypeDescriptor*,
                             std::shared_ptr<ast::TyPointer>>
      inferred_pointer_types;
  mutable std::unordered_map<ArrayTailKey, std::shared_ptr<ast::TyArray>,
                             ArrayTailKeyHash>
      array_tail_types;
  std::unordered_map<const TypeSymbol*, const TypeDescriptor*>
      metaclass_descriptors;
  std::unordered_map<std::string, const TypeLookupContext*>
      unit_interface_type_contexts;
  std::unordered_map<std::string, const TypeLookupContext*>
      unit_implementation_type_contexts;
  // Merged method overload sets across the class hierarchy, keyed by resolved
  // class identity plus member name. The identity key matters for nested or
  // same-named classes: once build has resolved the receiver, method lookup
  // must not recover it again from a unit/name spelling.
  // Populated on demand during emission; the registry is fully built before
  // emission starts so this cache is safe without invalidation. Mutable so the
  // const lookup can populate it.
  mutable std::unordered_map<MethodCacheKey, std::vector<MethodSig>,
                             MethodCacheKeyHash>
      merged_method_cache;
  bool runtime_initialized = false;
  std::unordered_map<std::string, ParseUnitTypeState> parse_unit_type_states;

  void initialize_runtime_types();
  void begin_parsed_unit(std::string_view name);
  void set_parsed_unit_imports(std::string_view name,
                               const std::vector<std::string>& imports,
                               bool in_interface);
  void bind_parsed_declarations(
      std::string_view name, const std::vector<ast::DeclPtr>& declarations,
      bool in_interface);
  void bind_parsed_unit_bodies(const ast::UnitNode& unit);
  const TypeLookupContext* lookup_context_for_type(
      const ast::TypeExpr* type) const;
  const TypeLookupContext* lookup_unit_context(
      PascalKey unit, bool implementation) const;
  const TypeLookupContext* lookup_proc_signature_context(
      const ast::ProcDecl* proc) const;
  const TypeLookupContext* lookup_proc_body_context(
      const ast::ProcDecl* proc) const;
  const TypeSymbol* method_owner_symbol_for_proc(
      const ast::ProcDecl* proc) const;
  std::optional<const TypeSymbol*> exception_handler_type_result(
      const ast::ExceptHandler* handler) const;
  std::optional<const TypeSymbol*> type_name_expression_result(
      const ast::Expr* expr) const;
  std::optional<const TypeSymbol*> value_type_expression_result(
      const ast::Expr* expr) const;
  const TypeDescriptor* expression_result_descriptor(
      const ast::Expr* expr) const;
  const TypeDescriptor* descriptor_for_type(
      const ast::TypeExpr* type) const;
  bool bound_signature_type_exprs_match(const ast::TypeExpr* a,
                                        const ast::TypeExpr* b) const;
  bool bound_signature_params_match(const std::vector<ast::Param>& a,
                                    const std::vector<ast::Param>& b) const;
  const TypeSymbol* referenced_symbol_for_type(
      const ast::TypeExpr* type) const;
  const TypeSymbol* resolved_symbol_for_type(
      const ast::TypeExpr* type) const;
  const TypeSymbol* metaclass_target_for_type(
      const ast::TypeExpr* type) const;
  const TypeSymbol* lookup_type_symbol_in_context(
      PascalKey name, const TypeLookupContext* context) const;
  const TypeSymbol* lookup_type_symbol_exact(PascalKey unit,
                                             PascalKey name) const;
  TypeSymbol* lookup_type_symbol_exact_mut(PascalKey unit,
                                           PascalKey name);
  const EnumInfoReg* enum_info_for_type(const ast::TyEnum* type) const;
  const EnumMemberInfo* lookup_enum_member_in_unit(
      std::string_view unit, std::string_view member) const;

  // Pascal class ancestry for semantic lookup/conversion. Root reference
  // classes implicitly inherit runtime TObject, matching emitted C++ bases.
  const ClassInfo* lookup_parent_class(const ClassInfo& class_info) const;
  bool same_class_identity(const ClassInfo& a, const ClassInfo& b) const;
  bool class_implements_interface(const ClassInfo& class_info,
                                  const InterfaceInfo& interface_info) const;
  // Returns the canonical TypeSymbol for a built-in type literal (lowercased
  // Pascal name), or nullptr if `name` is not a builtin atom. The symbol's
  // `type` field is the canonical AST representative; pointer equality on it
  // is type identity.
  const TypeSymbol* builtin_literal(std::string_view name) const;
  const ast::TySet* inferred_set_type(
      const ast::TypeExpr* element,
      std::optional<std::pair<int64_t, int64_t>> explicit_bounds) const;
  const ast::TyPointer* inferred_pointer_type(
      const ast::TypeExpr* target) const;
  const ast::TyArray* array_tail_type(
      const ast::TyArray* source, std::size_t first_dimension) const;

  // Pascal resolves type names and value names in different contexts. C++
  // class/struct scopes do not, so fields use value identifiers (`p_*`) while
  // named Pascal types use type identifiers (`t_*`).
  std::string field_cxx_name(std::string_view name) const;

  const FieldInfo* lookup_class_field(const ClassInfo& class_info,
                                      const std::string& member) const;

  // Class member bodies may resolve inherited field-type enum labels before
  // unit-level lookup; this cache follows the class inheritance chain.
  bool class_has_enum_member(const ClassInfo& class_info,
                             const std::string& member) const;
  const EnumMemberInfo* lookup_class_enum_member(
      const ClassInfo& class_info, const std::string& member) const;

  // Full overload set, walking up the inheritance chain.
  const std::vector<MethodSig>* lookup_class_methods(
      const ClassInfo& class_info, const std::string& member) const;
  const std::vector<MethodSig>* lookup_interface_methods(
      const InterfaceInfo& interface_info, const std::string& member) const;

  const PropertyInfo* lookup_class_property(const ClassInfo& class_info,
                                            const std::string& member) const;

  const PropertyInfo* lookup_default_property(
      const ClassInfo& class_info) const;
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
    TypeRegistry& registry, std::string_view unit, std::string_view name,
    std::shared_ptr<const ast::TypeExpr> type);

}  // namespace tp2cc
