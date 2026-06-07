#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "emit_context.h"

namespace tp2cc {

class EmitAnalysis;
class EmitCalls;
class EmitDecls;
class EmitTypes;

class EmitProcOps {
 public:
  virtual ~EmitProcOps() = default;
  virtual void emitln(std::string_view s) = 0;
  virtual void indent() = 0;
  virtual void dedent() = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
  virtual void emit_stmt(const ast::Stmt& s) = 0;
  virtual void emit_decl(const ast::Decl& d, bool in_header) = 0;
  virtual void emit_forward_struct_decls(
      const std::vector<ast::DeclPtr>& decls) = 0;
};

// Pascal routine-body emission. This module owns the frame setup/teardown,
// implicit-result-slot semantics, local-scope seeding, and nested-procedure
// lambda lowering that used to be duplicated between top-level and nested
// routine emit paths.
class EmitProcs {
 public:
  EmitProcs(ScopeStateView& scope, int& block_depth, EmitAnalysis& analysis,
            EmitTypes& types, EmitCalls& calls, EmitDecls& decls,
            EmitProcOps& emit_ops);

  void emit_proc_body(const ast::ProcDecl& pd);
  void emit_nested_proc_lambda(const ast::ProcDecl& pd);

 private:
  struct SavedProcState {
    std::string current_fn_name;
    std::vector<std::string> current_fn_param_names;
    bool current_fn_is_function = false;
    bool current_fn_is_ctor = false;
    const ast::TypeExpr* current_fn_result_type = nullptr;
    std::string current_result_slot_name;
    std::string bare_result_slot_name;
    const ast::TypeExpr* bare_result_type = nullptr;
    std::string outer_result_name;
    std::string outer_result_slot_name;
    const ast::TypeExpr* outer_result_type = nullptr;
    std::string current_class_name;
    std::unordered_set<std::string> local_scope;
    std::unordered_map<std::string, const ast::TypeExpr*> local_types;
    std::unordered_map<std::string, const ast::ConstDecl*> local_consts;
    std::unordered_map<std::string, std::vector<ScopeStateView::NestedFn>>
        local_nested_fns;
    std::unordered_set<std::string> local_nested_forwards;
    std::unordered_set<std::string> local_untyped_params;
    TypeScopeFrame* type_scope = nullptr;
    std::unordered_set<std::string> local_const_params;
    int block_depth = 0;
  };

  SavedProcState save_proc_state() const;
  void restore_proc_state(SavedProcState&& saved);
  void setup_proc_frame(const ast::ProcDecl& pd, bool nested_lambda);
  bool insert_proc_local_name(Location where, const std::string& name);
  void seed_proc_scope(const ast::ProcDecl& pd);
  std::string nested_proc_cxx_name(const ast::ProcDecl& pd) const;
  std::string nested_proc_signature_types(const ast::ProcDecl& pd);

  ScopeStateView& scope_;
  int& block_depth_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitCalls& calls_;
  EmitDecls& decls_;
  EmitProcOps& emit_ops_;
};

}  // namespace tp2cc
