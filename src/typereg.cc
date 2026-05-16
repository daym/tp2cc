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

void register_enum_type(TypeRegistry& r, UnitInfo* ui, bool is_interface,
                        const std::string& unit, const std::string& key,
                        const std::string& cxx_name, const TyEnum& te) {
  std::vector<std::string> members;
  for (const auto& m : te.members) members.push_back(lc(m.name));
  auto& by_member = r.enum_members_by_unit[unit];
  for (const auto& member : members) by_member[member] = &te;
  r.enums[key] = EnumInfoReg{.name = key,
                             .defining_unit = unit,
                             .type = &te,
                             .cxx_name = cxx_name,
                             .members = std::move(members)};
  r.enum_type_names[&te] = key;
  add_unit_enum_members(ui, is_interface, te);
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
    register_enum_type(r, ui, is_interface, unit, key, cxx_name, *te);
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
      const FieldInfo field{.type = f.type, .is_class_var = false};
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
                    .fields = record_fields(tr)};
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

ClassInfo* lookup_class_exact_mut(TypeRegistry& r, std::string_view unit,
                                  std::string_view name) {
  const std::string target_unit = lc(std::string(unit));
  const std::string target_name = lc(std::string(name));
  auto it = r.classes.find(target_name);
  if (it == r.classes.end()) return nullptr;
  for (auto& ci : it->second) {
    if (ci.defining_unit == target_unit) return &ci;
  }
  return nullptr;
}

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

  const FieldInfo* field =
      r.lookup_class_field(owner.name, path.front(), owner.defining_unit);
  if (!field) return std::nullopt;

  std::string out = r.field_cxx_name(path.front());
  const TypeExpr* current_type = field->type.get();
  for (size_t i = 1; i < path.size(); ++i) {
    const std::string nested_owner = r.direct_type_name(current_type);
    if (nested_owner.empty()) return std::nullopt;

    const FieldInfo* nested = nullptr;
    std::string access = ".";
    if (const ClassInfo* nested_class =
            r.lookup_class(nested_owner, owner.defining_unit)) {
      access = nested_class->is_reference_type ? "->" : ".";
      nested =
          r.lookup_class_field(nested_owner, path[i], owner.defining_unit);
    } else {
      nested = r.lookup_record_field(nested_owner, path[i]);
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

  if (auto cxx_path = resolve_field_accessor_cxx(r, owner, path)) {
    return PropertyAccessorInfo{.kind = PropertyAccessorKind::FieldPath,
                                .path = std::move(path),
                                .cxx_path = *cxx_path,
                                .method_name = {}};
  }

  if (path.size() == 1 &&
      r.lookup_class_methods(owner.name, path.front(), owner.defining_unit)) {
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

void resolve_property_accessors_from_decls(TypeRegistry& r,
                                           std::string_view unit,
                                           const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (!d || d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type || td.type->kind != Kind::TyObject) continue;

    ClassInfo* ci = lookup_class_exact_mut(r, unit, td.name);
    if (!ci) continue;
    const auto& to = static_cast<const TyObject&>(*td.type);
    for (const auto& member : to.members) {
      if (member.kind != ObjectMemberKind::Property) continue;
      auto pit = ci->properties.find(lc(member.property.name));
      if (pit == ci->properties.end()) continue;
      pit->second.read =
          resolve_property_accessor(r, *ci, member.property.read_accessor);
      pit->second.write =
          resolve_property_accessor(r, *ci, member.property.write_accessor);
    }
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
        std::string nm = lc(td.name);
        if (ui) (is_interface ? ui->iface_types : ui->impl_types).insert(nm);
        if (td.type->kind == Kind::TyObject) {
          const auto& to = static_cast<const TyObject&>(*td.type);
          ClassInfo ci = class_info_for(unit, nm, to);
          auto& bucket = r.classes[nm];
          auto same_unit = std::find_if(
              bucket.begin(), bucket.end(), [&](const ClassInfo& existing) {
                return existing.defining_unit == unit;
              });
          if (same_unit == bucket.end()) {
            bucket.push_back(std::move(ci));
          } else if (!ci.is_forward || same_unit->is_forward) {
            // A same-unit forward class declaration is only a placeholder for
            // the later body. Keep one registry entry per Pascal class identity
            // so member lookups do not hit an empty forward shell.
            *same_unit = std::move(ci);
          }
        } else if (td.type->kind == Kind::TyInterface) {
          const auto& ti = static_cast<const TyInterface&>(*td.type);
          r.interfaces[nm] = interface_info_for(unit, nm, ti);
        } else if (td.type->kind == Kind::TyRecord) {
          const auto& tr = static_cast<const TyRecord&>(*td.type);
          r.records[nm] = record_info_for(unit, nm, tr);
        } else if (td.type->kind == Kind::TyEnum) {
          const auto& te = static_cast<const TyEnum&>(*td.type);
          register_enum_type(r, ui, is_interface, unit, nm, type_mangle(nm),
                             te);
        } else {
          // Alias (possibly pointer / array / primitive).
          r.aliases[nm] = AliasInfo{.defining_unit = unit,
                                    .target = td.type};
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
        if (ui) (is_interface ? ui->iface_procs : ui->impl_procs)[lc(pd.name)]
                    .push_back(make_proc_info(unit, pd_sp));
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
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
  type_names.insert(ui.iface_types.begin(), ui.iface_types.end());
  type_names.insert(ui.impl_types.begin(), ui.impl_types.end());

  std::unordered_set<std::string> value_names;
  auto add_keys = [&](const auto& map) {
    for (const auto& [name, _] : map) {
      (void)_;
      value_names.insert(name);
    }
  };
  add_keys(ui.iface_vars);
  add_keys(ui.impl_vars);
  add_keys(ui.iface_consts);
  add_keys(ui.impl_consts);
  add_keys(ui.iface_procs);
  add_keys(ui.impl_procs);
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

  // Register the rt-side reference classes (tobject, exception, ...) so
  // the normal class-method lookup walks Pascal's parent chain into
  // them. Without this, a translated class that inherits Create from these
  // (e.g. `EListError = class(Exception)` calling
  // `EListError.Create(msg)`) doesn't resolve to a constructor -- the
  // constructor-call lowering falls back to a plain method call, which
  // fails because the rt method is non-static.
  //
  // Each MethodSig carries a synthesized ProcDecl with real params. Emit code
  // reads `decl->params` through the same path used for Pascal declarations.
  auto make_typename = [](const std::string& n) {
    return std::make_shared<ast::TyName>(n);
  };
  auto make_proc_param = [&](const std::string& name, ast::TypePtr type) {
    return ast::Param(ast::Param::Const, {name}, std::move(type));
  };
  auto make_rt_proc_decl =
      [](ast::ProcKind pkind, std::string name, std::vector<ast::Param> params,
         ast::TypePtr return_type = nullptr, bool class_method = false,
         bool is_operator = false, std::string operator_token = {},
         ast::ProcDecl::IntrinsicOperator intrinsic_operator =
             ast::ProcDecl::IntrinsicOperator::None) {
        return std::make_shared<ast::ProcDecl>(
            Location{}, pkind, std::move(name), is_operator,
            std::move(operator_token), intrinsic_operator, std::string{},
            class_method, std::move(params), std::move(return_type),
            ast::ProcModifiers{}, std::vector<ast::DeclPtr>{},
            nullptr);
  };
  auto add_rt_string_compare_operator =
      [&](const std::string& op, ast::TypePtr lhs_type, ast::TypePtr rhs_type,
          ast::TypePtr return_type) {
        auto pd = make_rt_proc_decl(
            ast::ProcKind::Function, "operator_" + op,
            {make_proc_param("a", std::move(lhs_type)),
             make_proc_param("b", std::move(rhs_type))},
            std::move(return_type), false, true, op,
            ast::ProcDecl::IntrinsicOperator::StringCompare);
        rt_exports.iface_operators[op].push_back(make_proc_info("__rt__", pd));
      };
  auto make_pointer = [](ast::TypePtr target = nullptr) {
    return std::make_shared<ast::TyPointer>(Location{}, std::move(target));
  };
  auto make_field = [](const std::string& name,
                       ast::TypePtr type) {
    return ast::RecordField({name}, std::move(type));
  };
  auto make_record = [](std::vector<ast::RecordField> fields) {
    return std::make_shared<ast::TyRecord>(
        Location{}, std::move(fields), nullptr, false);
  };
  auto make_set = [](ast::TypePtr element) {
    return std::make_shared<ast::TySet>(Location{}, std::move(element));
  };
  auto make_procedural = [](bool is_function, std::vector<ast::Param> params,
                            ast::TypePtr return_type = nullptr) {
    return std::make_shared<ast::TyProcedural>(
        Location{}, is_function, std::move(params), std::move(return_type),
        false, false);
  };
  auto make_enum = [](std::vector<std::string> names) {
    std::vector<ast::EnumMember> members;
    for (auto& name : names) {
      members.push_back(ast::EnumMember(std::move(name)));
    }
    return std::make_shared<ast::TyEnum>(Location{}, 4, std::move(members));
  };
  auto make_int_lit = [](int64_t value) {
    return std::make_shared<ast::IntLit>(static_cast<uint64_t>(value));
  };
  auto add_rt_var = [&](const std::string& name,
                        std::shared_ptr<const ast::TypeExpr> type) {
    rt_exports.iface_vars[lc(name)] =
        VarInfo{.defining_unit = "__rt__", .type = std::move(type)};
  };
  auto add_rt_const = [&](const std::string& name,
                          std::shared_ptr<const ast::TypeExpr> type,
                          std::shared_ptr<const ast::Expr> value) {
    rt_exports.iface_consts[lc(name)] =
        ConstInfo{.defining_unit = "__rt__",
                  .type = std::move(type),
                  .value = std::move(value)};
  };
  auto add_rt_alias = [&](const std::string& name,
                          std::shared_ptr<const ast::TypeExpr> target) {
    std::string low = lc(name);
    rt_exports.iface_types.insert(low);
    if (target && target->kind == Kind::TyEnum) {
      const auto* enum_type = static_cast<const TyEnum*>(target.get());
      std::vector<std::string> members;
      auto& by_member = enum_members_by_unit["__rt__"];
      for (const auto& m : static_cast<const TyEnum&>(*target).members) {
        std::string lm = lc(m.name);
        by_member[lm] = enum_type;
        members.push_back(lm);
        rt_exports.iface_enum_members.insert(lm);
      }
      enum_type_names[enum_type] = low;
      enums[low] = EnumInfoReg{.name = low,
                               .defining_unit = "__rt__",
                               .type = enum_type,
                               .cxx_name = type_mangle(low),
                               .members = std::move(members)};
    }
    aliases[low] =
        AliasInfo{.defining_unit = "__rt__", .target = std::move(target)};
  };

  // Runtime globals/constants that old compiler trees refer to directly.
  add_rt_var("doserror", make_typename("longint"));
  add_rt_var("filemode", make_typename("longint"));
  add_rt_var("stderr", make_typename("text"));
  add_rt_var("output", make_typename("text"));
  add_rt_var("input", make_typename("text"));
  add_rt_var("exitproc", make_procedural(false, {}));
  add_rt_var("erroraddr", make_typename("pointer"));
  add_rt_var("exitcode", make_typename("longint"));

  add_rt_const("sigint", make_typename("longint"), make_int_lit(2));
  add_rt_const("sigfpe", make_typename("longint"), make_int_lit(8));
  add_rt_const("sigsegv", make_typename("longint"), make_int_lit(11));
  add_rt_const("pi", make_typename("double"), nullptr);
  add_rt_const("maxint", make_typename("longint"), make_int_lit(2147483647));
  add_rt_const("readonly", make_typename("longint"), make_int_lit(0x01));
  add_rt_const("hidden", make_typename("longint"), make_int_lit(0x02));
  add_rt_const("directory", make_typename("longint"), make_int_lit(0x10));
  add_rt_const("archive", make_typename("longint"), make_int_lit(0x20));
  add_rt_const("fareadonly", make_typename("longint"), make_int_lit(0x01));
  add_rt_const("fahidden", make_typename("longint"), make_int_lit(0x02));
  add_rt_const("fadirectory", make_typename("longint"), make_int_lit(0x10));
  add_rt_const("faarchive", make_typename("longint"), make_int_lit(0x20));
  add_rt_const("anyfile", make_typename("longint"), make_int_lit(0x3F));
  add_rt_const("faanyfile", make_typename("longint"), make_int_lit(0x3F));
  add_rt_const("fmsharedenynone", make_typename("longint"), make_int_lit(0x40));
  add_rt_const("vtansistring", make_typename("longint"), make_int_lit(11));
  add_rt_const("varstrarg", make_typename("longint"), make_int_lit(0x48));
  add_rt_const("directoryseparator", make_typename("char"), nullptr);
  add_rt_const("driveseparator", make_typename("char"), nullptr);
  add_rt_const("pathseparator", make_typename("char"), nullptr);
  add_rt_const("maxlongint", make_typename("longint"),
               make_int_lit(2147483647));

  // Runtime type names that Pascal code can mention directly are registered as
  // aliases so casts and member lookups go through normal type analysis.
  add_rt_alias("signalhandler", make_pointer());
  add_rt_alias("tfpuexception",
               make_enum({"exinvalidop", "exdenormalized", "exzerodivide",
                          "exoverflow", "exunderflow", "exprecision"}));
  add_rt_alias("searchrec", make_record({
      make_field("time", make_typename("longint")),
      make_field("size", make_typename("longint")),
      make_field("attr", make_typename("byte")),
      make_field("name", make_typename("shortstring")),
  }));
  add_rt_alias("stat", make_record({
      make_field("mtime", make_typename("longint")),
      make_field("st_mtime", make_typename("longint")),
      make_field("mode", make_typename("longint")),
      make_field("st_mode", make_typename("longint")),
      make_field("size", make_typename("longint")),
      make_field("st_size", make_typename("longint")),
  }));
  add_rt_alias("datetime", make_record({
      make_field("year", make_typename("word")),
      make_field("month", make_typename("word")),
      make_field("day", make_typename("word")),
      make_field("hour", make_typename("word")),
      make_field("min", make_typename("word")),
      make_field("sec", make_typename("word")),
  }));
  add_rt_alias("tdatetime", make_typename("double"));
  add_rt_alias("dirstr", make_typename("shortstring"));
  add_rt_alias("namestr", make_typename("shortstring"));
  add_rt_alias("extstr", make_typename("shortstring"));
  add_rt_alias("pathstr", make_typename("shortstring"));
  add_rt_alias("comstr", make_typename("shortstring"));
  add_rt_alias("texecuteflag", make_enum({"execinheritshandles"}));
  add_rt_alias("texecuteflags", make_set(make_typename("texecuteflag")));
  add_rt_alias("tfpuexceptionmask", make_set(make_typename("tfpuexception")));
  add_rt_alias("tsyscharset", make_set(make_typename("char")));
  add_rt_alias("hresult", make_typename("longint"));
  add_rt_alias("ansichar", make_typename("char"));
  add_rt_alias("pansichar", make_pointer(make_typename("ansichar")));
  add_rt_alias("pcardinal", make_pointer(make_typename("cardinal")));
  add_rt_alias("pcurrency", make_pointer(make_typename("currency")));
  add_rt_alias("pdword", make_pointer(make_typename("dword")));
  add_rt_alias("pint64", make_pointer(make_typename("int64")));
  add_rt_alias("plongword", make_pointer(make_typename("longword")));
  add_rt_alias("ppointer", make_pointer(make_typename("pointer")));
  add_rt_alias("pqword", make_pointer(make_typename("qword")));
  add_rt_alias("pshortstring", make_pointer(make_typename("shortstring")));
  const std::array<const char*, 6> string_compare_ops{
      "=", "<>", "<", ">", "<=", ">="};
  for (const char* op : string_compare_ops) {
    add_rt_string_compare_operator(op, make_typename("shortstring"),
                                   make_typename("shortstring"),
                                   make_typename("boolean"));
    add_rt_string_compare_operator(op, make_typename("shortstring"),
                                   make_typename("ansistring"),
                                   make_typename("boolean"));
    add_rt_string_compare_operator(op, make_typename("ansistring"),
                                   make_typename("shortstring"),
                                   make_typename("boolean"));
    add_rt_string_compare_operator(op, make_typename("ansistring"),
                                   make_typename("ansistring"),
                                   make_typename("boolean"));
  }
  add_rt_var("allowdirectoryseparators", make_set(make_typename("char")));
  auto make_method = [&](const std::string& name, ast::ProcKind pkind,
                         std::vector<ast::Param> params,
                         ast::TypePtr return_type = nullptr,
                         bool class_method = false) {
    auto pd = make_rt_proc_decl(pkind, name, std::move(params),
                                std::move(return_type), class_method);
    const size_t param_count = proc_param_count(pd->params);
    return MethodSig{
        .kind = (pkind == ast::ProcKind::Constructor) ? SymKind::Constructor
                : (pkind == ast::ProcKind::Destructor) ? SymKind::Destructor
                : class_method ? SymKind::ClassMethod
                               : SymKind::Method,
        .defining_unit = "__rt__",
        .param_count = param_count,
        .accepts_zero_args = (param_count == 0),
        .is_function = (pkind == ast::ProcKind::Function),
        .is_virtual = false,
        .is_final = false,
        .decl = std::move(pd)};
  };
  auto add_rt_class = [&](const std::string& name, const std::string& parent,
                          std::vector<MethodSig> methods) {
    std::unordered_map<std::string, std::vector<MethodSig>> method_map;
    for (auto& m : methods) {
      const std::string mname = m.decl->name;
      method_map[mname].push_back(std::move(m));
    }
    rt_classes[name] =
        ClassInfo{.name = name,
                  .parent = parent,
                  .defining_unit = "__rt__",
                  .is_reference_type = true,
                  .is_abstract = false,
                  .is_forward = false,
                  .fields = {},
                  .methods = std::move(method_map),
                  .properties = {},
                  .enum_members = {},
                  .default_property_name = {}};
  };

  const ast::Param inh_aclass =
      make_proc_param("aclass", make_typename("tclass"));
  add_rt_class("tobject", "",
               {make_method("create",  ast::ProcKind::Constructor, {}),
                make_method("destroy", ast::ProcKind::Destructor,  {}),
                make_method("free",    ast::ProcKind::Procedure,   {}),
                make_method("classname", ast::ProcKind::Function, {},
                            make_typename("shortstring"),
                            /*class_method=*/true),
                // Pascal RTL declares these as `class function`. They live in
                // the runtime header (t_tobject::p_inheritsfrom etc.); register
                // them so the resolver/deduce path treats them like any other
                // method instead of needing a separate
                // `builtin_reference_class_member_type` shim.
                make_method("classtype", ast::ProcKind::Function, {},
                            make_typename("tclass"),
                            /*class_method=*/true),
                make_method("instancesize", ast::ProcKind::Function, {},
                            make_typename("longint"),
                            /*class_method=*/true),
                make_method("inheritsfrom", ast::ProcKind::Function,
                            {inh_aclass},
                            make_typename("boolean"),
                            /*class_method=*/true)});

  const ast::Param exc_msg = make_proc_param("msg", make_typename("shortstring"));
  add_rt_class("exception", "tobject",
               {make_method("create", ast::ProcKind::Constructor,
                            {exc_msg})});
  add_rt_class("eexternal", "exception", {});
  add_rt_class("einterror", "eexternal", {});
  add_rt_class("einouterror", "exception", {});
  add_rt_class("eheapmemoryerror", "exception", {});
  add_rt_class("eheapexception", "eheapmemoryerror", {});
  add_rt_class("eoutofmemory", "eheapmemoryerror", {});
  add_rt_class("eintoverflow", "einterror", {});
  add_rt_class("erangeerror", "einterror", {});
  add_rt_class("edivbyzero", "einterror", {});
  add_rt_class("eoserror", "exception", {});

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
    auto add_external_stub = [&](const std::string& used_name) {
      const std::string low = lc(used_name);
      if (low == "__rt__" || units.count(low) > 0) return;
      // UnitGraph/main.cc already treat missing `uses` entries as external RTL
      // units and emit `p_<unit>.h` via write_external_stub. Mirror that fact
      // in the semantic registry with an empty UnitInfo: the unit has no parsed
      // exports, but `Unit.name` is still a valid qualified spelling and
      // EmitLookup can fall back to the generated stub namespace.
      units[low] = unit_info_for(low);
    };
    for (const auto& nm : u->interface_uses) add_external_stub(nm);
    for (const auto& nm : u->impl_uses) add_external_stub(nm);
  }

  for (const auto* u : us) {
    if (!u) continue;
    const std::string unit = lc(u->name);
    resolve_property_accessors_from_decls(*this, unit, u->interface_decls);
    resolve_property_accessors_from_decls(*this, unit, u->impl_decls);
  }
}

const ClassInfo* TypeRegistry::lookup_class_exact(std::string_view unit,
                                                  std::string_view name) const {
  std::string target_unit = lc(std::string(unit));
  std::string target_name = lc(std::string(name));
  auto it = classes.find(target_name);
  if (it == classes.end()) return nullptr;
  for (const auto& ci : it->second) {
    if (ci.defining_unit == target_unit) return &ci;
  }
  return nullptr;
}

const ClassInfo* TypeRegistry::lookup_class(std::string_view name,
                                            std::string_view current_unit) const {
  std::string low = lc(std::string(name));
  if (auto dot = low.find('.'); dot != std::string::npos) {
    return lookup_class_exact(low.substr(0, dot), low.substr(dot + 1));
  }

  std::string cur_unit = lc(std::string(current_unit));
  if (!cur_unit.empty()) {
    if (const ClassInfo* own = lookup_class_exact(cur_unit, low)) return own;
    auto uit = units.find(cur_unit);
    if (uit != units.end()) {
      for (auto use = uit->second.uses.rbegin(); use != uit->second.uses.rend();
           ++use) {
        if (*use == "__rt__") continue;
        auto used = units.find(*use);
        if (used == units.end()) continue;
        if (used->second.has_export_type(low)) {
          if (const ClassInfo* exported = lookup_class_exact(*use, low)) {
            return exported;
          }
        }
      }
    }
  }

  auto it = classes.find(low);
  if (it == classes.end() || it->second.empty()) return nullptr;
  if (it->second.size() == 1) return &it->second.front();
  return nullptr;
}

const EnumInfoReg* TypeRegistry::enum_info_for_type(
    const TyEnum* type) const {
  if (!type) return nullptr;
  auto kit = enum_type_names.find(type);
  if (kit == enum_type_names.end()) return nullptr;
  auto eit = enums.find(kit->second);
  return eit == enums.end() ? nullptr : &eit->second;
}

const TyEnum* TypeRegistry::lookup_enum_member_in_unit(
    std::string_view unit, std::string_view member) const {
  auto uit = enum_members_by_unit.find(lc(std::string(unit)));
  if (uit == enum_members_by_unit.end()) return nullptr;
  auto mit = uit->second.find(lc(std::string(member)));
  return mit == uit->second.end() ? nullptr : mit->second;
}

const TypeExpr* TypeRegistry::canonicalize(const TypeExpr* te) const {
  int hops = 0;
  while (te && te->kind == Kind::TyName) {
    if (hops++ >= kMaxAliasChainHops) {
      throw std::runtime_error(
          "TypeRegistry::canonicalize: alias chain exceeds "
          "kMaxAliasChainHops; cycle or registry corruption");
    }
    const auto& n = static_cast<const TyName&>(*te);
    std::string low = lc(n.name);
    auto it = aliases.find(low);
    if (it == aliases.end()) {
      if (auto dot = low.find('.'); dot != std::string::npos) {
        // Default-argument lowering can qualify a formal type as `unit.alias`
        // because the C++ default value is emitted at a caller in another
        // unit. Alias lookup is keyed by Pascal type name plus defining unit,
        // so strip the qualifier only after verifying it names that unit.
        auto tail_it = aliases.find(low.substr(dot + 1));
        if (tail_it != aliases.end() &&
            tail_it->second.defining_unit == low.substr(0, dot)) {
          it = tail_it;
        }
      }
    }
    if (it == aliases.end()) return te;  // no registered alias target
    te = it->second.target.get();
  }
  return te;
}

std::string TypeRegistry::pointer_target_type_name(const TypeExpr* te) const {
  te = canonicalize(te);
  if (!te || te->kind != Kind::TyPointer) return {};
  const auto& p = static_cast<const TyPointer&>(*te);
  const TypeExpr* tgt = p.target.get();
  if (!tgt) return {};
  // Target may itself be a TyName.
  if (tgt->kind == Kind::TyName) {
    return lc(static_cast<const TyName&>(*tgt).name);
  }
  // Target could be an inline struct. Not useful here.
  return {};
}

std::string TypeRegistry::direct_type_name(const TypeExpr* te) const {
  if (!te) return {};
  if (te->kind == Kind::TyName) return lc(static_cast<const TyName&>(*te).name);
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
  std::unordered_set<std::string> seen;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
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
  std::unordered_set<std::string> seen;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    if (ci->enum_members.count(key)) return true;
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
  }
  return false;
}

const std::vector<MethodSig>* TypeRegistry::lookup_class_methods(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  // Walk the class chain looking for `member`, consulting translated
  // classes first; when the chain bottoms out into a name not in `classes`
  // (e.g. `Exception`, the parent of a translated `EFoo`), continue into
  // `rt_classes` so methods inherited from rt-side classes (like
  // `Exception.Create(string)`) still resolve. The lookup walks both
  // stores; code-gen iterates only `classes`.
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string class_name = lc(class_name_in);
  if (auto dot = class_name.find('.'); dot != std::string::npos) {
    class_name = class_name.substr(dot + 1);
  }
  std::string key = lc(member);
  auto iit = interfaces.find(class_name);
  if (iit != interfaces.end()) {
    auto mit = iit->second.methods.find(key);
    return mit == iit->second.methods.end() ? nullptr : &mit->second;
  }
  std::unordered_set<std::string> seen;
  auto step = [&](const std::string& class_name,
                  const std::unordered_map<std::string, ClassInfo>& store)
      -> std::pair<const std::vector<MethodSig>*, std::string> {
    auto cit = store.find(class_name);
    if (cit == store.end()) return {nullptr, std::string{}};
    auto mit = cit->second.methods.find(key);
    if (mit != cit->second.methods.end()) {
      return {&mit->second, std::string{}};
    }
    return {nullptr, cit->second.parent};
  };
  std::string rt_name;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
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
  while (!rt_name.empty() && !seen.count("__rt__." + rt_name)) {
    seen.insert("__rt__." + rt_name);
    auto [rt_hit, rt_parent] = step(rt_name, rt_classes);
    if (rt_hit) return rt_hit;
    rt_name = rt_parent;
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_class_property(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::string key = lc(member);
  std::unordered_set<std::string> seen;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    auto pit = ci->properties.find(key);
    if (pit != ci->properties.end()) return &pit->second;
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_default_property(
    const std::string& class_name_in, std::string_view current_unit) const {
  const ClassInfo* ci = lookup_class(class_name_in, current_unit);
  std::unordered_set<std::string> seen;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    if (!ci->default_property_name.empty()) {
      auto pit = ci->properties.find(ci->default_property_name);
      if (pit != ci->properties.end()) return &pit->second;
    }
    ci = ci->parent.empty() ? nullptr : lookup_class(ci->parent, ci->defining_unit);
  }
  return nullptr;
}

const FieldInfo* TypeRegistry::lookup_record_field(
    const std::string& record_name, const std::string& member) const {
  auto it = records.find(lc(record_name));
  if (it == records.end()) return nullptr;
  auto fit = it->second.fields.find(lc(member));
  if (fit == it->second.fields.end()) return nullptr;
  return &fit->second;
}

}  // namespace tp2cc
