#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "target_info.h"

namespace tp2cc::ast {
enum class BinOp : uint8_t;
struct ProcDecl;
struct Param;
struct TypeExpr;
struct TyName;
struct TyProcedural;
}  // namespace tp2cc::ast

namespace tp2cc {

struct MethodSig;

// Pascal value identifiers lower to `p_<name>` and named Pascal types lower to
// `t_<name>`. Keeping that split central avoids C++ scope collisions while the
// resolver still follows Pascal's context-sensitive type/value lookup.
std::string mangle(std::string_view name);
std::string type_mangle(std::string_view name);
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
// synthesis use the same TyName instance for each builtin.
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
std::string binary_pascal_operator_token(ast::BinOp op);
std::string pascal_operator_cxx_token(std::string_view op);
bool pascal_operator_decl_uses_named_helper(const ast::ProcDecl& pd);
std::string pascal_operator_named_helper_name(const ast::ProcDecl& pd);
std::string pascal_operator_decl_name_to_cxx(const ast::ProcDecl& pd);
std::string pascal_assignment_operator_helper_name(const ast::ProcDecl& pd);
size_t procedural_param_count(const ast::TyProcedural& p);
std::string enum_bound_name(std::string_view type_name, std::string_view which);

// Signedness classifier for value-choice tracking (e.g., which integer atom
// best fits a constant domain). This is metadata about a value or formal, not
// type identity: it must not be used as a proxy for "is this type an integer?"
// -- that question goes through atom identity (registry->builtin_literal).
enum class PrimitiveIntKind : uint8_t { None, Signed, Unsigned };

// Floating-point precision tier. Zero for non-real atoms; otherwise an
// ordering value (1 < 2 < 3) matching Pascal's implicit-conversion ranking
// (single < double < extended). `comp` carries the same rank as `extended`
// because Pascal treats it as an 80-bit fixed-point view of extended.
enum class PrimitiveFloatTier : uint8_t { None, Single, Double, Extended };

// Pascal primitive-type family. Atoms group naturally into ordinal,
// real, char, string, pointer, file, and "other" categories. Callers that
// need "is this a char-family atom?" or "is this a pointer primitive?" ask
// here instead of hand-enumerating atom names at every call site.
enum class PrimitiveFamily : uint8_t {
  Other,
  Integer,      // byte, shortint, smallint, word, longint, cardinal, int64, qword, ...
  Real,         // single, double, real, extended, comp
  Bool,         // boolean, bytebool, wordbool, longbool, qwordbool
  Char,         // char, ansichar
  WideChar,     // widechar (16-bit, distinct storage from char)
  String,       // shortstring, ansistring, utf8string
  Pointer,      // pointer, pchar, pansichar
  Text,         // text
};

struct PrimitiveInfo {
  // Pascal primitive-type table. Single source of truth for every property
  // callers need: the C++ type text, the family, and family-specific
  // metadata (signedness+width for integers, precision tier for reals).
  // These are metadata about the atom, not type-identity proxies.
  const char* cxx = nullptr;
  PrimitiveFamily family = PrimitiveFamily::Other;
  PrimitiveIntKind int_kind = PrimitiveIntKind::None;
  PrimitiveFloatTier float_tier = PrimitiveFloatTier::None;
  uint8_t bits = 0;
  // True for PtrInt/PtrUInt/SizeInt/SizeUInt: width is the target pointer
  // width, and `bits` is not read.
  bool pointer_sized = false;

  int float_rank() const {
    switch (float_tier) {
      case PrimitiveFloatTier::Single: return 1;
      case PrimitiveFloatTier::Double: return 2;
      case PrimitiveFloatTier::Extended: return 3;
      case PrimitiveFloatTier::None: return 0;
    }
    return 0;
  }

  bool is_integer() const { return int_kind != PrimitiveIntKind::None; }
  bool is_real() const { return float_tier != PrimitiveFloatTier::None; }
  bool is_bool() const { return family == PrimitiveFamily::Bool; }
  bool is_char() const { return family == PrimitiveFamily::Char; }
  bool is_widechar() const { return family == PrimitiveFamily::WideChar; }
  bool is_string() const { return family == PrimitiveFamily::String; }
  bool is_pointer_primitive() const { return family == PrimitiveFamily::Pointer; }
  bool is_text() const { return family == PrimitiveFamily::Text; }
};

// Runtime named types and reference-class helpers for stubs / builtins that do
// not come from a translated user unit.
const std::unordered_map<std::string, const char*>& runtime_named_type_map();
std::string runtime_named_type_cxx(std::string_view lowname);
std::string builtin_reference_class_struct_cxx(std::string_view lowname);
std::string unit_namespace_prefix(std::string_view unit_name);

// Primitive type-table queries used throughout emit-time type and constant
// lowering.
const PrimitiveInfo* primitive_info(std::string_view lowname);
bool is_primitive_type(std::string_view lowname);
std::string primitive_type_cxx(std::string_view lowname);
// The full primitive-type table. Each Pascal primitive name is a built-in
// type literal (atom) in the language; the registry iterates this table at
// build() time to intern one canonical descriptor per atom.
const std::unordered_map<std::string, PrimitiveInfo>& primitive_type_map();
uint64_t low_bits(uint64_t value, uint8_t bits);
std::string uint64_literal_text(uint64_t value);

// Resolve a primitive integer type's bit width.  For pointer-sized
// primitives (ptrint/ptruint/sizeint/sizeuint), returns the target pointer
// width; for everything else, returns the fixed table width.
// Throws std::logic_error if `info` is not an integer primitive.
[[nodiscard]] uint8_t primitive_bits(const PrimitiveInfo& info,
                                     TargetInfo target);

// Range/mask math for a *resolved* width in [1, 64]. Each throws
// std::logic_error on an out-of-range width.
[[nodiscard]] uint64_t unsigned_mask_for_bits(uint8_t bits);
[[nodiscard]] int64_t signed_min_for_bits(uint8_t bits);
[[nodiscard]] int64_t signed_max_for_bits(uint8_t bits);

std::string signed_bits_literal_text(uint64_t bits, uint8_t width,
                                     std::string_view cxx);
std::string primitive_low_high_expr(std::string_view lowname, bool want_low);
const ast::TyName* builtin_integer_type(std::string_view lowname);
const PrimitiveInfo* primitive_info_for_value(int64_t value);
const ast::TyName* builtin_integer_type(const PrimitiveInfo* info);
const PrimitiveInfo* shift_carrier_primitive(const PrimitiveInfo* info,
                                             TargetInfo target);

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
                              int64_t shift, int64_t* out,
                              TargetInfo target);
bool checked_pascal_shr_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out,
                              TargetInfo target);
bool primitive_name_is_charish(std::string_view lowname);

}  // namespace tp2cc
