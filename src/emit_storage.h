#pragma once

#include <optional>
#include <string>
#include <string_view>

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
  virtual void report_error(Location where, const std::string& msg) = 0;
};

struct EmitBytewiseStorage {
  // Byte-addressable storage view safe only for memcpy-style helpers.
  // The pointer text may denote misaligned storage (`packed` fields,
  // `unaligned(...)`, typed derefs over byte buffers), so callers must never
  // turn it into `*reinterpret_cast<T*>(p)` or a C++ reference.
  std::string void_ptr_text;
  std::string elem_cxx;
};

struct EmitUntypedStorageIndexView {
  std::string elem_cxx;
  std::string ptr_cxx;
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
  bool is_pointerish = false;
  bool is_const_storage = false;
};

struct EmitTypecastStorageView {
  const ast::Expr* source = nullptr;
  std::string source_cxx;
  std::string target_cxx;
  const ast::TypeExpr* target_type = nullptr;
  bool target_is_primitive = false;
  bool source_is_untyped_storage = false;
  bool pointee_view = false;
};

// Storage / aliasing lowering. This module owns the dangerous questions about
// "same bytes, new type" and "does this expression denote mutable storage?".
//
// The intended policy is simple:
// - naturally aligned ordinary storage uses normal typed lvalues/references
// - explicit bytewise views (`unaligned(...)`, typed casts over raw storage,
//   scalar fields inside packed records) go through memcpy-style helpers
// - aggregate subobjects inside packed records are rejected unless they are
//   byte-aligned carriers that are safe to index directly
class EmitStorage {
 public:
  EmitStorage(const TypeRegistry* registry, ScopeStateView& scope,
              EmitAnalysis& analysis, EmitTypes& types,
              ResolveNameProvider& resolve_name_provider,
              EmitStorageExprOps& expr_ops);

  std::string primitive_cast_lvalue_ref(const ast::Call& c);
  std::string primitive_cast_untyped_storage_ptr(const ast::Call& c);
  std::string primitive_cast_packed_field_ptr(const ast::Call& c);
  std::optional<EmitTypecastStorageView> typecast_storage_view(
      const ast::Expr& e);

  std::optional<EmitBytewiseStorage> bytewise_storage_ref(const ast::Expr& e);
  std::optional<EmitBytewiseStorage> packed_field_storage_ref(
      const ast::Expr& e);
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
  // Central pointer-like coercion policy used by explicit typecasts, plain
  // assignments, and value call arguments. Pascal is more permissive than C++
  // about:
  //   - `pointer`/`void*` <-> typed data pointers
  //   - raw data-pointer reinterpretation
  //   - function pointer <-> `pointer`
  // so funnel those through one helper instead of sprinkling ad hoc casts.
  std::string coerce_pointer_like_text(std::string_view dst_cxx,
                                       const ast::TypeExpr* dst_type,
                                       const ast::TypeExpr* src_type,
                                       const std::string& source_cxx,
                                       bool explicit_pascal_cast,
                                       bool source_is_const_storage = false);
  std::string reinterpret_ref_text(const std::string& ty_cxx,
                                   const std::string& source_cxx,
                                   bool pointee_view);

  std::optional<EmitAbsoluteTargetInfo> resolve_absolute_target(
      const ast::VarDecl& vd);

 private:
  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  ResolveNameProvider& resolve_name_provider_;
  EmitStorageExprOps& expr_ops_;
};

}  // namespace tp2cc
