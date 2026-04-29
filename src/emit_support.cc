#include "emit_support.h"

#include <cstdio>
#include <limits>
#include <unordered_map>

#include "ast.h"
#include "typereg.h"

namespace tp2cc {

std::string mangle(std::string_view name) {
  std::string s("p_");
  s.append(name);
  return s;
}

std::string ascii_lower(std::string_view text) {
  std::string s(text);
  for (char& ch : s) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return s;
}

const MethodSig* representative_method(const std::vector<MethodSig>& sigs) {
  return sigs.empty() ? nullptr : &sigs.front();
}

const ast::TyName* builtin_char_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "char";
    return n;
  }();
  return &t;
}

const ast::TyName* builtin_string_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "string";
    return n;
  }();
  return &t;
}

const ast::TyName* builtin_pchar_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "pchar";
    return n;
  }();
  return &t;
}

const ast::TyName* builtin_boolean_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "boolean";
    return n;
  }();
  return &t;
}

const ast::TyName* named_pascal_type(std::string_view name) {
  static std::unordered_map<std::string, ast::TyName> cache;
  auto [it, inserted] = cache.emplace(std::string(name), ast::TyName{});
  if (inserted) it->second.name = std::string(name);
  return &it->second;
}

std::string nested_result_slot_name(std::string_view fn_name) {
  return "tp2cc_result_" + mangle(fn_name);
}

bool is_pascal_result_ident(std::string_view name) {
  return ascii_lower(name) == "result";
}

std::string enum_bound_name(std::string_view type_name, std::string_view which) {
  return "tp2cc_enum_" + std::string(which) + "_" +
         ascii_lower(std::string(type_name));
}

const std::unordered_map<std::string, PrimitiveInfo>& primitive_type_map() {
  static const std::unordered_map<std::string, PrimitiveInfo> m = {
      {"integer", {"int32_t", PrimitiveIntKind::Signed, 32}},
      {"longint", {"int32_t", PrimitiveIntKind::Signed, 32}},
      {"cardinal", {"uint32_t", PrimitiveIntKind::Unsigned, 32}},
      {"longword", {"uint32_t", PrimitiveIntKind::Unsigned, 32}},
      {"smallint", {"int16_t", PrimitiveIntKind::Signed, 16}},
      {"word", {"uint16_t", PrimitiveIntKind::Unsigned, 16}},
      {"shortint", {"int8_t", PrimitiveIntKind::Signed, 8}},
      {"byte", {"uint8_t", PrimitiveIntKind::Unsigned, 8}},
      {"char", {"::rt::p_char", PrimitiveIntKind::None, 0}},
      {"widechar", {"uint16_t", PrimitiveIntKind::Unsigned, 16}},
      {"boolean", {"bool", PrimitiveIntKind::None, 0}},
      {"bytebool", {"uint8_t", PrimitiveIntKind::Unsigned, 8}},
      {"wordbool", {"uint16_t", PrimitiveIntKind::Unsigned, 16}},
      {"longbool", {"uint32_t", PrimitiveIntKind::Unsigned, 32}},
      {"single", {"float", PrimitiveIntKind::None, 0}},
      {"double", {"double", PrimitiveIntKind::None, 0}},
      {"real", {"double", PrimitiveIntKind::None, 0}},
      {"extended", {"long double", PrimitiveIntKind::None, 0}},
      {"comp", {"long double", PrimitiveIntKind::None, 0}},
      {"pointer", {"void*", PrimitiveIntKind::None, 0}},
      {"pchar", {"::rt::p_char*", PrimitiveIntKind::None, 0}},
      {"ppchar", {"::rt::p_char**", PrimitiveIntKind::None, 0}},
      {"text", {"::rt::tp2cc_TextFile", PrimitiveIntKind::None, 0}},
      {"int64", {"int64_t", PrimitiveIntKind::Signed, 64}},
      {"qword", {"uint64_t", PrimitiveIntKind::Unsigned, 64}},
      {"dword", {"uint32_t", PrimitiveIntKind::Unsigned, 32}},
      {"currency", {"::rt::p_currency", PrimitiveIntKind::Signed, 64}},
      {"ptrint", {"::rt::p_ptrint", PrimitiveIntKind::Signed, 32}},
      {"ptruint", {"::rt::p_ptruint", PrimitiveIntKind::Unsigned, 32}},
      {"sizeint", {"::rt::p_sizeint", PrimitiveIntKind::Signed, 32}},
      {"sizeuint", {"::rt::p_sizeuint", PrimitiveIntKind::Unsigned, 32}},
      {"string", {"::rt::tp2cc_ShortString<>", PrimitiveIntKind::None, 0}},
      {"shortstring", {"::rt::tp2cc_ShortString<>", PrimitiveIntKind::None, 0}},
      {"ansistring", {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None, 0}},
      {"utf8string", {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None, 0}},
  };
  return m;
}

const std::unordered_map<std::string, const char*>& runtime_named_type_map() {
  static const std::unordered_map<std::string, const char*> m = {
      {"currency", "::rt::p_currency"},
      {"datetime", "::rt::p_datetime"},
      {"tdatetime", "::rt::p_tdatetime"},
      {"dirstr", "::rt::p_dirstr"},
      {"namestr", "::rt::p_namestr"},
      {"extstr", "::rt::p_extstr"},
      {"pansistring", "::rt::p_pansistring"},
      {"pcardinal", "::rt::p_pcardinal"},
      {"pcurrency", "::rt::p_pcurrency"},
      {"pint64", "::rt::p_pint64"},
      {"pathstr", "::rt::p_pathstr"},
      {"ppointer", "::rt::p_ppointer"},
      {"ptrint", "::rt::p_ptrint"},
      {"ptruint", "::rt::p_ptruint"},
      {"searchrec", "::rt::p_searchrec"},
      {"signalhandler", "::rt::p_signalhandler"},
      {"sizeint", "::rt::p_sizeint"},
      {"sizeuint", "::rt::p_sizeuint"},
      {"stat", "::rt::p_stat"},
      {"tclass", "::rt::p_tclass"},
      {"tfpuexception", "::rt::p_tfpuexception"},
      {"tfpuexceptionmask", "::rt::p_tfpuexceptionmask"},
      {"tsearchrec", "::rt::p_tsearchrec"},
      {"tsystemtime", "::rt::p_tsystemtime"},
      {"tmethod", "::rt::p_tmethod"},
  };
  return m;
}

std::string runtime_named_type_cxx(std::string_view lowname) {
  auto it = runtime_named_type_map().find(ascii_lower(lowname));
  return it == runtime_named_type_map().end() ? std::string()
                                              : std::string(it->second);
}

struct BuiltinReferenceClassInfo {
  const char* struct_cxx;
  const char* parent;
  const char* defining_unit;
};

const std::unordered_map<std::string, BuiltinReferenceClassInfo>&
builtin_reference_class_map() {
  static const std::unordered_map<std::string, BuiltinReferenceClassInfo> m = {
      {"tobject", {"::rt::p_tobject", "", "__rt__"}},
      {"exception", {"::rt::p_exception", "tobject", "sysutils"}},
      {"eexternal", {"p_sysutils::p_eexternal", "exception", "sysutils"}},
      {"einterror", {"p_sysutils::p_einterror", "eexternal", "sysutils"}},
      {"eintoverflow",
       {"p_sysutils::p_eintoverflow", "einterror", "sysutils"}},
      {"eoserror", {"p_sysutils::p_eoserror", "exception", "sysutils"}},
  };
  return m;
}

std::string builtin_reference_class_struct_cxx(std::string_view lowname) {
  auto it = builtin_reference_class_map().find(ascii_lower(lowname));
  return it == builtin_reference_class_map().end()
             ? std::string()
             : std::string(it->second.struct_cxx);
}

std::string unit_namespace_prefix(std::string_view unit_name) {
  return unit_name == "__rt__" ? std::string("::rt::")
                               : (mangle(unit_name) + "::");
}

const PrimitiveInfo* primitive_info(std::string_view lowname) {
  auto it = primitive_type_map().find(std::string(lowname));
  return it == primitive_type_map().end() ? nullptr : &it->second;
}

bool is_primitive_type(std::string_view lowname) {
  return primitive_info(lowname) != nullptr;
}

std::string primitive_type_cxx(std::string_view lowname) {
  auto* info = primitive_info(lowname);
  return info ? info->cxx : std::string();
}

uint64_t low_bits(uint64_t value, uint8_t bits) {
  if (bits >= 64) return value;
  return value & ((uint64_t{1} << bits) - 1);
}

std::string uint64_literal_text(uint64_t value) {
  char buf[32];
  const char* fmt =
      (value > static_cast<uint64_t>(INT64_MAX)) ? "%lluULL" : "%llu";
  std::snprintf(buf, sizeof(buf), fmt,
                static_cast<unsigned long long>(value));
  return buf;
}

std::string signed_bits_literal_text(uint64_t bits, const PrimitiveInfo& info) {
  if (info.bits == 0) return "0";
  uint64_t sign_bit = uint64_t{1} << (info.bits - 1);
  if ((bits & sign_bit) == 0) return uint64_literal_text(bits);
  if (info.bits == 64 && bits == sign_bit) {
    return "::std::numeric_limits<" + std::string(info.cxx) + ">::min()";
  }
  uint64_t magnitude =
      (info.bits == 64) ? (uint64_t{0} - bits) : low_bits(~bits + 1, info.bits);
  return "-" + uint64_literal_text(magnitude);
}

std::string primitive_low_high_expr(std::string_view lowname, bool want_low) {
  if (lowname == "char") {
    return want_low ? "::rt::tp2cc_char_of(0)" : "::rt::tp2cc_char_of(255)";
  }
  if (lowname == "boolean" || lowname == "bytebool" ||
      lowname == "wordbool" || lowname == "longbool") {
    return want_low ? "false" : "true";
  }
  const PrimitiveInfo* info = primitive_info(lowname);
  if (!info || info->int_kind == PrimitiveIntKind::None) return {};
  if (want_low) {
    if (info->int_kind == PrimitiveIntKind::Unsigned) return "0";
    return "::std::numeric_limits<" + std::string(info->cxx) + ">::min()";
  }
  return "::std::numeric_limits<" + std::string(info->cxx) + ">::max()";
}

const ast::TyName* builtin_integer_type(std::string_view lowname) {
  auto make = [](const char* name) {
    ast::TyName n;
    n.name = name;
    return n;
  };
  static const ast::TyName t_shortint = make("shortint");
  static const ast::TyName t_byte = make("byte");
  static const ast::TyName t_smallint = make("smallint");
  static const ast::TyName t_word = make("word");
  static const ast::TyName t_longint = make("longint");
  static const ast::TyName t_cardinal = make("cardinal");
  static const ast::TyName t_int64 = make("int64");
  static const ast::TyName t_qword = make("qword");

  if (lowname == "shortint") return &t_shortint;
  if (lowname == "byte") return &t_byte;
  if (lowname == "smallint") return &t_smallint;
  if (lowname == "word") return &t_word;
  if (lowname == "longint" || lowname == "integer") return &t_longint;
  if (lowname == "cardinal" || lowname == "longword" || lowname == "dword")
    return &t_cardinal;
  if (lowname == "int64") return &t_int64;
  if (lowname == "qword") return &t_qword;
  return nullptr;
}

const PrimitiveInfo* primitive_info_for_value(int64_t value) {
  if (value >= -128 && value <= 127) return primitive_info("shortint");
  if (value >= 0 && value <= 255) return primitive_info("byte");
  if (value >= -32768 && value <= 32767) return primitive_info("smallint");
  if (value >= 0 && value <= 65535) return primitive_info("word");
  if (value >= INT32_MIN && value <= INT32_MAX) return primitive_info("longint");
  if (value >= 0) return primitive_info("cardinal");
  return primitive_info("int64");
}

const ast::TyName* builtin_integer_type(const PrimitiveInfo* info) {
  if (info == primitive_info("shortint")) return builtin_integer_type("shortint");
  if (info == primitive_info("byte")) return builtin_integer_type("byte");
  if (info == primitive_info("smallint")) return builtin_integer_type("smallint");
  if (info == primitive_info("word")) return builtin_integer_type("word");
  if (info == primitive_info("longint")) return builtin_integer_type("longint");
  if (info == primitive_info("cardinal")) return builtin_integer_type("cardinal");
  if (info == primitive_info("int64")) return builtin_integer_type("int64");
  if (info == primitive_info("qword")) return builtin_integer_type("qword");
  return nullptr;
}

bool checked_add_int64(int64_t a, int64_t b, int64_t* out) {
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
    return false;
  }
  *out = a + b;
  return true;
}

bool checked_sub_int64(int64_t a, int64_t b, int64_t* out) {
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
    return false;
  }
  *out = a - b;
  return true;
}

bool checked_mul_int64(int64_t a, int64_t b, int64_t* out) {
  if (a == 0 || b == 0) {
    *out = 0;
    return true;
  }
  if (a == -1) {
    if (b == INT64_MIN) return false;
    *out = -b;
    return true;
  }
  if (b == -1) {
    if (a == INT64_MIN) return false;
    *out = -a;
    return true;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > INT64_MAX / b) return false;
    } else if (b < INT64_MIN / a) {
      return false;
    }
  } else if (b > 0) {
    if (a < INT64_MIN / b) return false;
  } else if (a != 0 && b < INT64_MAX / a) {
    return false;
  }
  *out = a * b;
  return true;
}

bool checked_div_int64(int64_t a, int64_t b, int64_t* out) {
  if (b == 0) return false;
  if (a == INT64_MIN && b == -1) return false;
  *out = a / b;
  return true;
}

bool checked_mod_int64(int64_t a, int64_t b, int64_t* out) {
  if (b == 0) return false;
  if (a == INT64_MIN && b == -1) {
    *out = 0;
    return true;
  }
  *out = a % b;
  return true;
}

bool checked_shift_count(int64_t shift) {
  return shift >= 0 && shift < 64;
}

bool checked_shl_int64(int64_t a, int64_t shift, int64_t* out) {
  if (!checked_shift_count(shift)) return false;
  uint64_t bits = static_cast<uint64_t>(a);
  bits = low_bits(bits << static_cast<unsigned>(shift), 64);
  *out = static_cast<int64_t>(bits);
  return true;
}

bool checked_shr_int64(int64_t a, int64_t shift, int64_t* out) {
  if (!checked_shift_count(shift)) return false;
  uint64_t bits = static_cast<uint64_t>(a);
  bits >>= static_cast<unsigned>(shift);
  *out = static_cast<int64_t>(bits);
  return true;
}

bool tyname_is(const ast::TypeExpr* t, std::string_view expected) {
  return t && t->kind == ast::Kind::TyName &&
         ascii_lower(static_cast<const ast::TyName&>(*t).name) == expected;
}

}  // namespace tp2cc
