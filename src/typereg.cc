#include "typereg.h"

#include <algorithm>
#include <stdexcept>

namespace tp2cc {

using namespace ast;

namespace {

std::string lc(std::string s) {
  for (auto& ch : s)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  return s;
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
      for (const auto& n : m.field_names) ci.fields[lc(n)] = fi;
    } else if (m.kind == ObjectMemberKind::Method && m.method) {
      const auto& pd = *m.method;
      MethodSig ms;
      ms.decl = m.method;
      ms.is_virtual = pd.is_virtual;
      ms.is_function = (pd.pkind == ProcKind::Function);
      if (pd.pkind == ProcKind::Constructor) ms.kind = SymKind::Constructor;
      else if (pd.pkind == ProcKind::Destructor) ms.kind = SymKind::Destructor;
      else if (pd.is_class_method) ms.kind = SymKind::ClassMethod;
      else ms.kind = SymKind::Method;
      size_t pc = 0;
      for (const auto& p : pd.params) pc += p.names.size();
      ms.param_count = pc;
      ci.methods[lc(pd.name)] = ms;
    } else if (m.kind == ObjectMemberKind::Property) {
      PropertyInfo pi;
      pi.type = m.property.type;
      pi.params = m.property.params;
      pi.read_name = lc(m.property.read_name);
      pi.write_name = lc(m.property.write_name);
      pi.is_default = m.property.is_default;
      std::string name = lc(m.property.name);
      ci.properties[name] = pi;
      if (pi.is_default) ci.default_property_name = name;
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
          add_class_members(ci, to);
          r.classes[nm] = std::move(ci);
        } else if (td.type->kind == Kind::TyRecord) {
          RecordInfo ri;
          ri.name = nm;
          ri.defining_unit = unit;
          add_record_fields(ri, static_cast<const TyRecord&>(*td.type));
          r.records[nm] = std::move(ri);
        } else if (td.type->kind == Kind::TyEnum) {
          EnumInfoReg ei;
          ei.name = nm;
          ei.defining_unit = unit;
          for (const auto& m : static_cast<const TyEnum&>(*td.type).members) {
            std::string lm = lc(m);
            ei.members.push_back(lm);
            r.enum_members[lm] = unit;
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
        ProcInfo p;
        p.defining_unit = unit;
        p.decl = pd_sp;
        p.is_function = (pd.pkind == ProcKind::Function);
        size_t pc = 0;
        for (const auto& par : pd.params) pc += par.names.size();
        p.param_count = pc;
        r.procs[lc(pd.name)] = p;
        if (ui) (is_interface ? ui->iface_procs : ui->impl_procs)[lc(pd.name)] = p;
        break;
      }
      case Kind::VarDecl: {
        const auto& vd = static_cast<const VarDecl&>(*d);
        VarInfo v;
        v.defining_unit = unit;
        v.type = vd.type;
        for (const auto& n : vd.names) {
          r.vars[lc(n)] = v;
          if (ui) (is_interface ? ui->iface_vars : ui->impl_vars)[lc(n)] = v;
        }
        break;
      }
      case Kind::ConstDecl: {
        const auto& cd = static_cast<const ConstDecl&>(*d);
        ConstInfo c;
        c.defining_unit = unit;
        c.type = cd.type;
        c.value = cd.value;
        r.consts[lc(cd.name)] = c;
        if (ui)
          (is_interface ? ui->iface_consts : ui->impl_consts)[lc(cd.name)] = c;
        break;
      }
      default:
        break;
    }
  }
}

}  // namespace

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
      {"eof",        1, true,  true,  "boolean"},  // eof; or eof(f)
      {"eoln",       1, true,  true,  "boolean"},
      {"ioresult",   0, true,  false, "longint"},
      {"memavail",   0, true,  false, "longint"},
      {"heapavail",  0, true,  false, "longint"},
      {"maxavail",   0, true,  false, "longint"},
      {"paramcount", 0, true,  false, "longint"},
      {"dosexitcode",0, true,  false, "longint"},
      {"writeln",    1, false, true,  ""},
      {"write",      1, false, true,  ""},
      {"readln",     1, false, true,  ""},
      {"read",       1, false, true,  ""},
      {"halt",       1, false, true,  ""},
      {"swapvectors",0, false, false, ""},
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
    p.accepts_zero_args = b.zero_ok;
    p.return_type_name = b.ret;
    rtui.iface_procs[b.name] = p;
    procs[b.name] = p;
  }
  units["__rt__"] = std::move(rtui);

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
  }
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

const FieldInfo* TypeRegistry::lookup_class_field(
    const std::string& class_name_in, const std::string& member) const {
  std::string class_name = lc(class_name_in);
  std::string key = lc(member);
  std::unordered_set<std::string> seen;
  while (!class_name.empty() && !seen.count(class_name)) {
    seen.insert(class_name);
    auto cit = classes.find(class_name);
    if (cit == classes.end()) return nullptr;
    auto fit = cit->second.fields.find(key);
    if (fit != cit->second.fields.end()) return &fit->second;
    class_name = cit->second.parent;
  }
  return nullptr;
}

const MethodSig* TypeRegistry::lookup_class_method(
    const std::string& class_name_in, const std::string& member) const {
  std::string class_name = lc(class_name_in);
  std::string key = lc(member);
  std::unordered_set<std::string> seen;
  while (!class_name.empty() && !seen.count(class_name)) {
    seen.insert(class_name);
    auto cit = classes.find(class_name);
    if (cit == classes.end()) return nullptr;
    auto mit = cit->second.methods.find(key);
    if (mit != cit->second.methods.end()) return &mit->second;
    class_name = cit->second.parent;
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_class_property(
    const std::string& class_name_in, const std::string& member) const {
  std::string class_name = lc(class_name_in);
  std::string key = lc(member);
  std::unordered_set<std::string> seen;
  while (!class_name.empty() && !seen.count(class_name)) {
    seen.insert(class_name);
    auto cit = classes.find(class_name);
    if (cit == classes.end()) return nullptr;
    auto pit = cit->second.properties.find(key);
    if (pit != cit->second.properties.end()) return &pit->second;
    class_name = cit->second.parent;
  }
  return nullptr;
}

const PropertyInfo* TypeRegistry::lookup_default_property(
    const std::string& class_name_in) const {
  std::string class_name = lc(class_name_in);
  std::unordered_set<std::string> seen;
  while (!class_name.empty() && !seen.count(class_name)) {
    seen.insert(class_name);
    auto cit = classes.find(class_name);
    if (cit == classes.end()) return nullptr;
    if (!cit->second.default_property_name.empty()) {
      auto pit = cit->second.properties.find(cit->second.default_property_name);
      if (pit != cit->second.properties.end()) return &pit->second;
    }
    class_name = cit->second.parent;
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
