#include "emit_storage.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

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

std::string EmitStorage::offsetof_base_type_cxx(
    const TypeExpr* t, const std::string& base_expr_cxx) {
  // `offsetof` needs a C++ type, not a value expression. Prefer the registered
  // Pascal type name when one exists, because that names the generated struct
  // directly. Inline anonymous record/object variables have no Pascal type name;
  // for those, use the emitted object expression's `decltype` so offset math
  // still names the actual generated C++ aggregate without parsing generated
  // C++ type text.
  if (registry_) {
    if (std::string name = registry_->direct_type_name(t); !name.empty()) {
      return types_.named_type_struct_cxx(name);
    }
  }
  if (!base_expr_cxx.empty()) {
    return "::std::remove_reference_t<decltype(" + base_expr_cxx + ")>";
  }
  return {};
}

std::optional<EmitTypecastStorageView> EmitStorage::typecast_storage_view(
    const Expr& e) {
  if (e.kind != Kind::Call) return std::nullopt;
  const auto& outer = static_cast<const Call&>(e);
  if (outer.args.size() != 1 || outer.callee->kind != Kind::Ident) {
    return std::nullopt;
  }

  auto typecast_target = [&](const Ident& id, const TypeExpr** target_type,
                             bool* target_is_primitive) -> std::string {
    const std::string lower = ascii_lower(id.name);
    *target_type = nullptr;
    *target_is_primitive = false;
    if (is_primitive_type(lower)) {
      *target_is_primitive = true;
      return primitive_type_cxx(lower);
    }
    if (const TypeExpr* named = analysis_.lookup_named_type_expr(lower)) {
      *target_type = named;
      return types_.type_name_text_to_cxx(id.name);
    }
    if (registry_ &&
        (registry_->records.count(lower) ||
         registry_->has_class(lower, scope_.current_unit_name))) {
      return types_.type_name_text_to_cxx(id.name);
    }
    return {};
  };

  const auto& target_id = static_cast<const Ident&>(*outer.callee);
  const TypeExpr* target_type = nullptr;
  bool target_is_primitive = false;
  std::string target_cxx =
      typecast_target(target_id, &target_type, &target_is_primitive);
  if (target_cxx.empty()) return std::nullopt;

  const Expr* source = outer.args[0].get();
  while (source && source->kind == Kind::Call) {
    const auto& nested = static_cast<const Call&>(*source);
    if (nested.args.size() != 1 || nested.callee->kind != Kind::Ident) break;
    const auto& nested_id = static_cast<const Ident&>(*nested.callee);
    const TypeExpr* ignored_type = nullptr;
    bool ignored_primitive = false;
    if (typecast_target(nested_id, &ignored_type, &ignored_primitive).empty()) {
      break;
    }
    source = nested.args[0].get();
  }
  if (!source || !expr_is_storage_lvalue(*source)) return std::nullopt;

  if (source->kind == Kind::Ident) {
    ResolveResult rr = resolve_name_provider_.resolve_name(
        static_cast<const Ident&>(*source).name);
    if (rr.kind == ResolvedKind::UnitConst ||
        rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return std::nullopt;
    }
  }

  const bool untyped_storage = expr_is_untyped_storage_ref(*source);
  EmitTypecastStorageView view;
  view.source = source;
  view.source_cxx = expr_ops_.expr_to_cxx(*source);
  view.target_cxx = target_cxx;
  view.target_type = target_type;
  view.target_is_primitive = target_is_primitive;
  view.source_is_untyped_storage = untyped_storage;
  if (untyped_storage) {
    view.source_ptr_cxx = view.source_cxx;
  } else if (auto storage = storage_designator(*source)) {
    view.source_ptr_cxx = storage_designator_raw_address(*storage);
  }
  // Pointee-view applies only to untyped-param storage (`procedure foo(var x)`
  // with `T(x) := y` meaning "write T at *x"). A typed pointer lvalue cast
  // like `ptaiprop(field) := y` is a storage alias of the slot itself;
  // emitting pointee-view there dereferences the slot's (often null) value.
  view.pointee_view = untyped_storage;
  return view;
}

std::optional<EmitStorageDesignator> EmitStorage::storage_designator(
    const Expr& e) {
  auto scalar_storage_type_cxx = [&](const TypeExpr* t) -> std::string {
    t = analysis_.canonicalize_type(t);
    if (!t) return {};
    switch (t->kind) {
      case Kind::TyArray:
      case Kind::TyRecord:
      case Kind::TyObject:
      case Kind::TyProcedural:
      case Kind::TySet:
      case Kind::TyString:
        return {};
      default:
        return types_.type_to_cxx(*t);
    }
  };

  if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (id.name == "unaligned") {
        if (auto storage = bytewise_storage_ref(*c.args[0])) {
          // `unaligned(x)` promises only byte-addressable storage. Binding a
          // C++ `T&` would still require alignment and a live `T` object, so
          // reads/writes/increments must stay on memcpy-style helpers.
          return EmitStorageDesignator{EmitStorageAccess::UnalignedBytewise,
                                       {}, storage->void_ptr_text,
                                       storage->elem_cxx,
                                       EmitStorageAddressForm::RawBytePointer};
        }
      }
    }
    if (auto view = typecast_storage_view(e)) {
      if (view->target_is_primitive) {
        if (view->source_is_untyped_storage) {
          // Untyped Pascal parameters are already raw caller-storage pointers.
          // A primitive cast such as `longint(b)` says how to interpret those
          // bytes; it must not manufacture a typed C++ reference to storage
          // that may not contain a live `longint` object.
          return EmitStorageDesignator{EmitStorageAccess::Bytewise, {},
                                       view->source_cxx, view->target_cxx,
                                       EmitStorageAddressForm::RawBytePointer};
        }
        if (view->source) {
          if (auto packed = packed_scalar_storage_ref(*view->source)) {
            // A primitive cast over packed scalar storage is an aliasing view
            // in Pascal, but the packed field may be misaligned in C++. Keep it
            // as bytewise storage so stores and Inc/Dec use memcpy helpers.
            return EmitStorageDesignator{EmitStorageAccess::Bytewise, {},
                                         packed->void_ptr_text,
                                         view->target_cxx,
                                         EmitStorageAddressForm::RawBytePointer};
          }
        }
      }
      // In a storage context, `T(x)` aliases the original Pascal variable
      // designator as type `T`. This reference path is only for ordinary
      // aligned storage; raw/untyped/packed cases returned bytewise above.
      return EmitStorageDesignator{
          EmitStorageAccess::ReinterpretRef,
          reinterpret_ref_text(view->target_cxx, view->source_cxx,
                               view->pointee_view),
          view->source_ptr_cxx, view->target_cxx,
          EmitStorageAddressForm::TypedStoragePointer};
    }
  }

  if (e.kind == Kind::Index) {
    const auto& i = static_cast<const Index&>(e);
    if (auto view = untyped_storage_index_view(i)) {
      // `TArray(b)[i]` where `b` is untyped storage indexes bytes owned by the
      // caller, not a temporary C++ array object. Compute the element address
      // and let the load/store helpers copy the element representation.
      return EmitStorageDesignator{EmitStorageAccess::Bytewise, {},
                                   view->ptr_cxx, view->elem_cxx,
                                   EmitStorageAddressForm::RawBytePointer};
    }
    if (!expr_is_storage_lvalue(e)) return std::nullopt;
    const TypeExpr* t = analysis_.deduce_type(e);
    std::string type_cxx;
    if (t) type_cxx = types_.type_to_cxx(*t);

    // Do not call `expr_to_cxx(e)` for an ordinary index here:
    // `expr_to_cxx(Index)` asks this storage layer whether the index denotes
    // bytewise storage. Compose the ordinary indexed lvalue from its already
    // lowered base and index expressions so shared storage consumers like
    // `Inc(a[i])`, `@a[i]`, and var/out arguments can use one designator path
    // without recursing back into index expression emission.
    std::string text = expr_ops_.expr_to_cxx(*i.base);
    for (const auto& idx : i.indices) {
      text += "[" + expr_ops_.expr_to_cxx(*idx) + "]";
    }
    return EmitStorageDesignator{EmitStorageAccess::Ordinary, text, {},
                                 type_cxx};
  }

  if (e.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(e);
    // A storage probe must not recurse into the qualifier of `Unit.var`.
    // The qualifier is a namespace-like Pascal symbol, not an addressable
    // expression, and reporting it as unresolved would reject valid assignments
    // such as `constexp.internalerror := @internalerror`.
    if (auto unit_member = analysis_.resolve_unit_qualified_member(m)) {
      if (unit_member->resolved.kind == ResolvedKind::UnitVar) {
        const TypeExpr* t = analysis_.deduce_type(e);
        return EmitStorageDesignator{
            EmitStorageAccess::Ordinary, unit_member->resolved.cxx, {},
            t ? scalar_storage_type_cxx(t) : std::string{}};
      }
    }
    auto base = storage_designator(*m.base);
    if (base && !base->is_bytewise()) {
      auto reference_class_cast_pointer = [&](const Expr& base_expr) {
        if (base_expr.kind != Kind::Call) return std::string{};
        const auto& call = static_cast<const Call&>(base_expr);
        if (call.args.size() != 1 || call.callee->kind != Kind::Ident) {
          return std::string{};
        }
        const TypeExpr* target_ty =
            analysis_.canonicalize_type(analysis_.deduce_type(base_expr));
        if (!type_is_reference_class(target_ty)) return std::string{};
        const TypeExpr* source_ty = analysis_.deduce_type(*call.args[0]);
        if (!type_is_reference_class(source_ty) &&
            !type_is_pointerish(source_ty)) {
          return std::string{};
        }
        // A reference-class typecast used as a member base changes the object
        // pointer used for `->field`. It is different from a var/out actual
        // like `Take(TChild(p))`, where Pascal reinterprets the caller's
        // pointer slot. Field storage must therefore start from the casted
        // object pointer value, not from the address of the local pointer
        // variable.
        return coerce_pointer_like_text(
            types_.type_to_cxx(*target_ty), target_ty, source_ty,
            expr_ops_.expr_to_cxx(*call.args[0]),
            /*explicit_pascal_cast=*/true);
      };
      const std::string class_cast_base_ptr =
          reference_class_cast_pointer(*m.base);
      std::string owner = analysis_.deduce_class_alias(*m.base);
      std::string member_cxx = mangle(m.name);
      if (registry_ && !owner.empty()) {
        if (const auto* prop = registry_->lookup_class_property(
                owner, m.name, scope_.current_unit_name)) {
          // A property name is not a C++ member name. In storage contexts we
          // can only expose the property as storage when its read accessor is
          // an actual backing field path, e.g. `property Datatype read data.typ`.
          // Getter methods produce values, and write accessors are handled
          // before assignment asks for a raw storage designator.
          if (!prop->params.empty() ||
              prop->read.kind != PropertyAccessorKind::FieldPath) {
            return std::nullopt;
          }
          member_cxx = prop->read.cxx_path;
        } else if (registry_->lookup_class_field(owner, m.name,
                                                 scope_.current_unit_name) ||
                   registry_->lookup_record_field(owner, m.name)) {
          member_cxx = registry_->field_cxx_name(m.name);
        }
      }

      const std::string base_text =
          class_cast_base_ptr.empty() ? base->text : class_cast_base_ptr;
      const std::string text = base_text + member_access_op(*m.base) + member_cxx;
      const TypeExpr* field_type = analysis_.deduce_type(e);
      const std::string field_cxx = scalar_storage_type_cxx(field_type);
      std::string field_ptr_cxx;
      const std::string offset_type =
          offsetof_base_type_cxx(analysis_.deduce_type(*m.base), base->text);
      if (!offset_type.empty()) {
        // Packed fields and fields selected from a reinterpreted storage view
        // need the field address without first manufacturing a C++ reference to
        // the containing object. Compute it from the base storage address plus
        // the C++ field offset, so later memcpy helpers never depend on
        // misaligned or non-live typed references.
        const std::string base_addr =
            class_cast_base_ptr.empty() ? storage_designator_raw_address(*base)
                                        : class_cast_base_ptr;
        field_ptr_cxx = "::rt::tp2cc_byte_offset(" + base_addr +
                        ", offsetof(" + offset_type + ", " + member_cxx +
                        "))";
      }

      if (type_is_packed_record(analysis_.deduce_type(*m.base))) {
        if (field_cxx.empty() || field_ptr_cxx.empty()) return std::nullopt;
        // A field selected from a storage-view cast such as
        // `TRegisterRec(r).subreg := x` is storage in the original `r` slot.
        // If the view type is packed, use the offset-derived field address
        // above so the load/store helpers copy bytes without binding a C++
        // reference to a possibly misaligned field.
        return EmitStorageDesignator{EmitStorageAccess::Bytewise, {},
                                     field_ptr_cxx, field_cxx,
                                     EmitStorageAddressForm::RawBytePointer};
      }

      if (base->is_special() ||
          (!base->ptr_cxx.empty() && !field_ptr_cxx.empty())) {
        // Pointer-backed bases such as `p^` and reference-class casts already
        // know the address of the containing Pascal object. Propagate that
        // address through the field offset so `@base.field` and var/out
        // arguments use the field's address without manufacturing a C++ field
        // reference first.
        return EmitStorageDesignator{base->access, text, field_ptr_cxx,
                                     field_cxx,
                                     EmitStorageAddressForm::RawBytePointer};
      }
    }
  }

  if (auto packed = packed_scalar_storage_ref(e)) {
    return EmitStorageDesignator{EmitStorageAccess::Bytewise, {},
                                 packed->void_ptr_text, packed->elem_cxx,
                                 EmitStorageAddressForm::RawBytePointer};
  }

  if (!expr_is_storage_lvalue(e)) return std::nullopt;
  const TypeExpr* t = analysis_.deduce_type(e);
  std::string type_cxx;
  if (t) type_cxx = types_.type_to_cxx(*t);
  if (e.kind == Kind::Deref) {
    const auto& d = static_cast<const Deref&>(e);
    // `p^` is an ordinary typed Pascal lvalue for reads and writes, but Pascal
    // address-of cancels the dereference: `@p^` is the operand pointer
    // expression. This matters for untyped raw-memory calls such as
    // `Move(p^, q^, 0)`: Pascal only asks for a storage address, while
    // `&tp2cc_deref(p)` first forms a C++ reference and is already invalid if
    // `p` is nil even though the byte count is zero.
    return EmitStorageDesignator{EmitStorageAccess::Ordinary,
                                 expr_ops_.expr_to_cxx(e),
                                 expr_ops_.expr_to_cxx(*d.operand),
                                 type_cxx,
                                 EmitStorageAddressForm::TypedStoragePointer};
  }
  return EmitStorageDesignator{EmitStorageAccess::Ordinary,
                               expr_ops_.expr_to_cxx(e), {}, type_cxx};
}

std::string EmitStorage::storage_designator_value(
    const EmitStorageDesignator& d) {
  if (!d.is_bytewise()) return d.text;
  const char* helper = d.access == EmitStorageAccess::UnalignedBytewise
                           ? "::rt::tp2cc_unaligned_load<"
                           : "::rt::tp2cc_reinterpret_load<";
  return std::string(helper) + d.type_cxx + ">(" + d.ptr_cxx + ")";
}

std::string EmitStorage::storage_designator_raw_address(
    const EmitStorageDesignator& d) {
  if (d.is_bytewise()) return d.ptr_cxx;
  // Ordinary dereference designators carry their already-known address in
  // `ptr_cxx`; other ordinary lvalues need C++ address-of.
  if (!d.ptr_cxx.empty()) return d.ptr_cxx;
  return "(&" + d.text + ")";
}

std::string EmitStorage::storage_designator_typed_address_value(
    const EmitStorageDesignator& d) {
  const std::string address = storage_designator_raw_address(d);
  if (address.empty() || d.type_cxx.empty()) return address;
  if (d.raw_address_needs_typed_cast()) {
    // `@expr` has a typed Pascal pointer result even when `expr` reached its
    // storage through byte arithmetic or a storage-view typecast. Keep the raw
    // address form for untyped storage and memcpy helpers, but cast it here
    // where the Pascal expression itself is a typed address value. This is the
    // difference between `Move(x, y, n)` needing an untyped storage address and
    // `p := @pai(last)^.fileinfo` needing a `pfileposinfo`.
    return "reinterpret_cast<" + d.type_cxx + "*>(" + address + ")";
  }
  return address;
}

std::string EmitStorage::storage_designator_untyped_actual_address(
    const EmitStorageDesignator& d, std::string_view ptr_cast) {
  // Untyped Pascal parameters are modeled as `void*`/`const void*` pointing at
  // caller storage. This deliberately uses the raw storage address, not the
  // typed Pascal `@expr` value: `Move(p^, q^, 0)` must pass `p` and `q` without
  // forming references, and packed/unaligned storage must stay byte-addressed.
  return "((" + std::string(ptr_cast) + ")(" +
         storage_designator_raw_address(d) + "))";
}

std::string EmitStorage::storage_designator_store(
    const EmitStorageDesignator& d, const std::string& value_cxx) {
  if (!d.is_bytewise()) return d.text + " = " + value_cxx;
  const char* helper = d.access == EmitStorageAccess::UnalignedBytewise
                           ? "::rt::tp2cc_unaligned_store<"
                           : "::rt::tp2cc_reinterpret_store<";
  return std::string(helper) + d.type_cxx + ">(" + d.ptr_cxx + ", " +
         value_cxx + ")";
}

std::string EmitStorage::storage_designator_inc_dec(
    const EmitStorageDesignator& d, bool is_inc, const std::string& delta_cxx) {
  const std::string op =
      is_inc ? "::rt::tp2cc_reinterpret_inc" : "::rt::tp2cc_reinterpret_dec";
  if (d.is_bytewise()) {
    std::string out = op + "<" + d.type_cxx + ">(" + d.ptr_cxx;
    if (!delta_cxx.empty()) out += ", " + delta_cxx;
    out += ")";
    return out;
  }
  std::string out = std::string(is_inc ? "::rt::p_inc" : "::rt::p_dec") +
                    "(" + d.text;
  if (!delta_cxx.empty()) out += ", " + delta_cxx;
  out += ")";
  return out;
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

std::optional<EmitBytewiseStorage> EmitStorage::packed_scalar_storage_ref(
    const Expr& e) {
  const TypeExpr* elem_type = analysis_.canonicalize_type(analysis_.deduce_type(e));
  if (!elem_type) return std::nullopt;
  const std::string elem_cxx = types_.type_to_cxx(*elem_type);
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
  // the normal `T&` path. For direct packed fields, derive the byte address from
  // the containing object plus `offsetof`; taking `&(base.field)` can still ask
  // C++ to form a misaligned typed field access before memcpy sees the bytes.
  for (const Expr* cur = &e; cur && cur->kind == Kind::Member;
       cur = static_cast<const Member&>(*cur).base.get()) {
    const auto& m = static_cast<const Member&>(*cur);
    // `packed.outer.inner_scalar` cannot be used as mutable storage by taking
    // the final field address: C++ would still have to form the intermediate
    // packed aggregate lvalue. Only `packed_scalar_value_load` has the offset
    // based lowering needed for that expression, and it is intentionally read-only.
    if (direct_packed_aggregate_field_use(*m.base)) return std::nullopt;
    const TypeExpr* base_type = analysis_.deduce_type(*m.base);
    if (type_is_packed_record(base_type)) {
      if (auto base = storage_designator(*m.base)) {
        const std::string offset_type =
            offsetof_base_type_cxx(base_type, base->text);
        if (offset_type.empty()) return std::nullopt;
        const std::string field_name =
            registry_ ? registry_->field_cxx_name(m.name) : mangle(m.name);
        const std::string ptr =
            "::rt::tp2cc_byte_offset(" + storage_designator_raw_address(*base) +
            ", offsetof(" + offset_type + ", " + field_name + "))";
        return EmitBytewiseStorage{ptr, elem_cxx};
      }
      return std::nullopt;
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
        (registry_->records.count(low) ||
         registry_->has_class(low, scope_.current_unit_name))) {
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
      return primitive_name_is_charish(low) || low == "byte" ||
             low == "shortint" || low == "boolean";
    }
    default:
      return false;
  }
}

std::optional<EmitPackedScalarValueLoad> EmitStorage::packed_scalar_value_load(
    const Expr& e) {
  // This is deliberately not implemented in terms of `bytewise_storage_ref`.
  // That helper is for storage contexts and its fallback takes `&(expr)`.
  // For `packed_record.aggregate.scalar`, `&(expr)` would already have formed
  // the forbidden intermediate aggregate lvalue. Here we instead build
  // `offsetof` sums over the member chain and byte-load from the base object.
  // A packed aggregate subfield can be read safely as bytes, but binding a
  // C++ reference to the intermediate packed aggregate would not be safe.
  if (!registry_ || e.kind != Kind::Member) return std::nullopt;

  std::vector<const Member*> chain;
  const Expr* root = &e;
  while (root && root->kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(*root);
    chain.push_back(&m);
    root = m.base.get();
  }
  if (!root || chain.empty()) return std::nullopt;
  std::reverse(chain.begin(), chain.end());

  const TypeExpr* current_type = analysis_.deduce_type(*root);
  if (!current_type) return std::nullopt;

  bool crossed_packed_aggregate = false;
  std::vector<std::string> offsets;
  for (const Member* m : chain) {
    const TypeExpr* base_type = current_type;
    const TypeExpr* field_type =
        analysis_.lookup_record_field_type_in_type(base_type, m->name);
    if (!field_type) return std::nullopt;

    const std::string base_cxx = offsetof_base_type_cxx(base_type, {});
    if (base_cxx.empty()) return std::nullopt;
    offsets.push_back("offsetof(" + base_cxx + ", " +
                      registry_->field_cxx_name(m->name) + ")");

    if (type_is_packed_record(base_type) &&
        type_is_direct_packed_aggregate(field_type)) {
      crossed_packed_aggregate = true;
    }
    current_type = field_type;
  }
  if (!crossed_packed_aggregate) return std::nullopt;

  const TypeExpr* scalar_type = analysis_.canonicalize_type(current_type);
  if (!scalar_type || type_is_direct_packed_aggregate(scalar_type)) {
    return std::nullopt;
  }
  const std::string scalar_cxx = types_.type_to_cxx(*current_type);
  if (scalar_cxx.empty()) return std::nullopt;

  std::string base_ptr;
  if (root->kind == Kind::Deref) {
    const auto& d = static_cast<const Deref&>(*root);
    base_ptr =
        "static_cast<const void*>(" + expr_ops_.expr_to_cxx(*d.operand) + ")";
  } else {
    base_ptr =
        "static_cast<const void*>(&(" + expr_ops_.expr_to_cxx(*root) + "))";
  }

  std::string offset = offsets.front();
  for (size_t i = 1; i < offsets.size(); ++i) {
    offset += " + " + offsets[i];
  }

  return EmitPackedScalarValueLoad{
      "::rt::tp2cc_unaligned_load<" + scalar_cxx +
      ">(::rt::tp2cc_byte_offset(" + base_ptr + ", " + offset + "))"};
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
      if (registry_->lookup_class_property(
              cls, m.name, scope_.current_unit_name)) return false;
      if (registry_->lookup_class_methods(cls, m.name,
                                          scope_.current_unit_name)) return false;
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
  return tyname_is_charish(t);
}

bool EmitStorage::type_is_pcharish(const TypeExpr* t) {
  if (!t) return false;
  if (tyname_is(t, "pchar") || tyname_is(t, "pansichar")) return true;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (tyname_is(t, "pchar") || tyname_is(t, "pansichar")) return true;
  return t->kind == Kind::TyPointer &&
         tyname_is_charish(static_cast<const TyPointer&>(*t).target.get());
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
      const ClassInfo* ci =
          registry_->lookup_class(scope_.current_class_name,
                                  scope_.current_unit_name);
      return ci && ci->is_reference_type;
    }
  } else if (e.kind == Kind::Call && registry_) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (analysis_.is_builtin_reference_class_name(id.name)) return true;
      const ClassInfo* ci =
          registry_->lookup_class(id.name, scope_.current_unit_name);
      if (ci && ci->is_reference_type) {
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
  return tyname_is(t, "shortstring") || tyname_is(t, "ansistring") ||
         tyname_is(t, "utf8string");
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
         tyname_is(t, "pansichar") || tyname_is(t, "ppchar");
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
                                                vd.absolute_target,
                                                scope_.current_unit_name)) {
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
