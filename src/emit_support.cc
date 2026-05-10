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

std::string type_mangle(std::string_view name) {
  std::string s("t_");
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
    n.name = "shortstring";
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

namespace {

std::string encode_helper_param_mode(ast::Param::Mode mode) {
  switch (mode) {
    case ast::Param::Value: return "value";
    case ast::Param::Var: return "var";
    case ast::Param::Const: return "const";
    case ast::Param::ConstRef: return "constref";
    case ast::Param::Out: return "out";
  }
  return "value";
}

}  // namespace

std::string encode_helper_type(const ast::TypeExpr& t) {
  switch (t.kind) {
    case ast::Kind::TyName:
      return "name_" +
             encode_helper_ident(static_cast<const ast::TyName&>(t).name);
    case ast::Kind::TyArray: {
      const auto& a = static_cast<const ast::TyArray&>(t);
      std::string out;
      switch (a.array_kind) {
        case ast::ArrayKind::Open:
          out = "openarr";
          break;
        case ast::ArrayKind::Dynamic:
          out = "dynarr";
          break;
        case ast::ArrayKind::Fixed:
          out = "arr";
          out += std::to_string(a.dims.size());
          break;
      }
      out += "_";
      out += a.element ? encode_helper_type(*a.element)
                       : std::string("void");
      return out;
    }
    case ast::Kind::TyRecord:
      return "record";
    case ast::Kind::TyObject: {
      const auto& o = static_cast<const ast::TyObject&>(t);
      return o.is_reference_type ? "class" : "object";
    }
    case ast::Kind::TyInterface:
      return "interface";
    case ast::Kind::TySet:
      return "set_" +
             encode_helper_type(*static_cast<const ast::TySet&>(t).element);
    case ast::Kind::TyFile: {
      const auto& f = static_cast<const ast::TyFile&>(t);
      if (f.is_text) return "text";
      return f.element ? "file_" + encode_helper_type(*f.element)
                       : std::string("file_untyped");
    }
    case ast::Kind::TyPointer: {
      const auto& p = static_cast<const ast::TyPointer&>(t);
      return p.target ? "ptr_" + encode_helper_type(*p.target)
                      : std::string("ptr_void");
    }
    case ast::Kind::TyProcedural: {
      const auto& p = static_cast<const ast::TyProcedural&>(t);
      std::string out = p.is_method ? "method" : "proc";
      out += p.is_function ? "_fn_" : "_proc_";
      out += encode_helper_params(p.params);
      out += "_ret_";
      out += (p.is_function && p.return_type)
                 ? encode_helper_type(*p.return_type)
                 : std::string("void");
      return out;
    }
    case ast::Kind::TyEnum:
      return "enum";
    case ast::Kind::TySubrange:
      return "subrange";
    case ast::Kind::TyString: {
      const auto& s = static_cast<const ast::TyString&>(t);
      return s.max_length ? "string_sized" : std::string("string");
    }
    case ast::Kind::TyMetaclass:
      return "metaclass_" + encode_helper_ident(
                                 static_cast<const ast::TyMetaclass&>(t).class_name);
    case ast::Kind::TyDistinct:
      return "distinct_" + encode_helper_type(
                               *static_cast<const ast::TyDistinct&>(t).underlying);
    default:
      return "type";
  }
}

std::string encode_helper_params(const std::vector<ast::Param>& params) {
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

std::string pascal_operator_cxx_token(std::string_view op) {
  if (op == "+") return "+";
  if (op == "-") return "-";
  if (op == "*") return "*";
  if (op == "/") return "/";
  if (op == "mod") return "%";
  if (op == "=") return "==";
  if (op == "<>") return "!=";
  if (op == "<") return "<";
  if (op == ">") return ">";
  if (op == "<=") return "<=";
  if (op == ">=") return ">=";
  if (op == "and") return "&";
  if (op == "or") return "|";
  if (op == "xor") return "^";
  if (op == "shl") return "<<";
  if (op == "shr") return ">>";
  return {};
}

bool pascal_operator_decl_uses_named_helper(const ast::ProcDecl& pd) {
  if (!pd.is_operator) return false;
  return pd.operator_token == ":=" ||
         pascal_operator_cxx_token(pd.operator_token).empty();
}

std::string pascal_operator_named_helper_name(const ast::ProcDecl& pd) {
  std::string out = "tp2cc_operator_";
  out += encode_helper_ident(pd.operator_token);
  out += "_params_";
  out += encode_helper_params(pd.params);
  out += "_ret_";
  out += pd.return_type ? encode_helper_type(*pd.return_type)
                        : std::string("void");
  return out;
}

std::string pascal_assignment_operator_helper_name(const ast::ProcDecl& pd) {
  std::string out = "tp2cc_operator_assign";
  out += "_params_";
  out += encode_helper_params(pd.params);
  out += "_ret_";
  out += pd.return_type ? encode_helper_type(*pd.return_type)
                        : std::string("void");
  return out;
}

std::string pascal_operator_decl_name_to_cxx(const ast::ProcDecl& pd) {
  if (!pd.is_operator) return mangle(pd.name);
  if (pd.operator_token == ":=") {
    return pascal_assignment_operator_helper_name(pd);
  }
  if (pascal_operator_decl_uses_named_helper(pd)) {
    return pascal_operator_named_helper_name(pd);
  }
  std::string op = pascal_operator_cxx_token(pd.operator_token);
  return op.empty() ? mangle(pd.name) : "operator" + op;
}

size_t procedural_param_count(const ast::TyProcedural& p) {
  size_t count = 0;
  for (const auto& pp : p.params) {
    count += pp.names.empty() ? 1 : pp.names.size();
  }
  return count;
}

std::string enum_bound_name(std::string_view type_name, std::string_view which) {
  return "tp2cc_enum_" + std::string(which) + "_" +
         encode_helper_ident(type_name);
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
      {"currency", {"::rt::t_currency", PrimitiveIntKind::Signed, 64}},
      {"ptrint", {"::rt::t_ptrint", PrimitiveIntKind::Signed, 32}},
      {"ptruint", {"::rt::t_ptruint", PrimitiveIntKind::Unsigned, 32}},
      {"sizeint", {"::rt::t_sizeint", PrimitiveIntKind::Signed, 32}},
      {"sizeuint", {"::rt::t_sizeuint", PrimitiveIntKind::Unsigned, 32}},
      {"shortstring", {"::rt::tp2cc_ShortString<>", PrimitiveIntKind::None, 0}},
      {"ansistring", {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None, 0}},
      {"utf8string", {"::rt::tp2cc_AnsiString", PrimitiveIntKind::None, 0}},
  };
  return m;
}

const std::unordered_map<std::string, const char*>& runtime_named_type_map() {
  static const std::unordered_map<std::string, const char*> m = {
      {"currency", "::rt::t_currency"},
      {"datetime", "::rt::t_datetime"},
      {"hresult", "::rt::t_hresult"},
      {"tdatetime", "::rt::t_tdatetime"},
      {"texecuteflag", "::rt::t_texecuteflag"},
      {"texecuteflags", "::rt::t_texecuteflags"},
      {"tsyscharset", "::rt::t_tsyscharset"},
      {"comstr", "::rt::t_comstr"},
      {"dirstr", "::rt::t_dirstr"},
      {"namestr", "::rt::t_namestr"},
      {"extstr", "::rt::t_extstr"},
      {"pansistring", "::rt::t_pansistring"},
      {"pcardinal", "::rt::t_pcardinal"},
      {"pcurrency", "::rt::t_pcurrency"},
      {"pdword", "::rt::t_pdword"},
      {"pint64", "::rt::t_pint64"},
      {"plongword", "::rt::t_plongword"},
      {"pathstr", "::rt::t_pathstr"},
      {"ppointer", "::rt::t_ppointer"},
      {"pqword", "::rt::t_pqword"},
      {"pshortstring", "::rt::t_pshortstring"},
      {"ptrint", "::rt::t_ptrint"},
      {"ptruint", "::rt::t_ptruint"},
      {"searchrec", "::rt::t_searchrec"},
      {"signalhandler", "::rt::t_signalhandler"},
      {"sizeint", "::rt::t_sizeint"},
      {"sizeuint", "::rt::t_sizeuint"},
      {"stat", "::rt::t_stat"},
      {"tclass", "::rt::t_tclass"},
      {"tfpuexception", "::rt::t_tfpuexception"},
      {"tfpuexceptionmask", "::rt::t_tfpuexceptionmask"},
      {"tsearchrec", "::rt::t_tsearchrec"},
      {"tsystemtime", "::rt::t_tsystemtime"},
      {"tmethod", "::rt::t_tmethod"},
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
      {"tobject", {"::rt::t_tobject", "", "__rt__"}},
      {"exception", {"::rt::t_exception", "tobject", "sysutils"}},
      {"eexternal", {"::p_sysutils::t_eexternal", "exception", "sysutils"}},
      {"einterror", {"::p_sysutils::t_einterror", "eexternal", "sysutils"}},
      {"eintoverflow",
       {"::p_sysutils::t_eintoverflow", "einterror", "sysutils"}},
      {"eoserror", {"::p_sysutils::t_eoserror", "exception", "sysutils"}},
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
                               : ("::" + mangle(unit_name) + "::");
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

const PrimitiveInfo* shift_carrier_primitive(const PrimitiveInfo* info) {
  if (!info || info->int_kind == PrimitiveIntKind::None) return nullptr;
  if (info->bits >= 64) return info;
  return (info->int_kind == PrimitiveIntKind::Unsigned)
             ? primitive_info("cardinal")
             : primitive_info("longint");
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

namespace {

bool checked_pascal_shift_int64_impl(int64_t a, const PrimitiveInfo* carrier,
                                     int64_t shift, bool shift_left,
                                     int64_t* out) {
  carrier = shift_carrier_primitive(carrier);
  if (!carrier || carrier->int_kind == PrimitiveIntKind::None ||
      carrier->bits == 0 || carrier->bits > 64) {
    return false;
  }

  const uint8_t bits = carrier->bits;
  const uint64_t amount =
      static_cast<uint64_t>(shift) & static_cast<uint64_t>(bits - 1);
  uint64_t value_bits = low_bits(static_cast<uint64_t>(a), bits);
  uint64_t shifted = 0;
  if (shift_left) {
    shifted = low_bits(value_bits << static_cast<unsigned>(amount), bits);
  } else {
    shifted = value_bits >> static_cast<unsigned>(amount);
  }

  if (carrier->int_kind == PrimitiveIntKind::Unsigned || bits == 64) {
    *out = static_cast<int64_t>(shifted);
    return true;
  }

  const uint64_t sign_bit = uint64_t{1} << (bits - 1);
  if ((shifted & sign_bit) == 0) {
    *out = static_cast<int64_t>(shifted);
  } else {
    *out = static_cast<int64_t>(shifted | ~low_bits(UINT64_MAX, bits));
  }
  return true;
}

}  // namespace

bool checked_pascal_shl_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out) {
  return checked_pascal_shift_int64_impl(a, carrier, shift, true, out);
}

bool checked_pascal_shr_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out) {
  return checked_pascal_shift_int64_impl(a, carrier, shift, false, out);
}

bool tyname_is(const ast::TypeExpr* t, std::string_view expected) {
  return t && t->kind == ast::Kind::TyName &&
         ascii_lower(static_cast<const ast::TyName&>(*t).name) == expected;
}

}  // namespace tp2cc
