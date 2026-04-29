#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "emit_context.h"
#include "emit_resolution_types.h"

namespace tp2cc {

class EmitAnalysis;
class EmitCalls;
class EmitProperties;
class EmitResolution;
class EmitStorage;
class EmitTypes;
struct TypeRegistry;

class EmitStmtOps {
 public:
  virtual ~EmitStmtOps() = default;
  virtual std::string expr_to_cxx(const ast::Expr& e) = 0;
  virtual std::string const_value_to_cxx(
      const ast::Expr& e, const ast::TypeExpr* target = nullptr,
      bool explicit_conversion = false) = 0;
  virtual void emitln(std::string_view text) = 0;
  virtual void indent() = 0;
  virtual void dedent() = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
};

// Pascal statement lowering. This module owns control-flow emission,
// statement-context auto-calls, property writes, range-checked assignments,
// and loop/exception scaffolding. Keeping those rules together avoids growing
// more ad hoc statement-only semantics inside `emit.cc`.
class EmitStmts {
 public:
  EmitStmts(const TypeRegistry* registry, ScopeStateView& scope,
            int& except_handler_depth, int& try_stmt_counter,
            int& loop_label_counter,
            std::vector<std::string>& loop_break_labels,
            std::vector<std::string>& loop_continue_labels,
            EmitAnalysis& analysis, EmitTypes& types, EmitStorage& storage,
            ResolveNameProvider& resolve_name_provider,
            EmitResolution& resolution, EmitCalls& calls,
            EmitProperties& properties, EmitStmtOps& stmt_ops);

  void emit_raise_stmt(const ast::Raise& r);
  void emit_try_stmt(const ast::Try& t);
  void emit_stmt(const ast::Stmt& s);

 private:
  bool stmt_autocalls_procvar(const ast::Expr& expr);
  void emit_assign_stmt(const ast::Assign& a);
  void emit_expr_stmt(const ast::ExprStmt& es);
  std::string case_selector_expr(const ast::CaseStmt& cs, const ast::Expr& e);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  int& except_handler_depth_;
  int& try_stmt_counter_;
  int& loop_label_counter_;
  std::vector<std::string>& loop_break_labels_;
  std::vector<std::string>& loop_continue_labels_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  ResolveNameProvider& resolve_name_provider_;
  EmitResolution& resolution_;
  EmitCalls& calls_;
  EmitProperties& properties_;
  EmitStmtOps& stmt_ops_;
};

}  // namespace tp2cc
