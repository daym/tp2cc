#include "emit_values.h"

#include <memory>
#include <string>

#include "emit_analysis.h"
#include "diag.h"
#include "emit_resolution.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

std::string string_literal_body_char_to_cxx(char c) {
  switch (c) {
    case '\\': return "\\\\";
    case '"': return "\\\"";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: break;
  }
  unsigned char b = static_cast<unsigned char>(c);
  if (b < 0x20 || b >= 0x7f) {
    std::string out = "\\";
    out.push_back(static_cast<char>('0' + ((b >> 6) & 7)));
    out.push_back(static_cast<char>('0' + ((b >> 3) & 7)));
    out.push_back(static_cast<char>('0' + (b & 7)));
    return out;
  }
  return std::string(1, c);
}

}  // namespace

EmitValues::EmitValues(const TypeRegistry* registry, ScopeStateView& scope,
                       EmitAnalysis& analysis, EmitTypes& types,
                       EmitStorage& storage, EmitResolution& resolution,
                       OverloadTypeProvider& overload_types,
                       EmitValueExprOps& expr_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      storage_(storage),
      resolution_(resolution),
      overload_types_(overload_types),
      expr_ops_(expr_ops) {}

std::string EmitValues::set_literal_to_cxx(const SetLit& s,
                                           const TypeExpr* target) {
  if (!target) target = analysis_.deduce_set_literal_type(s);
  const TypeExpr* elem_type = nullptr;
  if (target) {
    const TypeExpr* canon = analysis_.canonicalize_type(target);
    if (canon && canon->kind == Kind::TySet) {
      elem_type = static_cast<const TySet&>(*canon).element.get();
    }
  }

  auto same_type_cxx = [&](const TypeExpr* a, const TypeExpr* b) {
    if (!a || !b) return false;
    return types_.type_to_cxx(*a) == types_.type_to_cxx(*b);
  };

  bool has_range = false;
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      has_range = true;
      break;
    }
  }

  if (elem_type) {
    const TypeExpr* canonical_elem_type = analysis_.canonicalize_type(elem_type);
    if (!has_range && canonical_elem_type &&
        canonical_elem_type->kind == Kind::TyEnum) {
      const TypeExpr* common_literal_type = nullptr;
      bool consistent_literal_type = true;
      for (const auto& el : s.elements) {
        const TypeExpr* literal_type = analysis_.deduce_type(*el);
        if (!literal_type) {
          consistent_literal_type = false;
          break;
        }
        if (!common_literal_type) {
          common_literal_type = literal_type;
          continue;
        }
        if (!same_type_cxx(common_literal_type, literal_type)) {
          consistent_literal_type = false;
          break;
        }
      }
      // Cross-unit calls can context-type a set literal through a runtime enum
      // alias (`tfpuexceptionmask`) even when every literal member comes from a
      // unit-local enum (`globals.tfpuexception`). Preserve the literal's more
      // specific enum type in that case so the generated `tp2cc_Set<...>`
      // matches the actual Pascal members instead of forcing them through the
      // runtime alias.
      if (consistent_literal_type && common_literal_type &&
          !same_type_cxx(common_literal_type, elem_type)) {
        elem_type = common_literal_type;
      }
    }

    const std::string elem_cxx = types_.type_to_cxx(*elem_type);
    if (s.elements.empty()) return "::rt::tp2cc_Set<" + elem_cxx + ">{}";
    if (!has_range) {
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
    const char* cap = expr_ops_.in_block_scope() ? "[&]" : "[]";
    return std::string("(") + cap + "{ " + body + " }())";
  }

  if (s.elements.empty()) {
    return "::rt::EmptySet{}";
  }
  if (!has_range) {
    std::string out = "::rt::set_of(";
    for (size_t i = 0; i < s.elements.size(); ++i) {
      if (i) out += ", ";
      out += expr_ops_.expr_to_cxx(*s.elements[i]);
    }
    out += ")";
    return out;
  }

  std::string first;
  if (s.elements.front()->kind == Kind::Range) {
    first = expr_ops_.expr_to_cxx(
        *static_cast<const Range&>(*s.elements.front()).lo);
  } else {
    first = expr_ops_.expr_to_cxx(*s.elements.front());
  }
  std::string body =
      "::rt::tp2cc_Set<decltype(" + first + ")> tp2cc_set{};";
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      const auto& r = static_cast<const Range&>(*el);
      body += " for (int64_t tp2cc_value = (int64_t)(" +
              expr_ops_.expr_to_cxx(*r.lo) + "); tp2cc_value <= (int64_t)(" +
              expr_ops_.expr_to_cxx(*r.hi) +
              "); ++tp2cc_value) tp2cc_set.add(static_cast<decltype(" + first +
              ")>(tp2cc_value));";
    } else {
      body += " tp2cc_set.add(" + expr_ops_.expr_to_cxx(*el) + ");";
    }
  }
  body += " return tp2cc_set;";
  const char* cap = expr_ops_.in_block_scope() ? "[&]" : "[]";
  return std::string("(") + cap + "{ " + body + " }())";
}

std::string EmitValues::pchar_string_literal_to_cxx(const StringLit& lit) {
  // Pascal string literals normally lower as ShortString values. This is a
  // local target-type-directed conversion at the use site: when an assignment,
  // return, typed initializer, array element, or call argument expects PChar,
  // FPC accepts the literal as a NUL-terminated character pointer. Use C++
  // string literal static storage, with no heap allocation or shared scratch
  // buffer. The resulting pointer is not safe to write through, just like a C
  // string literal cast to char*.
  std::string out =
      "const_cast<::rt::p_char*>(reinterpret_cast<const ::rt::p_char*>(\"";
  for (char c : lit.value) out += string_literal_body_char_to_cxx(c);
  out += "\"))";
  return out;
}

std::string EmitValues::const_value_to_cxx(const Expr& e, const TypeExpr* target,
                                           bool explicit_conversion) {
  if (!target) return expr_ops_.expr_to_cxx(e);
  if (e.kind == Kind::StringLit) {
    const auto& lit = static_cast<const StringLit&>(e);
    const TypeExpr* canon = analysis_.canonicalize_type(target);
    if (storage_.type_is_pcharish(target)) {
      return pchar_string_literal_to_cxx(lit);
    }
    if (canon && canon->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon);
      const TypeExpr* elem =
          arr.element ? analysis_.canonicalize_type(arr.element.get()) : nullptr;
      std::string lo;
      std::string size_expr;
      if (arr.array_kind == ArrayKind::Fixed && arr.dims.size() == 1 && elem &&
          types_.array_dim_bounds_to_cxx(*arr.dims[0], &lo, &size_expr) &&
          (tyname_is_charish(elem) || tyname_is(elem, "byte"))) {
        return "::rt::tp2cc_array_literal<" + types_.type_to_cxx(*elem) + ", " +
               lo + ", " + size_expr + ">(" + expr_ops_.expr_to_cxx(e) + ")";
      }
    }
    if (auto cap = types_.shortstring_capacity_to_cxx(target)) {
      if (lit.value.size() == 1) {
        return "::rt::tp2cc_shortstring_of<" + *cap +
               ">(::rt::tp2cc_char_of('" +
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
  const TypeExpr* canon_target = analysis_.canonicalize_type(target);
  if (e.kind == Kind::ArrayConst) {
    const TypeExpr* canon = canon_target;
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
    if (canon && canon->kind == Kind::TyArray) return "{.data = " + out + "}";
    return out;
  }
  if (e.kind == Kind::RecordConst) {
    const auto& rc = static_cast<const RecordConst&>(e);
    std::string out = "{";
    for (size_t i = 0; i < rc.fields.size(); ++i) {
      if (i) out += ", ";
      const TypeExpr* field_type =
          analysis_.lookup_record_field_type_in_type(target, rc.fields[i].first);
      const std::string field_name =
          registry_ ? registry_->field_cxx_name(rc.fields[i].first)
                    : mangle(rc.fields[i].first);
      out += "." + field_name + " = " +
             const_value_to_cxx(*rc.fields[i].second, field_type,
                                explicit_conversion);
    }
    out += "}";
    return out;
  }
  if (e.kind == Kind::SetLit) {
    return set_literal_to_cxx(static_cast<const SetLit&>(e), target);
  }
  if (auto text = maybe_convert_const_int_expr(e, target, explicit_conversion)) {
    return *text;
  }
  if (auto text = maybe_lower_metaclass_value(e, target)) {
    return *text;
  }
  if (auto text = maybe_convert_proc_value(e, target)) {
    return *text;
  }
  std::string out = expr_ops_.expr_to_cxx(e);
  const TypeExpr* source_type = overload_types_.type_for_overload(e);
  if (source_type) source_type = analysis_.canonicalize_type(source_type);
  if (source_type && canon_target) {
    if (auto conv = resolution_.find_assignment_operator(source_type, target);
        conv.decl) {
      std::string fn = pascal_assignment_operator_helper_name(*conv.decl);
      if (!conv.defining_unit.empty()) {
        fn = unit_namespace_prefix(conv.defining_unit) + fn;
      }
      return fn + "(" + out + ")";
    }
  }
  if (source_type && canon_target && source_type->kind == Kind::TySet &&
      canon_target->kind == Kind::TySet &&
      analysis_.classify_set_conversion(source_type, canon_target) !=
          SetConversionKind::Incompatible &&
      types_.type_to_cxx(*source_type) != types_.type_to_cxx(*canon_target)) {
    out = "::rt::tp2cc_set_cast<" + types_.type_to_cxx(*target) + ">(" + out +
          ")";
  }
  if (auto cap = types_.shortstring_capacity_to_cxx(target);
      cap && !(source_type && storage_.type_is_stringish(source_type))) {
    out = "::rt::tp2cc_shortstring_of<" + *cap + ">(" + out + ")";
  }
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

std::optional<std::string> EmitValues::maybe_convert_const_int_expr(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  if (!target) return std::nullopt;
  auto value = analysis_.eval_const_int_expr(e);
  if (!value) return std::nullopt;
  auto converted = analysis_.convert_const_int_value(
      e.loc, value->value, target, explicit_conversion, /*diagnose=*/true);
  if (!converted || !converted->type) return std::nullopt;
  std::string literal =
      (converted->type->int_kind == PrimitiveIntKind::Unsigned)
          ? uint64_literal_text(converted->bits)
          : ::tp2cc::signed_bits_literal_text(converted->bits,
                                              *converted->type);
  if (converted->type->bits == 64) {
    literal = "((" + std::string(converted->type->cxx) + ")(" + literal + "))";
  }
  return literal;
}

std::optional<std::string> EmitValues::maybe_convert_proc_value(
    const Expr& e, const TypeExpr* target) {
  if (!target) return std::nullopt;
  const TypeExpr* canon = analysis_.canonicalize_type(target);
  if (!(canon && canon->kind == Kind::TyProcedural)) return std::nullopt;
  const auto& proc = static_cast<const TyProcedural&>(*canon);

  if (proc.is_method && e.kind == Kind::NilLit) {
    return types_.type_to_cxx(*target) + "{}";
  }

  if (!proc.is_method) {
    auto reject_metaclass_member = [&](const Expr& value) -> bool {
      const Expr* candidate = &value;
      if (candidate->kind == Kind::AddrOf) {
        const auto& addr = static_cast<const AddrOf&>(*candidate);
        if (!addr.operand) return false;
        candidate = addr.operand.get();
      }
      if (candidate->kind != Kind::Member) return false;
      const auto& mem = static_cast<const Member&>(*candidate);
      if (!registry_ || !mem.base) return false;
      const std::string metaclass =
          analysis_.metaclass_target_name(analysis_.deduce_type(*mem.base));
      if (metaclass.empty()) return false;
      const auto* method = registry_->lookup_class_method(
          metaclass, mem.name, scope_.current_unit_name);
      if (!(method && (method->kind == SymKind::ClassMethod ||
                       method->kind == SymKind::Constructor))) {
        return false;
      }
      report_error(value.loc,
                   "cannot use class method through metaclass value as a "
                   "plain procedural value");
      return true;
    };

    if (reject_metaclass_member(e)) return "nullptr";

    switch (e.kind) {
      case Kind::Ident:
      case Kind::Member:
      case Kind::AddrOf:
        return expr_ops_.expr_to_cxx_no_autocall(e);
      default:
        return std::nullopt;
    }
  }

  if (!registry_) return std::nullopt;
  const std::string target_cxx = types_.type_to_cxx(*target);

  auto method_code_text = [&](const std::string& cls,
                              const ProcDecl& pd) -> std::string {
    return "::rt::tp2cc_method_code<&" + types_.named_type_struct_cxx(cls) + "::" +
           types_.method_pointer_helper_name(pd) + ">()";
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

  auto bind_current_method =
      [&](const std::string& name) -> std::optional<std::string> {
    if (scope_.current_class_name.empty()) return std::nullopt;
    if (auto* method =
            registry_->lookup_class_method(scope_.current_class_name, name,
                                           scope_.current_unit_name);
        method && method->decl && !method->decl->is_class_method) {
      return bind_method("(*this)", scope_.current_class_name, *method->decl,
                         false);
    }
    return std::nullopt;
  };

  auto bind_member = [&](const Member& m) -> std::optional<std::string> {
    std::string cls;
    if (m.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*m.base).name == "self") {
      cls = scope_.current_class_name;
    } else {
      cls = analysis_.deduce_class_alias(*m.base);
    }
    if (cls.empty()) return std::nullopt;
    if (auto* method = registry_->lookup_class_method(
            cls, m.name, scope_.current_unit_name);
        method && method->decl && !method->decl->is_class_method) {
      return bind_method(expr_ops_.expr_to_cxx(*m.base), cls, *method->decl,
                         storage_.expr_is_reference_class(*m.base));
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

std::optional<std::string> EmitValues::maybe_lower_metaclass_value(
    const Expr& e, const TypeExpr* target) {
  const std::string base_name = analysis_.metaclass_target_name(target);
  if (base_name.empty()) return std::nullopt;

  auto concrete_class_name = [&](const Expr& src) -> std::string {
    if (src.kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(src);
      if (const auto* ci = analysis_.class_info_for_type_name(id.name);
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
      if (const auto* ci = analysis_.class_info_for_type_name(qualified);
          ci && ci->is_reference_type) {
        return qualified;
      }
    }
    return {};
  };

  const std::string concrete_name = concrete_class_name(e);
  if (concrete_name.empty()) return std::nullopt;
  if (!analysis_.class_info_for_type_name(base_name)) return std::nullopt;
  return types_.metaclass_value_fn_cxx(concrete_name) + "()";
}

}  // namespace tp2cc
