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

}  // namespace

EmitStmts::EmitStmts(const TypeRegistry* registry, ScopeStateView& scope,
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
  if (const TypeExpr* t = analysis_.deduce_type(expr);
      t && (t = analysis_.canonicalize_type(t)) &&
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
    if (h.class_name.empty()) {
      stmt_ops_.emitln(opener + " (true) {");
    } else {
      // Pascal `on E: TException do` only matches exception classes, so the
      // translated `dynamic_cast` target must be a pointer type even when the
      // name comes from the `sysutils` stub alias and does not resolve through
      // the normal class registry.
      TyName handler_type(h.class_name);
      std::string handler_cxx = types_.type_to_cxx(handler_type);
      if (handler_cxx.empty() || handler_cxx.back() != '*') {
        handler_cxx += "*";
      }
      stmt_ops_.emitln(opener + " (auto tp2cc_match_" + n + "_" +
                       std::to_string(i) + " = dynamic_cast<" + handler_cxx +
                       ">(" + exc_name + "); tp2cc_match_" + n + "_" +
                       std::to_string(i) + ") {");
    }
    stmt_ops_.indent();
    stmt_ops_.emitln(handled_name + " = true;");
    std::optional<std::string> bound_name;
    std::optional<TyName> bound_type;
    auto saved_locals = scope_.local_scope;
    auto saved_types = scope_.local_types;
    if (!h.var_name.empty()) {
      bound_name = mangle(h.var_name);
      stmt_ops_.emitln("auto " + *bound_name + " = " +
                       (h.class_name.empty()
                            ? exc_name
                            : "tp2cc_match_" + n + "_" + std::to_string(i)) +
                       ";");
      scope_.local_scope.insert(h.var_name);
      bound_type.emplace();
      bound_type->name =
          h.class_name.empty() ? std::string("exception") : h.class_name;
      scope_.local_types[h.var_name] = &*bound_type;
    }
    ++except_handler_depth_;
    if (h.body) emit_stmt(*h.body);
    --except_handler_depth_;
    scope_.local_scope = std::move(saved_locals);
    scope_.local_types = std::move(saved_types);
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

void EmitStmts::emit_assign_stmt(const Assign& a) {
  if (registry_ && a.target->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*a.target);
    std::string cls;
    if (mem.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*mem.base).name == "self") {
      cls = scope_.current_class_name;
    } else {
      cls = analysis_.deduce_class_alias(*mem.base);
    }
    if (!cls.empty()) {
      if (auto* prop = registry_->lookup_class_property(
              cls, mem.name, scope_.current_unit_name)) {
        std::vector<const Expr*> no_indices;
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, stmt_ops_.expr_to_cxx(*mem.base), cls,
                             *prop, no_indices, *a.value) +
                         ";");
        return;
      }
    }
  }
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
    const TypeExpr* target_ty = analysis_.deduce_type(*a.target);
    std::string rhs_cxx = stmt_ops_.const_value_to_cxx(*a.value, target_ty);
    stmt_ops_.emitln(storage_.storage_designator_store(*target, rhs_cxx) + ";");
    return;
  }
  if (registry_ && a.target->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*a.target);
    if (auto text =
            properties_.maybe_lower_implicit_property_write(a.loc, id.name,
                                                            *a.value)) {
      stmt_ops_.emitln(*text + ";");
      return;
    }
  }
  if (registry_ && a.target->kind == Kind::Index) {
    const auto& ix = static_cast<const Index&>(*a.target);
    std::vector<const Expr*> indices;
    for (const auto& idx : ix.indices) indices.push_back(idx.get());
    if (ix.base->kind == Kind::Member) {
      const auto& mem = static_cast<const Member&>(*ix.base);
      std::string cls;
      if (mem.base->kind == Kind::Ident &&
          static_cast<const Ident&>(*mem.base).name == "self") {
        cls = scope_.current_class_name;
      } else {
        cls = analysis_.deduce_class_alias(*mem.base);
      }
      if (!cls.empty()) {
        if (auto* prop = registry_->lookup_class_property(
                cls, mem.name, scope_.current_unit_name)) {
          if (!prop->params.empty()) {
            stmt_ops_.emitln(properties_.lower_property_write(
                                 a.loc, stmt_ops_.expr_to_cxx(*mem.base), cls,
                                 *prop, indices, *a.value) +
                             ";");
            return;
          }
        }
      }
    }
    if (ix.base->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*ix.base);
      if (auto found = analysis_.find_implicit_class_property(id.name);
          found && found->prop && !found->prop->params.empty()) {
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, found->base_cxx, found->class_name,
                             *found->prop, indices, *a.value) +
                         ";");
        return;
      }
    }
    std::string cls = analysis_.deduce_class_alias(*ix.base);
    if (!cls.empty()) {
      if (auto* prop = registry_->lookup_default_property(
              cls, scope_.current_unit_name)) {
        stmt_ops_.emitln(properties_.lower_property_write(
                             a.loc, stmt_ops_.expr_to_cxx(*ix.base), cls,
                             *prop, indices, *a.value) +
                         ";");
        return;
      }
    }
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
  const TypeExpr* target_ty = analysis_.deduce_type(*a.target);
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
    const TypeExpr* tcanon = analysis_.canonicalize_type(target_ty);
    if (tcanon && tcanon->kind == Kind::TyName) {
      const PrimitiveInfo* dst =
          primitive_info(ascii_lower(static_cast<const TyName&>(*tcanon).name));
      if (dst && (dst->int_kind == PrimitiveIntKind::Signed ||
                  dst->int_kind == PrimitiveIntKind::Unsigned)) {
        const TypeExpr* src_ty = overload_types_.type_for_overload(*a.value);
        if (src_ty) src_ty = analysis_.canonicalize_type(src_ty);
        const PrimitiveInfo* src = nullptr;
        bool src_is_real = false;
        if (src_ty && src_ty->kind == Kind::TyName) {
          std::string sn =
              ascii_lower(static_cast<const TyName&>(*src_ty).name);
          src = primitive_info(sn);
          src_is_real = sn == "single" || sn == "double" || sn == "real" ||
                        sn == "extended" || sn == "comp" || sn == "bestreal";
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
  if (target_ty) {
    bool source_is_const_storage = false;
    if (a.value->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*a.value);
      source_is_const_storage =
          scope_.local_untyped_params.count(id.name) &&
          scope_.local_const_params.count(id.name);
    } else if (a.value->kind == Kind::AddrOf) {
      const auto& addr = static_cast<const AddrOf&>(*a.value);
      if (addr.operand && addr.operand->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*addr.operand);
        source_is_const_storage =
            scope_.local_untyped_params.count(id.name) &&
            scope_.local_const_params.count(id.name);
      }
    }
    rhs_cxx = storage_.coerce_pointer_like_text(
        types_.type_to_cxx(*target_ty), target_ty,
        overload_types_.type_for_overload(*a.value), rhs_cxx,
        /*explicit_pascal_cast=*/false, source_is_const_storage);
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
    return;
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
  if (name == "new" && call_expr && !call_expr->args.empty()) {
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
        // Bytewise variant/packed pointer slots are real Pascal storage, but
        // not live C++ pointer objects. Store the allocated pointer value into
        // the slot with byte-copy helpers instead of binding a C++ reference.
        stmt_ops_.emitln("::rt::p_new_slot<" + storage->type_cxx + ">(" +
                         storage->ptr_cxx + ");");
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
      const TypeExpr* ptr_arg_ty = analysis_.deduce_type(*call_expr->args[0]);
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
  if (name == "dispose" && call_expr && !call_expr->args.empty()) {
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
      if (registry_) {
        std::string cls;
        if (mem.base && mem.base->kind == Kind::Ident) {
          const std::string base =
              ascii_lower(static_cast<const Ident&>(*mem.base).name);
          const TypeSymbol* symbol =
              registry_->lookup_type_symbol(base, scope_.current_unit_name);
          if (!symbol || (!symbol->class_info() && !symbol->record_info())) {
            cls = analysis_.deduce_class_alias(*mem.base);
          }
        } else {
          cls = analysis_.deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (const auto* methods = registry_->lookup_class_methods(
                  cls, mem.name, scope_.current_unit_name)) {
            std::vector<const ProcDecl*> candidates;
            for (const auto& method : *methods) {
              if (method.decl) candidates.push_back(method.decl.get());
            }
            PickResult picked = resolution_.pick_overload(candidates, {});
            stmt_autocalls_member = !picked.ambiguous && picked.decl;
          } else if (ascii_lower(mem.name) == "destroy") {
            if (const auto* ci = analysis_.class_info_for_type_name(cls)) {
              stmt_autocalls_member = ci->is_reference_type;
            }
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
  if (es.expr->kind == Kind::Ident && registry_) {
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

  const TypeExpr* selector_type = analysis_.deduce_type(*cs.selector);
  bool scope_inserted = scope_.local_scope.insert(selector).second;
  bool type_inserted = false;
  if (selector_type) {
    scope_.local_types[selector] = selector_type;
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
    scope_.local_types.erase(selector);
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

std::optional<std::string> EmitStmts::for_in_type_rhs_name(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    const std::string low = ascii_lower(id.name);
    ResolveResult rr = resolve_name_provider_.resolve_name(id.name);
    if (rr.kind == ResolvedKind::UnitType) return id.name;
    if (rr.kind == ResolvedKind::Unknown &&
        (is_primitive_type(low) || analysis_.lookup_named_type_expr(id.name))) {
      return id.name;
    }
    return std::nullopt;
  }
  if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (auto resolved = analysis_.resolve_unit_qualified_member(mem);
        resolved && resolved->resolved.kind == ResolvedKind::UnitType) {
      return resolved->unit_name + "." + resolved->member_name;
    }
  }
  return std::nullopt;
}

std::string EmitStmts::for_in_class_type_name(
    const TypeExpr* type, std::string_view owner_class_name) {
  if (!type) return {};
  if (type->kind == Kind::TyName) {
    const std::string name = ascii_lower(static_cast<const TyName&>(*type).name);
    if (!owner_class_name.empty() && name.find('.') == std::string::npos) {
      const std::string nested =
          std::string(owner_class_name) + "." + name;
      if (analysis_.class_info_for_type_name(nested)) return nested;
    }
    if (registry_) {
      const std::string direct =
          registry_->direct_type_name(type, scope_.current_unit_name);
      if (!direct.empty() && analysis_.class_info_for_type_name(direct)) {
        return direct;
      }
    }
    if (analysis_.class_info_for_type_name(name)) return name;
  }
  type = analysis_.canonicalize_type(type);
  if (type && type->kind == Kind::TyName) {
    const std::string name = ascii_lower(static_cast<const TyName&>(*type).name);
    if (analysis_.class_info_for_type_name(name)) return name;
  }
  return {};
}

const MethodSig* EmitStmts::for_in_zero_arg_method(
    Location loc, const std::string& class_name,
    const std::string& method_name) {
  if (!registry_) return nullptr;
  const auto* methods = registry_->lookup_class_methods(
      class_name, method_name, scope_.current_unit_name);
  if (!methods) return nullptr;

  std::vector<const ProcDecl*> candidates;
  for (const auto& method : *methods) {
    if (method.decl) candidates.push_back(method.decl.get());
  }
  PickResult picked = resolution_.pick_overload(candidates, {});
  if (picked.ambiguous) {
    stmt_ops_.report_error(loc, "ambiguous " + method_name + " method");
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
  auto type_name = for_in_type_rhs_name(*f.in_expr);
  if (!type_name) return ForInEmitResult::NotMatched;

  const TypeExpr* named = analysis_.lookup_named_type_expr(*type_name);
  named = analysis_.canonicalize_type(named);
  if (named && named->kind == Kind::TyEnum &&
      types_.enum_has_explicit_values(static_cast<const TyEnum&>(*named))) {
    stmt_ops_.report_error(f.loc,
                           "for-in over non-contiguous enum type is not "
                           "supported");
    return ForInEmitResult::Error;
  }

  std::string low = types_.low_high_expr_for_named_type(*type_name, true);
  std::string high = types_.low_high_expr_for_named_type(*type_name, false);
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
  CallArgumentPlan op_plan =
      calls_.plan_call_arguments(op.decl, nullptr, op_args);
  std::string fn = pascal_operator_decl_name_to_cxx(*op.decl);
  if (!op.defining_unit.empty()) {
    fn = unit_namespace_prefix(op.defining_unit) + fn;
  }
  ForInEnumeratorProvider provider(
      fn + "(" + calls_.lower_call_arg(op_plan.slots[0]) + ")",
      op.decl->return_type.get(), f.loc);
  return emit_for_in_enumerator_provider(f, var, provider);
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_helper_get_enumerator(
    const For& f, const std::string& var) {
  (void)f;
  (void)var;
  // Helpers can add GetEnumerator to a type without changing the type's own
  // declaration. tp2cc does not yet model helper method lookup here, so the
  // dispatch continues to the type's own GetEnumerator and built-in forms.
  return ForInEmitResult::NotMatched;
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_own_get_enumerator(
    const For& f, const std::string& var) {
  if (!f.in_expr) return ForInEmitResult::NotMatched;
  const std::string class_name = analysis_.deduce_class_alias(*f.in_expr);
  if (class_name.empty()) return ForInEmitResult::NotMatched;
  const MethodSig* get =
      for_in_zero_arg_method(f.loc, class_name, "GetEnumerator");
  if (!get) return ForInEmitResult::NotMatched;
  if (!get->is_function || !get->decl || !get->decl->return_type) {
    stmt_ops_.report_error(f.loc, "GetEnumerator must return an enumerator");
    return ForInEmitResult::Error;
  }

  const ClassInfo* ci = analysis_.class_info_for_type_name(class_name);
  std::string access = (ci && ci->is_reference_type) ? "->" : ".";
  ForInEnumeratorProvider provider(
      stmt_ops_.expr_to_cxx(*f.in_expr) + access + mangle(get->decl->name) +
          "()",
      get->decl->return_type.get(), f.loc, class_name);
  return emit_for_in_enumerator_provider(f, var, provider);
}

EmitStmts::ForInEmitResult EmitStmts::emit_for_in_enumerator_provider(
    const For& f, const std::string& var,
    const ForInEnumeratorProvider& provider) {
  const std::string enum_class =
      for_in_class_type_name(provider.type, provider.owner_class_name);
  if (enum_class.empty()) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator provider must return an object or class");
    return ForInEmitResult::Error;
  }

  const MethodSig* move =
      for_in_zero_arg_method(provider.loc, enum_class, "MoveNext");
  if (!move) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator is missing MoveNext");
    return ForInEmitResult::Error;
  }
  const TypeExpr* move_ret = move->decl ? move->decl->return_type.get() : nullptr;
  move_ret = analysis_.canonicalize_type(move_ret);
  if (!move->is_function || !tyname_is(move_ret, "boolean")) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator MoveNext must return Boolean");
    return ForInEmitResult::Error;
  }

  const PropertyInfo* current = registry_->lookup_class_property(
      enum_class, "Current", scope_.current_unit_name);
  if (!current || current->read.empty() || !current->params.empty()) {
    stmt_ops_.report_error(provider.loc,
                           "enumerator is missing readable Current");
    return ForInEmitResult::Error;
  }

  const ClassInfo* enum_info = analysis_.class_info_for_type_name(enum_class);
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
                       provider.loc, enum_var, enum_class, *current,
                       no_indices) +
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
      analysis_.canonicalize_type(analysis_.deduce_type(*f.in_expr));
  bool is_string = in_type && in_type->kind == Kind::TyString;
  if (!is_string && in_type && in_type->kind == Kind::TyName) {
    const std::string name =
        ascii_lower(static_cast<const TyName&>(*in_type).name);
    is_string =
        name == "string" || name == "shortstring" || name == "ansistring";
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
      analysis_.canonicalize_type(analysis_.deduce_type(*f.in_expr));
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
      analysis_.canonicalize_type(analysis_.deduce_type(*f.in_expr));
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

  stmt_ops_.emitln("{");
  stmt_ops_.indent();
  stmt_ops_.emitln("auto " + set + " = (" +
                   stmt_ops_.expr_to_cxx(*f.in_expr) + ");");
  stmt_ops_.emitln(elem_type + " " + item + " = " + low + ";");
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
  stmt_ops_.emitln("if (" + item + " == " + high + ") break;");
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
        const TypeExpr* ty = analysis_.deduce_type(with_expr);
        if (ty) ty = analysis_.canonicalize_type(ty);
        std::string nm =
            "tp2cc_with_" + std::to_string(scope_.with_stack.size());
        auto storage = storage_.storage_designator(with_expr);
        std::string class_name = analysis_.deduce_class_alias(with_expr);
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
              nm, ty, class_name, access_op,
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
        scope_.with_stack.emplace_back(nm, ty, class_name, access_op);
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
