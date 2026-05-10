#include "typereg.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

#include "diag.h"
#include "emit_support.h"

namespace tp2cc {

using namespace ast;

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

ProcInfo make_proc_info(const std::string& unit,
                        std::shared_ptr<const ProcDecl> pd_sp) {
  const auto& pd = *pd_sp;
  ProcInfo p;
  p.defining_unit = unit;
  p.decl = pd_sp;
  p.is_function = (pd.pkind == ProcKind::Function);
  size_t pc = 0;
  for (const auto& par : pd.params) {
    pc += par.names.empty() ? 1 : par.names.size();
  }
  p.param_count = pc;
  p.accepts_zero_args = proc_accepts_zero_args(pd);
  return p;
}

void add_record_fields(RecordInfo& ri, const TyRecord& tr) {
  for (const auto& f : tr.fields) {
    FieldInfo fi;
    fi.type = f.type;
    for (const auto& n : f.names) ri.fields[lc(n)] = fi;
  }
  // Variant-record tag (`case typ : toptype of ...`) -- `typ` is an
  // actual field in the emitted struct, so it must be indexed as one.
  if (tr.has_variant && !tr.variant_tag_name.empty()) {
    FieldInfo fi;
    fi.type = tr.variant_tag_type;
    ri.fields[lc(tr.variant_tag_name)] = fi;
  }
  for (const auto& vc : tr.variant_cases) {
    for (const auto& f : vc.fields) {
      FieldInfo fi;
      fi.type = f.type;
      for (const auto& n : f.names) ri.fields[lc(n)] = fi;
    }
  }
}

void add_class_members(ClassInfo& ci, const TyObject& to) {
  for (const auto& m : to.members) {
    if (m.kind == ObjectMemberKind::Field) {
      FieldInfo fi;
      fi.type = m.field_type;
      fi.is_class_var = m.is_class_var;
      for (const auto& n : m.field_names) ci.fields[lc(n)] = fi;
      // Inline anonymous enum used as the field type contributes its
      // members to the enclosing class scope -- they're visible by bare
      // identifier inside the class's member functions, just like any
      // class constant.
      if (m.field_type && m.field_type->kind == Kind::TyEnum) {
        const auto& te = static_cast<const TyEnum&>(*m.field_type);
        for (const auto& em : te.members) ci.enum_members.insert(lc(em.name));
      }
    } else if (m.kind == ObjectMemberKind::Method && m.method) {
      const auto& pd = *m.method;
      MethodSig ms;
      ms.decl = m.method;
      ms.is_virtual = pd.is_virtual || pd.is_abstract || pd.is_override;
      ms.is_final = pd.is_final;
      ms.is_function = (pd.pkind == ProcKind::Function);
      if (pd.is_class_method) ms.kind = SymKind::ClassMethod;
      else if (pd.pkind == ProcKind::Constructor) ms.kind = SymKind::Constructor;
      else if (pd.pkind == ProcKind::Destructor) ms.kind = SymKind::Destructor;
      else ms.kind = SymKind::Method;
      size_t pc = 0;
      for (const auto& p : pd.params) pc += p.names.size();
      ms.param_count = pc;
      ms.accepts_zero_args = proc_accepts_zero_args(pd);
      ci.methods[lc(pd.name)].push_back(ms);
    } else if (m.kind == ObjectMemberKind::Property) {
      PropertyInfo pi;
      pi.type = m.property.type;
      pi.params = m.property.params;
      pi.is_default = m.property.is_default;
      std::string name = lc(m.property.name);
      ci.properties[name] = pi;
      if (pi.is_default) ci.default_property_name = name;
    }
  }
}

void add_interface_members(InterfaceInfo& ii, const TyInterface& ti) {
  for (const auto& m : ti.members) {
    if (m.kind != ObjectMemberKind::Method || !m.method) continue;
    const auto& pd = *m.method;
    MethodSig ms;
    ms.decl = m.method;
    ms.is_function = (pd.pkind == ProcKind::Function);
    ms.kind = SymKind::Method;
    size_t pc = 0;
    for (const auto& p : pd.params) pc += p.names.size();
    ms.param_count = pc;
    ms.accepts_zero_args = proc_accepts_zero_args(pd);
    ii.methods[lc(pd.name)].push_back(ms);
  }
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
  PropertyAccessorInfo out;
  out.path = lower_path(accessor);
  if (out.path.empty()) return out;

  if (auto cxx_path = resolve_field_accessor_cxx(r, owner, out.path)) {
    out.kind = PropertyAccessorKind::FieldPath;
    out.cxx_path = *cxx_path;
    return out;
  }

  if (out.path.size() == 1 &&
      r.lookup_class_methods(owner.name, out.path.front(), owner.defining_unit)) {
    out.kind = PropertyAccessorKind::Method;
    out.method_name = out.path.front();
    return out;
  }

  out.kind = PropertyAccessorKind::Unsupported;
  return out;
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
          ClassInfo ci;
          ci.name = nm;
          ci.defining_unit = unit;
          const auto& to = static_cast<const TyObject&>(*td.type);
          ci.parent = lc(to.parent);
          ci.is_reference_type = to.is_reference_type;
          ci.is_abstract = to.is_abstract;
          ci.is_forward = to.is_forward;
          add_class_members(ci, to);
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
          InterfaceInfo ii;
          ii.name = nm;
          ii.defining_unit = unit;
          const auto& ti = static_cast<const TyInterface&>(*td.type);
          ii.metadata_string = ti.metadata_string;
          add_interface_members(ii, ti);
          r.interfaces[nm] = std::move(ii);
        } else if (td.type->kind == Kind::TyRecord) {
          RecordInfo ri;
          ri.name = nm;
          ri.defining_unit = unit;
          const auto& tr = static_cast<const TyRecord&>(*td.type);
          ri.is_packed = tr.is_packed;
          add_record_fields(ri, tr);
          r.records[nm] = std::move(ri);
        } else if (td.type->kind == Kind::TyEnum) {
          EnumInfoReg ei;
          ei.name = nm;
          ei.defining_unit = unit;
          for (const auto& m : static_cast<const TyEnum&>(*td.type).members) {
            std::string lm = lc(m.name);
            ei.members.push_back(lm);
            if (ui)
              (is_interface ? ui->iface_enum_members : ui->impl_enum_members)
                  .insert(lm);
          }
          r.enums[nm] = std::move(ei);
        } else {
          // Alias (possibly pointer / array / primitive).
          AliasInfo a;
          a.defining_unit = unit;
          a.target = td.type;
          r.aliases[nm] = a;
        }
        break;
      }
      case Kind::ProcDecl: {
        auto pd_sp = std::static_pointer_cast<const ProcDecl>(d);
        const auto& pd = *pd_sp;
        if (!pd.of_type.empty()) continue;  // method body -- class handles it
        if (pd.is_operator) {
          if (pd.is_forward) break;
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
        if (pd.is_forward) break;
        ProcInfo p = make_proc_info(unit, pd_sp);
        if (ui) (is_interface ? ui->iface_procs : ui->impl_procs)[lc(pd.name)]
                    .push_back(p);
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        VarInfo v;
        v.defining_unit = unit;
        v.type = vd.type;
        for (const auto& n : vd.names) {
          if (ui) (is_interface ? ui->iface_vars : ui->impl_vars)[lc(n)] = v;
        }
        // Pascal: an inline anonymous enum used as a var-decl type
        // bleeds its members into the enclosing scope. Same rule as
        // class-field inline enums; without this, the resolver can't
        // find the members and falls through to the unknown-name
        // ::rt:: prefix.
        if (ui && vd.type && vd.type->kind == Kind::TyEnum) {
          const auto& te = static_cast<const TyEnum&>(*vd.type);
          for (const auto& em : te.members) {
            (is_interface ? ui->iface_enum_members : ui->impl_enum_members)
                .insert(lc(em.name));
          }
        }
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        ConstInfo c;
        c.defining_unit = unit;
        c.type = cd.type;
        c.value = cd.value;
        if (ui)
          (is_interface ? ui->iface_consts : ui->impl_consts)[lc(cd.name)] = c;
        // Same rule for typed consts: `const x : (a, b) = a;` -- if this
        // source construct lands in the source, the members must be visible.
        if (ui && cd.type && cd.type->kind == Kind::TyEnum) {
          const auto& te = static_cast<const TyEnum&>(*cd.type);
          for (const auto& em : te.members) {
            (is_interface ? ui->iface_enum_members : ui->impl_enum_members)
                .insert(lc(em.name));
          }
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
      {"ord",        1, true,  false, "longint"},
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
      {"heapavail",  0, true,  false, "longint"},
      {"maxavail",   0, true,  false, "longint"},
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
      {"reallocmem", 2, false, false, ""},
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
  // Synthetic unit "rt::" holds the builtins so lookups that walk
  // the uses chain can find them as an always-available fallback.
  UnitInfo rtui;
  rtui.name = "__rt__";
  for (const auto& b : rt_builtins) {
    ProcInfo p;
    p.defining_unit = "__rt__";
    p.decl = nullptr;
    p.param_count = b.params;
    p.is_function = b.is_fn;
    p.accepts_zero_args = b.zero_ok || b.params == 0;
    p.return_type_name = b.ret;
    rtui.iface_procs[b.name].push_back(p);
  }
  units["__rt__"] = std::move(rtui);
  UnitInfo& rt_exports = units["__rt__"];

  // Register the rt-side reference classes (tobject, exception, ...) so
  // the normal class-method lookup walks Pascal's parent chain into
  // them. Without this, a translated class that inherits Create from these
  // (e.g. `EListError = class(Exception)` calling
  // `EListError.Create(msg)`) doesn't resolve to a constructor -- the
  // constructor-call lowering falls back to a plain method call, which
  // fails because the rt method is non-static.
  //
  // Each MethodSig carries a synthesized ProcDecl with real params, so
  // consumers that walk `decl->params` (param_list_to_cxx,
  // procedural_param_types_to_cxx, ...) work uniformly. Without the
  // synthesized decl, every such consumer would need to special-case
  // null and we'd be back to ad-hoc.
  auto make_typename = [](const std::string& n) {
    auto t = std::make_shared<ast::TyName>();
    t->name = n;
    return t;
  };
  auto make_pointer = [](ast::TypePtr target = nullptr) {
    auto t = std::make_shared<ast::TyPointer>();
    t->target = std::move(target);
    return t;
  };
  auto make_field = [](const std::string& name,
                       ast::TypePtr type) {
    ast::RecordField f;
    f.names = {name};
    f.type = std::move(type);
    return f;
  };
  auto make_record = [](std::vector<ast::RecordField> fields) {
    auto t = std::make_shared<ast::TyRecord>();
    t->fields = std::move(fields);
    return t;
  };
  auto make_set = [](ast::TypePtr element) {
    auto t = std::make_shared<ast::TySet>();
    t->element = std::move(element);
    return t;
  };
  auto make_procedural = [](bool is_function, std::vector<ast::Param> params,
                            ast::TypePtr return_type = nullptr) {
    auto t = std::make_shared<ast::TyProcedural>();
    t->is_function = is_function;
    t->params = std::move(params);
    t->return_type = std::move(return_type);
    return t;
  };
  auto make_enum = [](std::vector<std::string> names) {
    auto t = std::make_shared<ast::TyEnum>();
    for (auto& name : names) {
      ast::EnumMember m;
      m.name = name;
      t->members.push_back(std::move(m));
    }
    return t;
  };
  auto make_int_lit = [](int64_t value) {
    auto e = std::make_shared<ast::IntLit>();
    e->value = static_cast<uint64_t>(value);
    return e;
  };
  auto add_rt_var = [&](const std::string& name,
                        std::shared_ptr<const ast::TypeExpr> type) {
    VarInfo v;
    v.defining_unit = "__rt__";
    v.type = std::move(type);
    rt_exports.iface_vars[lc(name)] = std::move(v);
  };
  auto add_rt_const = [&](const std::string& name,
                          std::shared_ptr<const ast::TypeExpr> type,
                          std::shared_ptr<const ast::Expr> value) {
    ConstInfo c;
    c.defining_unit = "__rt__";
    c.type = std::move(type);
    c.value = std::move(value);
    rt_exports.iface_consts[lc(name)] = std::move(c);
  };
  auto add_rt_alias = [&](const std::string& name,
                          std::shared_ptr<const ast::TypeExpr> target) {
    std::string low = lc(name);
    rt_exports.iface_types.insert(low);
    if (target && target->kind == Kind::TyEnum) {
      EnumInfoReg ei;
      ei.name = low;
      ei.defining_unit = "__rt__";
      for (const auto& m : static_cast<const TyEnum&>(*target).members) {
        std::string lm = lc(m.name);
        ei.members.push_back(lm);
        rt_exports.iface_enum_members.insert(lm);
      }
      enums[low] = std::move(ei);
    }
    AliasInfo a;
    a.defining_unit = "__rt__";
    a.target = std::move(target);
    aliases[low] = std::move(a);
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

  // Runtime type names that Pascal code can mention directly. Model them
  // explicitly instead of relying on the old unresolved-name -> ::rt::
  // fallback so casts/member lookups still go through normal type analysis.
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
  add_rt_alias("tfpuexceptionmask", make_set(make_typename("tfpuexception")));
  add_rt_alias("hresult", make_typename("longint"));
  add_rt_alias("pcardinal", make_pointer(make_typename("cardinal")));
  add_rt_alias("pcurrency", make_pointer(make_typename("currency")));
  add_rt_alias("pdword", make_pointer(make_typename("dword")));
  add_rt_alias("pint64", make_pointer(make_typename("int64")));
  add_rt_alias("ppointer", make_pointer(make_typename("pointer")));
  add_rt_alias("pqword", make_pointer(make_typename("qword")));
  add_rt_alias("pshortstring", make_pointer(make_typename("shortstring")));
  add_rt_var("allowdirectoryseparators", make_set(make_typename("char")));
  auto make_method = [&](const std::string& name, ast::ProcKind pkind,
                         std::vector<ast::Param> params,
                         ast::TypePtr return_type = nullptr,
                         bool class_method = false) {
    auto pd = std::make_shared<ast::ProcDecl>(class_method);
    pd->pkind = pkind;
    pd->name = name;
    pd->params = std::move(params);
    pd->return_type = std::move(return_type);
    MethodSig ms;
    ms.kind = (pkind == ast::ProcKind::Constructor) ? SymKind::Constructor
              : (pkind == ast::ProcKind::Destructor) ? SymKind::Destructor
              : class_method ? SymKind::ClassMethod
                                                     : SymKind::Method;
    size_t pc = 0;
    for (const auto& p : pd->params) pc += p.names.empty() ? 1 : p.names.size();
    ms.param_count = pc;
    ms.accepts_zero_args = (pc == 0);
    ms.decl = pd;
    return ms;
  };
  auto add_rt_class = [&](const std::string& name, const std::string& parent,
                          std::vector<MethodSig> methods) {
    ClassInfo ci;
    ci.name = name;
    ci.parent = parent;
    ci.defining_unit = "__rt__";
    ci.is_reference_type = true;
    for (auto& m : methods) {
      const std::string mname = m.decl->name;
      ci.methods[mname].push_back(std::move(m));
    }
    rt_classes[name] = std::move(ci);
  };

  ast::Param inh_aclass;
  inh_aclass.names = {"aclass"};
  inh_aclass.type = make_typename("tclass");
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

  ast::Param exc_msg;
  exc_msg.mode = ast::Param::Const;
  exc_msg.names = {"msg"};
  exc_msg.type = make_typename("shortstring");
  add_rt_class("exception", "tobject",
               {make_method("create", ast::ProcKind::Constructor,
                            {exc_msg})});

  for (const auto* u : us) {
    if (!u) continue;
    UnitInfo ui;
    ui.name = lc(u->name);
    for (const auto& nm : u->interface_uses) ui.uses.push_back(lc(nm));
    for (const auto& nm : u->impl_uses) ui.uses.push_back(lc(nm));
    // Every Pascal unit implicitly uses `System` -- we model rt as
    // that implicit last fallback on the uses chain.
    ui.uses.push_back("__rt__");
    units[lc(u->name)] = std::move(ui);

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
      UnitInfo stub;
      stub.name = low;
      units[low] = std::move(stub);
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

const TypeExpr* TypeRegistry::canonicalize(const TypeExpr* te) const {
  int hops = 0;
  while (te && te->kind == Kind::TyName) {
    if (hops++ >= kMaxAliasChainHops) {
      throw std::runtime_error(
          "TypeRegistry::canonicalize: alias chain exceeds "
          "kMaxAliasChainHops; cycle or registry corruption");
    }
    const auto& n = static_cast<const TyName&>(*te);
    auto it = aliases.find(lc(n.name));
    if (it == aliases.end()) return te;  // unknown alias; leave as-is
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

const MethodSig* TypeRegistry::lookup_class_method(
    const std::string& class_name_in, const std::string& member,
    std::string_view current_unit) const {
  if (auto* set = lookup_class_methods(class_name_in, member, current_unit);
      set && !set->empty()) {
    return &(*set)[0];
  }
  return nullptr;
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
