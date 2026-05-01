#include "emit_storage.h"

#include <unordered_set>

#include "emit_analysis.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

bool is_nonmethod_procedural_type(const TypeExpr* t) {
  if (!t) return false;
  t = static_cast<const TypeExpr*>(t);
  return t->kind == Kind::TyProcedural &&
         !static_cast<const TyProcedural&>(*t).is_method;
}

bool is_plain_pointer_type(const TypeExpr* t) {
  return t && tyname_is(t, "pointer");
}

std::string reference_class_name(const TypeExpr* t) {
  if (!t || t->kind != Kind::TyName) return {};
  return ascii_lower(static_cast<const TyName&>(*t).name);
}

}  // namespace

EmitStorage::EmitStorage(const TypeRegistry* registry, ScopeStateView& scope,
                         EmitAnalysis& analysis, EmitTypes& types,
                         ResolveNameProvider& resolve_name_provider,
                         EmitStorageExprOps& expr_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      resolve_name_provider_(resolve_name_provider),
      expr_ops_(expr_ops) {}

std::string EmitStorage::primitive_cast_lvalue_ref(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || !expr_is_storage_lvalue(*c.args[0])) return {};
  if (expr_is_untyped_storage_ref(*c.args[0])) return {};
  if (peeled->kind == Kind::Ident) {
    ResolveResult rr =
        resolve_name_provider_.resolve_name(static_cast<const Ident&>(*peeled).name);
    if (rr.kind == ResolvedKind::UnitConst || rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return {};
    }
  }
  // Pascal `T(lv)` used as an lvalue aliases the same storage with a different
  // type. Emit that reinterpretation directly.
  return reinterpret_ref_text(primitive_type_cxx(id.name),
                              expr_ops_.expr_to_cxx(*peeled), false);
}

std::string EmitStorage::primitive_cast_untyped_storage_ptr(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || !expr_is_storage_lvalue(*c.args[0]) ||
      !expr_is_untyped_storage_ref(*c.args[0])) {
    return {};
  }
  if (peeled->kind == Kind::Ident) {
    ResolveResult rr =
        resolve_name_provider_.resolve_name(static_cast<const Ident&>(*peeled).name);
    if (rr.kind == ResolvedKind::UnitConst || rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return {};
    }
  }
  return expr_ops_.expr_to_cxx(*peeled);
}

// Returns `&(field_expr)` when `c` is a primitive type-cast over a scalar
// field of a packed record. The resulting pointer text is only valid for the
// memcpy-style reinterpret helpers, never for a typed `T*` dereference.
std::string EmitStorage::primitive_cast_packed_field_ptr(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || peeled->kind != Kind::Member) return {};
  const auto& m = static_cast<const Member&>(*peeled);
  const TypeExpr* base_type = analysis_.deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return {};
  if (auto storage = bytewise_storage_ref(*peeled)) return storage->void_ptr_text;
  return {};
}

std::optional<EmitBytewiseStorage> EmitStorage::bytewise_storage_ref(
    const Expr& e) {
  const Expr* peeled = peel_primitive_casts(&e);
  const Expr& root = peeled ? *peeled : e;
  if (!expr_is_storage_lvalue(root)) return std::nullopt;

  const TypeExpr* elem_type = analysis_.deduce_type(root);
  if (!elem_type) return std::nullopt;
  const std::string elem_cxx = types_.type_to_cxx(*elem_type);

  if (root.kind == Kind::Deref) {
    const auto& d = static_cast<const Deref&>(root);
    return EmitBytewiseStorage{expr_ops_.expr_to_cxx(*d.operand), elem_cxx};
  }
  if (root.kind == Kind::Index) {
    if (auto view = untyped_storage_index_view(static_cast<const Index&>(root))) {
      return EmitBytewiseStorage{view->ptr_cxx, view->elem_cxx};
    }
  }
  return EmitBytewiseStorage{"&(" + expr_ops_.expr_to_cxx(root) + ")",
                             elem_cxx};
}

// Packed-field address plus element spelling for memcpy-style helpers.
std::optional<EmitBytewiseStorage> EmitStorage::packed_field_storage_ref(
    const Expr& e) {
  if (e.kind != Kind::Member) return std::nullopt;
  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = analysis_.deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return std::nullopt;
  return bytewise_storage_ref(e);
}

std::optional<EmitBytewiseStorage> EmitStorage::packed_scalar_storage_ref(
    const Expr& e) {
  const TypeExpr* elem_type = analysis_.canonicalize_type(analysis_.deduce_type(e));
  if (!elem_type) return std::nullopt;
  switch (elem_type->kind) {
    case Kind::TyArray:
    case Kind::TyRecord:
    case Kind::TyObject:
    case Kind::TyProcedural:
    case Kind::TySet:
    case Kind::TyString:
      return std::nullopt;
    default:
      break;
  }

  // Any scalar field whose access path crosses a packed record must stay off
  // the normal `T&` path. We still take the final subobject address, but only
  // to feed the memcpy-based reinterpret helpers.
  for (const Expr* cur = &e; cur && cur->kind == Kind::Member;
       cur = static_cast<const Member&>(*cur).base.get()) {
    const auto& m = static_cast<const Member&>(*cur);
    if (type_is_packed_record(analysis_.deduce_type(*m.base))) {
      return bytewise_storage_ref(e);
    }
  }
  return std::nullopt;
}

std::optional<EmitUntypedStorageIndexView> EmitStorage::untyped_storage_index_view(
    const Index& i) {
  if (i.indices.size() != 1 || i.base->kind != Kind::Call) return std::nullopt;
  const auto& cast = static_cast<const Call&>(*i.base);
  if (cast.args.size() != 1 || cast.callee->kind != Kind::Ident ||
      !expr_is_untyped_storage_ref(*cast.args[0])) {
    return std::nullopt;
  }
  const TypeExpr* base_ty = analysis_.deduce_type(*i.base);
  if (!base_ty) return std::nullopt;
  base_ty = analysis_.canonicalize_type(base_ty);
  if (!base_ty || base_ty->kind != Kind::TyArray) return std::nullopt;
  const auto& arr = static_cast<const TyArray&>(*base_ty);
  if (arr.array_kind != ArrayKind::Fixed || arr.dims.size() != 1 ||
      !arr.element) {
    return std::nullopt;
  }
  std::string lo;
  std::string size_expr;
  if (!types_.array_dim_bounds_to_cxx(*arr.dims[0], &lo, &size_expr)) {
    return std::nullopt;
  }
  EmitUntypedStorageIndexView view;
  view.elem_cxx = types_.type_to_cxx(*arr.element);
  const std::string index_cxx = expr_ops_.expr_to_cxx(*i.indices[0]);
  const std::string offset =
      "((" + index_cxx + ") - (" + lo + ")) * sizeof(" + view.elem_cxx + ")";
  view.ptr_cxx = "::rt::tp2cc_byte_offset(" +
                 expr_ops_.expr_to_cxx(*cast.args[0]) + ", " + offset + ")";
  return view;
}

bool EmitStorage::type_is_packed_record(const TypeExpr* t) {
  if (!registry_ || !t) return false;
  if (t->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*t);
    auto it = registry_->records.find(ascii_lower(n.name));
    if (it != registry_->records.end()) return it->second.is_packed;
  }
  t = analysis_.canonicalize_type(t);
  return t && t->kind == Kind::TyRecord &&
         static_cast<const TyRecord&>(*t).is_packed;
}

bool EmitStorage::type_is_direct_packed_aggregate(const TypeExpr* t) {
  if (!t) return false;
  if (t->kind == Kind::TyName) {
    const std::string low = ascii_lower(static_cast<const TyName&>(*t).name);
    if (!low.empty() && registry_ &&
        (registry_->records.count(low) || registry_->classes.count(low))) {
      return true;
    }
    static const std::unordered_set<std::string> runtime_aggregate_types = {
        "datetime", "dirstr", "extstr", "namestr",
        "pathstr",  "searchrec", "stat", "tmethod",
    };
    if (runtime_aggregate_types.count(low)) return true;
  }
  t = analysis_.canonicalize_type(t);
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

bool EmitStorage::type_is_byte_aligned_packed_index_carrier(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
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
          types_.enum_underlying_type_to_cxx(static_cast<const TyEnum&>(*t));
      return underlying == "uint8_t" || underlying == "int8_t";
    }
    case Kind::TySubrange: {
      const std::string lowered =
          types_.subrange_type_to_cxx(static_cast<const TySubrange&>(*t));
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

std::optional<EmitPackedAggregateFieldUse>
EmitStorage::direct_packed_aggregate_field_use(const Expr& e) {
  if (!registry_ || e.kind != Kind::Member) return std::nullopt;
  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = analysis_.deduce_type(*m.base);
  if (!type_is_packed_record(base_type)) return std::nullopt;
  const TypeExpr* field_type =
      analysis_.lookup_record_field_type_in_type(base_type, m.name);
  if (!field_type || !type_is_direct_packed_aggregate(field_type)) {
    return std::nullopt;
  }
  std::string record_name = registry_->direct_type_name(base_type);
  if (record_name.empty()) record_name = "packed record";
  return EmitPackedAggregateFieldUse{record_name, m.name};
}

void EmitStorage::report_packed_aggregate_subobject_use(
    Location where, std::string_view op,
    const EmitPackedAggregateFieldUse& use) {
  expr_ops_.report_error(where, std::string(op) +
                                    " through packed aggregate field '" +
                                    use.field_name + "' of '" + use.record_name +
                                    "' is unsupported");
}

// Strip primitive cast chains like `pointer(longint(x))` down to the
// underlying storage expression so lvalue analysis sees the real storage.
const Expr* EmitStorage::peel_primitive_casts(const Expr* e) {
  while (e && e->kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(*e);
    if (c.args.size() != 1 || c.callee->kind != Kind::Ident) break;
    if (!is_primitive_type(static_cast<const Ident&>(*c.callee).name)) break;
    e = c.args[0].get();
  }
  return e;
}

bool EmitStorage::expr_is_storage_lvalue(const Expr& e) {
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
  bool is_lvalue_expr =
      !is_inherited_member &&
      (root.kind == Kind::Ident || root.kind == Kind::Member ||
       root.kind == Kind::Index || root.kind == Kind::Deref);
  if (root.kind == Kind::Ident &&
      scope_.local_untyped_params.count(static_cast<const Ident&>(root).name)) {
    is_lvalue_expr = true;
  }
  if (!is_lvalue_expr || !registry_) return is_lvalue_expr;

  if (root.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(root);
    ResolveResult rr = resolve_name_provider_.resolve_name(id.name);
    if (rr.is_callable && rr.accepts_zero_args) return false;
    if (rr.kind == ResolvedKind::WithProperty ||
        rr.kind == ResolvedKind::ClassProperty) {
      return false;
    }
  } else if (root.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(root);
    std::string cls = analysis_.deduce_class_alias(*m.base);
    if (!cls.empty()) {
      if (registry_->lookup_class_property(cls, m.name)) return false;
      if (const auto* method = registry_->lookup_class_method(cls, m.name)) {
        if (method->accepts_zero_args) return false;
      }
    }
  }
  return true;
}

bool EmitStorage::expr_is_untyped_storage_ref(const Expr& e) {
  const Expr* peeled = peel_primitive_casts(&e);
  const Expr& root = peeled ? *peeled : e;
  return root.kind == Kind::Ident &&
         scope_.local_untyped_params.count(static_cast<const Ident&>(root).name);
}

bool EmitStorage::expr_is_charish(const Expr& e) {
  const TypeExpr* t = analysis_.deduce_type(e);
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  return tyname_is(t, "char");
}

bool EmitStorage::type_is_pcharish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (tyname_is(t, "pchar")) return true;
  return t->kind == Kind::TyPointer &&
         tyname_is(static_cast<const TyPointer&>(*t).target.get(), "char");
}

bool EmitStorage::type_is_metaclass(const TypeExpr* t) {
  return !analysis_.metaclass_target_name(t).empty();
}

bool EmitStorage::type_is_reference_class(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyObject) {
    return static_cast<const TyObject&>(*t).is_reference_type;
  }
  if (t->kind != Kind::TyName) return false;
  const auto& n = static_cast<const TyName&>(*t);
  if (const auto* ci = analysis_.class_info_for_type_name(n.name)) {
    return ci->is_reference_type;
  }
  return analysis_.is_builtin_reference_class_name(n.name);
}

bool EmitStorage::expr_is_reference_class(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self" && !scope_.current_class_name.empty() && registry_) {
      auto it = registry_->classes.find(scope_.current_class_name);
      return it != registry_->classes.end() && it->second.is_reference_type;
    }
  } else if (e.kind == Kind::Call && registry_) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (analysis_.is_builtin_reference_class_name(id.name)) return true;
      auto it = registry_->classes.find(id.name);
      if (it != registry_->classes.end() && it->second.is_reference_type) {
        return true;
      }
    }
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident &&
        analysis_.is_builtin_reference_class_name(
            static_cast<const Ident&>(*c.callee).name)) {
      return true;
    }
  }
  return type_is_reference_class(analysis_.deduce_type(e));
}

std::string EmitStorage::member_access_op(const Expr& e) {
  const TypeExpr* t = analysis_.deduce_type(e);
  if (expr_is_reference_class(e) || type_is_pointerish(t)) return "->";
  if (t) {
    std::string cxx = types_.type_to_cxx(*t);
    if (!cxx.empty() && cxx.back() == '*') return "->";
  }
  return ".";
}

bool EmitStorage::type_is_stringish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyString) return true;
  return tyname_is(t, "string") || tyname_is(t, "shortstring") ||
         tyname_is(t, "ansistring") || tyname_is(t, "utf8string");
}

bool EmitStorage::type_is_pointerish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (type_is_metaclass(t)) return true;
  if (type_is_reference_class(t)) return true;
  if (analysis_.type_is_interface(t)) return true;
  if (t->kind == Kind::TyPointer) return true;
  return tyname_is(t, "pointer") || tyname_is(t, "pchar") ||
         tyname_is(t, "ppchar");
}

bool EmitStorage::type_is_open_array(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  return t && t->kind == Kind::TyArray &&
         static_cast<const TyArray&>(*t).array_kind == ArrayKind::Open;
}

std::string EmitStorage::coerce_pointer_like_text(std::string_view dst_cxx_text,
                                                  const TypeExpr* dst_type,
                                                  const TypeExpr* src_type,
                                                  const std::string& source_cxx,
                                                  bool explicit_pascal_cast,
                                                  bool source_is_const_storage) {
  const TypeExpr* dst = analysis_.canonicalize_type(dst_type);
  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  if (!dst || !src) return source_cxx;

  const bool dst_proc = is_nonmethod_procedural_type(dst);
  const bool src_proc = is_nonmethod_procedural_type(src);
  const bool dst_ptr = type_is_pointerish(dst);
  const bool src_ptr = type_is_pointerish(src);
  if (!(dst_ptr || dst_proc) || !(src_ptr || src_proc)) return source_cxx;

  const std::string canonical_dst_cxx = types_.type_to_cxx(*dst);
  const std::string dst_cxx =
      dst_cxx_text.empty() ? canonical_dst_cxx : std::string(dst_cxx_text);
  const std::string src_cxx = types_.type_to_cxx(*src);
  // Compare canonical spellings here. Use-site casts may request an alias
  // spelling (`p_tchildclass`) even when both sides already canonicalize to
  // the same underlying metaclass/pointer type; emitting another cast around
  // an already-correct explicit Pascal cast only creates review-noise.
  if (canonical_dst_cxx == src_cxx || dst_cxx == src_cxx) return source_cxx;

  const bool dst_void_ptr = is_plain_pointer_type(dst);
  const bool src_void_ptr = is_plain_pointer_type(src);

  if (dst_proc && src_void_ptr) {
    return "::rt::tp2cc_funptr_from_bits<" + dst_cxx + ">(" + source_cxx +
           ")";
  }
  if (dst_void_ptr && src_proc) {
    return "::rt::tp2cc_funptr_bits(" + source_cxx + ")";
  }

  if (src_void_ptr || dst_void_ptr) {
    if (source_is_const_storage && !dst_void_ptr && !dst_proc) {
      // Pascal `const` untyped storage can still be re-viewed through a typed
      // pointer variable (`p := b; p[i] ...`), but in C++ that first appears
      // as `const void*`. Make the qualifier drop explicit instead of leaning
      // on `-fpermissive`.
      return "reinterpret_cast<" + dst_cxx + ">(const_cast<void*>(" +
             source_cxx + "))";
    }
    return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }

  const bool dst_ref_class = type_is_reference_class(dst);
  const bool src_ref_class = type_is_reference_class(src);
  if (dst_ref_class && src_ref_class) {
    auto related = [&](std::string_view ancestor, std::string current) {
      while (!current.empty()) {
        if (ascii_lower(current) == ascii_lower(std::string(ancestor))) {
          return true;
        }
        const ClassInfo* info = analysis_.class_info_for_type_name(current);
        if (!info) break;
        current = info->parent;
      }
      return false;
    };
    const std::string dst_name = reference_class_name(dst);
    const std::string src_name = reference_class_name(src);
    if ((!dst_name.empty() && !src_name.empty()) &&
        (related(dst_name, src_name) || related(src_name, dst_name))) {
      return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
    }
    if (!explicit_pascal_cast) return source_cxx;
  }

  if (explicit_pascal_cast || (dst_ptr && src_ptr)) {
    return "reinterpret_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }
  return source_cxx;
}

std::string EmitStorage::reinterpret_ref_text(const std::string& ty_cxx,
                                              const std::string& source_cxx,
                                              bool pointee_view) {
  // Keep "reinterpret the pointer slot" separate from "reinterpret the
  // pointee" even though the runtime currently spells both through similar
  // casts; later tightening must not blur those semantics.
  const char* helper = pointee_view ? "::rt::tp2cc_reinterpret_ref<"
                                    : "::rt::tp2cc_reinterpret_storage_ref<";
  return std::string(helper) + ty_cxx + ">(" + source_cxx + ")";
}

std::optional<EmitAbsoluteTargetInfo> EmitStorage::resolve_absolute_target(
    const VarDecl& vd) {
  EmitAbsoluteTargetInfo info;
  info.cxx = resolve_name_provider_.resolve_name(vd.absolute_target).cxx;

  if (scope_.local_untyped_params.count(vd.absolute_target)) {
    info.is_pointerish = true;
    return info;
  }

  auto lit = scope_.local_consts.find(vd.absolute_target);
  if (lit != scope_.local_consts.end()) {
    if (!lit->second || !lit->second->type) {
      expr_ops_.report_error(vd.loc,
                             "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    info.type = lit->second->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  auto tit = scope_.local_types.find(vd.absolute_target);
  if (tit != scope_.local_types.end()) {
    info.type = tit->second;
    info.is_pointerish = type_is_pointerish(info.type);
    info.is_const_storage = scope_.local_const_params.count(vd.absolute_target) > 0;
    return info;
  }

  ResolveResult rr = resolve_name_provider_.resolve_name(vd.absolute_target);
  if (rr.kind == ResolvedKind::ClassField && registry_ &&
      !scope_.current_class_name.empty()) {
    if (auto* f = registry_->lookup_class_field(scope_.current_class_name,
                                                vd.absolute_target)) {
      info.type = f->type.get();
      info.is_pointerish = type_is_pointerish(info.type);
      return info;
    }
  }

  if (const auto* v = analysis_.find_visible_unit_var(vd.absolute_target)) {
    info.type = v->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  if (const auto* c = analysis_.find_visible_unit_const(vd.absolute_target)) {
    if (!c->type) {
      expr_ops_.report_error(vd.loc,
                             "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    info.type = c->type.get();
    info.is_pointerish = type_is_pointerish(info.type);
    return info;
  }

  expr_ops_.report_error(vd.loc,
                         "absolute target must be a variable or typed const");
  return std::nullopt;
}

}  // namespace tp2cc
