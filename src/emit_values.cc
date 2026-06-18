#include "emit_values.h"

#include <memory>
#include <string>
#include <utility>

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

bool EmitValues::same_cxx_type(const TypeExpr* a, const TypeExpr* b) {
  if (!a || !b) return false;
  return types_.type_to_cxx(*a) == types_.type_to_cxx(*b);
}

const TypeExpr* EmitValues::set_literal_member_source_type(const Expr& e) {
  // Set-literal member typing preserves the source enum owner's unit. A target
  // set type can be an alias from a different unit, but the literal's enum
  // constants still need their own defining-unit carrier.
  return analysis_.deduce_type(e);
}

const TypeExpr* EmitValues::metaclass_value_base_type(const Expr& e) {
  // Plain procedural values cannot be class-method dispatches through a
  // metaclass variable. This query identifies that metaclass base; it is not a
  // target-typed conversion of the member expression.
  return analysis_.deduce_type(e);
}

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
        const TypeExpr* literal_type = set_literal_member_source_type(*el);
        if (!literal_type) {
          consistent_literal_type = false;
          break;
        }
        if (!common_literal_type) {
          common_literal_type = literal_type;
          continue;
        }
        if (!same_cxx_type(common_literal_type, literal_type)) {
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
          !same_cxx_type(common_literal_type, elem_type)) {
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

bool EmitValues::source_is_const_untyped_storage_arg(const Expr& e) const {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    return scope_.local_untyped_params.count(id.name) &&
           scope_.local_const_params.count(id.name);
  }
  if (e.kind == Kind::AddrOf) {
    const auto& addr = static_cast<const AddrOf&>(e);
    if (addr.operand && addr.operand->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*addr.operand);
      return scope_.local_untyped_params.count(id.name) &&
             scope_.local_const_params.count(id.name);
    }
  }
  return false;
}

std::string EmitValues::apply_target_pointer_conversion(
    const Expr& e, const TypeExpr* target, const TypeExpr* source_type,
    std::string out, bool explicit_conversion) {
  if (!target || !source_type) return out;
  // This is the target-value conversion point shared by assignment RHSs,
  // value call arguments, returns, variable initializers, and typed-const
  // leaves. Keep pointer-like adaptation here instead of repeating it at
  // individual use sites; storage contexts and explicit Pascal casts have
  // their own callers because they bind different language constructs.
  const std::string dst_cxx = types_.type_to_cxx(*target);
  return storage_.coerce_pointer_like_text(
      dst_cxx, target, source_type, out, explicit_conversion,
      source_is_const_untyped_storage_arg(e));
}

std::optional<std::string> EmitValues::maybe_lower_target_pointer_arithmetic(
    const Expr& e, const TypeExpr* target, bool explicit_conversion,
    bool typed_const_initializer) {
  const TypeExpr* canon_target = analysis_.canonicalize_type(target);
  if (!canon_target || !storage_.type_is_pointerish(canon_target) ||
      e.kind != Kind::Binary) {
    return std::nullopt;
  }

  const auto& b = static_cast<const Binary&>(e);
  if (b.op != BinOp::Add && b.op != BinOp::Sub) return std::nullopt;
  if (!b.lhs || !b.rhs) return std::nullopt;

  const TypeExpr* lhs_type =
      analysis_.canonicalize_type(overload_types_.type_for_overload(*b.lhs));
  const TypeExpr* rhs_type =
      analysis_.canonicalize_type(overload_types_.type_for_overload(*b.rhs));
  auto integer_type = [](const TypeExpr* t) {
    if (!t || t->kind != Kind::TyName) return false;
    const PrimitiveInfo* pi =
        primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
    return pi && pi->int_kind != PrimitiveIntKind::None;
  };

  const bool lhs_ptr = storage_.type_is_pointerish(lhs_type);
  const bool rhs_ptr = storage_.type_is_pointerish(rhs_type);
  const bool lhs_int = integer_type(lhs_type);
  const bool rhs_int = integer_type(rhs_type);
  const char* op = b.op == BinOp::Add ? " + " : " - ";

  // Pointer arithmetic is a target-typed value context for its pointer
  // operand: `pchar := @fixed_array + n` first adapts `@fixed_array` to
  // `pchar`, then applies C++ pointer arithmetic to that element pointer.
  // The actual pointer adaptation remains in coerce_pointer_like_text.
  if (lhs_ptr && rhs_int) {
    return "(" + const_value_to_cxx_impl(*b.lhs, target, explicit_conversion,
                                         typed_const_initializer) +
           op + expr_ops_.expr_to_cxx(*b.rhs) + ")";
  }
  if (b.op == BinOp::Add && lhs_int && rhs_ptr) {
    return "(" + expr_ops_.expr_to_cxx(*b.lhs) + op +
           const_value_to_cxx_impl(*b.rhs, target, explicit_conversion,
                                   typed_const_initializer) +
           ")";
  }
  return std::nullopt;
}

std::string EmitValues::const_value_to_cxx(const Expr& e, const TypeExpr* target,
                                           bool explicit_conversion) {
  return const_value_to_cxx_impl(e, target, explicit_conversion,
                                 /*typed_const_initializer=*/false);
}

std::string EmitValues::typed_const_value_to_cxx(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  return const_value_to_cxx_impl(e, target, explicit_conversion,
                                 /*typed_const_initializer=*/true);
}

std::string EmitValues::const_value_to_cxx_impl(
    const Expr& e, const TypeExpr* target, bool explicit_conversion,
    bool typed_const_initializer) {
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
      if (arr.array_kind == ArrayKind::Fixed && arr.dims.size() == 1 && elem &&
          (tyname_is_charish(elem) || tyname_is(elem, "byte"))) {
        auto bounds = types_.array_dim_bounds_to_cxx(*arr.dims[0]);
        if (bounds) {
          return "::rt::tp2cc_array_literal<" + types_.type_to_cxx(*elem) +
                 ", " + bounds->low + ", " + bounds->size_expr + ">(" +
                 expr_ops_.expr_to_cxx(e) + ")";
        }
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
        std::vector<TypePtr> nested_dims(arr.dims.begin() + 1,
                                         arr.dims.end());
        nested_array_target = std::make_shared<TyArray>(
            arr.loc, std::move(nested_dims), arr.element, arr.is_packed,
            arr.array_kind);
        elem_type = nested_array_target.get();
      } else {
        elem_type = arr.element.get();
      }
    }
    const auto& ac = static_cast<const ArrayConst&>(e);
    std::string out = "{";
    for (size_t i = 0; i < ac.elements.size(); ++i) {
      if (i) out += ", ";
      out += const_value_to_cxx_impl(*ac.elements[i], elem_type,
                                     explicit_conversion,
                                     typed_const_initializer);
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
      std::string field_val =
          const_value_to_cxx_impl(*rc.fields[i].second, field_type,
                                  explicit_conversion,
                                  typed_const_initializer);
      out += "." + field_name + " = " + field_val;
    }
    out += "}";
    return out;
  }
  if (e.kind == Kind::SetLit) {
    return set_literal_to_cxx(static_cast<const SetLit&>(e), target);
  }
  if (auto text = maybe_lower_target_pointer_arithmetic(
          e, target, explicit_conversion, typed_const_initializer)) {
    return *text;
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
  if (e.kind == Kind::Call) {
    const auto& call = static_cast<const Call&>(e);
    if (call.args.size() == 1 && call.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*call.callee);
      if (const TypeExpr* cast_type = analysis_.lookup_named_type_expr(id.name)) {
        source_type = cast_type;
      }
    }
  }
  if (!source_type) source_type = analysis_.deduce_type(e);
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
  out = apply_target_pointer_conversion(e, target, source_type, std::move(out),
                                        explicit_conversion);
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
      e.loc, *value, target, explicit_conversion, /*diagnose=*/true);
  if (!converted || !converted->type) return std::nullopt;
  const uint8_t width = analysis_.resolved_primitive_bits(*converted->type);
  std::string literal =
      (converted->type->int_kind == PrimitiveIntKind::Unsigned)
          ? uint64_literal_text(converted->bits)
          : ::tp2cc::signed_bits_literal_text(converted->bits, width,
                                              converted->type->cxx);
  if (width == 64) {
    literal = "((" + std::string(converted->type->cxx) + ")(" + literal + "))";
  }
  return literal;
}

bool EmitValues::reject_metaclass_member_as_plain_proc_value(
    const Expr& value) {
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
      analysis_.metaclass_target_name(metaclass_value_base_type(*mem.base));
  if (metaclass.empty()) return false;
  const auto* methods = registry_->lookup_class_methods(
      metaclass, mem.name, scope_.current_unit_name);
  bool rejects = false;
  if (methods) {
    for (const auto& method : *methods) {
      if (method.kind == SymKind::ClassMethod ||
          method.kind == SymKind::Constructor) {
        rejects = true;
        break;
      }
    }
  }
  if (!rejects) return false;

  report_error(value.loc,
               "cannot use class method through metaclass value as a "
               "plain procedural value");
  return true;
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
    if (reject_metaclass_member_as_plain_proc_value(e)) return "nullptr";

    switch (e.kind) {
      case Kind::Ident:
      case Kind::Member:
        if (const TypeExpr* source = overload_types_.type_for_overload(e);
            storage_.type_is_pointerish(
                analysis_.canonicalize_type(source ? source
                                                   : analysis_.deduce_type(e)))) {
          return std::nullopt;
        }
        return expr_ops_.expr_to_cxx_no_autocall(e);
      case Kind::AddrOf:
        return expr_ops_.expr_to_cxx_no_autocall(e);
      default:
        return std::nullopt;
    }
  }

  // Picker and emitter share `resolve_method_value_binding` so they agree
  // by construction on which method a `@expr` resolves to. If we get a
  // binding, format the `tp2cc_MethodPtr` constructor; otherwise nullopt
  // and the caller falls back to plain expr_to_cxx.
  auto bind = resolution_.resolve_method_value_binding(e, proc);
  if (!bind || !bind->has_matching_decl()) return std::nullopt;
  const ast::ProcDecl& method_decl = bind->matching_decl();

  const std::string target_cxx = types_.type_to_cxx(*target);
  const std::string code_text = "::rt::tp2cc_method_code<&" +
                                types_.named_type_struct_cxx(bind->class_name) +
                                "::" +
                                types_.method_pointer_helper_name(method_decl) +
                                ">()";
  std::string self_expr;
  if (!bind->member_base) {
    self_expr = "(void*)(&((*this)))";
  } else {
    const std::string base_cxx = expr_ops_.expr_to_cxx(*bind->member_base);
    self_expr = storage_.expr_is_reference_class(*bind->member_base)
                    ? "(void*)(" + base_cxx + ")"
                    : "(void*)(&(" + base_cxx + "))";
  }
  return target_cxx + "(" + code_text + ", " + self_expr + ")";
}

std::string EmitValues::concrete_class_name_for_metaclass_value(
    const Expr& src) {
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
    if (!mem.base || mem.base->kind != Kind::Ident) return {};
    const auto& base = static_cast<const Ident&>(*mem.base);
    const std::string qualified = base.name + "." + mem.name;
    if (const auto* ci = analysis_.class_info_for_type_name(qualified);
        ci && ci->is_reference_type) {
      return qualified;
    }
  }
  return {};
}

std::optional<std::string> EmitValues::maybe_lower_metaclass_value(
    const Expr& e, const TypeExpr* target) {
  const std::string base_name = analysis_.metaclass_target_name(target);
  if (base_name.empty()) return std::nullopt;

  const std::string concrete_name = concrete_class_name_for_metaclass_value(e);
  if (concrete_name.empty()) return std::nullopt;
  if (!analysis_.class_info_for_type_name(base_name)) return std::nullopt;
  return types_.metaclass_value_fn_cxx(concrete_name) + "()";
}

}  // namespace tp2cc
