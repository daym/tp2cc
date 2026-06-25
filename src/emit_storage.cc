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

const TypeSymbol* local_type_symbol(const TypeRegistry& registry,
                                    const ScopeStateView& scope,
                                    std::string_view name) {
  return lexical_type_symbol_in_context(registry, scope, name);
}

bool is_nonmethod_procedural_type(const TypeExpr* t) {
  if (!t) return false;
  t = static_cast<const TypeExpr*>(t);
  return t->kind == Kind::TyProcedural &&
         !static_cast<const TyProcedural&>(*t).is_method;
}

bool is_plain_pointer_type(const TypeExpr* t) {
  if (t == named_pascal_type("pointer")) return true;
  return t && t->kind == Kind::TyPointer &&
         !static_cast<const TyPointer&>(*t).target;
}

bool is_runtime_pointer_primitive_type(const TypeExpr* t) {
  if (!t || t->kind != Kind::TyName) return false;
  const PrimitiveInfo* pi = primitive_info(
      ascii_lower(static_cast<const TyName&>(*t).name));
  return pi && pi->is_pointer_primitive();
}

std::string reference_class_name(const TypeExpr* t) {
  if (!t || t->kind != Kind::TyName) return {};
  return ascii_lower(static_cast<const TyName&>(*t).name);
}

}  // namespace

EmitStorage::EmitStorage(const TypeRegistry& registry, ScopeStateView& scope,
                         EmitAnalysis& analysis, EmitTypes& types,
                         ResolveNameProvider& resolve_name_provider,
                         EmitStorageExprOps& expr_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      resolve_name_provider_(resolve_name_provider),
      expr_ops_(expr_ops) {}

const TypeExpr* EmitStorage::storage_expr_type(const Expr& e) {
  // Storage lowering needs the source/designator type for address composition
  // and aliasing decisions. Overload-selected value result typing is a
  // different query domain and must not be mixed into storage ownership.
  return analysis_.deduce_type(e);
}

const TypeExpr* EmitStorage::canonical_storage_expr_type(const Expr& e) {
  return analysis_.canonicalize_type(storage_expr_type(e));
}

std::string EmitStorage::offsetof_base_type_cxx(
    const TypeExpr* t, const std::string& base_expr_cxx) {
  // `offsetof` needs a C++ type, not a value expression. Prefer the registered
  // Pascal type name when one exists, because that names the generated struct
  // directly. Inline anonymous record/object variables have no Pascal type name;
  // for those, use the emitted object expression's `decltype` so offset math
  // still names the actual generated C++ aggregate without parsing generated
  // C++ type text.
  if (t && t->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*t);
    if (local_type_symbol(registry_, scope_, n.name)) {
      return types_.named_type_struct_cxx(n.name);
    }
  }
  if (std::string name =
          registry_.direct_type_name(t, scope_.current_unit_name);
      !name.empty()) {
    return types_.named_type_struct_cxx(name);
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

  const auto& target_id = static_cast<const Ident&>(*outer.callee);
  auto target = storage_typecast_target(target_id, *outer.args[0]);
  if (!target) return std::nullopt;

  const Expr* source = outer.args[0].get();
  while (source && source->kind == Kind::Call) {
    const auto& nested = static_cast<const Call&>(*source);
    if (nested.args.size() != 1 || nested.callee->kind != Kind::Ident) break;
    const auto& nested_id = static_cast<const Ident&>(*nested.callee);
    if (!storage_typecast_target(nested_id, *nested.args[0])) break;
    source = nested.args[0].get();
  }
  if (!source) return std::nullopt;

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
  std::optional<EmitStorageDesignator> source_storage =
      untyped_storage ? std::nullopt : storage_designator(*source);
  // A storage designator can prove storage even when the syntax is not a plain
  // lvalue. `unaligned(x)` is a Call node, but it still denotes `x`'s bytes.
  if (!untyped_storage && !source_storage && !expr_is_storage_lvalue(*source)) {
    return std::nullopt;
  }
  std::string source_cxx = expr_ops_.expr_to_cxx(*source);
  std::string source_ptr_cxx =
      untyped_storage
          ? source_cxx
          : (source_storage
                 ? storage_designator_raw_address(*source_storage)
                 : typecast_source_raw_pointer(*source, source_cxx, false));
  // Pointee-view applies only to untyped-param storage (`procedure foo(var x)`
  // with `T(x) := y` meaning "write T at *x"). A typed pointer lvalue cast
  // like `ptaiprop(field) := y` is a storage alias of the slot itself;
  // emitting pointee-view there dereferences the slot's (often null) value.
  return EmitTypecastStorageView(source, std::move(source_cxx),
                                 std::move(source_ptr_cxx),
                                 std::move(target->cxx), target->type,
                                 target->primitive, untyped_storage,
                                 source_storage && source_storage->is_bytewise(),
                                 source_storage &&
                                     source_storage->access ==
                                         EmitStorageAccess::UnalignedBytewise,
                                 untyped_storage);
}

std::string EmitStorage::ord_storage_target_cxx(const Expr& source) {
  if (const TypeExpr* target = analysis_.ord_result_type_for_operand(source)) {
    return types_.type_to_cxx(*target);
  }
  return {};
}

bool EmitStorage::chr_source_has_byte_storage(const Expr& source) {
  const TypeExpr* source_type = canonical_storage_expr_type(source);
  if (!source_type) return false;
  if (source_type->kind == Kind::TyName) {
    const TypeExpr* canon = analysis_.canonicalize_type(source_type);
    if (canon == named_pascal_type("byte") ||
        canon == named_pascal_type("shortint")) {
      return true;
    }
  }
  if (source_type->kind == Kind::TySubrange) {
    const std::string cxx = types_.type_to_cxx(*source_type);
    return cxx == "uint8_t" || cxx == "int8_t";
  }
  return false;
}

std::optional<EmitStorage::StorageCastTarget>
EmitStorage::storage_typecast_target(const Ident& id, const Expr& source) {
  const std::string lower = ascii_lower(id.name);
  if (lower == "ord") {
    if (std::string target = ord_storage_target_cxx(source); !target.empty()) {
      return StorageCastTarget{std::move(target), nullptr, true};
    }
    return std::nullopt;
  }
  if (lower == "chr") {
    if (chr_source_has_byte_storage(source)) {
      return StorageCastTarget{"::rt::p_char", builtin_char_type(), true};
    }
    return std::nullopt;
  }
  if (is_primitive_type(lower)) {
    return StorageCastTarget{primitive_type_cxx(lower), nullptr, true};
  }
  if (const TypeExpr* named = analysis_.lookup_named_type_expr(lower)) {
    return StorageCastTarget{types_.type_name_text_to_cxx(id.name), named,
                             false};
  }
  if (const TypeSymbol* symbol =
          registry_.lookup_type_symbol(lower, scope_.current_unit_name);
      symbol && (symbol->record_info() || symbol->class_info())) {
    return StorageCastTarget{types_.type_name_text_to_cxx(id.name), nullptr,
                             false};
  }
  return std::nullopt;
}

std::string EmitStorage::typecast_source_raw_pointer(
    const Expr& source, const std::string& source_cxx, bool untyped_storage) {
  if (untyped_storage) return source_cxx;
  if (auto storage = storage_designator(source)) {
    return storage_designator_raw_address(*storage);
  }
  return {};
}

std::string EmitStorage::storage_type_cxx(const TypeExpr* t) {
  return t ? types_.type_to_cxx(*t) : std::string{};
}

std::optional<EmitStorageDesignator>
EmitStorage::resolved_bytewise_with_field_storage(const ResolveResult& rr) {
  if (rr.kind != ResolvedKind::WithField || !rr.bytewise_with_field) {
    return std::nullopt;
  }
  const auto& field = *rr.bytewise_with_field;
  if (field.base_ptr_cxx.empty() || field.base_type_cxx.empty() ||
      field.field_cxx.empty() || !field.field_type) {
    return std::nullopt;
  }
  const std::string field_cxx = storage_type_cxx(field.field_type);
  if (field_cxx.empty()) return std::nullopt;
  const std::string ptr_cxx =
      "::rt::tp2cc_byte_offset(" + field.base_ptr_cxx + ", offsetof(" +
      field.base_type_cxx + ", " + field.field_cxx + "))";
  if (field.unaligned) {
    return EmitStorageDesignator::unaligned_bytewise(ptr_cxx, field_cxx);
  }
  return EmitStorageDesignator::bytewise(ptr_cxx, field_cxx);
}

const TypeExpr* EmitStorage::pointer_like_member_object_type(const TypeExpr* t) {
  t = analysis_.canonicalize_type(t);
  if (!t) return nullptr;
  if (t->kind == Kind::TyPointer) {
    return static_cast<const TyPointer&>(*t).target.get();
  }
  if (type_is_reference_class(t) || analysis_.type_is_interface(t)) {
    return t;
  }
  return nullptr;
}

std::string EmitStorage::scalar_storage_type_cxx(const TypeExpr* t) {
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
}

std::optional<EmitStorageDesignator> EmitStorage::raw_address_index_designator(
    const Index& i, const EmitStorageDesignator& base,
    const TypeExpr* base_type) {
  // Index suffixes over pointer-backed or byte-addressed storage must compose
  // the storage address. Rebuilding them as `&base[i]` would first dereference
  // pointer bases such as `p^` or load bytewise aggregate storage into a C++
  // temporary.
  base_type = analysis_.canonicalize_type(base_type);
  if (!base_type || i.indices.empty()) return std::nullopt;

  const bool base_address_is_pointer_value =
      base.access == EmitStorageAccess::Ordinary &&
      base.ptr_form == EmitStorageAddressForm::TypedStoragePointer &&
      !base.ptr_cxx.empty();

  std::string text;
  if (!base.is_bytewise()) {
    text = base.text;
    for (const auto& idx : i.indices) {
      text += "[" + expr_ops_.expr_value_to_cxx(*idx) + "]";
    }
  }

  if (i.indices.size() == 1) {
    if (auto cap = types_.shortstring_capacity_to_cxx(base_type)) {
      const std::string index_cxx =
          expr_ops_.expr_value_to_cxx(*i.indices[0]);
      const char* offset_helper = base_address_is_pointer_value
                                      ? "::rt::tp2cc_pointer_byte_offset"
                                      : "::rt::tp2cc_byte_offset";
      const std::string base_addr = storage_designator_raw_address(base);
      const std::string offset_cxx =
          "::rt::tp2cc_shortstring_index_offset<" + *cap + ">(" +
          index_cxx + ")";
      const std::string ptr_cxx =
          std::string(offset_helper) + "(" + base_addr + ", " + offset_cxx +
          ")";
      return EmitStorageDesignator::raw_byte_address(base.access, text, ptr_cxx,
                                                     "::rt::p_char");
    }
  }

  if (base_type->kind != Kind::TyArray) return std::nullopt;
  const auto& array_type = static_cast<const TyArray&>(*base_type);
  if (array_type.array_kind != ArrayKind::Fixed ||
      i.indices.size() > array_type.dims.size() || !array_type.element) {
    return std::nullopt;
  }
  auto element_cxx_after_dim = [&](size_t selected_dim) {
    std::string ty = storage_type_cxx(array_type.element.get());
    for (size_t dim = array_type.dims.size(); dim-- > selected_dim + 1;) {
      auto bounds = types_.array_dim_bounds_to_cxx(*array_type.dims[dim]);
      if (!bounds) return std::string{};
      ty = "::rt::tp2cc_Array<" + ty + ", " + bounds->low + ", " +
           bounds->size_expr + ">";
    }
    return ty;
  };
  std::string ptr_cxx = storage_designator_raw_address(base);
  std::string elem_cxx;
  for (size_t dim = 0; dim < i.indices.size(); ++dim) {
    auto bounds = types_.array_dim_bounds_to_cxx(*array_type.dims[dim]);
    if (!bounds) return std::nullopt;
    elem_cxx = element_cxx_after_dim(dim);
    if (elem_cxx.empty()) return std::nullopt;
    const std::string index_cxx =
        expr_ops_.expr_value_to_cxx(*i.indices[dim]);
    const std::string offset = "((" + index_cxx + ") - (" + bounds->low +
                               ")) * sizeof(" + elem_cxx + ")";
    const char* offset_helper = dim == 0 && base_address_is_pointer_value
                                    ? "::rt::tp2cc_pointer_byte_offset"
                                    : "::rt::tp2cc_byte_offset";
    ptr_cxx = std::string(offset_helper) + "(" + ptr_cxx + ", " + offset + ")";
  }
  return EmitStorageDesignator::raw_byte_address(base.access, text, ptr_cxx,
                                                 elem_cxx);
}

bool EmitStorage::index_base_denotes_property_value(const Index& i) {
  if (i.base->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*i.base);
    std::string cls;
    if (mem.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*mem.base).name == "self") {
      cls = scope_.current_class_name;
    } else {
      cls = analysis_.deduce_class_alias(*mem.base);
    }
    if (!cls.empty() &&
        registry_.lookup_class_property(cls, mem.name,
                                         scope_.current_unit_name)) {
      return true;
    }
  }

  if (i.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*i.base);
    if (auto found = analysis_.find_implicit_class_property(id.name);
        found && found->prop) {
      return true;
    }
  }

  std::string cls = analysis_.deduce_class_alias(*i.base);
  return !cls.empty() &&
         registry_.lookup_default_property(cls, scope_.current_unit_name);
}

std::optional<EmitStorageDesignator> EmitStorage::storage_designator(
    const Expr& e) {
  if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (id.name == "unaligned") {
        if (auto storage = bytewise_storage_ref(*c.args[0])) {
          // `unaligned(x)` promises only byte-addressable storage. Binding a
          // C++ `T&` would still require alignment and a live `T` object, so
          // reads/writes/increments must stay on memcpy-style helpers.
          return EmitStorageDesignator::unaligned_bytewise(
              storage->void_ptr_text, storage->elem_cxx);
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
          return EmitStorageDesignator::bytewise(view->source_cxx,
                                                 view->target_cxx);
        }
        if (view->source) {
          if (auto packed = packed_scalar_storage_ref(*view->source)) {
            // A primitive cast over packed scalar storage is an aliasing view
            // in Pascal, but the packed field may be misaligned in C++. Keep it
            // as bytewise storage so stores and Inc/Dec use memcpy helpers.
            return EmitStorageDesignator::unaligned_bytewise(
                packed->void_ptr_text, view->target_cxx);
          }
        }
        // Primitive storage views only need the source bytes. Model them as
        // byte-addressed storage so assignment and Inc/Dec use memcpy helpers
        // instead of binding a C++ reference of the cast target type.
        return view->source_is_unaligned_bytewise_storage
                   ? EmitStorageDesignator::unaligned_bytewise(
                         view->source_ptr_cxx, view->target_cxx)
                   : EmitStorageDesignator::bytewise(view->source_ptr_cxx,
                                                     view->target_cxx);
      }
      // In a storage context, `T(x)` aliases the original Pascal variable
      // designator as type `T`. This reference path is only for ordinary
      // aligned storage; raw/untyped/packed cases returned bytewise above.
      if (view->source_is_bytewise_storage) {
        if (view->source_is_unaligned_bytewise_storage) {
          return EmitStorageDesignator::unaligned_bytewise(
              view->source_ptr_cxx, view->target_cxx);
        }
        return EmitStorageDesignator::bytewise(view->source_ptr_cxx,
                                               view->target_cxx);
      }
      return EmitStorageDesignator::reinterpret_ref(
          reinterpret_ref_text(view->target_cxx, view->source_cxx,
                               view->pointee_view),
          view->source_ptr_cxx, view->target_cxx);
    }
  }

  if (e.kind == Kind::Index) {
    const auto& i = static_cast<const Index&>(e);
    if (auto view = untyped_storage_index_view(i)) {
      // `TArray(b)[i]` where `b` is untyped storage indexes bytes owned by the
      // caller, not a temporary C++ array object. Compute the element address
      // and let the load/store helpers copy the element representation.
      return EmitStorageDesignator::bytewise(view->ptr_cxx, view->elem_cxx);
    }
    if (auto base = storage_designator(*i.base)) {
      if (base->is_bytewise() || !base->ptr_cxx.empty()) {
        return raw_address_index_designator(
            i, *base, storage_expr_type(*i.base));
      }
    }
    if (index_base_denotes_property_value(i)) {
      // Indexed property syntax looks like an array lvalue, but the accessor
      // may be a getter/setter. Property lowering owns that semantic decision;
      // storage fallback must not first emit the bare property name as storage.
      return std::nullopt;
    }
    if (!expr_is_storage_lvalue(e)) return std::nullopt;
    const TypeExpr* t = storage_expr_type(e);
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
      text += "[" + expr_ops_.expr_value_to_cxx(*idx) + "]";
    }
    return EmitStorageDesignator::ordinary(text, type_cxx);
  }

  if (e.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(e);
    // A storage probe must not recurse into the qualifier of `Unit.var`.
    // The qualifier is a namespace-like Pascal symbol, not an addressable
    // expression, and reporting it as unresolved would reject valid assignments
    // such as `constexp.internalerror := @internalerror`.
    if (auto unit_member = analysis_.resolve_unit_qualified_member(m)) {
      if (unit_member->resolved.kind == ResolvedKind::UnitVar) {
        const TypeExpr* t = storage_expr_type(e);
        return EmitStorageDesignator::ordinary(
            unit_member->resolved.cxx,
            t ? scalar_storage_type_cxx(t) : std::string{});
      }
      return std::nullopt;
    }
    const TypeExpr* base_type = storage_expr_type(*m.base);
    const bool variant_payload_field =
        analysis_.record_field_is_variant_in_type(base_type, m.name);
    auto base = storage_designator(*m.base);
    if (!base && variant_payload_field && expr_is_storage_lvalue(*m.base)) {
      const TypeExpr* field_type = storage_expr_type(e);
      if (!field_type) {
        field_type = analysis_.lookup_record_field_type_in_type(base_type,
                                                                m.name);
      }
      const std::string field_cxx = storage_type_cxx(field_type);
      const std::string base_text = expr_ops_.expr_to_cxx(*m.base);
      const std::string offset_type =
          offsetof_base_type_cxx(base_type, base_text);
      if (field_cxx.empty() || offset_type.empty()) return std::nullopt;
      const std::string member_cxx = registry_.field_cxx_name(m.name);
      const std::string field_ptr_cxx =
          "::rt::tp2cc_byte_offset((&" + base_text + "), offsetof(" +
          offset_type + ", " + member_cxx + "))";
      return EmitStorageDesignator::bytewise(field_ptr_cxx, field_cxx);
    }
    if (base) {
      const std::string class_cast_base_ptr =
          base->is_bytewise() ? std::string{}
                              : reference_class_cast_pointer_cxx(*m.base);
      std::string owner = analysis_.deduce_class_alias(*m.base);
      std::string member_cxx = mangle(m.name);
      bool field_backed_property = false;
      if (!owner.empty()) {
        if (const auto* prop = registry_.lookup_class_property(
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
          field_backed_property = true;
        } else if (registry_.lookup_class_field(owner, m.name,
                                                 scope_.current_unit_name) ||
                   registry_.lookup_record_field(
                       owner, m.name, scope_.current_unit_name)) {
          member_cxx = registry_.field_cxx_name(m.name);
        } else if (registry_.lookup_class_methods(owner, m.name,
                                                   scope_.current_unit_name)) {
          // Storage designators are field/property paths. A method selected
          // from a byte-addressed variant payload must fall back to expression
          // lowering; otherwise the offset path would treat the method name as
          // a C++ data member and emit `offsetof(T, method)`.
          return std::nullopt;
        }
      }

      const std::string base_text =
          class_cast_base_ptr.empty() ? base->text : class_cast_base_ptr;
      const std::string text =
          base->is_bytewise() ? std::string{}
                              : base_text + member_access_op(*m.base) + member_cxx;
      const TypeExpr* field_type = storage_expr_type(e);
      if (!field_type) {
        field_type = analysis_.lookup_record_field_type_in_type(base_type,
                                                                m.name);
      }
      const bool field_uses_raw_address =
          base->is_bytewise() || variant_payload_field || base->is_special() ||
          !base->ptr_cxx.empty();
      const std::string field_cxx =
          field_uses_raw_address ? storage_type_cxx(field_type)
                                 : scalar_storage_type_cxx(field_type);
      std::string field_ptr_cxx;
      const TypeExpr* offset_base_type = base_type;
      const TypeExpr* pointer_member_object_type =
          pointer_like_member_object_type(base_type);
      const bool base_is_pointer_slot =
          pointer_member_object_type &&
          (base->is_bytewise() ||
           base->access == EmitStorageAccess::ReinterpretRef ||
           base->ptr_form == EmitStorageAddressForm::RawBytePointer ||
           (base->access == EmitStorageAccess::Ordinary &&
            base->ptr_form == EmitStorageAddressForm::TypedStoragePointer));
      if (base_is_pointer_slot) offset_base_type = pointer_member_object_type;
      const std::string offset_type =
          offsetof_base_type_cxx(offset_base_type, base->text);
      if (!offset_type.empty()) {
        // Packed fields and fields selected from a reinterpreted storage view
        // need the field address without first manufacturing a C++ reference to
        // the containing object. Compute it from the base storage address plus
        // the C++ field offset, so later memcpy helpers never depend on
        // misaligned or non-live typed references.
        std::string base_addr;
        // A raw address can be either the object address (`p^`) or the address
        // of a slot whose value is a pointer/reference. For the slot case,
        // Pascal member selection loads the pointer value before selecting the
        // next field; using the slot address as the object base reads adjacent
        // storage instead.
        const bool base_addr_is_pointer_value =
            !class_cast_base_ptr.empty() || base_is_pointer_slot ||
            (base->access == EmitStorageAccess::Ordinary &&
             base->ptr_form == EmitStorageAddressForm::TypedStoragePointer &&
             !base->ptr_cxx.empty());
        if (!class_cast_base_ptr.empty()) {
          base_addr = class_cast_base_ptr;
        } else if (base_is_pointer_slot) {
          base_addr = storage_designator_value(*base);
        } else {
          base_addr = storage_designator_raw_address(*base);
        }
        const char* offset_helper = base_addr_is_pointer_value
                                        ? "::rt::tp2cc_pointer_byte_offset"
                                        : "::rt::tp2cc_byte_offset";
        field_ptr_cxx = std::string(offset_helper) + "(" + base_addr +
                        ", offsetof(" + offset_type + ", " + member_cxx + "))";
      }

      if (base->is_bytewise()) {
        if (field_cxx.empty() || field_ptr_cxx.empty()) return std::nullopt;
        // Once a designator path is byte-addressed, later suffixes must keep
        // composing addresses. Loading the aggregate and then selecting a
        // field would create an rvalue and would re-enter C++ union active-arm
        // rules for Pascal variant-record payloads.
        return EmitStorageDesignator::raw_byte_address(base->access, {},
                                                       field_ptr_cxx,
                                                       field_cxx);
      }

      if (variant_payload_field) {
        if (field_cxx.empty() || field_ptr_cxx.empty()) return std::nullopt;
        // Pascal variant-record payload fields overlap. A program may write one
        // payload field and read another, so emitted code must not depend on a
        // C++ union arm being active. Use the generated union only for layout
        // and access the payload through its byte offset.
        return EmitStorageDesignator::bytewise(field_ptr_cxx, field_cxx);
      }

      if (type_is_packed_record(offset_base_type)) {
        if (field_cxx.empty() || field_ptr_cxx.empty()) return std::nullopt;
        // Decide packedness from the record that owns the selected field. In
        // `pp^.field`, the AST base has pointer type, but selecting `field`
        // still enters the pointee record; if that record is packed, C++ must
        // not bind a typed reference to the possibly misaligned field.
        return EmitStorageDesignator::unaligned_bytewise(field_ptr_cxx,
                                                         field_cxx);
      }

      if (base->is_special() ||
          (!base->ptr_cxx.empty() && !field_ptr_cxx.empty())) {
        // Pointer-backed bases such as `p^` and reference-class casts already
        // know the address of the containing Pascal object. Propagate that
        // address through the field offset so `@base.field` and var/out
        // arguments use the field's address without manufacturing a C++ field
        // reference first.
        return EmitStorageDesignator::raw_byte_address(base->access, text,
                                                       field_ptr_cxx,
                                                       field_cxx);
      }

      if (field_backed_property) {
        // Field-backed properties are Pascal storage aliases. Getter-backed
        // properties are rejected above, but a read accessor that names a
        // backing field can satisfy reference-style contexts such as
        // `var`/`out` and old-object `const` formals through the same
        // designator path as ordinary fields.
        return EmitStorageDesignator::ordinary(text,
                                               storage_type_cxx(field_type));
      }
    }
  }

  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (auto found = analysis_.find_implicit_class_property(id.name);
        found && found->prop && found->prop->params.empty() &&
        found->prop->read.kind == PropertyAccessorKind::FieldPath) {
      // A bare property in a method body, such as `Prop` for `self.Prop`, can
      // be used as storage only when its read accessor names backing storage.
      // Getter-backed properties stay values and must not satisfy var/out or
      // reference-style const arguments.
      const ClassInfo* ci =
          analysis_.class_info_for_type_name(found->class_name);
      const std::string access =
          (ci && ci->is_reference_type) ? "->" : ".";
      const std::string text = found->base_cxx + access +
                               found->prop->read.cxx_path;
      const TypeExpr* t = found->prop->type.get();
      return EmitStorageDesignator::ordinary(
          text, t ? types_.type_to_cxx(*t) : std::string{});
    }
  }

  if (auto packed = packed_scalar_storage_ref(e)) {
    return EmitStorageDesignator::unaligned_bytewise(packed->void_ptr_text,
                                                     packed->elem_cxx);
  }

  if (!expr_is_storage_lvalue(e)) return std::nullopt;
  const TypeExpr* t = storage_expr_type(e);
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
    return EmitStorageDesignator::ordinary_typed_address(
        expr_ops_.expr_to_cxx(e), expr_ops_.expr_to_cxx(*d.operand),
        type_cxx);
  }
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (auto storage = resolved_bytewise_with_field_storage(
            resolve_name_provider_.resolve_name(id.name))) {
      return storage;
    }
    return EmitStorageDesignator::ordinary(expr_ops_.expr_to_cxx(e), type_cxx);
  }
  if (e.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(e);
    const std::string owner = analysis_.deduce_class_alias(*m.base);
    if (owner.empty()) return std::nullopt;
    if (!registry_.lookup_class_field(owner, m.name,
                                       scope_.current_unit_name) &&
        !registry_.lookup_record_field(owner, m.name,
                                        scope_.current_unit_name)) {
      return std::nullopt;
    }
    // Only registered fields reach this fallback. Use their generated member
    // expression address as raw storage for memcpy-style typecast stores such
    // as `byte(obj.EnumField) := value`.
    const std::string text = expr_ops_.expr_to_cxx(*m.base) +
                             member_access_op(*m.base) +
                             registry_.field_cxx_name(m.name);
    return EmitStorageDesignator::ordinary(text, type_cxx);
  }
  return std::nullopt;
}

std::string EmitStorage::reference_class_cast_pointer_cxx(
    const Expr& base_expr) {
  if (base_expr.kind != Kind::Call) return {};
  const auto& call = static_cast<const Call&>(base_expr);
  if (call.args.size() != 1 || call.callee->kind != Kind::Ident) return {};
  const TypeExpr* target_ty =
      canonical_storage_expr_type(base_expr);
  if (!type_is_reference_class(target_ty)) return {};
  const TypeExpr* source_ty = storage_expr_type(*call.args[0]);
  if (!type_is_reference_class(source_ty) && !type_is_pointerish(source_ty)) {
    return {};
  }
  // A reference-class typecast used as a member base changes the object pointer
  // used for `->field`. It is different from a var/out actual like
  // `Take(TChild(p))`, where Pascal reinterprets the caller's pointer slot.
  // Field storage must therefore start from the casted object pointer value,
  // not from the address of the local pointer variable.
  return coerce_pointer_like_text(
      types_.type_to_cxx(*target_ty), target_ty, source_ty,
      expr_ops_.expr_to_cxx(*call.args[0]), /*explicit_pascal_cast=*/true);
}

std::string EmitStorage::storage_designator_value(
    const EmitStorageDesignator& d) {
  if (!d.is_bytewise()) return d.text;
  const char* helper = d.access == EmitStorageAccess::UnalignedBytewise
                           ? "::rt::tp2cc_unaligned_load<"
                           : "::rt::tp2cc_reinterpret_load<";
  return std::string(helper) + d.type_cxx + ">(" + d.ptr_cxx + ")";
}

std::string EmitStorage::storage_designator_member_base(
    const EmitStorageDesignator& d, Location loc) {
  if (!d.is_bytewise()) return d.text;
  if (d.access == EmitStorageAccess::UnalignedBytewise) {
    expr_ops_.report_error(
        loc, "method receiver through unaligned storage is unsupported");
    return "/* invalid unaligned method receiver */";
  }
  // Value reads from byte-addressed storage may copy aggregate payload fields.
  // Object and record methods need Pascal `Self` to name the original slot.
  return reinterpret_ref_text(d.type_cxx, d.ptr_cxx, true);
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
  if (d.is_bytewise()) {
    const bool unaligned = d.access == EmitStorageAccess::UnalignedBytewise;
    const char* helper =
        is_inc ? (unaligned ? "::rt::tp2cc_unaligned_inc"
                            : "::rt::tp2cc_reinterpret_inc")
               : (unaligned ? "::rt::tp2cc_unaligned_dec"
                            : "::rt::tp2cc_reinterpret_dec");
    std::string out = std::string(helper) + "<" + d.type_cxx + ">(" +
                      d.ptr_cxx;
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

  const TypeExpr* elem_type = storage_expr_type(root);
  if (!elem_type) return std::nullopt;
  const std::string elem_cxx = types_.type_to_cxx(*elem_type);

  // `unaligned(expr)` is a storage view. Reuse the same designator path used by
  // `@`, var/out, and assignment so variant payloads and `p^` do not regress to
  // `&(value-load)` or `&tp2cc_deref(p)`.
  if (auto storage = storage_designator(root)) {
    return EmitBytewiseStorage{storage_designator_raw_address(*storage),
                               elem_cxx};
  }

  return EmitBytewiseStorage{"&(" + expr_ops_.expr_to_cxx(root) + ")",
                             elem_cxx};
}

std::optional<EmitBytewiseStorage> EmitStorage::packed_scalar_storage_ref(
    const Expr& e) {
  const TypeExpr* elem_type = canonical_storage_expr_type(e);
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
    const TypeExpr* base_type = storage_expr_type(*m.base);
    if (type_is_packed_record(base_type)) {
      if (auto base = storage_designator(*m.base)) {
        const std::string offset_type =
            offsetof_base_type_cxx(base_type, base->text);
        if (offset_type.empty()) return std::nullopt;
        const std::string field_name = registry_.field_cxx_name(m.name);
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
  const TypeExpr* base_ty = storage_expr_type(*i.base);
  if (!base_ty) return std::nullopt;
  base_ty = analysis_.canonicalize_type(base_ty);
  if (!base_ty || base_ty->kind != Kind::TyArray) return std::nullopt;
  const auto& arr = static_cast<const TyArray&>(*base_ty);
  if (arr.array_kind != ArrayKind::Fixed || arr.dims.size() != 1 ||
      !arr.element) {
    return std::nullopt;
  }
  auto bounds = types_.array_dim_bounds_to_cxx(*arr.dims[0]);
  if (!bounds) {
    return std::nullopt;
  }
  std::string elem_cxx = types_.type_to_cxx(*arr.element);
  const std::string index_cxx = expr_ops_.expr_value_to_cxx(*i.indices[0]);
  const std::string offset =
      "((" + index_cxx + ") - (" + bounds->low + ")) * sizeof(" + elem_cxx +
      ")";
  std::string ptr_cxx = "::rt::tp2cc_byte_offset(" +
                        expr_ops_.expr_to_cxx(*cast.args[0]) + ", " + offset +
                        ")";
  return EmitUntypedStorageIndexView(std::move(elem_cxx), std::move(ptr_cxx));
}

bool EmitStorage::type_is_packed_record(const TypeExpr* t) {
  if (!t) return false;
  if (t->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*t);
    if (const TypeSymbol* symbol =
            local_type_symbol(registry_, scope_, n.name);
        symbol && symbol->record_info()) {
      return symbol->record_info()->is_packed;
    }
    const TypeSymbol* symbol =
        registry_.lookup_type_symbol(n.name, scope_.current_unit_name);
    if (symbol && symbol->record_info()) {
      const RecordInfo* record = symbol->record_info();
      if (record) return record->is_packed;
    }
  }
  t = analysis_.canonicalize_type(t);
  return t && t->kind == Kind::TyRecord &&
         static_cast<const TyRecord&>(*t).is_packed;
}

bool EmitStorage::type_is_direct_packed_aggregate(const TypeExpr* t) {
  if (!t) return false;
  if (t->kind == Kind::TyName) {
    const std::string low = ascii_lower(static_cast<const TyName&>(*t).name);
    const TypeSymbol* symbol = local_type_symbol(registry_, scope_, low);
    if (!symbol) {
      symbol = registry_.lookup_type_symbol(low, scope_.current_unit_name);
    }
    if (!low.empty() && symbol &&
        (symbol->record_info() || symbol->class_info())) {
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
  // That helper is for storage contexts and may take `&(expr)`.
  // For `packed_record.aggregate.scalar`, `&(expr)` would already have formed
  // the forbidden intermediate aggregate lvalue. Here we instead build
  // `offsetof` sums over the member chain and byte-load from the base object.
  // A packed aggregate subfield can be read safely as bytes, but binding a
  // C++ reference to the intermediate packed aggregate would not be safe.
  if (e.kind != Kind::Member) return std::nullopt;

  std::vector<const Member*> chain;
  const Expr* root = &e;
  while (root && root->kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(*root);
    chain.push_back(&m);
    root = m.base.get();
  }
  if (!root || chain.empty()) return std::nullopt;
  std::reverse(chain.begin(), chain.end());

  const TypeExpr* current_type = storage_expr_type(*root);
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
                      registry_.field_cxx_name(m->name) + ")");

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
  if (e.kind != Kind::Member) return std::nullopt;
  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = storage_expr_type(*m.base);
  if (!type_is_packed_record(base_type)) return std::nullopt;
  const TypeExpr* field_type =
      analysis_.lookup_record_field_type_in_type(base_type, m.name);
  if (!field_type || !type_is_direct_packed_aggregate(field_type)) {
    return std::nullopt;
  }
  std::string record_name =
      registry_.direct_type_name(base_type, scope_.current_unit_name);
  if (record_name.empty()) record_name = "packed record";
  return EmitPackedAggregateFieldUse{record_name, m.name};
}

std::optional<EmitPackedAggregateFieldUse>
EmitStorage::packed_aggregate_path_use(const Expr& e) {
  for (const Expr* cur = &e; cur && cur->kind == Kind::Member;
       cur = static_cast<const Member&>(*cur).base.get()) {
    const auto& m = static_cast<const Member&>(*cur);
    if (auto use = direct_packed_aggregate_field_use(*m.base)) return use;
  }
  return std::nullopt;
}

bool EmitStorage::variant_payload_path_use(const Expr& e) {
  if (e.kind == Kind::Index) {
    return variant_payload_path_use(*static_cast<const Index&>(e).base);
  }
  if (e.kind != Kind::Member) return false;

  const auto& m = static_cast<const Member&>(e);
  const TypeExpr* base_type = storage_expr_type(*m.base);
  if (analysis_.record_field_is_variant_in_type(base_type, m.name)) {
    return true;
  }
  return variant_payload_path_use(*m.base);
}

bool EmitStorage::member_value_may_need_storage_designator(const Expr& e) {
  if (e.kind != Kind::Member) return false;
  for (const Expr* cur = &e; cur && cur->kind == Kind::Member;) {
    const auto& m = static_cast<const Member&>(*cur);
    if (!m.base) return false;
    switch (m.base->kind) {
      case Kind::Index:
        if (index_base_denotes_property_value(
                static_cast<const Index&>(*m.base))) {
          // `Items[i].Field` may look like field selection from an array slot,
          // but an indexed property is selected by accessor semantics first.
          // Probing it as storage would lower the bare property name before the
          // property getter has a chance to produce the object value.
          return false;
        }
        [[fallthrough]];
      case Kind::Call:
      case Kind::Deref:
        // Typecast/unaligned views, pointer dereferences, and real indexed
        // storage bases may already be byte-addressed. Let the storage
        // designator decide whether the final member read is a bytewise value
        // load.
        return true;
      default:
        cur = m.base.get();
        break;
    }
  }
  return variant_payload_path_use(e);
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
  if (!is_lvalue_expr) return is_lvalue_expr;

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
      if (registry_.lookup_class_property(
              cls, m.name, scope_.current_unit_name)) return false;
      if (registry_.lookup_class_methods(cls, m.name,
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
  const TypeExpr* t = storage_expr_type(e);
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  return tyname_is_charish(t);
}

bool EmitStorage::type_is_pcharish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (t == named_pascal_type("pchar") || t == named_pascal_type("pansichar")) {
    return true;
  }
  if (t->kind != Kind::TyPointer) return false;
  const TypeExpr* target =
      analysis_.canonicalize_type(static_cast<const TyPointer&>(*t).target.get());
  return tyname_is_charish(target);
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
    if (id.name == "self" && !scope_.current_class_name.empty()) {
      const ClassInfo* ci =
          registry_.lookup_class(scope_.current_class_name,
                                  scope_.current_unit_name);
      return ci && ci->is_reference_type;
    }
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      if (analysis_.is_builtin_reference_class_name(id.name)) return true;
      const ClassInfo* ci =
          registry_.lookup_class(id.name, scope_.current_unit_name);
      if (ci && ci->is_reference_type) {
        return true;
      }
    }
  }
  return type_is_reference_class(storage_expr_type(e));
}

std::string EmitStorage::member_access_op(const Expr& e) {
  const TypeExpr* t = storage_expr_type(e);
  if (expr_is_reference_class(e) || type_is_pointerish(t)) return "->";
  if (t) {
    std::string cxx = types_.type_to_cxx(*t);
    if (!cxx.empty() && cxx.back() == '*') return "->";
  }
  return ".";
}

bool EmitStorage::type_is_stringish(const TypeExpr* t) {
  return analysis_.type_is_string_like(t);
}

bool EmitStorage::type_is_pointerish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (type_is_metaclass(t)) return true;
  if (type_is_reference_class(t)) return true;
  if (analysis_.type_is_interface(t)) return true;
  if (t->kind == Kind::TyPointer) return true;
  if (is_runtime_pointer_primitive_type(t)) return true;
  return false;
}

bool EmitStorage::type_is_open_array(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.canonicalize_type(t);
  return t && t->kind == Kind::TyArray &&
         static_cast<const TyArray&>(*t).array_kind == ArrayKind::Open;
}

bool EmitStorage::fixed_array_pointer_can_decay_to_element_pointer(
    const TypeExpr* src_type, const TypeExpr* dst_type) {
  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  const TypeExpr* dst = analysis_.canonicalize_type(dst_type);
  if (!src || src->kind != Kind::TyPointer || !dst) return false;

  const TypeExpr* src_pointee = analysis_.canonicalize_type(
      static_cast<const TyPointer&>(*src).target.get());
  if (!src_pointee || src_pointee->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*src_pointee).array_kind !=
          ArrayKind::Fixed) {
    return false;
  }
  const TypeExpr* src_elem = analysis_.canonicalize_type(
      static_cast<const TyArray&>(*src_pointee).element.get());
  if (!src_elem) return false;

  const TypeExpr* dst_elem = nullptr;
  if (dst->kind == Kind::TyPointer) {
    dst_elem = analysis_.canonicalize_type(
        static_cast<const TyPointer&>(*dst).target.get());
  } else if (type_is_pcharish(dst)) {
    dst_elem = builtin_char_type();
  }
  if (!dst_elem) return false;
  return types_.type_to_cxx(*src_elem) == types_.type_to_cxx(*dst_elem);
}

bool EmitStorage::fixed_char_array_value_can_decay_to_pchar(
    const TypeExpr* src_type, const TypeExpr* dst_type) {
  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  if (!src || src->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*src).array_kind != ArrayKind::Fixed) {
    return false;
  }
  const auto& arr = static_cast<const TyArray&>(*src);
  if (arr.dims.size() != 1 || !type_is_pcharish(dst_type)) return false;
  const TypeExpr* elem = analysis_.canonicalize_type(arr.element.get());
  return tyname_is_charish(elem);
}

std::string EmitStorage::lower_pointer_to_fixed_array_to_element(
    const TypeExpr* src_type, const std::string& source_cxx) {
  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  if (!src || src->kind != Kind::TyPointer) return source_cxx;
  const TypeExpr* pointee = analysis_.canonicalize_type(
      static_cast<const TyPointer&>(*src).target.get());
  if (!pointee || pointee->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*pointee).array_kind != ArrayKind::Fixed) {
    return source_cxx;
  }
  return "(" + source_cxx + ")->data";
}

std::string EmitStorage::lower_fixed_char_array_value_to_pchar(
    const TypeExpr* src_type, const TypeExpr* dst_type,
    const std::string& source_cxx) {
  if (!fixed_char_array_value_can_decay_to_pchar(src_type, dst_type)) {
    return source_cxx;
  }
  return "(" + source_cxx + ").data";
}

bool EmitStorage::pointer_value_conversion_is_valid(
    const TypeExpr* dst_type, const TypeExpr* src_type,
    bool explicit_pascal_cast) {
  const TypeExpr* raw_dst = dst_type;
  const TypeExpr* raw_src = src_type;
  const TypeExpr* dst = analysis_.canonicalize_type(dst_type);
  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  if (!dst || !src) return false;

  const bool dst_proc = is_nonmethod_procedural_type(dst);
  const bool src_proc = is_nonmethod_procedural_type(src);
  const bool dst_ptr = type_is_pointerish(dst);
  const bool src_ptr = type_is_pointerish(src);
  if (!(dst_ptr || dst_proc) || !(src_ptr || src_proc)) return false;

  if (types_.type_to_cxx(*dst) == types_.type_to_cxx(*src)) return true;

  const bool dst_void_ptr = is_plain_pointer_type(dst);
  const bool src_void_ptr = is_plain_pointer_type(src);
  if (fixed_array_pointer_can_decay_to_element_pointer(src, dst)) return true;
  if (pointer_to_object_upcast_is_valid(raw_dst, raw_src)) return true;
  if (class_to_interface_conversion_is_valid(raw_dst, raw_src)) return true;
  if ((dst_proc && src_void_ptr) || (dst_void_ptr && src_proc)) return true;
  if (src_void_ptr || dst_void_ptr) return true;

  const bool dst_ref_class = type_is_reference_class(dst);
  const bool src_ref_class = type_is_reference_class(src);
  if (dst_ref_class && src_ref_class) {
    std::string dst_name = reference_class_name(raw_dst);
    if (dst_name.empty()) dst_name = reference_class_name(dst);
    std::string src_name = reference_class_name(raw_src);
    if (src_name.empty()) src_name = reference_class_name(src);
    if (!dst_name.empty() && !src_name.empty() &&
        pascal_parent_chain_contains(dst_name, src_name)) {
      return true;
    }
    return explicit_pascal_cast;
  }

  return explicit_pascal_cast;
}

std::string EmitStorage::coerce_pointer_like_text(std::string_view dst_cxx_text,
                                                  const TypeExpr* dst_type,
                                                  const TypeExpr* src_type,
                                                  const std::string& source_cxx,
                                                  bool explicit_pascal_cast,
                                                  bool source_is_const_storage) {
  if (!pointer_value_conversion_is_valid(dst_type, src_type,
                                         explicit_pascal_cast)) {
    return source_cxx;
  }
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
  // Compare canonical C++ type names here. Use-site casts may request an alias
  // type name (`p_tchildclass`) even when both sides already canonicalize to
  // the same underlying metaclass/pointer type; emitting another cast around
  // an already-correct explicit Pascal cast only creates review-noise.
  if (canonical_dst_cxx == src_cxx || dst_cxx == src_cxx) return source_cxx;

  const bool dst_void_ptr = is_plain_pointer_type(dst);
  const bool src_void_ptr = is_plain_pointer_type(src);

  // `^fixed_array of T -> ^T`: lower via the shared element-pointer helper.
  // The destination must be a typed pointer whose pointee matches the source
  // array's element type; otherwise the conversion is not value-preserving.
  if (fixed_array_pointer_can_decay_to_element_pointer(src, dst)) {
    return lower_pointer_to_fixed_array_to_element(src, source_cxx);
  }

  if (pointer_to_object_upcast_is_valid(dst_type, src_type)) {
    return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }
  if (class_to_interface_conversion_is_valid(dst_type, src_type)) {
    return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }

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
    std::string dst_name = reference_class_name(dst_type);
    if (dst_name.empty()) dst_name = reference_class_name(dst);
    std::string src_name = reference_class_name(src_type);
    if (src_name.empty()) src_name = reference_class_name(src);
    const bool related_upcast =
        !dst_name.empty() && !src_name.empty() &&
        pascal_parent_chain_contains(dst_name, src_name);
    const bool related_downcast =
        !dst_name.empty() && !src_name.empty() &&
        pascal_parent_chain_contains(src_name, dst_name);
    if (related_upcast || (explicit_pascal_cast && related_downcast)) {
      return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
    }
    if (!explicit_pascal_cast) return source_cxx;
  }

  if (explicit_pascal_cast || (dst_ptr && src_ptr)) {
    return "reinterpret_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }
  return source_cxx;
}

bool EmitStorage::pointer_to_object_upcast_is_valid(const TypeExpr* dst_type,
                                                    const TypeExpr* src_type) {
  const std::string dst_name =
      registry_.pointer_target_type_name(dst_type, scope_.current_unit_name);
  const std::string src_name =
      registry_.pointer_target_type_name(src_type, scope_.current_unit_name);
  if (dst_name.empty() || src_name.empty()) return false;
  const ClassInfo* dst_class =
      analysis_.class_info_for_type_name(dst_name);
  const ClassInfo* src_class =
      analysis_.class_info_for_type_name(src_name);
  if (!dst_class || !src_class) return false;
  if (dst_class->is_reference_type || src_class->is_reference_type) {
    return false;
  }
  return pascal_parent_chain_contains(dst_name, src_name);
}

bool EmitStorage::class_to_interface_conversion_is_valid(
    const TypeExpr* dst_type, const TypeExpr* src_type) {
  const InterfaceInfo* interface =
      registry_.interface_info_for_type(dst_type, scope_.current_unit_name);
  if (!interface) return false;

  const TypeExpr* src = analysis_.canonicalize_type(src_type);
  if (!type_is_reference_class(src)) return false;

  std::string class_name = reference_class_name(src_type);
  if (class_name.empty()) class_name = reference_class_name(src);
  if (class_name.empty()) return false;
  return registry_.class_implements_interface(class_name, *interface,
                                               scope_.current_unit_name);
}

bool EmitStorage::pascal_parent_chain_contains(std::string_view ancestor,
                                               std::string current) {
  const std::string ancestor_key = ascii_lower(std::string(ancestor));
  while (!current.empty()) {
    if (ascii_lower(current) == ancestor_key) return true;
    const ClassInfo* info = analysis_.class_info_for_type_name(current);
    if (!info) break;
    current = info->parent;
  }
  return false;
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
  const std::string target_cxx =
      resolve_name_provider_.resolve_name(vd.absolute_target).cxx;

  if (scope_.local_untyped_params.count(vd.absolute_target)) {
    return EmitAbsoluteTargetInfo::untyped_param(target_cxx);
  }

  auto lit = scope_.local_consts.find(vd.absolute_target);
  if (lit != scope_.local_consts.end()) {
    if (!lit->second || !lit->second->type) {
      expr_ops_.report_error(vd.loc,
                             "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    return absolute_target_info(target_cxx, lit->second->type.get());
  }

  auto tit = scope_.local_types.find(vd.absolute_target);
  if (tit != scope_.local_types.end()) {
    return absolute_target_info(
        target_cxx, tit->second,
        scope_.local_const_params.count(vd.absolute_target) > 0);
  }

  ResolveResult rr = resolve_name_provider_.resolve_name(vd.absolute_target);
  if (rr.kind == ResolvedKind::ClassField && !scope_.current_class_name.empty()) {
    if (auto* f = registry_.lookup_class_field(scope_.current_class_name,
                                                vd.absolute_target,
                                                scope_.current_unit_name)) {
      return absolute_target_info(target_cxx, f->type.get());
    }
  }

  if (const auto* v = analysis_.find_visible_unit_var(vd.absolute_target)) {
    return absolute_target_info(target_cxx, v->type.get());
  }

  if (const auto* c = analysis_.find_visible_unit_const(vd.absolute_target)) {
    if (!c->type) {
      expr_ops_.report_error(vd.loc,
                             "absolute target must be a variable or typed const");
      return std::nullopt;
    }
    return absolute_target_info(target_cxx, c->type.get());
  }

  expr_ops_.report_error(vd.loc,
                         "absolute target must be a variable or typed const");
  return std::nullopt;
}

EmitAbsoluteTargetInfo EmitStorage::absolute_target_info(
    const std::string& target_cxx, const TypeExpr* type,
    bool is_const_storage) {
  return EmitAbsoluteTargetInfo{target_cxx, type, type_is_pointerish(type),
                                is_const_storage};
}

}  // namespace tp2cc
