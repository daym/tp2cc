#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ast.h"
#include "emit_context.h"
#include "emit_resolution_types.h"

namespace tp2cc {

struct ClassInfo;
struct ConstInfo;
class EmitAnalysis;
class EmitTypes;
struct TypeRegistry;
struct VarInfo;

class EmitStorageExprOps {
 public:
  virtual ~EmitStorageExprOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  // Emit a value subexpression from inside storage/address composition.
  // Contexts such as member-receiver emission must not leak into array indexes.
  virtual std::string expr_value_to_cxx(const ast::Expr& e) = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

struct EmitBytewiseStorage {
  // Byte-addressable storage view used before the caller decides whether the
  // view is merely offset-based or also unaligned.
  std::string void_ptr_text;
  std::string elem_cxx;
};

struct EmitUntypedStorageIndexView {
  std::string elem_cxx;
  std::string ptr_cxx;
  EmitUntypedStorageIndexView(std::string elem_cxx_in, std::string ptr_cxx_in)
      : elem_cxx(std::move(elem_cxx_in)), ptr_cxx(std::move(ptr_cxx_in)) {}
};

struct EmitPackedAggregateFieldUse {
  std::string record_name;
  std::string field_name;
};

struct EmitPackedScalarValueLoad {
  std::string text;
};

struct EmitAbsoluteTargetInfo {
  std::string cxx;
  const ast::TypeExpr* type = nullptr;
  bool value_is_storage_address = false;
  bool is_const_storage = false;
  static EmitAbsoluteTargetInfo untyped_param(std::string cxx_in) {
    return EmitAbsoluteTargetInfo{std::move(cxx_in), nullptr, true, false};
  }
};

struct EmitTypecastStorageView {
  const ast::Expr* source = nullptr;
  std::string source_cxx;
  std::string source_ptr_cxx;
  std::string target_cxx;
  const ast::TypeExpr* target_type = nullptr;
  bool target_is_primitive = false;
  bool source_is_untyped_storage = false;
  bool source_is_bytewise_storage = false;
  bool source_is_unaligned_bytewise_storage = false;
  bool pointee_view = false;
  std::string backing_ptr_cxx;
  std::string backing_type_cxx;
  EmitTypecastStorageView(const ast::Expr* source_in,
                          std::string source_cxx_in,
                          std::string source_ptr_cxx_in,
                          std::string target_cxx_in,
                          const ast::TypeExpr* target_type_in,
                          bool target_is_primitive_in,
                          bool source_is_untyped_storage_in,
                          bool source_is_bytewise_storage_in,
                          bool source_is_unaligned_bytewise_storage_in,
                          bool pointee_view_in,
                          std::string backing_ptr_cxx_in,
                          std::string backing_type_cxx_in)
      : source(source_in),
        source_cxx(std::move(source_cxx_in)),
        source_ptr_cxx(std::move(source_ptr_cxx_in)),
        target_cxx(std::move(target_cxx_in)),
        target_type(target_type_in),
        target_is_primitive(target_is_primitive_in),
        source_is_untyped_storage(source_is_untyped_storage_in),
        source_is_bytewise_storage(source_is_bytewise_storage_in),
        source_is_unaligned_bytewise_storage(
            source_is_unaligned_bytewise_storage_in),
        pointee_view(pointee_view_in),
        backing_ptr_cxx(std::move(backing_ptr_cxx_in)),
        backing_type_cxx(std::move(backing_type_cxx_in)) {}
};

enum class EmitStorageAccess {
  Ordinary,
  // Aligned storage reached through byte offsets. Variant-record payloads use
  // this to avoid selecting a C++ union member for reads/writes.
  Bytewise,
  // Storage that may violate the C++ alignment requirement for its Pascal
  // type, such as packed fields or explicit `unaligned(...)`.
  UnalignedBytewise,
};

enum class EmitStorageAddressForm {
  None,
  TypedStoragePointer,
  RawBytePointer,
};

// C++ lowering of one Pascal variable designator. Pascal has one storage
// concept for assignment targets, `Inc`/`Dec`, `@`, and var/out/untyped-var
// actuals, but that storage cannot always be represented as a C++ reference:
// untyped-var storage is already a raw caller-storage pointer, and packed
// fields may be misaligned and require byte-copy access instead of typed
// references. Keep the semantic decision in one object so callers ask for
// "store", "address", or "increment" and get the correct representation.
struct EmitStorageDesignator {
  static EmitStorageDesignator ordinary(std::string text_in,
                                        std::string type_cxx_in) {
    return EmitStorageDesignator(EmitStorageAccess::Ordinary,
                                 std::move(text_in), {},
                                 std::move(type_cxx_in), {}, {},
                                 EmitStorageAddressForm::None);
  }

  static EmitStorageDesignator ordinary_typed_address(
      std::string text_in, std::string ptr_cxx_in, std::string type_cxx_in) {
    return EmitStorageDesignator(EmitStorageAccess::Ordinary,
                                 std::move(text_in), std::move(ptr_cxx_in),
                                 std::move(type_cxx_in), {}, {},
                                 EmitStorageAddressForm::TypedStoragePointer);
  }

  static EmitStorageDesignator bytewise(std::string ptr_cxx_in,
                                        std::string type_cxx_in,
                                        std::string backing_ptr_cxx_in = {},
                                        std::string backing_type_cxx_in = {}) {
    return raw_byte_address(EmitStorageAccess::Bytewise, {},
                            std::move(ptr_cxx_in), std::move(type_cxx_in),
                            std::move(backing_ptr_cxx_in),
                            std::move(backing_type_cxx_in));
  }

  static EmitStorageDesignator unaligned_bytewise(std::string ptr_cxx_in,
                                                  std::string type_cxx_in,
                                                  std::string backing_ptr_cxx_in = {},
                                                  std::string backing_type_cxx_in = {}) {
    return raw_byte_address(EmitStorageAccess::UnalignedBytewise, {},
                            std::move(ptr_cxx_in), std::move(type_cxx_in),
                            std::move(backing_ptr_cxx_in),
                            std::move(backing_type_cxx_in));
  }

  static EmitStorageDesignator raw_byte_address(EmitStorageAccess access_in,
                                                std::string text_in,
                                                std::string ptr_cxx_in,
                                                std::string type_cxx_in,
                                                std::string backing_ptr_cxx_in = {},
                                                std::string backing_type_cxx_in = {}) {
    return EmitStorageDesignator(access_in, std::move(text_in),
                                 std::move(ptr_cxx_in),
                                 std::move(type_cxx_in),
                                 std::move(backing_ptr_cxx_in),
                                 std::move(backing_type_cxx_in),
                                 EmitStorageAddressForm::RawBytePointer);
  }

  EmitStorageAccess access = EmitStorageAccess::Ordinary;
  std::string text;
  // Address of the Pascal storage denoted by the designator, when that address
  // is known without forming a typed C++ reference. This is set for cases such
  // as `p^` and `T(x)`, where Pascal asks for storage but `&text` would first
  // dereference a pointer or bind a reinterpreted reference.
  std::string ptr_cxx;
  std::string type_cxx;
  // A bytewise designator can be a temporary type view over a live generated
  // C++ object. Typed-reference consumers restart this backing carrier after
  // the full expression; raw variant/untyped storage leaves both fields empty.
  std::string backing_ptr_cxx;
  std::string backing_type_cxx;
  EmitStorageAddressForm ptr_form = EmitStorageAddressForm::None;

  bool is_bytewise() const {
    return access == EmitStorageAccess::Bytewise ||
           access == EmitStorageAccess::UnalignedBytewise;
  }
  bool is_special() const { return access != EmitStorageAccess::Ordinary; }
  bool raw_address_needs_typed_cast() const {
    return is_bytewise() || ptr_form == EmitStorageAddressForm::RawBytePointer;
  }

 private:
  EmitStorageDesignator(EmitStorageAccess access_in, std::string text_in,
                        std::string ptr_cxx_in, std::string type_cxx_in,
                        std::string backing_ptr_cxx_in,
                        std::string backing_type_cxx_in,
                        EmitStorageAddressForm ptr_form_in)
      : access(access_in),
        text(std::move(text_in)),
        ptr_cxx(std::move(ptr_cxx_in)),
        type_cxx(std::move(type_cxx_in)),
        backing_ptr_cxx(std::move(backing_ptr_cxx_in)),
        backing_type_cxx(std::move(backing_type_cxx_in)),
        ptr_form(ptr_form_in) {}
};

// Storage / aliasing lowering. This module owns the dangerous questions about
// "same bytes, new type" and "does this expression denote mutable storage?".
//
// The intended policy is simple:
// - naturally aligned ordinary storage uses normal typed lvalues/references
// - aligned variant payload storage uses byte offsets for value operations so
//   reads/writes do not depend on C++ union active-member rules
// - unaligned bytewise views (`unaligned(...)`, typed casts over raw storage,
//   scalar fields inside packed records) go through memcpy-style helpers
// - typed var/out calls reject unaligned storage instead of manufacturing a
//   C++ reference that violates the callee's alignment assumptions
// - aggregate subobjects inside packed records are rejected unless they are
//   byte-aligned carriers that are safe to index directly
class EmitStorage {
 public:
  EmitStorage(const TypeRegistry& registry, ScopeStateView& scope,
              EmitAnalysis& analysis, EmitTypes& types,
              ResolveNameProvider& resolve_name_provider,
              EmitStorageExprOps& expr_ops);

  std::optional<EmitTypecastStorageView> typecast_storage_view(
      const ast::Expr& e);
  std::optional<EmitStorageDesignator> absolute_alias_designator(
      const ast::Ident& id);
  std::optional<EmitStorageDesignator> storage_designator(const ast::Expr& e);
  std::optional<EmitStorageDesignator> mutable_typecast_slot_designator(
      const ast::Expr& e);
  std::optional<EmitStorageDesignator> resolved_bytewise_with_field_storage(
      const ResolveResult& rr);
  std::string storage_designator_value(const EmitStorageDesignator& d);
  std::string storage_designator_member_base(const EmitStorageDesignator& d);
  std::string storage_designator_typed_lvalue(
      const EmitStorageDesignator& d);
  std::string storage_designator_typed_lvalue(
      const EmitStorageDesignator& d, std::string_view type_cxx);
  // Raw address of the Pascal storage denoted by a designator. This is for
  // internal storage computations: byte offsets, memcpy helpers, and backing
  // var/out machinery. It is not the public Pascal value of `@expr`.
  std::string storage_designator_raw_address(const EmitStorageDesignator& d);
  // Typed Pascal pointer value produced by `@expr`. This may cast an internal
  // raw byte address to `T*` because `@expr` has the type "^T" in Pascal.
  std::string storage_designator_typed_address_value(
      const EmitStorageDesignator& d, Location where);
  // Address passed to untyped `var`/`const` formals. Those formals receive raw
  // caller storage as `void*`/`const void*`, not the typed value of `@expr`.
  std::string storage_designator_untyped_actual_address(
      const EmitStorageDesignator& d, std::string_view ptr_cast);
  std::string storage_designator_store(const EmitStorageDesignator& d,
                                       const std::string& value_cxx);
  std::string storage_designator_inc_dec(const EmitStorageDesignator& d,
                                         bool is_inc,
                                         const std::string& delta_cxx = {});

  std::optional<EmitBytewiseStorage> bytewise_storage_ref(const ast::Expr& e);
  std::optional<EmitBytewiseStorage> packed_scalar_storage_ref(
      const ast::Expr& e);
  std::optional<EmitUntypedStorageIndexView> untyped_storage_index_view(
      const ast::Index& i);

  bool type_is_packed_record(const ast::TypeExpr* t);
  bool type_is_direct_packed_aggregate(const ast::TypeExpr* t);
  bool type_is_byte_aligned_packed_index_carrier(const ast::TypeExpr* t);
  // Safe value-only lowering for `packed.outer.inner_scalar`.
  //
  // Normal `&(outer.inner)` lowering is not acceptable here: C++ must form the
  // intermediate packed aggregate lvalue before taking the address. This helper
  // is the single place that may cross a packed aggregate field for an ordinary
  // scalar value read. It computes byte offsets from the aligned base object and
  // emits a memcpy-based unaligned load. Address-taking, var/out params,
  // assignment targets, method calls, and aggregate values still use the
  // rejecting paths below.
  std::optional<EmitPackedScalarValueLoad> packed_scalar_value_load(
      const ast::Expr& e);
  std::optional<EmitPackedAggregateFieldUse> direct_packed_aggregate_field_use(
      const ast::Expr& e);
  std::optional<EmitPackedAggregateFieldUse> packed_aggregate_path_use(
      const ast::Expr& e);
  bool variant_payload_path_use(const ast::Expr& e);
  bool member_value_may_need_storage_designator(const ast::Expr& e);
  void report_packed_aggregate_subobject_use(
      Location where, std::string_view op,
      const EmitPackedAggregateFieldUse& use);

  const ast::Expr* peel_primitive_casts(const ast::Expr* e);
  bool expr_is_storage_lvalue(const ast::Expr& e);
  bool expr_is_untyped_storage_ref(const ast::Expr& e);
  bool expr_is_charish(const ast::Expr& e);
  bool type_is_pcharish(const ast::TypeExpr* t);
  bool type_is_metaclass(const ast::TypeExpr* t);
  bool type_is_reference_class(const ast::TypeExpr* t);
  bool expr_is_reference_class(const ast::Expr& e);
  std::string member_access_op(const ast::Expr& e);
  bool type_is_stringish(const ast::TypeExpr* t);
  bool type_is_pointerish(const ast::TypeExpr* t);
  bool type_is_open_array(const ast::TypeExpr* t);
  bool fixed_array_pointer_can_decay_to_element_pointer(
      const ast::TypeExpr* src_type, const ast::TypeExpr* dst_type);
  bool fixed_char_array_value_can_decay_to_pchar(
      const ast::TypeExpr* src_type, const ast::TypeExpr* dst_type);
  bool pointer_to_object_upcast_is_valid(const ast::TypeExpr* dst_type,
                                         const ast::TypeExpr* src_type);
  bool pointer_to_object_downcast_is_valid(const ast::TypeExpr* dst_type,
                                           const ast::TypeExpr* src_type);
  bool class_to_interface_conversion_is_valid(const ast::TypeExpr* dst_type,
                                              const ast::TypeExpr* src_type);
  // Pascal `Ptype(p)^` where Ptype is a typed pointer and the cast is not
  // covered by the class-hierarchy path. Without this route the emitter would
  // produce `*(Ptype)p`, which is strict-aliasing UB whenever the pointee's
  // dynamic type is not Ptype's pointee. Return a bytewise storage designator
  // so downstream reads/writes go through the memcpy helpers.
  std::optional<EmitStorageDesignator> pointer_typecast_deref_as_bytewise(
      const ast::Deref& d);
  const ClassInfo* class_info_for_pointer_target(const ast::TypeExpr* t);
  const ClassInfo* class_info_for_value_type(const ast::TypeExpr* raw,
                                             const ast::TypeExpr* canonical);
  bool class_parent_chain_contains(const ClassInfo& ancestor,
                                   const ClassInfo& current) const;
  // Central pointer-value coercion policy used by explicit typecasts, plain
  // assignments, and value call arguments. Implicit Pascal pointer conversions
  // are concrete conversions such as `pointer`/`void*` <-> typed data pointers,
  // fixed-array pointer decay, pointer-to-object upcasts, and related
  // class-reference pointers; unrelated typed-pointer reinterpretation requires
  // an explicit Pascal cast.
  bool pointer_value_conversion_is_valid(const ast::TypeExpr* dst_type,
                                         const ast::TypeExpr* src_type,
                                         bool explicit_pascal_cast);
  std::string coerce_pointer_like_text(std::string_view dst_cxx,
                                       const ast::TypeExpr* dst_type,
                                       const ast::TypeExpr* src_type,
                                       const std::string& source_cxx,
                                       bool explicit_pascal_cast,
                                       bool source_is_const_storage = false);
  // Target-typed contexts may accept the same fixed-array address as either
  // `^array` or `^element`. When the destination is the element pointer form,
  // lower the wrapper pointer to its `T*` data member. Returns source_cxx
  // unchanged for any other shape.
  std::string lower_pointer_to_fixed_array_to_element(
      const ast::TypeExpr* src_type, const std::string& source_cxx);
  std::string lower_fixed_char_array_value_to_pchar(
      const ast::TypeExpr* src_type, const ast::TypeExpr* dst_type,
      const std::string& source_cxx);
  std::optional<EmitAbsoluteTargetInfo> resolve_absolute_target(
      const ast::VarDecl& vd);

 private:
  struct StorageCastTarget {
    std::string cxx;
    const ast::TypeExpr* type = nullptr;
    bool primitive = false;
  };

  // Return the C++ type text to use as `offsetof(TYPE, field)`. Named
  // Pascal record/object types use their generated struct name; anonymous local
  // aggregates use `decltype(base_expr_cxx)` because there is no named Pascal
  // type to ask the registry for.
  std::string offsetof_base_type_cxx(const ast::TypeExpr* t,
                                     const std::string& base_expr_cxx);
  const ast::TypeExpr* storage_expr_type(const ast::Expr& e);
  const ast::TypeExpr* canonical_storage_expr_type(const ast::Expr& e);
  std::string ord_storage_target_cxx(const ast::Expr& source);
  bool chr_source_has_byte_storage(const ast::Expr& source);
  std::optional<StorageCastTarget> storage_typecast_target(
      const ast::Call& call, const ast::Expr& source);
  std::string typecast_source_raw_pointer(const ast::Expr& source,
                                          const std::string& source_cxx,
                                          bool untyped_storage);
  // Pascal pointer/reference values support member selection on the pointed-to
  // object. Storage composition needs that object type when the current
  // designator is the pointer slot rather than the pointee.
  const ast::TypeExpr* pointer_like_member_object_type(const ast::TypeExpr* t);
  std::optional<EmitStorageDesignator> raw_address_index_designator(
      const ast::Index& i, const EmitStorageDesignator& base,
      const ast::TypeExpr* base_type);
  bool index_base_denotes_property_value(const ast::Index& i);
  std::string storage_type_cxx(const ast::TypeExpr* t);
  std::string scalar_storage_type_cxx(const ast::TypeExpr* t);
  std::string reference_class_cast_pointer_cxx(const ast::Expr& base_expr);
  EmitAbsoluteTargetInfo absolute_target_info(
      const std::string& target_cxx, const ast::TypeExpr* type,
      bool is_const_storage = false);

  const TypeRegistry& registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  ResolveNameProvider& resolve_name_provider_;
  EmitStorageExprOps& expr_ops_;
};

}  // namespace tp2cc
