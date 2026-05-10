#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "emit_analysis.h"
#include "emit_context.h"
#include "emit_resolution_types.h"

namespace tp2cc {

struct ProcInfo;
struct TypeRegistry;

struct AssignmentOperatorResult {
  const ast::ProcDecl* decl = nullptr;
  std::string defining_unit;
};

struct BinaryOperatorResult {
  const ast::ProcDecl* decl = nullptr;
  std::string defining_unit;
  bool ambiguous = false;
};

struct UnaryOperatorResult {
  const ast::ProcDecl* decl = nullptr;
  std::string defining_unit;
  bool ambiguous = false;
};

class EmitResolution {
 public:
  EmitResolution(const TypeRegistry* registry, ScopeStateView& scope,
                 EmitAnalysis& analysis, ResolutionTypeOps& type_ops,
                 OverloadTypeProvider& overload_types);

  // Pascal/FPC overload-resolution conversion ranks. Lower is better.
  // `NotViable` means no implicit conversion exists, so the candidate drops
  // out before the dominance check.
  ConvScore rank_conversion(const ast::TypeExpr* arg,
                            const ast::TypeExpr* param,
                            bool var_param);

  // Pick the Pascal-best ProcDecl from a list of candidates given the
  // call-site argument expressions. Used by both free-function overload
  // sets (built from `ProcInfo::decl`) and class-method overload sets
  // (built from `MethodSig::decl`); the picker only needs the decls.
  // Result `ambiguous=true` means multiple viable candidates were mutually
  // incomparable, so the caller must diagnose a Pascal-level ambiguity.
  PickResult pick_overload(
      const std::vector<const ast::ProcDecl*>& candidates,
      const std::vector<const ast::Expr*>& args,
      bool allow_assignment_operator_conversions = false);

  // Resolve a Pascal call expression all the way to the chosen declaration
  // plus the spelling policy the emitter should use for the callee. This is
  // the single semantic entry point for call resolution; printing should not
  // redo any of this logic ad hoc.
  ResolvedCall resolve_call(
      const ast::Expr& callee, const std::vector<const ast::Expr*>& args);

  // Resolve a Pascal binary operator overload by Pascal operator token and
  // operand expressions. The caller decides whether the chosen declaration is
  // spelled as C++ infix operator syntax or as a named helper.
  BinaryOperatorResult find_binary_operator(
      const std::string& op, const ast::Expr& lhs, const ast::Expr& rhs);

  UnaryOperatorResult find_unary_operator(
      const std::string& op, const ast::Expr& operand);

  // Pascal `operator :=' is a conversion operator. C++ has no namespace-scope
  // assignment operator, so emit sites that know a source and destination type
  // ask for the matching helper ProcDecl explicitly.
  AssignmentOperatorResult find_assignment_operator(
      const ast::TypeExpr* source, const ast::TypeExpr* target);

 private:
  // One row in a callable-name lookup result. `decl` is null only for
  // metadata-only runtime builtins; arity filtering still uses
  // `param_count` / `accepts_zero_args`.
  struct AnyCand {
    const ast::ProcDecl* decl = nullptr;
    size_t param_count = 0;
    bool accepts_zero_args = false;
    std::string callee_unit;       // nonempty for namespace-spelled unit procs
    std::string declaration_unit;  // scope for default parameter expressions
    std::string return_type_name;  // for decl-less runtime builtins
  };

  // Flatten Pascal formal parameters to call-site slots. Repeated names in one
  // parameter declaration become one row per actual argument position so the
  // picker and default-argument expansion reason in call-site order.
  struct FlatCallParamInfo {
    const ast::TypeExpr* type = nullptr;
    bool untyped = false;
    bool mutable_ref = false;
    const ast::Expr* default_value = nullptr;
  };

  // Pascal lookup order for an unqualified callable name:
  // `with` stack -> nested procs -> current class chain -> current unit ->
  // uses chain. The first contributing non-uses scope wins; the uses chain
  // aggregates so same-name overloads across imports compete together.
  void append_class_method_cands(const std::string& cls,
                                 const std::string& name,
                                 std::vector<AnyCand>& cands);
  void append_unit_export_proc_cands(const std::string& unit,
                                     const std::string& name,
                                     std::vector<AnyCand>& cands);
  void gather_callable_in_pascal_scope(const std::string& name,
                                       std::vector<AnyCand>& cands);
  void gather_operator_in_pascal_scope(const std::string& op,
                                       std::vector<AnyCand>& cands);
  void flatten_call_param_info(const ast::ProcDecl* decl,
                               std::vector<FlatCallParamInfo>& flat_params);
  ConvScore score_conversion(const ast::TypeExpr* arg,
                             const ast::TypeExpr* param,
                             bool var_param,
                             bool allow_assignment_operator_conversions);
  ConvScore score_argument_conversion(
      const ast::Expr& arg, const FlatCallParamInfo& param,
      bool allow_assignment_operator_conversions);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  ResolutionTypeOps& type_ops_;
  OverloadTypeProvider& overload_types_;
};

}  // namespace tp2cc
