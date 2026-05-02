#include "emit_types.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>

#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitTypes::EmitTypes(const TypeRegistry* registry, ScopeStateView& scope,
                     EmitAnalysis& analysis,
                     EmitTypeConstRender& const_render,
                     EmitTypeDiagOps& diag_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      const_render_(const_render),
      diag_ops_(diag_ops) {}

std::string EmitTypes::type_name_to_cxx(const TyName& n) {
  if (is_primitive_type(n.name)) return primitive_type_cxx(n.name);
  if (n.name == "nil") return "std::nullptr_t";
  if (n.name == "ansistring" || n.name == "utf8string") {
    return "::rt::tp2cc_AnsiString";
  }
  if (n.name == "text") return "::rt::tp2cc_TextFile";
  if (n.name == "string" || n.name == "shortstring") {
    return "::rt::tp2cc_ShortString<>";
  }
  // Runtime aliases are also registered for analysis, but their emitted names
  // are fixed by the runtime. Only non-runtime translated declarations use
  // generated `t_*` type spelling.
  if (registry_knows_translated_type(n.name)) {
    auto lookup_name = ascii_lower(n.name);
    if (registry_) {
      auto cit = registry_->classes.find(std::string(lookup_name));
      if (cit != registry_->classes.end() && cit->second.is_reference_type) {
        return named_type_struct_cxx(n.name) + "*";
      }
      if (registry_->interfaces.count(std::string(lookup_name))) {
        return named_type_struct_cxx(n.name) + "*";
      }
    }
    return named_type_struct_cxx(n.name);
  }
  if (std::string cls = builtin_reference_class_struct_cxx(n.name);
      !cls.empty()) {
    return cls + "*";
  }
  if (std::string rt = runtime_named_type_cxx(n.name); !rt.empty()) {
    return rt;
  }
  return named_type_struct_cxx(n.name);
}

std::string EmitTypes::type_name_text_to_cxx(std::string_view name) {
  TyName t;
  t.name = std::string(name);
  return type_name_to_cxx(t);
}

std::string EmitTypes::named_type_struct_cxx(std::string_view name) {
  // Same registry-first / rt-fallback ordering as type_name_to_cxx.
  if (!registry_knows_translated_type(name)) {
    if (std::string rt = runtime_named_type_cxx(name); !rt.empty()) {
      return rt;
    }
  }
  if (std::string cls = builtin_reference_class_struct_cxx(name);
      !cls.empty()) {
    return cls;
  }
  auto dot = name.find('.');
  if (dot != std::string_view::npos) {
    return unit_namespace_prefix(name.substr(0, dot)) +
           type_mangle(name.substr(dot + 1));
  }
  return visible_type_prefix(name) + type_mangle(name);
}

std::string EmitTypes::visible_type_prefix(std::string_view name) {
  if (!registry_) return {};
  std::string lower = ascii_lower(std::string(name));
  if (scope_.local_type_aliases_scoped.count(lower) ||
      scope_.local_enums.count(lower)) {
    return {};
  }
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur != registry_->units.end()) {
    if (cur->second.has_type(lower)) return {};
    for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
         ++it) {
      if (*it == "__rt__") continue;
      auto uit = registry_->units.find(*it);
      if (uit == registry_->units.end()) continue;
      if (uit->second.has_export_type(lower)) return unit_namespace_prefix(*it);
    }
  }
  auto class_it = registry_->classes.find(lower);
  if (class_it != registry_->classes.end() &&
      class_it->second.defining_unit != scope_.current_unit_name) {
    return unit_namespace_prefix(class_it->second.defining_unit);
  }
  auto interface_it = registry_->interfaces.find(lower);
  if (interface_it != registry_->interfaces.end() &&
      interface_it->second.defining_unit != scope_.current_unit_name) {
    return unit_namespace_prefix(interface_it->second.defining_unit);
  }
  auto record_it = registry_->records.find(lower);
  if (record_it != registry_->records.end() &&
      record_it->second.defining_unit != scope_.current_unit_name) {
    return unit_namespace_prefix(record_it->second.defining_unit);
  }
  auto enum_it = registry_->enums.find(lower);
  if (enum_it != registry_->enums.end() &&
      enum_it->second.defining_unit != scope_.current_unit_name) {
    return unit_namespace_prefix(enum_it->second.defining_unit);
  }
  auto alias_it = registry_->aliases.find(lower);
  if (alias_it != registry_->aliases.end() &&
      alias_it->second.defining_unit != scope_.current_unit_name) {
    return unit_namespace_prefix(alias_it->second.defining_unit);
  }
  return {};
}

bool EmitTypes::registry_knows_translated_type(std::string_view name) {
  if (!registry_) return false;
  std::string lower = ascii_lower(std::string(name));
  if (scope_.local_type_aliases_scoped.count(lower) ||
      scope_.local_enums.count(lower)) {
    return true;
  }
  if (auto cur = registry_->units.find(scope_.current_unit_name);
      cur != registry_->units.end()) {
    if (cur->second.has_type(lower)) return true;
    for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
         ++it) {
      if (*it == "__rt__") continue;
      auto uit = registry_->units.find(*it);
      if (uit == registry_->units.end()) continue;
      if (uit->second.has_export_type(lower)) return true;
    }
  }
  if (auto it = registry_->classes.find(lower);
      it != registry_->classes.end() && it->second.defining_unit != "__rt__") {
    return true;
  }
  if (auto it = registry_->records.find(lower);
      it != registry_->records.end() && it->second.defining_unit != "__rt__") {
    return true;
  }
  if (auto it = registry_->interfaces.find(lower);
      it != registry_->interfaces.end() && it->second.defining_unit != "__rt__") {
    return true;
  }
  if (auto it = registry_->enums.find(lower);
      it != registry_->enums.end() && it->second.defining_unit != "__rt__") {
    return true;
  }
  if (auto it = registry_->aliases.find(lower);
      it != registry_->aliases.end() && it->second.defining_unit != "__rt__") {
    return true;
  }
  return false;
}

std::string EmitTypes::metaclass_struct_cxx(std::string_view class_name) {
  auto dot = class_name.find('.');
  std::string tail =
      (dot == std::string_view::npos) ? std::string(class_name)
                                      : std::string(class_name.substr(dot + 1));
  std::string prefix;
  if (const auto* ci = analysis_.class_info_for_type_name(class_name);
      ci && !ci->defining_unit.empty() &&
      ci->defining_unit != scope_.current_unit_name) {
    prefix = unit_namespace_prefix(ci->defining_unit);
  }
  return prefix + "tp2cc_metaclass_" + type_mangle(tail);
}

std::string EmitTypes::metaclass_value_fn_cxx(std::string_view class_name) {
  auto dot = class_name.find('.');
  std::string tail =
      (dot == std::string_view::npos) ? std::string(class_name)
                                      : std::string(class_name.substr(dot + 1));
  std::string prefix;
  if (const auto* ci = analysis_.class_info_for_type_name(class_name);
      ci && !ci->defining_unit.empty() &&
      ci->defining_unit != scope_.current_unit_name) {
    prefix = unit_namespace_prefix(ci->defining_unit);
  }
  return prefix + "tp2cc_metaclass_value_" + type_mangle(tail);
}

bool EmitTypes::enum_has_explicit_values(const TyEnum& e) {
  for (const auto& member : e.members) {
    if (member.value) return true;
  }
  return false;
}

std::optional<int64_t> EmitTypes::enum_member_value_int64(const TyEnum& e,
                                                          size_t index) {
  int64_t value = 0;
  for (size_t i = 0; i <= index; ++i) {
    if (e.members[i].value) {
      auto info = analysis_.eval_const_int_expr(*e.members[i].value);
      if (!info) return std::nullopt;
      value = info->value;
    } else if (i != 0) {
      if (value == INT64_MAX) return std::nullopt;
      ++value;
    }
  }
  return value;
}

std::string EmitTypes::enum_member_value_to_cxx(const TyEnum& e, size_t index) {
  // Pascal/FPC enum ordinals are assigned left-to-right. Each explicit value
  // resets the running ordinal for later implicit members.
  std::string value = "0";
  for (size_t i = 0; i <= index; ++i) {
    if (e.members[i].value) {
      value = const_render_.const_value_to_cxx(*e.members[i].value);
    } else if (i != 0) {
      value = "((" + value + ") + 1)";
    }
  }
  return value;
}

std::string EmitTypes::enum_underlying_type_to_cxx(const TyEnum& e) {
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

  // Match old FPC's enum storage rule:
  // - default / `{$PACKENUM 4}` stores as 4 bytes
  // - `{$PACKENUM 2}` stores as 2 bytes when the signed/unsigned ordinal range
  //   fits, otherwise 4
  // - `{$PACKENUM 1}` stores as 1/2/4 depending on the range
  // Very large explicit ordinals still widen to 8 bytes so the emitted C++
  // stays representable.
  int width = 1;
  if (lo < std::numeric_limits<int32_t>::min() ||
      hi > std::numeric_limits<uint32_t>::max()) {
    width = 8;
  } else if (e.packenum >= 4 || lo < std::numeric_limits<int16_t>::min() ||
             hi > std::numeric_limits<uint16_t>::max()) {
    width = 4;
  } else if (e.packenum >= 2 || lo < std::numeric_limits<int8_t>::min() ||
             hi > std::numeric_limits<uint8_t>::max()) {
    width = 2;
  }

  if (lo < 0) {
    switch (width) {
      case 1:
        return "int8_t";
      case 2:
        return "int16_t";
      case 4:
        return "int32_t";
      default:
        return "int64_t";
    }
  }
  switch (width) {
    case 1:
      return "uint8_t";
    case 2:
      return "uint16_t";
    case 4:
      return "uint32_t";
    default:
      return "uint64_t";
  }
}

bool EmitTypes::array_dim_bounds_to_cxx(const TypeExpr& dim_in,
                                        std::string* lo,
                                        std::string* size_expr) {
  auto expr_is_char = [&](const Expr& e) -> bool {
    const TypeExpr* t = analysis_.deduce_type(e);
    if (t) t = analysis_.canonicalize_type(t);
    return tyname_is(t, "char");
  };
  auto ordinal_bound = [&](const Expr& e) -> std::string {
    std::string text = const_render_.const_value_to_cxx(e);
    return expr_is_char(e) ? "::rt::p_ord(" + text + ")" : text;
  };
  auto ordinal_text = [&](std::string text) -> std::string {
    return "::rt::tp2cc_ordinal_value(" + text + ")";
  };
  const TypeExpr* dim = analysis_.canonicalize_type(&dim_in);
  if (!dim) return false;
  if (dim->kind == Kind::TyDistinct) {
    // Distinct ordinal wrappers keep assignment-compatibility differences, but
    // their index range is still the underlying ordinal range.
    return array_dim_bounds_to_cxx(
        *static_cast<const TyDistinct&>(*dim).underlying, lo, size_expr);
  }
  *lo = "0";
  size_expr->clear();
  if (dim->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*dim);
    *lo = const_render_.const_value_to_cxx(*sr.lo);
    *size_expr = "((" + ordinal_bound(*sr.hi) + ") - (" +
                 ordinal_bound(*sr.lo) + ") + 1)";
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
  auto leit = scope_.local_enums.find(tn.name);
  if (leit != scope_.local_enums.end()) {
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
  if (registry_) {
    auto eit = registry_->enums.find(tn.name);
    if (eit != registry_->enums.end()) {
      if (!eit->second.members.empty()) {
        std::string prefix;
        if (eit->second.defining_unit != scope_.current_unit_name) {
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
    *lo = "false";
    *size_expr = "2";
    return true;
  }
  if (std::string lowname = ascii_lower(tn.name); is_primitive_type(lowname)) {
    // Match the old i386 FPC parser's fixed-array index whitelist:
    //   uchar/u8/u16/s8/s16/s32/bool*/widechar
    // and reject dword/cardinal/qword/int64 rather than silently decaying to
    // pointer semantics or inventing a wider-than-Pascal domain.
    if (lowname == "char" || lowname == "byte") {
      *size_expr = "256";
      return true;
    }
    if (lowname == "shortint") {
      *lo = "::std::numeric_limits<int8_t>::min()";
      *size_expr = "256";
      return true;
    }
    if (lowname == "word") {
      *size_expr = "65536";
      return true;
    }
    if (lowname == "smallint") {
      *lo = "::std::numeric_limits<int16_t>::min()";
      *size_expr = "65536";
      return true;
    }
    if (lowname == "longint" || lowname == "integer") {
      *lo = "::std::numeric_limits<int32_t>::min()";
      *size_expr =
          "((" + ordinal_text("::std::numeric_limits<int32_t>::max()") +
          ") - (" +
          ordinal_text("::std::numeric_limits<int32_t>::min()") +
          ") + 1)";
      return true;
    }
    if (lowname == "widechar") {
      *size_expr = "65536";
      return true;
    }
    if (lowname == "wordbool" || lowname == "longbool" ||
        lowname == "qwordbool") {
      *lo = "false";
      *size_expr = "2";
      return true;
    }
  }
  return false;
}

std::string EmitTypes::subrange_type_to_cxx(const TySubrange& r) {
  auto bound_type = [&](const Expr* e) -> const TypeExpr* {
    if (!e) return nullptr;
    const TypeExpr* t = analysis_.deduce_type(*e);
    return t ? analysis_.canonicalize_type(t) : nullptr;
  };
  auto is_char_literal = [&](const Expr* e) -> bool {
    return e && e->kind == Kind::StringLit &&
           static_cast<const StringLit&>(*e).value.size() == 1;
  };
  auto visible_enum_type_for_member = [&](std::string_view name) -> std::string {
    const std::string member = ascii_lower(name);
    for (const auto& [enum_name, en] : scope_.local_enums) {
      for (const auto& em : en->members) {
        if (ascii_lower(em.name) == member) return type_mangle(enum_name);
      }
    }
    if (!registry_) return {};
    const auto* info = analysis_.find_visible_enum_info_for_member(member);
    if (!info) return {};
    std::string prefix;
    if (!info->defining_unit.empty() &&
        info->defining_unit != scope_.current_unit_name) {
      prefix = unit_namespace_prefix(info->defining_unit);
    }
    return prefix + type_mangle(info->name);
  };

  auto enum_type_for_type_name = [&](std::string_view name) -> std::string {
    const std::string low = ascii_lower(name);
    if (scope_.local_enums.count(low)) return type_name_text_to_cxx(low);
    if (!registry_) return {};
    auto dot = low.find('.');
    if (dot != std::string::npos) {
      const std::string unit = low.substr(0, dot);
      const std::string tail = low.substr(dot + 1);
      auto eit = registry_->enums.find(tail);
      if (eit != registry_->enums.end() && eit->second.defining_unit == unit) {
        return type_name_text_to_cxx(low);
      }
      return {};
    }
    auto eit = registry_->enums.find(low);
    if (eit == registry_->enums.end()) return {};
    if (!eit->second.defining_unit.empty() &&
        eit->second.defining_unit != scope_.current_unit_name) {
      auto cur = registry_->units.find(scope_.current_unit_name);
      if (cur == registry_->units.end()) return {};
      bool visible = false;
      for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
        if (*it == eit->second.defining_unit) {
          visible = true;
          break;
        }
      }
      if (!visible) return {};
    }
    return type_name_text_to_cxx(low);
  };

  // If both bounds denote values from the same enum, preserve that enum carrier
  // so scalar subranges remain assignment-compatible with the parent enum.
  std::function<std::string(const Expr*)> bound_enum;
  bound_enum = [&](const Expr* e) -> std::string {
    if (!e) return {};
    if (e->kind == Kind::Ident) {
      return visible_enum_type_for_member(static_cast<const Ident&>(*e).name);
    }
    if (e->kind == Kind::Member) {
      const auto& mem = static_cast<const Member&>(*e);
      if (mem.base && mem.base->kind == Kind::Ident) {
        const std::string unit =
            ascii_lower(static_cast<const Ident&>(*mem.base).name);
        if (registry_) {
          const std::string member = ascii_lower(mem.name);
          for (const auto& [_, info] : registry_->enums) {
            if (info.defining_unit != unit) continue;
            if (std::find(info.members.begin(), info.members.end(), member) !=
                info.members.end()) {
              return unit_namespace_prefix(unit) + type_mangle(info.name);
            }
          }
        }
      }
      return {};
    }
    if (e->kind == Kind::Call) {
      const auto& call = static_cast<const Call&>(*e);
      if (!call.callee || call.callee->kind != Kind::Ident ||
          call.args.size() != 1 || !call.args[0]) {
        return {};
      }
      const std::string callee =
          ascii_lower(static_cast<const Ident&>(*call.callee).name);
      if (callee == "low" || callee == "high") {
        if (call.args[0]->kind == Kind::Ident) {
          return enum_type_for_type_name(
              static_cast<const Ident&>(*call.args[0]).name);
        }
        if (call.args[0]->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*call.args[0]);
          if (mem.base && mem.base->kind == Kind::Ident) {
            return enum_type_for_type_name(
                static_cast<const Ident&>(*mem.base).name + "." + mem.name);
          }
        }
        return {};
      }
      if (callee == "pred" || callee == "succ") return bound_enum(call.args[0].get());
    }
    return {};
  };
  std::string le = bound_enum(r.lo.get());
  std::string he = bound_enum(r.hi.get());
  if (!le.empty() && le == he) return le;

  const TypeExpr* lo_type = bound_type(r.lo.get());
  const TypeExpr* hi_type = bound_type(r.hi.get());
  if ((tyname_is(lo_type, "char") && tyname_is(hi_type, "char")) ||
      (is_char_literal(r.lo.get()) && is_char_literal(r.hi.get()))) {
    return primitive_type_cxx("char");
  }
  if (tyname_is(lo_type, "widechar") && tyname_is(hi_type, "widechar")) {
    return primitive_type_cxx("widechar");
  }

  auto lo_value = analysis_.eval_const_int_expr(*r.lo);
  auto hi_value = analysis_.eval_const_int_expr(*r.hi);
  if (!lo_value || !hi_value) return "int32_t";

  int64_t lo = lo_value->value;
  int64_t hi = hi_value->value;
  if (lo > hi) std::swap(lo, hi);

  // Match old FPC's ordinal subrange storage rule:
  // - 0..255 -> byte
  // - -128..127 -> shortint
  // - 0..65535 -> word
  // - -32768..32767 -> smallint
  // - 0..4294967295 -> longword/cardinal
  // - otherwise -> longint/int32_t
  //
  // We still widen beyond 32 bits if the explicit bounds exceed the classic
  // longint/cardinal domain so the emitted C++ can represent the source range.
  if (lo >= 0) {
    uint64_t uhi = static_cast<uint64_t>(hi);
    if (uhi <= UINT8_MAX) return primitive_type_cxx("byte");
    if (uhi <= UINT16_MAX) return primitive_type_cxx("word");
    if (uhi <= UINT32_MAX) return primitive_type_cxx("cardinal");
    return primitive_type_cxx("qword");
  }
  if (lo >= INT8_MIN && hi <= INT8_MAX) return primitive_type_cxx("shortint");
  if (lo >= INT16_MIN && hi <= INT16_MAX) {
    return primitive_type_cxx("smallint");
  }
  if (lo >= INT32_MIN && hi <= INT32_MAX) return primitive_type_cxx("longint");
  return primitive_type_cxx("int64");
}

std::string EmitTypes::string_type_to_cxx(const TyString& s) {
  if (s.max_length) {
    if (auto value = analysis_.eval_const_int_expr(*s.max_length)) {
      return "::rt::tp2cc_ShortString<" + std::to_string(value->value) + ">";
    }
    return "::rt::tp2cc_ShortString<" +
           const_render_.const_value_to_cxx(*s.max_length) + ">";
  }
  return "::rt::tp2cc_ShortString<>";
}

std::optional<std::string> EmitTypes::shortstring_capacity_to_cxx(
    const TypeExpr* t) {
  const TypeExpr* canon = analysis_.canonicalize_type(t);
  if (!(canon && canon->kind == Kind::TyString)) return std::nullopt;
  const auto& s = static_cast<const TyString&>(*canon);
  if (s.max_length) {
    if (auto value = analysis_.eval_const_int_expr(*s.max_length)) {
      return std::to_string(value->value);
    }
    return const_render_.const_value_to_cxx(*s.max_length);
  }
  return std::string("255");
}

bool EmitTypes::param_uses_shortstring_ref(const TypeExpr* t,
                                           Param::Mode mode) {
  return (mode == Param::Var || mode == Param::Out) &&
         shortstring_capacity_to_cxx(t).has_value();
}

std::string EmitTypes::shortstring_ref_type_to_cxx(const TypeExpr* t) {
  std::string cap = shortstring_capacity_to_cxx(t).value_or("255");
  if (cap == "255") return "::rt::tp2cc_ShortStringPtrRef<>";
  return "::rt::tp2cc_ShortStringPtrRef<" + cap + ">";
}

std::string EmitTypes::pointer_type_to_cxx(const TyPointer& p) {
  if (!p.target) return "void*";
  return type_to_cxx(*p.target) + "*";
}

std::string EmitTypes::set_type_to_cxx(const TySet& s) {
  return "::rt::tp2cc_Set<" + type_to_cxx(*s.element) + ">";
}

std::string EmitTypes::enum_type_to_cxx(const TyEnum& e,
                                        const std::string&) {
  // Inline anonymous enums are emitted as inline anonymous C++ enums so the
  // members stay usable in the enclosing scope.
  if (e.members.empty()) return "int32_t";
  std::string out = "enum : ";
  out += enum_underlying_type_to_cxx(e);
  out += " { ";
  for (size_t i = 0; i < e.members.size(); ++i) {
    if (i) out += ", ";
    out += mangle(e.members[i].name);
    if (e.members[i].value) {
      out += " = " + const_render_.const_value_to_cxx(*e.members[i].value);
    }
  }
  out += " }";
  return out;
}

std::string EmitTypes::open_array_type_to_cxx(const TypeExpr& t) {
  const TypeExpr* canon = analysis_.canonicalize_type(&t);
  const auto& a = static_cast<const TyArray&>(*canon);
  return "::rt::tp2cc_OpenArray<" +
         (a.element ? type_to_cxx(*a.element) : std::string("int32_t")) + ">";
}

std::string EmitTypes::array_type_to_cxx(const TyArray& a) {
  if (a.array_kind == ArrayKind::Open) {
    return open_array_type_to_cxx(a);
  }
  if (a.array_kind == ArrayKind::Dynamic) {
    return "::rt::tp2cc_DynArray<" + type_to_cxx(*a.element) + ">";
  }
  // Fixed arrays preserve Pascal bounds at the type level by nesting
  // `tp2cc_Array<T, Lo, N>` wrappers from innermost to outermost.
  std::string ty = type_to_cxx(*a.element);
  for (auto it = a.dims.rbegin(); it != a.dims.rend(); ++it) {
    std::string lo, size_expr;
    if (!array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
      // Keep unsupported fixed arrays as array wrappers so later emit paths do not
      // silently change aliasing/value semantics to pointer semantics.
      diag_ops_.report_error(
          a.loc,
          "unsupported fixed array index type; exact Pascal bounds are required");
      return "::rt::tp2cc_Array<" + ty + ", 0, 1>";
    }
    ty = "::rt::tp2cc_Array<" + ty + ", " + lo + ", " + size_expr + ">";
  }
  return ty;
}

std::string EmitTypes::procedural_param_types_to_cxx(
    const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& pp : params) {
    std::string pt;
    if (!pp.type) {
      pt = "void*";
    } else if (const TypeExpr* canon = analysis_.canonicalize_type(pp.type.get());
               canon && canon->kind == Kind::TyArray &&
               static_cast<const TyArray&>(*canon).array_kind ==
                   ArrayKind::Open) {
      pt = open_array_type_to_cxx(*pp.type);
    } else if (param_uses_shortstring_ref(pp.type.get(), pp.mode)) {
      pt = shortstring_ref_type_to_cxx(pp.type.get());
    } else {
      pt = type_to_cxx(*pp.type);
    }
    if (pp.type) {
      if (param_uses_shortstring_ref(pp.type.get(), pp.mode)) {
        // Mutable shortstrings are storage proxies, not C++ references; this
        // keeps virtual/interface method signatures capacity-agnostic.
      } else if (pp.mode == Param::Var || pp.mode == Param::Out) pt += "&";
      else if (pp.mode == Param::Const) {
        if (analysis_.const_param_needs_mutable_ref(pp.type.get())) pt += "&";
        else if (analysis_.const_param_needs_const_ref(pp.type.get()))
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

std::string EmitTypes::method_pointer_helper_name(const ProcDecl& pd) {
  // Overloaded methods need distinct helper thunks, but the helper itself must
  // still be an ordinary C++ identifier with no reserved spelling.
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

std::string EmitTypes::procedural_type_to_cxx(const TyProcedural& p) {
  std::string ret =
      p.is_function ? type_to_cxx(*p.return_type) : std::string("void");
  std::string params = procedural_param_types_to_cxx(p.params);
  if (p.is_method) {
    return "::rt::tp2cc_MethodPtr<" + ret + "(" + params + ")>";
  }
  return ret + " (*)(" + params + ")";
}

std::string EmitTypes::inline_record_type_to_cxx(const TyRecord& tr) {
  std::string out = "struct ";
  if (tr.is_packed) out += "[[gnu::packed]] ";
  out += "{ ";
  auto append_fields = [&](const std::vector<RecordField>& fields) {
    for (const auto& field : record_field_decls(fields)) {
      out += field.decl + "; ";
    }
  };

  append_fields(tr.fields);
  if (tr.has_variant) {
    if (!tr.variant_tag_name.empty() && tr.variant_tag_type) {
      RecordField tag_field;
      tag_field.names.push_back(tr.variant_tag_name);
      tag_field.type = tr.variant_tag_type;
      append_fields({tag_field});
    }
    out += "union { ";
    for (const auto& vc : tr.variant_cases) {
      if (vc.fields.empty()) continue;
      // GNU C++ anonymous structs inside anonymous unions match Pascal's
      // anonymous variant fields: `r.v.f` works without naming a case arm.
      out += "struct ";
      if (tr.is_packed) out += "[[gnu::packed]] ";
      out += "{ ";
      append_fields(vc.fields);
      out += "}; ";
    }
    out += "}; ";
  }
  out += "}";
  return out;
}

std::vector<EmitRecordFieldDecl> EmitTypes::record_field_decls(
    const std::vector<RecordField>& fields) {
  std::vector<EmitRecordFieldDecl> out;
  for (const auto& f : fields) {
    std::string type_cxx =
        f.type ? type_to_cxx(*f.type) : std::string("int32_t");
    for (const auto& fn : f.names) {
      EmitRecordFieldDecl entry;
      entry.type = f.type.get();
      entry.type_cxx = type_cxx;
      entry.mangled_name =
          registry_ ? registry_->field_cxx_name(fn) : mangle(fn);
      entry.decl = named_type_to_cxx(f.type.get(), entry.mangled_name);
      out.push_back(std::move(entry));
    }
  }
  return out;
}

EmitPackedRecordLayout EmitTypes::compute_packed_record_layout(
    const TyRecord& tr) {
  EmitPackedRecordLayout out;
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
        // Variant-case fields share the same outer offset; the per-case base
        // is `(packed_size_expr + case_size_so_far)`.
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

std::string EmitTypes::named_type_to_cxx(const TypeExpr* t, std::string_view name,
                                         std::string_view name_prefix) {
  if (!t) {
    if (name.empty()) {
      return name_prefix.empty() ? std::string("int32_t")
                                 : std::string("int32_t ") +
                                       std::string(name_prefix);
    }
    return std::string("int32_t ") + std::string(name_prefix) +
           std::string(name);
  }

  // Direct procvar declarations need the identifier inside the `(*)`
  // declarator. `void (*hook)(int)` is valid C++; `void (*)(int) hook` is not.
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

std::string EmitTypes::type_to_cxx(const TypeExpr& t) {
  switch (t.kind) {
    case Kind::TyName:
      return type_name_to_cxx(static_cast<const TyName&>(t));
    case Kind::TyPointer:
      return pointer_type_to_cxx(static_cast<const TyPointer&>(t));
    case Kind::TySet:
      return set_type_to_cxx(static_cast<const TySet&>(t));
    case Kind::TyArray:
      return array_type_to_cxx(static_cast<const TyArray&>(t));
    case Kind::TySubrange:
      return subrange_type_to_cxx(static_cast<const TySubrange&>(t));
    case Kind::TyString:
      return string_type_to_cxx(static_cast<const TyString&>(t));
    case Kind::TyEnum:
      return enum_type_to_cxx(static_cast<const TyEnum&>(t), "");
    case Kind::TyDistinct:
      return type_to_cxx(*static_cast<const TyDistinct&>(t).underlying);
    case Kind::TyProcedural:
      return procedural_type_to_cxx(static_cast<const TyProcedural&>(t));
    case Kind::TyMetaclass:
      return metaclass_struct_cxx(static_cast<const TyMetaclass&>(t).class_name) +
             "*";
    case Kind::TyFile: {
      const auto& tf = static_cast<const TyFile&>(t);
      if (tf.is_text || !tf.element) return "::rt::tp2cc_TextFile";
      return "::rt::tp2cc_TypedFile<" + type_to_cxx(*tf.element) + ">";
    }
    case Kind::TyRecord: {
      // Inline anonymous records are emitted as inline C++ anonymous structs.
      return inline_record_type_to_cxx(static_cast<const TyRecord&>(t));
    }
    case Kind::TyObject:
      // Inline anonymous objects need base-class context the type-expression
      // position cannot currently carry.
      return "/* inline-object */ int32_t";
    case Kind::TyInterface:
      // Named interface declarations are emitted as pure virtual structs.
      return "/* inline-interface */ void*";
    default:
      return "/* unsupported-type */ int32_t";
  }
}

std::string EmitTypes::low_high_expr_for_named_type(std::string_view name,
                                                    bool want_low) {
  if (std::string primitive = primitive_low_high_expr(name, want_low);
      !primitive.empty()) {
    return primitive;
  }

  auto emit_enum_bound = [&](std::string_view enum_name,
                             std::string_view defining_unit) -> std::string {
    if (defining_unit.empty() || defining_unit == scope_.current_unit_name) {
      return enum_bound_name(enum_name, want_low ? "low" : "high");
    }
    return mangle(defining_unit) + "::" +
           enum_bound_name(enum_name, want_low ? "low" : "high");
  };

  auto dot = name.find('.');
  if (dot != std::string_view::npos) {
    std::string unit(name.substr(0, dot));
    std::string tail(name.substr(dot + 1));
    if (registry_) {
      auto eit = registry_->enums.find(ascii_lower(tail));
      if (eit != registry_->enums.end() && eit->second.defining_unit == unit) {
        return emit_enum_bound(tail, unit);
      }
      auto ait = registry_->aliases.find(ascii_lower(tail));
      if (ait != registry_->aliases.end() && ait->second.defining_unit == unit &&
          ait->second.target) {
        return low_high_expr_for_type(ait->second.target.get(), want_low);
      }
    }
    return {};
  }

  if (scope_.local_enums.count(std::string(name))) {
    return enum_bound_name(name, want_low ? "low" : "high");
  }
  if (registry_) {
    auto eit = registry_->enums.find(ascii_lower(std::string(name)));
    if (eit != registry_->enums.end()) {
      return emit_enum_bound(name, eit->second.defining_unit);
    }
  }

  auto lit = scope_.local_type_aliases_scoped.find(std::string(name));
  if (lit != scope_.local_type_aliases_scoped.end() && lit->second) {
    return low_high_expr_for_type(lit->second, want_low);
  }
  if (registry_) {
    auto ait = registry_->aliases.find(ascii_lower(std::string(name)));
    if (ait != registry_->aliases.end() && ait->second.target) {
      return low_high_expr_for_type(ait->second.target.get(), want_low);
    }
  }
  return {};
}

std::string EmitTypes::low_high_expr_for_type(const TypeExpr* t,
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
    return const_render_.const_value_to_cxx(want_low ? *r.lo : *r.hi);
  }
  if (t->kind == Kind::TyArray) {
    // `low(arr)` / `high(arr)` on an array type recurse into the first index
    // type. Open/dynamic arrays need a value to ask `p_length` of, so the
    // type-only path deliberately gives up there.
    const auto& arr = static_cast<const TyArray&>(*t);
    if (arr.array_kind != ArrayKind::Fixed) return {};
    if (arr.dims.empty()) return {};
    return low_high_expr_for_type(arr.dims[0].get(), want_low);
  }
  return {};
}

}  // namespace tp2cc
