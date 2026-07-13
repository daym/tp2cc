#include "emit_storage.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>
#include <vector>

#include "emit_analysis.h"
#include "emit_signature_scope.h"
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

bool is_plain_pointer_type(EmitAnalysis& analysis, const TypeExpr* t) {
  if (const PrimitiveInfo* primitive = analysis.primitive_info_for_type(t);
      primitive && primitive->kind == PrimitiveKind::Pointer) {
    return true;
  }
  t = analysis.semantic_shape_type(t);
  return t && t->kind == Kind::TyPointer &&
         !static_cast<const TyPointer&>(*t).target;
}

const ClassInfo* class_info_for_value(EmitAnalysis& analysis, const Expr& e) {
  const TypeSymbol* symbol = analysis.deduce_class_symbol(e);
  return symbol ? symbol->class_info() : nullptr;
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
  return analysis_.semantic_shape_type(storage_expr_type(e));
}

std::string EmitStorage::offsetof_base_type_cxx(
    const TypeExpr* t, const std::string& base_expr_cxx) {
  // `offsetof` needs the containing C++ aggregate type, not a Pascal spelling.
  // If semantic binding resolved the source type, use that symbol directly; otherwise
  // anonymous record/object variables fall back to `decltype(base_expr_cxx)`.
  if (t) {
    if (const TypeSymbol* symbol = registry_.resolved_symbol_for_type(t)) {
      if (symbol->record_info() || symbol->class_info() ||
          symbol->interface_info()) {
        if (symbol->defining_unit == "__rt__" ||
            symbol->defining_unit == "__builtin__") {
          if (std::string cxx = types_.type_symbol_to_cxx(symbol);
              !cxx.empty()) {
            return cxx;
          }
        }
        return types_.type_symbol_struct_cxx(*symbol);
      }
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

  auto target = storage_typecast_target(outer, *outer.args[0]);
  if (!target) return std::nullopt;

  const Expr* source = outer.args[0].get();
  while (source && source->kind == Kind::Call) {
    const auto& nested = static_cast<const Call&>(*source);
    if (nested.args.size() != 1 || nested.callee->kind != Kind::Ident) break;
    if (!storage_typecast_target(nested, *nested.args[0])) break;
    source = nested.args[0].get();
  }
  if (!source) return std::nullopt;

  if (source->kind == Kind::Ident) {
    ResolveResult rr = resolve_name_provider_.resolve_name(
        static_cast<const Ident&>(*source).name);
    if (rr.kind == ResolvedKind::UnitConst ||
        rr.kind == ResolvedKind::EnumMember ||
        rr.is_callable) {
      return std::nullopt;
    }
  }

  const bool untyped_storage = expr_is_untyped_storage_ref(*source);
  std::optional<EmitStorageDesignator> source_storage =
      untyped_storage ? std::nullopt : storage_designator(*source);
  // Storage syntax alone is insufficient: an indexed property can be a getter
  // result even though its AST is an Index. Only a concrete designator or an
  // untyped caller-storage parameter proves that a cast denotes source bytes.
  // `unaligned(x)` is handled by storage_designator() and therefore still
  // reaches this path with a concrete bytewise designator.
  if (!untyped_storage && !source_storage) {
    return std::nullopt;
  }
  std::string source_cxx = expr_ops_.expr_to_cxx(*source);
  std::string source_ptr_cxx =
      untyped_storage
          ? source_cxx
          : (source_storage
                 ? storage_designator_raw_address(*source_storage)
                 : typecast_source_raw_pointer(*source, source_cxx, false));
  std::string backing_ptr_cxx;
  std::string backing_type_cxx;
  if (source_storage) {
    if (source_storage->is_bytewise()) {
      backing_ptr_cxx = source_storage->backing_ptr_cxx;
      backing_type_cxx = source_storage->backing_type_cxx;
    } else {
      backing_ptr_cxx = source_ptr_cxx;
      backing_type_cxx = source_storage->type_cxx;
    }
  }
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
                                 untyped_storage, std::move(backing_ptr_cxx),
                                 std::move(backing_type_cxx));
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
    const TypeExpr* canon = analysis_.semantic_shape_type(source_type);
    const PrimitiveInfo* primitive = analysis_.primitive_info_for_type(canon);
    if (primitive && (primitive->kind == PrimitiveKind::Byte ||
                      primitive->kind == PrimitiveKind::ShortInt)) {
      return true;
    }
  }
  if (source_type->kind == Kind::TySubrange) {
    auto domain = analysis_.ordinal_domain_for_type(source_type);
    if (!domain || domain->family != OrdinalFamily::Integer) return false;
    return (domain->low >= 0 && domain->high <= 255) ||
           (domain->low >= -128 && domain->high <= 127);
  }
  return false;
}

std::optional<EmitStorage::StorageCastTarget>
EmitStorage::storage_typecast_target(const Call& call, const Expr& source) {
  if (!call.callee || call.callee->kind != Kind::Ident) return std::nullopt;
  const auto& id = static_cast<const Ident&>(*call.callee);
  assert(pascal_key_is_canonical(id.name));
  if (id.name == "ord") {
    if (std::string target = ord_storage_target_cxx(source); !target.empty()) {
      return StorageCastTarget{std::move(target), nullptr, true};
    }
    return std::nullopt;
  }
  if (id.name == "chr") {
    if (chr_source_has_byte_storage(source)) {
      const TypeDescriptor* result =
          registry_.expression_result_descriptor(&call);
      return StorageCastTarget{"::rt::p_char",
                               result ? result->type : nullptr, true};
    }
    return std::nullopt;
  }
  if (const TypeSymbol* symbol =
          analysis_.explicit_typecast_target_symbol(call)) {
    const PrimitiveInfo* primitive =
        symbol->descriptor ? symbol->descriptor->primitive : nullptr;
    if (primitive) {
      return StorageCastTarget{primitive->cxx, nullptr, true};
    }
    return StorageCastTarget{types_.type_symbol_to_cxx(symbol), symbol->type,
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
  t = analysis_.semantic_shape_type(t);
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
  t = analysis_.semantic_shape_type(t);
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
  base_type = analysis_.semantic_shape_type(base_type);
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
                                                     "::rt::p_char",
                                                     base.backing_ptr_cxx,
                                                     base.backing_type_cxx);
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
                                                 elem_cxx,
                                                 base.backing_ptr_cxx,
                                                 base.backing_type_cxx);
}

bool EmitStorage::index_base_denotes_property_value(const Index& i) {
  if (i.base->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*i.base);
    const ClassInfo* cls = class_info_for_value(analysis_, *mem.base);
    if (cls && registry_.lookup_class_property(*cls, mem.name)) {
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

  const ClassInfo* cls = class_info_for_value(analysis_, *i.base);
  return cls && registry_.lookup_default_property(*cls);
}

std::optional<EmitStorageDesignator> EmitStorage::absolute_alias_designator(
    const Ident& id) {
  auto ait = scope_.local_absolute_targets.find(id.name);
  if (ait == scope_.local_absolute_targets.end()) return std::nullopt;

  const auto& alias = ait->second;
  const std::string alias_type_cxx =
      alias.type ? types_.type_to_cxx(*alias.type) : std::string{};
  // `absolute` aliases the target variable's bytes. A typed Pointer variable
  // therefore contributes its pointer slot, not the object it points at.
  // Untyped parameters are the exception because their generated void* value
  // already is the caller's storage address.
  std::string ptr_expr =
      alias.target_value_is_storage_address
          ? alias.target_cxx
          : "(&(" + alias.target_cxx + "))";
  const std::string backing_type_cxx =
      !alias.target_value_is_storage_address && alias.target_type
          ? types_.type_to_cxx(*alias.target_type)
          : std::string{};
  const std::string backing_ptr_cxx =
      backing_type_cxx.empty() ? std::string{} : ptr_expr;
  return EmitStorageDesignator::bytewise(
      std::move(ptr_expr), alias_type_cxx, backing_ptr_cxx,
      backing_type_cxx);
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
      if (type_is_reference_class(view->target_type)) {
        return std::nullopt;
      }
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
                packed->void_ptr_text, view->target_cxx,
                view->backing_ptr_cxx, view->backing_type_cxx);
          }
        }
        // Primitive storage views only need the source bytes. Model them as
        // byte-addressed storage so assignment and Inc/Dec use memcpy helpers
        // instead of binding a C++ reference of the cast target type.
        return view->source_is_unaligned_bytewise_storage
                   ? EmitStorageDesignator::unaligned_bytewise(
                         view->source_ptr_cxx, view->target_cxx,
                         view->backing_ptr_cxx, view->backing_type_cxx)
                   : EmitStorageDesignator::bytewise(view->source_ptr_cxx,
                                                     view->target_cxx,
                                                     view->backing_ptr_cxx,
                                                     view->backing_type_cxx);
      }
      // A typed storage view still names the source bytes. Reads and writes
      // must use byte-copy helpers; binding a C++ reference would require a
      // live object of the target type at that address.
      if (view->source_is_unaligned_bytewise_storage) {
        return EmitStorageDesignator::unaligned_bytewise(
            view->source_ptr_cxx, view->target_cxx,
            view->backing_ptr_cxx, view->backing_type_cxx);
      }
      return EmitStorageDesignator::bytewise(view->source_ptr_cxx,
                                             view->target_cxx,
                                             view->backing_ptr_cxx,
                                             view->backing_type_cxx);
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
      const ClassInfo* owner_class = class_info_for_value(analysis_, *m.base);
      std::string member_cxx = mangle(m.name);
      bool field_backed_property = false;
      if (owner_class) {
        if (const auto* prop =
                registry_.lookup_class_property(*owner_class, m.name)) {
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
        } else if (registry_.lookup_class_field(*owner_class, m.name)) {
          member_cxx = registry_.field_cxx_name(m.name);
        } else if (registry_.lookup_class_methods(*owner_class, m.name)) {
          // Storage designators are field/property paths. A method selected
          // from a byte-addressed variant payload must fall back to expression
          // lowering; otherwise the offset path would treat the method name as
          // a C++ data member and emit `offsetof(T, method)`.
          return std::nullopt;
        }
      } else if (analysis_.lookup_record_field_type_in_type(base_type,
                                                            m.name)) {
        member_cxx = registry_.field_cxx_name(m.name);
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
        if (type_is_packed_record(offset_base_type)) {
          return EmitStorageDesignator::unaligned_bytewise(field_ptr_cxx,
                                                           field_cxx,
                                                           base->backing_ptr_cxx,
                                                           base->backing_type_cxx);
        }
        return EmitStorageDesignator::raw_byte_address(base->access, {},
                                                       field_ptr_cxx,
                                                       field_cxx,
                                                       base->backing_ptr_cxx,
                                                       base->backing_type_cxx);
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

      if (base->is_special()) {
        // Pointer-backed bases such as `p^` and reference-class casts already
        // know the address of the containing Pascal object. Propagate that
        // address through the field offset so `@base.field` and var/out
        // arguments use the field's address without manufacturing a C++ field
        // reference first.
        return EmitStorageDesignator::raw_byte_address(base->access, text,
                                                       field_ptr_cxx,
                                                       field_cxx,
                                                       base->backing_ptr_cxx,
                                                       base->backing_type_cxx);
      }

      if (!base->ptr_cxx.empty() && !field_ptr_cxx.empty()) {
        // An ordinary typed pointer dereference denotes a live C++ object.
        // Keep its field as an ordinary lvalue while retaining the direct
        // field address for `@`, var/out, and untyped arguments. Converting it
        // to RawBytePointer here would turn managed fields into memcpy-loaded
        // temporaries and lose mutations.
        return EmitStorageDesignator::ordinary_typed_address(
            text, "static_cast<" + field_cxx + "*>(" + field_ptr_cxx + ")",
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
      const std::string text = found->base_cxx + found->base_access +
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
    // Pascal `Ptype(p)^` where Ptype's pointee is not related to `p`'s
    // pointee through a C++ inheritance chain. Bytewise routing prevents
    // `*(Ptype)p` strict-aliasing UB by loading/storing through memcpy
    // helpers; downstream field/index access composes byte offsets on top.
    if (auto bytewise = pointer_typecast_deref_as_bytewise(d)) {
      return bytewise;
    }
    // `p^` is an ordinary typed Pascal lvalue for reads and writes, but Pascal
    // address-of cancels the dereference: `@p^` is the operand pointer
    // expression. This matters for untyped raw-memory calls such as
    // `Move(p^, q^, 0)`: Pascal only asks for a storage address, while
    // `&tp2cc_deref(p)` first forms a C++ reference and is already invalid if
    // `p` is nil even though the byte count is zero.
    std::string pointee_cxx = expr_ops_.expr_to_cxx(e);
    std::optional<std::string> explicit_plain_pointer_cast_value;
    if (d.operand && d.operand->kind == Kind::Call) {
      const TypeExpr* cast_target_type =
          analysis_.explicit_typecast_result_type(*d.operand);
      const bool cast_is_plain_pointer =
          is_plain_pointer_type(analysis_, cast_target_type);
      if (cast_is_plain_pointer) {
        const auto& cast = static_cast<const Call&>(*d.operand);
        if (cast.args.size() == 1 && cast.args[0]) {
          const TypeExpr* source_type = analysis_.deduce_type(*cast.args[0]);
          const TypeSymbol* source_class_symbol =
              analysis_.deduce_class_symbol(*cast.args[0]);
          const ClassInfo* source_class =
              source_class_symbol ? source_class_symbol->class_info() : nullptr;
          const bool source_is_pointer_value =
              (source_class && source_class->is_reference_type) ||
              type_is_pointerish(source_type);
          // `Pointer(x)^` has no declared pointee type, but it still denotes
          // storage at the pointer value. If `x` is already pointer-valued,
          // use `x`; otherwise use the rendered explicit cast that produces
          // the pointer bits.
          explicit_plain_pointer_cast_value =
              source_is_pointer_value ? expr_ops_.expr_to_cxx(*cast.args[0])
                                      : expr_ops_.expr_to_cxx(*d.operand);
        }
      }
    }
    std::string pointer_cxx = explicit_plain_pointer_cast_value
                                  ? *explicit_plain_pointer_cast_value
                                  : expr_ops_.expr_to_cxx(*d.operand);
    if (!explicit_plain_pointer_cast_value) {
      if (auto pointer_storage = storage_designator(*d.operand);
          pointer_storage && pointer_storage->is_bytewise() &&
          type_is_pointerish(storage_expr_type(*d.operand))) {
      // `@p^` returns the pointer value stored in `p`. If `p` is itself a
      // byte-addressed slot, such as a Pascal variant-record payload, the
      // address of the pointee is the slot's loaded pointer value, not the
      // slot's storage address.
        pointer_cxx = storage_designator_value(*pointer_storage);
        pointee_cxx = "::rt::tp2cc_deref(" + pointer_cxx + ")";
      }
    }
    return EmitStorageDesignator::ordinary_typed_address(
        pointee_cxx, pointer_cxx, type_cxx);
  }
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    // Pascal `absolute` alias: no C++ variable exists for this name. Return
    // a bytewise designator pointing at the source's address so downstream
    // reads/writes use memcpy helpers instead of forming a typed reference
    // over storage whose C++ dynamic type differs from the alias type.
    if (auto alias = absolute_alias_designator(id)) return alias;
    if (auto storage = resolved_bytewise_with_field_storage(
            resolve_name_provider_.resolve_name(id.name))) {
      return storage;
    }
    return EmitStorageDesignator::ordinary(expr_ops_.expr_to_cxx(e), type_cxx);
  }
  if (e.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(e);
    const TypeSymbol* owner_symbol = analysis_.deduce_class_symbol(*m.base);
    const ClassInfo* owner_class =
        owner_symbol ? owner_symbol->class_info() : nullptr;
    const bool class_field =
        owner_class && registry_.lookup_class_field(*owner_class, m.name);
    const TypeExpr* base_type =
        owner_class ? nullptr : analysis_.deduce_type(*m.base);
    const bool record_field =
        !owner_class &&
        analysis_.lookup_record_field_type_in_type(base_type, m.name);
    if (!class_field && !record_field) {
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

std::optional<EmitStorageDesignator>
EmitStorage::mutable_typecast_slot_designator(const Expr& e) {
  auto view = typecast_storage_view(e);
  if (!view || view->target_is_primitive ||
      !type_is_reference_class(view->target_type)) {
    return std::nullopt;
  }
  const TypeExpr* source_type =
      view->source ? storage_expr_type(*view->source) : nullptr;
  bool source_is_untyped_pointer_deref = false;
  if (view->source && view->source->kind == Kind::Deref) {
    const auto& deref = static_cast<const Deref&>(*view->source);
    const TypeExpr* pointer_type =
        deref.operand ? analysis_.deduce_type(*deref.operand) : nullptr;
    pointer_type = analysis_.semantic_shape_type(pointer_type);
    source_is_untyped_pointer_deref =
        pointer_type && pointer_type->kind == Kind::TyPointer &&
        !static_cast<const TyPointer&>(*pointer_type).target;
  }
  if (!type_is_reference_class(source_type) &&
      !type_is_pointerish(source_type) &&
      !source_is_untyped_pointer_deref) {
    return std::nullopt;
  }

  // An explicit Pascal class-reference cast used as a var/out actual changes
  // the type view of the caller's pointer slot for this call. That is not the
  // same operation as `TBase(x).field`, which casts the object pointer value
  // before member selection. Keep this query separate from the general storage
  // designator so member/property lowering continues to use pointer values.
  if (view->source_is_unaligned_bytewise_storage) {
    return EmitStorageDesignator::unaligned_bytewise(view->source_ptr_cxx,
                                                    view->target_cxx,
                                                    view->backing_ptr_cxx,
                                                    view->backing_type_cxx);
  }
  return EmitStorageDesignator::bytewise(view->source_ptr_cxx,
                                         view->target_cxx,
                                         view->backing_ptr_cxx,
                                         view->backing_type_cxx);
}

std::string EmitStorage::reference_class_cast_pointer_cxx(
    const Expr& base_expr) {
  if (base_expr.kind != Kind::Call) return {};
  const auto& call = static_cast<const Call&>(base_expr);
  if (call.args.size() != 1) return {};
  const TypeSymbol* target_symbol =
      analysis_.explicit_typecast_target_symbol(base_expr);
  const TypeExpr* target_ty = target_symbol
                                  ? target_symbol->type
                                  : analysis_.explicit_typecast_result_type(
                                        base_expr);
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
      target_symbol ? types_.type_symbol_to_cxx(target_symbol)
                    : types_.type_to_cxx(*target_ty),
      target_ty, source_ty,
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
    const EmitStorageDesignator& d) {
  if (!d.is_bytewise()) return d.text;
  return storage_designator_typed_lvalue(d);
}

std::string EmitStorage::storage_designator_typed_lvalue(
    const EmitStorageDesignator& d) {
  return storage_designator_typed_lvalue(d, d.type_cxx);
}

std::string EmitStorage::storage_designator_typed_lvalue(
    const EmitStorageDesignator& d, std::string_view type_cxx) {
  const std::string address = storage_designator_raw_address(d);
  if (!d.backing_type_cxx.empty() && !d.backing_ptr_cxx.empty()) {
    return "(*::rt::tp2cc_ScopedStorageView<" + std::string(type_cxx) +
           ", " + d.backing_type_cxx + ">(" + address + ", " +
           d.backing_ptr_cxx + "))";
  }
  if (!d.is_bytewise() && !d.type_cxx.empty() &&
      d.type_cxx != type_cxx) {
    return "(*::rt::tp2cc_ScopedStorageView<" + std::string(type_cxx) +
           ", " + d.type_cxx + ">(" + address + ", " + address + "))";
  }
  if (!d.is_bytewise() && d.type_cxx == type_cxx) return d.text;
  return "::rt::tp2cc_storage_ref<" + std::string(type_cxx) + ">(" +
         address + ")";
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
    const EmitStorageDesignator& d, Location where) {
  const std::string address = storage_designator_raw_address(d);
  if (address.empty() || d.type_cxx.empty()) return address;
  if (d.raw_address_needs_typed_cast()) {
    if (d.access == EmitStorageAccess::Ordinary &&
        d.ptr_form == EmitStorageAddressForm::RawBytePointer) {
      // Address composition through an ordinary typed pointer, such as
      // `@p^[i]`, does not access the pointee and is valid when p is nil and
      // the offset is zero. Keep it as a pointer value instead of starting an
      // object lifetime by binding a reference at the computed address.
      return "static_cast<" + d.type_cxx + "*>(" + address + ")";
    }
    if (d.access == EmitStorageAccess::UnalignedBytewise) {
      expr_ops_.report_error(
          where, "cannot take a typed address of unaligned Pascal storage");
      return "static_cast<" + d.type_cxx + "*>(nullptr)";
    }
    if (!d.backing_type_cxx.empty()) {
      expr_ops_.report_error(
          where,
          "typed address of a temporary Pascal storage view would escape its "
          "backing object's lifetime");
      return "static_cast<" + d.type_cxx + "*>(nullptr)";
    }
    return "(&::rt::tp2cc_storage_ref<" + d.type_cxx + ">(" + address + "))";
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
  base_ty = analysis_.semantic_shape_type(base_ty);
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
  t = analysis_.semantic_shape_type(t);
  return t && t->kind == Kind::TyRecord &&
         static_cast<const TyRecord&>(*t).is_packed;
}

bool EmitStorage::type_is_direct_packed_aggregate(const TypeExpr* t) {
  t = analysis_.semantic_shape_type(t);
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
  t = analysis_.semantic_shape_type(t);
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
      const PrimitiveInfo* primitive = analysis_.primitive_info_for_type(t);
      return primitive &&
             (primitive->kind == PrimitiveKind::Char ||
              primitive->kind == PrimitiveKind::Byte ||
              primitive->kind == PrimitiveKind::ShortInt ||
              primitive->kind == PrimitiveKind::Boolean);
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

  const TypeExpr* scalar_type = analysis_.semantic_shape_type(current_type);
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
  std::string record_name = analysis_.direct_type_name(base_type);
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
    if (c.args.size() != 1) break;
    const TypeSymbol* symbol = analysis_.explicit_typecast_target_symbol(c);
    const PrimitiveInfo* primitive =
        symbol && symbol->descriptor ? symbol->descriptor->primitive : nullptr;
    if (!primitive || primitive->family == PrimitiveFamily::Pointer) {
      break;
    }
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
    const ClassInfo* cls = class_info_for_value(analysis_, *m.base);
    if (cls) {
      if (registry_.lookup_class_property(*cls, m.name)) return false;
      if (registry_.lookup_class_methods(*cls, m.name)) return false;
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
  const PrimitiveInfo* info = analysis_.primitive_info_for_type(t);
  return info && info->is_char();
}

bool EmitStorage::type_is_pcharish(const TypeExpr* t) {
  if (!t) return false;
  if (const PrimitiveInfo* primitive = analysis_.primitive_info_for_type(t);
      primitive && primitive->kind == PrimitiveKind::PChar) {
    return true;
  }
  t = analysis_.semantic_shape_type(t);
  if (!t) return false;
  if (t->kind != Kind::TyPointer) return false;
  const TypeExpr* target =
      analysis_.semantic_shape_type(static_cast<const TyPointer&>(*t).target.get());
  const PrimitiveInfo* info = analysis_.primitive_info_for_type(target);
  return info && info->is_char();
}

bool EmitStorage::type_is_metaclass(const TypeExpr* t) {
  return analysis_.metaclass_target_symbol(t) != nullptr;
}

bool EmitStorage::type_is_reference_class(const TypeExpr* t) {
  return analysis_.type_is_reference_class(t);
}

bool EmitStorage::expr_is_reference_class(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self") {
      const ClassInfo* ci = scope_.current_class_symbol
                                ? scope_.current_class_symbol->class_info()
                                : nullptr;
      return ci && ci->is_reference_type;
    }
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1) {
      if (const TypeExpr* cast_type =
              analysis_.explicit_typecast_result_type(e)) {
        return type_is_reference_class(cast_type);
      }
    }
  }
  return type_is_reference_class(storage_expr_type(e));
}

std::string EmitStorage::member_access_op(const Expr& e) {
  const TypeExpr* t = storage_expr_type(e);
  if (expr_is_reference_class(e) || type_is_pointerish(t)) return "->";
  return ".";
}

bool EmitStorage::type_is_stringish(const TypeExpr* t) {
  return analysis_.type_is_string_like(t);
}

bool EmitStorage::type_is_pointerish(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.semantic_shape_type(t);
  if (!t) return false;
  if (type_is_metaclass(t)) return true;
  if (type_is_reference_class(t)) return true;
  if (analysis_.type_is_interface(t)) return true;
  if (t->kind == Kind::TyPointer) return true;
  const PrimitiveInfo* info = analysis_.primitive_info_for_type(t);
  if (info && info->is_pointer_primitive()) return true;
  return false;
}

bool EmitStorage::type_is_open_array(const TypeExpr* t) {
  if (!t) return false;
  t = analysis_.semantic_shape_type(t);
  return t && t->kind == Kind::TyArray &&
         static_cast<const TyArray&>(*t).array_kind == ArrayKind::Open;
}

bool EmitStorage::fixed_array_pointer_can_decay_to_element_pointer(
    const TypeExpr* src_type, const TypeExpr* dst_type) {
  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  const TypeExpr* dst = analysis_.semantic_shape_type(dst_type);
  if (!src || src->kind != Kind::TyPointer || !dst) return false;

  const TypeExpr* src_pointee = analysis_.semantic_shape_type(
      static_cast<const TyPointer&>(*src).target.get());
  if (!src_pointee || src_pointee->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*src_pointee).array_kind !=
          ArrayKind::Fixed) {
    return false;
  }
  const TypeExpr* src_elem = analysis_.semantic_shape_type(
      static_cast<const TyArray&>(*src_pointee).element.get());
  if (!src_elem) return false;

  const TypeExpr* dst_elem = nullptr;
  if (dst->kind == Kind::TyPointer) {
    dst_elem = analysis_.semantic_shape_type(
        static_cast<const TyPointer&>(*dst).target.get());
  }
  if (!dst_elem) return false;
  return analysis_.same_type_ast(src_elem, dst_elem);
}

bool EmitStorage::fixed_char_array_value_can_decay_to_pchar(
    const TypeExpr* src_type, const TypeExpr* dst_type) {
  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  if (!src || src->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*src).array_kind != ArrayKind::Fixed) {
    return false;
  }
  const auto& arr = static_cast<const TyArray&>(*src);
  if (arr.dims.size() != 1 || !type_is_pcharish(dst_type)) return false;
  const TypeExpr* elem = analysis_.semantic_shape_type(arr.element.get());
  const PrimitiveInfo* info = analysis_.primitive_info_for_type(elem);
  return info && info->is_char();
}

std::string EmitStorage::lower_pointer_to_fixed_array_to_element(
    const TypeExpr* src_type, const std::string& source_cxx) {
  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  if (!src || src->kind != Kind::TyPointer) return source_cxx;
  const TypeExpr* pointee = analysis_.semantic_shape_type(
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
  const TypeExpr* dst = analysis_.semantic_shape_type(dst_type);
  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  if (!dst || !src) return false;

  const bool dst_proc = is_nonmethod_procedural_type(dst);
  const bool src_proc = is_nonmethod_procedural_type(src);
  const bool dst_ptr = type_is_pointerish(dst);
  const bool src_ptr = type_is_pointerish(src);
  if (!(dst_ptr || dst_proc) || !(src_ptr || src_proc)) return false;

  // Storage conversions are source-language decisions. Matching generated C++
  // carrier text is not enough: unrelated Pascal pointer/procedural types can
  // share a backend representation while still requiring an explicit cast.
  if (analysis_.same_type_ast(raw_dst, raw_src)) return true;

  const bool dst_void_ptr = is_plain_pointer_type(analysis_, dst);
  const bool src_void_ptr = is_plain_pointer_type(analysis_, src);
  if (fixed_array_pointer_can_decay_to_element_pointer(src, dst)) return true;
  if (pointer_to_object_upcast_is_valid(raw_dst, raw_src)) return true;
  if (class_to_interface_conversion_is_valid(raw_dst, raw_src)) return true;
  if ((dst_proc && src_void_ptr) || (dst_void_ptr && src_proc)) return true;
  if (src_void_ptr || dst_void_ptr) return true;

  const bool dst_ref_class = type_is_reference_class(dst);
  const bool src_ref_class = type_is_reference_class(src);
  if (dst_ref_class && src_ref_class) {
    // Reference-class conversions are nominal class-identity checks. Looking
    // up raw TyName spelling here would hide missing semantic type binding
    // and can pick the wrong unit/type when aliases or imports are involved.
    const ClassInfo* dst_class = class_info_for_value_type(raw_dst, dst);
    const ClassInfo* src_class = class_info_for_value_type(raw_src, src);
    if (dst_class && src_class &&
        class_parent_chain_contains(*dst_class, *src_class)) {
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
  const TypeExpr* dst = analysis_.semantic_shape_type(dst_type);
  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  if (!dst || !src) return source_cxx;

  const bool dst_proc = is_nonmethod_procedural_type(dst);
  const bool src_proc = is_nonmethod_procedural_type(src);
  const bool dst_ptr = type_is_pointerish(dst);
  const bool src_ptr = type_is_pointerish(src);
  if (!(dst_ptr || dst_proc) || !(src_ptr || src_proc)) return source_cxx;

  const std::string shaped_dst_cxx = types_.type_to_cxx(*dst);
  const std::string dst_cxx =
      dst_cxx_text.empty() ? shaped_dst_cxx : std::string(dst_cxx_text);
  const bool dst_ref_class = type_is_reference_class(dst);
  const bool src_ref_class = type_is_reference_class(src);
  if (!dst_ref_class && !src_ref_class) {
    if (analysis_.same_type_ast(dst_type, src_type)) return source_cxx;
  }

  const bool dst_void_ptr = is_plain_pointer_type(analysis_, dst);
  const bool src_void_ptr = is_plain_pointer_type(analysis_, src);

  // `^fixed_array of T -> ^T`: lower via the shared element-pointer helper.
  // The destination must be a typed pointer whose pointee matches the source
  // array's element type; otherwise the conversion is not value-preserving.
  if (fixed_array_pointer_can_decay_to_element_pointer(src, dst)) {
    return lower_pointer_to_fixed_array_to_element(src, source_cxx);
  }

  if (pointer_to_object_upcast_is_valid(dst_type, src_type)) {
    return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }
  // Class-hierarchy exception (downcast): explicit `Pchild(pparent)` where
  // Pchild's pointee descends from Pparent's pointee. Without this, the
  // caller falls into the raw `reinterpret_cast` branch and any subsequent
  // deref becomes strict-aliasing UB. `static_cast<Pchild>` keeps the C++
  // inheritance chain visible to the compiler; virtual dispatch through
  // `Pchild(pparent)^.VirtualMethod` then resolves against the object's
  // actual dynamic type (Pascal's precondition is that the pointee really
  // is a Pchild instance, same UB precondition as C++ downcast).
  // Do not remove as an "unnecessary special case": routing this through
  // the bytewise memcpy path would copy `*p as Pchild's pointee` into a
  // temporary and dispatch on the temporary's static type, which loses
  // virtual dispatch and object identity.
  if (explicit_pascal_cast &&
      pointer_to_object_downcast_is_valid(dst_type, src_type)) {
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
      // as `const void*`. Pascal's `const` qualifies the formal binding; it
      // does not create a different pointer value or a deep-const pointer type.
      // The conversion itself only carries that address. As in Pascal, writing
      // through the resulting pointer is valid only when the caller supplied
      // writable storage; make the shallow qualifier removal explicit rather
      // than relying on `-fpermissive`.
      return "reinterpret_cast<" + dst_cxx + ">(const_cast<void*>(" +
             source_cxx + "))";
    }
    return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }

  if (dst_ref_class && src_ref_class) {
    // Keep the emitted cast policy tied to the same ClassInfo relationship
    // test used for validity; do not reconstruct class names in emission.
    const ClassInfo* dst_class = class_info_for_value_type(dst_type, dst);
    const ClassInfo* src_class = class_info_for_value_type(src_type, src);
    if (!explicit_pascal_cast && dst_class && src_class &&
        registry_.same_class_identity(*dst_class, *src_class)) {
      return source_cxx;
    }
    const bool related_upcast =
        dst_class && src_class &&
        class_parent_chain_contains(*dst_class, *src_class);
    const bool related_downcast =
        dst_class && src_class &&
        class_parent_chain_contains(*src_class, *dst_class);
    if (related_upcast || (explicit_pascal_cast && related_downcast)) {
      return "static_cast<" + dst_cxx + ">(" + source_cxx + ")";
    }
    if (!explicit_pascal_cast) return source_cxx;
  }

  if (explicit_pascal_cast || (dst_ptr && src_ptr)) {
    // This is an address-value conversion, not a storage access. An immediate
    // Pascal dereference of an unrelated cast is intercepted by
    // pointer_typecast_deref_as_bytewise() and uses byte-addressed operations;
    // class-hierarchy casts were handled above with static_cast so object
    // identity and virtual dispatch remain intact. If this pointer value
    // escapes into a variable, Pascal gives the later dereference the same
    // source-level precondition as C++: the address must denote a live object
    // compatible with the pointer's declared target type.
    return "reinterpret_cast<" + dst_cxx + ">(" + source_cxx + ")";
  }
  return source_cxx;
}

bool EmitStorage::pointer_to_object_upcast_is_valid(const TypeExpr* dst_type,
                                                    const TypeExpr* src_type) {
  const ClassInfo* dst_class = class_info_for_pointer_target(dst_type);
  const ClassInfo* src_class = class_info_for_pointer_target(src_type);
  if (!dst_class || !src_class) return false;
  if (dst_class->is_reference_type || src_class->is_reference_type) {
    return false;
  }
  return class_parent_chain_contains(*dst_class, *src_class);
}

bool EmitStorage::pointer_to_object_downcast_is_valid(const TypeExpr* dst_type,
                                                     const TypeExpr* src_type) {
  const ClassInfo* dst_class = class_info_for_pointer_target(dst_type);
  const ClassInfo* src_class = class_info_for_pointer_target(src_type);
  if (!dst_class || !src_class) return false;
  if (dst_class->is_reference_type || src_class->is_reference_type) {
    return false;
  }
  return class_parent_chain_contains(*src_class, *dst_class);
}

std::optional<EmitStorageDesignator>
EmitStorage::pointer_typecast_deref_as_bytewise(const Deref& d) {
  if (!d.operand || d.operand->kind != Kind::Call) return std::nullopt;

  const TypeExpr* cast_target_type =
      analysis_.explicit_typecast_result_type(*d.operand);
  if (!cast_target_type) return std::nullopt;
  const TypeExpr* cast_shape = analysis_.semantic_shape_type(cast_target_type);
  if (!cast_shape || cast_shape->kind != Kind::TyPointer) return std::nullopt;

  const auto& call = static_cast<const Call&>(*d.operand);
  if (call.args.size() != 1 || !call.args[0]) return std::nullopt;

  const TypeExpr* src_type = analysis_.deduce_type(*call.args[0]);
  if (!src_type) return std::nullopt;

  // Skip class-hierarchy-safe casts: those emit `static_cast<T*>` in
  // `coerce_pointer_like_text`, and dereferencing through a `static_cast`
  // pointer stays inside the object's dynamic type (same object identity,
  // virtual dispatch preserved). Bytewise here would break virtual
  // dispatch by copying `*p` into a temporary of the cast type.
  if (pointer_to_object_upcast_is_valid(cast_target_type, src_type) ||
      pointer_to_object_downcast_is_valid(cast_target_type, src_type)) {
    return std::nullopt;
  }

  const TypeExpr* pointee =
      static_cast<const TyPointer&>(*cast_shape).target.get();
  if (!pointee) return std::nullopt;

  // `PT(arg)^` is the ordinary Pascal callback idiom when `arg : Pointer`
  // and PT points at a record/object or managed array carrier. Access the live
  // object directly so managed fields retain identity and mutations. A fixed
  // array cast remains a representation view: FPC uses `PBytes(raw)^[i]` to
  // inspect arbitrary storage, so that case stays on bytewise access.
  const TypeExpr* pointee_shape = analysis_.semantic_shape_type(pointee);
  const bool live_object_pointee =
      pointee_shape &&
      (pointee_shape->kind == Kind::TyRecord ||
       pointee_shape->kind == Kind::TyObject ||
       pointee_shape->kind == Kind::TyInterface ||
       (pointee_shape->kind == Kind::TyArray &&
        static_cast<const TyArray&>(*pointee_shape).array_kind !=
            ArrayKind::Fixed));
  if (is_plain_pointer_type(analysis_, src_type) && live_object_pointee) {
    return std::nullopt;
  }

  const std::string pointee_cxx = types_.type_to_cxx(*pointee);
  const TypeSymbol* source_class_symbol =
      analysis_.deduce_class_symbol(*call.args[0]);
  const ClassInfo* source_class =
      source_class_symbol ? source_class_symbol->class_info() : nullptr;
  // Reference-class expressions such as `self` are pointer values in Pascal.
  // The storage type may name the class body, so ask the bound class
  // symbol before falling back to generic pointer-shaped type predicates.
  const bool source_is_pointer_value =
      (source_class && source_class->is_reference_type) ||
      type_is_pointerish(src_type);
  const std::string cast_ptr_cxx =
      source_is_pointer_value ? expr_ops_.expr_to_cxx(*call.args[0])
                              : expr_ops_.expr_to_cxx(*d.operand);
  // Pascal `P(x)^` selects the pointee storage of the pointer value produced by
  // `P(x)`. When `x` is already pointer-typed, `x` itself is that address; using
  // the rendered target cast would introduce a typed pointer alias before the
  // byte-copy helper sees the storage. When `x` is not pointer-typed, the
  // explicit Pascal cast is what creates the pointer value, so the storage
  // address must come from the rendered `P(x)`, not from `x`'s own variable
  // slot. Because this is still carried as an emitted expression string, the
  // final load/store/inc or untyped-actual lowering may consume it only once;
  // any consumer needing it twice must bind a temporary first so side-effecting
  // cast sources are evaluated exactly once.
  std::string void_ptr =
      "const_cast<void*>(static_cast<const void*>(" + cast_ptr_cxx + "))";
  std::string backing_type_cxx;
  const TypeExpr* source_shape = analysis_.semantic_shape_type(src_type);
  if (source_shape && source_shape->kind == Kind::TyPointer) {
    const TypeExpr* source_pointee =
        static_cast<const TyPointer&>(*source_shape).target.get();
    if (source_pointee) {
      backing_type_cxx = types_.type_to_cxx(*source_pointee);
    }
  }
  const std::string backing_ptr_cxx =
      backing_type_cxx.empty() ? std::string{} : void_ptr;
  return EmitStorageDesignator::bytewise(
      std::move(void_ptr), pointee_cxx, backing_ptr_cxx,
      std::move(backing_type_cxx));
}

bool EmitStorage::class_to_interface_conversion_is_valid(
    const TypeExpr* dst_type, const TypeExpr* src_type) {
  const InterfaceInfo* interface = analysis_.interface_info_for_type(dst_type);
  if (!interface) return false;

  const TypeExpr* src = analysis_.semantic_shape_type(src_type);
  if (!type_is_reference_class(src)) return false;

  const ClassInfo* cls = class_info_for_value_type(src_type, src);
  if (!cls) return false;
  return registry_.class_implements_interface(*cls, *interface);
}

const ClassInfo* EmitStorage::class_info_for_pointer_target(
    const TypeExpr* t) {
  const TypeExpr* canonical = analysis_.semantic_shape_type(t);
  if (!canonical || canonical->kind != Kind::TyPointer) return nullptr;
  const TypeExpr* target =
      static_cast<const TyPointer&>(*canonical).target.get();
  return analysis_.class_info_for_type(target);
}

const ClassInfo* EmitStorage::class_info_for_value_type(
    const TypeExpr* raw, const TypeExpr* canonical) {
  if (const ClassInfo* info = analysis_.class_info_for_type(raw)) return info;
  if (const ClassInfo* info = analysis_.class_info_for_type(canonical)) {
    return info;
  }
  return nullptr;
}

bool EmitStorage::class_parent_chain_contains(
    const ClassInfo& ancestor, const ClassInfo& current) const {
  const ClassInfo* cls = &current;
  std::unordered_set<const ClassInfo*> seen;
  while (cls) {
    if (registry_.same_class_identity(ancestor, *cls)) return true;
    if (!seen.insert(cls).second) break;
    cls = registry_.lookup_parent_class(*cls);
  }
  return false;
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

  auto tit = scope_.local_value_types.find(vd.absolute_target);
  if (tit != scope_.local_value_types.end()) {
    return absolute_target_info(
        target_cxx, tit->second,
        scope_.local_const_params.count(vd.absolute_target) > 0);
  }

  ResolveResult rr = resolve_name_provider_.resolve_name(vd.absolute_target);
  if (rr.kind == ResolvedKind::ClassField) {
    const TypeSymbol* current_symbol = scope_.current_class_symbol;
    const ClassInfo* current_class =
        current_symbol ? current_symbol->class_info() : nullptr;
    if (current_class) {
      if (auto* f = registry_.lookup_class_field(*current_class,
                                                 vd.absolute_target)) {
        return absolute_target_info(target_cxx, f->type.get());
      }
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
  return EmitAbsoluteTargetInfo{target_cxx, type, false, is_const_storage};
}

}  // namespace tp2cc
