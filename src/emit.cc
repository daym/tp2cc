#include "emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "emit_analysis.h"
#include "emit_calls.h"
#include "emit_context.h"
#include "emit_decls.h"
#include "emit_lookup.h"
#include "emit_properties.h"
#include "emit_procs.h"
#include "emit_resolution.h"
#include "emit_signature_scope.h"
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

const MethodSig* method_sig_for_decl(
    const std::vector<MethodSig>& methods, const ProcDecl* decl) {
  for (const auto& method : methods) {
    if (method.decl.get() == decl) return &method;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Emitter state

struct Emitter : ResolveNameProvider,
                 OverloadTypeProvider,
                 CallTypeProvider,
                 ResolutionTypeOps,
                 EmitTypeConstRender,
                 EmitTypeOrdinalOps,
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
  std::string lookup_emission_unit_name;

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
  bool member_base_context = false;

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
  std::unordered_map<std::string, const ast::TypeExpr*> local_value_types;

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
  // Pascal name, store the overload set. Pascal overload identity is the
  // declaration, while C++ local lambda variables must each have a unique name.
  using NestedFn = ScopeStateView::NestedFn;
  std::unordered_map<std::string, std::vector<NestedFn>> local_nested_fns;
  std::unordered_set<std::string> local_nested_forwards;
  std::vector<std::string> current_fn_param_names;

  // Pascal type lookup is lexical. The current pointer names a
  // TypeRegistry-owned frame; lookup walks that frame's parent chain.
  const TypeLookupContext* current_type_scope = nullptr;
  // `const` parameters stay read-only storage. An `absolute` alias over one
  // must therefore bind a `const T&`, not a mutable `T&`, or C++ rejects the
  // alias and Pascal source that only reads through it stops compiling.
  std::unordered_set<std::string> local_const_params;
  // `with X do` bindings: for every `with target`, push the target's
  // expression text (already emitted) and its deduced type. Bare idents
  // inside the body that resolve as fields of one of the targets get
  // rewritten to `target.name`. For auto-call decisions on bare idents,
  // consult these types.
  using WithBind = ScopeStateView::WithBind;
  std::vector<WithBind> with_stack;

  // Overload scoring revisits the same operand nodes while resolving nested
  // binary expressions. Cache only for the active query tree: a unit-wide cache
  // keeps AST-derived type pointers alive longer than the emission query that
  // needed them, but no cache makes deep operator expressions recompute the
  // same subtrees repeatedly.
  std::unordered_set<const ast::Expr*> overload_type_in_progress;
  std::unordered_map<const ast::Expr*, const ast::TypeExpr*> overload_type_cache;
  size_t overload_type_query_depth = 0;
  size_t overload_type_expr_emit_depth = 0;

  struct OverloadTypeExprCacheScope {
    Emitter& emitter;
    bool outer = false;

    explicit OverloadTypeExprCacheScope(Emitter& e) : emitter(e) {
      outer = emitter.overload_type_expr_emit_depth++ == 0;
      if (outer) emitter.overload_type_cache.clear();
    }

    ~OverloadTypeExprCacheScope() {
      --emitter.overload_type_expr_emit_depth;
      if (outer) emitter.overload_type_cache.clear();
    }
  };

  // Reified type/symbol tree spanning all parsed units. tp2cc_Set by the
  // driver. Drives member-access and ident-call decisions.
  const TypeRegistry& registry;

  // Ordered unit names whose lifecycle hooks must run before the
  // program's `begin..end.` body. tp2cc_Set by the driver only when
  // emitting the `program` unit.
  const std::vector<std::string>* unit_init_order = nullptr;
  TargetInfo target;
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

  Emitter(const TypeRegistry& registry_in,
          const std::vector<std::string>* unit_init_order_in,
          TargetInfo target_in)
      : registry(registry_in),
        unit_init_order(unit_init_order_in),
        target(target_in),
        scope_state_{current_class_name,
                     current_unit_name,
                     lookup_emission_unit_name,
                     lhs_fn_rewrite,
                     lhs_fn_rewrite_slot,
                     lhs_outer_result_rewrite,
                     lhs_outer_result_rewrite_slot,
                     suppress_packed_scalar_value_load,
                     storage_view_context,
                     local_scope,
                     local_value_types,
                     local_consts,
                     local_untyped_params,
                     local_nested_fns,
                     local_nested_forwards,
                     current_type_scope,
                     local_const_params,
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
        analysis_(registry, scope_state_, *this, *this, target),
        types_(registry, scope_state_, analysis_, *this, *this, *this),
        storage_(registry, scope_state_, analysis_, types_, *this, *this),
        resolution_(registry, scope_state_, analysis_, *this, *this, target),
        calls_(registry, scope_state_, analysis_, types_, storage_,
               resolution_, *this, *this),
        properties_(analysis_, *this),
        lookup_(registry, scope_state_, analysis_, properties_),
        values_(registry, scope_state_, analysis_, types_, storage_,
                resolution_, *this, *this),
        decls_(registry, scope_state_, analysis_, types_, storage_, values_,
               *this),
        procs_(registry, scope_state_, block_depth, analysis_, types_, calls_,
               decls_, *this),
        stmts_(registry, scope_state_, except_handler_depth, try_stmt_counter,
               loop_label_counter, loop_break_labels, loop_continue_labels,
               analysis_, types_, storage_, *this, resolution_, *this, calls_,
               properties_, *this),
        units_(registry, scope_state_, block_depth, unit_init_order,
               kUnitInitName, kUnitFiniName, *this) {}

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
  std::string metaclass_struct_cxx(std::string_view class_name) {
    return types_.metaclass_struct_cxx(class_name);
  }
  std::string metaclass_value_fn_cxx(std::string_view class_name) {
    return types_.metaclass_value_fn_cxx(class_name);
  }
  std::string array_type_to_cxx(const TyArray& a) {
    return types_.array_type_to_cxx(a);
  }
  const ast::TypeExpr* semantic_shape_type(const ast::TypeExpr* t) {
    return analysis_.semantic_shape_type(t);
  }
  const ast::TypeExpr* semantic_shape_type_in_context(
      const ast::TypeExpr* t, const TypeLookupContext* context) {
    return analysis_.semantic_shape_type_in_context(t, context);
  }
  std::string enum_underlying_type_to_cxx(const TyEnum& e) {
    return types_.enum_underlying_type_to_cxx(e);
  }
  std::optional<ArrayDimBounds> array_dim_bounds_to_cxx(
      const ast::TypeExpr& dim) {
    return types_.array_dim_bounds_to_cxx(dim);
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
  std::string formal_param_types_to_cxx(const std::vector<Param>& params) {
    return types_.formal_param_types_to_cxx(params);
  }
  bool procedural_param_uses_pointer_carrier(const ast::Param& param) override {
    return types_.procedural_param_uses_pointer_carrier(param);
  }
  std::string procedural_param_type_to_cxx(const ast::Param& param) override {
    return types_.procedural_param_type_to_cxx(param);
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
  std::string expr_value_to_cxx(const Expr& e) override {
    bool saved_member_base = member_base_context;
    // Index operands are values even when the whole index expression is being
    // emitted as an object/member receiver, e.g. `slots[p^.kind]^.field`.
    member_base_context = false;
    std::string text = expr_to_cxx(e);
    member_base_context = saved_member_base;
    return text;
  }
  std::string ordinal_value_to_cxx(const Expr& e,
                                   std::string value_cxx) override;
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
  bool can_convert_value_to_type(const Expr& e, const TypeExpr* target,
                                 bool explicit_conversion) override {
    return values_.can_convert_value_to_type(e, target, explicit_conversion);
  }
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
  std::string class_cast_rhs_type_cxx(const ast::Expr& rhs);
  std::string char_concat_operand_cxx(const ast::Expr& x, bool wrap_as_string);
  bool type_is_boolean_value_type(const ast::TypeExpr* t);
  bool expr_is_boolean_value(const ast::Expr& x);
  bool ordinal_value_needs_explicit_cast(const ast::Expr& operand);
  bool call_arg_already_pins_formal_type(const ast::Expr& arg,
                                         const ast::TypeExpr* formal_type);
  std::string binary_operand_to_cxx(const ast::Expr& operand,
                                    const ast::Expr& other);
  const PrimitiveInfo* integer_primitive_for_expr(const ast::Expr& x);
  bool expr_is_integer_operand(const ast::Expr& x);
  bool expr_is_signed_integer_operand(const ast::Expr& x);
  const PrimitiveInfo* shift_carrier_for_expr(const ast::Expr& x);
  std::optional<std::string> member_base_ident(const ast::Member& m);
  std::string single_call_arg_cxx(const ast::Call& c);
  bool expr_is_const_untyped_storage_arg(const ast::Expr& e);
  const TypeSymbol* migration_fallback_type_symbol(std::string_view type_name);
  bool visible_class_or_record_type_name(std::string_view type_name);
  std::string visible_class_or_record_type_path(std::string_view type_name);
  bool type_uses_reinterpret_copy_for_scalar_cast(const ast::TypeExpr* t);
  std::optional<std::string> sizeof_type_operand_cxx(const ast::Expr& expr);
  std::optional<std::string> unit_qualified_type_name(const ast::Expr& expr);
  tp2cc::ResolvedCall resolve_new_constructor_call(
      const ast::Expr& pointer_type_expr, const ast::Expr& ctor_callee,
      const std::vector<const ast::Expr*>& args);
  std::string metaclass_target_name(const ast::TypeExpr* t) {
    return analysis_.metaclass_target_name(t);
  }
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'
  void emit_forward_struct_decls(
      const std::vector<ast::DeclPtr>& decls) override;

  // Plain analysis typing, before selected call/operator result substitution.
  // Use this for source/designator classification; value consumers that depend
  // on overload resolution use type_for_overload().
  const ast::TypeExpr* plain_expr_type(const ast::Expr& e);
  const ast::TypeExpr* type_for_overload(const ast::Expr& e) override;
  const ast::TypeExpr* compute_type_for_overload(const ast::Expr& e);
  const ast::TypeExpr* type_for_resolved_call(
      const ast::Call& call) override;

  enum class PascalTypecastKind {
    Unknown,
    Metaclass,
    Pointer,
    Set,
    Procedural,
    ReferenceClass,
    Aggregate,
    Scalar,
  };

  struct PascalTypecastTarget {
    bool known = false;
    PascalTypecastKind kind = PascalTypecastKind::Unknown;
    std::string cxx_type;
    const ast::TypeExpr* type = nullptr;
    const ast::TypeExpr* source_type = nullptr;
    std::shared_ptr<ast::TyName> owned_source_type;
  };

  PascalTypecastTarget classify_pascal_typecast_target(
      std::string_view type_name);

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

  // Member/property receivers are produced values. Overloaded calls/operators can
  // supply a more precise result type than syntax-only deduction.
  std::string value_class_alias(const ast::Expr& e);
  const ast::Expr* peel_primitive_casts(const ast::Expr* e) {
    return storage_.peel_primitive_casts(e);
  }
  bool expr_is_storage_lvalue(const ast::Expr& e) override {
    return storage_.expr_is_storage_lvalue(e);
  }
  bool expr_is_untyped_storage_ref(const ast::Expr& e) {
    return storage_.expr_is_untyped_storage_ref(e);
  }
  std::optional<std::string> untyped_pointer_deref_address(
      const ast::Expr& e);
  bool expr_is_charish(const ast::Expr& e) {
    const ast::TypeExpr* t = type_for_overload(e);
    if (!t) return false;
    const PrimitiveInfo* pi = analysis_.primitive_info_for_type(t);
    return pi && pi->is_char();
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
  bool type_is_interface(const ast::TypeExpr* t) {
    return analysis_.type_is_interface(t);
  }
  bool expr_is_reference_class(const ast::Expr& e) {
    return storage_.expr_is_reference_class(e);
  }
  std::string member_access_op(const ast::Expr& e) {
    return storage_.member_access_op(e);
  }
  std::string value_receiver_access_op(const ast::Expr& e) {
    if (const TypeExpr* t = type_for_overload(e)) {
      if (type_is_reference_class(t) || type_is_interface(t)) return "->";
    }
    return member_access_op(e);
  }
  bool type_is_stringish(const ast::TypeExpr* t) override {
    return storage_.type_is_stringish(t);
  }
  bool type_is_pointerish(const ast::TypeExpr* t) override {
    return storage_.type_is_pointerish(t);
  }
  bool fixed_char_array_value_can_decay_to_pchar(
      const ast::TypeExpr* src_type, const ast::TypeExpr* dst_type) override {
    return storage_.fixed_char_array_value_can_decay_to_pchar(src_type,
                                                              dst_type);
  }
  bool pointer_value_conversion_is_valid(const ast::TypeExpr* dst_type,
                                         const ast::TypeExpr* src_type,
                                         bool explicit_pascal_cast) override {
    return storage_.pointer_value_conversion_is_valid(dst_type, src_type,
                                                      explicit_pascal_cast);
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
  std::string storage_designator_value_or_member_base(
      Location loc, const EmitStorageDesignator& storage) {
    return member_base_context
               ? storage_.storage_designator_member_base(storage, loc)
               : storage_.storage_designator_value(storage);
  }
  using AbsoluteTargetInfo = EmitAbsoluteTargetInfo;
  std::optional<AbsoluteTargetInfo> resolve_absolute_target(
      const ast::VarDecl& vd) {
    return storage_.resolve_absolute_target(vd);
  }
  bool proc_accepts_zero_args(const ast::ProcDecl& decl) {
    return calls_.proc_accepts_zero_args(decl);
  }
  // Single entry point for resolving a Pascal call expression. The result
  // contains the chosen declaration and callee emission policy, so call printing
  // consumes a resolved fact instead of re-running lookup.
  using ResolvedCalleeKind = tp2cc::ResolvedCalleeKind;
  using ResolvedCall = tp2cc::ResolvedCall;
  ResolvedCall resolve_call(
      const ast::Expr& callee,
      const std::vector<const ast::Expr*>& args);
  std::string format_resolved_callee(const ResolvedCall& resolved,
                                     const ast::Expr& callee_ast);
  CallArgumentPlan plan_call_arguments(
      const ast::ProcDecl* decl, const ast::Expr* callee,
      const std::vector<const ast::Expr*>& explicit_args,
      std::string_view default_arg_unit = {},
      std::string_view signature_declaring_type = {}) {
    return calls_.plan_call_arguments(decl, callee, explicit_args,
                                      default_arg_unit,
                                      signature_declaring_type);
  }
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             UntypedArgKind untyped_arg,
                             bool mutable_ref_arg,
                             std::string_view default_arg_unit = {}) {
    return calls_.lower_call_arg(arg, param_type, untyped_arg,
                                 mutable_ref_arg, default_arg_unit);
  }
  std::string lower_call_arg(const CallArgumentSlot& slot,
                             std::string_view default_arg_unit = {}) {
    return calls_.lower_call_arg(slot, default_arg_unit);
  }
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl,
                                           std::string_view default_arg_unit,
                                           std::string_view signature_declaring_type = {}) {
    return calls_.lower_implicit_zero_arg_call(callee_text, decl,
                                               default_arg_unit,
                                               signature_declaring_type);
  }
  std::string lower_property_read(Location where,
                                  const std::string& base_cxx,
                                  std::string_view base_access,
                                  const std::string& class_name,
                                  const PropertyInfo& prop,
                                  const std::vector<const ast::Expr*>& indices) {
    return properties_.lower_property_read(where, base_cxx, base_access,
                                           class_name, prop, indices);
  }
  std::string lower_property_write(Location where,
                                   const std::string& base_cxx,
                                   std::string_view base_access,
                                   const std::string& class_name,
                                   const PropertyInfo& prop,
                                   const std::vector<const ast::Expr*>& indices,
                                   const ast::Expr& value) {
    return properties_.lower_property_write(where, base_cxx, base_access,
                                            class_name, prop, indices, value);
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
      const CallArgumentPlan& plan, const ast::ProcDecl* selected_decl) {
    return calls_.maybe_lower_class_constructor_call(
        where, class_name, member_name, plan, selected_decl);
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

const TypeExpr* Emitter::plain_expr_type(const Expr& e) {
  return analysis_.deduce_type(e);
}

std::string Emitter::ordinal_value_to_cxx(const Expr& e,
                                          std::string value_cxx) {
  if (value_cxx.empty()) value_cxx = expr_to_cxx(e);
  const TypeExpr* operand_type = semantic_shape_type(type_for_overload(e));
  const TypeExpr* result_type =
      analysis_.ord_result_type_for_type(operand_type);
  if (!result_type) return value_cxx;
  const PrimitiveInfo* pi = analysis_.primitive_info_for_type(operand_type);
  if (pi && pi->is_char()) {
    return "::rt::tp2cc_char_byte(" + value_cxx + ")";
  }
  if (!ordinal_value_needs_explicit_cast(e)) return value_cxx;
  return "static_cast<" + type_to_cxx(*result_type) + ">(" + value_cxx + ")";
}

const TypeExpr* Emitter::type_for_resolved_call(const Call& c) {
  if (c.callee->kind == Kind::Ident &&
      ascii_lower(static_cast<const Ident&>(*c.callee).name) == "new" &&
      !c.args.empty()) {
    if (c.args[0]->kind == Kind::Ident) {
      return analysis_.migration_fallback_named_type_expr_by_name(
          static_cast<const Ident&>(*c.args[0]).name);
    }
    if (auto qualified = unit_qualified_type_name(*c.args[0])) {
      return analysis_.migration_fallback_named_type_expr_by_name(*qualified);
    }
    return nullptr;
  }

  std::vector<const Expr*> args;
  args.reserve(c.args.size());
  for (const auto& arg : c.args) args.push_back(arg.get());
  ResolvedCall resolved = resolution_.resolve_call(*c.callee, args);
  if (resolved.ambiguous) return nullptr;
  if (resolved.decl && resolved.decl->return_type) {
    return resolved.decl->return_type.get();
  }
  if (resolved.return_type_name.empty()) return nullptr;
  if (const TypeExpr* named =
          analysis_.migration_fallback_named_type_expr_by_name(resolved.return_type_name)) {
    return named;
  }
  const std::string low = ascii_lower(resolved.return_type_name);
  if (const TyName* int_ty = builtin_integer_type(low)) return int_ty;
  if (low == "string" || low == "shortstring") return builtin_string_type();
  if (is_primitive_type(low) || !runtime_named_type_cxx(low).empty()) {
    return named_pascal_type(low);
  }
  return nullptr;
}

const TypeExpr* Emitter::type_for_overload(const Expr& e) {
  const bool root_query = overload_type_query_depth == 0;
  const bool expression_emit_scope_active = overload_type_expr_emit_depth != 0;
  if (root_query && !expression_emit_scope_active) overload_type_cache.clear();
  ++overload_type_query_depth;
  auto finish = [&](const TypeExpr* result) {
    --overload_type_query_depth;
    if (root_query && !expression_emit_scope_active) {
      overload_type_cache.clear();
    }
    return result;
  };

  if (auto cached = overload_type_cache.find(&e);
      cached != overload_type_cache.end()) {
    return finish(cached->second);
  }

  if (!overload_type_in_progress.insert(&e).second) {
    return finish(nullptr);
  }
  const TypeExpr* result = compute_type_for_overload(e);
  overload_type_in_progress.erase(&e);
  overload_type_cache.emplace(&e, result);
  return finish(result);
}

const TypeExpr* Emitter::compute_type_for_overload(const Expr& e) {
  if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.callee->kind == Kind::Ident &&
        static_cast<const Ident&>(*c.callee).name == "ord") {
      if (const TypeExpr* t = plain_expr_type(e)) return t;
    }
    if (const TypeExpr* t = type_for_resolved_call(c)) {
      return t;
    }
  } else if (e.kind == Kind::Binary) {
    const auto& b = static_cast<const Binary&>(e);
    if (std::string op = binary_pascal_operator_token(b.op); !op.empty()) {
      BinaryOperatorResult resolved =
          resolution_.find_binary_operator(op, *b.lhs, *b.rhs);
      if (!resolved.decl && !resolved.ambiguous && b.op == BinOp::NotEq) {
        resolved = resolution_.find_binary_operator("=", *b.lhs, *b.rhs);
      }
      if (!resolved.ambiguous && resolved.decl && resolved.decl->return_type) {
        return resolved.decl->return_type.get();
      }
    }
  }
  return plain_expr_type(e);
}

std::string Emitter::value_class_alias(const Expr& e) {
  if (e.kind == Kind::Ident &&
      static_cast<const Ident&>(e).name == "self") {
    return current_class_name;
  }
  const bool produced_value =
      e.kind == Kind::Call || e.kind == Kind::Binary || e.kind == Kind::Unary;
  if (!produced_value) {
    if (auto cls = analysis_.deduce_class_alias(e); !cls.empty()) return cls;
  }
  if (const TypeExpr* t = type_for_overload(e)) {
    if (auto cls = metaclass_target_name(t); !cls.empty()) return cls;
    if (auto cls = analysis_.direct_type_name(t);
        !cls.empty()) {
      return cls;
    }
    if (const TypeExpr* canon = semantic_shape_type(t)) {
      if (auto cls = analysis_.direct_type_name(canon);
          !cls.empty()) {
        return cls;
      }
    }
  }
  return analysis_.deduce_class_alias(e);
}

std::string Emitter::class_cast_rhs_type_cxx(const Expr& rhs) {
  if (rhs.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(rhs);
    if (analysis_.migration_fallback_class_info_by_name(id.name)) {
      TyName tn(rhs.loc, id.name);
      return type_name_to_cxx(tn);
    }
  }
  return expr_to_cxx(rhs);
}

std::string Emitter::char_concat_operand_cxx(const Expr& x,
                                             bool wrap_as_string) {
  std::string text = expr_to_cxx(x);
  if (!wrap_as_string) return text;
  return "::rt::tp2cc_shortstring_of<>(" + text + ")";
}

bool Emitter::type_is_boolean_value_type(const TypeExpr* t) {
  const PrimitiveInfo* pi = analysis_.primitive_info_for_type(t);
  return pi && pi->is_bool();
}

bool Emitter::expr_is_boolean_value(const Expr& x) {
  // Pascal `and` / `or` are selected from operand types. Keep that decision on
  // the same typed-expression path used for call and operator overload ranking,
  // so a call operand's result type comes from the selected Pascal callee instead
  // of a separate syntactic Boolean guess.
  return type_is_boolean_value_type(type_for_overload(x));
}

bool Emitter::ordinal_value_needs_explicit_cast(const Expr& operand) {
  const TypeExpr* operand_type = semantic_shape_type(type_for_overload(operand));
  const TypeExpr* result_type =
      analysis_.ord_result_type_for_type(operand_type);
  if (!operand_type || !result_type) return false;
  if (const PrimitiveInfo* pi = analysis_.primitive_info_for_type(operand_type);
      pi && pi->is_char()) {
    return false;
  }
  const std::string operand_cxx = type_to_cxx(*operand_type);
  const std::string result_cxx = type_to_cxx(*result_type);
  return !operand_cxx.empty() && !result_cxx.empty() &&
         operand_cxx != result_cxx;
}

bool Emitter::call_arg_already_pins_formal_type(const Expr& arg,
                                                const TypeExpr* formal_type) {
  if (!formal_type || arg.kind != Kind::Call) return false;
  const auto& call = static_cast<const Call&>(arg);
  if (call.callee->kind != Kind::Ident || call.args.size() != 1) return false;
  if (static_cast<const Ident&>(*call.callee).name != "ord") return false;
  if (!ordinal_value_needs_explicit_cast(*call.args[0])) return false;
  const TypeExpr* arg_type = type_for_overload(arg);
  if (!arg_type) return false;
  return type_to_cxx(*arg_type) == type_to_cxx(*formal_type);
}

std::string Emitter::binary_operand_to_cxx(const Expr& operand,
                                           const Expr& other) {
  const TypeExpr* other_ty = type_for_overload(other);
  const TypeExpr* canon = semantic_shape_type(other_ty);
  // A Pascal set literal is target-typed. In binary expressions the other
  // operand supplies that target when it is a set type.
  if (operand.kind == Kind::SetLit && canon && canon->kind == Kind::TySet) {
    return set_literal_to_cxx(static_cast<const SetLit&>(operand), other_ty);
  }
  const TypeExpr* operand_ty = type_for_overload(operand);
  // Binary pointer operators infer a common pointer target from the other
  // operand.  Emit fixed-array addresses through the normal typed value path so
  // that C++ arithmetic sees the element pointer, e.g. `(&buf)->data + n`.
  // The actual array-to-element pointer rule remains in EmitStorage.
  if (canon && storage_.type_is_pointerish(canon) &&
      storage_.fixed_array_pointer_can_decay_to_element_pointer(operand_ty,
                                                                other_ty)) {
    return const_value_to_cxx(operand, other_ty);
  }
  return expr_to_cxx(operand);
}

const PrimitiveInfo* Emitter::integer_primitive_for_expr(const Expr& x) {
  const TypeExpr* t = type_for_overload(x);
  if (!t) return nullptr;
  t = semantic_shape_type(t);
  const PrimitiveInfo* pi = analysis_.primitive_info_for_type(t);
  if (!pi) return nullptr;
  if (pi->int_kind != PrimitiveIntKind::None) {
    return pi;
  }
  return nullptr;
}

bool Emitter::expr_is_integer_operand(const Expr& x) {
  return integer_primitive_for_expr(x) != nullptr;
}

bool Emitter::expr_is_signed_integer_operand(const Expr& x) {
  const PrimitiveInfo* pi = integer_primitive_for_expr(x);
  return pi && pi->int_kind == PrimitiveIntKind::Signed;
}

const PrimitiveInfo* Emitter::shift_carrier_for_expr(const Expr& x) {
  return shift_carrier_primitive(integer_primitive_for_expr(x), target);
}

std::optional<std::string> Emitter::member_base_ident(const Member& m) {
  if (m.base->kind != Kind::Ident) return std::nullopt;
  return static_cast<const Ident&>(*m.base).name;
}

std::string Emitter::single_call_arg_cxx(const Call& c) {
  // Callers only use this in one-argument intrinsic/typecast branches; a
  // fallback value would hide a violated parser/emitter invariant.
  return expr_to_cxx(*c.args[0]);
}

std::optional<std::string> Emitter::untyped_pointer_deref_address(
    const Expr& e) {
  if (e.kind != Kind::Deref) return std::nullopt;
  const auto& d = static_cast<const Deref&>(e);
  const TypeExpr* operand_ty = semantic_shape_type(type_for_overload(*d.operand));
  if (!operand_ty || operand_ty->kind != Kind::TyPointer ||
      static_cast<const TyPointer&>(*operand_ty).target) {
    return std::nullopt;
  }
  // Pascal `pointer` has no pointee type. In `T(p^)`, the explicit typecast
  // supplies that missing type, so value lowering must load T's bytes from p
  // instead of first emitting `p^` as the runtime's byte-sized placeholder.
  return expr_to_cxx(*d.operand);
}

bool Emitter::expr_is_const_untyped_storage_arg(const Expr& e) {
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
}

const TypeSymbol* Emitter::migration_fallback_type_symbol(std::string_view type_name) {
  return migration_fallback_type_symbol_by_name(registry, scope_state_, type_name);
}

bool Emitter::visible_class_or_record_type_name(std::string_view type_name) {
  const TypeSymbol* symbol = migration_fallback_type_symbol(type_name);
  symbol = descriptor_payload_symbol(symbol);
  return symbol && (symbol->class_info() || symbol->record_info());
}

std::string Emitter::visible_class_or_record_type_path(
    std::string_view type_name) {
  const TypeSymbol* symbol = migration_fallback_type_symbol(type_name);
  const TypeSymbol* payload = descriptor_payload_symbol(symbol);
  if (payload && (payload->class_info() || payload->record_info())) {
    return type_symbol_pascal_path(*payload);
  }
  return {};
}

std::optional<std::string> Emitter::sizeof_type_operand_cxx(
    const Expr& expr) {
  if (expr.kind == Kind::Ident) {
    const auto& tn = static_cast<const Ident&>(expr);
    const std::string low = ascii_lower(tn.name);
    if (is_primitive_type(low) || !runtime_named_type_cxx(low).empty()) {
      return type_name_text_to_cxx(tn.name);
    }
    if (const TypeSymbol* symbol = migration_fallback_type_symbol(low)) {
      return types_.type_symbol_to_cxx(symbol);
    }
    if (ResolveResult rr = resolve_name(std::string(tn.name));
        rr.kind == ResolvedKind::UnitType) {
      return type_name_text_to_cxx(tn.name);
    }
    return std::nullopt;
  }

  if (auto qualified = unit_qualified_type_name(expr)) {
    return type_name_text_to_cxx(*qualified);
  }

  if (expr.kind != Kind::Deref) return std::nullopt;
  const auto& deref = static_cast<const Deref&>(expr);
  const TypeExpr* ptr_type = nullptr;
  if (deref.operand->kind == Kind::Ident) {
    ptr_type = analysis_.migration_fallback_named_type_expr_by_name(
        static_cast<const Ident&>(*deref.operand).name);
  } else if (auto qualified = unit_qualified_type_name(*deref.operand)) {
    ptr_type = analysis_.migration_fallback_named_type_expr_by_name(*qualified);
  }
  if (!ptr_type) ptr_type = type_for_overload(*deref.operand);
  ptr_type = semantic_shape_type(ptr_type);
  if (!ptr_type || ptr_type->kind != Kind::TyPointer) return std::nullopt;

  const TypeExpr* pointee =
      static_cast<const TyPointer&>(*ptr_type).target.get();
  if (!pointee) return std::nullopt;
  return type_to_cxx(*pointee);
}

Emitter::PascalTypecastTarget Emitter::classify_pascal_typecast_target(
    std::string_view type_name) {
  PascalTypecastTarget out;
  const TypeSymbol* symbol = migration_fallback_type_symbol(type_name);
  const TypeSymbol* payload = descriptor_payload_symbol(symbol);
  if (payload) {
    out.known = true;
    out.cxx_type = types_.type_symbol_to_cxx(symbol);
    out.owned_source_type =
        std::make_shared<TyName>(type_symbol_unit_pascal_path(*payload));
    out.source_type = out.owned_source_type.get();
    if (const ClassInfo* ci = payload->class_info()) {
      out.kind = ci->is_reference_type ? PascalTypecastKind::ReferenceClass
                                       : PascalTypecastKind::Aggregate;
      out.type = descriptor_payload_type(symbol);
      return out;
    }
    if (payload->record_info()) {
      out.kind = PascalTypecastKind::Aggregate;
      out.type = descriptor_payload_type(symbol);
      return out;
    }
  }

  const TypeExpr* named =
      analysis_.migration_fallback_named_type_expr_by_name(type_name);
  if (named) {
    out.known = true;
    out.source_type = named;
    out.cxx_type = type_to_cxx(*named);
    out.type = semantic_shape_type(named);
  }

  if (out.type && out.type->kind == Kind::TyName) {
    const TypeLookupContext* context = registry.lookup_context_for_type(out.type);
    out.type = descriptor_payload_type(
        resolved_type_symbol_in_context(registry, scope_state_, out.type,
                                        context));
  }

  if (!out.type) return out;
  if (out.type->kind == Kind::TyMetaclass) {
    out.kind = PascalTypecastKind::Metaclass;
  } else if (out.type->kind == Kind::TyPointer) {
    out.kind = PascalTypecastKind::Pointer;
  } else if (out.type->kind == Kind::TySet) {
    out.kind = PascalTypecastKind::Set;
  } else if (out.type->kind == Kind::TyProcedural) {
    out.kind = PascalTypecastKind::Procedural;
  } else if (type_is_reference_class(out.type)) {
    out.kind = PascalTypecastKind::ReferenceClass;
  } else if (out.type->kind == Kind::TyArray ||
             out.type->kind == Kind::TyRecord ||
             out.type->kind == Kind::TyObject) {
    out.kind = PascalTypecastKind::Aggregate;
  } else {
    out.kind = PascalTypecastKind::Scalar;
  }
  return out;
}

bool Emitter::type_uses_reinterpret_copy_for_scalar_cast(const TypeExpr* t) {
  t = semantic_shape_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyArray || t->kind == Kind::TyRecord ||
      t->kind == Kind::TyObject || t->kind == Kind::TyProcedural) {
    return true;
  }
  if (t->kind != Kind::TyName) return false;

  const TypeLookupContext* context = registry.lookup_context_for_type(t);
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry, scope_state_, t, context);
  symbol = descriptor_payload_symbol(symbol);
  if (!symbol) return false;
  if (symbol->record_info()) return true;
  if (const ClassInfo* ci = symbol->class_info()) return !ci->is_reference_type;
  return false;
}

std::optional<std::string> Emitter::unit_qualified_type_name(const Expr& expr) {
  if (expr.kind != Kind::Member) return std::nullopt;
  const auto& mem = static_cast<const Member&>(expr);
  auto unit_member = analysis_.resolve_unit_qualified_member(mem);
  if (!unit_member || unit_member->resolved.kind != ResolvedKind::UnitType) {
    return std::nullopt;
  }
  return unit_member->unit_name + "." + mem.name;
}

tp2cc::ResolvedCall Emitter::resolve_new_constructor_call(
    const Expr& pointer_type_expr, const Expr& ctor_callee,
    const std::vector<const Expr*>& args) {
  if (pointer_type_expr.kind != Kind::Ident) {
    return resolution_.resolve_pointer_target_constructor(nullptr, ctor_callee,
                                                          args);
  }
  const auto& ptr_ident = static_cast<const Ident&>(pointer_type_expr);
  TyName ptr_type_name(ptr_ident.loc, ptr_ident.name);
  return resolution_.resolve_pointer_target_constructor(&ptr_type_name,
                                                        ctor_callee, args);
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
// expansion (CallArgumentPlan construction), not here.
Emitter::ResolvedCall Emitter::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  return resolution_.resolve_call(callee, args);
}

std::string Emitter::format_resolved_callee(
    const ResolvedCall& resolved, const Expr& callee_ast) {
  // FreeFunctionInUnit is the only resolved call kind that overrides normal
  // expression formatting. Receiver/member/with semantics stay in
  // expr_to_cxx(callee_ast), where the matching value lookup state lives.
  if (resolved.callee_kind == ResolvedCalleeKind::FreeFunctionInUnit &&
      !resolved.defining_unit.empty()) {
    return unit_namespace_prefix(resolved.defining_unit) +
           mangle(resolved.member_name);
  }
  if (resolved.decl && callee_ast.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee_ast);
    auto it = local_nested_fns.find(id.name);
    if (it != local_nested_fns.end()) {
      for (const auto& overload : it->second) {
        if (overload.decl == resolved.decl) return overload.cxx_name;
      }
    }
  }
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
  // Overload-aware type queries are context-sensitive, so the cache cannot live
  // beyond the expression currently being emitted.  Keeping it for the whole
  // expression still prevents nested boolean/integer operator chains from
  // recomputing the same operand subtrees repeatedly while choosing built-in vs
  // overloaded operator lowering.
  OverloadTypeExprCacheScope overload_type_cache_scope(*this);
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
      if (auto storage = storage_.resolved_bytewise_with_field_storage(rr)) {
        return storage_designator_value_or_member_base(n.loc, *storage);
      }
      if (rr.kind == ResolvedKind::UnitType) {
        const TypeSymbol* symbol =
            descriptor_payload_symbol(migration_fallback_type_symbol(n.name));
        if (const auto* ci = symbol ? symbol->class_info() : nullptr;
            ci && ci->is_reference_type) {
          const std::string class_name = type_symbol_pascal_path(*symbol);
          return metaclass_value_fn_cxx(class_name) + "()";
        }
      }
      // At namespace scope (block_depth == 0) we leave callable
      // names bare: Pascal typed-const initialisers reference
      // function names as procedural-pointer values.
      bool want_call = !is_callee_context_ && block_depth > 0 &&
                       rr.is_callable && rr.accepts_zero_args;
      return want_call ? lower_implicit_zero_arg_call(
                             rr.cxx, rr.proc, rr.default_arg_unit,
                             rr.signature_declaring_type)
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
        std::string rhs_type = class_cast_rhs_type_cxx(*n.rhs);
        return "(dynamic_cast<" + rhs_type + ">(" +
               expr_to_cxx(*n.lhs) + ") != nullptr)";
      }
      if (n.op == BinOp::As) {
        std::string rhs_type = class_cast_rhs_type_cxx(*n.rhs);
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
          return "(" + char_concat_operand_cxx(*n.lhs, l_char) + " + " +
                 char_concat_operand_cxx(*n.rhs, r_char) + ")";
        }
      }
      if ((n.op == BinOp::Eq || n.op == BinOp::NotEq ||
           n.op == BinOp::Lt || n.op == BinOp::Gt ||
           n.op == BinOp::LtEq || n.op == BinOp::GtEq) &&
          expr_is_charish(*n.lhs) && expr_is_charish(*n.rhs)) {
        return "(" + expr_to_cxx(*n.lhs) + " " +
               pascal_operator_cxx_token(binary_pascal_operator_token(n.op)) + " " +
               expr_to_cxx(*n.rhs) + ")";
      }
      // Pascal `and` / `or` are polymorphic: bool operands get
      // short-circuit `&&` / `||` (crucial for `assigned(p) and
      // (p^.x = y)` idioms), integer/set operands get bitwise `&` /
      // `|`. Be strict here: treating a nested flag expression like
      // `(IF_SM or IF_SM2)` as "boolean because it is an `or`" silently
      // miscompiles bitmask code into `&&`/`||`.
      bool logical_bool = (n.op == BinOp::And || n.op == BinOp::Or) &&
                          expr_is_boolean_value(*n.lhs) &&
                          expr_is_boolean_value(*n.rhs);
      // Pascal `{$Q+}` makes integer add/sub/mul/inc/dec raise
      // EIntOverflow on overflow. Route through a checked helper when the
      // parser snapshotted Q+ active and both operands are integer-typed;
      // floats and sets stay on plain operators.
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
          std::vector<const Expr*> op_args{n.lhs.get(), n.rhs.get()};
          CallArgumentPlan op_plan =
              plan_call_arguments(resolved.decl, nullptr, op_args);
          std::string lhs = lower_call_arg(op_plan.slots[0]);
          std::string rhs = lower_call_arg(op_plan.slots[1]);
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
          expr_is_integer_operand(*n.lhs) && expr_is_integer_operand(*n.rhs)) {
        const char* fn = (n.op == BinOp::Add) ? "tp2cc_add_checked"
                       : (n.op == BinOp::Sub) ? "tp2cc_sub_checked"
                                              : "tp2cc_mul_checked";
        return std::string("::rt::") + fn + "(" +
               binary_operand_to_cxx(*n.lhs, *n.rhs) + ", " +
               binary_operand_to_cxx(*n.rhs, *n.lhs) + ")";
      }
      if (!n.q_check &&
          (n.op == BinOp::Add || n.op == BinOp::Sub || n.op == BinOp::Mul) &&
          expr_is_integer_operand(*n.lhs) && expr_is_integer_operand(*n.rhs) &&
          (expr_is_signed_integer_operand(*n.lhs) ||
           expr_is_signed_integer_operand(*n.rhs))) {
        const char* fn = (n.op == BinOp::Add) ? "tp2cc_wrap_add"
                       : (n.op == BinOp::Sub) ? "tp2cc_wrap_sub"
                                              : "tp2cc_wrap_mul";
        return std::string("::rt::") + fn + "(" +
               binary_operand_to_cxx(*n.lhs, *n.rhs) + ", " +
               binary_operand_to_cxx(*n.rhs, *n.lhs) + ")";
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
          expr_is_integer_operand(*n.lhs)) {
        if (const PrimitiveInfo* carrier = shift_carrier_for_expr(*n.lhs)) {
          const char* fn = (n.op == BinOp::Shl) ? "p_shl" : "p_shr";
          return std::string("::rt::") + fn + "<" + carrier->cxx + ">(" +
                 binary_operand_to_cxx(*n.lhs, *n.rhs) + ", " +
                 binary_operand_to_cxx(*n.rhs, *n.lhs) + ")";
        }
      }
      if ((n.op == BinOp::IntDiv || n.op == BinOp::Mod) &&
          expr_is_integer_operand(*n.lhs) && expr_is_integer_operand(*n.rhs)) {
        const char* fn = (n.op == BinOp::IntDiv) ? "tp2cc_int_div"
                                                 : "tp2cc_int_mod";
        return std::string("::rt::") + fn + "(" +
               binary_operand_to_cxx(*n.lhs, *n.rhs) + ", " +
               binary_operand_to_cxx(*n.rhs, *n.lhs) + ")";
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
      return "(" + binary_operand_to_cxx(*n.lhs, *n.rhs) + " " + op + " " +
             binary_operand_to_cxx(*n.rhs, *n.lhs) + ")";
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
        const TypeExpr* t = type_for_overload(*n.operand);
        if (t) t = semantic_shape_type(t);
        if (const PrimitiveInfo* pi = analysis_.primitive_info_for_type(t)) {
          if (pi->int_kind == PrimitiveIntKind::Signed) {
            const char* helper = n.q_check ? "::rt::tp2cc_negate_checked"
                                           : "::rt::tp2cc_wrap_negate";
            return std::string(helper) + "(" + expr_to_cxx(*n.operand) + ")";
          }
          if (pi->int_kind == PrimitiveIntKind::Unsigned && n.q_check) {
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
      if (storage_.member_value_may_need_storage_designator(e)) {
        if (auto storage = storage_.storage_designator(e);
            storage && storage->is_bytewise()) {
          // Packed scalar fields and variant payload fields are Pascal values,
          // but their storage address may not denote a live, aligned C++
          // object. Read the value through the same byte-addressed designator
          // used by assignment, Inc/Dec, address-of, and var/untyped actuals.
          return storage_designator_value_or_member_base(m.loc, *storage);
        }
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
      //   - otherwise           -> ordinary emitted C++ member access after
      //                            Pascal-specific meanings are ruled out.
      std::optional<std::string> base_ident = member_base_ident(m);
      // `inherited.foo` -- treat as class-qualified on the parent
      // alias (C++ `inherited::foo` via the in-struct `using
      // inherited = Parent;` alias).
      if (base_ident && *base_ident == "inherited") {
        std::string parent;
        if (!current_class_name.empty()) {
          const ClassInfo* ci =
              analysis_.migration_fallback_class_info_by_name(current_class_name);
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

      // `Unit.name` is not member access on a value named `Unit`. The shared
      // analysis helper has already applied Pascal shadowing and uses-visibility
      // rules, so this branch can emit the qualified symbol directly.
      if (auto unit_member = analysis_.resolve_unit_qualified_member(m)) {
        const ResolveResult& rr = unit_member->resolved;
        if (rr.kind == ResolvedKind::Unknown) {
          report_error(m.loc, "unresolved identifier `" +
                                  unit_member->unit_name + "." + m.name + "`");
          return {};
        }
        if (rr.kind == ResolvedKind::UnitType) {
          const std::string qualified =
              unit_member->unit_name + "." + m.name;
          if (const auto* ci =
                  analysis_.migration_fallback_class_info_by_name(qualified);
              ci && ci->is_reference_type) {
            return metaclass_value_fn_cxx(qualified) + "()";
          }
        }
        bool want_call =
            !is_callee_context_ && rr.is_callable && rr.accepts_zero_args;
        return want_call ? rr.cxx + "()" : rr.cxx;
      }
      if (base_ident) {
        const std::string& base_name = *base_ident;
        const std::string base_type_path =
            analysis_.identifier_is_shadowed_value(base_name)
                ? std::string{}
                : visible_class_or_record_type_path(base_name);
        // `TClass.method` -- Pascal's way to call a specific
        // class's method (typically the parent's version from
        // inside an override). Emit `TClass::method`. This is a
        // type-name interpretation, so a parameter/local/current-field named
        // `TClass` must block it: in Pascal `tsym.typedef` is member access on
        // value `tsym` even when a visible type named `tsym` also exists.
        if (!base_type_path.empty()) {
          if (!is_callee_context_) {
            std::vector<const Expr*> no_args;
            ResolvedCall ctor_resolved = resolve_call(m, no_args);
            CallArgumentPlan ctor_plan = plan_call_arguments(
                ctor_resolved.decl, &m, no_args, ctor_resolved.default_arg_unit,
                ctor_resolved.signature_declaring_type);
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    m.loc, base_type_path, m.name, ctor_plan,
                    ctor_resolved.decl)) {
              return *ctor_call;
            }
          }
          ResolveResult rr =
              resolve_name(m.name, QualifierKind::Class, base_type_path);
          if (ascii_lower(m.name) == "classname") {
            std::string text = metaclass_value_fn_cxx(base_type_path) +
                               "()->" + mangle(m.name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? lower_implicit_zero_arg_call(
                                   text, rr.proc, rr.default_arg_unit,
                                   rr.signature_declaring_type)
                             : text;
          }
          std::string text =
              named_type_struct_cxx(base_type_path) + "::" + mangle(m.name);
          bool want_call = !is_callee_context_ &&
                           rr.is_callable && rr.accepts_zero_args;
          return want_call ? lower_implicit_zero_arg_call(
                                 text, rr.proc, rr.default_arg_unit,
                                 rr.signature_declaring_type)
                           : text;
        }
      }

      // A `class of T` value is a pointer to a metaclass descriptor. Constructor
      // and class-method members are stored as descriptor function slots, not as
      // fields of an instance.
      const std::string metaclass =
          metaclass_target_name(type_for_overload(*m.base));
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
          if (const auto* methods =
                  registry.lookup_class_methods(metaclass, m.name,
                                                current_unit_name)) {
            std::vector<MethodSig> candidates;
            bool callable_metaclass_member = false;
            for (const auto& method : *methods) {
              if ((method.kind == SymKind::Constructor ||
                   method.kind == SymKind::ClassMethod) &&
                  method.decl) {
                callable_metaclass_member = true;
                candidates.push_back(method);
              }
            }
            std::string text = base_cxx + "->" + mangle(m.name);
            if (is_callee_context_ && callable_metaclass_member) {
              return text;
            }
            PickResult picked = resolution_.pick_method_overload(candidates, {});
            if (!picked.ambiguous && picked.decl) {
              const MethodSig* sig = method_sig_for_decl(*methods, picked.decl);
              return !is_callee_context_
                         ? lower_implicit_zero_arg_call(
                               text, picked.decl,
                               sig ? sig->defining_unit : std::string_view{},
                               sig ? sig->declaring_type : std::string_view{})
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

      // Otherwise: object/record field/method access. Emit `base.name`
      // and auto-call if the deduced class has `name` as a
      // parameterless method.
      bool saved_callee = is_callee_context_;
      is_callee_context_ = false;
      bool saved_storage_view = storage_view_context;
      bool saved_member_base = member_base_context;
      storage_view_context = true;
      member_base_context = true;
      std::string base_cxx = expr_to_cxx(*m.base);
      member_base_context = saved_member_base;
      storage_view_context = saved_storage_view;
      is_callee_context_ = saved_callee;
      if (!is_callee_context_) {
        if (auto free_call = maybe_lower_class_free_member(*m.base, m.name)) {
          return *free_call;
        }
      }
      std::string bcls = value_class_alias(*m.base);
      if (m.name == "classtype" || m.name == "instancesize") {
        const auto* ci = bcls.empty()
                             ? nullptr
                             : analysis_.migration_fallback_class_info_by_name(bcls);
        if ((ci && ci->is_reference_type) || expr_is_reference_class(*m.base)) {
          // Property/default-index results like `Items[i]` may already have
          // the right Pascal class alias even when the raw expression
          // no longer looks like a plain class lvalue. Recover the dynamic
          // class query from that alias so `Items[i].ClassType` still lowers
          // to an object-side method call.
          const std::string access = value_receiver_access_op(*m.base);
          std::string text = base_cxx + access + mangle(m.name);
          return is_callee_context_ ? text : text + "()";
        }
      }
      if (!bcls.empty()) {
        if (auto* prop = registry.lookup_class_property(
                bcls, m.name, current_unit_name)) {
          if (prop->params.empty()) {
            std::vector<const Expr*> no_indices;
            return lower_property_read(m.loc, base_cxx,
                                       value_receiver_access_op(*m.base), bcls,
                                       *prop, no_indices);
          }
        }
      }
      std::string member_cxx = mangle(m.name);
      if (!bcls.empty() &&
          (registry.lookup_class_field(bcls, m.name, current_unit_name) ||
           registry.lookup_record_field(bcls, m.name, current_unit_name))) {
        member_cxx = registry.field_cxx_name(m.name);
      }
      std::string text =
          base_cxx + value_receiver_access_op(*m.base) + member_cxx;
      if (is_callee_context_) return text;
      if (bcls.empty()) return text;
      if (const auto* methods =
              registry.lookup_class_methods(bcls, m.name, current_unit_name)) {
        PickResult picked = resolution_.pick_method_overload(*methods, {});
        if (!picked.ambiguous && picked.decl) {
          const MethodSig* sig = method_sig_for_decl(*methods, picked.decl);
          text = lower_implicit_zero_arg_call(
              text, picked.decl,
              sig ? sig->defining_unit : std::string_view{},
              sig ? sig->declaring_type : std::string_view{});
        }
      }
      return text;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      if (auto storage = storage_.storage_designator(*d.operand);
          storage && storage->is_bytewise() &&
          storage_.type_is_pointerish(plain_expr_type(*d.operand))) {
        // A byte-addressed pointer slot contains a pointer value. Load that
        // value by bytes, then dereference the pointee; do not turn packed or
        // variant payload storage itself into a typed C++ receiver.
        return "::rt::tp2cc_deref(" +
               storage_.storage_designator_value(*storage) + ")";
      }
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
          if (!analysis_.identifier_is_shadowed_value(id.name) &&
              visible_class_or_record_type_name(id.name)) {
            const ProcDecl* selected = nullptr;
            if (const auto* methods =
                    registry.lookup_class_methods(id.name, m.name,
                                                  current_unit_name)) {
              for (const auto& method : *methods) {
                if (!method.decl || method.decl->is_class_method) continue;
                if (selected) {
                  selected = nullptr;
                  break;
                }
                selected = method.decl.get();
              }
            }
            if (selected) {
              return "::rt::tp2cc_method_code<&" +
                     named_type_struct_cxx(id.name) + "::" +
                     method_pointer_helper_name(*selected) + ">()";
            }
          }
        }
        if (m.base) {
          const std::string metaclass =
              metaclass_target_name(plain_expr_type(*m.base));
          if (!metaclass.empty()) {
            bool class_method = false;
            if (const auto* methods =
                    registry.lookup_class_methods(metaclass, m.name,
                                                  current_unit_name)) {
              for (const auto& method : *methods) {
                if (method.kind == SymKind::ClassMethod ||
                    method.kind == SymKind::Constructor) {
                  class_method = true;
                  break;
                }
              }
            }
            if (class_method) {
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
      // Compiler intrinsics handled here:
      //   * `low(T)` / `high(T)` when T is a type name  -> emitted constant
      //   * `ord(x)`                                    -> ordinal value
      //   * `sizeof(x)`                                 -> C++ `sizeof`
      //   * `TypeName(expr)` function-style cast        -> paren-cast when
      //                                                   the C++ type is
      //                                                   compound
      //   * `new(...)` / `dispose(...)`                 -> placement form
      // Pascal intrinsics accept both `low(t)` and `system.low(t)` spellings.
      // intrinsic_call_name normalises both forms so the lowering below runs
      // once instead of being duplicated across Ident-callee and Member-callee
      // branches.
      if (auto intrinsic = analysis_.intrinsic_call_name(*c.callee)) {
        const std::string& n = *intrinsic;
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
          const TypeExpr* at = type_for_overload(*c.args[0]);
          if (at) {
            if (std::string rewrite = low_high_expr_for_type(at, want_low);
                !rewrite.empty()) {
              return rewrite;
            }
            const TypeExpr* canon = semantic_shape_type(at);
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
          // `Ord` is a value intrinsic: the emitted expression needs the
          // Pascal numeric result type even without a target-typed context.
          return ordinal_value_to_cxx(*c.args[0], single_call_arg_cxx(c));
        }

        if (n == "sizeof" && c.args.size() == 1) {
          // Pascal `sizeof` returns `longint`, but C++ `sizeof` is
          // `size_t` (typically `unsigned int` or `unsigned long`). Wrap
          // the result so arithmetic and overload resolution at the call
          // site see the Pascal-correct signed int32_t -- otherwise e.g.
          // `tostr(sizeof(aint))` ends up ambiguous against the
          // qword/int64/longint overload set.
          std::string inner;
          if (auto type_operand = sizeof_type_operand_cxx(*c.args[0])) {
            inner = "sizeof(" + *type_operand + ")";
          }
          if (inner.empty()) inner = "sizeof(" + expr_to_cxx(*c.args[0]) + ")";
          return "static_cast<int32_t>(" + inner + ")";
        }
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        const std::string& n = id.name;

        if (n == "unaligned" && c.args.size() == 1) {
          // Pascal `unaligned(x)` is an explicit bytewise load/store view,
          // not permission to manufacture a misaligned `T&`. When the
          // argument denotes storage, load it via memcpy from the storage
          // address. Non-storage forms fall back to the runtime helper.
          if (auto storage = storage_.bytewise_storage_ref(*c.args[0])) {
            return "::rt::tp2cc_unaligned_load<" + storage->elem_cxx + ">(" +
                   storage->void_ptr_text + ")";
          }
          return "::rt::p_unaligned(" + single_call_arg_cxx(c) + ")";
        } else if (n == "typeof" && c.args.size() == 1 &&
                   c.args[0]->kind == Kind::Ident) {
          // Pascal `typeof(T)` takes a TYPE NAME, not a value. In C++
          // we have no VMT-by-type-name runtime object; stub as
          // `nullptr` with a dummy template-arg tag so the expression
          // at least compiles. Users of this value compare it for
          // equality/inequality at runtime only.
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          if (visible_class_or_record_type_name(a.name)) {
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
            return "::rt::tp2cc_shortstring_of<>(" +
                   single_call_arg_cxx(c) + ")";
          }
          if (const TypeSymbol* atom = registry.builtin_literal(n)) {
            if (analysis_.type_is_long_string(atom->type)) {
              return "::rt::tp2cc_ansistring_of(" +
                     single_call_arg_cxx(c) + ")";
            }
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
          if (!storage_view_context) {
            if (auto ptr = untyped_pointer_deref_address(*c.args[0])) {
              return "::rt::tp2cc_reinterpret_load<" + primitive_type_cxx(n) +
                     ">(" + *ptr + ")";
            }
          }
          if (const PrimitiveInfo* info = primitive_info(n);
              info && (info->int_kind != PrimitiveIntKind::None)) {
            const TypeExpr* source_ty =
                semantic_shape_type(type_for_overload(*c.args[0]));
            bool source_is_set =
                c.args[0]->kind == Kind::SetLit ||
                (source_ty && source_ty->kind == Kind::TySet);
            if (source_is_set) {
              // Pascal `longint(set)` packs the set's element bits into
              // an integer (element i -> bit i). The set source can be
              // an rvalue literal `[a,b,c]`, so don't gate on lvalue.
              return "::rt::tp2cc_set_to_int<" + primitive_type_cxx(n) +
                     ">(" + single_call_arg_cxx(c) + ")";
            }
          }
          if (const PrimitiveInfo* pi_char = primitive_info(n);
              pi_char && pi_char->is_char()) {
            return "::rt::p_chr(" + single_call_arg_cxx(c) + ")";
          }
          if (n == "pointer" || n == "pchar" || n == "pansichar") {
            TyName cast_name(n);
            std::string source =
                (peeled && expr_is_storage_lvalue(*c.args[0]))
                    ? expr_to_cxx(*peeled)
                    : single_call_arg_cxx(c);
            std::string coerced = coerce_pointer_like_text(
                primitive_type_cxx(n), &cast_name,
                type_for_overload(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true,
                expr_is_const_untyped_storage_arg(*c.args[0]));
            if (coerced != source) return coerced;
            if (peeled && expr_is_storage_lvalue(*c.args[0])) {
              return "((" + primitive_type_cxx(n) + ")(" +
                     expr_to_cxx(*peeled) + "))";
            }
          }
          if (expr_is_charish(*c.args[0])) {
            return "((" + primitive_type_cxx(n) + ")(" +
                   ordinal_value_to_cxx(*c.args[0],
                                        single_call_arg_cxx(c)) + "))";
          }
          TyName target(n);
          if (auto conv = resolution_.find_assignment_operator(
                  type_for_overload(*c.args[0]), &target);
              conv.decl) {
            std::string fn = pascal_assignment_operator_helper_name(*conv.decl);
            if (!conv.defining_unit.empty()) {
              fn = unit_namespace_prefix(conv.defining_unit) + fn;
            }
            return fn + "(" + single_call_arg_cxx(c) + ")";
          }
          if (auto lit =
                  maybe_convert_const_int_expr(*c.args[0], &target, true)) {
            return *lit;
          }
          if (type_uses_reinterpret_copy_for_scalar_cast(
                  type_for_overload(*c.args[0]))) {
            // Some explicit scalar casts are representation casts over the
            // source value's bytes: records/objects/arrays, and procedural
            // values such as procvars. Procedural target casts are handled
            // earlier as target-typed procvar conversions, so reaching this
            // branch only means "copy source representation into scalar".
            return "::rt::tp2cc_reinterpret_copy<" + primitive_type_cxx(n) +
                   ">(" + single_call_arg_cxx(c) + ")";
          }
          return "((" + primitive_type_cxx(n) + ")(" +
                 single_call_arg_cxx(c) + "))";
        } else if (c.args.size() == 1 && n != "inc" && n != "dec") {
          PascalTypecastTarget target = classify_pascal_typecast_target(n);
          if (target.known && target.kind == PascalTypecastKind::Metaclass) {
            std::string source = single_call_arg_cxx(c);
            std::string coerced = coerce_pointer_like_text(
                target.cxx_type, target.type,
                type_for_overload(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true);
            if (coerced != source) return coerced;
            return "((" + target.cxx_type + ")(" + source + "))";
          }
          if (target.known && target.kind == PascalTypecastKind::Pointer) {
            const Expr* peeled = peel_primitive_casts(c.args[0].get());
            std::string source =
                (peeled && expr_is_storage_lvalue(*c.args[0]))
                    ? expr_to_cxx(*peeled)
                    : single_call_arg_cxx(c);
            std::string coerced = coerce_pointer_like_text(
                target.cxx_type, target.type,
                type_for_overload(*c.args[0]),
                source,
                /*explicit_pascal_cast=*/true,
                expr_is_const_untyped_storage_arg(*c.args[0]));
            if (coerced != source) return coerced;
            return "((" + target.cxx_type + ")(" + source + "))";
          }
          if (target.known && target.kind == PascalTypecastKind::Set) {
            if (!storage_view_context) {
              if (auto ptr = untyped_pointer_deref_address(*c.args[0])) {
                return "::rt::tp2cc_reinterpret_load<" +
                       type_to_cxx(*target.type) + ">(" + *ptr + ")";
              }
            }
            return "::rt::tp2cc_set_cast<" + type_to_cxx(*target.type) + ">(" +
                   single_call_arg_cxx(c) + ")";
          }
          if (target.known && target.kind == PascalTypecastKind::Procedural) {
            // A procedural typecast is target-typed. In particular,
            // `TMethodType(@instance_method)` must bind the method code and
            // the current Self pointer; treating the procedural value as an
            // aggregate byte reinterpretation leaves an invalid C++ member
            // function address.
            return const_value_to_cxx(*c.args[0], target.type,
                                      /*explicit_conversion=*/true);
          }
          if (target.known) {
            if (auto conv = resolution_.find_assignment_operator(
                    type_for_overload(*c.args[0]), target.source_type);
                conv.decl) {
              std::string fn =
                  pascal_assignment_operator_helper_name(*conv.decl);
              if (!conv.defining_unit.empty()) {
                fn = unit_namespace_prefix(conv.defining_unit) + fn;
              }
              return fn + "(" + single_call_arg_cxx(c) + ")";
            }
          }
          if (target.known &&
              target.kind == PascalTypecastKind::ReferenceClass) {
            // The explicit Pascal cast target is the source-level type name.
            // Passing a resolved payload type here loses alias qualification
            // and can make the pointer-slot coercion rules answer for the
            // class layout instead of the Pascal reference type being cast to.
            std::string coerced = coerce_pointer_like_text(
                target.cxx_type, target.source_type,
                type_for_overload(*c.args[0]), single_call_arg_cxx(c),
                /*explicit_pascal_cast=*/true);
            if (coerced != single_call_arg_cxx(c)) return coerced;
            return "((" + target.cxx_type + ")(" +
                   single_call_arg_cxx(c) + "))";
          }
          if (target.known && target.kind == PascalTypecastKind::Aggregate) {
            auto storage_view = storage_.typecast_storage_view(c);
            if (storage_view && storage_view_context) {
              return reinterpret_ref_text(storage_view->target_cxx,
                                          storage_view->source_cxx,
                                          storage_view->pointee_view);
            }
            if (!storage_view_context) {
              if (auto ptr = untyped_pointer_deref_address(*c.args[0])) {
                return "::rt::tp2cc_reinterpret_load<" +
                       target.cxx_type + ">(" + *ptr + ")";
              }
            }
            if (target.type && target.type->kind == Kind::TyArray) {
              const auto& arr = static_cast<const TyArray&>(*target.type);
              const TypeExpr* elem =
                  arr.element ? semantic_shape_type(arr.element.get()) : nullptr;
              if (arr.dims.size() == 1) {
                const PrimitiveInfo* pi_elem =
                    analysis_.primitive_info_for_type(elem);
                if (elem == named_pascal_type("byte") ||
                    (pi_elem && pi_elem->is_char())) {
                  if (storage_view && storage_view->source_is_untyped_storage) {
                    return "::rt::tp2cc_reinterpret_load<" +
                           target.cxx_type + ">(" +
                           storage_view->source_cxx + ")";
                  }
                  return "::rt::tp2cc_reinterpret_bytes<" +
                         target.cxx_type + ">(" +
                         single_call_arg_cxx(c) + ")";
                }
              }
            }
            if (storage_view && storage_view->source_is_untyped_storage) {
              return "::rt::tp2cc_reinterpret_load<" +
                     target.cxx_type + ">(" +
                     storage_view->source_cxx + ")";
            }
            return "::rt::tp2cc_reinterpret_copy<" + target.cxx_type +
                   ">(" + single_call_arg_cxx(c) + ")";
          }
          if (target.known && target.kind == PascalTypecastKind::Scalar) {
            if (auto lit =
                    maybe_convert_const_int_expr(*c.args[0],
                                                 target.source_type,
                                                 true)) {
              return *lit;
            }
            if (!storage_view_context) {
              if (auto ptr = untyped_pointer_deref_address(*c.args[0])) {
                return "::rt::tp2cc_reinterpret_load<" +
                       target.cxx_type + ">(" + *ptr + ")";
              }
            }
            return "((" + target.cxx_type + ")(" +
                   single_call_arg_cxx(c) + "))";
          }
          if (target.known) {
            report_error(c.loc, "unsupported typecast target `" + n + "`");
            return "/* unsupported typecast */";
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
          // it as type text keeps Pascal's type/value namespaces separate in
          // generated C++.
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
            std::vector<const Expr*> ctor_args;
            ctor_args.reserve(cc.args.size());
            for (const auto& arg : cc.args) ctor_args.push_back(arg.get());
            ResolvedCall ctor_resolved =
                resolve_new_constructor_call(*c.args[0], *cc.callee,
                                             ctor_args);
            CallArgumentPlan ctor_plan =
                plan_call_arguments(ctor_resolved.decl, cc.callee.get(),
                                    ctor_args, ctor_resolved.default_arg_unit,
                                    ctor_resolved.signature_declaring_type);
            for (size_t i = 0; i < ctor_plan.slots.size(); ++i) {
              if (i) margs += ", ";
              margs += lower_call_arg(ctor_plan.slots[i],
                                      ctor_plan.default_arg_unit);
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
          pointer_cast_ty = named_pascal_type("pointer");
        } else {
          const TypeSymbol* symbol = migration_fallback_type_symbol(id.name);
          const TypeExpr* tgt = descriptor_payload_type(symbol);
          if (tgt && tgt->kind == Kind::TyPointer) {
            cast_to_pointer = true;
            cast_type_cxx = types_.type_symbol_to_cxx(symbol);
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
              cast_type_cxx, pointer_cast_ty,
              type_for_overload(*c.args[0]), source,
              /*explicit_pascal_cast=*/true);
          if (coerced != source) return coerced;
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            return "((" + cast_type_cxx + ")(" + expr_to_cxx(*peeled) + "))";
          }
          return "((" + cast_type_cxx + ")(" + expr_to_cxx(*c.args[0]) + "))";
        }
      }
      if (c.args.size() == 1 && c.callee->kind == Kind::Member) {
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
            const TypeExpr* cast_ty =
                analysis_.migration_fallback_named_type_expr_by_name(qualified);
            if (cast_ty) cast_ty = semantic_shape_type(cast_ty);
            if (cast_ty && cast_ty->kind == Kind::TyProcedural) {
              // Unit-qualified procedural casts are the same Pascal operation
              // as unqualified ones: the target procedural type controls
              // method-value binding and plain procedure-pointer conversion.
              return const_value_to_cxx(*c.args[0], cast_ty,
                                        /*explicit_conversion=*/true);
            }
            if (cast_ty && cast_ty->kind == Kind::TyPointer) {
              // Pointer casts are still Pascal value conversions, but their
              // source may be a typed storage view such as `T(x)`. Peel only
              // primitive storage-view casts before the pointer coercion so the
              // emitted C++ casts the original storage expression, not
              // temporary/reference text built for another target type.
              const Expr* peeled = peel_primitive_casts(c.args[0].get());
              std::string source =
                  (peeled && expr_is_storage_lvalue(*c.args[0]))
                      ? expr_to_cxx(*peeled)
                      : expr_to_cxx(*c.args[0]);
              std::string coerced = coerce_pointer_like_text(
                  type_name_text_to_cxx(qualified), cast_ty,
                  type_for_overload(*c.args[0]), source,
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
      ResolvedCall resolved = resolve_call(*c.callee, call_args);
      if (resolved.no_match) {
        std::string name;
        if (c.callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*c.callee).name;
        } else if (c.callee->kind == Kind::Member) {
          name = static_cast<const Member&>(*c.callee).name;
        }
        report_error(c.loc,
                     "no matching call to '" + name +
                         "': incompatible argument types");
        return "/* no matching call to '" + name + "' */";
      }
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
      CallArgumentPlan call_plan = plan_call_arguments(
          call_decl, c.callee.get(), call_args, resolved.default_arg_unit,
          resolved.signature_declaring_type);
      if (resolved.needs_arg_validation &&
          !calls_.validate_call_arguments(call_plan)) {
        std::string name;
        if (c.callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*c.callee).name;
        } else if (c.callee->kind == Kind::Member) {
          name = static_cast<const Member&>(*c.callee).name;
        }
        report_error(c.loc, "no matching call to '" + name +
                                "': incompatible argument types");
        return "/* no matching call to '" + name + "' */";
      }
      if (c.args.empty() && c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (auto free_call = maybe_lower_class_free_member(*mem.base, mem.name)) {
          return *free_call;
        }
      }
      if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*mem.base);
          const std::string class_type_path =
              analysis_.identifier_is_shadowed_value(id.name)
                  ? std::string{}
                  : visible_class_or_record_type_path(id.name);
          if (!class_type_path.empty()) {
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    c.loc, class_type_path, mem.name, call_plan,
                    resolved.decl)) {
              return *ctor_call;
            }
          }
        }
      }
      std::string callee_text = format_resolved_callee(resolved, *c.callee);
      bool is_tpexcept_setjmp = false;
      if (c.args.size() == 1) {
        if (c.callee->kind == Kind::Ident) {
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
      const std::string memory_helper =
          callee_text == "::rt::p_getmem"
              ? "getmem"
              : callee_text == "::rt::p_reallocmem"
                    ? "reallocmem"
                    : callee_text == "::rt::p_strdispose" ? "strdispose" : "";
      if (!memory_helper.empty() && !c.args.empty()) {
        const bool bytewise_pointer_slot_helper =
            memory_helper == "getmem" || memory_helper == "reallocmem" ||
            memory_helper == "strdispose";
        if (bytewise_pointer_slot_helper) {
          if (auto storage = storage_.storage_designator(*c.args[0]);
              storage && storage->is_bytewise()) {
            if (storage->type_cxx.empty()) {
              report_error(c.args[0]->loc,
                           "memory helper requires a typed pointer slot");
              return "/* invalid pointer slot */";
            }
            // These helpers mutate the pointer value stored in the Pascal
            // slot. A bytewise slot is addressable storage, but not a live C++
            // pointer object, so the runtime helper updates it by byte-copy.
            if (memory_helper == "getmem" && c.args.size() >= 2) {
              return "::rt::p_getmem_slot<" + storage->type_cxx + ">(" +
                     storage->ptr_cxx + ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            if (memory_helper == "reallocmem" && c.args.size() >= 2) {
              return "::rt::p_reallocmem_slot<" + storage->type_cxx + ">(" +
                     storage->ptr_cxx + ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            if (memory_helper == "strdispose") {
              return "::rt::p_strdispose_slot(" + storage->ptr_cxx + ")";
            }
          }
        }
      }
      std::string out = callee_text + "(";
      for (size_t i = 0; i < call_plan.slots.size(); ++i) {
        if (i) out += ", ";
        const CallArgumentSlot& slot = call_plan.slots[i];
        std::string arg_text = lower_call_arg(slot, call_plan.default_arg_unit);
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
        if (resolved.needs_arg_casts && slot.param_type &&
            !slot.mutable_ref_arg &&
            slot.untyped_arg == UntypedArgKind::None) {
          const TypeExpr* canon_pt =
              semantic_shape_type_in_context(slot.param_type,
                                             slot.param_context);
          if (!canon_pt || canon_pt->kind != Kind::TyProcedural) {
            // `Ord` may already emit the cast that gives the expression its
            // Pascal result type, e.g. enum->LongInt or Boolean->Byte. If that
            // type is the selected formal, the generic overload guard would
            // duplicate the same cast.
            if (!call_arg_already_pins_formal_type(*slot.expr,
                                                   slot.param_type)) {
              arg_text = "static_cast<" + type_to_cxx(*slot.param_type) +
                         ">(" + arg_text + ")";
            }
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
          use && !type_is_byte_aligned_packed_index_carrier(
                     plain_expr_type(*i.base))) {
        report_packed_aggregate_subobject_use(i.loc, "indexing", *use);
      }
      std::vector<const Expr*> indices;
      for (const auto& idx : i.indices) indices.push_back(idx.get());
      if (i.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*i.base);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = value_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry.lookup_class_property(
                  cls, mem.name, current_unit_name)) {
            if (!prop->params.empty()) {
              return lower_property_read(i.loc, expr_to_cxx(*mem.base),
                                         value_receiver_access_op(*mem.base), cls,
                                         *prop, indices);
            }
          }
        }
      }
      if (i.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*i.base);
        if (auto found = find_implicit_class_property(id.name);
            found && found->prop && !found->prop->params.empty()) {
          return lower_property_read(i.loc, found->base_cxx,
                                     found->base_access, found->class_name,
                                     *found->prop, indices);
        }
      }
      std::string cls = value_class_alias(*i.base);
      if (!cls.empty()) {
        if (auto* prop =
                registry.lookup_default_property(cls, current_unit_name)) {
          return lower_property_read(i.loc, expr_to_cxx(*i.base),
                                     value_receiver_access_op(*i.base), cls,
                                     *prop, indices);
        }
      }
      if (auto storage = storage_.storage_designator(i);
          storage && storage->is_bytewise()) {
        // Value reads only need the storage designator for byte-addressed
        // storage. Ordinary indexes and aligned storage-view indexes keep the
        // normal expression path; bytewise indexes must load from the composed
        // element address instead of indexing a copied aggregate value.
        return storage_designator_value_or_member_base(i.loc, *storage);
      }
      std::string out = expr_to_cxx(*i.base);
      for (const auto& idx : i.indices) {
        out += "[" + expr_value_to_cxx(*idx) + "]";
      }
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
            registry.field_cxx_name(r.fields[i].first);
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

// FPC constant conversions first evaluate the integer constant expression,
// then convert that value to the destination type. Assignments, calls, and
// typed consts must share this path so range checks and wrapping stay
// identical.
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
static std::string nested_pascal_type_path(std::string_view owner,
                                           std::string_view name) {
  std::string out(owner);
  if (!out.empty()) out += ".";
  out += ascii_lower(name);
  return out;
}

static void emit_reference_metaclass_forward_decls_impl(
    Emitter& e, const TypeDecl& td, std::string_view pascal_path,
    bool emit_current) {
  if (!td.type) return;
  if (td.type->kind == Kind::TyObject) {
    const auto& to = static_cast<const TyObject&>(*td.type);
    if (to.is_reference_type && emit_current) {
      // Metaclass carriers live at namespace scope even for Pascal nested
      // classes. The nested class body friends the carrier value function, so
      // the carrier struct has to be declared before the owner body is emitted.
      e.emitln("struct " + e.metaclass_struct_cxx(pascal_path) + ";");
    }
    for (const auto& member : to.members) {
      if (member.kind != ObjectMemberKind::Type || !member.type_decl) {
        continue;
      }
      const std::string nested_path =
          nested_pascal_type_path(pascal_path, member.type_decl->name);
      emit_reference_metaclass_forward_decls_impl(
          e, *member.type_decl, nested_path, /*emit_current=*/true);
    }
    return;
  }
  if (td.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*td.type);
    for (const auto& nested : tr.nested_types) {
      if (!nested) continue;
      const std::string nested_path =
          nested_pascal_type_path(pascal_path, nested->name);
      emit_reference_metaclass_forward_decls_impl(
          e, *nested, nested_path, /*emit_current=*/true);
    }
  }
}

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
      emit_reference_metaclass_forward_decls_impl(
          e, td, ascii_lower(td.name), /*emit_current=*/false);
    }
  }
}

void Emitter::emit_unit(const UnitNode& u) {
  units_.emit_unit(u);
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u, const TypeRegistry& registry,
                      const std::vector<std::string>* unit_init_order,
                      TargetInfo target) {
  Emitter e(registry, unit_init_order, target);
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace tp2cc
