#include "emit_calls.h"

#include <string>
#include <string_view>
#include <vector>

#include "emit_analysis.h"
#include "emit_resolution.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

struct FlatCallParamInfo {
  const ast::TypeExpr* type = nullptr;
  bool untyped = false;
  bool mutable_ref = false;
  const ast::Expr* default_value = nullptr;
};

void mark_builtin_memory_helper_param_info(
    std::string_view name, std::vector<UntypedArgKind>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const ast::TypeExpr*>& param_types) {
  const std::string lower = ascii_lower(name);

  auto mark = [&](size_t index, UntypedArgKind untyped_kind, bool is_mutable,
                  const ast::TypeExpr* type = nullptr) {
    if (index < untyped_arg.size() &&
        untyped_kind != UntypedArgKind::None) {
      untyped_arg[index] = untyped_kind;
    }
    if (index < mutable_ref_arg.size() && is_mutable) mutable_ref_arg[index] = true;
    if (index < param_types.size()) param_types[index] = type;
  };

  // Pascal's raw memory helpers all operate on caller storage, not on the
  // value of the first expression. Reuse the normal untyped-argument
  // lowering path here so calls like `FillChar(FList^[I], ...)` become
  // `&slot` in C++ instead of reinterpreting the pointer value stored there.
  if (lower == "fillchar" || lower == "fillword") {
    mark(0, UntypedArgKind::Mutable, /*is_mutable=*/true);
    return;
  }
  if (lower == "move") {
    mark(0, UntypedArgKind::Const, /*is_mutable=*/false);
    mark(1, UntypedArgKind::Mutable, /*is_mutable=*/true);
    return;
  }
  if (lower == "getmem" || lower == "freemem" || lower == "reallocmem" ||
      lower == "dispose" || lower == "strdispose") {
    mark(0, UntypedArgKind::None, /*is_mutable=*/true);
  }
  // Pascal `Val(S; var V; var Code)` and `Str(X; var S)` write to caller
  // storage. Mark the var-mode slots so a call-site typecast like
  // `Val(s, aword(result), code)` lowers through `lower_call_arg`'s
  // mutable-ref-cast path -- it rebinds the `result` storage as the
  // typecast's target type, which matches the unsigned `p_val` rt
  // overload. Without this, the cast lowers as a value rvalue and the
  // overload set fails to match.
  if (lower == "val") {
    mark(1, UntypedArgKind::None, /*is_mutable=*/true);
    mark(2, UntypedArgKind::None, /*is_mutable=*/true);
    return;
  }
  if (lower == "str") {
    mark(1, UntypedArgKind::None, /*is_mutable=*/true);
    return;
  }
}

}  // namespace

EmitCalls::EmitCalls(const TypeRegistry* registry, ScopeStateView& scope,
                     EmitAnalysis& analysis, EmitTypes& types,
                     EmitStorage& storage, EmitResolution& resolution,
                     EmitCallExprOps& expr_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      storage_(storage),
      resolution_(resolution),
      expr_ops_(expr_ops) {}

bool EmitCalls::proc_accepts_zero_args(const ProcDecl& decl) {
  for (const auto& p : decl.params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    if (count != 0 && !p.default_value) return false;
  }
  return true;
}

void EmitCalls::mark_call_param_info(
    const ProcDecl* decl, std::vector<UntypedArgKind>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (!decl) return;
  size_t ai = 0;
  for (const auto& p : decl->params) {
    for (size_t k = 0; k < p.names.size(); ++k) {
      if (ai < untyped_arg.size() && !p.type) {
        untyped_arg[ai] =
            (p.mode == Param::Var || p.mode == Param::Out)
                ? UntypedArgKind::Mutable
                : UntypedArgKind::Const;
      }
      if (ai < mutable_ref_arg.size()) {
        mutable_ref_arg[ai] =
            p.mode == Param::Var || p.mode == Param::Out ||
            (p.mode == Param::Const &&
             analysis_.const_param_needs_mutable_ref(p.type.get()));
      }
      if (ai < param_types.size()) param_types[ai] = p.type.get();
      ++ai;
    }
  }
}

void EmitCalls::append_defaulted_trailing_call_args(
    const ProcDecl* decl, std::vector<const Expr*>& args) {
  if (!decl) return;
  std::vector<FlatCallParamInfo> flat_params;
  flat_params.clear();
  for (const auto& p : decl->params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) {
      FlatCallParamInfo info;
      info.type = p.type.get();
      info.untyped = !p.type;
      info.mutable_ref =
          p.mode == Param::Var || p.mode == Param::Out ||
          (p.mode == Param::Const &&
           analysis_.const_param_needs_mutable_ref(p.type.get()));
      info.default_value = p.default_value.get();
      flat_params.push_back(info);
    }
  }
  if (args.size() >= flat_params.size()) return;

  for (size_t i = args.size(); i < flat_params.size(); ++i) {
    if (!flat_params[i].default_value) return;
  }
  args.reserve(flat_params.size());
  for (size_t i = args.size(); i < flat_params.size(); ++i) {
    args.push_back(flat_params[i].default_value);
  }
}

void EmitCalls::collect_builtin_helper_param_info(
    const Expr& callee, std::vector<UntypedArgKind>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (callee.kind == Kind::Ident) {
    mark_builtin_memory_helper_param_info(
        static_cast<const Ident&>(callee).name, untyped_arg, mutable_ref_arg,
        param_types);
    return;
  }
  if (callee.kind != Kind::Member) return;
  const auto& mem = static_cast<const Member&>(callee);
  if (mem.base->kind == Kind::Ident &&
      ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system") {
    mark_builtin_memory_helper_param_info(mem.name, untyped_arg,
                                          mutable_ref_arg, param_types);
  }
}

void EmitCalls::collect_call_param_info(
    const Expr& callee, std::vector<UntypedArgKind>& untyped_arg,
    std::vector<bool>& mutable_ref_arg,
    std::vector<const TypeExpr*>& param_types) {
  collect_builtin_helper_param_info(callee, untyped_arg, mutable_ref_arg,
                                    param_types);
  mark_call_param_info(resolution_.resolve_call_decl(callee), untyped_arg,
                       mutable_ref_arg, param_types);
}

std::string EmitCalls::lower_call_arg(const Expr& arg, const TypeExpr* param_type,
                                      UntypedArgKind untyped_arg,
                                      bool mutable_ref_arg) {
  if (param_type && storage_.type_is_open_array(param_type) &&
      arg.kind == Kind::SetLit) {
    const auto& s = static_cast<const SetLit&>(arg);
    const TypeExpr* canon = analysis_.canonicalize_type(param_type);
    if (!canon || canon->kind != Kind::TyArray) return expr_ops_.expr_to_cxx(s);
    const auto& arr = static_cast<const TyArray&>(*canon);
    const TypeExpr* elem_type = arr.element.get();
    if (!elem_type) return "::rt::tp2cc_open_array<int32_t>()";
    if (s.elements.empty()) {
      return "::rt::tp2cc_open_array<" + types_.type_to_cxx(*elem_type) + ">()";
    }

    // Pascal reuses `[...]` for two different constructs:
    //   * set literals                -> `[a, b]`
    //   * open-array actuals in calls -> `foo([a, b])`
    // Keep the AST simple and decide here from the formal parameter type.
    for (const auto& el : s.elements) {
      if (el->kind == Kind::Range) {
        expr_ops_.report_error(s.loc,
                               "ranges in open-array constructors are unsupported");
        return "::rt::tp2cc_open_array<" + types_.type_to_cxx(*elem_type) + ">()";
      }
    }

    std::string out =
        "::rt::tp2cc_open_array_of<" + types_.type_to_cxx(*elem_type) + ">(";
    for (size_t i = 0; i < s.elements.size(); ++i) {
      if (i) out += ", ";
      out += expr_ops_.const_value_to_cxx(*s.elements[i], elem_type, false);
    }
    out += ")";
    return out;
  }
  const TypeExpr* arg_type = analysis_.deduce_type(arg);
  if (arg_type) arg_type = analysis_.canonicalize_type(arg_type);
  const TypeExpr* canon_param_type = analysis_.canonicalize_type(param_type);
  if (mutable_ref_arg && arg.kind == Kind::Call &&
      static_cast<const Call&>(arg).args.size() == 1 &&
      static_cast<const Call&>(arg).callee->kind == Kind::Ident &&
      (canon_param_type || untyped_arg == UntypedArgKind::None)) {
    const auto& cast = static_cast<const Call&>(arg);
    const auto& id = static_cast<const Ident&>(*cast.callee);
    bool is_type_cast = is_primitive_type(id.name);
    if (!is_type_cast && analysis_.lookup_named_type_expr(id.name)) {
      is_type_cast = true;
    }
    if (is_type_cast && storage_.expr_is_storage_lvalue(*cast.args[0])) {
      std::string ref_type_cxx;
      if (param_type) {
        ref_type_cxx = types_.type_to_cxx(*param_type);
      } else if (is_primitive_type(id.name)) {
        ref_type_cxx = primitive_type_cxx(id.name);
      } else {
        ref_type_cxx = types_.type_name_text_to_cxx(id.name);
      }
      return storage_.reinterpret_ref_text(ref_type_cxx,
                                           expr_ops_.expr_to_cxx(*cast.args[0]),
                                           false);
    }
  }
  if (mutable_ref_arg && canon_param_type && arg_type &&
      storage_.expr_is_storage_lvalue(arg) &&
      storage_.type_is_pointerish(canon_param_type) &&
      storage_.type_is_pointerish(arg_type) &&
      types_.type_to_cxx(*canon_param_type) != types_.type_to_cxx(*arg_type)) {
    return storage_.reinterpret_ref_text(types_.type_to_cxx(*param_type),
                                         expr_ops_.expr_to_cxx(arg), false);
  }
  if (canon_param_type && storage_.type_is_stringish(canon_param_type)) {
    if (mutable_ref_arg && storage_.expr_is_storage_lvalue(arg)) {
      return expr_ops_.expr_to_cxx(arg);
    }
    if (arg_type && storage_.type_is_stringish(arg_type)) {
      return expr_ops_.expr_to_cxx(arg);
    }
    if (storage_.type_is_pcharish(arg_type)) {
      return expr_ops_.const_value_to_cxx(arg, param_type, false);
    }
    if (arg.kind != Kind::StringLit && !storage_.expr_is_charish(arg)) {
      return expr_ops_.expr_to_cxx(arg);
    }
  }
  std::string arg_text = expr_ops_.const_value_to_cxx(arg, param_type, false);
  if (storage_.type_is_open_array(param_type)) {
    const TypeExpr* at = arg_type;
    if (!storage_.type_is_open_array(at)) {
      const TypeExpr* canon_open = analysis_.canonicalize_type(param_type);
      const auto& arr = static_cast<const TyArray&>(*canon_open);
      const TypeExpr* elem_type = arr.element ? arr.element.get() : nullptr;
      std::string elem_cxx =
          elem_type ? types_.type_to_cxx(*elem_type) : std::string("int32_t");
      arg_text = "::rt::tp2cc_open_array<" + elem_cxx + ">(" + arg_text + ")";
    }
  }
  if (untyped_arg == UntypedArgKind::None) return arg_text;

  // Untyped Pascal params are already lowered as "pointer to caller storage".
  // Forwarding one of them must preserve the pointer value; taking `&` here
  // would pass the address of the local pointer slot instead.
  if (arg.kind == Kind::AddrOf &&
      !static_cast<const AddrOf&>(arg).double_addr) {
    return "((void*)(" + arg_text + "))";
  }
  if (arg.kind == Kind::Ident &&
      scope_.local_untyped_params.count(static_cast<const Ident&>(arg).name)) {
    return arg_text;
  }
  if (untyped_arg == UntypedArgKind::Const &&
      !mutable_ref_arg && !storage_.expr_is_storage_lvalue(arg)) {
    return "::rt::tp2cc_const_untyped_ptr(" + arg_text + ")";
  }
  return "((void*)&(" + arg_text + "))";
}

std::string EmitCalls::lower_implicit_zero_arg_call(
    const std::string& callee_text, const ProcDecl* decl) {
  if (!decl) return callee_text + "()";

  std::vector<const Expr*> args;
  append_defaulted_trailing_call_args(decl, args);
  if (args.empty()) return callee_text + "()";

  std::vector<UntypedArgKind> untyped_arg(args.size(), UntypedArgKind::None);
  std::vector<bool> mutable_ref_arg(args.size(), false);
  std::vector<const TypeExpr*> param_types(args.size(), nullptr);
  mark_call_param_info(decl, untyped_arg, mutable_ref_arg, param_types);

  std::string out = callee_text + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) out += ", ";
    out += lower_call_arg(*args[i], param_types[i], untyped_arg[i],
                          mutable_ref_arg[i]);
  }
  out += ")";
  return out;
}

std::optional<std::string> EmitCalls::maybe_lower_class_free_member(
    const Expr& base, std::string_view member_name) {
  if (member_name != "free" || !storage_.expr_is_reference_class(base)) {
    return std::nullopt;
  }
  return "::rt::p_tobject::p_free(" + expr_ops_.expr_to_cxx(base) + ")";
}

std::optional<std::string> EmitCalls::maybe_lower_class_constructor_call(
    std::string_view class_name, std::string_view member_name,
    const std::vector<const Expr*>& args,
    const std::vector<const TypeExpr*>& param_types,
    const std::vector<UntypedArgKind>& untyped_arg,
    const std::vector<bool>& mutable_ref_arg) {
  if (!registry_) return std::nullopt;
  auto cit = registry_->classes.find(std::string(class_name));
  if (cit == registry_->classes.end() || !cit->second.is_reference_type) {
    return std::nullopt;
  }
  const auto* method =
      registry_->lookup_class_method(std::string(class_name),
                                     std::string(member_name));
  bool implicit_root_create = false;
  if (!method || method->kind != SymKind::Constructor) {
    if (ascii_lower(std::string(member_name)) != "create" || !args.empty()) {
      return std::nullopt;
    }
    implicit_root_create = true;
  }

  std::vector<const Expr*> effective_args(args.begin(), args.end());
  std::vector<const TypeExpr*> effective_param_types(param_types.begin(),
                                                     param_types.end());
  std::vector<UntypedArgKind> effective_untyped_arg(untyped_arg.begin(),
                                                    untyped_arg.end());
  std::vector<bool> effective_mutable_ref_arg(mutable_ref_arg.begin(),
                                              mutable_ref_arg.end());
  append_defaulted_trailing_call_args(method ? method->decl.get() : nullptr,
                                      effective_args);
  if (effective_param_types.size() < effective_args.size()) {
    effective_param_types.resize(effective_args.size(), nullptr);
    effective_untyped_arg.resize(effective_args.size(), UntypedArgKind::None);
    effective_mutable_ref_arg.resize(effective_args.size(), false);
    mark_call_param_info(method ? method->decl.get() : nullptr,
                         effective_untyped_arg, effective_mutable_ref_arg,
                         effective_param_types);
  }

  // Pascal constructor calls on a class value (`TNode.Create`) allocate a
  // fresh instance and then run the constructor body on that instance. They
  // are not plain static method calls, even though the emitted C++ helper
  // itself lives on the struct type.
  std::string args_cxx;
  for (size_t i = 0; i < effective_args.size(); ++i) {
    if (i) args_cxx += ", ";
    args_cxx += lower_call_arg(*effective_args[i], effective_param_types[i],
                               effective_untyped_arg[i],
                               effective_mutable_ref_arg[i]);
  }
  std::string struct_ty = types_.named_type_struct_cxx(class_name);
  return "([&]{ auto tp2cc_ptr = new " + struct_ty + "{}; tp2cc_ptr->" +
         (implicit_root_create ? std::string("p_create")
                               : mangle(std::string(member_name))) +
         "(" + args_cxx +
         "); return tp2cc_ptr; }())";
}

}  // namespace tp2cc
