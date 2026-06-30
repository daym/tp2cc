#include "typereg.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <unordered_set>

#include "diag.h"
#include "emit_support.h"
#include "runtime_units.h"

namespace tp2cc {

using namespace ast;

TypeSymbol make_enum_type_symbol(std::string_view unit, std::string_view name,
                                 std::string_view cxx_name,
                                 const TyEnum& type);

static void append_variant_part_enum_types(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::vector<const TyEnum*>& out);

static void append_enum_types(const TypeExpr& t,
                              std::vector<const TyEnum*>& out) {
  switch (t.kind) {
    case Kind::TyEnum:
      out.push_back(&static_cast<const TyEnum&>(t));
      return;
    case Kind::TyPointer:
      if (static_cast<const TyPointer&>(t).target) {
        append_enum_types(*static_cast<const TyPointer&>(t).target, out);
      }
      return;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      for (const auto& d : a.dims)
        if (d) append_enum_types(*d, out);
      if (a.element) append_enum_types(*a.element, out);
      return;
    }
    case Kind::TySet:
      if (static_cast<const TySet&>(t).element) {
        append_enum_types(*static_cast<const TySet&>(t).element, out);
      }
      return;
    case Kind::TyFile:
      if (static_cast<const TyFile&>(t).element) {
        append_enum_types(*static_cast<const TyFile&>(t).element, out);
      }
      return;
    case Kind::TyRecord: {
      const auto& r = static_cast<const TyRecord&>(t);
      for (const auto& f : r.fields)
        if (f.type) append_enum_types(*f.type, out);
      append_variant_part_enum_types(r.variant_part, out);
      return;
    }
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      for (const auto& m : o.members) {
        if (m.kind == ObjectMemberKind::Field && m.field_type) {
          append_enum_types(*m.field_type, out);
        } else if (m.kind == ObjectMemberKind::Method && m.method) {
          if (m.method->return_type) {
            append_enum_types(*m.method->return_type, out);
          }
          for (const auto& p : m.method->params)
            if (p.type) append_enum_types(*p.type, out);
        } else if (m.kind == ObjectMemberKind::Property) {
          if (m.property.type) append_enum_types(*m.property.type, out);
          for (const auto& p : m.property.params)
            if (p.type) append_enum_types(*p.type, out);
        }
      }
      return;
    }
    case Kind::TyInterface: {
      const auto& i = static_cast<const TyInterface&>(t);
      for (const auto& m : i.members) {
        if (m.kind != ObjectMemberKind::Method || !m.method) continue;
        if (m.method->return_type) {
          append_enum_types(*m.method->return_type, out);
        }
        for (const auto& p : m.method->params)
          if (p.type) append_enum_types(*p.type, out);
      }
      return;
    }
    case Kind::TyProcedural: {
      const auto& p = static_cast<const TyProcedural&>(t);
      if (p.return_type) append_enum_types(*p.return_type, out);
      for (const auto& par : p.params)
        if (par.type) append_enum_types(*par.type, out);
      return;
    }
    case Kind::TyDistinct:
      if (static_cast<const TyDistinct&>(t).underlying) {
        append_enum_types(*static_cast<const TyDistinct&>(t).underlying, out);
      }
      return;
    default:
      return;
  }
}

static void append_variant_part_enum_types(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::vector<const TyEnum*>& out) {
  if (!vpart) return;
  if (vpart->tag_type) append_enum_types(*vpart->tag_type, out);
  for (const auto& vc : vpart->cases) {
    for (const auto& f : vc.fields)
      if (f.type) append_enum_types(*f.type, out);
    append_variant_part_enum_types(vc.variant_part, out);
  }
}

std::vector<const TyEnum*> collect_enum_types(const TypeExpr& t) {
  std::vector<const TyEnum*> out;
  append_enum_types(t, out);
  return out;
}

void register_enum_types_for_owner(
    std::unordered_map<std::string, const TyEnum*>& out,
    const TypeExpr* type, std::string_view owner_name,
    const TyEnum* named_top_level) {
  if (!type) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*type);
  std::unordered_set<const TyEnum*> seen;
  size_t anon_index = 0;
  const std::string owner = ascii_lower(std::string(owner_name));
  for (const TyEnum* te : enums) {
    if (!te || !seen.insert(te).second) continue;
    if (named_top_level && te == named_top_level) {
      out[owner] = te;
      continue;
    }
    const bool whole_type_is_enum = te == type && !named_top_level;
    std::string key = whole_type_is_enum
                          ? owner
                          : owner + "_enum" + std::to_string(anon_index);
    out[key] = te;
    ++anon_index;
  }
}

namespace {

std::string lc(std::string s) {
  for (auto& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return s;
}

std::string join_path(const std::vector<std::string>& path) {
  std::string out;
  for (const auto& segment : path) {
    if (!out.empty()) out += ".";
    out += segment;
  }
  return out;
}

std::string type_symbol_path(const TypeSymbol& symbol) {
  std::vector<std::string> path = symbol.owner_path;
  path.push_back(symbol.name);
  return join_path(path);
}

std::string nested_type_symbol_key(std::string_view unit,
                                   const std::vector<std::string>& owner_path,
                                   std::string_view name) {
  std::string out = lc(std::string(unit));
  out += "$";
  for (const auto& owner : owner_path) {
    out += lc(owner);
    out += ".";
  }
  out += lc(std::string(name));
  return out;
}

void index_type_expr_unit(TypeRegistry& r, const TypeExpr* type,
                          const std::string& unit);

void index_param_type_units(TypeRegistry& r, const std::vector<Param>& params,
                            const std::string& unit) {
  for (const auto& param : params) {
    index_type_expr_unit(r, param.type.get(), unit);
  }
}

void index_proc_signature_type_units(TypeRegistry& r, const ProcDecl& proc,
                                     const std::string& unit) {
  index_param_type_units(r, proc.params, unit);
  index_type_expr_unit(r, proc.return_type.get(), unit);
}

void index_variant_type_units(TypeRegistry& r,
                              const std::shared_ptr<VariantPart>& variant,
                              const std::string& unit) {
  if (!variant) return;
  index_type_expr_unit(r, variant->tag_type.get(), unit);
  for (const auto& vcase : variant->cases) {
    for (const auto& field : vcase.fields) {
      index_type_expr_unit(r, field.type.get(), unit);
    }
    index_variant_type_units(r, vcase.variant_part, unit);
  }
}

void index_object_member_type_units(TypeRegistry& r, const ObjectMember& member,
                                    const std::string& unit) {
  switch (member.kind) {
    case ObjectMemberKind::Field:
      index_type_expr_unit(r, member.field_type.get(), unit);
      break;
    case ObjectMemberKind::Method:
      if (member.method) {
        index_proc_signature_type_units(r, *member.method, unit);
      }
      break;
    case ObjectMemberKind::Property:
      index_param_type_units(r, member.property.params, unit);
      index_type_expr_unit(r, member.property.type.get(), unit);
      break;
    case ObjectMemberKind::Type:
      if (member.type_decl) {
        index_type_expr_unit(r, member.type_decl->type.get(), unit);
      }
      break;
  }
}

void index_type_expr_unit(TypeRegistry& r, const TypeExpr* type,
                          const std::string& unit) {
  if (!type) return;
  if (type->loc.file) {
    r.source_file_units.try_emplace(type->loc.file.get(), unit);
  }
  switch (type->kind) {
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(*type);
      for (const auto& dim : a.dims) index_type_expr_unit(r, dim.get(), unit);
      index_type_expr_unit(r, a.element.get(), unit);
      break;
    }
    case Kind::TyRecord: {
      const auto& rec = static_cast<const TyRecord&>(*type);
      for (const auto& field : rec.fields) {
        index_type_expr_unit(r, field.type.get(), unit);
      }
      for (const auto& nested : rec.nested_types) {
        if (nested) index_type_expr_unit(r, nested->type.get(), unit);
      }
      index_variant_type_units(r, rec.variant_part, unit);
      break;
    }
    case Kind::TyObject: {
      const auto& obj = static_cast<const TyObject&>(*type);
      for (const auto& member : obj.members) {
        index_object_member_type_units(r, member, unit);
      }
      break;
    }
    case Kind::TyInterface: {
      const auto& intf = static_cast<const TyInterface&>(*type);
      for (const auto& member : intf.members) {
        index_object_member_type_units(r, member, unit);
      }
      break;
    }
    case Kind::TySet:
      index_type_expr_unit(r, static_cast<const TySet&>(*type).element.get(),
                           unit);
      break;
    case Kind::TyFile:
      index_type_expr_unit(r, static_cast<const TyFile&>(*type).element.get(),
                           unit);
      break;
    case Kind::TyPointer:
      index_type_expr_unit(
          r, static_cast<const TyPointer&>(*type).target.get(), unit);
      break;
    case Kind::TyDistinct:
      index_type_expr_unit(
          r, static_cast<const TyDistinct&>(*type).underlying.get(), unit);
      break;
    case Kind::TyProcedural: {
      const auto& proc = static_cast<const TyProcedural&>(*type);
      index_param_type_units(r, proc.params, unit);
      index_type_expr_unit(r, proc.return_type.get(), unit);
      break;
    }
    default:
      break;
  }
}

const std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>*
nested_type_map(const TypeSymbol& symbol) {
  if (const ClassInfo* ci = symbol.class_info()) return &ci->nested_types;
  if (const RecordInfo* ri = symbol.record_info()) return &ri->nested_types;
  return nullptr;
}

std::unordered_map<std::string, std::shared_ptr<TypeSymbol>>*
nested_type_map_mut(TypeSymbol& symbol) {
  if (ClassInfo* ci = symbol.mutable_class_info()) return &ci->nested_types;
  if (RecordInfo* ri = symbol.mutable_record_info()) return &ri->nested_types;
  return nullptr;
}

const TypeSymbol* lookup_nested_type_symbol_path(const TypeSymbol* root,
                                                 const std::string& path,
                                                 size_t start) {
  const TypeSymbol* current = root;
  while (current && start < path.size()) {
    const size_t dot = path.find('.', start);
    const size_t len =
        dot == std::string::npos ? std::string::npos : dot - start;
    const auto* nested = nested_type_map(*current);
    if (!nested) return nullptr;
    auto it = nested->find(path.substr(start, len));
    if (it == nested->end()) return nullptr;
    current = it->second.get();
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return current;
}

TypeSymbol* lookup_nested_type_symbol_path_mut(TypeSymbol* root,
                                               const std::string& path,
                                               size_t start) {
  TypeSymbol* current = root;
  while (current && start < path.size()) {
    const size_t dot = path.find('.', start);
    const size_t len =
        dot == std::string::npos ? std::string::npos : dot - start;
    auto* nested = nested_type_map_mut(*current);
    if (!nested) return nullptr;
    auto it = nested->find(path.substr(start, len));
    if (it == nested->end()) return nullptr;
    current = it->second.get();
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return current;
}

const TypeDescriptor* make_type_descriptor(TypeRegistry& r,
                                           const TypeExpr* type,
                                           const TypeSymbol* symbol) {
  r.type_descriptor_storage.push_back(
      TypeDescriptor{.id = r.type_descriptor_storage.size() + 1,
                     .type = type,
                     .symbol = symbol});
  return &r.type_descriptor_storage.back();
}

const TypeDescriptor* make_metaclass_descriptor(TypeRegistry& r,
                                                const TypeSymbol* target) {
  target = descriptor_payload_symbol(target);
  if (!target) return nullptr;
  if (auto it = r.metaclass_descriptors.find(target);
      it != r.metaclass_descriptors.end()) {
    return it->second;
  }
  r.type_descriptor_storage.push_back(
      TypeDescriptor{.id = r.type_descriptor_storage.size() + 1,
                     .type = nullptr,
                     .symbol = nullptr,
                     .metaclass_target = target});
  const TypeDescriptor* descriptor = &r.type_descriptor_storage.back();
  r.metaclass_descriptors.insert_or_assign(target, descriptor);
  return descriptor;
}

void bind_type_expr_descriptor(TypeRegistry& r, const TypeExpr* type,
                               const TypeDescriptor* descriptor) {
  if (!type || !descriptor) return;
  r.type_descriptors.insert_or_assign(type, descriptor);
}

void bind_type_expr_symbol(TypeRegistry& r, const TypeExpr* type,
                           const TypeSymbol* symbol) {
  if (!type || !symbol) return;
  r.type_reference_symbols.insert_or_assign(type, symbol);
}

void bind_symbol_descriptor(TypeRegistry& r, TypeSymbol& symbol,
                            const TypeDescriptor* descriptor) {
  if (!descriptor) return;
  symbol.descriptor = descriptor;
  bind_type_expr_descriptor(r, symbol.type, descriptor);
}

bool symbol_declares_fresh_type(const TypeSymbol& symbol) {
  const AliasInfo* alias = symbol.alias_info();
  if (!alias) return true;
  if (!alias->target) return true;
  return alias->target->kind != Kind::TyName &&
         alias->target->kind != Kind::TyMetaclass;
}

const TypeDescriptor* ensure_fresh_symbol_descriptor(TypeRegistry& r,
                                                     TypeSymbol& symbol) {
  if (symbol.descriptor) return symbol.descriptor;
  const TypeDescriptor* descriptor =
      make_type_descriptor(r, symbol.type, &symbol);
  bind_symbol_descriptor(r, symbol, descriptor);
  return descriptor;
}

void ensure_fresh_descriptors_for_symbol_tree(TypeRegistry& r,
                                              TypeSymbol& symbol) {
  if (symbol_declares_fresh_type(symbol)) {
    ensure_fresh_symbol_descriptor(r, symbol);
  }
  if (auto* nested = nested_type_map_mut(symbol)) {
    for (auto& [_, child] : *nested) {
      if (child) ensure_fresh_descriptors_for_symbol_tree(r, *child);
    }
  }
}

void ensure_fresh_descriptors_for_registered_symbols(TypeRegistry& r) {
  for (TypeSymbol& symbol : r.type_symbols) {
    ensure_fresh_descriptors_for_symbol_tree(r, symbol);
  }
  for (auto& [_, symbol] : r.nested_type_symbols) {
    if (symbol) ensure_fresh_descriptors_for_symbol_tree(r, *symbol);
  }
}

struct TypeDescriptorResolver {
  TypeRegistry& registry;
  const TypeLookupContext* context = nullptr;
  const std::unordered_map<std::string, const TypeSymbol*>* pointer_forwards =
      nullptr;
  const std::unordered_set<std::string>* ordinary_forwards = nullptr;
  const std::unordered_map<std::string, const TypeSymbol*>*
      visible_forward_classes = nullptr;
  std::unordered_set<const TypeSymbol*> resolving_symbols;

  TypeDescriptorResolver(
      TypeRegistry& registry_in, const TypeLookupContext* context_in,
      const std::unordered_map<std::string, const TypeSymbol*>*
          pointer_forwards_in = nullptr,
      const std::unordered_set<std::string>* ordinary_forwards_in = nullptr,
      const std::unordered_map<std::string, const TypeSymbol*>*
          visible_forward_classes_in = nullptr)
      : registry(registry_in),
        context(context_in),
        pointer_forwards(pointer_forwards_in),
        ordinary_forwards(ordinary_forwards_in),
        visible_forward_classes(visible_forward_classes_in) {}

  const TypeDescriptor* resolve_symbol_reference(const TypeSymbol* symbol) {
    return resolve_symbol(symbol, /*resolve_fresh_body=*/false);
  }

  const TypeDescriptor* resolve_symbol_declaration(const TypeSymbol* symbol) {
    return resolve_symbol(symbol, /*resolve_fresh_body=*/true);
  }

  const TypeDescriptor* resolve_symbol(const TypeSymbol* symbol,
                                       bool resolve_fresh_body) {
    if (!symbol) return nullptr;
    TypeSymbol& mut = *const_cast<TypeSymbol*>(symbol);
    if (symbol_declares_fresh_type(*symbol)) {
      const TypeDescriptor* descriptor =
          ensure_fresh_symbol_descriptor(registry, mut);
      if (!resolve_fresh_body) return descriptor;
      if (!resolving_symbols.insert(symbol).second) {
        return descriptor;
      }
      resolve_children(symbol->type);
      resolving_symbols.erase(symbol);
      return descriptor;
    }

    if (const TypeDescriptor* descriptor = symbol->descriptor) {
      return descriptor;
    }
    if (!resolving_symbols.insert(symbol).second) {
      report_error({}, "cyclic type alias involving `" + symbol->name + "`");
      return nullptr;
    }

    const TypeDescriptor* descriptor = nullptr;
    if (const AliasInfo* alias = symbol->alias_info()) {
      descriptor = resolve_type(alias->target.get(), /*pointer_target=*/false);
      bind_symbol_descriptor(registry, mut, descriptor);
    }

    resolving_symbols.erase(symbol);
    return descriptor;
  }

  const TypeSymbol* resolve_named_type_symbol(const TypeExpr* type,
                                              std::string_view raw_name,
                                              bool pointer_target) {
    const std::string name = lc(std::string(raw_name));
    const TypeSymbol* symbol = nullptr;
    if (pointer_target && pointer_forwards) {
      auto fit = pointer_forwards->find(name);
      if (fit != pointer_forwards->end()) symbol = fit->second;
    }
    if (!symbol && visible_forward_classes) {
      auto fit = visible_forward_classes->find(name);
      if (fit != visible_forward_classes->end()) symbol = fit->second;
    }
    if (!symbol && ordinary_forwards && ordinary_forwards->count(name) > 0) {
      report_error(type ? type->loc : Location{},
                   "type `" + name + "` is not visible before this "
                   "declaration");
      return nullptr;
    }
    if (!symbol) {
      symbol = registry.lookup_type_symbol_in_context(name, context);
    }
    if (!symbol) {
      symbol = registry.builtin_literal(name);
    }
    if (!symbol) {
      report_error(type ? type->loc : Location{},
                   "unresolved type `" + name + "`");
      return nullptr;
    }
    return symbol;
  }

  const TypeDescriptor* resolve_metaclass_type(const TyMetaclass& type) {
    const TypeSymbol* named = resolve_named_type_symbol(
        &type, type.class_name, /*pointer_target=*/false);
    if (!named) return nullptr;
    const TypeSymbol* target = descriptor_payload_symbol(named);
    const ClassInfo* class_info = target ? target->class_info() : nullptr;
    if (!class_info || !class_info->is_reference_type) {
      report_error(type.loc, "`class of` target `" +
                                 lc(type.class_name) +
                                 "` is not a reference class");
      return nullptr;
    }
    const TypeDescriptor* descriptor =
        make_metaclass_descriptor(registry, target);
    bind_type_expr_descriptor(registry, &type, descriptor);
    bind_type_expr_symbol(registry, &type, named);
    return descriptor;
  }

  const TypeDescriptor* resolve_type(const TypeExpr* type,
                                     bool pointer_target) {
    if (!type) return nullptr;
    const TypeLookupContext* saved_context = context;
    if (const TypeLookupContext* own_context =
            registry.lookup_context_for_type(type)) {
      context = own_context;
    }
    auto existing = registry.type_descriptors.find(type);
    if (existing != registry.type_descriptors.end() &&
        type->kind != Kind::TyName) {
      resolve_children(type);
      context = saved_context;
      return existing->second;
    }

    if (type->kind == Kind::TyName) {
      const TypeSymbol* symbol = resolve_named_type_symbol(
          type, static_cast<const TyName&>(*type).name, pointer_target);
      if (!symbol) {
        context = saved_context;
        return nullptr;
      }
      const TypeDescriptor* descriptor = resolve_symbol_reference(symbol);
      bind_type_expr_descriptor(registry, type, descriptor);
      bind_type_expr_symbol(registry, type, symbol);
      context = saved_context;
      return descriptor;
    }

    if (type->kind == Kind::TyMetaclass) {
      const TypeDescriptor* descriptor =
          resolve_metaclass_type(static_cast<const TyMetaclass&>(*type));
      context = saved_context;
      return descriptor;
    }

    const TypeDescriptor* descriptor = make_type_descriptor(registry, type,
                                                            nullptr);
    bind_type_expr_descriptor(registry, type, descriptor);
    resolve_children(type);
    context = saved_context;
    return descriptor;
  }

  void resolve_params(const std::vector<Param>& params) {
    for (const auto& param : params) {
      resolve_type(param.type.get(), /*pointer_target=*/false);
    }
  }

  void resolve_variant_part(const std::shared_ptr<VariantPart>& variant) {
    if (!variant) return;
    resolve_type(variant->tag_type.get(), /*pointer_target=*/false);
    for (const auto& vcase : variant->cases) {
      for (const auto& field : vcase.fields) {
        resolve_type(field.type.get(), /*pointer_target=*/false);
      }
      resolve_variant_part(vcase.variant_part);
    }
  }

  void resolve_object_member(const ObjectMember& member) {
    switch (member.kind) {
      case ObjectMemberKind::Field:
        resolve_type(member.field_type.get(), /*pointer_target=*/false);
        break;
      case ObjectMemberKind::Method:
        if (member.method) {
          const TypeLookupContext* saved_context = context;
          if (const TypeLookupContext* method_context =
                  registry.lookup_proc_signature_context(member.method.get())) {
            context = method_context;
          }
          resolve_params(member.method->params);
          resolve_type(member.method->return_type.get(),
                       /*pointer_target=*/false);
          context = saved_context;
        }
        break;
      case ObjectMemberKind::Property:
        resolve_params(member.property.params);
        resolve_type(member.property.type.get(), /*pointer_target=*/false);
        break;
      case ObjectMemberKind::Type:
        if (member.type_decl) {
          resolve_type(member.type_decl->type.get(),
                       /*pointer_target=*/false);
        }
        break;
    }
  }

  void resolve_children(const TypeExpr* type) {
    if (!type) return;
    switch (type->kind) {
      case Kind::TyArray: {
        const auto& array = static_cast<const TyArray&>(*type);
        for (const auto& dim : array.dims) {
          resolve_type(dim.get(), /*pointer_target=*/false);
        }
        resolve_type(array.element.get(), /*pointer_target=*/false);
        break;
      }
      case Kind::TyRecord: {
        const auto& record = static_cast<const TyRecord&>(*type);
        for (const auto& nested : record.nested_types) {
          if (nested) {
            resolve_type(nested->type.get(), /*pointer_target=*/false);
          }
        }
        for (const auto& field : record.fields) {
          resolve_type(field.type.get(), /*pointer_target=*/false);
        }
        resolve_variant_part(record.variant_part);
        break;
      }
      case Kind::TyObject: {
        const auto& object = static_cast<const TyObject&>(*type);
        for (const auto& member : object.members) {
          resolve_object_member(member);
        }
        break;
      }
      case Kind::TyInterface: {
        const auto& intf = static_cast<const TyInterface&>(*type);
        for (const auto& member : intf.members) {
          resolve_object_member(member);
        }
        break;
      }
      case Kind::TySet:
        resolve_type(static_cast<const TySet&>(*type).element.get(),
                     /*pointer_target=*/false);
        break;
      case Kind::TyFile:
        resolve_type(static_cast<const TyFile&>(*type).element.get(),
                     /*pointer_target=*/false);
        break;
      case Kind::TyPointer:
        resolve_type(static_cast<const TyPointer&>(*type).target.get(),
                     /*pointer_target=*/true);
        break;
      case Kind::TyDistinct:
        resolve_type(static_cast<const TyDistinct&>(*type).underlying.get(),
                     /*pointer_target=*/false);
        break;
      case Kind::TyProcedural: {
        const auto& proc = static_cast<const TyProcedural&>(*type);
        resolve_params(proc.params);
        resolve_type(proc.return_type.get(), /*pointer_target=*/false);
        break;
      }
      default:
        break;
    }
  }
};

const TypeSymbol* local_context_type_symbol(const TypeLookupContext* context,
                                            std::string_view name) {
  if (!context) return nullptr;
  const std::string low = lc(std::string(name));
  auto it = context->type_symbols.find(low);
  if (it != context->type_symbols.end()) return it->second;
  if (!context->unit_info) return nullptr;
  switch (context->kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto jt = context->unit_info->impl_types.find(low);
      return jt == context->unit_info->impl_types.end() ? nullptr : jt->second;
    }
    case ScopeFrameKind::UnitInterface: {
      auto jt = context->unit_info->iface_types.find(low);
      return jt == context->unit_info->iface_types.end() ? nullptr : jt->second;
    }
    case ScopeFrameKind::Local:
    case ScopeFrameKind::ImportedUnitInterface:
      return nullptr;
  }
  return nullptr;
}

bool is_forward_reference_class_type(const TypeExpr* type) {
  if (!type || type->kind != Kind::TyObject) return false;
  const auto& object = static_cast<const TyObject&>(*type);
  return object.is_reference_type && object.is_forward;
}

void resolve_type_decl_run_descriptors(
    TypeRegistry& r, const std::vector<DeclPtr>& decls, size_t begin,
    size_t end, const TypeLookupContext* context,
    std::unordered_map<std::string, const TypeSymbol*>&
        visible_forward_classes) {
  std::unordered_map<std::string, const TypeSymbol*> run_symbols;
  std::unordered_set<std::string> unresolved;
  for (size_t i = begin; i < end; ++i) {
    const auto& td = static_cast<const TypeDecl&>(*decls[i]);
    const std::string name = lc(td.name);
    if (const TypeSymbol* symbol = local_context_type_symbol(context, name)) {
      run_symbols[name] = symbol;
      unresolved.insert(name);
      if (symbol_declares_fresh_type(*symbol)) {
        ensure_fresh_symbol_descriptor(r, *const_cast<TypeSymbol*>(symbol));
      }
    }
  }

  for (size_t i = begin; i < end; ++i) {
    const auto& td = static_cast<const TypeDecl&>(*decls[i]);
    unresolved.erase(lc(td.name));
    TypeDescriptorResolver resolver(r, context, &run_symbols, &unresolved);
    if (const TypeSymbol* symbol =
            local_context_type_symbol(context, td.name)) {
      if (is_forward_reference_class_type(td.type.get())) {
        visible_forward_classes[lc(td.name)] = symbol;
      }
      TypeDescriptorResolver scoped_resolver(
          r, context, &run_symbols, &unresolved, &visible_forward_classes);
      if (is_forward_reference_class_type(td.type.get()) &&
          td.type.get() != symbol->type) {
        const TypeDescriptor* descriptor =
            ensure_fresh_symbol_descriptor(r, *const_cast<TypeSymbol*>(symbol));
        bind_type_expr_descriptor(r, td.type.get(), descriptor);
        bind_type_expr_symbol(r, td.type.get(), symbol);
        continue;
      }
      scoped_resolver.resolve_symbol_declaration(symbol);
    } else {
      resolver.resolve_type(td.type.get(), /*pointer_target=*/false);
    }
  }
}

void resolve_decl_list_type_descriptors(TypeRegistry& r,
                                        const std::vector<DeclPtr>& decls,
                                        const TypeLookupContext* context) {
  std::unordered_map<std::string, const TypeSymbol*> visible_forward_classes;
  for (size_t i = 0; i < decls.size();) {
    const auto& d = decls[i];
    if (!d) {
      ++i;
      continue;
    }
    if (d->kind == Kind::TypeDecl) {
      size_t end = i + 1;
      while (end < decls.size() && decls[end] &&
             decls[end]->kind == Kind::TypeDecl) {
        ++end;
      }
      resolve_type_decl_run_descriptors(r, decls, i, end, context,
                                        visible_forward_classes);
      i = end;
      continue;
    }

    TypeDescriptorResolver resolver(r, context);
    switch (d->kind) {
      case Kind::VarDecl:
        resolver.resolve_type(static_cast<const VarDecl&>(*d).type.get(),
                              /*pointer_target=*/false);
        break;
      case Kind::ConstDecl:
        resolver.resolve_type(static_cast<const ConstDecl&>(*d).type.get(),
                              /*pointer_target=*/false);
        break;
      case Kind::ProcDecl: {
        const auto& pd = static_cast<const ProcDecl&>(*d);
        const TypeLookupContext* sig_context =
            r.lookup_proc_signature_context(&pd);
        TypeDescriptorResolver sig_resolver(
            r, sig_context ? sig_context : context);
        sig_resolver.resolve_params(pd.params);
        sig_resolver.resolve_type(pd.return_type.get(),
                                  /*pointer_target=*/false);
        resolve_decl_list_type_descriptors(
            r, pd.locals,
            r.lookup_proc_body_context(&pd));
        break;
      }
      default:
        break;
    }
    ++i;
  }
}

bool proc_accepts_zero_args(const ProcDecl& pd) {
  // Defaulted trailing parameters make a Pascal routine callable as `foo`
  // / `foo()` even when its flattened formal count is non-zero.
  for (const auto& p : pd.params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    if (count != 0 && !p.default_value) return false;
  }
  return true;
}

size_t proc_param_count(const std::vector<Param>& params) {
  size_t count = 0;
  for (const auto& p : params) count += p.names.empty() ? 1 : p.names.size();
  return count;
}

bool signature_type_exprs_match(const TypeExpr* a, const TypeExpr* b);

std::vector<const Param*> flattened_params(const std::vector<Param>& params) {
  std::vector<const Param*> out;
  for (const Param& p : params) {
    const size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) out.push_back(&p);
  }
  return out;
}

bool signature_params_match(const std::vector<Param>& a,
                            const std::vector<Param>& b) {
  const std::vector<const Param*> as = flattened_params(a);
  const std::vector<const Param*> bs = flattened_params(b);
  if (as.size() != bs.size()) return false;
  for (size_t i = 0; i < as.size(); ++i) {
    if (as[i]->mode != bs[i]->mode) return false;
    if (!signature_type_exprs_match(as[i]->type.get(), bs[i]->type.get())) {
      return false;
    }
  }
  return true;
}

bool signature_type_exprs_match(const TypeExpr* a, const TypeExpr* b) {
  if (a == b) return true;
  if (!a || !b || a->kind != b->kind) return false;
  switch (a->kind) {
    case Kind::TyName:
      return lc(static_cast<const TyName&>(*a).name) ==
             lc(static_cast<const TyName&>(*b).name);
    case Kind::TyPointer:
      return signature_type_exprs_match(
          static_cast<const TyPointer&>(*a).target.get(),
          static_cast<const TyPointer&>(*b).target.get());
    case Kind::TySet:
      return signature_type_exprs_match(
          static_cast<const TySet&>(*a).element.get(),
          static_cast<const TySet&>(*b).element.get());
    case Kind::TyFile:
      return signature_type_exprs_match(
          static_cast<const TyFile&>(*a).element.get(),
          static_cast<const TyFile&>(*b).element.get());
    case Kind::TyString:
      return true;
    case Kind::TyArray: {
      const auto& aa = static_cast<const TyArray&>(*a);
      const auto& bb = static_cast<const TyArray&>(*b);
      if (aa.array_kind != bb.array_kind || aa.dims.size() != bb.dims.size()) {
        return false;
      }
      for (size_t i = 0; i < aa.dims.size(); ++i) {
        if (!signature_type_exprs_match(aa.dims[i].get(), bb.dims[i].get())) {
          return false;
        }
      }
      return signature_type_exprs_match(aa.element.get(), bb.element.get());
    }
    case Kind::TyProcedural: {
      const auto& ap = static_cast<const TyProcedural&>(*a);
      const auto& bp = static_cast<const TyProcedural&>(*b);
      return ap.is_function == bp.is_function &&
             ap.is_method == bp.is_method &&
             ap.is_cdecl == bp.is_cdecl &&
             signature_type_exprs_match(ap.return_type.get(),
                                        bp.return_type.get()) &&
             signature_params_match(ap.params, bp.params);
    }
    default:
      return false;
  }
}

bool proc_signature_matches(const ProcDecl& a, const ProcDecl& b) {
  return a.pkind == b.pkind &&
         a.is_operator == b.is_operator &&
         a.is_class_method == b.is_class_method &&
         signature_type_exprs_match(a.return_type.get(), b.return_type.get()) &&
         signature_params_match(a.params, b.params);
}

bool completes_interface_proc(const UnitInfo& ui, const ProcDecl& pd) {
  auto it = ui.iface_procs.find(lc(pd.name));
  if (it == ui.iface_procs.end()) return false;
  for (const ProcInfo& candidate : it->second) {
    if (candidate.decl && proc_signature_matches(*candidate.decl, pd)) {
      return true;
    }
  }
  return false;
}

template <typename Map>
void insert_map_keys(std::unordered_set<std::string>& out, const Map& map) {
  for (const auto& [name, value] : map) {
    (void)value;
    out.insert(name);
  }
}

ProcInfo make_proc_info(const std::string& unit,
                        std::shared_ptr<const ProcDecl> pd_sp) {
  const auto& pd = *pd_sp;
  return ProcInfo{.defining_unit = unit,
                  .decl = std::move(pd_sp),
                  .param_count = proc_param_count(pd.params),
                  .is_function = (pd.pkind == ProcKind::Function),
                  .is_overload = pd.modifiers.is_overload,
                  .accepts_zero_args = proc_accepts_zero_args(pd),
                  .return_type_name = {}};
}

void add_unit_enum_members(UnitInfo* ui, bool is_interface,
                           const TyEnum& te) {
  if (!ui) return;
  auto& members = is_interface ? ui->iface_enum_members
                               : ui->impl_enum_members;
  for (const auto& m : te.members) members.insert(lc(m.name));
}

void add_enum_members(std::unordered_set<std::string>& members,
                      const TypePtr& t) {
  if (!t) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*t);
  for (const TyEnum* te : enums)
    for (const auto& em : te->members) members.insert(lc(em.name));
}

EnumInfoReg make_enum_info(const std::string& unit, const std::string& key,
                           const std::string& cxx_name, const TyEnum& te) {
  std::vector<std::string> members;
  for (const auto& m : te.members) members.push_back(lc(m.name));
  return EnumInfoReg{.name = key,
                     .defining_unit = unit,
                     .type = &te,
                     .cxx_name = cxx_name,
                     .members = std::move(members)};
}

void index_enum_info(TypeRegistry& r, UnitInfo* ui, bool is_interface,
                     const EnumInfoReg& info) {
  if (!info.type) return;
  auto& by_member = r.enum_members_by_unit[info.defining_unit];
  for (const auto& member : info.members) by_member[member] = info.type;
  r.enum_type_info[info.type] = &info;
  add_unit_enum_members(ui, is_interface, *info.type);
}

TypeSymbol* find_unit_type_symbol(UnitInfo& ui, const std::string& name) {
  auto iit = ui.iface_types.find(name);
  if (iit != ui.iface_types.end()) return iit->second;
  auto mit = ui.impl_types.find(name);
  return mit == ui.impl_types.end() ? nullptr : mit->second;
}

void populate_nested_types(TypeRegistry& r, TypeSymbol& symbol);

TypeSymbol* upsert_unit_type_symbol(TypeRegistry& r, UnitInfo* ui,
                                    bool is_interface,
                                    TypeSymbol new_symbol) {
  const std::string low_name = new_symbol.name;
  TypeSymbol* stored = ui ? find_unit_type_symbol(*ui, low_name) : nullptr;
  if (!stored) {
    r.type_symbols.push_back(std::move(new_symbol));
    stored = &r.type_symbols.back();
  } else {
    const ClassInfo* next_class = new_symbol.class_info();
    const ClassInfo* current_class = stored->class_info();
    const bool keep_existing_full_class =
        next_class && current_class && next_class->is_forward &&
        !current_class->is_forward;
    if (!keep_existing_full_class) {
      *stored = std::move(new_symbol);
    }
  }
  populate_nested_types(r, *stored);
  if (ui) {
    if (is_interface || ui->iface_types.count(low_name)) {
      ui->iface_types[low_name] = stored;
    } else {
      ui->impl_types[low_name] = stored;
    }
  }
  return stored;
}

TypeSymbolKind type_symbol_kind_for_decl(const TypeExpr& type) {
  switch (type.kind) {
    case Kind::TyObject:
      return TypeSymbolKind::Class;
    case Kind::TyRecord:
      return TypeSymbolKind::Record;
    case Kind::TyInterface:
      return TypeSymbolKind::Interface;
    case Kind::TyEnum:
      return TypeSymbolKind::Enum;
    default:
      return TypeSymbolKind::Alias;
  }
}

void register_enums_in_type(TypeRegistry& r, UnitInfo* ui, bool is_interface,
                            const std::string& unit, const TypeExpr& t,
                            std::string_view owner_name,
                            const TyEnum* named_top_level) {
  std::vector<const TyEnum*> enums = collect_enum_types(t);
  std::unordered_set<const TyEnum*> seen;
  size_t anon_index = 0;
  for (const TyEnum* te : enums) {
    if (!te) continue;
    if (!seen.insert(te).second) continue;
    if (named_top_level && te == named_top_level) continue;
    const std::string owner = lc(std::string(owner_name));
    const std::string key =
        unit + "$" + owner + "$enum" + std::to_string(anon_index);
    const std::string cxx_name =
        type_mangle(owner) + "_enum" + std::to_string(anon_index);
    r.anonymous_enum_infos.push_back(
        make_enum_info(unit, key, cxx_name, *te));
    index_enum_info(r, ui, is_interface, r.anonymous_enum_infos.back());
    ++anon_index;
  }
}

void append_record_variant_fields(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::unordered_map<std::string, FieldInfo>& fields) {
  if (!vpart) return;
  if (!vpart->tag_name.empty()) {
    fields[lc(vpart->tag_name)] =
        FieldInfo{.type = vpart->tag_type, .is_class_var = false};
  }
  for (const auto& vc : vpart->cases) {
    for (const auto& f : vc.fields) {
      const FieldInfo field{.type = f.type,
                            .is_class_var = false,
                            .is_variant = true};
      for (const auto& n : f.names) fields[lc(n)] = field;
    }
    append_record_variant_fields(vc.variant_part, fields);
  }
}

std::unordered_map<std::string, FieldInfo> record_fields(const TyRecord& tr) {
  std::unordered_map<std::string, FieldInfo> fields;
  for (const auto& f : tr.fields) {
    const FieldInfo field{.type = f.type, .is_class_var = false};
    for (const auto& n : f.names) fields[lc(n)] = field;
  }
  append_record_variant_fields(tr.variant_part, fields);
  return fields;
}

SymKind method_kind_for(const ProcDecl& pd) {
  if (pd.is_class_method) return SymKind::ClassMethod;
  if (pd.pkind == ProcKind::Constructor) return SymKind::Constructor;
  if (pd.pkind == ProcKind::Destructor) return SymKind::Destructor;
  return SymKind::Method;
}

std::string type_source_name(std::string_view name,
                             const std::vector<std::string>& owner_path) {
  std::string out;
  for (const auto& owner : owner_path) {
    if (!out.empty()) out += ".";
    out += owner;
  }
  if (!out.empty()) out += ".";
  out += lc(std::string(name));
  return out;
}

MethodSig method_sig_for(std::string defining_unit,
                         std::string declaring_type,
                         std::shared_ptr<const ProcDecl> method) {
  const auto& pd = *method;
  std::string result_type =
      pd.pkind == ProcKind::Constructor ? declaring_type : std::string{};
  return MethodSig{.kind = method_kind_for(pd),
                   .defining_unit = std::move(defining_unit),
                   .declaring_type = std::move(declaring_type),
                   .param_count = proc_param_count(pd.params),
                   .accepts_zero_args = proc_accepts_zero_args(pd),
                   .is_function = (pd.pkind == ProcKind::Function),
                   .is_virtual = pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override,
                   .is_final = pd.modifiers.is_final,
                   .is_overload = pd.modifiers.is_overload,
                   .return_type_name = std::move(result_type),
                   .decl = std::move(method)};
}

std::unordered_map<std::string, FieldInfo> class_fields(const TyObject& to) {
  std::unordered_map<std::string, FieldInfo> fields;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Field) {
      const FieldInfo field{.type = m.field_type,
                            .is_class_var = m.is_class_var};
      for (const auto& n : m.field_names) fields[lc(n)] = field;
    }
  }
  return fields;
}

std::unordered_set<std::string> class_enum_members(const TyObject& to) {
  std::unordered_set<std::string> members;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Field) add_enum_members(members, m.field_type);
  }
  return members;
}

std::unordered_map<std::string, std::vector<MethodSig>> class_methods(
    const std::string& defining_unit, std::string_view declaring_type,
    const TyObject& to) {
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Method && m.method) {
      methods[lc(m.method->name)].push_back(
          method_sig_for(defining_unit, std::string(declaring_type), m.method));
    }
  }
  return methods;
}

std::unordered_map<std::string, PropertyInfo> class_properties(
    const TyObject& to) {
  std::unordered_map<std::string, PropertyInfo> properties;
  for (const auto& m : to.members) {
    if (m.kind != ObjectMemberKind::Property) continue;
    properties[lc(m.property.name)] =
        PropertyInfo{.type = m.property.type,
                     .params = m.property.params,
                     .read = {},
                     .write = {},
                     .is_default = m.property.is_default};
  }
  return properties;
}

std::string class_default_property_name(const TyObject& to) {
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Property && m.property.is_default) {
      return lc(m.property.name);
    }
  }
  return {};
}

std::vector<std::string> lower_names(const std::vector<std::string>& names) {
  std::vector<std::string> out;
  out.reserve(names.size());
  for (const auto& name : names) out.push_back(lc(name));
  return out;
}

std::unordered_map<std::string, std::vector<MethodSig>> interface_methods(
    const std::string& defining_unit, const TyInterface& ti) {
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  for (const auto& m : ti.members) {
    if (m.kind != ObjectMemberKind::Method || !m.method) continue;
    const auto& pd = *m.method;
    methods[lc(pd.name)].push_back(MethodSig{
        .kind = SymKind::Method,
        .defining_unit = defining_unit,
        .declaring_type = {},
        .param_count = proc_param_count(pd.params),
        .accepts_zero_args = proc_accepts_zero_args(pd),
        .is_function = (pd.pkind == ProcKind::Function),
        .is_virtual = false,
        .is_final = false,
        .return_type_name = {},
        .decl = m.method});
  }
  return methods;
}

ClassInfo class_info_for(const std::string& unit, const std::string& name,
                         const std::vector<std::string>& owner_path,
                         const TyObject& to) {
  const std::string declaring_type = type_source_name(name, owner_path);
  return ClassInfo{.name = name,
                   .parent = (to.is_reference_type && to.parent.empty())
                                 ? "__rt__.tobject"
                                 : lc(to.parent),
                   .defining_unit = unit,
                   .is_reference_type = to.is_reference_type,
                   .is_abstract = to.is_abstract,
                   .is_forward = to.is_forward,
                   .interfaces = lower_names(to.interfaces),
                   .fields = class_fields(to),
                   .methods = class_methods(unit, declaring_type, to),
                   .properties = class_properties(to),
                   .nested_types = {},
                   .enum_members = class_enum_members(to),
                   .default_property_name = class_default_property_name(to)};
}

InterfaceInfo interface_info_for(const std::string& unit,
                                 const std::string& name,
                                 const TyInterface& ti) {
  return InterfaceInfo{.name = name,
                       .defining_unit = unit,
                       .metadata_string = ti.metadata_string,
                       .methods = interface_methods(unit, ti)};
}

RecordInfo record_info_for(const std::string& unit, const std::string& name,
                           const TyRecord& tr) {
  return RecordInfo{.name = name,
                    .defining_unit = unit,
                    .is_packed = tr.is_packed,
                    .fields = record_fields(tr),
                    .nested_types = {}};
}

TypeSymbol make_type_symbol_for_type_with_owner(
    std::string_view unit, std::string_view name,
    std::shared_ptr<const TypeExpr> type,
    std::vector<std::string> owner_path);

std::shared_ptr<TypeSymbol> upsert_nested_type_symbol(
    TypeRegistry& r, TypeSymbol& owner, const TypeDecl& td,
    const std::vector<std::string>& child_owner_path) {
  // The registry is the lifetime owner for nested type symbols. Owner
  // ClassInfo/RecordInfo maps and emitter scope frames only keep shared/ref
  // indexes to this symbol, so a nested declaration has one semantic identity
  // across declaration lookup, type rendering, and alias canonicalization.
  const std::string key =
      nested_type_symbol_key(owner.defining_unit, child_owner_path, td.name);
  TypeSymbol next = make_type_symbol_for_type_with_owner(
      owner.defining_unit, td.name, td.type, child_owner_path);
  auto& slot = r.nested_type_symbols[key];
  if (!slot) {
    slot = std::make_shared<TypeSymbol>(std::move(next));
  } else {
    *slot = std::move(next);
  }
  populate_nested_types(r, *slot);
  return slot;
}

void populate_nested_types(TypeRegistry& r, TypeSymbol& symbol) {
  if (!symbol.type) return;
  auto* nested = nested_type_map_mut(symbol);
  if (!nested) return;

  std::vector<std::shared_ptr<TypeDecl>> nested_decls;
  if (symbol.type->kind == Kind::TyObject) {
    const auto& obj = static_cast<const TyObject&>(*symbol.type);
    for (const auto& member : obj.members) {
      if (member.kind == ObjectMemberKind::Type && member.type_decl) {
        nested_decls.push_back(member.type_decl);
      }
    }
  } else if (symbol.type->kind == Kind::TyRecord) {
    const auto& rec = static_cast<const TyRecord&>(*symbol.type);
    nested_decls = rec.nested_types;
  } else {
    return;
  }

  std::vector<std::string> child_owner_path = symbol.owner_path;
  child_owner_path.push_back(symbol.name);
  std::unordered_set<std::string> wanted;
  for (const auto& td : nested_decls) {
    if (!td || !td->type) continue;
    auto child = upsert_nested_type_symbol(r, symbol, *td, child_owner_path);
    wanted.insert(child->name);
    if (ClassInfo* ci = symbol.mutable_class_info()) {
      ci->nested_types.insert_or_assign(child->name, child);
    } else if (RecordInfo* ri = symbol.mutable_record_info()) {
      ri->nested_types.insert_or_assign(child->name, child);
    }
  }
  for (auto it = nested->begin(); it != nested->end();) {
    if (wanted.count(it->first)) {
      ++it;
    } else {
      it = nested->erase(it);
    }
  }
}

TypeSymbol make_type_symbol_for_type_with_owner(
    std::string_view unit, std::string_view name,
    std::shared_ptr<const TypeExpr> type,
    std::vector<std::string> owner_path) {
  const std::string low_name = lc(std::string(name));
  const std::string low_unit = lc(std::string(unit));
  if (!type) {
    throw std::logic_error("make_type_symbol_for_type called without a type");
  }
  TypeSymbolPayload payload =
      AliasInfo{.defining_unit = low_unit, .target = type};
  switch (type_symbol_kind_for_decl(*type)) {
    case TypeSymbolKind::Class:
      payload = class_info_for(low_unit, low_name, owner_path,
                               static_cast<const TyObject&>(*type));
      break;
    case TypeSymbolKind::Record:
      payload = record_info_for(low_unit, low_name,
                                static_cast<const TyRecord&>(*type));
      break;
    case TypeSymbolKind::Interface:
      payload = interface_info_for(low_unit, low_name,
                                   static_cast<const TyInterface&>(*type));
      break;
    case TypeSymbolKind::Enum:
      payload = make_enum_info(low_unit, low_name, type_mangle(low_name),
                               static_cast<const TyEnum&>(*type));
      break;
    case TypeSymbolKind::Alias:
      break;
  }
  TypeSymbol symbol(low_name, low_unit, std::move(type), std::move(payload));
  symbol.owner_path = std::move(owner_path);
  return symbol;
}

UnitInfo unit_info_for(
    std::string name, std::vector<std::string> interface_uses = {},
    std::vector<std::string> implementation_uses = {},
    std::unordered_map<std::string, std::vector<ProcInfo>> iface_procs = {}) {
  return UnitInfo{.name = std::move(name),
                  .interface_uses = std::move(interface_uses),
                  .implementation_uses = std::move(implementation_uses),
                  .iface_vars = {},
                  .iface_consts = {},
                  .iface_procs = std::move(iface_procs),
                  .iface_operators = {},
                  .iface_types = {},
                  .iface_enum_members = {},
                  .impl_vars = {},
                  .impl_consts = {},
                  .impl_procs = {},
                  .impl_operators = {},
                  .impl_types = {},
                  .impl_enum_members = {}};
}

TypePtr runtime_type_name(std::string name) {
  return TypePtr(const_cast<TyName*>(named_pascal_type(name)), [](TypeExpr*) {});
}

Param runtime_const_param(std::string name, TypePtr type) {
  return Param(Param::Const, {std::move(name)}, std::move(type));
}

std::shared_ptr<ProcDecl> runtime_proc_decl(
    ProcKind pkind, std::string name, std::vector<Param> params,
    TypePtr return_type = nullptr, bool class_method = false,
    bool is_operator = false, std::string operator_token = {},
    ProcDecl::IntrinsicOperator intrinsic_operator =
        ProcDecl::IntrinsicOperator::None) {
  return std::make_shared<ProcDecl>(
      Location{}, pkind, std::move(name), is_operator,
      std::move(operator_token), intrinsic_operator, std::string{},
      class_method, std::move(params), std::move(return_type), ProcModifiers{},
      std::vector<DeclPtr>{}, nullptr);
}

TypePtr runtime_pointer_type(TypePtr target = nullptr) {
  return std::make_shared<TyPointer>(Location{}, std::move(target));
}

RecordField runtime_record_field(std::string name, TypePtr type) {
  return RecordField({std::move(name)}, std::move(type));
}

TypePtr runtime_record_type(std::vector<RecordField> fields) {
  return std::make_shared<TyRecord>(Location{}, std::move(fields), nullptr,
                                    false);
}

TypePtr runtime_set_type(TypePtr element) {
  return std::make_shared<TySet>(Location{}, std::move(element));
}

TypePtr runtime_procedural_type(bool is_function, std::vector<Param> params,
                                TypePtr return_type = nullptr) {
  return std::make_shared<TyProcedural>(Location{}, is_function,
                                        std::move(params),
                                        std::move(return_type), false, false);
}

TypePtr runtime_enum_type(std::vector<std::string> names) {
  std::vector<EnumMember> members;
  for (auto& name : names) {
    members.push_back(EnumMember(std::move(name)));
  }
  return std::make_shared<TyEnum>(Location{}, 4, std::move(members));
}

ExprPtr runtime_int_literal(int64_t value) {
  return std::make_shared<IntLit>(static_cast<uint64_t>(value));
}

void register_runtime_var(UnitInfo& rt_exports, std::string name,
                          std::shared_ptr<const TypeExpr> type) {
  rt_exports.iface_vars[lc(std::move(name))] =
      VarInfo{.defining_unit = "__rt__", .type = std::move(type)};
}

void register_runtime_const(UnitInfo& rt_exports, std::string name,
                            std::shared_ptr<const TypeExpr> type,
                            std::shared_ptr<const Expr> value) {
  rt_exports.iface_consts[lc(std::move(name))] =
      ConstInfo{.defining_unit = "__rt__",
                .type = std::move(type),
                .value = std::move(value)};
}

void register_runtime_alias(TypeRegistry& r, UnitInfo& rt_exports,
                            std::string name,
                            std::shared_ptr<const TypeExpr> target) {
  const std::string low = lc(std::move(name));
  if (!target) return;
  TypeSymbol* symbol = upsert_unit_type_symbol(
      r, &rt_exports, /*is_interface=*/true,
      make_type_symbol_for_type("__rt__", low, std::move(target)));
  if (const EnumInfoReg* info = symbol->enum_info()) {
    index_enum_info(r, &rt_exports, /*is_interface=*/true, *info);
  }
  if (symbol->type) {
    register_enums_in_type(
        r, &rt_exports, /*is_interface=*/true, "__rt__", *symbol->type, low,
        symbol->type->kind == Kind::TyEnum
            ? static_cast<const TyEnum*>(symbol->type)
            : nullptr);
  }
}

void register_runtime_string_compare_operator(UnitInfo& rt_exports,
                                              std::string op,
                                              TypePtr lhs_type,
                                              TypePtr rhs_type,
                                              TypePtr return_type) {
  auto pd = runtime_proc_decl(
      ProcKind::Function, "operator_" + op,
      {runtime_const_param("a", std::move(lhs_type)),
       runtime_const_param("b", std::move(rhs_type))},
      std::move(return_type), false, true, op,
      ProcDecl::IntrinsicOperator::StringCompare);
  rt_exports.iface_operators[op].push_back(make_proc_info("__rt__", pd));
}

void register_runtime_proc(UnitInfo& rt_exports, ProcKind pkind,
                           std::string name, std::vector<Param> params,
                           TypePtr return_type = nullptr) {
  auto pd = runtime_proc_decl(pkind, std::move(name), std::move(params),
                              std::move(return_type));
  rt_exports.iface_procs[lc(pd->name)].push_back(make_proc_info("__rt__", pd));
}

void register_runtime_same_type_unary_overloads(
    UnitInfo& rt_exports, std::string_view name,
    std::span<const std::string_view> types) {
  for (std::string_view type : types) {
    register_runtime_proc(
        rt_exports, ProcKind::Function, std::string(name),
        {runtime_const_param("value", runtime_type_name(std::string(type)))},
        runtime_type_name(std::string(type)));
  }
}

MethodSig runtime_method_sig(std::string name, ProcKind pkind,
                             std::vector<Param> params,
                             TypePtr return_type = nullptr,
                             bool class_method = false) {
  auto pd = runtime_proc_decl(pkind, std::move(name), std::move(params),
                              std::move(return_type), class_method);
  const size_t param_count = proc_param_count(pd->params);
  return MethodSig{
      .kind = (pkind == ProcKind::Constructor) ? SymKind::Constructor
              : (pkind == ProcKind::Destructor) ? SymKind::Destructor
              : class_method ? SymKind::ClassMethod
                             : SymKind::Method,
      .defining_unit = "__rt__",
      .declaring_type = {},
      .param_count = param_count,
      .accepts_zero_args = (param_count == 0),
      .is_function = (pkind == ProcKind::Function),
      .is_virtual = false,
      .is_final = false,
      .return_type_name = {},
      .decl = std::move(pd)};
}

void register_runtime_class(TypeRegistry& r, std::string name,
                            std::string parent,
                            std::vector<MethodSig> methods) {
  const std::string key = name;
  std::unordered_map<std::string, std::vector<MethodSig>> method_map;
  for (auto& m : methods) {
    if (m.declaring_type.empty()) {
      m.declaring_type = key;
    }
    if (m.kind == SymKind::Constructor && m.return_type_name.empty()) {
      m.return_type_name = key;
    }
    const std::string method_name = m.decl->name;
    method_map[method_name].push_back(std::move(m));
  }
  ClassInfo info{.name = key,
                 .parent = std::move(parent),
                 .defining_unit = "__rt__",
                 .is_reference_type = true,
                 .is_abstract = false,
                 .is_forward = false,
                 .interfaces = {},
                 .fields = {},
                 .methods = std::move(method_map),
                 .properties = {},
                 .nested_types = {},
                 .enum_members = {},
                 .default_property_name = {}};
  r.rt_classes[key] = info;
  if (auto rt = r.units.find("__rt__"); rt != r.units.end()) {
    upsert_unit_type_symbol(
        r, &rt->second, /*is_interface=*/true,
        TypeSymbol(key, "__rt__", runtime_type_name(key), std::move(info)));
  }
}

void register_runtime_class_field(TypeRegistry& r, std::string class_name,
                                  std::string field_name, TypePtr type) {
  const std::string low_class = lc(std::move(class_name));
  const std::string low_field = lc(std::move(field_name));
  auto it = r.rt_classes.find(low_class);
  if (it == r.rt_classes.end()) return;
  FieldInfo field{.type = std::move(type), .is_class_var = false};
  if (TypeSymbol* symbol =
          r.lookup_type_symbol_exact_mut("__rt__", low_class)) {
    if (ClassInfo* info = symbol->mutable_class_info()) {
      info->fields[low_field] = field;
    }
  }
  it->second.fields[low_field] = std::move(field);
}

void register_runtime_backed_unit(TypeRegistry& r, std::string used_name) {
  const std::string low = lc(std::move(used_name));
  if (low == "__rt__" || r.units.count(low) > 0) return;
  const RuntimeUnitModel* model = runtime_unit_model(low);
  if (!model) return;
  auto rt = r.units.find("__rt__");
  if (rt == r.units.end()) {
    report_error(Location{}, "runtime-backed unit `" + low +
                             "` cannot be registered before runtime exports");
    return;
  }

  UnitInfo unit = unit_info_for(low);
  for (const RuntimeUnitExport& export_info : model->exports) {
    const std::string name = lc(std::string(export_info.name));
    switch (export_info.kind) {
      case RuntimeUnitExportKind::TypeAlias: {
        TypeSymbol* symbol = r.lookup_type_symbol_exact_mut("__rt__", name);
        if (!symbol) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime type `" +
                                   name + "`");
          continue;
        }
        unit.iface_types[name] = symbol;
        break;
      }
      case RuntimeUnitExportKind::RuntimeClass: {
        auto cls = r.rt_classes.find(name);
        if (cls == r.rt_classes.end()) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime class `" +
                                   name + "`");
          continue;
        }
        ClassInfo info = cls->second;
        info.defining_unit = low;
        TypeSymbol& symbol = r.type_symbols.emplace_back(
            name, low, static_cast<const TypeExpr*>(nullptr),
            TypeSymbolPayload{std::move(info)});
        unit.iface_types[name] = &symbol;
        break;
      }
      case RuntimeUnitExportKind::Proc: {
        if (export_info.param_count >= 0) {
          unit.iface_procs[name].push_back(ProcInfo{
              .defining_unit = low,
              .decl = nullptr,
              .param_count = static_cast<size_t>(export_info.param_count),
              .is_function = export_info.is_function,
              .accepts_zero_args = export_info.param_count == 0,
              .return_type_name = std::string(export_info.return_type_name)});
          break;
        }
        const std::vector<ProcInfo>* procs =
            rt->second.find_export_procs(name);
        if (!procs) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime proc `" +
                                   name + "`");
          continue;
        }
        unit.iface_procs[name] = *procs;
        break;
      }
      case RuntimeUnitExportKind::Var: {
        const VarInfo* var = rt->second.find_export_var(name);
        if (!var) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime var `" +
                                   name + "`");
          continue;
        }
        unit.iface_vars[name] = *var;
        break;
      }
      case RuntimeUnitExportKind::Const: {
        const ConstInfo* constant = rt->second.find_export_const(name);
        if (!constant) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime const `" +
                                   name + "`");
          continue;
        }
        unit.iface_consts[name] = *constant;
        break;
      }
    }
  }
  r.units[low] = std::move(unit);
}

class SeenClassChain {
 public:
  bool mark(const ClassInfo* cls) {
    if (!cls) return false;
    for (size_t i = 0; i < inline_count_; ++i) {
      if (inline_seen_[i] == cls) return false;
    }
    if (!overflow_seen_.empty()) {
      return overflow_seen_.insert(cls).second;
    }
    if (inline_count_ < inline_seen_.size()) {
      inline_seen_[inline_count_++] = cls;
      return true;
    }
    // Member lookup is a hot path during type deduction. ClassInfo addresses
    // are stable registry identities, so only genuinely deep inheritance chains
    // need heap storage for cycle detection.
    overflow_seen_.reserve(inline_seen_.size() + 1);
    for (const ClassInfo* seen : inline_seen_) {
      overflow_seen_.insert(seen);
    }
    return overflow_seen_.insert(cls).second;
  }

 private:
  std::array<const ClassInfo*, 16> inline_seen_{};
  size_t inline_count_ = 0;
  std::unordered_set<const ClassInfo*> overflow_seen_;
};

std::vector<std::string> lower_path(const PropertyDecl::Accessor& accessor) {
  std::vector<std::string> out;
  out.reserve(accessor.path.size());
  for (const auto& segment : accessor.path) out.push_back(lc(segment));
  return out;
}

std::string direct_type_name_for_registry(
    const TypeRegistry& r, const TypeExpr* te, std::string_view current_unit) {
  if (!te) return {};
  if (te->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*te);
    if (const TypeSymbol* symbol = r.resolved_symbol_for_type(te)) {
      return type_symbol_path(*symbol);
    }
    if (const TypeSymbol* symbol = r.lookup_type_symbol(n.name, current_unit)) {
      return type_symbol_path(*symbol);
    }
    return lc(n.name);
  }
  return {};
}

std::optional<std::string> resolve_field_accessor_cxx(
    const TypeRegistry& r, const ClassInfo& owner,
    std::string_view owner_lookup_name,
    const std::vector<std::string>& path) {
  if (path.empty()) return std::nullopt;

  const FieldInfo* field =
      r.lookup_class_field(std::string(owner_lookup_name), path.front(),
                           owner.defining_unit);
  if (!field) return std::nullopt;

  std::string out = r.field_cxx_name(path.front());
  const TypeExpr* current_type = field->type.get();
  for (size_t i = 1; i < path.size(); ++i) {
    const std::string nested_owner =
        direct_type_name_for_registry(r, current_type, owner.defining_unit);
    if (nested_owner.empty()) return std::nullopt;

    const FieldInfo* nested = nullptr;
    std::string access = ".";
    if (const ClassInfo* nested_class =
            r.lookup_class(nested_owner, owner.defining_unit)) {
      access = nested_class->is_reference_type ? "->" : ".";
      nested =
          r.lookup_class_field(nested_owner, path[i], owner.defining_unit);
    } else {
      nested = r.lookup_record_field(nested_owner, path[i],
                                     owner.defining_unit);
    }
    if (!nested) return std::nullopt;

    out += access + r.field_cxx_name(path[i]);
    current_type = nested->type.get();
  }
  return out;
}

PropertyAccessorInfo resolve_property_accessor(
    const TypeRegistry& r, const ClassInfo& owner,
    std::string_view owner_lookup_name,
    const PropertyDecl::Accessor& accessor) {
  std::vector<std::string> path = lower_path(accessor);
  if (path.empty()) return PropertyAccessorInfo{};

  if (auto cxx_path =
          resolve_field_accessor_cxx(r, owner, owner_lookup_name, path)) {
    return PropertyAccessorInfo{.kind = PropertyAccessorKind::FieldPath,
                                .path = std::move(path),
                                .cxx_path = *cxx_path,
                                .method_name = {}};
  }

  if (path.size() == 1 &&
      r.lookup_class_methods(std::string(owner_lookup_name), path.front(),
                             owner.defining_unit)) {
    return PropertyAccessorInfo{.kind = PropertyAccessorKind::Method,
                                .path = path,
                                .cxx_path = {},
                                .method_name = path.front()};
  }

  return PropertyAccessorInfo{.kind = PropertyAccessorKind::Unsupported,
                              .path = std::move(path),
                              .cxx_path = {},
                              .method_name = {}};
}

void resolve_property_accessors_in_symbol(TypeRegistry& r,
                                          TypeSymbol& symbol) {
  if (ClassInfo* ci = symbol.mutable_class_info()) {
    if (symbol.type && symbol.type->kind == Kind::TyObject) {
      const auto& to = static_cast<const TyObject&>(*symbol.type);
      const std::string owner_lookup_name = type_symbol_path(symbol);
      for (const auto& member : to.members) {
        if (member.kind != ObjectMemberKind::Property) continue;
        auto pit = ci->properties.find(lc(member.property.name));
        if (pit == ci->properties.end()) continue;
        pit->second.read = resolve_property_accessor(
            r, *ci, owner_lookup_name, member.property.read_accessor);
        pit->second.write = resolve_property_accessor(
            r, *ci, owner_lookup_name, member.property.write_accessor);
      }
    }
  }

  // Nested classes are stored in the registry as TypeSymbols, not unit-level
  // declarations. Resolve their property accessors here so a nested property
  // such as Outer.Inner.Current can be emitted through the same lookup path as
  // a top-level class property.
  if (auto* nested = nested_type_map_mut(symbol)) {
    for (auto& [_, child] : *nested) {
      if (child) resolve_property_accessors_in_symbol(r, *child);
    }
  }
}

void resolve_property_accessors_from_decls(TypeRegistry& r,
                                           std::string_view unit,
                                           const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (!d || d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type || td.type->kind != Kind::TyObject) continue;

    TypeSymbol* symbol = r.lookup_type_symbol_exact_mut(unit, td.name);
    if (!symbol) continue;
    resolve_property_accessors_in_symbol(r, *symbol);
  }
}

void register_decl_list(TypeRegistry& r, const std::string& unit,
                        const std::vector<DeclPtr>& decls,
                        bool is_interface) {
  UnitInfo* ui = nullptr;
  {
    auto it = r.units.find(unit);
    if (it != r.units.end()) ui = &it->second;
  }
  for (const auto& d : decls) {
    if (!d) continue;
    switch (d->kind) {
      case Kind::TypeDecl: {
        const auto& td = static_cast<const TypeDecl&>(*d);
        if (!td.type) continue;
        index_type_expr_unit(r, td.type.get(), unit);
        std::string nm = lc(td.name);
        TypeSymbol* symbol = upsert_unit_type_symbol(
            r, ui, is_interface,
            make_type_symbol_for_type(unit, nm, td.type));
        if (symbol->enum_info()) {
          index_enum_info(r, ui, is_interface, *symbol->enum_info());
        }
        register_enums_in_type(
            r, ui, is_interface, unit, *td.type, nm,
            td.type->kind == Kind::TyEnum
                ? &static_cast<const TyEnum&>(*td.type)
                : nullptr);
        break;
      }
      case Kind::ProcDecl: {
        auto pd_sp = std::static_pointer_cast<const ProcDecl>(d);
        const auto& pd = *pd_sp;
        index_proc_signature_type_units(r, pd, unit);
        if (!pd.of_type.empty()) continue;  // method body -- class handles it
        if (pd.is_operator) {
          if (pd.modifiers.is_forward) break;
          if (ui) {
            auto& ops = is_interface ? ui->iface_operators
                                     : ui->impl_operators;
            ops[pd.operator_token].push_back(make_proc_info(unit, pd_sp));
          }
          break;
        }
        // Forward decls are bound to a real implementation later in the
        // same unit; registering both as separate ProcInfos would make
        // overload resolution see two identically-typed candidates and
        // (correctly) flag the call ambiguous. Skip the forward stub --
        // the implementation pass will register the real one.
        if (pd.modifiers.is_forward) break;
        if (!is_interface && ui && completes_interface_proc(*ui, pd)) break;
        if (ui) (is_interface ? ui->iface_procs : ui->impl_procs)[lc(pd.name)]
                    .push_back(make_proc_info(unit, pd_sp));
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        index_type_expr_unit(r, vd.type.get(), unit);
        const VarInfo var{.defining_unit = unit, .type = vd.type};
        for (const auto& n : vd.names) {
          if (ui) (is_interface ? ui->iface_vars : ui->impl_vars)[lc(n)] = var;
        }
        if (vd.type && !vd.names.empty()) {
          register_enums_in_type(r, ui, is_interface, unit, *vd.type,
                                 lc(vd.names.front()), nullptr);
        }
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        index_type_expr_unit(r, cd.type.get(), unit);
        if (ui)
          (is_interface ? ui->iface_consts : ui->impl_consts)[lc(cd.name)] =
              ConstInfo{.defining_unit = unit,
                        .type = cd.type,
                        .value = cd.value};
        if (cd.type) {
          register_enums_in_type(r, ui, is_interface, unit, *cd.type,
                                 lc(cd.name), nullptr);
        }
        break;
      }
      default:
        break;
    }
  }
}

TypeLookupContext* make_scope_frame(TypeRegistry& r, std::string_view unit,
                                    const TypeLookupContext* parent,
                                    ScopeFrameKind kind,
                                    const UnitInfo* unit_info) {
  r.type_lookup_context_storage.push_back(
      TypeLookupContext{.unit = lc(std::string(unit)),
                        .parent = parent,
                        .kind = kind,
                        .unit_info = unit_info,
                        .type_symbols = {}});
  return &r.type_lookup_context_storage.back();
}

TypeLookupContext* make_type_lookup_context(
    TypeRegistry& r, std::string_view unit, const TypeLookupContext* parent) {
  return make_scope_frame(r, unit, parent, ScopeFrameKind::Local, nullptr);
}

void insert_type_ref(TypeLookupContext& context, const TypeSymbol* symbol) {
  if (!symbol) return;
  context.type_symbols.insert_or_assign(symbol->name, symbol);
}

TypeSymbol* register_context_type_decl(TypeRegistry& r,
                                       TypeLookupContext& context,
                                       const TypeDecl& td) {
  if (!td.type) return nullptr;
  r.type_symbols.push_back(make_type_symbol_for_type({}, td.name, td.type));
  TypeSymbol* stored = &r.type_symbols.back();
  populate_nested_types(r, *stored);
  insert_type_ref(context, stored);
  return stored;
}

void register_context_enum_symbol(TypeRegistry& r, TypeLookupContext& context,
                                  std::string_view name,
                                  std::string_view cxx_name,
                                  const TyEnum& type) {
  r.type_symbols.push_back(
      make_enum_type_symbol({}, name, cxx_name, type));
  insert_type_ref(context, &r.type_symbols.back());
}

void register_context_enum_symbols_for_owner(TypeRegistry& r,
                                             TypeLookupContext& context,
                                             const TypePtr& type,
                                             std::string_view owner_name,
                                             const TyEnum* named_top_level =
                                                 nullptr) {
  if (!type) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*type);
  std::unordered_set<const TyEnum*> seen;
  size_t anon_index = 0;
  const std::string owner = lc(std::string(owner_name));
  for (const TyEnum* te : enums) {
    if (!te || !seen.insert(te).second) continue;
    if (named_top_level && te == named_top_level) {
      register_context_enum_symbol(r, context, owner, type_mangle(owner), *te);
      continue;
    }
    const bool whole_type_is_enum = te == type.get() && !named_top_level;
    const std::string key = whole_type_is_enum
                                ? owner
                                : owner + "_enum" + std::to_string(anon_index);
    const std::string cxx_name =
        whole_type_is_enum ? type_mangle(owner)
                           : type_mangle(owner) + "_enum" +
                                 std::to_string(anon_index);
    register_context_enum_symbol(r, context, key, cxx_name, *te);
    ++anon_index;
  }
}

void register_decl_list_context_symbols(TypeRegistry& r,
                                        TypeLookupContext& context,
                                        const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (!d) continue;
    if (d->kind == Kind::TypeDecl) {
      const auto& td = static_cast<const TypeDecl&>(*d);
      register_context_type_decl(r, context, td);
    } else if (d->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*d);
      if (!vd.names.empty()) {
        register_context_enum_symbols_for_owner(
            r, context, vd.type, vd.names.front());
      }
    } else if (d->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*d);
      register_context_enum_symbols_for_owner(
          r, context, cd.type, cd.name);
    }
  }
}

TypeLookupContext* make_unit_type_context(TypeRegistry& r,
                                          std::string_view unit,
                                          bool is_interface) {
  auto uit = r.units.find(lc(std::string(unit)));
  if (uit == r.units.end()) return make_type_lookup_context(r, unit, nullptr);

  const TypeLookupContext* chain = nullptr;
  if (auto rt = r.units.find("__rt__"); rt != r.units.end()) {
    chain = make_scope_frame(r, "__rt__", nullptr,
                             ScopeFrameKind::ImportedUnitInterface,
                             &rt->second);
  }
  auto push_import_frames =
      [&](const std::vector<std::string>& uses,
          const TypeLookupContext* parent) -> const TypeLookupContext* {
    const TypeLookupContext* top = parent;
    for (const std::string& used_name : uses) {
      auto used = r.units.find(used_name);
      if (used == r.units.end()) continue;
      // A used unit's own interface `uses` can type that unit's exported
      // declarations, but it is not re-exported as an unqualified or
      // qualified namespace to consumers of the unit.
      top = make_scope_frame(r, used_name, top,
                             ScopeFrameKind::ImportedUnitInterface,
                             &used->second);
    }
    return top;
  };

  chain = push_import_frames(uit->second.interface_uses, chain);
  if (!is_interface) {
    chain = push_import_frames(uit->second.implementation_uses, chain);
  }
  TypeLookupContext* interface_frame =
      make_scope_frame(r, unit, chain, ScopeFrameKind::UnitInterface,
                       &uit->second);
  if (is_interface) return interface_frame;
  return make_scope_frame(r, unit, interface_frame,
                          ScopeFrameKind::UnitImplementation,
                          &uit->second);
}

const TypeLookupContext* make_type_member_context(
    TypeRegistry& r, const TypeSymbol* symbol,
    const TypeLookupContext* parent) {
  if (!symbol) return parent;
  // Member declarations are looked up in the enclosing type's lexical scope
  // first, then in the unit/import scope represented by the parent context.
  TypeLookupContext* context =
      make_type_lookup_context(r, symbol->defining_unit, parent);
  if (const auto* nested = nested_type_map(*symbol)) {
    for (const auto& [_, child] : *nested) {
      insert_type_ref(*context, child.get());
    }
  }
  return context;
}

void index_type_expr_context(TypeRegistry& r, const TypeExpr* type,
                             const TypeLookupContext* context,
                             const TypeSymbol* symbol = nullptr);

void index_param_type_contexts(TypeRegistry& r, const std::vector<Param>& params,
                               const TypeLookupContext* context) {
  for (const auto& param : params) {
    index_type_expr_context(r, param.type.get(), context);
  }
}

void index_proc_signature_context(TypeRegistry& r, const ProcDecl& proc,
                                  const TypeLookupContext* context) {
  index_param_type_contexts(r, proc.params, context);
  index_type_expr_context(r, proc.return_type.get(), context);
}

void index_variant_type_contexts(TypeRegistry& r,
                                 const std::shared_ptr<VariantPart>& variant,
                                 const TypeLookupContext* context) {
  if (!variant) return;
  index_type_expr_context(r, variant->tag_type.get(), context);
  for (const auto& vcase : variant->cases) {
    for (const auto& field : vcase.fields) {
      index_type_expr_context(r, field.type.get(), context);
    }
    index_variant_type_contexts(r, vcase.variant_part, context);
  }
}

const TypeSymbol* nested_type_symbol_for_decl(const TypeSymbol* owner,
                                              const TypeDecl& td) {
  if (!owner) return nullptr;
  const auto* nested = nested_type_map(*owner);
  if (!nested) return nullptr;
  auto it = nested->find(lc(td.name));
  return it == nested->end() ? nullptr : it->second.get();
}

void index_type_decl_context(TypeRegistry& r, const TypeDecl& td,
                             const TypeLookupContext* context,
                             const TypeSymbol* symbol) {
  index_type_expr_context(r, td.type.get(), context, symbol);
}

void index_object_member_type_context(TypeRegistry& r,
                                      const ObjectMember& member,
                                      const TypeLookupContext* context,
                                      const TypeSymbol* owner_symbol) {
  switch (member.kind) {
    case ObjectMemberKind::Field:
      index_type_expr_context(r, member.field_type.get(), context);
      break;
    case ObjectMemberKind::Method:
      if (member.method) {
        r.proc_signature_type_contexts[member.method.get()] = context;
        index_proc_signature_context(r, *member.method, context);
      }
      break;
    case ObjectMemberKind::Property:
      index_param_type_contexts(r, member.property.params, context);
      index_type_expr_context(r, member.property.type.get(), context);
      break;
    case ObjectMemberKind::Type:
      if (member.type_decl) {
        index_type_decl_context(
            r, *member.type_decl, context,
            nested_type_symbol_for_decl(owner_symbol, *member.type_decl));
      }
      break;
  }
}

void index_type_expr_context(TypeRegistry& r, const TypeExpr* type,
                             const TypeLookupContext* context,
                             const TypeSymbol* symbol) {
  if (!type || !context) return;
  r.type_lookup_contexts.try_emplace(type, context);
  switch (type->kind) {
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(*type);
      for (const auto& dim : a.dims) {
        index_type_expr_context(r, dim.get(), context);
      }
      index_type_expr_context(r, a.element.get(), context);
      break;
    }
    case Kind::TyRecord: {
      const auto& rec = static_cast<const TyRecord&>(*type);
      const TypeLookupContext* member_context =
          make_type_member_context(r, symbol, context);
      for (const auto& field : rec.fields) {
        index_type_expr_context(r, field.type.get(), member_context);
      }
      for (const auto& nested : rec.nested_types) {
        if (!nested) continue;
        index_type_decl_context(
            r, *nested, member_context,
            nested_type_symbol_for_decl(symbol, *nested));
      }
      index_variant_type_contexts(r, rec.variant_part, member_context);
      break;
    }
    case Kind::TyObject: {
      const auto& obj = static_cast<const TyObject&>(*type);
      const TypeLookupContext* member_context =
          make_type_member_context(r, symbol, context);
      for (const auto& member : obj.members) {
        index_object_member_type_context(r, member, member_context, symbol);
      }
      break;
    }
    case Kind::TyInterface: {
      const auto& intf = static_cast<const TyInterface&>(*type);
      for (const auto& member : intf.members) {
        index_object_member_type_context(r, member, context, symbol);
      }
      break;
    }
    case Kind::TySet:
      index_type_expr_context(
          r, static_cast<const TySet&>(*type).element.get(), context);
      break;
    case Kind::TyFile:
      index_type_expr_context(
          r, static_cast<const TyFile&>(*type).element.get(), context);
      break;
    case Kind::TyPointer:
      index_type_expr_context(
          r, static_cast<const TyPointer&>(*type).target.get(), context);
      break;
    case Kind::TyDistinct:
      index_type_expr_context(
          r, static_cast<const TyDistinct&>(*type).underlying.get(), context);
      break;
    case Kind::TyProcedural: {
      const auto& proc = static_cast<const TyProcedural&>(*type);
      index_param_type_contexts(r, proc.params, context);
      index_type_expr_context(r, proc.return_type.get(), context);
      break;
    }
    default:
      break;
  }
}

void index_decl_list_contexts(TypeRegistry& r, const std::string& unit,
                              const std::vector<DeclPtr>& decls,
                              TypeLookupContext* context) {
  for (const auto& d : decls) {
    if (!d) continue;
    switch (d->kind) {
      case Kind::TypeDecl: {
        const auto& td = static_cast<const TypeDecl&>(*d);
        index_type_decl_context(r, td, context,
                                r.lookup_type_symbol_in_context(td.name,
                                                                context));
        if (td.type && td.type->kind != Kind::TyEnum) {
          register_context_enum_symbols_for_owner(
              r, *context, td.type, td.name);
        }
        break;
      }
      case Kind::ProcDecl: {
        const auto& pd = static_cast<const ProcDecl&>(*d);
        const TypeLookupContext* proc_context = context;
        if (!pd.of_type.empty()) {
          if (const TypeSymbol* owner =
                  r.lookup_type_symbol_in_context(pd.of_type, context)) {
            proc_context = make_type_member_context(r, owner, context);
          }
        }
        r.proc_signature_type_contexts[&pd] = proc_context;
        index_proc_signature_context(r, pd, proc_context);
        TypeLookupContext* body_context =
            make_type_lookup_context(r, unit, proc_context);
        r.proc_body_type_contexts[&pd] = body_context;
        register_decl_list_context_symbols(r, *body_context, pd.locals);
        index_decl_list_contexts(r, unit, pd.locals, body_context);
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        if (!vd.names.empty()) {
          register_context_enum_symbols_for_owner(
              r, *context, vd.type, vd.names.front());
        }
        index_type_expr_context(r, vd.type.get(), context);
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        register_context_enum_symbols_for_owner(
            r, *context, cd.type, cd.name);
        index_type_expr_context(r, cd.type.get(), context);
        break;
      }
      default:
        break;
    }
  }
}

void report_type_value_collisions(const UnitInfo& ui) {
  std::unordered_set<std::string> type_names;
  insert_map_keys(type_names, ui.iface_types);
  insert_map_keys(type_names, ui.impl_types);

  std::unordered_set<std::string> value_names;
  insert_map_keys(value_names, ui.iface_vars);
  insert_map_keys(value_names, ui.impl_vars);
  insert_map_keys(value_names, ui.iface_consts);
  insert_map_keys(value_names, ui.impl_consts);
  insert_map_keys(value_names, ui.iface_procs);
  insert_map_keys(value_names, ui.impl_procs);
  value_names.insert(ui.iface_enum_members.begin(), ui.iface_enum_members.end());
  value_names.insert(ui.impl_enum_members.begin(), ui.impl_enum_members.end());

  for (const auto& name : type_names) {
    if (value_names.count(name)) {
      report_error({}, "duplicate identifier `" + name + "`");
    }
  }
}

}  // namespace

bool PropertyAccessorInfo::empty() const {
  return kind == PropertyAccessorKind::None;
}

std::string PropertyAccessorInfo::display_name() const {
  if (kind == PropertyAccessorKind::Method) return method_name;
  return join_path(path);
}

TypeSymbol make_type_symbol_for_type(
    std::string_view unit, std::string_view name,
    std::shared_ptr<const TypeExpr> type) {
  return make_type_symbol_for_type_with_owner(
      unit, name, std::move(type), std::vector<std::string>{});
}

TypeSymbol make_enum_type_symbol(std::string_view unit, std::string_view name,
                                 std::string_view cxx_name,
                                 const TyEnum& type) {
  const std::string low_name = lc(std::string(name));
  const std::string low_unit = lc(std::string(unit));
  return TypeSymbol(low_name, low_unit, &type,
                    make_enum_info(low_unit, low_name,
                                   std::string(cxx_name), type));
}

void register_type_symbols_for_owner(
    TypeSymbolScopeMap& out,
    std::shared_ptr<const TypeExpr> type, std::string_view owner_name,
    const TyEnum* named_top_level) {
  if (!type) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*type);
  std::unordered_set<const TyEnum*> seen;
  size_t anon_index = 0;
  const std::string owner = lc(std::string(owner_name));
  for (const TyEnum* te : enums) {
    if (!te || !seen.insert(te).second) continue;
    if (named_top_level && te == named_top_level) {
      out.insert_or_assign(
          owner, make_enum_type_symbol({}, owner, type_mangle(owner), *te));
      continue;
    }
    const bool whole_type_is_enum = te == type.get() && !named_top_level;
    std::string key = whole_type_is_enum
                          ? owner
                          : owner + "_enum" + std::to_string(anon_index);
    std::string cxx_name = whole_type_is_enum
                               ? type_mangle(owner)
                               : type_mangle(owner) + "_enum" +
                                     std::to_string(anon_index);
    out.insert_or_assign(
        key, make_enum_type_symbol({}, key, cxx_name, *te));
    ++anon_index;
  }
}

void TypeRegistry::build(const std::vector<const UnitNode*>& us) {
  // Intern descriptors for every built-in type literal (atom) in the Pascal
  // language. Pascal is nominal: a type's identity is its declaration
  // identity. Atoms are one declaration per atom, so each gets one stable
  // TypeSymbol and references bind to that descriptor during build().
  // The builtin-literal set is the primitive_type_map (single source of truth
  // for which Pascal names lower to runtime carriers); each entry is an atom.
  for (const auto& [name, _] : primitive_type_map()) {
    // The atom's AST representative is the named_pascal_type singleton. The
    // same singleton is what builtin_*_type() and named_pascal_type() hand out
    // elsewhere, so build-time descriptor resolution lands on one object.
    const ast::TyName* atom_type = named_pascal_type(name);
    AliasInfo info;
    info.defining_unit = "__builtin__";
    info.target = nullptr;
    type_symbols.emplace_back(name, "__builtin__", atom_type,
                              std::move(info));
    TypeSymbol& symbol = type_symbols.back();
    bind_symbol_descriptor(*this, symbol,
                           make_type_descriptor(*this, atom_type, &symbol));
    builtin_literal_descriptors[name] = &symbol;
  }
  // Runtime-side named types (tclass, tmethod, currency, ...) are also atoms:
  // each is a single declaration in the runtime that every translated unit
  // refers to by name. Interning them lets emitter identity checks use the
  // same pointer-equality path as primitive atoms. Names that overlap with
  // primitive_type_map (currency, ptrint, sizeint, ...) land on the same
  // singleton, so the two tables share identity for shared names.
  for (const auto& [name, _] : runtime_named_type_map()) {
    if (builtin_literal_descriptors.count(name)) continue;
    const ast::TyName* atom_type = named_pascal_type(name);
    AliasInfo info;
    info.defining_unit = "__builtin__";
    info.target = nullptr;
    type_symbols.emplace_back(name, "__builtin__", atom_type,
                              std::move(info));
    TypeSymbol& symbol = type_symbols.back();
    bind_symbol_descriptor(*this, symbol,
                           make_type_descriptor(*this, atom_type, &symbol));
    builtin_literal_descriptors[name] = &symbol;
  }
  // rt:: builtins that live in `tp2cc_rt/prelude.h` rather than a
  // Pascal unit. Model them as ProcInfos so deduce_type / is_bool
  // / auto-call decisions go through the same lookup path as real
  // Pascal procs. Fields: param_count, is_function,
  // accepts_zero_args, return_type_name (lowercased Pascal type).
  struct RtBuiltin {
    const char* name;
    size_t params;
    bool is_fn;
    bool zero_ok;
    const char* ret;
  };
  static const RtBuiltin rt_builtins[] = {
      {"assigned",   1, true,  false, "boolean"},
      {"odd",        1, true,  false, "boolean"},
      {"chr",        1, true,  false, "char"},
      {"hi",         1, true,  false, ""},
      {"lo",         1, true,  false, ""},
      {"abs",        1, true,  false, ""},
      {"sqr",        1, true,  false, ""},
      {"sqrt",       1, true,  false, "double"},
      {"sin",        1, true,  false, "double"},
      {"cos",        1, true,  false, "double"},
      {"ln",         1, true,  false, "double"},
      {"exp",        1, true,  false, "double"},
      {"arctan",     1, true,  false, "double"},
      {"round",      1, true,  false, "longint"},
      {"int",        1, true,  false, "double"},
      {"length",     1, true,  false, "longint"},
      {"eof",        1, true,  true,  "boolean"},  // eof; or eof(f)
      {"eoln",       1, true,  true,  "boolean"},
      {"ioresult",   0, true,  false, "longint"},
      {"memavail",   0, true,  false, "longint"},
      {"paramcount", 0, true,  false, "longint"},
      {"dosexitcode",0, true,  false, "longint"},
      {"date",       0, true,  false, "tdatetime"},
      {"time",       0, true,  false, "tdatetime"},
      {"writeln",    1, false, true,  ""},
      {"write",      1, false, true,  ""},
      {"readln",     1, false, true,  ""},
      {"read",       1, false, true,  ""},
      {"halt",       1, false, true,  ""},
      {"inc",        1, false, false, ""},
      {"inc",        2, false, false, ""},
      {"dec",        1, false, false, ""},
      {"dec",        2, false, false, ""},
      {"fillchar",   3, false, false, ""},
      {"fillbyte",   3, false, false, ""},
      {"fillword",   3, false, false, ""},
      {"move",       3, false, false, ""},
      {"prefetch",   1, false, false, ""},
      {"getmem",     2, false, false, ""},
      {"freemem",    1, false, false, ""},
      {"freemem",    2, false, false, ""},
      {"reallocmem", 2, true,  false, "pointer"},
      {"allocmem",   1, true,  false, "pointer"},
      {"setlength",  2, false, false, ""},
      {"setstring",  3, false, false, ""},
      {"dispose",    1, false, false, ""},
      {"strdispose", 1, false, false, ""},
      {"val",        3, false, false, ""},
      {"str",        2, false, false, ""},
      {"inttostr",   1, true,  false, "shortstring"},
      {"strtoint",   1, true,  false, "longint"},
      {"stringofchar", 2, true, false, "ansistring"},
      {"comparetext", 2, true, false, "longint"},
      {"strlen",     1, true,  false, "longint"},
      {"strpcopy",   2, true,  false, "pchar"},
      {"strrscan",   2, true,  false, "pchar"},
      {"assert",     1, false, false, ""},
      {"assert",     2, false, false, ""},
      {"assign",     2, false, false, ""},
      {"append",     1, false, false, ""},
      {"reset",      1, false, false, ""},
      {"reset",      2, false, false, ""},
      {"rewrite",    1, false, false, ""},
      {"rewrite",    2, false, false, ""},
      {"close",      1, false, false, ""},
      {"blockread",  3, false, false, ""},
      {"blockread",  4, false, false, ""},
      {"blockwrite", 3, false, false, ""},
      {"blockwrite", 4, false, false, ""},
      {"copy",       3, true,  false, "shortstring"},
      {"delete",     3, false, false, ""},
      {"insert",     3, false, false, ""},
      {"pos",        2, true,  false, "longint"},
      {"trim",       1, true,  false, "ansistring"},
      {"initialize", 1, false, false, ""},
      {"swap",       1, true,  false, "longint"},
      {"swapendian", 1, true,  false, ""},
      {"beton",      1, true,  false, ""},
      {"leton",      1, true,  false, ""},
      {"ntobe",      1, true,  false, ""},
      {"ntole",      1, true,  false, ""},
      {"rorbyte",    1, true,  false, "byte"},
      {"rorbyte",    2, true,  false, "byte"},
      {"rorword",    1, true,  false, "word"},
      {"rorword",    2, true,  false, "word"},
      {"rordword",   1, true,  false, "dword"},
      {"rordword",   2, true,  false, "dword"},
      {"rorqword",   1, true,  false, "qword"},
      {"rorqword",   2, true,  false, "qword"},
      {"rolbyte",    1, true,  false, "byte"},
      {"rolbyte",    2, true,  false, "byte"},
      {"rolword",    1, true,  false, "word"},
      {"rolword",    2, true,  false, "word"},
      {"roldword",   1, true,  false, "dword"},
      {"roldword",   2, true,  false, "dword"},
      {"rolqword",   1, true,  false, "qword"},
      {"rolqword",   2, true,  false, "qword"},
      {"sarlongint", 1, true,  false, "longint"},
      {"sarlongint", 2, true,  false, "longint"},
      {"bsfbyte",    1, true,  false, "byte"},
      {"bsrbyte",    1, true,  false, "byte"},
      {"bsfword",    1, true,  false, "cardinal"},
      {"bsrword",    1, true,  false, "cardinal"},
      {"bsfdword",   1, true,  false, "cardinal"},
      {"bsrdword",   1, true,  false, "cardinal"},
      {"bsfqword",   1, true,  false, "cardinal"},
      {"bsrqword",   1, true,  false, "cardinal"},
      {"pred",       1, true,  false, ""},
      {"succ",       1, true,  false, ""},
      {"include",    2, false, false, ""},
      {"exclude",    2, false, false, ""},
      {"paramstr",   1, true,  false, "shortstring"},
      {"findfirst",  3, true,  false, "longint"},
      {"findnext",   1, true,  false, "longint"},
      {"findclose",  1, false, false, ""},
      {"flush",      1, false, false, ""},
      {"erase",      1, false, false, ""},
      {"chdir",      1, false, false, ""},
      {"mkdir",      1, false, false, ""},
      {"rmdir",      1, false, false, ""},
      {"getdir",     2, false, false, ""},
      {"deletefile", 1, true,  false, "boolean"},
      {"directoryexists", 1, true, false, "boolean"},
      {"fsearch",    2, true,  false, "shortstring"},
      {"fsplit",     4, false, false, ""},
      {"extractfilepath", 1, true, false, "ansistring"},
      {"extractfiledir", 1, true, false, "ansistring"},
      {"extractfilename", 1, true, false, "ansistring"},
      {"extractfileext", 1, true, false, "ansistring"},
      {"changefileext", 2, true, false, "ansistring"},
      {"ansicomparefilename", 2, true, false, "longint"},
      {"expandfilename", 1, true, false, "ansistring"},
      {"setdirseparators", 1, true, false, "ansistring"},
      {"getenvironmentvariable", 1, true, false, "ansistring"},
      {"includetrailingpathdelimiter", 1, true, false, "ansistring"},
      {"getftime",   2, false, false, ""},
      {"setftime",   2, false, false, ""},
      {"filegetdate", 1, true, false, "longint"},
      {"filesetdate", 2, true, false, "longint"},
      {"fileage",    1, true,  false, "longint"},
      {"getfilehandle", 1, true, false, "longint"},
      {"settextbuf", 2, false, false, ""},
      {"settextbuf", 3, false, false, ""},
      {"strpas",     1, true,  false, "shortstring"},
      {"strcomp",    2, true,  false, "longint"},
      {"strnew",     1, true,  false, "pchar"},
      {"getdate",    4, false, false, ""},
      {"gettime",    3, false, false, ""},
      {"gettime",    4, false, false, ""},
      {"gettime",    5, false, false, ""},
      {"getlocaltime", 1, false, false, ""},
      {"decodedate", 4, false, false, ""},
      {"decodetime", 5, false, false, ""},
      {"filedatetodatetime", 1, true, false, "tdatetime"},
      {"packtime",   2, false, false, ""},
      {"unpacktime", 2, false, false, ""},
      {"epochtolocal", 7, false, false, ""},
      {"chmod",      2, true,  false, "boolean"},
      {"fpchmod",    2, true,  false, "longint"},
      {"fstat",      2, true,  false, "boolean"},
      {"fpfstat",    2, true,  false, "longint"},
      {"getfattr",   2, false, false, ""},
      {"getenv",     1, true,  false, "shortstring"},
      {"popen",      3, false, false, ""},
      {"pclose",     1, true,  false, "longint"},
      {"filepos",    1, true,  false, "longint"},
      {"filesize",   1, true,  false, "longint"},
      {"disksize",   1, true,  false, "longint"},
      {"seek",       2, false, false, ""},
      {"truncate",   1, false, false, ""},
      {"rename",     2, false, false, ""},
      {"renamefile", 2, true,  false, "boolean"},
      {"exec",       2, false, false, ""},
      {"executeprocess", 2, true, false, "longint"},
      {"executeprocess", 3, true, false, "longint"},
      {"trunc",      1, true,  false, "longint"},
      {"frac",       1, true,  false, "double"},
      {"octstr",     2, true,  false, "shortstring"},
      {"signal",     2, true,  false, "signalhandler"},
      {"fpsignal",   2, true,  false, "signalhandler"},
      {"swapvectors",0, false, false, ""},
      {"runerror",   1, false, true,  ""},
      {"hexstr",     1, true,  false, "shortstring"},
      {"hexstr",     2, true,  false, "shortstring"},
      {"freeandnil", 1, false, false, ""},
      {"getexceptionmask", 0, true, false, "tfpuexceptionmask"},
      {"setexceptionmask", 1, true, false, "tfpuexceptionmask"},
      {"fileexists", 1, true,  false, "boolean"},
      {"indexbyte",  3, true,  false, "longint"},
      {"indexword",  3, true,  false, "longint"},
      {"comparebyte", 3, true, false, "longint"},
      {"comparechar", 3, true, false, "longint"},
      {"compareword", 3, true, false, "longint"},
      {"filldword",  3, false, false, ""},
      {"get8087cw",  0, true,  false, "word"},
      {"set8087cw",  1, false, false, ""},
  };
  // Synthetic unit "rt::" holds the builtins so lookups that walk the uses
  // chain find the implicit System/runtime names in one place.
  std::unordered_map<std::string, std::vector<ProcInfo>> rt_iface_procs;
  for (const auto& b : rt_builtins) {
    rt_iface_procs[b.name].push_back(
        ProcInfo{.defining_unit = "__rt__",
                 .decl = nullptr,
                 .param_count = b.params,
                 .is_function = b.is_fn,
                 .accepts_zero_args = b.zero_ok || b.params == 0,
                 .return_type_name = b.ret});
  }
  units["__rt__"] = unit_info_for("__rt__", {}, {}, std::move(rt_iface_procs));
  UnitInfo& rt_exports = units["__rt__"];

  register_runtime_proc(
      rt_exports, ProcKind::Function, "upcase",
      {runtime_const_param("c", runtime_type_name("char"))},
      runtime_type_name("char"));
  register_runtime_proc(
      rt_exports, ProcKind::Function, "upcase",
      {runtime_const_param("s", runtime_type_name("shortstring"))},
      runtime_type_name("shortstring"));
  register_runtime_proc(
      rt_exports, ProcKind::Function, "upcase",
      {runtime_const_param("s", runtime_type_name("ansistring"))},
      runtime_type_name("ansistring"));
  register_runtime_proc(
      rt_exports, ProcKind::Function, "fexpand",
      {runtime_const_param("path", runtime_type_name("pathstr"))},
      runtime_type_name("pathstr"));
  static constexpr std::string_view endian_ordinals[] = {
      "smallint", "word", "longint", "dword", "int64", "qword"};
  for (std::string_view helper :
       {"beton", "leton", "ntobe", "ntole", "swapendian"}) {
    register_runtime_same_type_unary_overloads(rt_exports, helper,
                                               endian_ordinals);
  }

  // Runtime globals/constants that old compiler trees refer to directly.
  register_runtime_var(rt_exports, "doserror", runtime_type_name("longint"));
  register_runtime_var(rt_exports, "filemode", runtime_type_name("longint"));
  register_runtime_var(rt_exports, "stderr", runtime_type_name("text"));
  register_runtime_var(rt_exports, "output", runtime_type_name("text"));
  register_runtime_var(rt_exports, "input", runtime_type_name("text"));
  register_runtime_var(rt_exports, "exitproc",
                       runtime_procedural_type(false, {}));
  register_runtime_var(rt_exports, "erroraddr", runtime_type_name("pointer"));
  register_runtime_var(rt_exports, "exitcode", runtime_type_name("longint"));
  register_runtime_var(rt_exports, "heapsize", runtime_type_name("longint"));

  register_runtime_const(rt_exports, "sigint", runtime_type_name("longint"),
                         runtime_int_literal(2));
  register_runtime_const(rt_exports, "sigfpe", runtime_type_name("longint"),
                         runtime_int_literal(8));
  register_runtime_const(rt_exports, "sigsegv", runtime_type_name("longint"),
                         runtime_int_literal(11));
  register_runtime_const(rt_exports, "pi", runtime_type_name("double"),
                         nullptr);
  register_runtime_const(rt_exports, "maxint", runtime_type_name("longint"),
                         runtime_int_literal(2147483647));
  register_runtime_const(rt_exports, "readonly", runtime_type_name("longint"),
                         runtime_int_literal(0x01));
  register_runtime_const(rt_exports, "hidden", runtime_type_name("longint"),
                         runtime_int_literal(0x02));
  register_runtime_const(rt_exports, "sysfile", runtime_type_name("longint"),
                         runtime_int_literal(0x04));
  register_runtime_const(rt_exports, "volumeid", runtime_type_name("longint"),
                         runtime_int_literal(0x08));
  register_runtime_const(rt_exports, "directory",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x10));
  register_runtime_const(rt_exports, "archive", runtime_type_name("longint"),
                         runtime_int_literal(0x20));
  register_runtime_const(rt_exports, "fareadonly",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x01));
  register_runtime_const(rt_exports, "fahidden", runtime_type_name("longint"),
                         runtime_int_literal(0x02));
  register_runtime_const(rt_exports, "fadirectory",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x10));
  register_runtime_const(rt_exports, "faarchive",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x20));
  register_runtime_const(rt_exports, "anyfile", runtime_type_name("longint"),
                         runtime_int_literal(0x3F));
  register_runtime_const(rt_exports, "faanyfile",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x3F));
  register_runtime_const(rt_exports, "fmsharedenynone",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x40));
  register_runtime_const(rt_exports, "vtansistring",
                         runtime_type_name("longint"), runtime_int_literal(11));
  register_runtime_const(rt_exports, "varstrarg",
                         runtime_type_name("longint"),
                         runtime_int_literal(0x48));
  register_runtime_const(rt_exports, "directoryseparator",
                         runtime_type_name("char"), nullptr);
  register_runtime_const(rt_exports, "driveseparator",
                         runtime_type_name("char"), nullptr);
  register_runtime_const(rt_exports, "extensionseparator",
                         runtime_type_name("char"), nullptr);
  register_runtime_const(rt_exports, "pathseparator", runtime_type_name("char"),
                         nullptr);
  register_runtime_const(rt_exports, "maxlongint",
                         runtime_type_name("longint"),
                         runtime_int_literal(2147483647));

  // Runtime type names that Pascal code can mention directly are registered as
  // aliases so casts and member lookups go through normal type analysis.
  register_runtime_alias(*this, rt_exports, "tprocedure",
                         runtime_procedural_type(false, {}));
  register_runtime_alias(*this, rt_exports, "signalhandler",
                         runtime_pointer_type());
  register_runtime_alias(
      *this, rt_exports, "tfpuexception",
      runtime_enum_type({"exinvalidop", "exdenormalized", "exzerodivide",
                         "exoverflow", "exunderflow", "exprecision"}));
  register_runtime_alias(*this, rt_exports, "searchrec", runtime_record_type({
      runtime_record_field("time", runtime_type_name("longint")),
      runtime_record_field("size", runtime_type_name("longint")),
      runtime_record_field("attr", runtime_type_name("byte")),
      runtime_record_field("name", runtime_type_name("shortstring")),
  }));
  register_runtime_alias(*this, rt_exports, "tsearchrec",
                         runtime_type_name("searchrec"));
  register_runtime_alias(*this, rt_exports, "stat", runtime_record_type({
      runtime_record_field("mtime", runtime_type_name("longint")),
      runtime_record_field("st_mtime", runtime_type_name("longint")),
      runtime_record_field("mode", runtime_type_name("longint")),
      runtime_record_field("st_mode", runtime_type_name("longint")),
      runtime_record_field("size", runtime_type_name("longint")),
      runtime_record_field("st_size", runtime_type_name("longint")),
  }));
  register_runtime_alias(*this, rt_exports, "datetime", runtime_record_type({
      runtime_record_field("year", runtime_type_name("word")),
      runtime_record_field("month", runtime_type_name("word")),
      runtime_record_field("day", runtime_type_name("word")),
      runtime_record_field("hour", runtime_type_name("word")),
      runtime_record_field("min", runtime_type_name("word")),
      runtime_record_field("sec", runtime_type_name("word")),
  }));
  register_runtime_alias(*this, rt_exports, "tdatetime",
                         runtime_type_name("double"));
  register_runtime_alias(*this, rt_exports, "tsystemtime", runtime_record_type({
      runtime_record_field("year", runtime_type_name("word")),
      runtime_record_field("month", runtime_type_name("word")),
      runtime_record_field("dayofweek", runtime_type_name("word")),
      runtime_record_field("day", runtime_type_name("word")),
      runtime_record_field("hour", runtime_type_name("word")),
      runtime_record_field("minute", runtime_type_name("word")),
      runtime_record_field("second", runtime_type_name("word")),
      runtime_record_field("millisecond", runtime_type_name("word")),
  }));
  register_runtime_alias(*this, rt_exports, "dirstr",
                         runtime_type_name("shortstring"));
  register_runtime_alias(*this, rt_exports, "namestr",
                         runtime_type_name("shortstring"));
  register_runtime_alias(*this, rt_exports, "extstr",
                         runtime_type_name("shortstring"));
  register_runtime_alias(*this, rt_exports, "pathstr",
                         runtime_type_name("shortstring"));
  register_runtime_alias(*this, rt_exports, "comstr",
                         runtime_type_name("shortstring"));
  register_runtime_alias(*this, rt_exports, "texecuteflag",
                         runtime_enum_type({"execinheritshandles"}));
  register_runtime_alias(
      *this, rt_exports, "texecuteflags",
      runtime_set_type(runtime_type_name("texecuteflag")));
  register_runtime_alias(
      *this, rt_exports, "tfpuexceptionmask",
      runtime_set_type(runtime_type_name("tfpuexception")));
  register_runtime_alias(*this, rt_exports, "tsyscharset",
                         runtime_set_type(runtime_type_name("char")));
  register_runtime_alias(*this, rt_exports, "hresult",
                         runtime_type_name("longint"));
  register_runtime_alias(*this, rt_exports, "pointer",
                         runtime_pointer_type());
  register_runtime_alias(*this, rt_exports, "ansichar",
                         runtime_type_name("char"));
  register_runtime_alias(
      *this, rt_exports, "pchar",
      runtime_pointer_type(runtime_type_name("char")));
  register_runtime_alias(
      *this, rt_exports, "pansichar",
      runtime_pointer_type(runtime_type_name("ansichar")));
  register_runtime_alias(
      *this, rt_exports, "pansistring",
      runtime_pointer_type(runtime_type_name("ansistring")));
  register_runtime_alias(
      *this, rt_exports, "pcardinal",
      runtime_pointer_type(runtime_type_name("cardinal")));
  register_runtime_alias(
      *this, rt_exports, "pcurrency",
      runtime_pointer_type(runtime_type_name("currency")));
  register_runtime_alias(*this, rt_exports, "pdword",
                         runtime_pointer_type(runtime_type_name("dword")));
  register_runtime_alias(*this, rt_exports, "pint64",
                         runtime_pointer_type(runtime_type_name("int64")));
  register_runtime_alias(
      *this, rt_exports, "plongword",
      runtime_pointer_type(runtime_type_name("longword")));
  register_runtime_alias(*this, rt_exports, "ppointer",
                         runtime_pointer_type(runtime_type_name("pointer")));
  register_runtime_alias(*this, rt_exports, "pqword",
                         runtime_pointer_type(runtime_type_name("qword")));
  register_runtime_alias(
      *this, rt_exports, "pshortstring",
      runtime_pointer_type(runtime_type_name("shortstring")));
  const std::array<const char*, 6> string_compare_ops{
      "=", "<>", "<", ">", "<=", ">="};
  for (const char* op : string_compare_ops) {
    register_runtime_string_compare_operator(
        rt_exports, op, runtime_type_name("shortstring"),
        runtime_type_name("shortstring"), runtime_type_name("boolean"));
    register_runtime_string_compare_operator(
        rt_exports, op, runtime_type_name("shortstring"),
        runtime_type_name("ansistring"), runtime_type_name("boolean"));
    register_runtime_string_compare_operator(
        rt_exports, op, runtime_type_name("ansistring"),
        runtime_type_name("shortstring"), runtime_type_name("boolean"));
    register_runtime_string_compare_operator(
        rt_exports, op, runtime_type_name("ansistring"),
        runtime_type_name("ansistring"), runtime_type_name("boolean"));
  }
  register_runtime_var(rt_exports, "allowdirectoryseparators",
                       runtime_set_type(runtime_type_name("char")));

  // Runtime reference classes participate in Pascal parent-chain lookup; a
  // translated `class(Exception)` must inherit the synthesized constructor
  // signature for `Exception.Create`.
  // TObject's class functions are registered as ordinary MethodSigs so the
  // resolver and result-type deduction use the same path as translated methods.
  const Param inh_aclass =
      runtime_const_param("aclass", runtime_type_name("tclass"));
  register_runtime_class(
      *this, "tobject", "",
      {runtime_method_sig("create", ProcKind::Constructor, {}),
       runtime_method_sig("destroy", ProcKind::Destructor, {}),
       runtime_method_sig("free", ProcKind::Procedure, {}),
       runtime_method_sig("classname", ProcKind::Function, {},
                          runtime_type_name("shortstring"),
                          /*class_method=*/true),
       runtime_method_sig("classtype", ProcKind::Function, {},
                          runtime_type_name("tclass"),
                          /*class_method=*/true),
       runtime_method_sig("instancesize", ProcKind::Function, {},
                          runtime_type_name("longint"),
                          /*class_method=*/true),
       runtime_method_sig("inheritsfrom", ProcKind::Function, {inh_aclass},
                          runtime_type_name("boolean"),
                          /*class_method=*/true)});

  const Param exc_msg =
      runtime_const_param("msg", runtime_type_name("shortstring"));
  register_runtime_class(
      *this, "exception", "tobject",
      {runtime_method_sig("create", ProcKind::Constructor, {exc_msg})});
  register_runtime_class(*this, "eexternal", "exception", {});
  register_runtime_class(*this, "einterror", "eexternal", {});
  register_runtime_class(*this, "einouterror", "exception", {});
  register_runtime_class(*this, "eheapmemoryerror", "exception", {});
  register_runtime_class(*this, "eheapexception", "eheapmemoryerror", {});
  register_runtime_class(*this, "eoutofmemory", "eheapmemoryerror", {});
  register_runtime_class(*this, "eintoverflow", "einterror", {});
  register_runtime_class(*this, "erangeerror", "einterror", {});
  register_runtime_class(*this, "edivbyzero", "einterror", {});
  register_runtime_class(*this, "eoserror", "exception", {});
  register_runtime_class_field(*this, "exception", "message",
                               runtime_type_name("ansistring"));
  register_runtime_class_field(*this, "einouterror", "errorcode",
                               runtime_type_name("longint"));
  register_runtime_class_field(*this, "eoserror", "errorcode",
                               runtime_type_name("longint"));

  for (const auto* u : us) {
    if (!u) continue;
    std::vector<std::string> interface_uses;
    std::vector<std::string> implementation_uses;
    for (const auto& nm : u->interface_uses) {
      interface_uses.push_back(lc(nm));
    }
    for (const auto& nm : u->impl_uses) {
      implementation_uses.push_back(lc(nm));
    }
    units[lc(u->name)] =
        unit_info_for(lc(u->name), std::move(interface_uses),
                      std::move(implementation_uses));
  }

  for (const auto* u : us) {
    if (!u) continue;
    for (const auto& nm : u->interface_uses) {
      register_runtime_backed_unit(*this, nm);
    }
    for (const auto& nm : u->impl_uses) {
      register_runtime_backed_unit(*this, nm);
    }
  }

  for (const auto* u : us) {
    if (!u) continue;
    register_decl_list(*this, lc(u->name), u->interface_decls,
                       /*is_interface=*/true);
    register_decl_list(*this, lc(u->name), u->impl_decls,
                       /*is_interface=*/false);
  }

  for (const auto* u : us) {
    if (!u) continue;
    const std::string unit_name = lc(u->name);
    TypeLookupContext* interface_context =
        make_unit_type_context(*this, unit_name, /*is_interface=*/true);
    unit_interface_type_contexts[unit_name] = interface_context;
    index_decl_list_contexts(*this, unit_name, u->interface_decls,
                             interface_context);
    TypeLookupContext* implementation_context =
        make_unit_type_context(*this, unit_name, /*is_interface=*/false);
    unit_implementation_type_contexts[unit_name] = implementation_context;
    index_decl_list_contexts(*this, unit_name, u->impl_decls,
                             implementation_context);
    report_type_value_collisions(units[lc(u->name)]);
  }

  ensure_fresh_descriptors_for_registered_symbols(*this);
  for (const auto* u : us) {
    if (!u) continue;
    const std::string unit = lc(u->name);
    resolve_decl_list_type_descriptors(
        *this, u->interface_decls,
        lookup_unit_context(unit, /*implementation=*/false));
    resolve_decl_list_type_descriptors(
        *this, u->impl_decls,
        lookup_unit_context(unit, /*implementation=*/true));
  }

  for (const auto* u : us) {
    if (!u) continue;
    const std::string unit = lc(u->name);
    resolve_property_accessors_from_decls(*this, unit, u->interface_decls);
    resolve_property_accessors_from_decls(*this, unit, u->impl_decls);
  }
}

const TypeSymbol* TypeRegistry::lookup_type_symbol_exact(
    std::string_view unit, std::string_view name) const {
  const std::string target_unit = lc(std::string(unit));
  const std::string target_name = lc(std::string(name));
  auto uit = units.find(target_unit);
  if (uit == units.end()) return nullptr;
  const size_t dot = target_name.find('.');
  if (dot == std::string::npos) return uit->second.find_type(target_name);
  const TypeSymbol* root = uit->second.find_type(target_name.substr(0, dot));
  return lookup_nested_type_symbol_path(root, target_name, dot + 1);
}

bool is_import_frame(const TypeLookupContext& frame) {
  return frame.kind == ScopeFrameKind::ImportedUnitInterface;
}

const TypeSymbol* lookup_type_symbol_in_frame(const TypeLookupContext& frame,
                                              const std::string& low,
                                              bool include_imports) {
  if (!include_imports && is_import_frame(frame)) return nullptr;
  auto local = frame.type_symbols.find(low);
  if (local != frame.type_symbols.end()) return local->second;
  if (!frame.unit_info) return nullptr;
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_types.find(low);
      return it == frame.unit_info->impl_types.end() ? nullptr : it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_types.find(low);
      return it == frame.unit_info->iface_types.end() ? nullptr : it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

const TypeSymbol* lookup_type_path_in_frame(const TypeLookupContext& frame,
                                            const std::string& path,
                                            bool include_imports) {
  const size_t dot = path.find('.');
  if (dot == std::string::npos) {
    return lookup_type_symbol_in_frame(frame, path, include_imports);
  }
  const TypeSymbol* root =
      lookup_type_symbol_in_frame(frame, path.substr(0, dot),
                                  include_imports);
  return lookup_nested_type_symbol_path(root, path, dot + 1);
}

const TypeSymbol* lookup_unit_qualified_type_in_context(
    std::string_view unit, std::string_view path,
    const TypeLookupContext* context, bool include_imports) {
  std::string target_unit = lc(std::string(unit));
  if (target_unit == "system") target_unit = "__rt__";
  const std::string target_path = lc(std::string(path));
  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    if (frame->unit != target_unit) continue;
    if (!include_imports && is_import_frame(*frame)) continue;
    if (const TypeSymbol* symbol =
            lookup_type_path_in_frame(*frame, target_path, include_imports)) {
      return symbol;
    }
  }
  return nullptr;
}

TypeSymbol* TypeRegistry::lookup_type_symbol_exact_mut(
    std::string_view unit, std::string_view name) {
  const std::string target_unit = lc(std::string(unit));
  const std::string target_name = lc(std::string(name));
  auto uit = units.find(target_unit);
  if (uit == units.end()) return nullptr;
  const size_t dot = target_name.find('.');
  if (dot == std::string::npos) return uit->second.find_type_mut(target_name);
  const std::string root_name = target_name.substr(0, dot);
  if (auto iit = uit->second.iface_types.find(root_name);
      iit != uit->second.iface_types.end()) {
    return lookup_nested_type_symbol_path_mut(iit->second, target_name,
                                              dot + 1);
  }
  auto mit = uit->second.impl_types.find(root_name);
  return mit == uit->second.impl_types.end()
             ? nullptr
             : lookup_nested_type_symbol_path_mut(mit->second, target_name,
                                                   dot + 1);
}

const TypeSymbol* TypeRegistry::lookup_type_symbol(
    std::string_view name, std::string_view current_unit) const {
  const std::string low = lc(std::string(name));
  if (auto dot = low.find('.'); dot != std::string::npos) {
    // Non-dotted lookups are on the hot path for expression typing. Avoid
    // constructing a vector of path segments for every query; only dotted
    // Pascal type names need segment walking.
    const std::string root_name = low.substr(0, dot);
    if (const TypeSymbol* root = lookup_type_symbol(root_name, current_unit)) {
      if (const TypeSymbol* nested =
              lookup_nested_type_symbol_path(root, low, dot + 1)) {
        return nested;
      }
    }
    return lookup_type_symbol_exact(root_name, low.substr(dot + 1));
  }

  const std::string cur_unit = lc(std::string(current_unit));
  if (!cur_unit.empty()) {
    if (const TypeLookupContext* context =
            lookup_unit_context(cur_unit, /*implementation=*/true)) {
      if (const TypeSymbol* symbol =
              lookup_type_symbol_in_context(low, context)) {
        return symbol;
      }
    }
    if (const TypeLookupContext* context =
            lookup_unit_context(cur_unit, /*implementation=*/false)) {
      if (const TypeSymbol* symbol =
              lookup_type_symbol_in_context(low, context)) {
        return symbol;
      }
    }
  }

  const TypeSymbol* only_source = nullptr;
  const TypeSymbol* only_runtime = nullptr;
  for (const auto& [unit_name, unit] : units) {
    const TypeSymbol* symbol = unit.find_type(low);
    if (!symbol) continue;
    if (unit_name == "__rt__") {
      only_runtime = symbol;
      continue;
    }
    if (only_source) return nullptr;
    only_source = symbol;
  }
  if (only_source) return only_source;
  return only_runtime;
}

const ClassInfo* TypeRegistry::lookup_class(std::string_view name,
                                            std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  if (!symbol) return nullptr;
  symbol = descriptor_payload_symbol(symbol);
  return symbol ? symbol->class_info() : nullptr;
}

const ClassInfo* TypeRegistry::lookup_parent_class(
    const ClassInfo& class_info) const {
  if (!class_info.parent.empty()) {
    if (const ClassInfo* parent =
            lookup_class(class_info.parent, class_info.defining_unit)) {
      return parent;
    }
    auto runtime_parent = rt_classes.find(class_info.parent);
    if (runtime_parent != rt_classes.end()) return &runtime_parent->second;
    return nullptr;
  }
  if (!class_info.is_reference_type) return nullptr;
  auto runtime_tobject = rt_classes.find("tobject");
  if (runtime_tobject == rt_classes.end() ||
      (class_info.defining_unit == "__rt__" && class_info.name == "tobject")) {
    return nullptr;
  }
  return &runtime_tobject->second;
}

bool TypeRegistry::class_implements_interface(
    const ClassInfo& class_info, const InterfaceInfo& interface_info) const {
  const ClassInfo* cls = &class_info;
  SeenClassChain seen;
  while (cls && seen.mark(cls)) {
    for (const auto& implemented_name : cls->interfaces) {
      const InterfaceInfo* implemented =
          lookup_interface(implemented_name, cls->defining_unit);
      if (implemented == &interface_info ||
          (implemented && implemented->name == interface_info.name &&
           implemented->defining_unit == interface_info.defining_unit)) {
        return true;
      }
    }
    cls = lookup_parent_class(*cls);
  }
  return false;
}

const InterfaceInfo* TypeRegistry::lookup_interface(
    std::string_view name, std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  symbol = descriptor_payload_symbol(symbol);
  return symbol ? symbol->interface_info() : nullptr;
}

const RecordInfo* TypeRegistry::lookup_record(
    std::string_view name, std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  symbol = descriptor_payload_symbol(symbol);
  return symbol ? symbol->record_info() : nullptr;
}

const EnumInfoReg* TypeRegistry::enum_info_for_type(
    const TyEnum* type) const {
  if (!type) return nullptr;
  if (auto it = enum_type_info.find(type); it != enum_type_info.end()) {
    return it->second;
  }
  return nullptr;
}

std::string_view TypeRegistry::declaration_unit_for_type(
    const TypeExpr* type) const {
  if (!type || !type->loc.file) return {};
  auto it = source_file_units.find(type->loc.file.get());
  return it == source_file_units.end() ? std::string_view{} : it->second;
}

const TypeLookupContext* TypeRegistry::lookup_context_for_type(
    const TypeExpr* type) const {
  if (!type) return nullptr;
  auto it = type_lookup_contexts.find(type);
  return it == type_lookup_contexts.end() ? nullptr : it->second;
}

const TypeLookupContext* TypeRegistry::lookup_unit_context(
    std::string_view unit, bool implementation) const {
  const std::string low = lc(std::string(unit));
  const auto& map = implementation ? unit_implementation_type_contexts
                                   : unit_interface_type_contexts;
  auto it = map.find(low);
  return it == map.end() ? nullptr : it->second;
}

const TypeLookupContext* TypeRegistry::lookup_proc_signature_context(
    const ProcDecl* proc) const {
  if (!proc) return nullptr;
  auto it = proc_signature_type_contexts.find(proc);
  return it == proc_signature_type_contexts.end() ? nullptr : it->second;
}

const TypeLookupContext* TypeRegistry::lookup_proc_body_context(
    const ProcDecl* proc) const {
  if (!proc) return nullptr;
  auto it = proc_body_type_contexts.find(proc);
  return it == proc_body_type_contexts.end() ? nullptr : it->second;
}

const TypeDescriptor* TypeRegistry::descriptor_for_type(
    const TypeExpr* type) const {
  if (!type) return nullptr;
  if (auto it = type_descriptors.find(type); it != type_descriptors.end()) {
    return it->second;
  }
  return nullptr;
}

const TypeSymbol* TypeRegistry::referenced_symbol_for_type(
    const TypeExpr* type) const {
  if (!type) return nullptr;
  if (auto it = type_reference_symbols.find(type);
      it != type_reference_symbols.end()) {
    return it->second;
  }
  return nullptr;
}

const TypeSymbol* TypeRegistry::canonical_symbol_for_type(
    const TypeExpr* type) const {
  const TypeDescriptor* descriptor = descriptor_for_type(type);
  return descriptor ? descriptor->symbol : nullptr;
}

const TypeSymbol* TypeRegistry::resolved_symbol_for_type(
    const TypeExpr* type) const {
  if (const TypeSymbol* symbol = referenced_symbol_for_type(type)) {
    return symbol;
  }
  return canonical_symbol_for_type(type);
}

const TypeSymbol* TypeRegistry::metaclass_target_for_type(
    const TypeExpr* type) const {
  const TypeDescriptor* descriptor = descriptor_for_type(type);
  return descriptor ? descriptor->metaclass_target : nullptr;
}

const TypeSymbol* TypeRegistry::lookup_type_symbol_in_context(
    std::string_view name, const TypeLookupContext* context) const {
  if (!context) return lookup_type_symbol(name, {});
  const std::string low = lc(std::string(name));
  if (auto dot = low.find('.'); dot != std::string::npos) {
    const std::string root_name = low.substr(0, dot);
    if (const TypeSymbol* root =
            lookup_type_symbol_in_context(root_name, context)) {
      if (const TypeSymbol* nested =
              lookup_nested_type_symbol_path(root, low, dot + 1)) {
        return nested;
      }
    }
    return lookup_unit_qualified_type_in_context(
        root_name, low.substr(dot + 1), context, /*include_imports=*/true);
  }

  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    if (const TypeSymbol* symbol =
            lookup_type_symbol_in_frame(*frame, low,
                                        /*include_imports=*/true)) {
      return symbol;
    }
  }
  return nullptr;
}

const TypeSymbol* TypeRegistry::lookup_type_symbol_in_scope_chain(
    std::string_view name, const TypeLookupContext* context) const {
  if (!context) return nullptr;
  const std::string low = lc(std::string(name));
  if (auto dot = low.find('.'); dot != std::string::npos) {
    const std::string root_name = low.substr(0, dot);
    if (const TypeSymbol* root =
            lookup_type_symbol_in_scope_chain(root_name, context)) {
      if (const TypeSymbol* nested =
              lookup_nested_type_symbol_path(root, low, dot + 1)) {
        return nested;
      }
    }
    return lookup_unit_qualified_type_in_context(
        root_name, low.substr(dot + 1), context, /*include_imports=*/false);
  }

  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    if (const TypeSymbol* symbol =
            lookup_type_symbol_in_frame(*frame, low,
                                        /*include_imports=*/false)) {
      return symbol;
    }
  }
  return nullptr;
}

const TyEnum* TypeRegistry::lookup_enum_member_in_unit(
    std::string_view unit, std::string_view member) const {
  auto uit = enum_members_by_unit.find(lc(std::string(unit)));
  if (uit == enum_members_by_unit.end()) return nullptr;
  auto mit = uit->second.find(lc(std::string(member)));
  return mit == uit->second.end() ? nullptr : mit->second;
}

const TypeSymbol* TypeRegistry::builtin_literal(std::string_view name) const {
  auto it = builtin_literal_descriptors.find(name);
  return it == builtin_literal_descriptors.end() ? nullptr : it->second;
}

std::string TypeRegistry::field_cxx_name(std::string_view name) const {
  return mangle(name);
}

const FieldInfo* TypeRegistry::lookup_class_field(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto fit = ci->fields.find(key);
    if (fit != ci->fields.end()) return &fit->second;
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

bool TypeRegistry::class_has_enum_member(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    if (ci->enum_members.count(key)) return true;
    ci = lookup_parent_class(*ci);
  }
  return false;
}

const std::vector<MethodSig>* TypeRegistry::lookup_class_methods(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  // FPC's overload resolution is two-phase:
  //   1. Name lookup: Walk the class chain from derived to parent. At each
  //      class, if the name is found, add those methods to the candidate set.
  //      If ALL overloads at this class have `is_overload=true`, continue
  //      walking to merge parent overloads. Otherwise stop (name hiding).
  //   2. Argument matching: Run overload ranking on the collected set.
  //
  // This correctly handles:
  //   - No overload: stops at the first class with the name (name hiding).
  //   - Unbroken overload chain: merges parent + child overloads.
  //   - Broken chain: parent without `overload` hides grandparent; child with
  //     `overload` only sees the parent's overloads.
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  if (const InterfaceInfo* interface =
          lookup_interface(class_name_in, current_unit)) {
    auto mit = interface->methods.find(key);
    return mit == interface->methods.end() ? nullptr : &mit->second;
  }
  if (!ci) return nullptr;
  // Cache key: current_unit + NUL + class_name + NUL + member_lowered.
  std::string cache_key;
  cache_key.reserve(current_unit.size() + 1 + class_name_in.size() + 1 +
                    key.size());
  cache_key.append(current_unit);
  cache_key.push_back('\0');
  cache_key.append(class_name_in);
  cache_key.push_back('\0');
  cache_key.append(key);
  auto cache_it = merged_method_cache.find(cache_key);
  if (cache_it != merged_method_cache.end()) {
    return cache_it->second.empty() ? nullptr : &cache_it->second;
  }
  std::vector<MethodSig> merged;
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto mit = ci->methods.find(key);
    if (mit != ci->methods.end()) {
      merged.insert(merged.end(), mit->second.begin(), mit->second.end());
      // Check if ALL overloads at this class have is_overload=true. If so,
      // continue walking to merge parent overloads. Otherwise stop (name
      // hiding); parent overloads are hidden.
      bool all_overload = true;
      for (const auto& sig : mit->second) {
        if (!sig.is_overload) {
          all_overload = false;
          break;
        }
      }
      if (!all_overload) break;
    }
    ci = lookup_parent_class(*ci);
  }
  auto [it, _] =
      merged_method_cache.emplace(std::move(cache_key), std::move(merged));
  return it->second.empty() ? nullptr : &it->second;
}

const PropertyInfo* TypeRegistry::lookup_class_property(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto pit = ci->properties.find(key);
    if (pit != ci->properties.end()) return &pit->second;
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_default_property(
    const std::string& class_name_in, std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    if (!ci->default_property_name.empty()) {
      auto pit = ci->properties.find(ci->default_property_name);
      if (pit != ci->properties.end()) return &pit->second;
    }
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

const FieldInfo* TypeRegistry::lookup_record_field(
    const std::string& record_name, const std::string& member,
    std::string_view current_unit) const {
  const RecordInfo* record = lookup_record(record_name, current_unit);
  if (!record) return nullptr;
  auto fit = record->fields.find(lc(member));
  return fit == record->fields.end() ? nullptr : &fit->second;
}

}  // namespace tp2cc
