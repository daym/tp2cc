#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
struct MethodSig;
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
// separate statement-only semantics inside `emit.cc`.
class EmitStmts {
 public:
  EmitStmts(const TypeRegistry* registry, ScopeStateView& scope,
            int& except_handler_depth, int& try_stmt_counter,
            int& loop_label_counter,
            std::vector<std::string>& loop_break_labels,
            std::vector<std::string>& loop_continue_labels,
            EmitAnalysis& analysis, EmitTypes& types, EmitStorage& storage,
            ResolveNameProvider& resolve_name_provider,
            EmitResolution& resolution, OverloadTypeProvider& overload_types,
            EmitCalls& calls, EmitProperties& properties,
            EmitStmtOps& stmt_ops);

  void emit_raise_stmt(const ast::Raise& r);
  void emit_try_stmt(const ast::Try& t);
  void emit_stmt(const ast::Stmt& s);

 private:
  enum class ForInEmitResult { NotMatched, Emitted, Error };

  struct ForInEnumeratorProvider {
    std::string value_cxx;
    const ast::TypeExpr* type = nullptr;
    const MethodSig* method = nullptr;
    // A class-scoped GetEnumerator can return a nested class by bare name.
    // Keep the provider owner so return-type lookup does not fall back to an
    // unrelated unit-level type with the same spelling.
    std::string owner_class_name;
    Location loc;
    ForInEnumeratorProvider(std::string value_cxx_in,
                            const ast::TypeExpr* type_in, Location loc_in,
                            std::string owner_class_name_in = {})
        : value_cxx(std::move(value_cxx_in)),
          type(type_in),
          owner_class_name(std::move(owner_class_name_in)),
          loc(loc_in) {}
  };

  bool stmt_autocalls_procvar(const ast::Expr& expr);
  bool emit_property_assign_stmt(const ast::Assign& a);
  void emit_assign_stmt(const ast::Assign& a);
  void emit_expr_stmt(const ast::ExprStmt& es);
  void emit_ordinal_for_body(const ast::For& f, const std::string& var,
                             const std::string& from, const std::string& to,
                             bool downto);
  // Pascal `for x in rhs` has a fixed dispatch order. Type RHS iterates
  // Low(T)..High(T). Expression RHS first checks operator enumerators, then
  // GetEnumerator providers, then built-in string/array/set iteration.
  void emit_for_in_stmt(const ast::For& f, const std::string& var);
  ForInEmitResult emit_for_in_type_rhs(const ast::For& f,
                                       const std::string& var);
  ForInEmitResult emit_for_in_operator_enumerator(const ast::For& f,
                                                  const std::string& var);
  ForInEmitResult emit_for_in_helper_get_enumerator(const ast::For& f,
                                                    const std::string& var);
  ForInEmitResult emit_for_in_own_get_enumerator(const ast::For& f,
                                                 const std::string& var);
  ForInEmitResult emit_for_in_builtin_string(const ast::For& f,
                                             const std::string& var);
  ForInEmitResult emit_for_in_builtin_array(const ast::For& f,
                                            const std::string& var);
  ForInEmitResult emit_for_in_builtin_set(const ast::For& f,
                                          const std::string& var);
  ForInEmitResult emit_for_in_enumerator_provider(
      const ast::For& f, const std::string& var,
      const ForInEnumeratorProvider& provider);
  const ast::TypeExpr* assignment_target_type(const ast::Expr& expr);
  const ast::TypeExpr* new_pointer_slot_type(const ast::Expr& expr);
  const ast::TypeExpr* procedural_designator_type(const ast::Expr& expr);
  const ast::TypeExpr* selected_value_type(const ast::Expr& expr);
  const ast::TypeExpr* with_receiver_type(const ast::Expr& expr);
  std::string value_class_alias(const ast::Expr& expr);
  std::optional<std::string> for_in_type_rhs_name(const ast::Expr& e);
  std::string for_in_class_type_name(
      const ast::TypeExpr* type,
      std::string_view owner_class_name = std::string_view{},
      const MethodSig* method = nullptr);
  const MethodSig* for_in_zero_arg_method(Location loc,
                                          const std::string& class_name,
                                          const std::string& method_name);
  std::string case_selector_expr(const ast::Expr& e);
  std::string case_binary_condition(const ast::ExprPtr& selector_expr,
                                    ast::BinOp op, const ast::Expr& rhs);
  std::string case_label_condition(const ast::ExprPtr& selector_expr,
                                   const ast::Expr& label);
  std::string case_arm_condition(const ast::ExprPtr& selector_expr,
                                const ast::CaseArm& arm);
  void emit_case_stmt(const ast::CaseStmt& cs);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  int& except_handler_depth_;
  int& try_stmt_counter_;
  int& loop_label_counter_;
  int case_stmt_counter_ = 0;
  std::vector<std::string>& loop_break_labels_;
  std::vector<std::string>& loop_continue_labels_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  ResolveNameProvider& resolve_name_provider_;
  EmitResolution& resolution_;
  OverloadTypeProvider& overload_types_;
  EmitCalls& calls_;
  EmitProperties& properties_;
  EmitStmtOps& stmt_ops_;
};

}  // namespace tp2cc
