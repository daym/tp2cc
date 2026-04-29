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
#include "emit_properties.h"
#include "emit_resolution.h"
#include "emit_storage.h"
#include "emit_types.h"
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

// ---------------------------------------------------------------------------
// Emitter state

struct Emitter : ResolveNameProvider,
                 ResolutionTypeOps,
                 EmitTypeConstRender,
                 EmitStorageExprOps,
                 EmitCallExprOps,
                 EmitPropertyExprOps,
                 EmitValueExprOps,
                 EmitDeclOps {
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
  EmitValues values_;
  EmitDecls decls_;

  Emitter(const TypeRegistry* registry_in = nullptr,
          const std::vector<std::string>* unit_init_order_in = nullptr)
      : registry(registry_in),
        unit_init_order(unit_init_order_in),
        scope_state_{current_class_name,
                     current_unit_name,
                     lhs_fn_rewrite,
                     lhs_fn_rewrite_slot,
                     lhs_outer_result_rewrite,
                     lhs_outer_result_rewrite_slot,
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
        types_(registry, scope_state_, analysis_, *this),
        storage_(registry, scope_state_, analysis_, types_, *this, *this),
        resolution_(registry, scope_state_, analysis_, *this),
        calls_(registry, scope_state_, analysis_, types_, storage_,
               resolution_, *this),
        properties_(registry, analysis_, *this),
        values_(registry, scope_state_, analysis_, types_, storage_, *this),
        decls_(registry, scope_state_, analysis_, types_, storage_, values_,
               *this) {}

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
  bool registry_knows_type(std::string_view name) {
    return types_.registry_knows_type(name);
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
  bool enum_has_explicit_values(const TyEnum& e) {
    return types_.enum_has_explicit_values(e);
  }
  std::optional<int64_t> enum_member_value_int64(const TyEnum& e,
                                                 size_t index) {
    return types_.enum_member_value_int64(e, index);
  }
  std::string enum_member_value_to_cxx(const TyEnum& e, size_t index) {
    return types_.enum_member_value_to_cxx(e, index);
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
  std::optional<std::string> maybe_convert_proc_value(
      const Expr& e, const TypeExpr* target);
  std::optional<std::string> maybe_lower_metaclass_value(
      const Expr& e, const TypeExpr* target);

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
  std::string primitive_cast_lvalue_ref(const ast::Call& c) {
    return storage_.primitive_cast_lvalue_ref(c);
  }
  std::string primitive_cast_untyped_storage_ptr(const ast::Call& c) {
    return storage_.primitive_cast_untyped_storage_ptr(c);
  }
  std::string primitive_cast_packed_field_ptr(const ast::Call& c) {
    return storage_.primitive_cast_packed_field_ptr(c);
  }
  // Carries the result of `packed_field_storage_ref`. `void_ptr_text` is
  // an `&(field_expr)` snippet -- safe to consume only via the memcpy-based
  // runtime helpers (`tp2cc_reinterpret_load` / `_store` / `_inc` / `_dec`).
  // Going through `*reinterpret_cast<T*>(p)` instead would re-introduce the
  // misaligned-`T*`-deref UB the existing `[[gnu::packed]]` emit deliberately
  // makes the compiler complain about. `elem_cxx` is the C++ type to use as
  // the load/store operand at the call site.
  using PackedFieldStorage = EmitPackedFieldStorage;
  std::optional<PackedFieldStorage> packed_field_storage_ref(
      const ast::Expr& e) {
    return storage_.packed_field_storage_ref(e);
  }
  using UntypedStorageIndexView = EmitUntypedStorageIndexView;
  std::optional<UntypedStorageIndexView> untyped_storage_index_view(
      const ast::Index& i) {
    return storage_.untyped_storage_index_view(i);
  }
  std::string param_list_to_cxx(const std::vector<Param>& params);
  std::string proc_return_type_to_cxx(const ProcDecl& pd);
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_raise_stmt(const ast::Raise& r);
  void emit_try_stmt(const ast::Try& t);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'

  // Expression-type deduction. Returns the Pascal TypeExpr that the
  // expression has, or nullptr when unknown. Consults the TypeRegistry
  // for globals and the current scope tables for locals/self-class.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);
  const ast::TypeExpr* deduce_const_decl_type(const ast::ConstDecl& cd);

  // `with` targets can be anonymous local records, so name-based
  // registry lookup is not enough. These helpers walk the stacked
  // bound type itself and recover the member type/text directly.
  const ast::TypeExpr* lookup_record_field_type_in_with(
      const WithBind& wb, std::string_view field_name);
  bool with_bind_has_visible_member(const WithBind& wb, std::string_view name);
  bool type_is_packed_record(const ast::TypeExpr* t) {
    return storage_.type_is_packed_record(t);
  }
  bool type_is_direct_packed_aggregate(const ast::TypeExpr* t) {
    return storage_.type_is_direct_packed_aggregate(t);
  }
  bool type_is_byte_aligned_packed_index_carrier(const ast::TypeExpr* t) {
    return storage_.type_is_byte_aligned_packed_index_carrier(t);
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
  bool type_is_stringish(const ast::TypeExpr* t) {
    return storage_.type_is_stringish(t);
  }
  bool type_is_pointerish(const ast::TypeExpr* t) {
    return storage_.type_is_pointerish(t);
  }
  bool type_is_open_array(const ast::TypeExpr* t) {
    return storage_.type_is_open_array(t);
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
                             bool mutable_ref_arg) {
    return calls_.lower_call_arg(arg, param_type, untyped_arg,
                                 mutable_ref_arg);
  }
  std::string lower_implicit_zero_arg_call(const std::string& callee_text,
                                           const ast::ProcDecl* decl) {
    return calls_.lower_implicit_zero_arg_call(callee_text, decl);
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
      std::string_view class_name, std::string_view member_name,
      const std::vector<const ast::Expr*>& args,
      const std::vector<const ast::TypeExpr*>& param_types,
      const std::vector<UntypedArgKind>& untyped_arg,
      const std::vector<bool>& mutable_ref_arg) {
    return calls_.maybe_lower_class_constructor_call(
        class_name, member_name, args, param_types, untyped_arg,
        mutable_ref_arg);
  }

  // ---------------------------------------------------------------------
  // Pascal name resolution (one function, every emit path goes through
  // it). Given a name and optional qualifier, model the full Pascal
  // lookup:
  //   - unqualified: `with` -> locals -> enclosing nested fns ->
  //                  class+ancestors (in method body) -> current unit ->
  //                  `uses` chain -> rt:: builtins.
  //   - `Unit.name`: symbols exported by `Unit` (which must be in the
  //                  current unit's `uses` list).
  //   - `Class.name` / `obj.name`: class's members walking ancestors.
  //
  // The resolved result tells the emitter:
  //   - how to spell the access in C++,
  //   - whether it's a parameterless callable (value context -> auto-call),
  //   - the ProcDecl* for call-site untyped-var arg wrapping,
  //   - whether it's a field/var/const/enum-member (never auto-call).
  using ResolvedKind = tp2cc::ResolvedKind;
  using ResolveResult = tp2cc::ResolveResult;
  std::optional<ResolveResult> maybe_resolve_implicit_property(
      std::string_view name) {
    return properties_.maybe_resolve_implicit_property(name);
  }
  // Qualifier: empty means unqualified lookup.  Otherwise it's a
  // unit name or a class/record alias name (both lowercased).
  using QualifierKind = tp2cc::QualifierKind;
  ResolveResult resolve_name(const std::string& name,
                             QualifierKind qk = QualifierKind::None,
                             const std::string& qualifier = {}) override;

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

  void emit_tpexcept_unit(const UnitNode& u);
};

// ---------------------------------------------------------------------------
// Expression-type deduction. Used by the Member / Ident emitters so
// decisions like "is `obj.name` a method call or a field read?" come
// from the actual type tree, not name-matching heuristics.

const TypeExpr* Emitter::deduce_const_decl_type(const ConstDecl& cd) {
  return analysis_.deduce_const_decl_type(cd);
}

const TypeExpr* Emitter::deduce_type(const Expr& e) {
  return analysis_.deduce_type(e);
}

std::string Emitter::deduce_class_alias(const Expr& e) {
  return analysis_.deduce_class_alias(e);
}

const TypeExpr* Emitter::lookup_record_field_type_in_with(
    const WithBind& wb, std::string_view field_name) {
  return analysis_.lookup_record_field_type_in_with(wb, field_name);
}

bool Emitter::with_bind_has_visible_member(const WithBind& wb,
                                           std::string_view name) {
  return analysis_.with_bind_has_visible_member(wb, name);
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

size_t procedural_param_count(const TyProcedural& p) {
  size_t count = 0;
  for (const auto& pp : p.params) {
    count += pp.names.empty() ? 1 : pp.names.size();
  }
  return count;
}

// ---------------------------------------------------------------------------
// Single-point Pascal name resolution. `resolve_name` walks the real
// Pascal lookup order and returns a `ResolveResult` every emit site
// consumes uniformly; this avoids having the same "is it a method?
// is it a unit-qualified proc? should we auto-call?" logic grow in
// three different places in the emitter.

Emitter::ResolveResult Emitter::resolve_name(
    const std::string& name, QualifierKind qk, const std::string& qualifier) {
  ResolveResult r;

  // ----- Qualified lookups first: `Unit.name` / `Class.name`. -----
  if (qk == QualifierKind::Unit) {
    r.cxx = unit_namespace_prefix(qualifier) + mangle(name);
    if (registry) {
      auto uit = registry->units.find(qualifier);
      if (uit != registry->units.end()) {
        const UnitInfo& u = uit->second;
        if (auto* pi = u.find_export_proc(name)) {
          r.kind = ResolvedKind::UnitProc;
          r.proc = pi->decl.get();
          r.is_callable = true;
          r.is_parameterless = (pi->param_count == 0);
          r.accepts_zero_args = pi->accepts_zero_args;
          r.return_type_name = pi->return_type_name;
          return r;
        }
        if (u.find_export_var(name)) { r.kind = ResolvedKind::UnitVar; return r; }
        if (u.find_export_const(name)) { r.kind = ResolvedKind::UnitConst; return r; }
        if (u.has_export_enum_member(name)) { r.kind = ResolvedKind::EnumMember; return r; }
        if (u.has_export_type(name)) { r.kind = ResolvedKind::UnitType; return r; }
      }
    }
    // RTL unit we don't parse (e.g. `dos.getenv` when dos.pas isn't in our
    // source tree). Keep the Pascal unit qualifier in the emitted text and
    // let the runtime's stub namespace alias own that lookup.
    r.kind = ResolvedKind::Unknown;
    return r;
  }
  if (qk == QualifierKind::Class) {
    if (registry) {
      if (auto* m = registry->lookup_class_method(qualifier, name)) {
        r.kind = ResolvedKind::ClassMethod;
        r.proc = m->decl.get();
        r.is_callable = true;
        r.is_parameterless = (m->param_count == 0);
        r.accepts_zero_args = m->accepts_zero_args;
        r.cxx = mangle(name);  // caller emits the `base.` prefix
        return r;
      }
      if (registry->lookup_class_field(qualifier, name)) {
        r.kind = ResolvedKind::ClassField;
        r.cxx = mangle(name);  // caller emits the `base.` prefix
        return r;
      }
    }
    r.cxx = mangle(name);
    r.kind = ResolvedKind::Unknown;
    return r;
  }

  // ----- Unqualified lookup. -----

  // 1. Function-name-as-read inside its own body -> the implicit Pascal
  //    result variable.
  if (current_fn_is_function && current_fn_result_type &&
      !current_fn_name.empty() && name == current_fn_name) {
    r.cxx = current_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (bare_result_type && is_pascal_result_ident(name)) {
    r.cxx = bare_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  if (outer_result_type && !outer_result_name.empty() &&
      name == outer_result_name) {
    r.cxx = outer_result_slot_name;
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  // 2. `with X do` bindings (inside-out). Fields and methods of X's
  //    class (walking ancestors) shadow outer scopes.
  if (registry) {
    for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
      const std::string& cls = it->class_name;
      const std::string& access = it->access_op;
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type &&
          (name == "classtype" || name == "instancesize")) {
        r.cxx = it->cxx_text + access + mangle(name);
        r.kind = ResolvedKind::WithMethod;
        r.is_callable = true;
        r.is_parameterless = true;
        r.accepts_zero_args = true;
        return r;
      }
      // `with obj do ... Free;` -- bare-Ident form of `obj.Free`. The
      // Member-form has its own lowering through `maybe_lower_class_free_member`;
      // mirror it here so the inherited TObject method is found via the
      // with-bound expression rather than the registry chain (TObject
      // itself is built-in, not in `registry->classes`, so a generic
      // class-method lookup dead-ends before reaching it).
      if (const auto* ci = class_info_for_type_name(cls);
          ci && ci->is_reference_type && name == "free") {
        r.cxx = "::rt::p_tobject::p_free(" + it->cxx_text + ")";
        r.kind = ResolvedKind::WithMethod;
        // The expression is already a complete call; no implicit-zero-arg
        // wrap is wanted at the use site.
        r.is_callable = false;
        return r;
      }
      if (!cls.empty()) {
        if (auto* m = registry->lookup_class_method(cls, name)) {
          r.cxx = it->cxx_text + access + mangle(name);
          r.kind = ResolvedKind::WithMethod;
          r.proc = m->decl.get();
          r.is_callable = true;
          r.is_parameterless = (m->param_count == 0);
          r.accepts_zero_args = m->accepts_zero_args;
          return r;
        }
        if (registry->lookup_class_field(cls, name)) {
          r.cxx = it->cxx_text + access + mangle(name);
          r.kind = ResolvedKind::WithField;
          return r;
        }
      }
      if (lookup_record_field_type_in_with(*it, name)) {
        r.cxx = it->cxx_text + access + mangle(name);
        r.kind = ResolvedKind::WithField;
        return r;
      }
    }
  }
  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = local_nested_fns.find(name);
    if (nit != local_nested_fns.end()) {
      r.kind = ResolvedKind::NestedFn;
      r.cxx = mangle(name);
       r.proc = nit->second.decl;
      r.is_callable = true;
      r.is_parameterless = (nit->second.param_count == 0);
      r.accepts_zero_args = nit->second.accepts_zero_args;
      return r;
    }
  }
  // 4. Procedure-local (param, var, typed const, nested-proc-name).
  if (local_scope.count(name)) {
    r.kind = ResolvedKind::Local;
    r.cxx = mangle(name);
    return r;
  }
  for (const auto& [_, en] : local_enums) {
    if (!en) continue;
    for (const auto& member : en->members) {
      if (ascii_lower(member.name) == ascii_lower(name)) {
        r.kind = ResolvedKind::EnumMember;
        r.cxx = mangle(name);
        return r;
      }
    }
  }
  if (auto prop = maybe_resolve_implicit_property(name)) return *prop;
  // 5. Current class's members (chain).
  if (!current_class_name.empty() && registry) {
    if (const auto* ci = class_info_for_type_name(current_class_name);
        ci && ci->is_reference_type &&
        (name == "classtype" || name == "instancesize")) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassMethod;
      r.is_callable = true;
      r.is_parameterless = true;
      r.accepts_zero_args = true;
      return r;
    }
    if (auto* m = registry->lookup_class_method(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassMethod;
      r.proc = m->decl.get();
      r.is_callable = true;
      r.is_parameterless = (m->param_count == 0);
      r.accepts_zero_args = m->accepts_zero_args;
      return r;
    }
    if (registry->lookup_class_field(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::ClassField;
      return r;
    }
    // Inline anonymous enum used as a class-field type contributes its
    // members to the enclosing class scope. C++ resolves the bare name
    // through the enclosing-class scope at the use site, so we emit
    // unqualified.
    if (registry->class_has_enum_member(current_class_name, name)) {
      r.cxx = mangle(name);
      r.kind = ResolvedKind::EnumMember;
      return r;
    }
  }
  // 6. Unit-level -- own unit first, then cross-unit (`uses` chain).
  if (registry) {
    auto uit = registry->units.find(current_unit_name);
    const UnitInfo* ui = (uit != registry->units.end())
                            ? &uit->second : nullptr;
    bool own = ui && ui->has(name);
    // Current unit's own symbols shadow everything from `uses`.
    // Emit bare (C++ picks them up in the current namespace).
    if (ui) {
      if (auto* pi = ui->find_proc(name)) {
        r.cxx = mangle(name);
        r.kind = ResolvedKind::UnitProc;
        r.proc = pi->decl.get();
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        r.accepts_zero_args = pi->accepts_zero_args;
        return r;
      }
      if (ui->find_var(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitVar; return r;
      }
      if (ui->find_const(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitConst; return r;
      }
      if (ui->has_enum_member(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::EnumMember; return r;
      }
      if (ui->has_type(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitType; return r;
      }
    }
    // Cross-unit lookup: walk the current unit's `uses` list and pick
    // the first match in a unit that actually exports this name.
    // Ambiguity between same-named symbols in two `using namespace`'d
    // units is resolved by emitting the fully-qualified form.
    auto check_unit = [&](const std::string& un) -> bool {
      auto it = registry->units.find(un);
      if (it == registry->units.end()) return false;
      const UnitInfo& u = it->second;
      // Synthetic `__rt__` unit holds runtime builtins. Emit them fully
      // qualified so translated units do not depend on `using namespace
      // ::rt;` for correctness.
      const std::string prefix = unit_namespace_prefix(un);
      // Other units contribute only their interface-exported names.
      if (auto* pi = u.find_export_proc(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = (un == "__rt__") ? ResolvedKind::RtBuiltin
                                  : ResolvedKind::UnitProc;
        r.proc = pi->decl.get();
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        r.accepts_zero_args = pi->accepts_zero_args;
        r.return_type_name = pi->return_type_name;
        return true;
      }
      if (u.find_export_var(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitVar; return true;
      }
      if (u.find_export_const(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitConst; return true;
      }
      if (u.has_export_enum_member(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::EnumMember; return true;
      }
      if (u.has_export_type(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitType; return true;
      }
      return false;
    };
    if (ui) {
      // Right-to-left is Pascal's uses resolution order.
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (check_unit(*it)) return r;
      }
    }
    (void)own;  // already handled by the per-unit lookup above.
  }
  // 7. Fallback: unresolved free names are much more often runtime helpers
  //    than cross-unit symbols. Emit them as explicit `::rt::...`
  //    references instead of depending on open namespaces in generated
  //    units.
  r.cxx = "::rt::" + mangle(name);
  r.kind = ResolvedKind::Unknown;
  return r;
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
      if (n.name == "inherited") return "inherited{}";
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
      return want_call ? lower_implicit_zero_arg_call(rr.cxx, rr.proc) : rr.cxx;
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
      if (n.q_check &&
          (n.op == BinOp::Add || n.op == BinOp::Sub || n.op == BinOp::Mul) &&
          operand_is_integer(*n.lhs) && operand_is_integer(*n.rhs)) {
        const char* fn = (n.op == BinOp::Add) ? "tp2cc_add_checked"
                       : (n.op == BinOp::Sub) ? "tp2cc_sub_checked"
                                              : "tp2cc_mul_checked";
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
          auto cit = registry->classes.find(current_class_name);
          if (cit != registry->classes.end()) {
            parent = cit->second.parent;
            if (parent.empty() && cit->second.is_reference_type) {
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
      // `Unit.name` -- only when the base ident names a known unit
      // AND isn't shadowed by any nearer binding.
      if (registry && base_is_ident(base_name)) {
        bool shadowed = local_scope.count(base_name) > 0;
        if (!shadowed && !current_class_name.empty() &&
            (registry->lookup_class_method(current_class_name, base_name) ||
             registry->lookup_class_field(current_class_name, base_name) ||
             registry->lookup_class_property(current_class_name, base_name)))
          shadowed = true;
        if (!shadowed) {
          for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
            if (with_bind_has_visible_member(*it, base_name)) {
              shadowed = true; break;
            }
          }
        }
        if (!shadowed) {
          bool is_unit = registry->units.count(base_name) > 0;
          if (!is_unit) {
            auto uit = registry->units.find(current_unit_name);
            if (uit != registry->units.end()) {
              for (const auto& nm : uit->second.uses) {
                if (nm == base_name) { is_unit = true; break; }
              }
            }
          }
          if (is_unit) {
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Unit, base_name);
            if (rr.kind == ResolvedKind::UnitType) {
              const std::string qualified = base_name + "." + m.name;
              if (const auto* ci = class_info_for_type_name(qualified);
                  ci && ci->is_reference_type) {
                return metaclass_value_fn_cxx(qualified) + "()";
              }
            }
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? rr.cxx + "()" : rr.cxx;
          }
          // `TClass.method` -- Pascal's way to call a specific
          // class's method (typically the parent's version from
          // inside an override). Emit `TClass::method`.
          if (registry->classes.count(base_name) ||
              registry->records.count(base_name)) {
            if (!is_callee_context_) {
              std::vector<const Expr*> no_args;
              std::vector<const TypeExpr*> no_param_types;
              std::vector<UntypedArgKind> no_untyped_arg;
              std::vector<bool> no_mutable_ref_arg;
              if (auto ctor_call = maybe_lower_class_constructor_call(
                      base_name, m.name, no_args, no_param_types,
                      no_untyped_arg, no_mutable_ref_arg)) {
                return *ctor_call;
              }
            }
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Class, base_name);
            std::string text = mangle(base_name) + "::" + mangle(m.name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.accepts_zero_args;
            return want_call ? lower_implicit_zero_arg_call(text, rr.proc) : text;
          }
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
          if (const auto* method =
                  registry->lookup_class_method(metaclass, m.name)) {
            if (method->kind == SymKind::Constructor ||
                method->kind == SymKind::ClassMethod) {
              std::string text = base_cxx + "->" + mangle(m.name);
              bool want_call = !is_callee_context_ &&
                               method->accepts_zero_args;
              return want_call
                         ? lower_implicit_zero_arg_call(text, method->decl.get())
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
        if (auto* prop = registry->lookup_class_property(bcls, m.name)) {
          if (prop->params.empty()) {
            std::vector<const Expr*> no_indices;
            return lower_property_read(m.loc, base_cxx, bcls, *prop, no_indices);
          }
        }
      }
      std::string text = base_cxx + member_access_op(*m.base) + mangle(m.name);
      if (is_callee_context_ || !registry) return text;
      if (bcls.empty()) return text;
      if (const auto* method = registry->lookup_class_method(bcls, m.name)) {
        if (method->accepts_zero_args) {
          text = lower_implicit_zero_arg_call(text, method->decl.get());
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
          if (registry && (registry->classes.count(id.name) ||
                           registry->records.count(id.name))) {
            if (auto* method = registry->lookup_class_method(id.name, m.name);
                method && method->decl && !method->decl->is_class_method) {
              return "::rt::tp2cc_method_code<&" + mangle(id.name) + "::" +
                     method_pointer_helper_name(*method->decl) + ">()";
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
                  std::string field_cxx = mangle(m.name);
                  std::string field_type_cxx =
                      fi->type ? type_to_cxx(*fi->type) : std::string("void");
                  return "reinterpret_cast<" + field_type_cxx +
                         "*>(reinterpret_cast<uintptr_t>(" +
                         expr_to_cxx(*d.operand) + ") + offsetof(" +
                         struct_cxx + ", " + field_cxx + "))";
                }
              }
            }
          }
        }
      }
      bool saved = is_callee_context_;
      is_callee_context_ = true;
      std::string inner = expr_to_cxx(*a.operand);
      is_callee_context_ = saved;
      // Pascal `@arr` where `arr` is a flat byte-array (`array of
      // char` / `array of byte`) typically lands in a `pchar` or
      // `pointer` context -- the fpc compiler's fill buffers and
      // inline byte tables do exactly this. For that narrow case
      // emit `(::rt::p_char*)arr` using `rt::tp2cc_Array<byte>`'s pointer
      // decay. Anything deeper than one array level (e.g.
      // `array of array of char`) stays as `&arr` and the source
      // is expected to use a flatter spelling -- we do not paper
      // over nested-array type-punning at the translator level.
      if (registry) {
        const TypeExpr* ot = deduce_type(*a.operand);
        if (ot) ot = registry->canonicalize(ot);
        if (ot && ot->kind == Kind::TyArray) {
          const auto& ar = static_cast<const TyArray&>(*ot);
          const TypeExpr* elem = ar.element.get();
          if (elem) elem = registry->canonicalize(elem);
          if (elem && elem->kind == Kind::TyName) {
            std::string en = ascii_lower(static_cast<const TyName&>(*elem).name);
            if (en == "byte" || en == "char" || en == "uint8_t" ||
                en == "shortint") {
              return "((::rt::p_char*)(" + inner + "))";
            }
          }
        }
      }
      return "(&" + inner + ")";
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Only three Pascal builtins need special emit-time handling -- the
      // rest (length, ord, chr, assigned, odd, abs, sqr, sqrt, sin, cos,
      // ln, exp, arctan, trunc, round, int, frac, inc, dec, succ, pred,
      // ...) live in `rt::` under their exact Pascal names and pass through
      // ordinary name resolution as explicit `::rt::...` calls.
      //
      // Special cases below:
      //   * `low(T)` / `high(T)` when T is a type name  -> emitted constant
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
            return registry->classes.count(low) ||
                   registry->records.count(low) ||
                   registry->enums.count(low) ||
                   registry->aliases.count(low);
          }
          return false;
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
          if (inner.empty()) inner = "sizeof(" + expr_to_cxx(*c.args[0]) + ")";
          return "static_cast<int32_t>(" + inner + ")";
        } else if (n == "typeof" && c.args.size() == 1 &&
                   c.args[0]->kind == Kind::Ident && registry) {
          // Pascal `typeof(T)` takes a TYPE NAME, not a value. In C++
          // we have no VMT-by-type-name runtime object; stub as
          // `nullptr` with a dummy template-arg tag so the expression
          // at least compiles. Users of this value compare it for
          // equality/inequality at runtime only.
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          if (registry->classes.count(a.name) ||
              registry->records.count(a.name)) {
            return "((void*)nullptr)";
          }
        } else if (c.args.size() == 1 && is_primitive_type(n)) {
          // Function-style type cast in expression context.
          // Only the explicit lvalue forms handled elsewhere
          // (`T(lv) := ...`, `inc(T(lv))`, `dec(T(lv))`) reinterpret
          // storage. Plain `T(expr)` remains a value conversion.
          if (n == "ansistring" || n == "utf8string") {
            return "::rt::tp2cc_ansistring_of(" + arg0() + ")";
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
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            const TypeExpr* source_ty = canonicalize_type(deduce_type(*peeled));
            if (source_ty &&
                (source_ty->kind == Kind::TyArray ||
                 source_ty->kind == Kind::TyRecord ||
                 source_ty->kind == Kind::TyObject ||
                 source_ty->kind == Kind::TyProcedural)) {
              // Aggregate-to-primitive typecasts in Pascal are byte
              // reinterpretations, not numeric conversions. `double(MathInf)`
              // in the compiler sources depends on preserving the byte pattern.
              return "::rt::tp2cc_reinterpret_copy<" + primitive_type_cxx(n) +
                     ">(" + expr_to_cxx(*peeled) + ")";
            }
          }
          if (n == "char") {
            return "::rt::p_chr(" + arg0() + ")";
          }
          if (n == "pointer" || n == "pchar" || n == "ppchar") {
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
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          if (registry) {
            auto cit = registry->classes.find(n);
            if (cit != registry->classes.end() && cit->second.is_reference_type) {
              // Direct named class casts (`TNode(p)`) do not go through the
              // alias table, because reference classes are registered as
              // classes rather than aliases. Treat them as pointer casts here.
              TyName cast_name;
              cast_name.name = n;
              return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
            }
          }
          const TypeExpr* cast_ty = nullptr;
          auto lit = local_type_aliases_scoped.find(n);
          if (lit != local_type_aliases_scoped.end()) {
            cast_ty = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(n);
            if (ait != registry->aliases.end() && ait->second.target.get()) {
              cast_ty = canonicalize_type(ait->second.target.get());
            }
          }
          if (cast_ty && cast_ty->kind == Kind::TyMetaclass) {
            return "((" + type_name_text_to_cxx(n) + ")(" +
                   const_value_to_cxx(*c.args[0], cast_ty,
                                      /*explicit_conversion=*/true) +
                   "))";
          }
          if (cast_ty && cast_ty->kind == Kind::TySet) {
            return "::rt::tp2cc_set_cast<" + type_to_cxx(*cast_ty) + ">(" +
                   arg0() + ")";
          }
          if (cast_ty && type_is_reference_class(cast_ty)) {
            // `TClass(expr)` is a class-pointer cast in Pascal, not a C++
            // direct-initialisation attempt. Emit an explicit pointer cast so
            // bootstrap casts like `TLinkedListItemClass(ClassType)` keep
            // pointer semantics instead of turning into constructor calls.
            TyName cast_name;
            cast_name.name = n;
            return "((" + type_name_to_cxx(cast_name) + ")(" + arg0() + "))";
          }
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          bool named_storage_view_type =
              registry && (registry->records.count(n) || registry->classes.count(n));
          if (cast_ty && peeled && expr_is_storage_lvalue(*c.args[0]) &&
              (cast_ty->kind == Kind::TyArray ||
               cast_ty->kind == Kind::TyRecord ||
               cast_ty->kind == Kind::TyObject ||
               cast_ty->kind == Kind::TyProcedural)) {
            const TypeExpr* source_ty = deduce_type(*peeled);
            bool pointee_view =
                expr_is_untyped_storage_ref(*c.args[0]) ||
                type_is_pointerish(source_ty);
            return reinterpret_ref_text(type_name_text_to_cxx(n),
                                        expr_to_cxx(*peeled),
                                        pointee_view);
          }
          if (named_storage_view_type && peeled &&
              expr_is_storage_lvalue(*c.args[0])) {
            const TypeExpr* source_ty = deduce_type(*peeled);
            bool pointee_view =
                expr_is_untyped_storage_ref(*c.args[0]) ||
                type_is_pointerish(source_ty);
            return reinterpret_ref_text(type_name_text_to_cxx(n),
                                        expr_to_cxx(*peeled),
                                        pointee_view);
          }
          if (cast_ty && cast_ty->kind == Kind::TyArray) {
            const auto& arr = static_cast<const TyArray&>(*cast_ty);
            const TypeExpr* elem =
                arr.element ? canonicalize_type(arr.element.get()) : nullptr;
            if (arr.dims.size() == 1 &&
                (tyname_is(elem, "byte") || tyname_is(elem, "char"))) {
              return "::rt::tp2cc_reinterpret_bytes<" +
                     type_name_text_to_cxx(n) + ">(" + arg0() + ")";
            }
          }
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2) &&
                   c.args[0]->kind == Kind::Call) {
          // Pascal `inc(T(lv))` / `dec(T(lv))` mutate the storage behind
          // `lv` as type T. Emit that reinterpreting lvalue explicitly.
          const auto& inner = static_cast<const Call&>(*c.args[0]);
          if (std::string ptr = primitive_cast_untyped_storage_ptr(inner);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*inner.callee);
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ", " +
                     expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ")";
          }
          // `inc(T(packed_record.field))`: the packed field cannot be
          // bound as `T&`, so route through the same memcpy-based path as
          // untyped storage. The address-of is byte-safe because
          // `tp2cc_reinterpret_inc` reads/writes via memcpy.
          if (std::string ptr = primitive_cast_packed_field_ptr(inner);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*inner.callee);
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ", " +
                     expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + primitive_type_cxx(id.name) + ">(" + ptr + ")";
          }
          if (std::string ref = primitive_cast_lvalue_ref(inner);
              !ref.empty()) {
            std::string op = (n == "inc") ? "::rt::p_inc" : "::rt::p_dec";
            if (c.args.size() == 2) {
              return op + "(" + ref + ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "(" + ref + ")";
          }
          // Fall through to generic emission.
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2)) {
          // `inc(packed_record.field)` without an outer typed cast: same
          // packed-field problem as the cast case above, but the operand
          // type is the field's own declared type rather than a cast type.
          if (auto storage = packed_field_storage_ref(*c.args[0])) {
            std::string op = (n == "inc") ? "::rt::tp2cc_reinterpret_inc"
                                          : "::rt::tp2cc_reinterpret_dec";
            if (c.args.size() == 2) {
              return op + "<" + storage->elem_cxx + ">(" + storage->void_ptr_text +
                     ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "<" + storage->elem_cxx + ">(" + storage->void_ptr_text + ")";
          }
          // Fall through to generic emission for non-packed scalar args.
        } else if (n == "new" && !c.args.empty()) {
          // Expression-form `new(T)` or `new(T, Ctor(args))`. The first
          // arg is the *pointer-type name* (an Ident), which we already
          // emit as `p_T` -- the underlying struct is
          // `std::remove_pointer_t<p_T>`.
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
          std::string t = expr_to_cxx(*c.args[0]);
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
            if (registry && c.args[0]->kind == Kind::Ident &&
                cc.callee->kind == Kind::Ident) {
              TyName ptr_type;
              ptr_type.name = static_cast<const Ident&>(*c.args[0]).name;
              std::string pointee = registry->pointer_target_type_name(&ptr_type);
              if (!pointee.empty()) {
                if (auto* m = registry->lookup_class_method(
                        pointee, static_cast<const Ident&>(*cc.callee).name)) {
                  ctor_decl = m->decl.get();
                }
              }
            }
            std::vector<const Expr*> ctor_args;
            ctor_args.reserve(cc.args.size());
            for (const auto& arg : cc.args) ctor_args.push_back(arg.get());
            append_defaulted_trailing_call_args(ctor_decl, ctor_args);
            std::vector<UntypedArgKind> untyped_arg(ctor_args.size(),
                                                    UntypedArgKind::None);
            std::vector<bool> mutable_ref_arg(ctor_args.size(), false);
            std::vector<const TypeExpr*> param_types(ctor_args.size(), nullptr);
            mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                 param_types);
            for (size_t i = 0; i < ctor_args.size(); ++i) {
              if (i) margs += ", ";
              margs += lower_call_arg(*ctor_args[i], param_types[i],
                                      untyped_arg[i], mutable_ref_arg[i]);
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
        if (id.name == "pointer") {
          cast_to_pointer = true;
          cast_type_cxx = "void*";
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
          }
        }
        if (cast_to_pointer) {
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            return "((" + cast_type_cxx + ")(" + expr_to_cxx(*peeled) + "))";
          }
          return "((" + cast_type_cxx + ")(" + expr_to_cxx(*c.args[0]) + "))";
        }
      }
      if (c.args.size() == 1 && c.callee->kind == Kind::Member && registry) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        if (mem.base->kind == Kind::Ident) {
          const auto& base_id = static_cast<const Ident&>(*mem.base);
          const std::string& base_name = base_id.name;
          bool shadowed = local_scope.count(base_name) > 0;
          if (!shadowed && !current_class_name.empty() &&
              (registry->lookup_class_method(current_class_name, base_name) ||
               registry->lookup_class_field(current_class_name, base_name) ||
               registry->lookup_class_property(current_class_name, base_name))) {
            shadowed = true;
          }
          if (!shadowed) {
            for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
              if (with_bind_has_visible_member(*it, base_name)) {
                shadowed = true;
                break;
              }
            }
          }
          bool is_unit = !shadowed && registry->units.count(base_name) > 0;
          if (!is_unit && !shadowed) {
            auto uit = registry->units.find(current_unit_name);
            if (uit != registry->units.end()) {
              for (const auto& nm : uit->second.uses) {
                if (nm == base_name) {
                  is_unit = true;
                  break;
                }
              }
            }
          }
          if (is_unit) {
            ResolveResult rr =
                resolve_name(mem.name, QualifierKind::Unit, base_name);
            if (rr.kind == ResolvedKind::UnitType) {
              const std::string qualified = base_name + "." + mem.name;
              const TypeExpr* cast_ty = lookup_named_type_expr(qualified);
              if (cast_ty) cast_ty = canonicalize_type(cast_ty);
              if (cast_ty && cast_ty->kind == Kind::TyPointer) {
                const Expr* peeled = peel_primitive_casts(c.args[0].get());
                if (peeled && expr_is_storage_lvalue(*c.args[0])) {
                  return "((" + type_name_text_to_cxx(qualified) + ")(" +
                         expr_to_cxx(*peeled) + "))";
                }
              }
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
          auto cit = registry->classes.find(id.name);
          if (cit != registry->classes.end()) {
            if (auto ctor_call = maybe_lower_class_constructor_call(
                    id.name, mem.name, call_args, call_param_types,
                    call_untyped_arg, call_mutable_ref_arg)) {
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
            is_tpexcept_setjmp = (rr.cxx == "p_tpexcept::p_setjmp");
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
        return "setjmp(p_tpexcept::p_detail::p_state_for(&(" +
               expr_to_cxx(*c.args[0]) + ")).p_env)";
      }
      std::string out = callee_text + "(";
      for (size_t i = 0; i < call_args.size(); ++i) {
        if (i) out += ", ";
        std::string arg_text = lower_call_arg(*call_args[i], call_param_types[i],
                                              call_untyped_arg[i],
                                              call_mutable_ref_arg[i]);
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
          if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
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
          if (auto* prop = registry->lookup_default_property(cls)) {
            return lower_property_read(i.loc, expr_to_cxx(*i.base), cls, *prop,
                                       indices);
          }
        }
      }
      if (auto view = untyped_storage_index_view(i)) {
        return "::rt::tp2cc_reinterpret_load<" + view->elem_cxx + ">(" +
               view->ptr_cxx + ")";
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
        out += "." + mangle(r.fields[i].first) + " = " +
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

std::string Emitter::param_list_to_cxx(const std::vector<Param>& params) {
  return decls_.param_list_to_cxx(params);
}

std::string Emitter::proc_return_type_to_cxx(const ProcDecl& pd) {
  return decls_.proc_return_type_to_cxx(pd);
}

void Emitter::emit_decl(const Decl& d, bool in_header) {
  decls_.emit_decl(d, in_header);
}

// ---------------------------------------------------------------------------
// Statements

void Emitter::emit_raise_stmt(const Raise& r) {
  if (r.value) {
    emitln("throw " + expr_to_cxx(*r.value) + ";");
    return;
  }
  if (except_handler_depth == 0) {
    report_error(r.loc, "bare raise is only valid inside an except handler");
  }
  emitln("throw;");
}

void Emitter::emit_try_stmt(const Try& t) {
  const std::string n = std::to_string(++try_stmt_counter);

  if (t.is_finally) {
    emitln("{");
    indent();
    // Pascal `finally` runs on every exit path from the block. Model that
    // with a C++ scope guard so `Exit`, loop control, and exception unwinding
    // all funnel through one emitted finally-body.
    emitln("auto tp2cc_finally_" + n + " = ::rt::tp2cc_make_scope_exit([&]() {");
    indent();
    for (const auto& sub : t.finally_body) emit_stmt(*sub);
    dedent();
    emitln("});");
    for (const auto& sub : t.body) emit_stmt(*sub);
    dedent();
    emitln("}");
    return;
  }

  if (t.handlers.empty()) {
    emitln("try {");
    indent();
    for (const auto& sub : t.body) emit_stmt(*sub);
    dedent();
    emitln("} catch (...) {");
    indent();
    ++except_handler_depth;
    if (t.except_else) emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
    emitln("}");
    return;
  }

  const std::string exc_name = "tp2cc_exc_" + n;
  const std::string handled_name = "tp2cc_handled_" + n;
  emitln("try {");
  indent();
  for (const auto& sub : t.body) emit_stmt(*sub);
  dedent();
  emitln("} catch (::rt::p_exception* " + exc_name + ") {");
  indent();
  emitln("bool " + handled_name + " = false;");
  for (size_t i = 0; i < t.handlers.size(); ++i) {
    const auto& h = t.handlers[i];
    std::string opener = (i == 0) ? "if" : "else if";
    if (h.class_name.empty()) {
      emitln(opener + " (true) {");
    } else {
      // Pascal `on E: TException do` only matches exception classes, so the
      // translated `dynamic_cast` target must be a pointer type even when the
      // name comes from the `sysutils` stub alias and does not resolve through
      // the normal class registry.
      TyName handler_type;
      handler_type.name = h.class_name;
      std::string handler_cxx = type_to_cxx(handler_type);
      if (handler_cxx.empty() || handler_cxx.back() != '*') {
        handler_cxx += "*";
      }
      emitln(opener + " (auto tp2cc_match_" + n + "_" + std::to_string(i) +
             " = dynamic_cast<" + handler_cxx + ">(" +
             exc_name + "); tp2cc_match_" + n + "_" + std::to_string(i) +
             ") {");
    }
    indent();
    emitln(handled_name + " = true;");
    std::optional<std::string> bound_name;
    std::optional<TyName> bound_type;
    auto saved_locals = local_scope;
    auto saved_types = local_types;
    if (!h.var_name.empty()) {
      bound_name = mangle(h.var_name);
      emitln("auto " + *bound_name + " = " +
             (h.class_name.empty()
                  ? exc_name
                  : "tp2cc_match_" + n + "_" + std::to_string(i)) +
             ";");
      local_scope.insert(h.var_name);
      bound_type.emplace();
      bound_type->name =
          h.class_name.empty() ? std::string("exception") : h.class_name;
      local_types[h.var_name] = &*bound_type;
    }
    ++except_handler_depth;
    if (h.body) emit_stmt(*h.body);
    --except_handler_depth;
    local_scope = std::move(saved_locals);
    local_types = std::move(saved_types);
    dedent();
    emitln("}");
  }
  if (t.except_else) {
    emitln("else {");
    indent();
    emitln(handled_name + " = true;");
    ++except_handler_depth;
    emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
    emitln("}");
  }
  emitln("if (!" + handled_name + ") throw;");
  dedent();
  if (t.except_else) {
    emitln("} catch (...) {");
    indent();
    ++except_handler_depth;
    emit_stmt(*t.except_else);
    --except_handler_depth;
    dedent();
  }
  emitln("}");
}

void Emitter::emit_stmt(const Stmt& s) {
  switch (s.kind) {
    case Kind::Compound: {
      const auto& c = static_cast<const Compound&>(s);
      emitln("{");
      indent();
      for (const auto& sub : c.body) emit_stmt(*sub);
      dedent();
      emitln("}");
      break;
    }
    case Kind::EmptyStmt: {
      emitln(";");
      break;
    }
    case Kind::Assign: {
      const auto& a = static_cast<const Assign&>(s);
      // Pascal `T(lv) := rhs` writes through a cast view of the same
      // storage. Emit that storage reinterpret explicitly.
      if (a.target->kind == Kind::Call) {
        const auto& c = static_cast<const Call&>(*a.target);
        if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
          if (std::string ptr = primitive_cast_untyped_storage_ptr(c);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*c.callee);
            emitln("::rt::tp2cc_reinterpret_store<" + primitive_type_cxx(id.name) +
                   ">(" + ptr + ", " + expr_to_cxx(*a.value) + ");");
            break;
          }
          // `T(packed_record.field) := rhs` -- forming a `T&` to a packed
          // field is UB, so route the assignment through `memcpy` via
          // `tp2cc_reinterpret_store` instead of `lvalue_ref = rhs;`.
          if (std::string ptr = primitive_cast_packed_field_ptr(c);
              !ptr.empty()) {
            const auto& id = static_cast<const Ident&>(*c.callee);
            emitln("::rt::tp2cc_reinterpret_store<" + primitive_type_cxx(id.name) +
                   ">(" + ptr + ", " + expr_to_cxx(*a.value) + ");");
            break;
          }
          if (std::string ref = primitive_cast_lvalue_ref(c); !ref.empty()) {
            emitln(ref + " = " + expr_to_cxx(*a.value) + ";");
            break;
          }
          const auto& id = static_cast<const Ident&>(*c.callee);
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
            std::string lv = expr_to_cxx(*c.args[0]);
            std::string rhs = expr_to_cxx(*a.value);
            emitln(reinterpret_ref_text(type_name_text_to_cxx(id.name), lv,
                                        expr_is_untyped_storage_ref(*c.args[0])) +
                   " = " + rhs + ";");
            break;
          }
        }
      }
      if (registry && a.target->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*a.target);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
            std::vector<const Expr*> no_indices;
            emitln(lower_property_write(a.loc, expr_to_cxx(*mem.base), cls, *prop,
                                        no_indices, *a.value) +
                   ";");
            break;
          }
        }
      }
      if (registry && a.target->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*a.target);
        if (auto text = maybe_lower_implicit_property_write(a.loc, id.name,
                                                            *a.value)) {
          emitln(*text + ";");
          break;
        }
      }
      if (registry && a.target->kind == Kind::Index) {
        const auto& ix = static_cast<const Index&>(*a.target);
        if (auto view = untyped_storage_index_view(ix)) {
          emitln("::rt::tp2cc_reinterpret_store<" + view->elem_cxx + ">(" +
                 view->ptr_cxx + ", " + expr_to_cxx(*a.value) + ");");
          break;
        }
        std::vector<const Expr*> indices;
        for (const auto& idx : ix.indices) indices.push_back(idx.get());
        if (ix.base->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*ix.base);
          std::string cls;
          if (mem.base->kind == Kind::Ident &&
              static_cast<const Ident&>(*mem.base).name == "self") {
            cls = current_class_name;
          } else {
            cls = deduce_class_alias(*mem.base);
          }
          if (!cls.empty()) {
            if (auto* prop = registry->lookup_class_property(cls, mem.name)) {
              if (!prop->params.empty()) {
                emitln(lower_property_write(a.loc, expr_to_cxx(*mem.base), cls,
                                            *prop, indices, *a.value) +
                       ";");
                break;
              }
            }
          }
        }
        if (ix.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*ix.base);
          if (auto found = find_implicit_class_property(id.name);
              found && found->prop && !found->prop->params.empty()) {
            emitln(lower_property_write(a.loc, found->base_cxx,
                                        found->class_name, *found->prop,
                                        indices, *a.value) +
                   ";");
            break;
          }
        }
        std::string cls = deduce_class_alias(*ix.base);
        if (!cls.empty()) {
          if (auto* prop = registry->lookup_default_property(cls)) {
            emitln(lower_property_write(a.loc, expr_to_cxx(*ix.base), cls, *prop,
                                        indices, *a.value) +
                   ";");
            break;
          }
        }
      }
      // Enable LHS-rewrite for the function name so that Pascal
      // `funcname := x`, `funcname[i] := x`, `funcname.field := x` etc.
      // all route to the result slot. We only scope the rewrite to the
      // target emission so the RHS still sees the function for recursive
      // calls.
      lhs_fn_rewrite = current_fn_is_function ? current_fn_name : "";
      lhs_fn_rewrite_slot =
          current_fn_is_function ? current_result_slot_name : "";
      lhs_outer_result_rewrite = outer_result_name;
      lhs_outer_result_rewrite_slot = outer_result_slot_name;
      std::string target_cxx = expr_to_cxx(*a.target);
      lhs_fn_rewrite.clear();
      lhs_fn_rewrite_slot.clear();
      lhs_outer_result_rewrite.clear();
      lhs_outer_result_rewrite_slot.clear();
      const TypeExpr* target_ty = deduce_type(*a.target);
      std::string rhs_cxx = const_value_to_cxx(*a.value, target_ty);
      if (target_ty && shortstring_capacity_to_cxx(target_ty)) {
        emitln("::rt::tp2cc_shortstring_assign(" + target_cxx + ", " + rhs_cxx +
               ");");
        break;
      }
      // Narrowing-assignment lowering. With `{$R+}`, route through
      // tp2cc_range_check_assign which raises p_erangeerror when the
      // source can't be represented. With `{$R-}`, real->int still
      // needs a truncating helper because plain `(int)real` is UB in
      // C++ when out of range; integer->integer narrowing is fine
      // (modular truncation is well-defined on unsigned, and gcc
      // implements two's-complement on signed).
      if (target_ty) {
        const TypeExpr* tcanon = canonicalize_type(target_ty);
        if (tcanon && tcanon->kind == Kind::TyName) {
          const PrimitiveInfo* dst = primitive_info(
              ascii_lower(static_cast<const TyName&>(*tcanon).name));
          if (dst && (dst->int_kind == PrimitiveIntKind::Signed ||
                      dst->int_kind == PrimitiveIntKind::Unsigned)) {
            const TypeExpr* src_ty = deduce_type(*a.value);
            if (src_ty) src_ty = canonicalize_type(src_ty);
            const PrimitiveInfo* src = nullptr;
            bool src_is_real = false;
            if (src_ty && src_ty->kind == Kind::TyName) {
              std::string sn = ascii_lower(
                  static_cast<const TyName&>(*src_ty).name);
              src = primitive_info(sn);
              src_is_real = sn == "single" || sn == "double" ||
                            sn == "real" || sn == "extended" ||
                            sn == "comp" || sn == "bestreal";
            }
            if (a.r_check) {
              bool wrap = false;
              if (src_is_real) {
                wrap = true;
              } else if (src && (src->int_kind == PrimitiveIntKind::Signed ||
                                 src->int_kind == PrimitiveIntKind::Unsigned)) {
                if (src->bits > dst->bits || src->int_kind != dst->int_kind) {
                  wrap = true;
                }
              }
              if (wrap) {
                rhs_cxx = "::rt::tp2cc_range_check_assign<" +
                          std::string(dst->cxx) + ">(" + rhs_cxx + ")";
              }
            } else if (src_is_real) {
              rhs_cxx = "::rt::tp2cc_real_to_int_trunc<" +
                        std::string(dst->cxx) + ">(" + rhs_cxx + ")";
            }
          }
        }
      }
      emitln(target_cxx + " = " + rhs_cxx + ";");
      break;
    }
    case Kind::ExprStmt: {
      const auto& es = static_cast<const ExprStmt&>(s);
      // Pascal builtin control-flow statements (break / continue / exit)
      // and allocation builtins (new / dispose) need special lowering.
      // Classify once, then handle in a single if/else chain.
      std::string name;
      const Call* call_expr = nullptr;
      if (es.expr->kind == Kind::Ident) {
        name = static_cast<const Ident&>(*es.expr).name;
      } else if (es.expr->kind == Kind::Call) {
        call_expr = &static_cast<const Call&>(*es.expr);
        if (call_expr->callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*call_expr->callee).name;
        }
      }

      if (name == "break") {
        // Pascal break exits the enclosing loop even from inside a case.
        // Emit as goto so switch nesting can't swallow it.
        if (!loop_break_labels.empty()) {
          emitln("goto " + loop_break_labels.back() + ";");
        } else {
          emitln("break;");  // outside any loop -- let C++ diagnose
        }
      } else if (name == "continue") {
        if (!loop_continue_labels.empty()) {
          emitln("goto " + loop_continue_labels.back() + ";");
        } else {
          emitln("continue;");
        }
      } else if (name == "exit") {
        // exit or exit(v). In a Function, fill the result slot and return;
        // in a Procedure, return; in a Constructor, return the status.
        if (call_expr && !call_expr->args.empty() && current_fn_is_function) {
          emitln(current_result_slot_name + " = " +
                 const_value_to_cxx(*call_expr->args[0], current_fn_result_type) +
                 ";");
          emitln(std::string("return ") + current_result_slot_name + ";");
        } else if (current_fn_is_function || current_fn_is_ctor) {
          emitln(std::string("return ") +
                 (current_fn_is_function ? current_result_slot_name
                                         : kCtorStatusSlotName) +
                 ";");
        } else {
          emitln("return;");
        }
      } else if (name == "fail") {
        if (current_fn_is_ctor) {
          emitln(std::string(kCtorStatusSlotName) + " = false;");
          emitln(std::string("return ") + kCtorStatusSlotName + ";");
        } else {
          report_error(es.loc, "`fail` outside constructors is unsupported");
        }
      } else if (name == "new" && call_expr && !call_expr->args.empty()) {
        // new(p) or new(p, Ctor(args)). `p` might be `arr[i]` whose
        // `decltype` is a reference (`T&`); strip it before computing
        // the pointee so `new remove_pointer_t<T&>` doesn't arise. Route
        // statement-form Pascal `new` through the runtime helper rather than
        // raw C++ `new`, so later `reallocmem` / `dispose` on the same typed
        // storage stays in one allocation family.
        std::string p = lower_call_arg(*call_expr->args[0],
                                       /*param_type=*/nullptr,
                                       UntypedArgKind::None,
                                       /*mutable_ref_arg=*/true);
        emitln("::rt::p_new(" + p + ");");
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          const TypeExpr* ptr_arg_ty = deduce_type(*call_expr->args[0]);
          std::string args;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            const ProcDecl* ctor_decl = nullptr;
            if (registry && ptr_arg_ty && cc.callee->kind == Kind::Ident) {
              std::string pointee = registry->pointer_target_type_name(ptr_arg_ty);
              if (!pointee.empty()) {
                if (auto* m = registry->lookup_class_method(
                        pointee, static_cast<const Ident&>(*cc.callee).name)) {
                  ctor_decl = m->decl.get();
                }
              }
            }
            std::vector<UntypedArgKind> untyped_arg(cc.args.size(),
                                                    UntypedArgKind::None);
            std::vector<bool> mutable_ref_arg(cc.args.size(), false);
            std::vector<const TypeExpr*> param_types(cc.args.size(), nullptr);
            mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                 param_types);
            for (size_t i = 0; i < cc.args.size(); ++i) {
              if (i) args += ", ";
              args += lower_call_arg(*cc.args[i], param_types[i],
                                     untyped_arg[i], mutable_ref_arg[i]);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) {
            emitln("(*" + p + ")." + method + "(" + args + ");");
          }
        }
      } else if (name == "dispose" && call_expr && !call_expr->args.empty()) {
        // dispose(p) or dispose(p, Done)
        std::string p = lower_call_arg(*call_expr->args[0],
                                       /*param_type=*/nullptr,
                                       UntypedArgKind::None,
                                       /*mutable_ref_arg=*/true);
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) emitln("(*" + p + ")." + method + "();");
        }
        emitln("::rt::p_dispose(" + p + ");");
      } else if (es.expr->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*es.expr);
        if (auto free_call = maybe_lower_class_free_member(*mem.base, mem.name)) {
          emitln(*free_call + ";");
        } else {
          std::string text = expr_to_cxx(*es.expr);
          bool stmt_autocalls_member = false;
          if (registry) {
            std::string cls = deduce_class_alias(*mem.base);
            if (!cls.empty()) {
              if (const auto* method = registry->lookup_class_method(cls, mem.name)) {
                stmt_autocalls_member = method->accepts_zero_args;
              } else if (ascii_lower(mem.name) == "destroy") {
                if (const auto* ci = class_info_for_type_name(cls)) {
                  stmt_autocalls_member = ci->is_reference_type;
                }
              }
            }
          }
          auto stmt_autocalls_procvar = [&](const Expr& expr) -> bool {
            switch (expr.kind) {
              case Kind::Ident:
              case Kind::Member:
              case Kind::Index:
              case Kind::Deref:
                break;
              default:
                return false;
            }
            if (const TypeExpr* t = deduce_type(expr);
                t && (t = canonicalize_type(t)) &&
                t->kind == Kind::TyProcedural) {
              const auto& p = static_cast<const TyProcedural&>(*t);
              return procedural_param_count(p) == 0;
            }
            return false;
          };
          if ((stmt_autocalls_member || stmt_autocalls_procvar(*es.expr)) &&
              (text.empty() || text.back() != ')')) {
            text += "()";
          }
          emitln(text + ";");
        }
      } else {
        // `expr_to_cxx` auto-calls parameterless procs/methods in
        // value context via `resolve_name`. The one extra case we
        // handle here: Pascal statement-form `writeln;` / `readln;`
        // / `halt;` -- rt variadic builtins where 0 args is a
        // legitimate call. We don't try to auto-call parameterful
        // callables -- those are either emitted as `Call` (handled
        // above with args) or they're real source bugs (like an
        // `inherited init;` whose parent wants an arg) that should
        // be fixed in the Pascal, not papered over.
        std::string text = expr_to_cxx(*es.expr);
        if (es.expr->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*es.expr);
          ResolveResult rr = resolve_name(id.name);
          if (rr.accepts_zero_args &&
              !text.empty() && text.back() != ')') {
            text += "()";
          }
        }
        // Parameterless procedural variables are callable in statement
        // position (`olddo_stop;`) but must stay as plain values in
        // assignments like `do_stop := olddo_stop;`. Detect that only here.
        auto stmt_autocalls_procvar = [&](const Expr& expr) -> bool {
          switch (expr.kind) {
            case Kind::Ident:
            case Kind::Member:
            case Kind::Index:
            case Kind::Deref:
              break;
            default:
              return false;
          }
          if (const TypeExpr* t = deduce_type(expr);
              t && (t = canonicalize_type(t)) &&
              t->kind == Kind::TyProcedural) {
            const auto& p = static_cast<const TyProcedural&>(*t);
            return procedural_param_count(p) == 0;
          }
          return false;
        };
        if (stmt_autocalls_procvar(*es.expr)) text += "()";
        emitln(text + ";");
      }
      break;
    }
    case Kind::If: {
      const auto& i = static_cast<const If&>(s);
      emitln("if (" + expr_to_cxx(*i.cond) + ") {");
      indent();
      if (i.then_branch) emit_stmt(*i.then_branch);
      dedent();
      if (i.else_branch) {
        emitln("} else {");
        indent();
        emit_stmt(*i.else_branch);
        dedent();
      }
      emitln("}");
      break;
    }
    case Kind::While: {
      const auto& w = static_cast<const While&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      emitln("while (" + expr_to_cxx(*w.cond) + ") {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (w.body) emit_stmt(*w.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::Repeat: {
      const auto& r = static_cast<const Repeat&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      emitln("do {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      for (const auto& sub : r.body) emit_stmt(*sub);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("} while (!(" + expr_to_cxx(*r.cond) + "));");
      emitln(brk + ":;");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      ResolveResult vr = resolve_name(f.var);
      std::string var = vr.cxx.empty() ? mangle(f.var) : vr.cxx;
      std::string from = expr_to_cxx(*f.from);
      std::string to = expr_to_cxx(*f.to);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      // Pascal `for X := A to B do S` is NOT `for (X=A; X<=B; ++X)`:
      // when X's type is `byte` and B is 255, ++X wraps to 0 and the
      // condition never fails. True semantics: body runs for each X in
      // [A,B]; terminate by equality after the body. Snapshot the end
      // bound so mid-body assignments to B don't alter the loop count.
      emitln("{");
      indent();
      emitln("auto tp2cc_from = (" + from + ");");
      emitln("auto tp2cc_to = (" + to + ");");
      const char* cmp = f.downto ? ">=" : "<=";
      const char* step = f.downto ? "::rt::p_dec" : "::rt::p_inc";
      emitln(std::string("if (tp2cc_from ") + cmp + " tp2cc_to) {");
      indent();
      emitln(var + " = tp2cc_from;");
      emitln("while (true) {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (f.body) emit_stmt(*f.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      emitln("if (" + var + " == tp2cc_to) break;");
      emitln(step + std::string("(") + var + ");");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      auto selector_is_charish = [&]() -> bool {
        const TypeExpr* t = deduce_type(*cs.selector);
        if (!t) return false;
        t = canonicalize_type(t);
        return tyname_is(t, "char");
      };
      auto case_expr = [&](const Expr& e) -> std::string {
        std::string text = expr_to_cxx(e);
        return selector_is_charish() ? "::rt::p_ord(" + text + ")" : text;
      };
      emitln("switch (" + case_expr(*cs.selector) + ") {");
      indent();
      for (const auto& arm : cs.arms) {
        for (const auto& lab : arm.labels) {
          if (lab->kind == Kind::Range) {
            // GCC case-range extension: `case lo ... hi:`. Acceptable here;
            // the gnu profile compiler supports it. TODO: iterate label
            // values for strict standard C++.
            const auto& r = static_cast<const Range&>(*lab);
            emitln("case " + case_expr(*r.lo) + " ... " +
                   case_expr(*r.hi) + ":");
          } else {
            emitln("case " + case_expr(*lab) + ":");
          }
        }
        indent();
        if (arm.body) emit_stmt(*arm.body);
        emitln("break;");
        dedent();
      }
      if (cs.else_branch) {
        emitln("default:");
        indent();
        emit_stmt(*cs.else_branch);
        emitln("break;");
        dedent();
      }
      dedent();
      emitln("}");
      break;
    }
    case Kind::With: {
      // Pascal `with A, B do S` opens A's and B's fields (and methods)
      // as unqualified names inside S. We alias each target and push
      // its deduced type onto `with_stack`; bare
      // idents inside S that match a field of any stacked type are
      // rewritten by the expression emitter to that alias.
      const auto& w = static_cast<const With&>(s);
      emitln("{");
      indent();
      size_t pushed = 0;
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        const Expr& with_expr = *w.exprs[i];
        const TypeExpr* ty = deduce_type(with_expr);
        if (ty) ty = canonicalize_type(ty);
        std::string nm = "tp2cc_with_" + std::to_string(with_stack.size());
        std::string init = expr_to_cxx(with_expr);
        bool bind_by_ref = expr_is_storage_lvalue(with_expr);
        // `with T(p) do` and similar casts produce pointer rvalues. Bind those
        // by value; only genuine lvalues can be safely aliased with `auto&`.
        emitln(std::string(bind_by_ref ? "auto& " : "auto ") + nm + " = " +
               init + ";");
        WithBind wb;
        wb.cxx_text = nm;
        wb.type = ty;
        wb.class_name = deduce_class_alias(with_expr);
        wb.access_op = member_access_op(with_expr);
        with_stack.push_back(std::move(wb));
        ++pushed;
      }
      if (w.body) emit_stmt(*w.body);
      for (size_t i = 0; i < pushed; ++i) with_stack.pop_back();
      dedent();
      emitln("}");
      break;
    }
    case Kind::Goto: {
      const auto& g = static_cast<const Goto&>(s);
      emitln("goto p_" + g.label + ";");
      break;
    }
    case Kind::Labeled: {
      const auto& lb = static_cast<const Labeled&>(s);
      emitln("p_" + lb.label + ":");
      if (lb.body) emit_stmt(*lb.body);
      break;
    }
    case Kind::AsmStmt: {
      report_error(s.loc, "asm blocks are unsupported");
      emitln("/* unsupported asm */");
      break;
    }
    case Kind::Raise:
      emit_raise_stmt(static_cast<const Raise&>(s));
      break;
    case Kind::Try:
      emit_try_stmt(static_cast<const Try&>(s));
      break;
    default:
      report_error(s.loc,
                   "unsupported statement kind " +
                       std::to_string(static_cast<int>(s.kind)));
      emitln("/* unsupported-stmt kind=" +
             std::to_string(static_cast<int>(s.kind)) + " */;");
      break;
  }
}

// Forward decl so emit_proc_body / emit_nested_proc_lambda can call it
// to forward-declare record/object types in local type-decls before
// pointer aliases that reference them.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<ast::DeclPtr>& decls);

void Emitter::emit_proc_body(const ProcDecl& pd) {
  // Header line: ret ClassName::Method(args) or ret Method(args).
  std::string ret = proc_return_type_to_cxx(pd);
  std::string qname = mangle(pd.name);
  if (!pd.of_type.empty()) qname = mangle(pd.of_type) + "::" + qname;
  emitln(ret + " " + qname + "(" + param_list_to_cxx(pd.params) + ") {");
  indent();

  if (pd.is_abstract && !pd.body) {
    // Pascal's abstract methods are often placeholder hooks on classes that
    // native FPC still instantiates. Emit a fail-fast body instead of a pure
    // virtual so the translated class layout stays constructible while any
    // accidental call still stops immediately.
    emitln("::std::abort();");
    dedent();
    emitln("}");
    return;
  }

  // Save outer state and set for this body.
  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  const ast::TypeExpr* saved_result_type = current_fn_result_type;
  std::string saved_result_slot_name = current_result_slot_name;
  std::string saved_bare_result_slot_name = bare_result_slot_name;
  const ast::TypeExpr* saved_bare_result_type = bare_result_type;
  std::string saved_outer_result_name = outer_result_name;
  std::string saved_outer_result_slot_name = outer_result_slot_name;
  const ast::TypeExpr* saved_outer_result_type = outer_result_type;
  std::string saved_class = current_class_name;
  auto saved_locals = local_scope;
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = (pd.pkind == ProcKind::Constructor);
  current_fn_result_type = pd.return_type.get();
  std::string inherited_outer_result_name;
  std::string inherited_outer_result_slot_name;
  const ast::TypeExpr* inherited_outer_result_type = nullptr;
  if (saved_fn && saved_result_type) {
    inherited_outer_result_name = saved_name;
    inherited_outer_result_slot_name = saved_result_slot_name;
    inherited_outer_result_type = saved_result_type;
  } else {
    inherited_outer_result_name = saved_outer_result_name;
    inherited_outer_result_slot_name = saved_outer_result_slot_name;
    inherited_outer_result_type = saved_outer_result_type;
  }
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    current_result_slot_name =
        inherited_outer_result_type ? nested_result_slot_name(pd.name)
                                    : std::string(kPascalResultSlotName);
    bare_result_slot_name = current_result_slot_name;
    bare_result_type = pd.return_type.get();
  } else {
    current_result_slot_name.clear();
    bare_result_slot_name = inherited_outer_result_slot_name;
    bare_result_type = inherited_outer_result_type;
  }
  outer_result_name = inherited_outer_result_name;
  outer_result_slot_name = inherited_outer_result_slot_name;
  outer_result_type = inherited_outer_result_type;
  current_class_name = pd.of_type;  // empty for free functions
  ++block_depth;

  // Populate local-scope set so the expression emitter won't auto-call
  // identifiers that happen to name a parameterless method in another
  // unit (e.g. a local `typename: string;` shadowing a method). Also
  // record declared types so `.field` / `.method` access on those
  // locals can be resolved from the type registry.
  auto saved_types = local_types;
  auto saved_consts = local_consts;
  auto saved_nested = local_nested_fns;
  auto saved_nested_forwards = local_nested_forwards;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_const_params = local_const_params;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto insert_local_name = [&](Location where, const std::string& name) {
    // Pascal functions already own an implicit `Result` variable, so any
    // local/parameter/const nested in that body may not reuse the name.
    if (bare_result_type && is_pascal_result_ident(name)) {
      report_error(where, "duplicate identifier `Result`");
      return false;
    }
    local_scope.insert(name);
    return true;
  };
  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      if (!insert_local_name(pd.loc, nm)) continue;
      if (p.type) {
        local_types[nm] = p.type.get();
        if (p.mode == Param::Const) local_const_params.insert(nm);
      } else {
        local_untyped_params.insert(nm);
      }
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        if (!insert_local_name(vd.loc, nm)) continue;
        if (vd.type) local_types[nm] = vd.type.get();
      }
      // Inline anonymous enum used as a var type (`var m : (a, b);`)
      // bleeds its members into the enclosing scope -- same rule as
      // class fields and unit-level vars (see typereg.cc:177).
      if (vd.type && vd.type->kind == Kind::TyEnum && !vd.names.empty()) {
        local_enums[vd.names.front()] =
            static_cast<const ast::TyEnum*>(vd.type.get());
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      if (!insert_local_name(cd.loc, cd.name)) continue;
      local_consts[cd.name] = &cd;
      if (const TypeExpr* ct = deduce_const_decl_type(cd)) {
        local_types[cd.name] = ct;
      }
      if (cd.type && cd.type->kind == Kind::TyEnum) {
        local_enums[cd.name] =
            static_cast<const ast::TyEnum*>(cd.type.get());
      }
    } else if (l->kind == Kind::TypeDecl) {
      // Pascal's local `type` section is statically visible to the
      // translator too -- record enums (for array-dim sizing and
      // `low(T)`/`high(T)` rewrites) and aliases (for canonicalize).
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      if (!insert_local_name(npd.loc, npd.name)) continue;
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.accepts_zero_args = proc_accepts_zero_args(npd);
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      nf.decl = &npd;
      local_nested_fns[npd.name] = nf;
    }
  }

  // `Result` is a Pascal-visible implicit variable in functions, so it
  // uses ordinary Pascal name mangling. Declare it before nested local
  // procedures/functions: Pascal lets those inner routines read and write
  // the enclosing function result, so the generated lambda must be able to
  // capture an already-declared C++ local.
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " " + current_result_slot_name + "{};");
  } else if (pd.pkind == ProcKind::Constructor) {
    emitln(std::string("bool ") + kCtorStatusSlotName + " = true;");
  }
  // Forward-declare any record/object types in locals so a pointer
  // alias that textually precedes its target still compiles inside
  // the function body.
  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function ||
      pd.pkind == ProcKind::Constructor) {
    emitln(std::string("return ") +
           (pd.pkind == ProcKind::Function ? current_result_slot_name
                                           : kCtorStatusSlotName) +
           ";");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  current_fn_result_type = saved_result_type;
  current_result_slot_name = std::move(saved_result_slot_name);
  bare_result_slot_name = std::move(saved_bare_result_slot_name);
  bare_result_type = saved_bare_result_type;
  outer_result_name = std::move(saved_outer_result_name);
  outer_result_slot_name = std::move(saved_outer_result_slot_name);
  outer_result_type = saved_outer_result_type;
  current_class_name = std::move(saved_class);
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_consts = std::move(saved_consts);
  local_nested_fns = std::move(saved_nested);
  local_nested_forwards = std::move(saved_nested_forwards);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_const_params = std::move(saved_local_const_params);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("}");
}

void Emitter::emit_nested_proc_lambda(const ProcDecl& pd) {
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  // Build the param-type list for the std::function signature.
  std::string sig_params;
  {
    bool first = true;
    for (const auto& p : pd.params) {
      std::string pt;
      if (!p.type) {
        pt = "void*";
      } else {
        pt = type_to_cxx(*p.type);
        if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
        else if (p.mode == Param::Const &&
                 const_param_needs_mutable_ref(p.type.get()))
          pt += "&";
        else if (p.mode == Param::Const &&
                 const_param_needs_const_ref(p.type.get()))
          pt = "const " + pt + "&";
      }
      for (const auto& n : p.names) {
        (void)n;
        if (!first) sig_params += ", ";
        first = false;
        sig_params += pt;
      }
      if (p.names.empty()) {
        if (!first) sig_params += ", ";
        first = false;
        sig_params += pt;
      }
    }
  }

  const std::string lname = mangle(pd.name);
  // Forward-declare the std::function so the lambda can recurse by name.
  if (!local_nested_forwards.count(pd.name)) {
    emitln("::std::function<" + ret + "(" + sig_params + ")> " + lname + ";");
  }
  emitln(lname + " = [&](" + param_list_to_cxx(pd.params) + ") -> " + ret +
         " {");
  indent();

  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  const ast::TypeExpr* saved_result_type = current_fn_result_type;
  std::string saved_result_slot_name = current_result_slot_name;
  std::string saved_bare_result_slot_name = bare_result_slot_name;
  const ast::TypeExpr* saved_bare_result_type = bare_result_type;
  std::string saved_outer_result_name = outer_result_name;
  std::string saved_outer_result_slot_name = outer_result_slot_name;
  const ast::TypeExpr* saved_outer_result_type = outer_result_type;
  auto saved_locals = local_scope;
  auto saved_types = local_types;
  auto saved_consts = local_consts;
  auto saved_nested = local_nested_fns;
  auto saved_nested_forwards = local_nested_forwards;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_const_params = local_const_params;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto insert_local_name = [&](Location where, const std::string& name) {
    if (bare_result_type && is_pascal_result_ident(name)) {
      report_error(where, "duplicate identifier `Result`");
      return false;
    }
    local_scope.insert(name);
    return true;
  };
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = false;
  current_fn_result_type = pd.return_type.get();
  std::string inherited_outer_result_name;
  std::string inherited_outer_result_slot_name;
  const ast::TypeExpr* inherited_outer_result_type = nullptr;
  if (saved_fn && saved_result_type) {
    inherited_outer_result_name = saved_name;
    inherited_outer_result_slot_name = saved_result_slot_name;
    inherited_outer_result_type = saved_result_type;
  } else {
    inherited_outer_result_name = saved_outer_result_name;
    inherited_outer_result_slot_name = saved_outer_result_slot_name;
    inherited_outer_result_type = saved_outer_result_type;
  }
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    current_result_slot_name =
        inherited_outer_result_type ? nested_result_slot_name(pd.name)
                                    : std::string(kPascalResultSlotName);
    bare_result_slot_name = current_result_slot_name;
    bare_result_type = pd.return_type.get();
  } else {
    current_result_slot_name.clear();
    bare_result_slot_name = inherited_outer_result_slot_name;
    bare_result_type = inherited_outer_result_type;
  }
  outer_result_name = inherited_outer_result_name;
  outer_result_slot_name = inherited_outer_result_slot_name;
  outer_result_type = inherited_outer_result_type;
  ++block_depth;

  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      if (!insert_local_name(pd.loc, nm)) continue;
      if (p.type) {
        local_types[nm] = p.type.get();
        if (p.mode == Param::Const) local_const_params.insert(nm);
      } else {
        local_untyped_params.insert(nm);
      }
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        if (!insert_local_name(vd.loc, nm)) continue;
        if (vd.type) local_types[nm] = vd.type.get();
      }
      if (vd.type && vd.type->kind == Kind::TyEnum && !vd.names.empty()) {
        local_enums[vd.names.front()] =
            static_cast<const ast::TyEnum*>(vd.type.get());
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      if (!insert_local_name(cd.loc, cd.name)) continue;
      local_consts[cd.name] = &cd;
      if (const TypeExpr* ct = deduce_const_decl_type(cd)) {
        local_types[cd.name] = ct;
      }
      if (cd.type && cd.type->kind == Kind::TyEnum) {
        local_enums[cd.name] =
            static_cast<const ast::TyEnum*>(cd.type.get());
      }
    } else if (l->kind == Kind::TypeDecl) {
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      if (!insert_local_name(npd.loc, npd.name)) continue;
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.accepts_zero_args = proc_accepts_zero_args(npd);
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      nf.decl = &npd;
      local_nested_fns[npd.name] = nf;
    }
  }

  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " " + current_result_slot_name + "{};");
  }
  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function) {
    emitln(std::string("return ") + current_result_slot_name + ";");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  current_fn_result_type = saved_result_type;
  current_result_slot_name = std::move(saved_result_slot_name);
  bare_result_slot_name = std::move(saved_bare_result_slot_name);
  bare_result_type = saved_bare_result_type;
  outer_result_name = std::move(saved_outer_result_name);
  outer_result_slot_name = std::move(saved_outer_result_slot_name);
  outer_result_type = saved_outer_result_type;
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_consts = std::move(saved_consts);
  local_nested_fns = std::move(saved_nested);
  local_nested_forwards = std::move(saved_nested_forwards);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_const_params = std::move(saved_local_const_params);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("};");
}

// ---------------------------------------------------------------------------
// Unit

// Scan the decl list and emit forward declarations for every record/object
// type, so a pointer type that textually precedes its target still compiles.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    if (td.type->kind == Kind::TyRecord || td.type->kind == Kind::TyObject) {
      e.emitln("struct " + std::string("p_") + td.name + ";");
      if (td.type->kind == Kind::TyObject &&
          static_cast<const TyObject&>(*td.type).is_reference_type) {
        e.emitln("struct tp2cc_metaclass_" + std::string("p_") + td.name + ";");
      }
    }
  }
}

// Collect every TyName (lowercased) mentioned in a TypeExpr. Recurses into
// records/objects so that a record's field types contribute dependencies.
static void collect_type_refs(const TypeExpr& t,
                              std::unordered_set<std::string>& out) {
  switch (t.kind) {
    case Kind::TyName:
      out.insert(static_cast<const TyName&>(t).name);
      return;
    case Kind::TyPointer:
      if (static_cast<const TyPointer&>(t).target)
        collect_type_refs(*static_cast<const TyPointer&>(t).target, out);
      return;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      for (const auto& d : a.dims) if (d) collect_type_refs(*d, out);
      if (a.element) collect_type_refs(*a.element, out);
      return;
    }
    case Kind::TySet:
      if (static_cast<const TySet&>(t).element)
        collect_type_refs(*static_cast<const TySet&>(t).element, out);
      return;
    case Kind::TyFile:
      if (static_cast<const TyFile&>(t).element)
        collect_type_refs(*static_cast<const TyFile&>(t).element, out);
      return;
    case Kind::TyRecord: {
      const auto& r = static_cast<const TyRecord&>(t);
      for (const auto& f : r.fields) if (f.type) collect_type_refs(*f.type, out);
      for (const auto& vc : r.variant_cases)
        for (const auto& f : vc.fields)
          if (f.type) collect_type_refs(*f.type, out);
      return;
    }
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      if (!o.parent.empty()) out.insert(o.parent);
      for (const auto& m : o.members) {
        if (m.kind == ObjectMemberKind::Field && m.field_type) {
          collect_type_refs(*m.field_type, out);
        } else if (m.kind == ObjectMemberKind::Method && m.method) {
          if (m.method->return_type) collect_type_refs(*m.method->return_type, out);
          for (const auto& p : m.method->params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        } else if (m.kind == ObjectMemberKind::Property) {
          if (m.property.type) collect_type_refs(*m.property.type, out);
          for (const auto& p : m.property.params) {
            if (p.type) collect_type_refs(*p.type, out);
          }
        }
      }
      return;
    }
    case Kind::TySubrange:
    case Kind::TyString:
    case Kind::TyEnum:
      return;
    case Kind::TyProcedural: {
      const auto& p = static_cast<const TyProcedural&>(t);
      if (p.return_type) collect_type_refs(*p.return_type, out);
      for (const auto& par : p.params) {
        if (par.type) collect_type_refs(*par.type, out);
      }
      return;
    }
    default:
      return;
  }
}

// Reorder type decls so every alias (non-record, non-object) appears after
// the types it references by name. Record/object types are already
// forward-declared by emit_forward_struct_decls, so aliases that point to
// them via `^T` always work; this function only needs to handle aliases
// that depend on other aliases (e.g. `pfoo = ^tfoo` where `tfoo` is itself
// an alias to an array type).
//
// `in` must contain only type decls (checked by the caller); this runs
// against a single contiguous Pascal `type` section.
static std::vector<const Decl*> ordered_type_decls(
    const std::vector<const Decl*>& in) {
  std::vector<const Decl*> type_decls(in);

  // Map name -> index for quick lookup.
  std::unordered_map<std::string, int> index_of;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    index_of[static_cast<const TypeDecl*>(type_decls[i])->name] = i;
  }

  // For each type decl, which in-unit types does it reference?
  std::vector<std::unordered_set<int>> deps(type_decls.size());
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    const auto& td = *static_cast<const TypeDecl*>(type_decls[i]);
    if (!td.type) continue;
    std::unordered_set<std::string> refs;
    collect_type_refs(*td.type, refs);
    for (const auto& r : refs) {
      auto it = index_of.find(r);
      if (it == index_of.end()) continue;  // external / primitive
      int j = it->second;
      if (j == i) continue;
      const auto& rd = *static_cast<const TypeDecl*>(type_decls[j]);
      // Pointer-to-record aliases don't need the record body before them:
      // `using p_pfoo = p_tfoo*;` only needs the struct forward declaration
      // (emitted by emit_forward_struct_decls). This break lets cycles
      // like `Pfoo = ^Tfoo; Tfoo = record next: Pfoo; end;` remain a DAG.
      if (rd.type && (rd.type->kind == Kind::TyRecord ||
                      rd.type->kind == Kind::TyObject) &&
          td.type->kind == Kind::TyPointer) {
        continue;
      }
      deps[i].insert(j);
    }
  }

  // Kahn topological sort (stable: ties broken by original order).
  std::vector<int> indeg(type_decls.size(), 0);
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    for (int j : deps[i]) (void)j, ++indeg[i];
  }
  std::vector<int> ready;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (indeg[i] == 0) ready.push_back(i);
  }
  std::vector<const Decl*> out;
  std::unordered_set<int> emitted_set;
  while (!ready.empty()) {
    int n = ready.front();
    ready.erase(ready.begin());
    out.push_back(type_decls[n]);
    emitted_set.insert(n);
    for (int i = 0; i < (int)type_decls.size(); ++i) {
      if (!deps[i].count(n)) continue;
      if (--indeg[i] == 0) ready.push_back(i);
    }
  }
  // Anything left has a cycle among non-pointer aliases. Emit in source
  // order as a fallback -- probably won't compile, but we don't silently
  // drop declarations.
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (!emitted_set.count(i)) out.push_back(type_decls[i]);
  }
  return out;
}

void Emitter::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;  // used for the #include stem
  current_unit_name = ascii_lower(u.name);
  auto saved_local_enums = local_enums;
  auto saved_local_aliases = local_type_aliases_scoped;
  auto seed_unit_type_scope = [&](const std::vector<DeclPtr>& decls) {
    for (const auto& d : decls) {
      if (d->kind != Kind::TypeDecl) continue;
      const auto& td = static_cast<const TypeDecl&>(*d);
      if (!td.type) continue;
      if (td.type->kind == Kind::TyEnum) {
        local_enums[td.name] =
            static_cast<const ast::TyEnum*>(td.type.get());
      } else if (td.type->kind != Kind::TyRecord &&
                 td.type->kind != Kind::TyObject) {
        local_type_aliases_scoped[td.name] = td.type.get();
      }
    }
  };
  if (current_unit_name == "tpexcept") {
    emit_tpexcept_unit(u);
    local_enums = std::move(saved_local_enums);
    local_type_aliases_scoped = std::move(saved_local_aliases);
    return;
  }

  // Header.
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <array>");
  emitln("#include <limits>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  seed_unit_type_scope(u.interface_decls);
  // Emitted headers are prefixed `p_` so the filename never collides with
  // a C/C++ standard header (e.g. Pascal unit `strings` vs libc strings.h).
  for (const auto& uu : u.interface_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  emit_forward_struct_decls(*this, u.interface_decls);
  // Walk source order. Types are reordered topologically only within a
  // single contiguous run (a Pascal `type` section); any intervening
  // const/var/proc breaks the run. This respects Pascal's rule that
  // forward references are only allowed within the same type section.
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/true);
      }
      run.clear();
    };
    for (const auto& d : u.interface_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, true);
      }
    }
    flush();
  }
  // Forward-declare unit lifecycle hooks so program startup can
  // initialize units and register their cleanup.
  if (!u.is_program) {
    nl();
    emitln(std::string("void ") + kUnitInitName + "();");
    emitln(std::string("void ") + kUnitFiniName + "();");
  }
  nl();
  emitln("}  // namespace " + ns);

  // Implementation.
  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_" + hguard + ".h\"");
  seed_unit_type_scope(u.impl_decls);
  for (const auto& uu : u.impl_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  // The program emits a startup call chain over every parsed unit;
  // include all of their headers so the declarations are visible.
  if (u.is_program && unit_init_order) {
    for (const auto& uu : *unit_init_order) {
      if (uu == u.name) continue;
      emitln("#include \"p_" + uu + ".h\"");
    }
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  emit_forward_struct_decls(*this, u.impl_decls);
  // Emit definitions (not just extern declarations) for interface
  // vars in the .cc so external references resolve at link time.
  for (const auto& d : u.interface_decls) {
    if (d->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*d);
      if (vd.is_external) continue;
      emit_decl(*d, /*in_header=*/false);
    }
  }
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/false);
      }
      run.clear();
    };
    for (const auto& d : u.impl_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, false);
      }
    }
    flush();
  }
  auto emit_unit_hook = [&](const char* name, const StmtPtr& body) {
    nl();
    emitln(std::string("void ") + name + "() {");
    indent();
    ++block_depth;
    if (body) emit_stmt(*body);
    --block_depth;
    dedent();
    emitln("}");
  };

  // Emit the unit/program lifecycle bodies.
  if (!u.is_program) {
    emit_unit_hook(kUnitInitName, u.init_body);
    emit_unit_hook(kUnitFiniName, u.final_body);
    nl();
    emitln("}  // namespace " + ns);
  } else {
    nl();
    emitln("}  // namespace " + ns);
    nl();
    emitln("int main(int argc, char** argv) {");
    indent();
    emitln("::rt::init_argv(argc, argv);");
    if (unit_init_order) {
      // Register each finalizer only after its init hook returns.
      // That gives reverse-order teardown on normal exit/Halt and
      // leaves never-finished units out of the finalization chain.
      for (const auto& uu : *unit_init_order) {
        if (uu == u.name) continue;
        std::string ns_name = mangle(uu);
        emitln(ns_name + "::" + kUnitInitName + "();");
        emitln("if (std::atexit(" + ns_name + "::" + kUnitFiniName +
               ") != 0) std::abort();");
      }
    }
    emitln("using namespace " + ns + ";");
    ++block_depth;
    if (u.init_body) emit_stmt(*u.init_body);
    --block_depth;
    emitln("return 0;");
    dedent();
    emitln("}");
  }
  local_enums = std::move(saved_local_enums);
  local_type_aliases_scoped = std::move(saved_local_aliases);
}

void Emitter::emit_tpexcept_unit(const UnitNode& u) {
  (void)u;
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <setjmp.h>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("struct p_jmp_buf {");
  indent();
  emitln("int32_t p_eax;");
  emitln("int32_t p_ebx;");
  emitln("int32_t p_ecx;");
  emitln("int32_t p_edx;");
  emitln("int32_t p_esi;");
  emitln("int32_t p_edi;");
  emitln("int32_t p_ebp;");
  emitln("int32_t p_esp;");
  emitln("int32_t p_eip;");
  emitln("int32_t p_flags;");
  emitln("uint16_t p_cs;");
  emitln("uint16_t p_ds;");
  emitln("uint16_t p_es;");
  emitln("uint16_t p_fs;");
  emitln("uint16_t p_gs;");
  emitln("uint16_t p_ss;");
  dedent();
  emitln("};");
  emitln("using p_pjmp_buf = p_jmp_buf*;");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("struct p_jump_state {");
  indent();
  emitln("::jmp_buf p_env;");
  dedent();
  emitln("};");
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec);");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("int32_t p_setjmp(p_jmp_buf& p_rec) = delete;");
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value);");
  emitln("inline p_pjmp_buf p_recoverpospointer = nullptr;");
  emitln("inline bool p_longjump_used = false;");
  nl();
  emitln(std::string("void ") + kUnitInitName + "();");
  emitln(std::string("void ") + kUnitFiniName + "();");
  nl();
  emitln("}  // namespace p_tpexcept");

  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_tpexcept.h\"");
  emitln("#include <cstdlib>");
  emitln("#include <unordered_map>");
  nl();
  emitln("namespace {");
  indent();
  emitln("std::unordered_map<const p_tpexcept::p_jmp_buf*,");
  emitln("                   p_tpexcept::p_detail::p_jump_state> p_jump_states;");
  dedent();
  emitln("}  // namespace");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec) {");
  indent();
  emitln("return ::p_jump_states[p_rec];");
  dedent();
  emitln("}");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value) {");
  indent();
  emitln("auto it = ::p_jump_states.find(&p_rec);");
  emitln("if (it == ::p_jump_states.end()) std::abort();");
  emitln("p_longjump_used = true;");
  emitln("::longjmp(it->second.p_env, p_return_value == 0 ? 1 : p_return_value);");
  dedent();
  emitln("}");
  nl();
  emitln(std::string("void ") + kUnitInitName + "() {");
  emitln("}");
  nl();
  emitln(std::string("void ") + kUnitFiniName + "() {");
  emitln("}");
  nl();
  emitln("}  // namespace p_tpexcept");
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u, const TypeRegistry* registry,
                      const std::vector<std::string>* unit_init_order) {
  Emitter e(registry, unit_init_order);
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace tp2cc
