#include "emit_units.h"

#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "emit_support.h"

namespace tp2cc {

using namespace ast;

namespace {

struct ClassLifecycleHooks {
  std::vector<const ProcDecl*> constructors;
  std::vector<const ProcDecl*> destructors;
};

bool is_class_lifecycle_proc(const ProcDecl& pd) {
  return pd.is_class_method &&
         (pd.pkind == ProcKind::Constructor ||
          pd.pkind == ProcKind::Destructor);
}

ClassLifecycleHooks collect_class_lifecycle_hooks(
    const std::vector<DeclPtr>& decls) {
  ClassLifecycleHooks hooks;
  for (const auto& d : decls) {
    if (!d || d->kind != Kind::ProcDecl) continue;
    const auto& pd = static_cast<const ProcDecl&>(*d);
    if (pd.of_type.empty() || !is_class_lifecycle_proc(pd)) continue;
    if (pd.pkind == ProcKind::Constructor) {
      hooks.constructors.push_back(&pd);
    } else {
      hooks.destructors.push_back(&pd);
    }
  }
  return hooks;
}

std::string class_lifecycle_call_cxx(const ProcDecl& pd) {
  return type_mangle(pd.of_type) + "::" + mangle(pd.name);
}

// Collect every TyName (lowercased) mentioned in a TypeExpr. Recurses into
// records/objects so that a record's field types contribute dependencies.
void collect_type_refs(const TypeExpr& t, std::unordered_set<std::string>& out) {
  switch (t.kind) {
    case Kind::TyName:
      out.insert(static_cast<const TyName&>(t).name);
      return;
    case Kind::TyPointer:
      if (static_cast<const TyPointer&>(t).target) {
        collect_type_refs(*static_cast<const TyPointer&>(t).target, out);
      }
      return;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      for (const auto& d : a.dims)
        if (d) collect_type_refs(*d, out);
      if (a.element) collect_type_refs(*a.element, out);
      return;
    }
    case Kind::TySet:
      if (static_cast<const TySet&>(t).element) {
        collect_type_refs(*static_cast<const TySet&>(t).element, out);
      }
      return;
    case Kind::TyFile:
      if (static_cast<const TyFile&>(t).element) {
        collect_type_refs(*static_cast<const TyFile&>(t).element, out);
      }
      return;
    case Kind::TyRecord: {
      const auto& r = static_cast<const TyRecord&>(t);
      for (const auto& f : r.fields)
        if (f.type) collect_type_refs(*f.type, out);
      for (const auto& vc : r.variant_cases)
        for (const auto& f : vc.fields)
          if (f.type) collect_type_refs(*f.type, out);
      return;
    }
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      if (!o.parent.empty()) out.insert(o.parent);
      for (const auto& iface : o.interfaces) out.insert(iface);
      for (const auto& m : o.members) {
        if (m.kind == ObjectMemberKind::Field && m.field_type) {
          collect_type_refs(*m.field_type, out);
        } else if (m.kind == ObjectMemberKind::Method && m.method) {
          if (m.method->return_type) collect_type_refs(*m.method->return_type, out);
          for (const auto& p : m.method->params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        } else if (m.kind == ObjectMemberKind::Property) {
          if (m.property.type) collect_type_refs(*m.property.type, out);
          for (const auto& p : m.property.params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        }
      }
      return;
    }
    case Kind::TyInterface: {
      const auto& i = static_cast<const TyInterface&>(t);
      for (const auto& m : i.members) {
        if (m.kind != ObjectMemberKind::Method || !m.method) continue;
        if (m.method->return_type) collect_type_refs(*m.method->return_type, out);
        for (const auto& p : m.method->params) {
          if (p.type) collect_type_refs(*p.type, out);
        }
      }
      return;
    }
    case Kind::TySubrange:
    case Kind::TyString:
    case Kind::TyEnum:
      return;
    case Kind::TyProcedural: {
      const auto& p = static_cast<const TyProcedural&>(t);
      if (p.return_type) collect_type_refs(*p.return_type, out);
      for (const auto& par : p.params) {
        if (par.type) collect_type_refs(*par.type, out);
      }
      return;
    }
    default:
      return;
  }
}

// Reorder type decls so every alias (non-record, non-object) appears after
// the types it references by name. Record/object types are already
// forward-declared by emit_forward_struct_decls, so aliases that point to
// them via `^T` always work; this function only needs to handle aliases
// that depend on other aliases (e.g. `pfoo = ^tfoo` where `tfoo` is itself
// an alias to an array type).
//
// `in` must contain only type decls (checked by the caller); this runs
// against a single contiguous Pascal `type` section.
std::vector<const Decl*> ordered_type_decls(const std::vector<const Decl*>& in) {
  std::vector<const Decl*> type_decls(in);

  // Map name -> index for quick lookup.
  std::unordered_map<std::string, int> index_of;
  for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
    index_of[static_cast<const TypeDecl*>(type_decls[i])->name] = i;
  }

  // For each type decl, which in-unit types does it reference?
  std::vector<std::unordered_set<int>> deps(type_decls.size());
  for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
    const auto& td = *static_cast<const TypeDecl*>(type_decls[i]);
    if (!td.type) continue;
    std::unordered_set<std::string> refs;
    collect_type_refs(*td.type, refs);
    for (const auto& r : refs) {
      auto it = index_of.find(r);
      if (it == index_of.end()) continue;
      int j = it->second;
      if (j == i) continue;
      const auto& rd = *static_cast<const TypeDecl*>(type_decls[j]);
      // Pointer-to-record aliases don't need the record body before them:
      // `using t_pfoo = t_tfoo*;` only needs the struct forward declaration
      // (emitted by emit_forward_struct_decls). This break lets cycles
      // like `Pfoo = ^Tfoo; Tfoo = record next: Pfoo; end;` remain a DAG.
      if (rd.type && (rd.type->kind == Kind::TyRecord ||
                      rd.type->kind == Kind::TyObject ||
                      rd.type->kind == Kind::TyInterface) &&
          td.type->kind == Kind::TyPointer) {
        continue;
      }
      deps[i].insert(j);
    }
  }

  // Kahn topological sort (stable: ties broken by original order).
  std::vector<int> indeg(type_decls.size(), 0);
  for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
    for (int j : deps[i]) (void)j, ++indeg[i];
  }
  std::vector<int> ready;
  for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
    if (indeg[i] == 0) ready.push_back(i);
  }
  std::vector<const Decl*> out;
  std::unordered_set<int> emitted_set;
  while (!ready.empty()) {
    int n = ready.front();
    ready.erase(ready.begin());
    out.push_back(type_decls[n]);
    emitted_set.insert(n);
    for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
      if (!deps[i].count(n)) continue;
      if (--indeg[i] == 0) ready.push_back(i);
    }
  }
  // Anything left has a cycle among non-pointer aliases. Emit in source
  // order as a fallback -- probably won't compile, but we don't silently
  // drop declarations.
  for (int i = 0; i < static_cast<int>(type_decls.size()); ++i) {
    if (!emitted_set.count(i)) out.push_back(type_decls[i]);
  }
  return out;
}

}  // namespace

EmitUnits::EmitUnits(ScopeStateView& scope, int& block_depth,
                     const std::vector<std::string>* unit_init_order,
                     std::string_view unit_init_name,
                     std::string_view unit_fini_name, EmitUnitOps& ops)
    : scope_(scope),
      block_depth_(block_depth),
      unit_init_order_(unit_init_order),
      unit_init_name_(unit_init_name),
      unit_fini_name_(unit_fini_name),
      ops_(ops) {}

void EmitUnits::seed_unit_type_scope(const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    if (td.type->kind == Kind::TyEnum) {
      scope_.local_enums[td.name] = static_cast<const TyEnum*>(td.type.get());
    } else if (td.type->kind != Kind::TyRecord &&
               td.type->kind != Kind::TyObject &&
               td.type->kind != Kind::TyInterface) {
      scope_.local_type_aliases_scoped[td.name] = td.type.get();
    }
  }
}

void EmitUnits::seed_unit_const_scope(const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::ConstDecl) continue;
    const auto& cd = static_cast<const ConstDecl&>(*d);
    scope_.local_consts[cd.name] = &cd;
  }
}

void EmitUnits::emit_type_decl_run(const std::vector<DeclPtr>& decls,
                                   bool in_header) {
  std::vector<const Decl*> run;
  auto flush = [&] {
    if (run.empty()) return;
    for (const auto* td : ordered_type_decls(run)) {
      ops_.emit_decl(*td, in_header);
    }
    run.clear();
  };
  for (const auto& d : decls) {
    if (d->kind == Kind::TypeDecl) {
      run.push_back(d.get());
    } else {
      flush();
      ops_.emit_decl(*d, in_header);
    }
  }
  flush();
}

void EmitUnits::emit_unit_hook(
    std::string_view name, const StmtPtr& body,
    const std::vector<const ProcDecl*>& before_body,
    const std::vector<const ProcDecl*>& after_body) {
  ops_.nl();
  ops_.emitln(std::string("void ") + std::string(name) + "() {");
  ops_.indent();
  ++block_depth_;
  for (const ProcDecl* pd : before_body) {
    ops_.emitln(class_lifecycle_call_cxx(*pd) + "();");
  }
  if (body) ops_.emit_stmt(*body);
  for (const ProcDecl* pd : after_body) {
    ops_.emitln(class_lifecycle_call_cxx(*pd) + "();");
  }
  --block_depth_;
  ops_.dedent();
  ops_.emitln("}");
}

void EmitUnits::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;
  scope_.current_unit_name = ascii_lower(u.name);
  auto saved_local_enums = scope_.local_enums;
  auto saved_local_aliases = scope_.local_type_aliases_scoped;
  auto saved_local_consts = scope_.local_consts;

  if (scope_.current_unit_name == "tpexcept") {
    emit_tpexcept_unit(u);
    scope_.local_enums = std::move(saved_local_enums);
    scope_.local_type_aliases_scoped = std::move(saved_local_aliases);
    scope_.local_consts = std::move(saved_local_consts);
    return;
  }

  ops_.set_header();
  ops_.emitln("// Generated by tp2cc. Do not edit.");
  ops_.emitln("#pragma once");
  ops_.emitln("#include <cstdint>");
  ops_.emitln("#include <cstddef>");
  ops_.emitln("#include <array>");
  ops_.emitln("#include <limits>");
  ops_.emitln("#include \"tp2cc_rt/prelude.h\"");
  seed_unit_type_scope(u.interface_decls);
  seed_unit_const_scope(u.interface_decls);
  // Emitted headers are prefixed `p_` so the filename never collides with
  // a C/C++ standard header (e.g. Pascal unit `strings` vs libc strings.h).
  for (const auto& uu : u.interface_uses) {
    ops_.emitln("#include \"p_" + uu + ".h\"");
  }
  ops_.nl();
  ops_.emitln("namespace " + ns + " {");
  ops_.nl();
  ops_.emit_forward_struct_decls(u.interface_decls);
  // Walk source order. Types are reordered topologically only within a
  // single contiguous run (a Pascal `type` section); any intervening
  // const/var/proc breaks the run. This respects Pascal's rule that
  // forward references are only allowed within the same type section.
  emit_type_decl_run(u.interface_decls, /*in_header=*/true);
  if (!u.is_program) {
    ops_.nl();
    ops_.emitln("void " + unit_init_name_ + "();");
    ops_.emitln("void " + unit_fini_name_ + "();");
  }
  ops_.nl();
  ops_.emitln("}  // namespace " + ns);

  ops_.set_impl();
  ops_.emitln("// Generated by tp2cc. Do not edit.");
  ops_.emitln("#include \"p_" + hguard + ".h\"");
  seed_unit_type_scope(u.impl_decls);
  seed_unit_const_scope(u.impl_decls);
  for (const auto& uu : u.impl_uses) {
    ops_.emitln("#include \"p_" + uu + ".h\"");
  }
  // The program emits a startup call chain over every parsed unit;
  // include all of their headers so the declarations are visible.
  if (u.is_program && unit_init_order_) {
    for (const auto& uu : *unit_init_order_) {
      if (uu == u.name) continue;
      ops_.emitln("#include \"p_" + uu + ".h\"");
    }
  }
  ops_.nl();
  ops_.emitln("namespace " + ns + " {");
  ops_.nl();
  ops_.emit_forward_struct_decls(u.impl_decls);
  // Emit definitions (not just extern declarations) for interface
  // vars in the .cc so external references resolve at link time.
  for (const auto& d : u.interface_decls) {
    if (d->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*d);
      if (vd.is_external) continue;
      ops_.emit_decl(*d, /*in_header=*/false);
    }
  }
  emit_type_decl_run(u.impl_decls, /*in_header=*/false);
  const ClassLifecycleHooks class_lifecycle =
      collect_class_lifecycle_hooks(u.impl_decls);

  if (!u.is_program) {
    // Emit class init hooks before the unit body and fini hooks after it.
    emit_unit_hook(unit_init_name_, u.init_body,
                   class_lifecycle.constructors, {});
    emit_unit_hook(unit_fini_name_, u.final_body, {},
                   class_lifecycle.destructors);
    ops_.nl();
    ops_.emitln("}  // namespace " + ns);
  } else {
    ops_.nl();
    ops_.emitln("}  // namespace " + ns);
    ops_.nl();
    ops_.emitln("int main(int argc, char** argv) {");
    ops_.indent();
    ops_.emitln("::rt::init_argv(argc, argv);");
    if (unit_init_order_) {
      // Register each finalizer only after its init hook returns.
      // That gives reverse-order teardown on normal exit/Halt and
      // leaves never-finished units out of the finalization chain.
      for (const auto& uu : *unit_init_order_) {
        if (uu == u.name) continue;
        std::string ns_name = unit_namespace_prefix(uu);
        ops_.emitln(ns_name + unit_init_name_ + "();");
        ops_.emitln("if (std::atexit(" + ns_name + unit_fini_name_ +
                   ") != 0) std::abort();");
      }
    }
    ops_.emitln("using namespace " + ns + ";");
    ++block_depth_;
    // Register program-local fini hooks so Halt and normal return share the
    // same cleanup path.
    for (const ProcDecl* pd : class_lifecycle.constructors) {
      ops_.emitln(class_lifecycle_call_cxx(*pd) + "();");
    }
    for (const ProcDecl* pd : class_lifecycle.destructors) {
      ops_.emitln("if (std::atexit(" + class_lifecycle_call_cxx(*pd) +
                  ") != 0) std::abort();");
    }
    if (u.init_body) ops_.emit_stmt(*u.init_body);
    --block_depth_;
    ops_.emitln("return 0;");
    ops_.dedent();
    ops_.emitln("}");
  }

  scope_.local_enums = std::move(saved_local_enums);
  scope_.local_type_aliases_scoped = std::move(saved_local_aliases);
  scope_.local_consts = std::move(saved_local_consts);
}

void EmitUnits::emit_tpexcept_unit(const UnitNode& u) {
  (void)u;
  ops_.set_header();
  ops_.emitln("// Generated by tp2cc. Do not edit.");
  ops_.emitln("#pragma once");
  ops_.emitln("#include <cstdint>");
  ops_.emitln("#include <cstddef>");
  ops_.emitln("#include <setjmp.h>");
  ops_.emitln("#include \"tp2cc_rt/prelude.h\"");
  ops_.nl();
  ops_.emitln("namespace p_tpexcept {");
  ops_.nl();
  ops_.emitln("struct t_jmp_buf {");
  ops_.indent();
  ops_.emitln("int32_t p_eax;");
  ops_.emitln("int32_t p_ebx;");
  ops_.emitln("int32_t p_ecx;");
  ops_.emitln("int32_t p_edx;");
  ops_.emitln("int32_t p_esi;");
  ops_.emitln("int32_t p_edi;");
  ops_.emitln("int32_t p_ebp;");
  ops_.emitln("int32_t p_esp;");
  ops_.emitln("int32_t p_eip;");
  ops_.emitln("int32_t p_flags;");
  ops_.emitln("uint16_t p_cs;");
  ops_.emitln("uint16_t p_ds;");
  ops_.emitln("uint16_t p_es;");
  ops_.emitln("uint16_t p_fs;");
  ops_.emitln("uint16_t p_gs;");
  ops_.emitln("uint16_t p_ss;");
  ops_.dedent();
  ops_.emitln("};");
  ops_.emitln("using t_pjmp_buf = t_jmp_buf*;");
  ops_.nl();
  ops_.emitln("namespace p_detail {");
  ops_.indent();
  ops_.emitln("struct p_jump_state {");
  ops_.indent();
  ops_.emitln("::jmp_buf p_env;");
  ops_.dedent();
  ops_.emitln("};");
  ops_.emitln("p_jump_state& p_state_for(t_jmp_buf* p_rec);");
  ops_.dedent();
  ops_.emitln("}  // namespace p_detail");
  ops_.nl();
  ops_.emitln("int32_t p_setjmp(t_jmp_buf& p_rec) = delete;");
  ops_.emitln(
      "[[noreturn]] void p_longjmp(const t_jmp_buf& p_rec, int32_t p_return_value);");
  ops_.emitln("inline t_pjmp_buf p_recoverpospointer = nullptr;");
  ops_.emitln("inline bool p_longjump_used = false;");
  ops_.nl();
  ops_.emitln("void " + unit_init_name_ + "();");
  ops_.emitln("void " + unit_fini_name_ + "();");
  ops_.nl();
  ops_.emitln("}  // namespace p_tpexcept");

  ops_.set_impl();
  ops_.emitln("// Generated by tp2cc. Do not edit.");
  ops_.emitln("#include \"p_tpexcept.h\"");
  ops_.emitln("#include <cstdlib>");
  ops_.emitln("#include <unordered_map>");
  ops_.nl();
  ops_.emitln("namespace {");
  ops_.indent();
  ops_.emitln("std::unordered_map<const ::p_tpexcept::t_jmp_buf*,");
  ops_.emitln("                   ::p_tpexcept::p_detail::p_jump_state> p_jump_states;");
  ops_.dedent();
  ops_.emitln("}  // namespace");
  ops_.nl();
  ops_.emitln("namespace p_tpexcept {");
  ops_.nl();
  ops_.emitln("namespace p_detail {");
  ops_.indent();
  ops_.emitln("p_jump_state& p_state_for(t_jmp_buf* p_rec) {");
  ops_.indent();
  ops_.emitln("return ::p_jump_states[p_rec];");
  ops_.dedent();
  ops_.emitln("}");
  ops_.dedent();
  ops_.emitln("}  // namespace p_detail");
  ops_.nl();
  ops_.emitln(
      "[[noreturn]] void p_longjmp(const t_jmp_buf& p_rec, int32_t p_return_value) {");
  ops_.indent();
  ops_.emitln("auto it = ::p_jump_states.find(&p_rec);");
  ops_.emitln("if (it == ::p_jump_states.end()) std::abort();");
  ops_.emitln("p_longjump_used = true;");
  ops_.emitln(
      "::longjmp(it->second.p_env, p_return_value == 0 ? 1 : p_return_value);");
  ops_.dedent();
  ops_.emitln("}");
  emit_unit_hook(unit_init_name_, nullptr, {}, {});
  emit_unit_hook(unit_fini_name_, nullptr, {}, {});
  ops_.nl();
  ops_.emitln("}  // namespace p_tpexcept");
}

}  // namespace tp2cc
