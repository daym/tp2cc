#include "emit_calls.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag.h"
#include "emit_analysis.h"
#include "emit_resolution.h"
#include "emit_signature_scope.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

struct PackedScalarValueLoadSuppressor {
  // Var/out/untyped formals need caller storage, not the value stored there.
  // Suppress packed scalar value loads while their actuals are being lowered
  // so `r.sub.x` still reaches the packed-aggregate rejection path instead of
  // becoming an address of a temporary load expression.
  bool& flag;
  bool saved;

  PackedScalarValueLoadSuppressor(bool& flag_in, bool enable)
      : flag(flag_in), saved(flag_in) {
    if (enable) flag = true;
  }

  ~PackedScalarValueLoadSuppressor() { flag = saved; }
};

struct DefaultArgumentUnitScope {
  // A default argument expression belongs to the declaration that introduced
  // it, even though tp2cc lowers it while emitting a later call site.
  std::string& current_unit;
  std::string& emission_unit;
  std::string saved_current_unit;
  std::string saved_emission_unit;
  bool active = false;

  DefaultArgumentUnitScope(std::string& current_unit_in,
                           std::string& emission_unit_in,
                           std::string_view default_arg_unit)
      : current_unit(current_unit_in), emission_unit(emission_unit_in) {
    if (!default_arg_unit.empty() && default_arg_unit != current_unit) {
      saved_current_unit = current_unit;
      saved_emission_unit = emission_unit;
      emission_unit = current_unit;
      current_unit = std::string(default_arg_unit);
      active = true;
    }
  }

  ~DefaultArgumentUnitScope() {
    if (active) {
      current_unit = saved_current_unit;
      emission_unit = saved_emission_unit;
    }
  }
};

std::string visible_type_unit_from(std::string_view type_name,
                                   std::string_view unit_name,
                                   const TypeRegistry* registry) {
  if (!registry) return {};
  if (const TypeSymbol* symbol =
          registry->lookup_type_symbol(type_name, unit_name)) {
    return symbol->defining_unit;
  }
  return {};
}

std::shared_ptr<TyName> qualified_signature_type_name(
    const TypeRegistry* registry, ScopeStateView& scope,
    const TypeExpr* param_type, std::string_view param_unit,
    std::string_view param_declaring_type) {
  if (!param_type || param_type->kind != Kind::TyName) return nullptr;
  const auto& tn = static_cast<const TyName&>(*param_type);
  if (tn.name == "nil" || is_primitive_type(tn.name) ||
      !runtime_named_type_cxx(tn.name).empty()) {
    return nullptr;
  }
  ScopedSignatureLookupUnit signature_scope(scope, registry, param_unit,
                                            param_declaring_type);
  const TypeSymbol* symbol =
      signature_type_symbol_for(registry, scope, tn.name);
  if (!symbol || symbol->defining_unit.empty() ||
      symbol->defining_unit == "__rt__") {
    return nullptr;
  }
  auto qualified = std::make_shared<TyName>(tn);
  qualified->name = type_symbol_unit_pascal_path(*symbol);
  return qualified;
}

struct EffectiveParamType {
  const TypeExpr* type = nullptr;
  std::shared_ptr<TyName> signature_qualified;
  std::shared_ptr<TyName> default_qualified;
};

EffectiveParamType effective_call_param_type(
    const TypeRegistry* registry, ScopeStateView& scope,
    const TypeExpr* param_type, std::string_view param_unit,
    std::string_view param_declaring_type, std::string_view default_arg_unit) {
  EffectiveParamType out;
  out.type = param_type;
  out.signature_qualified = qualified_signature_type_name(
      registry, scope, param_type, param_unit, param_declaring_type);
  if (out.signature_qualified) out.type = out.signature_qualified.get();

  if (!default_arg_unit.empty() && default_arg_unit != scope.current_unit_name &&
      out.type && out.type->kind == Kind::TyName) {
    const auto& tn = static_cast<const TyName&>(*out.type);
    if (tn.name.find('.') == std::string::npos &&
        !is_primitive_type(tn.name) && tn.name != "nil" &&
        runtime_named_type_cxx(tn.name).empty()) {
      if (std::string unit =
              visible_type_unit_from(tn.name, default_arg_unit, registry);
          !unit.empty()) {
        // Default argument expressions are resolved as if they were still in
        // the declaring unit. The generated argument text is inserted at the
        // caller, so a named formal type from that declaration must keep its
        // defining unit in the emitted C++ type name.
        out.default_qualified = std::make_shared<TyName>(tn);
        out.default_qualified->name = unit + "." + tn.name;
        out.type = out.default_qualified.get();
      }
    }
  }
  return out;
}

}  // namespace

void EmitCalls::mark_call_slot(std::vector<CallArgumentSlot>& slots,
                               std::size_t index,
                               UntypedArgKind untyped_kind, bool is_mutable,
                               const ast::TypeExpr* type) {
  // Builtin memory helpers are registered as runtime routines, but several of
  // their Pascal arguments still mean caller storage. Store that fact in the
  // same slot table used for parsed formal parameters.
  if (index >= slots.size()) return;
  CallArgumentSlot& slot = slots[index];
  if (untyped_kind != UntypedArgKind::None) {
    slot.untyped_arg = untyped_kind;
  }
  if (is_mutable) slot.mutable_ref_arg = true;
  slot.param_type = type;
}

std::vector<CallArgumentSlot>
EmitCalls::call_slots_with_builtin_memory_helper_info(
    std::string_view name, std::vector<CallArgumentSlot> slots) {
  const std::string lower = ascii_lower(name);

  // Pascal's raw memory helpers all operate on caller storage, not on the
  // value of the first expression. Reuse the normal untyped-argument
  // lowering path here so calls like `FillChar(FList^[I], ...)` become
  // `&slot` in C++ instead of reinterpreting the pointer value stored there.
  if (lower == "fillchar" || lower == "fillbyte" ||
      lower == "fillword" || lower == "filldword") {
    mark_call_slot(slots, 0, UntypedArgKind::Mutable, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "initialize") {
    mark_call_slot(slots, 0, UntypedArgKind::None, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "move") {
    mark_call_slot(slots, 0, UntypedArgKind::Const, /*is_mutable=*/false);
    mark_call_slot(slots, 1, UntypedArgKind::Mutable, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "setstring") {
    // System.SetString mutates the destination string variable. The source
    // pointer is still an ordinary value because the third argument supplies
    // the byte count to copy.
    mark_call_slot(slots, 0, UntypedArgKind::None, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "blockread") {
    mark_call_slot(slots, 1, UntypedArgKind::Mutable, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "blockwrite") {
    mark_call_slot(slots, 1, UntypedArgKind::Const, /*is_mutable=*/false);
    return slots;
  }
  if (lower == "indexbyte" || lower == "indexword") {
    mark_call_slot(slots, 0, UntypedArgKind::Const, /*is_mutable=*/false);
    return slots;
  }
  if (lower == "comparebyte" || lower == "comparechar" ||
      lower == "compareword") {
    mark_call_slot(slots, 0, UntypedArgKind::Const, /*is_mutable=*/false);
    mark_call_slot(slots, 1, UntypedArgKind::Const, /*is_mutable=*/false);
    return slots;
  }
  if (lower == "getmem" || lower == "reallocmem" ||
      lower == "strdispose") {
    mark_call_slot(slots, 0, UntypedArgKind::None, /*is_mutable=*/true);
  }
  // Pascal `Val(S; var V; var Code)` and `Str(X; var S)` write to caller
  // storage. Mark the var-mode slots so a call-site typecast like
  // `Val(s, aword(result), code)` lowers through `lower_call_arg`'s
  // mutable-ref-cast path -- it rebinds the `result` storage as the
  // typecast's target type, which matches the unsigned `p_val` rt
  // overload. Without this, the cast lowers as a value rvalue and the
  // overload set fails to match.
  if (lower == "val") {
    mark_call_slot(slots, 1, UntypedArgKind::None, /*is_mutable=*/true);
    mark_call_slot(slots, 2, UntypedArgKind::None, /*is_mutable=*/true);
    return slots;
  }
  if (lower == "str") {
    mark_call_slot(slots, 1, UntypedArgKind::None, /*is_mutable=*/true);
    return slots;
  }
  return slots;
}

EmitCalls::EmitCalls(const TypeRegistry* registry, ScopeStateView& scope,
                     EmitAnalysis& analysis, EmitTypes& types,
                     EmitStorage& storage, EmitResolution& resolution,
                     OverloadTypeProvider& overload_types,
                     EmitCallExprOps& expr_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      storage_(storage),
      resolution_(resolution),
      overload_types_(overload_types),
      expr_ops_(expr_ops) {}

bool EmitCalls::proc_accepts_zero_args(const ProcDecl& decl) {
  for (const auto& p : decl.params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    if (count != 0 && !p.default_value) return false;
  }
  return true;
}

std::vector<CallArgumentSlot> EmitCalls::append_default_call_slots(
    const ProcDecl* decl, std::vector<CallArgumentSlot> slots) {
  if (!decl) return slots;
  const std::vector<FlatCallParamInfo> flat_params =
      resolution_.flatten_call_param_info(decl);
  if (slots.size() >= flat_params.size()) return slots;

  bool all_defaults_present = true;
  for (size_t i = slots.size(); i < flat_params.size(); ++i) {
    if (!flat_params[i].default_value) {
      all_defaults_present = false;
      break;
    }
  }
  if (!all_defaults_present) return slots;

  slots.reserve(flat_params.size());
  for (size_t i = slots.size(); i < flat_params.size(); ++i) {
    slots.push_back(CallArgumentSlot{
        .expr = flat_params[i].default_value,
        .param_type = nullptr,
        .param_unit = {},
        .param_declaring_type = {},
        .untyped_arg = UntypedArgKind::None,
        .mutable_ref_arg = false,
        .defaulted = true});
  }
  return slots;
}

std::vector<CallArgumentSlot> EmitCalls::call_slots_with_decl_param_info(
    const ProcDecl* decl, std::vector<CallArgumentSlot> slots,
    std::string_view param_unit, std::string_view param_declaring_type) {
  if (!decl) return slots;
  size_t ai = 0;
  for (const auto& p : decl->params) {
    const size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t k = 0; k < count; ++k) {
      if (ai >= slots.size()) return slots;
      CallArgumentSlot& slot = slots[ai];
      if (!p.type) {
        slot.untyped_arg = (p.mode == Param::Var || p.mode == Param::Out)
                               ? UntypedArgKind::Mutable
                               : UntypedArgKind::Const;
      }
      slot.mutable_ref_arg =
          p.mode == Param::Var || p.mode == Param::Out ||
          (p.mode == Param::Const &&
           analysis_.const_param_needs_mutable_ref(p.type.get()));
      slot.param_type = p.type.get();
      slot.param_unit = std::string(param_unit);
      slot.param_declaring_type = std::string(param_declaring_type);
      ++ai;
    }
  }
  return slots;
}

std::vector<CallArgumentSlot>
EmitCalls::call_slots_with_builtin_helper_param_info(
    const Expr& callee, std::vector<CallArgumentSlot> slots) {
  if (callee.kind == Kind::Ident) {
    return call_slots_with_builtin_memory_helper_info(
        static_cast<const Ident&>(callee).name, std::move(slots));
  }
  if (callee.kind != Kind::Member) return slots;
  const auto& mem = static_cast<const Member&>(callee);
  if (mem.base->kind == Kind::Ident &&
      ascii_lower(static_cast<const Ident&>(*mem.base).name) == "system") {
    return call_slots_with_builtin_memory_helper_info(mem.name,
                                                     std::move(slots));
  }
  return slots;
}

std::vector<CallArgumentSlot>
EmitCalls::call_slots_with_procedural_callee_param_info(
    const Expr& callee, std::vector<CallArgumentSlot> slots) {
  const TypeExpr* callee_type = procedural_callee_type(callee);
  if (callee_type) callee_type = analysis_.canonicalize_type(callee_type);
  if (!callee_type || callee_type->kind != Kind::TyProcedural) return slots;

  const auto& proc = static_cast<const TyProcedural&>(*callee_type);
  size_t ai = 0;
  for (const auto& p : proc.params) {
    const size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t k = 0; k < count; ++k) {
      if (ai >= slots.size()) return slots;
      CallArgumentSlot& slot = slots[ai];
      if (!p.type) {
        slot.untyped_arg = (p.mode == Param::Var || p.mode == Param::Out)
                               ? UntypedArgKind::Mutable
                               : UntypedArgKind::Const;
      }
      slot.mutable_ref_arg =
          p.mode == Param::Var || p.mode == Param::Out ||
          (p.mode == Param::Const &&
           analysis_.const_param_needs_mutable_ref(p.type.get()));
      slot.param_type = p.type.get();
      ++ai;
    }
  }
  return slots;
}

const TypeExpr* EmitCalls::procedural_callee_type(const Expr& callee) {
  // Argument adaptation for a procvar call comes from the callee expression's
  // declared procedural type. It is not a target-typed conversion of the call
  // result and it is not storage binding for the callee slot.
  return analysis_.deduce_type(callee);
}

CallArgumentPlan EmitCalls::plan_call_arguments(
    const ProcDecl* decl, const Expr* callee,
    const std::vector<const Expr*>& explicit_args,
    std::string_view default_arg_unit,
    std::string_view signature_declaring_type) {
  std::vector<CallArgumentSlot> slots;
  slots.reserve(explicit_args.size());
  for (const Expr* arg : explicit_args) {
    slots.push_back(CallArgumentSlot{
        .expr = arg,
        .param_type = nullptr,
        .param_unit = {},
        .param_declaring_type = {},
        .untyped_arg = UntypedArgKind::None,
        .mutable_ref_arg = false,
        .defaulted = false});
  }

  slots = append_default_call_slots(decl, std::move(slots));

  if (callee) {
    slots = call_slots_with_builtin_helper_param_info(*callee, std::move(slots));
  }
  if (decl) {
    slots = call_slots_with_decl_param_info(decl, std::move(slots),
                                            default_arg_unit,
                                            signature_declaring_type);
  } else if (callee) {
    slots =
        call_slots_with_procedural_callee_param_info(*callee, std::move(slots));
  }
  return CallArgumentPlan{.slots = std::move(slots),
                          .default_arg_unit = std::string(default_arg_unit)};
}

bool EmitCalls::slot_accepts_argument(const CallArgumentSlot& slot,
                                      std::string_view default_arg_unit) {
  if (!slot.expr || slot.defaulted) return true;
  EffectiveParamType effective = effective_call_param_type(
      registry_, scope_, slot.param_type, slot.param_unit,
      slot.param_declaring_type, default_arg_unit);
  const TypeExpr* param_type = effective.type;
  if (!param_type || slot.untyped_arg != UntypedArgKind::None) return true;

  if (slot.mutable_ref_arg) {
    if (!storage_.expr_is_storage_lvalue(*slot.expr) &&
        !storage_.storage_designator(*slot.expr)) {
      return false;
    }
    const TypeExpr* arg_type = overload_types_.type_for_overload(*slot.expr);
    if (!arg_type) arg_type = analysis_.deduce_type(*slot.expr);
    if (arg_type) arg_type = analysis_.canonicalize_type(arg_type);
    const TypeExpr* canon_param = analysis_.canonicalize_type(param_type);
    if (!arg_type || !canon_param) return true;
    if (storage_.type_is_stringish(canon_param) &&
        storage_.type_is_stringish(arg_type) &&
        storage_.expr_is_storage_lvalue(*slot.expr)) {
      return true;
    }
    if (resolution_.rank_conversion(arg_type, param_type,
                                    /*var_param=*/true)
            .viable()) {
      return true;
    }
    return storage_.type_is_pointerish(canon_param) &&
           storage_.type_is_pointerish(arg_type);
  }

  return expr_ops_.can_convert_value_to_type(*slot.expr, param_type,
                                             /*explicit_conversion=*/false);
}

bool EmitCalls::validate_call_arguments(const CallArgumentPlan& plan) {
  for (const CallArgumentSlot& slot : plan.slots) {
    if (!slot_accepts_argument(slot, plan.default_arg_unit)) return false;
  }
  return true;
}

std::string EmitCalls::lower_call_arg(const Expr& arg, const TypeExpr* param_type,
                                      UntypedArgKind untyped_arg,
                                      bool mutable_ref_arg,
                                      std::string_view default_arg_unit,
                                      std::string_view param_unit,
                                      std::string_view param_declaring_type) {
  EffectiveParamType effective = effective_call_param_type(
      registry_, scope_, param_type, param_unit, param_declaring_type,
      default_arg_unit);
  param_type = effective.type;
  DefaultArgumentUnitScope default_scope(scope_.current_unit_name,
                                         scope_.lookup_emission_unit_name,
                                         default_arg_unit);
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
  PackedScalarValueLoadSuppressor suppress_packed_scalar_value_load(
      scope_.suppress_packed_scalar_value_load,
      mutable_ref_arg || untyped_arg != UntypedArgKind::None);
  // `var`, `out`, and untyped actuals require a Pascal variable designator.
  // Mark that context before emitting the argument so `T(x)` is treated as a
  // storage view there, while the same source expression in a value argument
  // still builds a converted/copied value.
  struct StorageViewContextScope {
    bool& slot;
    bool saved;
    StorageViewContextScope(bool& slot_in, bool value)
        : slot(slot_in), saved(slot_in) {
      slot = value;
    }
    ~StorageViewContextScope() { slot = saved; }
  } storage_view_context(scope_.storage_view_context,
                         scope_.storage_view_context || mutable_ref_arg ||
                             untyped_arg != UntypedArgKind::None);
  const TypeExpr* arg_type = overload_types_.type_for_overload(arg);
  if (arg_type) arg_type = analysis_.canonicalize_type(arg_type);
  const TypeExpr* canon_param_type = analysis_.canonicalize_type(param_type);
  if (mutable_ref_arg && (canon_param_type || untyped_arg == UntypedArgKind::None)) {
    // `var`/`out` actuals are storage contexts. Reuse the same designator
    // lowering as assignment so call-site typecasts like `Val(s,
    // cardinal(result), code)` are storage views, not value casts.
    if (auto use = storage_.packed_aggregate_path_use(arg)) {
      storage_.report_packed_aggregate_subobject_use(
          arg.loc, "var/out argument", *use);
      return expr_ops_.expr_to_cxx(arg);
    }
    if (auto storage = storage_.storage_designator(arg)) {
      if (storage->access == EmitStorageAccess::ReinterpretRef) {
        if (param_type && storage->type_cxx != types_.type_to_cxx(*param_type)) {
          return storage_.reinterpret_ref_text(types_.type_to_cxx(*param_type),
                                               storage->text, false);
        }
        return storage->text;
      }
      if (storage->access == EmitStorageAccess::UnalignedBytewise) {
        expr_ops_.report_error(
            arg.loc, "unaligned storage cannot be passed to var/out parameter");
        return expr_ops_.expr_to_cxx(arg);
      }
      if (storage->is_bytewise()) {
        const std::string ref_type =
            param_type ? types_.type_to_cxx(*param_type) : storage->type_cxx;
        if (!ref_type.empty()) {
          // Variant payloads are aligned Pascal storage, but they are reached
          // through byte offsets so value reads/writes avoid C++ union
          // active-member rules. A typed var/out call needs the callee's
          // reference view of that same storage.
          return storage_.reinterpret_ref_text(ref_type, storage->ptr_cxx,
                                               true);
        }
      }
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
    if (mutable_ref_arg && arg_type &&
        types_.shortstring_capacity_to_cxx(param_type) &&
        types_.shortstring_capacity_to_cxx(arg_type) &&
        storage_.expr_is_storage_lvalue(arg)) {
      const std::string cap =
          types_.shortstring_capacity_to_cxx(param_type).value_or("255");
      return "::rt::tp2cc_shortstring_ref<" + cap + ">(" +
             expr_ops_.expr_to_cxx(arg) + ")";
    }
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
  if (param_type && arg_type && !mutable_ref_arg &&
      untyped_arg == UntypedArgKind::None) {
    if (auto conv = resolution_.find_assignment_operator(arg_type, param_type);
        conv.decl) {
      std::string fn = pascal_assignment_operator_helper_name(*conv.decl);
      if (!conv.defining_unit.empty()) {
        fn = unit_namespace_prefix(conv.defining_unit) + fn;
      }
      return fn + "(" + expr_ops_.const_value_to_cxx(arg, arg_type, false) + ")";
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
  const char* ptr_cast =
      (untyped_arg == UntypedArgKind::Const && !mutable_ref_arg)
          ? "const void*"
          : "void*";
  // Untyped formals receive the address of Pascal caller storage. Ordinary
  // lvalues use `&x`; `p^`, untyped byte views, and packed fields already have a
  // storage address from the designator, so pass that address instead of first
  // forming a typed C++ reference.
  if (auto storage = storage_.storage_designator(arg)) {
    return storage_.storage_designator_untyped_actual_address(*storage,
                                                              ptr_cast);
  }
  return "((" + std::string(ptr_cast) + ")&(" + arg_text + "))";
}

std::string EmitCalls::lower_call_arg(const CallArgumentSlot& slot,
                                      std::string_view default_arg_unit) {
  if (!slot.expr) return {};
  return lower_call_arg(*slot.expr, slot.param_type, slot.untyped_arg,
                        slot.mutable_ref_arg,
                        slot.defaulted ? default_arg_unit : std::string_view{},
                        slot.param_unit, slot.param_declaring_type);
}

std::string EmitCalls::lower_implicit_zero_arg_call(
    const std::string& callee_text, const ProcDecl* decl,
    std::string_view default_arg_unit,
    std::string_view signature_declaring_type) {
  if (!decl) return callee_text + "()";

  std::vector<const Expr*> args;
  CallArgumentPlan plan =
      plan_call_arguments(decl, nullptr, args, default_arg_unit,
                          signature_declaring_type);
  if (plan.slots.empty()) return callee_text + "()";

  std::string out = callee_text + "(";
  for (size_t i = 0; i < plan.slots.size(); ++i) {
    if (i) out += ", ";
    out += lower_call_arg(plan.slots[i], plan.default_arg_unit);
  }
  out += ")";
  return out;
}

std::optional<std::string> EmitCalls::maybe_lower_class_free_member(
    const Expr& base, std::string_view member_name) {
  if (member_name != "free" || !storage_.expr_is_reference_class(base)) {
    return std::nullopt;
  }
  return "::rt::t_tobject::p_free(" + expr_ops_.expr_to_cxx(base) + ")";
}

std::optional<std::string> EmitCalls::maybe_lower_class_constructor_call(
    Location where, std::string_view class_name, std::string_view member_name,
    const CallArgumentPlan& plan, const ProcDecl* selected_decl) {
  if (!registry_) return std::nullopt;
  const ClassInfo* ci =
      analysis_.class_info_for_type_name(std::string(class_name));
  if (!ci || !ci->is_reference_type) {
    return std::nullopt;
  }
  const MethodSig* method = nullptr;
  if (selected_decl) {
    if (const auto* methods = registry_->lookup_class_methods(
            std::string(class_name), std::string(member_name),
            scope_.current_unit_name)) {
      for (const auto& candidate : *methods) {
        if (candidate.decl.get() == selected_decl) {
          method = &candidate;
          break;
        }
      }
    }
  }
  bool implicit_root_create = false;
  if (!method || method->kind != SymKind::Constructor) {
    if (ascii_lower(std::string(member_name)) != "create" ||
        !plan.slots.empty()) {
      return std::nullopt;
    }
    implicit_root_create = true;
  }
  std::string_view default_arg_unit = plan.default_arg_unit;
  if (default_arg_unit.empty() && method) {
    default_arg_unit = method->defining_unit;
  }
  if (ci->is_abstract) {
    report_warning(where,
                   "instantiating abstract class `" +
                       std::string(class_name) + "`");
  }

  // Pascal constructor calls on a class value (`TNode.Create`) allocate a
  // fresh instance and then run the constructor body on that instance. They
  // are not plain static method calls, even though the emitted C++ helper
  // itself lives on the struct type.
  std::string args_cxx;
  for (size_t i = 0; i < plan.slots.size(); ++i) {
    if (i) args_cxx += ", ";
    args_cxx += lower_call_arg(plan.slots[i], default_arg_unit);
  }
  std::string struct_ty = types_.named_type_struct_cxx(class_name);
  return "([&]{ auto tp2cc_ptr = new " + struct_ty + "{}; tp2cc_ptr->" +
         (implicit_root_create ? std::string("p_create")
                               : mangle(std::string(member_name))) +
         "(" + args_cxx +
         "); return tp2cc_ptr; }())";
}

}  // namespace tp2cc
