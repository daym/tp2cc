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
struct TypeDescriptor;
struct TypeRegistry;

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

// Names for synthesized helpers and enum bound constants emitted alongside
// translated units.
std::string nested_result_slot_name(std::string_view fn_name);
bool is_pascal_result_ident(std::string_view name);
std::string encode_helper_ident(std::string_view name);
std::string encode_helper_type(const TypeRegistry& registry,
                               const ast::TypeExpr& t);
std::string encode_helper_params(const TypeRegistry& registry,
                                 const std::vector<ast::Param>& params);
std::string binary_pascal_operator_token(ast::BinOp op);
std::string pascal_operator_cxx_token(std::string_view op);
bool pascal_operator_decl_uses_named_helper(const ast::ProcDecl& pd);
std::string pascal_operator_named_helper_name(
    const TypeRegistry& registry, const ast::ProcDecl& pd);
std::string pascal_operator_decl_name_to_cxx(
    const TypeRegistry& registry, const ast::ProcDecl& pd);
std::string pascal_assignment_operator_helper_name(
    const TypeRegistry& registry, const ast::ProcDecl& pd);
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

enum class PrimitiveKind : uint8_t {
  LongInt,
  LongWord,
  SmallInt,
  Word,
  ShortInt,
  Byte,
  Char,
  WideChar,
  Boolean,
  ByteBool,
  WordBool,
  LongBool,
  QWordBool,
  Single,
  Double,
  Real,
  Extended,
  Comp,
  Pointer,
  PChar,
  Text,
  Int64,
  QWord,
  Currency,
  PtrInt,
  PtrUInt,
  SizeInt,
  SizeUInt,
  ShortString,
  AnsiString,
  Utf8String,
};

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
  const TypeDescriptor* descriptor = nullptr;
  // Pascal primitive-type table. Single source of truth for every property
  // callers need: the C++ type text, the family, and family-specific
  // metadata (signedness+width for integers, precision tier for reals).
  // These are metadata about the atom, not type-identity proxies.
  PrimitiveKind kind = PrimitiveKind::LongInt;
  const char* cxx = nullptr;
  PrimitiveFamily family = PrimitiveFamily::Other;
  PrimitiveIntKind int_kind = PrimitiveIntKind::None;
  PrimitiveFloatTier float_tier = PrimitiveFloatTier::None;
  uint8_t bits = 0;
  // True for PtrInt/PtrUInt/SizeInt/SizeUInt: width is the target pointer
  // width, and `bits` is not read.
  bool pointer_sized = false;
  // One of the fixed-width ordinal integer declarations used when Pascal
  // infers an integer carrier from a value domain. Currency and pointer-sized
  // ABI integers deliberately do not participate.
  bool is_ordinal_integer = false;

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

std::string unit_namespace_prefix(std::string_view unit_name);

// Compiler-provided primitive declaration data. TypeRegistry interns these
// declarations and semantic consumers use the resulting descriptors.
const std::unordered_map<std::string, PrimitiveInfo>& primitive_type_map();
const PrimitiveInfo* primitive_info(std::string_view lowname);
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
std::string primitive_low_high_expr(const PrimitiveInfo* info, bool want_low);
const PrimitiveInfo* ordinal_integer_primitive(
    const TypeRegistry& registry, PrimitiveIntKind kind, uint8_t bits);
const PrimitiveInfo* primitive_info_for_value(const TypeRegistry& registry,
                                              int64_t value);
const PrimitiveInfo* shift_carrier_primitive(const TypeRegistry& registry,
                                             const PrimitiveInfo* info,
                                             TargetInfo target);

// Checked Pascal integer arithmetic helpers. These are about Pascal overflow
// semantics, not generic C++ math convenience.
bool checked_add_int64(int64_t a, int64_t b, int64_t* out);
bool checked_sub_int64(int64_t a, int64_t b, int64_t* out);
bool checked_pascal_shl_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out,
                              TargetInfo target);
bool checked_pascal_shr_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out,
                              TargetInfo target);

}  // namespace tp2cc
