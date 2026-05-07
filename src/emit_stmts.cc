#include "emit_stmts.h"

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

}  // namespace

EmitStmts::EmitStmts(const TypeRegistry* registry, ScopeStateView& scope,
                     int& except_handler_depth, int& try_stmt_counter,
                     int& loop_label_counter,
                     std::vector<std::string>& loop_break_labels,
                     std::vector<std::string>& loop_continue_labels,
                     EmitAnalysis& analysis, EmitTypes& types,
                     EmitStorage& storage,
                     ResolveNameProvider& resolve_name_provider,
                     EmitResolution& resolution, EmitCalls& calls,
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
      TyName handler_type;
      handler_type.name = h.class_name;
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
  auto assignment_operator_rhs =
      [&](const TypeExpr* value_ty,
          const TypeExpr* target_ty) -> std::optional<std::string> {
    if (auto conv = resolution_.find_assignment_operator(value_ty, target_ty);
        conv.decl) {
      std::string fn = pascal_assignment_operator_helper_name(*conv.decl);
      if (!conv.defining_unit.empty()) {
        fn = unit_namespace_prefix(conv.defining_unit) + fn;
      }
      return fn + "(" + stmt_ops_.expr_to_cxx(*a.value) + ")";
    }
    return std::nullopt;
  };

  // Assignment targets are storage contexts in Pascal. Most targets also spell
  // ordinary C++ lvalues, and those must still use the normal assignment path
  // below so shortstrings, range checks, properties, and custom assignment
  // operators keep their existing rules. Intercept only targets whose storage
  // cannot safely be expressed as a plain C++ lvalue, such as untyped storage,
  // packed scalar storage, `unaligned(...)`, or a storage-view typecast.
  if (auto target = storage_.storage_designator(*a.target);
      target && target->is_special()) {
    const TypeExpr* target_ty = analysis_.deduce_type(*a.target);
    const TypeExpr* value_ty = analysis_.deduce_type(*a.value);
    std::string rhs_cxx;
    if (auto converted = assignment_operator_rhs(value_ty, target_ty)) {
      rhs_cxx = *converted;
    } else {
      // Special storage changes only how the destination address is spelled.
      // The RHS is still a Pascal assignment, so constants and user-defined
      // `operator :=` conversions must be lowered exactly as for an ordinary
      // lvalue before the bytewise/reference store receives the value.
      rhs_cxx = stmt_ops_.const_value_to_cxx(*a.value, target_ty);
    }
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
  const TypeExpr* value_ty = analysis_.deduce_type(*a.value);
  if (auto converted = assignment_operator_rhs(value_ty, target_ty)) {
    stmt_ops_.emitln(target_cxx + " = " + *converted + ";");
    return;
  }
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
        const TypeExpr* src_ty = analysis_.deduce_type(*a.value);
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
        types_.type_to_cxx(*target_ty), target_ty, analysis_.deduce_type(*a.value), rhs_cxx,
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
    std::string p = calls_.lower_call_arg(*call_expr->args[0],
                                          /*param_type=*/nullptr,
                                          UntypedArgKind::None,
                                          /*mutable_ref_arg=*/true);
    stmt_ops_.emitln("::rt::p_new(" + p + ");");
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
        const ProcDecl* ctor_decl = nullptr;
        if (registry_ && ptr_arg_ty && cc.callee->kind == Kind::Ident) {
          std::string pointee = registry_->pointer_target_type_name(ptr_arg_ty);
          if (!pointee.empty()) {
            if (auto* m = registry_->lookup_class_method(
                    pointee, static_cast<const Ident&>(*cc.callee).name,
                    scope_.current_unit_name)) {
              ctor_decl = m->decl.get();
            }
          }
        }
        std::vector<UntypedArgKind> untyped_arg(cc.args.size(),
                                                UntypedArgKind::None);
        std::vector<bool> mutable_ref_arg(cc.args.size(), false);
        std::vector<const TypeExpr*> param_types(cc.args.size(), nullptr);
        calls_.mark_call_param_info(ctor_decl, untyped_arg, mutable_ref_arg,
                                    param_types);
        for (size_t i = 0; i < cc.args.size(); ++i) {
          if (i) args += ", ";
          args += calls_.lower_call_arg(*cc.args[i], param_types[i],
                                        untyped_arg[i], mutable_ref_arg[i]);
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
    std::string p = calls_.lower_call_arg(*call_expr->args[0],
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
          if (!registry_->has_class(base, scope_.current_unit_name) &&
              !registry_->records.count(base)) {
            cls = analysis_.deduce_class_alias(*mem.base);
          }
        } else {
          cls = analysis_.deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (const auto* method = registry_->lookup_class_method(
                  cls, mem.name, scope_.current_unit_name)) {
            stmt_autocalls_member = method->accepts_zero_args;
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

std::string EmitStmts::case_selector_expr(const CaseStmt& cs, const Expr& e) {
  const TypeExpr* t = analysis_.deduce_type(*cs.selector);
  bool selector_is_charish = false;
  if (t) {
    t = analysis_.canonicalize_type(t);
    selector_is_charish = tyname_is(t, "char");
  }
  std::string text = stmt_ops_.expr_to_cxx(e);
  return selector_is_charish ? "::rt::p_ord(" + text + ")" : text;
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
      loop_break_labels_.push_back(brk);
      loop_continue_labels_.push_back(cont);
      if (w.body) emit_stmt(*w.body);
      stmt_ops_.emitln(cont + ":;");
      loop_continue_labels_.pop_back();
      loop_break_labels_.pop_back();
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
      loop_break_labels_.push_back(brk);
      loop_continue_labels_.push_back(cont);
      for (const auto& sub : r.body) emit_stmt(*sub);
      stmt_ops_.emitln(cont + ":;");
      loop_continue_labels_.pop_back();
      loop_break_labels_.pop_back();
      stmt_ops_.dedent();
      stmt_ops_.emitln("} while (!(" + stmt_ops_.expr_to_cxx(*r.cond) + "));");
      stmt_ops_.emitln(brk + ":;");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      ResolveResult vr = resolve_name_provider_.resolve_name(f.var);
      std::string var = vr.cxx.empty() ? mangle(f.var) : vr.cxx;
      std::string from = stmt_ops_.expr_to_cxx(*f.from);
      std::string to = stmt_ops_.expr_to_cxx(*f.to);
      std::string n = std::to_string(++loop_label_counter_);
      std::string brk = "tp2cc_loop_break_" + n;
      std::string cont = "tp2cc_loop_continue_" + n;
      // Pascal `for X := A to B do S` is NOT `for (X=A; X<=B; ++X)`:
      // when X's type is `byte` and B is 255, ++X wraps to 0 and the
      // condition never fails. True semantics: body runs for each X in
      // [A,B]; terminate by equality after the body. Snapshot the end
      // bound so mid-body assignments to B don't alter the loop count.
      stmt_ops_.emitln("{");
      stmt_ops_.indent();
      stmt_ops_.emitln("auto tp2cc_from = (" + from + ");");
      stmt_ops_.emitln("auto tp2cc_to = (" + to + ");");
      const char* cmp = f.downto ? ">=" : "<=";
      const char* step = f.downto ? "::rt::p_dec" : "::rt::p_inc";
      stmt_ops_.emitln(std::string("if (tp2cc_from ") + cmp + " tp2cc_to) {");
      stmt_ops_.indent();
      stmt_ops_.emitln(var + " = tp2cc_from;");
      stmt_ops_.emitln("while (true) {");
      stmt_ops_.indent();
      loop_break_labels_.push_back(brk);
      loop_continue_labels_.push_back(cont);
      if (f.body) emit_stmt(*f.body);
      stmt_ops_.emitln(cont + ":;");
      loop_continue_labels_.pop_back();
      loop_break_labels_.pop_back();
      stmt_ops_.emitln("if (" + var + " == tp2cc_to) break;");
      stmt_ops_.emitln(std::string(step) + "(" + var + ");");
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
      stmt_ops_.emitln(brk + ":;");
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      stmt_ops_.emitln("switch (" + case_selector_expr(cs, *cs.selector) + ") {");
      stmt_ops_.indent();
      for (const auto& arm : cs.arms) {
        for (const auto& lab : arm.labels) {
          if (lab->kind == Kind::Range) {
            // GCC case-range extension: `case lo ... hi:`. Acceptable here;
            // the gnu profile compiler supports it. TODO: iterate label
            // values for strict standard C++.
            const auto& r = static_cast<const Range&>(*lab);
            stmt_ops_.emitln("case " + case_selector_expr(cs, *r.lo) + " ... " +
                             case_selector_expr(cs, *r.hi) + ":");
          } else {
            stmt_ops_.emitln("case " + case_selector_expr(cs, *lab) + ":");
          }
        }
        stmt_ops_.indent();
        if (arm.body) emit_stmt(*arm.body);
        stmt_ops_.emitln("break;");
        stmt_ops_.dedent();
      }
      if (cs.else_branch) {
        stmt_ops_.emitln("default:");
        stmt_ops_.indent();
        emit_stmt(*cs.else_branch);
        stmt_ops_.emitln("break;");
        stmt_ops_.dedent();
      }
      stmt_ops_.dedent();
      stmt_ops_.emitln("}");
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
      size_t pushed = 0;
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        const Expr& with_expr = *w.exprs[i];
        const TypeExpr* ty = analysis_.deduce_type(with_expr);
        if (ty) ty = analysis_.canonicalize_type(ty);
        std::string nm =
            "tp2cc_with_" + std::to_string(scope_.with_stack.size());
        std::string init = stmt_ops_.expr_to_cxx(with_expr);
        bool bind_by_ref = storage_.expr_is_storage_lvalue(with_expr);
        // `with T(p) do` and similar casts produce pointer rvalues. Bind those
        // by value; only genuine lvalues can be safely aliased with `auto&`.
        stmt_ops_.emitln(std::string(bind_by_ref ? "auto& " : "auto ") + nm +
                         " = " + init + ";");
        ScopeStateView::WithBind wb;
        wb.cxx_text = nm;
        wb.type = ty;
        wb.class_name = analysis_.deduce_class_alias(with_expr);
        wb.access_op = storage_.member_access_op(with_expr);
        scope_.with_stack.push_back(std::move(wb));
        ++pushed;
      }
      if (w.body) emit_stmt(*w.body);
      for (size_t i = 0; i < pushed; ++i) scope_.with_stack.pop_back();
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
