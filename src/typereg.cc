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

TypeSymbol make_enum_type_symbol(TypeRegistry& registry,
                                 std::string_view unit, std::string_view name,
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

void bind_descriptor_payload(TypeDescriptor& descriptor) {
  if (ClassInfo* info = descriptor.mutable_class_info()) {
    info->descriptor = &descriptor;
  } else if (RecordInfo* info = descriptor.mutable_record_info()) {
    info->descriptor = &descriptor;
  } else if (InterfaceInfo* info = descriptor.mutable_interface_info()) {
    info->descriptor = &descriptor;
  } else if (EnumInfoReg* info = descriptor.mutable_enum_info()) {
    info->descriptor = &descriptor;
  }
}

const TypeDescriptor* builtin_descriptor(TypeRegistry& registry,
                                         std::string_view name) {
  const TypeSymbol* symbol = registry.builtin_literal(name);
  return symbol ? symbol->descriptor : nullptr;
}

void initialize_structural_descriptor_edges(TypeRegistry& registry,
                                            TypeDescriptor& descriptor) {
  if (!descriptor.type) return;
  switch (descriptor.type->kind) {
    case Kind::TyEnum:
      descriptor.ordinal_result =
          builtin_descriptor(registry, "longint");
      descriptor.set_literal_element_result = &descriptor;
      break;
    case Kind::TySubrange:
      descriptor.ordinal_result = &descriptor;
      descriptor.set_literal_element_result = &descriptor;
      break;
    case Kind::TyDistinct: {
      const auto& distinct =
          static_cast<const TyDistinct&>(*descriptor.type);
      const TypeDescriptor* underlying =
          distinct.underlying ? distinct.underlying->descriptor : nullptr;
      if (underlying && underlying->ordinal_result) {
        descriptor.ordinal_result = &descriptor;
        descriptor.set_literal_element_result = &descriptor;
      }
      break;
    }
    case Kind::TyPointer:
      descriptor.pointer_difference_result =
          builtin_descriptor(registry, "ptrint");
      break;
    case Kind::TyString:
      descriptor.element_result = builtin_descriptor(registry, "char");
      break;
    case Kind::TyArray: {
      const auto& array = static_cast<const TyArray&>(*descriptor.type);
      if (array.array_kind != ArrayKind::Fixed) {
        descriptor.low_high_result =
            builtin_descriptor(registry, "longint");
      }
      break;
    }
    default:
      break;
  }
}

const TypeDescriptor* make_type_descriptor(TypeRegistry& r,
                                           const TypeExpr* type,
                                           const TypeSymbol* symbol,
                                           TypeDescriptorPayload payload = {},
                                           const PrimitiveInfo* primitive =
                                               nullptr) {
  r.type_descriptor_storage.push_back(
      TypeDescriptor{.id = r.type_descriptor_storage.size() + 1,
                     .type = type,
                     .symbol = symbol,
                     .metaclass_target = nullptr,
                     .primitive = primitive,
                     .payload = std::move(payload)});
  TypeDescriptor& descriptor = r.type_descriptor_storage.back();
  bind_descriptor_payload(descriptor);
  initialize_structural_descriptor_edges(r, descriptor);
  return &descriptor;
}

const TypeDescriptor* make_metaclass_descriptor(TypeRegistry& r,
                                                const TypeSymbol* target) {
  if (target && target->descriptor && target->descriptor->symbol) {
    target = target->descriptor->symbol;
  }
  if (!target) return nullptr;
  if (auto it = r.metaclass_descriptors.find(target);
      it != r.metaclass_descriptors.end()) {
    return it->second;
  }
  r.type_descriptor_storage.push_back(
      TypeDescriptor{.id = r.type_descriptor_storage.size() + 1,
                     .type = nullptr,
                     .symbol = nullptr,
                     .metaclass_target = target,
                     .payload = {}});
  const TypeDescriptor* descriptor = &r.type_descriptor_storage.back();
  r.metaclass_descriptors.insert_or_assign(target, descriptor);
  return descriptor;
}

void bind_type_expr_descriptor(const TypeExpr* type,
                               const TypeDescriptor* descriptor) {
  if (!type || !descriptor) return;
  type->descriptor = descriptor;
}

void bind_type_expr_symbol(const TypeExpr* type, const TypeSymbol* symbol) {
  if (!type || !symbol) return;
  type->referenced_symbol = symbol;
}

void bind_symbol_descriptor(TypeSymbol& symbol,
                            const TypeDescriptor* descriptor) {
  if (!descriptor) return;
  symbol.descriptor = descriptor;
  TypeDescriptor& mutable_descriptor =
      *const_cast<TypeDescriptor*>(descriptor);
  const bool owns_fresh_syntax =
      symbol.type && symbol.type->kind != Kind::TyName &&
      symbol.type->kind != Kind::TyMetaclass;
  const bool owns_semantic_payload =
      !std::holds_alternative<std::monostate>(mutable_descriptor.payload);
  if (!mutable_descriptor.symbol &&
      (owns_fresh_syntax || owns_semantic_payload)) {
    mutable_descriptor.symbol = &symbol;
  }
  bind_type_expr_descriptor(symbol.type, descriptor);
}

bool symbol_declares_fresh_type(const TypeSymbol& symbol) {
  return symbol.type && symbol.type->kind != Kind::TyName &&
         symbol.type->kind != Kind::TyMetaclass;
}

const TypeDescriptor* ensure_fresh_symbol_descriptor(TypeRegistry& r,
                                                     TypeSymbol& symbol) {
  if (symbol.descriptor) {
    bind_symbol_descriptor(symbol, symbol.descriptor);
    return symbol.descriptor;
  }
  const TypeDescriptor* descriptor =
      make_type_descriptor(r, symbol.type, &symbol);
  bind_symbol_descriptor(symbol, descriptor);
  return descriptor;
}

bool is_forward_reference_class_type(const TypeExpr* type);
bool is_complete_reference_class_type(const TypeExpr* type);
TypeDescriptorPayload semantic_payload_for_type(
    const std::string& unit, const std::string& name,
    const std::vector<std::string>& owner_path, const TypeExpr& type);
void resolve_anonymous_descriptor_payload(
    TypeRegistry& r, const TypeDescriptor* descriptor,
    const TypeLookupContext* context);

enum class ExprSemanticUse {
  Value,
  Statement,
};

void bind_expr_semantics(
    TypeRegistry& r, const ExprPtr& expr,
    const TypeLookupContext* context,
    ExprSemanticUse use = ExprSemanticUse::Value);
void bind_stmt_semantics(TypeRegistry& r, const StmtPtr& stmt,
                         const TypeLookupContext* context);

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
      initialize_structural_descriptor_edges(
          registry, *const_cast<TypeDescriptor*>(descriptor));
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

    const TypeDescriptor* descriptor =
        resolve_type(symbol->type, /*pointer_target=*/false);
    bind_symbol_descriptor(mut, descriptor);

    resolving_symbols.erase(symbol);
    return descriptor;
  }

  const TypeSymbol* resolve_named_type_symbol(const TypeExpr* type,
                                              std::string_view raw_name,
                                              bool pointer_target) {
    const std::string name = lc(std::string(raw_name));
    const TypeSymbol* symbol = nullptr;
    bool permitted_forward = false;
    if (pointer_target && pointer_forwards) {
      auto fit = pointer_forwards->find(name);
      if (fit != pointer_forwards->end()) {
        symbol = fit->second;
        permitted_forward = true;
      }
    }
    if (!symbol && visible_forward_classes) {
      auto fit = visible_forward_classes->find(name);
      if (fit != visible_forward_classes->end()) {
        symbol = fit->second;
        permitted_forward = true;
      }
    }
    if (!symbol) {
      symbol = registry.lookup_type_symbol_in_context(pascal_key(name),
                                                      context);
    }
    if (!permitted_forward && ordinary_forwards &&
        ordinary_forwards->count(name) > 0) {
      const TypeSymbol* same_run_symbol = nullptr;
      if (pointer_forwards) {
        auto fit = pointer_forwards->find(name);
        if (fit != pointer_forwards->end()) same_run_symbol = fit->second;
      }
      if (!symbol ||
          (symbol == same_run_symbol && !symbol->has_forward_declaration)) {
        report_error(type ? type->loc : Location{},
                     "type `" + name + "` is not visible before this "
                     "declaration");
        return nullptr;
      }
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
    const TypeSymbol* target =
        named->descriptor && named->descriptor->symbol
            ? named->descriptor->symbol
            : named;
    const ClassInfo* class_info = target ? target->class_info() : nullptr;
    if (!class_info || !class_info->is_reference_type) {
      report_error(type.loc, "`class of` target `" +
                                 lc(type.class_name) +
                                 "` is not a reference class");
      return nullptr;
    }
    const TypeDescriptor* descriptor =
        make_metaclass_descriptor(registry, target);
    bind_type_expr_descriptor(&type, descriptor);
    bind_type_expr_symbol(&type, named);
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
    if (type->descriptor && type->kind != Kind::TyName) {
      resolve_children(type);
      initialize_structural_descriptor_edges(
          registry, *const_cast<TypeDescriptor*>(type->descriptor));
      context = saved_context;
      return type->descriptor;
    }

    if (type->kind == Kind::TyName) {
      const TypeSymbol* symbol = resolve_named_type_symbol(
          type, static_cast<const TyName&>(*type).name, pointer_target);
      if (!symbol) {
        context = saved_context;
        return nullptr;
      }
      const TypeDescriptor* descriptor = resolve_symbol_reference(symbol);
      bind_type_expr_descriptor(type, descriptor);
      bind_type_expr_symbol(type, symbol);
      context = saved_context;
      return descriptor;
    }

    if (type->kind == Kind::TyMetaclass) {
      const TypeDescriptor* descriptor =
          resolve_metaclass_type(static_cast<const TyMetaclass&>(*type));
      context = saved_context;
      return descriptor;
    }

    const std::string unit = context ? context->unit : std::string{};
    const TypeDescriptor* descriptor = make_type_descriptor(
        registry, type, nullptr,
        semantic_payload_for_type(unit, {}, {}, *type));
    bind_type_expr_descriptor(type, descriptor);
    resolve_children(type);
    initialize_structural_descriptor_edges(
        registry, *const_cast<TypeDescriptor*>(descriptor));
    resolve_anonymous_descriptor_payload(registry, descriptor, context);
    context = saved_context;
    return descriptor;
  }

  void resolve_params(const std::vector<Param>& params) {
    for (const auto& param : params) {
      param.descriptor =
          param.type
              ? resolve_type(param.type.get(), /*pointer_target=*/false)
              : builtin_descriptor(registry, "pointer");
      bind_expr_semantics(registry, param.default_value, context);
    }
  }

  void resolve_variant_part(const std::shared_ptr<VariantPart>& variant) {
    if (!variant) return;
    resolve_type(variant->tag_type.get(), /*pointer_target=*/false);
    for (const auto& vcase : variant->cases) {
      for (const ExprPtr& label : vcase.labels) {
        bind_expr_semantics(registry, label, context);
      }
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

  void resolve_type_decl_section(
      const std::vector<const TypeDecl*>& declarations,
      std::unordered_map<std::string, const TypeSymbol*>&
          visible_forward_classes) {
    std::unordered_map<std::string, const TypeSymbol*> section_symbols;
    std::unordered_set<std::string> unresolved;
    for (const TypeDecl* declaration : declarations) {
      if (!declaration) continue;
      const std::string name = lc(declaration->name);
      if (const TypeSymbol* symbol = declaration->symbol) {
        section_symbols[name] = symbol;
        unresolved.insert(name);
        if (symbol_declares_fresh_type(*symbol)) {
          ensure_fresh_symbol_descriptor(
              registry, *const_cast<TypeSymbol*>(symbol));
        }
      }
    }

    for (const TypeDecl* declaration : declarations) {
      if (!declaration) continue;
      const std::string name = lc(declaration->name);
      unresolved.erase(name);
      const TypeLookupContext* declaration_context =
          declaration->type_context ? declaration->type_context : context;
      const TypeSymbol* symbol = declaration->symbol;
      if (!symbol) {
        TypeDescriptorResolver resolver(
            registry, declaration_context, &section_symbols, &unresolved);
        resolver.resolve_type(declaration->type.get(),
                              /*pointer_target=*/false);
        continue;
      }
      if (symbol->has_forward_declaration ||
          is_forward_reference_class_type(declaration->type.get())) {
        visible_forward_classes[name] = symbol;
      }
      TypeDescriptorResolver resolver(
          registry, declaration_context, &section_symbols, &unresolved,
          &visible_forward_classes);
      if (is_forward_reference_class_type(declaration->type.get()) &&
          declaration->type.get() != symbol->type) {
        const TypeDescriptor* descriptor = ensure_fresh_symbol_descriptor(
            registry, *const_cast<TypeSymbol*>(symbol));
        bind_type_expr_descriptor(declaration->type.get(), descriptor);
        bind_type_expr_symbol(declaration->type.get(), symbol);
        continue;
      }
      resolver.resolve_symbol_declaration(symbol);
    }
  }

  void resolve_nested_type_declarations(
      const std::vector<std::shared_ptr<TypeDecl>>& declarations) {
    std::unordered_map<std::string, const TypeSymbol*> visible_forwards;
    for (size_t begin = 0; begin < declarations.size();) {
      const size_t section_id = declarations[begin]
                                    ? declarations[begin]->type_section_id
                                    : 0;
      size_t end = begin + 1;
      while (end < declarations.size() && declarations[end] &&
             declarations[end]->type_section_id == section_id) {
        ++end;
      }
      std::vector<const TypeDecl*> section;
      section.reserve(end - begin);
      for (size_t i = begin; i < end; ++i) {
        section.push_back(declarations[i].get());
      }
      resolve_type_decl_section(section, visible_forwards);
      begin = end;
    }
  }

  void resolve_object_nested_types(
      const std::vector<ObjectMember>& members) {
    std::unordered_map<std::string, const TypeSymbol*> visible_forwards;
    for (size_t begin = 0; begin < members.size();) {
      if (members[begin].kind != ObjectMemberKind::Type ||
          !members[begin].type_decl) {
        ++begin;
        continue;
      }
      const size_t section_id = members[begin].type_decl->type_section_id;
      size_t end = begin;
      std::vector<const TypeDecl*> section;
      while (end < members.size() &&
             members[end].kind == ObjectMemberKind::Type &&
             members[end].type_decl &&
             members[end].type_decl->type_section_id == section_id) {
        section.push_back(members[end].type_decl.get());
        ++end;
      }
      resolve_type_decl_section(section, visible_forwards);
      begin = end;
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
        resolve_nested_type_declarations(record.nested_types);
        for (const auto& field : record.fields) {
          resolve_type(field.type.get(), /*pointer_target=*/false);
        }
        resolve_variant_part(record.variant_part);
        break;
      }
      case Kind::TyObject: {
        const auto& object = static_cast<const TyObject&>(*type);
        resolve_object_nested_types(object.members);
        for (const auto& member : object.members) {
          if (member.kind != ObjectMemberKind::Type) {
            resolve_object_member(member);
          }
        }
        break;
      }
      case Kind::TyInterface: {
        const auto& intf = static_cast<const TyInterface&>(*type);
        resolve_object_nested_types(intf.members);
        for (const auto& member : intf.members) {
          if (member.kind != ObjectMemberKind::Type) {
            resolve_object_member(member);
          }
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
      case Kind::TyEnum:
        for (const EnumMember& member :
             static_cast<const TyEnum&>(*type).members) {
          bind_expr_semantics(registry, member.value, context);
        }
        break;
      case Kind::TySubrange: {
        const auto& subrange = static_cast<const TySubrange&>(*type);
        bind_expr_semantics(registry, subrange.lo, context);
        bind_expr_semantics(registry, subrange.hi, context);
        break;
      }
      case Kind::TyString:
        bind_expr_semantics(
            registry, static_cast<const TyString&>(*type).max_length, context);
        break;
      default:
        break;
    }
  }
};

bool is_forward_reference_class_type(const TypeExpr* type) {
  if (!type || type->kind != Kind::TyObject) return false;
  const auto& object = static_cast<const TyObject&>(*type);
  return object.is_reference_type && object.is_forward;
}

bool is_complete_reference_class_type(const TypeExpr* type) {
  if (!type || type->kind != Kind::TyObject) return false;
  const auto& object = static_cast<const TyObject&>(*type);
  return object.is_reference_type && !object.is_forward;
}

void resolve_type_decl_run_descriptors(
    TypeRegistry& r, const std::vector<DeclPtr>& decls, size_t begin,
    size_t end, const TypeLookupContext* context,
    std::unordered_map<std::string, const TypeSymbol*>&
        visible_forward_classes) {
  std::vector<const TypeDecl*> declarations;
  declarations.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    declarations.push_back(
        &static_cast<const TypeDecl&>(*decls[i]));
  }
  TypeDescriptorResolver(r, context).resolve_type_decl_section(
      declarations, visible_forward_classes);
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
      const size_t section_id =
          static_cast<const TypeDecl&>(*d).type_section_id;
      size_t end = i + 1;
      while (end < decls.size() && decls[end] &&
             decls[end]->kind == Kind::TypeDecl &&
             static_cast<const TypeDecl&>(*decls[end]).type_section_id ==
                 section_id) {
        ++end;
      }
      resolve_type_decl_run_descriptors(r, decls, i, end, context,
                                        visible_forward_classes);
      i = end;
      continue;
    }

    const TypeLookupContext* decl_context =
        d->type_context ? d->type_context : context;
    TypeDescriptorResolver resolver(r, decl_context);
    switch (d->kind) {
      case Kind::VarDecl:
        resolver.resolve_type(static_cast<const VarDecl&>(*d).type.get(),
                              /*pointer_target=*/false);
        bind_expr_semantics(r, static_cast<const VarDecl&>(*d).init,
                            decl_context);
        bind_expr_semantics(r, static_cast<const VarDecl&>(*d).external_name,
                            decl_context);
        break;
      case Kind::ConstDecl:
        resolver.resolve_type(static_cast<const ConstDecl&>(*d).type.get(),
                              /*pointer_target=*/false);
        bind_expr_semantics(r, static_cast<const ConstDecl&>(*d).value,
                            decl_context);
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
        bind_stmt_semantics(r, pd.body, r.lookup_proc_body_context(&pd));
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

std::vector<const Param*> flattened_params(const std::vector<Param>& params) {
  std::vector<const Param*> out;
  for (const Param& p : params) {
    const size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) out.push_back(&p);
  }
  return out;
}

const TypeExpr* signature_payload_shape(const TypeRegistry& r,
                                        const TypeExpr* type) {
  if (!type) return nullptr;
  const TypeDescriptor* descriptor = r.descriptor_for_type(type);
  if (!descriptor) return nullptr;
  return descriptor->type ? descriptor->type : type;
}

const TyPointer* signature_pointer_shape(const TypeRegistry& r,
                                         const TypeExpr* type) {
  const TypeExpr* shape = signature_payload_shape(r, type);
  if (!shape) return nullptr;
  if (shape->kind == Kind::TyPointer) {
    return &static_cast<const TyPointer&>(*shape);
  }
  if (shape->kind == Kind::TyDistinct) {
    const auto& distinct = static_cast<const TyDistinct&>(*shape);
    return signature_pointer_shape(r, distinct.underlying.get());
  }
  return nullptr;
}

bool bound_signature_params_match(const TypeRegistry& r,
                                  const std::vector<Param>& a,
                                  const std::vector<Param>& b);

bool bound_signature_type_exprs_match(
    const TypeRegistry& r, const TypeExpr* a, const TypeExpr* b,
    std::vector<std::pair<const TypeExpr*, const TypeExpr*>>& seen) {
  if (a == b) return true;
  if (!a || !b) return false;
  for (const auto& pair : seen) {
    if (pair.first == a && pair.second == b) return true;
  }
  seen.push_back({a, b});

  const TypeDescriptor* ad = r.descriptor_for_type(a);
  const TypeDescriptor* bd = r.descriptor_for_type(b);
  if (ad && bd && ad == bd) return true;

  const TyPointer* ap = signature_pointer_shape(r, a);
  const TyPointer* bp = signature_pointer_shape(r, b);
  if (ap || bp) {
    if (!ap || !bp) return false;
    return bound_signature_type_exprs_match(r, ap->target.get(),
                                            bp->target.get(), seen);
  }

  const TypeExpr* ashape = signature_payload_shape(r, a);
  const TypeExpr* bshape = signature_payload_shape(r, b);
  if (!ashape || !bshape || ashape->kind != bshape->kind) return false;

  switch (ashape->kind) {
    case Kind::TyString:
      return true;
    case Kind::TySet:
      return bound_signature_type_exprs_match(
          r, static_cast<const TySet&>(*ashape).element.get(),
          static_cast<const TySet&>(*bshape).element.get(), seen);
    case Kind::TyFile: {
      const auto& af = static_cast<const TyFile&>(*ashape);
      const auto& bf = static_cast<const TyFile&>(*bshape);
      if (af.is_text != bf.is_text) return false;
      // Procedure signatures use null file elements for the single untyped
      // Pascal file type, not for an unresolved element type.
      if (!af.element || !bf.element) return !af.element && !bf.element;
      return bound_signature_type_exprs_match(
          r, af.element.get(), bf.element.get(), seen);
    }
    case Kind::TyArray: {
      const auto& aa = static_cast<const TyArray&>(*ashape);
      const auto& bb = static_cast<const TyArray&>(*bshape);
      if (aa.array_kind != bb.array_kind || aa.dims.size() != bb.dims.size()) {
        return false;
      }
      for (size_t i = 0; i < aa.dims.size(); ++i) {
        if (!bound_signature_type_exprs_match(r, aa.dims[i].get(),
                                              bb.dims[i].get(), seen)) {
          return false;
        }
      }
      return bound_signature_type_exprs_match(r, aa.element.get(),
                                              bb.element.get(), seen);
    }
    case Kind::TyProcedural: {
      const auto& apc = static_cast<const TyProcedural&>(*ashape);
      const auto& bpc = static_cast<const TyProcedural&>(*bshape);
      return apc.is_function == bpc.is_function &&
             apc.is_method == bpc.is_method &&
             apc.is_cdecl == bpc.is_cdecl &&
             bound_signature_type_exprs_match(r, apc.return_type.get(),
                                              bpc.return_type.get(), seen) &&
             bound_signature_params_match(r, apc.params, bpc.params);
    }
    default:
      // For nominal source forms such as records, classes, enums, and
      // ordinary type names, descriptor identity above is the whole answer.
      // Falling back to spelling here would recreate the pre-build matcher.
      return false;
  }
}

bool bound_signature_type_exprs_match(const TypeRegistry& r, const TypeExpr* a,
                                      const TypeExpr* b) {
  std::vector<std::pair<const TypeExpr*, const TypeExpr*>> seen;
  return bound_signature_type_exprs_match(r, a, b, seen);
}

bool bound_signature_params_match(const TypeRegistry& r,
                                  const std::vector<Param>& a,
                                  const std::vector<Param>& b) {
  const std::vector<const Param*> as = flattened_params(a);
  const std::vector<const Param*> bs = flattened_params(b);
  if (as.size() != bs.size()) return false;
  for (size_t i = 0; i < as.size(); ++i) {
    if (as[i]->mode != bs[i]->mode) return false;
    if (!bound_signature_type_exprs_match(r, as[i]->type.get(),
                                          bs[i]->type.get())) {
      return false;
    }
  }
  return true;
}

bool bound_proc_signature_matches(const TypeRegistry& r, const ProcDecl& a,
                                  const ProcDecl& b) {
  return a.pkind == b.pkind && a.is_operator == b.is_operator &&
         a.is_class_method == b.is_class_method &&
         bound_signature_type_exprs_match(r, a.return_type.get(),
                                          b.return_type.get()) &&
         bound_signature_params_match(r, a.params, b.params);
}

void prune_completed_interface_proc_impls(TypeRegistry& r, UnitInfo& ui) {
  for (auto it = ui.impl_procs.begin(); it != ui.impl_procs.end();) {
    auto& [name, impls] = *it;
    auto iface_it = ui.iface_procs.find(name);
    if (iface_it == ui.iface_procs.end()) {
      ++it;
      continue;
    }
    const std::vector<ProcInfo>& iface_procs = iface_it->second;
    impls.erase(std::remove_if(impls.begin(), impls.end(),
                               [&](const ProcInfo& impl) {
                                 if (!impl.decl) return false;
                                 for (const ProcInfo& iface : iface_procs) {
                                   if (iface.decl &&
                                       bound_proc_signature_matches(
                                           r, *iface.decl, *impl.decl)) {
                                     return true;
                                   }
                                 }
                                 return false;
                               }),
                impls.end());
    if (impls.empty()) {
      it = ui.impl_procs.erase(it);
    } else {
      ++it;
    }
  }
}

template <typename Map>
void insert_map_keys(std::unordered_set<std::string>& out, const Map& map) {
  for (const auto& entry : map) out.insert(entry.first);
}

template <typename Map, typename Value>
bool insert_unit_storage_symbol(Map& map, const std::string& name, Value value,
                                Location loc) {
  auto [_, inserted] = map.emplace(name, std::move(value));
  if (!inserted) {
    report_error(loc, "duplicate identifier `" + name + "`");
    return false;
  }
  return true;
}

bool unit_value_name_is_taken(const UnitInfo& ui, bool is_interface,
                              const std::string& name) {
  // Enum labels are unit value declarations. Treating a duplicate as
  // shadowing here would bake an invalid Pascal namespace into the build
  // frame and make later expression binding depend on insertion order.
  return is_interface
             ? (ui.find_export_var(name) || ui.find_export_const(name) ||
                ui.find_export_procs(name))
             : (ui.find_var(name) || ui.find_const(name) ||
                ui.find_procs(name));
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
                  .return_type_name = {},
                  .return_type_symbol = nullptr,
                  .slot_info = {}};
}

void add_unit_enum_members(UnitInfo* ui, bool is_interface,
                           const EnumInfoReg& info) {
  if (!ui) return;
  auto& members = is_interface ? ui->iface_enum_members
                               : ui->impl_enum_members;
  for (size_t ordinal = 0; ordinal < info.members.size(); ++ordinal) {
    members.try_emplace(
        info.members[ordinal],
        EnumMemberInfo{&info, static_cast<int64_t>(ordinal)});
  }
}

void add_enum_members(
    std::unordered_map<std::string, EnumMemberInfo>& members,
    const TypeExpr* t) {
  if (!t) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*t);
  for (const TyEnum* te : enums) {
    const EnumInfoReg* info =
        te && te->descriptor ? te->descriptor->enum_info() : nullptr;
    if (!info) continue;
    for (size_t ordinal = 0; ordinal < info->members.size(); ++ordinal) {
      members.try_emplace(
          info->members[ordinal],
          EnumMemberInfo{info, static_cast<int64_t>(ordinal)});
    }
  }
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
  std::unordered_set<std::string> enum_members;
  for (size_t ordinal = 0; ordinal < info.members.size(); ++ordinal) {
    const std::string& member = info.members[ordinal];
    if (!enum_members.insert(member).second) {
      report_error(info.type->loc, "duplicate identifier `" + member + "`");
      continue;
    }
    if (ui && unit_value_name_is_taken(*ui, is_interface, member)) {
      report_error(info.type->loc, "duplicate identifier `" + member + "`");
      continue;
    }
    auto [it, inserted] = by_member.emplace(
        member, EnumMemberInfo{&info, static_cast<int64_t>(ordinal)});
    if (!inserted && it->second.owner != &info) {
      report_error(info.type->loc, "duplicate identifier `" + member + "`");
    }
  }
  add_unit_enum_members(ui, is_interface, info);
}

EnumInfoReg* ensure_enum_descriptor_info(TypeRegistry& r, const TyEnum& type,
                                         std::string unit,
                                         std::string name,
                                         std::string cxx_name) {
  TypeDescriptor* descriptor = const_cast<TypeDescriptor*>(type.descriptor);
  if (!descriptor) {
    descriptor = const_cast<TypeDescriptor*>(
        make_type_descriptor(r, &type, nullptr));
    bind_type_expr_descriptor(&type, descriptor);
  }
  if (!descriptor->enum_info()) {
    descriptor->payload =
        make_enum_info(unit, name, cxx_name, type);
    bind_descriptor_payload(*descriptor);
  }
  return descriptor->mutable_enum_info();
}

void bind_method_owner_symbol(TypeSymbol& symbol) {
  auto bind_methods = [&](auto& methods) {
    for (auto& [_, overloads] : methods) {
      for (auto& method : overloads) {
        method.declaring_symbol = &symbol;
      }
    }
  };
  if (ClassInfo* ci = symbol.mutable_class_info()) {
    ci->symbol = &symbol;
    bind_methods(ci->methods);
  } else if (InterfaceInfo* ii = symbol.mutable_interface_info()) {
    ii->symbol = &symbol;
    bind_methods(ii->methods);
  }
}

TypeSymbol* find_unit_type_symbol(UnitInfo& ui, const std::string& name) {
  auto iit = ui.iface_types.find(name);
  if (iit != ui.iface_types.end()) return iit->second;
  auto mit = ui.impl_types.find(name);
  return mit == ui.impl_types.end() ? nullptr : mit->second;
}

void populate_nested_types(TypeRegistry& r, TypeSymbol& symbol);
void report_local_enum_duplicates(const TypeExpr* type);

TypeSymbol* upsert_unit_type_symbol(TypeRegistry& r, UnitInfo* ui,
                                    bool is_interface,
                                    TypeSymbol new_symbol,
                                    bool allow_forward_completion = false) {
  const std::string low_name = new_symbol.name;
  TypeSymbol* stored = ui ? find_unit_type_symbol(*ui, low_name) : nullptr;
  if (!stored) {
    r.type_symbols.push_back(std::move(new_symbol));
    stored = &r.type_symbols.back();
  } else {
    const ClassInfo* next_class = new_symbol.class_info();
    const ClassInfo* current_class = stored->class_info();
    const bool completes_forward_class =
        allow_forward_completion && next_class && current_class &&
        current_class->is_forward && !next_class->is_forward;
    if (completes_forward_class) {
      const bool had_forward_declaration =
          stored->has_forward_declaration;
      const TypeDescriptor* descriptor = stored->descriptor;
      const TypeDescriptor* completed_descriptor = new_symbol.descriptor;
      if (descriptor && completed_descriptor) {
        TypeDescriptor& target =
            *const_cast<TypeDescriptor*>(descriptor);
        TypeDescriptor& completed =
            *const_cast<TypeDescriptor*>(completed_descriptor);
        target.type = new_symbol.type;
        target.metaclass_target = completed.metaclass_target;
        target.payload = std::move(completed.payload);
        bind_descriptor_payload(target);
      }
      *stored = std::move(new_symbol);
      stored->descriptor = descriptor;
      stored->has_forward_declaration = had_forward_declaration;
      if (descriptor) bind_type_expr_descriptor(stored->type, descriptor);
    }
  }
  if (stored->descriptor) {
    bind_symbol_descriptor(*stored, stored->descriptor);
  }
  bind_method_owner_symbol(*stored);
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
    EnumInfoReg* info =
        ensure_enum_descriptor_info(r, *te, unit, key, cxx_name);
    index_enum_info(r, ui, is_interface, *info);
    ++anon_index;
  }
}

void append_record_variant_fields(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::unordered_map<std::string, FieldInfo>& fields, Location record_loc);

void insert_field_info(std::unordered_map<std::string, FieldInfo>& fields,
                       std::string_view name, const FieldInfo& field,
                       Location where) {
  const std::string key = lc(std::string(name));
  auto [_, inserted] = fields.emplace(key, field);
  if (!inserted) {
    // Keep the first declaration after diagnosing the duplicate. Replacing it
    // would let later semantic lookup use a different member than the Pascal
    // declaration order admitted.
    report_error(where, "duplicate identifier `" + key + "`");
  }
}

void append_record_variant_fields(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::unordered_map<std::string, FieldInfo>& fields, Location record_loc) {
  if (!vpart) return;
  if (!vpart->tag_name.empty()) {
    insert_field_info(
        fields, vpart->tag_name,
        FieldInfo{.type = vpart->tag_type, .is_class_var = false},
        record_loc);
  }
  for (const auto& vc : vpart->cases) {
    for (const auto& f : vc.fields) {
      const FieldInfo field{.type = f.type,
                            .is_class_var = false,
                            .is_variant = true};
      for (const auto& n : f.names) {
        insert_field_info(fields, n, field, record_loc);
      }
    }
    append_record_variant_fields(vc.variant_part, fields, record_loc);
  }
}

std::unordered_map<std::string, FieldInfo> record_fields(const TyRecord& tr) {
  std::unordered_map<std::string, FieldInfo> fields;
  for (const auto& f : tr.fields) {
    const FieldInfo field{.type = f.type, .is_class_var = false};
    for (const auto& n : f.names) insert_field_info(fields, n, field, tr.loc);
  }
  append_record_variant_fields(tr.variant_part, fields, tr.loc);
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
  return MethodSig{.kind = method_kind_for(pd),
                   .defining_unit = std::move(defining_unit),
                   .declaring_type = std::move(declaring_type),
                   .param_count = proc_param_count(pd.params),
                   .accepts_zero_args = proc_accepts_zero_args(pd),
                   .is_function = (pd.pkind == ProcKind::Function),
                   .is_virtual = pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override,
                   .is_final = pd.modifiers.is_final,
                   .is_overload = pd.modifiers.is_overload,
                   .decl = std::move(method)};
}

std::unordered_map<std::string, FieldInfo> class_fields(const TyObject& to) {
  std::unordered_map<std::string, FieldInfo> fields;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Field) {
      const FieldInfo field{.type = m.field_type,
                            .is_class_var = m.is_class_var};
      for (const auto& n : m.field_names) {
        insert_field_info(fields, n, field, m.loc);
      }
    }
  }
  return fields;
}

std::unordered_map<std::string, EnumMemberInfo> class_enum_members(
    const TyObject& to) {
  std::unordered_map<std::string, EnumMemberInfo> members;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Field) {
      add_enum_members(members, m.field_type.get());
    }
  }
  return members;
}

void refresh_class_enum_members(ClassInfo& info) {
  info.enum_members.clear();
  for (const auto& [_, field] : info.fields) {
    add_enum_members(info.enum_members, field.type.get());
  }
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
    const std::string key = lc(m.property.name);
    auto [_, inserted] = properties.emplace(
        key, PropertyInfo{.type = m.property.type,
                          .params = m.property.params,
                          .read = {},
                          .write = {},
                          .is_default = m.property.is_default});
    if (!inserted) {
      report_error(m.loc, "duplicate identifier `" + key + "`");
    }
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
        .decl = m.method});
  }
  return methods;
}

ClassInfo class_info_for(const std::string& unit, const std::string& name,
                         const std::vector<std::string>& owner_path,
                         const TyObject& to) {
  const std::string declaring_type = type_source_name(name, owner_path);
  return ClassInfo{.name = name,
                   .owner_path = owner_path,
                   .parent = (to.is_reference_type && to.parent.empty())
                                 ? "__rt__.tobject"
                                 : lc(to.parent),
                   .defining_unit = unit,
                   .is_reference_type = to.is_reference_type,
                   .is_abstract = to.is_abstract,
                   .is_forward = to.is_forward,
                   .interface_symbols = {},
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

TypeDescriptorPayload semantic_payload_for_type(
    const std::string& unit, const std::string& name,
    const std::vector<std::string>& owner_path, const TypeExpr& type) {
  switch (type.kind) {
    case Kind::TyObject:
      return class_info_for(unit, name, owner_path,
                            static_cast<const TyObject&>(type));
    case Kind::TyRecord:
      return record_info_for(unit, name, static_cast<const TyRecord&>(type));
    case Kind::TyInterface:
      return interface_info_for(unit, name,
                                static_cast<const TyInterface&>(type));
    case Kind::TyEnum:
      if (!name.empty()) {
        return make_enum_info(unit, name, type_mangle(name),
                              static_cast<const TyEnum&>(type));
      }
      break;
    default:
      break;
  }
  return {};
}

TypeSymbol make_type_symbol_for_type_with_owner(
    TypeRegistry& registry, std::string_view unit, std::string_view name,
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
  if (td.symbol) {
    if (auto existing_slot = r.nested_type_symbols.find(key);
        existing_slot != r.nested_type_symbols.end()) {
      return existing_slot->second;
    }
  }
  TypeSymbol next = make_type_symbol_for_type_with_owner(
      r, owner.defining_unit, td.name, td.type, child_owner_path);
  auto [slot_it, inserted] =
      r.nested_type_symbols.emplace(key, std::shared_ptr<TypeSymbol>{});
  auto& slot = slot_it->second;
  if (!inserted && slot) {
    // Pascal nested declarations share one owner scope. A duplicate name is a
    // source error; keeping the first symbol prevents later build passes from
    // changing the identity already visible to earlier members.
    report_error(td.loc, "duplicate identifier `" + lc(td.name) + "`");
    td.symbol = slot.get();
    return slot;
  }
  slot = std::make_shared<TypeSymbol>(std::move(next));
  if (slot->descriptor) {
    bind_symbol_descriptor(*slot, slot->descriptor);
  }
  report_local_enum_duplicates(td.type.get());
  bind_method_owner_symbol(*slot);
  populate_nested_types(r, *slot);
  td.symbol = slot.get();
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
    TypeRegistry& registry, std::string_view unit, std::string_view name,
    std::shared_ptr<const TypeExpr> type,
    std::vector<std::string> owner_path) {
  const std::string low_name = lc(std::string(name));
  const std::string low_unit = lc(std::string(unit));
  if (!type) {
    throw std::logic_error("make_type_symbol_for_type called without a type");
  }
  TypeDescriptorPayload payload =
      semantic_payload_for_type(low_unit, low_name, owner_path, *type);
  TypeSymbol symbol(low_name, low_unit, std::move(type));
  symbol.owner_path = std::move(owner_path);
  symbol.has_forward_declaration =
      is_forward_reference_class_type(symbol.type);
  if (symbol_declares_fresh_type(symbol)) {
    symbol.descriptor =
        make_type_descriptor(registry, symbol.type, nullptr,
                             std::move(payload));
    bind_type_expr_descriptor(symbol.type, symbol.descriptor);
  }
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
  // Runtime declarations are synthetic AST, but they should still obey the
  // same ownership rule as parsed declarations: the registry owns the syntax
  // node and build later binds it to the canonical builtin/runtime symbol.
  return std::make_shared<TyName>(std::move(name));
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

TypePtr runtime_metaclass_type(std::string class_name) {
  return std::make_shared<TyMetaclass>(Location{}, std::move(class_name));
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
      VarInfo{.defining_unit = "__rt__",
              .type = std::move(type),
              .loc = {}};
}

void register_runtime_const(UnitInfo& rt_exports, std::string name,
                            std::shared_ptr<const TypeExpr> type,
                            std::shared_ptr<const Expr> value) {
  rt_exports.iface_consts[lc(std::move(name))] =
      ConstInfo{.defining_unit = "__rt__",
                .type = std::move(type),
                .value = std::move(value),
                .loc = {}};
}

TypeSymbol* register_runtime_type(TypeRegistry& r, UnitInfo& rt_exports,
                                   std::string name,
                                   std::shared_ptr<const TypeExpr> target) {
  const std::string low = lc(std::move(name));
  if (!target) return nullptr;
  TypeSymbol symbol =
      make_type_symbol_for_type(r, "__rt__", low, std::move(target));
  TypeSymbol* stored = upsert_unit_type_symbol(
      r, &rt_exports, /*is_interface=*/true, std::move(symbol));
  if (const EnumInfoReg* info = stored->enum_info()) {
    index_enum_info(r, &rt_exports, /*is_interface=*/true, *info);
  }
  if (stored->type) {
    register_enums_in_type(
        r, &rt_exports, /*is_interface=*/true, "__rt__", *stored->type, low,
        stored->type->kind == Kind::TyEnum
            ? static_cast<const TyEnum*>(stored->type)
            : nullptr);
  }
  return stored;
}

TypeSymbol* register_runtime_type_alias(TypeRegistry& r,
                                          UnitInfo& rt_exports,
                                          std::string name,
                                          const TypeSymbol* target) {
  if (!target || !target->descriptor) return nullptr;
  const std::string low = lc(std::move(name));
  auto alias_type = std::make_shared<TyName>(target->name);
  bind_type_expr_symbol(alias_type.get(), target);
  bind_type_expr_descriptor(alias_type.get(), target->descriptor);
  TypeSymbol symbol(low, "__rt__", alias_type);
  bind_symbol_descriptor(symbol, target->descriptor);
  return upsert_unit_type_symbol(
      r, &rt_exports, /*is_interface=*/true, std::move(symbol));
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
    const std::string method_name = m.decl->name;
    method_map[method_name].push_back(std::move(m));
  }
  ClassInfo info{.name = key,
                 .owner_path = {},
                 .parent = std::move(parent),
                 .defining_unit = "__rt__",
                 .is_reference_type = true,
                 .is_abstract = false,
                 .is_forward = false,
                 .interface_symbols = {},
                 .fields = {},
                 .methods = std::move(method_map),
                 .properties = {},
                 .nested_types = {},
                 .enum_members = {},
                 .default_property_name = {}};
  if (auto rt = r.units.find("__rt__"); rt != r.units.end()) {
    TypePtr class_type = runtime_type_name(key);
    TypeSymbol symbol(key, "__rt__", class_type);
    symbol.descriptor =
        make_type_descriptor(r, symbol.type, nullptr, std::move(info));
    bind_type_expr_descriptor(symbol.type, symbol.descriptor);
    TypeSymbol* stored = upsert_unit_type_symbol(
        r, &rt->second, /*is_interface=*/true, std::move(symbol));
    if (stored) {
      // Runtime class declarations are synthetic, so no parser type block
      // exists to bind their representative TyName to the declaration.
      bind_type_expr_symbol(stored->type, stored);
    }
  }
}

void register_runtime_class_field(TypeRegistry& r, std::string class_name,
                                  std::string field_name, TypePtr type) {
  const std::string low_class = lc(std::move(class_name));
  const std::string low_field = lc(std::move(field_name));
  FieldInfo field{.type = std::move(type), .is_class_var = false};
  if (TypeSymbol* symbol =
          r.lookup_type_symbol_exact_mut(pascal_key("__rt__"),
                                         pascal_key(low_class))) {
    if (ClassInfo* info = symbol->mutable_class_info()) {
      info->fields[low_field] = std::move(field);
    }
  }
}

const TypeSymbol* resolve_runtime_return_type_symbol(TypeRegistry& r,
                                                     std::string_view name) {
  if (name.empty()) return nullptr;
  if (const TypeSymbol* literal = r.builtin_literal(name)) return literal;
  if (const TypeLookupContext* context =
          r.lookup_unit_context(pascal_key("__rt__"),
                                /*implementation=*/false)) {
    if (const TypeSymbol* symbol =
            r.lookup_type_symbol_in_context(pascal_key(name), context)) {
      return symbol;
    }
  }
  return r.lookup_type_symbol_exact(pascal_key("__rt__"), pascal_key(name));
}

void resolve_runtime_proc_return_symbols(TypeRegistry& r, UnitInfo& unit) {
  auto resolve_map = [&](auto& proc_map) {
    for (auto& [_, procs] : proc_map) {
      for (ProcInfo& proc : procs) {
        if (proc.return_type_name.empty() || proc.return_type_symbol) continue;
        proc.return_type_symbol =
            resolve_runtime_return_type_symbol(r, proc.return_type_name);
        if (!proc.return_type_symbol) {
          report_error(Location{}, "runtime proc `" + proc.defining_unit +
                                       "` has unresolved return type `" +
                                       proc.return_type_name + "`");
        }
      }
    }
  };
  resolve_map(unit.iface_procs);
  resolve_map(unit.iface_operators);
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
      case RuntimeUnitExportKind::Type: {
        TypeSymbol* symbol = r.lookup_type_symbol_exact_mut(
            pascal_key("__rt__"), pascal_key(name));
        if (!symbol) {
          report_error(Location{}, "runtime-backed unit `" + low +
                                   "` references missing runtime type `" +
                                   name + "`");
          continue;
        }
        unit.iface_types[name] = symbol;
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
              .return_type_name = std::string(export_info.return_type_name),
              .return_type_symbol = resolve_runtime_return_type_symbol(
                  r, export_info.return_type_name),
              .slot_info = {}});
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

std::optional<std::string> resolve_field_accessor_cxx(
    const TypeRegistry& r, const ClassInfo& owner,
    const std::vector<std::string>& path) {
  if (path.empty()) return std::nullopt;

  const FieldInfo* field = r.lookup_class_field(owner, path.front());
  if (!field) return std::nullopt;

  std::string out = r.field_cxx_name(path.front());
  const TypeExpr* current_type = field->type.get();
  for (size_t i = 1; i < path.size(); ++i) {
    const FieldInfo* nested = nullptr;
    std::string access = ".";
    const TypeDescriptor* descriptor = r.descriptor_for_type(current_type);
    if (!descriptor) return std::nullopt;
    if (const ClassInfo* nested_class = descriptor->class_info()) {
      access = nested_class->is_reference_type ? "->" : ".";
      nested = r.lookup_class_field(*nested_class, path[i]);
    } else if (const RecordInfo* nested_record = descriptor->record_info()) {
      auto fit = nested_record->fields.find(lc(path[i]));
      if (fit != nested_record->fields.end()) nested = &fit->second;
    }
    if (!nested) return std::nullopt;

    out += access + r.field_cxx_name(path[i]);
    current_type = nested->type.get();
  }
  return out;
}

PropertyAccessorInfo resolve_property_accessor(
    const TypeRegistry& r, const ClassInfo& owner,
    const PropertyDecl::Accessor& accessor) {
  std::vector<std::string> path = lower_path(accessor);
  if (path.empty()) return PropertyAccessorInfo{};

  if (auto cxx_path =
          resolve_field_accessor_cxx(r, owner, path)) {
    return PropertyAccessorInfo{.kind = PropertyAccessorKind::FieldPath,
                                .path = std::move(path),
                                .cxx_path = *cxx_path,
                                .method_name = {}};
  }

  if (path.size() == 1 &&
      r.lookup_class_methods(owner, path.front())) {
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

void resolve_property_accessors(TypeRegistry& r, ClassInfo& class_info,
                                const TyObject& type) {
  for (const auto& member : type.members) {
    if (member.kind != ObjectMemberKind::Property) continue;
    auto property = class_info.properties.find(lc(member.property.name));
    if (property == class_info.properties.end()) continue;
    property->second.read = resolve_property_accessor(
        r, class_info, member.property.read_accessor);
    property->second.write = resolve_property_accessor(
        r, class_info, member.property.write_accessor);
  }
}

void resolve_property_accessors_in_symbol(TypeRegistry& r,
                                          TypeSymbol& symbol) {
  if (symbol.descriptor && symbol.descriptor->symbol &&
      symbol.descriptor->symbol != &symbol) {
    return;
  }
  if (ClassInfo* ci = symbol.mutable_class_info()) {
    if (symbol.type && symbol.type->kind == Kind::TyObject) {
      resolve_property_accessors(
          r, *ci, static_cast<const TyObject&>(*symbol.type));
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

void resolve_class_parent(TypeRegistry& r, ClassInfo& class_info,
                          const TypeLookupContext* context, Location loc) {
  class_info.parent_symbol = nullptr;
  if (!class_info.parent.empty()) {
    const TypeSymbol* parent_symbol =
        context ? r.lookup_type_symbol_in_context(
                      pascal_key(class_info.parent), context)
                : nullptr;
    const TypeSymbol* payload =
        parent_symbol && parent_symbol->descriptor &&
                parent_symbol->descriptor->symbol
            ? parent_symbol->descriptor->symbol
            : parent_symbol;
    if (!payload || !payload->class_info()) {
      report_error(loc, "parent class `" + class_info.parent +
                            "` is not visible");
    } else {
      class_info.parent_symbol = payload;
    }
  }
}

void resolve_class_links(TypeRegistry& r, ClassInfo& class_info,
                         const TyObject& type,
                         const TypeLookupContext* context, Location loc) {
  resolve_class_parent(r, class_info, context, loc);
  class_info.interface_symbols.clear();
  for (const std::string& interface_name : type.interfaces) {
    const TypeSymbol* interface_symbol =
        context ? r.lookup_type_symbol_in_context(
                      pascal_key(interface_name), context)
                : nullptr;
    const TypeSymbol* payload =
        interface_symbol && interface_symbol->descriptor &&
                interface_symbol->descriptor->symbol
            ? interface_symbol->descriptor->symbol
            : interface_symbol;
    if (!payload || !payload->interface_info()) {
      report_error(loc, "implemented interface `" + interface_name +
                            "` is not a visible interface");
      continue;
    }
    class_info.interface_symbols.push_back(payload);
  }
}

void resolve_class_links_for_symbol(TypeRegistry& r, TypeSymbol& symbol) {
  if (symbol.descriptor && symbol.descriptor->symbol &&
      symbol.descriptor->symbol != &symbol) {
    return;
  }
  if (ClassInfo* ci = symbol.mutable_class_info()) {
    const TypeLookupContext* context =
        r.lookup_context_for_type(symbol.type);
    if (symbol.type && symbol.type->kind == Kind::TyObject) {
      resolve_class_links(
          r, *ci, static_cast<const TyObject&>(*symbol.type),
          context, symbol.type->loc);
    } else {
      // Synthetic runtime classes are ordinary semantic class objects even
      // though their owned placeholder syntax is a TyName. Bind their parent
      // edge here, once, just like a parsed class declaration.
      resolve_class_parent(r, *ci, context,
                           symbol.type ? symbol.type->loc : Location{});
    }
    refresh_class_enum_members(*ci);
  }
  if (auto* nested = nested_type_map_mut(symbol)) {
    for (auto& [_, child] : *nested) {
      if (child) resolve_class_links_for_symbol(r, *child);
    }
  }
}

void resolve_anonymous_descriptor_payload(
    TypeRegistry& r, const TypeDescriptor* descriptor,
    const TypeLookupContext* context) {
  if (!descriptor || descriptor->symbol || !descriptor->type) return;
  TypeDescriptor& mutable_descriptor =
      *const_cast<TypeDescriptor*>(descriptor);
  ClassInfo* class_info = mutable_descriptor.mutable_class_info();
  if (!class_info || descriptor->type->kind != Kind::TyObject) return;
  const auto& type = static_cast<const TyObject&>(*descriptor->type);
  resolve_class_links(r, *class_info, type, context, type.loc);
  resolve_property_accessors(r, *class_info, type);
  refresh_class_enum_members(*class_info);
}

void register_decl_list(TypeRegistry& r, const std::string& unit,
                        const std::vector<DeclPtr>& decls,
                        bool is_interface) {
  UnitInfo* ui = nullptr;
  {
    auto it = r.units.find(unit);
    if (it != r.units.end()) ui = &it->second;
  }
  std::unordered_map<std::string, const TypeDecl*> seen_types;
  for (const auto& d : decls) {
    if (!d) continue;
    switch (d->kind) {
      case Kind::TypeDecl: {
        const auto& td = static_cast<const TypeDecl&>(*d);
        if (!td.type) continue;
        std::string nm = lc(td.name);
        bool allow_forward_completion = false;
        bool duplicate = false;
        if (auto seen = seen_types.find(nm); seen != seen_types.end()) {
          const TypeDecl* prior = seen->second;
          allow_forward_completion =
              prior && is_forward_reference_class_type(prior->type.get()) &&
              is_complete_reference_class_type(td.type.get());
          duplicate = !allow_forward_completion;
        } else if (ui) {
          TypeSymbol* existing = find_unit_type_symbol(*ui, nm);
          allow_forward_completion =
              existing && existing->class_info() &&
              existing->class_info()->is_forward &&
              is_complete_reference_class_type(td.type.get());
          if (!allow_forward_completion &&
              ((!is_interface && ui->iface_types.count(nm)) ||
               (is_interface && ui->iface_types.count(nm)) ||
               (!is_interface && ui->impl_types.count(nm)))) {
            duplicate = true;
          }
        }
        if (!is_interface && ui && ui->iface_types.count(nm) &&
            !allow_forward_completion) {
          duplicate = true;
        }
        if (duplicate) {
          report_error(td.loc, "duplicate identifier `" + nm + "`");
          break;
        }
        TypeSymbol* symbol = upsert_unit_type_symbol(
            r, ui, is_interface,
            make_type_symbol_for_type(r, unit, nm, td.type),
            allow_forward_completion);
        td.symbol = symbol;
        seen_types[nm] = &td;
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
        if (ui) {
          const std::string name = lc(pd.name);
          if (ui->has_enum_member(name)) {
            report_error(pd.loc, "duplicate identifier `" + name + "`");
            break;
          }
          (is_interface ? ui->iface_procs : ui->impl_procs)[name].push_back(
              make_proc_info(unit, pd_sp));
        }
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        const VarInfo var{
            .defining_unit = unit, .type = vd.type, .loc = vd.loc};
        for (const auto& n : vd.names) {
          if (!ui) continue;
          const std::string name = lc(n);
          if (ui->find_var(name) || ui->find_const(name) ||
              ui->has_enum_member(name)) {
            report_error(vd.loc, "duplicate identifier `" + name + "`");
            continue;
          }
          insert_unit_storage_symbol(is_interface ? ui->iface_vars
                                                  : ui->impl_vars,
                                     name, var, vd.loc);
        }
        if (vd.type && !vd.names.empty()) {
          register_enums_in_type(r, ui, is_interface, unit, *vd.type,
                                 lc(vd.names.front()), nullptr);
        }
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        if (ui) {
          const std::string name = lc(cd.name);
          if (ui->find_var(name) || ui->find_const(name) ||
              ui->has_enum_member(name)) {
            report_error(cd.loc, "duplicate identifier `" + name + "`");
          } else {
            insert_unit_storage_symbol(is_interface ? ui->iface_consts
                                                    : ui->impl_consts,
                                       name,
                                       ConstInfo{.defining_unit = unit,
                                                 .type = cd.type,
                                                 .value = cd.value,
                                                 .loc = cd.loc},
                                       cd.loc);
          }
        }
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
                                    const UnitInfo* unit_info,
                                    bool restrict_unit_type_lookup = false,
                                    bool preserve_local_value_scope = false) {
  r.type_lookup_context_storage.push_back(
      TypeLookupContext{.unit = lc(std::string(unit)),
                        .parent = parent,
                        .kind = kind,
                        .unit_info = unit_info,
                        .restrict_unit_type_lookup =
                            restrict_unit_type_lookup,
                        .preserve_local_value_scope =
                            preserve_local_value_scope,
                        .type_symbols = {},
                        .enum_members = {},
                        .const_symbols = {}});
  return &r.type_lookup_context_storage.back();
}

TypeLookupContext* make_type_lookup_context(
    TypeRegistry& r, std::string_view unit, const TypeLookupContext* parent) {
  return make_scope_frame(r, unit, parent, ScopeFrameKind::Local, nullptr);
}

TypeLookupContext* make_proc_body_type_context(
    TypeRegistry& r, std::string_view unit, const TypeLookupContext* parent) {
  return make_scope_frame(r, unit, parent, ScopeFrameKind::Local, nullptr,
                          /*restrict_unit_type_lookup=*/false,
                          /*preserve_local_value_scope=*/true);
}

void insert_type_ref(TypeLookupContext& context, const TypeSymbol* symbol) {
  if (!symbol) return;
  context.type_symbols.insert_or_assign(symbol->name, symbol);
}

void report_enum_member_duplicates(const TyEnum& type) {
  std::unordered_set<std::string> members;
  for (const auto& member : type.members) {
    if (!members.insert(member.name).second) {
      report_error(type.loc, "duplicate identifier `" + member.name + "`");
    }
  }
}

void report_local_enum_duplicates(const TypeExpr* type) {
  if (!type || type->kind != Kind::TyEnum) return;
  report_enum_member_duplicates(static_cast<const TyEnum&>(*type));
}

TypeSymbol* register_local_type_decl_symbol(TypeRegistry& r,
                                            const TypeDecl& td) {
  if (!td.type) return nullptr;
  if (td.symbol) return const_cast<TypeSymbol*>(td.symbol);
  r.type_symbols.push_back(
      make_type_symbol_for_type(r, {}, td.name, td.type));
  TypeSymbol* stored = &r.type_symbols.back();
  if (stored->descriptor) {
    bind_symbol_descriptor(*stored, stored->descriptor);
  }
  report_local_enum_duplicates(td.type.get());
  populate_nested_types(r, *stored);
  td.symbol = stored;
  return stored;
}

void register_context_enum_symbol(TypeRegistry& r, TypeLookupContext& context,
                                  std::string_view name,
                                  std::string_view cxx_name,
                                  const TyEnum& type) {
  report_enum_member_duplicates(type);
  EnumInfoReg* info = ensure_enum_descriptor_info(
      r, type, context.unit, lc(std::string(name)), std::string(cxx_name));
  for (size_t ordinal = 0; ordinal < info->members.size(); ++ordinal) {
    context.enum_members.try_emplace(
        info->members[ordinal],
        EnumMemberInfo{info, static_cast<int64_t>(ordinal)});
  }
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

const TypeSymbol* ensure_type_decl_symbol(TypeRegistry& r,
                                          const TypeDecl& td) {
  if (td.symbol) return td.symbol;
  return register_local_type_decl_symbol(r, td);
}

const TypeLookupContext* push_visible_type_decl_frame(
    TypeRegistry& r, std::string_view unit, const TypeLookupContext* parent,
    ScopeFrameKind kind, const UnitInfo* unit_info,
    const TypeSymbol* symbol, bool preserve_local_value_scope = false) {
  if (!symbol) return parent;
  const bool restrict_unit_types =
      kind == ScopeFrameKind::UnitInterface ||
      kind == ScopeFrameKind::UnitImplementation;
  TypeLookupContext* frame =
      make_scope_frame(r, unit, parent, kind, unit_info,
                       restrict_unit_types, preserve_local_value_scope);
  insert_type_ref(*frame, symbol);
  return frame;
}

const TypeLookupContext* make_unit_import_context(TypeRegistry& r,
                                                  std::string_view unit,
                                                  bool is_interface) {
  auto uit = r.units.find(lc(std::string(unit)));
  if (uit == r.units.end()) return nullptr;

  const TypeLookupContext* chain = nullptr;
  if (auto builtin = r.units.find("__builtin__");
      builtin != r.units.end()) {
    chain = make_scope_frame(r, "__builtin__", nullptr,
                             ScopeFrameKind::ImportedUnitInterface,
                             &builtin->second);
  }
  if (auto rt = r.units.find("__rt__"); rt != r.units.end()) {
    chain = make_scope_frame(r, "__rt__", chain,
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
  return chain;
}

const TypeLookupContext* make_type_member_context(
    TypeRegistry& r, const TypeSymbol* symbol,
    const TypeLookupContext* parent, bool include_nested_types = true) {
  if (!symbol) return parent;
  // Member declarations are looked up in the enclosing type's lexical scope
  // first, then in the unit/import scope represented by the parent context.
  TypeLookupContext* context =
      make_type_lookup_context(r, symbol->defining_unit, parent);
  if (include_nested_types) {
    const auto* nested = nested_type_map(*symbol);
    if (!nested) return context;
    for (const auto& [_, child] : *nested) {
      insert_type_ref(*context, child.get());
    }
  }
  return context;
}

const TypeSymbol* canonical_method_owner_symbol(const TypeSymbol* symbol) {
  return symbol && symbol->descriptor && symbol->descriptor->symbol
             ? symbol->descriptor->symbol
             : symbol;
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
  td.type_context = context;
  index_type_expr_context(r, td.type.get(), context, symbol);
}

const TypeLookupContext* push_nested_type_decl_context(
    TypeRegistry& r, const TypeSymbol* owner_symbol,
    const TypeLookupContext* parent, const TypeDecl& td) {
  return push_visible_type_decl_frame(
      r, owner_symbol ? owner_symbol->defining_unit : std::string_view{},
      parent, ScopeFrameKind::Local, nullptr,
      nested_type_symbol_for_decl(owner_symbol, td));
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
        member.method->signature_type_context = context;
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
  if (!type->type_context) type->type_context = context;
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
          make_type_member_context(r, symbol, context,
                                   /*include_nested_types=*/false);
      if (!rec.member_order.empty()) {
        for (const RecordMember& member : rec.member_order) {
          switch (member.kind) {
            case RecordMemberKind::Field:
              if (member.index < rec.fields.size()) {
                index_type_expr_context(
                    r, rec.fields[member.index].type.get(), member_context);
              }
              break;
            case RecordMemberKind::Type:
              if (member.index < rec.nested_types.size() &&
                  rec.nested_types[member.index]) {
                const auto& nested = *rec.nested_types[member.index];
                index_type_decl_context(
                    r, nested, member_context,
                    nested_type_symbol_for_decl(symbol, nested));
                member_context = push_nested_type_decl_context(
                    r, symbol, member_context, nested);
              }
              break;
          }
        }
      } else {
        for (const auto& field : rec.fields) {
          index_type_expr_context(r, field.type.get(), member_context);
        }
        for (const auto& nested : rec.nested_types) {
          if (!nested) continue;
          index_type_decl_context(
              r, *nested, member_context,
              nested_type_symbol_for_decl(symbol, *nested));
          member_context = push_nested_type_decl_context(
              r, symbol, member_context, *nested);
        }
      }
      index_variant_type_contexts(r, rec.variant_part, member_context);
      break;
    }
    case Kind::TyObject: {
      const auto& obj = static_cast<const TyObject&>(*type);
      const TypeLookupContext* member_context =
          make_type_member_context(r, symbol, context,
                                   /*include_nested_types=*/false);
      for (const auto& member : obj.members) {
        index_object_member_type_context(r, member, member_context, symbol);
        if (member.kind == ObjectMemberKind::Type && member.type_decl) {
          member_context = push_nested_type_decl_context(
              r, symbol, member_context, *member.type_decl);
        }
      }
      break;
    }
    case Kind::TyInterface: {
      const auto& intf = static_cast<const TyInterface&>(*type);
      const TypeLookupContext* member_context =
          make_type_member_context(r, symbol, context,
                                   /*include_nested_types=*/false);
      for (const auto& member : intf.members) {
        index_object_member_type_context(r, member, member_context, symbol);
        if (member.kind == ObjectMemberKind::Type && member.type_decl) {
          member_context = push_nested_type_decl_context(
              r, symbol, member_context, *member.type_decl);
        }
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

const TypeLookupContext* index_decl_list_contexts(
    TypeRegistry& r, const std::string& unit,
    const std::vector<DeclPtr>& decls, const TypeLookupContext* context,
    ScopeFrameKind type_frame_kind = ScopeFrameKind::Local,
    const UnitInfo* unit_info = nullptr,
    bool preserve_local_value_scope = false,
    const TypeLookupContext* value_context = nullptr) {
  const TypeLookupContext* current = context;
  if (!value_context) value_context = context;
  const bool check_local_type_duplicates =
      type_frame_kind == ScopeFrameKind::Local && unit_info == nullptr;
  std::unordered_map<std::string, const TypeDecl*> local_type_decls;
  std::unordered_set<std::string> local_enum_members;
  std::unordered_set<std::string> local_value_names;
  auto note_local_value_name = [&](const std::string& name, Location loc) {
    if (!check_local_type_duplicates || name.empty()) return;
    if (local_enum_members.count(name)) {
      report_error(loc, "duplicate identifier `" + name + "`");
    }
    local_value_names.insert(name);
  };
  auto check_local_enum_member_duplicates = [&](const TypePtr& type) {
    if (!check_local_type_duplicates || !type) return;
    for (const TyEnum* te : collect_enum_types(*type)) {
      if (!te) continue;
      std::unordered_set<std::string> members_in_enum;
      for (const auto& member : te->members) {
        const std::string name = lc(member.name);
        if (!members_in_enum.insert(name).second) continue;
        if (!local_value_names.insert(name).second) {
          // Enum labels are value declarations in the current Pascal lexical
          // scope. A same-scope var/const/proc or another enum label cannot
          // coexist with the label; otherwise the build frame would model an
          // invalid declaration order as ordinary shadowing.
          report_error(te->loc, "duplicate identifier `" + name + "`");
        }
        local_enum_members.insert(name);
      }
    }
  };
  for (size_t i = 0; i < decls.size();) {
    const auto& d = decls[i];
    if (!d) {
      ++i;
      continue;
    }
    if (d->kind == Kind::TypeDecl) {
      const size_t section_id =
          static_cast<const TypeDecl&>(*d).type_section_id;
      size_t end = i + 1;
      while (end < decls.size() && decls[end] &&
             decls[end]->kind == Kind::TypeDecl &&
             static_cast<const TypeDecl&>(*decls[end]).type_section_id ==
                 section_id) {
        ++end;
      }
      std::vector<bool> skip(end - i, false);
      for (size_t j = i; j < end; ++j) {
        const auto& td = static_cast<const TypeDecl&>(*decls[j]);
        if (check_local_type_duplicates) {
          const std::string name = lc(td.name);
          if (auto prior = local_type_decls.find(name);
              prior != local_type_decls.end()) {
            const bool completes_forward_class =
                prior->second &&
                is_forward_reference_class_type(prior->second->type.get()) &&
                is_complete_reference_class_type(td.type.get());
            if (!completes_forward_class) {
              // A local type frame models one Pascal lexical type scope. A
              // duplicate must not push another frame and shadow the binding
              // already visible to earlier declarations in that scope.
              report_error(td.loc, "duplicate identifier `" + name + "`");
              if (const TypeSymbol* prior_symbol = prior->second->symbol) {
                td.symbol = prior_symbol;
              }
              skip[j - i] = true;
              continue;
            }
          }
          local_type_decls[name] = &td;
        }
        check_local_enum_member_duplicates(td.type);
        ensure_type_decl_symbol(r, td);
      }
      for (size_t j = i; j < end; ++j) {
        if (skip[j - i]) continue;
        const auto& td = static_cast<const TypeDecl&>(*decls[j]);
        const TypeSymbol* symbol = td.symbol;
        current = push_visible_type_decl_frame(
            r, unit, current, type_frame_kind, unit_info, symbol,
            preserve_local_value_scope);
        decls[j]->type_context = current;
        index_type_decl_context(r, td, current, symbol);
        if (td.type) {
          register_context_enum_symbols_for_owner(
              r, *const_cast<TypeLookupContext*>(current), td.type, td.name,
              td.type->kind == Kind::TyEnum
                  ? static_cast<const TyEnum*>(td.type.get())
                  : nullptr);
        }
      }
      i = end;
      continue;
    }
    d->type_context = current;
    switch (d->kind) {
      case Kind::ProcDecl: {
        const auto& pd = static_cast<const ProcDecl&>(*d);
        if (pd.of_type.empty() && !pd.is_operator) {
          note_local_value_name(lc(pd.name), pd.loc);
        }
        const TypeLookupContext* proc_context = current;
        const TypeLookupContext* body_parent_context =
            preserve_local_value_scope ? current : value_context;
        const TypeSymbol* method_owner = nullptr;
        if (!pd.of_type.empty()) {
          method_owner =
              r.lookup_type_symbol_in_context(pascal_key(pd.of_type),
                                              current);
          if (method_owner) {
            // A method implementation may be written on a type alias, but the
            // method belongs to the aliased class/record/interface. Use that
            // owner for both signature and body scopes so nested type names
            // bind to the same owner that emission will define on.
            method_owner = canonical_method_owner_symbol(method_owner);
            pd.method_owner_symbol = method_owner;
            proc_context = make_type_member_context(r, method_owner, current);
            body_parent_context =
                make_type_member_context(r, method_owner, body_parent_context);
          }
        }
        pd.signature_type_context = proc_context;
        index_proc_signature_context(r, pd, proc_context);
        TypeLookupContext* body_context =
            make_proc_body_type_context(r, unit, body_parent_context);
        const TypeLookupContext* body_final = index_decl_list_contexts(
            r, unit, pd.locals, body_context, ScopeFrameKind::Local, nullptr,
            /*preserve_local_value_scope=*/true);
        pd.body_type_context = body_final;
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        for (const auto& name : vd.names) {
          note_local_value_name(lc(name), vd.loc);
        }
        check_local_enum_member_duplicates(vd.type);
        if (!vd.names.empty()) {
          current = make_scope_frame(r, unit, current, type_frame_kind,
                                     unit_info,
                                     type_frame_kind != ScopeFrameKind::Local,
                                     preserve_local_value_scope);
          register_context_enum_symbols_for_owner(
              r, *const_cast<TypeLookupContext*>(current), vd.type,
              vd.names.front());
        }
        index_type_expr_context(r, vd.type.get(), d->type_context);
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        note_local_value_name(lc(cd.name), cd.loc);
        check_local_enum_member_duplicates(cd.type);
        current = make_scope_frame(r, unit, current, type_frame_kind,
                                   unit_info,
                                   type_frame_kind != ScopeFrameKind::Local,
                                   preserve_local_value_scope);
        const std::string name = lc(cd.name);
        // Ordered declaration contexts must not use the unit's complete const
        // table: type bounds and enum ordinals may only see constants already
        // reached in source order.
        const_cast<TypeLookupContext*>(current)->const_symbols[name] =
            ConstInfo{.defining_unit = unit, .type = cd.type,
                      .value = cd.value, .loc = cd.loc};
        register_context_enum_symbols_for_owner(
            r, *const_cast<TypeLookupContext*>(current), cd.type, cd.name);
        index_type_expr_context(r, cd.type.get(), d->type_context);
        break;
      }
      default:
        break;
    }
    ++i;
  }
  return current;
}

void bind_exception_handler_type(TypeRegistry& r, const ExceptHandler& handler,
                                 const TypeLookupContext* context,
                                 Location loc) {
  const TypeSymbol* symbol = nullptr;
  if (handler.class_name.empty()) {
    symbol = r.lookup_type_symbol_exact(pascal_key("__rt__"),
                                        pascal_key("exception"));
  } else if (context) {
    symbol = r.lookup_type_symbol_in_context(pascal_key(handler.class_name),
                                             context);
  }
  if (!symbol) {
    if (handler.class_name.empty()) {
      report_error(loc, "runtime Exception type is not registered");
    } else {
      report_error(loc, "unresolved exception handler type `" +
                            handler.class_name + "`");
    }
  } else {
    const TypeSymbol* payload =
        symbol && symbol->descriptor && symbol->descriptor->symbol
            ? symbol->descriptor->symbol
            : symbol;
    const ClassInfo* class_info = payload ? payload->class_info() : nullptr;
    if (!class_info || !class_info->is_reference_type) {
      report_error(loc, "exception handler type `" +
                            (handler.class_name.empty()
                                 ? std::string("Exception")
                                 : handler.class_name) +
                            "` is not a reference class");
      symbol = nullptr;
    }
  }
  handler.class_symbol = symbol;
  handler.class_binding_complete = true;
}

std::optional<std::string> type_name_path_from_expr(const Expr& expr) {
  if (expr.kind == Kind::Ident) {
    return static_cast<const Ident&>(expr).name;
  }
  if (expr.kind != Kind::Member) return std::nullopt;
  const auto& member = static_cast<const Member&>(expr);
  if (!member.base) return std::nullopt;
  std::optional<std::string> base = type_name_path_from_expr(*member.base);
  if (!base) return std::nullopt;
  return *base + "." + member.name;
}

void bind_type_name_expr_if_visible(TypeRegistry& r, const Expr& expr,
                                    const TypeLookupContext* context,
                                    bool report_missing) {
  if (expr.kind == Kind::Deref) {
    const auto& deref = static_cast<const Deref&>(expr);
    if (deref.operand) {
      bind_type_name_expr_if_visible(r, *deref.operand, context,
                                     report_missing);
    }
    return;
  }
  std::optional<std::string> path = type_name_path_from_expr(expr);
  if (!path) return;
  const TypeSymbol* symbol = nullptr;
  if (context) {
    symbol = r.lookup_type_symbol_in_context(pascal_key(*path), context);
  }
  if (!symbol && !report_missing) return;
  if (!symbol) {
    report_error(expr.loc, "unresolved type `" + *path + "`");
  }
  expr.type_operand_symbol = symbol;
  expr.type_operand_bound = true;
}

bool value_name_visible_in_context(const TypeLookupContext* context,
                                   PascalKey name) {
  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    if (scope_frame_find_var(*frame, pascal_key_string(name)) ||
        scope_frame_find_const(*frame, pascal_key_string(name)) ||
        scope_frame_find_procs(*frame, pascal_key_string(name)) ||
        scope_frame_has_enum_member(*frame, pascal_key_string(name))) {
      return true;
    }
  }
  return false;
}

bool root_value_name_visible_in_context(const TypeLookupContext* context,
                                        PascalKey path) {
  std::string key = pascal_key_string(path);
  if (const size_t dot = key.find('.'); dot != std::string::npos) {
    key.resize(dot);
  }
  return value_name_visible_in_context(context, pascal_key(key));
}

void bind_value_type_expr_if_visible(
    TypeRegistry& r, const Expr& expr, const TypeLookupContext* context) {
  std::optional<std::string> path = type_name_path_from_expr(expr);
  if (!path) return;
  if (root_value_name_visible_in_context(context, pascal_key(*path))) return;
  const TypeSymbol* symbol = nullptr;
  if (context) {
    symbol = r.lookup_type_symbol_in_context(pascal_key(*path), context);
  }
  if (!symbol) return;
  expr.type_value_symbol = symbol;
  expr.type_value_bound = true;
}

void bind_expr_list_semantics(TypeRegistry& r, const std::vector<ExprPtr>& exprs,
                              const TypeLookupContext* context) {
  for (const ExprPtr& expr : exprs) {
    bind_expr_semantics(r, expr, context);
  }
}

bool call_first_arg_can_be_type_operand(const Call& call,
                                        ExprSemanticUse use) {
  if (!call.callee) return false;
  std::string_view name;
  if (call.callee->kind == Kind::Ident) {
    name = static_cast<const Ident&>(*call.callee).name;
  } else if (call.callee->kind == Kind::Member) {
    const auto& member = static_cast<const Member&>(*call.callee);
    if (!member.base || member.base->kind != Kind::Ident ||
        static_cast<const Ident&>(*member.base).name != "system") {
      return false;
    }
    name = member.name;
  } else {
    return false;
  }
  if (name == "new") {
    // Pascal statement-form `new(p)` allocates through an existing pointer
    // slot. Expression-form `new(T, ...)` uses a type operand and produces the
    // allocated pointer value, so only expression use binds the first argument
    // in the type namespace.
    return use == ExprSemanticUse::Value && !call.args.empty();
  }
  if (call.args.size() != 1) return false;
  return name == "sizeof" || name == "low" || name == "high";
}

void bind_builtin_expression_result(TypeRegistry& registry, const Expr& expr,
                                    std::string_view name) {
  expr.result_descriptor = builtin_descriptor(registry, name);
}

void bind_integer_literal_result(TypeRegistry& registry,
                                 const IntLit& literal) {
  std::string_view type_name;
  if (literal.value > static_cast<uint64_t>(INT64_MAX)) {
    type_name = "qword";
  } else if (literal.value <= 127) {
    type_name = "shortint";
  } else if (literal.value <= 255) {
    type_name = "byte";
  } else if (literal.value <= 32767) {
    type_name = "smallint";
  } else if (literal.value <= 65535) {
    type_name = "word";
  } else if (literal.value <= static_cast<uint64_t>(INT32_MAX)) {
    type_name = "longint";
  } else if (literal.value <= UINT32_MAX) {
    type_name = "longword";
  } else {
    type_name = "int64";
  }
  bind_builtin_expression_result(registry, literal, type_name);
}

std::string_view semantic_intrinsic_name(const Expr* callee) {
  if (!callee) return {};
  if (callee->kind == Kind::Ident) {
    return static_cast<const Ident&>(*callee).name;
  }
  if (callee->kind != Kind::Member) return {};
  const auto& member = static_cast<const Member&>(*callee);
  if (!member.base || member.base->kind != Kind::Ident ||
      static_cast<const Ident&>(*member.base).name != "system") {
    return {};
  }
  return member.name;
}

void bind_syntax_expression_result(TypeRegistry& registry, const Expr& expr) {
  switch (expr.kind) {
    case Kind::BoolLit:
      bind_builtin_expression_result(registry, expr, "boolean");
      return;
    case Kind::RealLit:
      bind_builtin_expression_result(registry, expr, "real");
      return;
    case Kind::IntLit:
      bind_integer_literal_result(registry,
                                  static_cast<const IntLit&>(expr));
      return;
    case Kind::StringLit:
      bind_builtin_expression_result(
          registry, expr,
          static_cast<const StringLit&>(expr).value.size() == 1
              ? std::string_view("char")
              : std::string_view("shortstring"));
      return;
    case Kind::Binary: {
      const auto& binary = static_cast<const Binary&>(expr);
      if (binary.op == BinOp::As && binary.rhs &&
          binary.rhs->type_operand_symbol) {
        expr.result_descriptor =
            binary.rhs->type_operand_symbol->descriptor;
      } else if (binary.op == BinOp::Is || binary.op == BinOp::In ||
                 binary.op == BinOp::Eq || binary.op == BinOp::NotEq ||
                 binary.op == BinOp::Lt || binary.op == BinOp::Gt ||
                 binary.op == BinOp::LtEq || binary.op == BinOp::GtEq) {
        bind_builtin_expression_result(registry, expr, "boolean");
      }
      return;
    }
    case Kind::Call: {
      const auto& call = static_cast<const Call&>(expr);
      if (call.args.size() == 1 && call.callee &&
          call.callee->type_operand_symbol) {
        expr.result_descriptor =
            call.callee->type_operand_symbol->descriptor;
        return;
      }
      const std::string_view intrinsic = semantic_intrinsic_name(call.callee.get());
      if (intrinsic == "chr" && call.args.size() == 1) {
        bind_builtin_expression_result(registry, expr, "char");
      } else if (intrinsic == "sizeof" && call.args.size() == 1) {
        bind_builtin_expression_result(registry, expr, "longint");
      }
      return;
    }
    default:
      return;
  }
}

void bind_expr_semantics(TypeRegistry& r, const ExprPtr& expr,
                         const TypeLookupContext* context,
                         ExprSemanticUse use) {
  if (!expr) return;
  switch (expr->kind) {
    case Kind::Ident:
      if (use == ExprSemanticUse::Value) {
        bind_value_type_expr_if_visible(r, *expr, context);
      }
      break;
    case Kind::Binary: {
      const auto& e = static_cast<const Binary&>(*expr);
      if ((e.op == BinOp::Is || e.op == BinOp::As) && e.rhs) {
        // The right side of Pascal `is`/`as` is a type operand, not a value.
        // Bind it here so code generation can emit the class test/cast from
        // type identity instead of re-querying a name.
        bind_type_name_expr_if_visible(r, *e.rhs, context,
                                       /*report_missing=*/true);
      }
      bind_expr_semantics(r, e.lhs, context);
      bind_expr_semantics(r, e.rhs, context);
      break;
    }
    case Kind::Unary:
      bind_expr_semantics(r, static_cast<const Unary&>(*expr).operand,
                          context);
      break;
    case Kind::Call: {
      const auto& e = static_cast<const Call&>(*expr);
      if (call_first_arg_can_be_type_operand(e, use)) {
        bind_type_name_expr_if_visible(r, *e.args[0], context,
                                       /*report_missing=*/false);
      }
      if (e.args.size() == 1 && e.callee) {
        // The parser represents Pascal typecasts as one-argument calls. Bind a
        // visible type-name callee during build so later overload and value
        // emission can distinguish `T(x)` from an ordinary call without
        // re-running lexical type lookup.
        bind_type_name_expr_if_visible(r, *e.callee, context,
                                       /*report_missing=*/false);
      }
      bind_expr_semantics(r, e.callee, context);
      bind_expr_list_semantics(r, e.args, context);
      bind_expr_list_semantics(r, e.width, context);
      bind_expr_list_semantics(r, e.precision, context);
      break;
    }
    case Kind::Index: {
      const auto& e = static_cast<const Index&>(*expr);
      bind_expr_semantics(r, e.base, context);
      bind_expr_list_semantics(r, e.indices, context);
      break;
    }
    case Kind::Member:
      if (use == ExprSemanticUse::Value) {
        bind_value_type_expr_if_visible(r, *expr, context);
      }
      bind_expr_semantics(r, static_cast<const Member&>(*expr).base, context);
      break;
    case Kind::Deref:
      bind_expr_semantics(r, static_cast<const Deref&>(*expr).operand,
                          context);
      break;
    case Kind::AddrOf:
      bind_expr_semantics(r, static_cast<const AddrOf&>(*expr).operand,
                          context);
      break;
    case Kind::SetLit:
      bind_expr_list_semantics(r, static_cast<const SetLit&>(*expr).elements,
                               context);
      break;
    case Kind::Range: {
      const auto& e = static_cast<const Range&>(*expr);
      bind_expr_semantics(r, e.lo, context);
      bind_expr_semantics(r, e.hi, context);
      break;
    }
    case Kind::ArrayConst:
      bind_expr_list_semantics(
          r, static_cast<const ArrayConst&>(*expr).elements, context);
      break;
    case Kind::RecordConst:
      for (const auto& [_, value] :
           static_cast<const RecordConst&>(*expr).fields) {
        bind_expr_semantics(r, value, context);
      }
      break;
    default:
      break;
  }
  bind_syntax_expression_result(r, *expr);
}

void bind_stmt_list_semantics(TypeRegistry& r, const std::vector<StmtPtr>& stmts,
                              const TypeLookupContext* context) {
  for (const StmtPtr& stmt : stmts) {
    bind_stmt_semantics(r, stmt, context);
  }
}

void bind_stmt_semantics(TypeRegistry& r, const StmtPtr& stmt,
                         const TypeLookupContext* context) {
  if (!stmt) return;
  switch (stmt->kind) {
    case Kind::Compound:
      bind_stmt_list_semantics(r, static_cast<const Compound&>(*stmt).body,
                               context);
      break;
    case Kind::Assign: {
      const auto& s = static_cast<const Assign&>(*stmt);
      bind_expr_semantics(r, s.target, context);
      bind_expr_semantics(r, s.value, context);
      break;
    }
    case Kind::ExprStmt:
      bind_expr_semantics(r, static_cast<const ExprStmt&>(*stmt).expr,
                          context, ExprSemanticUse::Statement);
      break;
    case Kind::If: {
      const auto& s = static_cast<const If&>(*stmt);
      bind_expr_semantics(r, s.cond, context);
      bind_stmt_semantics(r, s.then_branch, context);
      bind_stmt_semantics(r, s.else_branch, context);
      break;
    }
    case Kind::While: {
      const auto& s = static_cast<const While&>(*stmt);
      bind_expr_semantics(r, s.cond, context);
      bind_stmt_semantics(r, s.body, context);
      break;
    }
    case Kind::Repeat: {
      const auto& s = static_cast<const Repeat&>(*stmt);
      bind_stmt_list_semantics(r, s.body, context);
      bind_expr_semantics(r, s.cond, context);
      break;
    }
    case Kind::For: {
      const auto& s = static_cast<const For&>(*stmt);
      bind_expr_semantics(r, s.from, context);
      bind_expr_semantics(r, s.to, context);
      bind_expr_semantics(r, s.in_expr, context);
      bind_stmt_semantics(r, s.body, context);
      break;
    }
    case Kind::CaseStmt: {
      const auto& s = static_cast<const CaseStmt&>(*stmt);
      bind_expr_semantics(r, s.selector, context);
      for (const CaseArm& arm : s.arms) {
        bind_expr_list_semantics(r, arm.labels, context);
        bind_stmt_semantics(r, arm.body, context);
      }
      bind_stmt_semantics(r, s.else_branch, context);
      break;
    }
    case Kind::With: {
      const auto& s = static_cast<const With&>(*stmt);
      bind_expr_list_semantics(r, s.exprs, context);
      bind_stmt_semantics(r, s.body, context);
      break;
    }
    case Kind::Labeled:
      bind_stmt_semantics(r, static_cast<const Labeled&>(*stmt).body, context);
      break;
    case Kind::ExitStmt:
      bind_expr_semantics(r, static_cast<const ExitStmt&>(*stmt).value,
                          context);
      break;
    case Kind::Try: {
      const auto& s = static_cast<const Try&>(*stmt);
      bind_stmt_list_semantics(r, s.body, context);
      if (s.is_finally) {
        bind_stmt_list_semantics(r, s.finally_body, context);
        break;
      }
      for (const ExceptHandler& handler : s.handlers) {
        bind_exception_handler_type(r, handler, context, s.loc);
        bind_stmt_semantics(r, handler.body, context);
      }
      bind_stmt_semantics(r, s.except_else, context);
      break;
    }
    case Kind::Raise:
      bind_expr_semantics(r, static_cast<const Raise&>(*stmt).value, context);
      break;
    default:
      break;
  }
}

void index_proc_decl_runtime_context(TypeRegistry& r, const ProcDecl* proc,
                                     const TypeLookupContext* context) {
  if (!proc) return;
  proc->signature_type_context = context;
  index_proc_signature_context(r, *proc, context);
}

void index_runtime_method_contexts(TypeRegistry& r, const MethodSig& method,
                                   const TypeLookupContext* context) {
  index_proc_decl_runtime_context(r, method.decl.get(), context);
}

void index_runtime_class_contexts(TypeRegistry& r, const ClassInfo& info,
                                  const TypeLookupContext* context) {
  for (const auto& [_, field] : info.fields) {
    index_type_expr_context(r, field.type.get(), context);
  }
  for (const auto& [_, prop] : info.properties) {
    index_param_type_contexts(r, prop.params, context);
    index_type_expr_context(r, prop.type.get(), context);
  }
  for (const auto& [_, methods] : info.methods) {
    for (const MethodSig& method : methods) {
      index_runtime_method_contexts(r, method, context);
    }
  }
}

void index_runtime_unit_contexts(TypeRegistry& r, const UnitInfo& unit,
                                 TypeLookupContext* context) {
  for (const auto& [_, symbol] : unit.iface_types) {
    if (!symbol) continue;
    index_type_expr_context(r, symbol->type, context, symbol);
    if (const ClassInfo* info = symbol->class_info()) {
      index_runtime_class_contexts(r, *info, context);
    }
  }
  for (const auto& [_, var] : unit.iface_vars) {
    index_type_expr_context(r, var.type.get(), context);
  }
  for (const auto& [_, cnst] : unit.iface_consts) {
    index_type_expr_context(r, cnst.type.get(), context);
  }
  for (const auto& [_, procs] : unit.iface_procs) {
    for (const ProcInfo& proc : procs) {
      index_proc_decl_runtime_context(r, proc.decl.get(), context);
    }
  }
  for (const auto& [_, ops] : unit.iface_operators) {
    for (const ProcInfo& op : ops) {
      index_proc_decl_runtime_context(r, op.decl.get(), context);
    }
  }
}

void resolve_proc_decl_signature(TypeDescriptorResolver& resolver,
                                 const ProcDecl* proc) {
  if (!proc) return;
  resolver.resolve_params(proc->params);
  resolver.resolve_type(proc->return_type.get(), /*pointer_target=*/false);
}

void resolve_runtime_class_descriptors(TypeDescriptorResolver& resolver,
                                       const ClassInfo& info) {
  for (const auto& [_, field] : info.fields) {
    resolver.resolve_type(field.type.get(), /*pointer_target=*/false);
  }
  for (const auto& [_, prop] : info.properties) {
    resolver.resolve_params(prop.params);
    resolver.resolve_type(prop.type.get(), /*pointer_target=*/false);
  }
  for (const auto& [_, methods] : info.methods) {
    for (const MethodSig& method : methods) {
      resolve_proc_decl_signature(resolver, method.decl.get());
    }
  }
}

void resolve_runtime_unit_descriptors(TypeRegistry& r, const UnitInfo& unit,
                                      const TypeLookupContext* context) {
  TypeDescriptorResolver resolver(r, context);
  for (const auto& [_, symbol] : unit.iface_types) {
    if (!symbol) continue;
    resolver.resolve_symbol_declaration(symbol);
    if (const ClassInfo* info = symbol->class_info()) {
      resolve_runtime_class_descriptors(resolver, *info);
    }
  }
  for (const auto& [_, var] : unit.iface_vars) {
    resolver.resolve_type(var.type.get(), /*pointer_target=*/false);
  }
  for (const auto& [_, cnst] : unit.iface_consts) {
    resolver.resolve_type(cnst.type.get(), /*pointer_target=*/false);
    bind_expr_semantics(
        r, std::const_pointer_cast<Expr>(cnst.value), context);
  }
  for (const auto& [_, procs] : unit.iface_procs) {
    for (const ProcInfo& proc : procs) {
      resolve_proc_decl_signature(resolver, proc.decl.get());
    }
  }
  for (const auto& [_, ops] : unit.iface_operators) {
    for (const ProcInfo& op : ops) {
      resolve_proc_decl_signature(resolver, op.decl.get());
    }
  }
}

Location type_value_collision_location(const UnitInfo& ui,
                                       const std::string& name) {
  if (const VarInfo* var = ui.find_var(name)) return var->loc;
  if (const ConstInfo* cnst = ui.find_const(name)) return cnst->loc;
  if (const std::vector<ProcInfo>* procs = ui.find_procs(name)) {
    for (const ProcInfo& proc : *procs) {
      if (proc.decl) return proc.decl->loc;
    }
  }
  if (const TypeSymbol* type = ui.find_type(name)) {
    if (type->type) return type->type->loc;
  }
  return {};
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
  for (const auto& [name, _] : ui.iface_enum_members) {
    value_names.insert(name);
  }
  for (const auto& [name, _] : ui.impl_enum_members) {
    value_names.insert(name);
  }

  for (const auto& name : type_names) {
    if (value_names.count(name)) {
      report_error(type_value_collision_location(ui, name),
                   "duplicate identifier `" + name + "`");
    }
  }
}

}  // namespace

bool TypeRegistry::bound_signature_type_exprs_match(const TypeExpr* a,
                                                    const TypeExpr* b) const {
  return tp2cc::bound_signature_type_exprs_match(*this, a, b);
}

bool TypeRegistry::bound_signature_params_match(
    const std::vector<Param>& a, const std::vector<Param>& b) const {
  return tp2cc::bound_signature_params_match(*this, a, b);
}

bool PropertyAccessorInfo::empty() const {
  return kind == PropertyAccessorKind::None;
}

std::string PropertyAccessorInfo::display_name() const {
  if (kind == PropertyAccessorKind::Method) return method_name;
  return join_path(path);
}

TypeSymbol make_type_symbol_for_type(
    TypeRegistry& registry, std::string_view unit, std::string_view name,
    std::shared_ptr<const TypeExpr> type) {
  return make_type_symbol_for_type_with_owner(
      registry, unit, name, std::move(type), std::vector<std::string>{});
}

TypeSymbol make_enum_type_symbol(TypeRegistry& registry,
                                 std::string_view unit, std::string_view name,
                                 std::string_view cxx_name,
                                 const TyEnum& type) {
  const std::string low_name = lc(std::string(name));
  const std::string low_unit = lc(std::string(unit));
  TypeSymbol symbol(low_name, low_unit, &type);
  symbol.descriptor = make_type_descriptor(
      registry, &type, nullptr,
      make_enum_info(low_unit, low_name, std::string(cxx_name), type));
  bind_type_expr_descriptor(&type, symbol.descriptor);
  return symbol;
}

void TypeRegistry::initialize_runtime_types() {
  if (runtime_initialized) return;
  // Register the compiler-provided Pascal type declarations. Fundamental
  // types own descriptors; aliases below are ordinary `type Name = Target`
  // declarations and share the descriptor selected by their TyName target.
  // This is declaration data only. No later semantic consumer infers identity
  // from these Pascal spellings or from equal C++ carriers.
  // The builtin-literal set is the primitive_type_map (single source of truth
  // for which Pascal names lower to runtime carriers).
  struct BuiltinAliasDecl {
    std::string_view name;
    std::string_view target;
  };
  static constexpr BuiltinAliasDecl aliases[] = {
      {"integer", "longint"},
      {"cardinal", "longword"},
      {"dword", "longword"},
      {"ansichar", "char"},
      {"pansichar", "pchar"},
  };
  auto alias_declaration = [](std::string_view name)
      -> const BuiltinAliasDecl* {
    for (const BuiltinAliasDecl& alias : aliases) {
      if (alias.name == name) return &alias;
    }
    return nullptr;
  };

  units["__builtin__"] = unit_info_for("__builtin__");
  UnitInfo& builtin_types = units["__builtin__"];
  for (const auto& [name, info] : primitive_type_map()) {
    if (alias_declaration(name) || name == "pointer" || name == "pchar") {
      continue;
    }
    auto atom_type = std::make_shared<ast::TyName>(name);
    primitive_info_storage.push_back(info);
    PrimitiveInfo& semantic_info = primitive_info_storage.back();
    type_symbols.emplace_back(name, "__builtin__", atom_type);
    TypeSymbol& symbol = type_symbols.back();
    const TypeDescriptor* descriptor = make_type_descriptor(
        *this, symbol.type, &symbol, {}, &semantic_info);
    semantic_info.descriptor = descriptor;
    bind_symbol_descriptor(symbol, descriptor);
    builtin_types.iface_types[name] = &symbol;
  }
  auto register_structural_builtin =
      [&](std::string name, std::shared_ptr<TypeExpr> type) -> TypeSymbol* {
    const PrimitiveInfo* info = primitive_info(name);
    assert(info);
    primitive_info_storage.push_back(*info);
    PrimitiveInfo& semantic_info = primitive_info_storage.back();
    type_symbols.emplace_back(name, "__builtin__", std::move(type));
    TypeSymbol& symbol = type_symbols.back();
    const TypeDescriptor* descriptor = make_type_descriptor(
        *this, symbol.type, &symbol, {}, &semantic_info);
    semantic_info.descriptor = descriptor;
    bind_symbol_descriptor(symbol, descriptor);
    builtin_types.iface_types[name] = &symbol;
    return &symbol;
  };
  register_structural_builtin(
      "pointer", std::make_shared<TyPointer>(Location{}, nullptr));
  {
    const TypeSymbol* character = builtin_types.iface_types.at("char");
    auto target = std::make_shared<TyName>("char");
    bind_type_expr_symbol(target.get(), character);
    bind_type_expr_descriptor(target.get(), character->descriptor);
    register_structural_builtin(
        "pchar",
        std::make_shared<TyPointer>(Location{}, std::move(target)));
  }
  for (const BuiltinAliasDecl& declaration : aliases) {
    const auto target =
        builtin_types.iface_types.find(std::string(declaration.target));
    assert(target != builtin_types.iface_types.end());
    auto alias_type =
        std::make_shared<ast::TyName>(std::string(declaration.target));
    bind_type_expr_symbol(alias_type.get(), target->second);
    bind_type_expr_descriptor(alias_type.get(), target->second->descriptor);
    type_symbols.emplace_back(std::string(declaration.name), "__builtin__",
                              alias_type);
    TypeSymbol& alias = type_symbols.back();
    bind_symbol_descriptor(alias, target->second->descriptor);
    builtin_types.iface_types[std::string(declaration.name)] = &alias;
  }
  auto link_builtin =
      [&](std::string_view source, std::string_view target,
          const TypeDescriptor* TypeDescriptor::*edge) {
    const TypeDescriptor* source_descriptor =
        builtin_descriptor(*this, source);
    const TypeDescriptor* target_descriptor =
        builtin_descriptor(*this, target);
    assert(source_descriptor && target_descriptor);
    const_cast<TypeDescriptor*>(source_descriptor)->*edge =
        target_descriptor;
  };
  auto link_builtin_to_self =
      [&](std::string_view source,
          const TypeDescriptor* TypeDescriptor::*edge) {
    link_builtin(source, source, edge);
  };

  // These are intrinsic result rules between declarations, not aliases.
  // Resolve the declaration spellings once while constructing the builtin
  // registry root; consumers follow the descriptor edges above.
  link_builtin("char", "byte", &TypeDescriptor::ordinal_result);
  link_builtin("boolean", "byte", &TypeDescriptor::ordinal_result);
  link_builtin("widechar", "word", &TypeDescriptor::ordinal_result);
  link_builtin("bytebool", "shortint", &TypeDescriptor::ordinal_result);
  link_builtin("wordbool", "smallint", &TypeDescriptor::ordinal_result);
  link_builtin("longbool", "longint", &TypeDescriptor::ordinal_result);
  link_builtin("qwordbool", "int64", &TypeDescriptor::ordinal_result);
  for (std::string_view integer :
       {"shortint", "byte", "smallint", "word", "longint", "longword",
        "int64", "qword", "ptrint", "ptruint", "sizeint", "sizeuint"}) {
    link_builtin_to_self(integer, &TypeDescriptor::ordinal_result);
    link_builtin(integer, "real", &TypeDescriptor::real_division_result);
    link_builtin(integer, "longint",
                 &TypeDescriptor::set_literal_element_result);
  }
  link_builtin("currency", "real",
               &TypeDescriptor::real_division_result);
  link_builtin_to_self("boolean",
                       &TypeDescriptor::set_literal_element_result);
  link_builtin_to_self("char",
                       &TypeDescriptor::set_literal_element_result);
  link_builtin_to_self("widechar",
                       &TypeDescriptor::set_literal_element_result);

  link_builtin("char", "shortstring",
               &TypeDescriptor::string_concat_result);
  link_builtin_to_self("shortstring",
                       &TypeDescriptor::string_concat_result);
  link_builtin_to_self("ansistring",
                       &TypeDescriptor::string_concat_result);
  link_builtin_to_self("utf8string",
                       &TypeDescriptor::string_concat_result);
  for (std::string_view string_type :
       {"shortstring", "ansistring", "utf8string"}) {
    link_builtin(string_type, "char", &TypeDescriptor::element_result);
  }

  link_builtin("pointer", "ptrint",
               &TypeDescriptor::pointer_difference_result);
  link_builtin("pchar", "ptrint",
               &TypeDescriptor::pointer_difference_result);

  for (std::string_view integer : {"shortint", "byte", "smallint", "word"}) {
    link_builtin(integer, "byte", &TypeDescriptor::lo_hi_result);
  }
  for (std::string_view integer : {"longint", "longword"}) {
    link_builtin(integer, "word", &TypeDescriptor::lo_hi_result);
  }
  for (std::string_view integer : {"int64", "qword"}) {
    link_builtin(integer, "longword", &TypeDescriptor::lo_hi_result);
  }
  // rt:: builtins that live in `tp2cc_rt/prelude.h` rather than a
  // Pascal unit. Model them as ProcInfos so type analysis and auto-call
  // decisions go through the same lookup path as real Pascal procs. The result
  // spelling is resolved to a TypeSymbol after the runtime aliases exist.
  struct RtBuiltin {
    const char* name;
    size_t params;
    bool is_fn;
    bool zero_ok;
    const char* ret;
    std::span<const ProcInfo::SlotInfo> slot_info = {};
  };
  static constexpr ProcInfo::SlotInfo first_slot_untyped_const[] = {
      {ProcInfo::SlotStorage::UntypedConst},
  };
  static constexpr ProcInfo::SlotInfo first_slot_untyped_mutable[] = {
      {ProcInfo::SlotStorage::UntypedMutable},
  };
  static constexpr ProcInfo::SlotInfo first_slot_mutable[] = {
      {ProcInfo::SlotStorage::Mutable},
  };
  static constexpr ProcInfo::SlotInfo second_slot_untyped_const[] = {
      {ProcInfo::SlotStorage::Value},
      {ProcInfo::SlotStorage::UntypedConst},
  };
  static constexpr ProcInfo::SlotInfo second_slot_untyped_mutable[] = {
      {ProcInfo::SlotStorage::Value},
      {ProcInfo::SlotStorage::UntypedMutable},
  };
  static constexpr ProcInfo::SlotInfo second_slot_mutable[] = {
      {ProcInfo::SlotStorage::Value},
      {ProcInfo::SlotStorage::Mutable},
  };
  static constexpr ProcInfo::SlotInfo first_two_slots_untyped_const[] = {
      {ProcInfo::SlotStorage::UntypedConst},
      {ProcInfo::SlotStorage::UntypedConst},
  };
  static constexpr ProcInfo::SlotInfo move_slots[] = {
      {ProcInfo::SlotStorage::UntypedConst},
      {ProcInfo::SlotStorage::UntypedMutable},
  };
  static constexpr ProcInfo::SlotInfo second_and_third_slots_mutable[] = {
      {ProcInfo::SlotStorage::Value},
      {ProcInfo::SlotStorage::Mutable},
      {ProcInfo::SlotStorage::Mutable},
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
      {"fillchar",   3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_mutable)},
      {"fillbyte",   3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_mutable)},
      {"fillword",   3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_mutable)},
      {"move",       3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(move_slots)},
      {"prefetch",   1, false, false, ""},
      {"getmem",     2, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
      {"freemem",    1, false, false, ""},
      {"freemem",    2, false, false, ""},
      {"reallocmem", 2, true,  false, "pointer",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
      {"allocmem",   1, true,  false, "pointer"},
      {"setlength",  2, false, false, ""},
      {"setstring",  3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
      {"dispose",    1, false, false, ""},
      {"strdispose", 1, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
      {"val",        3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_and_third_slots_mutable)},
      {"str",        2, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_slot_mutable)},
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
      {"blockread",  3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_slot_untyped_mutable)},
      {"blockread",  4, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_slot_untyped_mutable)},
      {"blockwrite", 3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_slot_untyped_const)},
      {"blockwrite", 4, false, false, "",
       std::span<const ProcInfo::SlotInfo>(second_slot_untyped_const)},
      {"copy",       3, true,  false, "shortstring"},
      {"delete",     3, false, false, ""},
      {"insert",     3, false, false, ""},
      {"pos",        2, true,  false, "longint"},
      {"trim",       1, true,  false, "ansistring"},
      {"initialize", 1, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
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
      {"include",    2, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
      {"exclude",    2, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_mutable)},
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
      {"indexbyte",  3, true,  false, "longint",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_const)},
      {"indexword",  3, true,  false, "longint",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_const)},
      {"comparebyte", 3, true, false, "longint",
       std::span<const ProcInfo::SlotInfo>(first_two_slots_untyped_const)},
      {"comparechar", 3, true, false, "longint",
       std::span<const ProcInfo::SlotInfo>(first_two_slots_untyped_const)},
      {"compareword", 3, true, false, "longint",
       std::span<const ProcInfo::SlotInfo>(first_two_slots_untyped_const)},
      {"filldword",  3, false, false, "",
       std::span<const ProcInfo::SlotInfo>(first_slot_untyped_mutable)},
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
                 .return_type_name = b.ret,
                 .return_type_symbol = nullptr,
                 .slot_info = std::vector<ProcInfo::SlotInfo>(
                     b.slot_info.begin(), b.slot_info.end())});
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

  // tp2cc does not translate the RTL/System unit, but compiler sources still
  // compile with its interface in scope. Register those source-visible System
  // names on the synthetic runtime unit so semantic type binding sees a real
  // imported unit export instead of recovering the names later in emission.
  register_runtime_type(*this, rt_exports, "tprocedure",
                         runtime_procedural_type(false, {}));
  register_runtime_type(*this, rt_exports, "signalhandler",
                         runtime_pointer_type());
  register_runtime_type(
      *this, rt_exports, "tfpuexception",
      runtime_enum_type({"exinvalidop", "exdenormalized", "exzerodivide",
                         "exoverflow", "exunderflow", "exprecision"}));
  register_runtime_type(*this, rt_exports, "searchrec", runtime_record_type({
      runtime_record_field("time", runtime_type_name("longint")),
      runtime_record_field("size", runtime_type_name("longint")),
      runtime_record_field("attr", runtime_type_name("byte")),
      runtime_record_field("name", runtime_type_name("shortstring")),
  }));
  register_runtime_type(*this, rt_exports, "tsearchrec",
                         runtime_type_name("searchrec"));
  register_runtime_type(*this, rt_exports, "stat", runtime_record_type({
      runtime_record_field("mtime", runtime_type_name("longint")),
      runtime_record_field("st_mtime", runtime_type_name("longint")),
      runtime_record_field("mode", runtime_type_name("longint")),
      runtime_record_field("st_mode", runtime_type_name("longint")),
      runtime_record_field("size", runtime_type_name("longint")),
      runtime_record_field("st_size", runtime_type_name("longint")),
  }));
  register_runtime_type(*this, rt_exports, "datetime", runtime_record_type({
      runtime_record_field("year", runtime_type_name("word")),
      runtime_record_field("month", runtime_type_name("word")),
      runtime_record_field("day", runtime_type_name("word")),
      runtime_record_field("hour", runtime_type_name("word")),
      runtime_record_field("min", runtime_type_name("word")),
      runtime_record_field("sec", runtime_type_name("word")),
  }));
  register_runtime_type(*this, rt_exports, "tdatetime",
                         runtime_type_name("double"));
  register_runtime_type(*this, rt_exports, "tsystemtime", runtime_record_type({
      runtime_record_field("year", runtime_type_name("word")),
      runtime_record_field("month", runtime_type_name("word")),
      runtime_record_field("dayofweek", runtime_type_name("word")),
      runtime_record_field("day", runtime_type_name("word")),
      runtime_record_field("hour", runtime_type_name("word")),
      runtime_record_field("minute", runtime_type_name("word")),
      runtime_record_field("second", runtime_type_name("word")),
      runtime_record_field("millisecond", runtime_type_name("word")),
  }));
  register_runtime_type(*this, rt_exports, "dirstr",
                         runtime_type_name("shortstring"));
  register_runtime_type(*this, rt_exports, "namestr",
                         runtime_type_name("shortstring"));
  register_runtime_type(*this, rt_exports, "extstr",
                         runtime_type_name("shortstring"));
  register_runtime_type(*this, rt_exports, "pathstr",
                         runtime_type_name("shortstring"));
  register_runtime_type(*this, rt_exports, "comstr",
                         runtime_type_name("shortstring"));
  register_runtime_type(*this, rt_exports, "texecuteflag",
                         runtime_enum_type({"execinheritshandles"}));
  register_runtime_type(
      *this, rt_exports, "texecuteflags",
      runtime_set_type(runtime_type_name("texecuteflag")));
  register_runtime_type(
      *this, rt_exports, "tfpuexceptionmask",
      runtime_set_type(runtime_type_name("tfpuexception")));
  register_runtime_type(*this, rt_exports, "tsyscharset",
                         runtime_set_type(runtime_type_name("char")));
  register_runtime_type(*this, rt_exports, "hresult",
                         runtime_type_name("longint"));
  register_runtime_type_alias(
      *this, rt_exports, "pointer", builtin_literal("pointer"));
  register_runtime_type(*this, rt_exports, "tclass",
                         runtime_metaclass_type("__rt__.tobject"));
  if (TypeSymbol* tmethod =
          register_runtime_type(*this, rt_exports, "tmethod",
                                 runtime_record_type({
                                     runtime_record_field(
                                         "code", runtime_type_name("pointer")),
                                     runtime_record_field(
                                         "data", runtime_type_name("pointer")),
                                 }))) {
    const_cast<TypeDescriptor*>(tmethod->descriptor)->is_method_carrier = true;
  }
  register_runtime_type_alias(
      *this, rt_exports, "ansichar", builtin_literal("ansichar"));
  register_runtime_type_alias(
      *this, rt_exports, "pchar", builtin_literal("pchar"));
  register_runtime_type_alias(
      *this, rt_exports, "pansichar", builtin_literal("pansichar"));
  register_runtime_type(
      *this, rt_exports, "pansistring",
      runtime_pointer_type(runtime_type_name("ansistring")));
  register_runtime_type(
      *this, rt_exports, "pcardinal",
      runtime_pointer_type(runtime_type_name("cardinal")));
  register_runtime_type(
      *this, rt_exports, "pcurrency",
      runtime_pointer_type(runtime_type_name("currency")));
  register_runtime_type(*this, rt_exports, "pdword",
                         runtime_pointer_type(runtime_type_name("dword")));
  register_runtime_type(*this, rt_exports, "pint64",
                         runtime_pointer_type(runtime_type_name("int64")));
  register_runtime_type(
      *this, rt_exports, "plongword",
      runtime_pointer_type(runtime_type_name("longword")));
  register_runtime_type(*this, rt_exports, "ppointer",
                         runtime_pointer_type(runtime_type_name("pointer")));
  register_runtime_type(*this, rt_exports, "pqword",
                         runtime_pointer_type(runtime_type_name("qword")));
  register_runtime_type(
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
  register_runtime_type_alias(
      *this, rt_exports, "eheapexception",
      rt_exports.find_type("eheapmemoryerror"));
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

  TypeLookupContext* builtin_context =
      make_scope_frame(*this, "__builtin__", nullptr,
                       ScopeFrameKind::ImportedUnitInterface,
                       &builtin_types);
  unit_interface_type_contexts["__builtin__"] = builtin_context;
  TypeLookupContext* runtime_context =
      make_scope_frame(*this, "__rt__", builtin_context,
                       ScopeFrameKind::UnitInterface, &rt_exports);
  unit_interface_type_contexts["__rt__"] = runtime_context;
  // Runtime-backed units copy these declarations; resolve the originals once
  // so every exported copy carries the same bound TypeExpr identities.
  index_runtime_unit_contexts(*this, rt_exports, runtime_context);
  resolve_runtime_unit_descriptors(*this, rt_exports, runtime_context);
  for (const auto& [_, symbol] : rt_exports.iface_types) {
    if (symbol && symbol->defining_unit == "__rt__") {
      resolve_class_links_for_symbol(*this, *symbol);
    }
  }
  resolve_runtime_proc_return_symbols(*this, rt_exports);
  runtime_initialized = true;
}

void TypeRegistry::begin_parsed_unit(std::string_view name) {
  initialize_runtime_types();
  const std::string unit = lc(std::string(name));
  if (unit.empty()) return;
  units.try_emplace(unit, unit_info_for(unit));
  parse_unit_type_states.try_emplace(unit);
}

void TypeRegistry::set_parsed_unit_imports(
    std::string_view name, const std::vector<std::string>& imports,
    bool in_interface) {
  const std::string unit = lc(std::string(name));
  begin_parsed_unit(unit);
  UnitInfo& info = units[unit];
  std::vector<std::string> normalized;
  normalized.reserve(imports.size());
  for (const std::string& imported : imports) {
    const std::string imported_unit = lc(imported);
    normalized.push_back(imported_unit);
    register_runtime_backed_unit(*this, imported_unit);
  }
  if (in_interface) {
    info.interface_uses = std::move(normalized);
  } else {
    info.implementation_uses = std::move(normalized);
  }

  ParseUnitTypeState& state = parse_unit_type_states[unit];
  const TypeLookupContext* imports_context =
      make_unit_import_context(*this, unit, in_interface);
  if (in_interface) {
    state.interface_current = imports_context;
    unit_interface_type_contexts[unit] = imports_context;
    return;
  }

  const TypeLookupContext* interface_scope =
      make_scope_frame(*this, unit, imports_context,
                       ScopeFrameKind::UnitInterface, &info);
  state.implementation_current =
      make_scope_frame(*this, unit, interface_scope,
                       ScopeFrameKind::UnitImplementation, &info);
  unit_implementation_type_contexts[unit] =
      state.implementation_current;
}

void TypeRegistry::bind_parsed_declarations(
    std::string_view name, const std::vector<DeclPtr>& declarations,
    bool in_interface) {
  if (declarations.empty()) return;
  const std::string unit = lc(std::string(name));
  begin_parsed_unit(unit);
  UnitInfo& info = units[unit];
  ParseUnitTypeState& state = parse_unit_type_states[unit];
  const TypeLookupContext*& current =
      in_interface ? state.interface_current : state.implementation_current;
  if (!current) {
    set_parsed_unit_imports(unit, {}, in_interface);
  }
  const TypeLookupContext* before = current;

  register_decl_list(*this, unit, declarations, in_interface);
  current = index_decl_list_contexts(
      *this, unit, declarations, current,
      in_interface ? ScopeFrameKind::UnitInterface
                   : ScopeFrameKind::UnitImplementation,
      &info, /*preserve_local_value_scope=*/false, before);
  if (in_interface) {
    unit_interface_type_contexts[unit] = current;
  } else {
    unit_implementation_type_contexts[unit] = current;
  }

  resolve_decl_list_type_descriptors(*this, declarations, current);

  for (const DeclPtr& declaration : declarations) {
    if (!declaration || declaration->kind != Kind::TypeDecl) continue;
    const auto& type_decl = static_cast<const TypeDecl&>(*declaration);
    if (type_decl.symbol) {
      resolve_class_links_for_symbol(
          *this, *const_cast<TypeSymbol*>(type_decl.symbol));
      resolve_property_accessors_in_symbol(
          *this, *const_cast<TypeSymbol*>(type_decl.symbol));
    }
  }
}

void TypeRegistry::bind_parsed_unit_bodies(const UnitNode& unit) {
  const std::string name = lc(unit.name);
  const TypeLookupContext* context =
      lookup_unit_context(pascal_key(name), /*implementation=*/true);
  bind_stmt_semantics(*this, unit.init_body, context);
  bind_stmt_semantics(*this, unit.final_body, context);
  auto found = units.find(name);
  if (found != units.end()) {
    prune_completed_interface_proc_impls(*this, found->second);
    report_type_value_collisions(found->second);
  }
}

const TypeSymbol* TypeRegistry::lookup_type_symbol_exact(
    PascalKey unit, PascalKey name) const {
  const std::string target_unit = pascal_key_string(unit);
  const std::string target_name = pascal_key_string(name);
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
                                              PascalKey name,
                                              bool include_imports) {
  if (!include_imports && is_import_frame(frame)) return nullptr;
  const std::string key = pascal_key_string(name);
  auto local = frame.type_symbols.find(key);
  if (local != frame.type_symbols.end()) return local->second;
  if (frame.restrict_unit_type_lookup) return nullptr;
  if (!frame.unit_info) return nullptr;
  switch (frame.kind) {
    case ScopeFrameKind::UnitImplementation: {
      auto it = frame.unit_info->impl_types.find(key);
      return it == frame.unit_info->impl_types.end() ? nullptr : it->second;
    }
    case ScopeFrameKind::UnitInterface:
    case ScopeFrameKind::ImportedUnitInterface: {
      auto it = frame.unit_info->iface_types.find(key);
      return it == frame.unit_info->iface_types.end() ? nullptr : it->second;
    }
    case ScopeFrameKind::Local:
      return nullptr;
  }
  return nullptr;
}

const TypeSymbol* lookup_type_path_in_frame(const TypeLookupContext& frame,
                                            PascalKey path,
                                            bool include_imports) {
  const std::string path_key = pascal_key_string(path);
  const size_t dot = path_key.find('.');
  if (dot == std::string::npos) {
    return lookup_type_symbol_in_frame(frame, path, include_imports);
  }
  const std::string root_name = path_key.substr(0, dot);
  const TypeSymbol* root =
      lookup_type_symbol_in_frame(frame, pascal_key(root_name),
                                  include_imports);
  return lookup_nested_type_symbol_path(root, path_key, dot + 1);
}

const TypeSymbol* lookup_unit_qualified_type_in_context(
    PascalKey unit, PascalKey path,
    const TypeLookupContext* context, bool include_imports) {
  std::string target_unit = pascal_key_string(unit);
  if (target_unit == "system") target_unit = "__rt__";
  const std::string target_path = pascal_key_string(path);
  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    // `Unit.T` selects a unit namespace. Local and type-member frames carry the
    // current unit name for unqualified lexical lookup, but they are not part
    // of the qualified unit export/implementation namespace.
    if (frame->kind == ScopeFrameKind::Local) continue;
    if (frame->unit != target_unit) continue;
    if (!include_imports && is_import_frame(*frame)) continue;
    if (const TypeSymbol* symbol =
            lookup_type_path_in_frame(*frame, pascal_key(target_path),
                                      include_imports)) {
      return symbol;
    }
  }
  return nullptr;
}

TypeSymbol* TypeRegistry::lookup_type_symbol_exact_mut(
    PascalKey unit, PascalKey name) {
  const std::string target_unit = pascal_key_string(unit);
  const std::string target_name = pascal_key_string(name);
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

const ClassInfo* TypeRegistry::lookup_parent_class(
    const ClassInfo& class_info) const {
  const TypeSymbol* parent_symbol = class_info.parent_symbol;
  return parent_symbol ? parent_symbol->class_info() : nullptr;
}

bool TypeRegistry::class_implements_interface(
    const ClassInfo& class_info, const InterfaceInfo& interface_info) const {
  const TypeSymbol* target_symbol = interface_info.symbol;
  const ClassInfo* cls = &class_info;
  SeenClassChain seen;
  while (cls && seen.mark(cls)) {
    for (const TypeSymbol* implemented_symbol : cls->interface_symbols) {
      if (target_symbol && implemented_symbol == target_symbol) {
        return true;
      }
    }
    cls = lookup_parent_class(*cls);
  }
  return false;
}

const EnumInfoReg* TypeRegistry::enum_info_for_type(
    const TyEnum* type) const {
  return type && type->descriptor ? type->descriptor->enum_info() : nullptr;
}

const TypeLookupContext* TypeRegistry::lookup_context_for_type(
    const TypeExpr* type) const {
  return type ? type->type_context : nullptr;
}

const TypeLookupContext* TypeRegistry::lookup_unit_context(
    PascalKey unit, bool implementation) const {
  const std::string key = pascal_key_string(unit);
  const auto& map = implementation ? unit_implementation_type_contexts
                                   : unit_interface_type_contexts;
  auto it = map.find(key);
  return it == map.end() ? nullptr : it->second;
}

const TypeLookupContext* TypeRegistry::lookup_proc_signature_context(
    const ProcDecl* proc) const {
  return proc ? proc->signature_type_context : nullptr;
}

const TypeLookupContext* TypeRegistry::lookup_proc_body_context(
    const ProcDecl* proc) const {
  return proc ? proc->body_type_context : nullptr;
}

const TypeSymbol* TypeRegistry::method_owner_symbol_for_proc(
    const ProcDecl* proc) const {
  return proc ? proc->method_owner_symbol : nullptr;
}

std::optional<const TypeSymbol*> TypeRegistry::exception_handler_type_result(
    const ExceptHandler* handler) const {
  if (!handler || !handler->class_binding_complete) return std::nullopt;
  return handler->class_symbol;
}

std::optional<const TypeSymbol*> TypeRegistry::type_name_expression_result(
    const Expr* expr) const {
  if (!expr || !expr->type_operand_bound) return std::nullopt;
  return expr->type_operand_symbol;
}

std::optional<const TypeSymbol*>
TypeRegistry::value_type_expression_result(
    const Expr* expr) const {
  if (!expr || !expr->type_value_bound) return std::nullopt;
  return expr->type_value_symbol;
}

const TypeDescriptor* TypeRegistry::expression_result_descriptor(
    const Expr* expr) const {
  return expr ? expr->result_descriptor : nullptr;
}

const TypeDescriptor* TypeRegistry::descriptor_for_type(
    const TypeExpr* type) const {
  return type ? type->descriptor : nullptr;
}

const TypeSymbol* TypeRegistry::referenced_symbol_for_type(
    const TypeExpr* type) const {
  return type ? type->referenced_symbol : nullptr;
}

const TypeSymbol* TypeRegistry::resolved_symbol_for_type(
    const TypeExpr* type) const {
  const TypeDescriptor* descriptor = descriptor_for_type(type);
  if (!descriptor) return nullptr;
  if (descriptor->symbol) return descriptor->symbol;
  return referenced_symbol_for_type(type);
}

const TypeSymbol* TypeRegistry::metaclass_target_for_type(
    const TypeExpr* type) const {
  const TypeDescriptor* descriptor = descriptor_for_type(type);
  return descriptor ? descriptor->metaclass_target : nullptr;
}

const TypeSymbol* TypeRegistry::lookup_type_symbol_in_context(
    PascalKey name, const TypeLookupContext* context) const {
  const std::string key = pascal_key_string(name);
  if (!context) {
    // A missing context means there is no Pascal source scope to walk. Only
    // builtin atoms live outside lexical/unit scopes; unit-qualified recovery
    // here would be a second type lookup path after semantic binding failed.
    return builtin_literal(key);
  }
  if (auto dot = key.find('.'); dot != std::string::npos) {
    const std::string root_name = key.substr(0, dot);
    if (const TypeSymbol* root =
            lookup_type_symbol_in_context(pascal_key(root_name), context)) {
      if (const TypeSymbol* nested =
              lookup_nested_type_symbol_path(root, key, dot + 1)) {
        return nested;
      }
    }
    const std::string path = key.substr(dot + 1);
    return lookup_unit_qualified_type_in_context(
        pascal_key(root_name), pascal_key(path), context,
        /*include_imports=*/true);
  }

  for (const TypeLookupContext* frame = context; frame;
       frame = frame->parent) {
    if (const TypeSymbol* symbol =
            lookup_type_symbol_in_frame(*frame, name,
                                        /*include_imports=*/true)) {
      return symbol;
    }
  }
  // Primitive and runtime atoms are the implicit outer type scope. They are
  // consulted after lexical/unit frames so source declarations keep ordinary
  // Pascal shadowing behavior.
  return builtin_literal(key);
}

const EnumMemberInfo* TypeRegistry::lookup_enum_member_in_unit(
    std::string_view unit, std::string_view member) const {
  assert(pascal_key_is_canonical(unit));
  assert(pascal_key_is_canonical(member));
  auto uit = enum_members_by_unit.find(std::string(unit));
  if (uit == enum_members_by_unit.end()) return nullptr;
  auto mit = uit->second.find(std::string(member));
  return mit == uit->second.end() ? nullptr : &mit->second;
}

const TypeSymbol* TypeRegistry::builtin_literal(std::string_view name) const {
  if (!pascal_key_is_canonical(name)) return nullptr;
  return lookup_type_symbol_exact(pascal_key("__builtin__"),
                                  pascal_key(name));
}

const TySet* TypeRegistry::inferred_set_type(
    const TypeExpr* element,
    std::optional<std::pair<int64_t, int64_t>> explicit_bounds) const {
  if (!element) return nullptr;
  const TypeDescriptor* element_descriptor = descriptor_for_type(element);
  if (!element_descriptor) return nullptr;
  const bool has_explicit_bounds = explicit_bounds.has_value();
  InferredSetKey key{.element = element_descriptor,
                     .has_explicit_bounds = has_explicit_bounds,
                     .low = has_explicit_bounds ? explicit_bounds->first : 0,
                     .high = has_explicit_bounds ? explicit_bounds->second : 0};
  if (auto found = inferred_set_types.find(key);
      found != inferred_set_types.end()) {
    return found->second.get();
  }
  auto borrowed_element =
      TypePtr(const_cast<TypeExpr*>(element), [](TypeExpr*) {});
  auto type = std::make_shared<TySet>(
      element->loc, std::move(borrowed_element), key.has_explicit_bounds,
      key.low, key.high);
  type_descriptor_storage.push_back(
      TypeDescriptor{.id = type_descriptor_storage.size() + 1,
                     .type = type.get(),
                     .symbol = nullptr,
                     .metaclass_target = nullptr,
                     .payload = {}});
  type->descriptor = &type_descriptor_storage.back();
  inferred_set_types.emplace(key, type);
  return type.get();
}

const TyPointer* TypeRegistry::inferred_pointer_type(
    const TypeExpr* target) const {
  if (!target) return nullptr;
  const TypeDescriptor* target_descriptor = descriptor_for_type(target);
  if (!target_descriptor) return nullptr;
  if (auto found = inferred_pointer_types.find(target_descriptor);
      found != inferred_pointer_types.end()) {
    return found->second.get();
  }
  auto borrowed_target =
      TypePtr(const_cast<TypeExpr*>(target), [](TypeExpr*) {});
  auto type =
      std::make_shared<TyPointer>(target->loc, std::move(borrowed_target));
  TypeRegistry& mutable_registry = const_cast<TypeRegistry&>(*this);
  type->descriptor =
      make_type_descriptor(mutable_registry, type.get(), nullptr);
  inferred_pointer_types.emplace(target_descriptor, type);
  return type.get();
}

const TyArray* TypeRegistry::array_tail_type(
    const TyArray* source, std::size_t first_dimension) const {
  if (!source || first_dimension >= source->dims.size()) return nullptr;
  const TypeDescriptor* source_descriptor = descriptor_for_type(source);
  if (!source_descriptor) return nullptr;
  ArrayTailKey key{source_descriptor, first_dimension};
  if (auto found = array_tail_types.find(key);
      found != array_tail_types.end()) {
    return found->second.get();
  }
  std::vector<TypePtr> dimensions(source->dims.begin() + first_dimension,
                                  source->dims.end());
  auto type = std::make_shared<TyArray>(
      source->loc, std::move(dimensions), source->element, source->is_packed,
      source->array_kind);
  type->type_context = source->type_context;
  type_descriptor_storage.push_back(
      TypeDescriptor{.id = type_descriptor_storage.size() + 1,
                     .type = type.get(),
                     .symbol = nullptr,
                     .metaclass_target = nullptr,
                     .payload = {}});
  type->descriptor = &type_descriptor_storage.back();
  array_tail_types.emplace(key, type);
  return type.get();
}

std::string TypeRegistry::field_cxx_name(std::string_view name) const {
  return mangle(name);
}

bool TypeRegistry::same_class_identity(const ClassInfo& a,
                                       const ClassInfo& b) const {
  if (a.descriptor && b.descriptor) return a.descriptor == b.descriptor;
  return &a == &b;
}

const FieldInfo* TypeRegistry::lookup_class_field(
    const ClassInfo& class_info, const std::string& member) const {
  assert(pascal_key_is_canonical(member));
  const ClassInfo* ci = &class_info;
  const std::string& key = member;
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto fit = ci->fields.find(key);
    if (fit != ci->fields.end()) return &fit->second;
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

bool TypeRegistry::class_has_enum_member(
    const ClassInfo& class_info, const std::string& member) const {
  return lookup_class_enum_member(class_info, member) != nullptr;
}

const EnumMemberInfo* TypeRegistry::lookup_class_enum_member(
    const ClassInfo& class_info, const std::string& member) const {
  assert(pascal_key_is_canonical(member));
  const ClassInfo* ci = &class_info;
  const std::string& key = member;
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto found = ci->enum_members.find(key);
    if (found != ci->enum_members.end()) return &found->second;
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

const std::vector<MethodSig>* TypeRegistry::lookup_class_methods(
    const ClassInfo& class_info, const std::string& member) const {
  // Pascal class lookup applies name hiding before overload ranking: the first
  // class with the member name stops the parent walk unless that discovered
  // overload set explicitly keeps inherited overloads visible.
  assert(pascal_key_is_canonical(member));
  const std::string& key = member;
  const ClassInfo* ci = &class_info;
  if (!ci) return nullptr;
  MethodCacheKey cache_key{class_info.descriptor, key};
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
      bool has_overload = false;
      for (const auto& sig : mit->second) {
        if (sig.is_overload) {
          has_overload = true;
          break;
        }
      }
      if (!has_overload) break;
    }
    ci = lookup_parent_class(*ci);
  }
  auto [it, _] =
      merged_method_cache.emplace(std::move(cache_key), std::move(merged));
  return it->second.empty() ? nullptr : &it->second;
}

const std::vector<MethodSig>* TypeRegistry::lookup_interface_methods(
    const InterfaceInfo& interface_info, const std::string& member) const {
  assert(pascal_key_is_canonical(member));
  auto mit = interface_info.methods.find(member);
  return mit == interface_info.methods.end() ? nullptr : &mit->second;
}

const PropertyInfo* TypeRegistry::lookup_class_property(
    const ClassInfo& class_info, const std::string& member) const {
  assert(pascal_key_is_canonical(member));
  const ClassInfo* ci = &class_info;
  const std::string& key = member;
  SeenClassChain seen;
  while (ci && seen.mark(ci)) {
    auto pit = ci->properties.find(key);
    if (pit != ci->properties.end()) return &pit->second;
    ci = lookup_parent_class(*ci);
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_default_property(
    const ClassInfo& class_info) const {
  const ClassInfo* ci = &class_info;
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

}  // namespace tp2cc
