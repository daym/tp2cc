#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast.h"
#include "emit_analysis.h"
#include "emit_context.h"

namespace tp2cc {

struct MethodSig;
struct TypeRegistry;

class EmitTypeConstRender {
 public:
  virtual ~EmitTypeConstRender() = default;
  virtual std::string const_value_to_cxx(
      const ast::Expr& e, const ast::TypeExpr* target = nullptr,
      bool explicit_conversion = false) = 0;
};

class EmitTypeDiagOps {
 public:
  virtual ~EmitTypeDiagOps() = default;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

struct EmitRecordFieldDecl {
  // Field declaration metadata shared by named-record emission, inline record
  // spelling, and packed-layout computation.
  const ast::TypeExpr* type = nullptr;
  std::string type_cxx;
  std::string mangled_name;
  std::string decl;
  EmitRecordFieldDecl(const ast::TypeExpr* type_in, std::string type_cxx_in,
                      std::string mangled_name_in, std::string decl_in)
      : type(type_in),
        type_cxx(std::move(type_cxx_in)),
        mangled_name(std::move(mangled_name_in)),
        decl(std::move(decl_in)) {}
};

struct EmitPackedRecordLayout {
  // Byte-exact packed-record layout summary: each field's Pascal byte offset
  // expression plus the total packed size expression.
  std::vector<std::pair<std::string, std::string>> field_offsets;
  std::string size_expr;
  EmitPackedRecordLayout(
      std::vector<std::pair<std::string, std::string>> field_offsets_in,
      std::string size_expr_in)
      : field_offsets(std::move(field_offsets_in)),
        size_expr(std::move(size_expr_in)) {}
};

// Central Pascal type/layout lowering. This module owns the rules for spelling
// Pascal types in C++, keeping registry-defined types ahead of runtime stubs,
// preserving enum/subrange/array layout decisions, and computing packed-record
// layout metadata for later static_asserts.
class EmitTypes {
 public:
  EmitTypes(const TypeRegistry* registry, ScopeStateView& scope,
            EmitAnalysis& analysis, EmitTypeConstRender& const_render,
            EmitTypeDiagOps& diag_ops);

  std::string type_to_cxx(const ast::TypeExpr& t);
  std::string type_name_to_cxx(const ast::TyName& n);
  std::string type_name_text_to_cxx(std::string_view name);
  std::string named_type_struct_cxx(std::string_view name);
  std::string visible_type_prefix(std::string_view name);
  std::string metaclass_struct_cxx(std::string_view class_name);
  std::string metaclass_value_fn_cxx(std::string_view class_name);
  bool enum_has_explicit_values(const ast::TyEnum& e);
  std::optional<int64_t> enum_member_value_int64(const ast::TyEnum& e,
                                                 size_t index);
  std::string enum_member_value_to_cxx(const ast::TyEnum& e, size_t index);
  std::string enum_underlying_type_to_cxx(const ast::TyEnum& e);
  bool array_dim_bounds_to_cxx(const ast::TypeExpr& dim, std::string* lo,
                               std::string* size_expr);
  std::string subrange_type_to_cxx(const ast::TySubrange& r);
  std::string string_type_to_cxx(const ast::TyString& s);
  std::optional<std::string> shortstring_capacity_to_cxx(
      const ast::TypeExpr* t);
  bool param_uses_shortstring_ref(const ast::TypeExpr* t,
                                  ast::Param::Mode mode);
  std::string shortstring_ref_type_to_cxx(const ast::TypeExpr* t);
  std::string pointer_type_to_cxx(const ast::TyPointer& p);
  std::string set_type_to_cxx(const ast::TySet& s);
  std::optional<std::string> enum_carrier_type_to_cxx(
      const ast::TyEnum& e);
  std::string enum_type_to_cxx(const ast::TyEnum& e,
                               const std::string& context);
  // Pascal fixed arrays keep their source bounds in the type, so successful
  // lowering here must preserve those bounds rather than silently normalizing
  // everything to zero-based storage.
  std::string array_type_to_cxx(const ast::TyArray& a);
  std::string procedural_param_types_to_cxx(
      const std::vector<ast::Param>& params);
  std::string method_pointer_helper_name(const ast::ProcDecl& pd);
  std::string procedural_type_to_cxx(const ast::TyProcedural& p);
  std::string inline_record_type_to_cxx(const ast::TyRecord& tr);
  std::vector<EmitRecordFieldDecl> record_field_decls(
      const std::vector<ast::RecordField>& fields);
  EmitPackedRecordLayout compute_packed_record_layout(const ast::TyRecord& tr);
  // Attach a declarator name to a lowered type, including the special procvar
  // case where the identifier must live inside the `(*)` declarator.
  std::string named_type_to_cxx(const ast::TypeExpr* t, std::string_view name,
                                std::string_view name_prefix = {});
  std::string low_high_expr_for_named_type(std::string_view name,
                                           bool want_low);
  std::string low_high_expr_for_type(const ast::TypeExpr* t, bool want_low);
  std::string open_array_type_to_cxx(const ast::TypeExpr& t);

 private:
  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypeConstRender& const_render_;
  EmitTypeDiagOps& diag_ops_;

  // Did any non-runtime translated unit declare this type? Runtime aliases are
  // present in the registry for analysis/member lookup, but their C++ spelling
  // remains the explicit ::rt::t_* names from runtime_named_type_cxx().
  bool registry_knows_translated_type(std::string_view name);
};

}  // namespace tp2cc
