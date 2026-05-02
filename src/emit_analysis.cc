#include "emit_analysis.h"

#include <limits>
#include <stdexcept>

#include "diag.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitAnalysis::EmitAnalysis(const TypeRegistry* registry, ScopeStateView& scope,
                           ResolveNameProvider& resolve_name_provider)
    : registry_(registry),
      scope_(scope),
      resolve_name_provider_(resolve_name_provider) {}

const TypeExpr* EmitAnalysis::canonicalize_type(const TypeExpr* t) {
  int hops = 0;
  while (t && t->kind == Kind::TyName) {
    if (hops++ >= kMaxAliasChainHops) {
      throw std::runtime_error(
          "EmitAnalysis::canonicalize_type: alias chain exceeds "
          "kMaxAliasChainHops; cycle or registry corruption");
    }
    const auto& n = static_cast<const TyName&>(*t);
    auto lit = scope_.local_type_aliases_scoped.find(n.name);
    if (lit != scope_.local_type_aliases_scoped.end() && lit->second &&
        lit->second != t) {
      t = lit->second;
      continue;
    }
    if (registry_) {
      const TypeExpr* next = registry_->canonicalize(t);
      if (next && next != t) {
        t = next;
        continue;
      }
    }
    break;
  }
  return t;
}

const ClassInfo* EmitAnalysis::class_info_for_type_name(std::string_view name) {
  std::string low = ascii_lower(name);
  if (std::string builtin = builtin_reference_class_struct_cxx(low);
      !builtin.empty()) {
    static std::unordered_map<std::string, ClassInfo> builtins;
    auto [it, inserted] = builtins.try_emplace(low);
    if (inserted) {
      it->second.name = low;
      if (low == "exception") it->second.parent = "tobject";
      else if (low == "eexternal") it->second.parent = "exception";
      else if (low == "einterror") it->second.parent = "eexternal";
      else if (low == "eintoverflow") it->second.parent = "einterror";
      else if (low == "eoserror") it->second.parent = "exception";
      it->second.defining_unit =
          (low == "tobject") ? "__rt__" : "sysutils";
      it->second.is_reference_type = true;
    }
    return &it->second;
  }

  if (!registry_) return nullptr;
  auto dot = low.find('.');
  if (dot == std::string::npos) {
    auto it = registry_->classes.find(low);
    return it == registry_->classes.end() ? nullptr : &it->second;
  }

  // TypeRegistry indexes classes by unqualified Pascal name. Qualified
  // references like `unitname.tfoo` therefore need a second defining-unit
  // check here so `u1.tnode` and `u2.tnode` stay distinguishable.
  std::string unit = low.substr(0, dot);
  std::string tail = low.substr(dot + 1);
  auto it = registry_->classes.find(tail);
  if (it == registry_->classes.end()) return nullptr;
  return it->second.defining_unit == unit ? &it->second : nullptr;
}

const TypeExpr* EmitAnalysis::lookup_named_type_expr(std::string_view name) {
  std::string low = ascii_lower(name);

  auto lit = scope_.local_type_aliases_scoped.find(low);
  if (lit != scope_.local_type_aliases_scoped.end() && lit->second) {
    return lit->second;
  }

  if (!registry_) return nullptr;

  auto dot = low.find('.');
  if (dot != std::string::npos) {
    std::string unit = low.substr(0, dot);
    std::string tail = low.substr(dot + 1);

    auto ait = registry_->aliases.find(tail);
    if (ait != registry_->aliases.end() &&
        ait->second.defining_unit == unit &&
        ait->second.target) {
      return ait->second.target.get();
    }
    auto cit = registry_->classes.find(tail);
    if (cit != registry_->classes.end() && cit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    auto rit = registry_->records.find(tail);
    if (rit != registry_->records.end() && rit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    auto eit = registry_->enums.find(tail);
    if (eit != registry_->enums.end() && eit->second.defining_unit == unit) {
      return named_pascal_type(name);
    }
    return nullptr;
  }

  auto ait = registry_->aliases.find(low);
  if (ait != registry_->aliases.end() && ait->second.target) {
    return ait->second.target.get();
  }
  if (registry_->classes.count(low) || registry_->records.count(low) ||
      registry_->enums.count(low)) {
    return named_pascal_type(name);
  }
  return nullptr;
}

bool EmitAnalysis::is_builtin_reference_class_name(std::string_view name) const {
  return !builtin_reference_class_struct_cxx(ascii_lower(name)).empty();
}

std::string EmitAnalysis::metaclass_target_name(const TypeExpr* t) {
  t = canonicalize_type(t);
  if (!t || t->kind != Kind::TyMetaclass) return {};
  return static_cast<const TyMetaclass&>(*t).class_name;
}

bool EmitAnalysis::type_is_reference_class(const TypeExpr* t) {
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

bool EmitAnalysis::type_is_interface(const TypeExpr* t) {
  if (!registry_ || !t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyInterface) return true;
  if (t->kind != Kind::TyName) return false;
  const auto& n = static_cast<const TyName&>(*t);
  return registry_->interfaces.count(ascii_lower(n.name)) > 0;
}

bool EmitAnalysis::type_is_value_object(const TypeExpr* t) {
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

bool EmitAnalysis::const_param_needs_mutable_ref(const TypeExpr* t) {
  // Old object-style `object` values still treat `const` mostly as a
  // calling-convention hint. Their methods may mutate bookkeeping through the
  // hidden self slot, so lowering them to `const T&` would make valid Pascal
  // source ill-formed in C++.
  if (type_is_reference_class(t)) return false;
  return type_is_value_object(t);
}

bool EmitAnalysis::const_param_needs_const_ref(const TypeExpr* t) {
  // Pascal `const` is not a blanket request for C++ reference semantics.
  // Keep the default path as plain-by-value unless the source model proves we
  // need aliasing; otherwise reviewability suffers because "harmless" params
  // silently become storage-identity-preserving references.
  (void)t;
  return false;
}

const TypeExpr* EmitAnalysis::deduce_const_decl_type(const ConstDecl& cd) {
  if (cd.type) return cd.type.get();
  auto info = eval_const_int_expr(*cd.value);
  if (info && info->type) return builtin_integer_type(info->type);
  if (cd.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*cd.value));
  }
  return cd.value ? deduce_type(*cd.value) : nullptr;
}

const TypeExpr* EmitAnalysis::deduce_const_info_type(const ConstInfo& c) {
  if (c.type) return c.type.get();
  if (!c.value) return nullptr;
  auto info = eval_const_int_expr(*c.value);
  if (info && info->type) return builtin_integer_type(info->type);
  if (c.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*c.value));
  }
  return deduce_type(*c.value);
}

const TySet* EmitAnalysis::synthesize_set_type(
    const TypeExpr* element,
    std::optional<std::pair<int64_t, int64_t>> explicit_bounds) {
  if (!element) return nullptr;
  auto tp = std::make_shared<TySet>();
  // The synthesized set view borrows the existing element type; it is only an
  // emit-time compatibility/type-inference artifact and does not own or mutate
  // the underlying type node.
  tp->element = std::shared_ptr<TypeExpr>(const_cast<TypeExpr*>(element),
                                          [](TypeExpr*) {});
  if (explicit_bounds) {
    tp->has_explicit_bounds = true;
    tp->explicit_low = explicit_bounds->first;
    tp->explicit_high = explicit_bounds->second;
  }
  synthesized_types_.push_back(tp);
  return tp.get();
}

bool EmitAnalysis::same_type_ast(const TypeExpr* a, const TypeExpr* b) {
  a = canonicalize_type(a);
  b = canonicalize_type(b);
  if (!a || !b) return false;
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case Kind::TyName:
      return ascii_lower(static_cast<const TyName&>(*a).name) ==
             ascii_lower(static_cast<const TyName&>(*b).name);
    case Kind::TyDistinct:
      return same_type_ast(static_cast<const TyDistinct&>(*a).underlying.get(),
                             static_cast<const TyDistinct&>(*b).underlying.get());
    case Kind::TySubrange: {
      const auto& as = static_cast<const TySubrange&>(*a);
      const auto& bs = static_cast<const TySubrange&>(*b);
      int64_t alo = 0, ahi = 0, blo = 0, bhi = 0;
      OrdinalFamily af = OrdinalFamily::Invalid;
      OrdinalFamily bf = OrdinalFamily::Invalid;
      std::string akey;
      std::string bkey;
      return try_eval_ordinal_expr(*as.lo, &alo, &af, &akey) &&
             try_eval_ordinal_expr(*as.hi, &ahi, &af, &akey) &&
             try_eval_ordinal_expr(*bs.lo, &blo, &bf, &bkey) &&
             try_eval_ordinal_expr(*bs.hi, &bhi, &bf, &bkey) && af == bf &&
             akey == bkey && alo == blo && ahi == bhi;
    }
    case Kind::TyEnum:
      return false;
    default:
      return false;
  }
}

bool EmitAnalysis::try_eval_ordinal_expr(const Expr& e, int64_t* value,
                                         OrdinalFamily* family,
                                         std::string* enum_key) {
  if (!value || !family || !enum_key) return false;
  if (e.kind == Kind::StringLit) {
    const auto& sl = static_cast<const StringLit&>(e);
    if (sl.value.size() == 1) {
      *value = static_cast<unsigned char>(sl.value[0]);
      *family = OrdinalFamily::Char;
      enum_key->clear();
      return true;
    }
  }
  if (e.kind == Kind::Ident) {
    const std::string low = ascii_lower(static_cast<const Ident&>(e).name);
    if (low == "false" || low == "true") {
      *value = (low == "true") ? 1 : 0;
      *family = OrdinalFamily::Boolean;
      enum_key->clear();
      return true;
    }
    if (const auto* info = find_visible_enum_info_for_member(low)) {
      for (size_t i = 0; i < info->members.size(); ++i) {
        if (info->members[i] == low) {
          *value = static_cast<int64_t>(i);
          *family = OrdinalFamily::Enum;
          *enum_key = info->defining_unit.empty()
                          ? info->name
                          : info->defining_unit + "." + info->name;
          return true;
        }
      }
    }
  }
  if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (mem.base->kind == Kind::Ident && registry_) {
      const std::string unit =
          ascii_lower(static_cast<const Ident&>(*mem.base).name);
      const std::string member = ascii_lower(mem.name);
      for (const auto& [enum_name, info] : registry_->enums) {
        if (info.defining_unit != unit) continue;
        for (size_t i = 0; i < info.members.size(); ++i) {
          if (info.members[i] == member) {
            *value = static_cast<int64_t>(i);
            *family = OrdinalFamily::Enum;
            *enum_key = info.defining_unit.empty()
                            ? info.name
                            : info.defining_unit + "." + info.name;
            return true;
          }
        }
      }
    }
  }
  if (auto info = eval_const_int_expr(e)) {
    *value = info->value;
    *family = OrdinalFamily::Integer;
    enum_key->clear();
    return true;
  }
  return false;
}

std::optional<EmitAnalysis::OrdinalDomain> EmitAnalysis::ordinal_domain_for_type(
    const TypeExpr* t) {
  t = canonicalize_type(t);
  if (!t) return std::nullopt;
  if (t->kind == Kind::TyDistinct) {
    return ordinal_domain_for_type(
        static_cast<const TyDistinct&>(*t).underlying.get());
  }
  if (t->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*t);
    int64_t lo = 0;
    int64_t hi = 0;
    OrdinalFamily lo_family = OrdinalFamily::Invalid;
    OrdinalFamily hi_family = OrdinalFamily::Invalid;
    std::string lo_key;
    std::string hi_key;
    if (!try_eval_ordinal_expr(*sr.lo, &lo, &lo_family, &lo_key) ||
        !try_eval_ordinal_expr(*sr.hi, &hi, &hi_family, &hi_key) ||
        lo_family != hi_family || lo_key != hi_key) {
      return std::nullopt;
    }
    OrdinalDomain dom;
    dom.family = lo_family;
    dom.low = std::min(lo, hi);
    dom.high = std::max(lo, hi);
    dom.enum_key = lo_key;
    return dom;
  }
  if (t->kind == Kind::TyEnum) {
    const auto& en = static_cast<const TyEnum&>(*t);
    OrdinalDomain dom;
    dom.family = OrdinalFamily::Enum;
    dom.low = 0;
    dom.high =
        en.members.empty() ? 0 : static_cast<int64_t>(en.members.size() - 1);
    dom.enum_key = "enum@" +
                   std::to_string(reinterpret_cast<uintptr_t>(&en));
    return dom;
  }
  if (t->kind != Kind::TyName) return std::nullopt;

  const std::string low = ascii_lower(static_cast<const TyName&>(*t).name);
  if (low == "boolean") {
    return OrdinalDomain{OrdinalFamily::Boolean, 0, 1, {}};
  }
  if (low == "char") {
    return OrdinalDomain{OrdinalFamily::Char, 0, 255, {}};
  }
  if (low == "widechar") {
    return OrdinalDomain{OrdinalFamily::WideChar, 0, 65535, {}};
  }
  if (const auto* info = primitive_info(low);
      info && info->int_kind != PrimitiveIntKind::None) {
    OrdinalDomain dom;
    dom.family = OrdinalFamily::Integer;
    dom.low = 0;
    dom.high = 0;
    if (info->int_kind == PrimitiveIntKind::Unsigned) {
      dom.low = 0;
      if (info->bits >= 63) {
        dom.high = std::numeric_limits<int64_t>::max();
      } else {
        dom.high = (int64_t{1} << info->bits) - 1;
      }
    } else if (info->bits >= 64) {
      dom.low = std::numeric_limits<int64_t>::min();
      dom.high = std::numeric_limits<int64_t>::max();
    } else {
      dom.low = -(int64_t{1} << (info->bits - 1));
      dom.high = (int64_t{1} << (info->bits - 1)) - 1;
    }
    return dom;
  }
  if (!registry_) return std::nullopt;
  std::string unit;
  std::string tail = low;
  if (auto dot = low.find('.'); dot != std::string::npos) {
    unit = low.substr(0, dot);
    tail = low.substr(dot + 1);
  }
  auto eit = registry_->enums.find(tail);
  if (eit == registry_->enums.end()) return std::nullopt;
  if (!unit.empty() && eit->second.defining_unit != unit) return std::nullopt;
  OrdinalDomain dom;
  dom.family = OrdinalFamily::Enum;
  dom.low = 0;
  dom.high =
      eit->second.members.empty()
          ? 0
          : static_cast<int64_t>(eit->second.members.size() - 1);
  dom.enum_key = eit->second.defining_unit.empty()
                     ? eit->second.name
                     : eit->second.defining_unit + "." + eit->second.name;
  return dom;
}

std::optional<EmitAnalysis::OrdinalDomain>
EmitAnalysis::ordinal_domain_for_set_type(const TypeExpr* t) {
  t = canonicalize_type(t);
  if (!t || t->kind != Kind::TySet) return std::nullopt;
  const auto& s = static_cast<const TySet&>(*t);
  auto dom = ordinal_domain_for_type(s.element.get());
  if (!dom) return std::nullopt;
  if (s.has_explicit_bounds) {
    dom->low = s.explicit_low;
    dom->high = s.explicit_high;
  }
  return dom;
}

const TypeExpr* EmitAnalysis::deduce_set_literal_type(const SetLit& s,
                                                      const TypeExpr* target) {
  const TypeExpr* canon_target = canonicalize_type(target);
  if (canon_target && canon_target->kind == Kind::TySet) return canon_target;
  if (s.elements.empty()) return nullptr;

  OrdinalFamily family = OrdinalFamily::Invalid;
  std::string enum_key;
  const TypeExpr* enum_type = nullptr;
  int64_t low = 0;
  int64_t high = 0;
  bool have_bounds = false;

  auto absorb = [&](const Expr& e) -> bool {
    int64_t value = 0;
    OrdinalFamily expr_family = OrdinalFamily::Invalid;
    std::string expr_key;
    if (!try_eval_ordinal_expr(e, &value, &expr_family, &expr_key)) return false;
    if (family == OrdinalFamily::Invalid) {
      family = expr_family;
      enum_key = expr_key;
      if (expr_family == OrdinalFamily::Enum && !enum_type) {
        enum_type = deduce_type(e);
      }
    } else if (family != expr_family || enum_key != expr_key) {
      return false;
    }
    if (!have_bounds) {
      low = high = value;
      have_bounds = true;
    } else {
      low = std::min(low, value);
      high = std::max(high, value);
    }
    return true;
  };

  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      const auto& r = static_cast<const Range&>(*el);
      if (!absorb(*r.lo) || !absorb(*r.hi)) return nullptr;
    } else if (!absorb(*el)) {
      return nullptr;
    }
  }

  const TypeExpr* element_type = nullptr;
  switch (family) {
    case OrdinalFamily::Integer:
      // Untyped integer set literals should not inherit the accidental
      // narrow C++ type of their first constant/member. Keep the Pascal
      // type broad (`longint`) and preserve the actual range separately.
      element_type = builtin_integer_type("longint");
      break;
    case OrdinalFamily::Boolean:
      element_type = builtin_boolean_type();
      break;
    case OrdinalFamily::Char:
      element_type = builtin_char_type();
      break;
    case OrdinalFamily::WideChar:
      element_type = named_pascal_type("widechar");
      break;
    case OrdinalFamily::Enum:
      element_type = enum_type;
      break;
    case OrdinalFamily::Invalid:
      return nullptr;
  }
  if (!element_type) return nullptr;
  return synthesize_set_type(element_type, std::make_pair(low, high));
}

SetConversionKind EmitAnalysis::classify_set_conversion(
    const TypeExpr* source, const TypeExpr* target) {
  source = canonicalize_type(source);
  target = canonicalize_type(target);
  if (!(source && target && source->kind == Kind::TySet &&
        target->kind == Kind::TySet)) {
    return SetConversionKind::Incompatible;
  }

  auto src_dom = ordinal_domain_for_set_type(source);
  auto dst_dom = ordinal_domain_for_set_type(target);
  if (!src_dom || !dst_dom) return SetConversionKind::Incompatible;

  bool family_compatible = false;
  switch (dst_dom->family) {
    case OrdinalFamily::Integer:
      family_compatible = src_dom->family == OrdinalFamily::Integer;
      break;
    case OrdinalFamily::Boolean:
      family_compatible = src_dom->family == OrdinalFamily::Boolean;
      break;
    case OrdinalFamily::Char:
      family_compatible = src_dom->family == OrdinalFamily::Char;
      break;
    case OrdinalFamily::WideChar:
      family_compatible = src_dom->family == OrdinalFamily::WideChar;
      break;
    case OrdinalFamily::Enum:
      family_compatible = src_dom->family == OrdinalFamily::Enum &&
                          src_dom->enum_key == dst_dom->enum_key;
      break;
    case OrdinalFamily::Invalid:
      break;
  }
  if (!family_compatible) return SetConversionKind::Incompatible;
  if (src_dom->low < dst_dom->low || src_dom->high > dst_dom->high) {
    return SetConversionKind::Incompatible;
  }

  const auto& src_set = static_cast<const TySet&>(*source);
  const auto& dst_set = static_cast<const TySet&>(*target);
  const bool same_bounds =
      src_dom->low == dst_dom->low && src_dom->high == dst_dom->high;
  if (same_bounds && same_type_ast(src_set.element.get(), dst_set.element.get())) {
    return SetConversionKind::Exact;
  }
  return SetConversionKind::Compatible;
}

std::optional<ConvertedConstInt> EmitAnalysis::convert_const_int_value(
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

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_cast(
    const Call& c,
    std::unordered_set<std::string>* visiting_const_names) {
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

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_expr(
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
          if (!checked_pascal_shl_int64(lhs->value, lhs->type, rhs->value, &value)) {
            return std::nullopt;
          }
          return ConstIntExprInfo{value, shift_carrier_primitive(lhs->type)};
        case BinOp::Shr:
          if (!checked_pascal_shr_int64(lhs->value, lhs->type, rhs->value, &value)) {
            return std::nullopt;
          }
          return ConstIntExprInfo{value, shift_carrier_primitive(lhs->type)};
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
        // Untyped `const X = ...` is a compile-time constant. Typed
        // `const X: T = ...` is writable storage in this dialect and must not
        // be folded back through its initializer.
        if (cd.type || !cd.value) return std::nullopt;
        return eval_const_int_expr(*cd.value, visiting_const_names);
      };

      auto maybe_fold_const_info =
          [&](const ConstInfo& c) -> std::optional<ConstIntExprInfo> {
        if (c.type || !c.value) return std::nullopt;
        return eval_const_int_expr(*c.value, visiting_const_names);
      };

      auto lit = scope_.local_consts.find(id.name);
      if (lit != scope_.local_consts.end() && lit->second && lit->second->value) {
        auto out = maybe_fold_const_decl(*lit->second);
        pop();
        return out;
      }

      auto lookup_const = [&](const UnitInfo& u, bool export_only)
          -> const ConstInfo* {
        return export_only ? u.find_export_const(id.name) : u.find_const(id.name);
      };

      if (registry_) {
        auto cur = registry_->units.find(scope_.current_unit_name);
        if (cur != registry_->units.end()) {
          if (const auto* c = lookup_const(cur->second, false)) {
            auto out = maybe_fold_const_info(*c);
            pop();
            return out;
          }
          for (auto it = cur->second.uses.rbegin();
               it != cur->second.uses.rend(); ++it) {
            auto uit = registry_->units.find(*it);
            if (uit == registry_->units.end()) continue;
            if (const auto* c = lookup_const(uit->second, true)) {
              auto out = maybe_fold_const_info(*c);
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

const TypeExpr* EmitAnalysis::deduce_type(const Expr& e) {
  if (!registry_) return nullptr;
  auto find_unit_enum_type = [&](const std::string& unit_name,
                                 const std::string& member_name)
      -> const TypeExpr* {
    const std::string unit_low = ascii_lower(unit_name);
    const std::string member_low = ascii_lower(member_name);
    for (const auto& [enum_name, en] : registry_->enums) {
      if (en.defining_unit != unit_low) continue;
      for (const auto& member : en.members) {
        if (member != member_low) continue;
        if (unit_low == scope_.current_unit_name) {
          return named_pascal_type(enum_name);
        }
        return named_pascal_type(unit_low + "." + enum_name);
      }
    }
    return nullptr;
  };
  switch (e.kind) {
    case Kind::IntLit:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      return nullptr;
    case Kind::Unary:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      return deduce_type(*static_cast<const Unary&>(e).operand);
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      if (b.op == BinOp::Shl || b.op == BinOp::Shr) {
        const TypeExpr* lt = deduce_type(*b.lhs);
        if (lt) lt = canonicalize_type(lt);
        if (lt && lt->kind == Kind::TyName) {
          const auto& tn = static_cast<const TyName&>(*lt);
          if (const PrimitiveInfo* pi = primitive_info(ascii_lower(tn.name));
              pi && shift_carrier_primitive(pi)) {
            return builtin_integer_type(shift_carrier_primitive(pi));
          }
        }
      }
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      if (e.kind != Kind::Binary) return nullptr;
      {
        if (b.op == BinOp::Is) return builtin_boolean_type();
        if (b.op == BinOp::As) {
          if (b.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*b.rhs);
            auto ait = registry_->aliases.find(id.name);
            if (ait != registry_->aliases.end()) return ait->second.target.get();
            if (class_info_for_type_name(id.name)) return named_pascal_type(id.name);
          }
          return nullptr;
        }
        auto is_string_like = [&](const TypeExpr* t) {
          if (!t) return false;
          if (t->kind == Kind::TyString) return true;
          if (t->kind == Kind::TyName) {
            const auto& nm = ascii_lower(static_cast<const TyName&>(*t).name);
            return nm == "string" || nm == "shortstring" || nm == "ansistring";
          }
          return false;
        };
        auto canon_set = [&](const TypeExpr* t) -> const TypeExpr* {
          if (!t) return nullptr;
          const TypeExpr* c = canonicalize_type(t);
          return (c && c->kind == Kind::TySet) ? c : nullptr;
        };
        const TypeExpr* lt = deduce_type(*b.lhs);
        const TypeExpr* rt = deduce_type(*b.rhs);
        auto prim_of = [&](const TypeExpr* t) -> const PrimitiveInfo* {
          t = canonicalize_type(t);
          if (!t || t->kind != Kind::TyName) return nullptr;
          return primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
        };
        auto is_numeric_primitive = [&](const TypeExpr* t) {
          t = canonicalize_type(t);
          if (!t || t->kind != Kind::TyName) return false;
          const auto& name = ascii_lower(static_cast<const TyName&>(*t).name);
          const PrimitiveInfo* pi = primitive_info(name);
          return pi && (pi->int_kind != PrimitiveIntKind::None ||
                        name == "single" || name == "double" ||
                        name == "real" || name == "extended" ||
                        name == "comp");
        };
        auto is_overloadable_value_type = [&](const TypeExpr* t) {
          t = canonicalize_type(t);
          if (!t) return false;
          if (t->kind == Kind::TyName) {
            const auto& name = ascii_lower(static_cast<const TyName&>(*t).name);
            return !primitive_info(name) && !registry_->enums.count(name);
          }
          return t->kind == Kind::TyRecord || t->kind == Kind::TyObject ||
                 t->kind == Kind::TyInterface || t->kind == Kind::TyPointer ||
                 t->kind == Kind::TyArray || t->kind == Kind::TyMetaclass;
        };
        auto is_comparison = [&] {
          return b.op == BinOp::Eq || b.op == BinOp::NotEq ||
                 b.op == BinOp::Lt || b.op == BinOp::Gt ||
                 b.op == BinOp::LtEq || b.op == BinOp::GtEq;
        };
        if (is_comparison()) return builtin_boolean_type();
        auto is_arithmetic_like = [&] {
          return b.op == BinOp::Add || b.op == BinOp::Sub ||
                 b.op == BinOp::Mul || b.op == BinOp::IntDiv ||
                 b.op == BinOp::Mod || b.op == BinOp::Shl ||
                 b.op == BinOp::Shr || b.op == BinOp::And ||
                 b.op == BinOp::Or || b.op == BinOp::Xor;
        };
        if (is_arithmetic_like()) {
          if (same_type_ast(lt, rt)) return lt ? lt : rt;
          if (is_overloadable_value_type(lt) && is_numeric_primitive(rt)) {
            return lt;
          }
          if (is_numeric_primitive(lt) && is_overloadable_value_type(rt)) {
            return rt;
          }
          if (is_numeric_primitive(lt) && is_numeric_primitive(rt)) {
            const PrimitiveInfo* lp = prim_of(lt);
            const PrimitiveInfo* rp = prim_of(rt);
            if (lp && rp && lp->int_kind != PrimitiveIntKind::None &&
                rp->int_kind != PrimitiveIntKind::None) {
              return (lp->bits >= rp->bits) ? lt : rt;
            }
            return lt ? lt : rt;
          }
        }
        // String / set binary ops. Pascal's `+`/`-`/`*` are overloaded on
        // string concatenation and set operations. Typing them here keeps
        // overload ranking and later emit-time lowering anchored to Pascal
        // semantics instead of falling through as "unknown binary expr".
        if (b.op == BinOp::Add &&
            (is_string_like(lt) || is_string_like(rt) ||
             b.lhs->kind == Kind::StringLit || b.rhs->kind == Kind::StringLit)) {
          auto is_ansistring = [&](const TypeExpr* t) {
            return t && t->kind == Kind::TyName &&
                   ascii_lower(static_cast<const TyName&>(*t).name) ==
                       "ansistring";
          };
          if (is_ansistring(lt) || is_ansistring(rt)) {
            return named_pascal_type("ansistring");
          }
          return named_pascal_type("shortstring");
        }
        if (b.op == BinOp::Add || b.op == BinOp::Sub || b.op == BinOp::Mul) {
          // Set union (+) / difference (-) / intersection (*). Pascal forbids
          // mixing element types, so either typed operand anchors the whole
          // expression. The other side may be a bare `[...]` literal with no
          // type of its own; once one side is a set, we still know the result.
          if (const TypeExpr* lset = canon_set(lt)) {
            return b.lhs->kind == Kind::SetLit ? lset : lt;
          }
          if (const TypeExpr* rset = canon_set(rt)) {
            return b.rhs->kind == Kind::SetLit ? rset : rt;
          }
        }
      }
      break;
    }
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      // Local variables and parameters shadow everything.
      auto lit = scope_.local_types.find(id.name);
      if (lit != scope_.local_types.end()) return lit->second;
      auto lcit = scope_.local_consts.find(id.name);
      if (lcit != scope_.local_consts.end() && lcit->second) {
        return deduce_const_decl_type(*lcit->second);
      }
      // Nested functions live in `local_nested_fns`, not `local_types`. Type
      // deduction still needs to see their result type so boolean expressions
      // like `if ready and flag then` lower correctly before the ident emitter
      // auto-calls a parameterless `ready`.
      auto nit = scope_.local_nested_fns.find(id.name);
      if (nit != scope_.local_nested_fns.end() && nit->second.is_function) {
        return nit->second.return_type;
      }
      if (scope_.local_untyped_params.count(id.name)) {
        // Untyped Pascal params are raw storage slots. Treat the identifier
        // itself as Pascal `pointer` so pointer-slot assignments/casts can
        // still apply the central coercion rules instead of falling back to
        // a naked C++ `void*` assignment.
        return named_pascal_type("pointer");
      }
      // Self is handled structurally elsewhere; returning nullptr here keeps
      // member-access deduction on the Pascal-facing class-name path.
      if (id.name == "self" && !scope_.current_class_name.empty()) {
        return nullptr;
      }
      if (scope_.current_fn_is_function && scope_.current_fn_result_type &&
          !scope_.current_fn_name.empty() && id.name == scope_.current_fn_name) {
        return scope_.current_fn_result_type;
      }
      if (scope_.bare_result_type && is_pascal_result_ident(id.name)) {
        return scope_.bare_result_type;
      }
      if (scope_.outer_result_type && !scope_.outer_result_name.empty() &&
          id.name == scope_.outer_result_name) {
        return scope_.outer_result_type;
      }
      // Class member lookup inside a known method body.
      if (!scope_.current_class_name.empty()) {
        if (const auto* ci = class_info_for_type_name(scope_.current_class_name);
            ci && ci->is_reference_type) {
          if (id.name == "instancesize") return builtin_integer_type("longint");
          if (id.name == "classtype") return named_pascal_type("tclass");
          if (id.name == "inheritsfrom") return builtin_boolean_type();
        }
        if (auto* f = registry_->lookup_class_field(scope_.current_class_name, id.name)) {
          return f->type.get();
        }
        if (auto* p = registry_->lookup_class_property(scope_.current_class_name, id.name)) {
          return p->type.get();
        }
        if (auto* m = registry_->lookup_class_method(scope_.current_class_name, id.name);
            m && m->decl && m->decl->return_type) {
          return m->decl->return_type.get();
        }
      }
      // `with X do` bindings contribute fields/properties/methods from their
      // target type. The innermost active `with` binding wins.
      for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
           ++it) {
        const std::string& ac = it->class_name;
        if (const auto* ci = class_info_for_type_name(ac);
            ci && ci->is_reference_type) {
          if (id.name == "instancesize") return builtin_integer_type("longint");
          if (id.name == "classtype") return named_pascal_type("tclass");
          if (id.name == "inheritsfrom") return builtin_boolean_type();
        }
        if (!ac.empty()) {
          if (auto* f = registry_->lookup_class_field(ac, id.name)) {
            return f->type.get();
          }
          if (auto* p = registry_->lookup_class_property(ac, id.name)) {
            return p->type.get();
          }
          if (auto* m = registry_->lookup_class_method(ac, id.name);
              m && m->decl && m->decl->return_type) {
            return m->decl->return_type.get();
          }
        }
        if (const TypeExpr* rf = lookup_record_field_type_in_with(*it, id.name)) {
          return rf;
        }
      }
      auto lookup_own = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_var(id.name)) return v->type.get();
        if (auto* c = u.find_const(id.name)) return deduce_const_info_type(*c);
        if (auto* p = u.find_proc(id.name);
            p && p->decl && p->decl->return_type) {
          return p->decl->return_type.get();
        }
        return nullptr;
      };
      auto lookup_export = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_export_var(id.name)) return v->type.get();
        if (auto* c = u.find_export_const(id.name)) return deduce_const_info_type(*c);
        if (auto* p = u.find_export_proc(id.name);
            p && p->decl && p->decl->return_type) {
          return p->decl->return_type.get();
        }
        return nullptr;
      };
      // Own-unit: both interface and implementation are visible. Other units:
      // only interface exports reached through the actual `uses` chain count.
      // Do not consult the registry's global last-wins maps here; two units
      // can export the same name with different meanings and Pascal only sees
      // the units actually imported by the current unit.
      auto cur = registry_->units.find(scope_.current_unit_name);
      if (cur != registry_->units.end()) {
        if (const auto* t = lookup_own(cur->second)) return t;
        if (const auto* t = find_unit_enum_type(cur->second.name, id.name)) {
          return t;
        }
        for (auto it = cur->second.uses.rbegin();
             it != cur->second.uses.rend(); ++it) {
          auto uit = registry_->units.find(*it);
          if (uit == registry_->units.end()) continue;
          if (const auto* t = lookup_export(uit->second)) return t;
          if (const auto* t = find_unit_enum_type(uit->second.name, id.name)) {
            return t;
          }
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
        return static_cast<const TyPointer&>(*t).target.get();
      }
      return nullptr;
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        auto uit = registry_->units.find(id.name);
        if (uit != registry_->units.end() &&
            uit->second.has_export_enum_member(m.name)) {
          return find_unit_enum_type(uit->second.name, m.name);
        }
      }
      std::string cls;
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        if (id.name == "self") cls = scope_.current_class_name;
        else if (registry_->classes.count(id.name) ||
                 registry_->records.count(id.name)) cls = id.name;
      }
      if (cls.empty()) {
        // Chained accesses like `x.sym.name` and result-slot writes like
        // `clone.next := nil` must recover the Pascal class alias from the
        // base expression, not from the canonicalized class body node.
        const TypeExpr* bt = deduce_type(*m.base);
        if (bt) cls = metaclass_target_name(bt);
      }
      if (cls.empty()) cls = deduce_class_alias(*m.base);
      if (cls.empty()) {
        if (const TypeExpr* rf =
                lookup_record_field_type_in_type(deduce_type(*m.base), m.name)) {
          return rf;
        }
        return nullptr;
      }
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type) {
        if (m.name == "instancesize") return builtin_integer_type("longint");
        if (m.name == "classtype") return named_pascal_type("tclass");
        if (m.name == "inheritsfrom") return builtin_boolean_type();
      }
      if (auto* pm = registry_->lookup_class_method(cls, m.name)) {
        if (pm->decl.get() && pm->decl.get()->return_type) {
          return pm->decl.get()->return_type.get();
        }
        return nullptr;
      }
      if (auto* pf = registry_->lookup_class_field(cls, m.name)) {
        return pf->type.get();
      }
      if (auto* pp = registry_->lookup_class_property(cls, m.name)) {
        return pp->type.get();
      }
      if (auto* rf = registry_->lookup_record_field(cls, m.name)) {
        return rf->type.get();
      }
      // Procedure-local record aliases can still produce a non-empty
      // `cls` (e.g. `p^.field` through `type PDir = ^TDir;`) even though the
      // alias is not in the global registry maps above. Fall back to direct
      // structural field lookup on the deduced base type before giving up.
      if (const TypeExpr* rf =
              lookup_record_field_type_in_type(deduce_type(*m.base), m.name)) {
        return rf;
      }
      return nullptr;
    }
    case Kind::Index: {
      const auto& ix = static_cast<const Index&>(e);
      if (registry_ && ix.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*ix.base);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = scope_.current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry_->lookup_class_property(cls, mem.name);
              prop && !prop->params.empty()) {
            return prop->type.get();
          }
        }
      }
      if (registry_ && ix.base->kind == Kind::Ident) {
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
      if (bt && bt->kind == Kind::TyArray) {
        return static_cast<const TyArray&>(*bt).element.get();
      }
      if (bt && bt->kind == Kind::TyPointer) {
        return static_cast<const TyPointer&>(*bt).target.get();
      }
      if (registry_) {
        std::string cls = deduce_class_alias(*ix.base);
        if (!cls.empty()) {
          if (auto* prop = registry_->lookup_default_property(cls)) {
            return prop->type.get();
          }
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
        if ((id.name == "char" || id.name == "chr") && c.args.size() == 1) {
          return builtin_char_type();
        }
        if (c.args.size() == 1) {
          if (const TyName* int_ty = builtin_integer_type(id.name)) {
            return int_ty;
          }
          if (is_builtin_reference_class_name(id.name) ||
              registry_->classes.count(id.name) ||
              registry_->records.count(id.name)) {
            return named_pascal_type(id.name);
          }
        }
        if ((id.name == "low" || id.name == "high") && c.args.size() == 1) {
          if (c.args[0]->kind == Kind::Ident) {
            const auto& arg_id = static_cast<const Ident&>(*c.args[0]);
            if (const TypeExpr* named = lookup_named_type_expr(arg_id.name)) {
              return named;
            }
            if (const TyName* int_ty = builtin_integer_type(arg_id.name)) {
              return int_ty;
            }
          }
          return deduce_type(*c.args[0]);
        }
        if (id.name == "sizeof" && c.args.size() == 1) {
          return builtin_integer_type("longint");
        }
        if (id.name == "pointer" && c.args.size() == 1) {
          return named_pascal_type("pointer");
        }
        if (id.name == "pchar" && c.args.size() == 1) return builtin_pchar_type();
        if ((id.name == "succ" || id.name == "pred" || id.name == "upcase" ||
             id.name == "abs" || id.name == "sqr" ||
             id.name == "swapendian" || id.name == "beton" ||
             id.name == "leton" || id.name == "ntobe" ||
             id.name == "ntole") &&
            c.args.size() == 1) {
          return deduce_type(*c.args[0]);
        }
        auto nit = scope_.local_nested_fns.find(id.name);
        if (nit != scope_.local_nested_fns.end() && nit->second.is_function) {
          return nit->second.return_type;
        }
        auto ait = registry_->aliases.find(id.name);
        if (ait != registry_->aliases.end() && c.args.size() == 1) {
          // Type cast `T(expr)` -- the call expression has the alias's own
          // Pascal type even before later emit-time cast spelling happens.
          return ait->second.target.get();
        }
        ResolveResult rr = resolve_name_provider_.resolve_name(id.name);
        if (rr.proc && rr.proc->return_type) {
          // Function call -> return type. Name resolution already obeyed
          // Pascal lexical / unit visibility rules, so avoid any global
          // last-wins fallback here.
          return rr.proc->return_type.get();
        }
        if (!rr.return_type_name.empty()) {
          return named_pascal_type(rr.return_type_name);
        }
      } else if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          if (registry_->units.count(id.name)) {
            ResolveResult rr = resolve_name_provider_.resolve_name(
                mem.name, QualifierKind::Unit, id.name);
            if (rr.proc && rr.proc->return_type) {
              return rr.proc->return_type.get();
            }
            if (!rr.return_type_name.empty()) {
              return named_pascal_type(rr.return_type_name);
            }
          }
        }
        std::string cls;
        if (mem.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          if (id.name == "self") cls = scope_.current_class_name;
          else if (registry_->classes.count(id.name) ||
                   registry_->records.count(id.name)) cls = id.name;
        }
        if (cls.empty()) {
          const TypeExpr* bt = deduce_type(*mem.base);
          if (bt) cls = metaclass_target_name(bt);
        }
        if (cls.empty()) cls = deduce_class_alias(*mem.base);
        if (!cls.empty()) {
          if (auto* cm = registry_->lookup_class_method(cls, mem.name)) {
            if (cm->decl.get() && cm->decl.get()->return_type) {
              return cm->decl.get()->return_type.get();
            }
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
    case Kind::SetLit:
      return deduce_set_literal_type(static_cast<const SetLit&>(e));
    case Kind::AddrOf:
      // Keep `@array` intentionally untyped: the emitter still needs to pick
      // between pointer-to-array and pointer-to-first-element at the use site.
      // Everything else can expose its typed pointer result here so later
      // pointer-slot coercion sees the actual Pascal pointee type.
      if (const auto& a = static_cast<const AddrOf&>(e); a.operand) {
        if (a.operand->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*a.operand);
          if (scope_.local_untyped_params.count(id.name)) {
            return named_pascal_type("pointer");
          }
        }
        const TypeExpr* operand_ty = deduce_type(*a.operand);
        const TypeExpr* canon = canonicalize_type(operand_ty);
        if (operand_ty && canon && canon->kind != Kind::TyArray) {
          auto tp = std::make_shared<TyPointer>();
          // The synthesized pointer type borrows the existing operand type; it
          // is only an emit-time view and does not own or mutate the pointee.
          tp->target = std::shared_ptr<TypeExpr>(
              const_cast<TypeExpr*>(operand_ty), [](TypeExpr*) {});
          synthesized_types_.push_back(tp);
          return tp.get();
        }
      }
      return nullptr;
    default:
      return nullptr;
  }
  return nullptr;
}

std::string EmitAnalysis::deduce_class_alias(const Expr& e) {
  if (!registry_) return {};
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    // Fast path for `self`: the surrounding method already tells us the class.
    if (id.name == "self") return scope_.current_class_name;
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      auto cit = registry_->classes.find(id.name);
      if (cit != registry_->classes.end() && cit->second.is_reference_type) {
        return id.name;
      }
    }
  }
  const TypeExpr* t = deduce_type(e);
  if (!t) return {};
  // Returning the metaclass target or direct Pascal type name keeps class
  // alias recovery on the source-language path instead of canonicalized class
  // bodies. That matters for casts, `self`, and chained member accesses.
  if (auto cls = metaclass_target_name(t); !cls.empty()) return cls;
  if (auto cls = registry_->direct_type_name(t); !cls.empty()) return cls;
  return registry_->direct_type_name(registry_->canonicalize(t));
}

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!registry_ || !type) return nullptr;
  type = canonicalize_type(type);
  if (!type) return nullptr;
  if (type->kind == Kind::TyPointer) {
    return lookup_record_field_type_in_type(
        static_cast<const TyPointer&>(*type).target.get(), field_name);
  }
  if (type->kind == Kind::TyName) {
    const auto& tn = static_cast<const TyName&>(*type);
    if (auto* rf = registry_->lookup_record_field(tn.name,
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

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_with(
    const ScopeStateView::WithBind& wb, std::string_view field_name) {
  if (const TypeExpr* ft = lookup_record_field_type_in_type(
          wb.type, field_name)) {
    return ft;
  }
  if (!registry_ || wb.class_name.empty()) return nullptr;
  if (auto* rf = registry_->lookup_record_field(wb.class_name,
                                                std::string(field_name))) {
    return rf->type.get();
  }
  return nullptr;
}

bool EmitAnalysis::with_bind_has_visible_member(
    const ScopeStateView::WithBind& wb, std::string_view name) {
  if (!registry_) return false;
  if (!wb.class_name.empty()) {
    if (registry_->lookup_class_method(wb.class_name, std::string(name)) ||
        registry_->lookup_class_field(wb.class_name, std::string(name)) ||
        registry_->lookup_class_property(wb.class_name, std::string(name))) {
      return true;
    }
  }
  return lookup_record_field_type_in_with(wb, name) != nullptr;
}

const VarInfo* EmitAnalysis::find_visible_unit_var(const std::string& name) {
  if (!registry_) return nullptr;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return nullptr;
  // Own unit first, then `uses` entries right-to-left: the same visibility
  // order used by ordinary name resolution, without registry-global shadowing.
  if (const auto* v = cur->second.find_var(name)) return v;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    auto uit = registry_->units.find(*it);
    if (uit == registry_->units.end()) continue;
    if (const auto* v = uit->second.find_export_var(name)) return v;
  }
  return nullptr;
}

const ConstInfo* EmitAnalysis::find_visible_unit_const(const std::string& name) {
  if (!registry_) return nullptr;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return nullptr;
  // Same visibility policy as variables: own unit first, then imported units
  // in the current unit's actual `uses` order.
  if (const auto* c = cur->second.find_const(name)) return c;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    auto uit = registry_->units.find(*it);
    if (uit == registry_->units.end()) continue;
    if (const auto* c = uit->second.find_export_const(name)) return c;
  }
  return nullptr;
}

const EnumInfoReg* EmitAnalysis::find_visible_enum_info_for_member(
    const std::string& name) {
  if (!registry_) return nullptr;

  auto find_in_unit = [&](const std::string& unit_name) -> const EnumInfoReg* {
    for (const auto& [enum_name, info] : registry_->enums) {
      (void)enum_name;
      if (info.defining_unit != unit_name) continue;
      for (const auto& member : info.members) {
        if (member == name) return &info;
      }
    }
    return nullptr;
  };

  if (const auto* info = find_in_unit(scope_.current_unit_name)) return info;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return nullptr;
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend(); ++it) {
    if (const auto* info = find_in_unit(*it)) return info;
  }
  return nullptr;
}

std::string EmitAnalysis::implicit_self_cxx() {
  if (!scope_.current_class_name.empty()) {
    if (const auto* ci = class_info_for_type_name(scope_.current_class_name)) {
      // Old object values use `(*this)` while reference classes use `this`.
      // Property lowering asks this helper instead of re-deriving that split.
      return ci->is_reference_type ? "this" : "(*this)";
    }
  }
  return "(*this)";
}

std::optional<ImplicitPropertyLookup> EmitAnalysis::find_implicit_class_property(
    std::string_view name) {
  if (!registry_) return std::nullopt;
  if (scope_.local_scope.count(std::string(name))) return std::nullopt;

  // `with` bindings shadow the ambient class scope for property lookup just as
  // they do for fields/methods, so search them from innermost to outermost.
  for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend(); ++it) {
    const std::string& cls = it->class_name;
    if (cls.empty()) continue;
    if (auto* prop = registry_->lookup_class_property(cls, std::string(name))) {
      return ImplicitPropertyLookup{prop, cls, it->cxx_text, true};
    }
  }

  if (scope_.current_class_name.empty()) return std::nullopt;
  // Fall back to the current class only after locals and `with` scopes have
  // been ruled out; otherwise a same-named local would spuriously become a
  // property access.
  if (auto* prop = registry_->lookup_class_property(scope_.current_class_name,
                                                    std::string(name))) {
    return ImplicitPropertyLookup{prop, scope_.current_class_name,
                                  implicit_self_cxx(), false};
  }
  return std::nullopt;
}

}  // namespace tp2cc
