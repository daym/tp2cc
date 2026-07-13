#include "emit_units.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "diag.h"
#include "emit_support.h"
#include "typereg.h"

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
  std::vector<const ProcDecl*> constructors;
  std::vector<const ProcDecl*> destructors;
  for (const auto& d : decls) {
    if (!d || d->kind != Kind::ProcDecl) continue;
    const auto& pd = static_cast<const ProcDecl&>(*d);
    if (pd.of_type.empty() || !is_class_lifecycle_proc(pd)) continue;
    if (pd.pkind == ProcKind::Constructor) {
      constructors.push_back(&pd);
    } else {
      destructors.push_back(&pd);
    }
  }
  return ClassLifecycleHooks{.constructors = std::move(constructors),
                             .destructors = std::move(destructors)};
}

std::string type_symbol_struct_cxx_in_unit(const TypeSymbol& symbol,
                                           std::string_view current_unit) {
  std::string out = (!symbol.defining_unit.empty() &&
                     symbol.defining_unit != current_unit)
                        ? unit_namespace_prefix(symbol.defining_unit)
                        : std::string{};
  for (const auto& owner : symbol.owner_path) {
    out += type_mangle(owner);
    out += "::";
  }
  out += type_mangle(symbol.name);
  return out;
}

void collect_type_dependencies(
    const TypeExpr& type,
    std::unordered_set<const TypeDescriptor*>& dependencies);

void collect_variant_dependencies(
    const std::shared_ptr<VariantPart>& variant,
    std::unordered_set<const TypeDescriptor*>& dependencies) {
  if (!variant) return;
  if (variant->tag_type) {
    collect_type_dependencies(*variant->tag_type, dependencies);
  }
  for (const auto& variant_case : variant->cases) {
    for (const auto& field : variant_case.fields) {
      if (field.type) collect_type_dependencies(*field.type, dependencies);
    }
    collect_variant_dependencies(variant_case.variant_part, dependencies);
  }
}

void collect_params_dependencies(
    const std::vector<Param>& params,
    std::unordered_set<const TypeDescriptor*>& dependencies) {
  for (const auto& param : params) {
    if (param.type) collect_type_dependencies(*param.type, dependencies);
  }
}

// C++ declaration ordering is a backend constraint. Its edges come only from
// parser-bound descriptor identity; source spelling is never looked up here.
void collect_type_dependencies(
    const TypeExpr& type,
    std::unordered_set<const TypeDescriptor*>& dependencies) {
  switch (type.kind) {
    case Kind::TyName:
      if (type.descriptor) dependencies.insert(type.descriptor);
      return;
    case Kind::TyPointer: {
      const auto& pointer = static_cast<const TyPointer&>(type);
      if (pointer.target) {
        collect_type_dependencies(*pointer.target, dependencies);
      }
      return;
    }
    case Kind::TyArray: {
      const auto& array = static_cast<const TyArray&>(type);
      for (const auto& dim : array.dims) {
        if (dim) collect_type_dependencies(*dim, dependencies);
      }
      if (array.element) {
        collect_type_dependencies(*array.element, dependencies);
      }
      return;
    }
    case Kind::TySet: {
      const auto& set = static_cast<const TySet&>(type);
      if (set.element) collect_type_dependencies(*set.element, dependencies);
      return;
    }
    case Kind::TyFile: {
      const auto& file = static_cast<const TyFile&>(type);
      if (file.element) collect_type_dependencies(*file.element, dependencies);
      return;
    }
    case Kind::TyRecord: {
      const auto& record = static_cast<const TyRecord&>(type);
      for (const auto& nested : record.nested_types) {
        if (nested && nested->type) {
          collect_type_dependencies(*nested->type, dependencies);
        }
      }
      for (const auto& field : record.fields) {
        if (field.type) collect_type_dependencies(*field.type, dependencies);
      }
      collect_variant_dependencies(record.variant_part, dependencies);
      return;
    }
    case Kind::TyObject: {
      const auto& object = static_cast<const TyObject&>(type);
      if (const ClassInfo* info =
              type.descriptor ? type.descriptor->class_info() : nullptr) {
        if (info->parent_symbol && info->parent_symbol->descriptor) {
          dependencies.insert(info->parent_symbol->descriptor);
        }
        for (const TypeSymbol* interface_symbol : info->interface_symbols) {
          if (interface_symbol && interface_symbol->descriptor) {
            dependencies.insert(interface_symbol->descriptor);
          }
        }
      }
      for (const auto& member : object.members) {
        if (member.kind == ObjectMemberKind::Field && member.field_type) {
          collect_type_dependencies(*member.field_type, dependencies);
        } else if (member.kind == ObjectMemberKind::Method && member.method) {
          collect_params_dependencies(member.method->params, dependencies);
          if (member.method->return_type) {
            collect_type_dependencies(*member.method->return_type,
                                      dependencies);
          }
        } else if (member.kind == ObjectMemberKind::Property) {
          collect_params_dependencies(member.property.params, dependencies);
          if (member.property.type) {
            collect_type_dependencies(*member.property.type, dependencies);
          }
        } else if (member.kind == ObjectMemberKind::Type &&
                   member.type_decl && member.type_decl->type) {
          collect_type_dependencies(*member.type_decl->type, dependencies);
        }
      }
      return;
    }
    case Kind::TyInterface: {
      const auto& interface_type = static_cast<const TyInterface&>(type);
      for (const auto& member : interface_type.members) {
        if (member.kind == ObjectMemberKind::Method && member.method) {
          collect_params_dependencies(member.method->params, dependencies);
          if (member.method->return_type) {
            collect_type_dependencies(*member.method->return_type,
                                      dependencies);
          }
        } else if (member.kind == ObjectMemberKind::Type &&
                   member.type_decl && member.type_decl->type) {
          collect_type_dependencies(*member.type_decl->type, dependencies);
        }
      }
      return;
    }
    case Kind::TyProcedural: {
      const auto& procedural = static_cast<const TyProcedural&>(type);
      collect_params_dependencies(procedural.params, dependencies);
      if (procedural.return_type) {
        collect_type_dependencies(*procedural.return_type, dependencies);
      }
      return;
    }
    case Kind::TyDistinct: {
      const auto& distinct = static_cast<const TyDistinct&>(type);
      if (distinct.underlying) {
        collect_type_dependencies(*distinct.underlying, dependencies);
      }
      return;
    }
    case Kind::TyMetaclass:
      if (type.descriptor && type.descriptor->metaclass_target &&
          type.descriptor->metaclass_target->descriptor) {
        dependencies.insert(
            type.descriptor->metaclass_target->descriptor);
      }
      return;
    case Kind::TyEnum:
    case Kind::TySubrange:
    case Kind::TyString:
      return;
    default:
      return;
  }
}

bool is_forward_declarable_type(const TypeDecl& declaration) {
  return declaration.type &&
         (declaration.type->kind == Kind::TyRecord ||
          declaration.type->kind == Kind::TyObject ||
          declaration.type->kind == Kind::TyInterface);
}

std::vector<const Decl*> ordered_type_decls(
    const std::vector<const Decl*>& declarations) {
  std::unordered_map<const TypeDescriptor*, size_t> declaration_for_descriptor;
  for (size_t i = 0; i < declarations.size(); ++i) {
    const auto& declaration =
        static_cast<const TypeDecl&>(*declarations[i]);
    if (declaration.symbol && declaration.symbol->descriptor &&
        declaration.symbol->descriptor->symbol == declaration.symbol) {
      // A completed class intentionally replaces its earlier `T = class;`
      // entry for ordering purposes.
      declaration_for_descriptor[declaration.symbol->descriptor] = i;
    }
  }

  std::vector<std::unordered_set<size_t>> dependencies(declarations.size());
  for (size_t i = 0; i < declarations.size(); ++i) {
    const auto& declaration =
        static_cast<const TypeDecl&>(*declarations[i]);
    if (!declaration.type) continue;

    std::unordered_set<const TypeDescriptor*> descriptors;
    collect_type_dependencies(*declaration.type, descriptors);
    for (const TypeDescriptor* descriptor : descriptors) {
      auto target = declaration_for_descriptor.find(descriptor);
      if (target == declaration_for_descriptor.end() || target->second == i) {
        continue;
      }
      const auto& target_declaration =
          static_cast<const TypeDecl&>(*declarations[target->second]);
      // `using P = T*` only needs a prior `struct T;`. Sets, arrays, pointer
      // aliases, and other C++ aliases cannot be forward-declared and retain
      // the dependency edge.
      if (declaration.type->kind == Kind::TyPointer &&
          is_forward_declarable_type(target_declaration)) {
        continue;
      }
      dependencies[i].insert(target->second);
    }
  }

  std::vector<size_t> indegree(declarations.size());
  std::vector<size_t> ready;
  for (size_t i = 0; i < declarations.size(); ++i) {
    indegree[i] = dependencies[i].size();
    if (indegree[i] == 0) ready.push_back(i);
  }

  std::vector<const Decl*> ordered;
  std::unordered_set<size_t> emitted;
  for (size_t ready_index = 0; ready_index < ready.size(); ++ready_index) {
    const size_t declaration_index = ready[ready_index];
    ordered.push_back(declarations[declaration_index]);
    emitted.insert(declaration_index);
    for (size_t i = 0; i < declarations.size(); ++i) {
      if (!dependencies[i].contains(declaration_index)) continue;
      if (--indegree[i] == 0) ready.push_back(i);
    }
  }

  // Invalid non-pointer cycles remain in source order so generated C++ still
  // diagnoses them rather than silently dropping declarations.
  for (size_t i = 0; i < declarations.size(); ++i) {
    if (!emitted.contains(i)) ordered.push_back(declarations[i]);
  }
  return ordered;
}

}  // namespace

EmitUnits::EmitUnits(const TypeRegistry& registry, ScopeStateView& scope,
                     int& block_depth,
                     const std::vector<std::string>* unit_init_order,
                     std::string_view unit_init_name,
                     std::string_view unit_fini_name, EmitUnitOps& ops)
    : registry_(registry),
      scope_(scope),
      block_depth_(block_depth),
      unit_init_order_(unit_init_order),
      unit_init_name_(unit_init_name),
      unit_fini_name_(unit_fini_name),
      ops_(ops) {}

void EmitUnits::seed_unit_const_scope(const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::ConstDecl) continue;
    const auto& cd = static_cast<const ConstDecl&>(*d);
    scope_.local_consts[cd.name] = &cd;
  }
}

void EmitUnits::emit_ordered_type_decls(
    const std::vector<const Decl*>& decls, bool in_header) {
  for (const auto* td : ordered_type_decls(decls)) {
    ops_.emit_decl(*td, in_header);
  }
}

void EmitUnits::emit_type_decl_run(const std::vector<DeclPtr>& decls,
                                   bool in_header) {
  std::vector<const Decl*> run;
  size_t section_id = 0;
  for (const auto& d : decls) {
    if (d->kind == Kind::TypeDecl) {
      const auto& type_decl = static_cast<const TypeDecl&>(*d);
      if (!run.empty() && type_decl.type_section_id != section_id) {
        emit_ordered_type_decls(run, in_header);
        run.clear();
      }
      section_id = type_decl.type_section_id;
      run.push_back(d.get());
    } else {
      emit_ordered_type_decls(run, in_header);
      run.clear();
      ops_.emit_decl(*d, in_header);
    }
  }
  emit_ordered_type_decls(run, in_header);
}

std::optional<std::string> EmitUnits::class_lifecycle_call_cxx(
    const ProcDecl& pd) const {
  const TypeSymbol* owner = registry_.method_owner_symbol_for_proc(&pd);
  if (!owner) {
    report_error(pd.loc, "unresolved method owner `" + pd.of_type + "`");
    return std::nullopt;
  }
  return type_symbol_struct_cxx_in_unit(*owner, scope_.current_unit_name) +
         "::" + mangle(pd.name);
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
    if (std::optional<std::string> call = class_lifecycle_call_cxx(*pd)) {
      ops_.emitln(*call + "();");
    }
  }
  if (body) ops_.emit_stmt(*body);
  for (const ProcDecl* pd : after_body) {
    if (std::optional<std::string> call = class_lifecycle_call_cxx(*pd)) {
      ops_.emitln(*call + "();");
    }
  }
  --block_depth_;
  ops_.dedent();
  ops_.emitln("}");
}

void EmitUnits::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;
  scope_.current_unit_name = ascii_lower(u.name);
  const TypeLookupContext* saved_type_scope = scope_.type_scope;
  scope_.type_scope = registry_.lookup_unit_context(
      pascal_key(scope_.current_unit_name), /*implementation=*/false);
  auto saved_local_consts = scope_.local_consts;

  if (scope_.current_unit_name == "tpexcept") {
    emit_tpexcept_unit(u);
    scope_.type_scope = saved_type_scope;
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
  scope_.type_scope = registry_.lookup_unit_context(
      pascal_key(scope_.current_unit_name), /*implementation=*/true);
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
      if (std::optional<std::string> call = class_lifecycle_call_cxx(*pd)) {
        ops_.emitln(*call + "();");
      }
    }
    for (const ProcDecl* pd : class_lifecycle.destructors) {
      if (std::optional<std::string> call = class_lifecycle_call_cxx(*pd)) {
        ops_.emitln("if (std::atexit(" + *call + ") != 0) std::abort();");
      }
    }
    if (u.init_body) ops_.emit_stmt(*u.init_body);
    --block_depth_;
    ops_.emitln("return 0;");
    ops_.dedent();
    ops_.emitln("}");
  }

  scope_.type_scope = saved_type_scope;
  scope_.local_consts = std::move(saved_local_consts);
}

void EmitUnits::emit_tpexcept_unit(const UnitNode&) {
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
