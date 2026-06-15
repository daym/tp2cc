#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "emit_analysis.h"
#include "emit_context.h"
#include "emit_resolution_types.h"
#include "target_info.h"

namespace tp2cc {

struct ProcInfo;
struct PrimitiveInfo;
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

// Result of resolving an `@method` / nil / bare-ident expression against a
// `procedure of object` target. One source of truth for both the overload
// picker (scoring `@method` against a procedural-typed parameter slot) and
// the emitter (lowering `@method` to a `tp2cc_MethodPtr` constructor).
//
// `member_base == nullptr` means the receiver is the current method's
// `Self`; nonnull means the receiver is the Pascal expression `*member_base`
// and the emitter decides reference-class-ness from that.
struct MethodValueBinding {
  enum class Kind { Bound, SignatureMismatch };

  Kind kind;
  const ast::ProcDecl* decl = nullptr;
  std::string class_name;
  const ast::Expr* member_base = nullptr;

  bool has_matching_decl() const { return kind == Kind::Bound; }
  const ast::ProcDecl& matching_decl() const { return *decl; }

  static MethodValueBinding via_self(const ast::ProcDecl* decl,
                                     std::string class_name) {
    return {Kind::Bound, decl, std::move(class_name), nullptr};
  }
  static MethodValueBinding via_member(const ast::ProcDecl* decl,
                                       std::string class_name,
                                       const ast::Expr* base) {
    return {Kind::Bound, decl, std::move(class_name), base};
  }
  static MethodValueBinding signature_mismatch(std::string class_name,
                                               const ast::Expr* base) {
    return {Kind::SignatureMismatch, nullptr, std::move(class_name), base};
  }
};

class EmitResolution {
 public:
  EmitResolution(const TypeRegistry* registry, ScopeStateView& scope,
                  EmitAnalysis& analysis, ResolutionTypeOps& type_ops,
                  OverloadTypeProvider& overload_types, TargetInfo target);

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
  // plus the callee emission policy. This is the single semantic entry point
  // for call resolution; printing consumes the resolved result.
  ResolvedCall resolve_call(
      const ast::Expr& callee, const std::vector<const ast::Expr*>& args);
  ResolvedCall resolve_pointer_target_constructor(
      const ast::TypeExpr* pointer_type, const ast::Expr& ctor_callee,
      const std::vector<const ast::Expr*>& args);

  // Flatten Pascal formal parameters to call-site slots. Repeated names in one
  // parameter declaration become one row per actual argument position so the
  // picker and default-argument expansion reason in call-site order.
  std::vector<FlatCallParamInfo> flatten_call_param_info(
      const ast::ProcDecl* decl);

  // Resolve a Pascal binary operator overload by Pascal operator token and
  // operand expressions. The caller decides whether the chosen declaration is
  // emitted as C++ infix operator syntax or as a named helper.
  BinaryOperatorResult find_binary_operator(
      const std::string& op, const ast::Expr& lhs, const ast::Expr& rhs);

  UnaryOperatorResult find_unary_operator(
      const std::string& op, const ast::Expr& operand);

  // Pascal `operator :=' is a conversion operator. C++ has no namespace-scope
  // assignment operator, so emit sites that know a source and destination type
  // ask for the matching helper ProcDecl explicitly.
  AssignmentOperatorResult find_assignment_operator(
      const ast::TypeExpr* source, const ast::TypeExpr* target);

  // Resolve a method-value expression against a `procedure of object` target.
  // Pascal admits `@method`, a bare method name, or `receiver.method` here.
  // Returns nullopt when the expression is not one of those forms, or when the
  // target is an ordinary procedural type that cannot carry `Self`.
  std::optional<MethodValueBinding> resolve_method_value_binding(
      const ast::Expr& arg, const ast::TyProcedural& proc);

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
    std::string declaring_type;    // owning type scope for method signatures
    std::string return_type_name;  // for decl-less runtime builtins
  };
  struct ScoredCandidate {
    const ast::ProcDecl* decl = nullptr;
    std::vector<ConvScore> scores;
  };
  struct IntegerActualDomain {
    int64_t low = 0;
    uint64_t high = 0;
    PrimitiveIntKind preferred_kind = PrimitiveIntKind::None;
  };
  struct InstanceMethodLookup {
    enum class Kind { NoInstanceMethod, SignatureMismatch, Match };

    Kind kind = Kind::NoInstanceMethod;
    const ast::ProcDecl* decl = nullptr;

    static InstanceMethodLookup no_instance_method() { return {}; }
    static InstanceMethodLookup signature_mismatch() {
      return {Kind::SignatureMismatch, nullptr};
    }
    static InstanceMethodLookup match(const ast::ProcDecl* decl) {
      return {Kind::Match, decl};
    }
  };
  // Pascal lookup order for an unqualified callable name:
  // `with` stack -> nested procs -> current class chain -> current unit ->
  // uses chain. The first contributing non-uses scope wins; the uses chain
  // aggregates so same-name overloads across imports compete together.
  std::vector<AnyCand> class_method_cands(const std::string& cls,
                                          const std::string& name);
  std::vector<AnyCand> metaclass_method_cands(const std::string& cls,
                                              const std::string& name);
  std::vector<AnyCand> unit_export_proc_cands(const std::string& unit,
                                              const std::string& name);
  std::vector<AnyCand> gather_callable_in_pascal_scope(
      const std::string& name);
  std::vector<AnyCand> gather_operator_in_pascal_scope(const std::string& op);
  std::string type_cxx_or_empty(const ast::TypeExpr* t);
  const ast::TypeExpr* strip_conversion_wrapper(const ast::TypeExpr* t);
  ConvScore class_hierarchy_conversion_score(const ast::TypeExpr* arg,
                                             const ast::TypeExpr* param);
  const PrimitiveInfo* primitive_for_type(const ast::TypeExpr* t);
  std::optional<IntegerActualDomain> integer_actual_domain_for_type(
      const ast::TypeExpr* t);
  std::optional<IntegerActualDomain> integer_actual_domain_for_expr(
      const ast::Expr& arg);
  bool is_untyped_integer_constant_expr(const ast::Expr& arg);
  bool integer_domain_fits_primitive(const IntegerActualDomain& domain,
                                     const PrimitiveInfo& formal) const;
  bool set_literal_can_construct_open_array(const ast::SetLit& literal,
                                            const ast::TypeExpr* param) const;
  ConvScore rank_integer_domain_conversion(
      const IntegerActualDomain& domain, const ast::TypeExpr* param,
      bool var_param);
  std::optional<PickResult> pick_integer_domain_overload(
      const std::vector<ScoredCandidate>& viable,
      const std::vector<const ast::Expr*>& args);
  int real_conversion_rank(std::string_view name) const;
  bool type_is_shortstring_family(const ast::TypeExpr* t) const;
  bool type_is_ansistring(const ast::TypeExpr* t) const;
  bool type_is_char_type(const ast::TypeExpr* t) const;
  bool procedural_signatures_match(const ast::ProcDecl& decl,
                                   const ast::TyProcedural& proc);
  const ast::TypeExpr* method_value_member_base_type(const ast::Expr& base);
  // NoInstanceMethod means ordinary expression lowering may still apply;
  // SignatureMismatch means the Pascal method name exists but is not viable for
  // this procedural target type.
  InstanceMethodLookup pick_instance_method_decl(
      const std::string& cls, const std::string& name,
      const ast::TyProcedural& proc);
  bool conversion_score_less(const ConvScore& a, const ConvScore& b) const;
  bool conversion_candidate_dominates(const ScoredCandidate& a,
                                      const ScoredCandidate& b) const;
  ResolvedCall resolved_call_from_candidate(const std::string& member_name,
                                            const AnyCand& chosen,
                                            bool ran_type_picker) const;
  std::string value_class_alias(const ast::Expr& e);
  std::string value_metaclass_target(const ast::Expr& e);
  std::string receiver_class_for_member_call(const ast::Expr& callee);
  bool operand_type_allows_operator_lookup(const ast::TypeExpr* t);
  ConvScore score_conversion(const ast::TypeExpr* arg,
                             const ast::TypeExpr* param,
                             bool var_param,
                             bool allow_assignment_operator_conversions);
  std::optional<ConvScore> score_procedural_argument_conversion(
      const ast::Expr& arg, const ast::TyProcedural& proc);
  ConvScore score_argument_conversion(
      const ast::Expr& arg, const FlatCallParamInfo& param,
      bool allow_assignment_operator_conversions);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  ResolutionTypeOps& type_ops_;
  OverloadTypeProvider& overload_types_;
  TargetInfo target_;
};

}  // namespace tp2cc
