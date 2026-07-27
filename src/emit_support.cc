#include "emit_support.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "ast.h"
#include "target_info.h"
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

std::string encode_helper_type(const TypeRegistry& registry,
                               const ast::TypeExpr& t) {
  if (const TypeDescriptor* descriptor = registry.descriptor_for_type(&t)) {
    if (const TypeSymbol* symbol = descriptor->symbol) {
      // The generated helper is already inside the declaring C++ unit/type
      // scope. Use the descriptor owner's local name: aliases share this owner
      // and therefore cannot create a second helper spelling.
      return "name_" + encode_helper_ident(symbol->name);
    }
  }
  switch (t.kind) {
    case ast::Kind::TyName:
      return "unbound";
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
      out += a.element ? encode_helper_type(registry, *a.element)
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
             encode_helper_type(
                 registry, *static_cast<const ast::TySet&>(t).element);
    case ast::Kind::TyFile: {
      const auto& f = static_cast<const ast::TyFile&>(t);
      if (f.is_text) return "text";
      return f.element ? "file_" + encode_helper_type(registry, *f.element)
                       : std::string("file_untyped");
    }
    case ast::Kind::TyPointer: {
      const auto& p = static_cast<const ast::TyPointer&>(t);
      return p.target ? "ptr_" + encode_helper_type(registry, *p.target)
                      : std::string("ptr_void");
    }
    case ast::Kind::TyProcedural: {
      const auto& p = static_cast<const ast::TyProcedural&>(t);
      std::string out = p.is_method ? "method" : "proc";
      out += p.is_function ? "_fn_" : "_proc_";
      out += encode_helper_params(registry, p.params);
      out += "_ret_";
      out += (p.is_function && p.return_type)
                 ? encode_helper_type(registry, *p.return_type)
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
      if (const TypeSymbol* target = registry.metaclass_target_for_type(&t)) {
        return "metaclass_" +
               encode_helper_ident(target->name);
      }
      return "metaclass_unbound";
    case ast::Kind::TyDistinct:
      return "distinct_" +
             encode_helper_type(
                 registry,
                 *static_cast<const ast::TyDistinct&>(t).underlying);
    default:
      return "type";
  }
}

std::string encode_helper_params(const TypeRegistry& registry,
                                 const std::vector<ast::Param>& params) {
  if (params.empty()) return "noargs";
  std::string out;
  for (const auto& param : params) {
    size_t repeats = param.names.empty() ? 1 : param.names.size();
    std::string type_code =
        param.type ? encode_helper_type(registry, *param.type)
                   : std::string("untyped");
    for (size_t i = 0; i < repeats; ++i) {
      if (!out.empty()) out += "_";
      out += encode_helper_param_mode(param.mode);
      out += "_";
      out += type_code;
    }
  }
  return out;
}

std::string binary_pascal_operator_token(ast::BinOp op) {
  switch (op) {
    case ast::BinOp::Add: return "+";
    case ast::BinOp::Sub: return "-";
    case ast::BinOp::Mul: return "*";
    case ast::BinOp::RealDiv: return "/";
    case ast::BinOp::IntDiv: return "div";
    case ast::BinOp::Mod: return "mod";
    case ast::BinOp::Shl: return "shl";
    case ast::BinOp::Shr: return "shr";
    case ast::BinOp::And: return "and";
    case ast::BinOp::Or: return "or";
    case ast::BinOp::Xor: return "xor";
    case ast::BinOp::Eq: return "=";
    case ast::BinOp::NotEq: return "<>";
    case ast::BinOp::Lt: return "<";
    case ast::BinOp::Gt: return ">";
    case ast::BinOp::LtEq: return "<=";
    case ast::BinOp::GtEq: return ">=";
    default: return {};
  }
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

std::string pascal_operator_named_helper_name(
    const TypeRegistry& registry, const ast::ProcDecl& pd) {
  std::string out = "tp2cc_operator_";
  out += encode_helper_ident(pd.operator_token);
  out += "_params_";
  out += encode_helper_params(registry, pd.params);
  out += "_ret_";
  out += pd.return_type ? encode_helper_type(registry, *pd.return_type)
                        : std::string("void");
  return out;
}

std::string pascal_assignment_operator_helper_name(
    const TypeRegistry& registry, const ast::ProcDecl& pd) {
  std::string out = "tp2cc_operator_assign";
  out += "_params_";
  out += encode_helper_params(registry, pd.params);
  out += "_ret_";
  out += pd.return_type ? encode_helper_type(registry, *pd.return_type)
                        : std::string("void");
  return out;
}

std::string pascal_operator_decl_name_to_cxx(
    const TypeRegistry& registry, const ast::ProcDecl& pd) {
  if (!pd.is_operator) return mangle(pd.name);
  if (pd.operator_token == ":=") {
    return pascal_assignment_operator_helper_name(registry, pd);
  }
  if (pascal_operator_decl_uses_named_helper(pd)) {
    return pascal_operator_named_helper_name(registry, pd);
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
      {"longint",      {.kind = PrimitiveKind::LongInt,     .cxx = "int32_t",              .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .bits = 32, .is_ordinal_integer = true}},
      {"longword",     {.kind = PrimitiveKind::LongWord,    .cxx = "uint32_t",             .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .bits = 32, .is_ordinal_integer = true}},
      {"smallint",     {.kind = PrimitiveKind::SmallInt,    .cxx = "int16_t",              .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .bits = 16, .is_ordinal_integer = true}},
      {"word",         {.kind = PrimitiveKind::Word,        .cxx = "uint16_t",             .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .bits = 16, .is_ordinal_integer = true}},
      {"shortint",     {.kind = PrimitiveKind::ShortInt,    .cxx = "int8_t",               .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .bits = 8, .is_ordinal_integer = true}},
      {"byte",         {.kind = PrimitiveKind::Byte,        .cxx = "uint8_t",              .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .bits = 8, .is_ordinal_integer = true}},
      {"char",         {.kind = PrimitiveKind::Char,        .cxx = "::rt::p_char",         .family = PrimitiveFamily::Char}},
      {"widechar",     {.kind = PrimitiveKind::WideChar,    .cxx = "uint16_t",             .family = PrimitiveFamily::WideChar, .int_kind = PrimitiveIntKind::Unsigned, .bits = 16}},
      {"boolean",      {.kind = PrimitiveKind::Boolean,     .cxx = "bool",                 .family = PrimitiveFamily::Bool}},
      {"bytebool",     {.kind = PrimitiveKind::ByteBool,    .cxx = "uint8_t",              .family = PrimitiveFamily::Bool, .int_kind = PrimitiveIntKind::Unsigned, .bits = 8}},
      {"wordbool",     {.kind = PrimitiveKind::WordBool,    .cxx = "uint16_t",             .family = PrimitiveFamily::Bool, .int_kind = PrimitiveIntKind::Unsigned, .bits = 16}},
      {"longbool",     {.kind = PrimitiveKind::LongBool,    .cxx = "uint32_t",             .family = PrimitiveFamily::Bool, .int_kind = PrimitiveIntKind::Unsigned, .bits = 32}},
      {"qwordbool",    {.kind = PrimitiveKind::QWordBool,   .cxx = "uint64_t",             .family = PrimitiveFamily::Bool, .int_kind = PrimitiveIntKind::Unsigned, .bits = 64}},
      {"single",       {.kind = PrimitiveKind::Single,      .cxx = "float",                .family = PrimitiveFamily::Real, .float_tier = PrimitiveFloatTier::Single}},
      {"double",       {.kind = PrimitiveKind::Double,      .cxx = "double",               .family = PrimitiveFamily::Real, .float_tier = PrimitiveFloatTier::Double}},
      {"real",         {.kind = PrimitiveKind::Real,        .cxx = "double",               .family = PrimitiveFamily::Real, .float_tier = PrimitiveFloatTier::Double}},
      {"extended",     {.kind = PrimitiveKind::Extended,    .cxx = "long double",          .family = PrimitiveFamily::Real, .float_tier = PrimitiveFloatTier::Extended}},
      {"comp",         {.kind = PrimitiveKind::Comp,        .cxx = "long double",          .family = PrimitiveFamily::Real, .float_tier = PrimitiveFloatTier::Extended}},
      {"pointer",      {.kind = PrimitiveKind::Pointer,     .cxx = "void*",                .family = PrimitiveFamily::Pointer}},
      {"pchar",        {.kind = PrimitiveKind::PChar,       .cxx = "::rt::p_char*",        .family = PrimitiveFamily::Pointer}},
      {"text",         {.kind = PrimitiveKind::Text,        .cxx = "::rt::tp2cc_TextFile", .family = PrimitiveFamily::Text}},
      {"int64",        {.kind = PrimitiveKind::Int64,       .cxx = "int64_t",              .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .bits = 64, .is_ordinal_integer = true}},
      {"qword",        {.kind = PrimitiveKind::QWord,       .cxx = "uint64_t",             .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .bits = 64, .is_ordinal_integer = true}},
      {"currency",     {.kind = PrimitiveKind::Currency,    .cxx = "::rt::t_currency",     .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .bits = 64}},
      {"ptrint",       {.kind = PrimitiveKind::PtrInt,      .cxx = "::rt::t_ptrint",       .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .pointer_sized = true}},
      {"ptruint",      {.kind = PrimitiveKind::PtrUInt,     .cxx = "::rt::t_ptruint",      .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .pointer_sized = true}},
      {"sizeint",      {.kind = PrimitiveKind::SizeInt,     .cxx = "::rt::t_sizeint",      .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Signed,   .pointer_sized = true}},
      {"sizeuint",     {.kind = PrimitiveKind::SizeUInt,    .cxx = "::rt::t_sizeuint",     .family = PrimitiveFamily::Integer, .int_kind = PrimitiveIntKind::Unsigned, .pointer_sized = true}},
      {"shortstring",  {.kind = PrimitiveKind::ShortString, .cxx = "::rt::tp2cc_ShortString<>", .family = PrimitiveFamily::String}},
      {"ansistring",   {.kind = PrimitiveKind::AnsiString,  .cxx = "::rt::tp2cc_AnsiString",    .family = PrimitiveFamily::String}},
      {"utf8string",   {.kind = PrimitiveKind::Utf8String,  .cxx = "::rt::tp2cc_AnsiString",    .family = PrimitiveFamily::String}},
  };
  return m;
}

const PrimitiveInfo* primitive_info(std::string_view lowname) {
  auto it = primitive_type_map().find(std::string(lowname));
  return it == primitive_type_map().end() ? nullptr : &it->second;
}

std::string unit_namespace_prefix(std::string_view unit_name) {
  return unit_name == "__rt__" ? std::string("::rt::")
                               : ("::" + mangle(unit_name) + "::");
}

uint64_t low_bits(uint64_t value, uint8_t bits) {
  // A resolved width must be in [1, 64].
  if (bits == 0 || bits > 64) {
    throw std::logic_error("low_bits: bit width " + std::to_string(bits) +
                           " out of range [1, 64]");
  }
  if (bits == 64) return value;
  return value & ((uint64_t{1} << bits) - 1);
}

uint8_t primitive_bits(const PrimitiveInfo& info, TargetInfo target) {
  if (info.int_kind == PrimitiveIntKind::None) {
    throw std::logic_error(
        "primitive_bits: non-integer primitive has no integer bit width");
  }
  if (info.pointer_sized) {
    if (target.pointer_bits != 32 && target.pointer_bits != 64) {
      throw std::logic_error(
          "primitive_bits: target pointer width must be 32 or 64, got " +
          std::to_string(target.pointer_bits));
    }
    return target.pointer_bits;
  }
  if (info.bits == 0 || info.bits > 64) {
    throw std::logic_error(
        "primitive_bits: fixed-width integer primitive has invalid width " +
        std::to_string(info.bits));
  }
  return info.bits;
}

uint64_t unsigned_mask_for_bits(uint8_t bits) {
  if (bits == 0 || bits > 64) {
    throw std::logic_error("unsigned_mask_for_bits: bit width " +
                           std::to_string(bits) + " out of range [1, 64]");
  }
  if (bits == 64) return UINT64_MAX;
  return (uint64_t{1} << bits) - 1;
}

int64_t signed_min_for_bits(uint8_t bits) {
  if (bits == 0 || bits > 64) {
    throw std::logic_error("signed_min_for_bits: bit width " +
                           std::to_string(bits) + " out of range [1, 64]");
  }
  if (bits == 64) return std::numeric_limits<int64_t>::min();
  return -(int64_t{1} << (bits - 1));
}

int64_t signed_max_for_bits(uint8_t bits) {
  if (bits == 0 || bits > 64) {
    throw std::logic_error("signed_max_for_bits: bit width " +
                           std::to_string(bits) + " out of range [1, 64]");
  }
  if (bits == 64) return std::numeric_limits<int64_t>::max();
  return (int64_t{1} << (bits - 1)) - 1;
}

std::string uint64_literal_text(uint64_t value) {
  char buf[32];
  const char* fmt =
      (value > static_cast<uint64_t>(INT64_MAX)) ? "%lluULL" : "%llu";
  std::snprintf(buf, sizeof(buf), fmt,
                static_cast<unsigned long long>(value));
  return buf;
}

std::string signed_bits_literal_text(uint64_t bits, uint8_t width,
                                     std::string_view cxx) {
  if (width == 0 || width > 64) {
    throw std::logic_error("signed_bits_literal_text: bit width " +
                           std::to_string(width) + " out of range [1, 64]");
  }
  uint64_t sign_bit = uint64_t{1} << (width - 1);
  if ((bits & sign_bit) == 0) return uint64_literal_text(bits);
  if (width == 64 && bits == sign_bit) {
    return "::std::numeric_limits<" + std::string(cxx) + ">::min()";
  }
  uint64_t magnitude =
      (width == 64) ? (uint64_t{0} - bits) : low_bits(~bits + 1, width);
  return "-" + uint64_literal_text(magnitude);
}

std::string primitive_low_high_expr(const PrimitiveInfo* info, bool want_low) {
  if (!info) return {};
  if (info->is_char()) {
    return want_low ? "::rt::tp2cc_char_of(0)" : "::rt::tp2cc_char_of(255)";
  }
  if (info->is_bool()) return want_low ? "false" : "true";
  if (info->int_kind == PrimitiveIntKind::None) return {};
  if (want_low) {
    if (info->int_kind == PrimitiveIntKind::Unsigned) return "0";
    return "::std::numeric_limits<" + std::string(info->cxx) + ">::min()";
  }
  return "::std::numeric_limits<" + std::string(info->cxx) + ">::max()";
}

const PrimitiveInfo* ordinal_integer_primitive(
    const TypeRegistry& registry, PrimitiveIntKind kind, uint8_t bits) {
  for (const PrimitiveInfo& candidate : registry.primitive_info_storage) {
    if (candidate.is_ordinal_integer && candidate.int_kind == kind &&
        candidate.bits == bits) {
      return &candidate;
    }
  }
  return nullptr;
}

const PrimitiveInfo* primitive_info_for_value(const TypeRegistry& registry,
                                              int64_t value) {
  if (value >= -128 && value <= 127) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Signed, 8);
  }
  if (value >= 0 && value <= 255) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Unsigned, 8);
  }
  if (value >= -32768 && value <= 32767) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Signed, 16);
  }
  if (value >= 0 && value <= 65535) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Unsigned, 16);
  }
  if (value >= INT32_MIN && value <= INT32_MAX) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Signed, 32);
  }
  if (value >= 0 &&
      static_cast<uint64_t>(value) <=
          static_cast<uint64_t>(UINT32_MAX)) {
    return ordinal_integer_primitive(
        registry, PrimitiveIntKind::Unsigned, 32);
  }
  return ordinal_integer_primitive(
      registry, PrimitiveIntKind::Signed, 64);
}

const PrimitiveInfo* shift_carrier_primitive(const TypeRegistry& registry,
                                             const PrimitiveInfo* info,
                                             TargetInfo target) {
  if (!info || info->int_kind == PrimitiveIntKind::None) return nullptr;
  if (primitive_bits(*info, target) >= 64) return info;
  return ordinal_integer_primitive(registry, info->int_kind, 32);
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

namespace {

bool checked_pascal_shift_int64_impl(int64_t a, const PrimitiveInfo* carrier,
                                     int64_t shift, bool shift_left,
                                     int64_t* out, TargetInfo target) {
  if (!carrier || carrier->int_kind == PrimitiveIntKind::None) {
    return false;
  }

  const uint8_t bits =
      std::max<uint8_t>(primitive_bits(*carrier, target), 32);
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
                              int64_t shift, int64_t* out,
                              TargetInfo target) {
  return checked_pascal_shift_int64_impl(a, carrier, shift, true, out, target);
}

bool checked_pascal_shr_int64(int64_t a, const PrimitiveInfo* carrier,
                              int64_t shift, int64_t* out,
                              TargetInfo target) {
  return checked_pascal_shift_int64_impl(a, carrier, shift, false, out, target);
}

}  // namespace tp2cc
