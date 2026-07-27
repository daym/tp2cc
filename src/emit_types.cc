#include "emit_types.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

std::string record_layout_add_size(const std::string& left,
                                   const std::string& right) {
  if (left == "0") return right;
  if (right == "0") return left;
  return "(" + left + " + " + right + ")";
}

std::string record_layout_add_offset(const std::string& base,
                                     const std::string& delta) {
  if (delta == "0") return base;
  return record_layout_add_size(base, delta);
}

std::string record_layout_max_size(const std::vector<std::string>& sizes) {
  if (sizes.empty()) return "0";
  if (sizes.size() == 1) return sizes.front();
  std::string out = "::std::max({";
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (i) out += ", ";
    out += "static_cast<::std::size_t>(" + sizes[i] + ")";
  }
  out += "})";
  return out;
}

std::string ordinal_value_text(std::string text) {
  return "::rt::tp2cc_ordinal_value(" + text + ")";
}

bool is_single_char_string_literal(const Expr* e) {
  return e && e->kind == Kind::StringLit &&
         static_cast<const StringLit&>(*e).value.size() == 1;
}

bool array_bound_is_plain_integer_const_syntax(const TypeRegistry& registry,
                                               const Expr& e) {
  switch (e.kind) {
    case Kind::IntLit:
    case Kind::Ident:
    case Kind::Member:
      return true;
    case Kind::Unary:
      return array_bound_is_plain_integer_const_syntax(registry,
          *static_cast<const Unary&>(e).operand);
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      return array_bound_is_plain_integer_const_syntax(registry, *b.lhs) &&
             array_bound_is_plain_integer_const_syntax(registry, *b.rhs);
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      if (!c.callee || c.args.size() != 1 || !c.args[0]) {
        return false;
      }
      const TypeSymbol* symbol = nullptr;
      if (std::optional<const TypeSymbol*> type_callee =
              registry.type_name_expression_result(c.callee.get())) {
        symbol = *type_callee;
      }
      return symbol && symbol->descriptor && symbol->descriptor->primitive &&
             array_bound_is_plain_integer_const_syntax(registry, *c.args[0]);
    }
    default:
      return false;
  }
}

const TypeLookupContext* declaration_context_for_type(
    const TypeRegistry& registry, const TypeExpr* primary,
    const TypeExpr* fallback = nullptr) {
  if (primary) {
    if (const TypeLookupContext* context =
            registry.lookup_context_for_type(primary)) {
      return context;
    }
  }
  if (fallback) {
    if (const TypeLookupContext* context =
            registry.lookup_context_for_type(fallback)) {
      return context;
    }
  }
  return nullptr;
}

}  // namespace

class EmitTypes::RecordLayoutBuilder {
 public:
  RecordLayoutBuilder(EmitTypes& types, bool is_packed)
      : types_(types), is_packed_(is_packed) {
    inline_text_ = "struct ";
    if (is_packed_) inline_text_ += "[[gnu::packed]] ";
    inline_text_ += "{ ";
  }

  void append_root_fields(const std::vector<RecordField>& fields) {
    append_fields(fields, "0", size_expr_);
  }

  void append_root_variant_part(
      const std::shared_ptr<ast::VariantPart>& vpart) {
    const std::string variant_base = size_expr_;
    size_expr_ = record_layout_add_size(
        size_expr_, append_variant_part(vpart, variant_base));
  }

  CxxRecordLayout finish() && {
    inline_text_ += "}";
    return {std::move(decl_lines_), std::move(inline_text_),
            EmitPackedRecordLayout(std::move(field_offsets_),
                                   std::move(size_expr_))};
  }

 private:
  void append_field(const EmitRecordFieldDecl& field,
                    const std::string& base_offset, std::string& size_expr) {
    decl_lines_.push_back(field.decl + ";");
    inline_text_ += field.type_cxx + " " + field.mangled_name + "; ";

    field_offsets_.emplace_back(
        field.mangled_name, record_layout_add_offset(base_offset, size_expr));
    size_expr = record_layout_add_size(size_expr,
                                       "sizeof(" + field.type_cxx + ")");
  }

  void append_fields(const std::vector<RecordField>& fields,
                     const std::string& base_offset,
                     std::string& size_expr) {
    for (const auto& field : types_.record_field_decls(fields)) {
      append_field(field, base_offset, size_expr);
    }
  }

  std::string append_variant_part(
      const std::shared_ptr<ast::VariantPart>& vpart,
      const std::string& base_offset) {
    if (!vpart) return "0";
    std::string prefix_size = "0";
    if (!vpart->tag_name.empty() && vpart->tag_type) {
      append_field(types_.record_field_decl(vpart->tag_type.get(),
                                            vpart->tag_name),
                   base_offset, prefix_size);
    }

    decl_lines_.push_back("union {");
    inline_text_ += "union { ";

    std::vector<std::string> case_sizes;
    const std::string union_offset =
        record_layout_add_offset(base_offset, prefix_size);
    for (const auto& vc : vpart->cases) {
      if (vc.fields.empty() && !vc.variant_part) continue;

      std::string case_open = "struct ";
      if (is_packed_) case_open += "[[gnu::packed]] ";

      decl_lines_.push_back(case_open + "{");
      inline_text_ += case_open + "{ ";

      std::string case_size_expr = "0";
      append_fields(vc.fields, union_offset, case_size_expr);

      if (vc.variant_part) {
        const std::string nested_base =
            record_layout_add_offset(union_offset, case_size_expr);
        case_size_expr = record_layout_add_size(
            case_size_expr, append_variant_part(vc.variant_part, nested_base));
      }

      decl_lines_.push_back("};");
      inline_text_ += "}; ";

      case_sizes.push_back(case_size_expr);
    }

    decl_lines_.push_back("};");
    inline_text_ += "}; ";
    return record_layout_add_size(prefix_size,
                                  record_layout_max_size(case_sizes));
  }

  EmitTypes& types_;
  bool is_packed_;
  std::vector<std::string> decl_lines_;
  std::string inline_text_;
  std::vector<std::pair<std::string, std::string>> field_offsets_;
  std::string size_expr_ = "0";
};

EmitTypes::EmitTypes(const TypeRegistry& registry, ScopeStateView& scope,
                     EmitAnalysis& analysis,
                     EmitTypeConstRender& const_render,
                     EmitTypeOrdinalOps& ordinal_ops,
                     EmitTypeDiagOps& diag_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      const_render_(const_render),
      ordinal_ops_(ordinal_ops),
      diag_ops_(diag_ops) {}

std::string EmitTypes::type_name_to_cxx(const TyName& n) {
  auto emit_symbol_type = [&](const TypeSymbol* symbol) -> std::string {
    if (!symbol) return {};
    if (std::string cxx = type_symbol_to_cxx(symbol); !cxx.empty()) {
      return cxx;
    }
    if (symbol->defining_unit == "__rt__" ||
        symbol->defining_unit == "__builtin__") {
      diag_ops_.report_error(n.loc, "unresolved type `" + n.name + "`");
      return "void";
    }
    return {};
  };

  if (std::string cxx = emit_symbol_type(registry_.resolved_symbol_for_type(&n));
      !cxx.empty()) {
    return cxx;
  }
  // The semantic model owns Pascal type binding. Accepting raw spelling here would make
  // this renderer a second type lookup path and hide missing binding.
  diag_ops_.report_error(n.loc, "unresolved type `" + n.name + "`");
  return "void";
}

std::string EmitTypes::type_symbol_to_cxx(const TypeSymbol* symbol) {
  if (!symbol) return {};
  const TypeDescriptor* descriptor = symbol->descriptor;
  const TypeSymbol* canonical =
      descriptor && descriptor->symbol ? descriptor->symbol : symbol;

  if (descriptor && descriptor->primitive) {
    return descriptor->primitive->cxx;
  }
  if (const ClassInfo* ci = canonical->class_info(); ci) {
    if (ci->is_reference_type) {
      return type_symbol_struct_cxx(*canonical) + "*";
    }
    return type_symbol_struct_cxx(*canonical);
  }
  if (canonical->interface_info()) {
    return type_symbol_struct_cxx(*canonical) + "*";
  }
  if (canonical->record_info() || canonical->enum_info()) {
    return type_symbol_struct_cxx(*canonical);
  }
  if (descriptor && descriptor->symbol == canonical) {
    // Backend naming is descriptor-owned: fresh named Pascal type syntax gets
    // one `t_<declaration>` C++ carrier. A Pascal alias has no descriptor
    // ownership, so it reaches this function with the target descriptor's
    // owner and can never preserve the alias spelling.
    return type_symbol_struct_cxx(*canonical);
  }
  if (descriptor && descriptor->metaclass_target) {
    return metaclass_struct_cxx(*descriptor->metaclass_target) + "*";
  }
  if (descriptor && descriptor->type) {
    return type_to_cxx(*descriptor->type);
  }
  return {};
}

std::string EmitTypes::active_emission_unit_name() const {
  return scope_.lookup_emission_unit_name.empty()
             ? scope_.current_unit_name
             : scope_.lookup_emission_unit_name;
}

bool EmitTypes::should_qualify_unit(std::string_view defining_unit) const {
  return !defining_unit.empty() && defining_unit != active_emission_unit_name();
}

std::string EmitTypes::type_symbol_struct_cxx(
    const TypeSymbol& symbol) const {
  std::string out = should_qualify_unit(symbol.defining_unit)
                        ? unit_namespace_prefix(symbol.defining_unit)
                        : std::string{};
  for (const auto& owner : symbol.owner_path) {
    out += type_mangle(owner);
    out += "::";
  }
  out += type_mangle(symbol.name);
  return out;
}

std::string EmitTypes::metaclass_symbol_cxx(
    const TypeSymbol& symbol, std::string_view name_prefix) const {
  std::string tail;
  for (const auto& owner : symbol.owner_path) {
    if (!tail.empty()) tail += "_";
    tail += type_mangle(owner);
  }
  if (!tail.empty()) tail += "_";
  tail += type_mangle(symbol.name);
  std::string prefix;
  if (should_qualify_unit(symbol.defining_unit)) {
    prefix = unit_namespace_prefix(symbol.defining_unit);
  }
  return prefix + std::string(name_prefix) + tail;
}

std::string EmitTypes::metaclass_struct_cxx(const TypeSymbol& symbol) const {
  return metaclass_symbol_cxx(symbol, "tp2cc_metaclass_");
}

std::string EmitTypes::metaclass_value_fn_cxx(
    const TypeSymbol& symbol) const {
  return metaclass_symbol_cxx(symbol, "tp2cc_metaclass_value_");
}

bool EmitTypes::enum_has_explicit_values(const TyEnum& e) {
  for (const auto& member : e.members) {
    if (member.value) return true;
  }
  return false;
}

std::optional<int64_t> EmitTypes::enum_member_value_int64(const TyEnum& e,
                                                          size_t index) {
  const EnumInfoReg* info =
      e.descriptor ? e.descriptor->enum_info() : nullptr;
  if (!info || index >= info->members.size()) return std::nullopt;
  return analysis_.enum_member_ordinal(EnumMemberInfo{info, index});
}

std::string EmitTypes::enum_member_value_to_cxx(const TyEnum& e, size_t index) {
  const TypeLookupContext* declaration_context =
      declaration_context_for_type(registry_, &e);
  // An enum value expression is emitted where the enum is declared. Rendering
  // it in a later caller's scope could bind an unqualified constant to a
  // different declaration.
  ScopedDeclarationLookup declaration_scope(
      scope_, declaration_context,
      declaration_context ? std::string_view(declaration_context->unit)
                          : std::string_view{});
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

std::optional<ArrayDimBounds> EmitTypes::array_dim_bounds_to_cxx(
    const TypeExpr& dim_in) {
  if (dim_in.kind == Kind::TyName) {
    const TypeSymbol* symbol = registry_.resolved_symbol_for_type(&dim_in);
    const EnumInfoReg* enum_info = symbol ? symbol->enum_info() : nullptr;
    if (enum_info) {
      std::string lo = "0";
      std::string size_expr;
      const TyEnum* enum_type = enum_info->type;
      if (!enum_type) return std::nullopt;
      if (!enum_info->members.empty()) {
        if (enum_has_explicit_values(*enum_type)) {
          std::string prefix;
          if (should_qualify_unit(enum_info->defining_unit)) {
            prefix = unit_namespace_prefix(enum_info->defining_unit);
          }
          lo = prefix + mangle(enum_info->members.front());
          const std::string hi = prefix + mangle(enum_info->members.back());
          size_expr = "((" + ordinal_value_text(hi) + ") - (" +
                      ordinal_value_text(lo) + ") + 1)";
        } else {
          size_expr = std::to_string(enum_info->members.size());
        }
      }
      return ArrayDimBounds(std::move(lo), std::move(size_expr));
    }
  }

  const TypeExpr* dim = analysis_.semantic_shape_type(&dim_in);
  if (!dim) return std::nullopt;
  if (dim->kind == Kind::TyDistinct) {
    // Distinct ordinal wrappers keep assignment-compatibility differences, but
    // their index range is still the underlying ordinal range.
    const auto& distinct = static_cast<const TyDistinct&>(*dim);
    return array_dim_bounds_to_cxx(*distinct.underlying);
  }
  const TypeLookupContext* declaration_context =
      declaration_context_for_type(registry_, dim, &dim_in);
  ScopedDeclarationLookup type_bound_scope(
      scope_, declaration_context,
      declaration_context ? std::string_view(declaration_context->unit)
                          : std::string_view{});
  std::string lo = "0";
  std::string size_expr;
  if (dim->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*dim);
    // C++ array template arguments need integer expressions, so fold imported
    // Pascal const arithmetic such as `max_operands-1`. Ordinal intrinsics
    // stay on `array_bound_ordinal_to_cxx` because enum/char bounds carry a
    // domain in addition to a number. Fold before rendering: rendering an
    // imported const as raw Pascal text would record an unresolved-id error.
    std::optional<ConstIntExprInfo> lo_value;
    std::optional<ConstIntExprInfo> hi_value;
    if (array_bound_is_plain_integer_const_syntax(registry_, *sr.lo)) {
      lo_value = analysis_.eval_const_int_expr(*sr.lo);
    }
    if (array_bound_is_plain_integer_const_syntax(registry_, *sr.hi)) {
      hi_value = analysis_.eval_const_int_expr(*sr.hi);
    }
    lo = lo_value ? std::to_string(lo_value->value)
                  : const_render_.const_value_to_cxx(*sr.lo);
    std::string lo_ord =
        lo_value ? lo : array_bound_ordinal_to_cxx(*sr.lo);
    std::string hi_ord =
        hi_value ? std::to_string(hi_value->value)
                 : array_bound_ordinal_to_cxx(*sr.hi);
    size_expr = "((" + hi_ord + ") - (" + lo_ord + ") + 1)";
    return ArrayDimBounds(std::move(lo), std::move(size_expr));
  }
  if (dim->kind == Kind::TyEnum) {
    const auto& en = static_cast<const TyEnum&>(*dim);
    if (!enum_has_explicit_values(en)) {
      size_expr = std::to_string(en.members.size());
    } else if (!en.members.empty()) {
      lo = enum_member_value_to_cxx(en, 0);
      std::string hi = enum_member_value_to_cxx(en, en.members.size() - 1);
      size_expr = "((" + ordinal_value_text(hi) + ") - (" +
                  ordinal_value_text(lo) + ") + 1)";
    }
    return ArrayDimBounds(std::move(lo), std::move(size_expr));
  }
  const PrimitiveInfo* info = analysis_.primitive_info_for_type(dim);
  if (!info) return std::nullopt;
  // Pascal boolean atoms share a two-value ordinal domain regardless of
  // carrier width.
  if (info->is_bool()) return ArrayDimBounds("false", "2");
  // Char and WideChar atoms occupy the full value space of their width.
  if (info->is_char()) return ArrayDimBounds(std::move(lo), "256");
  if (info->is_widechar()) return ArrayDimBounds(std::move(lo), "65536");
  // Integer atoms: derive (lo, size) from primitive metadata. The element
  // count is 2^bits, which must fit in size_t (max 2^pointer_bits - 1); a
  // width equal to or exceeding pointer_bits names an array too large to
  // address.
  if (info->int_kind == PrimitiveIntKind::None || info->pointer_sized) {
    return std::nullopt;
  }
  const uint8_t bits = analysis_.resolved_primitive_bits(*info);
  if (bits >= analysis_.target().pointer_bits) return std::nullopt;
  const uint64_t count = uint64_t{1} << bits;
  std::string size = std::to_string(count);
  if (info->int_kind == PrimitiveIntKind::Unsigned) {
    return ArrayDimBounds(std::move(lo), std::move(size));
  }
  std::string low =
      "::std::numeric_limits<" + std::string(info->cxx) + ">::min()";
  return ArrayDimBounds(std::move(low), std::move(size));
}

std::string EmitTypes::array_bound_ordinal_to_cxx(const Expr& e) {
  return ordinal_ops_.ordinal_value_to_cxx(
      e, const_render_.const_value_to_cxx(e));
}

const TypeExpr* EmitTypes::subrange_bound_source_type(const Expr& e) {
  // Subrange declarations use each bound expression's own Pascal type to
  // choose char/enum/integer storage. There is no surrounding target type to
  // apply and no addressable storage slot involved.
  return analysis_.deduce_type(e);
}

const TypeExpr* EmitTypes::subrange_bound_canonical_type(const Expr* e) {
  if (!e) return nullptr;
  const TypeExpr* t = subrange_bound_source_type(*e);
  return t ? analysis_.semantic_shape_type(t) : nullptr;
}

std::string EmitTypes::visible_enum_type_for_member(std::string_view name) {
  if (const EnumInfoReg* info = local_enum_info_for_member(scope_, name)) {
    return info->cxx_name;
  }
  assert(pascal_key_is_canonical(name));
  const std::string member(name);
  const auto* info = analysis_.find_visible_enum_info_for_member(member);
  if (!info) return {};
  std::string prefix;
  if (should_qualify_unit(info->defining_unit)) {
    prefix = unit_namespace_prefix(info->defining_unit);
  }
  return prefix + info->cxx_name;
}

std::string EmitTypes::visible_enum_type_for_type_symbol(
    const TypeSymbol* symbol) {
  const TypeSymbol* canonical =
      symbol && symbol->descriptor && symbol->descriptor->symbol
          ? symbol->descriptor->symbol
          : symbol;
  if (!canonical || !canonical->enum_info()) return {};
  return type_symbol_to_cxx(symbol);
}

std::string EmitTypes::subrange_bound_enum_cxx_type(const Expr* e) {
  if (!e) return {};
  if (e->kind == Kind::Ident) {
    return visible_enum_type_for_member(static_cast<const Ident&>(*e).name);
  }
  if (e->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*e);
    if (auto unit_member = analysis_.resolve_unit_qualified_member(mem);
        unit_member &&
        unit_member->resolved.kind == ResolvedKind::EnumMember) {
      if (const EnumInfoReg* info =
              analysis_.find_enum_info_in_unit(unit_member->unit_name,
                                               unit_member->member_name)) {
        return unit_namespace_prefix(unit_member->unit_name) +
               type_mangle(info->name);
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
    const std::string& callee = static_cast<const Ident&>(*call.callee).name;
    if (callee == "low" || callee == "high") {
      if (std::optional<const TypeSymbol*> symbol =
              registry_.type_name_expression_result(call.args[0].get())) {
        return visible_enum_type_for_type_symbol(*symbol);
      }
      return {};
    }
    if (callee == "pred" || callee == "succ") {
      return subrange_bound_enum_cxx_type(call.args[0].get());
    }
  }
  return {};
}

std::string EmitTypes::subrange_type_to_cxx(const TySubrange& r) {
  auto primitive_cxx = [&](PrimitiveKind kind) -> std::string {
    for (const PrimitiveInfo& primitive : registry_.primitive_info_storage) {
      if (primitive.kind == kind && primitive.cxx) return primitive.cxx;
    }
    return {};
  };
  auto ordinal_cxx = [&](PrimitiveIntKind kind, uint8_t bits) -> std::string {
    const PrimitiveInfo* primitive =
        ordinal_integer_primitive(registry_, kind, bits);
    return primitive && primitive->cxx ? primitive->cxx : std::string{};
  };

  // If both bounds denote values from the same enum, preserve that enum carrier
  // so scalar subranges remain assignment-compatible with the parent enum.
  std::string le = subrange_bound_enum_cxx_type(r.lo.get());
  std::string he = subrange_bound_enum_cxx_type(r.hi.get());
  if (!le.empty() && le == he) return le;

  const TypeExpr* lo_type = subrange_bound_canonical_type(r.lo.get());
  const TypeExpr* hi_type = subrange_bound_canonical_type(r.hi.get());
  const PrimitiveInfo* lo_info = analysis_.primitive_info_for_type(lo_type);
  const PrimitiveInfo* hi_info = analysis_.primitive_info_for_type(hi_type);
  if (((lo_info && lo_info->is_char()) &&
       (hi_info && hi_info->is_char())) ||
      (is_single_char_string_literal(r.lo.get()) &&
       is_single_char_string_literal(r.hi.get()))) {
    return primitive_cxx(PrimitiveKind::Char);
  }
  if (lo_info && hi_info && lo_info->kind == PrimitiveKind::WideChar &&
      hi_info->kind == PrimitiveKind::WideChar) {
    return primitive_cxx(PrimitiveKind::WideChar);
  }

  // Subrange bounds belong to the type declaration's lexical context. Use the
  // analysis domain query here so C++ carrier selection agrees with overload
  // checks and set-literal conversion for imported aliases such as
  // `set of 0..MaxRegister`.
  auto domain = analysis_.ordinal_domain_for_type(&r);
  if (!domain) return "int32_t";

  if (domain->family == OrdinalFamily::Boolean) {
    return primitive_cxx(PrimitiveKind::Boolean);
  }
  if (domain->family == OrdinalFamily::Char) {
    return primitive_cxx(PrimitiveKind::Char);
  }
  if (domain->family == OrdinalFamily::WideChar) {
    return primitive_cxx(PrimitiveKind::WideChar);
  }
  if (domain->family != OrdinalFamily::Integer) return "int32_t";

  int64_t lo = domain->low;
  int64_t hi = domain->high;

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
    if (uhi <= UINT8_MAX)
      return ordinal_cxx(PrimitiveIntKind::Unsigned, 8);
    if (uhi <= UINT16_MAX)
      return ordinal_cxx(PrimitiveIntKind::Unsigned, 16);
    if (uhi <= UINT32_MAX)
      return ordinal_cxx(PrimitiveIntKind::Unsigned, 32);
    return ordinal_cxx(PrimitiveIntKind::Unsigned, 64);
  }
  if (lo >= INT8_MIN && hi <= INT8_MAX)
    return ordinal_cxx(PrimitiveIntKind::Signed, 8);
  if (lo >= INT16_MIN && hi <= INT16_MAX) {
    return ordinal_cxx(PrimitiveIntKind::Signed, 16);
  }
  if (lo >= INT32_MIN && hi <= INT32_MAX)
    return ordinal_cxx(PrimitiveIntKind::Signed, 32);
  return ordinal_cxx(PrimitiveIntKind::Signed, 64);
}

std::string EmitTypes::enum_bound_cxx_name(std::string_view enum_name,
                                           std::string_view defining_unit,
                                           bool want_low) {
  std::string bound = enum_bound_name(enum_name, want_low ? "low" : "high");
  if (!should_qualify_unit(defining_unit)) {
    return bound;
  }
  return unit_namespace_prefix(defining_unit) + bound;
}

std::string EmitTypes::string_type_to_cxx(const TyString& s) {
  const TypeLookupContext* declaration_context =
      declaration_context_for_type(registry_, &s);
  ScopedDeclarationLookup type_bound_scope(
      scope_, declaration_context,
      declaration_context ? std::string_view(declaration_context->unit)
                          : std::string_view{});
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
  if (const PrimitiveInfo* primitive = analysis_.primitive_info_for_type(t);
      primitive && primitive->kind == PrimitiveKind::ShortString) {
    return std::string("255");
  }
  const TypeExpr* canon = analysis_.semantic_shape_type(t);
  if (!(canon && canon->kind == Kind::TyString)) return std::nullopt;
  const TypeLookupContext* declaration_context =
      declaration_context_for_type(registry_, canon, t);
  ScopedDeclarationLookup type_bound_scope(
      scope_, declaration_context,
      declaration_context ? std::string_view(declaration_context->unit)
                          : std::string_view{});
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

std::optional<std::string> EmitTypes::enum_carrier_type_to_cxx(
    const TyEnum& e) {
  const EnumInfoReg* info = registry_.enum_info_for_type(&e);
  if (!info || info->cxx_name.empty()) return std::nullopt;
  if (info->defining_unit == "__rt__") {
    return std::string("::rt::") + info->cxx_name;
  }
  std::string prefix;
  if (should_qualify_unit(info->defining_unit)) {
    prefix = unit_namespace_prefix(info->defining_unit);
  }
  return prefix + info->cxx_name;
}

std::string EmitTypes::enum_type_to_cxx(const TyEnum& e,
                                        const std::string&) {
  if (auto carrier = enum_carrier_type_to_cxx(e)) return *carrier;
  if (e.members.empty()) return "int32_t";
  std::string out = "enum : ";
  out += enum_underlying_type_to_cxx(e);
  out += " { ";
  auto members = enum_members_to_cxx(e);
  for (size_t i = 0; i < members.size(); ++i) {
    if (i) out += ", ";
    out += members[i];
  }
  out += " }";
  return out;
}

std::vector<std::string> EmitTypes::enum_members_to_cxx(const TyEnum& e) {
  std::vector<std::string> out;
  for (size_t i = 0; i < e.members.size(); ++i) {
    std::string m = mangle(e.members[i].name);
    if (e.members[i].value) {
      m += " = " + enum_member_value_to_cxx(e, i);
    }
    out.push_back(std::move(m));
  }
  return out;
}

std::string EmitTypes::open_array_type_to_cxx(const TypeExpr& t) {
  const TypeExpr* canon = analysis_.semantic_shape_type(&t);
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
    auto bounds = array_dim_bounds_to_cxx(**it);
    if (!bounds) {
      // Keep unsupported fixed arrays as array wrappers so later emit paths do not
      // silently change aliasing/value semantics to pointer semantics.
      diag_ops_.report_error(
          a.loc,
          "unsupported fixed array index type; exact Pascal bounds are required");
      return "::rt::tp2cc_Array<" + ty + ", 0, 1>";
    }
    ty = "::rt::tp2cc_Array<" + ty + ", " + bounds->low + ", " +
         bounds->size_expr + ">";
  }
  return ty;
}

std::string EmitTypes::formal_param_type_to_cxx(const Param& pp) {
  std::string pt;
  if (!pp.type) {
    pt = (pp.mode == Param::Const || pp.mode == Param::ConstRef)
             ? "const void*"
             : "void*";
  } else if (const TypeExpr* canon = analysis_.semantic_shape_type(pp.type.get());
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
    } else if (pp.mode == Param::ConstRef) {
      pt = "const " + pt + "&";
    } else if (pp.mode == Param::Var || pp.mode == Param::Out) {
      pt += "&";
    } else if (pp.mode == Param::Const) {
      if (analysis_.const_param_needs_mutable_ref(pp.type.get())) pt += "&";
      else if (analysis_.const_param_needs_const_ref(pp.type.get()))
        pt = "const " + pt + "&";
    }
  }
  return pt;
}

std::string EmitTypes::formal_param_types_to_cxx(
    const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& pp : params) {
    std::string pt = formal_param_type_to_cxx(pp);
    size_t repeats = pp.names.empty() ? 1 : pp.names.size();
    for (size_t i = 0; i < repeats; ++i) {
      if (!first) out += ", ";
      first = false;
      out += pt;
    }
  }
  return out;
}

bool EmitTypes::procedural_param_uses_pointer_carrier(const Param& pp) {
  if (!pp.type) return false;
  if (pp.mode == Param::Var || pp.mode == Param::Out ||
      pp.mode == Param::ConstRef) {
    return false;
  }
  if (pp.mode == Param::Const &&
      (analysis_.const_param_needs_mutable_ref(pp.type.get()) ||
       analysis_.const_param_needs_const_ref(pp.type.get()))) {
    return false;
  }

  const TypeExpr* canon = analysis_.semantic_shape_type(pp.type.get());
  if (!canon) return false;
  if (canon->kind == Kind::TyProcedural) return false;
  if (canon->kind == Kind::TyPointer || canon->kind == Kind::TyMetaclass) {
    return true;
  }
  if (analysis_.type_is_reference_class(canon) ||
      analysis_.type_is_interface(canon)) {
    return true;
  }
  if (canon->kind == Kind::TyName) {
    const PrimitiveInfo* pi = analysis_.primitive_info_for_type(canon);
    return pi && pi->is_pointer_primitive();
  }
  return false;
}

bool EmitTypes::procedural_param_uses_plain_pointer_carrier(const Param& pp) {
  if (!procedural_param_uses_pointer_carrier(pp) || !pp.type) return false;
  if (const PrimitiveInfo* primitive =
          analysis_.primitive_info_for_type(pp.type.get());
      primitive && primitive->kind == PrimitiveKind::Pointer) {
    return true;
  }
  const TypeExpr* shape = analysis_.semantic_shape_type(pp.type.get());
  return shape && shape->kind == Kind::TyPointer &&
         !static_cast<const TyPointer&>(*shape).target;
}

bool EmitTypes::procedural_param_needs_pointer_carrier_restore(
    const Param& pp) {
  if (!procedural_param_uses_pointer_carrier(pp)) return false;
  return !procedural_param_uses_plain_pointer_carrier(pp);
}

std::string EmitTypes::procedural_param_type_to_cxx(const Param& pp) {
  // Procvar calls are indirect C++ calls. By-value Pascal pointer-like slots
  // use `void*` as the carrier so compatible callback casts such as
  // `procedure(TObject; Pointer)` <-> `procedure(Pointer; Pointer)` do not call
  // through mismatched C++ function types. The real Pascal type is restored in
  // the generated adapter/thunk before calling the actual routine.
  if (procedural_param_uses_pointer_carrier(pp)) return "void*";
  return formal_param_type_to_cxx(pp);
}

std::string EmitTypes::procedural_param_types_to_cxx(
    const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& pp : params) {
    std::string pt = procedural_param_type_to_cxx(pp);
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
  // still be an ordinary C++ identifier with no reserved prefix.
  std::string out = "tp2cc_methodptr_";
  out += encode_helper_ident(pd.name);
  out += "_";
  out += encode_helper_params(registry_, pd.params);
  out += "_ret_";
  out += (pd.pkind == ProcKind::Function && pd.return_type)
             ? encode_helper_type(registry_, *pd.return_type)
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

CxxRecordLayout EmitTypes::compute_record_layout(const TyRecord& tr) {
  // Packed-record offset assertions must be computed from the same traversal
  // that prints the C++ fields; variant-record nesting changes both products.
  RecordLayoutBuilder builder(*this, tr.is_packed);
  builder.append_root_fields(tr.fields);
  builder.append_root_variant_part(tr.variant_part);
  return std::move(builder).finish();
}

EmitRecordFieldDecl EmitTypes::record_field_decl(const TypeExpr* type,
                                                 std::string_view name) {
  std::string type_cxx = type ? type_to_cxx(*type) : std::string("int32_t");
  std::string mangled_name =
      registry_.field_cxx_name(name);
  return EmitRecordFieldDecl(type, type_cxx, mangled_name,
                             named_type_to_cxx(type, mangled_name));
}

std::vector<EmitRecordFieldDecl> EmitTypes::record_field_decls(
    const std::vector<RecordField>& fields) {
  std::vector<EmitRecordFieldDecl> out;
  for (const auto& f : fields) {
    for (const auto& fn : f.names) {
      out.push_back(record_field_decl(f.type.get(), fn));
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
      if (const TypeSymbol* target = registry_.metaclass_target_for_type(&t)) {
        return metaclass_struct_cxx(*target) + "*";
      }
      diag_ops_.report_error(
          t.loc, "unresolved metaclass target `" +
                     static_cast<const TyMetaclass&>(t).class_name + "`");
      return "::rt::t_tclass";
    case Kind::TyFile: {
      const auto& tf = static_cast<const TyFile&>(t);
      if (tf.is_text || !tf.element) return "::rt::tp2cc_TextFile";
      return "::rt::tp2cc_TypedFile<" + type_to_cxx(*tf.element) + ">";
    }
    case Kind::TyRecord: {
      // Inline anonymous records are emitted as inline C++ anonymous structs.
      return compute_record_layout(static_cast<const TyRecord&>(t)).inline_text;
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

std::string EmitTypes::low_high_expr_for_type_symbol(
    const TypeSymbol* symbol, bool want_low) {
  const TypeSymbol* canonical =
      symbol && symbol->descriptor && symbol->descriptor->symbol
          ? symbol->descriptor->symbol
          : symbol;
  if (canonical && canonical->enum_info()) {
    return enum_bound_cxx_name(canonical->name, canonical->defining_unit,
                               want_low);
  }
  if (const TypeExpr* type =
          symbol && symbol->descriptor ? symbol->descriptor->type : nullptr) {
    return low_high_expr_for_type(type, want_low);
  }
  return {};
}

std::string EmitTypes::low_high_expr_for_type(const TypeExpr* t,
                                              bool want_low) {
  return low_high_expr_for_type_in_context(
      t, want_low, declaration_context_for_type(registry_, t));
}

std::string EmitTypes::low_high_expr_for_type_in_context(
    const TypeExpr* t, bool want_low,
    const TypeLookupContext* declaration_context) {
  if (!t) return {};
  if (const TypeLookupContext* own_context =
          declaration_context_for_type(registry_, t)) {
    declaration_context = own_context;
  }
  if (t->kind == Kind::TyName) {
    if (const TypeSymbol* symbol = registry_.resolved_symbol_for_type(t)) {
      const TypeSymbol* canonical = symbol;
      if (symbol->descriptor && symbol->descriptor->primitive) {
        if (std::string primitive =
                primitive_low_high_expr(symbol->descriptor->primitive, want_low);
            !primitive.empty()) {
          return primitive;
        }
      }
      if (canonical && canonical->enum_info()) {
        return enum_bound_cxx_name(canonical->name, canonical->defining_unit,
                                   want_low);
      }
      if (const TypeExpr* payload =
              symbol->descriptor ? symbol->descriptor->type : nullptr;
          payload && payload != t) {
        const TypeLookupContext* payload_context = declaration_context;
        // The TyName node's own context is the use site. Bounds inside the
        // resolved payload type belong to the declaration that created it.
        if (const TypeLookupContext* symbol_context =
                declaration_context_for_type(registry_,
                                             canonical ? canonical->type
                                                       : symbol->type)) {
          payload_context = symbol_context;
        }
        return low_high_expr_for_type_in_context(payload, want_low,
                                                 payload_context);
      }
    }
    return {};
  }
  if (t->kind == Kind::TyDistinct) {
    return low_high_expr_for_type_in_context(
        static_cast<const TyDistinct&>(*t).underlying.get(), want_low,
        declaration_context);
  }
  if (t->kind == Kind::TySubrange) {
    const auto& r = static_cast<const TySubrange&>(*t);
    // Subrange bound expressions belong to the type declaration, not to the
    // later unit or routine that asks `Low(T)` / `High(T)`.
    ScopedDeclarationLookup type_bound_scope(
        scope_, declaration_context,
        declaration_context ? std::string_view(declaration_context->unit)
                            : std::string_view{});
    return const_render_.const_value_to_cxx(want_low ? *r.lo : *r.hi);
  }
  if (t->kind == Kind::TyArray) {
    // `low(arr)` / `high(arr)` on an array type recurse into the first index
    // type. Open/dynamic arrays need a value to ask `p_length` of, so the
    // type-only path deliberately gives up there.
    const auto& arr = static_cast<const TyArray&>(*t);
    if (arr.array_kind != ArrayKind::Fixed) return {};
    if (arr.dims.empty()) return {};
    return low_high_expr_for_type_in_context(arr.dims[0].get(), want_low,
                                             declaration_context);
  }
  if (t->kind == Kind::TySet) {
    const auto& s = static_cast<const TySet&>(*t);
    if (s.has_explicit_bounds) {
      return std::to_string(want_low ? s.explicit_low : s.explicit_high);
    }
    return low_high_expr_for_type_in_context(s.element.get(), want_low,
                                             declaration_context);
  }
  return {};
}

}  // namespace tp2cc
