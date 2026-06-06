#include "typereg.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <unordered_set>

#include "diag.h"
#include "emit_support.h"

namespace tp2cc {

using namespace ast;

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

void populate_nested_types(TypeSymbol& symbol);

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
  populate_nested_types(*stored);
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

MethodSig method_sig_for(std::string defining_unit,
                         std::shared_ptr<const ProcDecl> method) {
  const auto& pd = *method;
  return MethodSig{.kind = method_kind_for(pd),
                   .defining_unit = std::move(defining_unit),
                   .param_count = proc_param_count(pd.params),
                   .accepts_zero_args = proc_accepts_zero_args(pd),
                   .is_function = (pd.pkind == ProcKind::Function),
                   .is_virtual = pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override,
                   .is_final = pd.modifiers.is_final,
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
    const std::string& defining_unit, const TyObject& to) {
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Method && m.method) {
      methods[lc(m.method->name)].push_back(
          method_sig_for(defining_unit, m.method));
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

std::unordered_map<std::string, std::vector<MethodSig>> interface_methods(
    const std::string& defining_unit, const TyInterface& ti) {
  std::unordered_map<std::string, std::vector<MethodSig>> methods;
  for (const auto& m : ti.members) {
    if (m.kind != ObjectMemberKind::Method || !m.method) continue;
    const auto& pd = *m.method;
    methods[lc(pd.name)].push_back(MethodSig{
        .kind = SymKind::Method,
        .defining_unit = defining_unit,
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
                         const TyObject& to) {
  return ClassInfo{.name = name,
                   .parent = lc(to.parent),
                   .defining_unit = unit,
                   .is_reference_type = to.is_reference_type,
                   .is_abstract = to.is_abstract,
                   .is_forward = to.is_forward,
                   .fields = class_fields(to),
                   .methods = class_methods(unit, to),
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

void populate_nested_types(TypeSymbol& symbol) {
  if (!symbol.type) return;
  if (auto* nested = nested_type_map_mut(symbol)) {
    // A forward class symbol can later be replaced by its full declaration.
    // Rebuild child type symbols from the current AST so old nested aliases or
    // classes cannot survive that replacement.
    nested->clear();
  }
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
  for (const auto& td : nested_decls) {
    if (!td || !td->type) continue;
    auto child = std::make_shared<TypeSymbol>(
        make_type_symbol_for_type_with_owner(symbol.defining_unit, td->name,
                                             td->type, child_owner_path));
    if (ClassInfo* ci = symbol.mutable_class_info()) {
      ci->nested_types.insert_or_assign(child->name, child);
    } else if (RecordInfo* ri = symbol.mutable_record_info()) {
      ri->nested_types.insert_or_assign(child->name, child);
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
      payload = class_info_for(low_unit, low_name,
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
  populate_nested_types(symbol);
  return symbol;
}

UnitInfo unit_info_for(
    std::string name, std::vector<std::string> uses = {},
    std::unordered_map<std::string, std::vector<ProcInfo>> iface_procs = {}) {
  return UnitInfo{.name = std::move(name),
                  .uses = std::move(uses),
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
  std::unordered_map<std::string, std::vector<MethodSig>> method_map;
  for (auto& m : methods) {
    const std::string method_name = m.decl->name;
    method_map[method_name].push_back(std::move(m));
  }
  const std::string key = name;
  r.rt_classes[key] =
      ClassInfo{.name = std::move(name),
                .parent = std::move(parent),
                .defining_unit = "__rt__",
                .is_reference_type = true,
                .is_abstract = false,
                .is_forward = false,
                .fields = {},
                .methods = std::move(method_map),
                .properties = {},
                .nested_types = {},
                .enum_members = {},
                .default_property_name = {}};
}

void register_external_stub_unit(TypeRegistry& r, std::string used_name) {
  const std::string low = lc(std::move(used_name));
  if (low == "__rt__" || r.units.count(low) > 0) return;
  // UnitGraph/main.cc emits empty headers for missing RTL-style `uses` entries.
  // The registry needs matching no-export units so qualified `Unit.Name`
  // expressions can still lower to the generated stub namespace.
  r.units[low] = unit_info_for(low);
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
        r.direct_type_name(current_type, owner.defining_unit);
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
      {"bsfbyte",    1, true,  false, "byte"},
      {"bsrbyte",    1, true,  false, "byte"},
      {"bsfword",    1, true,  false, "cardinal"},
      {"bsrword",    1, true,  false, "cardinal"},
      {"bsfdword",   1, true,  false, "cardinal"},
      {"bsrdword",   1, true,  false, "cardinal"},
      {"bsfqword",   1, true,  false, "cardinal"},
      {"bsrqword",   1, true,  false, "cardinal"},
      {"upcase",     1, true,  false, "char"},
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
      {"mkdir",      1, false, false, ""},
      {"rmdir",      1, false, false, ""},
      {"getdir",     2, false, false, ""},
      {"deletefile", 1, true,  false, "boolean"},
      {"directoryexists", 1, true, false, "boolean"},
      {"fsearch",    2, true,  false, "shortstring"},
      {"fsplit",     4, false, false, ""},
      {"fexpand",    1, true,  false, "shortstring"},
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
  units["__rt__"] = unit_info_for("__rt__", {}, std::move(rt_iface_procs));
  UnitInfo& rt_exports = units["__rt__"];

  // Runtime MethodSigs below carry synthesized ProcDecls so overload
  // resolution sees the same parameter data for runtime classes as for
  // Pascal declarations.

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
  register_runtime_alias(*this, rt_exports, "ansichar",
                         runtime_type_name("char"));
  register_runtime_alias(
      *this, rt_exports, "pansichar",
      runtime_pointer_type(runtime_type_name("ansichar")));
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

  for (const auto* u : us) {
    if (!u) continue;
    std::vector<std::string> uses;
    for (const auto& nm : u->interface_uses) uses.push_back(lc(nm));
    for (const auto& nm : u->impl_uses) uses.push_back(lc(nm));
    // Every Pascal unit implicitly uses `System`; model the runtime helpers as
    // the last unit in that uses chain.
    uses.push_back("__rt__");
    units[lc(u->name)] = unit_info_for(lc(u->name), std::move(uses));

    register_decl_list(*this, lc(u->name), u->interface_decls,
                       /*is_interface=*/true);
    register_decl_list(*this, lc(u->name), u->impl_decls,
                       /*is_interface=*/false);
    report_type_value_collisions(units[lc(u->name)]);
  }

  for (const auto* u : us) {
    if (!u) continue;
    for (const auto& nm : u->interface_uses) {
      register_external_stub_unit(*this, nm);
    }
    for (const auto& nm : u->impl_uses) {
      register_external_stub_unit(*this, nm);
    }
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
    if (const TypeSymbol* own = lookup_type_symbol_exact(cur_unit, low)) {
      return own;
    }

    const TypeSymbol* runtime = nullptr;
    auto uit = units.find(cur_unit);
    if (uit != units.end()) {
      for (auto use = uit->second.uses.rbegin(); use != uit->second.uses.rend();
           ++use) {
        auto used = units.find(*use);
        if (used == units.end()) continue;
        const TypeSymbol* exported = used->second.find_export_type(low);
        if (!exported) continue;
        if (*use == "__rt__") {
          runtime = exported;
          continue;
        }
        return exported;
      }
    }
    if (runtime) return runtime;
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

const ClassInfo* TypeRegistry::lookup_class_exact(std::string_view unit,
                                                  std::string_view name) const {
  const TypeSymbol* symbol = lookup_type_symbol_exact(unit, name);
  if (!symbol || !symbol->class_info()) {
    return nullptr;
  }
  return symbol->class_info();
}

const ClassInfo* TypeRegistry::lookup_class(std::string_view name,
                                            std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  if (!symbol || !symbol->class_info()) {
    return nullptr;
  }
  return symbol->class_info();
}

const InterfaceInfo* TypeRegistry::lookup_interface_exact(
    std::string_view unit, std::string_view name) const {
  const TypeSymbol* symbol = lookup_type_symbol_exact(unit, name);
  if (!symbol || !symbol->interface_info()) {
    return nullptr;
  }
  return symbol->interface_info();
}

const InterfaceInfo* TypeRegistry::lookup_interface(
    std::string_view name, std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  if (!symbol || !symbol->interface_info()) {
    return nullptr;
  }
  return symbol->interface_info();
}

const RecordInfo* TypeRegistry::lookup_record_exact(
    std::string_view unit, std::string_view name) const {
  const TypeSymbol* symbol = lookup_type_symbol_exact(unit, name);
  if (!symbol || !symbol->record_info()) {
    return nullptr;
  }
  return symbol->record_info();
}

const RecordInfo* TypeRegistry::lookup_record(
    std::string_view name, std::string_view current_unit) const {
  const TypeSymbol* symbol = lookup_type_symbol(name, current_unit);
  if (!symbol || !symbol->record_info()) {
    return nullptr;
  }
  return symbol->record_info();
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

const TyEnum* TypeRegistry::lookup_enum_member_in_unit(
    std::string_view unit, std::string_view member) const {
  auto uit = enum_members_by_unit.find(lc(std::string(unit)));
  if (uit == enum_members_by_unit.end()) return nullptr;
  auto mit = uit->second.find(lc(std::string(member)));
  return mit == uit->second.end() ? nullptr : mit->second;
}

const TypeExpr* TypeRegistry::canonicalize(
    const TypeExpr* te, std::string_view current_unit) const {
  int hops = 0;
  while (te && te->kind == Kind::TyName) {
    if (hops++ >= kMaxAliasChainHops) {
      throw std::runtime_error(
          "TypeRegistry::canonicalize: alias chain exceeds "
          "kMaxAliasChainHops; cycle or registry corruption");
    }
    const auto& n = static_cast<const TyName&>(*te);
    const TypeSymbol* symbol = lookup_type_symbol(n.name, current_unit);
    const AliasInfo* alias = symbol ? symbol->alias_info() : nullptr;
    if (!alias || !alias->target) {
      return te;
    }
    te = alias->target.get();
    current_unit = symbol->defining_unit;
  }
  return te;
}

std::string TypeRegistry::pointer_target_type_name(
    const TypeExpr* te, std::string_view current_unit) const {
  te = canonicalize(te, current_unit);
  if (!te || te->kind != Kind::TyPointer) return {};
  const auto& p = static_cast<const TyPointer&>(*te);
  const TypeExpr* tgt = p.target.get();
  if (!tgt) return {};
  // Target may itself be a TyName.
  if (tgt->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*tgt);
    if (const TypeSymbol* symbol = lookup_type_symbol(n.name, current_unit)) {
      return type_symbol_path(*symbol);
    }
    return lc(n.name);
  }
  // Target could be an inline struct. Not useful here.
  return {};
}

std::string TypeRegistry::direct_type_name(
    const TypeExpr* te, std::string_view current_unit) const {
  if (!te) return {};
  if (te->kind == Kind::TyName) {
    const auto& n = static_cast<const TyName&>(*te);
    if (const TypeSymbol* symbol = lookup_type_symbol(n.name, current_unit)) {
      return type_symbol_path(*symbol);
    }
    return lc(n.name);
  }
  return {};
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
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
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
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
  }
  return false;
}

const std::vector<MethodSig>* TypeRegistry::lookup_class_methods(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  // Walk the class chain looking for `member`, consulting translated type
  // symbols first; when the chain bottoms out into a runtime parent such as
  // `Exception`, continue into `rt_classes` so inherited runtime methods still
  // resolve.
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  if (const InterfaceInfo* interface =
          lookup_interface(class_name_in, current_unit)) {
    auto mit = interface->methods.find(key);
    return mit == interface->methods.end() ? nullptr : &mit->second;
  }
  SeenClassChain seen;
  std::string rt_name;
  while (ci && seen.mark(ci)) {
    auto mit = ci->methods.find(key);
    if (mit != ci->methods.end()) return &mit->second;
    if (ci->parent.empty() && ci->is_reference_type) {
      rt_name = "tobject";
      break;
    }
    const ClassInfo* next = lookup_class(ci->parent, ci->defining_unit);
    if (!next) {
      rt_name = ci->parent;
      break;
    }
    ci = next;
  }
  while (!rt_name.empty()) {
    auto cit = rt_classes.find(rt_name);
    if (cit == rt_classes.end() || !seen.mark(&cit->second)) break;
    auto mit = cit->second.methods.find(key);
    if (mit != cit->second.methods.end()) return &mit->second;
    rt_name = cit->second.parent;
  }
  return nullptr;
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
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
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
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
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
