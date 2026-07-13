#include "emit_stmts.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "emit_analysis.h"
#include "emit_calls.h"
#include "emit_properties.h"
#include "emit_resolution.h"
#include "emit_resolution_types.h"
#include "emit_signature_scope.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

constexpr const char* kCtorStatusSlotName = "tp2cc_ctor_ok";

// `break` and `continue` resolve to the innermost loop currently being
// emitted. Recursive statement emission therefore needs a scoped label stack
// entry around exactly the Pascal loop body.
class LoopLabelScope {
 public:
  LoopLabelScope(std::vector<std::string>& break_labels,
                 std::vector<std::string>& continue_labels,
                 const std::string& break_label,
                 const std::string& continue_label)
      : break_labels_(break_labels), continue_labels_(continue_labels) {
    break_labels_.push_back(break_label);
    continue_labels_.push_back(continue_label);
  }

  LoopLabelScope(const LoopLabelScope&) = delete;
  LoopLabelScope& operator=(const LoopLabelScope&) = delete;

  ~LoopLabelScope() {
    continue_labels_.pop_back();
    break_labels_.pop_back();
  }

 private:
  std::vector<std::string>& break_labels_;
  std::vector<std::string>& continue_labels_;
};

// A `with` statement opens temporary member lookup bases while its body is
// emitted. Restoring by saved depth keeps nested `with` statements independent
// of how many receiver expressions the outer statement contains.
class WithStackScope {
 public:
  explicit WithStackScope(std::vector<ScopeStateView::WithBind>& stack)
      : stack_(stack), size_(stack.size()) {}

  WithStackScope(const WithStackScope&) = delete;
  WithStackScope& operator=(const WithStackScope&) = delete;

  ~WithStackScope() {
    while (stack_.size() > size_) stack_.pop_back();
  }

 private:
  std::vector<ScopeStateView::WithBind>& stack_;
  size_t size_;
};

ExprPtr borrowed_expr_ptr(const Expr& expr) {
  // Binary lowering needs ExprPtr operands, but case-label lowering compares
  // existing selector/label nodes without taking ownership of either subtree.
  return ExprPtr(const_cast<Expr*>(&expr), [](Expr*) {});
}

bool unqualified_builtin_name_is_shadowed(const Call* call_expr,
                                          std::string_view name,
                                          ResolveNameProvider& resolver) {
  if (!call_expr || !call_expr->callee ||
      call_expr->callee->kind != Kind::Ident) {
    return false;
  }
  ResolveResult resolved = resolver.resolve_name(std::string(name));
  // `new` has type/storage syntax and is not represented as an ordinary
  // runtime proc, so unresolved means "still eligible for builtin lowering".
  // `dispose` normally resolves through the implicit runtime unit. Any closer
  // source declaration must win over the builtin statement form.
  return resolved.kind != ResolvedKind::Unknown &&
         resolved.kind != ResolvedKind::RtBuiltin;
}

}  // namespace

EmitStmts::EmitStmts(const TypeRegistry& registry, ScopeStateView& scope,
                     int& except_handler_depth, int& try_stmt_counter,
                     int& loop_label_counter,
                     std::vector<std::string>& loop_break_labels,
                     std::vector<std::string>& loop_continue_labels,
                     EmitAnalysis& analysis, EmitTypes& types,
                     EmitStorage& storage,
                     ResolveNameProvider& resolve_name_provider,
                     EmitResolution& resolution,
                     OverloadTypeProvider& overload_types, EmitCalls& calls,
                     EmitProperties& properties,
                     EmitStmtOps& stmt_ops)
    : registry_(registry),
      scope_(scope),
      except_handler_depth_(except_handler_depth),
      try_stmt_counter_(try_stmt_counter),
      loop_label_counter_(loop_label_counter),
      loop_break_labels_(loop_break_labels),
      loop_continue_labels_(loop_continue_labels),
      analysis_(analysis),
      types_(types),
      storage_(storage),
      resolve_name_provider_(resolve_name_provider),
      resolution_(resolution),
      overload_types_(overload_types),
      calls_(calls),
      properties_(properties),
      stmt_ops_(stmt_ops) {}

const TypeExpr* EmitStmts::selected_value_type(const Expr& expr) {
  // Query a value expression after overload/operator selection. Use this only
  // for statement dispatch that depends on the produced value type. Assignment
  // targets need lvalue storage identity for writes, var/out checks, range
  // checks, and byte-addressed packed/variant slots. `with` receivers need the
  // bound receiver's storage/member-access form, not only the type of a value
  // produced by an expression.
  return overload_types_.type_for_overload(expr);
}

const TypeExpr* EmitStmts::assignment_target_type(const Expr& expr) {
  // Assignment target typing is a storage query. It must describe the Pascal
  // slot being written so range checks, shortstring capacity, and custom
  // assignment see the target storage type, not merely a selected value result.
  return analysis_.deduce_type(expr);
}

const TypeExpr* EmitStmts::new_pointer_slot_type(const Expr& expr) {
  // `new(p, ctor)` needs the declared pointer slot type to find the pointee
  // constructor surface after allocating through the caller's storage.
  return analysis_.deduce_type(expr);
}

const TypeExpr* EmitStmts::procedural_designator_type(const Expr& expr) {
  // Statement-form procvar autocall asks whether the designator itself is a
  // parameterless procedural value. Do not substitute a generic value-result
  // query here; assignments of the same expression must stay plain values.
  return analysis_.deduce_type(expr);
}

const TypeExpr* EmitStmts::with_receiver_type(const Expr& expr) {
  // `with` binds a receiver environment. The type must match the bound storage
  // or receiver expression used for member lookup inside the block.
  return analysis_.deduce_type(expr);
}

const TypeSymbol* EmitStmts::value_class_symbol(const Expr& expr) {
  if (expr.kind == Kind::Ident &&
      static_cast<const Ident&>(expr).name == "self") {
    return scope_.current_class_symbol;
  }
  const bool produced_value =
      expr.kind == Kind::Call || expr.kind == Kind::Binary ||
      expr.kind == Kind::Unary;
  if (!produced_value) {
    if (const TypeSymbol* symbol = analysis_.deduce_class_symbol(expr)) {
      return symbol;
    }
  }
  if (const TypeExpr* t = selected_value_type(expr)) {
    if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
      return target;
    }
    if (const TypeSymbol* symbol = analysis_.class_symbol_for_type(t)) {
      return symbol;
    }
    if (const TypeExpr* canon = analysis_.semantic_shape_type(t)) {
      if (const TypeSymbol* target =
              registry_.metaclass_target_for_type(canon)) {
        return target;
      }
      if (const TypeSymbol* symbol = analysis_.class_symbol_for_type(canon)) {
        return symbol;
      }
    }
  }
  return analysis_.deduce_class_symbol(expr);
}

const ClassInfo* EmitStmts::value_class_info(const Expr& expr) {
  const TypeSymbol* symbol = value_class_symbol(expr);
  return symbol ? symbol->class_info() : nullptr;
}

std::string EmitStmts::value_receiver_access_op(const Expr& expr) {
  if (const TypeExpr* t = selected_value_type(expr)) {
    if (storage_.type_is_reference_class(t) || analysis_.type_is_interface(t)) {
      return "->";
    }
  }
  return storage_.member_access_op(expr);
}

bool EmitStmts::stmt_autocalls_procvar(const Expr& expr) {
  switch (expr.kind) {
    case Kind::Ident:
    case Kind::Member:
    case Kind::Index:
    case Kind::Deref:
      break;
    default:
      return false;
  }
  if (const TypeExpr* t = procedural_designator_type(expr);
      t && (t = analysis_.semantic_shape_type(t)) &&
      t->kind == Kind::TyProcedural) {
    return procedural_param_count(static_cast<const TyProcedural&>(*t)) == 0;
  }
  return false;
}

void EmitStmts::emit_raise_stmt(const Raise& r) {
  if (r.value) {
    stmt_ops_.emitln("throw " + stmt_ops_.expr_to_cxx(*r.value) + ";");
    return;
  }
  if (except_handler_depth_ == 0) {
    stmt_ops_.report_error(r.loc,
                           "bare raise is only valid inside an except handler");
  }
  stmt_ops_.emitln("throw;");
}

void EmitStmts::emit_try_stmt(const Try& t) {
  const std::string n = std::to_string(++try_stmt_counter_);

  if (t.is_finally) {
    stmt_ops_.emitln("{");
    stmt_ops_.indent();
    // Pascal `finally` runs on every exit path from the block. Model that
    // with a C++ scope guard so `Exit`, loop control, and exception unwinding
    // all funnel through one emitted finally-body.
    stmt_ops_.emitln("auto tp2cc_finally_" + n +
                     " = ::rt::tp2cc_make_scope_exit([&]() {");
    stmt_ops_.indent();
    for (const auto& sub : t.finally_body) emit_stmt(*sub);
    stmt_ops_.dedent();
    stmt_ops_.emitln("});");
    for (const auto& sub : t.body) emit_stmt(*sub);
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
    return;
  }

  if (t.handlers.empty()) {
    stmt_ops_.emitln("try {");
    stmt_ops_.indent();
    for (const auto& sub : t.body) emit_stmt(*sub);
    stmt_ops_.dedent();
    stmt_ops_.emitln("} catch (...) {");
    stmt_ops_.indent();
    ++except_handler_depth_;
    if (t.except_else) emit_stmt(*t.except_else);
    --except_handler_depth_;
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
    return;
  }

  const std::string exc_name = "tp2cc_exc_" + n;
  const std::string handled_name = "tp2cc_handled_" + n;
  stmt_ops_.emitln("try {");
  stmt_ops_.indent();
  for (const auto& sub : t.body) emit_stmt(*sub);
  stmt_ops_.dedent();
  stmt_ops_.emitln("} catch (::rt::t_exception* " + exc_name + ") {");
  stmt_ops_.indent();
  stmt_ops_.emitln("bool " + handled_name + " = false;");
  for (size_t i = 0; i < t.handlers.size(); ++i) {
    const auto& h = t.handlers[i];
    std::string opener = (i == 0) ? "if" : "else if";
    const TypeSymbol* handler_symbol = nullptr;
    const TypeExpr* handler_type = nullptr;
    std::optional<const TypeSymbol*> handler_result =
        registry_.exception_handler_type_result(&h);
    if (!handler_result) {
      stmt_ops_.report_error(t.loc,
                             "exception handler type was not resolved during "
                             "build");
    } else if ((handler_symbol = *handler_result)) {
      handler_type = handler_symbol->type;
    }
    if (h.class_name.empty()) {
      stmt_ops_.emitln(opener + " (true) {");
    } else {
      std::string handler_cxx = types_.type_symbol_to_cxx(handler_symbol);
      if (handler_cxx.empty()) {
        handler_cxx = "::rt::t_exception*";
      }
      stmt_ops_.emitln(opener + " (auto tp2cc_match_" + n + "_" +
                       std::to_string(i) + " = dynamic_cast<" + handler_cxx +
                       ">(" + exc_name + "); tp2cc_match_" + n + "_" +
                       std::to_string(i) + ") {");
    }
    stmt_ops_.indent();
    stmt_ops_.emitln(handled_name + " = true;");
    std::optional<std::string> bound_name;
    auto saved_locals = scope_.local_scope;
    auto saved_types = scope_.local_value_types;
    if (!h.var_name.empty()) {
      bound_name = mangle(h.var_name);
      stmt_ops_.emitln("auto " + *bound_name + " = " +
                       (h.class_name.empty()
                            ? exc_name
                           : "tp2cc_match_" + n + "_" + std::to_string(i)) +
                       ";");
      scope_.local_scope.insert(h.var_name);
      scope_.local_value_types[h.var_name] = handler_type;
    }
    ++except_handler_depth_;
    if (h.body) emit_stmt(*h.body);
    --except_handler_depth_;
    scope_.local_scope = std::move(saved_locals);
    scope_.local_value_types = std::move(saved_types);
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
  }
  if (t.except_else) {
    stmt_ops_.emitln("else {");
    stmt_ops_.indent();
    stmt_ops_.emitln(handled_name + " = true;");
    ++except_handler_depth_;
    emit_stmt(*t.except_else);
    --except_handler_depth_;
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
  }
  stmt_ops_.emitln("if (!" + handled_name + ") throw;");
  stmt_ops_.dedent();
  if (t.except_else) {
    stmt_ops_.emitln("} catch (...) {");
    stmt_ops_.indent();
    ++except_handler_depth_;
    emit_stmt(*t.except_else);
    --except_handler_depth_;
    stmt_ops_.dedent();
  }
  stmt_ops_.emitln("}");
}

bool EmitStmts::emit_property_assign_stmt(const Assign& a) {

  if (a.target->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*a.target);
    const TypeSymbol* cls_symbol = value_class_symbol(*mem.base);
    const ClassInfo* cls_info = cls_symbol ? cls_symbol->class_info() : nullptr;
    if (cls_info) {
      if (auto* prop = registry_.lookup_class_property(*cls_info, mem.name)) {
        std::vector<const Expr*> no_indices;
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, stmt_ops_.expr_to_cxx(*mem.base),
                             value_receiver_access_op(*mem.base), *prop,
                             no_indices, *a.value) +
                         ";");
        return true;
      }
    }
  }
  if (a.target->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*a.target);
    if (auto text =
            properties_.maybe_lower_implicit_property_write(a.loc, id.name,
                                                            *a.value)) {
      stmt_ops_.emitln(*text + ";");
      return true;
    }
  }
  if (a.target->kind == Kind::Index) {
    const auto& ix = static_cast<const Index&>(*a.target);
    std::vector<const Expr*> indices;
    for (const auto& idx : ix.indices) indices.push_back(idx.get());
    if (ix.base->kind == Kind::Member) {
      const auto& mem = static_cast<const Member&>(*ix.base);
      const TypeSymbol* cls_symbol = value_class_symbol(*mem.base);
      const ClassInfo* cls_info =
          cls_symbol ? cls_symbol->class_info() : nullptr;
      if (cls_info) {
        if (auto* prop = registry_.lookup_class_property(*cls_info, mem.name)) {
          if (!prop->params.empty()) {
            stmt_ops_.emitln(properties_.lower_property_write(
                                 a.loc, stmt_ops_.expr_to_cxx(*mem.base),
                                 value_receiver_access_op(*mem.base), *prop,
                                 indices, *a.value) +
                             ";");
            return true;
          }
        }
      }
    }
    if (ix.base->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*ix.base);
      if (auto found = analysis_.find_implicit_class_property(id.name);
          found && found->prop && !found->prop->params.empty()) {
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, found->base_cxx, found->base_access,
                             *found->prop, indices, *a.value) +
                         ";");
        return true;
      }
    }
    const TypeSymbol* cls_symbol = value_class_symbol(*ix.base);
    const ClassInfo* cls_info = cls_symbol ? cls_symbol->class_info() : nullptr;
    if (cls_info) {
      if (auto* prop = registry_.lookup_default_property(*cls_info)) {
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, stmt_ops_.expr_to_cxx(*ix.base),
                             value_receiver_access_op(*ix.base), *prop,
                             indices, *a.value) +
                         ";");
        return true;
      }
    }
  }
  return false;
}

void EmitStmts::emit_assign_stmt(const Assign& a) {
  if (emit_property_assign_stmt(a)) return;

  if (auto use = storage_.packed_aggregate_path_use(*a.target)) {
    storage_.report_packed_aggregate_subobject_use(a.loc, "assignment", *use);
    return;
  }
  // Assignment targets are storage contexts in Pascal. Most targets also spell
  // ordinary C++ lvalues, and those must still use the normal assignment path
  // below so shortstrings, range checks, properties, and custom assignment
  // operators keep their existing rules. Intercept only targets whose storage
  // cannot safely be expressed as a plain C++ lvalue, such as untyped storage,
  // packed scalar storage, variant-record payload storage, `unaligned(...)`, or
  // a storage-view typecast.
  if (auto target = storage_.storage_designator(*a.target);
      target && target->is_special()) {
    const TypeExpr* target_ty = assignment_target_type(*a.target);
    std::string rhs_cxx = stmt_ops_.const_value_to_cxx(*a.value, target_ty);
    stmt_ops_.emitln(storage_.storage_designator_store(*target, rhs_cxx) + ";");
    return;
  }
  // Enable LHS-rewrite for the function name so that Pascal
  // `funcname := x`, `funcname[i] := x`, `funcname.field := x` etc.
  // all route to the result slot. We only scope the rewrite to the
  // target emission so the RHS still sees the function for recursive
  // calls.
  scope_.lhs_fn_rewrite = scope_.current_fn_is_function ? scope_.current_fn_name
                                                        : "";
  scope_.lhs_fn_rewrite_slot =
      scope_.current_fn_is_function ? scope_.current_result_slot_name : "";
  scope_.lhs_outer_result_rewrite = scope_.outer_result_name;
  scope_.lhs_outer_result_rewrite_slot = scope_.outer_result_slot_name;
  bool saved_suppress_packed_scalar_value_load =
      scope_.suppress_packed_scalar_value_load;
  scope_.suppress_packed_scalar_value_load = true;
  bool saved_storage_view_context = scope_.storage_view_context;
  scope_.storage_view_context = true;
  std::string target_cxx = stmt_ops_.expr_to_cxx(*a.target);
  scope_.storage_view_context = saved_storage_view_context;
  scope_.suppress_packed_scalar_value_load =
      saved_suppress_packed_scalar_value_load;
  scope_.lhs_fn_rewrite.clear();
  scope_.lhs_fn_rewrite_slot.clear();
  scope_.lhs_outer_result_rewrite.clear();
  scope_.lhs_outer_result_rewrite_slot.clear();
  const TypeExpr* target_ty = assignment_target_type(*a.target);
  std::string rhs_cxx = stmt_ops_.const_value_to_cxx(*a.value, target_ty);
  if (target_ty && types_.shortstring_capacity_to_cxx(target_ty)) {
    stmt_ops_.emitln("::rt::tp2cc_shortstring_assign(" + target_cxx + ", " +
                     rhs_cxx + ");");
    return;
  }
  // Narrowing-assignment lowering. With `{$R+}`, route through
  // tp2cc_range_check_assign which raises p_erangeerror when the
  // source can't be represented. With `{$R-}`, real->int still
  // needs a truncating helper because plain `(int)real` is UB in
  // C++ when out of range; integer->integer narrowing is fine
  // (modular truncation is well-defined on unsigned, and gcc
  // implements two's-complement on signed).
  if (target_ty) {
    const TypeExpr* tcanon = analysis_.semantic_shape_type(target_ty);
    const PrimitiveInfo* dst = analysis_.primitive_info_for_type(tcanon);
    if (dst && dst->int_kind != PrimitiveIntKind::None) {
      const TypeExpr* src_ty = overload_types_.type_for_overload(*a.value);
      if (src_ty) src_ty = analysis_.semantic_shape_type(src_ty);
      const PrimitiveInfo* src = analysis_.primitive_info_for_type(src_ty);
      bool src_is_real = src && src->is_real();
      if (a.r_check) {
        bool wrap = false;
        if (src_is_real) {
          wrap = true;
        } else if (src && (src->int_kind != PrimitiveIntKind::None)) {
          if (analysis_.resolved_primitive_bits(*src) >
                  analysis_.resolved_primitive_bits(*dst) ||
              src->int_kind != dst->int_kind) {
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
  stmt_ops_.emitln(target_cxx + " = " + rhs_cxx + ";");
}

void EmitStmts::emit_expr_stmt(const ExprStmt& es) {
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
    } else if (call_expr->callee->kind == Kind::Member) {
      const auto& mem = static_cast<const Member&>(*call_expr->callee);
      if (mem.base->kind == Kind::Ident &&
          ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system") {
        name = mem.name;
      }
    }
  }

  if (call_expr && ascii_lower(name) == "prefetch") {
    std::vector<const Expr*> call_args;
    call_args.reserve(call_expr->args.size());
    for (const auto& arg : call_expr->args) call_args.push_back(arg.get());
    ResolvedCall resolved_prefetch =
        resolution_.resolve_call(*call_expr->callee, call_args);
    // `Prefetch` is an optimization hint only for the implicit runtime helper.
    // A source declaration with the same spelling is an ordinary Pascal call
    // and must keep flowing through normal expression-statement lowering.
    if (resolved_prefetch.proc_info &&
        resolved_prefetch.proc_info->defining_unit == "__rt__" &&
        ascii_lower(resolved_prefetch.member_name) == "prefetch") {
      return;
    }
  }
  if (name == "break") {
    // Pascal break exits the enclosing loop even from inside a case.
    // Emit as goto so switch nesting can't swallow it.
    if (!loop_break_labels_.empty()) {
      stmt_ops_.emitln("goto " + loop_break_labels_.back() + ";");
    } else {
      stmt_ops_.emitln("break;");
    }
    return;
  }
  if (name == "continue") {
    if (!loop_continue_labels_.empty()) {
      stmt_ops_.emitln("goto " + loop_continue_labels_.back() + ";");
    } else {
      stmt_ops_.emitln("continue;");
    }
    return;
  }
  if (name == "exit") {
    // exit or exit(v). In a Function, fill the result slot and return;
    // in a Procedure, return; in a Constructor, return the status.
    if (call_expr && !call_expr->args.empty() && scope_.current_fn_is_function) {
      stmt_ops_.emitln(scope_.current_result_slot_name + " = " +
                       stmt_ops_.const_value_to_cxx(*call_expr->args[0],
                                                    scope_.current_fn_result_type) +
                       ";");
      stmt_ops_.emitln(std::string("return ") + scope_.current_result_slot_name +
                       ";");
    } else if (scope_.current_fn_is_function || scope_.current_fn_is_ctor) {
      stmt_ops_.emitln(std::string("return ") +
                       (scope_.current_fn_is_function
                            ? scope_.current_result_slot_name
                            : std::string(kCtorStatusSlotName)) +
                       ";");
    } else {
      stmt_ops_.emitln("return;");
    }
    return;
  }
  if (name == "fail") {
    if (scope_.current_fn_is_ctor) {
      stmt_ops_.emitln(std::string(kCtorStatusSlotName) + " = false;");
      stmt_ops_.emitln("return " + std::string(kCtorStatusSlotName) + ";");
    } else {
      stmt_ops_.report_error(es.loc, "`fail` outside constructors is unsupported");
    }
    return;
  }
  if (name == "new" && call_expr && !call_expr->args.empty() &&
      !unqualified_builtin_name_is_shadowed(call_expr, name,
                                            resolve_name_provider_)) {
    // new(p) or new(p, Ctor(args)). `p` might be `arr[i]` whose
    // `decltype` is a reference (`T&`); strip it before computing
    // the pointee so `new remove_pointer_t<T&>` doesn't arise. Route
    // statement-form Pascal `new` through the runtime helper rather than
    // raw C++ `new`, so later `reallocmem` / `dispose` on the same typed
    // storage stays in one allocation family.
    std::string p;
    if (auto storage = storage_.storage_designator(*call_expr->args[0]);
        storage && storage->is_bytewise()) {
      if (storage->type_cxx.empty()) {
        stmt_ops_.report_error(call_expr->args[0]->loc,
                               "new requires a typed pointer slot");
        p = stmt_ops_.expr_to_cxx(*call_expr->args[0]);
      } else {
        stmt_ops_.emitln("::rt::p_new(" +
                         storage_.storage_designator_typed_lvalue(
                             *storage) +
                         ");");
        p = "::rt::tp2cc_reinterpret_load<" + storage->type_cxx + ">(" +
            storage->ptr_cxx + ")";
      }
    } else {
      p = calls_.lower_call_arg(*call_expr->args[0],
                                /*param_type=*/nullptr,
                                UntypedArgKind::None,
                                /*mutable_ref_arg=*/true);
      stmt_ops_.emitln("::rt::p_new(" + p + ");");
    }
    if (call_expr->args.size() >= 2) {
      const auto& second = *call_expr->args[1];
      std::string method;
      const TypeExpr* ptr_arg_ty =
          new_pointer_slot_type(*call_expr->args[0]);
      std::string args;
      if (second.kind == Kind::Call) {
        const auto& cc = static_cast<const Call&>(second);
        if (cc.callee->kind == Kind::Ident) {
          method = mangle(static_cast<const Ident&>(*cc.callee).name);
        }
        std::vector<const Expr*> ctor_args;
        ctor_args.reserve(cc.args.size());
        for (const auto& arg : cc.args) ctor_args.push_back(arg.get());
        ResolvedCall ctor_resolved =
            resolution_.resolve_pointer_target_constructor(
                ptr_arg_ty, *cc.callee, ctor_args);
        CallArgumentPlan ctor_plan =
            calls_.plan_call_arguments(ctor_resolved.decl, cc.callee.get(),
                                       ctor_args,
                                       ctor_resolved.default_arg_unit);
        for (size_t i = 0; i < ctor_plan.slots.size(); ++i) {
          if (i) args += ", ";
          args += calls_.lower_call_arg(ctor_plan.slots[i],
                                        ctor_plan.default_arg_unit);
        }
      } else if (second.kind == Kind::Ident) {
        method = mangle(static_cast<const Ident&>(second).name);
      }
      if (!method.empty()) {
        stmt_ops_.emitln("(*" + p + ")." + method + "(" + args + ");");
      }
    }
    return;
  }
  if (name == "dispose" && call_expr && !call_expr->args.empty() &&
      !unqualified_builtin_name_is_shadowed(call_expr, name,
                                            resolve_name_provider_)) {
    // dispose(p) or dispose(p, Done)
    std::string p = stmt_ops_.expr_to_cxx(*call_expr->args[0]);
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
      if (!method.empty()) stmt_ops_.emitln("(*" + p + ")." + method + "();");
    }
    stmt_ops_.emitln("::rt::p_dispose(" + p + ");");
    return;
  }
  if (es.expr->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*es.expr);
    if (auto free_call = calls_.maybe_lower_class_free_member(*mem.base, mem.name)) {
      stmt_ops_.emitln(*free_call + ";");
    } else {
      std::string text = stmt_ops_.expr_to_cxx(*es.expr);
      bool stmt_autocalls_member = false;
      {
        const TypeSymbol* cls_symbol = nullptr;
        if (mem.base && mem.base->kind == Kind::Ident) {
          if (!analysis_.class_or_record_type_value_symbol(*mem.base)) {
            cls_symbol = value_class_symbol(*mem.base);
          }
        } else {
          cls_symbol = value_class_symbol(*mem.base);
        }
        const ClassInfo* cls_info =
            cls_symbol ? cls_symbol->class_info() : nullptr;
        if (cls_info) {
          if (const auto* methods =
                  registry_.lookup_class_methods(*cls_info, mem.name)) {
            PickResult picked = resolution_.pick_method_overload(*methods, {});
            stmt_autocalls_member = !picked.ambiguous && picked.decl;
          } else if (ascii_lower(mem.name) == "destroy") {
            stmt_autocalls_member = cls_info->is_reference_type;
          }
        }
      }
      if ((stmt_autocalls_member || stmt_autocalls_procvar(*es.expr)) &&
          (text.empty() || text.back() != ')')) {
        text += "()";
      }
      stmt_ops_.emitln(text + ";");
    }
    return;
  }

  // `expr_to_cxx` auto-calls parameterless procs/methods in value context via
  // `resolve_name`. The one extra case we handle here: Pascal statement-form
  // `writeln;` / `readln;` / `halt;` -- rt variadic builtins where 0 args is a
  // legitimate call. We don't try to auto-call parameterful callables -- those
  // are either emitted as `Call` (handled above with args) or they're real
  // source bugs that should be fixed in the Pascal, not papered over.
  std::string text = stmt_ops_.expr_to_cxx(*es.expr);
  if (es.expr->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*es.expr);
    ResolveResult rr = resolve_name_provider_.resolve_name(id.name);
    if (rr.accepts_zero_args && !text.empty() && text.back() != ')') {
      text += "()";
    }
  }
  // Parameterless procedural variables are callable in statement
  // position (`olddo_stop;`) but must stay as plain values in assignments
  // like `do_stop := olddo_stop;`. Detect that only here.
  if (stmt_autocalls_procvar(*es.expr)) text += "()";
  stmt_ops_.emitln(text + ";");
}

std::string EmitStmts::case_selector_expr(const Expr& e) {
  std::string text = stmt_ops_.expr_to_cxx(e);
  return text;
}

std::string EmitStmts::case_binary_condition(const ExprPtr& selector_expr,
                                             BinOp op, const Expr& rhs) {
  std::string text = stmt_ops_.expr_to_cxx(
      Binary(selector_expr->loc, op, selector_expr, borrowed_expr_ptr(rhs),
             false));
  if (text.empty()) return "false";
  return text;
}

std::string EmitStmts::case_label_condition(const ExprPtr& selector_expr,
                                            const Expr& label) {
  if (label.kind != Kind::Range) {
    return case_binary_condition(selector_expr, BinOp::Eq, label);
  }
  const auto& r = static_cast<const Range&>(label);
  return "(" + case_binary_condition(selector_expr, BinOp::GtEq, *r.lo) +
         " && " + case_binary_condition(selector_expr, BinOp::LtEq, *r.hi) +
         ")";
}

std::string EmitStmts::case_arm_condition(const ExprPtr& selector_expr,
                                         const CaseArm& arm) {
  std::vector<std::string> parts;
  for (const auto& lab : arm.labels) {
    parts.push_back(case_label_condition(selector_expr, *lab));
  }
  if (parts.empty()) return "false";
  std::string out = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) out += " || " + parts[i];
  return out;
}

void EmitStmts::emit_case_stmt(const CaseStmt& cs) {
  const std::string selector_id = "tp2cc_case_" +
                                 std::to_string(++case_stmt_counter_);
  std::string selector = selector_id;
  while (scope_.local_scope.count(selector) > 0) {
    selector += "_";
  }

  const TypeExpr* selector_type = selected_value_type(*cs.selector);
  bool scope_inserted = scope_.local_scope.insert(selector).second;
  bool type_inserted = false;
  if (selector_type) {
    scope_.local_value_types[selector] = selector_type;
    type_inserted = true;
  }

  const ExprPtr selector_expr =
      std::make_shared<Ident>(cs.loc, selector);

  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto " + mangle(selector) + " = " +
                   case_selector_expr(*cs.selector) + ";");
  bool first = true;
  for (const auto& arm : cs.arms) {
    stmt_ops_.emitln(std::string(first ? "if" : "else if") + " (" +
                     case_arm_condition(selector_expr, arm) + ") {");
    stmt_ops_.indent();
    if (arm.body) emit_stmt(*arm.body);
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
    first = false;
  }
  if (cs.else_branch) {
    if (first) {
      emit_stmt(*cs.else_branch);
    } else {
      stmt_ops_.emitln("else {");
      stmt_ops_.indent();
      emit_stmt(*cs.else_branch);
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
    }
  }
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");

  if (type_inserted) {
    scope_.local_value_types.erase(selector);
  }
  if (scope_inserted) {
    scope_.local_scope.erase(selector);
  }
}

void EmitStmts::emit_ordinal_for_body(const For& f, const std::string& var,
                                      const std::string& from,
                                      const std::string& to, bool downto) {
  std::string n = std::to_string(++loop_label_counter_);
  std::string brk = "tp2cc_loop_break_" + n;
  std::string cont = "tp2cc_loop_continue_" + n;
  const char* cmp = downto ? ">=" : "<=";
  const char* step = downto ? "::rt::p_dec" : "::rt::p_inc";
  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto tp2cc_from = (" + from + ");");
  stmt_ops_.emitln("auto tp2cc_to = (" + to + ");");
  stmt_ops_.emitln(std::string("if (tp2cc_from ") + cmp + " tp2cc_to) {");
  stmt_ops_.indent();
  stmt_ops_.emitln(var + " = tp2cc_from;");
  stmt_ops_.emitln("while (true) {");
  stmt_ops_.indent();
  {
    LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk, cont);
    if (f.body) emit_stmt(*f.body);
    stmt_ops_.emitln(cont + ":;");
  }
  stmt_ops_.emitln("if (" + var + " == tp2cc_to) break;");
  stmt_ops_.emitln(std::string(step) + "(" + var + ");");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln(brk + ":;");
}

std::optional<EmitStmts::ForInTypeRhs> EmitStmts::for_in_type_rhs(
    const Expr& e) {
  if (std::optional<const TypeSymbol*> symbol =
          registry_.value_type_expression_result(&e)) {
    if (*symbol) return ForInTypeRhs{.symbol = *symbol};
  }
  return std::nullopt;
}

const TypeSymbol* EmitStmts::for_in_class_symbol(const TypeExpr* type) {
  return analysis_.class_symbol_for_type(type);
}

const MethodSig* EmitStmts::for_in_zero_arg_method(
    Location loc, const TypeSymbol& class_symbol,
    std::string_view method_key, std::string_view display_name) {
  const ClassInfo* class_info = class_symbol.class_info();
  const auto* methods =
      class_info
          ? registry_.lookup_class_methods(*class_info,
                                           std::string(method_key))
                 : nullptr;
  if (!methods) return nullptr;

  PickResult picked = resolution_.pick_method_overload(*methods, {});
  if (picked.ambiguous) {
    stmt_ops_.report_error(loc, "ambiguous " + std::string(display_name) +
                                    " method");
    return nullptr;
  }
  if (!picked.decl) return nullptr;
  for (const auto& method : *methods) {
    if (method.decl.get() == picked.decl) return &method;
  }
  return nullptr;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_type_rhs(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  auto type_rhs = for_in_type_rhs(*f.in_expr);
  if (!type_rhs) return ForInEmitResult::NotMatched;

  const TypeSymbol* symbol =
      type_rhs->symbol && type_rhs->symbol->descriptor &&
              type_rhs->symbol->descriptor->symbol
          ? type_rhs->symbol->descriptor->symbol
          : type_rhs->symbol;
  const TypeExpr* named =
      symbol && symbol->descriptor ? symbol->descriptor->type : nullptr;
  named = analysis_.semantic_shape_type(named);
  if (named && named->kind == Kind::TyEnum &&
      types_.enum_has_explicit_values(static_cast<const TyEnum&>(*named))) {
    stmt_ops_.report_error(f.loc,
                           "for-in over non-contiguous enum type is not "
                           "supported");
    return ForInEmitResult::Error;
  }

  std::string low = types_.low_high_expr_for_type_symbol(type_rhs->symbol,
                                                         true);
  std::string high = types_.low_high_expr_for_type_symbol(type_rhs->symbol,
                                                          false);
  if (low.empty() || high.empty()) {
    stmt_ops_.report_error(f.loc, "cannot determine bounds for for-in type");
    return ForInEmitResult::Error;
  }
  emit_ordinal_for_body(f, var, low, high, false);
  return ForInEmitResult::Emitted;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_operator_enumerator(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  UnaryOperatorResult op =
      resolution_.find_unary_operator("enumerator", *f.in_expr);
  if (op.ambiguous) {
    stmt_ops_.report_error(f.loc, "ambiguous operator enumerator");
    return ForInEmitResult::Error;
  }
  if (!op.decl) return ForInEmitResult::NotMatched;

  std::vector<const Expr*> op_args{f.in_expr.get()};
  CallArgumentPlan op_plan = calls_.plan_call_arguments(
      op.decl, nullptr, op_args, op.defining_unit);
  std::string fn =
      pascal_operator_decl_name_to_cxx(registry_, *op.decl);
  if (!op.defining_unit.empty()) {
    fn = unit_namespace_prefix(op.defining_unit) + fn;
  }
  ForInEnumeratorProvider provider(
      fn + "(" + calls_.lower_call_arg(op_plan.slots[0]) + ")",
      op.decl->return_type.get(), f.loc);
  return emit_for_in_enumerator_provider(f, var, provider);
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_helper_get_enumerator(
    const For&, const std::string&) {
  // Helpers can add GetEnumerator to a type without changing the type's own
  // declaration. tp2cc does not yet model helper method lookup here, so the
  // dispatch continues to the type's own GetEnumerator and built-in forms.
  return ForInEmitResult::NotMatched;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_own_get_enumerator(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  const TypeSymbol* class_symbol = value_class_symbol(*f.in_expr);
  if (!class_symbol || !class_symbol->class_info()) {
    return ForInEmitResult::NotMatched;
  }
  const MethodSig* get =
      for_in_zero_arg_method(f.loc, *class_symbol, "getenumerator",
                             "GetEnumerator");
  if (!get) return ForInEmitResult::NotMatched;
  if (!get->is_function || !get->decl || !get->decl->return_type) {
    stmt_ops_.report_error(f.loc, "GetEnumerator must return an enumerator");
    return ForInEmitResult::Error;
  }

  ForInEnumeratorProvider provider(
      stmt_ops_.expr_to_cxx(*f.in_expr) + value_receiver_access_op(*f.in_expr) +
          mangle(get->decl->name) + "()",
      get->decl->return_type.get(), f.loc);
  provider.method = get;
  return emit_for_in_enumerator_provider(f, var, provider);
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_enumerator_provider(
    const For& f, const std::string& var,
    const ForInEnumeratorProvider& provider) {
  const TypeSymbol* enum_symbol = for_in_class_symbol(provider.type);
  const ClassInfo* enum_info = enum_symbol ? enum_symbol->class_info() : nullptr;
  if (!enum_info) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator provider must return an object or class");
    return ForInEmitResult::Error;
  }

  const MethodSig* move =
      for_in_zero_arg_method(provider.loc, *enum_symbol, "movenext",
                             "MoveNext");
  if (!move) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator is missing MoveNext");
    return ForInEmitResult::Error;
  }
  const TypeExpr* move_ret = move->decl ? move->decl->return_type.get() : nullptr;
  const PrimitiveInfo* move_primitive =
      analysis_.primitive_info_for_type(move_ret);
  if (!move->is_function || !move_primitive ||
      move_primitive->kind != PrimitiveKind::Boolean) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator MoveNext must return Boolean");
    return ForInEmitResult::Error;
  }

  const PropertyInfo* current =
      registry_.lookup_class_property(*enum_info, "current");
  if (!current || current->read.empty() || !current->params.empty()) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator is missing readable Current");
    return ForInEmitResult::Error;
  }

  const bool enum_is_reference = enum_info && enum_info->is_reference_type;
  const std::string access = enum_is_reference ? "->" : ".";
  const std::string n = std::to_string(++loop_label_counter_);
  const std::string enum_var = "tp2cc_enum_" + n;
  const std::string brk = "tp2cc_loop_break_" + n;
  const std::string cont = "tp2cc_loop_continue_" + n;

  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto " + enum_var + " = " + provider.value_cxx + ";");
  if (enum_is_reference) {
    stmt_ops_.emitln("if (" + enum_var + " != nullptr) {");
    stmt_ops_.indent();
  }
  stmt_ops_.emitln("while (" + enum_var + access + mangle(move->decl->name) +
                   "()) {");
  stmt_ops_.indent();
  std::vector<const Expr*> no_indices;
  stmt_ops_.emitln(var + " = " +
                   properties_.lower_property_read(
                       provider.loc, enum_var, access, *current, no_indices) +
                   ";");
  {
    LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk, cont);
    if (f.body) emit_stmt(*f.body);
    stmt_ops_.emitln(cont + ":;");
  }
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  if (enum_is_reference) {
    stmt_ops_.dedent();
    stmt_ops_.emitln("}");
  }
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln(brk + ":;");
  return ForInEmitResult::Emitted;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_builtin_string(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  const TypeExpr* in_type =
      analysis_.semantic_shape_type(selected_value_type(*f.in_expr));
  bool is_string = in_type && in_type->kind == Kind::TyString;
  if (!is_string && in_type) {
    is_string = analysis_.type_is_string_like(in_type);
  }
  if (!is_string) return ForInEmitResult::NotMatched;

  std::string n = std::to_string(++loop_label_counter_);
  std::string brk = "tp2cc_loop_break_" + n;
  std::string cont = "tp2cc_loop_continue_" + n;
  std::string str = "tp2cc_string_" + n;
  std::string idx = "tp2cc_index_" + n;
  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto " + str + " = (" + stmt_ops_.expr_to_cxx(*f.in_expr) +
                   ");");
  stmt_ops_.emitln("int32_t " + idx + " = 1;");
  stmt_ops_.emitln("int32_t tp2cc_high_" + n + " = ::rt::p_length(" + str +
                   ");");
  stmt_ops_.emitln("if (" + idx + " <= tp2cc_high_" + n + ") {");
  stmt_ops_.indent();
  stmt_ops_.emitln("while (true) {");
  stmt_ops_.indent();
  stmt_ops_.emitln(var + " = " + str + "[" + idx + "];");
  {
    LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk, cont);
    if (f.body) emit_stmt(*f.body);
    stmt_ops_.emitln(cont + ":;");
  }
  stmt_ops_.emitln("if (" + idx + " == tp2cc_high_" + n + ") break;");
  stmt_ops_.emitln("::rt::p_inc(" + idx + ");");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln(brk + ":;");
  return ForInEmitResult::Emitted;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_builtin_array(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  const TypeExpr* in_type =
      analysis_.semantic_shape_type(selected_value_type(*f.in_expr));
  if (!(in_type && in_type->kind == Kind::TyArray)) {
    return ForInEmitResult::NotMatched;
  }
  const auto& arr_type = static_cast<const TyArray&>(*in_type);
  std::string n = std::to_string(++loop_label_counter_);
  std::string brk = "tp2cc_loop_break_" + n;
  std::string cont = "tp2cc_loop_continue_" + n;
  std::string arr = "tp2cc_array_" + n;
  std::string idx = "tp2cc_index_" + n;
  std::string low;
  std::string high;
  if (arr_type.array_kind == ArrayKind::Fixed) {
    low = types_.low_high_expr_for_type(in_type, true);
    high = types_.low_high_expr_for_type(in_type, false);
    if (low.empty() || high.empty()) {
      stmt_ops_.report_error(f.loc, "cannot determine bounds for array for-in");
      return ForInEmitResult::Error;
    }
  } else {
    // Open and dynamic arrays have no declared Pascal index type to preserve;
    // FPC for-in walks their zero-based runtime storage.
    low = "0";
    high = "::rt::p_length(" + arr + ") - 1";
  }

  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto&& " + arr + " = (" +
                   stmt_ops_.expr_to_cxx(*f.in_expr) + ");");
  stmt_ops_.emitln("auto " + idx + " = (" + low + ");");
  stmt_ops_.emitln("auto tp2cc_high_" + n + " = (" + high + ");");
  stmt_ops_.emitln("if (" + idx + " <= tp2cc_high_" + n + ") {");
  stmt_ops_.indent();
  stmt_ops_.emitln("while (true) {");
  stmt_ops_.indent();
  stmt_ops_.emitln(var + " = " + arr + "[" + idx + "];");
  {
    LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk, cont);
    if (f.body) emit_stmt(*f.body);
    stmt_ops_.emitln(cont + ":;");
  }
  stmt_ops_.emitln("if (" + idx + " == tp2cc_high_" + n + ") break;");
  stmt_ops_.emitln("::rt::p_inc(" + idx + ");");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln(brk + ":;");
  return ForInEmitResult::Emitted;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_builtin_set(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  const TypeExpr* in_type =
      analysis_.semantic_shape_type(selected_value_type(*f.in_expr));
  if (!(in_type && in_type->kind == Kind::TySet)) {
    return ForInEmitResult::NotMatched;
  }

  const auto& set_type = static_cast<const TySet&>(*in_type);
  std::string low;
  std::string high;
  if (set_type.has_explicit_bounds) {
    low = std::to_string(set_type.explicit_low);
    high = std::to_string(set_type.explicit_high);
  } else {
    low = types_.low_high_expr_for_type(set_type.element.get(), true);
    high = types_.low_high_expr_for_type(set_type.element.get(), false);
  }
  if (low.empty() || high.empty()) {
    stmt_ops_.report_error(f.loc, "cannot determine bounds for set for-in loop");
    return ForInEmitResult::Error;
  }

  std::string n = std::to_string(++loop_label_counter_);
  std::string brk = "tp2cc_loop_break_" + n;
  std::string cont = "tp2cc_loop_continue_" + n;
  std::string set = "tp2cc_set_" + n;
  std::string item = "tp2cc_item_" + n;
  std::string elem_type = types_.type_to_cxx(*set_type.element);
  std::string low_value = "static_cast<" + elem_type + ">(" + low + ")";
  std::string high_value = "static_cast<" + elem_type + ">(" + high + ")";

  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto " + set + " = (" +
                   stmt_ops_.expr_to_cxx(*f.in_expr) + ");");
  // Set literals remember numeric ordinal bounds, but the loop item has the
  // Pascal element type. Cast the bounds here so enum-set iteration does not
  // depend on C++ accepting raw integers as enum values.
  stmt_ops_.emitln(elem_type + " " + item + " = " + low_value + ";");
  stmt_ops_.emitln("while (true) {");
  stmt_ops_.indent();
  stmt_ops_.emitln("if (" + set + ".contains(" + item + ")) {");
  stmt_ops_.indent();
  stmt_ops_.emitln(var + " = " + item + ";");
  {
    LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk, cont);
    if (f.body) emit_stmt(*f.body);
    stmt_ops_.emitln(cont + ":;");
  }
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln("if (" + item + " == " + high_value + ") break;");
  stmt_ops_.emitln("::rt::p_inc(" + item + ");");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.dedent();
  stmt_ops_.emitln("}");
  stmt_ops_.emitln(brk + ":;");
  return ForInEmitResult::Emitted;
}

void EmitStmts::emit_for_in_stmt(const For& f, const std::string& var) {
  // Keep this order aligned with Pascal for-in lookup: type iteration is not an
  // expression lookup, expression enumerators precede built-in carriers, and a
  // successful provider owns the MoveNext/Current contract.
  if (emit_for_in_type_rhs(f, var) != ForInEmitResult::NotMatched) return;
  if (emit_for_in_operator_enumerator(f, var) != ForInEmitResult::NotMatched) {
    return;
  }
  if (emit_for_in_helper_get_enumerator(f, var) !=
      ForInEmitResult::NotMatched) {
    return;
  }
  if (emit_for_in_own_get_enumerator(f, var) != ForInEmitResult::NotMatched) {
    return;
  }
  if (emit_for_in_builtin_string(f, var) != ForInEmitResult::NotMatched) return;
  if (emit_for_in_builtin_array(f, var) != ForInEmitResult::NotMatched) return;
  if (emit_for_in_builtin_set(f, var) != ForInEmitResult::NotMatched) return;
  stmt_ops_.report_error(f.loc, "for-in expression type is not supported");
}

void EmitStmts::emit_stmt(const Stmt& s) {
  switch (s.kind) {
    case Kind::Compound: {
      const auto& c = static_cast<const Compound&>(s);
      stmt_ops_.emitln("{");
      stmt_ops_.indent();
      for (const auto& sub : c.body) emit_stmt(*sub);
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      break;
    }
    case Kind::EmptyStmt:
      stmt_ops_.emitln(";");
      break;
    case Kind::Assign:
      emit_assign_stmt(static_cast<const Assign&>(s));
      break;
    case Kind::ExprStmt:
      emit_expr_stmt(static_cast<const ExprStmt&>(s));
      break;
    case Kind::If: {
      const auto& i = static_cast<const If&>(s);
      stmt_ops_.emitln("if (" + stmt_ops_.expr_to_cxx(*i.cond) + ") {");
      stmt_ops_.indent();
      if (i.then_branch) emit_stmt(*i.then_branch);
      stmt_ops_.dedent();
      if (i.else_branch) {
        stmt_ops_.emitln("} else {");
        stmt_ops_.indent();
        emit_stmt(*i.else_branch);
        stmt_ops_.dedent();
      }
      stmt_ops_.emitln("}");
      break;
    }
    case Kind::While: {
      const auto& w = static_cast<const While&>(s);
      std::string n = std::to_string(++loop_label_counter_);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      stmt_ops_.emitln("while (" + stmt_ops_.expr_to_cxx(*w.cond) + ") {");
      stmt_ops_.indent();
      {
        LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk,
                              cont);
        if (w.body) emit_stmt(*w.body);
        stmt_ops_.emitln(cont + ":;");
      }
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      stmt_ops_.emitln(brk + ":;");
      break;
    }
    case Kind::Repeat: {
      const auto& r = static_cast<const Repeat&>(s);
      std::string n = std::to_string(++loop_label_counter_);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      stmt_ops_.emitln("do {");
      stmt_ops_.indent();
      {
        LoopLabelScope labels(loop_break_labels_, loop_continue_labels_, brk,
                              cont);
        for (const auto& sub : r.body) emit_stmt(*sub);
        stmt_ops_.emitln(cont + ":;");
      }
      stmt_ops_.dedent();
      stmt_ops_.emitln("} while (!(" + stmt_ops_.expr_to_cxx(*r.cond) + "));");
      stmt_ops_.emitln(brk + ":;");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      ResolveResult vr = resolve_name_provider_.resolve_name(f.var);
      std::string var = vr.cxx.empty() ? mangle(f.var) : vr.cxx;
      if (f.for_in) {
        emit_for_in_stmt(f, var);
        break;
      }

      std::string from = stmt_ops_.expr_to_cxx(*f.from);
      std::string to = stmt_ops_.expr_to_cxx(*f.to);
      emit_ordinal_for_body(f, var, from, to, f.downto);
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      emit_case_stmt(cs);
      break;
    }
    case Kind::With: {
      // Pascal `with A, B do S` opens A's and B's fields (and methods)
      // as unqualified names inside S. We alias each target and push its
      // deduced type onto `with_stack`; bare idents inside S that match a
      // field of any stacked type are rewritten by the expression emitter
      // to that alias.
      const auto& w = static_cast<const With&>(s);
      stmt_ops_.emitln("{");
      stmt_ops_.indent();
      WithStackScope with_scope(scope_.with_stack);
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        const Expr& with_expr = *w.exprs[i];
        const TypeExpr* ty = with_receiver_type(with_expr);
        if (ty) ty = analysis_.semantic_shape_type(ty);
        std::string nm =
            "tp2cc_with_" + std::to_string(scope_.with_stack.size());
        auto storage = storage_.storage_designator(with_expr);
        const TypeSymbol* class_symbol = analysis_.class_symbol_for_type(ty);
        std::string access_op = storage_.member_access_op(with_expr);
        const bool reference_receiver =
            storage_.type_is_reference_class(ty) ||
            analysis_.type_is_interface(ty);
        if (storage && storage->is_bytewise() && !reference_receiver &&
            !storage->type_cxx.empty()) {
          stmt_ops_.emitln("auto " + nm + " = " +
                           storage_.storage_designator_raw_address(*storage) +
                           ";");
          scope_.with_stack.emplace_back(
              nm, ty, class_symbol, access_op,
              ScopeStateView::WithBind::BytewiseStorage(
                  nm, storage->type_cxx,
                  storage->access == EmitStorageAccess::UnalignedBytewise,
                  ScopeStateView::WithBind::BytewiseStorage::FieldSelection::
                      AllFields));
          continue;
        }
        std::string init = stmt_ops_.expr_to_cxx(with_expr);
        bool bind_by_ref =
            storage_.expr_is_storage_lvalue(with_expr) &&
            !(storage && storage->is_bytewise());
        // `with T(p) do` and similar casts produce pointer rvalues. Bind those
        // by value; byte-addressed storage also loads a value, not a C++ field
        // lvalue. Only genuine lvalues can be safely aliased with `auto&`.
        stmt_ops_.emitln(std::string(bind_by_ref ? "auto& " : "auto ") + nm +
                         " = " + init + ";");
        scope_.with_stack.emplace_back(nm, ty, class_symbol, access_op);
        if (storage && !reference_receiver && !storage->type_cxx.empty()) {
          // A normal `with rec do` binding may later expose a Pascal variant
          // payload field. Those payload fields need byte-offset selection
          // even though ordinary fields on the same binding stay direct.
          scope_.with_stack.back().bytewise_storage =
              ScopeStateView::WithBind::BytewiseStorage(
                  storage_.storage_designator_raw_address(*storage),
                  storage->type_cxx,
                  storage->access == EmitStorageAccess::UnalignedBytewise,
                  ScopeStateView::WithBind::BytewiseStorage::FieldSelection::
                      VariantPayloadFieldsOnly);
        }
      }
      if (w.body) emit_stmt(*w.body);
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      break;
    }
    case Kind::Goto: {
      const auto& g = static_cast<const Goto&>(s);
      stmt_ops_.emitln("goto p_" + g.label + ";");
      break;
    }
    case Kind::Labeled: {
      const auto& lb = static_cast<const Labeled&>(s);
      stmt_ops_.emitln("p_" + lb.label + ":");
      if (lb.body) emit_stmt(*lb.body);
      break;
    }
    case Kind::AsmStmt:
      stmt_ops_.report_error(s.loc, "asm blocks are unsupported");
      stmt_ops_.emitln("/* unsupported asm */");
      break;
    case Kind::Raise:
      emit_raise_stmt(static_cast<const Raise&>(s));
      break;
    case Kind::Try:
      emit_try_stmt(static_cast<const Try&>(s));
      break;
    default:
      stmt_ops_.report_error(
          s.loc, "unsupported statement kind " +
                     std::to_string(static_cast<int>(s.kind)));
      stmt_ops_.emitln("/* unsupported-stmt kind=" +
                       std::to_string(static_cast<int>(s.kind)) + " */;");
      break;
  }
}

}  // namespace tp2cc
