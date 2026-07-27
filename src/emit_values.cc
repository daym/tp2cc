#include "emit_values.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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

// Reference-class value compatibility is a class identity question, not a
// spelling question. Use the built registry parent links so validation sees
// the same hierarchy that overload scoring and method lookup use.
bool reference_class_derives_from(const TypeRegistry& registry,
                                  const ClassInfo* source,
                                  const ClassInfo* target) {
  if (!source || !target || !source->is_reference_type ||
      !target->is_reference_type) {
    return false;
  }
  std::unordered_set<const ClassInfo*> seen;
  for (const ClassInfo* cur = source; cur; cur = registry.lookup_parent_class(*cur)) {
    if (!seen.insert(cur).second) return false;
    if (registry.same_class_identity(*cur, *target)) {
      return true;
    }
  }
  return false;
}

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

struct FlatProcParamUse {
  const Param* param = nullptr;
  std::string name;
};

std::vector<FlatProcParamUse> flatten_proc_params_with_names(
    const std::vector<Param>& params) {
  std::vector<FlatProcParamUse> out;
  int unnamed_index = 0;
  for (const auto& param : params) {
    if (param.names.empty()) {
      out.push_back(
          FlatProcParamUse{&param, "tp2cc_arg" + std::to_string(++unnamed_index)});
      continue;
    }
    for (const auto& name : param.names) {
      out.push_back(FlatProcParamUse{&param, mangle(name)});
    }
  }
  return out;
}

const Expr* strip_single_addr_of(const Expr& e) {
  if (e.kind != Kind::AddrOf) return &e;
  const auto& addr = static_cast<const AddrOf&>(e);
  if (addr.double_addr || !addr.operand) return &e;
  return addr.operand.get();
}

bool record_has_pointer_field(EmitAnalysis& analysis, EmitStorage& storage,
                              const TypeExpr* record,
                              std::string_view field_name) {
  return storage.type_is_pointerish(
      analysis.lookup_record_field_type_in_type(record, field_name));
}

}  // namespace

EmitValues::EmitValues(const TypeRegistry& registry, ScopeStateView& scope,
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
    const TypeExpr* canon = analysis_.semantic_shape_type(target);
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
    const TypeExpr* canonical_elem_type = analysis_.semantic_shape_type(elem_type);
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
        if (!analysis_.same_type_ast(common_literal_type, literal_type)) {
          consistent_literal_type = false;
          break;
        }
      }
      const TypeExpr* literal_set_type = analysis_.deduce_set_literal_type(s);
      const bool literal_fits_target =
          literal_set_type &&
          analysis_.classify_set_conversion(literal_set_type, target) !=
              SetConversionKind::Incompatible;
      // Target typing can arrive through an alias whose element has the same
      // ordinal domain as the enum constants but a different declaration
      // identity. Use the constants' owner type for emission only after the
      // Pascal set-conversion check says that source set is compatible.
      if (consistent_literal_type && common_literal_type &&
          literal_fits_target &&
          !analysis_.same_type_ast(common_literal_type, elem_type)) {
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
  // p2cc accepts only Pascal's default {$T-} address mode today; the lexer
  // rejects {$T+}. Under {$T-}, `@expr` is an untyped address value that may be
  // converted by its target pointer slot. Do not use this as a general typed
  // pointer compatibility rule for non-address expressions.
  const bool address_value_conversion =
      e.kind == Kind::AddrOf && storage_.type_is_pointerish(target);
  return storage_.coerce_pointer_like_text(
      dst_cxx, target, source_type, out,
      explicit_conversion || address_value_conversion,
      source_is_const_untyped_storage_arg(e));
}

std::optional<std::string> EmitValues::maybe_lower_target_pointer_arithmetic(
    const Expr& e, const TypeExpr* target, bool explicit_conversion,
    bool constant_initializer) {
  const TypeExpr* canon_target = analysis_.semantic_shape_type(target);
  if (!canon_target || !storage_.type_is_pointerish(canon_target) ||
      e.kind != Kind::Binary) {
    return std::nullopt;
  }

  const auto& b = static_cast<const Binary&>(e);
  if (b.op != BinOp::Add && b.op != BinOp::Sub) return std::nullopt;
  if (!b.lhs || !b.rhs) return std::nullopt;

  const TypeExpr* lhs_type =
      analysis_.semantic_shape_type(overload_types_.type_for_overload(*b.lhs));
  const TypeExpr* rhs_type =
      analysis_.semantic_shape_type(overload_types_.type_for_overload(*b.rhs));

  const bool lhs_ptr = storage_.type_is_pointerish(lhs_type);
  const bool rhs_ptr = storage_.type_is_pointerish(rhs_type);
  const bool lhs_int = type_is_integer_primitive(lhs_type);
  const bool rhs_int = type_is_integer_primitive(rhs_type);
  const char* op = b.op == BinOp::Add ? " + " : " - ";

  // Pointer arithmetic is a target-typed value context for its pointer
  // operand: `pchar := @fixed_array + n` first adapts `@fixed_array` to
  // `pchar`, then applies C++ pointer arithmetic to that element pointer.
  // The actual pointer adaptation remains in coerce_pointer_like_text.
  if (lhs_ptr && rhs_int) {
    return "(" + const_value_to_cxx_impl(*b.lhs, target, explicit_conversion,
                                         constant_initializer) +
           op + expr_ops_.expr_to_cxx(*b.rhs) + ")";
  }
  if (b.op == BinOp::Add && lhs_int && rhs_ptr) {
    return "(" + expr_ops_.expr_to_cxx(*b.lhs) + op +
           const_value_to_cxx_impl(*b.rhs, target, explicit_conversion,
                                   constant_initializer) +
           ")";
  }
  return std::nullopt;
}

bool EmitValues::can_lower_target_pointer_arithmetic(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  const TypeExpr* canon_target = analysis_.semantic_shape_type(target);
  if (!canon_target || !storage_.type_is_pointerish(canon_target) ||
      e.kind != Kind::Binary) {
    return false;
  }

  const auto& b = static_cast<const Binary&>(e);
  if (b.op != BinOp::Add && b.op != BinOp::Sub) return false;
  if (!b.lhs || !b.rhs) return false;

  const TypeExpr* lhs_type =
      analysis_.semantic_shape_type(overload_types_.type_for_overload(*b.lhs));
  const TypeExpr* rhs_type =
      analysis_.semantic_shape_type(overload_types_.type_for_overload(*b.rhs));

  const bool lhs_ptr = storage_.type_is_pointerish(lhs_type);
  const bool rhs_ptr = storage_.type_is_pointerish(rhs_type);
  const bool lhs_int = type_is_integer_primitive(lhs_type);
  const bool rhs_int = type_is_integer_primitive(rhs_type);
  if (lhs_ptr && rhs_int) {
    return can_convert_value_to_type(*b.lhs, target, explicit_conversion);
  }
  if (b.op == BinOp::Add && lhs_int && rhs_ptr) {
    return can_convert_value_to_type(*b.rhs, target, explicit_conversion);
  }
  return false;
}

bool EmitValues::type_is_integer_primitive(const TypeExpr* t) {
  const PrimitiveInfo* pi = analysis_.primitive_info_for_type(t);
  return pi && pi->int_kind != PrimitiveIntKind::None;
}

std::string EmitValues::const_value_to_cxx(const Expr& e, const TypeExpr* target,
                                           bool explicit_conversion) {
  return const_value_to_cxx_impl(e, target, explicit_conversion,
                                 /*constant_initializer=*/false);
}

std::string EmitValues::const_initializer_to_cxx(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  return const_value_to_cxx_impl(e, target, explicit_conversion,
                                 /*constant_initializer=*/true);
}

bool EmitValues::can_convert_proc_value(const Expr& e, const TypeExpr* target,
                                        bool explicit_conversion) {
  if (!target) return false;
  const TypeExpr* canon = analysis_.semantic_shape_type(target);
  if (!(canon && canon->kind == Kind::TyProcedural)) return false;
  const auto& proc = static_cast<const TyProcedural&>(*canon);

  if (proc.is_method && e.kind == Kind::NilLit) return true;

  const TypeExpr* source_type = proc_value_source_type(e);
  source_type = analysis_.semantic_shape_type(source_type);
  if (source_type && source_type->kind == Kind::TyProcedural) {
    return resolution_.procedural_types_match(
        static_cast<const TyProcedural&>(*source_type), proc);
  }

  if (proc.is_method) {
    if (explicit_conversion && source_is_runtime_tmethod(source_type)) {
      return true;
    }
    auto bind =
        resolution_.resolve_method_value_binding(e, proc, explicit_conversion);
    return bind && bind->has_matching_decl();
  }

  TyProcedural method_view = proc;
  method_view.is_method = true;
  if (resolution_.resolve_method_value_binding(e, method_view)) return false;
  auto bind =
      resolution_.resolve_plain_proc_value_binding(e, proc, explicit_conversion);
  return bind && bind->decl;
}

bool EmitValues::can_convert_reference_class_value(const Expr& e,
                                                   const TypeExpr* source_type,
                                                   const TypeExpr* target) {
  const ClassInfo* source_class = analysis_.class_info_for_type(source_type);
  if (!source_class) {
    // A bare class identifier such as `TChild` is a value expression whose
    // concrete class is carried by the identifier binding, not by the
    // expression's TypeExpr.
    if (const TypeSymbol* source_symbol = analysis_.deduce_class_symbol(e)) {
      source_class = source_symbol->class_info();
    }
  }
  const ClassInfo* target_class = analysis_.class_info_for_type(target);
  return reference_class_derives_from(registry_, source_class, target_class);
}

bool EmitValues::can_convert_value_to_type(const Expr& e,
                                           const TypeExpr* target,
                                           bool explicit_conversion) {
  if (!target) return true;
  const TypeLookupContext* target_context =
      registry_.lookup_context_for_type(target);
  const TypeExpr* canon_target = analysis_.semantic_shape_type(target);
  if (!canon_target) return true;

  if (e.kind == Kind::NilLit) {
    return storage_.type_is_pointerish(canon_target);
  }

  if (e.kind == Kind::StringLit) {
    if (storage_.type_is_pcharish(target) ||
        types_.shortstring_capacity_to_cxx(target)) {
      return true;
    }
    if (canon_target->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon_target);
      const TypeExpr* elem =
          arr.element ? analysis_.semantic_shape_type(arr.element.get()) : nullptr;
      const PrimitiveInfo* elem_info = analysis_.primitive_info_for_type(elem);
      if (arr.array_kind == ArrayKind::Fixed && arr.dims.size() == 1 && elem &&
          (elem_info && (elem_info->is_char() ||
                         elem_info->kind == PrimitiveKind::Byte))) {
        return types_.array_dim_bounds_to_cxx(*arr.dims[0]).has_value();
      }
    }
  }

  if (e.kind == Kind::SetLit) {
    const auto& set = static_cast<const SetLit&>(e);
    if (canon_target->kind == Kind::TySet) return true;
    if (storage_.type_is_open_array(canon_target)) {
      for (const auto& element : set.elements) {
        if (element->kind == Kind::Range) return false;
      }
      return true;
    }
  }

  if (e.kind == Kind::ArrayConst) return canon_target->kind == Kind::TyArray;
  if (e.kind == Kind::RecordConst) return canon_target->kind == Kind::TyRecord;

  if (can_lower_target_pointer_arithmetic(e, target, explicit_conversion)) {
    return true;
  }

  if (auto value = analysis_.eval_const_int_expr(e)) {
    if (analysis_.convert_const_int_value(e.loc, *value, target,
                                          explicit_conversion,
                                          /*diagnose=*/false)) {
      return true;
    }
  }

  if (canon_target->kind == Kind::TyProcedural) {
    return can_convert_proc_value(e, target, explicit_conversion);
  }

  if (analysis_.concrete_class_symbol_for_metaclass_target(e, target) ||
      analysis_.concrete_class_symbol_for_metaclass_target(e, canon_target)) {
    return true;
  }

  const TypeExpr* explicit_cast_type =
      analysis_.explicit_typecast_result_type(e);
  const TypeExpr* source_type = explicit_cast_type;
  if (!source_type) source_type = analysis_.deduce_type(e);
  if (!source_type) source_type = overload_types_.type_for_overload(e);
  const TypeExpr* raw_source_type = source_type;
  const TypeExpr* canon_source_type = analysis_.semantic_shape_type(source_type);
  if (!raw_source_type || !canon_source_type) return false;

  if (can_convert_reference_class_value(e, raw_source_type, target)) {
    return true;
  }

  if (resolution_.rank_conversion(raw_source_type, target,
                                  /*var_param=*/false, target_context)
          .viable()) {
    return true;
  }
  if (resolution_.find_assignment_operator(canon_source_type, target,
                                           target_context)
          .decl) {
    return true;
  }
  // See apply_target_pointer_conversion: this is the supported {$T-}
  // target-typed address rule. Enabled typed-address mode is rejected by the
  // lexer until p2cc models it explicitly.
  if (e.kind == Kind::AddrOf && storage_.type_is_pointerish(canon_target)) {
    return true;
  }
  if (storage_.pointer_value_conversion_is_valid(target, raw_source_type,
                                                 explicit_conversion)) {
    return true;
  }
  if (storage_.expr_is_storage_lvalue(e) &&
      storage_.fixed_char_array_value_can_decay_to_pchar(raw_source_type,
                                                         target)) {
    return true;
  }
  if (canon_target->kind == Kind::TySet &&
      canon_source_type->kind == Kind::TySet &&
      analysis_.classify_set_conversion(canon_source_type, canon_target) !=
          SetConversionKind::Incompatible) {
    return true;
  }
  if (types_.shortstring_capacity_to_cxx(target) &&
      (storage_.type_is_stringish(canon_source_type) ||
       storage_.type_is_pcharish(canon_source_type) ||
       storage_.expr_is_charish(e))) {
    return true;
  }
  if (analysis_.type_is_long_string(canon_target)) {
    return storage_.type_is_stringish(canon_source_type) ||
           storage_.type_is_pcharish(canon_source_type) ||
           storage_.expr_is_charish(e);
  }
  if (storage_.type_is_open_array(canon_target)) {
    return canon_source_type->kind == Kind::TyArray;
  }
  return false;
}

std::string EmitValues::const_value_to_cxx_impl(
    const Expr& e, const TypeExpr* target, bool explicit_conversion,
    bool constant_initializer) {
  if (!target) return expr_ops_.expr_to_cxx(e);
  const TypeLookupContext* target_context =
      registry_.lookup_context_for_type(target);
  if (e.kind == Kind::StringLit) {
    const auto& lit = static_cast<const StringLit&>(e);
    const TypeExpr* canon = analysis_.semantic_shape_type(target);
    if (storage_.type_is_pcharish(target)) {
      return pchar_string_literal_to_cxx(lit);
    }
    if (canon && canon->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon);
      const TypeExpr* elem =
          arr.element ? analysis_.semantic_shape_type(arr.element.get()) : nullptr;
      const PrimitiveInfo* elem_info = analysis_.primitive_info_for_type(elem);
      if (arr.array_kind == ArrayKind::Fixed && arr.dims.size() == 1 && elem &&
          (elem_info && (elem_info->is_char() ||
                         elem_info->kind == PrimitiveKind::Byte))) {
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
  const TypeExpr* canon_target = analysis_.semantic_shape_type(target);
  if (e.kind == Kind::ArrayConst) {
    const TypeExpr* canon = canon_target;
    const TypeExpr* elem_type = nullptr;
    if (canon && canon->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*canon);
      if (arr.dims.size() > 1) {
        elem_type = registry_.array_tail_type(&arr, 1);
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
                                     constant_initializer);
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
          registry_.field_cxx_name(rc.fields[i].first);
      std::string field_val =
          const_value_to_cxx_impl(*rc.fields[i].second, field_type,
                                  explicit_conversion,
                                  constant_initializer);
      out += "." + field_name + " = " + field_val;
    }
    out += "}";
    return out;
  }
  if (e.kind == Kind::SetLit) {
    return set_literal_to_cxx(static_cast<const SetLit&>(e), target);
  }
  if (auto text = maybe_lower_target_pointer_arithmetic(
          e, target, explicit_conversion, constant_initializer)) {
    return *text;
  }
  if (constant_initializer || e.kind != Kind::Binary) {
    if (auto text =
            maybe_convert_const_int_expr(e, target, explicit_conversion)) {
      return *text;
    }
  }
  if (auto text = maybe_lower_metaclass_value(e, target)) {
    return *text;
  }
  if (auto text = maybe_convert_proc_value(e, target, explicit_conversion)) {
    return *text;
  }
  const TypeExpr* explicit_cast_type =
      analysis_.explicit_typecast_result_type(e);
  const TypeExpr* source_type = explicit_cast_type;
  if (!source_type) source_type = analysis_.deduce_type(e);
  if (!source_type) source_type = overload_types_.type_for_overload(e);
  if (source_type) source_type = analysis_.semantic_shape_type(source_type);
  if (canon_target && canon_target->kind == Kind::TyProcedural) {
    const auto& proc = static_cast<const TyProcedural&>(*canon_target);
    if (auto text = reject_method_pointer_record_cast(
            e, target, proc, source_type, explicit_conversion)) {
      return *text;
    }
  }
  std::string out = expr_ops_.expr_to_cxx(e);
  if (source_type && canon_target) {
    if (auto conv = resolution_.find_assignment_operator(source_type, target,
                                                         target_context);
        conv.decl) {
      std::string fn =
          pascal_assignment_operator_helper_name(registry_, *conv.decl);
      if (!conv.defining_unit.empty()) {
        fn = unit_namespace_prefix(conv.defining_unit) + fn;
      }
      return fn + "(" + out + ")";
    }
  }
  if (storage_.expr_is_storage_lvalue(e) &&
      storage_.fixed_char_array_value_can_decay_to_pchar(source_type, target)) {
    out = storage_.lower_fixed_char_array_value_to_pchar(source_type, target,
                                                         out);
  }
  if (!explicit_cast_type ||
      !analysis_.same_type_ast(explicit_cast_type, target)) {
    out = apply_target_pointer_conversion(e, target, source_type,
                                          std::move(out),
                                          explicit_conversion);
  }
  if (source_type && canon_target && source_type->kind == Kind::TySet &&
      canon_target->kind == Kind::TySet &&
      analysis_.classify_set_conversion(source_type, canon_target) !=
          SetConversionKind::Incompatible) {
    const auto& source_set = static_cast<const TySet&>(*source_type);
    const auto& target_set = static_cast<const TySet&>(*canon_target);
    const bool same_element =
        analysis_.same_type_ast(source_set.element.get(),
                                target_set.element.get());
    if (!same_element) {
      out = "::rt::tp2cc_set_cast<" + types_.type_to_cxx(*target) + ">(" +
            out + ")";
    }
  }
  if (auto cap = types_.shortstring_capacity_to_cxx(target);
      cap && !(source_type && storage_.type_is_stringish(source_type))) {
    out = "::rt::tp2cc_shortstring_of<" + *cap + ">(" + out + ")";
  }
  if (analysis_.type_is_long_string(canon_target)) {
    if (!analysis_.type_is_long_string(source_type)) {
      out = "::rt::tp2cc_ansistring_of(" + out + ")";
    }
  }
  return out;
}

std::optional<std::string> EmitValues::maybe_convert_const_int_expr(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  if (!target) return std::nullopt;
  // Named constants and intrinsics already have resolved Pascal types and
  // target-correct lowerings. Replacing them with evaluator literals would
  // discard declaration qualification and enum/subrange result identity.
  if (e.kind == Kind::Ident) return std::nullopt;
  if (e.kind == Kind::Call) {
    const auto& call = static_cast<const Call&>(e);
    if (call.callee && analysis_.intrinsic_call_name(*call.callee)) {
      return std::nullopt;
    }
  }
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
  if (!mem.base) return false;
  const TypeSymbol* metaclass =
      analysis_.metaclass_target_symbol(metaclass_value_base_type(*mem.base));
  const ClassInfo* class_info = metaclass ? metaclass->class_info() : nullptr;
  if (!class_info) return false;
  const auto* methods = registry_.lookup_class_methods(*class_info, mem.name);
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
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  if (!target) return std::nullopt;
  const TypeExpr* canon = analysis_.semantic_shape_type(target);
  if (!(canon && canon->kind == Kind::TyProcedural)) return std::nullopt;
  const auto& proc = static_cast<const TyProcedural&>(*canon);

  if (proc.is_method && e.kind == Kind::NilLit) {
    return types_.type_to_cxx(*target) + "{}";
  }

  if (!proc.is_method) {
    if (reject_metaclass_member_as_plain_proc_value(e)) return "nullptr";
    if (auto adapted = maybe_convert_plain_proc_value(e, proc,
                                                      explicit_conversion)) {
      return *adapted;
    }

    switch (e.kind) {
      case Kind::Ident:
      case Kind::Member:
        if (const TypeExpr* source = overload_types_.type_for_overload(e);
            storage_.type_is_pointerish(
                analysis_.semantic_shape_type(source ? source
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

  if (auto method = maybe_convert_tmethod_value(e, target, proc,
                                                explicit_conversion)) {
    return *method;
  }

  // Picker and emitter share `resolve_method_value_binding` so they agree
  // by construction on which method a `@expr` resolves to. If we get a
  // binding, format the `tp2cc_MethodPtr` constructor; otherwise nullopt
  // and the caller falls back to plain expr_to_cxx.
  auto bind =
      resolution_.resolve_method_value_binding(e, proc, explicit_conversion);
  if (!bind || !bind->has_matching_decl()) return std::nullopt;
  const ast::ProcDecl& method_decl = bind->matching_decl();
  if (!bind->owner_symbol) {
    expr_ops_.report_error(e.loc, "unresolved method owner `" +
                                      bind->class_name + "`");
    return types_.type_to_cxx(*target) + "{}";
  }

  const std::string target_cxx = types_.type_to_cxx(*target);
  const std::string code_text = "::rt::tp2cc_method_code<&" +
                                types_.type_symbol_struct_cxx(*bind->owner_symbol) +
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

const TypeExpr* EmitValues::proc_value_source_type(const Expr& e) {
  if (const TypeExpr* t = analysis_.explicit_typecast_result_type(e)) {
    return t;
  }
  if (const TypeExpr* t = overload_types_.type_for_overload(e)) {
    return t;
  }
  return analysis_.deduce_type(e);
}

bool EmitValues::source_is_runtime_tmethod(const TypeExpr* source_type) {
  const TypeDescriptor* descriptor =
      registry_.descriptor_for_type(source_type);
  return descriptor && descriptor->is_method_carrier;
}

std::optional<std::string> EmitValues::maybe_convert_tmethod_value(
    const Expr& e, const TypeExpr* target, const TyProcedural& proc,
    bool explicit_conversion) {
  if (!explicit_conversion || !proc.is_method) return std::nullopt;
  if (!source_is_runtime_tmethod(proc_value_source_type(e))) {
    return std::nullopt;
  }

  // `TMethod` is Pascal's standard Code/Data carrier for method values.
  // An explicit cast from that carrier to `procedure/function ... of object`
  // constructs the target method pointer from those named slots; arbitrary
  // same-layout records still go through reject_method_pointer_record_cast.
  return "::rt::tp2cc_method_ptr_from_tmethod<" + types_.type_to_cxx(*target) +
         ">(" + expr_ops_.expr_to_cxx(e) + ")";
}

std::optional<std::string> EmitValues::reject_method_pointer_record_cast(
    const Expr& e, const TypeExpr* target, const TyProcedural& proc,
    const TypeExpr* source_type, bool explicit_conversion) {
  if (!explicit_conversion || !proc.is_method || !source_type) {
    return std::nullopt;
  }
  const TypeExpr* source = analysis_.semantic_shape_type(source_type);
  if (!(source && source->kind == Kind::TyRecord)) return std::nullopt;

  if (!record_has_pointer_field(analysis_, storage_, source, "proc") ||
      !record_has_pointer_field(analysis_, storage_, source, "obj")) {
    return std::nullopt;
  }

  // Do not infer procedure-of-object semantics from an arbitrary record layout.
  // `TMethod` is the Pascal representation whose Code/Data fields explicitly
  // describe a method-pointer carrier.
  expr_ops_.report_error(
      e.loc, "cannot cast record to procedure-of-object value; use TMethod");
  return types_.type_to_cxx(*target) + "{}";
}

std::optional<std::string> EmitValues::maybe_convert_plain_proc_value(
    const Expr& e, const TyProcedural& proc, bool explicit_conversion) {
  auto bind =
      resolution_.resolve_plain_proc_value_binding(e, proc, explicit_conversion);
  if (!bind || !bind->decl) return std::nullopt;
  const ProcDecl& decl = *bind->decl;
  // Resolution has already accepted the Pascal procedural type. This comparison
  // is only the backend thunk decision: by-value pointer-like Pascal formals
  // erase to `void*` in procvar carriers, so an otherwise exact routine may
  // still need an adapter before it can be stored as that C++ function pointer.
  const std::string source_sig = types_.formal_param_types_to_cxx(decl.params);
  const std::string target_sig = types_.procedural_param_types_to_cxx(proc.params);
  if (source_sig == target_sig) return expr_ops_.expr_to_cxx_no_autocall(e);
  return plain_proc_adapter_value(e, proc, decl);
}

std::string EmitValues::plain_proc_callee_text(const Expr& e) {
  return expr_ops_.expr_to_cxx_no_autocall(*strip_single_addr_of(e));
}

std::string EmitValues::plain_proc_adapter_value(const Expr& e,
                                                 const TyProcedural& proc,
                                                 const ProcDecl& decl) {
  const std::string ret =
      proc.is_function ? types_.type_to_cxx(*proc.return_type) : "void";
  const auto target_params = flatten_proc_params_with_names(proc.params);
  const auto source_params = flatten_proc_params_with_names(decl.params);
  if (target_params.size() != source_params.size()) {
    expr_ops_.report_error(e.loc,
                           "internal error: procedural adapter arity mismatch");
    return "nullptr";
  }
  std::string lambda_params;
  std::string call_args;
  for (size_t i = 0; i < target_params.size(); ++i) {
    const FlatProcParamUse& target = target_params[i];
    const FlatProcParamUse& source = source_params[i];
    if (i) {
      lambda_params += ", ";
      call_args += ", ";
    }
    std::string carrier = types_.procedural_param_type_to_cxx(*target.param);
    lambda_params += carrier + " " + target.name;
    if (types_.procedural_param_uses_pointer_carrier(*target.param) &&
        types_.procedural_param_needs_pointer_carrier_restore(*source.param) &&
        source.param->type) {
      // Resolution has already proven that the source and target procedural
      // signatures are compatible through the erased pointer carrier. The
      // adapter must restore the actual callee formal from the source Pascal
      // type identity; comparing generated C++ spellings would make aliases and
      // carrier-equivalent types depend on backend naming.
      std::string actual = types_.type_to_cxx(*source.param->type);
      call_args += "static_cast<" + actual + ">(" + target.name + ")";
    } else {
      call_args += target.name;
    }
  }

  std::string out = "+[](" + lambda_params + ") -> " + ret + " { ";
  if (proc.is_function) out += "return ";
  out += plain_proc_callee_text(e) + "(" + call_args + ");";
  out += " }";
  return out;
}

std::optional<std::string> EmitValues::maybe_lower_metaclass_value(
    const Expr& e, const TypeExpr* target) {
  const TypeSymbol* concrete =
      analysis_.concrete_class_symbol_for_metaclass_target(e, target);
  if (!concrete) {
    const TypeExpr* shape = analysis_.semantic_shape_type(target);
    concrete =
        analysis_.concrete_class_symbol_for_metaclass_target(e, shape);
  }
  if (!concrete) return std::nullopt;
  return types_.metaclass_value_fn_cxx(*concrete) + "()";
}

}  // namespace tp2cc
