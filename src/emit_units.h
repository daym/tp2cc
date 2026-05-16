#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "emit_context.h"

namespace tp2cc {

class EmitUnitOps {
 public:
  virtual ~EmitUnitOps() = default;
  virtual void set_header() = 0;
  virtual void set_impl() = 0;
  virtual void emitln(std::string_view text) = 0;
  virtual void nl() = 0;
  virtual void indent() = 0;
  virtual void dedent() = 0;
  virtual void emit_decl(const ast::Decl& d, bool in_header) = 0;
  virtual void emit_stmt(const ast::Stmt& s) = 0;
  virtual void emit_forward_struct_decls(
      const std::vector<ast::DeclPtr>& decls) = 0;
};

// Whole-unit / whole-program emission. This module owns type-section ordering,
// header-vs-impl orchestration, lifecycle hook emission, and the `tpexcept`
// wrapper path. Those are top-level translation concerns, separate from
// expression/statement/procedure semantics.
class EmitUnits {
 public:
  EmitUnits(ScopeStateView& scope, int& block_depth,
            const std::vector<std::string>* unit_init_order,
            std::string_view unit_init_name, std::string_view unit_fini_name,
            EmitUnitOps& ops);

  void emit_unit(const ast::UnitNode& u);

 private:
  void seed_unit_type_scope(const std::vector<ast::DeclPtr>& decls);
  void seed_unit_const_scope(const std::vector<ast::DeclPtr>& decls);
  void emit_type_decl_run(const std::vector<ast::DeclPtr>& decls,
                          bool in_header);
  void emit_unit_hook(std::string_view name, const ast::StmtPtr& body,
                      const std::vector<const ast::ProcDecl*>& before_body,
                      const std::vector<const ast::ProcDecl*>& after_body);
  void emit_tpexcept_unit(const ast::UnitNode& u);

  ScopeStateView& scope_;
  int& block_depth_;
  const std::vector<std::string>* unit_init_order_;
  std::string unit_init_name_;
  std::string unit_fini_name_;
  EmitUnitOps& ops_;
};

}  // namespace tp2cc
