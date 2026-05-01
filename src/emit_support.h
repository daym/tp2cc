#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tp2cc::ast {
struct ProcDecl;
struct Param;
struct TypeExpr;
struct TyName;
struct TyProcedural;
}  // namespace tp2cc::ast

namespace tp2cc {

struct MethodSig;

// Pascal identifiers always lower to `p_<name>` in generated C++. Keeping that
// policy in one support module avoids ad hoc spelling rules elsewhere.
std::string mangle(std::string_view name);
std::string ascii_lower(std::string_view text);
std::string char_literal_body_to_cxx(char c);
std::string attach_named_cxx_type(std::string_view ty, std::string_view name,
                                  std::string_view name_prefix);

// Pascal class methods are stored as overload vectors per name. Many emit
// paths only need a representative signature for shared metadata
// (kind/decl/virtual/accepts-zero-args), so centralize the "front if any"
// choice here rather than redoing it at each call site.
const MethodSig* representative_method(const std::vector<MethodSig>& sigs);

// Canonical AST nodes for frequently synthesized builtin Pascal types. These
// are intentionally shared singletons so emit-time comparisons and helper
// synthesis stay anchored to one spelling per Pascal builtin.
const ast::TyName* builtin_char_type();
const ast::TyName* builtin_string_type();
const ast::TyName* builtin_pchar_type();
const ast::TyName* builtin_boolean_type();
const ast::TyName* named_pascal_type(std::string_view name);

// Names for synthesized helpers and enum bound constants emitted alongside
// translated units.
std::string nested_result_slot_name(std::string_view fn_name);
bool is_pascal_result_ident(std::string_view name);
std::string encode_helper_ident(std::string_view name);
std::string encode_helper_type(const ast::TypeExpr& t);
std::string encode_helper_params(const std::vector<ast::Param>& params);
std::string pascal_operator_cxx_token(std::string_view op);
std::string pascal_assignment_operator_helper_name(const ast::ProcDecl& pd);
size_t procedural_param_count(const ast::TyProcedural& p);
std::string enum_bound_name(std::string_view type_name, std::string_view which);

enum class PrimitiveIntKind : uint8_t { None, Signed, Unsigned };

struct PrimitiveInfo {
  // Pascal primitive-type table. Single source of truth for every property
  // callers need: the C++ spelling plus, for ordinal primitives, the
  // signedness and width the emitter must use when reproducing Pascal
  // overflow and range semantics.
  const char* cxx = nullptr;
  PrimitiveIntKind int_kind = PrimitiveIntKind::None;
  uint8_t bits = 0;
};

// Runtime named types and reference-class helpers for stubs / builtins that do
// not come from a translated user unit.
std::string runtime_named_type_cxx(std::string_view lowname);
std::string builtin_reference_class_struct_cxx(std::string_view lowname);
std::string unit_namespace_prefix(std::string_view unit_name);

// Primitive type-table queries used throughout emit-time type and constant
// lowering.
const PrimitiveInfo* primitive_info(std::string_view lowname);
bool is_primitive_type(std::string_view lowname);
std::string primitive_type_cxx(std::string_view lowname);
uint64_t low_bits(uint64_t value, uint8_t bits);
std::string uint64_literal_text(uint64_t value);
std::string signed_bits_literal_text(uint64_t bits, const PrimitiveInfo& info);
std::string primitive_low_high_expr(std::string_view lowname, bool want_low);
const ast::TyName* builtin_integer_type(std::string_view lowname);
const PrimitiveInfo* primitive_info_for_value(int64_t value);
const ast::TyName* builtin_integer_type(const PrimitiveInfo* info);
const PrimitiveInfo* shift_carrier_primitive(const PrimitiveInfo* info);

// Checked Pascal integer arithmetic helpers. These are about Pascal overflow
// semantics, not generic C++ math convenience.
bool checked_add_int64(int64_t a, int64_t b, int64_t* out);
bool checked_sub_int64(int64_t a, int64_t b, int64_t* out);
bool checked_mul_int64(int64_t a, int64_t b, int64_t* out);
bool checked_div_int64(int64_t a, int64_t b, int64_t* out);
bool checked_mod_int64(int64_t a, int64_t b, int64_t* out);
bool checked_shift_count(int64_t shift);
bool checked_shl_int64(int64_t a, int64_t shift, int64_t* out);
bool checked_shr_int64(int64_t a, int64_t shift, int64_t* out);
bool checked_pascal_shl_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out);
bool checked_pascal_shr_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out);
bool tyname_is(const ast::TypeExpr* t, std::string_view expected);

}  // namespace tp2cc
