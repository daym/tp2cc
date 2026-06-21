#include "emit_procs.h"

#include <utility>

#include "emit_analysis.h"
#include "emit_calls.h"
#include "emit_decls.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

constexpr const char* kPascalResultSlotName = "p_result";
constexpr const char* kCtorStatusSlotName = "tp2cc_ctor_ok";

void upsert_current_type_symbol(TypeScopeFrame& frame, TypeSymbol symbol) {
  TypeSymbol* existing = frame.find_here_lower_mut(symbol.name);
  if (!existing) {
    frame.insert_or_assign(std::move(symbol));
    return;
  }

  if (const ClassInfo* next_class = symbol.class_info()) {
    if (ClassInfo* current_class = existing->mutable_class_info();
        current_class && next_class->is_forward && !current_class->is_forward) {
      return;
    }
  }
  *existing = std::move(symbol);
}

void register_type_decl_in_current_scope(TypeScopeFrame& frame,
                                         const TypeDecl& td) {
  if (!td.type) return;
  upsert_current_type_symbol(
      frame, make_type_symbol_for_type({}, td.name, td.type));
  if (td.type->kind != Kind::TyEnum) {
    register_type_symbols_for_owner(frame.symbols, td.type, td.name);
  }
}

void seed_owner_nested_types(TypeScopeFrame& frame, const ClassInfo* owner) {
  if (!owner) return;
  for (const auto& [name, symbol] : owner->nested_types) {
    (void)name;
    if (symbol) frame.insert_ref(*symbol);
  }
}

}  // namespace

EmitProcs::EmitProcs(ScopeStateView& scope, int& block_depth,
                     EmitAnalysis& analysis, EmitTypes& types,
                     EmitCalls& calls, EmitDecls& decls, EmitProcOps& emit_ops)
    : scope_(scope),
      block_depth_(block_depth),
      analysis_(analysis),
      types_(types),
      calls_(calls),
      decls_(decls),
      emit_ops_(emit_ops) {}

EmitProcs::SavedProcState EmitProcs::save_proc_state() const {
  return SavedProcState{
      .current_fn_name = scope_.current_fn_name,
      .current_fn_param_names = scope_.current_fn_param_names,
      .current_fn_is_function = scope_.current_fn_is_function,
      .current_fn_is_ctor = scope_.current_fn_is_ctor,
      .current_fn_result_type = scope_.current_fn_result_type,
      .current_result_slot_name = scope_.current_result_slot_name,
      .bare_result_slot_name = scope_.bare_result_slot_name,
      .bare_result_type = scope_.bare_result_type,
      .outer_result_name = scope_.outer_result_name,
      .outer_result_slot_name = scope_.outer_result_slot_name,
      .outer_result_type = scope_.outer_result_type,
      .current_class_name = scope_.current_class_name,
      .local_scope = scope_.local_scope,
      .local_types = scope_.local_types,
      .local_consts = scope_.local_consts,
      .local_nested_fns = scope_.local_nested_fns,
      .local_nested_forwards = scope_.local_nested_forwards,
      .local_untyped_params = scope_.local_untyped_params,
      .type_scope = scope_.type_scope,
      .local_const_params = scope_.local_const_params,
      .block_depth = block_depth_};
}

void EmitProcs::restore_proc_state(SavedProcState&& saved) {
  scope_.current_fn_name = std::move(saved.current_fn_name);
  scope_.current_fn_param_names = std::move(saved.current_fn_param_names);
  scope_.current_fn_is_function = saved.current_fn_is_function;
  scope_.current_fn_is_ctor = saved.current_fn_is_ctor;
  scope_.current_fn_result_type = saved.current_fn_result_type;
  scope_.current_result_slot_name = std::move(saved.current_result_slot_name);
  scope_.bare_result_slot_name = std::move(saved.bare_result_slot_name);
  scope_.bare_result_type = saved.bare_result_type;
  scope_.outer_result_name = std::move(saved.outer_result_name);
  scope_.outer_result_slot_name = std::move(saved.outer_result_slot_name);
  scope_.outer_result_type = saved.outer_result_type;
  scope_.current_class_name = std::move(saved.current_class_name);
  scope_.local_scope = std::move(saved.local_scope);
  scope_.local_types = std::move(saved.local_types);
  scope_.local_consts = std::move(saved.local_consts);
  scope_.local_nested_fns = std::move(saved.local_nested_fns);
  scope_.local_nested_forwards = std::move(saved.local_nested_forwards);
  scope_.local_untyped_params = std::move(saved.local_untyped_params);
  scope_.type_scope = saved.type_scope;
  scope_.local_const_params = std::move(saved.local_const_params);
  block_depth_ = saved.block_depth;
}

void EmitProcs::setup_proc_frame(const ProcDecl& pd, bool nested_lambda) {
  std::string inherited_outer_result_name;
  std::string inherited_outer_result_slot_name;
  const TypeExpr* inherited_outer_result_type = nullptr;

  if (scope_.current_fn_is_function && scope_.current_fn_result_type) {
    inherited_outer_result_name = scope_.current_fn_name;
    inherited_outer_result_slot_name = scope_.current_result_slot_name;
    inherited_outer_result_type = scope_.current_fn_result_type;
  } else {
    inherited_outer_result_name = scope_.outer_result_name;
    inherited_outer_result_slot_name = scope_.outer_result_slot_name;
    inherited_outer_result_type = scope_.outer_result_type;
  }

  scope_.current_fn_name = pd.name;
  scope_.current_fn_param_names.clear();
  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) scope_.current_fn_param_names.push_back(mangle(nm));
  }
  scope_.current_fn_is_function = (pd.pkind == ProcKind::Function);
  scope_.current_fn_is_ctor =
      !nested_lambda && !pd.is_class_method &&
      (pd.pkind == ProcKind::Constructor);
  scope_.current_fn_result_type = pd.return_type.get();

  if (pd.pkind == ProcKind::Function && pd.return_type) {
    scope_.current_result_slot_name =
        inherited_outer_result_type ? nested_result_slot_name(pd.name)
                                    : std::string(kPascalResultSlotName);
    scope_.bare_result_slot_name = scope_.current_result_slot_name;
    scope_.bare_result_type = pd.return_type.get();
  } else {
    scope_.current_result_slot_name.clear();
    scope_.bare_result_slot_name = inherited_outer_result_slot_name;
    scope_.bare_result_type = inherited_outer_result_type;
  }
  scope_.outer_result_name = inherited_outer_result_name;
  scope_.outer_result_slot_name = inherited_outer_result_slot_name;
  scope_.outer_result_type = inherited_outer_result_type;
  if (!nested_lambda) {
    scope_.current_class_name = pd.of_type.empty()
                                    ? std::string{}
                                    : analysis_.canonical_method_owner_type_name(
                                          pd.of_type);
  }
  ++block_depth_;
}

bool EmitProcs::insert_proc_local_name(Location where,
                                       const std::string& name) {
  // Pascal functions already own an implicit `Result` variable, so any
  // local/parameter/const nested in that body may not reuse the name.
  if (scope_.bare_result_type && is_pascal_result_ident(name)) {
    emit_ops_.report_error(where, "duplicate identifier `Result`");
    return false;
  }
  scope_.local_scope.insert(name);
  return true;
}

void EmitProcs::seed_proc_scope(const ProcDecl& pd) {
  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      if (!insert_proc_local_name(pd.loc, nm)) continue;
      if (p.type) {
        scope_.local_types[nm] = p.type.get();
        if (p.mode == Param::Const || p.mode == Param::ConstRef) {
          scope_.local_const_params.insert(nm);
        }
      } else {
        // Untyped read-only params arrive as `const void*` in C++; keep the
        // const-mode bit so later pointer-slot coercions can make the
        // qualifier drop explicit instead of relying on `-fpermissive`.
        scope_.local_untyped_params.insert(nm);
        if (p.mode == Param::Const || p.mode == Param::ConstRef) {
          scope_.local_const_params.insert(nm);
        }
      }
    }
  }

  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        if (!insert_proc_local_name(vd.loc, nm)) continue;
        if (vd.type) scope_.local_types[nm] = vd.type.get();
      }
      if (vd.type && !vd.names.empty()) {
        register_type_symbols_for_owner(scope_.type_scope->symbols, vd.type,
                                        vd.names.front());
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      if (!insert_proc_local_name(cd.loc, cd.name)) continue;
      scope_.local_consts[cd.name] = &cd;
      if (const TypeExpr* ct = analysis_.deduce_const_decl_type(cd)) {
        scope_.local_types[cd.name] = ct;
      }
      register_type_symbols_for_owner(scope_.type_scope->symbols, cd.type,
                                      cd.name);
    } else if (l->kind == Kind::TypeDecl) {
      const auto& td = static_cast<const TypeDecl&>(*l);
      register_type_decl_in_current_scope(*scope_.type_scope, td);
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      if (!insert_proc_local_name(npd.loc, npd.name)) continue;
      if (npd.modifiers.is_forward) {
        // A Pascal `forward` local routine is completed by the later body in
        // the same block. Only the body is callable; registering both would
        // create duplicate overload candidates for recursive parser helpers.
        continue;
      }
      size_t param_count = 0;
      for (const auto& p : npd.params) param_count += p.names.size();
      auto& overloads = scope_.local_nested_fns[npd.name];
      const size_t overload_index = overloads.size();
      overloads.push_back(ScopeStateView::NestedFn{
          .param_count = param_count,
          .accepts_zero_args = calls_.proc_accepts_zero_args(npd),
          .is_function = (npd.pkind == ProcKind::Function),
          .return_type = npd.return_type.get(),
          .decl = &npd,
          .cxx_name = overload_index == 0
                          ? mangle(npd.name)
                          : mangle(npd.name) + "_ov" +
                                std::to_string(overload_index)});
    }
  }
}

std::string EmitProcs::nested_proc_cxx_name(const ProcDecl& pd) const {
  auto it = scope_.local_nested_fns.find(pd.name);
  if (it != scope_.local_nested_fns.end()) {
    for (const auto& overload : it->second) {
      if (overload.decl == &pd) return overload.cxx_name;
    }
  }
  return mangle(pd.name);
}

std::string EmitProcs::nested_proc_signature_types(const ProcDecl& pd) {
  return decls_.param_type_list_to_cxx(pd.params);
}

void EmitProcs::emit_proc_body(const ProcDecl& pd) {
  std::string method_owner;
  TypeScopeFrame signature_type_scope(scope_.type_scope);
  TypeScopeFrame* saved_signature_type_scope = scope_.type_scope;
  if (!pd.of_type.empty()) {
    method_owner = analysis_.canonical_method_owner_type_name(pd.of_type);
    scope_.type_scope = &signature_type_scope;
    // A Pascal method implementation is written outside the class declaration,
    // but its signature still resolves unqualified nested type names through
    // the owner class scope.
    seed_owner_nested_types(
        signature_type_scope, analysis_.class_info_for_type_name(method_owner));
  }

  // Header line: ret ClassName::Method(args) or ret Method(args).
  std::string ret = decls_.proc_return_type_to_cxx(pd);
  std::string qname = pascal_operator_decl_name_to_cxx(pd);
  if (!pd.of_type.empty()) {
    qname = types_.named_type_struct_cxx(method_owner) +
            "::" + qname;
  }
  emit_ops_.emitln(decls_.proc_attributes_to_cxx(pd) + ret + " " + qname +
                   "(" + decls_.param_list_to_cxx(pd.params) + ") {");
  scope_.type_scope = saved_signature_type_scope;
  emit_ops_.indent();

  if (pd.modifiers.is_abstract && !pd.body) {
    // Pascal's abstract methods are often placeholder hooks on classes that
    // native FPC still instantiates. Emit a fail-fast body instead of a pure
    // virtual so the translated class layout stays constructible while any
    // accidental call still stops immediately.
    emit_ops_.emitln("::std::abort();");
    emit_ops_.dedent();
    emit_ops_.emitln("}");
    return;
  }

  SavedProcState saved = save_proc_state();
  TypeScopeFrame proc_type_scope(scope_.type_scope);
  scope_.type_scope = &proc_type_scope;
  setup_proc_frame(pd, /*nested_lambda=*/false);
  if (!scope_.current_class_name.empty()) {
    seed_owner_nested_types(
        proc_type_scope,
        analysis_.class_info_for_type_name(scope_.current_class_name));
  }
  seed_proc_scope(pd);

  // `Result` is a Pascal-visible implicit variable in functions, so it uses
  // ordinary Pascal name mangling. Declare it before nested local
  // procedures/functions: Pascal lets those inner routines read and write the
  // enclosing function result, so the generated lambda must be able to capture
  // an already-declared C++ local.
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emit_ops_.emitln(ret + " " + scope_.current_result_slot_name + "{};");
  } else if (pd.pkind == ProcKind::Constructor && !pd.is_class_method) {
    emit_ops_.emitln(std::string("bool ") + kCtorStatusSlotName + " = true;");
  }
  // Forward-declare any record/object types in locals so a pointer alias that
  // textually precedes its target still compiles inside the function body.
  emit_ops_.emit_forward_struct_decls(pd.locals);
  for (const auto& l : pd.locals) emit_ops_.emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_ops_.emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function ||
      (pd.pkind == ProcKind::Constructor && !pd.is_class_method)) {
    emit_ops_.emitln(std::string("return ") +
                     (pd.pkind == ProcKind::Function
                          ? scope_.current_result_slot_name
                          : std::string(kCtorStatusSlotName)) +
                     ";");
  }

  restore_proc_state(std::move(saved));
  emit_ops_.dedent();
  emit_ops_.emitln("}");
}

void EmitProcs::emit_nested_proc_lambda(const ProcDecl& pd) {
  std::string ret =
      (pd.pkind == ProcKind::Function && pd.return_type)
          ? decls_.proc_return_type_to_cxx(pd)
          : std::string("void");
  const std::string sig_params = nested_proc_signature_types(pd);

  const std::string lname = nested_proc_cxx_name(pd);
  // Forward-declare the std::function so the lambda can recurse by name.
  if (!scope_.local_nested_forwards.count(lname)) {
    emit_ops_.emitln("::std::function<" + ret + "(" + sig_params + ")> " +
                     lname + ";");
  }
  emit_ops_.emitln(lname + " = [&](" + decls_.param_list_to_cxx(pd.params) +
                   ") -> " + ret + " {");
  emit_ops_.indent();

  SavedProcState saved = save_proc_state();
  TypeScopeFrame proc_type_scope(scope_.type_scope);
  scope_.type_scope = &proc_type_scope;
  setup_proc_frame(pd, /*nested_lambda=*/true);
  seed_proc_scope(pd);

  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emit_ops_.emitln(ret + " " + scope_.current_result_slot_name + "{};");
  }
  emit_ops_.emit_forward_struct_decls(pd.locals);
  for (const auto& l : pd.locals) emit_ops_.emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_ops_.emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function) {
    emit_ops_.emitln(std::string("return ") + scope_.current_result_slot_name +
                     ";");
  }

  restore_proc_state(std::move(saved));
  emit_ops_.dedent();
  emit_ops_.emitln("};");
}

}  // namespace tp2cc
