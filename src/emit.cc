#include "emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "emit_analysis.h"
#include "emit_calls.h"
#include "emit_context.h"
#include "emit_decls.h"
#include "emit_lookup.h"
#include "emit_properties.h"
#include "emit_procs.h"
#include "emit_resolution.h"
#include "emit_storage.h"
#include "emit_stmts.h"
#include "emit_types.h"
#include "emit_units.h"
#include "emit_values.h"
#include "diag.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

constexpr const char* kUnitInitName = "tp2cc_unit_init";
constexpr const char* kUnitFiniName = "tp2cc_unit_fini";
constexpr const char* kPascalResultSlotName = "p_result";
constexpr const char* kCtorStatusSlotName = "tp2cc_ctor_ok";

std::string binary_pascal_operator_token(BinOp op) {
  switch (op) {
    case BinOp::Add: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::RealDiv: return "/";
    case BinOp::IntDiv: return "div";
    case BinOp::Mod: return "mod";
    case BinOp::Shl: return "shl";
    case BinOp::Shr: return "shr";
    case BinOp::And: return "and";
    case BinOp::Or: return "or";
    case BinOp::Xor: return "xor";
    case BinOp::Eq: return "=";
    case BinOp::NotEq: return "<>";
    case BinOp::Lt: return "<";
    case BinOp::Gt: return ">";
    case BinOp::LtEq: return "<=";
    case BinOp::GtEq: return ">=";
    default: return {};
  }
}

// ---------------------------------------------------------------------------
// Emitter state

struct Emitter : ResolveNameProvider,
                 ResolutionTypeOps,
                 EmitTypeConstRender,
                 EmitTypeDiagOps,
                 EmitStorageExprOps,
                 EmitCallExprOps,
                 EmitPropertyExprOps,
                 EmitValueExprOps,
                 EmitStmtOps,
                 EmitUnitOps,
                 EmitDeclOps,
                 EmitProcOps {
  std::string header;
  std::string impl;
  // Current sink pointer.
  std::string* out = &header;
  int indent_level = 0;
  // Depth of proc bodies we're currently emitting. >0 means block scope,
  // which means C++ `inline` is invalid for local decls.
  int block_depth = 0;

  // Name of the Pascal class whose method body we're currently emitting
  // (if any). Empty when emitting a free function or at namespace scope.
  std::string current_class_name;

  // Name of the Pascal unit we are emitting (lowercased). Used to
  // decide whether a cross-unit reference needs explicit qualification
  // to disambiguate it from a same-named symbol brought in by another
  // `uses` clause.
  std::string current_unit_name;
  std::string default_arg_emission_unit_name;

  // Suppresses the "bare method reference -> append ()" rewrite. tp2cc_Set
  // while emitting (a) the CALLEE of a Call (else `foo(args)` would
  // become `foo()(args)`), and (b) the operand of AddrOf (else `@foo`
  // would become `&foo()`).
  bool is_callee_context_ = false;

  // When emitting an LHS expression, if set, any bare Ident whose name
  // equals this value is rewritten to the Pascal-visible implicit result
  // variable. `Result` is in the Pascal identifier namespace, so it uses
  // the ordinary `p_...` mangling rather than an internal helper name.
  std::string lhs_fn_rewrite;
  std::string lhs_fn_rewrite_slot;
  std::string lhs_outer_result_rewrite;
  std::string lhs_outer_result_rewrite_slot;
  bool suppress_packed_scalar_value_load = false;
  bool storage_view_context = false;

  // Names bound in the current function's scope (parameters + locals).
  // `obj` resolved bare at block scope that hits this set must be a
  // variable, so auto-call (`name()`) is suppressed. Prevents false
  // auto-call on local vars whose names happen to coincide with a
  // parameterless method in another unit.
  std::unordered_set<std::string> local_scope;

  // Variable-to-declared-type map for the current scope (parameters +
  // locals). Populated at proc-body entry so expression-type deduction
  // can answer "what class does this variable belong to?" and the
  // Member-access emitter can auto-call only actual methods.
  std::unordered_map<std::string, const ast::TypeExpr*> local_types;

  // Function-local const declarations. Needed so integer constant
  // expressions can fold through local const references instead of
  // falling back to C++'s type rules.
  std::unordered_map<std::string, const ast::ConstDecl*> local_consts;

  // Names of the current scope's parameters that are Pascal's untyped
  // `var X` / `const X` / `X` form. Their C++ type is `void*` (not
  // `void*&`); the callee receives the caller's storage address.
  // `@X` on one of these emits as the ident itself -- no `&`.
  std::unordered_set<std::string> local_untyped_params;

  // Nested procs/functions declared in the current scope. For each
  // name, store the parameter count and whether it returns a value.
  // Used so bare references to a parameterless nested `function`
  // auto-call (the lambda itself is `std::function<T()>`, not a `T`).
  using NestedFn = ScopeStateView::NestedFn;
  std::unordered_map<std::string, NestedFn> local_nested_fns;
  std::unordered_set<std::string> local_nested_forwards;
  std::vector<std::string> current_fn_param_names;

  // Function-local enum types: name -> the TyEnum AST node. Pascal
  // lets a `type T = (a, b, c)` and `const X : array[T] of ... = ...`
  // live inside a proc's declaration section. These aren't in the
  // unit-wide TypeRegistry (which only indexes interface/impl top-
  // level decls), so we layer them on here while emitting the proc.
  std::unordered_map<std::string, const ast::TyEnum*> local_enums;
  // `const` parameters stay read-only storage. An `absolute` alias over one
  // must therefore bind a `const T&`, not a mutable `T&`, or C++ rejects the
  // alias and Pascal source that only reads through it stops compiling.
  std::unordered_set<std::string> local_const_params;
  // Function-local type aliases: `type pi = ^integer;` style.
  std::unordered_map<std::string, const ast::TypeExpr*>
      local_type_aliases_scoped;

  // `with X do` bindings: for every `with target`, push the target's
  // expression text (already emitted) and its deduced type. Bare idents
  // inside the body that resolve as fields of one of the targets get
  // rewritten to `target.name`. For auto-call decisions on bare idents,
  // consult these types.
  using WithBind = ScopeStateView::WithBind;
  std::vector<WithBind> with_stack;

  // Reified type/symbol tree spanning all parsed units. tp2cc_Set by the
  // driver. Drives member-access and ident-call decisions.
  const TypeRegistry* registry = nullptr;

  // Ordered unit names whose lifecycle hooks must run before the
  // program's `begin..end.` body. tp2cc_Set by the driver only when
  // emitting the `program` unit.
  const std::vector<std::string>* unit_init_order = nullptr;
  ScopeStateView scope_state_;
  EmitAnalysis analysis_;
  EmitTypes types_;
  EmitStorage storage_;
  EmitResolution resolution_;
  EmitCalls calls_;
  EmitProperties properties_;
  EmitLookup lookup_;
  EmitValues values_;
  EmitDecls decls_;
  EmitProcs procs_;
  EmitStmts stmts_;
  EmitUnits units_;

  Emitter(const TypeRegistry* registry_in = nullptr,
          const std::vector<std::string>* unit_init_order_in = nullptr)
      : registry(registry_in),
        unit_init_order(unit_init_order_in),
        scope_state_{current_class_name,
                     current_unit_name,
                     default_arg_emission_unit_name,
                     lhs_fn_rewrite,
                     lhs_fn_rewrite_slot,
                     lhs_outer_result_rewrite,
                     lhs_outer_result_rewrite_slot,
                     suppress_packed_scalar_value_load,
                     storage_view_context,
                     local_scope,
                     local_types,
                     local_consts,
                     local_untyped_params,
                     local_nested_fns,
                     local_nested_forwards,
                     local_enums,
                     local_const_params,
                     local_type_aliases_scoped,
                     with_stack,
                     current_fn_name,
                     current_fn_param_names,
                     current_fn_is_function,
                     current_fn_is_ctor,
                     current_fn_result_type,
                     current_result_slot_name,
                     bare_result_slot_name,
                     bare_result_type,
                     outer_result_name,
                     outer_result_slot_name,
                     outer_result_type},
        analysis_(registry, scope_state_, *this),
        types_(registry, scope_state_, analysis_, *this, *this),
        storage_(registry, scope_state_, analysis_, types_, *this, *this),
        resolution_(registry, scope_state_, analysis_, *this),
        calls_(registry, scope_state_, analysis_, types_, storage_,
               resolution_, *this),
        properties_(registry, analysis_, *this),
        lookup_(registry, scope_state_, analysis_, properties_),
        values_(registry, scope_state_, analysis_, types_, storage_,
                resolution_, *this),
        decls_(registry, scope_state_, analysis_, types_, storage_, values_,
               *this),
        procs_(scope_state_, block_depth, analysis_, types_, calls_, decls_,
               *this),
        stmts_(registry, scope_state_, except_handler_depth, try_stmt_counter,
               loop_label_counter, loop_break_labels, loop_continue_labels,
               analysis_, types_, storage_, *this, resolution_, calls_,
               properties_, *this),
        units_(scope_state_, block_depth, unit_init_order, kUnitInitName,
               kUnitFiniName, *this) {}

  void set_header() { out = &header; }
  void set_impl()   { out = &impl; }

  void emit(std::string_view s) { out->append(s); }
  void emitln(std::string_view s) {
    for (int i = 0; i < indent_level; ++i) out->append("  ");
    out->append(s);
    out->push_back('\n');
  }
  void nl() { out->push_back('\n'); }
  void indent() { ++indent_level; }
  void dedent() { if (indent_level > 0) --indent_level; }
  void report_error(Location where, const std::string& msg) override {
    ::tp2cc::report_error(where, msg);
  }

  // Top-level drivers.
  void emit_unit(const UnitNode& u);
  void emit_decl(const Decl& d, bool in_header);

  // Types -> C++ type string.
  std::string type_to_cxx(const TypeExpr& t) override {
    return types_.type_to_cxx(t);
  }
  std::string type_name_to_cxx(const TyName& n) {
    return types_.type_name_to_cxx(n);
  }
  std::string type_name_text_to_cxx(std::string_view name) {
    return types_.type_name_text_to_cxx(name);
  }
  std::string named_type_struct_cxx(std::string_view name) {
    return types_.named_type_struct_cxx(name);
  }
  std::string visible_type_prefix(std::string_view name) {
    return types_.visible_type_prefix(name);
  }
  std::string metaclass_struct_cxx(std::string_view class_name) {
    return types_.metaclass_struct_cxx(class_name);
  }
  std::string metaclass_value_fn_cxx(std::string_view class_name) {
    return types_.metaclass_value_fn_cxx(class_name);
  }
  std::string array_type_to_cxx(const TyArray& a) {
    return types_.array_type_to_cxx(a);
  }
  const ast::TypeExpr* canonicalize_type(const ast::TypeExpr* t) {
    return analysis_.canonicalize_type(t);
  }
  std::string enum_underlying_type_to_cxx(const TyEnum& e) {
    return types_.enum_underlying_type_to_cxx(e);
  }
  bool array_dim_bounds_to_cxx(const ast::TypeExpr& dim,
                               std::string* lo,
                               std::string* size_expr) {
    return types_.array_dim_bounds_to_cxx(dim, lo, size_expr);
  }
  std::string set_type_to_cxx(const TySet& s) {
    return types_.set_type_to_cxx(s);
  }
  std::string enum_type_to_cxx(const TyEnum& e, const std::string& context) {
    return types_.enum_type_to_cxx(e, context);
  }
  std::string subrange_type_to_cxx(const TySubrange& r) {
    return types_.subrange_type_to_cxx(r);
  }
  std::string string_type_to_cxx(const TyString& s) {
    return types_.string_type_to_cxx(s);
  }
  std::optional<std::string> shortstring_capacity_to_cxx(
      const TypeExpr* t) {
    return types_.shortstring_capacity_to_cxx(t);
  }
  std::string pointer_type_to_cxx(const TyPointer& p) {
    return types_.pointer_type_to_cxx(p);
  }
  std::string procedural_type_to_cxx(const TyProcedural& p) {
    return types_.procedural_type_to_cxx(p);
  }
  std::string procedural_param_types_to_cxx(const std::vector<Param>& params) {
    return types_.procedural_param_types_to_cxx(params);
  }
  std::string named_type_to_cxx(const TypeExpr* t, std::string_view name,
                                std::string_view name_prefix = {}) {
    return types_.named_type_to_cxx(t, name, name_prefix);
  }
  std::string method_pointer_helper_name(const ast::ProcDecl& pd) {
    return types_.method_pointer_helper_name(pd);
  }
  std::string low_high_expr_for_named_type(std::string_view name,
                                           bool want_low) {
    return types_.low_high_expr_for_named_type(name, want_low);
  }
  std::string low_high_expr_for_type(const ast::TypeExpr* t,
                                     bool want_low) {
    return types_.low_high_expr_for_type(t, want_low);
  }

  // Expressions -> C++ expression.
  std::string expr_to_cxx(const Expr& e);
  std::string expr_to_cxx_no_autocall(const Expr& e) override {
    bool saved = is_callee_context_;
    is_callee_context_ = true;
    std::string text = expr_to_cxx(e);
    is_callee_context_ = saved;
    return text;
  }
  bool in_block_scope() const override { return block_depth > 0; }
  // When a target type is provided, rewrites integer constant
  // expressions to the exact value the destination type would hold,
  // and descends into ArrayConst / RecordConst with the per-element /
  // per-field type.
  std::string const_value_to_cxx(const Expr& e,
                                 const TypeExpr* target = nullptr,
                                 bool explicit_conversion = false);
  std::string set_literal_to_cxx(const SetLit& s,
                                 const TypeExpr* target = nullptr);
  // If `e` is an integer constant expression and `target` is an
  // integer primitive, return the exact destination value as a C++
  // literal. Implicit conversions diagnose range errors; explicit
  // Pascal casts do not.
  std::optional<std::string> maybe_convert_const_int_expr(
      const Expr& e, const TypeExpr* target, bool explicit_conversion);

  // Small helpers.
  bool const_param_needs_mutable_ref(const ast::TypeExpr* t) {
    return analysis_.const_param_needs_mutable_ref(t);
  }
  bool const_param_needs_const_ref(const ast::TypeExpr* t) {
    return analysis_.const_param_needs_const_ref(t);
  }
  const ClassInfo* class_info_for_type_name(std::string_view name) {
    return analysis_.class_info_for_type_name(name);
  }
  const ast::TypeExpr* lookup_named_type_expr(std::string_view name) {
    return analysis_.lookup_named_type_expr(name);
  }
  bool is_builtin_reference_class_name(std::string_view name) const {
    return analysis_.is_builtin_reference_class_name(name);
  }
  std::string metaclass_target_name(const ast::TypeExpr* t) {
    return analysis_.metaclass_target_name(t);
  }
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'
  void emit_forward_struct_decls(
      const std::vector<ast::DeclPtr>& decls) override;

  // Expression-type deduction. Returns the Pascal TypeExpr that the
  // expression has, or nullptr when unknown. Consults the TypeRegistry
  // for globals and the current scope tables for locals/self-class.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);

  bool type_is_packed_record(const ast::TypeExpr* t) {
    return storage_.type_is_packed_record(t);
  }
  bool type_is_direct_packed_aggregate(const ast::TypeExpr* t) {
    return storage_.type_is_direct_packed_aggregate(t);
  }
  bool type_is_byte_aligned_packed_index_carrier(const ast::TypeExpr* t) {
    return storage_.type_is_byte_aligned_packed_index_carrier(t);
  }
  using PackedScalarValueLoad = EmitPackedScalarValueLoad;
  std::optional<PackedScalarValueLoad> packed_scalar_value_load(
      const ast::Expr& e) {
    return storage_.packed_scalar_value_load(e);
  }
  using PackedAggregateFieldUse = EmitPackedAggregateFieldUse;
  std::optional<PackedAggregateFieldUse> direct_packed_aggregate_field_use(
      const ast::Expr& e) {
    return storage_.direct_packed_aggregate_field_use(e);
  }
  void report_packed_aggregate_subobject_use(
      Location where, std::string_view op,
      const PackedAggregateFieldUse& use) {
    storage_.report_packed_aggregate_subobject_use(where, op, use);
  }

  // Class/record alias name ("tfoo") of `e`, lowercased, if detectable.
  // Empty if the type can't be narrowed to a named object/record type.
  std::string deduce_class_alias(const ast::Expr& e);
  const ast::Expr* peel_primitive_casts(const ast::Expr* e) {
    return storage_.peel_primitive_casts(e);
  }
  bool expr_is_storage_lvalue(const ast::Expr& e) {
    return storage_.expr_is_storage_lvalue(e);
  }
  bool expr_is_untyped_storage_ref(const ast::Expr& e) {
    return storage_.expr_is_untyped_storage_ref(e);
  }
  bool expr_is_charish(const ast::Expr& e) {
    return storage_.expr_is_charish(e);
  }
  bool type_is_pcharish(const ast::TypeExpr* t) override {
    return storage_.type_is_pcharish(t);
  }
  bool type_is_metaclass(const ast::TypeExpr* t) {
    return storage_.type_is_metaclass(t);
  }
  bool type_is_reference_class(const ast::TypeExpr* t) {
    return storage_.type_is_reference_class(t);
  }
  bool expr_is_reference_class(const ast::Expr& e) {
    return storage_.expr_is_reference_class(e);
  }
  std::string member_access_op(const ast::Expr& e) {
    return storage_.member_access_op(e);
  }
  bool type_is_stringish(const ast::TypeExpr* t) override {
    return storage_.type_is_stringish(t);
  }
  bool type_is_pointerish(const ast::TypeExpr* t) {
    return storage_.type_is_pointerish(t);
  }
  bool type_is_open_array(const ast::TypeExpr* t) {
    return storage_.type_is_open_array(t);
  }
  std::string coerce_pointer_like_text(std::string_view dst_cxx,
                                       const ast::TypeExpr* dst_type,
                                       const ast::TypeExpr* src_type,
                                       const std::string& source_cxx,
                                       bool explicit_pascal_cast,
                                       bool source_is_const_storage = false) {
    return storage_.coerce_pointer_like_text(dst_cxx, dst_type, src_type,
                                             source_cxx, explicit_pascal_cast,
                                             source_is_const_storage);
  }
  std::string open_array_type_to_cxx(const ast::TypeExpr& t) {
    return types_.open_array_type_to_cxx(t);
  }
  std::string reinterpret_ref_text(const std::string& ty_cxx,
                                   const std::string& source_cxx,
                                   bool pointee_view) {
    return storage_.reinterpret_ref_text(ty_cxx, source_cxx, pointee_view);
  }
  using AbsoluteTargetInfo = EmitAbsoluteTargetInfo;
  std::optional<AbsoluteTargetInfo> resolve_absolute_target(
      const ast::VarDecl& vd) {
    return storage_.resolve_absolute_target(vd);
  }
  bool proc_accepts_zero_args(const ast::ProcDecl& decl) {
    return calls_.proc_accepts_zero_args(decl);
  }
  // Single entry point for resolving a Pascal call expression. Returns
  // both the picked decl AND the information needed to emit the C++ callee
  // text. `format_resolved_callee` is the only place the call branch
  // turns this into text -- the call branch never calls `expr_to_cxx`
  // directly on the callee, so resolution and emitted text cannot disagree.
  //
  // `callee_kind` records whether the callee needs special C++ output.
  // `FreeFunctionInUnit` is
  // the only case that overrides default expression
  // formatting; otherwise
  // the callee text comes from `expr_to_cxx(callee)`.
  // (This refactor is deliberately scoped to free-function output --
  // class-method/instance-receiver spelling still flows through the
  // existing expr_to_cxx logic. Per-overload mangling would let us
  // drop the per-arg `static_cast` workaround entirely; that is a
  // larger follow-up, intentionally not done here.)
  using ResolvedCalleeKind = tp2cc::ResolvedCalleeKind;
  using ResolvedCall = tp2cc::ResolvedCall;
  ResolvedCall resolve_call(
      const ast::Expr& callee, const std::vector<const ast::Expr*>& args);
  std::string format_resolved_callee(const ResolvedCall& resolved,
                                     const ast::Expr& callee_ast);
  void append_defaulted_trailing_call_args(
      const ast::ProcDecl* decl, std::vector<const ast::Expr*>& args) {
    calls_.append_defaulted_trailing_call_args(decl, args);
  }
  void mark_call_param_info(const ast::ProcDecl* decl,
                            std::vector<UntypedArgKind>& untyped_arg,
                            std::vector<bool>& mutable_ref_arg,
                            std::vector<const ast::TypeExpr*>& param_types) {
    calls_.mark_call_param_info(decl, untyped_arg, mutable_ref_arg,
                                param_types);
  }
  void collect_builtin_helper_param_info(
      const ast::Expr& callee, std::vector<UntypedArgKind>& untyped_arg,
      std::vector<bool>& mutable_ref_arg,
      std::vector<const ast::TypeExpr*>& param_types) {
    calls_.collect_builtin_helper_param_info(callee, untyped_arg,
                                             mutable_ref_arg, param_types);
  }
  void collect_call_param_info(const ast::Expr& callee,
                               std::vector<UntypedArgKind>& untyped_arg,
                               std::vector<bool>& mutable_ref_arg,
                               std::vector<const ast::TypeExpr*>& param_types) {
    calls_.collect_call_param_info(callee, untyped_arg, mutable_ref_arg,
                                   param_types);
  }
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             UntypedArgKind untyped_arg,
                             bool mutable_ref_arg,
                             std::string_view default_arg_unit = {}) {
    return calls_.lower_call_arg(arg, param_type, untyped_arg,
                                 mutable_ref_arg, default_arg_unit);
  }
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl,
                                           std::string_view default_arg_unit) {
    return calls_.lower_implicit_zero_arg_call(callee_text, decl,
                                               default_arg_unit);
  }
  std::string lower_property_read(Location where,
                                  const std::string& base_cxx,
                                  const std::string& class_name,
                                  const PropertyInfo& prop,
                                  const std::vector<const ast::Expr*>& indices) {
    return properties_.lower_property_read(where, base_cxx, class_name, prop,
                                           indices);
  }
  std::string lower_property_write(Location where,
                                   const std::string& base_cxx,
                                   const std::string& class_name,
                                   const PropertyInfo& prop,
                                   const std::vector<const ast::Expr*>& indices,
                                   const ast::Expr& value) {
    return properties_.lower_property_write(where, base_cxx, class_name, prop,
                                            indices, value);
  }
  std::optional<ImplicitPropertyLookup> find_implicit_class_property(
      std::string_view name) {
    return analysis_.find_implicit_class_property(name);
  }
  std::optional<std::string> maybe_lower_implicit_property_write(
      Location where,
      std::string_view name,
      const ast::Expr& value) {
    return properties_.maybe_lower_implicit_property_write(where, name, value);
  }
  std::optional<std::string> maybe_lower_class_free_member(
      const ast::Expr& base, std::string_view member_name) {
    return calls_.maybe_lower_class_free_member(base, member_name);
  }
  std::optional<std::string> maybe_lower_class_constructor_call(
      Location where, std::string_view class_name, std::string_view member_name,
      const std::vector<const ast::Expr*>& args,
      const std::vector<const ast::TypeExpr*>& param_types,
      const std::vector<UntypedArgKind>& untyped_arg,
      const std::vector<bool>& mutable_ref_arg,
      size_t explicit_arg_count,
      std::string_view default_arg_unit) {
    return calls_.maybe_lower_class_constructor_call(
        where, class_name, member_name, args, param_types, untyped_arg,
        mutable_ref_arg, explicit_arg_count, default_arg_unit);
  }
  using ResolvedKind = tp2cc::ResolvedKind;
  using ResolveResult = tp2cc::ResolveResult;
  // Qualifier: empty means unqualified lookup.  Otherwise it's a
  // unit name or a class/record alias name (both lowercased).
  using QualifierKind = tp2cc::QualifierKind;
  ResolveResult resolve_name(const std::string& name,
                             QualifierKind qk = QualifierKind::None,
                             const std::string& qualifier = {}) override {
    return lookup_.resolve_name(name, qk, qualifier);
  }

  // State: the Pascal identifier of the current function whose body we are
  // emitting (not mangled). Used by `exit`/`exit(v)` translation so we
  // know which result slot to fill.
  std::string current_fn_name;
  bool current_fn_is_function = false;
  bool current_fn_is_ctor = false;
  const ast::TypeExpr* current_fn_result_type = nullptr;
  std::string current_result_slot_name;
  // Bare `Result` resolves differently in nested procedures and nested
  // functions. Procedures inherit the nearest enclosing function result;
  // functions get their own `Result` and can reach the outer one only by
  // writing the outer function's Pascal name explicitly.
  std::string bare_result_slot_name;
  const ast::TypeExpr* bare_result_type = nullptr;
  std::string outer_result_name;
  std::string outer_result_slot_name;
  const ast::TypeExpr* outer_result_type = nullptr;
  // Stack of loop-exit labels. Pascal `break` inside a `case` arm must
  // exit the enclosing loop, but C++ `break` inside `switch` exits the
  // switch -- so we emit Pascal `break` as `goto` to a fresh label
  // placed right after each loop. Also used by `continue` -> a separate
  // label placed at the loop's re-test/re-increment point.
  std::vector<std::string> loop_break_labels;
  std::vector<std::string> loop_continue_labels;
  int loop_label_counter = 0;
  int try_stmt_counter = 0;
  int except_handler_depth = 0;
};

// ---------------------------------------------------------------------------
// Expression-type deduction. Used by the Member / Ident emitters so
// decisions like "is `obj.name` a method call or a field read?" come
// from the actual type tree, not name-matching heuristics.

const TypeExpr* Emitter::deduce_type(const Expr& e) {
  return analysis_.deduce_type(e);
}

std::string Emitter::deduce_class_alias(const Expr& e) {
  return analysis_.deduce_class_alias(e);
}

// Pascal/FPC overload resolution conversion-rank table.
//
//   rank | name                    | example
//   -----+-------------------------+----------------------------------------
//    1   | Exact                   | tidstring -> tidstring (same canonical)
//    2   | Equal                   | TSubrangeInt -> Integer (same underlying)
//    3   | ClassHierarchy          | TButton -> TControl
//    4   | IntWideningSameSign     | byte -> word -> longint (same signedness)
//    5   | RealWidening            | single -> double -> extended
//    6   | StringSameTagWiden      | ShortString<N> -> ShortString<M>, M >= N
//    7   | StringToShortString     | Char -> ShortString; PChar -> ShortString;
//        |                         | AnsiString -> ShortString
//    8   | StringToAnsiString      | Char -> AnsiString; PChar -> AnsiString;
//        |                         | ShortString -> AnsiString;
//        |                         | ShortString/AnsiString -> PChar
//    9   | OrdinalSignChange       | longint -> longword (or back)
//   10   | Variant                 | anything <-> variant
//    -   | NotViable               | no implicit conversion exists
//
// Ranks 7 vs 8 split because Pascal under `{$H-}` (compiler-bootstrap
// default) prefers ShortString-typed parameters over AnsiString-typed
// parameters when both are otherwise tied -- e.g. `upper(PChar)` picks
// `upper(string)` over `upper(ansistring)`.
//
// `var`/`const`/`out` parameters require ranks 1..3 only (Pascal does not
// allow implicit conversion through a var/out alias).
//
// A defaulted-trailing-arg fill is rank 1 and handled at the call-site
// expansion (`append_defaulted_trailing_call_args`), not here.
Emitter::ResolvedCall Emitter::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  return resolution_.resolve_call(callee, args);
}

std::string Emitter::format_resolved_callee(
    const ResolvedCall& resolved, const Expr& callee_ast) {
  // The single source of truth for C++ callee text. Every Call branch
  // emit path goes through this -- do NOT add a parallel
  // `expr_to_cxx(callee)` call elsewhere; the two would diverge.
  if (resolved.callee_kind == ResolvedCalleeKind::FreeFunctionInUnit &&
      !resolved.defining_unit.empty()) {
    return unit_namespace_prefix(resolved.defining_unit) +
           mangle(resolved.member_name);
  }
  // Fallback: receivers, class-qualified static calls, and anything
  // the resolver classified as `Unknown` flow through the existing
  // expression formatter, which already handles
  // `instance->method`/`Class::method`/`unit::name`/with-binding. We
  // keep that path here rather than reimplementing it because it is
  // tied to the emitter's deduce_class_alias / with-stack state.
  bool prev_callee_ctx = is_callee_context_;
  is_callee_context_ = true;
  std::string text = expr_to_cxx(callee_ast);
  is_callee_context_ = prev_callee_ctx;
  return text;
}

// ---------------------------------------------------------------------------
// Expressions (coarse -- just enough for constant values)

std::string Emitter::set_literal_to_cxx(const SetLit& s,
                                        const TypeExpr* target) {
  return values_.set_literal_to_cxx(s, target);
}

std::string Emitter::expr_to_cxx(const Expr& e) {
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      return uint64_literal_text(n.value);
    }
    case Kind::RealLit: {
      const auto& n = static_cast<const RealLit&>(e);
      return n.text;
    }
    case Kind::StringLit: {
      const auto& n = static_cast<const StringLit&>(e);
      // Single-character Pascal string literals are semantically chars.
      // Emit them as C++ character literals so they can appear as
      // subrange bounds (`'A'..'Z'`), case labels, and set-elements.
      // Multi-character literals are emitted as tp2cc_ShortString so that `+`
      // resolves to concatenation (not pointer arithmetic).
      if (n.value.size() == 1) {
        return "::rt::tp2cc_char_of('" + char_literal_body_to_cxx(n.value[0]) + "')";
      }
      std::string out = "::rt::tp2cc_shortstring_literal<255>(";
      bool first = true;
      for (char c : n.value) {
        if (!first) out += ", ";
        first = false;
        out += "::rt::tp2cc_char_of('";
        out += char_literal_body_to_cxx(c);
        out += "')";
      }
      out += ")";
      return out;
    }
    case Kind::NilLit: return "nullptr";
    case Kind::BoolLit: {
      const auto& n = static_cast<const BoolLit&>(e);
      return n.value ? "true" : "false";
    }
    case Kind::Ident: {
      const auto& n = static_cast<const Ident&>(e);
      if (n.name == "inherited") {
        if (!is_callee_context_ && !current_fn_name.empty() &&
            !current_class_name.empty()) {
          std::string args;
          for (const auto& arg : current_fn_param_names) {
            if (!args.empty()) args += ", ";
            args += arg;
          }
          return "inherited::" + mangle(current_fn_name) + "(" + args + ")";
        }
        return "inherited{}";
      }
      if (n.name == "self") {
        return expr_is_reference_class(e) ? "this" : "(*this)";
      }
      // LHS rewrite for `funcname := ...` assignments during Assign target
      // emission. We handle this BEFORE resolve_name so recursive
      // calls using `funcname(...)` still see the function name.
      if (!lhs_fn_rewrite.empty() && n.name == lhs_fn_rewrite) {
        return lhs_fn_rewrite_slot;
      }
      if (!lhs_outer_result_rewrite.empty() &&
          n.name == lhs_outer_result_rewrite) {
        return lhs_outer_result_rewrite_slot;
      }
      // The function-name-as-read rewrite is already in resolve_name
      // (only fires outside is_callee_context_), but we need to
      // suppress it in callee context to keep recursive call sites
      // spelled with the function's name.
      if (is_callee_context_ && current_fn_is_function &&
          !current_fn_name.empty() && n.name == current_fn_name) {
        return mangle(n.name);
      }
      // Same suppression for the *outer* function-name when emitting a
      // nested function body. Without this, a recursive call to the
      // outer function (e.g. `foreachnodestatic` calling itself from
      // inside its own nested `process_children`) gets rewritten to the
      // outer result-slot (`p_result(...)`), producing "expression
      // cannot be used as a function".
      if (is_callee_context_ && outer_result_type &&
          !outer_result_name.empty() && n.name == outer_result_name) {
        return mangle(n.name);
      }
      ResolveResult rr = resolve_name(n.name);
      if (rr.kind == ResolvedKind::Unknown) {
        report_error(n.loc, "unresolved identifier `" + n.name + "`");
      }
      if (rr.kind == ResolvedKind::UnitType) {
        if (const auto* ci = class_info_for_type_name(n.name);
            ci && ci->is_reference_type) {
          return metaclass_value_fn_cxx(n.name) + "()";
        }
        // Type aliases of reference classes (`texportlibwdosx = texportlibwin`)
        // appear in value position to mean the underlying class's metaclass.
        // Follow the alias chain to its concrete class and emit that metaclass.
        if (registry) {
          auto ait = registry->aliases.find(ascii_lower(n.name));
          if (ait != registry->aliases.end() && ait->second.target) {
            const TypeExpr* canon =
                registry->canonicalize(ait->second.target.get());
            if (canon && canon->kind == Kind::TyName) {
              const std::string& target =
                  static_cast<const TyName&>(*canon).name;
              if (const auto* tci = class_info_for_type_name(target);
                  tci && tci->is_reference_type) {
                return metaclass_value_fn_cxx(target) + "()";
              }
            }
          }
        }
      }
      // At namespace scope (block_depth == 0) we leave callable
      // names bare: Pascal typed-const initialisers reference
      // function names as procedural-pointer values.
      bool want_call = !is_callee_context_ && block_depth > 0 &&
                       rr.is_callable && rr.accepts_zero_args;
      return want_call ? lower_implicit_zero_arg_call(
                             rr.cxx, rr.proc, rr.default_arg_unit)
                       : rr.cxx;
    }
    case Kind::Binary: {
      const auto& n = static_cast<const Binary&>(e);
      // Pascal operators that don't map cleanly to a C++ infix operator
      // need dedicated lowering.
      if (n.op == BinOp::In) {
        // `elem in set` -> `set.contains(elem)`
        return "(" + expr_to_cxx(*n.rhs) + ").contains(" +
               expr_to_cxx(*n.lhs) + ")";
      }
      if (n.op == BinOp::SymDiff) {
        // tp2cc_Set symmetric difference `a >< b` -> `(a + b) - (a * b)` on our
        // tp2cc_Set<> type (rt::tp2cc_Set has union/intersect/subtract overloads).
        std::string a = expr_to_cxx(*n.lhs);
        std::string b = expr_to_cxx(*n.rhs);
        return "((" + a + " + " + b + ") - (" + a + " * " + b + "))";
      }
      if (n.op == BinOp::Is) {
        // `class` names lower to pointer types, so `x is TChild` becomes a
        // pointer dynamic_cast rather than address-taking the lhs storage.
        auto rhs_type = [&]() {
          if (n.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*n.rhs);
            if (class_info_for_type_name(id.name)) {
              TyName tn;
              tn.name = id.name;
              return type_name_to_cxx(tn);
            }
          }
          return expr_to_cxx(*n.rhs);
        }();
        return "(dynamic_cast<" + rhs_type + ">(" +
               expr_to_cxx(*n.lhs) + ") != nullptr)";
      }
      if (n.op == BinOp::As) {
        auto rhs_type = [&]() {
          if (n.rhs->kind == Kind::Ident) {
            const auto& id = static_cast<const Ident&>(*n.rhs);
            if (class_info_for_type_name(id.name)) {
              TyName tn;
              tn.name = id.name;
              return type_name_to_cxx(tn);
            }
          }
          return expr_to_cxx(*n.rhs);
        }();
        return "dynamic_cast<" + rhs_type + ">(" +
               expr_to_cxx(*n.lhs) + ")";
      }
      // Pascal `+` on `char` operands means string concatenation
      // (produces a 2-char string). C++ `char + char` is int
      // arithmetic, so wrap a char-side in tp2cc_ShortString<> to force
      // the tp2cc_ShortString `operator+` overload.
      if (n.op == BinOp::Add) {
        bool l_char = expr_is_charish(*n.lhs);
        bool r_char = expr_is_charish(*n.rhs);
        if (l_char || r_char) {
          auto wrap = [&](const Expr& x, bool want) {
            return want ? "::rt::tp2cc_shortstring_of<>(" + expr_to_cxx(x) + ")"
                        : expr_to_cxx(x);
          };
          return "(" + wrap(*n.lhs, l_char) + " + " + wrap(*n.rhs, r_char) + ")";
        }
      }
      // Pascal `and` / `or` are polymorphic: bool operands get
      // short-circuit `&&` / `||` (crucial for `assigned(p) and
      // (p^.x = y)` idioms), integer/set operands get bitwise `&` /
      // `|`. Be strict here: treating a nested flag expression like
      // `(IF_SM or IF_SM2)` as "boolean because it is an `or`" silently
      // miscompiles bitmask code into `&&`/`||`.
      std::function<bool(const Expr&)> is_bool = [&](const Expr& x) -> bool {
        // Calls to rt:: builtins and source procs: consult the resolved proc's
        // recorded return type instead of a global last-wins name map.
        if (x.kind == Kind::Call && registry) {
          const auto& c = static_cast<const Call&>(x);
          if (c.callee->kind == Kind::Ident) {
            const std::string& nm =
                static_cast<const Ident&>(*c.callee).name;
            ResolveResult rr = resolve_name(nm);
            if (rr.return_type_name == "boolean")
              return true;
          }
        }
        // Comparisons always yield bool.
        if (x.kind == Kind::Binary) {
          const auto& bx = static_cast<const Binary&>(x);
          auto bop = bx.op;
          if (bop == BinOp::Eq || bop == BinOp::NotEq ||
              bop == BinOp::Lt || bop == BinOp::Gt ||
              bop == BinOp::LtEq || bop == BinOp::GtEq ||
              bop == BinOp::In || bop == BinOp::Is)
            return true;
          if (bop == BinOp::And || bop == BinOp::Or || bop == BinOp::Xor)
            return is_bool(*bx.lhs) && is_bool(*bx.rhs);
        }
        if (x.kind == Kind::Unary &&
            static_cast<const Unary&>(x).op == UnOp::Not)
          return is_bool(*static_cast<const Unary&>(x).operand);
        if (x.kind == Kind::BoolLit) return true;
        if (!registry) return false;
        const TypeExpr* t = deduce_type(x);
        if (!t) return false;
        t = registry->canonicalize(t);
        if (!t || t->kind != Kind::TyName) return false;
        std::string nm = static_cast<const TyName&>(*t).name;
        for (auto& c : nm)
          if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return nm == "boolean" || nm == "bytebool" ||
               nm == "wordbool" || nm == "longbool";
      };
      bool logical_bool = (n.op == BinOp::And || n.op == BinOp::Or) &&
                          is_bool(*n.lhs) && is_bool(*n.rhs);
      auto emit_operand = [&](const Expr& operand, const Expr& other) {
        if (operand.kind != Kind::SetLit) return expr_to_cxx(operand);
        const TypeExpr* other_ty = deduce_type(other);
        const TypeExpr* canon = canonicalize_type(other_ty);
        if (canon && canon->kind == Kind::TySet) {
          return set_literal_to_cxx(static_cast<const SetLit&>(operand),
                                    other_ty);
        }
        return expr_to_cxx(operand);
      };
      // Pascal `{$Q+}` makes integer add/sub/mul/inc/dec raise
      // EIntOverflow on overflow. Route through a checked helper when the
      // parser snapshotted Q+ active and both operands are integer-typed;
      // floats and sets stay on plain operators.
      auto operand_is_integer = [&](const Expr& x) {
        const TypeExpr* t = deduce_type(x);
        if (!t) return false;
        t = canonicalize_type(t);
        if (!t || t->kind != Kind::TyName) return false;
        const PrimitiveInfo* pi = primitive_info(
            ascii_lower(static_cast<const TyName&>(*t).name));
        return pi && (pi->int_kind == PrimitiveIntKind::Signed ||
                      pi->int_kind == PrimitiveIntKind::Unsigned);
      };
      auto operand_is_signed_integer = [&](const Expr& x) {
        const TypeExpr* t = deduce_type(x);
        if (!t) return false;
        t = canonicalize_type(t);
        if (!t || t->kind != Kind::TyName) return false;
        const PrimitiveInfo* pi = primitive_info(
            ascii_lower(static_cast<const TyName&>(*t).name));
        return pi && pi->int_kind == PrimitiveIntKind::Signed;
      };
      auto shift_carrier = [&](const Expr& x) -> const PrimitiveInfo* {
        const TypeExpr* t = deduce_type(x);
        if (!t) return nullptr;
        t = canonicalize_type(t);
        if (!t || t->kind != Kind::TyName) return nullptr;
        const PrimitiveInfo* pi = primitive_info(
            ascii_lower(static_cast<const TyName&>(*t).name));
        return shift_carrier_primitive(pi);
      };
      if (std::string pas_op = binary_pascal_operator_token(n.op);
          !pas_op.empty()) {
        BinaryOperatorResult resolved =
            resolution_.find_binary_operator(pas_op, *n.lhs, *n.rhs);
        bool negate_resolved_operator = false;
        if (!resolved.decl && !resolved.ambiguous && n.op == BinOp::NotEq) {
          resolved = resolution_.find_binary_operator("=", *n.lhs, *n.rhs);
          negate_resolved_operator = resolved.decl != nullptr;
        }
        if (resolved.ambiguous) {
          report_error(n.loc, "ambiguous overloaded operator " + pas_op);
          return "/* ambiguous overloaded operator */";
        }
        if (resolved.decl) {
          std::vector<UntypedArgKind> untyped_arg(2, UntypedArgKind::None);
          std::vector<bool> mutable_ref_arg(2, false);
          std::vector<const TypeExpr*> param_types(2, nullptr);
          mark_call_param_info(resolved.decl, untyped_arg, mutable_ref_arg,
                               param_types);
          std::string lhs =
              lower_call_arg(*n.lhs, param_types[0], untyped_arg[0],
                             mutable_ref_arg[0]);
          std::string rhs =
              lower_call_arg(*n.rhs, param_types[1], untyped_arg[1],
                             mutable_ref_arg[1]);
          if (resolved.decl->intrinsic_operator ==
              ProcDecl::IntrinsicOperator::StringCompare) {
            std::string cmp =
                pascal_operator_cxx_token(resolved.decl->operator_token);
            return "(::rt::tp2cc_string_compare(" + lhs + ", " + rhs + ") " +
                   cmp + " 0)";
          }
          if (pascal_operator_decl_uses_named_helper(*resolved.decl)) {
            std::string fn = pascal_operator_decl_name_to_cxx(*resolved.decl);
            if (!resolved.defining_unit.empty()) {
              fn = unit_namespace_prefix(resolved.defining_unit) + fn;
            }
            std::string out = fn + "(" + lhs + ", " + rhs + ")";
            return negate_resolved_operator ? "::rt::p_not(" + out + ")"
                                             : out;
          }
          std::string op = pascal_operator_cxx_token(resolved.decl->operator_token);
          if (!op.empty()) {
            std::string out = "(" + lhs + " " + op + " " + rhs + ")";
            return negate_resolved_operator ? "::rt::p_not(" + out + ")"
                                             : out;
          }
        }
      }
      if (n.q_check &&
          (n.op == BinOp::Add || n.op == BinOp::Sub || n.op == BinOp::Mul) &&
          operand_is_integer(*n.lhs) && operand_is_integer(*n.rhs)) {
        const char* fn = (n.op == BinOp::Add) ? "tp2cc_add_checked"
                       : (n.op == BinOp::Sub) ? "tp2cc_sub_checked"
                                              : "tp2cc_mul_checked";
        return std::string("::rt::") + fn + "(" + emit_operand(*n.lhs, *n.rhs) +
               ", " + emit_operand(*n.rhs, *n.lhs) + ")";
      }
      if (!n.q_check &&
          (n.op == BinOp::Add || n.op == BinOp::Sub || n.op == BinOp::Mul) &&
          operand_is_integer(*n.lhs) && operand_is_integer(*n.rhs) &&
          (operand_is_signed_integer(*n.lhs) ||
           operand_is_signed_integer(*n.rhs))) {
        const char* fn = (n.op == BinOp::Add) ? "tp2cc_wrap_add"
                       : (n.op == BinOp::Sub) ? "tp2cc_wrap_sub"
                                              : "tp2cc_wrap_mul";
        return std::string("::rt::") + fn + "(" + emit_operand(*n.lhs, *n.rhs) +
               ", " + emit_operand(*n.rhs, *n.lhs) + ")";
      }
      // Keep the shift/rotate vocabulary precise here:
      // - shl: shift left, zeros come in on the right, high bits are discarded
      // - shr: logical shift right, zeros come in on the left, low bits are discarded
      // - rol: rotate left, high bits that fall off the left wrap around into the low end
      // - ror: rotate right, low bits that fall off the right wrap around into the high end
      //
      // ARM backend code like
      //   rotl(d, b) = (d shr (32-b)) or (d shl b)
      // is implementing `rol` by combining `shr` and `shl`; it is not
      // confusing shift and rotate. Lower Pascal `shl`/`shr` through runtime
      // helpers instead of raw C++ `<<`/`>>`, because FPC masks the shift
      // count to the carrier width and `shr` stays logical even for signed
      // integers.
      if ((n.op == BinOp::Shl || n.op == BinOp::Shr) &&
          operand_is_integer(*n.lhs)) {
        if (const PrimitiveInfo* carrier = shift_carrier(*n.lhs)) {
          const char* fn = (n.op == BinOp::Shl) ? "p_shl" : "p_shr";
          return std::string("::rt::") + fn + "<" + carrier->cxx + ">(" +
                 emit_operand(*n.lhs, *n.rhs) + ", " +
                 emit_operand(*n.rhs, *n.lhs) + ")";
        }
      }
      if ((n.op == BinOp::IntDiv || n.op == BinOp::Mod) &&
          operand_is_integer(*n.lhs) && operand_is_integer(*n.rhs)) {
        const char* fn = (n.op == BinOp::IntDiv) ? "tp2cc_int_div"
                                                 : "tp2cc_int_mod";
        return std::string("::rt::") + fn + "(" + emit_operand(*n.lhs, *n.rhs) +
               ", " + emit_operand(*n.rhs, *n.lhs) + ")";
      }
      const char* op = "?";
      switch (n.op) {
        case BinOp::Add:    op = "+"; break;
        case BinOp::Sub:    op = "-"; break;
        case BinOp::Mul:    op = "*"; break;
        case BinOp::RealDiv:op = "/"; break;
        case BinOp::IntDiv: op = "/"; break;
        case BinOp::Mod:    op = "%"; break;
        case BinOp::Shl:    op = "<<"; break;
        case BinOp::Shr:    op = ">>"; break;
        case BinOp::And:    op = logical_bool ? "&&" : "&"; break;
        case BinOp::Or:     op = logical_bool ? "||" : "|"; break;
        case BinOp::Xor:    op = "^"; break;
        case BinOp::Eq:     op = "=="; break;
        case BinOp::NotEq:  op = "!="; break;
        case BinOp::Lt:     op = "<"; break;
        case BinOp::Gt:     op = ">"; break;
        case BinOp::LtEq:   op = "<="; break;
        case BinOp::GtEq:   op = ">="; break;
        default:            op = "/*?*/"; break;
      }
      return "(" + emit_operand(*n.lhs, *n.rhs) + " " + op + " " +
             emit_operand(*n.rhs, *n.lhs) + ")";
    }
    case Kind::Unary: {
      const auto& n = static_cast<const Unary&>(e);
      if (n.op == UnOp::Not) {
        // Pascal `not` is logical for bool, bitwise for int. Dispatch
        // at compile time via a runtime helper.
        return "::rt::p_not(" + expr_to_cxx(*n.operand) + ")";
      }
      if (n.op == UnOp::Neg && n.operand &&
          n.operand->kind == Kind::IntLit &&
          static_cast<const IntLit&>(*n.operand).value ==
              (uint64_t{1} << 63)) {
        return "::std::numeric_limits<int64_t>::min()";
      }
      // `{$Q+}` extends to unary minus on integer types -- `-INT_MIN`
      // raises EIntOverflow. Under `{$Q-}`, route signed integer negation
      // through tp2cc_wrap_negate so plain `-INT_MIN` (UB in C++, but
      // silently wrapping on i386) doesn't trip UBSan.
      if (n.op == UnOp::Neg && n.operand) {
        const TypeExpr* t = deduce_type(*n.operand);
        if (t) t = canonicalize_type(t);
        if (t && t->kind == Kind::TyName) {
          const PrimitiveInfo* pi = primitive_info(
              ascii_lower(static_cast<const TyName&>(*t).name));
          if (pi && pi->int_kind == PrimitiveIntKind::Signed) {
            const char* helper = n.q_check ? "::rt::tp2cc_negate_checked"
                                           : "::rt::tp2cc_wrap_negate";
            return std::string(helper) + "(" + expr_to_cxx(*n.operand) + ")";
          }
          if (pi && pi->int_kind == PrimitiveIntKind::Unsigned && n.q_check) {
            return "::rt::tp2cc_negate_checked(" +
                   expr_to_cxx(*n.operand) + ")";
          }
        }
      }
      const char* op = (n.op == UnOp::Neg) ? "-" : "+";
      return std::string(op) + expr_to_cxx(*n.operand);
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (!suppress_packed_scalar_value_load) {
        if (auto load = packed_scalar_value_load(e)) {
          return load->text;
        }
      }
      if (auto use = direct_packed_aggregate_field_use(*m.base)) {
        report_packed_aggregate_subobject_use(
            m.loc, "nested member access", *use);
      }
      // Classify the base into one of the qualifier kinds that
      // `resolve_name` understands. The base cases are:
      //   - `inherited.name`    -> class-qualified on parent alias
      //   - `Unit.name`         -> unit-qualified (Unit must be a
      //                            known unit or in the current
      //                            unit's `uses` list)
      //   - `expr.name` where   -> class-qualified on deduced type
      //     expr's type is a
      //     named class/record
      //   - otherwise           -> unknown: emit `base.name` and let
      //                            C++ member lookup do its thing.
      auto base_is_ident = [&](std::string& out) -> bool {
        if (m.base->kind != Kind::Ident) return false;
        out = static_cast<const Ident&>(*m.base).name;
        return true;
      };

      std::string base_name;
      // `inherited.foo` -- treat as class-qualified on the parent
      // alias (C++ `inherited::foo` via the in-struct `using
      // inherited = Parent;` alias).
      if (base_is_ident(base_name) && base_name == "inherited") {
        std::string parent;
        if (registry && !current_class_name.empty()) {
          const ClassInfo* ci = registry->lookup_class(current_class_name,
                                                       current_unit_name);
          if (ci) {
            parent = ci->parent;
            if (parent.empty() && ci->is_reference_type) {
              parent = "tobject";
            }
          }
        }
        std::string text = "inherited::" + mangle(m.name);
        if (parent.empty()) return text;
        ResolveResult rr =
            resolve_name(m.name, QualifierKind::Class, parent);
        // The implicit TObject ancestor lives in the runtime, not in the
        // Pascal registry. `inherited Create;` / `inherited Destroy;` still
        // need statement-form auto-call even though name resolution cannot
        // see their zero-argument signatures there.
        bool implicit_tobject_root =
            parent == "tobject" &&
            (ascii_lower(m.name) == "create" || ascii_lower(m.name) == "destroy");
        bool same_current_method =
            !current_fn_name.empty() &&
            ascii_lower(m.name) == ascii_lower(current_fn_name);
        bool want_call = !is_callee_context_ &&
                         ((rr.is_callable && rr.accepts_zero_args) ||
                          implicit_tobject_root || same_current_method);
        return want_call ? text + "()" : text;
      }

      // Pascal's `System` unit is implicitly used everywhere. Route
      // `System.x` straight to `::rt::x` so every builtin (delete,
      // length, copy, pos, ...) resolves without a per-method stub
      // on some `p_system` object.
      if (base_is_ident(base_name) && base_name == "system") {
        return "::rt::" + mangle(m.name);
      }
      // `Unit.name` is not member access on a value named `Unit`. The shared
      // analysis helper has already applied Pascal shadowing and uses-visibility
      // rules, so this branch can emit the qualified symbol directly.
      if (auto unit_member = analysis_.resolve_unit_qualified_member(m)) {
        const ResolveResult& rr = unit_member->resolved;
        if (rr.kind == ResolvedKind::UnitType) {
          const std::string qualified =
              unit_member->unit_name + "." + m.name;
          if (const auto* ci = class_info_for_type_name(qualified);
              ci && ci->is_reference_type) {
            return metaclass_value_fn_cxx(qualified) + "()";
          }
        }
        bool want_call =
            !is_callee_context_ && rr.is_callable && rr.accepts_zero_args;
        return want_call ? rr.cxx + "()" : rr.cxx;
      }
      if (registry && base_is_ident(base_name)) {
        // `TClass.method` -- Pascal's way to call a specific
        // class's method (typically the parent's version from
        // inside an override). Emit `TClass::method`. This is a
        // type-name interpretation, so a parameter/local/current-field named
        // `TClass` must block it: in Pascal `tsym.typedef` is member access on
        // value `tsym` even when a visible type named `tsym` also exists.
        if (!analysis_.identifier_is_shadowed_value(base_name) &&
            (registry->has_class(base_name, current_unit_name) ||
             registry->records.count(base_name))) {
          if (!is_callee_context_) {
            std::vector<const Expr*> no_args;
            std::vector<const TypeExpr*> no_param_types;
            std::vector<UntypedArgKind> no_untyped_arg;
            std::vector<bool> no_mutable_ref_arg;
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    m.loc, base_name, m.name, no_args, no_param_types,
                    no_untyped_arg, no_mutable_ref_arg, no_args.size(),
                    {})) {
              return *ctor_call;
            }
          }
          ResolveResult rr =
              resolve_name(m.name, QualifierKind::Class, base_name);
          if (ascii_lower(m.name) == "classname") {
            std::string text = metaclass_value_fn_cxx(base_name) +
                               "()->" + mangle(m.name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? lower_implicit_zero_arg_call(
                                   text, rr.proc, rr.default_arg_unit)
                             : text;
          }
          std::string text =
              named_type_struct_cxx(base_name) + "::" + mangle(m.name);
          bool want_call = !is_callee_context_ &&
                           rr.is_callable && rr.accepts_zero_args;
          return want_call ? lower_implicit_zero_arg_call(
                                 text, rr.proc, rr.default_arg_unit)
                           : text;
        }
      }

      // A `class of T` value is a pointer to a metaclass descriptor, so its
      // callable surface is the descriptor's constructor/class-method thunks
      // rather than instance fields. Emit `klass->p_create` / `klass->p_load`
      // here so ordinary call lowering can treat the result like any other
      // function pointer expression.
      if (registry) {
        const std::string metaclass = metaclass_target_name(deduce_type(*m.base));
        if (!metaclass.empty()) {
          // Member's base is an *object-position* expression, not a callee.
          // Suppress callee-context auto-call suppression while emitting it
          // so e.g. `TBaseClass(classtype).Create(...)` lowers the inner
          // `classtype` with implicit-call parens. Object-member access
          // below does the same save/false/restore dance.
          bool saved_callee = is_callee_context_;
          is_callee_context_ = false;
          std::string base_cxx = expr_to_cxx(*m.base);
          is_callee_context_ = saved_callee;
          if (const auto* method = registry->lookup_class_method(
                  metaclass, m.name, current_unit_name)) {
            if (method->kind == SymKind::Constructor ||
                method->kind == SymKind::ClassMethod) {
              std::string text = base_cxx + "->" + mangle(m.name);
              bool want_call = !is_callee_context_ &&
                               method->accepts_zero_args;
              return want_call
                         ? lower_implicit_zero_arg_call(
                               text, method->decl.get(), method->defining_unit)
                         : text;
            }
          }
          if (ascii_lower(m.name) == "create") {
            std::string text = base_cxx + "->p_create";
            bool want_call = !is_callee_context_;
            return want_call ? text + "()" : text;
          }
          report_error(m.loc, "unsupported metaclass member '" + m.name + "'");
          return base_cxx + "->" + mangle(m.name);
        }
      }

      // Otherwise: object/record field/method access. Emit `base.name`
      // and auto-call if the deduced class has `name` as a
      // parameterless method.
      bool saved_callee = is_callee_context_;
      is_callee_context_ = false;
      std::string base_cxx = expr_to_cxx(*m.base);
      is_callee_context_ = saved_callee;
      if (!is_callee_context_) {
        if (auto free_call = maybe_lower_class_free_member(*m.base, m.name)) {
          return *free_call;
        }
      }
      std::string bcls = deduce_class_alias(*m.base);
      if (m.name == "classtype" || m.name == "instancesize") {
        const auto* ci = bcls.empty() ? nullptr : class_info_for_type_name(bcls);
        if ((ci && ci->is_reference_type) || expr_is_reference_class(*m.base)) {
          // Property/default-index results like `Items[i]` may already have
          // the right Pascal class alias even when the raw expression
          // no longer looks like a plain class lvalue. Recover the dynamic
          // class query from that alias so `Items[i].ClassType` still lowers
          // to an object-side method call.
          const std::string access =
              (ci && ci->is_reference_type) ? "->" : member_access_op(*m.base);
          std::string text = base_cxx + access + mangle(m.name);
          return is_callee_context_ ? text : text + "()";
        }
      }
      if (registry && !bcls.empty()) {
        if (auto* prop = registry->lookup_class_property(
                bcls, m.name, current_unit_name)) {
          if (prop->params.empty()) {
            std::vector<const Expr*> no_indices;
            return lower_property_read(m.loc, base_cxx, bcls, *prop, no_indices);
          }
        }
      }
      std::string member_cxx = mangle(m.name);
      if (registry && !bcls.empty() &&
          (registry->lookup_class_field(bcls, m.name, current_unit_name) ||
           registry->lookup_record_field(bcls, m.name))) {
        member_cxx = registry->field_cxx_name(m.name);
      }
      std::string text = base_cxx + member_access_op(*m.base) + member_cxx;
      if (is_callee_context_ || !registry) return text;
      if (bcls.empty()) return text;
      if (const auto* method = registry->lookup_class_method(
              bcls, m.name, current_unit_name)) {
        if (method->accepts_zero_args) {
          text = lower_implicit_zero_arg_call(text, method->decl.get(),
                                              method->defining_unit);
        }
      }
      return text;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      // `::rt::tp2cc_deref(p)` is equivalent to `*p` for typed pointers and
      // yields `char&` for `void*` so Pascal `ptr^` on untyped pointers
      // still compiles.
      return "::rt::tp2cc_deref(" + expr_to_cxx(*d.operand) + ")";
    }
    case Kind::AddrOf: {
      const auto& a = static_cast<const AddrOf&>(e);
      // `@TClass.method` is an unbound-method pointer in Pascal; the
      // compiler subset we handle lowers that to the thunk code slot,
      // not to a C++ member-function pointer. Detect the AST pattern
      // `AddrOf(Member(Ident=TypeName, method))` where TypeName is a
      // known class/record alias in the registry.
      if (a.operand && a.operand->kind == Kind::Member) {
        const auto& m = static_cast<const Member&>(*a.operand);
        if (m.base && m.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*m.base);
          if (registry && !analysis_.identifier_is_shadowed_value(id.name) &&
              (registry->has_class(id.name, current_unit_name) ||
                           registry->records.count(id.name))) {
            if (auto* method = registry->lookup_class_method(
                    id.name, m.name, current_unit_name);
                method && method->decl && !method->decl->is_class_method) {
              return "::rt::tp2cc_method_code<&" +
                     named_type_struct_cxx(id.name) + "::" +
                     method_pointer_helper_name(*method->decl) + ">()";
            }
          }
        }
        if (registry && m.base) {
          const std::string metaclass =
              metaclass_target_name(deduce_type(*m.base));
          if (!metaclass.empty()) {
            if (const auto* method =
                    registry->lookup_class_method(metaclass, m.name,
                                                  current_unit_name);
                method && (method->kind == SymKind::ClassMethod ||
                           method->kind == SymKind::Constructor)) {
              report_error(a.loc,
                           "cannot take address of class method through "
                           "metaclass value");
              return "nullptr";
            }
          }
        }
      }
      // `@X` where X is a Pascal untyped-var parameter: X is already
      // `void*` holding the caller's storage address, so the
      // address-of-X is just X itself.
      if (a.operand && a.operand->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*a.operand);
        if (local_untyped_params.count(id.name)) {
          return "(" + mangle(id.name) + ")";
        }
      }
      // Pascal `@p^.field` computes the address of `field` inside the
      // pointee of `p`; under fpc no memory is read through `p`. The
      // naive C++ `&deref(p).field` binds a reference to `*p`, which
      // is UB when `p` is nil. Lower through integer arithmetic +
      // reinterpret_cast<T*> so the C++ output never derefs `p`.
      // Restrict to record pointees so `offsetof` stays on standard-
      // layout types.
      //
      // When the field itself is an array, keep the emitted address as a
      // runtime proxy rather than guessing "whole array" vs "first
      // element" here. Native FPC accepts both contexts for the same
      // source `@arrfield`; the use site decides.
      if (registry && a.operand && a.operand->kind == Kind::Member) {
        const auto& m = static_cast<const Member&>(*a.operand);
        if (m.base && m.base->kind == Kind::Deref) {
          const auto& d = static_cast<const Deref&>(*m.base);
          const TypeExpr* pt = deduce_type(*d.operand);
          if (pt) pt = canonicalize_type(pt);
          if (pt && pt->kind == Kind::TyPointer) {
            const TypeExpr* target =
                canonicalize_type(static_cast<const TyPointer&>(*pt).target.get());
            if (target && target->kind == Kind::TyName) {
              const auto& tn = static_cast<const TyName&>(*target);
              std::string rec_lc = ascii_lower(tn.name);
              if (registry->records.count(rec_lc)) {
                if (const auto* fi = registry->lookup_record_field(rec_lc, m.name)) {
                  std::string struct_cxx = named_type_struct_cxx(rec_lc);
                  std::string field_cxx = registry->field_cxx_name(m.name);
                  std::string field_type_cxx =
                      fi->type ? type_to_cxx(*fi->type) : std::string("void");
                  std::string field_addr =
                      "reinterpret_cast<" + field_type_cxx +
                      "*>(reinterpret_cast<uintptr_t>(" +
                      expr_to_cxx(*d.operand) + ") + offsetof(" +
                      struct_cxx + ", " + field_cxx + "))";
                  const TypeExpr* field_ty =
                      fi->type ? canonicalize_type(fi->type.get()) : nullptr;
                  if (!a.double_addr && field_ty &&
                      field_ty->kind == Kind::TyArray &&
                      static_cast<const TyArray&>(*field_ty).array_kind ==
                          ArrayKind::Fixed) {
                    return "::rt::tp2cc_array_addr(" + field_addr + ")";
                  }
                  return field_addr;
                }
              }
            }
          }
        }
      }
      bool saved = is_callee_context_;
      is_callee_context_ = true;
      bool saved_suppress = suppress_packed_scalar_value_load;
      suppress_packed_scalar_value_load = true;
      bool saved_storage_view = storage_view_context;
      storage_view_context = true;
      // `@expr` needs the address of the Pascal storage denoted by `expr`.
      // That is not always `&expr_cxx`: for `p^`, Pascal address-of cancels
      // the dereference (`@p^` is the operand pointer expression); packed
      // fields can be byte-addressable Pascal storage that is not aligned
      // enough for typed C++ references, so reads/writes through that storage
      // need byte-copy access; and because address-of requires a variable
      // designator, `T(x)` addresses `x`'s storage viewed as `T`, not a
      // converted copy.
      auto storage = storage_.storage_designator(*a.operand);
      std::string inner = storage ? storage->text : expr_to_cxx(*a.operand);
      storage_view_context = saved_storage_view;
      suppress_packed_scalar_value_load = saved_suppress;
      is_callee_context_ = saved;
      if (!a.double_addr && registry) {
        const TypeExpr* ot = deduce_type(*a.operand);
        if (ot) ot = registry->canonicalize(ot);
        if (ot && ot->kind == Kind::TyArray &&
            static_cast<const TyArray&>(*ot).array_kind == ArrayKind::Fixed) {
          // Keep raw `@arr` as an address proxy that can convert to both
          // `^array` and `^element` forms. Native FPC accepts both
          // assignments and reports ambiguity when both overloads are
          // equally viable; hardwiring a decay here miscompiles one side.
          return "::rt::tp2cc_array_addr(" + inner + ")";
        }
      }
      if (storage) {
        return storage_.storage_designator_typed_address_value(*storage);
      }
      return "(&" + inner + ")";
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Most Pascal builtins go through ordinary name resolution, which maps
      // Pascal names to their C++ runtime helpers. The cases below need
      // emitter handling because they are type-sensitive or lower to C++
      // syntax instead of a normal function call.
      //
      // Special cases below:
      //   * `low(T)` / `high(T)` when T is a type name  -> emitted constant
      //   * `ord(x)`                                    -> runtime ordinal
      //                                                   helper
      //   * `sizeof(x)`                                 -> C++ `sizeof`
      //   * `TypeName(expr)` function-style cast        -> paren-cast when
      //                                                   the C++ type is
      //                                                   compound
      //   * `new(...)` / `dispose(...)`                 -> placement form
      // `system.low(...)` / `system.high(...)`: Pascal sometimes spells
      // these intrinsics with an explicit `system.` qualifier. System is
      // the implicit unit, so semantically these are identical to the
      // bare-name form -- forward to the same low/high lowering instead
      // of falling through to a runtime call.
      if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident &&
            ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system" &&
            (mem.name == "low" || mem.name == "high") && c.args.size() == 1) {
          const bool want_low = (mem.name == "low");
          if (c.args[0]->kind == Kind::Ident) {
            const auto& a = static_cast<const Ident&>(*c.args[0]);
            if (std::string rewrite =
                    low_high_expr_for_named_type(a.name, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
          if (const TypeExpr* at = deduce_type(*c.args[0])) {
            if (std::string rewrite = low_high_expr_for_type(at, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
        }
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        const std::string& n = id.name;
        auto arg0 = [&] {
          return c.args.empty() ? std::string("0") : expr_to_cxx(*c.args[0]);
        };
        auto arg_is_const_untyped_storage = [&](const Expr& e) {
          if (e.kind == Kind::Ident) {
            const auto& arg_id = static_cast<const Ident&>(e);
            return local_untyped_params.count(arg_id.name) &&
                   local_const_params.count(arg_id.name);
          }
          if (e.kind == Kind::AddrOf) {
            const auto& a = static_cast<const AddrOf&>(e);
            if (a.operand && a.operand->kind == Kind::Ident) {
              const auto& arg_id = static_cast<const Ident&>(*a.operand);
              return local_untyped_params.count(arg_id.name) &&
                     local_const_params.count(arg_id.name);
            }
          }
          return false;
        };
        auto is_visible_type_name = [&](const std::string& type_name) {
          const std::string low = ascii_lower(type_name);
          if (is_primitive_type(low)) return true;
          if (!runtime_named_type_cxx(low).empty()) return true;
          if (!builtin_reference_class_struct_cxx(low).empty()) {
            return true;
          }
          if (local_type_aliases_scoped.count(low) || local_enums.count(low)) {
            return true;
          }
          if (ResolveResult rr = resolve_name(type_name);
              rr.kind == ResolvedKind::UnitType) {
            return true;
          }
          if (registry) {
            return registry->has_class(low, current_unit_name) ||
                   registry->records.count(low) ||
                   registry->enums.count(low) ||
                   registry->aliases.count(low);
          }
          return false;
        };
        auto unit_qualified_type_name =
            [&](const Expr& expr) -> std::optional<std::string> {
          if (!registry || expr.kind != Kind::Member) return std::nullopt;
          const auto& mem = static_cast<const Member&>(expr);
          auto unit_member = analysis_.resolve_unit_qualified_member(mem);
          if (!unit_member ||
              unit_member->resolved.kind != ResolvedKind::UnitType) {
            return std::nullopt;
          }
          return unit_member->unit_name + "." + mem.name;
        };

        // Pascal `low` / `high` are type-driven:
        //   `high(longint)`   -> max value of the type
        //   `high(a)`         -> max value of a's type
        //   `high(arr)`       -> last array index of arr's type
        // so lower them from the resolved Pascal type rather than leaving
        // a raw runtime call in the generated C++.
        if ((n == "low" || n == "high") && c.args.size() == 1) {
          const bool want_low = (n == "low");
          if (c.args[0]->kind == Kind::Ident) {
            const auto& a = static_cast<const Ident&>(*c.args[0]);
            if (std::string rewrite =
                    low_high_expr_for_named_type(a.name, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
          }
          const TypeExpr* at = deduce_type(*c.args[0]);
          if (at) {
            if (std::string rewrite = low_high_expr_for_type(at, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
            const TypeExpr* canon = canonicalize_type(at);
            if (canon && canon->kind == Kind::TyArray) {
              const auto& arr = static_cast<const TyArray&>(*canon);
              if (arr.array_kind != ArrayKind::Fixed) {
                return want_low ? "0"
                                : "(::rt::p_length(" + expr_to_cxx(*c.args[0]) +
                                      ") - 1)";
              }
              return type_to_cxx(*at) + "::" + n + "()";
            }
          }
        }

        if (n == "ord" && c.args.size() == 1) {
          return "::rt::p_ord(" + arg0() + ")";
        }

        if (n == "sizeof" && c.args.size() == 1) {
          // Pascal `sizeof` returns `longint`, but C++ `sizeof` is
          // `size_t` (typically `unsigned int` or `unsigned long`). Wrap
          // the result so arithmetic and overload resolution at the call
          // site see the Pascal-correct signed int32_t -- otherwise e.g.
          // `tostr(sizeof(aint))` ends up ambiguous against the
          // qword/int64/longint overload set.
          std::string inner;
          if (c.args[0]->kind == Kind::Ident) {
            const auto& tn = static_cast<const Ident&>(*c.args[0]);
            if (is_visible_type_name(tn.name)) {
              inner = "sizeof(" + type_name_text_to_cxx(tn.name) + ")";
            }
          }
          if (inner.empty()) {
            if (auto qualified = unit_qualified_type_name(*c.args[0])) {
              inner = "sizeof(" + type_name_text_to_cxx(*qualified) + ")";
            }
          }
          if (inner.empty()) inner = "sizeof(" + expr_to_cxx(*c.args[0]) + ")";
          return "static_cast<int32_t>(" + inner + ")";
        } else if (n == "unaligned" && c.args.size() == 1) {
          // Pascal `unaligned(x)` is an explicit bytewise load/store view,
          // not permission to manufacture a misaligned `T&`. When the
          // argument denotes storage, load it via memcpy from the storage
          // address. Non-storage forms fall back to the runtime helper.
          if (auto storage = storage_.bytewise_storage_ref(*c.args[0])) {
            return "::rt::tp2cc_unaligned_load<" + storage->elem_cxx + ">(" +
                   storage->void_ptr_text + ")";
          }
          return "::rt::p_unaligned(" + arg0() + ")";
        } else if (n == "typeof" && c.args.size() == 1 &&
                   c.args[0]->kind == Kind::Ident && registry) {
          // Pascal `typeof(T)` takes a TYPE NAME, not a value. In C++
          // we have no VMT-by-type-name runtime object; stub as
          // `nullptr` with a dummy template-arg tag so the expression
          // at least compiles. Users of this value compare it for
          // equality/inequality at runtime only.
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          if (registry->has_class(a.name, current_unit_name) ||
              registry->records.count(a.name)) {
            return "((void*)nullptr)";
          }
        } else if (c.args.size() == 1 && is_primitive_type(n)) {
          // Function-style type cast in expression context.
          // Only the explicit lvalue forms handled elsewhere
          // (`T(lv) := ...`, `inc(T(lv))`, `dec(T(lv))`) reinterpret
          // storage. Plain `T(expr)` remains a value conversion.
          if (n == "string") {
            report_error(c.loc,
                         "internal unresolved H-mode `string' typecast");
            return "/* unresolved string typecast */";
          }
          if (n == "shortstring") {
            return "::rt::tp2cc_shortstring_of<>(" + arg0() + ")";
          }
          if (n == "ansistring" || n == "utf8string") {
            return "::rt::tp2cc_ansistring_of(" + arg0() + ")";
          }
          if (n == "text") {
            if (auto view = storage_.typecast_storage_view(c)) {
              // `text(p^)` is a typed view of file storage, commonly used
              // before `write`/`writeln`. Keep it as an lvalue so the file
              // overload receives the original TextFile, not a temporary copy.
              return reinterpret_ref_text(view->target_cxx, view->source_cxx,
                                          view->pointee_view);
            }
          }
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          if (peeled && expr_is_untyped_storage_ref(*c.args[0])) {
            return "::rt::tp2cc_reinterpret_load<" + primitive_type_cxx(n) +
                   ">(" + expr_to_cxx(*peeled) + ")";
          }
          if (const PrimitiveInfo* info = primitive_info(n);
              info && (info->int_kind == PrimitiveIntKind::Signed ||
                       info->int_kind == PrimitiveIntKind::Unsigned)) {
            const TypeExpr* source_ty = canonicalize_type(deduce_type(*c.args[0]));
            bool source_is_set =
                c.args[0]->kind == Kind::SetLit ||
                (source_ty && source_ty->kind == Kind::TySet);
            if (source_is_set) {
              // Pascal `longint(set)` packs the set's element bits into
              // an integer (element i -> bit i). The set source can be
              // an rvalue literal `[a,b,c]`, so don't gate on lvalue.
              return "::rt::tp2cc_set_to_int<" + primitive_type_cxx(n) +
                     ">(" + arg0() + ")";
            }
          }
          if (const TypeExpr* source_ty =
                  canonicalize_type(deduce_type(*c.args[0]));
              source_ty &&
              (source_ty->kind == Kind::TyArray ||
               source_ty->kind == Kind::TyRecord ||
               source_ty->kind == Kind::TyObject ||
               source_ty->kind == Kind::TyProcedural)) {
            // FPC accepts same-size aggregate-to-scalar casts as
            // representation casts. This is a value context, so first build the
            // source value, then copy its bytes into the scalar target; storage
            // contexts request storage views before reaching this branch.
            return "::rt::tp2cc_reinterpret_copy<" + primitive_type_cxx(n) +
                   ">(" + arg0() + ")";
          }
          if (primitive_name_is_charish(n)) {
            return "::rt::p_chr(" + arg0() + ")";
          }
          if (n == "pointer" || n == "pchar" || n == "pansichar" ||
              n == "ppchar") {
            TyName cast_name;
            cast_name.name = n;
            std::string source =
                (peeled && expr_is_storage_lvalue(*c.args[0]))
                    ? expr_to_cxx(*peeled)
                    : arg0();
            std::string coerced = coerce_pointer_like_text(
                primitive_type_cxx(n), &cast_name, deduce_type(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true,
                arg_is_const_untyped_storage(*c.args[0]));
            if (coerced != source) return coerced;
            if (peeled && expr_is_storage_lvalue(*c.args[0])) {
              return "((" + primitive_type_cxx(n) + ")(" +
                     expr_to_cxx(*peeled) + "))";
            }
          }
          if (expr_is_charish(*c.args[0])) {
            return "((" + primitive_type_cxx(n) + ")(::rt::p_ord(" +
                   arg0() + ")))";
          }
          TyName target;
          target.name = n;
          if (auto conv = resolution_.find_assignment_operator(
                  deduce_type(*c.args[0]), &target);
              conv.decl) {
            std::string fn = pascal_assignment_operator_helper_name(*conv.decl);
            if (!conv.defining_unit.empty()) {
              fn = unit_namespace_prefix(conv.defining_unit) + fn;
            }
            return fn + "(" + arg0() + ")";
          }
          if (auto lit =
                  maybe_convert_const_int_expr(*c.args[0], &target, true)) {
            return *lit;
          }
          return "((" + primitive_type_cxx(n) + ")(" + arg0() + "))";
        } else if (c.args.size() == 1 && n != "inc" && n != "dec") {
          if (is_builtin_reference_class_name(n)) {
            // `TObject(expr)` is a pointer cast even though `TObject` itself
            // comes from the runtime root instead of the registry.
            TyName cast_name;
            cast_name.name = n;
            std::string coerced = coerce_pointer_like_text(
                type_name_to_cxx(cast_name), &cast_name,
                deduce_type(*c.args[0]), arg0(),
                /*explicit_pascal_cast=*/true);
            if (coerced != arg0()) return coerced;
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          if (registry) {
            const ClassInfo* ci = class_info_for_type_name(n);
            if (ci && ci->is_reference_type) {
              // Direct named class casts (`TNode(p)`) do not go through the
              // alias table, because reference classes are registered as
              // classes rather than aliases. Treat them as pointer casts here.
              TyName cast_name;
              cast_name.name = n;
              std::string coerced = coerce_pointer_like_text(
                  type_name_to_cxx(cast_name), &cast_name,
                  deduce_type(*c.args[0]), arg0(),
                  /*explicit_pascal_cast=*/true);
              if (coerced != arg0()) return coerced;
              return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
            }
          }
          const TypeExpr* cast_ty = lookup_named_type_expr(n);
          if (cast_ty) cast_ty = canonicalize_type(cast_ty);
          if (cast_ty && cast_ty->kind == Kind::TyMetaclass) {
            std::string source = const_value_to_cxx(*c.args[0], cast_ty,
                                                    /*explicit_conversion=*/true);
            std::string coerced = coerce_pointer_like_text(
                type_name_text_to_cxx(n), cast_ty, deduce_type(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true);
            if (coerced != source) return coerced;
            return "((" + type_name_text_to_cxx(n) + ")(" + source + "))";
          }
          if (cast_ty && cast_ty->kind == Kind::TyPointer) {
            const Expr* peeled = peel_primitive_casts(c.args[0].get());
            std::string source =
                (peeled && expr_is_storage_lvalue(*c.args[0]))
                    ? expr_to_cxx(*peeled)
                    : arg0();
            std::string coerced = coerce_pointer_like_text(
                type_name_text_to_cxx(n), cast_ty, deduce_type(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true,
                arg_is_const_untyped_storage(*c.args[0]));
            if (coerced != source) return coerced;
            return "((" + type_name_text_to_cxx(n) + ")(" + source + "))";
          }
          if (cast_ty && cast_ty->kind == Kind::TySet) {
            return "::rt::tp2cc_set_cast<" + type_to_cxx(*cast_ty) + ">(" +
                   arg0() + ")";
          }
          {
            TyName target;
            target.name = n;
            if (auto conv = resolution_.find_assignment_operator(
                    deduce_type(*c.args[0]), &target);
                conv.decl) {
              std::string fn =
                  pascal_assignment_operator_helper_name(*conv.decl);
              if (!conv.defining_unit.empty()) {
                fn = unit_namespace_prefix(conv.defining_unit) + fn;
              }
              return fn + "(" + arg0() + ")";
            }
          }
          if (cast_ty && type_is_reference_class(cast_ty)) {
            // `TClass(expr)` is a class-pointer cast in Pascal, not a C++
            // direct-initialisation attempt. Emit an explicit pointer cast so
            // bootstrap casts like `TLinkedListItemClass(ClassType)` keep
            // pointer semantics instead of turning into constructor calls.
            TyName cast_name;
            cast_name.name = n;
            std::string coerced = coerce_pointer_like_text(
                type_name_to_cxx(cast_name), cast_ty,
                deduce_type(*c.args[0]), arg0(),
                /*explicit_pascal_cast=*/true);
            if (coerced != arg0()) return coerced;
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          bool named_storage_view_type =
              registry &&
              (registry->records.count(n) ||
               registry->has_class(n, current_unit_name));
          bool aggregate_alias =
              cast_ty && (cast_ty->kind == Kind::TyArray ||
                          cast_ty->kind == Kind::TyRecord ||
                          cast_ty->kind == Kind::TyObject ||
                          cast_ty->kind == Kind::TyProcedural);
          auto storage_view = storage_.typecast_storage_view(c);
          if (storage_view) {
            if (storage_view_context &&
                (aggregate_alias || named_storage_view_type)) {
              // Pascal decides `T(x)` from the enclosing context. In a storage
              // context this is not an aggregate temporary: it is the same
              // variable designator viewed as `T`, so assignment, `@`, and
              // var/out actuals can mutate or address the original storage.
              return reinterpret_ref_text(storage_view->target_cxx,
                                          storage_view->source_cxx,
                                          storage_view->pointee_view);
            }
          }
          if (cast_ty && cast_ty->kind == Kind::TyArray) {
            const auto& arr = static_cast<const TyArray&>(*cast_ty);
            const TypeExpr* elem =
                arr.element ? canonicalize_type(arr.element.get()) : nullptr;
            if (arr.dims.size() == 1 &&
                (tyname_is(elem, "byte") || tyname_is_charish(elem))) {
              // Array casts are value casts in expression context. Pascal
              // arrays are first-class values: assigning or passing one copies
              // the whole array, and there is no C-style array-to-pointer decay.
              // Untyped `var` storage already denotes caller bytes, so copy from
              // that address; otherwise copy bytes from the source value.
              if (storage_view && storage_view->source_is_untyped_storage) {
                return "::rt::tp2cc_reinterpret_load<" +
                       type_name_text_to_cxx(n) + ">(" +
                       storage_view->source_cxx + ")";
              }
              return "::rt::tp2cc_reinterpret_bytes<" +
                     type_name_text_to_cxx(n) + ">(" + arg0() + ")";
            }
          }
          if (aggregate_alias || named_storage_view_type) {
            // Value context produces a copied aggregate value, not a storage
            // alias. Untyped `var` sources already are storage addresses; other
            // sources are ordinary values whose object representation is copied.
            if (storage_view && storage_view->source_is_untyped_storage) {
              return "::rt::tp2cc_reinterpret_load<" +
                     type_name_text_to_cxx(n) + ">(" +
                     storage_view->source_cxx + ")";
            }
            return "::rt::tp2cc_reinterpret_copy<" + type_name_text_to_cxx(n) +
                   ">(" + arg0() + ")";
          }
          if (cast_ty) {
            return "((" + type_name_text_to_cxx(n) + ")(" + arg0() + "))";
          }
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2)) {
          bool saved_storage_view = storage_view_context;
          storage_view_context = true;
          // `Inc`/`Dec` mutate a Pascal variable designator. The designator
          // decides whether that is an ordinary C++ lvalue, a reinterpreted
          // lvalue, or bytewise storage that must use memcpy-style helpers.
          auto storage = storage_.storage_designator(*c.args[0]);
          storage_view_context = saved_storage_view;
          if (storage) {
            return storage_.storage_designator_inc_dec(
                *storage, n == "inc",
                c.args.size() == 2 ? expr_to_cxx(*c.args[1]) : std::string{});
          }
          // Fall through to generic emission for invalid non-storage args.
        } else if (n == "new" && !c.args.empty()) {
          // Expression-form `new(T)` or `new(T, Ctor(args))`. The first
          // argument is a pointer type name, not a value expression; lowering
          // it through type spelling keeps Pascal's type/value namespaces
          // separate in generated C++.
          // STUB: if the type is one of our stub target-back-end
          // aliases (t_win32 / t_os2 / t_go32v* classes that got
          // skipped), emit `nullptr` -- the call site is inside an
          // unreachable `case target_info.target of` arm.
          if (c.args[0]->kind == Kind::Ident) {
            const std::string& tname =
                static_cast<const Ident&>(*c.args[0]).name;
            static const std::unordered_set<std::string> stub_targets = {
                "pimportlibwin32", "timportlibwin32",
                "pimportlibos2",   "timportlibos2",
                "pimportlibgo32v2","timportlibgo32v2",
                "pexportlibwin32", "texportlibwin32",
                "pexportlibos2",   "texportlibos2",
                "pexportlibgo32v2","texportlibgo32v2",
                "plinkerwin32",    "tlinkerwin32",
                "plinkeros2",      "tlinkeros2",
                "plinkergo32v1",   "tlinkergo32v1",
                "plinkergo32v2",   "tlinkergo32v2",
            };
            if (stub_targets.count(tname)) return "nullptr";
          }
          std::string t = (c.args[0]->kind == Kind::Ident)
                              ? type_name_text_to_cxx(
                                    static_cast<const Ident&>(*c.args[0]).name)
                              : expr_to_cxx(*c.args[0]);
          std::string make =
              "([&]{ auto tp2cc_ptr = static_cast<::std::remove_pointer_t<" +
              t + ">*>(nullptr); ::rt::p_new(tp2cc_ptr); return tp2cc_ptr; }())";
          if (c.args.size() == 1) return make;
          // c.args[1] is either Call(Ctor, args) or Ident(Ctor).
          std::string method;
          std::string margs;
          const auto& second = *c.args[1];
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            const ProcDecl* ctor_decl = nullptr;
            std::string ctor_default_arg_unit;
            if (registry && c.args[0]->kind == Kind::Ident &&
                cc.callee->kind == Kind::Ident) {
              TyName ptr_type;
              ptr_type.name = static_cast<const Ident&>(*c.args[0]).name;
              std::string pointee = registry->pointer_target_type_name(&ptr_type);
              if (!pointee.empty()) {
                if (auto* m = registry->lookup_class_method(
                        pointee, static_cast<const Ident&>(*cc.callee).name,
                        current_unit_name)) {
                  ctor_decl = m->decl.get();
                  ctor_default_arg_unit = m->defining_unit;
                }
              }
            }
            std::vector<const Expr*> ctor_args;
            ctor_args.reserve(cc.args.size());
            for (const auto& arg : cc.args) ctor_args.push_back(arg.get());
            const size_t explicit_ctor_arg_count = ctor_args.size();
            append_defaulted_trailing_call_args(ctor_decl, ctor_args);
            std::vector<UntypedArgKind> untyped_arg(ctor_args.size(),
                                                    UntypedArgKind::None);
            std::vector<bool> mutable_ref_arg(ctor_args.size(), false);
            std::vector<const TypeExpr*> param_types(ctor_args.size(), nullptr);
            mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                 param_types);
            for (size_t i = 0; i < ctor_args.size(); ++i) {
              if (i) margs += ", ";
              const std::string_view default_arg_unit =
                  i >= explicit_ctor_arg_count
                      ? std::string_view(ctor_default_arg_unit)
                      : std::string_view{};
              margs += lower_call_arg(*ctor_args[i], param_types[i],
                                      untyped_arg[i], mutable_ref_arg[i],
                                      default_arg_unit);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          return "([&]{ auto tp2cc_ptr = " + make + "; tp2cc_ptr->" +
                 method + "(" + margs + "); return tp2cc_ptr; }())";
        }
      }
      // Pointer cast `T(lv)` where T resolves to a pointer type AND
      // the argument is an addressable expression (Ident, Member,
      // Index, Deref): emit `(*(T*)&(lv))` so the result is an lvalue
      // and can bind to a `var`-parameter reference. Pascal routinely
      // casts pointer storage this way
      // (e.g. `resolvederef(pderef(def), ...)`). If the argument is
      // an rvalue (a call result, arithmetic, another cast, or a
      // parameterless-method access like `inherited.name` which the
      // emitter silently calls), a plain functional cast works and
      // an address-of would not compile.
      if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        bool cast_to_pointer = false;
        std::string cast_type_cxx;
        const TypeExpr* pointer_cast_ty = nullptr;
        if (id.name == "pointer") {
          cast_to_pointer = true;
          cast_type_cxx = "void*";
          static TyName pointer_type_name;
          pointer_type_name.name = "pointer";
          pointer_cast_ty = &pointer_type_name;
        } else {
          const TypeExpr* tgt = nullptr;
          auto lit = local_type_aliases_scoped.find(id.name);
          if (lit != local_type_aliases_scoped.end() && lit->second) {
            tgt = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(id.name);
            if (ait != registry->aliases.end() && ait->second.target.get()) {
              tgt = registry->canonicalize(ait->second.target.get());
            }
          }
          if (tgt && tgt->kind == Kind::TyPointer) {
            cast_to_pointer = true;
            cast_type_cxx = type_name_text_to_cxx(id.name);
            pointer_cast_ty = tgt;
          }
        }
        if (cast_to_pointer) {
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          std::string source =
              (peeled && expr_is_storage_lvalue(*c.args[0]))
                  ? expr_to_cxx(*peeled)
                  : expr_to_cxx(*c.args[0]);
          std::string coerced = coerce_pointer_like_text(
              cast_type_cxx, pointer_cast_ty, deduce_type(*c.args[0]), source,
              /*explicit_pascal_cast=*/true);
          if (coerced != source) return coerced;
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            return "((" + cast_type_cxx + ")(" + expr_to_cxx(*peeled) + "))";
          }
          return "((" + cast_type_cxx + ")(" + expr_to_cxx(*c.args[0]) + "))";
        }
      }
      if (c.args.size() == 1 && c.callee->kind == Kind::Member && registry) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        // Same `Member` AST node as a call, but semantically this can be a
        // unit-qualified typecast: `Unit.Type(expr)`.
        if (auto unit_member = analysis_.resolve_unit_qualified_member(mem)) {
          if (unit_member->resolved.kind == ResolvedKind::UnitType) {
            // The parser uses Call for both calls and Pascal typecasts, so
            // `Unit.Type(expr)` reaches this point as a member callee. Once the
            // shared resolver has proven the member is a unit-qualified type,
            // lower it as an explicit cast instead of sending it through
            // procedure overload resolution.
            const std::string qualified =
                unit_member->unit_name + "." + mem.name;
            const TypeExpr* cast_ty = lookup_named_type_expr(qualified);
            if (cast_ty) cast_ty = canonicalize_type(cast_ty);
            if (cast_ty && cast_ty->kind == Kind::TyPointer) {
              // Pointer casts are still Pascal value conversions, but their
              // source may be a typed storage view such as `T(x)`. Peel only
              // primitive storage-view casts before the pointer coercion so the
              // emitted C++ casts the original storage expression, not a
              // temporary/reference spelling built for another target type.
              const Expr* peeled = peel_primitive_casts(c.args[0].get());
              std::string source =
                  (peeled && expr_is_storage_lvalue(*c.args[0]))
                      ? expr_to_cxx(*peeled)
                      : expr_to_cxx(*c.args[0]);
              std::string coerced = coerce_pointer_like_text(
                  type_name_text_to_cxx(qualified), cast_ty,
                  deduce_type(*c.args[0]), source,
                  /*explicit_pascal_cast=*/true);
              if (coerced != source) return coerced;
              if (peeled && expr_is_storage_lvalue(*c.args[0])) {
                return "((" + type_name_text_to_cxx(qualified) + ")(" +
                       expr_to_cxx(*peeled) + "))";
              }
              return "((" + type_name_text_to_cxx(qualified) + ")(" +
                     source + "))";
            }
            if (cast_ty) {
              return "((" + type_name_text_to_cxx(qualified) + ")(" +
                     expr_to_cxx(*c.args[0]) + "))";
            }
          }
        }
      }
      // Build the explicit-args list first so overload resolution can score
      // candidates by their static types; defaults are filled per-overload
      // after the pick.
      std::vector<const Expr*> call_args;
      call_args.reserve(c.args.size());
      for (const auto& arg : c.args) call_args.push_back(arg.get());
      const size_t explicit_arg_count = call_args.size();
      ResolvedCall resolved = resolve_call(*c.callee, call_args);
      if (resolved.ambiguous) {
        // Pascal-level ambiguous call: two or more overloads were
        // mutually incomparable on the conversion-rank vector. Report
        // and emit a placeholder so the build fails loudly rather than
        // silently picking one and hoping C++ figures it out.
        std::string name;
        if (c.callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*c.callee).name;
        } else if (c.callee->kind == Kind::Member) {
          name = static_cast<const Member&>(*c.callee).name;
        }
        report_error(c.loc,
                     "ambiguous call to overloaded '" + name +
                     "': no candidate dominates on argument conversions");
        return "/* ambiguous call to '" + name + "' */";
      }
      const ProcDecl* call_decl = resolved.decl;
      append_defaulted_trailing_call_args(call_decl, call_args);
      std::vector<UntypedArgKind> call_untyped_arg(call_args.size(),
                                                   UntypedArgKind::None);
      std::vector<bool> call_mutable_ref_arg(call_args.size(), false);
      std::vector<const TypeExpr*> call_param_types(call_args.size(), nullptr);
      if (call_decl) {
        // Use the resolver's picked decl directly so per-arg types match
        // exactly the overload we are landing on. The builtin-helper hook
        // (move/fillchar/etc.) still runs because it overrides specific
        // slots that the decl-based path leaves null.
        collect_builtin_helper_param_info(*c.callee, call_untyped_arg,
                                          call_mutable_ref_arg,
                                          call_param_types);
        mark_call_param_info(call_decl, call_untyped_arg,
                             call_mutable_ref_arg, call_param_types);
      } else {
        collect_call_param_info(*c.callee, call_untyped_arg,
                                call_mutable_ref_arg, call_param_types);
      }
      if (c.args.empty() && c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (auto free_call = maybe_lower_class_free_member(*mem.base, mem.name)) {
          return *free_call;
        }
      }
      if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          if (!analysis_.identifier_is_shadowed_value(id.name) &&
              registry->has_class(id.name, current_unit_name)) {
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    c.loc, id.name, mem.name, call_args, call_param_types,
                    call_untyped_arg, call_mutable_ref_arg, explicit_arg_count,
                    resolved.default_arg_unit)) {
              return *ctor_call;
            }
          }
        }
      }
      std::string callee_text = format_resolved_callee(resolved, *c.callee);
      bool is_tpexcept_setjmp = false;
      if (c.args.size() == 1) {
        if (c.callee->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*c.callee);
          if (id.name == "setjmp") {
            ResolveResult rr = resolve_name(id.name);
            is_tpexcept_setjmp = (rr.cxx == "::p_tpexcept::p_setjmp");
          }
        } else if (c.callee->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*c.callee);
          if (mem.name == "setjmp" && mem.base->kind == Kind::Ident &&
              static_cast<const Ident&>(*mem.base).name == "tpexcept") {
            is_tpexcept_setjmp = true;
          }
        }
      }
      if (is_tpexcept_setjmp) {
        return "setjmp(::p_tpexcept::p_detail::p_state_for(&(" +
               expr_to_cxx(*c.args[0]) + ")).p_env)";
      }
      std::string out = callee_text + "(";
      for (size_t i = 0; i < call_args.size(); ++i) {
        if (i) out += ", ";
        const std::string_view default_arg_unit =
            i >= explicit_arg_count ? std::string_view(resolved.default_arg_unit)
                                    : std::string_view{};
        std::string arg_text = lower_call_arg(*call_args[i], call_param_types[i],
                                              call_untyped_arg[i],
                                              call_mutable_ref_arg[i],
                                              default_arg_unit);
        // For overloaded callees, force the C++ compiler onto the picked
        // overload by casting every value-arg to the picked param's type.
        // C++ ranks competing implicit conversions equally in many cases
        // (ShortString-to-ShortString vs ShortString-to-AnsiString;
        // uint32->uint64 vs uint32->int32) so without this cast the C++
        // call is ambiguous even though Pascal already chose. Skip for
        // var/const/out (the call site passes the storage as-is), for
        // untyped params (no concrete C++ type to cast to), and for
        // procedural-type params (`static_cast<funcptr>(value)` is
        // ill-formed and overload resolution against a function-pointer
        // slot does not produce ambiguity with value-type overloads).
        if (resolved.needs_arg_casts && call_param_types[i] &&
            !call_mutable_ref_arg[i] &&
            call_untyped_arg[i] == UntypedArgKind::None) {
          const TypeExpr* canon_pt = canonicalize_type(call_param_types[i]);
          if (!canon_pt || canon_pt->kind != Kind::TyProcedural) {
            arg_text = "static_cast<" + type_to_cxx(*call_param_types[i]) +
                       ">(" + arg_text + ")";
          }
        }
        out += arg_text;
      }
      out += ")";
      return out;
    }
    case Kind::Index: {
      const auto& i = static_cast<const Index&>(e);
      if (auto use = direct_packed_aggregate_field_use(*i.base);
          use && !type_is_byte_aligned_packed_index_carrier(deduce_type(*i.base))) {
        report_packed_aggregate_subobject_use(i.loc, "indexing", *use);
      }
      std::vector<const Expr*> indices;
      for (const auto& idx : i.indices) indices.push_back(idx.get());
      if (registry && i.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*i.base);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_class_property(
                  cls, mem.name, current_unit_name)) {
            if (!prop->params.empty()) {
              return lower_property_read(i.loc, expr_to_cxx(*mem.base), cls,
                                         *prop, indices);
            }
          }
        }
      }
      if (registry && i.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*i.base);
        if (auto found = find_implicit_class_property(id.name);
            found && found->prop && !found->prop->params.empty()) {
          return lower_property_read(i.loc, found->base_cxx, found->class_name,
                                     *found->prop, indices);
        }
      }
      if (registry) {
        std::string cls = deduce_class_alias(*i.base);
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_default_property(
                  cls, current_unit_name)) {
            return lower_property_read(i.loc, expr_to_cxx(*i.base), cls, *prop,
                                       indices);
          }
        }
      }
      if (auto storage = storage_.storage_designator(i);
          storage && storage->is_special()) {
        return storage_.storage_designator_value(*storage);
      }
      std::string out = expr_to_cxx(*i.base);
      for (const auto& idx : i.indices) out += "[" + expr_to_cxx(*idx) + "]";
      return out;
    }
    case Kind::SetLit: {
      return set_literal_to_cxx(static_cast<const SetLit&>(e));
    }
    case Kind::Range: {
      const auto& r = static_cast<const Range&>(e);
      return "::rt::range(" + expr_to_cxx(*r.lo) + ", " + expr_to_cxx(*r.hi) + ")";
    }
    case Kind::ArrayConst: {
      const auto& a = static_cast<const ArrayConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < a.elements.size(); ++i) {
        if (i) out += ", ";
        out += expr_to_cxx(*a.elements[i]);
      }
      out += "}";
      return out;
    }
    case Kind::RecordConst: {
      const auto& r = static_cast<const RecordConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < r.fields.size(); ++i) {
        if (i) out += ", ";
        const std::string field_name =
            registry ? registry->field_cxx_name(r.fields[i].first)
                     : mangle(r.fields[i].first);
        out += "." + field_name + " = " +
               expr_to_cxx(*r.fields[i].second);
      }
      out += "}";
      return out;
    }
    default:
      return "/* unsupported-expr */ 0";
  }
}

std::string Emitter::const_value_to_cxx(const Expr& e,
                                        const TypeExpr* target,
                                        bool explicit_conversion) {
  return values_.const_value_to_cxx(e, target, explicit_conversion);
}

// FPC constant conversions first evaluate the integer constant
// expression, then convert that value to the destination type. Keep
// the same split here so assignments/calls/typed consts all share one
// checked path instead of ad-hoc literal special cases.
std::optional<std::string> Emitter::maybe_convert_const_int_expr(
    const Expr& e, const TypeExpr* target, bool explicit_conversion) {
  return values_.maybe_convert_const_int_expr(e, target, explicit_conversion);
}

// ---------------------------------------------------------------------------
// Declarations

void Emitter::emit_decl(const Decl& d, bool in_header) {
  decls_.emit_decl(d, in_header);
}

// ---------------------------------------------------------------------------
// Statements

void Emitter::emit_stmt(const Stmt& s) {
  stmts_.emit_stmt(s);
}

// Forward decl so emit_proc_body / emit_nested_proc_lambda can call it
// to forward-declare record/object types in local type-decls before
// pointer aliases that reference them.
static void emit_forward_struct_decls_impl(
    Emitter& e, const std::vector<ast::DeclPtr>& decls);

void Emitter::emit_proc_body(const ProcDecl& pd) {
  procs_.emit_proc_body(pd);
}

void Emitter::emit_nested_proc_lambda(const ProcDecl& pd) {
  procs_.emit_nested_proc_lambda(pd);
}

void Emitter::emit_forward_struct_decls(
    const std::vector<ast::DeclPtr>& decls) {
  emit_forward_struct_decls_impl(*this, decls);
}

// ---------------------------------------------------------------------------
// Unit

// Scan the decl list and emit forward declarations for every record/object/interface
// type, so a pointer type that textually precedes its target still compiles.
static void emit_forward_struct_decls_impl(Emitter& e,
                                           const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    if (td.type->kind == Kind::TyRecord || td.type->kind == Kind::TyObject ||
        td.type->kind == Kind::TyInterface) {
      e.emitln("struct " + type_mangle(td.name) + ";");
      if (td.type->kind == Kind::TyObject &&
          static_cast<const TyObject&>(*td.type).is_reference_type) {
        e.emitln("struct tp2cc_metaclass_" + type_mangle(td.name) + ";");
      }
    }
  }
}

void Emitter::emit_unit(const UnitNode& u) {
  units_.emit_unit(u);
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u, const TypeRegistry* registry,
                      const std::vector<std::string>* unit_init_order) {
  Emitter e(registry, unit_init_order);
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace tp2cc
