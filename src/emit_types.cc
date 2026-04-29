#include "emit_types.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>

#include "emit_support.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitTypes::EmitTypes(const TypeRegistry* registry, ScopeStateView& scope,
                     EmitAnalysis& analysis,
                     EmitTypeConstRender& const_render)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      const_render_(const_render) {}

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
  // The runtime_named_type_map shortcut is only a fallback for stub units. A
  // translated user unit's own declaration must win over an rt-side stub of
  // the same name, so we ask the translated registry first and only fall back
  // to `::rt::...` when no translated declaration exists.
  if (registry_knows_type(n.name)) {
    auto lookup_name = ascii_lower(n.name);
    if (registry_) {
      auto cit = registry_->classes.find(std::string(lookup_name));
      return cit != registry_->classes.end() && cit->second.is_reference_type
                 ? named_type_struct_cxx(n.name) + "*"
                 : named_type_struct_cxx(n.name);
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
  if (!registry_knows_type(name)) {
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
           mangle(name.substr(dot + 1));
  }
  return visible_type_prefix(name) + mangle(name);
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

bool EmitTypes::registry_knows_type(std::string_view name) {
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
      auto uit = registry_->units.find(*it);
      if (uit == registry_->units.end()) continue;
      if (uit->second.has_export_type(lower)) return true;
    }
  }
  return registry_->classes.count(lower) || registry_->records.count(lower) ||
         registry_->enums.count(lower) || registry_->aliases.count(lower);
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
  return prefix + "tp2cc_metaclass_" + mangle(tail);
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
  return prefix + "tp2cc_metaclass_value_" + mangle(tail);
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

std::string EmitTypes::subrange_type_to_cxx(const TySubrange& r) {
  // If both bounds are enum members of the same enum, preserve that enum type
  // so set/array element typing stays faithful instead of collapsing to int.
  auto bound_enum = [&](const Expr* e) -> std::string {
    if (!e || e->kind != Kind::Ident) return {};
    const std::string member = static_cast<const Ident&>(*e).name;
    for (const auto& [enum_name, en] : scope_.local_enums) {
      for (const auto& em : en->members) {
        if (em.name == member) return mangle(enum_name);
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
    return prefix + mangle(info->name);
  };
  std::string le = bound_enum(r.lo.get());
  std::string he = bound_enum(r.hi.get());
  if (!le.empty() && le == he) return le;
  // Without further info we can only represent the subrange as its base type.
  return "int32_t";
}

std::string EmitTypes::string_type_to_cxx(const TyString& s) {
  if (s.max_length) {
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
  if (s.max_length) return const_render_.const_value_to_cxx(*s.max_length);
  return std::string("255");
}

std::string EmitTypes::pointer_type_to_cxx(const TyPointer& p) {
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
      // If we cannot compute the bounds statically, this is still a semantic
      // degradation and should eventually become a hard error rather than a
      // pointer fallback.
      return type_to_cxx(*a.element) + "*";
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
    } else {
      pt = type_to_cxx(*pp.type);
    }
    if (pp.type) {
      if (pp.mode == Param::Var || pp.mode == Param::Out) pt += "&";
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
      entry.mangled_name = mangle(fn);
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
      return "const " +
             metaclass_struct_cxx(static_cast<const TyMetaclass&>(t).class_name) +
             "*";
    case Kind::TyFile: {
      const auto& tf = static_cast<const TyFile&>(t);
      if (tf.is_text || !tf.element) return "::rt::tp2cc_TextFile";
      return "::rt::tp2cc_TypedFile<" + type_to_cxx(*tf.element) + ">";
    }
    case Kind::TyRecord: {
      // Inline anonymous records are emitted as inline C++ anonymous structs.
      // Variant cases stay a loud stub until inline-variant lowering is real.
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
      // Inline anonymous objects need base-class context the type-expression
      // position cannot currently carry.
      return "/* inline-object */ int32_t";
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
