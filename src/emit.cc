#include "emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diag.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

std::string char_literal_body_to_cxx(char c) {
  std::string o;
  switch (c) {
    case '\\': o += "\\\\"; return o;
    case '\n': o += "\\n"; return o;
    case '\r': o += "\\r"; return o;
    case '\t': o += "\\t"; return o;
    case '\0': o += "\\0"; return o;
    default: break;
  }
  if (c == '\'') { o += "\\'"; return o; }
  if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7f) {
    char esc[8];
    std::snprintf(esc, sizeof(esc), "\\x%02x", (unsigned char)c);
    o += esc;
    return o;
  }
  o.push_back(c);
  return o;
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

// ---------------------------------------------------------------------------
// Name mangling

// Identifiers already start with `p_` in the output. Pascal built-in type
// names map directly to C++ types below without the prefix.
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

// Pascal class methods can be overloaded; the registry stores them as a
// vector per name. Most consumers want one representative MethodSig --
// either the first overload (for kind/decl/virtual queries that overloads
// share) or any overload that happens to be there. This helper centralises
// the "skip empty / take front" boilerplate so iteration sites do not have
// to redo it.
const tp2cc::MethodSig* representative_method(
    const std::vector<tp2cc::MethodSig>& sigs) {
  return sigs.empty() ? nullptr : &sigs.front();
}

// Some emit paths already know the exact C++ carrier type they need,
// for example `tp2cc_OpenArray<T>` instead of the generic array-to-pointer
// decay used elsewhere. In those cases we must only attach the name and
// any `&` / `const &` declarator text here, not re-derive the type from
// the Pascal AST and lose the chosen ABI.
std::string attach_named_cxx_type(std::string_view ty, std::string_view name,
                                  std::string_view name_prefix) {
  if (name_prefix == "const &") {
    std::string out = "const " + std::string(ty) + "&";
    if (!name.empty()) out += " " + std::string(name);
    return out;
  }
  if (name.empty()) {
    return name_prefix.empty() ? std::string(ty)
                               : std::string(ty) + " " +
                                     std::string(name_prefix);
  }
  return std::string(ty) + " " + std::string(name_prefix) +
         std::string(name);
}

constexpr const char* kUnitInitName = "tp2cc_unit_init";
constexpr const char* kUnitFiniName = "tp2cc_unit_fini";
constexpr const char* kPascalResultSlotName = "p_result";
constexpr const char* kCtorStatusSlotName = "tp2cc_ctor_ok";

std::string nested_result_slot_name(std::string_view fn_name) {
  return "tp2cc_result_" + mangle(fn_name);
}

bool is_pascal_result_ident(std::string_view name) {
  return ascii_lower(name) == "result";
}

std::string encode_helper_ident(std::string_view name) {
  std::string out;
  if (name.empty()) return "empty";
  for (char ch : name) {
    if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      out.push_back(ch);
    } else if (ch == '_') {
      out += "_u";
    } else {
      out += "x";
      constexpr char kHex[] = "0123456789abcdef";
      out.push_back(kHex[(static_cast<unsigned char>(ch) >> 4) & 0xF]);
      out.push_back(kHex[static_cast<unsigned char>(ch) & 0xF]);
    }
  }
  return out;
}

std::string encode_helper_type(const TypeExpr& t);

std::string encode_helper_param_mode(Param::Mode mode) {
  switch (mode) {
    case Param::Value: return "value";
    case Param::Var: return "var";
    case Param::Const: return "const";
    case Param::Out: return "out";
  }
  return "value";
}

std::string encode_helper_params(const std::vector<Param>& params) {
  if (params.empty()) return "noargs";
  std::string out;
  for (const auto& param : params) {
    size_t repeats = param.names.empty() ? 1 : param.names.size();
    std::string type_code =
        param.type ? encode_helper_type(*param.type) : std::string("untyped");
    for (size_t i = 0; i < repeats; ++i) {
      if (!out.empty()) out += "_";
      out += encode_helper_param_mode(param.mode);
      out += "_";
      out += type_code;
    }
  }
  return out;
}

std::string encode_helper_type(const TypeExpr& t) {
  switch (t.kind) {
    case Kind::TyName:
      return "name_" + encode_helper_ident(static_cast<const TyName&>(t).name);
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      std::string out;
      switch (a.array_kind) {
        case ArrayKind::Open:
          out = "openarr";
          break;
        case ArrayKind::Dynamic:
          out = "dynarr";
          break;
        case ArrayKind::Fixed:
          out = "arr";
          out += std::to_string(a.dims.size());
          break;
      }
      out += "_";
      out += a.element ? encode_helper_type(*a.element)
                       : std::string("void");
      return out;
    }
    case Kind::TyRecord:
      return "record";
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      return o.is_reference_type ? "class" : "object";
    }
    case Kind::TySet:
      return "set_" + encode_helper_type(*static_cast<const TySet&>(t).element);
    case Kind::TyFile: {
      const auto& f = static_cast<const TyFile&>(t);
      if (f.is_text) return "text";
      return f.element ? "file_" + encode_helper_type(*f.element)
                       : std::string("file_untyped");
    }
    case Kind::TyPointer: {
      const auto& p = static_cast<const TyPointer&>(t);
      return p.target ? "ptr_" + encode_helper_type(*p.target)
                      : std::string("ptr_void");
    }
    case Kind::TyProcedural: {
      const auto& p = static_cast<const TyProcedural&>(t);
      std::string out = p.is_method ? "method" : "proc";
      out += p.is_function ? "_fn_" : "_proc_";
      out += encode_helper_params(p.params);
      out += "_ret_";
      out += (p.is_function && p.return_type)
                 ? encode_helper_type(*p.return_type)
                 : std::string("void");
      return out;
    }
    case Kind::TyEnum:
      return "enum";
    case Kind::TySubrange:
      return "subrange";
    case Kind::TyString: {
      const auto& s = static_cast<const TyString&>(t);
      return s.max_length ? "string_sized" : std::string("string");
    }
    case Kind::TyMetaclass:
      return "metaclass_" +
             encode_helper_ident(static_cast<const TyMetaclass&>(t).class_name);
    case Kind::TyDistinct:
      return "distinct_" +
             encode_helper_type(*static_cast<const TyDistinct&>(t).underlying);
    default:
      return "type";
  }
}

std::string enum_bound_name(std::string_view type_name, std::string_view which) {
  return "tp2cc_enum_" + std::string(which) + "_" +
         encode_helper_ident(type_name);
}

bool tyname_is(const TypeExpr* t, std::string_view expected) {
  return t && t->kind == Kind::TyName &&
         ascii_lower(static_cast<const TyName&>(*t).name) == expected;
}

void mark_builtin_memory_helper_param_info(
    std::string_view name, std::vector<bool>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const ast::TypeExpr*>& param_types) {
  const std::string lower = ascii_lower(name);

  auto mark = [&](size_t index, bool is_untyped, bool is_mutable,
                  const ast::TypeExpr* type = nullptr) {
    if (index < untyped_arg.size() && is_untyped) untyped_arg[index] = true;
    if (index < mutable_ref_arg.size() && is_mutable) mutable_ref_arg[index] = true;
    if (index < param_types.size()) param_types[index] = type;
  };

  // Pascal's raw memory helpers all operate on caller storage, not on the
  // value of the first expression. Reuse the normal untyped-argument
  // lowering path here so calls like `FillChar(FList^[I], ...)` become
  // `&slot` in C++ instead of reinterpreting the pointer value stored there.
  if (lower == "fillchar" || lower == "fillword") {
    mark(0, /*is_untyped=*/true, /*is_mutable=*/true);
    return;
  }
  if (lower == "move") {
    mark(0, /*is_untyped=*/true, /*is_mutable=*/false);
    mark(1, /*is_untyped=*/true, /*is_mutable=*/true);
    return;
  }
  if (lower == "getmem" || lower == "freemem" || lower == "reallocmem" ||
      lower == "dispose" || lower == "strdispose") {
    mark(0, /*is_untyped=*/false, /*is_mutable=*/true);
  }
}

enum class PrimitiveIntKind : uint8_t { None, Signed, Unsigned };

// Pascal primitive-type table. Single source of truth for every
// property callers need: the C++ spelling plus, for integer
// primitives, the signedness and width used when lowering untyped
// integer literals into a typed context.
struct PrimitiveInfo {
  const char* cxx;
  PrimitiveIntKind int_kind;
  uint8_t bits;
};

const std::unordered_map<std::string, PrimitiveInfo>& primitive_type_map() {
  static const std::unordered_map<std::string, PrimitiveInfo> m = {
      {"integer",     {"int32_t",         PrimitiveIntKind::Signed,   32}},
      {"longint",     {"int32_t",         PrimitiveIntKind::Signed,   32}},
      {"cardinal",    {"uint32_t",        PrimitiveIntKind::Unsigned, 32}},
      {"longword",    {"uint32_t",        PrimitiveIntKind::Unsigned, 32}},
      {"smallint",    {"int16_t",         PrimitiveIntKind::Signed,   16}},
      {"word",        {"uint16_t",        PrimitiveIntKind::Unsigned, 16}},
      {"shortint",    {"int8_t",          PrimitiveIntKind::Signed,    8}},
      {"byte",        {"uint8_t",         PrimitiveIntKind::Unsigned,  8}},
      {"char",        {"::rt::p_char",    PrimitiveIntKind::None,      0}},
      // FPC's WideChar is still a 16-bit ordinal code unit in 2.0.x.
      // Keep it as a plain 16-bit value type here; the compiler-side
      // bootstrap helper in compiler/widestr.pas handles wide string
      // storage/decoding separately.
      {"widechar",    {"uint16_t",        PrimitiveIntKind::Unsigned, 16}},
      {"boolean",     {"bool",            PrimitiveIntKind::None,      0}},
      {"bytebool",    {"uint8_t",         PrimitiveIntKind::Unsigned,  8}},
      {"wordbool",    {"uint16_t",        PrimitiveIntKind::Unsigned, 16}},
      {"longbool",    {"uint32_t",        PrimitiveIntKind::Unsigned, 32}},
      {"single",      {"float",           PrimitiveIntKind::None,      0}},
      {"double",      {"double",          PrimitiveIntKind::None,      0}},
      {"real",        {"double",          PrimitiveIntKind::None,      0}},
      {"extended",    {"long double",     PrimitiveIntKind::None,      0}},
      {"comp",        {"long double",     PrimitiveIntKind::None,      0}},
      {"pointer",     {"void*",           PrimitiveIntKind::None,      0}},
      {"pchar",       {"::rt::p_char*",   PrimitiveIntKind::None,      0}},
      {"ppchar",      {"::rt::p_char**",  PrimitiveIntKind::None,      0}},
      {"text",        {"::rt::tp2cc_TextFile",  PrimitiveIntKind::None,      0}},
      {"int64",       {"int64_t",         PrimitiveIntKind::Signed,   64}},
      {"qword",       {"uint64_t",        PrimitiveIntKind::Unsigned, 64}},
      {"dword",       {"uint32_t",        PrimitiveIntKind::Unsigned, 32}},
      {"string",      {"::rt::tp2cc_ShortString<>", PrimitiveIntKind::None,  0}},
      {"shortstring", {"::rt::tp2cc_ShortString<>", PrimitiveIntKind::None,  0}},
      {"ansistring",  {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None,      0}},
      {"utf8string",  {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None,      0}},
  };
  return m;
}

const std::unordered_map<std::string, const char*>& runtime_named_type_map() {
  static const std::unordered_map<std::string, const char*> m = {
      {"tunicodechar", "::rt::p_tunicodechar"},
      {"tunicodestring", "::rt::p_tunicodestring"},
      {"tunicodecharmappingflag", "::rt::p_tunicodecharmappingflag"},
      {"tunicodecharmapping", "::rt::p_tunicodecharmapping"},
      {"punicodecharmapping", "::rt::p_punicodecharmapping"},
      {"tunicodemap", "::rt::p_tunicodemap"},
      {"punicodemap", "::rt::p_punicodemap"},
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
  auto it = runtime_named_type_map().find(ascii_lower(std::string(lowname)));
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
  auto it =
      builtin_reference_class_map().find(ascii_lower(std::string(lowname)));
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
  if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
    return false;
  *out = a + b;
  return true;
}

bool checked_sub_int64(int64_t a, int64_t b, int64_t* out) {
  if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
    return false;
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
    } else {
      if (b < INT64_MIN / a) return false;
    }
  } else {
    if (b > 0) {
      if (a < INT64_MIN / b) return false;
    } else {
      if (a != 0 && b < INT64_MAX / a) return false;
    }
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

struct ConstIntExprInfo {
  int64_t value = 0;
  const PrimitiveInfo* type = nullptr;
};

struct ConvertedConstInt {
  int64_t value = 0;
  uint64_t bits = 0;
  const PrimitiveInfo* type = nullptr;
};

// ---------------------------------------------------------------------------
// Emitter state

struct Emitter {
  std::string header;
  std::string impl;
  // Current sink pointer.
  std::string* out = &header;
  int indent_level = 0;
  // Depth of proc bodies we're currently emitting. >0 means block scope,
  // which means C++ `inline` is invalid for local decls.
  int block_depth = 0;

  // Name of the Pascal class whose method body we're currently emitting
  // (if any). Empty when emitting a free function or at namespace scope.
  std::string current_class_name;

  // Name of the Pascal unit we are emitting (lowercased). Used to
  // decide whether a cross-unit reference needs explicit qualification
  // to disambiguate it from a same-named symbol brought in by another
  // `uses` clause.
  std::string current_unit_name;

  // Suppresses the "bare method reference -> append ()" rewrite. tp2cc_Set
  // while emitting (a) the CALLEE of a Call (else `foo(args)` would
  // become `foo()(args)`), and (b) the operand of AddrOf (else `@foo`
  // would become `&foo()`).
  bool is_callee_context_ = false;

  // When emitting an LHS expression, if set, any bare Ident whose name
  // equals this value is rewritten to the Pascal-visible implicit result
  // variable. `Result` is in the Pascal identifier namespace, so it uses
  // the ordinary `p_...` mangling rather than an internal helper name.
  std::string lhs_fn_rewrite;
  std::string lhs_fn_rewrite_slot;
  std::string lhs_outer_result_rewrite;
  std::string lhs_outer_result_rewrite_slot;

  // Names bound in the current function's scope (parameters + locals).
  // `obj` resolved bare at block scope that hits this set must be a
  // variable, so auto-call (`name()`) is suppressed. Prevents false
  // auto-call on local vars whose names happen to coincide with a
  // parameterless method in another unit.
  std::unordered_set<std::string> local_scope;

  // Variable-to-declared-type map for the current scope (parameters +
  // locals). Populated at proc-body entry so expression-type deduction
  // can answer "what class does this variable belong to?" and the
  // Member-access emitter can auto-call only actual methods.
  std::unordered_map<std::string, const ast::TypeExpr*> local_types;

  // Function-local const declarations. Needed so integer constant
  // expressions can fold through local const references instead of
  // falling back to C++'s type rules.
  std::unordered_map<std::string, const ast::ConstDecl*> local_consts;

  // Names of the current scope's parameters that are Pascal's untyped
  // `var X` / `const X` / `X` form. Their C++ type is `void*` (not
  // `void*&`); the callee receives the caller's storage address.
  // `@X` on one of these emits as the ident itself -- no `&`.
  std::unordered_set<std::string> local_untyped_params;

  // Nested procs/functions declared in the current scope. For each
  // name, store the parameter count and whether it returns a value.
  // Used so bare references to a parameterless nested `function`
  // auto-call (the lambda itself is `std::function<T()>`, not a `T`).
  struct NestedFn {
    size_t param_count = 0;
    bool accepts_zero_args = false;
    bool is_function = false;
    const ast::TypeExpr* return_type = nullptr;
    const ast::ProcDecl* decl = nullptr;
  };
  std::unordered_map<std::string, NestedFn> local_nested_fns;
  std::unordered_set<std::string> local_nested_forwards;

  // Function-local enum types: name -> the TyEnum AST node. Pascal
  // lets a `type T = (a, b, c)` and `const X : array[T] of ... = ...`
  // live inside a proc's declaration section. These aren't in the
  // unit-wide TypeRegistry (which only indexes interface/impl top-
  // level decls), so we layer them on here while emitting the proc.
  std::unordered_map<std::string, const ast::TyEnum*> local_enums;
  // `const` parameters stay read-only storage. An `absolute` alias over one
  // must therefore bind a `const T&`, not a mutable `T&`, or C++ rejects the
  // alias and Pascal source that only reads through it stops compiling.
  std::unordered_set<std::string> local_const_params;
  // Function-local type aliases: `type pi = ^integer;` style.
  std::unordered_map<std::string, const ast::TypeExpr*>
      local_type_aliases_scoped;

  // `with X do` bindings: for every `with target`, push the target's
  // expression text (already emitted) and its deduced type. Bare idents
  // inside the body that resolve as fields of one of the targets get
  // rewritten to `target.name`. For auto-call decisions on bare idents,
  // consult these types.
  struct WithBind {
    std::string cxx_text;
    const ast::TypeExpr* type = nullptr;
    std::string class_name;
    std::string access_op;
  };
  std::vector<WithBind> with_stack;

  struct MetaclassCallable {
    std::string name;
    const MethodSig* sig = nullptr;
    bool implicit_root_create = false;
  };
  struct MetaclassCallableImpl {
    std::string owner_class;
    const MethodSig* sig = nullptr;
    bool implicit_root_create = false;
  };

  // Reified type/symbol tree spanning all parsed units. tp2cc_Set by the
  // driver. Drives member-access and ident-call decisions.
  const TypeRegistry* registry = nullptr;

  // Ordered unit names whose lifecycle hooks must run before the
  // program's `begin..end.` body. tp2cc_Set by the driver only when
  // emitting the `program` unit.
  const std::vector<std::string>* unit_init_order = nullptr;

  void set_header() { out = &header; }
  void set_impl()   { out = &impl; }

  void emit(std::string_view s) { out->append(s); }
  void emitln(std::string_view s) {
    for (int i = 0; i < indent_level; ++i) out->append("  ");
    out->append(s);
    out->push_back('\n');
  }
  void nl() { out->push_back('\n'); }
  void indent() { ++indent_level; }
  void dedent() { if (indent_level > 0) --indent_level; }

  // Top-level drivers.
  void emit_unit(const UnitNode& u);
  void emit_decl(const Decl& d, bool in_header);
  void emit_const_decl(const ConstDecl& cd, bool in_header);
  void emit_type_decl(const TypeDecl& td, bool in_header);
  void emit_var_decl(const VarDecl& vd, bool in_header);
  void emit_proc_decl_signature(const ProcDecl& pd);

  // Types -> C++ type string.
  std::string type_to_cxx(const TypeExpr& t);
  std::string type_name_to_cxx(const TyName& n);
  std::string type_name_text_to_cxx(std::string_view name);
  std::string named_type_struct_cxx(std::string_view name);
  std::string visible_type_prefix(std::string_view name);
  std::string metaclass_struct_cxx(std::string_view class_name);
  std::string metaclass_value_fn_cxx(std::string_view class_name);
  std::vector<MetaclassCallable> collect_metaclass_callables(
      std::string_view class_name);
  std::optional<MetaclassCallableImpl> find_metaclass_callable_impl(
      std::string_view concrete_class, const MetaclassCallable& target);
  std::string array_type_to_cxx(const TyArray& a);
  const ast::TypeExpr* canonicalize_type(const ast::TypeExpr* t);
  bool enum_has_explicit_values(const TyEnum& e);
  std::optional<int64_t> enum_member_value_int64(const TyEnum& e,
                                                 size_t index);
  std::string enum_member_value_to_cxx(const TyEnum& e, size_t index);
  std::string enum_underlying_type_to_cxx(const TyEnum& e);
  bool array_dim_bounds_to_cxx(const ast::TypeExpr& dim,
                               std::string* lo,
                               std::string* size_expr);
  std::string set_type_to_cxx(const TySet& s);
  std::string enum_type_to_cxx(const TyEnum& e, const std::string& context);
  std::string subrange_type_to_cxx(const TySubrange& r);
  std::string string_type_to_cxx(const TyString& s);
  std::optional<std::string> shortstring_capacity_to_cxx(
      const TypeExpr* t);
  std::string shortstring_value_to_cxx(std::string text,
                                       const TypeExpr* target);
  std::string pointer_type_to_cxx(const TyPointer& p);
  std::string procedural_type_to_cxx(const TyProcedural& p);
  std::string procedural_param_types_to_cxx(const std::vector<Param>& params);
  // Per-field decl info used by both the inline and named record-emission
  // paths. `type_cxx` is the resolved C++ type spelling (kept separately
  // because the named path also needs it for `sizeof(...)` offset
  // tracking); `decl` is `type_cxx + " " + mangled_name`, the form a
  // C++ struct body wants for the field declaration.
  struct RecordFieldDecl {
    const ast::TypeExpr* type;     // raw field type, for offset/packed checks
    std::string type_cxx;          // resolved C++ type spelling
    std::string mangled_name;      // `p_<pascal_name>`
    std::string decl;              // `<type_cxx> <mangled_name>`
  };
  std::vector<RecordFieldDecl> record_field_decls(
      const std::vector<ast::RecordField>& fields);

  // Layout description for a packed record: per-field offset expressions
  // (in declaration order, with mangled field names) plus the total size
  // expression. Used by both the named type-decl path (asserts via
  // `offsetof(<typename>, ...)`) and the variable-decl path (asserts via
  // `offsetof(decltype(<varname>), ...)` for inline anonymous records).
  struct PackedRecordLayout {
    std::vector<std::pair<std::string, std::string>> field_offsets;
    std::string size_expr;
  };
  PackedRecordLayout compute_packed_record_layout(const ast::TyRecord& tr);
  // Emit the offsetof / sizeof static_asserts for a packed record. The
  // type expression `type_text` is whatever names the record at the emit
  // site -- a typedef name for the named-decl path, `decltype(varname)`
  // for an inline anonymous record bound to a variable.
  void emit_packed_record_asserts(const std::string& type_text,
                                  const PackedRecordLayout& layout,
                                  std::string_view label);

  std::string named_type_to_cxx(const TypeExpr* t, std::string_view name,
                                std::string_view name_prefix = {});
  std::string method_pointer_helper_name(const ast::ProcDecl& pd);
  std::string low_high_expr_for_named_type(std::string_view name,
                                           bool want_low);
  std::string low_high_expr_for_type(const ast::TypeExpr* t,
                                     bool want_low);

  // Expressions -> C++ expression.
  std::string expr_to_cxx(const Expr& e);
  // When a target type is provided, rewrites integer constant
  // expressions to the exact value the destination type would hold,
  // and descends into ArrayConst / RecordConst with the per-element /
  // per-field type.
  std::string const_value_to_cxx(const Expr& e,
                                 const TypeExpr* target = nullptr,
                                 bool explicit_conversion = false);
  std::string set_literal_to_cxx(const SetLit& s,
                                 const TypeExpr* target = nullptr);
  // If `e` is an integer constant expression and `target` is an
  // integer primitive, return the exact destination value as a C++
  // literal. Implicit conversions diagnose range errors; explicit
  // Pascal casts do not.
  std::optional<std::string> maybe_convert_const_int_expr(
      const Expr& e, const TypeExpr* target, bool explicit_conversion);
  std::optional<std::string> maybe_convert_proc_value(
      const Expr& e, const TypeExpr* target);
  std::optional<std::string> maybe_lower_metaclass_value(
      const Expr& e, const TypeExpr* target);

  // Small helpers.
  bool const_param_needs_mutable_ref(const ast::TypeExpr* t);
  bool const_param_needs_const_ref(const ast::TypeExpr* t);
  const ClassInfo* class_info_for_type_name(std::string_view name);
  const ast::TypeExpr* lookup_named_type_expr(std::string_view name);
  bool is_builtin_reference_class_name(std::string_view name) const;
  std::string metaclass_target_name(const ast::TypeExpr* t);
  bool type_is_value_object(const ast::TypeExpr* t);
  std::string primitive_cast_lvalue_ref(const ast::Call& c);
  std::string primitive_cast_untyped_storage_ptr(const ast::Call& c);
  std::string primitive_cast_packed_field_ptr(const ast::Call& c);
  // Carries the result of `packed_field_storage_ref`. `void_ptr_text` is
  // an `&(field_expr)` snippet -- safe to consume only via the memcpy-based
  // runtime helpers (`tp2cc_reinterpret_load` / `_store` / `_inc` / `_dec`).
  // Going through `*reinterpret_cast<T*>(p)` instead would re-introduce the
  // misaligned-`T*`-deref UB the existing `[[gnu::packed]]` emit deliberately
  // makes the compiler complain about. `elem_cxx` is the C++ type to use as
  // the load/store operand at the call site.
  struct PackedFieldStorage {
    std::string void_ptr_text;
    std::string elem_cxx;
  };
  std::optional<PackedFieldStorage> packed_field_storage_ref(
      const ast::Expr& e);
  struct UntypedStorageIndexView {
    std::string elem_cxx;
    std::string ptr_cxx;
  };
  std::optional<UntypedStorageIndexView> untyped_storage_index_view(
      const ast::Index& i);
  std::string param_list_to_cxx(const std::vector<Param>& params);
  std::string proc_return_type_to_cxx(const ProcDecl& pd);
  void emit_method_pointer_thunk(const std::string& owner_name,
                                 const ast::ProcDecl& pd,
                                 const std::string& ret);
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_raise_stmt(const ast::Raise& r);
  void emit_try_stmt(const ast::Try& t);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'

  // Expression-type deduction. Returns the Pascal TypeExpr that the
  // expression has, or nullptr when unknown. Consults the TypeRegistry
  // for globals and the current scope tables for locals/self-class.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);
  const ast::TypeExpr* deduce_const_decl_type(const ast::ConstDecl& cd);
  const ast::TypeExpr* deduce_const_info_type(const ConstInfo& c);
  std::optional<ConstIntExprInfo> eval_const_int_expr(
      const ast::Expr& e,
      std::unordered_set<std::string>* visiting_const_names = nullptr);
  std::optional<ConstIntExprInfo> eval_const_int_cast(
      const ast::Call& c,
      std::unordered_set<std::string>* visiting_const_names);
  std::optional<ConvertedConstInt> convert_const_int_value(
      Location where, int64_t value, const ast::TypeExpr* target,
      bool explicit_conversion, bool diagnose);

  // `with` targets can be anonymous local records, so name-based
  // registry lookup is not enough. These helpers walk the stacked
  // bound type itself and recover the member type/text directly.
  const ast::TypeExpr* lookup_record_field_type_in_type(
      const ast::TypeExpr* type, std::string_view field_name);
  const ast::TypeExpr* lookup_record_field_type_in_with(
      const WithBind& wb, std::string_view field_name);
  bool with_bind_has_visible_member(const WithBind& wb, std::string_view name);
  struct PackedAggregateFieldUse {
    std::string record_name;
    std::string field_name;
  };
  bool type_is_packed_record(const ast::TypeExpr* t);
  bool type_is_direct_packed_aggregate(const ast::TypeExpr* t);
  bool type_is_byte_aligned_packed_index_carrier(const ast::TypeExpr* t);
  std::optional<PackedAggregateFieldUse> direct_packed_aggregate_field_use(
      const ast::Expr& e);
  void report_packed_aggregate_subobject_use(
      Location where, std::string_view op,
      const PackedAggregateFieldUse& use);

  // Class/record alias name ("tfoo") of `e`, lowercased, if detectable.
  // Empty if the type can't be narrowed to a named object/record type.
  std::string deduce_class_alias(const ast::Expr& e);
  const ast::Expr* peel_primitive_casts(const ast::Expr* e);
  bool expr_is_storage_lvalue(const ast::Expr& e);
  bool expr_is_untyped_storage_ref(const ast::Expr& e);
  bool expr_is_charish(const ast::Expr& e);
  bool type_is_pcharish(const ast::TypeExpr* t);
  bool type_is_metaclass(const ast::TypeExpr* t);
  bool type_is_reference_class(const ast::TypeExpr* t);
  bool expr_is_reference_class(const ast::Expr& e);
  std::string member_access_op(const ast::TypeExpr* t);
  std::string member_access_op(const ast::Expr& e);
  bool type_is_stringish(const ast::TypeExpr* t);
  bool type_is_pointerish(const ast::TypeExpr* t);
  bool type_is_open_array(const ast::TypeExpr* t);
  std::string open_array_type_to_cxx(const ast::TypeExpr& t);
  std::string open_array_constructor_to_cxx(const SetLit& s,
                                            const TypeExpr& param_type);
  std::string reinterpret_ref_text(const std::string& ty_cxx,
                                   const std::string& source_cxx,
                                   bool pointee_view);
  const VarInfo* find_visible_unit_var(const std::string& name);
  const ConstInfo* find_visible_unit_const(const std::string& name);
  const EnumInfoReg* find_visible_enum_info_for_member(const std::string& name);
  struct AbsoluteTargetInfo {
    std::string cxx;
    const ast::TypeExpr* type = nullptr;
    bool is_pointerish = false;
    bool is_const_storage = false;
  };
  std::optional<AbsoluteTargetInfo> resolve_absolute_target(const ast::VarDecl& vd);
  struct FlatCallParamInfo {
    const ast::TypeExpr* type = nullptr;
    bool untyped = false;
    bool mutable_ref = false;
    const ast::Expr* default_value = nullptr;
  };
  bool proc_accepts_zero_args(const ast::ProcDecl& decl);
  const ast::ProcDecl* resolve_call_decl(const ast::Expr& callee);

  // Pascal/FPC overload-resolution conversion ranks. Lower = better. See
  // the rank table in `rank_conversion`. `NotViable` means no conversion
  // exists (or only an explicit one); the candidate is filtered out.
  enum class ConvRank : uint8_t {
    Exact = 1,             // identity after canonicalization
    Equal = 2,             // equal-modulo-distinct/subrange (same underlying)
    ClassHierarchy = 3,    // descendant -> ancestor class
    IntWideningSameSign = 4,  // byte->word->longint, etc.
    RealWidening = 5,      // single->double->extended
    StringSameTagWiden = 6,   // ShortString<N> -> ShortString<M>, M >= N
    // Cross-tag string conversions split by target family because Pascal's
    // tiebreaker prefers ShortString-typed params over AnsiString-typed
    // params when both are viable for the same source (the compiler
    // bootstrap runs under `{$H-}` semantics: `string` aliases ShortString).
    StringToShortString = 7,  // Char/PChar/AnsiString -> ShortString
    StringToAnsiString  = 8,  // Char/PChar/ShortString -> AnsiString; ShortString/AnsiString -> PChar
    OrdinalSignChange   = 9,  // longint <-> longword
    Variant            = 10,  // any -> variant or variant -> any
    NotViable = 255,
  };
  // Conversion score: the major rank above plus a tie-breaking
  // `distance`. Within a single rank, Pascal prefers the candidate
  // whose target type is the closest fit to the source -- e.g. a
  // `byte` argument prefers `tostr(cardinal)` over `tostr(qword)`
  // even though both are `IntWideningSameSign`. `distance` encodes
  // that: smaller distance wins. 0 means "no distance applies"
  // (e.g. Exact, Equal). The picker compares (rank, distance)
  // lexicographically; without this, equal-rank widenings tie and
  // the call gets flagged ambiguous.
  struct ConvScore {
    ConvRank rank = ConvRank::NotViable;
    int distance = 0;
    bool viable() const { return rank != ConvRank::NotViable; }
  };
  ConvScore rank_conversion(const ast::TypeExpr* arg,
                            const ast::TypeExpr* param,
                            bool var_param);
  // Picks the Pascal-best ProcDecl from a list of candidates given the
  // call-site argument expressions. Used by both free-function overload
  // sets (built from `ProcInfo::decl`) and class-method overload sets
  // (built from `MethodSig::decl`); the picker only needs the decls.
  // Result of overload picking. `ambiguous` distinguishes "no candidate
  // dominates" (Pascal-level error -- caller must diagnose) from
  // "no candidate is viable at all" (decl null, ambiguous false; caller
  // can fall back to single-decl resolution).
  struct PickResult {
    const ast::ProcDecl* decl = nullptr;
    bool ambiguous = false;
  };
  PickResult pick_overload(
      const std::vector<const ast::ProcDecl*>& candidates,
      const std::vector<const ast::Expr*>& args);
  // Single entry point for resolving a Pascal call expression. Returns
  // both the picked decl AND the information needed to emit the C++ callee
  // text. `format_resolved_callee` is the only place the call branch
  // turns this into text -- the call branch never calls `expr_to_cxx`
  // directly on the callee, so resolution and emitted text cannot disagree.
  //
  // `callee_kind` records whether the callee needs special C++ output.
  // `FreeFunctionInUnit` is
  // the only case that overrides default expression
  // formatting; otherwise
  // the callee text comes from `expr_to_cxx(callee)`.
  // (This refactor is deliberately scoped to free-function output --
  // class-method/instance-receiver spelling still flows through the
  // existing expr_to_cxx logic. Per-overload mangling would let us
  // drop the per-arg `static_cast` workaround entirely; that is a
  // larger follow-up, intentionally not done here.)
  enum class ResolvedCalleeKind {
    Unknown,             // emitter spells callee via expr_to_cxx(callee)
    FreeFunctionInUnit,  // emitter spells callee as <unit_ns>::<mangled_name>
  };
  struct ResolvedCall {
    const ast::ProcDecl* decl = nullptr;
    ResolvedCalleeKind shape = ResolvedCalleeKind::Unknown;
    // For `FreeFunctionInUnit`: the unit that owns `decl`. Drives the
    // namespace prefix in `format_resolved_callee`.
    std::string defining_unit;
    // Pascal-side member/proc name (unmangled, lowercased). Used by
    // `format_resolved_callee` for `FreeFunctionInUnit`.
    std::string member_name;
    // True iff the resolver had to pick among multiple arity-viable
    // candidates by type-rank scoring. The call site wraps each value
    // arg in `static_cast<param_type>(...)` so C++ overload resolution
    // lands on the same overload Pascal picked. When arity alone
    // narrows to one candidate, casts are not needed.
    bool needs_arg_casts = false;
    // Set when two or more arity-viable candidates were mutually
    // incomparable on type-rank. The call site reports a Pascal-level
    // "ambiguous call" error and emits `decl=null`. We do not silently
    // pick one -- silent pick has been a real source of miscompiles.
    bool ambiguous = false;
  };
  ResolvedCall resolve_call(
      const ast::Expr& callee, const std::vector<const ast::Expr*>& args);
  std::string format_resolved_callee(const ResolvedCall& resolved,
                                     const ast::Expr& callee_ast);
  void flatten_call_param_info(const ast::ProcDecl* decl,
                               std::vector<FlatCallParamInfo>& flat_params);
  void append_defaulted_trailing_call_args(
      const ast::ProcDecl* decl, std::vector<const ast::Expr*>& args);
  void mark_call_param_info(const ast::ProcDecl* decl,
                            std::vector<bool>& untyped_arg,
                            std::vector<bool>& mutable_ref_arg,
                            std::vector<const ast::TypeExpr*>& param_types);
  void collect_call_param_info(const ast::Expr& callee,
                               std::vector<bool>& untyped_arg,
                               std::vector<bool>& mutable_ref_arg,
                               std::vector<const ast::TypeExpr*>& param_types);
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             bool untyped_arg,
                             bool mutable_ref_arg);
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl);
  std::string lower_property_read(Location where,
                                  const std::string& base_cxx,
                                  const std::string& class_name,
                                  const PropertyInfo& prop,
                                  const std::vector<const ast::Expr*>& indices);
  std::string lower_property_write(Location where,
                                   const std::string& base_cxx,
                                   const std::string& class_name,
                                   const PropertyInfo& prop,
                                   const std::vector<const ast::Expr*>& indices,
                                   const ast::Expr& value);
  std::optional<std::string> maybe_property_read_text(
      const std::string& base_cxx,
      const std::string& class_name,
      const PropertyInfo& prop,
      const std::vector<const ast::Expr*>& indices);
  std::optional<std::string> maybe_property_write_text(
      const std::string& base_cxx,
      const std::string& class_name,
      const PropertyInfo& prop,
      const std::vector<const ast::Expr*>& indices,
      const ast::Expr& value);
  std::string implicit_self_cxx();
  struct ImplicitPropertyLookup {
    const PropertyInfo* prop = nullptr;
    std::string class_name;
    std::string base_cxx;
    bool from_with = false;
  };
  std::optional<ImplicitPropertyLookup> find_implicit_class_property(
      std::string_view name);
  std::optional<std::string> maybe_lower_implicit_property_write(
      Location where,
      std::string_view name,
      const ast::Expr& value);
  std::optional<std::string> maybe_lower_class_free_member(
      const ast::Expr& base, std::string_view member_name);
  std::optional<std::string> maybe_lower_class_constructor_call(
      std::string_view class_name, std::string_view member_name,
      const std::vector<const ast::Expr*>& args,
      const std::vector<const ast::TypeExpr*>& param_types,
      const std::vector<bool>& untyped_arg,
      const std::vector<bool>& mutable_ref_arg);

  // ---------------------------------------------------------------------
  // Pascal name resolution (one function, every emit path goes through
  // it). Given a name and optional qualifier, model the full Pascal
  // lookup:
  //   - unqualified: `with` -> locals -> enclosing nested fns ->
  //                  class+ancestors (in method body) -> current unit ->
  //                  `uses` chain -> rt:: builtins.
  //   - `Unit.name`: symbols exported by `Unit` (which must be in the
  //                  current unit's `uses` list).
  //   - `Class.name` / `obj.name`: class's members walking ancestors.
  //
  // The resolved result tells the emitter:
  //   - how to spell the access in C++,
  //   - whether it's a parameterless callable (value context -> auto-call),
  //   - the ProcDecl* for call-site untyped-var arg wrapping,
  //   - whether it's a field/var/const/enum-member (never auto-call).
  enum class ResolvedKind {
    Unknown,          // emit the mangled name; let C++ lookup sort it out
    ResultSlot,       // Pascal fn's name-as-read inside its own body
    Local,            // param/local/typed-const/nested-fn-name
    NestedFn,         // a parameterless nested function value
    WithField,        // field under a `with X do` binding
    WithMethod,       // method under a `with X do` binding
    WithProperty,     // property under a `with X do` binding
    ClassField,       // member of current class (or ancestor)
    ClassMethod,      // method of current class (or ancestor)
    ClassProperty,    // property of current class (or ancestor)
    UnitVar,
    UnitConst,
    UnitProc,
    UnitType,
    EnumMember,
    RtBuiltin,
  };
  struct ResolveResult {
    ResolvedKind kind = ResolvedKind::Unknown;
    std::string cxx;              // the full C++ expression text
    bool is_parameterless = false;
    bool is_callable = false;
    const ast::ProcDecl* proc = nullptr;   // for call-site analysis
    bool accepts_zero_args = false;
    std::string return_type_name;
  };
  std::optional<ResolveResult> maybe_resolve_implicit_property(
      std::string_view name);
  // Qualifier: empty means unqualified lookup.  Otherwise it's a
  // unit name or a class/record alias name (both lowercased).
  enum class QualifierKind { None, Unit, Class };
  ResolveResult resolve_name(const std::string& name,
                             QualifierKind qk = QualifierKind::None,
                             const std::string& qualifier = {});

  // State: the Pascal identifier of the current function whose body we are
  // emitting (not mangled). Used by `exit`/`exit(v)` translation so we
  // know which result slot to fill.
  std::string current_fn_name;
  bool current_fn_is_function = false;
  bool current_fn_is_ctor = false;
  const ast::TypeExpr* current_fn_result_type = nullptr;
  std::string current_result_slot_name;
  // Bare `Result` resolves differently in nested procedures and nested
  // functions. Procedures inherit the nearest enclosing function result;
  // functions get their own `Result` and can reach the outer one only by
  // writing the outer function's Pascal name explicitly.
  std::string bare_result_slot_name;
  const ast::TypeExpr* bare_result_type = nullptr;
  std::string outer_result_name;
  std::string outer_result_slot_name;
  const ast::TypeExpr* outer_result_type = nullptr;
  // Stack of loop-exit labels. Pascal `break` inside a `case` arm must
  // exit the enclosing loop, but C++ `break` inside `switch` exits the
  // switch -- so we emit Pascal `break` as `goto` to a fresh label
  // placed right after each loop. Also used by `continue` -> a separate
  // label placed at the loop's re-test/re-increment point.
  std::vector<std::string> loop_break_labels;
  std::vector<std::string> loop_continue_labels;
  int loop_label_counter = 0;
  int try_stmt_counter = 0;
  int except_handler_depth = 0;

  void emit_tpexcept_unit(const UnitNode& u);
};

// ---------------------------------------------------------------------------
// Types

std::string Emitter::type_name_to_cxx(const TyName& n) {
  if (is_primitive_type(n.name)) return primitive_type_cxx(n.name);
  if (std::string rt = runtime_named_type_cxx(n.name); !rt.empty()) return rt;
  // Delphi/FPC `class` names denote references to heap objects, so
  // plain uses of the type name lower to a pointer type. The raw struct
  // spelling is kept separate in `named_type_struct_cxx()` for places
  // like base-class lists and `TClass.Method`.
  if (std::string cls = builtin_reference_class_struct_cxx(n.name);
      !cls.empty()) {
    return cls + "*";
  }
  if (registry) {
    auto mark_if_class_ref = [&](std::string_view lookup_name,
                                 std::string_view defining_unit) -> bool {
      auto cit = registry->classes.find(std::string(lookup_name));
      return cit != registry->classes.end() && cit->second.is_reference_type &&
             (defining_unit.empty() || cit->second.defining_unit == defining_unit);
    };
    auto dot = n.name.find('.');
    if (dot != std::string::npos) {
      std::string_view unit = std::string_view(n.name).substr(0, dot);
      std::string_view tail = std::string_view(n.name).substr(dot + 1);
      if (mark_if_class_ref(tail, unit)) {
        return named_type_struct_cxx(n.name) + "*";
      }
    } else if (mark_if_class_ref(n.name, {})) {
      return named_type_struct_cxx(n.name) + "*";
    }
  }
  return named_type_struct_cxx(n.name);
}

std::string Emitter::type_name_text_to_cxx(std::string_view name) {
  TyName t;
  t.name = std::string(name);
  return type_name_to_cxx(t);
}

std::string Emitter::named_type_struct_cxx(std::string_view name) {
  if (std::string rt = runtime_named_type_cxx(ascii_lower(std::string(name)));
      !rt.empty()) {
    return rt;
  }
  if (std::string cls =
          builtin_reference_class_struct_cxx(ascii_lower(std::string(name)));
      !cls.empty()) {
    return cls;
  }
  // Qualified name `unitname.Type` -> `p_unitname::p_type`
  auto dot = name.find('.');
  if (dot != std::string_view::npos) {
    return unit_namespace_prefix(name.substr(0, dot)) +
           mangle(name.substr(dot + 1));
  }
  return visible_type_prefix(name) + mangle(name);
}

std::string Emitter::visible_type_prefix(std::string_view name) {
  if (!registry) return {};

  std::string lower = ascii_lower(std::string(name));
  if (local_type_aliases_scoped.count(lower) || local_enums.count(lower)) {
    return {};
  }

  auto cur = registry->units.find(current_unit_name);
  if (cur != registry->units.end()) {
    if (cur->second.has_type(lower)) return {};
    for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
      auto uit = registry->units.find(*it);
      if (uit == registry->units.end()) continue;
      if (uit->second.has_export_type(lower)) {
        return unit_namespace_prefix(*it);
      }
    }
  }

  auto class_it = registry->classes.find(lower);
  if (class_it != registry->classes.end() &&
      !class_it->second.defining_unit.empty() &&
      class_it->second.defining_unit != current_unit_name) {
    return unit_namespace_prefix(class_it->second.defining_unit);
  }
  auto record_it = registry->records.find(lower);
  if (record_it != registry->records.end() &&
      !record_it->second.defining_unit.empty() &&
      record_it->second.defining_unit != current_unit_name) {
    return unit_namespace_prefix(record_it->second.defining_unit);
  }
  auto enum_it = registry->enums.find(lower);
  if (enum_it != registry->enums.end() &&
      !enum_it->second.defining_unit.empty() &&
      enum_it->second.defining_unit != current_unit_name) {
    return unit_namespace_prefix(enum_it->second.defining_unit);
  }
  auto alias_it = registry->aliases.find(lower);
  if (alias_it != registry->aliases.end() &&
      !alias_it->second.defining_unit.empty() &&
      alias_it->second.defining_unit != current_unit_name) {
    return unit_namespace_prefix(alias_it->second.defining_unit);
  }

  return {};
}

std::string Emitter::metaclass_struct_cxx(std::string_view class_name) {
  auto dot = class_name.find('.');
  std::string tail =
      (dot == std::string_view::npos) ? std::string(class_name)
                                      : std::string(class_name.substr(dot + 1));
  std::string prefix;
  if (const auto* ci = class_info_for_type_name(class_name);
      ci && !ci->defining_unit.empty() &&
      ci->defining_unit != current_unit_name) {
    prefix = unit_namespace_prefix(ci->defining_unit);
  }
  return prefix + "tp2cc_metaclass_" + mangle(tail);
}

std::string Emitter::metaclass_value_fn_cxx(std::string_view class_name) {
  auto dot = class_name.find('.');
  std::string tail =
      (dot == std::string_view::npos) ? std::string(class_name)
                                      : std::string(class_name.substr(dot + 1));
  std::string prefix;
  if (const auto* ci = class_info_for_type_name(class_name);
      ci && !ci->defining_unit.empty() &&
      ci->defining_unit != current_unit_name) {
    prefix = unit_namespace_prefix(ci->defining_unit);
  }
  return prefix + "tp2cc_metaclass_value_" + mangle(tail);
}

std::vector<Emitter::MetaclassCallable> Emitter::collect_metaclass_callables(
    std::string_view class_name) {
  std::vector<MetaclassCallable> out;
  if (!registry) return out;

  std::string cls = ascii_lower(std::string(class_name));
  std::vector<std::string> chain;
  std::unordered_set<std::string> seen;
  while (!cls.empty() && !seen.count(cls)) {
    seen.insert(cls);
    auto it = registry->classes.find(cls);
    if (it == registry->classes.end()) break;
    chain.push_back(cls);
    cls = it->second.parent;
  }
  std::reverse(chain.begin(), chain.end());

  std::unordered_map<std::string, size_t> pos;
  for (const auto& current : chain) {
    auto it = registry->classes.find(current);
    if (it == registry->classes.end()) continue;
    std::vector<std::string> names;
    names.reserve(it->second.methods.size());
    // Pascal does not let overloads of one name disagree on
    // Constructor/ClassMethod-ness, so the representative overload's kind
    // classifies the whole slot for the metaclass-callable test.
    for (const auto& [name, sigs] : it->second.methods) {
      const MethodSig* sig = representative_method(sigs);
      if (!sig) continue;
      if (sig->kind == SymKind::Constructor || sig->kind == SymKind::ClassMethod) {
        names.push_back(name);
      }
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const MethodSig* sig = representative_method(it->second.methods.at(name));
      auto pit = pos.find(name);
      if (pit == pos.end()) {
        pos[name] = out.size();
        out.push_back({name, sig, false});
      } else {
        out[pit->second].sig = sig;
        out[pit->second].implicit_root_create = false;
      }
    }
  }

  if (const auto* ci = class_info_for_type_name(class_name);
      ci && ci->is_reference_type && !pos.count("create")) {
    // Every Delphi/FPC class inherits `TObject.Create` even if the source
    // does not redeclare a constructor. Class-value calls like
    // `TDLLScannerClass(x).Create` therefore still need a zero-arg metaclass
    // entry instead of falling out as an unsupported member.
    out.push_back({"create", nullptr, true});
  }
  return out;
}

std::optional<Emitter::MetaclassCallableImpl>
Emitter::find_metaclass_callable_impl(std::string_view concrete_class,
                                      const MetaclassCallable& target) {
  if (!registry) return std::nullopt;

  auto matches_target = [&](const MethodSig& candidate) {
    if (!candidate.decl) return false;
    if (target.implicit_root_create) {
      return candidate.kind == SymKind::Constructor &&
             candidate.param_count == 0;
    }
    if (!target.sig || !target.sig->decl) return false;
    return candidate.kind == target.sig->kind &&
           procedural_param_types_to_cxx(candidate.decl->params) ==
               procedural_param_types_to_cxx(target.sig->decl->params);
  };

  std::string cls = ascii_lower(std::string(concrete_class));
  std::unordered_set<std::string> seen;
  while (!cls.empty() && !seen.count(cls)) {
    seen.insert(cls);
    auto it = registry->classes.find(cls);
    if (it == registry->classes.end()) break;
    auto mit = it->second.methods.find(target.name);
    if (mit != it->second.methods.end()) {
      for (const auto& candidate : mit->second) {
        if (matches_target(candidate)) {
          return MetaclassCallableImpl{cls, &candidate, false};
        }
      }
    }
    cls = it->second.parent;
  }

  if (target.implicit_root_create) {
    // `TLinkedListItemClass(x).Create` and similar base-typed class refs still
    // inherit `TObject.Create` even when a derived class also declares
    // `Create(...)` with a different signature. Keep that zero-arg slot alive
    // through the concrete metaclass instead of treating the derived
    // declaration as if it erased the inherited constructor.
    return MetaclassCallableImpl{"tobject", nullptr, true};
  }
  return std::nullopt;
}

const TypeExpr* Emitter::canonicalize_type(const TypeExpr* t) {
  int hops = 0;
  while (t && t->kind == Kind::TyName) {
    if (hops++ >= kMaxAliasChainHops) {
      throw std::runtime_error(
          "Emitter::canonicalize_type: alias chain exceeds "
          "kMaxAliasChainHops; cycle or registry corruption");
    }
    const auto& n = static_cast<const TyName&>(*t);
    auto lit = local_type_aliases_scoped.find(n.name);
    if (lit != local_type_aliases_scoped.end() && lit->second &&
        lit->second != t) {
      t = lit->second;
      continue;
    }
    if (registry) {
      const TypeExpr* next = registry->canonicalize(t);
      if (next && next != t) {
        t = next;
        continue;
      }
    }
    break;
  }
  return t;
}

bool Emitter::const_param_needs_mutable_ref(const TypeExpr* t) {
  if (type_is_reference_class(t)) return false;
  // Old object-style `object` values still treat `const` mostly as a
  // calling-convention hint. Their methods may mutate internal bookkeeping
  // through `self`, so these parameters cannot become `const T&`.
  return type_is_value_object(t);
}

bool Emitter::const_param_needs_const_ref(const TypeExpr* t) {
  (void)t;
  // Pascal `const` is not a blanket request for C++ reference semantics.
  // Use explicit special cases only when the source model proves we need
  // aliasing. The remaining bootstrap paths do not rely on fixed-array
  // `const` parameters preserving caller storage identity, so keep them as
  // plain values.
  return false;
}

const ClassInfo* Emitter::class_info_for_type_name(std::string_view name) {
  std::string low = ascii_lower(std::string(name));
  auto builtin_it = builtin_reference_class_map().find(low);
  if (builtin_it != builtin_reference_class_map().end()) {
    static std::unordered_map<std::string, ClassInfo> builtins;
    auto [it, inserted] = builtins.try_emplace(low);
    if (inserted) {
      it->second.name = low;
      it->second.parent = builtin_it->second.parent;
      it->second.defining_unit = builtin_it->second.defining_unit;
      it->second.is_reference_type = true;
    }
    return &it->second;
  }

  if (!registry) return nullptr;
  auto dot = low.find('.');
  if (dot == std::string::npos) {
    auto it = registry->classes.find(low);
    return it == registry->classes.end() ? nullptr : &it->second;
  }

  // TypeRegistry indexes classes by unqualified Pascal name. Qualified
  // references like `unitname.tfoo` therefore need a second defining-unit
  // check here so `u1.tnode` and `u2.tnode` stay distinguishable.
  std::string unit = low.substr(0, dot);
  std::string tail = low.substr(dot + 1);
  auto it = registry->classes.find(tail);
  if (it == registry->classes.end()) return nullptr;
  return it->second.defining_unit == unit ? &it->second : nullptr;
}

const TypeExpr* Emitter::lookup_named_type_expr(std::string_view name) {
  std::string low = ascii_lower(std::string(name));

  auto lit = local_type_aliases_scoped.find(low);
  if (lit != local_type_aliases_scoped.end() && lit->second) {
    return lit->second;
  }

  if (!registry) return nullptr;

  auto dot = low.find('.');
  if (dot != std::string::npos) {
    std::string unit = low.substr(0, dot);
    std::string tail = low.substr(dot + 1);

    auto ait = registry->aliases.find(tail);
    if (ait != registry->aliases.end() &&
        ait->second.defining_unit == unit &&
        ait->second.target) {
      return ait->second.target.get();
    }
    auto cit = registry->classes.find(tail);
    if (cit != registry->classes.end() && cit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    auto rit = registry->records.find(tail);
    if (rit != registry->records.end() && rit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    auto eit = registry->enums.find(tail);
    if (eit != registry->enums.end() && eit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    return nullptr;
  }

  auto ait = registry->aliases.find(low);
  if (ait != registry->aliases.end() && ait->second.target) {
    return ait->second.target.get();
  }
  if (registry->classes.count(low) || registry->records.count(low) ||
      registry->enums.count(low)) {
    return named_pascal_type(name);
  }
  return nullptr;
}

bool Emitter::is_builtin_reference_class_name(std::string_view name) const {
  return !builtin_reference_class_struct_cxx(
              ascii_lower(std::string(name))).empty();
}

std::string Emitter::metaclass_target_name(const TypeExpr* t) {
  t = canonicalize_type(t);
  if (!t || t->kind != Kind::TyMetaclass) return {};
  return static_cast<const TyMetaclass&>(*t).class_name;
}

bool Emitter::type_is_value_object(const TypeExpr* t) {
  t = canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyObject) {
    return !static_cast<const TyObject&>(*t).is_reference_type;
  }
  if (t->kind != Kind::TyName) return false;

  const auto& n = static_cast<const TyName&>(*t);
  if (const auto* ci = class_info_for_type_name(n.name)) {
    return !ci->is_reference_type;
  }
  return false;
}

bool Emitter::enum_has_explicit_values(const TyEnum& e) {
  for (const auto& member : e.members) {
    if (member.value) return true;
  }
  return false;
}

std::optional<int64_t> Emitter::enum_member_value_int64(const TyEnum& e,
                                                        size_t index) {
  int64_t value = 0;
  for (size_t i = 0; i <= index; ++i) {
    if (e.members[i].value) {
      auto info = eval_const_int_expr(*e.members[i].value);
      if (!info) return std::nullopt;
      value = info->value;
    } else if (i != 0) {
      if (value == INT64_MAX) return std::nullopt;
      ++value;
    }
  }
  return value;
}

std::string Emitter::enum_member_value_to_cxx(const TyEnum& e, size_t index) {
  // Pascal/FPC enum ordinals are assigned left-to-right:
  // the first implicit member is 0, each later implicit member is the
  // previous ordinal plus 1, and any explicit assignment resets the
  // running ordinal for following implicit members.
  // Examples:
  //   (a := 5, b, c)    => a=5,  b=6,  c=7
  //   (a, b := 10, c)   => a=0,  b=10, c=11
  std::string value = "0";
  for (size_t i = 0; i <= index; ++i) {
    if (e.members[i].value) {
      value = const_value_to_cxx(*e.members[i].value);
    } else if (i != 0) {
      value = "((" + value + ") + 1)";
    }
  }
  return value;
}

std::string Emitter::enum_underlying_type_to_cxx(const TyEnum& e) {
  if (e.members.empty()) return "int32_t";

  int64_t lo = 0;
  int64_t hi = 0;
  for (size_t i = 0; i < e.members.size(); ++i) {
    auto value = enum_member_value_int64(e, i);
    if (!value) return "int32_t";
    if (i == 0) {
      lo = *value;
      hi = *value;
    } else {
      lo = std::min(lo, *value);
      hi = std::max(hi, *value);
    }
  }

  // The current bootstrap subset is compiled with {$PACKENUM 1}, so choose
  // the smallest ordinal storage that can represent the enum's full range.
  // That keeps packed-record overlays such as cgbase's TRegisterRec bit-exact
  // without hardcoding compiler-specific enum names here. Full {$PACKENUM n}
  // support can widen this selection later when directive state is tracked.
  if (lo >= 0) {
    uint64_t uhi = static_cast<uint64_t>(hi);
    if (uhi <= UINT8_MAX) return "uint8_t";
    if (uhi <= UINT16_MAX) return "uint16_t";
    if (uhi <= UINT32_MAX) return "uint32_t";
    return "uint64_t";
  }
  if (lo >= INT8_MIN && hi <= INT8_MAX) return "int8_t";
  if (lo >= INT16_MIN && hi <= INT16_MAX) return "int16_t";
  if (lo >= INT32_MIN && hi <= INT32_MAX) return "int32_t";
  return "int64_t";
}

std::string Emitter::primitive_cast_lvalue_ref(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || !expr_is_storage_lvalue(*c.args[0])) return {};
  if (expr_is_untyped_storage_ref(*c.args[0])) return {};
  if (peeled->kind == Kind::Ident) {
    ResolveResult rr = resolve_name(static_cast<const Ident&>(*peeled).name);
    if (rr.kind == ResolvedKind::UnitConst || rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return {};
    }
  }
  // Pascal `T(lv)` used as an lvalue aliases the same storage with a
  // different type. Emit that reinterpretation directly.
  return reinterpret_ref_text(primitive_type_cxx(id.name), expr_to_cxx(*peeled),
                              false);
}

std::string Emitter::primitive_cast_untyped_storage_ptr(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || !expr_is_storage_lvalue(*c.args[0]) ||
      !expr_is_untyped_storage_ref(*c.args[0])) {
    return {};
  }
  if (peeled->kind == Kind::Ident) {
    ResolveResult rr = resolve_name(static_cast<const Ident&>(*peeled).name);
    if (rr.kind == ResolvedKind::UnitConst || rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return {};
    }
  }
  return expr_to_cxx(*peeled);
}

// Returns `&(field_expr)` when `c` is a primitive type-cast over a scalar
// field of a packed record (e.g. `longint(g.d1)` where `g` has type
// `tguid = packed record d1: LongWord; ... end`). Empty otherwise.
//
// The returned text is a *void*-convertible address-of expression*. C++
// permits taking the address of any object regardless of alignment, but
// dereferencing the resulting pointer through a `T*` for which the
// alignment isn't satisfied is UB ([expr.reinterpret.cast]/7,
// [basic.lval]). Callers must therefore feed the text only into runtime
// helpers that read/write through `memcpy` (`tp2cc_reinterpret_load`,
// `tp2cc_reinterpret_store`, `tp2cc_reinterpret_inc`, `tp2cc_reinterpret_dec`).
// Routing it through `*reinterpret_cast<T*>(p)` -- including via
// `tp2cc_reinterpret_storage_ref<T>(void*)` -- would reintroduce the UB
// the existing `[[gnu::packed]]` emit went out of its way to make the
// compiler complain about.
std::string Emitter::primitive_cast_packed_field_ptr(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || peeled->kind != Kind::Member) return {};
  const auto& m = static_cast<const Member&>(*peeled);
  const TypeExpr* base_type = deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return {};
  return "&(" + expr_to_cxx(*peeled) + ")";
}

// Same shape as `primitive_cast_packed_field_ptr`, but for a *bare*
// packed-record-field access -- no outer primitive type-cast wrapping it.
// Used by call sites like `Inc(g.d1, n)` where the field's own declared
// type is the operand type for the read-modify-write. Returns
// `(void_ptr_text, elem_cxx)` so callers can feed both into the
// memcpy-based runtime helpers; same UB caveat applies (the address-of
// text MUST only be consumed via `tp2cc_reinterpret_load` / `_store` /
// `_inc` / `_dec`, never via a `T*` deref).
std::optional<Emitter::PackedFieldStorage>
Emitter::packed_field_storage_ref(const Expr& e) {
  if (e.kind != Kind::Member) return std::nullopt;
  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return std::nullopt;
  const TypeExpr* field_type =
      lookup_record_field_type_in_type(base_type, m.name);
  if (!field_type) return std::nullopt;
  return PackedFieldStorage{"&(" + expr_to_cxx(e) + ")", type_to_cxx(*field_type)};
}

std::optional<Emitter::UntypedStorageIndexView>
Emitter::untyped_storage_index_view(const Index& i) {
  if (i.indices.size() != 1 || i.base->kind != Kind::Call) return std::nullopt;
  const auto& cast = static_cast<const Call&>(*i.base);
  if (cast.args.size() != 1 || cast.callee->kind != Kind::Ident ||
      !expr_is_untyped_storage_ref(*cast.args[0])) {
    return std::nullopt;
  }
  const TypeExpr* base_ty = deduce_type(*i.base);
  if (!base_ty) return std::nullopt;
  base_ty = canonicalize_type(base_ty);
  if (!base_ty || base_ty->kind != Kind::TyArray) return std::nullopt;
  const auto& arr = static_cast<const TyArray&>(*base_ty);
  if (arr.array_kind != ArrayKind::Fixed || arr.dims.size() != 1 ||
      !arr.element) {
    return std::nullopt;
  }
  std::string lo;
  std::string size_expr;
  if (!array_dim_bounds_to_cxx(*arr.dims[0], &lo, &size_expr)) {
    return std::nullopt;
  }
  UntypedStorageIndexView view;
  view.elem_cxx = type_to_cxx(*arr.element);
  const std::string index_cxx = expr_to_cxx(*i.indices[0]);
  const std::string offset =
      "((" + index_cxx + ") - (" + lo + ")) * sizeof(" + view.elem_cxx + ")";
  view.ptr_cxx = "::rt::tp2cc_byte_offset(" + expr_to_cxx(*cast.args[0]) + ", " +
                 offset + ")";
  return view;
}

bool Emitter::array_dim_bounds_to_cxx(const TypeExpr& dim_in,
                                      std::string* lo,
                                      std::string* size_expr) {
  auto expr_is_char = [&](const Expr& e) -> bool {
    const TypeExpr* t = deduce_type(e);
    if (t) t = canonicalize_type(t);
    return tyname_is(t, "char");
  };
  auto ordinal_bound = [&](const Expr& e) -> std::string {
    std::string text = const_value_to_cxx(e);
    return expr_is_char(e) ? "::rt::p_ord(" + text + ")" : text;
  };
  auto ordinal_text = [&](std::string text) -> std::string {
    return "::rt::tp2cc_ordinal_value(" + text + ")";
  };
  const TypeExpr* dim = canonicalize_type(&dim_in);
  if (!dim) return false;
  if (dim->kind == Kind::TyDistinct) {
    // `type TIndex = type word; array[TIndex] of ...` keeps TIndex distinct
    // for assignment compatibility, but its ordinal range is still the same
    // as the underlying type's range. Recurse through the underlying type
    // here so fixed arrays do not silently degrade to pointer aliases.
    return array_dim_bounds_to_cxx(
        *static_cast<const TyDistinct&>(*dim).underlying, lo, size_expr);
  }
  *lo = "0";
  size_expr->clear();
  if (dim->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*dim);
    *lo = const_value_to_cxx(*sr.lo);
    *size_expr = "((" + ordinal_bound(*sr.hi) + ") - (" + ordinal_bound(*sr.lo) +
                 ") + 1)";
    return true;
  }
  if (dim->kind == Kind::TyEnum) {
    const auto& en = static_cast<const TyEnum&>(*dim);
    if (!enum_has_explicit_values(en)) {
      *size_expr = std::to_string(en.members.size());
    } else if (!en.members.empty()) {
      *lo = enum_member_value_to_cxx(en, 0);
      std::string hi = enum_member_value_to_cxx(en, en.members.size() - 1);
      *size_expr = "((" + ordinal_text(hi) + ") - (" +
                   ordinal_text(*lo) + ") + 1)";
    }
    return true;
  }
  if (dim->kind != Kind::TyName) return false;
  const auto& tn = static_cast<const TyName&>(*dim);
  auto leit = local_enums.find(tn.name);
  if (leit != local_enums.end()) {
    if (!leit->second->members.empty() &&
        enum_has_explicit_values(*leit->second)) {
      *lo = mangle(leit->second->members.front().name);
      *size_expr = "((" +
                   ordinal_text(mangle(leit->second->members.back().name)) +
                   ") - (" + ordinal_text(*lo) + ") + 1)";
    } else {
      *size_expr = std::to_string(leit->second->members.size());
    }
    return true;
  }
  if (registry) {
    auto eit = registry->enums.find(tn.name);
    if (eit != registry->enums.end()) {
      if (!eit->second.members.empty()) {
        std::string prefix;
        if (eit->second.defining_unit != current_unit_name) {
          prefix = mangle(eit->second.defining_unit) + "::";
        }
        auto first = prefix + mangle(eit->second.members.front());
        auto last = prefix + mangle(eit->second.members.back());
        *lo = first;
        *size_expr = "((" + ordinal_text(last) + ") - (" +
                     ordinal_text(*lo) + ") + 1)";
      }
      return true;
    }
  }
  if (tn.name == "boolean" || tn.name == "bytebool") {
    *size_expr = "2";
    return true;
  }
  if (tn.name == "byte" || tn.name == "char" || tn.name == "shortint") {
    *size_expr = "256";
    return true;
  }
  if (tn.name == "word" || tn.name == "smallint" || tn.name == "wordbool") {
    *size_expr = "65536";
    return true;
  }
  return false;
}

std::string Emitter::subrange_type_to_cxx(const TySubrange& r) {
  // If both bounds are enum members of the same enum, the subrange's
  // base type IS that enum -- and we want to keep that typing so
  // things like `tp2cc_Set of R_EAX..R_BL` get `tp2cc_Set<tregister>` rather
  // than `tp2cc_Set<int32_t>`.
  auto bound_enum = [&](const Expr* e) -> std::string {
    if (!e || e->kind != Kind::Ident) return {};
    const std::string member = static_cast<const Ident&>(*e).name;
    for (const auto& [enum_name, en] : local_enums) {
      for (const auto& em : en->members) {
        if (em.name == member) return mangle(enum_name);
      }
    }
    if (!registry) return {};
    const auto* info = find_visible_enum_info_for_member(member);
    if (!info) return {};
    std::string prefix;
    if (!info->defining_unit.empty() && info->defining_unit != current_unit_name) {
      prefix = unit_namespace_prefix(info->defining_unit);
    }
    return prefix + mangle(info->name);
  };
  std::string le = bound_enum(r.lo.get());
  std::string he = bound_enum(r.hi.get());
  if (!le.empty() && le == he) return le;
  // Without further info we can only represent subranges as their
  // base type; pick int32_t as a safe default.
  return "int32_t";
}

std::string Emitter::string_type_to_cxx(const TyString& s) {
  if (s.max_length) {
    // `string[N]`
    return "::rt::tp2cc_ShortString<" + const_value_to_cxx(*s.max_length) + ">";
  }
  return "::rt::tp2cc_ShortString<>";
}

std::optional<std::string> Emitter::shortstring_capacity_to_cxx(
    const TypeExpr* t) {
  const TypeExpr* canon = canonicalize_type(t);
  if (!(canon && canon->kind == Kind::TyString)) return std::nullopt;
  const auto& s = static_cast<const TyString&>(*canon);
  if (s.max_length) return const_value_to_cxx(*s.max_length);
  return std::string("255");
}

std::string Emitter::shortstring_value_to_cxx(std::string text,
                                              const TypeExpr* target) {
  auto cap = shortstring_capacity_to_cxx(target);
  if (!cap) return text;
  return "::rt::tp2cc_shortstring_of<" + *cap + ">(" + text + ")";
}

std::string Emitter::pointer_type_to_cxx(const TyPointer& p) {
  return type_to_cxx(*p.target) + "*";
}

std::string Emitter::set_type_to_cxx(const TySet& s) {
  // For enum element types we can use the enum itself to parameterise the
  // set; for primitives we'd use a bounded-integer tp2cc_Set. Keep it coarse for
  // now: element type tagged into the template.
  return "::rt::tp2cc_Set<" + type_to_cxx(*s.element) + ">";
}

std::string Emitter::enum_type_to_cxx(const TyEnum& e, const std::string&) {
  // Inline anonymous enum used as a type-expression -- e.g. a class
  // field declared as `libctype : (libc5, glibc2, glibc21, uclibc);`
  // (fpc-2.2.4/compiler/systems/t_linux.pas). Emit an inline C++
  // anonymous enum with the same underlying width so the member
  // identifiers are accessible in the enclosing scope (member functions
  // of the surrounding class for a class-field, or the surrounding
  // function body for a local-typed var).
  if (e.members.empty()) return "int32_t";
  std::string out = "enum : ";
  out += enum_underlying_type_to_cxx(e);
  out += " { ";
  for (size_t i = 0; i < e.members.size(); ++i) {
    if (i) out += ", ";
    out += mangle(e.members[i].name);
    if (e.members[i].value) {
      out += " = " + const_value_to_cxx(*e.members[i].value);
    }
  }
  out += " }";
  return out;
}

std::string Emitter::array_type_to_cxx(const TyArray& a) {
  if (a.array_kind == ArrayKind::Open) {
    return open_array_type_to_cxx(a);
  }
  if (a.array_kind == ArrayKind::Dynamic) {
    return "::rt::tp2cc_DynArray<" + type_to_cxx(*a.element) + ">";
  }
  // `array[D1, D2, ...] of T` emits as nested ::rt::tp2cc_Array<T, Lo, N>`
  // wrappers with the Pascal bounds preserved at the type level.
  std::string ty = type_to_cxx(*a.element);
  // Wrap from innermost to outermost.
  for (auto it = a.dims.rbegin(); it != a.dims.rend(); ++it) {
    std::string lo, size_expr;
    if (!array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
      // Can't compute dimension statically; fall back to pointer.
      return type_to_cxx(*a.element) + "*";
    }
    ty = "::rt::tp2cc_Array<" + ty + ", " + lo + ", " + size_expr + ">";
  }
  return ty;
}

std::string Emitter::procedural_param_types_to_cxx(
    const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& pp : params) {
    std::string pt;
    if (!pp.type) {
      pt = "void*";
    } else if (type_is_open_array(pp.type.get())) {
      pt = open_array_type_to_cxx(*pp.type);
    } else {
      pt = type_to_cxx(*pp.type);
    }
    if (pp.type) {
      if (pp.mode == Param::Var || pp.mode == Param::Out) pt += "&";
      else if (pp.mode == Param::Const) {
        if (const_param_needs_mutable_ref(pp.type.get())) pt += "&";
        else if (const_param_needs_const_ref(pp.type.get()))
          pt = "const " + pt + "&";
      }
    }
    size_t repeats = pp.names.empty() ? 1 : pp.names.size();
    for (size_t i = 0; i < repeats; ++i) {
      if (!first) out += ", ";
      first = false;
      out += pt;
    }
  }
  return out;
}

std::string Emitter::method_pointer_helper_name(const ProcDecl& pd) {
  // Emit one thunk per Pascal signature. Overloaded methods therefore need
  // distinct helper names, but the helper itself still has to be a valid
  // ordinary C++ identifier with no reserved `__...` spelling.
  std::string out = "tp2cc_methodptr_";
  out += encode_helper_ident(pd.name);
  out += "_";
  out += encode_helper_params(pd.params);
  out += "_ret_";
  out += (pd.pkind == ProcKind::Function && pd.return_type)
             ? encode_helper_type(*pd.return_type)
             : std::string("void");
  return out;
}

std::string Emitter::procedural_type_to_cxx(const TyProcedural& p) {
  std::string ret = p.is_function ? type_to_cxx(*p.return_type) : std::string("void");
  std::string params = procedural_param_types_to_cxx(p.params);
  if (p.is_method) {
    return "::rt::tp2cc_MethodPtr<" + ret + "(" + params + ")>";
  }
  return ret + " (*)(" + params + ")";
}

std::vector<Emitter::RecordFieldDecl>
Emitter::record_field_decls(const std::vector<RecordField>& fields) {
  std::vector<RecordFieldDecl> out;
  for (const auto& f : fields) {
    std::string type_cxx = f.type ? type_to_cxx(*f.type) : std::string("int32_t");
    for (const auto& fn : f.names) {
      RecordFieldDecl entry;
      entry.type = f.type.get();
      entry.type_cxx = type_cxx;
      entry.mangled_name = mangle(fn);
      entry.decl = named_type_to_cxx(f.type.get(), entry.mangled_name);
      out.push_back(std::move(entry));
    }
  }
  return out;
}

Emitter::PackedRecordLayout
Emitter::compute_packed_record_layout(const TyRecord& tr) {
  PackedRecordLayout out;
  out.size_expr = "0";
  auto append_run =
      [&](const std::vector<RecordField>& fields, std::string& size_expr) {
    for (const auto& field : record_field_decls(fields)) {
      out.field_offsets.emplace_back(field.mangled_name, size_expr);
      size_expr = "(" + size_expr + " + sizeof(" + field.type_cxx + "))";
    }
  };
  append_run(tr.fields, out.size_expr);
  if (tr.has_variant) {
    if (!tr.variant_tag_name.empty() && tr.variant_tag_type) {
      RecordField tag_field;
      tag_field.names.push_back(tr.variant_tag_name);
      tag_field.type = tr.variant_tag_type;
      append_run({tag_field}, out.size_expr);
    }
    std::vector<std::string> case_sizes;
    for (const auto& vc : tr.variant_cases) {
      if (vc.fields.empty()) continue;
      std::string case_size_expr = "0";
      for (const auto& field : record_field_decls(vc.fields)) {
        // Variant-case fields share the same outer offset; the per-case
        // base is `(packed_size_expr + case_size_so_far)`.
        const std::string field_offset =
            "(" + out.size_expr + " + " + case_size_expr + ")";
        out.field_offsets.emplace_back(field.mangled_name, field_offset);
        case_size_expr =
            "(" + case_size_expr + " + sizeof(" + field.type_cxx + "))";
      }
      case_sizes.push_back(case_size_expr);
    }
    if (!case_sizes.empty()) {
      std::string max_case = case_sizes.front();
      for (size_t i = 1; i < case_sizes.size(); ++i) {
        max_case = "((" + max_case + ") < (" + case_sizes[i] + ") ? (" +
                   case_sizes[i] + ") : (" + max_case + "))";
      }
      out.size_expr = "(" + out.size_expr + " + " + max_case + ")";
    }
  }
  return out;
}

void Emitter::emit_packed_record_asserts(const std::string& type_text,
                                         const PackedRecordLayout& layout,
                                         std::string_view label) {
  for (const auto& [field_name, offset_expr] : layout.field_offsets) {
    emitln("static_assert(offsetof(" + type_text + ", " + field_name +
           ") == " + offset_expr + ", "
           "\"packed record '" + std::string(label) + "' must place field '" +
           field_name + "' at the exact Pascal byte offset with no inserted "
           "padding.\");");
  }
  emitln("static_assert(sizeof(" + type_text + ") == " + layout.size_expr +
         ", \"packed record '" + std::string(label) +
         "' must have the exact packed Pascal size.\");");
}

std::string Emitter::named_type_to_cxx(const TypeExpr* t, std::string_view name,
                                       std::string_view name_prefix) {
  if (!t) {
    if (name.empty()) {
      return name_prefix.empty() ? std::string("int32_t")
                                 : std::string("int32_t ") +
                                       std::string(name_prefix);
    }
    return std::string("int32_t ") + std::string(name_prefix) + std::string(name);
  }

  // Direct procvar declarations need the identifier inside the `(*)`
  // declarator. `void (*hook)(int)` is valid C++; `void (*)(int) hook`
  // is not.
  if (t->kind == Kind::TyProcedural) {
    const auto& p = static_cast<const TyProcedural&>(*t);
    if (!p.is_method) {
      std::string ret =
          p.is_function ? type_to_cxx(*p.return_type) : std::string("void");
      std::string params = procedural_param_types_to_cxx(p.params);
      if (name.empty()) {
        return ret + " (*" + std::string(name_prefix) + ")(" + params + ")";
      }
      return ret + " (*" + std::string(name_prefix) + std::string(name) +
             ")(" + params + ")";
    }
  }

  std::string ty = type_to_cxx(*t);
  return attach_named_cxx_type(ty, name, name_prefix);
}

std::string Emitter::type_to_cxx(const TypeExpr& t) {
  switch (t.kind) {
    case Kind::TyName:       return type_name_to_cxx(static_cast<const TyName&>(t));
    case Kind::TyPointer:    return pointer_type_to_cxx(static_cast<const TyPointer&>(t));
    case Kind::TySet:        return set_type_to_cxx(static_cast<const TySet&>(t));
    case Kind::TyArray:      return array_type_to_cxx(static_cast<const TyArray&>(t));
    case Kind::TySubrange:   return subrange_type_to_cxx(static_cast<const TySubrange&>(t));
    case Kind::TyString:     return string_type_to_cxx(static_cast<const TyString&>(t));
    case Kind::TyEnum:       return enum_type_to_cxx(static_cast<const TyEnum&>(t), "");
    case Kind::TyDistinct:   return type_to_cxx(*static_cast<const TyDistinct&>(t).underlying);
    case Kind::TyProcedural: return procedural_type_to_cxx(static_cast<const TyProcedural&>(t));
    case Kind::TyMetaclass:
      return "const " +
             metaclass_struct_cxx(static_cast<const TyMetaclass&>(t).class_name) +
             "*";
    case Kind::TyFile: {
      // Pascal `text`, `file`, `file of T`.
      const auto& tf = static_cast<const TyFile&>(t);
      if (tf.is_text || !tf.element) return "::rt::tp2cc_TextFile";
      return "::rt::tp2cc_TypedFile<" + type_to_cxx(*tf.element) + ">";
    }
    case Kind::TyRecord: {
      // Inline anonymous record used as a type-expression -- e.g. a local
      // var declared as `r : packed record a, b : byte; end;`. Emit a C++
      // anonymous struct so subsequent field accesses resolve correctly.
      // Variant cases are deliberately not lowered here; if encountered,
      // fall back to the stub so the bug is visible at C++ compile time
      // rather than silently miscompiled.
      const auto& tr = static_cast<const TyRecord&>(t);
      if (tr.has_variant) return "/* inline-variant-record */ int32_t";
      std::string out = "struct ";
      if (tr.is_packed) out += "[[gnu::packed]] ";
      out += "{ ";
      for (const auto& field : record_field_decls(tr.fields)) {
        out += field.decl + "; ";
      }
      out += "}";
      return out;
    }
    case Kind::TyObject:
      // Inline anonymous object: rare, and the C++ shape would need a
      // base-class context the type-expression position can't carry.
      // Stub so the call site fails loudly if it ever appears.
      return "/* inline-object */ int32_t";
    default:                 return "/* unsupported-type */ int32_t";
  }
}

std::string Emitter::low_high_expr_for_named_type(std::string_view name,
                                                  bool want_low) {
  if (std::string primitive = primitive_low_high_expr(name, want_low);
      !primitive.empty()) {
    return primitive;
  }

  auto emit_enum_bound = [&](std::string_view enum_name,
                             std::string_view defining_unit) -> std::string {
    if (defining_unit.empty() || defining_unit == current_unit_name) {
      return enum_bound_name(enum_name, want_low ? "low" : "high");
    }
    return mangle(defining_unit) + "::" +
           enum_bound_name(enum_name, want_low ? "low" : "high");
  };

  auto dot = name.find('.');
  if (dot != std::string_view::npos) {
    std::string unit(name.substr(0, dot));
    std::string tail(name.substr(dot + 1));
    if (registry) {
      auto eit = registry->enums.find(ascii_lower(tail));
      if (eit != registry->enums.end() && eit->second.defining_unit == unit) {
        return emit_enum_bound(tail, unit);
      }
      auto ait = registry->aliases.find(ascii_lower(tail));
      if (ait != registry->aliases.end() && ait->second.defining_unit == unit &&
          ait->second.target) {
        return low_high_expr_for_type(ait->second.target.get(), want_low);
      }
    }
    return {};
  }

  if (local_enums.count(std::string(name))) {
    return enum_bound_name(name, want_low ? "low" : "high");
  }
  if (registry) {
    auto eit = registry->enums.find(ascii_lower(std::string(name)));
    if (eit != registry->enums.end()) {
      return emit_enum_bound(name, eit->second.defining_unit);
    }
  }

  auto lit = local_type_aliases_scoped.find(std::string(name));
  if (lit != local_type_aliases_scoped.end() && lit->second) {
    return low_high_expr_for_type(lit->second, want_low);
  }
  if (registry) {
    auto ait = registry->aliases.find(ascii_lower(std::string(name)));
    if (ait != registry->aliases.end() && ait->second.target) {
      return low_high_expr_for_type(ait->second.target.get(), want_low);
    }
  }
  return {};
}

std::string Emitter::low_high_expr_for_type(const TypeExpr* t,
                                            bool want_low) {
  if (!t) return {};
  if (t->kind == Kind::TyName) {
    return low_high_expr_for_named_type(static_cast<const TyName&>(*t).name,
                                        want_low);
  }
  if (t->kind == Kind::TyDistinct) {
    return low_high_expr_for_type(
        static_cast<const TyDistinct&>(*t).underlying.get(), want_low);
  }
  if (t->kind == Kind::TySubrange) {
    const auto& r = static_cast<const TySubrange&>(*t);
    return const_value_to_cxx(want_low ? *r.lo : *r.hi);
  }
  if (t->kind == Kind::TyArray) {
    // `low(arr)` / `high(arr)` on an array TYPE (not a value): the
    // bounds come from the index type. Recurse on the first dimension.
    // Open/dynamic arrays need a value to ask `p_length` of, which the
    // type-only path doesn't have -- caller will fall back.
    const auto& arr = static_cast<const TyArray&>(*t);
    if (arr.array_kind != ArrayKind::Fixed) return {};
    if (arr.dims.empty()) return {};
    return low_high_expr_for_type(arr.dims[0].get(), want_low);
  }
  return {};
}

// ---------------------------------------------------------------------------
// Expression-type deduction. Used by the Member / Ident emitters so
// decisions like "is `obj.name` a method call or a field read?" come
// from the actual type tree, not name-matching heuristics.

const TypeExpr* Emitter::deduce_const_decl_type(const ConstDecl& cd) {
  if (cd.type) return cd.type.get();
  auto info = eval_const_int_expr(*cd.value);
  if (!info || !info->type) return nullptr;
  return builtin_integer_type(info->type);
}

const TypeExpr* Emitter::deduce_const_info_type(const ConstInfo& c) {
  if (c.type) return c.type.get();
  if (!c.value) return nullptr;
  auto info = eval_const_int_expr(*c.value);
  if (!info || !info->type) return nullptr;
  return builtin_integer_type(info->type);
}

std::optional<ConvertedConstInt> Emitter::convert_const_int_value(
    Location where, int64_t value, const TypeExpr* target,
    bool explicit_conversion, bool diagnose) {
  if (!target) return std::nullopt;
  const TypeExpr* canon = canonicalize_type(target);
  if (!canon || canon->kind != Kind::TyName) return std::nullopt;
  std::string name = ascii_lower(static_cast<const TyName&>(*canon).name);
  auto* info = primitive_info(name);
  if (!info || info->int_kind == PrimitiveIntKind::None) return std::nullopt;

  bool fits = true;
  if (info->int_kind == PrimitiveIntKind::Unsigned) {
    if (value < 0) {
      fits = false;
    } else if (info->bits < 64) {
      fits = static_cast<uint64_t>(value) <= ((uint64_t{1} << info->bits) - 1);
    }
  } else {
    int64_t lo = 0;
    int64_t hi = 0;
    if (info->bits == 64) {
      lo = INT64_MIN;
      hi = INT64_MAX;
    } else {
      lo = -(int64_t{1} << (info->bits - 1));
      hi = (int64_t{1} << (info->bits - 1)) - 1;
    }
    fits = value >= lo && value <= hi;
  }
  if (!fits && diagnose && !explicit_conversion) {
    report_warning(where, "range check error while evaluating constants");
  }

  ConvertedConstInt converted;
  converted.type = info;
  converted.bits = low_bits(static_cast<uint64_t>(value), info->bits);
  if (info->int_kind == PrimitiveIntKind::Unsigned) {
    converted.value = static_cast<int64_t>(converted.bits);
    return converted;
  }
  if (info->bits == 64) {
    converted.value = static_cast<int64_t>(converted.bits);
    return converted;
  }
  uint64_t sign_bit = uint64_t{1} << (info->bits - 1);
  if ((converted.bits & sign_bit) == 0) {
    converted.value = static_cast<int64_t>(converted.bits);
  } else {
    converted.value =
        static_cast<int64_t>(converted.bits | ~low_bits(UINT64_MAX, info->bits));
  }
  return converted;
}

std::optional<ConstIntExprInfo> Emitter::eval_const_int_cast(
    const Call& c, std::unordered_set<std::string>* visiting_const_names) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return std::nullopt;
  const auto& callee = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(callee.name)) return std::nullopt;
  auto* cast_type = builtin_integer_type(callee.name);
  if (!cast_type) return std::nullopt;
  auto arg = eval_const_int_expr(*c.args[0], visiting_const_names);
  if (!arg) return std::nullopt;
  auto converted =
      convert_const_int_value(c.loc, arg->value, cast_type, true, false);
  if (!converted) return std::nullopt;
  return ConstIntExprInfo{converted->value, converted->type};
}

std::optional<ConstIntExprInfo> Emitter::eval_const_int_expr(
    const Expr& e, std::unordered_set<std::string>* visiting_const_names) {
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      if (n.value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
      int64_t value = static_cast<int64_t>(n.value);
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
    }
    case Kind::Unary: {
      const auto& u = static_cast<const Unary&>(e);
      auto operand = eval_const_int_expr(*u.operand, visiting_const_names);
      if (!operand) return std::nullopt;
      if (u.op == UnOp::Plus) return operand;
      if (u.op != UnOp::Neg) return std::nullopt;
      int64_t value = 0;
      if (!checked_sub_int64(0, operand->value, &value)) return std::nullopt;
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
    }
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      auto lhs = eval_const_int_expr(*b.lhs, visiting_const_names);
      auto rhs = eval_const_int_expr(*b.rhs, visiting_const_names);
      if (!lhs || !rhs) return std::nullopt;
      int64_t value = 0;
      switch (b.op) {
        case BinOp::Add:
          if (!checked_add_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Sub:
          if (!checked_sub_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Mul:
          if (!checked_mul_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::IntDiv:
          if (!checked_div_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Mod:
          if (!checked_mod_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Shl:
          if (!checked_shl_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Shr:
          if (!checked_shr_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::And:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) &
                                       static_cast<uint64_t>(rhs->value));
          break;
        case BinOp::Or:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) |
                                       static_cast<uint64_t>(rhs->value));
          break;
        case BinOp::Xor:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) ^
                                       static_cast<uint64_t>(rhs->value));
          break;
        default:
          return std::nullopt;
      }
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
    }
    case Kind::Call:
      return eval_const_int_cast(static_cast<const Call&>(e),
                                 visiting_const_names);
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      if (!visiting_const_names) {
        std::unordered_set<std::string> local_visiting;
        return eval_const_int_expr(e, &local_visiting);
      }
      if (!visiting_const_names->insert(id.name).second) return std::nullopt;
      auto pop = [&]() { visiting_const_names->erase(id.name); };

      auto maybe_fold_const_decl =
          [&](const ConstDecl& cd) -> std::optional<ConstIntExprInfo> {
        // Untyped `const X = ...` is a compile-time constant.
        // Typed `const X: T = ...` is writable storage in this dialect and
        // must not be folded through its initializer.
        if (cd.type || !cd.value) return std::nullopt;
        return eval_const_int_expr(*cd.value, visiting_const_names);
      };

      auto maybe_fold_const_info =
          [&](const ConstInfo& c) -> std::optional<ConstIntExprInfo> {
        if (c.type || !c.value) return std::nullopt;
        return eval_const_int_expr(*c.value, visiting_const_names);
      };

      auto lit = local_consts.find(id.name);
      if (lit != local_consts.end() && lit->second && lit->second->value) {
        std::optional<ConstIntExprInfo> out =
            maybe_fold_const_decl(*lit->second);
        pop();
        return out;
      }

      auto lookup_const = [&](const UnitInfo& u, bool export_only)
          -> const ConstInfo* {
        return export_only ? u.find_export_const(id.name) : u.find_const(id.name);
      };

      if (registry) {
        auto cur = registry->units.find(current_unit_name);
        if (cur != registry->units.end()) {
          if (const auto* c = lookup_const(cur->second, false)) {
            std::optional<ConstIntExprInfo> out = maybe_fold_const_info(*c);
            pop();
            return out;
          }
          for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
               ++it) {
            auto uit = registry->units.find(*it);
            if (uit == registry->units.end()) continue;
            if (const auto* c = lookup_const(uit->second, true)) {
              std::optional<ConstIntExprInfo> out = maybe_fold_const_info(*c);
              pop();
              return out;
            }
          }
        }
      }
      pop();
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

const TypeExpr* Emitter::deduce_type(const Expr& e) {
  if (!registry) return nullptr;
  switch (e.kind) {
    case Kind::IntLit:
    case Kind::Unary:
    case Kind::Binary:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      if (e.kind != Kind::Binary) return nullptr;
      {
        const auto& b = static_cast<const Binary&>(e);
        if (b.op == BinOp::Is) return builtin_boolean_type();
        if (b.op == BinOp::As) {
          if (b.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*b.rhs);
            auto ait = registry->aliases.find(id.name);
            if (ait != registry->aliases.end()) return ait->second.target.get();
            if (class_info_for_type_name(id.name)) return named_pascal_type(id.name);
          }
          return nullptr;
        }
      }
      break;
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      // Local variables and parameters shadow everything.
      auto lit = local_types.find(id.name);
      if (lit != local_types.end()) return lit->second;
      auto lcit = local_consts.find(id.name);
      if (lcit != local_consts.end() && lcit->second)
        return deduce_const_decl_type(*lcit->second);
      // Nested functions live in `local_nested_fns`, not `local_types`.
      // Type deduction still needs to see their result type so boolean
      // expressions like `if ready and flag then` lower to `&&` even
      // before the ident emitter auto-calls a parameterless `ready`.
      auto nit = local_nested_fns.find(id.name);
      if (nit != local_nested_fns.end() && nit->second.is_function)
        return nit->second.return_type;
      // Self -- canonically the current class's type.
      if (id.name == "self" && !current_class_name.empty()) {
        // We don't track a direct TypeExpr for the class here. Returning
        // nullptr is fine; Member access will fall through to class-name
        // handling via `current_class_name` in the caller.
        return nullptr;
      }
      if (current_fn_is_function && current_fn_result_type &&
          !current_fn_name.empty() && id.name == current_fn_name) {
        return current_fn_result_type;
      }
      if (bare_result_type && is_pascal_result_ident(id.name)) {
        return bare_result_type;
      }
      if (outer_result_type && !outer_result_name.empty() &&
          id.name == outer_result_name) {
        return outer_result_type;
      }
      // Class member (inside method body of a known class).
      if (!current_class_name.empty()) {
        if (const auto* ci = class_info_for_type_name(current_class_name);
            ci && ci->is_reference_type) {
          if (id.name == "instancesize") return builtin_integer_type("longint");
          if (id.name == "classtype") return named_pascal_type("tclass");
          if (id.name == "inheritsfrom") return builtin_boolean_type();
        }
        if (auto* f = registry->lookup_class_field(current_class_name, id.name))
          return f->type.get();
        if (auto* p = registry->lookup_class_property(current_class_name, id.name))
          return p->type.get();
        if (auto* m = registry->lookup_class_method(current_class_name, id.name);
            m && m->decl && m->decl->return_type) {
          return m->decl->return_type.get();
        }
      }
      // `with X do` bindings contribute fields of their target type --
      // the ident might name such a field.
      for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
        const std::string& ac = it->class_name;
        if (const auto* ci = class_info_for_type_name(ac);
            ci && ci->is_reference_type) {
          if (id.name == "instancesize") return builtin_integer_type("longint");
          if (id.name == "classtype") return named_pascal_type("tclass");
          if (id.name == "inheritsfrom") return builtin_boolean_type();
        }
        if (!ac.empty()) {
          if (auto* f = registry->lookup_class_field(ac, id.name))
            return f->type.get();
          if (auto* p = registry->lookup_class_property(ac, id.name))
            return p->type.get();
          if (auto* m = registry->lookup_class_method(ac, id.name);
              m && m->decl && m->decl->return_type) {
            return m->decl->return_type.get();
          }
        }
        if (const TypeExpr* rf = lookup_record_field_type_in_with(*it, id.name))
          return rf;
      }
      // Unit-level lookup: own unit first, then each `uses` entry
      // (right-to-left). The global last-wins maps on TypeRegistry
      // are NOT consulted here -- two units can share a name with
      // different types and the only right answer is to find the
      // one exported from a unit the current unit actually uses.
      // Own-unit: both interface and impl visible. Other units:
      // interface-exports only.
      auto lookup_own = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_var(id.name)) return v->type.get();
        if (auto* c = u.find_const(id.name)) return deduce_const_info_type(*c);
        if (auto* p = u.find_proc(id.name);
            p && p->decl && p->decl->return_type)
          return p->decl->return_type.get();
        return nullptr;
      };
      auto lookup_export = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_export_var(id.name)) return v->type.get();
        if (auto* c = u.find_export_const(id.name)) return deduce_const_info_type(*c);
        if (auto* p = u.find_export_proc(id.name);
            p && p->decl && p->decl->return_type)
          return p->decl->return_type.get();
        return nullptr;
      };
      auto cur = registry->units.find(current_unit_name);
      if (cur != registry->units.end()) {
        if (const auto* t = lookup_own(cur->second)) return t;
        for (auto it = cur->second.uses.rbegin();
             it != cur->second.uses.rend(); ++it) {
          auto uit = registry->units.find(*it);
          if (uit == registry->units.end()) continue;
          if (const auto* t = lookup_export(uit->second)) return t;
        }
      }
      return nullptr;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      const TypeExpr* t = deduce_type(*d.operand);
      if (!t) return nullptr;
      t = canonicalize_type(t);
      if (tyname_is(t, "pchar")) return builtin_char_type();
      if (tyname_is(t, "ppchar")) return builtin_pchar_type();
      if (t && t->kind == Kind::TyPointer) {
        const auto& p = static_cast<const TyPointer&>(*t);
        return p.target.get();
      }
      return nullptr;
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      std::string cls;
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        if (id.name == "self") cls = current_class_name;
        else if (registry->classes.count(id.name) ||
                 registry->records.count(id.name)) cls = id.name;
      }
      if (cls.empty()) {
        const TypeExpr* bt = deduce_type(*m.base);
        if (bt) cls = metaclass_target_name(bt);
      }
      if (cls.empty()) {
        // Chained accesses like `x.sym.name` and result-slot writes like
        // `clone.next := nil` must recover the Pascal class alias from the
        // base expression, not from the canonicalized class body node.
        // `deduce_class_alias` keeps casts, `self`, and named class values
        // on that Pascal-facing path.
        cls = deduce_class_alias(*m.base);
      }
      if (cls.empty()) return nullptr;
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type) {
        if (m.name == "instancesize") return builtin_integer_type("longint");
        if (m.name == "classtype") return named_pascal_type("tclass");
        if (m.name == "inheritsfrom") return builtin_boolean_type();
      }
      if (auto* pm = registry->lookup_class_method(cls, m.name)) {
        if (pm->decl.get() && pm->decl.get()->return_type)
          return pm->decl.get()->return_type.get();
        return nullptr;
      }
      if (auto* pf = registry->lookup_class_field(cls, m.name))
        return pf->type.get();
      if (auto* pp = registry->lookup_class_property(cls, m.name))
        return pp->type.get();
      if (auto* rf = registry->lookup_record_field(cls, m.name))
        return rf->type.get();
      return nullptr;
    }
    case Kind::Index: {
      const auto& ix = static_cast<const Index&>(e);
      if (registry && ix.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*ix.base);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_class_property(cls, mem.name);
              prop && !prop->params.empty())
            return prop->type.get();
        }
      }
      if (registry && ix.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*ix.base);
        if (auto found = find_implicit_class_property(id.name);
            found && found->prop && !found->prop->params.empty()) {
          return found->prop->type.get();
        }
      }
      const TypeExpr* bt = deduce_type(*ix.base);
      if (!bt) return nullptr;
      bt = canonicalize_type(bt);
      if (bt && bt->kind == Kind::TyString) return builtin_char_type();
      if (tyname_is(bt, "pchar")) return builtin_char_type();
      if (tyname_is(bt, "ppchar")) return builtin_pchar_type();
      if (bt && bt->kind == Kind::TyArray)
        return static_cast<const TyArray&>(*bt).element.get();
      if (bt && bt->kind == Kind::TyPointer)
        return static_cast<const TyPointer&>(*bt).target.get();
      if (registry) {
        std::string cls = registry->direct_type_name(bt);
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_default_property(cls))
            return prop->type.get();
        }
      }
      return nullptr;
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      const TypeExpr* callee_type = deduce_type(*c.callee);
      if (callee_type) callee_type = canonicalize_type(callee_type);
      if (callee_type && callee_type->kind == Kind::TyProcedural) {
        const auto& p = static_cast<const TyProcedural&>(*callee_type);
        if (p.is_function) return p.return_type.get();
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        if ((id.name == "char" || id.name == "chr") && c.args.size() == 1)
          return builtin_char_type();
        if (id.name == "pchar" && c.args.size() == 1) return builtin_pchar_type();
        if ((id.name == "succ" || id.name == "pred" || id.name == "upcase") &&
            c.args.size() == 1)
          return deduce_type(*c.args[0]);
        auto nit = local_nested_fns.find(id.name);
        if (nit != local_nested_fns.end() && nit->second.is_function)
          return nit->second.return_type;
        // Type cast `T(expr)` -- target type is the alias's own type.
        auto ait = registry->aliases.find(id.name);
        if (ait != registry->aliases.end() && c.args.size() == 1)
          return ait->second.target.get();
        // Function call -> return type. Per-unit resolution avoids
        // the last-wins global-map pitfall.
        ResolveResult rr = resolve_name(id.name);
        if (rr.proc && rr.proc->return_type)
          return rr.proc->return_type.get();
      } else if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        std::string cls;
        if (mem.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          if (id.name == "self") cls = current_class_name;
          else if (registry->classes.count(id.name) ||
                   registry->records.count(id.name)) cls = id.name;
        }
        if (cls.empty()) {
          const TypeExpr* bt = deduce_type(*mem.base);
          if (bt) cls = metaclass_target_name(bt);
        }
        if (cls.empty()) cls = deduce_class_alias(*mem.base);
        if (!cls.empty()) {
          if (auto* cm = registry->lookup_class_method(cls, mem.name)) {
            if (cm->decl.get() && cm->decl.get()->return_type)
              return cm->decl.get()->return_type.get();
          }
        }
      }
      return nullptr;
    }
    case Kind::StringLit: {
      const auto& sl = static_cast<const StringLit&>(e);
      return sl.value.size() == 1 ? builtin_char_type()
                                  : builtin_string_type();
    }
    case Kind::AddrOf: {
      // Returning a pointer type would be ideal, but synthesising it on
      // the fly requires owning a TypeExpr we don't have. Unused today.
      return nullptr;
    }
    default:
      return nullptr;
  }
  return nullptr;
}

std::string Emitter::deduce_class_alias(const Expr& e) {
  if (!registry) return {};
  // Fast path for `self` -- we already know the class.
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self") return current_class_name;
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      auto cit = registry->classes.find(id.name);
      if (cit != registry->classes.end() && cit->second.is_reference_type) {
        return id.name;
      }
    }
  }
  const TypeExpr* t = deduce_type(e);
  if (!t) return {};
  if (auto cls = metaclass_target_name(t); !cls.empty()) return cls;
  if (auto cls = registry->direct_type_name(t); !cls.empty()) return cls;
  return registry->direct_type_name(registry->canonicalize(t));
}

const TypeExpr* Emitter::lookup_record_field_type_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!registry || !type) return nullptr;
  type = canonicalize_type(type);
  if (!type) return nullptr;
  if (type->kind == Kind::TyPointer) {
    const auto& ptr = static_cast<const TyPointer&>(*type);
    return lookup_record_field_type_in_type(ptr.target.get(), field_name);
  }
  if (type->kind == Kind::TyName) {
    const auto& tn = static_cast<const TyName&>(*type);
    if (auto* rf = registry->lookup_record_field(tn.name,
                                                 std::string(field_name))) {
      return rf->type.get();
    }
  }
  if (type->kind != Kind::TyRecord) return nullptr;

  const auto& rec = static_cast<const TyRecord&>(*type);
  auto match_field = [&](const RecordField& rf) -> const TypeExpr* {
    for (const auto& name : rf.names) {
      if (ascii_lower(name) == ascii_lower(field_name)) return rf.type.get();
    }
    return nullptr;
  };

  for (const auto& rf : rec.fields) {
    if (const TypeExpr* ft = match_field(rf)) return ft;
  }
  for (const auto& vc : rec.variant_cases) {
    for (const auto& rf : vc.fields) {
      if (const TypeExpr* ft = match_field(rf)) return ft;
    }
  }
  return nullptr;
}

const TypeExpr* Emitter::lookup_record_field_type_in_with(
    const WithBind& wb, std::string_view field_name) {
  if (const TypeExpr* ft = lookup_record_field_type_in_type(
          wb.type, field_name)) {
    return ft;
  }
  if (!registry || wb.class_name.empty()) return nullptr;
  if (auto* rf = registry->lookup_record_field(wb.class_name,
                                               std::string(field_name))) {
    return rf->type.get();
  }
  return nullptr;
}

bool Emitter::with_bind_has_visible_member(const WithBind& wb,
                                           std::string_view name) {
  if (!registry) return false;
  if (!wb.class_name.empty()) {
    if (registry->lookup_class_method(wb.class_name, std::string(name)) ||
        registry->lookup_class_field(wb.class_name, std::string(name)) ||
        registry->lookup_class_property(wb.class_name, std::string(name))) {
      return true;
    }
  }
  return lookup_record_field_type_in_with(wb, name) != nullptr;
}

bool Emitter::type_is_packed_record(const TypeExpr* t) {
  if (!registry || !t) return false;
  if (t->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*t);
    auto it = registry->records.find(ascii_lower(n.name));
    if (it != registry->records.end()) return it->second.is_packed;
  }
  t = canonicalize_type(t);
  return t && t->kind == Kind::TyRecord &&
         static_cast<const TyRecord&>(*t).is_packed;
}

bool Emitter::type_is_direct_packed_aggregate(const TypeExpr* t) {
  if (!t) return false;
  if (t->kind == Kind::TyName) {
    const std::string low = ascii_lower(static_cast<const TyName&>(*t).name);
    if (!low.empty() && registry &&
        (registry->records.count(low) || registry->classes.count(low))) {
      return true;
    }
    static const std::unordered_set<std::string> runtime_aggregate_types = {
        "datetime", "dirstr", "extstr", "namestr",
        "pathstr",  "searchrec", "stat", "tmethod",
    };
    if (runtime_aggregate_types.count(low)) return true;
  }
  t = canonicalize_type(t);
  if (!t) return false;
  switch (t->kind) {
    case Kind::TyArray:
    case Kind::TyRecord:
    case Kind::TyObject:
    case Kind::TyProcedural:
    case Kind::TySet:
    case Kind::TyString:
      return true;
    default:
      return false;
  }
}

bool Emitter::type_is_byte_aligned_packed_index_carrier(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  switch (t->kind) {
    case Kind::TyString:
    case Kind::TySet:
      return true;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(*t);
      return type_is_byte_aligned_packed_index_carrier(a.element.get());
    }
    case Kind::TyEnum: {
      const std::string underlying =
          enum_underlying_type_to_cxx(static_cast<const TyEnum&>(*t));
      return underlying == "uint8_t" || underlying == "int8_t";
    }
    case Kind::TySubrange: {
      const std::string lowered =
          subrange_type_to_cxx(static_cast<const TySubrange&>(*t));
      return lowered == "uint8_t" || lowered == "int8_t";
    }
    case Kind::TyName: {
      const std::string low = ascii_lower(static_cast<const TyName&>(*t).name);
      return low == "char" || low == "byte" || low == "shortint" ||
             low == "boolean";
    }
    default:
      return false;
  }
}

std::optional<Emitter::PackedAggregateFieldUse>
Emitter::direct_packed_aggregate_field_use(const Expr& e) {
  if (!registry || e.kind != Kind::Member) return std::nullopt;
  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return std::nullopt;
  const TypeExpr* field_type = lookup_record_field_type_in_type(base_type, m.name);
  if (!field_type || !type_is_direct_packed_aggregate(field_type)) {
    return std::nullopt;
  }
  std::string record_name = registry->direct_type_name(base_type);
  if (record_name.empty()) {
    record_name = "packed record";
  }
  return PackedAggregateFieldUse{record_name, m.name};
}

void Emitter::report_packed_aggregate_subobject_use(
    Location where, std::string_view op,
    const PackedAggregateFieldUse& use) {
  report_error(where, std::string(op) + " through packed aggregate field '" +
                          use.field_name + "' of '" + use.record_name +
                          "' is unsupported");
}

// Strip a chain of primitive casts like `pointer(longint(x))` down to the
// underlying storage expression. Pascal uses these casts to satisfy type
// checking before reinterpreting bytes, so emit-time lvalue analysis has to
// look through them.
const Expr* Emitter::peel_primitive_casts(const Expr* e) {
  while (e && e->kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(*e);
    if (c.args.size() != 1 || c.callee->kind != Kind::Ident) break;
    if (!is_primitive_type(static_cast<const Ident&>(*c.callee).name)) break;
    e = c.args[0].get();
  }
  return e;
}

// Decide whether an expression names mutable storage we can legally
// reinterpret in-place. This is stricter than "AST looks like an lvalue":
// parameterless methods auto-call in value context, and `inherited.name`
// can also lower to a call, so those must not be treated as addressable
// storage here.
bool Emitter::expr_is_storage_lvalue(const Expr& e) {
  const Expr* peeled = peel_primitive_casts(&e);
  const Expr& root = peeled ? *peeled : e;
  bool is_inherited_member = false;
  if (root.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(root);
    if (m.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*m.base).name == "inherited") {
      is_inherited_member = true;
    }
  }
  bool is_lvalue_shape =
      !is_inherited_member &&
      (root.kind == Kind::Ident || root.kind == Kind::Member ||
       root.kind == Kind::Index || root.kind == Kind::Deref);
  if (root.kind == Kind::Ident &&
      local_untyped_params.count(static_cast<const Ident&>(root).name)) {
    is_lvalue_shape = true;
  }
  if (!is_lvalue_shape || !registry) return is_lvalue_shape;

  if (root.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(root);
    ResolveResult rr = resolve_name(id.name);
    if (rr.is_callable && rr.accepts_zero_args) return false;
    if (rr.kind == ResolvedKind::WithProperty ||
        rr.kind == ResolvedKind::ClassProperty) {
      return false;
    }
  } else if (root.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(root);
    std::string cls = deduce_class_alias(*m.base);
    if (!cls.empty()) {
      if (registry->lookup_class_property(cls, m.name)) return false;
      if (const auto* method = registry->lookup_class_method(cls, m.name)) {
        if (method->accepts_zero_args) return false;
      }
    }
  }
  return true;
}

bool Emitter::expr_is_untyped_storage_ref(const Expr& e) {
  const Expr* peeled = peel_primitive_casts(&e);
  const Expr& root = peeled ? *peeled : e;
  return root.kind == Kind::Ident &&
         local_untyped_params.count(static_cast<const Ident&>(root).name);
}

bool Emitter::expr_is_charish(const Expr& e) {
  const TypeExpr* t = deduce_type(e);
  if (!t) return false;
  t = canonicalize_type(t);
  return tyname_is(t, "char");
}

bool Emitter::type_is_pcharish(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (tyname_is(t, "pchar")) return true;
  return t->kind == Kind::TyPointer &&
         tyname_is(static_cast<const TyPointer&>(*t).target.get(), "char");
}

bool Emitter::type_is_metaclass(const TypeExpr* t) {
  return !metaclass_target_name(t).empty();
}

bool Emitter::type_is_reference_class(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyObject) {
    return static_cast<const TyObject&>(*t).is_reference_type;
  }
  if (t->kind != Kind::TyName) return false;
  const auto& n = static_cast<const TyName&>(*t);
  if (const auto* ci = class_info_for_type_name(n.name)) {
    return ci->is_reference_type;
  }
  return is_builtin_reference_class_name(n.name);
}

bool Emitter::expr_is_reference_class(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self" && !current_class_name.empty() && registry) {
      auto it = registry->classes.find(current_class_name);
      return it != registry->classes.end() && it->second.is_reference_type;
    }
  } else if (e.kind == Kind::Call && registry) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (is_builtin_reference_class_name(id.name)) return true;
      auto it = registry->classes.find(id.name);
      if (it != registry->classes.end() && it->second.is_reference_type) {
        return true;
      }
    }
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident &&
        is_builtin_reference_class_name(
            static_cast<const Ident&>(*c.callee).name)) {
      return true;
    }
  }
  return type_is_reference_class(deduce_type(e));
}

std::string Emitter::member_access_op(const TypeExpr* t) {
  return (type_is_reference_class(t) || type_is_metaclass(t)) ? "->" : ".";
}

std::string Emitter::member_access_op(const Expr& e) {
  const TypeExpr* t = deduce_type(e);
  // Pascal always spells member access as `base.member`, but after lowering
  // the base may already be a pointer-typed C++ value: reference classes,
  // metaclasses, explicit pointer/class casts, or fields/results whose
  // Pascal type is pointer-ish. Once the base is such a value, every further
  // hop has to stay on `->`.
  if (expr_is_reference_class(e) || type_is_pointerish(t)) return "->";
  if (t) {
    std::string cxx = type_to_cxx(*t);
    if (!cxx.empty() && cxx.back() == '*') return "->";
  }
  return ".";
}

bool Emitter::type_is_stringish(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyString) return true;
  return tyname_is(t, "string") || tyname_is(t, "shortstring") ||
         tyname_is(t, "ansistring") || tyname_is(t, "utf8string");
}

bool Emitter::type_is_pointerish(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (type_is_metaclass(t)) return true;
  if (type_is_reference_class(t)) return true;
  if (t->kind == Kind::TyPointer) return true;
  return tyname_is(t, "pointer") || tyname_is(t, "pchar") ||
         tyname_is(t, "ppchar");
}

bool Emitter::type_is_open_array(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  return t && t->kind == Kind::TyArray &&
         static_cast<const TyArray&>(*t).array_kind == ArrayKind::Open;
}

std::string Emitter::reinterpret_ref_text(const std::string& ty_cxx,
                                          const std::string& source_cxx,
                                          bool pointee_view) {
  // Two distinct Pascal operations lower through this helper:
  //   - "same storage, new type" (`absolute`, typed lvalue casts) =>
  //     tp2cc_reinterpret_storage_ref<T>(x)
  //   - "pointer points at T" (`absolute p` where p is pointer-ish) =>
  //     tp2cc_reinterpret_ref<T>(p)
  // They currently compile to the same cast sequence in the runtime, but
  // the emitter must keep the intent separate so later runtime tightening
  // does not blur "reinterpret the pointer slot" with "reinterpret pointee".
  const char* helper = pointee_view ? "::rt::tp2cc_reinterpret_ref<"
                                    : "::rt::tp2cc_reinterpret_storage_ref<";
  return std::string(helper) + ty_cxx + ">(" + source_cxx + ")";
}

const VarInfo* Emitter::find_visible_unit_var(const std::string& name) {
  if (!registry) return nullptr;
  auto cur = registry->units.find(current_unit_name);
  if (cur == registry->units.end()) return nullptr;
  if (const auto* v = cur->second.find_var(name)) return v;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    auto uit = registry->units.find(*it);
    if (uit == registry->units.end()) continue;
    if (const auto* v = uit->second.find_export_var(name)) return v;
  }
  return nullptr;
}

const ConstInfo* Emitter::find_visible_unit_const(const std::string& name) {
  if (!registry) return nullptr;
  auto cur = registry->units.find(current_unit_name);
  if (cur == registry->units.end()) return nullptr;
  if (const auto* c = cur->second.find_const(name)) return c;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    auto uit = registry->units.find(*it);
    if (uit == registry->units.end()) continue;
    if (const auto* c = uit->second.find_export_const(name)) return c;
  }
  return nullptr;
}

const EnumInfoReg* Emitter::find_visible_enum_info_for_member(
    const std::string& name) {
  if (!registry) return nullptr;

  auto find_in_unit = [&](const std::string& unit_name) -> const EnumInfoReg* {
    for (const auto& [enum_name, info] : registry->enums) {
      (void)enum_name;
      if (info.defining_unit != unit_name) continue;
      for (const auto& member : info.members) {
        if (member == name) return &info;
      }
    }
    return nullptr;
  };

  if (const auto* info = find_in_unit(current_unit_name)) return info;
  auto cur = registry->units.find(current_unit_name);
  if (cur == registry->units.end()) return nullptr;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    if (const auto* info = find_in_unit(*it)) return info;
  }
  return nullptr;
}

std::optional<Emitter::AbsoluteTargetInfo> Emitter::resolve_absolute_target(
    const VarDecl& vd) {
  AbsoluteTargetInfo info;
  info.cxx = resolve_name(vd.absolute_target).cxx;

  if (local_untyped_params.count(vd.absolute_target)) {
    info.is_pointerish = true;
    return info;
  }

  auto lit = local_consts.find(vd.absolute_target);
  if (lit != local_consts.end()) {
    if (!lit->second || !lit->second->type) {
      report_error(vd.loc, "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    info.type = lit->second->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  auto tit = local_types.find(vd.absolute_target);
  if (tit != local_types.end()) {
    info.type = tit->second;
    info.is_pointerish = type_is_pointerish(info.type);
    info.is_const_storage = local_const_params.count(vd.absolute_target) > 0;
    return info;
  }

  ResolveResult rr = resolve_name(vd.absolute_target);
  if (rr.kind == ResolvedKind::ClassField && registry &&
      !current_class_name.empty()) {
    if (auto* f = registry->lookup_class_field(current_class_name,
                                               vd.absolute_target)) {
      info.type = f->type.get();
      info.is_pointerish = type_is_pointerish(info.type);
      return info;
    }
  }

  if (const auto* v = find_visible_unit_var(vd.absolute_target)) {
    info.type = v->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  if (const auto* c = find_visible_unit_const(vd.absolute_target)) {
    if (!c->type) {
      report_error(vd.loc, "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    info.type = c->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  report_error(vd.loc, "absolute target must be a variable or typed const");
  return std::nullopt;
}

std::string Emitter::open_array_type_to_cxx(const TypeExpr& t) {
  const TypeExpr* canon = canonicalize_type(&t);
  const auto& a = static_cast<const TyArray&>(*canon);
  return "::rt::tp2cc_OpenArray<" +
         (a.element ? type_to_cxx(*a.element) : std::string("int32_t")) + ">";
}

std::string Emitter::open_array_constructor_to_cxx(const SetLit& s,
                                                   const TypeExpr& param_type) {
  const TypeExpr* canon = canonicalize_type(&param_type);
  if (!canon || canon->kind != Kind::TyArray) return expr_to_cxx(s);
  const auto& arr = static_cast<const TyArray&>(*canon);
  const TypeExpr* elem_type = arr.element.get();
  if (!elem_type) return "::rt::tp2cc_open_array<int32_t>()";
  if (s.elements.empty()) return "::rt::tp2cc_open_array<" + type_to_cxx(*elem_type) + ">()";

  // Pascal reuses `[...]` for two different constructs:
  //   * set literals                -> `[a, b]`
  //   * open-array actuals in calls -> `foo([a, b])`
  // Keep the AST simple and decide here from the formal parameter type.
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      report_error(s.loc, "ranges in open-array constructors are unsupported");
      return "::rt::tp2cc_open_array<" + type_to_cxx(*elem_type) + ">()";
    }
  }

  std::string out = "::rt::tp2cc_open_array_of<" + type_to_cxx(*elem_type) + ">(";
  for (size_t i = 0; i < s.elements.size(); ++i) {
    if (i) out += ", ";
    out += const_value_to_cxx(*s.elements[i], elem_type);
  }
  out += ")";
  return out;
}

void Emitter::mark_call_param_info(
    const ProcDecl* decl, std::vector<bool>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (!decl) return;
  size_t ai = 0;
  for (const auto& p : decl->params) {
    for (size_t k = 0; k < p.names.size(); ++k) {
      if (ai < untyped_arg.size() && !p.type) untyped_arg[ai] = true;
      if (ai < mutable_ref_arg.size()) {
        mutable_ref_arg[ai] =
            p.mode == Param::Var || p.mode == Param::Out ||
            (p.mode == Param::Const && const_param_needs_mutable_ref(p.type.get()));
      }
      if (ai < param_types.size()) param_types[ai] = p.type.get();
      ++ai;
    }
  }
}

const ProcDecl* Emitter::resolve_call_decl(const Expr& callee) {
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    if (!registry) return nullptr;
    // Recursive calls: `resolve_name` rewrites the current function's
    // own name to its result slot (Pascal `f := f(...)` is the common
    // case), but for *call lowering* we still need the proc decl so
    // arg/param type info reaches `lower_call_arg`. Look up the decl
    // directly in the current unit before falling through to the
    // result-slot rewrite.
    auto current_unit_proc = [&](const std::string& name)
        -> const ProcDecl* {
      auto it = registry->units.find(current_unit_name);
      if (it == registry->units.end()) return nullptr;
      const auto* pi = it->second.find_proc(name);
      return (pi && pi->decl) ? pi->decl.get() : nullptr;
    };
    if (current_fn_is_function && !current_fn_name.empty() &&
        id.name == current_fn_name) {
      if (auto* d = current_unit_proc(id.name)) return d;
    }
    if (outer_result_type && !outer_result_name.empty() &&
        id.name == outer_result_name) {
      if (auto* d = current_unit_proc(id.name)) return d;
    }
    if (!current_class_name.empty()) {
      if (auto* m = registry->lookup_class_method(current_class_name, id.name)) {
        return m->decl.get();
      }
    }
    ResolveResult rr = resolve_name(id.name);
    return rr.proc;
  }
  if (callee.kind != Kind::Member || !registry) return nullptr;
  const auto& mem = static_cast<const Member&>(callee);
  if (mem.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*mem.base);
    if (registry->units.count(id.name)) {
      // Unit-qualified calls (`verbose.message(...)`, `cfileutils.fileexists(...)`)
      // still need the callee declaration here so trailing default arguments
      // can be materialized before we emit the C++ call.
      ResolveResult rr = resolve_name(mem.name, QualifierKind::Unit, id.name);
      return rr.proc;
    }
  }
  std::string cls;
  if (mem.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*mem.base);
    if (id.name == "self") {
      cls = current_class_name;
    } else if (registry->classes.count(id.name) ||
               registry->records.count(id.name)) {
      cls = id.name;
    } else {
      // Method calls through a variable or parameter receiver (`source.read`)
      // still need the receiver's declared Pascal type here so later call
      // lowering can recover parameter modes and any trailing defaults.
      cls = deduce_class_alias(*mem.base);
    }
  } else {
    cls = deduce_class_alias(*mem.base);
  }
  if (cls.empty()) return nullptr;
  if (auto* m = registry->lookup_class_method(cls, mem.name)) {
    return m->decl.get();
  }
  return nullptr;
}

bool Emitter::proc_accepts_zero_args(const ProcDecl& decl) {
  for (const auto& p : decl.params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    if (count != 0 && !p.default_value) return false;
  }
  return true;
}

// Pascal/FPC overload resolution conversion-rank table.
//
//   rank | name                    | example
//   -----+-------------------------+----------------------------------------
//    1   | Exact                   | tidstring -> tidstring (same canonical)
//    2   | Equal                   | TSubrangeInt -> Integer (same underlying)
//    3   | ClassHierarchy          | TButton -> TControl
//    4   | IntWideningSameSign     | byte -> word -> longint (same signedness)
//    5   | RealWidening            | single -> double -> extended
//    6   | StringSameTagWiden      | ShortString<N> -> ShortString<M>, M >= N
//    7   | StringToShortString     | Char -> ShortString; PChar -> ShortString;
//        |                         | AnsiString -> ShortString
//    8   | StringToAnsiString      | Char -> AnsiString; PChar -> AnsiString;
//        |                         | ShortString -> AnsiString;
//        |                         | ShortString/AnsiString -> PChar
//    9   | OrdinalSignChange       | longint -> longword (or back)
//   10   | Variant                 | anything <-> variant
//    -   | NotViable               | no implicit conversion exists
//
// Ranks 7 vs 8 split because Pascal under `{$H-}` (compiler-bootstrap
// default) prefers ShortString-typed parameters over AnsiString-typed
// parameters when both are otherwise tied -- e.g. `upper(PChar)` picks
// `upper(string)` over `upper(ansistring)`.
//
// `var`/`const`/`out` parameters require ranks 1..3 only (Pascal does not
// allow implicit conversion through a var/out alias).
//
// A defaulted-trailing-arg fill is rank 1 and handled at the call-site
// expansion (`append_defaulted_trailing_call_args`), not here.
Emitter::ConvScore Emitter::rank_conversion(const TypeExpr* arg,
                                            const TypeExpr* param,
                                            bool var_param) {
  if (!arg || !param) return {};
  const TypeExpr* a = canonicalize_type(arg);
  const TypeExpr* p = canonicalize_type(param);
  if (!a || !p) return {};

  auto type_text = [&](const TypeExpr* t) {
    return t ? type_to_cxx(*t) : std::string{};
  };

  // 1. Exact identity (same canonical type by C++ spelling).
  std::string a_cxx = type_text(a);
  std::string p_cxx = type_text(p);
  if (!a_cxx.empty() && a_cxx == p_cxx) return {ConvRank::Exact, 0};

  // 2. Equal-modulo-distinct/subrange. Strip TyDistinct/TySubrange wrappers
  // on both sides; if the remaining canonical types match, treat as Equal.
  auto strip_wrap = [&](const TypeExpr* t) {
    while (t && (t->kind == Kind::TyDistinct || t->kind == Kind::TySubrange)) {
      if (t->kind == Kind::TyDistinct) {
        t = canonicalize_type(
            static_cast<const TyDistinct&>(*t).underlying.get());
      } else {
        t = builtin_integer_type("longint");  // subrange -> base int
        break;
      }
    }
    return t;
  };
  const TypeExpr* a_under = strip_wrap(a);
  const TypeExpr* p_under = strip_wrap(p);
  if (a_under && p_under && type_text(a_under) == type_text(p_under)) {
    return {ConvRank::Equal, 0};
  }

  // var/const/out params: only ranks 1..3 are valid. Stop here for them
  // unless we can find a class-hierarchy match below.
  if (var_param) {
    if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
      const auto& an = static_cast<const TyName&>(*a).name;
      const auto& pn = static_cast<const TyName&>(*p).name;
      const auto* aci = class_info_for_type_name(an);
      const auto* pci = class_info_for_type_name(pn);
      if (aci && pci) {
        std::unordered_set<std::string> seen;
        std::string cur = aci->name;
        int depth = 0;
        while (!cur.empty() && !seen.count(cur)) {
          if (cur == pci->name) return {ConvRank::ClassHierarchy, depth};
          seen.insert(cur);
          auto cit = registry ? registry->classes.find(cur)
                              : decltype(registry->classes)::const_iterator{};
          if (!registry || cit == registry->classes.end()) break;
          cur = cit->second.parent;
          ++depth;
        }
      }
    }
    return {};
  }

  // 3. Class hierarchy: a derived class may pass to an ancestor parameter.
  // The fewer hops up the hierarchy, the better fit (closer ancestor wins).
  if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
    const auto* aci = class_info_for_type_name(
        static_cast<const TyName&>(*a).name);
    const auto* pci = class_info_for_type_name(
        static_cast<const TyName&>(*p).name);
    if (aci && pci) {
      std::unordered_set<std::string> seen;
      std::string cur = aci->name;
      int depth = 0;
      while (!cur.empty() && !seen.count(cur)) {
        if (cur == pci->name) return {ConvRank::ClassHierarchy, depth};
        seen.insert(cur);
        auto cit = registry ? registry->classes.find(cur)
                            : decltype(registry->classes)::const_iterator{};
        if (!registry || cit == registry->classes.end()) break;
        cur = cit->second.parent;
        ++depth;
      }
    }
  }

  auto prim_of = [&](const TypeExpr* t) -> const PrimitiveInfo* {
    if (!t || t->kind != Kind::TyName) return nullptr;
    return primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
  };

  // 4. Integer widening with same signedness. Distance is the bit-width
  // gap between source and target -- Pascal prefers the smallest target
  // that contains the source (byte->cardinal beats byte->qword).
  if (const auto* ai = prim_of(a); ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = prim_of(p);
        pi && pi->int_kind == ai->int_kind && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::IntWideningSameSign,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  // 5. Real widening (single -> double -> extended). Pascal real types
  // are floats with monotonically increasing precision in this order.
  // Distance is the rank gap so single->double beats single->extended.
  auto real_rank = [](std::string_view name) -> int {
    if (name == "single") return 1;
    if (name == "double" || name == "real") return 2;
    if (name == "extended" || name == "comp") return 3;
    return 0;
  };
  if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
    int ar = real_rank(ascii_lower(static_cast<const TyName&>(*a).name));
    int pr = real_rank(ascii_lower(static_cast<const TyName&>(*p).name));
    if (ar > 0 && pr > 0 && pr >= ar) return {ConvRank::RealWidening, pr - ar};
  }

  // String-family helpers. ShortString comes either as `TyString` (parsed
  // `string[N]`) or as a `TyName` whose canonical resolves to ShortString.
  auto is_shortstring_param = [&](const TypeExpr* t) {
    if (!t) return false;
    if (t->kind == Kind::TyString) return true;
    if (t->kind != Kind::TyName) return false;
    const auto& n = ascii_lower(static_cast<const TyName&>(*t).name);
    return n == "string" || n == "shortstring";
  };
  auto is_ansistring = [&](const TypeExpr* t) {
    return t && t->kind == Kind::TyName &&
           ascii_lower(static_cast<const TyName&>(*t).name) == "ansistring";
  };
  auto is_char = [&](const TypeExpr* t) {
    return t && t->kind == Kind::TyName &&
           ascii_lower(static_cast<const TyName&>(*t).name) == "char";
  };

  // 6. ShortString-to-ShortString (same string family). Both kinds resolve
  // to ::rt::tp2cc_ShortString<N> in C++; Pascal allows the assignment as
  // long as M >= N (truncation otherwise is a runtime concern, but Pascal
  // still accepts it). Since we don't always know N statically here, just
  // recognise the family and rank it.
  if (is_shortstring_param(a) && is_shortstring_param(p)) {
    return {ConvRank::StringSameTagWiden, 0};
  }

  // 7-8. String cross-tag conversions, split by target family. Char/PChar
  // sources and AnsiString-source converging on a ShortString param are
  // rank 7; the same sources converging on AnsiString (and any string
  // family converging on PChar) are rank 8 -- matches Pascal's preference
  // for ShortString-typed parameters under `{$H-}` semantics.
  const bool param_is_shortstring = is_shortstring_param(p);
  const bool param_is_ansistring = is_ansistring(p);
  const bool arg_is_shortstring = is_shortstring_param(a);
  const bool arg_is_ansistring = is_ansistring(a);
  if (arg_is_ansistring && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (is_char(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (type_is_pcharish(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (arg_is_shortstring && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (is_char(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (type_is_pcharish(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if ((arg_is_shortstring || arg_is_ansistring) && type_is_pcharish(p)) {
    return {ConvRank::StringToAnsiString, 0};
  }

  // 9. Ordinal signedness change (longint <-> longword, etc.). Distance
  // is the bit-width gap so byte->longint beats byte->int64.
  if (const auto* ai = prim_of(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = prim_of(p);
        pi && pi->int_kind != PrimitiveIntKind::None && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::OrdinalSignChange,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  // Variant: not really used by the bootstrap compiler, but reserve
  // the slot. Left as `NotViable` for now; real variant support would
  // need TyVariant detection in the AST.

  return {};
}

Emitter::PickResult Emitter::pick_overload(
    const std::vector<const ProcDecl*>& candidates,
    const std::vector<const Expr*>& args) {
  if (candidates.empty()) return {};
  if (candidates.size() == 1) return {candidates[0], false};

  // Filter candidates by arity (with default-arg slack), then score each
  // viable one by a per-arg conversion rank vector. Pick a strict winner:
  // ranks <= every rival at every position AND strictly < at least one.
  // If no candidate strictly dominates all rivals, the call is ambiguous;
  // signal that to the caller so it can report a Pascal-level error.
  struct Scored {
    const ProcDecl* decl;
    std::vector<ConvScore> scores;
  };
  std::vector<Scored> viable;
  for (const ProcDecl* decl : candidates) {
    if (!decl) continue;
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(decl, flat);
    if (args.size() > flat.size()) continue;
    bool ok = true;
    for (size_t i = args.size(); i < flat.size(); ++i) {
      if (!flat[i].default_value) { ok = false; break; }
    }
    if (!ok) continue;

    Scored s{decl, {}};
    s.scores.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      const TypeExpr* arg_t = deduce_type(*args[i]);
      ConvScore r = rank_conversion(arg_t, flat[i].type, flat[i].mutable_ref);
      if (!r.viable()) { ok = false; break; }
      s.scores.push_back(r);
    }
    if (ok) viable.push_back(std::move(s));
  }
  if (viable.empty()) return {};
  if (viable.size() == 1) return {viable[0].decl, false};

  // Lexicographic compare of (rank, distance): a "less" b means a is
  // a tighter fit at this arg position (better rank, or same rank with
  // smaller distance).
  auto score_less = [](const ConvScore& a, const ConvScore& b) {
    if (a.rank != b.rank) return a.rank < b.rank;
    return a.distance < b.distance;
  };
  auto score_greater = [&](const ConvScore& a, const ConvScore& b) {
    return score_less(b, a);
  };
  auto dominates = [&](const Scored& a, const Scored& b) {
    // Per-arg scores: a dominates b iff a's score is no worse at every
    // position AND strictly better at least once. With unequal arg
    // vectors (defaulted slots make `scores.size() != args.size()` for
    // some candidates) we only compare the explicit-arg portion.
    size_t n = std::min(a.scores.size(), b.scores.size());
    bool any_strict = false;
    for (size_t i = 0; i < n; ++i) {
      if (score_greater(a.scores[i], b.scores[i])) return false;
      if (score_less(a.scores[i], b.scores[i])) any_strict = true;
    }
    return any_strict;
  };
  size_t best = 0;
  for (size_t i = 1; i < viable.size(); ++i) {
    if (dominates(viable[i], viable[best])) best = i;
  }
  for (size_t i = 0; i < viable.size(); ++i) {
    if (i == best) continue;
    if (!dominates(viable[best], viable[i])) {
      // No strict winner -- two or more candidates are mutually
      // incomparable on the arg ranks. This is a Pascal-level
      // ambiguous-call error; the caller is responsible for
      // diagnosing it.
      return {nullptr, /*ambiguous=*/true};
    }
  }
  return {viable[best].decl, false};
}

Emitter::ResolvedCall Emitter::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  ResolvedCall out;
  if (callee.kind == Kind::Ident) {
    out.member_name = static_cast<const Ident&>(callee).name;
  } else if (callee.kind == Kind::Member) {
    out.member_name = static_cast<const Member&>(callee).name;
  }
  if (!registry) {
    out.decl = resolve_call_decl(callee);
    return out;
  }
  // Method overloads and free-function overloads share the SAME picker.
  // The only differences are how candidates are gathered: methods come
  // from `lookup_class_methods` (which already walks the parent chain),
  // free functions come from the unit's procs and the visible uses
  // chain. Both feed into one `all_cands` row type that downstream
  // arity filtering and conversion-rank scoring don't distinguish
  // between. (`inherited foo(...)` is a separate AST shape lowered at
  // emit time -- it does NOT route through here. Its picking is done
  // by C++ rather than by this resolver.)
  struct AnyCand {
    const ProcDecl* decl = nullptr;     // may be null for rt builtins
    size_t param_count = 0;
    bool accepts_zero_args = false;
    std::string unit;                   // empty for class methods
  };
  std::vector<AnyCand> all_cands;
  auto add_proc_cand = [&](const ProcInfo& pi, const std::string& unit) {
    all_cands.push_back({pi.decl.get(), pi.param_count,
                         pi.accepts_zero_args, unit});
  };
  auto add_method_cand = [&](const MethodSig& ms) {
    if (!ms.decl) return;  // class stubs without a decl are skipped
    all_cands.push_back({ms.decl.get(), ms.param_count,
                         ms.accepts_zero_args, {}});
  };
  auto gather_unit_procs = [&](const std::string& name,
                                const std::string* qualifier_unit) {
    auto append = [&](const std::vector<ProcInfo>* v, const std::string& u) {
      if (!v) return;
      for (const auto& pi : *v) add_proc_cand(pi, u);
    };
    if (qualifier_unit) {
      auto it = registry->units.find(*qualifier_unit);
      if (it != registry->units.end())
        append(it->second.find_export_procs(name), *qualifier_unit);
      return;
    }
    auto cur = registry->units.find(current_unit_name);
    if (cur == registry->units.end()) return;
    // Pascal scope: a decl in the current unit shadows same-named decls
    // reached through `uses`. Without this stop, an unqualified call
    // in a unit that has its own local helper would aggregate both the
    // local and the uses-imported overloads -- and if both sides have
    // a same-typed match (comphook's local `tostr(longint)` plus
    // cutils' overloaded `tostr(longint)`), the picker sees two tied
    // Exact candidates and flags the call ambiguous. Pascal's lookup
    // never reaches cutils in that case at all.
    if (auto* local = cur->second.find_procs(name); local && !local->empty()) {
      append(local, current_unit_name);
      return;
    }
    for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
         ++it) {
      auto uit = registry->units.find(*it);
      if (uit == registry->units.end()) continue;
      append(uit->second.find_export_procs(name), *it);
    }
  };
  auto gather_class_methods = [&](const std::string& cls,
                                   const std::string& name) {
    if (cls.empty()) return;
    auto* set = registry->lookup_class_methods(cls, name);
    if (!set) return;
    for (const auto& ms : *set) add_method_cand(ms);
  };
  // Receiver class for a member callee. Returns empty for callees that
  // don't have a class-typed receiver (e.g. unit-qualified or untyped).
  auto receiver_class = [&](const Expr& c) -> std::string {
    if (c.kind != Kind::Member) return {};
    const auto& mem = static_cast<const Member&>(c);
    if (mem.base->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*mem.base);
      if (id.name == "self") return current_class_name;
      if (registry->classes.count(id.name) ||
          registry->records.count(id.name)) {
        return id.name;
      }
      return deduce_class_alias(*mem.base);
    }
    return deduce_class_alias(*mem.base);
  };

  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    // Class methods of the current class take priority over unit-level
    // procs with the same name -- Pascal's normal scope order
    // (locals/enclosing -> class+ancestors -> unit -> uses).
    if (!current_class_name.empty()) {
      gather_class_methods(current_class_name, id.name);
    }
    if (all_cands.empty()) {
      gather_unit_procs(id.name, nullptr);
    }
  } else if (callee.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(callee);
    bool unit_qualified = false;
    bool inherited_call = false;
    if (mem.base->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*mem.base);
      // `inherited Foo(args)` looks up `Foo` in the PARENT class chain
      // (skipping the current class). The picker then runs the same
      // arity + conv-rank disambiguation as any other method call --
      // C++ overload resolution on `inherited::p_foo` is no longer the
      // last word, so a Pascal-disambiguated call lands on the right
      // overload even when C++ would have picked differently.
      if (id.name == "inherited" && !current_class_name.empty()) {
        inherited_call = true;
        auto cit = registry->classes.find(current_class_name);
        if (cit != registry->classes.end()) {
          std::string parent = cit->second.parent;
          if (parent.empty() && cit->second.is_reference_type) {
            parent = "tobject";
          }
          gather_class_methods(parent, mem.name);
        }
      }
      // A bare ident here can be a unit name AND a local/field name --
      // e.g. fpc's compiler unit `symtable` plus a `symtable` field on
      // tabstractrecorddef. Pascal lexical scope says locals/fields
      // shadow unit names, so try the receiver-class interpretation
      // first; only fall back to unit-qualified lookup when the ident
      // is purely a unit reference.
      bool ident_is_value =
          local_scope.count(id.name) > 0 ||
          (!current_class_name.empty() &&
           (registry->lookup_class_field(current_class_name, id.name) ||
            registry->lookup_class_property(current_class_name, id.name) ||
            registry->lookup_class_method(current_class_name, id.name)));
      if (!inherited_call && !ident_is_value &&
          registry->units.count(id.name)) {
        unit_qualified = true;
        gather_unit_procs(mem.name, &id.name);
      }
    }
    if (!inherited_call && !unit_qualified) {
      gather_class_methods(receiver_class(callee), mem.name);
    }
  }

  // Arity-filter using ProcInfo metadata so rt builtins (which have no
  // decl) participate in the filter even though they cannot be ranked
  // by the type-based picker.
  std::vector<AnyCand> arity_ok;
  for (const auto& a : all_cands) {
    if (a.decl) {
      std::vector<FlatCallParamInfo> flat;
      flatten_call_param_info(a.decl, flat);
      if (args.size() > flat.size()) continue;
      bool ok = true;
      for (size_t i = args.size(); i < flat.size(); ++i) {
        if (!flat[i].default_value) { ok = false; break; }
      }
      if (!ok) continue;
    } else {
      // rt builtin: param_count is exact; `accepts_zero_args` lets some
      // builtins (writeln/readln/halt) be called with zero args
      // regardless.
      if (args.size() == 0 && a.accepts_zero_args) {
        // ok
      } else if (args.size() != a.param_count) {
        continue;
      }
    }
    arity_ok.push_back(a);
  }

  // Helper: a chosen candidate's defining unit drives the
  // `FreeFunctionInUnit` spelling, so promote whichever AnyCand we
  // pick into the ResolvedCall struct uniformly.
  auto adopt = [&](const AnyCand& chosen, bool ran_type_picker) {
    out.decl = chosen.decl ? chosen.decl : resolve_call_decl(callee);
    out.needs_arg_casts = ran_type_picker;
    if (!chosen.unit.empty()) {
      out.shape = ResolvedCalleeKind::FreeFunctionInUnit;
      out.defining_unit = chosen.unit;
    }
  };

  if (arity_ok.size() == 1) {
    // Single arity-viable candidate -- C++ overload resolution already
    // narrows by arity, so no per-arg cast is needed. Adopt the
    // candidate (its `unit`, if any, locks the spelling to the right
    // namespace even when single-name lookup would have found a
    // different unit's version of this name).
    adopt(arity_ok[0], /*ran_type_picker=*/false);
    return out;
  }

  if (arity_ok.size() > 1) {
    // Multiple arity-viable candidates -- run the type-based picker
    // over the subset that has decls. rt builtins without decls cannot
    // be ranked, so they are excluded; if all viable candidates lack
    // decls we fall back to single-name resolution.
    std::vector<const ProcDecl*> with_decl;
    for (const auto& a : arity_ok) {
      if (a.decl) with_decl.push_back(a.decl);
    }
    if (with_decl.empty()) {
      out.decl = resolve_call_decl(callee);
      return out;
    }
    PickResult pr = pick_overload(with_decl, args);
    if (pr.ambiguous) {
      // Surface ambiguity to the caller; do NOT pick one silently.
      out.decl = nullptr;
      out.ambiguous = true;
      return out;
    }
    if (!pr.decl) {
      out.decl = resolve_call_decl(callee);
      return out;
    }
    for (const auto& a : arity_ok) {
      if (a.decl == pr.decl) {
        adopt(a, /*ran_type_picker=*/true);
        return out;
      }
    }
    // Defensive: pr.decl came from with_decl, which came from
    // arity_ok, so the loop above must have hit. Fall through anyway.
    out.decl = pr.decl;
    out.needs_arg_casts = true;
    return out;
  }

  out.decl = resolve_call_decl(callee);
  return out;
}

std::string Emitter::format_resolved_callee(
    const ResolvedCall& resolved, const Expr& callee_ast) {
  // The single source of truth for C++ callee text. Every Call branch
  // emit path goes through this -- do NOT add a parallel
  // `expr_to_cxx(callee)` call elsewhere; the two would diverge.
  if (resolved.callee_kind == ResolvedCalleeKind::FreeFunctionInUnit &&
      !resolved.defining_unit.empty()) {
    return unit_namespace_prefix(resolved.defining_unit) +
           mangle(resolved.member_name);
  }
  // Fallback: receivers, class-qualified static calls, and anything
  // the resolver classified as `Unknown` flow through the existing
  // expression formatter, which already handles
  // `instance->method`/`Class::method`/`unit::name`/with-binding. We
  // keep that path here rather than reimplementing it because it is
  // tied to the emitter's deduce_class_alias / with-stack state.
  bool prev_callee_ctx = is_callee_context_;
  is_callee_context_ = true;
  std::string text = expr_to_cxx(callee_ast);
  is_callee_context_ = prev_callee_ctx;
  return text;
}

void Emitter::flatten_call_param_info(
    const ProcDecl* decl, std::vector<FlatCallParamInfo>& flat_params) {
  flat_params.clear();
  if (!decl) return;
  for (const auto& p : decl->params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) {
      FlatCallParamInfo info;
      info.type = p.type.get();
      info.untyped = !p.type;
      info.mutable_ref =
          p.mode == Param::Var || p.mode == Param::Out ||
          (p.mode == Param::Const && const_param_needs_mutable_ref(p.type.get()));
      info.default_value = p.default_value.get();
      flat_params.push_back(info);
    }
  }
}

void Emitter::append_defaulted_trailing_call_args(
    const ProcDecl* decl, std::vector<const Expr*>& args) {
  if (!decl) return;
  std::vector<FlatCallParamInfo> flat_params;
  flatten_call_param_info(decl, flat_params);
  if (args.size() >= flat_params.size()) return;

  // Pascal trailing default parameters are compile-time sugar: the callee sees
  // an ordinary full argument list, so the emitter expands omitted suffix
  // actuals here before any later call lowering asks about parameter modes.
  for (size_t i = args.size(); i < flat_params.size(); ++i) {
    if (!flat_params[i].default_value) return;
  }
  args.reserve(flat_params.size());
  for (size_t i = args.size(); i < flat_params.size(); ++i) {
    args.push_back(flat_params[i].default_value);
  }
}

// Call emission only cares about parameter metadata for a narrow set of
// bootstrap-sensitive rewrites: untyped `var` arguments stay as raw storage
// pointers, `char` actuals may need string wrapping, and open arrays need the
// adapter type. Keep the lookup in one helper instead of restating it inside
// the giant call-expression path.
void Emitter::collect_call_param_info(
    const Expr& callee, std::vector<bool>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    mark_builtin_memory_helper_param_info(id.name, untyped_arg,
                                          mutable_ref_arg, param_types);
    mark_call_param_info(resolve_call_decl(callee), untyped_arg, mutable_ref_arg,
                         param_types);
    return;
  }
  if (callee.kind != Kind::Member) return;
  const auto& mem = static_cast<const Member&>(callee);
  if (mem.base->kind == Kind::Ident &&
      ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system") {
    mark_builtin_memory_helper_param_info(mem.name, untyped_arg,
                                          mutable_ref_arg, param_types);
  }
  mark_call_param_info(resolve_call_decl(callee), untyped_arg, mutable_ref_arg,
                       param_types);
}

std::string Emitter::lower_call_arg(const Expr& arg, const TypeExpr* param_type,
                                    bool untyped_arg,
                                    bool mutable_ref_arg) {
  if (param_type && type_is_open_array(param_type) && arg.kind == Kind::SetLit) {
    return open_array_constructor_to_cxx(static_cast<const SetLit&>(arg),
                                         *param_type);
  }
  const TypeExpr* arg_type = deduce_type(arg);
  if (arg_type) arg_type = canonicalize_type(arg_type);
  const TypeExpr* canon_param_type = canonicalize_type(param_type);
  if (mutable_ref_arg && arg.kind == Kind::Call &&
      static_cast<const Call&>(arg).args.size() == 1 &&
      static_cast<const Call&>(arg).callee->kind == Kind::Ident &&
      (canon_param_type || !untyped_arg)) {
    const auto& cast = static_cast<const Call&>(arg);
    const auto& id = static_cast<const Ident&>(*cast.callee);
    bool is_type_cast = is_primitive_type(id.name);
    if (!is_type_cast && lookup_named_type_expr(id.name)) {
      is_type_cast = true;
    }
    if (is_type_cast && expr_is_storage_lvalue(*cast.args[0])) {
      std::string ref_type_cxx;
      if (param_type) {
        ref_type_cxx = type_to_cxx(*param_type);
      } else if (is_primitive_type(id.name)) {
        ref_type_cxx = primitive_type_cxx(id.name);
      } else {
        ref_type_cxx = type_name_text_to_cxx(id.name);
      }
      return reinterpret_ref_text(ref_type_cxx, expr_to_cxx(*cast.args[0]),
                                  false);
    }
  }
  if (mutable_ref_arg && canon_param_type && arg_type &&
      expr_is_storage_lvalue(arg) &&
      type_is_pointerish(canon_param_type) &&
      type_is_pointerish(arg_type) &&
      type_to_cxx(*canon_param_type) != type_to_cxx(*arg_type)) {
    // Pascal `var`/`out` parameters alias the caller's storage slot. When
    // the slot holds a more specific pointer type than the formal parameter
    // (e.g. `var p: TNode` called with a `TBlockNode` field), C++'s normal
    // derived-to-base conversion produces an rvalue and loses that aliasing.
    // Reinterpret the slot itself so the callee still sees writable storage.
    return reinterpret_ref_text(type_to_cxx(*param_type), expr_to_cxx(arg),
                                false);
  }
  if (canon_param_type && type_is_stringish(canon_param_type)) {
    if (mutable_ref_arg && expr_is_storage_lvalue(arg)) {
      return expr_to_cxx(arg);
    }
    if (arg_type && type_is_stringish(arg_type)) {
      return expr_to_cxx(arg);
    }
    if (type_is_pcharish(arg_type)) {
      return const_value_to_cxx(arg, param_type);
    }
    if (arg.kind != Kind::StringLit && !expr_is_charish(arg)) {
      return expr_to_cxx(arg);
    }
  }
  std::string arg_text = const_value_to_cxx(arg, param_type);
  if (type_is_open_array(param_type)) {
    const TypeExpr* at = arg_type;
    if (!type_is_open_array(at)) {
      const TypeExpr* canon_param_type = canonicalize_type(param_type);
      const auto& arr = static_cast<const TyArray&>(*canon_param_type);
      const TypeExpr* elem_type = arr.element ? arr.element.get() : nullptr;
      std::string elem_cxx = elem_type ? type_to_cxx(*elem_type)
                                       : std::string("int32_t");
      arg_text = "::rt::tp2cc_open_array<" + elem_cxx + ">(" + arg_text + ")";
    }
  }
  if (!untyped_arg) return arg_text;

  // Untyped Pascal params are already lowered as "pointer to caller storage".
  // Forwarding one of them must preserve the pointer value; taking `&` here
  // would pass the address of the local pointer slot instead.
  if (arg.kind == Kind::AddrOf &&
      !static_cast<const AddrOf&>(arg).double_addr) {
    return "((void*)(" + arg_text + "))";
  }
  if (arg.kind == Kind::Ident &&
      local_untyped_params.count(static_cast<const Ident&>(arg).name)) {
    return arg_text;
  }
  return "((void*)&(" + arg_text + "))";
}

std::string Emitter::lower_implicit_zero_arg_call(const std::string& callee_text,
                                                  const ProcDecl* decl) {
  if (!decl) return callee_text + "()";

  std::vector<const Expr*> args;
  append_defaulted_trailing_call_args(decl, args);
  if (args.empty()) return callee_text + "()";

  std::vector<bool> untyped_arg(args.size(), false);
  std::vector<bool> mutable_ref_arg(args.size(), false);
  std::vector<const TypeExpr*> param_types(args.size(), nullptr);
  mark_call_param_info(decl, untyped_arg, mutable_ref_arg, param_types);

  // Bare Pascal `foo;` / `obj.meth;` can still mean a call when the omitted
  // trailing actuals all come from defaults. Rebuild that full call here so
  // implicit-call sites share the same argument lowering as explicit `Call`.
  std::string out = callee_text + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) out += ", ";
    out += lower_call_arg(*args[i], param_types[i], untyped_arg[i],
                          mutable_ref_arg[i]);
  }
  out += ")";
  return out;
}

std::string Emitter::lower_property_read(
    Location where, const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  if (auto text = maybe_property_read_text(base_cxx, class_name, prop, indices)) {
    return *text;
  }
  report_error(where, "unsupported property read accessor '" + prop.read_name + "'");
  return base_cxx + "." + mangle(prop.read_name);
}

std::optional<std::string> Emitter::maybe_property_read_text(
    const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices) {
  // Properties are Pascal-side metadata only. Reads/writes rewrite to the
  // declared backing field/getter/setter so we do not invent extra C++
  // members whose names could collide in ways Pascal itself forbids.
  if (!registry) return std::nullopt;
  const std::string access =
      (registry->classes.count(class_name) &&
       registry->classes.at(class_name).is_reference_type)
          ? "->"
          : ".";
  if (const auto* field = registry->lookup_class_field(class_name, prop.read_name)) {
    (void)field;
    std::string text = base_cxx + access + mangle(prop.read_name);
    for (const auto* idx : indices) {
      text += "[" + expr_to_cxx(*idx) + "]";
    }
    return {text};
  }
  if (const auto* method = registry->lookup_class_method(class_name, prop.read_name)) {
    (void)method;
    std::string text = base_cxx + access + mangle(prop.read_name) + "(";
    for (size_t i = 0; i < indices.size(); ++i) {
      if (i) text += ", ";
      text += expr_to_cxx(*indices[i]);
    }
    text += ")";
    return {text};
  }
  return std::nullopt;
}

std::string Emitter::lower_property_write(
    Location where, const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  if (auto text = maybe_property_write_text(base_cxx, class_name, prop, indices,
                                            value)) {
    return *text;
  }
  if (prop.write_name.empty()) {
    report_error(where, "property is read-only");
  } else {
    report_error(where, "unsupported property write accessor '" + prop.write_name +
                            "'");
  }
  return base_cxx;
}

std::optional<std::string> Emitter::maybe_property_write_text(
    const std::string& base_cxx, const std::string& class_name,
    const PropertyInfo& prop, const std::vector<const Expr*>& indices,
    const Expr& value) {
  if (!registry) return std::nullopt;
  const std::string access =
      (registry->classes.count(class_name) &&
       registry->classes.at(class_name).is_reference_type)
          ? "->"
          : ".";
  if (prop.write_name.empty()) {
    return std::nullopt;
  }
  std::string rhs = const_value_to_cxx(value, prop.type.get());
  if (const auto* field = registry->lookup_class_field(class_name, prop.write_name)) {
    (void)field;
    std::string text = base_cxx + access + mangle(prop.write_name);
    for (const auto* idx : indices) {
      text += "[" + expr_to_cxx(*idx) + "]";
    }
    return {text + " = " + rhs};
  }
  if (const auto* method = registry->lookup_class_method(class_name, prop.write_name)) {
    (void)method;
    std::string text = base_cxx + access + mangle(prop.write_name) + "(";
    bool first = true;
    for (const auto* idx : indices) {
      if (!first) text += ", ";
      text += expr_to_cxx(*idx);
      first = false;
    }
    if (!first) text += ", ";
    text += rhs;
    text += ")";
    return {text};
  }
  return std::nullopt;
}

std::string Emitter::implicit_self_cxx() {
  if (!current_class_name.empty()) {
    if (const auto* ci = class_info_for_type_name(current_class_name)) {
      return ci->is_reference_type ? "this" : "(*this)";
    }
  }
  return "(*this)";
}

std::optional<Emitter::ImplicitPropertyLookup>
Emitter::find_implicit_class_property(std::string_view name) {
  if (!registry) return std::nullopt;
  if (local_scope.count(std::string(name))) return std::nullopt;

  // Bare member names inside a method body must resolve exactly like
  // `self.Name`, including indexed/default properties. Keep the lookup
  // in one place so reads, writes, and type deduction stay aligned.
  for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
    const std::string& cls = it->class_name;
    if (cls.empty()) continue;
    if (auto* prop = registry->lookup_class_property(cls, std::string(name))) {
      return ImplicitPropertyLookup{prop, cls, it->cxx_text, true};
    }
  }

  if (current_class_name.empty()) return std::nullopt;
  if (auto* prop = registry->lookup_class_property(current_class_name,
                                                   std::string(name))) {
    return ImplicitPropertyLookup{prop, current_class_name, implicit_self_cxx(),
                                  false};
  }
  return std::nullopt;
}

std::optional<Emitter::ResolveResult> Emitter::maybe_resolve_implicit_property(
    std::string_view name) {
  auto found = find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  if (auto text = maybe_property_read_text(found->base_cxx, found->class_name,
                                           *found->prop, no_indices)) {
    ResolveResult r;
    r.kind = found->from_with ? ResolvedKind::WithProperty
                              : ResolvedKind::ClassProperty;
    r.cxx = *text;
    return r;
  }
  return std::nullopt;
}

std::optional<std::string> Emitter::maybe_lower_implicit_property_write(
    Location where, std::string_view name, const Expr& value) {
  auto found = find_implicit_class_property(name);
  if (!found || !found->prop || !found->prop->params.empty()) return std::nullopt;
  std::vector<const Expr*> no_indices;
  return lower_property_write(where, found->base_cxx, found->class_name,
                              *found->prop, no_indices, value);
}

std::optional<std::string> Emitter::maybe_lower_class_free_member(
    const Expr& base, std::string_view member_name) {
  if (member_name != "free" || !expr_is_reference_class(base)) {
    return std::nullopt;
  }
  // Pascal `obj.Free` is the null-safe TObject cleanup entrypoint, not a
  // normal instance call. Lower it to the runtime static helper so the
  // null check happens before any C++ member dispatch.
  return "::rt::p_tobject::p_free(" + expr_to_cxx(base) + ")";
}

std::optional<std::string> Emitter::maybe_lower_class_constructor_call(
    std::string_view class_name, std::string_view member_name,
    const std::vector<const Expr*>& args,
    const std::vector<const TypeExpr*>& param_types,
    const std::vector<bool>& untyped_arg,
    const std::vector<bool>& mutable_ref_arg) {
  if (!registry) return std::nullopt;
  const auto* ci = class_info_for_type_name(class_name);
  if (!ci || !ci->is_reference_type) {
    return std::nullopt;
  }
  const auto* method = registry->lookup_class_method(std::string(class_name),
                                                     std::string(member_name));
  bool implicit_root_create = false;
  if (!method || method->kind != SymKind::Constructor) {
    if (ascii_lower(std::string(member_name)) != "create" || !args.empty()) {
      return std::nullopt;
    }
    implicit_root_create = true;
  }

  std::vector<const Expr*> effective_args(args.begin(), args.end());
  std::vector<const TypeExpr*> effective_param_types(param_types.begin(),
                                                     param_types.end());
  std::vector<bool> effective_untyped_arg(untyped_arg.begin(),
                                          untyped_arg.end());
  std::vector<bool> effective_mutable_ref_arg(mutable_ref_arg.begin(),
                                              mutable_ref_arg.end());
  append_defaulted_trailing_call_args(method ? method->decl.get() : nullptr,
                                      effective_args);
  if (effective_param_types.size() < effective_args.size()) {
    effective_param_types.resize(effective_args.size(), nullptr);
    effective_untyped_arg.resize(effective_args.size(), false);
    effective_mutable_ref_arg.resize(effective_args.size(), false);
    mark_call_param_info(method ? method->decl.get() : nullptr,
                         effective_untyped_arg, effective_mutable_ref_arg,
                         effective_param_types);
  }

  // Pascal constructor calls on a class value (`TNode.Create`) allocate a
  // fresh instance and then run the constructor body on that instance. They
  // are not plain static method calls, even though the emitted C++ helper
  // itself lives on the struct type.
  std::string args_cxx;
  for (size_t i = 0; i < effective_args.size(); ++i) {
    if (i) args_cxx += ", ";
    args_cxx += lower_call_arg(*effective_args[i], effective_param_types[i],
                               effective_untyped_arg[i],
                               effective_mutable_ref_arg[i]);
  }
  std::string struct_ty = named_type_struct_cxx(class_name);
  return "([&]{ auto tp2cc_ptr = new " + struct_ty + "{}; tp2cc_ptr->" +
         (implicit_root_create ? std::string("p_create")
                               : mangle(std::string(member_name))) +
         "(" + args_cxx +
         "); return tp2cc_ptr; }())";
}

size_t procedural_param_count(const TyProcedural& p) {
  size_t count = 0;
  for (const auto& pp : p.params) {
    count += pp.names.empty() ? 1 : pp.names.size();
  }
  return count;
}

// ---------------------------------------------------------------------------
// Single-point Pascal name resolution. `resolve_name` walks the real
// Pascal lookup order and returns a `ResolveResult` every emit site
// consumes uniformly; this avoids having the same "is it a method?
// is it a unit-qualified proc? should we auto-call?" logic grow in
// three different places in the emitter.

Emitter::ResolveResult Emitter::resolve_name(
    const std::string& name, QualifierKind qk, const std::string& qualifier) {
  ResolveResult r;

  // ----- Qualified lookups first: `Unit.name` / `Class.name`. -----
  if (qk == QualifierKind::Unit) {
    r.cxx = unit_namespace_prefix(qualifier) + mangle(name);
    if (registry) {
      auto uit = registry->units.find(qualifier);
      if (uit != registry->units.end()) {
        const UnitInfo& u = uit->second;
        if (auto* pi = u.find_export_proc(name)) {
          r.kind = ResolvedKind::UnitProc;
          r.proc = pi->decl.get();
          r.is_callable = true;
          r.is_parameterless = (pi->param_count == 0);
          r.accepts_zero_args = pi->accepts_zero_args;
          r.return_type_name = pi->return_type_name;
          return r;
        }
        if (u.find_export_var(name)) { r.kind = ResolvedKind::UnitVar; return r; }
        if (u.find_export_const(name)) { r.kind = ResolvedKind::UnitConst; return r; }
        if (u.has_export_enum_member(name)) { r.kind = ResolvedKind::EnumMember; return r; }
        if (u.has_export_type(name)) { r.kind = ResolvedKind::UnitType; return r; }
      }
    }
    // RTL unit we don't parse (e.g. `dos.getenv` when dos.pas isn't in our
    // source tree). Keep the Pascal unit qualifier in the emitted text and
    // let the runtime's stub namespace alias own that lookup.
    r.kind = ResolvedKind::Unknown;
    return r;
  }
  if (qk == QualifierKind::Class) {
    if (registry) {
      if (auto* m = registry->lookup_class_method(qualifier, name)) {
        r.kind = ResolvedKind::ClassMethod;
        r.proc = m->decl.get();
        r.is_callable = true;
        r.is_parameterless = (m->param_count == 0);
        r.accepts_zero_args = m->accepts_zero_args;
        r.cxx = mangle(name);  // caller emits the `base.` prefix
        return r;
      }
      if (registry->lookup_class_field(qualifier, name)) {
        r.kind = ResolvedKind::ClassField;
        r.cxx = mangle(name);  // caller emits the `base.` prefix
        return r;
      }
    }
    r.cxx = mangle(name);
    r.kind = ResolvedKind::Unknown;
    return r;
  }

  // ----- Unqualified lookup. -----

  // 1. Function-name-as-read inside its own body -> the implicit Pascal
  //    result variable.
  if (current_fn_is_function && current_fn_result_type &&
      !current_fn_name.empty() && name == current_fn_name) {
    r.cxx = current_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (bare_result_type && is_pascal_result_ident(name)) {
    r.cxx = bare_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (outer_result_type && !outer_result_name.empty() &&
      name == outer_result_name) {
    r.cxx = outer_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  // 2. `with X do` bindings (inside-out). Fields and methods of X's
  //    class (walking ancestors) shadow outer scopes.
  if (registry) {
    for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
      const std::string& cls = it->class_name;
      const std::string& access = it->access_op;
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type &&
          (name == "classtype" || name == "instancesize")) {
        r.cxx = it->cxx_text + access + mangle(name);
        r.kind = ResolvedKind::WithMethod;
        r.is_callable = true;
        r.is_parameterless = true;
        r.accepts_zero_args = true;
        return r;
      }
      // `with obj do ... Free;` -- bare-Ident form of `obj.Free`. The
      // Member-form has its own lowering through `maybe_lower_class_free_member`;
      // mirror it here so the inherited TObject method is found via the
      // with-bound expression rather than the registry chain (TObject
      // itself is built-in, not in `registry->classes`, so a generic
      // class-method lookup dead-ends before reaching it).
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type && name == "free") {
        r.cxx = "::rt::p_tobject::p_free(" + it->cxx_text + ")";
        r.kind = ResolvedKind::WithMethod;
        // The expression is already a complete call; no implicit-zero-arg
        // wrap is wanted at the use site.
        r.is_callable = false;
        return r;
      }
      if (!cls.empty()) {
        if (auto* m = registry->lookup_class_method(cls, name)) {
          r.cxx = it->cxx_text + access + mangle(name);
          r.kind = ResolvedKind::WithMethod;
          r.proc = m->decl.get();
          r.is_callable = true;
          r.is_parameterless = (m->param_count == 0);
          r.accepts_zero_args = m->accepts_zero_args;
          return r;
        }
        if (registry->lookup_class_field(cls, name)) {
          r.cxx = it->cxx_text + access + mangle(name);
          r.kind = ResolvedKind::WithField;
          return r;
        }
      }
      if (lookup_record_field_type_in_with(*it, name)) {
        r.cxx = it->cxx_text + access + mangle(name);
        r.kind = ResolvedKind::WithField;
        return r;
      }
    }
  }
  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = local_nested_fns.find(name);
    if (nit != local_nested_fns.end()) {
      r.kind = ResolvedKind::NestedFn;
      r.cxx = mangle(name);
       r.proc = nit->second.decl;
      r.is_callable = true;
      r.is_parameterless = (nit->second.param_count == 0);
      r.accepts_zero_args = nit->second.accepts_zero_args;
      return r;
    }
  }
  // 4. Procedure-local (param, var, typed const, nested-proc-name).
  if (local_scope.count(name)) {
    r.kind = ResolvedKind::Local;
    r.cxx = mangle(name);
    return r;
  }
  for (const auto& [_, en] : local_enums) {
    if (!en) continue;
    for (const auto& member : en->members) {
      if (ascii_lower(member.name) == ascii_lower(name)) {
        r.kind = ResolvedKind::EnumMember;
        r.cxx = mangle(name);
        return r;
      }
    }
  }
  if (auto prop = maybe_resolve_implicit_property(name)) return *prop;
  // 5. Current class's members (chain).
  if (!current_class_name.empty() && registry) {
    if (const auto* ci = class_info_for_type_name(current_class_name);
        ci && ci->is_reference_type &&
        (name == "classtype" || name == "instancesize")) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassMethod;
      r.is_callable = true;
      r.is_parameterless = true;
      r.accepts_zero_args = true;
      return r;
    }
    if (auto* m = registry->lookup_class_method(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassMethod;
      r.proc = m->decl.get();
      r.is_callable = true;
      r.is_parameterless = (m->param_count == 0);
      r.accepts_zero_args = m->accepts_zero_args;
      return r;
    }
    if (registry->lookup_class_field(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassField;
      return r;
    }
    // Inline anonymous enum used as a class-field type contributes its
    // members to the enclosing class scope. C++ resolves the bare name
    // through the enclosing-class scope at the use site, so we emit
    // unqualified.
    if (registry->class_has_enum_member(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::EnumMember;
      return r;
    }
  }
  // 6. Unit-level -- own unit first, then cross-unit (`uses` chain).
  if (registry) {
    auto uit = registry->units.find(current_unit_name);
    const UnitInfo* ui = (uit != registry->units.end())
                            ? &uit->second : nullptr;
    bool own = ui && ui->has(name);
    // Current unit's own symbols shadow everything from `uses`.
    // Emit bare (C++ picks them up in the current namespace).
    if (ui) {
      if (auto* pi = ui->find_proc(name)) {
        r.cxx = mangle(name);
        r.kind = ResolvedKind::UnitProc;
        r.proc = pi->decl.get();
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        r.accepts_zero_args = pi->accepts_zero_args;
        return r;
      }
      if (ui->find_var(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitVar; return r;
      }
      if (ui->find_const(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitConst; return r;
      }
      if (ui->has_enum_member(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::EnumMember; return r;
      }
      if (ui->has_type(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitType; return r;
      }
    }
    // Cross-unit lookup: walk the current unit's `uses` list and pick
    // the first match in a unit that actually exports this name.
    // Ambiguity between same-named symbols in two `using namespace`'d
    // units is resolved by emitting the fully-qualified form.
    auto check_unit = [&](const std::string& un) -> bool {
      auto it = registry->units.find(un);
      if (it == registry->units.end()) return false;
      const UnitInfo& u = it->second;
      // Synthetic `__rt__` unit holds runtime builtins. Emit them fully
      // qualified so translated units do not depend on `using namespace
      // ::rt;` for correctness.
      const std::string prefix = unit_namespace_prefix(un);
      // Other units contribute only their interface-exported names.
      if (auto* pi = u.find_export_proc(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = (un == "__rt__") ? ResolvedKind::RtBuiltin
                                  : ResolvedKind::UnitProc;
        r.proc = pi->decl.get();
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        r.accepts_zero_args = pi->accepts_zero_args;
        r.return_type_name = pi->return_type_name;
        return true;
      }
      if (u.find_export_var(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitVar; return true;
      }
      if (u.find_export_const(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitConst; return true;
      }
      if (u.has_export_enum_member(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::EnumMember; return true;
      }
      if (u.has_export_type(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitType; return true;
      }
      return false;
    };
    if (ui) {
      // Right-to-left is Pascal's uses resolution order.
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (check_unit(*it)) return r;
      }
    }
    (void)own;  // already handled by the per-unit lookup above.
  }
  // 7. Fallback: unresolved free names are much more often runtime helpers
  //    than cross-unit symbols. Emit them as explicit `::rt::...`
  //    references instead of depending on open namespaces in generated
  //    units.
  r.cxx = "::rt::" + mangle(name);
  r.kind = ResolvedKind::Unknown;
  return r;
}

// ---------------------------------------------------------------------------
// Expressions (coarse -- just enough for constant values)

std::string Emitter::set_literal_to_cxx(const SetLit& s,
                                        const TypeExpr* target) {
  const TypeExpr* elem_type = nullptr;
  if (target) {
    const TypeExpr* canon = canonicalize_type(target);
    if (canon && canon->kind == Kind::TySet) {
      elem_type = static_cast<const TySet&>(*canon).element.get();
    }
  }

  bool has_range = false;
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      has_range = true;
      break;
    }
  }

  if (elem_type) {
    const std::string elem_cxx = type_to_cxx(*elem_type);
    if (s.elements.empty()) return "::rt::tp2cc_Set<" + elem_cxx + ">{}";
    if (!has_range) {
      // Pascal set literals inherit the surrounding set type. Make that
      // explicit in the generated C++ so `typed_set + [EnumValue]` does
      // not depend on any cross-tp2cc_Set implicit conversion.
      std::string out = "::rt::tp2cc_Set<" + elem_cxx + ">::from_list({";
      for (size_t i = 0; i < s.elements.size(); ++i) {
        if (i) out += ", ";
        out += const_value_to_cxx(*s.elements[i], elem_type);
      }
      out += "})";
      return out;
    }

    std::string body = "::rt::tp2cc_Set<" + elem_cxx + "> tp2cc_set{};";
    for (const auto& el : s.elements) {
      if (el->kind == Kind::Range) {
        const auto& r = static_cast<const Range&>(*el);
        // Pascal set elements are ordinal, so a range like ['a'..'z'] means
        // "walk the ordinal values from low to high". Iterate in integer
        // space and cast back to the set element type instead of depending on
        // wrapper types (e.g. `p_char`) to provide `++`.
        body += " for (int64_t tp2cc_value = (int64_t)(" +
                const_value_to_cxx(*r.lo, elem_type) +
                "); tp2cc_value <= (int64_t)(" +
                const_value_to_cxx(*r.hi, elem_type) +
                "); ++tp2cc_value) tp2cc_set.add(static_cast<" + elem_cxx +
                ">(tp2cc_value));";
      } else {
        body += " tp2cc_set.add(" + const_value_to_cxx(*el, elem_type) + ");";
      }
    }
    body += " return tp2cc_set;";
    const char* cap = (block_depth > 0) ? "[&]" : "[]";
    return std::string("(") + cap + "{ " + body + " }())";
  }

  if (s.elements.empty()) {
    // `[]` in Pascal: untyped empty set. EmptySet converts to any
    // `tp2cc_Set<T>` implicitly, so the value is usable in any set
    // context.
    return "::rt::EmptySet{}";
  }
  if (!has_range) {
    // Variadic-pack form so the element types don't have to
    // match exactly (Pascal set literals freely mix e.g. a
    // CharConst `p_newline` with plain char literals like
    // `'\r'`, `';'`). The tp2cc_Set's element type is deduced from
    // the first argument.
    std::string out = "::rt::set_of(";
    for (size_t i = 0; i < s.elements.size(); ++i) {
      if (i) out += ", ";
      out += expr_to_cxx(*s.elements[i]);
    }
    out += ")";
    return out;
  }

  // Slow path: mixed scalar + range elements. Build a tp2cc_Set in an IIFE
  // whose element type is deduced from the first element (either a
  // scalar value or a range low-bound). Use `[&]` inside function
  // bodies (may reference outer locals); use `[]` at namespace scope
  // where `[&]` is invalid.
  std::string first;
  if (s.elements.front()->kind == Kind::Range) {
    first = expr_to_cxx(*static_cast<const Range&>(*s.elements.front()).lo);
  } else {
    first = expr_to_cxx(*s.elements.front());
  }
  // Value-init: `tp2cc_Set` has no default member initialisers, so a
  // bare temporary would leave the bitmask uninitialised and
  // `add(...)` only sets specific bits -- every unset bit
  // would be stack garbage, making `contains()` return true for
  // arbitrary values.
  std::string body = "::rt::tp2cc_Set<decltype(" + first + ")> tp2cc_set{};";
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      const auto& r = static_cast<const Range&>(*el);
      // Untyped set literals follow the same ordinal rule; `first` supplies
      // the chosen element type once we have inferred it from context.
      body += " for (int64_t tp2cc_value = (int64_t)(" + expr_to_cxx(*r.lo) +
              "); tp2cc_value <= (int64_t)(" + expr_to_cxx(*r.hi) +
              "); ++tp2cc_value) tp2cc_set.add(static_cast<decltype(" + first +
              ")>(tp2cc_value));";
    } else {
      body += " tp2cc_set.add(" + expr_to_cxx(*el) + ");";
    }
  }
  body += " return tp2cc_set;";
  const char* cap = (block_depth > 0) ? "[&]" : "[]";
  return std::string("(") + cap + "{ " + body + " }())";
}

std::string Emitter::expr_to_cxx(const Expr& e) {
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      return uint64_literal_text(n.value);
    }
    case Kind::RealLit: {
      const auto& n = static_cast<const RealLit&>(e);
      return n.text;
    }
    case Kind::StringLit: {
      const auto& n = static_cast<const StringLit&>(e);
      // Single-character Pascal string literals are semantically chars.
      // Emit them as C++ character literals so they can appear as
      // subrange bounds (`'A'..'Z'`), case labels, and set-elements.
      // Multi-character literals are emitted as tp2cc_ShortString so that `+`
      // resolves to concatenation (not pointer arithmetic).
      if (n.value.size() == 1) {
        return "::rt::tp2cc_char_of('" + char_literal_body_to_cxx(n.value[0]) + "')";
      }
      std::string out = "::rt::tp2cc_shortstring_literal<255>(";
      bool first = true;
      for (char c : n.value) {
        if (!first) out += ", ";
        first = false;
        out += "::rt::tp2cc_char_of('";
        out += char_literal_body_to_cxx(c);
        out += "')";
      }
      out += ")";
      return out;
    }
    case Kind::NilLit: return "nullptr";
    case Kind::BoolLit: {
      const auto& n = static_cast<const BoolLit&>(e);
      return n.value ? "true" : "false";
    }
    case Kind::Ident: {
      const auto& n = static_cast<const Ident&>(e);
      if (n.name == "inherited") return "inherited{}";
      if (n.name == "self") {
        return expr_is_reference_class(e) ? "this" : "(*this)";
      }
      // LHS rewrite for `funcname := ...` assignments during Assign target
      // emission. We handle this BEFORE resolve_name so recursive
      // calls using `funcname(...)` still see the function name.
      if (!lhs_fn_rewrite.empty() && n.name == lhs_fn_rewrite) {
        return lhs_fn_rewrite_slot;
      }
      if (!lhs_outer_result_rewrite.empty() &&
          n.name == lhs_outer_result_rewrite) {
        return lhs_outer_result_rewrite_slot;
      }
      // The function-name-as-read rewrite is already in resolve_name
      // (only fires outside is_callee_context_), but we need to
      // suppress it in callee context to keep recursive call sites
      // spelled with the function's name.
      if (is_callee_context_ && current_fn_is_function &&
          !current_fn_name.empty() && n.name == current_fn_name) {
        return mangle(n.name);
      }
      // Same suppression for the *outer* function-name when emitting a
      // nested function body. Without this, a recursive call to the
      // outer function (e.g. `foreachnodestatic` calling itself from
      // inside its own nested `process_children`) gets rewritten to the
      // outer result-slot (`p_result(...)`), producing "expression
      // cannot be used as a function".
      if (is_callee_context_ && outer_result_type &&
          !outer_result_name.empty() && n.name == outer_result_name) {
        return mangle(n.name);
      }
      ResolveResult rr = resolve_name(n.name);
      if (rr.kind == ResolvedKind::UnitType) {
        if (const auto* ci = class_info_for_type_name(n.name);
            ci && ci->is_reference_type) {
          return metaclass_value_fn_cxx(n.name) + "()";
        }
        // Type aliases of reference classes (`texportlibwdosx = texportlibwin`)
        // appear in value position to mean the underlying class's metaclass.
        // Follow the alias chain to its concrete class and emit that metaclass.
        if (registry) {
          auto ait = registry->aliases.find(ascii_lower(n.name));
          if (ait != registry->aliases.end() && ait->second.target) {
            const TypeExpr* canon =
                registry->canonicalize(ait->second.target.get());
            if (canon && canon->kind == Kind::TyName) {
              const std::string& target =
                  static_cast<const TyName&>(*canon).name;
              if (const auto* tci = class_info_for_type_name(target);
                  tci && tci->is_reference_type) {
                return metaclass_value_fn_cxx(target) + "()";
              }
            }
          }
        }
      }
      // At namespace scope (block_depth == 0) we leave callable
      // names bare: Pascal typed-const initialisers reference
      // function names as procedural-pointer values.
      bool want_call = !is_callee_context_ && block_depth > 0 &&
                       rr.is_callable && rr.accepts_zero_args;
      return want_call ? lower_implicit_zero_arg_call(rr.cxx, rr.proc) : rr.cxx;
    }
    case Kind::Binary: {
      const auto& n = static_cast<const Binary&>(e);
      // Pascal operators that don't map cleanly to a C++ infix operator
      // need dedicated lowering.
      if (n.op == BinOp::In) {
        // `elem in set` -> `set.contains(elem)`
        return "(" + expr_to_cxx(*n.rhs) + ").contains(" +
               expr_to_cxx(*n.lhs) + ")";
      }
      if (n.op == BinOp::SymDiff) {
        // tp2cc_Set symmetric difference `a >< b` -> `(a + b) - (a * b)` on our
        // tp2cc_Set<> type (rt::tp2cc_Set has union/intersect/subtract overloads).
        std::string a = expr_to_cxx(*n.lhs);
        std::string b = expr_to_cxx(*n.rhs);
        return "((" + a + " + " + b + ") - (" + a + " * " + b + "))";
      }
      if (n.op == BinOp::Is) {
        // `class` names lower to pointer types, so `x is TChild` becomes a
        // pointer dynamic_cast rather than address-taking the lhs storage.
        auto rhs_type = [&]() {
          if (n.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*n.rhs);
            if (class_info_for_type_name(id.name)) {
              TyName tn;
              tn.name = id.name;
              return type_name_to_cxx(tn);
            }
          }
          return expr_to_cxx(*n.rhs);
        }();
        return "(dynamic_cast<" + rhs_type + ">(" +
               expr_to_cxx(*n.lhs) + ") != nullptr)";
      }
      if (n.op == BinOp::As) {
        auto rhs_type = [&]() {
          if (n.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*n.rhs);
            if (class_info_for_type_name(id.name)) {
              TyName tn;
              tn.name = id.name;
              return type_name_to_cxx(tn);
            }
          }
          return expr_to_cxx(*n.rhs);
        }();
        return "dynamic_cast<" + rhs_type + ">(" +
               expr_to_cxx(*n.lhs) + ")";
      }
      // Pascal `+` on `char` operands means string concatenation
      // (produces a 2-char string). C++ `char + char` is int
      // arithmetic, so wrap a char-side in tp2cc_ShortString<> to force
      // the tp2cc_ShortString `operator+` overload.
      if (n.op == BinOp::Add) {
        bool l_char = expr_is_charish(*n.lhs);
        bool r_char = expr_is_charish(*n.rhs);
        if (l_char || r_char) {
          auto wrap = [&](const Expr& x, bool want) {
            return want ? "::rt::tp2cc_shortstring_of<>(" + expr_to_cxx(x) + ")"
                        : expr_to_cxx(x);
          };
          return "(" + wrap(*n.lhs, l_char) + " + " + wrap(*n.rhs, r_char) + ")";
        }
      }
      // Pascal `and` / `or` are polymorphic: bool operands get
      // short-circuit `&&` / `||` (crucial for `assigned(p) and
      // (p^.x = y)` idioms), integer/set operands get bitwise `&` /
      // `|`. Be strict here: treating a nested flag expression like
      // `(IF_SM or IF_SM2)` as "boolean because it is an `or`" silently
      // miscompiles bitmask code into `&&`/`||`.
      std::function<bool(const Expr&)> is_bool = [&](const Expr& x) -> bool {
        // Calls to rt:: builtins and source procs: consult the resolved proc's
        // recorded return type instead of a global last-wins name map.
        if (x.kind == Kind::Call && registry) {
          const auto& c = static_cast<const Call&>(x);
          if (c.callee->kind == Kind::Ident) {
            const std::string& nm =
                static_cast<const Ident&>(*c.callee).name;
            ResolveResult rr = resolve_name(nm);
            if (rr.return_type_name == "boolean")
              return true;
          }
        }
        // Comparisons always yield bool.
        if (x.kind == Kind::Binary) {
          const auto& bx = static_cast<const Binary&>(x);
          auto bop = bx.op;
          if (bop == BinOp::Eq || bop == BinOp::NotEq ||
              bop == BinOp::Lt || bop == BinOp::Gt ||
              bop == BinOp::LtEq || bop == BinOp::GtEq ||
              bop == BinOp::In || bop == BinOp::Is)
            return true;
          if (bop == BinOp::And || bop == BinOp::Or)
            return is_bool(*bx.lhs) && is_bool(*bx.rhs);
        }
        if (x.kind == Kind::Unary &&
            static_cast<const Unary&>(x).op == UnOp::Not)
          return is_bool(*static_cast<const Unary&>(x).operand);
        if (x.kind == Kind::BoolLit) return true;
        if (!registry) return false;
        const TypeExpr* t = deduce_type(x);
        if (!t) return false;
        t = registry->canonicalize(t);
        if (!t || t->kind != Kind::TyName) return false;
        std::string nm = static_cast<const TyName&>(*t).name;
        for (auto& c : nm)
          if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return nm == "boolean" || nm == "bytebool" ||
               nm == "wordbool" || nm == "longbool";
      };
      bool logical_bool = (n.op == BinOp::And || n.op == BinOp::Or) &&
                          is_bool(*n.lhs) && is_bool(*n.rhs);
      auto emit_operand = [&](const Expr& operand, const Expr& other) {
        if (operand.kind != Kind::SetLit) return expr_to_cxx(operand);
        const TypeExpr* other_ty = deduce_type(other);
        const TypeExpr* canon = canonicalize_type(other_ty);
        if (canon && canon->kind == Kind::TySet) {
          return set_literal_to_cxx(static_cast<const SetLit&>(operand),
                                    other_ty);
        }
        return expr_to_cxx(operand);
      };
      const char* op = "?";
      switch (n.op) {
        case BinOp::Add:    op = "+"; break;
        case BinOp::Sub:    op = "-"; break;
        case BinOp::Mul:    op = "*"; break;
        case BinOp::RealDiv:op = "/"; break;
        case BinOp::IntDiv: op = "/"; break;
        case BinOp::Mod:    op = "%"; break;
        case BinOp::Shl:    op = "<<"; break;
        case BinOp::Shr:    op = ">>"; break;
        case BinOp::And:    op = logical_bool ? "&&" : "&"; break;
        case BinOp::Or:     op = logical_bool ? "||" : "|"; break;
        case BinOp::Xor:    op = "^"; break;
        case BinOp::Eq:     op = "=="; break;
        case BinOp::NotEq:  op = "!="; break;
        case BinOp::Lt:     op = "<"; break;
        case BinOp::Gt:     op = ">"; break;
        case BinOp::LtEq:   op = "<="; break;
        case BinOp::GtEq:   op = ">="; break;
        default:            op = "/*?*/"; break;
      }
      return "(" + emit_operand(*n.lhs, *n.rhs) + " " + op + " " +
             emit_operand(*n.rhs, *n.lhs) + ")";
    }
    case Kind::Unary: {
      const auto& n = static_cast<const Unary&>(e);
      if (n.op == UnOp::Not) {
        // Pascal `not` is logical for bool, bitwise for int. Dispatch
        // at compile time via a runtime helper.
        return "::rt::p_not(" + expr_to_cxx(*n.operand) + ")";
      }
      if (n.op == UnOp::Neg && n.operand &&
          n.operand->kind == Kind::IntLit &&
          static_cast<const IntLit&>(*n.operand).value ==
              (uint64_t{1} << 63)) {
        return "::std::numeric_limits<int64_t>::min()";
      }
      const char* op = (n.op == UnOp::Neg) ? "-" : "+";
      return std::string(op) + expr_to_cxx(*n.operand);
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (auto use = direct_packed_aggregate_field_use(*m.base)) {
        report_packed_aggregate_subobject_use(
            m.loc, "nested member access", *use);
      }
      // Classify the base into one of the qualifier kinds that
      // `resolve_name` understands. The base cases are:
      //   - `inherited.name`    -> class-qualified on parent alias
      //   - `Unit.name`         -> unit-qualified (Unit must be a
      //                            known unit or in the current
      //                            unit's `uses` list)
      //   - `expr.name` where   -> class-qualified on deduced type
      //     expr's type is a
      //     named class/record
      //   - otherwise           -> unknown: emit `base.name` and let
      //                            C++ member lookup do its thing.
      auto base_is_ident = [&](std::string& out) -> bool {
        if (m.base->kind != Kind::Ident) return false;
        out = static_cast<const Ident&>(*m.base).name;
        return true;
      };

      std::string base_name;
      // `inherited.foo` -- treat as class-qualified on the parent
      // alias (C++ `inherited::foo` via the in-struct `using
      // inherited = Parent;` alias).
      if (base_is_ident(base_name) && base_name == "inherited") {
        std::string parent;
        if (registry && !current_class_name.empty()) {
          auto cit = registry->classes.find(current_class_name);
          if (cit != registry->classes.end()) {
            parent = cit->second.parent;
            if (parent.empty() && cit->second.is_reference_type) {
              parent = "tobject";
            }
          }
        }
        std::string text = "inherited::" + mangle(m.name);
        if (parent.empty()) return text;
        ResolveResult rr =
            resolve_name(m.name, QualifierKind::Class, parent);
        // The implicit TObject ancestor lives in the runtime, not in the
        // Pascal registry. `inherited Create;` / `inherited Destroy;` still
        // need statement-form auto-call even though name resolution cannot
        // see their zero-argument signatures there.
        bool implicit_tobject_root =
            parent == "tobject" &&
            (ascii_lower(m.name) == "create" || ascii_lower(m.name) == "destroy");
        bool same_current_method =
            !current_fn_name.empty() &&
            ascii_lower(m.name) == ascii_lower(current_fn_name);
        bool want_call = !is_callee_context_ &&
                         ((rr.is_callable && rr.accepts_zero_args) ||
                          implicit_tobject_root || same_current_method);
        return want_call ? text + "()" : text;
      }

      // Pascal's `System` unit is implicitly used everywhere. Route
      // `System.x` straight to `::rt::x` so every builtin (delete,
      // length, copy, pos, ...) resolves without a per-method stub
      // on some `p_system` object.
      if (base_is_ident(base_name) && base_name == "system") {
        return "::rt::" + mangle(m.name);
      }
      // `Unit.name` -- only when the base ident names a known unit
      // AND isn't shadowed by any nearer binding.
      if (registry && base_is_ident(base_name)) {
        bool shadowed = local_scope.count(base_name) > 0;
        if (!shadowed && !current_class_name.empty() &&
            (registry->lookup_class_method(current_class_name, base_name) ||
             registry->lookup_class_field(current_class_name, base_name) ||
             registry->lookup_class_property(current_class_name, base_name)))
          shadowed = true;
        if (!shadowed) {
          for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
            if (with_bind_has_visible_member(*it, base_name)) {
              shadowed = true; break;
            }
          }
        }
        if (!shadowed) {
          bool is_unit = registry->units.count(base_name) > 0;
          if (!is_unit) {
            auto uit = registry->units.find(current_unit_name);
            if (uit != registry->units.end()) {
              for (const auto& nm : uit->second.uses) {
                if (nm == base_name) { is_unit = true; break; }
              }
            }
          }
          if (is_unit) {
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Unit, base_name);
            if (rr.kind == ResolvedKind::UnitType) {
              const std::string qualified = base_name + "." + m.name;
              if (const auto* ci = class_info_for_type_name(qualified);
                  ci && ci->is_reference_type) {
                return metaclass_value_fn_cxx(qualified) + "()";
              }
            }
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? rr.cxx + "()" : rr.cxx;
          }
          // `TClass.method` -- Pascal's way to call a specific
          // class's method (typically the parent's version from
          // inside an override). Emit `TClass::method`.
          if (registry->classes.count(base_name) ||
              registry->records.count(base_name)) {
            if (!is_callee_context_) {
              std::vector<const Expr*> no_args;
              std::vector<const TypeExpr*> no_param_types;
              std::vector<bool> no_untyped_arg;
              std::vector<bool> no_mutable_ref_arg;
              if (auto ctor_call = maybe_lower_class_constructor_call(
                      base_name, m.name, no_args, no_param_types,
                      no_untyped_arg, no_mutable_ref_arg)) {
                return *ctor_call;
              }
            }
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Class, base_name);
            std::string text = mangle(base_name) + "::" + mangle(m.name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? lower_implicit_zero_arg_call(text, rr.proc) : text;
          }
        }
      }

      // A `class of T` value is a pointer to a metaclass descriptor, so its
      // callable surface is the descriptor's constructor/class-method thunks
      // rather than instance fields. Emit `klass->p_create` / `klass->p_load`
      // here so ordinary call lowering can treat the result like any other
      // function pointer expression.
      if (registry) {
        const std::string metaclass = metaclass_target_name(deduce_type(*m.base));
        if (!metaclass.empty()) {
          // Member's base is an *object-position* expression, not a callee.
          // Suppress callee-context auto-call suppression while emitting it
          // so e.g. `TBaseClass(classtype).Create(...)` lowers the inner
          // `classtype` with implicit-call parens. Object-member access
          // below does the same save/false/restore dance.
          bool saved_callee = is_callee_context_;
          is_callee_context_ = false;
          std::string base_cxx = expr_to_cxx(*m.base);
          is_callee_context_ = saved_callee;
          if (const auto* method =
                  registry->lookup_class_method(metaclass, m.name)) {
            if (method->kind == SymKind::Constructor ||
                method->kind == SymKind::ClassMethod) {
              std::string text = base_cxx + "->" + mangle(m.name);
              bool want_call = !is_callee_context_ &&
                               method->accepts_zero_args;
              return want_call
                         ? lower_implicit_zero_arg_call(text, method->decl.get())
                         : text;
            }
          }
          if (ascii_lower(m.name) == "create") {
            std::string text = base_cxx + "->p_create";
            bool want_call = !is_callee_context_;
            return want_call ? text + "()" : text;
          }
          report_error(m.loc, "unsupported metaclass member '" + m.name + "'");
          return base_cxx + "->" + mangle(m.name);
        }
      }

      // Otherwise: object/record field/method access. Emit `base.name`
      // and auto-call if the deduced class has `name` as a
      // parameterless method.
      bool saved_callee = is_callee_context_;
      is_callee_context_ = false;
      std::string base_cxx = expr_to_cxx(*m.base);
      is_callee_context_ = saved_callee;
      if (!is_callee_context_) {
        if (auto free_call = maybe_lower_class_free_member(*m.base, m.name)) {
          return *free_call;
        }
      }
      std::string bcls = deduce_class_alias(*m.base);
      if (m.name == "classtype" || m.name == "instancesize") {
        const auto* ci = bcls.empty() ? nullptr : class_info_for_type_name(bcls);
        if ((ci && ci->is_reference_type) || expr_is_reference_class(*m.base)) {
          // Property/default-index results like `Items[i]` may already have
          // the right Pascal class alias even when the raw expression
          // no longer looks like a plain class lvalue. Recover the dynamic
          // class query from that alias so `Items[i].ClassType` still lowers
          // to an object-side method call.
          const std::string access =
              (ci && ci->is_reference_type) ? "->" : member_access_op(*m.base);
          std::string text = base_cxx + access + mangle(m.name);
          return is_callee_context_ ? text : text + "()";
        }
      }
      if (registry && !bcls.empty()) {
        if (auto* prop = registry->lookup_class_property(bcls, m.name)) {
          if (prop->params.empty()) {
            std::vector<const Expr*> no_indices;
            return lower_property_read(m.loc, base_cxx, bcls, *prop, no_indices);
          }
        }
      }
      std::string text = base_cxx + member_access_op(*m.base) + mangle(m.name);
      if (is_callee_context_ || !registry) return text;
      if (bcls.empty()) return text;
      if (const auto* method = registry->lookup_class_method(bcls, m.name)) {
        if (method->accepts_zero_args) {
          text = lower_implicit_zero_arg_call(text, method->decl.get());
        }
      }
      return text;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      // `::rt::tp2cc_deref(p)` is equivalent to `*p` for typed pointers and
      // yields `char&` for `void*` so Pascal `ptr^` on untyped pointers
      // still compiles.
      return "::rt::tp2cc_deref(" + expr_to_cxx(*d.operand) + ")";
    }
    case Kind::AddrOf: {
      const auto& a = static_cast<const AddrOf&>(e);
      // `@TClass.method` is an unbound-method pointer in Pascal; the
      // compiler subset we handle lowers that to the thunk code slot,
      // not to a C++ member-function pointer. Detect the AST pattern
      // `AddrOf(Member(Ident=TypeName, method))` where TypeName is a
      // known class/record alias in the registry.
      if (a.operand && a.operand->kind == Kind::Member) {
        const auto& m = static_cast<const Member&>(*a.operand);
        if (m.base && m.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*m.base);
          if (registry && (registry->classes.count(id.name) ||
                           registry->records.count(id.name))) {
            if (auto* method = registry->lookup_class_method(id.name, m.name);
                method && method->decl && !method->decl->is_class_method) {
              return "::rt::tp2cc_method_code<&" + mangle(id.name) + "::" +
                     method_pointer_helper_name(*method->decl) + ">()";
            }
          }
        }
      }
      // `@X` where X is a Pascal untyped-var parameter: X is already
      // `void*` holding the caller's storage address, so the
      // address-of-X is just X itself.
      if (a.operand && a.operand->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*a.operand);
        if (local_untyped_params.count(id.name)) {
          return "(" + mangle(id.name) + ")";
        }
      }
      bool saved = is_callee_context_;
      is_callee_context_ = true;
      std::string inner = expr_to_cxx(*a.operand);
      is_callee_context_ = saved;
      // Pascal `@arr` where `arr` is a flat byte-array (`array of
      // char` / `array of byte`) typically lands in a `pchar` or
      // `pointer` context -- the fpc compiler's fill buffers and
      // inline byte tables do exactly this. For that narrow case
      // emit `(::rt::p_char*)arr` using `rt::tp2cc_Array<byte>`'s pointer
      // decay. Anything deeper than one array level (e.g.
      // `array of array of char`) stays as `&arr` and the source
      // is expected to use a flatter spelling -- we do not paper
      // over nested-array type-punning at the translator level.
      if (registry) {
        const TypeExpr* ot = deduce_type(*a.operand);
        if (ot) ot = registry->canonicalize(ot);
        if (ot && ot->kind == Kind::TyArray) {
          const auto& ar = static_cast<const TyArray&>(*ot);
          const TypeExpr* elem = ar.element.get();
          if (elem) elem = registry->canonicalize(elem);
          if (elem && elem->kind == Kind::TyName) {
            std::string en = ascii_lower(static_cast<const TyName&>(*elem).name);
            if (en == "byte" || en == "char" || en == "uint8_t" ||
                en == "shortint") {
              return "((::rt::p_char*)(" + inner + "))";
            }
          }
        }
      }
      return "(&" + inner + ")";
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Only three Pascal builtins need special emit-time handling -- the
      // rest (length, ord, chr, assigned, odd, abs, sqr, sqrt, sin, cos,
      // ln, exp, arctan, trunc, round, int, frac, inc, dec, succ, pred,
      // ...) live in `rt::` under their exact Pascal names and pass through
      // ordinary name resolution as explicit `::rt::...` calls.
      //
      // Special cases below:
      //   * `low(T)` / `high(T)` when T is a type name  -> emitted constant
      //   * `sizeof(x)`                                 -> C++ `sizeof`
      //   * `TypeName(expr)` function-style cast        -> paren-cast when
      //                                                   the C++ type is
      //                                                   compound
      //   * `new(...)` / `dispose(...)`                 -> placement form
      // `system.low(...)` / `system.high(...)`: Pascal sometimes spells
      // these intrinsics with an explicit `system.` qualifier. System is
      // the implicit unit, so semantically these are identical to the
      // bare-name form -- forward to the same low/high lowering instead
      // of falling through to a runtime call.
      if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident &&
            ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system" &&
            (mem.name == "low" || mem.name == "high") && c.args.size() == 1) {
          const bool want_low = (mem.name == "low");
          if (c.args[0]->kind == Kind::Ident) {
            const auto& a = static_cast<const Ident&>(*c.args[0]);
            if (std::string rewrite =
                    low_high_expr_for_named_type(a.name, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
          if (const TypeExpr* at = deduce_type(*c.args[0])) {
            if (std::string rewrite = low_high_expr_for_type(at, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
        }
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        const std::string& n = id.name;
        auto arg0 = [&] {
          return c.args.empty() ? std::string("0") : expr_to_cxx(*c.args[0]);
        };
        auto is_visible_type_name = [&](const std::string& type_name) {
          const std::string low = ascii_lower(type_name);
          if (is_primitive_type(low)) return true;
          if (!runtime_named_type_cxx(low).empty()) return true;
          if (!builtin_reference_class_struct_cxx(low).empty()) {
            return true;
          }
          if (local_type_aliases_scoped.count(low) || local_enums.count(low)) {
            return true;
          }
          if (ResolveResult rr = resolve_name(type_name);
              rr.kind == ResolvedKind::UnitType) {
            return true;
          }
          if (registry) {
            return registry->classes.count(low) ||
                   registry->records.count(low) ||
                   registry->enums.count(low) ||
                   registry->aliases.count(low);
          }
          return false;
        };

        // Pascal `low` / `high` are type-driven:
        //   `high(longint)`   -> max value of the type
        //   `high(a)`         -> max value of a's type
        //   `high(arr)`       -> last array index of arr's type
        // so lower them from the resolved Pascal type rather than leaving
        // a raw runtime call in the generated C++.
        if ((n == "low" || n == "high") && c.args.size() == 1) {
          const bool want_low = (n == "low");
          if (c.args[0]->kind == Kind::Ident) {
            const auto& a = static_cast<const Ident&>(*c.args[0]);
            if (std::string rewrite =
                    low_high_expr_for_named_type(a.name, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
          const TypeExpr* at = deduce_type(*c.args[0]);
          if (at) {
            if (std::string rewrite = low_high_expr_for_type(at, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
            const TypeExpr* canon = canonicalize_type(at);
            if (canon && canon->kind == Kind::TyArray) {
              const auto& arr = static_cast<const TyArray&>(*canon);
              if (arr.array_kind != ArrayKind::Fixed) {
                return want_low ? "0"
                                : "(::rt::p_length(" + expr_to_cxx(*c.args[0]) +
                                      ") - 1)";
              }
              return type_to_cxx(*at) + "::" + n + "()";
            }
          }
        }

        if (n == "sizeof" && c.args.size() == 1) {
          // Pascal `sizeof` returns `longint`, but C++ `sizeof` is
          // `size_t` (typically `unsigned int` or `unsigned long`). Wrap
          // the result so arithmetic and overload resolution at the call
          // site see the Pascal-correct signed int32_t -- otherwise e.g.
          // `tostr(sizeof(aint))` ends up ambiguous against the
          // qword/int64/longint overload set.
          std::string inner;
          if (c.args[0]->kind == Kind::Ident) {
            const auto& tn = static_cast<const Ident&>(*c.args[0]);
            if (is_visible_type_name(tn.name)) {
              inner = "sizeof(" + type_name_text_to_cxx(tn.name) + ")";
            }
          }
          if (inner.empty()) inner = "sizeof(" + expr_to_cxx(*c.args[0]) + ")";
          return "static_cast<int32_t>(" + inner + ")";
        } else if (n == "typeof" && c.args.size() == 1 &&
                   c.args[0]->kind == Kind::Ident && registry) {
          // Pascal `typeof(T)` takes a TYPE NAME, not a value. In C++
          // we have no VMT-by-type-name runtime object; stub as
          // `nullptr` with a dummy template-arg tag so the expression
          // at least compiles. Users of this value compare it for
          // equality/inequality at runtime only.
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          if (registry->classes.count(a.name) ||
              registry->records.count(a.name)) {
            return "((void*)nullptr)";
          }
        } else if (c.args.size() == 1 && is_primitive_type(n)) {
          // Function-style type cast in expression context.
          // Only the explicit lvalue forms handled elsewhere
          // (`T(lv) := ...`, `inc(T(lv))`, `dec(T(lv))`) reinterpret
          // storage. Plain `T(expr)` remains a value conversion.
          if (n == "ansistring" || n == "utf8string") {
            return "::rt::tp2cc_ansistring_of(" + arg0() + ")";
          }
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          if (peeled && expr_is_untyped_storage_ref(*c.args[0])) {
            return "::rt::tp2cc_reinterpret_load<" + primitive_type_cxx(n) +
                   ">(" + expr_to_cxx(*peeled) + ")";
          }
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            const TypeExpr* source_ty = canonicalize_type(deduce_type(*peeled));
            if (source_ty &&
                (source_ty->kind == Kind::TyArray ||
                 source_ty->kind == Kind::TyRecord ||
                 source_ty->kind == Kind::TyObject ||
                 source_ty->kind == Kind::TyProcedural)) {
              // Aggregate-to-primitive typecasts in Pascal are byte
              // reinterpretations, not numeric conversions. `double(MathInf)`
              // in the compiler sources depends on preserving the byte pattern.
              return "::rt::tp2cc_reinterpret_copy<" + primitive_type_cxx(n) +
                     ">(" + expr_to_cxx(*peeled) + ")";
            }
          }
          if (n == "char") {
            return "::rt::p_chr(" + arg0() + ")";
          }
          if (n == "pointer" || n == "pchar" || n == "ppchar") {
            if (peeled && expr_is_storage_lvalue(*c.args[0])) {
              return "((" + primitive_type_cxx(n) + ")(" +
                     expr_to_cxx(*peeled) + "))";
            }
          }
          if (expr_is_charish(*c.args[0])) {
            return "((" + primitive_type_cxx(n) + ")(::rt::p_ord(" +
                   arg0() + ")))";
          }
          TyName target;
          target.name = n;
          if (auto lit =
                  maybe_convert_const_int_expr(*c.args[0], &target, true)) {
            return *lit;
          }
          return "((" + primitive_type_cxx(n) + ")(" + arg0() + "))";
        } else if (c.args.size() == 1 && n != "inc" && n != "dec") {
          if (is_builtin_reference_class_name(n)) {
            // `TObject(expr)` is a pointer cast even though `TObject` itself
            // comes from the runtime root instead of the registry.
            TyName cast_name;
            cast_name.name = n;
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          if (registry) {
            auto cit = registry->classes.find(n);
            if (cit != registry->classes.end() && cit->second.is_reference_type) {
              // Direct named class casts (`TNode(p)`) do not go through the
              // alias table, because reference classes are registered as
              // classes rather than aliases. Treat them as pointer casts here.
              TyName cast_name;
              cast_name.name = n;
              return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
            }
          }
          const TypeExpr* cast_ty = nullptr;
          auto lit = local_type_aliases_scoped.find(n);
          if (lit != local_type_aliases_scoped.end()) {
            cast_ty = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(n);
            if (ait != registry->aliases.end() && ait->second.target.get()) {
              cast_ty = canonicalize_type(ait->second.target.get());
            }
          }
          if (cast_ty && cast_ty->kind == Kind::TyMetaclass) {
            return "((" + type_name_text_to_cxx(n) + ")(" +
                   const_value_to_cxx(*c.args[0], cast_ty,
                                      /*explicit_conversion=*/true) +
                   "))";
          }
          if (cast_ty && cast_ty->kind == Kind::TySet) {
            return "::rt::tp2cc_set_cast<" + type_to_cxx(*cast_ty) + ">(" +
                   arg0() + ")";
          }
          if (cast_ty && type_is_reference_class(cast_ty)) {
            // `TClass(expr)` is a class-pointer cast in Pascal, not a C++
            // direct-initialisation attempt. Emit an explicit pointer cast so
            // bootstrap casts like `TLinkedListItemClass(ClassType)` keep
            // pointer semantics instead of turning into constructor calls.
            TyName cast_name;
            cast_name.name = n;
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          bool named_storage_view_type =
              registry && (registry->records.count(n) || registry->classes.count(n));
          if (cast_ty && peeled && expr_is_storage_lvalue(*c.args[0]) &&
              (cast_ty->kind == Kind::TyArray ||
               cast_ty->kind == Kind::TyRecord ||
               cast_ty->kind == Kind::TyObject ||
               cast_ty->kind == Kind::TyProcedural)) {
            const TypeExpr* source_ty = deduce_type(*peeled);
            bool pointee_view =
                expr_is_untyped_storage_ref(*c.args[0]) ||
                type_is_pointerish(source_ty);
            return reinterpret_ref_text(type_name_text_to_cxx(n),
                                        expr_to_cxx(*peeled),
                                        pointee_view);
          }
          if (named_storage_view_type && peeled &&
              expr_is_storage_lvalue(*c.args[0])) {
            const TypeExpr* source_ty = deduce_type(*peeled);
            bool pointee_view =
                expr_is_untyped_storage_ref(*c.args[0]) ||
                type_is_pointerish(source_ty);
            return reinterpret_ref_text(type_name_text_to_cxx(n),
                                        expr_to_cxx(*peeled),
                                        pointee_view);
          }
          if (cast_ty && cast_ty->kind == Kind::TyArray) {
            const auto& arr = static_cast<const TyArray&>(*cast_ty);
            const TypeExpr* elem =
                arr.element ? canonicalize_type(arr.element.get()) : nullptr;
            if (arr.dims.size() == 1 &&
                (tyname_is(elem, "byte") || tyname_is(elem, "char"))) {
              return "::rt::tp2cc_reinterpret_bytes<" +
                     type_name_text_to_cxx(n) + ">(" + arg0() + ")";
            }
          }
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2) &&
                   c.args[0]->kind == Kind::Call) {
          // Pascal `inc(T(lv))` / `dec(T(lv))` mutate the storage behind
          // `lv` as type T. Emit that reinterpreting lvalue explicitly.
          const auto& inner = static_cast<const Call&>(*c.args[0]);
          if (std::string ptr = primitive_cast_untyped_storage_ptr(inner);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*inner.callee);
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ", " +
                     expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ")";
          }
          // `inc(T(packed_record.field))`: the packed field cannot be
          // bound as `T&`, so route through the same memcpy-based path as
          // untyped storage. The address-of is byte-safe because
          // `tp2cc_reinterpret_inc` reads/writes via memcpy.
          if (std::string ptr = primitive_cast_packed_field_ptr(inner);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*inner.callee);
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ", " +
                     expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ")";
          }
          if (std::string ref = primitive_cast_lvalue_ref(inner);
              !ref.empty()) {
            std::string op = (n == "inc") ? "::rt::p_inc" : "::rt::p_dec";
            if (c.args.size() == 2) {
              return op + "(" + ref + ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "(" + ref + ")";
          }
          // Fall through to generic emission.
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2)) {
          // `inc(packed_record.field)` without an outer typed cast: same
          // packed-field problem as the cast case above, but the operand
          // type is the field's own declared type rather than a cast type.
          if (auto storage = packed_field_storage_ref(*c.args[0])) {
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + storage->elem_cxx + ">(" + storage->void_ptr_text +
                     ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + storage->elem_cxx + ">(" + storage->void_ptr_text + ")";
          }
          // Fall through to generic emission for non-packed scalar args.
        } else if (n == "new" && !c.args.empty()) {
          // Expression-form `new(T)` or `new(T, Ctor(args))`. The first
          // arg is the *pointer-type name* (an Ident), which we already
          // emit as `p_T` -- the underlying struct is
          // `std::remove_pointer_t<p_T>`.
          // STUB: if the type is one of our stub target-back-end
          // aliases (t_win32 / t_os2 / t_go32v* classes that got
          // skipped), emit `nullptr` -- the call site is inside an
          // unreachable `case target_info.target of` arm.
          if (c.args[0]->kind == Kind::Ident) {
            const std::string& tname =
                static_cast<const Ident&>(*c.args[0]).name;
            static const std::unordered_set<std::string> stub_targets = {
                "pimportlibwin32", "timportlibwin32",
                "pimportlibos2",   "timportlibos2",
                "pimportlibgo32v2","timportlibgo32v2",
                "pexportlibwin32", "texportlibwin32",
                "pexportlibos2",   "texportlibos2",
                "pexportlibgo32v2","texportlibgo32v2",
                "plinkerwin32",    "tlinkerwin32",
                "plinkeros2",      "tlinkeros2",
                "plinkergo32v1",   "tlinkergo32v1",
                "plinkergo32v2",   "tlinkergo32v2",
            };
            if (stub_targets.count(tname)) return "nullptr";
          }
          std::string t = expr_to_cxx(*c.args[0]);
          std::string make =
              "([&]{ auto tp2cc_ptr = static_cast<::std::remove_pointer_t<" +
              t + ">*>(nullptr); ::rt::p_new(tp2cc_ptr); return tp2cc_ptr; }())";
          if (c.args.size() == 1) return make;
          // c.args[1] is either Call(Ctor, args) or Ident(Ctor).
          std::string method;
          std::string margs;
          const auto& second = *c.args[1];
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            const ProcDecl* ctor_decl = nullptr;
            if (registry && c.args[0]->kind == Kind::Ident &&
                cc.callee->kind == Kind::Ident) {
              TyName ptr_type;
              ptr_type.name = static_cast<const Ident&>(*c.args[0]).name;
              std::string pointee = registry->pointer_target_type_name(&ptr_type);
              if (!pointee.empty()) {
                if (auto* m = registry->lookup_class_method(
                        pointee, static_cast<const Ident&>(*cc.callee).name)) {
                  ctor_decl = m->decl.get();
                }
              }
            }
            std::vector<const Expr*> ctor_args;
            ctor_args.reserve(cc.args.size());
            for (const auto& arg : cc.args) ctor_args.push_back(arg.get());
            append_defaulted_trailing_call_args(ctor_decl, ctor_args);
            std::vector<bool> untyped_arg(ctor_args.size(), false);
            std::vector<bool> mutable_ref_arg(ctor_args.size(), false);
            std::vector<const TypeExpr*> param_types(ctor_args.size(), nullptr);
            mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                 param_types);
            for (size_t i = 0; i < ctor_args.size(); ++i) {
              if (i) margs += ", ";
              margs += lower_call_arg(*ctor_args[i], param_types[i],
                                      untyped_arg[i], mutable_ref_arg[i]);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          return "([&]{ auto tp2cc_ptr = " + make + "; tp2cc_ptr->" +
                 method + "(" + margs + "); return tp2cc_ptr; }())";
        }
      }
      // Pointer cast `T(lv)` where T resolves to a pointer type AND
      // the argument is an addressable expression (Ident, Member,
      // Index, Deref): emit `(*(T*)&(lv))` so the result is an lvalue
      // and can bind to a `var`-parameter reference. Pascal routinely
      // casts pointer storage this way
      // (e.g. `resolvederef(pderef(def), ...)`). If the argument is
      // an rvalue (a call result, arithmetic, another cast, or a
      // parameterless-method access like `inherited.name` which the
      // emitter silently calls), a plain functional cast works and
      // an address-of would not compile.
      if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        bool cast_to_pointer = false;
        std::string cast_type_cxx;
        if (id.name == "pointer") {
          cast_to_pointer = true;
          cast_type_cxx = "void*";
        } else {
          const TypeExpr* tgt = nullptr;
          auto lit = local_type_aliases_scoped.find(id.name);
          if (lit != local_type_aliases_scoped.end() && lit->second) {
            tgt = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(id.name);
            if (ait != registry->aliases.end() && ait->second.target.get()) {
              tgt = registry->canonicalize(ait->second.target.get());
            }
          }
          if (tgt && tgt->kind == Kind::TyPointer) {
            cast_to_pointer = true;
            cast_type_cxx = type_name_text_to_cxx(id.name);
          }
        }
        if (cast_to_pointer) {
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            return "((" + cast_type_cxx + ")(" + expr_to_cxx(*peeled) + "))";
          }
          return "((" + cast_type_cxx + ")(" + expr_to_cxx(*c.args[0]) + "))";
        }
      }
      if (c.args.size() == 1 && c.callee->kind == Kind::Member && registry) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident) {
          const auto& base_id = static_cast<const Ident&>(*mem.base);
          const std::string& base_name = base_id.name;
          bool shadowed = local_scope.count(base_name) > 0;
          if (!shadowed && !current_class_name.empty() &&
              (registry->lookup_class_method(current_class_name, base_name) ||
               registry->lookup_class_field(current_class_name, base_name) ||
               registry->lookup_class_property(current_class_name, base_name))) {
            shadowed = true;
          }
          if (!shadowed) {
            for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
              if (with_bind_has_visible_member(*it, base_name)) {
                shadowed = true;
                break;
              }
            }
          }
          bool is_unit = !shadowed && registry->units.count(base_name) > 0;
          if (!is_unit && !shadowed) {
            auto uit = registry->units.find(current_unit_name);
            if (uit != registry->units.end()) {
              for (const auto& nm : uit->second.uses) {
                if (nm == base_name) {
                  is_unit = true;
                  break;
                }
              }
            }
          }
          if (is_unit) {
            ResolveResult rr =
                resolve_name(mem.name, QualifierKind::Unit, base_name);
            if (rr.kind == ResolvedKind::UnitType) {
              const std::string qualified = base_name + "." + mem.name;
              const TypeExpr* cast_ty = lookup_named_type_expr(qualified);
              if (cast_ty) cast_ty = canonicalize_type(cast_ty);
              if (cast_ty && cast_ty->kind == Kind::TyPointer) {
                const Expr* peeled = peel_primitive_casts(c.args[0].get());
                if (peeled && expr_is_storage_lvalue(*c.args[0])) {
                  return "((" + type_name_text_to_cxx(qualified) + ")(" +
                         expr_to_cxx(*peeled) + "))";
                }
              }
            }
          }
        }
      }
      // Build the explicit-args list first so overload resolution can score
      // candidates by their static types; defaults are filled per-overload
      // after the pick.
      std::vector<const Expr*> call_args;
      call_args.reserve(c.args.size());
      for (const auto& arg : c.args) call_args.push_back(arg.get());
      ResolvedCall resolved = resolve_call(*c.callee, call_args);
      if (resolved.ambiguous) {
        // Pascal-level ambiguous call: two or more overloads were
        // mutually incomparable on the conversion-rank vector. Report
        // and emit a placeholder so the build fails loudly rather than
        // silently picking one and hoping C++ figures it out.
        std::string name;
        if (c.callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*c.callee).name;
        } else if (c.callee->kind == Kind::Member) {
          name = static_cast<const Member&>(*c.callee).name;
        }
        report_error(c.loc,
                     "ambiguous call to overloaded '" + name +
                     "': no candidate dominates on argument conversions");
        return "/* ambiguous call to '" + name + "' */";
      }
      const ProcDecl* call_decl = resolved.decl;
      append_defaulted_trailing_call_args(call_decl, call_args);
      std::vector<bool> call_untyped_arg(call_args.size(), false);
      std::vector<bool> call_mutable_ref_arg(call_args.size(), false);
      std::vector<const TypeExpr*> call_param_types(call_args.size(), nullptr);
      if (call_decl) {
        // Use the resolver's picked decl directly so per-arg types match
        // exactly the overload we are landing on. The builtin-helper hook
        // (move/fillchar/etc.) still runs because it overrides specific
        // slots that the decl-based path leaves null.
        if (c.callee->kind == Kind::Ident) {
          mark_builtin_memory_helper_param_info(
              static_cast<const Ident&>(*c.callee).name,
              call_untyped_arg, call_mutable_ref_arg, call_param_types);
        } else if (c.callee->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*c.callee);
          if (mem.base->kind == Kind::Ident &&
              ascii_lower(static_cast<const Ident&>(*mem.base).name) ==
                  "system") {
            mark_builtin_memory_helper_param_info(
                mem.name, call_untyped_arg, call_mutable_ref_arg,
                call_param_types);
          }
        }
        mark_call_param_info(call_decl, call_untyped_arg,
                             call_mutable_ref_arg, call_param_types);
      } else {
        collect_call_param_info(*c.callee, call_untyped_arg,
                                call_mutable_ref_arg, call_param_types);
      }
      if (c.args.empty() && c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (auto free_call = maybe_lower_class_free_member(*mem.base, mem.name)) {
          return *free_call;
        }
      }
      if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          auto cit = registry->classes.find(id.name);
          if (cit != registry->classes.end()) {
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    id.name, mem.name, call_args, call_param_types,
                    call_untyped_arg, call_mutable_ref_arg)) {
              return *ctor_call;
            }
          }
        }
      }
      std::string callee_text = format_resolved_callee(resolved, *c.callee);
      bool is_tpexcept_setjmp = false;
      if (c.args.size() == 1) {
        if (c.callee->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*c.callee);
          if (id.name == "setjmp") {
            ResolveResult rr = resolve_name(id.name);
            is_tpexcept_setjmp = (rr.cxx == "p_tpexcept::p_setjmp");
          }
        } else if (c.callee->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*c.callee);
          if (mem.name == "setjmp" && mem.base->kind == Kind::Ident &&
              static_cast<const Ident&>(*mem.base).name == "tpexcept") {
            is_tpexcept_setjmp = true;
          }
        }
      }
      if (is_tpexcept_setjmp) {
        return "setjmp(p_tpexcept::p_detail::p_state_for(&(" +
               expr_to_cxx(*c.args[0]) + ")).p_env)";
      }
      std::string out = callee_text + "(";
      for (size_t i = 0; i < call_args.size(); ++i) {
        if (i) out += ", ";
        std::string arg_text = lower_call_arg(*call_args[i], call_param_types[i],
                                              call_untyped_arg[i],
                                              call_mutable_ref_arg[i]);
        // For overloaded callees, force the C++ compiler onto the picked
        // overload by casting every value-arg to the picked param's type.
        // C++ ranks competing implicit conversions equally in many cases
        // (ShortString-to-ShortString vs ShortString-to-AnsiString;
        // uint32->uint64 vs uint32->int32) so without this cast the C++
        // call is ambiguous even though Pascal already chose. Skip for
        // var/const/out (the call site passes the storage as-is), for
        // untyped params (no concrete C++ type to cast to), and for
        // procedural-type params (`static_cast<funcptr>(value)` is
        // ill-formed and overload resolution against a function-pointer
        // slot does not produce ambiguity with value-type overloads).
        if (resolved.needs_arg_casts && call_param_types[i] &&
            !call_mutable_ref_arg[i] && !call_untyped_arg[i]) {
          const TypeExpr* canon_pt = canonicalize_type(call_param_types[i]);
          if (!canon_pt || canon_pt->kind != Kind::TyProcedural) {
            arg_text = "static_cast<" + type_to_cxx(*call_param_types[i]) +
                       ">(" + arg_text + ")";
          }
        }
        out += arg_text;
      }
      out += ")";
      return out;
    }
    case Kind::Index: {
      const auto& i = static_cast<const Index&>(e);
      if (auto use = direct_packed_aggregate_field_use(*i.base);
          use && !type_is_byte_aligned_packed_index_carrier(deduce_type(*i.base))) {
        report_packed_aggregate_subobject_use(i.loc, "indexing", *use);
      }
      std::vector<const Expr*> indices;
      for (const auto& idx : i.indices) indices.push_back(idx.get());
      if (registry && i.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*i.base);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
            if (!prop->params.empty()) {
              return lower_property_read(i.loc, expr_to_cxx(*mem.base), cls,
                                         *prop, indices);
            }
          }
        }
      }
      if (registry && i.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*i.base);
        if (auto found = find_implicit_class_property(id.name);
            found && found->prop && !found->prop->params.empty()) {
          return lower_property_read(i.loc, found->base_cxx, found->class_name,
                                     *found->prop, indices);
        }
      }
      if (registry) {
        std::string cls = deduce_class_alias(*i.base);
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_default_property(cls)) {
            return lower_property_read(i.loc, expr_to_cxx(*i.base), cls, *prop,
                                       indices);
          }
        }
      }
      if (auto view = untyped_storage_index_view(i)) {
        return "::rt::tp2cc_reinterpret_load<" + view->elem_cxx + ">(" +
               view->ptr_cxx + ")";
      }
      std::string out = expr_to_cxx(*i.base);
      for (const auto& idx : i.indices) out += "[" + expr_to_cxx(*idx) + "]";
      return out;
    }
    case Kind::SetLit: {
      return set_literal_to_cxx(static_cast<const SetLit&>(e));
    }
    case Kind::Range: {
      const auto& r = static_cast<const Range&>(e);
      return "::rt::range(" + expr_to_cxx(*r.lo) + ", " + expr_to_cxx(*r.hi) + ")";
    }
    case Kind::ArrayConst: {
      const auto& a = static_cast<const ArrayConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < a.elements.size(); ++i) {
        if (i) out += ", ";
        out += expr_to_cxx(*a.elements[i]);
      }
      out += "}";
      return out;
    }
    case Kind::RecordConst: {
      const auto& r = static_cast<const RecordConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < r.fields.size(); ++i) {
        if (i) out += ", ";
        out += "." + mangle(r.fields[i].first) + " = " +
               expr_to_cxx(*r.fields[i].second);
      }
      out += "}";
      return out;
    }
    default:
      return "/* unsupported-expr */ 0";
  }
}

std::string Emitter::const_value_to_cxx(const Expr& e,
                                        const TypeExpr* target,
                                        bool explicit_conversion) {
  if (!target) return expr_to_cxx(e);
  if (e.kind == Kind::StringLit) {
    const auto& lit = static_cast<const StringLit&>(e);
    const TypeExpr* canon = canonicalize_type(target);
    if (canon && canon->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon);
      const TypeExpr* elem =
          arr.element ? canonicalize_type(arr.element.get()) : nullptr;
      std::string lo;
      std::string size_expr;
      if (arr.array_kind == ArrayKind::Fixed && arr.dims.size() == 1 && elem &&
          array_dim_bounds_to_cxx(*arr.dims[0], &lo, &size_expr) &&
          (tyname_is(elem, "char") || tyname_is(elem, "byte"))) {
        return "::rt::tp2cc_array_literal<" + type_to_cxx(*elem) + ", " + lo +
               ", " + size_expr + ">(" + expr_to_cxx(e) + ")";
      }
    }
    if (auto cap = shortstring_capacity_to_cxx(target)) {
      if (lit.value.size() == 1) {
        return "::rt::tp2cc_shortstring_of<" + *cap + ">(::rt::tp2cc_char_of('" +
               char_literal_body_to_cxx(lit.value[0]) + "'))";
      }
      std::string out = "::rt::tp2cc_shortstring_literal<" + *cap + ">(";
      bool first = true;
      for (char c : lit.value) {
        if (!first) out += ", ";
        first = false;
        out += "::rt::tp2cc_char_of('";
        out += char_literal_body_to_cxx(c);
        out += "')";
      }
      out += ")";
      return out;
    }
  }
  // Aggregate initialisers recurse into their element/field type.
  if (e.kind == Kind::ArrayConst) {
    const TypeExpr* canon = canonicalize_type(target);
    std::shared_ptr<TyArray> nested_array_target;
    const TypeExpr* elem_type = nullptr;
    if (canon && canon->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon);
      if (arr.dims.size() > 1) {
        nested_array_target = std::make_shared<TyArray>();
        nested_array_target->dims.assign(arr.dims.begin() + 1, arr.dims.end());
        nested_array_target->element = arr.element;
        nested_array_target->is_packed = arr.is_packed;
        nested_array_target->array_kind = arr.array_kind;
        elem_type = nested_array_target.get();
      } else {
        elem_type = arr.element.get();
      }
    }
    const auto& ac = static_cast<const ArrayConst&>(e);
    std::string out = "{";
    for (size_t i = 0; i < ac.elements.size(); ++i) {
      if (i) out += ", ";
      out += const_value_to_cxx(*ac.elements[i], elem_type,
                                explicit_conversion);
    }
    out += "}";
    if (canon && canon->kind == Kind::TyArray) {
      return "{" + out + "}";
    }
    return out;
  }
  if (e.kind == Kind::RecordConst) {
    const auto& rc = static_cast<const RecordConst&>(e);
    std::string out = "{";
    for (size_t i = 0; i < rc.fields.size(); ++i) {
      if (i) out += ", ";
      const TypeExpr* field_type =
          lookup_record_field_type_in_type(target, rc.fields[i].first);
      out += "." + mangle(rc.fields[i].first) + " = " +
             const_value_to_cxx(*rc.fields[i].second, field_type,
                                explicit_conversion);
    }
    out += "}";
    return out;
  }
  if (e.kind == Kind::SetLit) {
    return set_literal_to_cxx(static_cast<const SetLit&>(e), target);
  }
  if (auto text =
          maybe_convert_const_int_expr(e, target, explicit_conversion)) {
    return *text;
  }
  if (auto text = maybe_lower_metaclass_value(e, target)) {
    return *text;
  }
  if (auto text = maybe_convert_proc_value(e, target)) {
    return *text;
  }
  std::string out = expr_to_cxx(e);
  const TypeExpr* source_type = deduce_type(e);
  if (source_type) source_type = canonicalize_type(source_type);
  if (auto cap = shortstring_capacity_to_cxx(target);
      cap && !(source_type && type_is_stringish(source_type))) {
    out = "::rt::tp2cc_shortstring_of<" + *cap + ">(" + out + ")";
  }
  const TypeExpr* canon_target = canonicalize_type(target);
  if (canon_target &&
      (tyname_is(canon_target, "ansistring") ||
       tyname_is(canon_target, "utf8string"))) {
    if (!(source_type &&
          (tyname_is(source_type, "ansistring") ||
           tyname_is(source_type, "utf8string")))) {
      out = "::rt::tp2cc_ansistring_of(" + out + ")";
    }
  }
  return out;
}

// FPC constant conversions first evaluate the integer constant
// expression, then convert that value to the destination type. Keep
// the same split here so assignments/calls/typed consts all share one
// checked path instead of ad-hoc literal special cases.
std::optional<std::string> Emitter::maybe_convert_const_int_expr(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  if (!target) return std::nullopt;
  auto value = eval_const_int_expr(e);
  if (!value) return std::nullopt;
  auto converted =
      convert_const_int_value(e.loc, value->value, target, explicit_conversion,
                              /*diagnose=*/true);
  if (!converted || !converted->type) return std::nullopt;
  if (converted->type->int_kind == PrimitiveIntKind::Unsigned) {
    return uint64_literal_text(converted->bits);
  }
  return signed_bits_literal_text(converted->bits, *converted->type);
}

std::optional<std::string> Emitter::maybe_convert_proc_value(
    const Expr& e, const TypeExpr* target) {
  if (!target) return std::nullopt;
  const TypeExpr* canon = canonicalize_type(target);
  if (!(canon && canon->kind == Kind::TyProcedural)) return std::nullopt;
  const auto& proc = static_cast<const TyProcedural&>(*canon);

  if (proc.is_method && e.kind == Kind::NilLit) {
    // `... of object` still uses the plain two-slot `tp2cc_MethodPtr` carrier.
    // A typed Pascal `nil` for that target becomes an explicit empty carrier
    // value here; the runtime type itself stays a dumb aggregate.
    return type_to_cxx(*target) + "{}";
  }

  // Typed procvar destinations want the callable value itself. In ordinary
  // value context the emitter auto-calls parameterless routines, so turn that
  // off here before deciding whether we also need to bind `self`.
  auto no_autocall = [&](const Expr& src) {
    bool saved = is_callee_context_;
    is_callee_context_ = true;
    std::string text = expr_to_cxx(src);
    is_callee_context_ = saved;
    return text;
  };

  if (!proc.is_method) {
    switch (e.kind) {
      case Kind::Ident:
      case Kind::Member:
      case Kind::AddrOf:
        return no_autocall(e);
      default:
        return std::nullopt;
    }
  }

  if (!registry) return std::nullopt;
  const std::string target_cxx = type_to_cxx(*target);

  // `... of object` lowers to the runtime tp2cc_MethodPtr wrapper, which stores
  // the method thunk separately from the bound object pointer.
  auto method_code_text = [&](const std::string& cls,
                              const ProcDecl& pd) -> std::string {
    return "::rt::tp2cc_method_code<&" + mangle(cls) + "::" +
           method_pointer_helper_name(pd) + ">()";
  };

  auto bind_method = [&](const std::string& base_cxx, const std::string& cls,
                         const ProcDecl& pd,
                         bool base_is_reference_class) -> std::string {
    std::string self_expr = base_is_reference_class
                                ? "(void*)(" + base_cxx + ")"
                                : "(void*)(&(" + base_cxx + "))";
    return target_cxx + "(" + method_code_text(cls, pd) +
           ", " + self_expr + ")";
  };

  auto bind_current_method = [&](const std::string& name)
      -> std::optional<std::string> {
    if (current_class_name.empty()) return std::nullopt;
    if (auto* method = registry->lookup_class_method(current_class_name, name);
        method && method->decl && !method->decl->is_class_method) {
      return bind_method("(*this)", current_class_name, *method->decl, false);
    }
    return std::nullopt;
  };

  auto bind_member = [&](const Member& m) -> std::optional<std::string> {
    std::string cls;
    if (m.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*m.base).name == "self") {
      cls = current_class_name;
    } else {
      cls = deduce_class_alias(*m.base);
    }
    if (cls.empty()) return std::nullopt;
    if (auto* method = registry->lookup_class_method(cls, m.name);
        method && method->decl && !method->decl->is_class_method) {
      return bind_method(expr_to_cxx(*m.base), cls, *method->decl,
                         expr_is_reference_class(*m.base));
    }
    return std::nullopt;
  };

  if (e.kind == Kind::Ident) {
    return bind_current_method(static_cast<const Ident&>(e).name);
  }
  if (e.kind == Kind::Member) {
    return bind_member(static_cast<const Member&>(e));
  }
  if (e.kind == Kind::AddrOf) {
    const auto& a = static_cast<const AddrOf&>(e);
    if (!a.operand) return std::nullopt;
    if (a.operand->kind == Kind::Ident) {
      return bind_current_method(static_cast<const Ident&>(*a.operand).name);
    }
    if (a.operand->kind == Kind::Member) {
      return bind_member(static_cast<const Member&>(*a.operand));
    }
  }
  return std::nullopt;
}

std::optional<std::string> Emitter::maybe_lower_metaclass_value(
    const Expr& e, const TypeExpr* target) {
  const std::string base_name = metaclass_target_name(target);
  if (base_name.empty()) return std::nullopt;

  auto concrete_class_name = [&](const Expr& src) -> std::string {
    if (src.kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(src);
      if (const auto* ci = class_info_for_type_name(id.name);
          ci && ci->is_reference_type) {
        return id.name;
      }
      return {};
    }
    if (src.kind == Kind::Member) {
      const auto& mem = static_cast<const Member&>(src);
      if (mem.base->kind != Kind::Ident) return {};
      const auto& base = static_cast<const Ident&>(*mem.base);
      const std::string qualified = base.name + "." + mem.name;
      if (const auto* ci = class_info_for_type_name(qualified);
          ci && ci->is_reference_type) {
        return qualified;
      }
    }
    return {};
  };

  const std::string concrete_name = concrete_class_name(e);
  if (concrete_name.empty()) return std::nullopt;
  if (!class_info_for_type_name(base_name)) return std::nullopt;

  // A Pascal class identifier used as a value means "the metaclass value for
  // that exact concrete class", not the instance type itself. Preserve the
  // concrete class here so a later cast back to a more specific `class of T`
  // can recover the derived metaclass descriptor instead of collapsing the
  // value down to the current assignment target's base type.
  return metaclass_value_fn_cxx(concrete_name) + "()";
}

// ---------------------------------------------------------------------------
// Declarations

void Emitter::emit_const_decl(const ConstDecl& cd, bool in_header) {
  const std::string name = mangle(cd.name);
  std::string val = const_value_to_cxx(*cd.value, cd.type.get());

  // Two things drive the qualifiers:
  //   `inline` -- required on definitions at namespace scope in a header so
  //              multiple translation units that include it don't violate
  //              ODR. Invalid at block scope, so we drop it there.
  //   `const`  -- Pascal's UNTYPED const (`const X = 5;`) is immutable,
  //              TYPED const (`const X : T = 5;`) is writable.
  const bool block = block_depth > 0;
  const std::string linkage = block ? std::string() : std::string("inline ");
  const TypeExpr* typed_const_ty =
      cd.type ? canonicalize_type(cd.type.get()) : nullptr;

  // Typed array (or named alias ultimately resolving to one) with an
  // array-constant initialiser emits an `rt::tp2cc_Array<T, Lo, N>` so
  //   (a) the size is known even when the index is an enum (Pascal),
  //   (b) the array has value-copy semantics on pass (Pascal),
  //   (c) `arr[Lo]` picks the first element (Pascal arbitrary low bound).
  if (cd.type && cd.value->kind == Kind::ArrayConst) {
    // Chase through named aliases (cross-unit-aware) until we see the
    // underlying TyArray.
    const TypeExpr* t = typed_const_ty;
    if (t && t->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*t);
      // Wrap the element type in `tp2cc_Array<..., Lo, N>` for each dim
      // from innermost to outermost.
      std::string ty = arr.element ? type_to_cxx(*arr.element)
                                   : std::string("int32_t");
      for (auto it = arr.dims.rbegin(); it != arr.dims.rend(); ++it) {
        std::string lo, size_expr;
        if (!array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
          // Still unknown -- fall through to the generic emit below.
          goto generic_emit;
        }
        ty = "::rt::tp2cc_Array<" + ty + ", " + lo + ", " + size_expr + ">";
      }
      emitln(linkage + ty + " " + name + " = " + val + ";");
      return;
    }
  }
generic_emit:;

  if (cd.type) {
    // Typed Pascal const -- writable.  `val' was produced by
    // const_value_to_cxx with cd.type as the target, so aggregate
    // and scalar leaves are already lowered to the destination value.
    if (typed_const_ty && typed_const_ty->kind == Kind::TyArray &&
        cd.value->kind != Kind::StringLit) {
      emitln(linkage + type_to_cxx(*cd.type) + " " + name + " = {" + val +
             "};");
      return;
    }
    emitln(linkage + type_to_cxx(*cd.type) + " " + name + " = " + val + ";");
    return;
  }

  // Untyped Pascal const -- immutable.
  //   - Single-char string literal: wrap in `rt::CharConst` so it's
  //     usable as both `p_char` (Pascal char) and `tp2cc_ShortString<>`
  //     (Pascal string) by context, matching Pascal's polymorphic
  //     `const X = 'c';` semantics.
  //   - Multi-char string literal: plain `tp2cc_ShortString<>` so `+`
  //     resolves to concatenation.
  if (cd.value->kind == Kind::StringLit) {
    const auto& sl = static_cast<const StringLit&>(*cd.value);
    if (sl.value.size() == 1) {
      // Keep char consts visibly aggregate-shaped in the emitted C++.
      emitln(linkage + "constexpr ::rt::CharConst " + name + "{" +
             val + "};");
    } else {
      emitln(linkage + "const ::rt::tp2cc_ShortString<> " + name + " = " +
             val + ";");
    }
    return;
  }
  if (const TypeExpr* inferred_ty = deduce_const_decl_type(cd)) {
    const TypeExpr* canon = canonicalize_type(inferred_ty);
    if (canon && canon->kind == Kind::TyName) {
      std::string nm = ascii_lower(static_cast<const TyName&>(*canon).name);
      if (auto* info = primitive_info(nm);
          info && info->int_kind != PrimitiveIntKind::None) {
        val = const_value_to_cxx(*cd.value, inferred_ty);
        emitln(linkage + "const " + type_to_cxx(*inferred_ty) + " " + name +
               " = " + val + ";");
        return;
      }
    }
  }
  emitln(linkage + "const auto " + name + " = " + val + ";");
}

void Emitter::emit_type_decl(const TypeDecl& td, bool) {
  const std::string name = mangle(td.name);

  // Pascal enums are unscoped: members leak into the enclosing namespace
  // and are referenced directly. We emit a plain `enum` (not `enum class`)
  // with an explicit underlying type so the members are usable bare.
  //
  // We also emit per-enum helper constants -- Pascal's
  // `low(T)` / `high(T)` take a type name as argument; we rewrite those
  // calls to the constants at emit time.
  if (td.type && td.type->kind == Kind::TyEnum) {
    const auto& te = static_cast<const TyEnum&>(*td.type);
    emitln("enum " + name + " : " + enum_underlying_type_to_cxx(te) + " {");
    indent();
    for (size_t i = 0; i < te.members.size(); ++i) {
      std::string m = mangle(te.members[i].name);
      if (te.members[i].value) {
        m += " = " + const_value_to_cxx(*te.members[i].value);
      }
      if (i + 1 < te.members.size()) m += ",";
      emitln(m);
    }
    dedent();
    emitln("};");
    if (!te.members.empty()) {
      // `inline` is not permitted on block-scope variables, so omit it
      // when we're emitting inside a function body (local type decl).
      const char* lin = (block_depth > 0) ? "" : "inline ";
      emitln(std::string(lin) + "constexpr " + name + " " +
             enum_bound_name(td.name, "low") + " = " +
             mangle(te.members.front().name) + ";");
      emitln(std::string(lin) + "constexpr " + name + " " +
             enum_bound_name(td.name, "high") + " = " +
             mangle(te.members.back().name) + ";");
    }
    return;
  }

  if (td.type && td.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*td.type);
    // Pascal `packed record` -> `struct [[gnu::packed]]`. We deliberately use
    // the attribute rather than `#pragma pack(push,1)` because the attribute
    // makes GCC *warn* about latent UB that `#pragma pack` silently permits:
    // taking an aligned pointer/reference to a packed field
    // (`-Waddress-of-packed-member`), binding a non-const reference to one,
    // or calling a method on an inner struct-typed field that would form a
    // typed `this` with lost-packedness. All of those are real bugs -- e.g.
    // the `set of ttargetflags` field inside `packed record ttargetinfo` in
    // `compiler/systems.pas` would, under `#pragma pack`, happily silently
    // invoke `tp2cc_Set::add` through a misaligned `this`. With the attribute the
    // compiler flags it, and we can fix it (see rt::tp2cc_Set, now byte-aligned).
    std::string open = "struct ";
    if (tr.is_packed) open += "[[gnu::packed]] ";
    emitln(open + name + " {");
    indent();

    // For Pascal `packed record`, the property we care about is the final
    // byte layout: no inserted padding between fields, and total size equal
    // to the packed Pascal size. Emit the ordinary `[[gnu::packed]]` struct,
    // then assert the resulting `offsetof(...)` / `sizeof(...)` after the
    // complete type is known.
    auto emit_field_decls = [&](const std::vector<RecordField>& fs) {
      for (const auto& field : record_field_decls(fs)) {
        emitln(field.decl + ";");
      }
    };
    emit_field_decls(tr.fields);
    if (tr.has_variant) {
      // Pascal variant records expose their case-fields directly on the
      // outer record -- `rec.fieldOfCase1` works without saying which case.
      // We match that by emitting one anonymous struct per case inside an
      // anonymous union. GCC accepts this as an extension.
      if (!tr.variant_tag_name.empty() && tr.variant_tag_type) {
        RecordField tag_field;
        tag_field.names.push_back(tr.variant_tag_name);
        tag_field.type = tr.variant_tag_type;
        emit_field_decls({tag_field});
      }
      emitln("union {");
      indent();
      for (const auto& vc : tr.variant_cases) {
        if (vc.fields.empty()) continue;
        std::string case_open = "struct ";
        if (tr.is_packed) case_open += "[[gnu::packed]] ";
        case_open += "{";
        emitln(case_open);
        indent();
        emit_field_decls(vc.fields);
        dedent();
        emitln("};");
      }
      dedent();
      emitln("};");
    }
    dedent();
    emitln("};");
    if (tr.is_packed) {
      const PackedRecordLayout layout = compute_packed_record_layout(tr);
      if (!layout.field_offsets.empty()) {
        emit_packed_record_asserts(name, layout, name);
      }
    }
    return;
  }

  if (td.type && td.type->kind == Kind::TyObject) {
    const auto& to = static_cast<const TyObject&>(*td.type);
    if (to.is_forward) {
      // `T = class;` only reserves the name for later use in the same type
      // section. `emit_forward_struct_decls` already emitted the necessary
      // C++ `struct T;`, so the forward declaration itself has no body here.
      return;
    }
    std::string line = "struct " + name;
    if (!to.parent.empty()) {
      line += " : public " + named_type_struct_cxx(to.parent);
    } else if (to.is_reference_type) {
      // Delphi/FPC `class` types implicitly inherit `TObject` even when
      // the source omits an explicit ancestor.
      line += " : public ::rt::p_tobject";
    }
    line += " {";
    emitln(line);
    indent();
    // Pascal `inherited X` is unambiguous under single inheritance: it
    // refers to the parent object's X. Emit `using inherited = Parent;`
    // so the method-body translator can rewrite `inherited X` to
    // `inherited::p_X`. The alias is named `inherited` (bare, no p_
    // prefix) because `inherited` is a Pascal keyword -- user code cannot
    // declare a field or variable with that name, so the alias is
    // guaranteed collision-free.
    if (!to.parent.empty()) {
      emitln("using inherited = " + named_type_struct_cxx(to.parent) + ";");
    } else if (to.is_reference_type) {
      emitln("using inherited = ::rt::p_tobject;");
    }
    if (to.is_reference_type) {
      emitln("virtual ::rt::p_tclass p_classtype() const override;");
      emitln("virtual int32_t p_instancesize() const override;");
    }
    bool has_virtual = false;
    for (const auto& m : to.members) {
      if (m.kind == ObjectMemberKind::Field) {
        for (const auto& fn : m.field_names) {
          emitln(named_type_to_cxx(m.field_type.get(), mangle(fn)) + ";");
        }
      } else if (m.kind == ObjectMemberKind::Method) {
        // Method signature. We do NOT emit the body here -- bodies live in
        // the implementation .cc, emitted as `ret Class::method(...) { }`.
        // Exception: `virtual; abstract;` Pascal methods have no
        // implementation-side body in the source, so we emit an inline
        // fail-fast definition right here. Without it the vtable carries
        // a slot whose definition the linker can't find.
        const auto& pd = *m.method;
        std::string ret = proc_return_type_to_cxx(pd);
        std::string prefix;
        if (pd.is_class_method) {
          // Current class-method support is the non-virtual subset only:
          // emit a real C++ static so `TClass.Method(...)` and unqualified
          // in-class calls resolve directly, and reject metaclass-only
          // Pascal features earlier in the parser.
          prefix = "static ";
        } else if (pd.is_virtual || pd.is_abstract || pd.is_override) {
          prefix = "virtual ";
          has_virtual = true;
        }
        std::string suffix;
        if (!pd.is_class_method) {
          // Native FPC still instantiates some classes that carry abstract
          // placeholder methods. Model those methods as fail-fast virtuals in
          // C++ instead of pure-virtual slots so the translated bootstrap
          // compiler can construct the same placeholder classes.
          if (pd.is_override) suffix = " override";
        }
        if (pd.is_abstract && !pd.is_class_method) {
          // Inline fail-fast body. Returning a non-void value would
          // require fabricating one; `::std::abort()` aborts before any
          // value is needed, and C++ accepts the function as having a
          // valid path because abort is `[[noreturn]]`.
          emitln(prefix + ret + " " + mangle(pd.name) + "(" +
                 param_list_to_cxx(pd.params) + ")" + suffix +
                 " { ::std::abort(); }");
        } else {
          emitln(prefix + ret + " " + mangle(pd.name) + "(" +
                 param_list_to_cxx(pd.params) + ")" + suffix + ";");
        }
        if (!pd.is_class_method) emit_method_pointer_thunk(name, pd, ret);
      }
    }
    // Polymorphic objects must have a virtual C++ destructor, otherwise
    // `delete p_base_ptr;` passes sizeof(Base) to operator delete even when
    // the object is actually a larger Derived -- new-delete-type-mismatch UB.
    // Pascal's `destructor Done; virtual;` maps to `virtual void p_done()`,
    // but that's just a regular method; the C++ dtor (`~Name()`) is separate
    // and must be virtual so the correct sized-delete is issued via the vtable.
    if (has_virtual) {
      emitln("virtual ~" + name + "() = default;");
    }
    dedent();
    emitln("};");

    if (to.is_reference_type) {
      const std::string meta_name = "tp2cc_metaclass_" + name;
      const std::string value_fn = "tp2cc_metaclass_value_" + name;
      const bool has_parent_meta =
          !to.parent.empty() && class_info_for_type_name(to.parent) &&
          class_info_for_type_name(to.parent)->is_reference_type;
      const std::string parent_meta =
          has_parent_meta ? metaclass_struct_cxx(to.parent) : std::string{};
      const std::string base_meta =
          has_parent_meta ? parent_meta
                          : std::string("::rt::tp2cc_metaclass_p_tobject");
      const auto visible_callables = collect_metaclass_callables(td.name);
      const auto parent_callables =
          has_parent_meta ? collect_metaclass_callables(to.parent)
                          : std::vector<MetaclassCallable>{};
      auto callable_param_types = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
        return procedural_param_types_to_cxx(callable.sig->decl->params);
      };
      auto same_callable_surface = [&](const MetaclassCallable& lhs,
                                       const MetaclassCallable& rhs) {
        if (lhs.name != rhs.name) return false;
        if (lhs.implicit_root_create || rhs.implicit_root_create) {
          return lhs.implicit_root_create == rhs.implicit_root_create;
        }
        if (!lhs.sig || !rhs.sig) return lhs.sig == rhs.sig;
        return lhs.sig->kind == rhs.sig->kind &&
               callable_param_types(lhs) == callable_param_types(rhs);
      };
      std::unordered_map<std::string, MetaclassCallable> parent_surface;
      for (const auto& callable : parent_callables) {
        parent_surface.emplace(callable.name, callable);
      }
      std::vector<MetaclassCallable> own_callables;
      for (const auto& callable : visible_callables) {
        auto pit = parent_surface.find(callable.name);
        // A derived metaclass only reuses the parent slot when the visible
        // callable surface is identical. If `TChild.Create(name, mode)` hides
        // the parent's zero-arg `Create`, `class of TChild` needs its own
        // `p_create(filename, mode)` entry while `class of TBase` keeps the
        // original zero-arg slot.
        if (pit == parent_surface.end() ||
            !same_callable_surface(callable, pit->second)) {
          own_callables.push_back(callable);
        }
      }

      auto callable_return_type = [&](std::string_view target_class,
                                      const MetaclassCallable& callable) {
        if (callable.implicit_root_create ||
            (callable.sig && callable.sig->kind == SymKind::Constructor)) {
          return named_type_struct_cxx(target_class) + "*";
        }
        const auto& pd = *callable.sig->decl;
        if (pd.pkind == ProcKind::Function && pd.return_type) {
          return type_to_cxx(*pd.return_type);
        }
        return std::string("void");
      };
      auto callable_param_list = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
        return param_list_to_cxx(callable.sig->decl->params);
      };
      auto callable_arg_list = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
        std::string out;
        bool first = true;
        size_t unnamed_index = 0;
        for (const auto& par : callable.sig->decl->params) {
          if (par.names.empty()) {
            if (!first) out += ", ";
            out += "tp2cc_arg" + std::to_string(unnamed_index++);
            first = false;
            continue;
          }
          for (const auto& pn : par.names) {
            if (!first) out += ", ";
            out += mangle(pn);
            first = false;
          }
        }
        return out;
      };
      auto callable_ctor_param = [&](std::string_view target_class,
                                     const MetaclassCallable& callable) {
        return callable_return_type(target_class, callable) + " (*tp2cc_" +
               mangle(callable.name) + ")(" + callable_param_types(callable) +
               ")";
      };
      auto callable_ctor_init = [&](const MetaclassCallable& callable) {
        return mangle(callable.name) + "(tp2cc_" + mangle(callable.name) + ")";
      };

      // Pascal `class of T` values must preserve the concrete class they
      // came from so an upcast to `class of TBase` and a later downcast back
      // to `class of TDerived` still sees the same constructor/class-method
      // table. Mirror the class hierarchy here: each metaclass descriptor
      // inherits the parent's descriptor and only adds newly visible
      // constructor/class-method entries of its own.
      std::string meta_decl = "struct " + meta_name;
      // Every emitted metaclass must remain convertible to `::rt::p_tclass`.
      // Even Pascal classes with only an implicit TObject ancestor therefore
      // need to inherit the runtime root descriptor, not a standalone struct.
      meta_decl += " : public " + base_meta;
      meta_decl += " {";
      emitln(meta_decl);
      indent();
      for (const auto& callable : own_callables) {
        emitln(callable_return_type(td.name, callable) + " (*" +
               mangle(callable.name) + ")(" +
               callable_param_types(callable) + ");");
      }
      const std::string direct_parent_meta =
          has_parent_meta ? (metaclass_value_fn_cxx(to.parent) + "()")
                          : std::string("::rt::tp2cc_metaclass_value_p_tobject()");
      emitln("::rt::p_tclass tp2cc_parentclass() const override { return " +
             direct_parent_meta + "; }");
      if (visible_callables.empty()) {
        // A class with no visible constructor/class-method surface still
        // needs a metaclass object for `ClassType` / `TClass`, but that
        // descriptor should stay trivially default-constructible instead of
        // forcing a synthetic parent-descriptor constructor argument.
        emitln(meta_name + "() = default;");
      } else {
        std::string ctor_params;
        bool first = true;
        if (has_parent_meta && !parent_callables.empty()) {
          ctor_params += parent_meta + " tp2cc_parent";
          first = false;
        }
        for (const auto& callable : own_callables) {
          if (!first) ctor_params += ", ";
          ctor_params += callable_ctor_param(td.name, callable);
          first = false;
        }
        std::string init_list;
        if (has_parent_meta && !parent_callables.empty()) {
          init_list = " : " + parent_meta + "(tp2cc_parent)";
        }
        for (const auto& callable : own_callables) {
          init_list += (init_list.empty() ? " : " : ", ") +
                       callable_ctor_init(callable);
        }
        emitln(meta_name + "(" + ctor_params + ")" +
               init_list + " {}");
      }
      dedent();
      emitln("};");

      std::function<std::string(std::string_view, std::string_view)>
          build_metaclass_ctor_expr =
              [&](std::string_view target_class,
                  std::string_view concrete_class) -> std::string {
        const auto target_callables = collect_metaclass_callables(target_class);
        std::string current = ascii_lower(std::string(target_class));
        std::string parent_class;
        if (const auto* ci = class_info_for_type_name(current)) {
          parent_class = ci->parent;
        }
        const bool has_parent =
            !parent_class.empty() && class_info_for_type_name(parent_class) &&
            class_info_for_type_name(parent_class)->is_reference_type;
        const auto parent_visible =
            has_parent ? collect_metaclass_callables(parent_class)
                       : std::vector<MetaclassCallable>{};
        std::unordered_map<std::string, MetaclassCallable> parent_surface;
        for (const auto& callable : parent_visible) {
          parent_surface.emplace(callable.name, callable);
        }
        auto ctor_member_call = [&](std::string_view owner_class,
                                    std::string_view method_name,
                                    const std::string& args) {
          if (ascii_lower(std::string(owner_class)) ==
              ascii_lower(std::string(concrete_class))) {
            return "tp2cc_ptr->" + std::string(method_name) + "(" + args + ")";
          }
          return "static_cast<" + named_type_struct_cxx(owner_class) +
                 "*>(tp2cc_ptr)->" + std::string(method_name) + "(" + args +
                 ")";
        };
        std::string out = metaclass_struct_cxx(target_class) + "(";
        bool first = true;
        if (has_parent && !parent_visible.empty()) {
          out += build_metaclass_ctor_expr(parent_class, concrete_class);
          first = false;
        }
        for (const auto& callable : target_callables) {
          auto pit = parent_surface.find(callable.name);
          if (pit != parent_surface.end() &&
              same_callable_surface(callable, pit->second)) {
            continue;
          }
          if (!first) out += ", ";
          const auto concrete_impl =
              find_metaclass_callable_impl(concrete_class, callable);
          if (!concrete_impl) {
            // A `class of TBase` value can hold `TChild`, but a hidden
            // constructor/class-method is not callable through the base
            // metaclass surface anymore. Keep such entries as "fail loudly"
            // thunks instead of guessing a mismatched signature.
            const std::string ret =
                callable_return_type(target_class, callable);
            out += "+[](" + callable_param_list(callable) + ") -> " + ret +
                   " { ::std::abort();";
            if (ret != "void") out += " return {};";
            out += " }";
            first = false;
            continue;
          }
          const bool use_implicit_root_create =
              concrete_impl->implicit_root_create;
          const auto* concrete_sig = concrete_impl->sig;
          if (use_implicit_root_create ||
              (concrete_sig && concrete_sig->kind == SymKind::Constructor)) {
            out += "+[](" + callable_param_list(callable) + ") -> " +
                   callable_return_type(target_class, callable) + " { auto "
                   "tp2cc_ptr = new " +
                   named_type_struct_cxx(concrete_class) + "{}; ";
            const std::string ctor_name = use_implicit_root_create
                                              ? "p_create"
                                              : mangle(callable.name);
            out += ctor_member_call(concrete_impl->owner_class, ctor_name,
                                    callable_arg_list(callable)) +
                   "; return tp2cc_ptr; }";
          } else {
            std::string ret = callable_return_type(target_class, callable);
            out += "+[](" + callable_param_list(callable) + ") -> " + ret +
                   " { ";
            if (ret != "void") out += "return ";
            out += named_type_struct_cxx(concrete_impl->owner_class) + "::" +
                   mangle(callable.name) + "(" +
                   callable_arg_list(callable) + ");";
            out += " }";
          }
          first = false;
        }
        out += ")";
        return out;
      };

      emitln("inline const " + meta_name + "* " + value_fn + "() {");
      indent();
      if (visible_callables.empty()) {
        emitln("static const " + meta_name + " value{};");
      } else {
        emitln("static const " + meta_name + " value = " +
               build_metaclass_ctor_expr(td.name, td.name) + ";");
      }
      emitln("return &value;");
      dedent();
      emitln("}");
      emitln("inline ::rt::p_tclass " + name + "::p_classtype() const {");
      indent();
      emitln("return " + value_fn + "();");
      dedent();
      emitln("}");
      emitln("inline int32_t " + name + "::p_instancesize() const {");
      indent();
      emitln("return sizeof(" + name + ");");
      dedent();
      emitln("}");
    }
    return;
  }

  // Ordinary alias.
  std::string rhs = td.type ? type_to_cxx(*td.type) : std::string("int32_t");
  emitln("using " + name + " = " + rhs + ";");
}

void Emitter::emit_var_decl(const VarDecl& vd, bool in_header) {
  if (vd.is_absolute) {
    auto target = resolve_absolute_target(vd);
    if (!target) return;
    std::string ty = vd.type ? type_to_cxx(*vd.type) : std::string("int32_t");
    bool pointee_view = target->is_pointerish && !type_is_pointerish(vd.type.get());
    for (const auto& n : vd.names) {
      std::string name = mangle(n);
      std::string decl = attach_named_cxx_type(
          ty, name, target->is_const_storage ? "const &" : "&");
      if (in_header) {
        emitln("extern " + decl + ";");
      } else {
        emitln(decl + " = " +
               reinterpret_ref_text(ty, target->cxx, pointee_view) + ";");
      }
    }
    return;
  }
  if (vd.is_external) {
    report_error(vd.loc, "external variables are unsupported");
    return;
  }
  // Inline anonymous packed records bound to a var lose access to a
  // typedef name for `offsetof`/`sizeof` asserts -- but `decltype(var)`
  // is a usable substitute, so we emit the same layout asserts the
  // named-type-decl path already enforces.
  const TyRecord* inline_packed_record = nullptr;
  if (vd.type && vd.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*vd.type);
    if (tr.is_packed) inline_packed_record = &tr;
  }
  for (const auto& n : vd.names) {
    std::string name = mangle(n);
    std::string decl = named_type_to_cxx(vd.type.get(), name);
    if (in_header) {
      emitln("extern " + decl + ";");
    } else if (vd.init) {
      std::string rhs = const_value_to_cxx(*vd.init, vd.type.get());
      emitln(decl + " = " + rhs + ";");
    } else if (block_depth > 0) {
      // Function-local `var X : T;` with no initialiser. Pascal itself
      // leaves locals undefined, but lots of FPC code still expects
      // ShortStrings to start with `length = 0` and Arrays to start
      // zeroed because that was the de-facto behaviour under FPC's
      // stack-zeroing in debug builds. Until our rt types carried
      // default member initialisers (`length = 0;`, `T data[N]{}`),
      // those locals read as zero "for free". We are about to remove
      // those member initialisers so the rt types can be `is_trivial`
      // (required for them to live inside a `[[gnu::packed]]` struct
      // without GCC silently ignoring the packing). To preserve the
      // zero-init behaviour that existing emitted code depends on, we
      // emit `T name{};` (value-initialisation) for locals instead of
      // `T name;` (default-initialisation, which would leave primitive
      emitln(decl + "{};");
    } else {
      // File-scope / unit-level `var X : T;` with no initialiser.
      // C++ zero-initialises static-storage objects before any dynamic
      // init, so plain `T name;` is already equivalent to value-init
      // for the trivial types we emit.
      emitln(decl + ";");
    }
    if (inline_packed_record) {
      const PackedRecordLayout layout =
          compute_packed_record_layout(*inline_packed_record);
      if (!layout.field_offsets.empty()) {
        emit_packed_record_asserts("decltype(" + name + ")", layout, n);
      }
    }
  }
}

std::string Emitter::param_list_to_cxx(const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& p : params) {
    // Untyped `var X` / `const X` / `X` in Pascal means "pass the
    // storage, any type". The closest C++ is a raw address: `void*`
    // (no reference). Inside the body, Pascal's `@X` becomes just
    // `X` (the pointer itself). At call sites we wrap the argument
    // with `(void*)&(arg)` to pass the caller's storage address.
    std::string pt;
    std::string name_prefix;
    if (!p.type) {
      pt = "void*";
    } else {
      if (type_is_open_array(p.type.get())) {
        pt = open_array_type_to_cxx(*p.type);
      } else {
        pt = type_to_cxx(*p.type);
      }
      if (p.mode == Param::Var || p.mode == Param::Out) name_prefix = "&";
      else if (p.mode == Param::Const &&
               const_param_needs_mutable_ref(p.type.get()))
        name_prefix = "&";
      else if (p.mode == Param::Const &&
               const_param_needs_const_ref(p.type.get()))
        name_prefix = "const &";
    }
    for (const auto& n : p.names) {
      if (!first) out += ", ";
      first = false;
      if (!p.type) out += pt + " " + mangle(n);
      else out += attach_named_cxx_type(pt, mangle(n), name_prefix);
    }
    if (p.names.empty()) {
      if (!first) out += ", ";
      first = false;
      if (!p.type) out += pt;
      else out += attach_named_cxx_type(pt, "", name_prefix);
    }
  }
  return out;
}

std::string Emitter::proc_return_type_to_cxx(const ProcDecl& pd) {
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    return type_to_cxx(*pd.return_type);
  }
  if (pd.pkind == ProcKind::Constructor) {
    return "bool";
  }
  return "void";
}

void Emitter::emit_method_pointer_thunk(const std::string& owner_name,
                                        const ProcDecl& pd,
                                        const std::string& ret) {
  if (pd.is_class_method) return;
  if (pd.pkind != ProcKind::Procedure && pd.pkind != ProcKind::Function) {
    return;
  }

  // Pascal `... of object` values are `(code,self)` pairs, not C++
  // member-function pointers. Each method therefore gets a plain static
  // thunk with explicit `self`, and the runtime stores the thunk address
  // plus the bound object pointer as the method-pointer value.
  std::string helper_params = "void* tp2cc_self";
  std::string helper_args;
  bool first_arg = true;
  int unnamed_index = 0;
  auto append_arg = [&](const std::string& pt, const std::string& name) {
    helper_params += ", " + pt + " " + name;
    if (!first_arg) helper_args += ", ";
    first_arg = false;
    helper_args += name;
  };

  for (const auto& par : pd.params) {
    std::string pt;
    if (!par.type) {
      pt = "void*";
    } else if (type_is_open_array(par.type.get())) {
      pt = open_array_type_to_cxx(*par.type);
    } else {
      pt = type_to_cxx(*par.type);
    }
    // The thunk must preserve the method's real parameter ABI so
    // `var`/`out`/`const` and open-array calls behave identically
    // whether they go through a method pointer or a direct call.
    if (par.type) {
      // Untyped Pascal params always carry "address of caller storage",
      // even for `const`, so method-pointer thunks must keep the raw
      // `void*` ABI instead of inventing `const void*&`.
      if (par.mode == Param::Var || par.mode == Param::Out) pt += "&";
      else if (par.mode == Param::Const &&
               const_param_needs_mutable_ref(par.type.get()))
        pt += "&";
      else if (par.mode == Param::Const &&
               const_param_needs_const_ref(par.type.get()))
        pt = "const " + pt + "&";
    }
    if (par.names.empty()) {
      append_arg(pt, "tp2cc_arg" + std::to_string(++unnamed_index));
      continue;
    }
    for (const auto& pn : par.names) {
      append_arg(pt, mangle(pn));
    }
  }

  std::string call = "static_cast<" + owner_name + "*>(tp2cc_self)->" +
                     mangle(pd.name) + "(" + helper_args + ")";
  if (pd.pkind == ProcKind::Function) {
    emitln("static " + ret + " " + method_pointer_helper_name(pd) + "(" +
           helper_params + ") { return " + call + "; }");
  } else {
    emitln("static void " + method_pointer_helper_name(pd) + "(" +
           helper_params + ") { " + call + "; }");
  }
}

void Emitter::emit_proc_decl_signature(const ProcDecl& pd) {
  if (pd.is_external) {
    report_error(pd.loc, "external routines are unsupported");
    return;
  }
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  std::string params = param_list_to_cxx(pd.params);
  emitln(ret + " " + mangle(pd.name) + "(" + params + ");");
}

void Emitter::emit_decl(const Decl& d, bool in_header) {
  switch (d.kind) {
    case Kind::ConstDecl:
      emit_const_decl(static_cast<const ConstDecl&>(d), in_header);
      break;
    case Kind::TypeDecl:
      emit_type_decl(static_cast<const TypeDecl&>(d), in_header);
      break;
    case Kind::VarDecl:
      emit_var_decl(static_cast<const VarDecl&>(d), in_header);
      break;
    case Kind::ProcDecl: {
      const auto& pd = static_cast<const ProcDecl&>(d);
      if (in_header) {
        emit_proc_decl_signature(pd);
      } else if (pd.is_external) {
        report_error(pd.loc, "external routines are unsupported");
      } else if (pd.is_forward) {
        if (block_depth > 0) {
          std::string ret =
              (pd.pkind == ProcKind::Function && pd.return_type)
                  ? type_to_cxx(*pd.return_type)
                  : std::string("void");
          std::string sig_params;
          bool first = true;
          for (const auto& p : pd.params) {
            std::string pt;
            if (!p.type) {
              pt = "void*";
            } else {
              pt = type_to_cxx(*p.type);
              if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
              else if (p.mode == Param::Const &&
                       const_param_needs_mutable_ref(p.type.get()))
                pt += "&";
              else if (p.mode == Param::Const &&
                       const_param_needs_const_ref(p.type.get()))
                pt = "const " + pt + "&";
            }
            for (const auto& n : p.names) {
              (void)n;
              if (!first) sig_params += ", ";
              first = false;
              sig_params += pt;
            }
            if (p.names.empty()) {
              if (!first) sig_params += ", ";
              first = false;
              sig_params += pt;
            }
          }
          emitln("::std::function<" + ret + "(" + sig_params + ")> " +
                 mangle(pd.name) + ";");
          local_nested_forwards.insert(pd.name);
        } else {
          // Pascal `forward;` in the impl section means "the body
          // comes later in this same unit". C++ needs a prototype
          // up-front so calls earlier in the file resolve.
          emit_proc_decl_signature(pd);
        }
      } else if (!pd.is_external && (pd.body || pd.is_abstract)) {
        if (block_depth > 0) {
          // Nested proc. C++ forbids nested function definitions; emit
          // as a lambda captured by reference so it sees the enclosing
          // routine's locals. std::function so the lambda name is in
          // scope inside its own body (required for recursion).
          emit_nested_proc_lambda(pd);
        } else {
          emit_proc_body(pd);
        }
      }
      break;
    }
    case Kind::LabelDecl:
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Statements

void Emitter::emit_raise_stmt(const Raise& r) {
  if (r.value) {
    emitln("throw " + expr_to_cxx(*r.value) + ";");
    return;
  }
  if (except_handler_depth == 0) {
    report_error(r.loc, "bare raise is only valid inside an except handler");
  }
  emitln("throw;");
}

void Emitter::emit_try_stmt(const Try& t) {
  const std::string n = std::to_string(++try_stmt_counter);

  if (t.is_finally) {
    emitln("{");
    indent();
    // Pascal `finally` runs on every exit path from the block. Model that
    // with a C++ scope guard so `Exit`, loop control, and exception unwinding
    // all funnel through one emitted finally-body.
    emitln("auto tp2cc_finally_" + n + " = ::rt::tp2cc_make_scope_exit([&]() {");
    indent();
    for (const auto& sub : t.finally_body) emit_stmt(*sub);
    dedent();
    emitln("});");
    for (const auto& sub : t.body) emit_stmt(*sub);
    dedent();
    emitln("}");
    return;
  }

  if (t.handlers.empty()) {
    emitln("try {");
    indent();
    for (const auto& sub : t.body) emit_stmt(*sub);
    dedent();
    emitln("} catch (...) {");
    indent();
    ++except_handler_depth;
    if (t.except_else) emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
    emitln("}");
    return;
  }

  const std::string exc_name = "tp2cc_exc_" + n;
  const std::string handled_name = "tp2cc_handled_" + n;
  emitln("try {");
  indent();
  for (const auto& sub : t.body) emit_stmt(*sub);
  dedent();
  emitln("} catch (::rt::p_exception* " + exc_name + ") {");
  indent();
  emitln("bool " + handled_name + " = false;");
  for (size_t i = 0; i < t.handlers.size(); ++i) {
    const auto& h = t.handlers[i];
    std::string opener = (i == 0) ? "if" : "else if";
    if (h.class_name.empty()) {
      emitln(opener + " (true) {");
    } else {
      // Pascal `on E: TException do` only matches exception classes, so the
      // translated `dynamic_cast` target must be a pointer type even when the
      // name comes from the `sysutils` stub alias and does not resolve through
      // the normal class registry.
      TyName handler_type;
      handler_type.name = h.class_name;
      std::string handler_cxx = type_to_cxx(handler_type);
      if (handler_cxx.empty() || handler_cxx.back() != '*') {
        handler_cxx += "*";
      }
      emitln(opener + " (auto tp2cc_match_" + n + "_" + std::to_string(i) +
             " = dynamic_cast<" + handler_cxx + ">(" +
             exc_name + "); tp2cc_match_" + n + "_" + std::to_string(i) +
             ") {");
    }
    indent();
    emitln(handled_name + " = true;");
    std::optional<std::string> bound_name;
    std::optional<TyName> bound_type;
    auto saved_locals = local_scope;
    auto saved_types = local_types;
    if (!h.var_name.empty()) {
      bound_name = mangle(h.var_name);
      emitln("auto " + *bound_name + " = " +
             (h.class_name.empty()
                  ? exc_name
                  : "tp2cc_match_" + n + "_" + std::to_string(i)) +
             ";");
      local_scope.insert(h.var_name);
      bound_type.emplace();
      bound_type->name =
          h.class_name.empty() ? std::string("exception") : h.class_name;
      local_types[h.var_name] = &*bound_type;
    }
    ++except_handler_depth;
    if (h.body) emit_stmt(*h.body);
    --except_handler_depth;
    local_scope = std::move(saved_locals);
    local_types = std::move(saved_types);
    dedent();
    emitln("}");
  }
  if (t.except_else) {
    emitln("else {");
    indent();
    emitln(handled_name + " = true;");
    ++except_handler_depth;
    emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
    emitln("}");
  }
  emitln("if (!" + handled_name + ") throw;");
  dedent();
  if (t.except_else) {
    emitln("} catch (...) {");
    indent();
    ++except_handler_depth;
    emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
  }
  emitln("}");
}

void Emitter::emit_stmt(const Stmt& s) {
  switch (s.kind) {
    case Kind::Compound: {
      const auto& c = static_cast<const Compound&>(s);
      emitln("{");
      indent();
      for (const auto& sub : c.body) emit_stmt(*sub);
      dedent();
      emitln("}");
      break;
    }
    case Kind::EmptyStmt: {
      emitln(";");
      break;
    }
    case Kind::Assign: {
      const auto& a = static_cast<const Assign&>(s);
      // Pascal `T(lv) := rhs` writes through a cast view of the same
      // storage. Emit that storage reinterpret explicitly.
      if (a.target->kind == Kind::Call) {
        const auto& c = static_cast<const Call&>(*a.target);
        if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
          if (std::string ptr = primitive_cast_untyped_storage_ptr(c);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*c.callee);
            emitln("::rt::tp2cc_reinterpret_store<" + primitive_type_cxx(id.name) +
                   ">(" + ptr + ", " + expr_to_cxx(*a.value) + ");");
            break;
          }
          // `T(packed_record.field) := rhs` -- forming a `T&` to a packed
          // field is UB, so route the assignment through `memcpy` via
          // `tp2cc_reinterpret_store` instead of `lvalue_ref = rhs;`.
          if (std::string ptr = primitive_cast_packed_field_ptr(c);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*c.callee);
            emitln("::rt::tp2cc_reinterpret_store<" + primitive_type_cxx(id.name) +
                   ">(" + ptr + ", " + expr_to_cxx(*a.value) + ");");
            break;
          }
          if (std::string ref = primitive_cast_lvalue_ref(c); !ref.empty()) {
            emitln(ref + " = " + expr_to_cxx(*a.value) + ";");
            break;
          }
          const auto& id = static_cast<const Ident&>(*c.callee);
          const TypeExpr* tgt = nullptr;
          auto lit = local_type_aliases_scoped.find(id.name);
          if (lit != local_type_aliases_scoped.end() && lit->second) {
            tgt = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(id.name);
            if (ait != registry->aliases.end() && ait->second.target.get()) {
              tgt = registry->canonicalize(ait->second.target.get());
            }
          }
          if (tgt && tgt->kind == Kind::TyPointer) {
            std::string lv = expr_to_cxx(*c.args[0]);
            std::string rhs = expr_to_cxx(*a.value);
            emitln(reinterpret_ref_text(type_name_text_to_cxx(id.name), lv,
                                        expr_is_untyped_storage_ref(*c.args[0])) +
                   " = " + rhs + ";");
            break;
          }
        }
      }
      if (registry && a.target->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*a.target);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
            std::vector<const Expr*> no_indices;
            emitln(lower_property_write(a.loc, expr_to_cxx(*mem.base), cls, *prop,
                                        no_indices, *a.value) +
                   ";");
            break;
          }
        }
      }
      if (registry && a.target->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*a.target);
        if (auto text = maybe_lower_implicit_property_write(a.loc, id.name,
                                                            *a.value)) {
          emitln(*text + ";");
          break;
        }
      }
      if (registry && a.target->kind == Kind::Index) {
        const auto& ix = static_cast<const Index&>(*a.target);
        if (auto view = untyped_storage_index_view(ix)) {
          emitln("::rt::tp2cc_reinterpret_store<" + view->elem_cxx + ">(" +
                 view->ptr_cxx + ", " + expr_to_cxx(*a.value) + ");");
          break;
        }
        std::vector<const Expr*> indices;
        for (const auto& idx : ix.indices) indices.push_back(idx.get());
        if (ix.base->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*ix.base);
          std::string cls;
          if (mem.base->kind == Kind::Ident &&
              static_cast<const Ident&>(*mem.base).name == "self") {
            cls = current_class_name;
          } else {
            cls = deduce_class_alias(*mem.base);
          }
          if (!cls.empty()) {
            if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
              if (!prop->params.empty()) {
                emitln(lower_property_write(a.loc, expr_to_cxx(*mem.base), cls,
                                            *prop, indices, *a.value) +
                       ";");
                break;
              }
            }
          }
        }
        if (ix.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*ix.base);
          if (auto found = find_implicit_class_property(id.name);
              found && found->prop && !found->prop->params.empty()) {
            emitln(lower_property_write(a.loc, found->base_cxx,
                                        found->class_name, *found->prop,
                                        indices, *a.value) +
                   ";");
            break;
          }
        }
        std::string cls = deduce_class_alias(*ix.base);
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_default_property(cls)) {
            emitln(lower_property_write(a.loc, expr_to_cxx(*ix.base), cls, *prop,
                                        indices, *a.value) +
                   ";");
            break;
          }
        }
      }
      // Enable LHS-rewrite for the function name so that Pascal
      // `funcname := x`, `funcname[i] := x`, `funcname.field := x` etc.
      // all route to the result slot. We only scope the rewrite to the
      // target emission so the RHS still sees the function for recursive
      // calls.
      lhs_fn_rewrite = current_fn_is_function ? current_fn_name : "";
      lhs_fn_rewrite_slot =
          current_fn_is_function ? current_result_slot_name : "";
      lhs_outer_result_rewrite = outer_result_name;
      lhs_outer_result_rewrite_slot = outer_result_slot_name;
      std::string target_cxx = expr_to_cxx(*a.target);
      lhs_fn_rewrite.clear();
      lhs_fn_rewrite_slot.clear();
      lhs_outer_result_rewrite.clear();
      lhs_outer_result_rewrite_slot.clear();
      const TypeExpr* target_ty = deduce_type(*a.target);
      std::string rhs_cxx = const_value_to_cxx(*a.value, target_ty);
      if (target_ty && shortstring_capacity_to_cxx(target_ty)) {
        emitln("::rt::tp2cc_shortstring_assign(" + target_cxx + ", " + rhs_cxx +
               ");");
        break;
      }
      emitln(target_cxx + " = " + rhs_cxx + ";");
      break;
    }
    case Kind::ExprStmt: {
      const auto& es = static_cast<const ExprStmt&>(s);
      // Pascal builtin control-flow statements (break / continue / exit)
      // and allocation builtins (new / dispose) need special lowering.
      // Classify once, then handle in a single if/else chain.
      std::string name;
      const Call* call_expr = nullptr;
      if (es.expr->kind == Kind::Ident) {
        name = static_cast<const Ident&>(*es.expr).name;
      } else if (es.expr->kind == Kind::Call) {
        call_expr = &static_cast<const Call&>(*es.expr);
        if (call_expr->callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*call_expr->callee).name;
        }
      }

      if (name == "break") {
        // Pascal break exits the enclosing loop even from inside a case.
        // Emit as goto so switch nesting can't swallow it.
        if (!loop_break_labels.empty()) {
          emitln("goto " + loop_break_labels.back() + ";");
        } else {
          emitln("break;");  // outside any loop -- let C++ diagnose
        }
      } else if (name == "continue") {
        if (!loop_continue_labels.empty()) {
          emitln("goto " + loop_continue_labels.back() + ";");
        } else {
          emitln("continue;");
        }
      } else if (name == "exit") {
        // exit or exit(v). In a Function, fill the result slot and return;
        // in a Procedure, return; in a Constructor, return the status.
        if (call_expr && !call_expr->args.empty() && current_fn_is_function) {
          emitln(current_result_slot_name + " = " +
                 const_value_to_cxx(*call_expr->args[0], current_fn_result_type) +
                 ";");
          emitln(std::string("return ") + current_result_slot_name + ";");
        } else if (current_fn_is_function || current_fn_is_ctor) {
          emitln(std::string("return ") +
                 (current_fn_is_function ? current_result_slot_name
                                         : kCtorStatusSlotName) +
                 ";");
        } else {
          emitln("return;");
        }
      } else if (name == "fail") {
        if (current_fn_is_ctor) {
          emitln(std::string(kCtorStatusSlotName) + " = false;");
          emitln(std::string("return ") + kCtorStatusSlotName + ";");
        } else {
          report_error(es.loc, "`fail` outside constructors is unsupported");
        }
      } else if (name == "new" && call_expr && !call_expr->args.empty()) {
        // new(p) or new(p, Ctor(args)). `p` might be `arr[i]` whose
        // `decltype` is a reference (`T&`); strip it before computing
        // the pointee so `new remove_pointer_t<T&>` doesn't arise. Route
        // statement-form Pascal `new` through the runtime helper rather than
        // raw C++ `new`, so later `reallocmem` / `dispose` on the same typed
        // storage stays in one allocation family.
        std::string p = lower_call_arg(*call_expr->args[0],
                                       /*param_type=*/nullptr,
                                       /*untyped_arg=*/false,
                                       /*mutable_ref_arg=*/true);
        emitln("::rt::p_new(" + p + ");");
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          const TypeExpr* ptr_arg_ty = deduce_type(*call_expr->args[0]);
          std::string args;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            const ProcDecl* ctor_decl = nullptr;
            if (registry && ptr_arg_ty && cc.callee->kind == Kind::Ident) {
              std::string pointee = registry->pointer_target_type_name(ptr_arg_ty);
              if (!pointee.empty()) {
                if (auto* m = registry->lookup_class_method(
                        pointee, static_cast<const Ident&>(*cc.callee).name)) {
                  ctor_decl = m->decl.get();
                }
              }
            }
            std::vector<bool> untyped_arg(cc.args.size(), false);
            std::vector<bool> mutable_ref_arg(cc.args.size(), false);
            std::vector<const TypeExpr*> param_types(cc.args.size(), nullptr);
            mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                 param_types);
            for (size_t i = 0; i < cc.args.size(); ++i) {
              if (i) args += ", ";
              args += lower_call_arg(*cc.args[i], param_types[i],
                                     untyped_arg[i], mutable_ref_arg[i]);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) {
            emitln("(*" + p + ")." + method + "(" + args + ");");
          }
        }
      } else if (name == "dispose" && call_expr && !call_expr->args.empty()) {
        // dispose(p) or dispose(p, Done)
        std::string p = lower_call_arg(*call_expr->args[0],
                                       /*param_type=*/nullptr,
                                       /*untyped_arg=*/false,
                                       /*mutable_ref_arg=*/true);
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) emitln("(*" + p + ")." + method + "();");
        }
        emitln("::rt::p_dispose(" + p + ");");
      } else if (es.expr->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*es.expr);
        if (auto free_call = maybe_lower_class_free_member(*mem.base, mem.name)) {
          emitln(*free_call + ";");
        } else {
          std::string text = expr_to_cxx(*es.expr);
          bool stmt_autocalls_member = false;
          if (registry) {
            std::string cls = deduce_class_alias(*mem.base);
            if (!cls.empty()) {
              if (const auto* method = registry->lookup_class_method(cls, mem.name)) {
                stmt_autocalls_member = method->accepts_zero_args;
              } else if (ascii_lower(mem.name) == "destroy") {
                if (const auto* ci = class_info_for_type_name(cls)) {
                  stmt_autocalls_member = ci->is_reference_type;
                }
              }
            }
          }
          auto stmt_autocalls_procvar = [&](const Expr& expr) -> bool {
            switch (expr.kind) {
              case Kind::Ident:
              case Kind::Member:
              case Kind::Index:
              case Kind::Deref:
                break;
              default:
                return false;
            }
            if (const TypeExpr* t = deduce_type(expr);
                t && (t = canonicalize_type(t)) &&
                t->kind == Kind::TyProcedural) {
              const auto& p = static_cast<const TyProcedural&>(*t);
              return procedural_param_count(p) == 0;
            }
            return false;
          };
          if ((stmt_autocalls_member || stmt_autocalls_procvar(*es.expr)) &&
              (text.empty() || text.back() != ')')) {
            text += "()";
          }
          emitln(text + ";");
        }
      } else {
        // `expr_to_cxx` auto-calls parameterless procs/methods in
        // value context via `resolve_name`. The one extra case we
        // handle here: Pascal statement-form `writeln;` / `readln;`
        // / `halt;` -- rt variadic builtins where 0 args is a
        // legitimate call. We don't try to auto-call parameterful
        // callables -- those are either emitted as `Call` (handled
        // above with args) or they're real source bugs (like an
        // `inherited init;` whose parent wants an arg) that should
        // be fixed in the Pascal, not papered over.
        std::string text = expr_to_cxx(*es.expr);
        if (es.expr->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*es.expr);
          ResolveResult rr = resolve_name(id.name);
          if (rr.accepts_zero_args &&
              !text.empty() && text.back() != ')') {
            text += "()";
          }
        }
        // Parameterless procedural variables are callable in statement
        // position (`olddo_stop;`) but must stay as plain values in
        // assignments like `do_stop := olddo_stop;`. Detect that only here.
        auto stmt_autocalls_procvar = [&](const Expr& expr) -> bool {
          switch (expr.kind) {
            case Kind::Ident:
            case Kind::Member:
            case Kind::Index:
            case Kind::Deref:
              break;
            default:
              return false;
          }
          if (const TypeExpr* t = deduce_type(expr);
              t && (t = canonicalize_type(t)) &&
              t->kind == Kind::TyProcedural) {
            const auto& p = static_cast<const TyProcedural&>(*t);
            return procedural_param_count(p) == 0;
          }
          return false;
        };
        if (stmt_autocalls_procvar(*es.expr)) text += "()";
        emitln(text + ";");
      }
      break;
    }
    case Kind::If: {
      const auto& i = static_cast<const If&>(s);
      emitln("if (" + expr_to_cxx(*i.cond) + ") {");
      indent();
      if (i.then_branch) emit_stmt(*i.then_branch);
      dedent();
      if (i.else_branch) {
        emitln("} else {");
        indent();
        emit_stmt(*i.else_branch);
        dedent();
      }
      emitln("}");
      break;
    }
    case Kind::While: {
      const auto& w = static_cast<const While&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      emitln("while (" + expr_to_cxx(*w.cond) + ") {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (w.body) emit_stmt(*w.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::Repeat: {
      const auto& r = static_cast<const Repeat&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      emitln("do {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      for (const auto& sub : r.body) emit_stmt(*sub);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("} while (!(" + expr_to_cxx(*r.cond) + "));");
      emitln(brk + ":;");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      ResolveResult vr = resolve_name(f.var);
      std::string var = vr.cxx.empty() ? mangle(f.var) : vr.cxx;
      std::string from = expr_to_cxx(*f.from);
      std::string to = expr_to_cxx(*f.to);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      // Pascal `for X := A to B do S` is NOT `for (X=A; X<=B; ++X)`:
      // when X's type is `byte` and B is 255, ++X wraps to 0 and the
      // condition never fails. True semantics: body runs for each X in
      // [A,B]; terminate by equality after the body. Snapshot the end
      // bound so mid-body assignments to B don't alter the loop count.
      emitln("{");
      indent();
      emitln("auto tp2cc_from = (" + from + ");");
      emitln("auto tp2cc_to = (" + to + ");");
      const char* cmp = f.downto ? ">=" : "<=";
      const char* step = f.downto ? "::rt::p_dec" : "::rt::p_inc";
      emitln(std::string("if (tp2cc_from ") + cmp + " tp2cc_to) {");
      indent();
      emitln(var + " = tp2cc_from;");
      emitln("while (true) {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (f.body) emit_stmt(*f.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      emitln("if (" + var + " == tp2cc_to) break;");
      emitln(step + std::string("(") + var + ");");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      auto selector_is_charish = [&]() -> bool {
        const TypeExpr* t = deduce_type(*cs.selector);
        if (!t) return false;
        t = canonicalize_type(t);
        return tyname_is(t, "char");
      };
      auto case_expr = [&](const Expr& e) -> std::string {
        std::string text = expr_to_cxx(e);
        return selector_is_charish() ? "::rt::p_ord(" + text + ")" : text;
      };
      emitln("switch (" + case_expr(*cs.selector) + ") {");
      indent();
      for (const auto& arm : cs.arms) {
        for (const auto& lab : arm.labels) {
          if (lab->kind == Kind::Range) {
            // GCC case-range extension: `case lo ... hi:`. Acceptable here;
            // the gnu profile compiler supports it. TODO: iterate label
            // values for strict standard C++.
            const auto& r = static_cast<const Range&>(*lab);
            emitln("case " + case_expr(*r.lo) + " ... " +
                   case_expr(*r.hi) + ":");
          } else {
            emitln("case " + case_expr(*lab) + ":");
          }
        }
        indent();
        if (arm.body) emit_stmt(*arm.body);
        emitln("break;");
        dedent();
      }
      if (cs.else_branch) {
        emitln("default:");
        indent();
        emit_stmt(*cs.else_branch);
        emitln("break;");
        dedent();
      }
      dedent();
      emitln("}");
      break;
    }
    case Kind::With: {
      // Pascal `with A, B do S` opens A's and B's fields (and methods)
      // as unqualified names inside S. We alias each target and push
      // its deduced type onto `with_stack`; bare
      // idents inside S that match a field of any stacked type are
      // rewritten by the expression emitter to that alias.
      const auto& w = static_cast<const With&>(s);
      emitln("{");
      indent();
      size_t pushed = 0;
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        const Expr& with_expr = *w.exprs[i];
        const TypeExpr* ty = deduce_type(with_expr);
        if (ty) ty = canonicalize_type(ty);
        std::string nm = "tp2cc_with_" + std::to_string(with_stack.size());
        std::string init = expr_to_cxx(with_expr);
        bool bind_by_ref = expr_is_storage_lvalue(with_expr);
        // `with T(p) do` and similar casts produce pointer rvalues. Bind those
        // by value; only genuine lvalues can be safely aliased with `auto&`.
        emitln(std::string(bind_by_ref ? "auto& " : "auto ") + nm + " = " +
               init + ";");
        WithBind wb;
        wb.cxx_text = nm;
        wb.type = ty;
        wb.class_name = deduce_class_alias(with_expr);
        wb.access_op = member_access_op(with_expr);
        with_stack.push_back(std::move(wb));
        ++pushed;
      }
      if (w.body) emit_stmt(*w.body);
      for (size_t i = 0; i < pushed; ++i) with_stack.pop_back();
      dedent();
      emitln("}");
      break;
    }
    case Kind::Goto: {
      const auto& g = static_cast<const Goto&>(s);
      emitln("goto p_" + g.label + ";");
      break;
    }
    case Kind::Labeled: {
      const auto& lb = static_cast<const Labeled&>(s);
      emitln("p_" + lb.label + ":");
      if (lb.body) emit_stmt(*lb.body);
      break;
    }
    case Kind::AsmStmt: {
      report_error(s.loc, "asm blocks are unsupported");
      emitln("/* unsupported asm */");
      break;
    }
    case Kind::Raise:
      emit_raise_stmt(static_cast<const Raise&>(s));
      break;
    case Kind::Try:
      emit_try_stmt(static_cast<const Try&>(s));
      break;
    default:
      report_error(s.loc,
                   "unsupported statement kind " +
                       std::to_string(static_cast<int>(s.kind)));
      emitln("/* unsupported-stmt kind=" +
             std::to_string(static_cast<int>(s.kind)) + " */;");
      break;
  }
}

// Forward decl so emit_proc_body / emit_nested_proc_lambda can call it
// to forward-declare record/object types in local type-decls before
// pointer aliases that reference them.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<ast::DeclPtr>& decls);

void Emitter::emit_proc_body(const ProcDecl& pd) {
  // Header line: ret ClassName::Method(args) or ret Method(args).
  std::string ret = proc_return_type_to_cxx(pd);
  std::string qname = mangle(pd.name);
  if (!pd.of_type.empty()) qname = mangle(pd.of_type) + "::" + qname;
  emitln(ret + " " + qname + "(" + param_list_to_cxx(pd.params) + ") {");
  indent();

  if (pd.is_abstract && !pd.body) {
    // Pascal's abstract methods are often placeholder hooks on classes that
    // native FPC still instantiates. Emit a fail-fast body instead of a pure
    // virtual so the translated class layout stays constructible while any
    // accidental call still stops immediately.
    emitln("::std::abort();");
    dedent();
    emitln("}");
    return;
  }

  // Save outer state and set for this body.
  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  const ast::TypeExpr* saved_result_type = current_fn_result_type;
  std::string saved_result_slot_name = current_result_slot_name;
  std::string saved_bare_result_slot_name = bare_result_slot_name;
  const ast::TypeExpr* saved_bare_result_type = bare_result_type;
  std::string saved_outer_result_name = outer_result_name;
  std::string saved_outer_result_slot_name = outer_result_slot_name;
  const ast::TypeExpr* saved_outer_result_type = outer_result_type;
  std::string saved_class = current_class_name;
  auto saved_locals = local_scope;
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = (pd.pkind == ProcKind::Constructor);
  current_fn_result_type = pd.return_type.get();
  std::string inherited_outer_result_name;
  std::string inherited_outer_result_slot_name;
  const ast::TypeExpr* inherited_outer_result_type = nullptr;
  if (saved_fn && saved_result_type) {
    inherited_outer_result_name = saved_name;
    inherited_outer_result_slot_name = saved_result_slot_name;
    inherited_outer_result_type = saved_result_type;
  } else {
    inherited_outer_result_name = saved_outer_result_name;
    inherited_outer_result_slot_name = saved_outer_result_slot_name;
    inherited_outer_result_type = saved_outer_result_type;
  }
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    current_result_slot_name =
        inherited_outer_result_type ? nested_result_slot_name(pd.name)
                                    : std::string(kPascalResultSlotName);
    bare_result_slot_name = current_result_slot_name;
    bare_result_type = pd.return_type.get();
  } else {
    current_result_slot_name.clear();
    bare_result_slot_name = inherited_outer_result_slot_name;
    bare_result_type = inherited_outer_result_type;
  }
  outer_result_name = inherited_outer_result_name;
  outer_result_slot_name = inherited_outer_result_slot_name;
  outer_result_type = inherited_outer_result_type;
  current_class_name = pd.of_type;  // empty for free functions
  ++block_depth;

  // Populate local-scope set so the expression emitter won't auto-call
  // identifiers that happen to name a parameterless method in another
  // unit (e.g. a local `typename: string;` shadowing a method). Also
  // record declared types so `.field` / `.method` access on those
  // locals can be resolved from the type registry.
  auto saved_types = local_types;
  auto saved_consts = local_consts;
  auto saved_nested = local_nested_fns;
  auto saved_nested_forwards = local_nested_forwards;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_const_params = local_const_params;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto insert_local_name = [&](Location where, const std::string& name) {
    // Pascal functions already own an implicit `Result` variable, so any
    // local/parameter/const nested in that body may not reuse the name.
    if (bare_result_type && is_pascal_result_ident(name)) {
      report_error(where, "duplicate identifier `Result`");
      return false;
    }
    local_scope.insert(name);
    return true;
  };
  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      if (!insert_local_name(pd.loc, nm)) continue;
      if (p.type) {
        local_types[nm] = p.type.get();
        if (p.mode == Param::Const) local_const_params.insert(nm);
      } else {
        local_untyped_params.insert(nm);
      }
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        if (!insert_local_name(vd.loc, nm)) continue;
        if (vd.type) local_types[nm] = vd.type.get();
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      if (!insert_local_name(cd.loc, cd.name)) continue;
      local_consts[cd.name] = &cd;
      if (const TypeExpr* ct = deduce_const_decl_type(cd)) {
        local_types[cd.name] = ct;
      }
    } else if (l->kind == Kind::TypeDecl) {
      // Pascal's local `type` section is statically visible to the
      // translator too -- record enums (for array-dim sizing and
      // `low(T)`/`high(T)` rewrites) and aliases (for canonicalize).
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      if (!insert_local_name(npd.loc, npd.name)) continue;
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.accepts_zero_args = proc_accepts_zero_args(npd);
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      nf.decl = &npd;
      local_nested_fns[npd.name] = nf;
    }
  }

  // `Result` is a Pascal-visible implicit variable in functions, so it
  // uses ordinary Pascal name mangling. Declare it before nested local
  // procedures/functions: Pascal lets those inner routines read and write
  // the enclosing function result, so the generated lambda must be able to
  // capture an already-declared C++ local.
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " " + current_result_slot_name + "{};");
  } else if (pd.pkind == ProcKind::Constructor) {
    emitln(std::string("bool ") + kCtorStatusSlotName + " = true;");
  }
  // Forward-declare any record/object types in locals so a pointer
  // alias that textually precedes its target still compiles inside
  // the function body.
  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function ||
      pd.pkind == ProcKind::Constructor) {
    emitln(std::string("return ") +
           (pd.pkind == ProcKind::Function ? current_result_slot_name
                                           : kCtorStatusSlotName) +
           ";");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  current_fn_result_type = saved_result_type;
  current_result_slot_name = std::move(saved_result_slot_name);
  bare_result_slot_name = std::move(saved_bare_result_slot_name);
  bare_result_type = saved_bare_result_type;
  outer_result_name = std::move(saved_outer_result_name);
  outer_result_slot_name = std::move(saved_outer_result_slot_name);
  outer_result_type = saved_outer_result_type;
  current_class_name = std::move(saved_class);
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_consts = std::move(saved_consts);
  local_nested_fns = std::move(saved_nested);
  local_nested_forwards = std::move(saved_nested_forwards);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_const_params = std::move(saved_local_const_params);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("}");
}

void Emitter::emit_nested_proc_lambda(const ProcDecl& pd) {
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  // Build the param-type list for the std::function signature.
  std::string sig_params;
  {
    bool first = true;
    for (const auto& p : pd.params) {
      std::string pt;
      if (!p.type) {
        pt = "void*";
      } else {
        pt = type_to_cxx(*p.type);
        if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
        else if (p.mode == Param::Const &&
                 const_param_needs_mutable_ref(p.type.get()))
          pt += "&";
        else if (p.mode == Param::Const &&
                 const_param_needs_const_ref(p.type.get()))
          pt = "const " + pt + "&";
      }
      for (const auto& n : p.names) {
        (void)n;
        if (!first) sig_params += ", ";
        first = false;
        sig_params += pt;
      }
      if (p.names.empty()) {
        if (!first) sig_params += ", ";
        first = false;
        sig_params += pt;
      }
    }
  }

  const std::string lname = mangle(pd.name);
  // Forward-declare the std::function so the lambda can recurse by name.
  if (!local_nested_forwards.count(pd.name)) {
    emitln("::std::function<" + ret + "(" + sig_params + ")> " + lname + ";");
  }
  emitln(lname + " = [&](" + param_list_to_cxx(pd.params) + ") -> " + ret +
         " {");
  indent();

  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  const ast::TypeExpr* saved_result_type = current_fn_result_type;
  std::string saved_result_slot_name = current_result_slot_name;
  std::string saved_bare_result_slot_name = bare_result_slot_name;
  const ast::TypeExpr* saved_bare_result_type = bare_result_type;
  std::string saved_outer_result_name = outer_result_name;
  std::string saved_outer_result_slot_name = outer_result_slot_name;
  const ast::TypeExpr* saved_outer_result_type = outer_result_type;
  auto saved_locals = local_scope;
  auto saved_types = local_types;
  auto saved_consts = local_consts;
  auto saved_nested = local_nested_fns;
  auto saved_nested_forwards = local_nested_forwards;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_const_params = local_const_params;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto insert_local_name = [&](Location where, const std::string& name) {
    if (bare_result_type && is_pascal_result_ident(name)) {
      report_error(where, "duplicate identifier `Result`");
      return false;
    }
    local_scope.insert(name);
    return true;
  };
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = false;
  current_fn_result_type = pd.return_type.get();
  std::string inherited_outer_result_name;
  std::string inherited_outer_result_slot_name;
  const ast::TypeExpr* inherited_outer_result_type = nullptr;
  if (saved_fn && saved_result_type) {
    inherited_outer_result_name = saved_name;
    inherited_outer_result_slot_name = saved_result_slot_name;
    inherited_outer_result_type = saved_result_type;
  } else {
    inherited_outer_result_name = saved_outer_result_name;
    inherited_outer_result_slot_name = saved_outer_result_slot_name;
    inherited_outer_result_type = saved_outer_result_type;
  }
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    current_result_slot_name =
        inherited_outer_result_type ? nested_result_slot_name(pd.name)
                                    : std::string(kPascalResultSlotName);
    bare_result_slot_name = current_result_slot_name;
    bare_result_type = pd.return_type.get();
  } else {
    current_result_slot_name.clear();
    bare_result_slot_name = inherited_outer_result_slot_name;
    bare_result_type = inherited_outer_result_type;
  }
  outer_result_name = inherited_outer_result_name;
  outer_result_slot_name = inherited_outer_result_slot_name;
  outer_result_type = inherited_outer_result_type;
  ++block_depth;

  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      if (!insert_local_name(pd.loc, nm)) continue;
      if (p.type) {
        local_types[nm] = p.type.get();
        if (p.mode == Param::Const) local_const_params.insert(nm);
      } else {
        local_untyped_params.insert(nm);
      }
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        if (!insert_local_name(vd.loc, nm)) continue;
        if (vd.type) local_types[nm] = vd.type.get();
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      if (!insert_local_name(cd.loc, cd.name)) continue;
      local_consts[cd.name] = &cd;
      if (const TypeExpr* ct = deduce_const_decl_type(cd)) {
        local_types[cd.name] = ct;
      }
    } else if (l->kind == Kind::TypeDecl) {
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      if (!insert_local_name(npd.loc, npd.name)) continue;
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.accepts_zero_args = proc_accepts_zero_args(npd);
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      nf.decl = &npd;
      local_nested_fns[npd.name] = nf;
    }
  }

  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " " + current_result_slot_name + "{};");
  }
  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function) {
    emitln(std::string("return ") + current_result_slot_name + ";");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  current_fn_result_type = saved_result_type;
  current_result_slot_name = std::move(saved_result_slot_name);
  bare_result_slot_name = std::move(saved_bare_result_slot_name);
  bare_result_type = saved_bare_result_type;
  outer_result_name = std::move(saved_outer_result_name);
  outer_result_slot_name = std::move(saved_outer_result_slot_name);
  outer_result_type = saved_outer_result_type;
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_consts = std::move(saved_consts);
  local_nested_fns = std::move(saved_nested);
  local_nested_forwards = std::move(saved_nested_forwards);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_const_params = std::move(saved_local_const_params);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("};");
}

// ---------------------------------------------------------------------------
// Unit

// Scan the decl list and emit forward declarations for every record/object
// type, so a pointer type that textually precedes its target still compiles.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    if (td.type->kind == Kind::TyRecord || td.type->kind == Kind::TyObject) {
      e.emitln("struct " + std::string("p_") + td.name + ";");
      if (td.type->kind == Kind::TyObject &&
          static_cast<const TyObject&>(*td.type).is_reference_type) {
        e.emitln("struct tp2cc_metaclass_" + std::string("p_") + td.name + ";");
      }
    }
  }
}

// Collect every TyName (lowercased) mentioned in a TypeExpr. Recurses into
// records/objects so that a record's field types contribute dependencies.
static void collect_type_refs(const TypeExpr& t,
                              std::unordered_set<std::string>& out) {
  switch (t.kind) {
    case Kind::TyName:
      out.insert(static_cast<const TyName&>(t).name);
      return;
    case Kind::TyPointer:
      if (static_cast<const TyPointer&>(t).target)
        collect_type_refs(*static_cast<const TyPointer&>(t).target, out);
      return;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      for (const auto& d : a.dims) if (d) collect_type_refs(*d, out);
      if (a.element) collect_type_refs(*a.element, out);
      return;
    }
    case Kind::TySet:
      if (static_cast<const TySet&>(t).element)
        collect_type_refs(*static_cast<const TySet&>(t).element, out);
      return;
    case Kind::TyFile:
      if (static_cast<const TyFile&>(t).element)
        collect_type_refs(*static_cast<const TyFile&>(t).element, out);
      return;
    case Kind::TyRecord: {
      const auto& r = static_cast<const TyRecord&>(t);
      for (const auto& f : r.fields) if (f.type) collect_type_refs(*f.type, out);
      for (const auto& vc : r.variant_cases)
        for (const auto& f : vc.fields)
          if (f.type) collect_type_refs(*f.type, out);
      return;
    }
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      if (!o.parent.empty()) out.insert(o.parent);
      for (const auto& m : o.members) {
        if (m.kind == ObjectMemberKind::Field && m.field_type) {
          collect_type_refs(*m.field_type, out);
        } else if (m.kind == ObjectMemberKind::Method && m.method) {
          if (m.method->return_type) collect_type_refs(*m.method->return_type, out);
          for (const auto& p : m.method->params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        } else if (m.kind == ObjectMemberKind::Property) {
          if (m.property.type) collect_type_refs(*m.property.type, out);
          for (const auto& p : m.property.params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        }
      }
      return;
    }
    case Kind::TySubrange:
    case Kind::TyString:
    case Kind::TyEnum:
      return;
    case Kind::TyProcedural: {
      const auto& p = static_cast<const TyProcedural&>(t);
      if (p.return_type) collect_type_refs(*p.return_type, out);
      for (const auto& par : p.params) {
        if (par.type) collect_type_refs(*par.type, out);
      }
      return;
    }
    default:
      return;
  }
}

// Reorder type decls so every alias (non-record, non-object) appears after
// the types it references by name. Record/object types are already
// forward-declared by emit_forward_struct_decls, so aliases that point to
// them via `^T` always work; this function only needs to handle aliases
// that depend on other aliases (e.g. `pfoo = ^tfoo` where `tfoo` is itself
// an alias to an array type).
//
// `in` must contain only type decls (checked by the caller); this runs
// against a single contiguous Pascal `type` section.
static std::vector<const Decl*> ordered_type_decls(
    const std::vector<const Decl*>& in) {
  std::vector<const Decl*> type_decls(in);

  // Map name -> index for quick lookup.
  std::unordered_map<std::string, int> index_of;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    index_of[static_cast<const TypeDecl*>(type_decls[i])->name] = i;
  }

  // For each type decl, which in-unit types does it reference?
  std::vector<std::unordered_set<int>> deps(type_decls.size());
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    const auto& td = *static_cast<const TypeDecl*>(type_decls[i]);
    if (!td.type) continue;
    std::unordered_set<std::string> refs;
    collect_type_refs(*td.type, refs);
    for (const auto& r : refs) {
      auto it = index_of.find(r);
      if (it == index_of.end()) continue;  // external / primitive
      int j = it->second;
      if (j == i) continue;
      const auto& rd = *static_cast<const TypeDecl*>(type_decls[j]);
      // Pointer-to-record aliases don't need the record body before them:
      // `using p_pfoo = p_tfoo*;` only needs the struct forward declaration
      // (emitted by emit_forward_struct_decls). This break lets cycles
      // like `Pfoo = ^Tfoo; Tfoo = record next: Pfoo; end;` remain a DAG.
      if (rd.type && (rd.type->kind == Kind::TyRecord ||
                      rd.type->kind == Kind::TyObject) &&
          td.type->kind == Kind::TyPointer) {
        continue;
      }
      deps[i].insert(j);
    }
  }

  // Kahn topological sort (stable: ties broken by original order).
  std::vector<int> indeg(type_decls.size(), 0);
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    for (int j : deps[i]) (void)j, ++indeg[i];
  }
  std::vector<int> ready;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (indeg[i] == 0) ready.push_back(i);
  }
  std::vector<const Decl*> out;
  std::unordered_set<int> emitted_set;
  while (!ready.empty()) {
    int n = ready.front();
    ready.erase(ready.begin());
    out.push_back(type_decls[n]);
    emitted_set.insert(n);
    for (int i = 0; i < (int)type_decls.size(); ++i) {
      if (!deps[i].count(n)) continue;
      if (--indeg[i] == 0) ready.push_back(i);
    }
  }
  // Anything left has a cycle among non-pointer aliases. Emit in source
  // order as a fallback -- probably won't compile, but we don't silently
  // drop declarations.
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (!emitted_set.count(i)) out.push_back(type_decls[i]);
  }
  return out;
}

void Emitter::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;  // used for the #include stem
  current_unit_name = ascii_lower(u.name);
  auto saved_local_enums = local_enums;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto seed_unit_type_scope = [&](const std::vector<DeclPtr>& decls) {
    for (const auto& d : decls) {
      if (d->kind != Kind::TypeDecl) continue;
      const auto& td = static_cast<const TypeDecl&>(*d);
      if (!td.type) continue;
      if (td.type->kind == Kind::TyEnum) {
        local_enums[td.name] =
            static_cast<const ast::TyEnum*>(td.type.get());
      } else if (td.type->kind != Kind::TyRecord &&
                 td.type->kind != Kind::TyObject) {
        local_type_aliases_scoped[td.name] = td.type.get();
      }
    }
  };
  if (current_unit_name == "tpexcept") {
    emit_tpexcept_unit(u);
    local_enums = std::move(saved_local_enums);
    local_type_aliases_scoped = std::move(saved_local_aliases);
    return;
  }

  // Header.
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <array>");
  emitln("#include <limits>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  seed_unit_type_scope(u.interface_decls);
  // Emitted headers are prefixed `p_` so the filename never collides with
  // a C/C++ standard header (e.g. Pascal unit `strings` vs libc strings.h).
  for (const auto& uu : u.interface_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  emit_forward_struct_decls(*this, u.interface_decls);
  // Walk source order. Types are reordered topologically only within a
  // single contiguous run (a Pascal `type` section); any intervening
  // const/var/proc breaks the run. This respects Pascal's rule that
  // forward references are only allowed within the same type section.
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/true);
      }
      run.clear();
    };
    for (const auto& d : u.interface_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, true);
      }
    }
    flush();
  }
  // Forward-declare unit lifecycle hooks so program startup can
  // initialize units and register their cleanup.
  if (!u.is_program) {
    nl();
    emitln(std::string("void ") + kUnitInitName + "();");
    emitln(std::string("void ") + kUnitFiniName + "();");
  }
  nl();
  emitln("}  // namespace " + ns);

  // Implementation.
  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_" + hguard + ".h\"");
  seed_unit_type_scope(u.impl_decls);
  for (const auto& uu : u.impl_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  // The program emits a startup call chain over every parsed unit;
  // include all of their headers so the declarations are visible.
  if (u.is_program && unit_init_order) {
    for (const auto& uu : *unit_init_order) {
      if (uu == u.name) continue;
      emitln("#include \"p_" + uu + ".h\"");
    }
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  emit_forward_struct_decls(*this, u.impl_decls);
  // Emit definitions (not just extern declarations) for interface
  // vars in the .cc so external references resolve at link time.
  for (const auto& d : u.interface_decls) {
    if (d->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*d);
      if (vd.is_external) continue;
      emit_decl(*d, /*in_header=*/false);
    }
  }
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/false);
      }
      run.clear();
    };
    for (const auto& d : u.impl_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, false);
      }
    }
    flush();
  }
  auto emit_unit_hook = [&](const char* name, const StmtPtr& body) {
    nl();
    emitln(std::string("void ") + name + "() {");
    indent();
    ++block_depth;
    if (body) emit_stmt(*body);
    --block_depth;
    dedent();
    emitln("}");
  };

  // Emit the unit/program lifecycle bodies.
  if (!u.is_program) {
    emit_unit_hook(kUnitInitName, u.init_body);
    emit_unit_hook(kUnitFiniName, u.final_body);
    nl();
    emitln("}  // namespace " + ns);
  } else {
    nl();
    emitln("}  // namespace " + ns);
    nl();
    emitln("int main(int argc, char** argv) {");
    indent();
    emitln("::rt::init_argv(argc, argv);");
    if (unit_init_order) {
      // Register each finalizer only after its init hook returns.
      // That gives reverse-order teardown on normal exit/Halt and
      // leaves never-finished units out of the finalization chain.
      for (const auto& uu : *unit_init_order) {
        if (uu == u.name) continue;
        std::string ns_name = mangle(uu);
        emitln(ns_name + "::" + kUnitInitName + "();");
        emitln("if (std::atexit(" + ns_name + "::" + kUnitFiniName +
               ") != 0) std::abort();");
      }
    }
    emitln("using namespace " + ns + ";");
    ++block_depth;
    if (u.init_body) emit_stmt(*u.init_body);
    --block_depth;
    emitln("return 0;");
    dedent();
    emitln("}");
  }
  local_enums = std::move(saved_local_enums);
  local_type_aliases_scoped = std::move(saved_local_aliases);
}

void Emitter::emit_tpexcept_unit(const UnitNode& u) {
  (void)u;
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <setjmp.h>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("struct p_jmp_buf {");
  indent();
  emitln("int32_t p_eax;");
  emitln("int32_t p_ebx;");
  emitln("int32_t p_ecx;");
  emitln("int32_t p_edx;");
  emitln("int32_t p_esi;");
  emitln("int32_t p_edi;");
  emitln("int32_t p_ebp;");
  emitln("int32_t p_esp;");
  emitln("int32_t p_eip;");
  emitln("int32_t p_flags;");
  emitln("uint16_t p_cs;");
  emitln("uint16_t p_ds;");
  emitln("uint16_t p_es;");
  emitln("uint16_t p_fs;");
  emitln("uint16_t p_gs;");
  emitln("uint16_t p_ss;");
  dedent();
  emitln("};");
  emitln("using p_pjmp_buf = p_jmp_buf*;");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("struct p_jump_state {");
  indent();
  emitln("::jmp_buf p_env;");
  dedent();
  emitln("};");
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec);");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("int32_t p_setjmp(p_jmp_buf& p_rec) = delete;");
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value);");
  emitln("inline p_pjmp_buf p_recoverpospointer = nullptr;");
  emitln("inline bool p_longjump_used = false;");
  nl();
  emitln(std::string("void ") + kUnitInitName + "();");
  emitln(std::string("void ") + kUnitFiniName + "();");
  nl();
  emitln("}  // namespace p_tpexcept");

  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_tpexcept.h\"");
  emitln("#include <cstdlib>");
  emitln("#include <unordered_map>");
  nl();
  emitln("namespace {");
  indent();
  emitln("std::unordered_map<const p_tpexcept::p_jmp_buf*,");
  emitln("                   p_tpexcept::p_detail::p_jump_state> p_jump_states;");
  dedent();
  emitln("}  // namespace");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec) {");
  indent();
  emitln("return ::p_jump_states[p_rec];");
  dedent();
  emitln("}");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value) {");
  indent();
  emitln("auto it = ::p_jump_states.find(&p_rec);");
  emitln("if (it == ::p_jump_states.end()) std::abort();");
  emitln("p_longjump_used = true;");
  emitln("::longjmp(it->second.p_env, p_return_value == 0 ? 1 : p_return_value);");
  dedent();
  emitln("}");
  nl();
  emitln(std::string("void ") + kUnitInitName + "() {");
  emitln("}");
  nl();
  emitln(std::string("void ") + kUnitFiniName + "() {");
  emitln("}");
  nl();
  emitln("}  // namespace p_tpexcept");
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u, const TypeRegistry* registry,
                      const std::vector<std::string>* unit_init_order) {
  Emitter e;
  e.registry = registry;
  if (unit_init_order) e.unit_init_order = unit_init_order;
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace tp2cc
