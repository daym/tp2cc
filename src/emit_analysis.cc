#include "emit_analysis.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <unordered_set>
#include <vector>

#include "diag.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitAnalysis::EmitAnalysis(const TypeRegistry& registry, ScopeStateView& scope,
                           ResolveNameProvider& resolve_name_provider,
                           CallTypeProvider& call_type_provider,
                           TargetInfo target)
    : registry_(registry),
      scope_(scope),
      resolve_name_provider_(resolve_name_provider),
      call_type_provider_(call_type_provider),
      target_(target) {}

const ClassInfo* EmitAnalysis::current_class_info() const {
  const TypeSymbol* symbol = scope_.current_class_symbol;
  return symbol ? symbol->class_info() : nullptr;
}

uint8_t EmitAnalysis::resolved_primitive_bits(const PrimitiveInfo& info) const {
  return primitive_bits(info, target_);
}

static std::string type_symbol_source_name(const TypeSymbol& symbol) {
  std::string out;
  for (const auto& owner : symbol.owner_path) {
    if (!out.empty()) out += ".";
    out += owner;
  }
  if (!out.empty()) out += ".";
  out += symbol.name;
  return out;
}

static const RecordInfo* record_info_for_resolved_type(
    const TypeRegistry& registry, const TypeExpr* type) {
  const TypeDescriptor* descriptor = registry.descriptor_for_type(type);
  return descriptor ? descriptor->record_info() : nullptr;
}

static const TypeExpr* bound_descriptor_type_for_expr(
    const TypeRegistry& registry, const TypeExpr* type,
    const TypeLookupContext*& context) {
  if (!type) return nullptr;
  if (const TypeLookupContext* own_context =
          registry.lookup_context_for_type(type)) {
    context = own_context;
  }
  const TypeDescriptor* descriptor = registry.descriptor_for_type(type);
  const TypeExpr* payload = descriptor ? descriptor->type : nullptr;
  if (!payload) return nullptr;
  if (const TypeLookupContext* payload_context =
          registry.lookup_context_for_type(payload)) {
    context = payload_context;
  }
  return payload;
}

static const ProcInfo* unique_zero_arg_proc(
    const std::vector<ProcInfo>* procs) {
  if (!procs) return nullptr;
  const ProcInfo* found = nullptr;
  for (const auto& proc : *procs) {
    if (!proc.accepts_zero_args) continue;
    if (found) return nullptr;
    found = &proc;
  }
  return found;
}

static const MethodSig* unique_zero_arg_method(
    const std::vector<MethodSig>* methods) {
  if (!methods) return nullptr;
  const MethodSig* found = nullptr;
  for (const auto& method : *methods) {
    if (!method.accepts_zero_args) continue;
    if (found) return nullptr;
    found = &method;
  }
  return found;
}

static const TypeExpr* proc_result_type(const ProcInfo* proc) {
  if (!proc) return nullptr;
  if (proc->decl && proc->decl->return_type) return proc->decl->return_type.get();
  if (proc->return_type_symbol && proc->return_type_symbol->type) {
    return proc->return_type_symbol->type;
  }
  return nullptr;
}

static const TypeExpr* method_result_type(const MethodSig* method) {
  if (!method) return nullptr;
  if (method->decl && method->decl->return_type) {
    return method->decl->return_type.get();
  }
  if (method->kind == SymKind::Constructor) {
        if (const TypeSymbol* owner = method->declaring_symbol) {
      return owner->type;
    }
  }
  return nullptr;
}

static const TypeExpr* proc_set_value_result_type(
    const std::vector<ProcInfo>* procs) {
  if (!procs || procs->empty()) return nullptr;
  return proc_result_type(unique_zero_arg_proc(procs));
}

static const TypeExpr* method_set_value_result_type(
    const std::vector<MethodSig>* methods) {
  if (!methods || methods->empty()) return nullptr;
  return method_result_type(unique_zero_arg_method(methods));
}

static bool type_is_pointer_arithmetic_operand(const TypeExpr* t) {
  return t && t->kind == Kind::TyPointer;
}

const TypeExpr* EmitAnalysis::semantic_shape_type(const TypeExpr* t) {
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  return semantic_shape_type(t, context);
}

const TypeExpr* EmitAnalysis::semantic_shape_type_in_context(
    const TypeExpr* t, const TypeLookupContext* context) {
  return semantic_shape_type(t, context);
}

const TypeExpr* EmitAnalysis::semantic_shape_type(
    const TypeExpr* t, const TypeLookupContext*& context) {
  if (!t || !registry_.descriptor_for_type(t)) return nullptr;
  if (const TypeExpr* payload =
          bound_descriptor_type_for_expr(registry_, t, context)) {
    return payload;
  }
  // Metaclass descriptors have no TypeExpr payload; the bound source node is
  // their structural form. Missing descriptors were rejected above.
  return t;
}

const TypeExpr* EmitAnalysis::ord_result_type_for_type(const TypeExpr* t) {
  const TypeDescriptor* descriptor = registry_.descriptor_for_type(t);
  const TypeDescriptor* result =
      descriptor ? descriptor->ordinal_result : nullptr;
  return result ? result->type : nullptr;
}

const TypeExpr* EmitAnalysis::ord_result_type_for_operand(
    const Expr& operand) {
  return ord_result_type_for_type(deduce_type(operand));
}

const ClassInfo* EmitAnalysis::class_info_for_type(const TypeExpr* t) {
  const TypeDescriptor* descriptor = registry_.descriptor_for_type(t);
  return descriptor ? descriptor->class_info() : nullptr;
}

const TypeSymbol* EmitAnalysis::class_symbol_for_type(const TypeExpr* t) {
  const TypeSymbol* symbol = registry_.resolved_symbol_for_type(t);
  return symbol && symbol->class_info() ? symbol : nullptr;
}

const InterfaceInfo* EmitAnalysis::interface_info_for_type(const TypeExpr* t) {
  const TypeDescriptor* descriptor = registry_.descriptor_for_type(t);
  return descriptor ? descriptor->interface_info() : nullptr;
}

std::string EmitAnalysis::direct_type_name(const TypeExpr* t) {
  if (!t) return {};
  if (const TypeSymbol* symbol = registry_.resolved_symbol_for_type(t)) {
    return type_symbol_source_name(*symbol);
  }
  return {};
}

const TypeSymbol* EmitAnalysis::explicit_typecast_target_symbol(
    const Expr& e) {
  if (e.kind != Kind::Call) return nullptr;
  const auto& c = static_cast<const Call&>(e);
  if (c.args.size() != 1 || !c.callee) return nullptr;

  if (std::optional<const TypeSymbol*> type_callee =
          registry_.type_name_expression_result(c.callee.get())) {
    const TypeSymbol* symbol = *type_callee;
    return symbol && symbol->descriptor && symbol->descriptor->symbol
               ? symbol->descriptor->symbol
               : symbol;
  }

  return nullptr;
}

const TypeExpr* EmitAnalysis::explicit_typecast_result_type(const Expr& e) {
  if (e.kind != Kind::Call) return nullptr;
  const auto& c = static_cast<const Call&>(e);
  if (c.args.size() != 1 || !c.callee) return nullptr;

  if (std::optional<const TypeSymbol*> type_callee =
          registry_.type_name_expression_result(c.callee.get())) {
    const TypeSymbol* symbol = *type_callee;
    if (!symbol) return nullptr;
    // Metaclass descriptors are target-keyed and deliberately own no syntax
    // node. The parser-selected declaration still carries the bound TyMetaclass
    // expression needed by expression typing. Ordinary aliases likewise keep
    // their bound TyName here while sharing the target descriptor.
    if (symbol->type) return symbol->type;
    return symbol->descriptor ? symbol->descriptor->type : nullptr;
  }

  return nullptr;
}

const TypeSymbol* EmitAnalysis::metaclass_target_symbol(const TypeExpr* t) {
  if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
    return target;
  }
  t = semantic_shape_type(t);
  if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
    return target;
  }
  return nullptr;
}

bool EmitAnalysis::type_accepts_class_value(const TypeExpr* t) {
  return metaclass_target_symbol(t) != nullptr;
}

bool EmitAnalysis::type_is_reference_class(const TypeExpr* t) {
  if (!t) return false;
  if (const auto* ci = class_info_for_type(t)) {
    return ci->is_reference_type;
  }
  t = semantic_shape_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyObject) {
    return static_cast<const TyObject&>(*t).is_reference_type;
  }
  if (const auto* ci = class_info_for_type(t)) {
    return ci->is_reference_type;
  }
  return false;
}

bool EmitAnalysis::type_is_interface(const TypeExpr* t) {
  if (!t) return false;
  if (interface_info_for_type(t)) return true;
  t = semantic_shape_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyInterface) return true;
  return interface_info_for_type(t) != nullptr;
}

bool EmitAnalysis::type_is_value_object(const TypeExpr* t) {
  if (const auto* ci = class_info_for_type(t)) {
    return !ci->is_reference_type;
  }
  t = semantic_shape_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyObject) {
    return !static_cast<const TyObject&>(*t).is_reference_type;
  }
  if (const auto* ci = class_info_for_type(t)) {
    return !ci->is_reference_type;
  }
  return false;
}

bool EmitAnalysis::const_param_needs_mutable_ref(const TypeExpr* t) {
  // Old object-style `object` values still treat `const` mostly as a
  // calling-convention hint. Their methods may mutate bookkeeping through the
  // hidden self slot, so lowering them to `const T&` would make valid Pascal
  // source ill-formed in C++.
  if (type_is_reference_class(t)) return false;
  return type_is_value_object(t);
}

bool EmitAnalysis::const_param_needs_const_ref(const TypeExpr*) {
  // Pascal `const` is not a blanket request for C++ reference semantics.
  // Keep the default path as plain-by-value unless the source model proves we
  // need aliasing; otherwise reviewability suffers because "harmless" params
  // silently become storage-identity-preserving references.
  return false;
}

const TypeExpr* EmitAnalysis::deduce_const_decl_type(const ConstDecl& cd) {
  if (cd.type) return cd.type.get();
  auto info = eval_const_int_expr(*cd.value);
  if (info && info->type) return integer_type(info->type);
  if (cd.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*cd.value));
  }
  return cd.value ? deduce_type(*cd.value) : nullptr;
}

const TypeExpr* EmitAnalysis::deduce_const_info_type(const ConstInfo& c) {
  if (c.type) return c.type.get();
  if (!c.value) return nullptr;
  auto info = eval_const_int_expr(*c.value);
  if (info && info->type) return integer_type(info->type);
  if (c.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*c.value));
  }
  return deduce_type(*c.value);
}

const TySet* EmitAnalysis::synthesize_set_type(
    const TypeExpr* element,
    std::optional<std::pair<int64_t, int64_t>> explicit_bounds) {
  return registry_.inferred_set_type(element, explicit_bounds);
}

const TyPointer* EmitAnalysis::synthesize_pointer_type(
    const TypeExpr* target) {
  return registry_.inferred_pointer_type(target);
}

bool EmitAnalysis::same_type_ast(const TypeExpr* a, const TypeExpr* b) {
  return same_type_ast_in_context(a, registry_.lookup_context_for_type(a), b,
                                  registry_.lookup_context_for_type(b));
}

bool EmitAnalysis::same_type_ast_in_context(
    const TypeExpr* a, const TypeLookupContext* a_context,
    const TypeExpr* b, const TypeLookupContext* b_context) {
  if (const TypeLookupContext* own_context =
          registry_.lookup_context_for_type(a)) {
    a_context = own_context;
  }
  if (const TypeLookupContext* own_context =
          registry_.lookup_context_for_type(b)) {
    b_context = own_context;
  }
  const TypeDescriptor* initial_ad = registry_.descriptor_for_type(a);
  const TypeDescriptor* initial_bd = registry_.descriptor_for_type(b);
  if (initial_ad && initial_bd && initial_ad == initial_bd) return true;
  // Several Pascal families are structural even when the declarations that
  // name them are distinct: ordinal subranges, fixed arrays, sets, files,
  // shortstrings, and typed pointers. Resolve aliases to their declaration
  // bodies and carry the body's scope with the shape, so child bounds and
  // pointees are interpreted where the type was declared. Records/classes/enums
  // remain nominal below.
  a = semantic_shape_type(a, a_context);
  b = semantic_shape_type(b, b_context);
  if (!a || !b) return false;
  if (a == b) return true;
  auto child_context = [this](const TypeExpr* child,
                              const TypeLookupContext* parent) {
    if (const TypeLookupContext* own =
            registry_.lookup_context_for_type(child)) {
      return own;
    }
    return parent;
  };
  struct PointerView {
    const TyPointer* pointer;
    const TypeLookupContext* context;
  };
  auto pointer_shape =
      [&](const TypeExpr* t, const TypeLookupContext* context) -> PointerView {
    if (!t) return {};
    if (t->kind == Kind::TyPointer) {
      return {&static_cast<const TyPointer&>(*t), context};
    }
    if (t->kind != Kind::TyDistinct) return {};
    const auto& distinct = static_cast<const TyDistinct&>(*t);
    const TypeLookupContext* underlying_context =
        child_context(distinct.underlying.get(), context);
    const TypeExpr* underlying =
        semantic_shape_type_in_context(distinct.underlying.get(),
                                       underlying_context);
    if (!underlying || underlying->kind != Kind::TyPointer) return {};
    return {&static_cast<const TyPointer&>(*underlying), underlying_context};
  };
  auto same_ordinal_domain =
      [&](const TypeExpr* left, const TypeLookupContext* left_context,
          const TypeExpr* right,
          const TypeLookupContext* right_context) -> bool {
    std::optional<OrdinalDomain> ld =
        ordinal_domain_for_type(left, left_context);
    std::optional<OrdinalDomain> rd =
        ordinal_domain_for_type(right, right_context);
    return ld && rd && ld->family == rd->family && ld->low == rd->low &&
           ld->high == rd->high && ld->enum_key == rd->enum_key;
  };
  auto shortstring_capacity =
      [&](const TyString& s,
          const TypeLookupContext* context) -> std::optional<int64_t> {
    if (!s.max_length) return 255;
    std::optional<OrdinalExprValue> value =
        eval_ordinal_expr(*s.max_length, context);
    if (!value || value->family != OrdinalFamily::Integer) return std::nullopt;
    return value->value;
  };
  if (PointerView ap = pointer_shape(a, a_context); ap.pointer) {
    if (PointerView bp = pointer_shape(b, b_context); bp.pointer) {
      if (!ap.pointer->target || !bp.pointer->target) {
        return !ap.pointer->target && !bp.pointer->target;
      }
      return same_type_ast_in_context(
          ap.pointer->target.get(),
          child_context(ap.pointer->target.get(), ap.context),
          bp.pointer->target.get(),
          child_context(bp.pointer->target.get(), bp.context));
    }
  }
  if (same_ordinal_domain(a, a_context, b, b_context)) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case Kind::TyName: {
      const TypeDescriptor* ad = registry_.descriptor_for_type(a);
      const TypeDescriptor* bd = registry_.descriptor_for_type(b);
      return ad && ad == bd;
    }
    case Kind::TyDistinct:
      return false;
    case Kind::TyPointer: {
      const auto& ap = static_cast<const TyPointer&>(*a);
      const auto& bp = static_cast<const TyPointer&>(*b);
      if (!ap.target || !bp.target) return !ap.target && !bp.target;
      return same_type_ast_in_context(
          ap.target.get(), child_context(ap.target.get(), a_context),
          bp.target.get(), child_context(bp.target.get(), b_context));
    }
    case Kind::TyArray: {
      const auto& aa = static_cast<const TyArray&>(*a);
      const auto& ba = static_cast<const TyArray&>(*b);
      if (aa.array_kind != ba.array_kind || aa.is_packed != ba.is_packed) {
        return false;
      }
      if (aa.array_kind != ArrayKind::Fixed ||
          aa.dims.size() != ba.dims.size()) {
        return false;
      }
      for (size_t i = 0; i < aa.dims.size(); ++i) {
        if (!same_ordinal_domain(aa.dims[i].get(),
                                 child_context(aa.dims[i].get(), a_context),
                                 ba.dims[i].get(),
                                 child_context(ba.dims[i].get(), b_context))) {
          return false;
        }
      }
      return same_type_ast_in_context(
          aa.element.get(), child_context(aa.element.get(), a_context),
          ba.element.get(), child_context(ba.element.get(), b_context));
    }
    case Kind::TySet: {
      const auto& as = static_cast<const TySet&>(*a);
      const auto& bs = static_cast<const TySet&>(*b);
      std::optional<OrdinalDomain> ad = ordinal_domain_for_type(
          as.element.get(), child_context(as.element.get(), a_context));
      std::optional<OrdinalDomain> bd = ordinal_domain_for_type(
          bs.element.get(), child_context(bs.element.get(), b_context));
      if (!ad || !bd) return false;
      if (as.has_explicit_bounds) {
        ad->low = as.explicit_low;
        ad->high = as.explicit_high;
      }
      if (bs.has_explicit_bounds) {
        bd->low = bs.explicit_low;
        bd->high = bs.explicit_high;
      }
      return ad->family == bd->family && ad->low == bd->low &&
             ad->high == bd->high && ad->enum_key == bd->enum_key;
    }
    case Kind::TyFile: {
      const auto& af = static_cast<const TyFile&>(*a);
      const auto& bf = static_cast<const TyFile&>(*b);
      if (af.is_text != bf.is_text) return false;
      // Bare `file' is the standard untyped file type; each parsed occurrence
      // is not a fresh nominal declaration. Typed files compare through their
      // element type, while text stays a distinct file kind.
      if (!af.element || !bf.element) return !af.element && !bf.element;
      return same_type_ast_in_context(
          af.element.get(), child_context(af.element.get(), a_context),
          bf.element.get(), child_context(bf.element.get(), b_context));
    }
    case Kind::TySubrange: {
      const auto& as = static_cast<const TySubrange&>(*a);
      const auto& bs = static_cast<const TySubrange&>(*b);
      auto alo = eval_ordinal_expr(*as.lo, a_context);
      auto ahi = eval_ordinal_expr(*as.hi, a_context);
      auto blo = eval_ordinal_expr(*bs.lo, b_context);
      auto bhi = eval_ordinal_expr(*bs.hi, b_context);
      return alo && ahi && blo && bhi && alo->family == ahi->family &&
             alo->family == blo->family && blo->family == bhi->family &&
             alo->enum_key == ahi->enum_key && alo->enum_key == blo->enum_key &&
             blo->enum_key == bhi->enum_key && alo->value == blo->value &&
             ahi->value == bhi->value;
    }
    case Kind::TyString: {
      const auto& as = static_cast<const TyString&>(*a);
      const auto& bs = static_cast<const TyString&>(*b);
      // Scalar `var string[n]` uses the shortstring storage-proxy path. Here we
      // compare layout identity for recursive contexts such as fixed arrays.
      std::optional<int64_t> ac = shortstring_capacity(as, a_context);
      std::optional<int64_t> bc = shortstring_capacity(bs, b_context);
      return ac && bc && *ac == *bc;
    }
    case Kind::TyEnum:
      return false;
    default:
      return false;
  }
}

std::optional<EmitAnalysis::OrdinalExprValue>
EmitAnalysis::eval_ordinal_expr(const Expr& e) {
  if (e.kind == Kind::StringLit) {
    const auto& sl = static_cast<const StringLit&>(e);
    if (sl.value.size() == 1) {
      return OrdinalExprValue{static_cast<unsigned char>(sl.value[0]),
                              OrdinalFamily::Char, nullptr};
    }
  }
  if (e.kind == Kind::Ident) {
    const std::string& low = static_cast<const Ident&>(e).name;
    if (low == "false" || low == "true") {
      return OrdinalExprValue{(low == "true") ? 1 : 0,
                              OrdinalFamily::Boolean, nullptr};
    }
    if (const EnumMemberInfo* member = find_visible_enum_member(low);
        member && member->owner) {
      return OrdinalExprValue{member->ordinal, OrdinalFamily::Enum,
                              member->owner->descriptor};
    }
  }
  if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (auto unit_member = resolve_unit_qualified_member(mem);
        unit_member &&
        unit_member->resolved.kind == ResolvedKind::EnumMember) {
      if (const EnumMemberInfo* member =
              registry_.lookup_enum_member_in_unit(
                  unit_member->unit_name, unit_member->member_name);
          member && member->owner) {
        return OrdinalExprValue{member->ordinal, OrdinalFamily::Enum,
                                member->owner->descriptor};
      }
    }
  }
  if (e.kind == Kind::Call) {
    const auto& call = static_cast<const Call&>(e);
    if (call.callee && call.callee->kind == Kind::Ident &&
        call.args.size() == 1 && call.args[0]) {
      const std::string name =
          ascii_lower(static_cast<const Ident&>(*call.callee).name);
      if (name == "low" || name == "high") {
        if (const TypeExpr* type = const_intrinsic_type_arg(*call.args[0])) {
          if (std::optional<OrdinalDomain> domain =
                  ordinal_domain_for_type(type)) {
            return OrdinalExprValue{
                name == "low" ? domain->low : domain->high,
                domain->family, domain->enum_key};
          }
        }
      }
      if (name == "pred" || name == "succ") {
        if (std::optional<OrdinalExprValue> value =
                eval_ordinal_expr(*call.args[0])) {
          int64_t adjusted = value->value;
          const bool valid =
              name == "pred"
                  ? checked_sub_int64(adjusted, 1, &adjusted)
                  : checked_add_int64(adjusted, 1, &adjusted);
          if (valid) {
            value->value = adjusted;
            return value;
          }
        }
      }
      if (name == "ord") {
        if (std::optional<OrdinalExprValue> value =
                eval_ordinal_expr(*call.args[0])) {
          return OrdinalExprValue{
              value->value, OrdinalFamily::Integer, nullptr};
        }
      }
    }
  }
  if (e.kind == Kind::Binary) {
    const auto& b = static_cast<const Binary&>(e);
    if (binop_is_comparison(b.op)) {
      auto lhs = eval_ordinal_expr(*b.lhs);
      auto rhs = eval_ordinal_expr(*b.rhs);
      if (!lhs || !rhs || lhs->family != rhs->family ||
          lhs->enum_key != rhs->enum_key) {
        return std::nullopt;
      }
      bool value = false;
      switch (b.op) {
        case BinOp::Eq:
          value = lhs->value == rhs->value;
          break;
        case BinOp::NotEq:
          value = lhs->value != rhs->value;
          break;
        case BinOp::Lt:
          value = lhs->value < rhs->value;
          break;
        case BinOp::Gt:
          value = lhs->value > rhs->value;
          break;
        case BinOp::LtEq:
          value = lhs->value <= rhs->value;
          break;
        case BinOp::GtEq:
          value = lhs->value >= rhs->value;
          break;
        default:
          return std::nullopt;
      }
      return OrdinalExprValue{value ? 1 : 0, OrdinalFamily::Boolean, nullptr};
    }
  }
  if (auto info = eval_const_int_expr(e)) {
    return OrdinalExprValue{info->value, OrdinalFamily::Integer, nullptr};
  }
  return std::nullopt;
}

std::optional<OrdinalDomain> EmitAnalysis::ordinal_domain_for_type(
    const TypeExpr* t) {
  const TypeLookupContext* context =
      registry_.lookup_context_for_type(t);
  return ordinal_domain_for_type(t, context);
}

std::optional<EmitAnalysis::OrdinalExprValue>
EmitAnalysis::eval_ordinal_expr(const Expr& e,
                                const TypeLookupContext* context) {
  if (!context) return eval_ordinal_expr(e);
  // Bound expressions inside a type declaration are lexical Pascal source, not
  // caller-context expressions. Evaluate unqualified constants such as
  // `set of 0..MaxRegister` in the unit that declared the type.
  ScopedDeclarationLookup declaration_scope(scope_, context, context->unit);
  return eval_ordinal_expr(e);
}

std::optional<OrdinalDomain> EmitAnalysis::ordinal_domain_for_type(
    const TypeExpr* t, const TypeLookupContext* context) {
  t = semantic_shape_type(t, context);
  if (const TypeLookupContext* canon_context =
          registry_.lookup_context_for_type(t)) {
    context = canon_context;
  }
  if (!t) return std::nullopt;
  if (t->kind == Kind::TyDistinct) {
    return ordinal_domain_for_type(
        static_cast<const TyDistinct&>(*t).underlying.get(), context);
  }
  if (t->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*t);
    auto lo = eval_ordinal_expr(*sr.lo, context);
    auto hi = eval_ordinal_expr(*sr.hi, context);
    if (!lo || !hi || lo->family != hi->family ||
        lo->enum_key != hi->enum_key) {
      return std::nullopt;
    }
    return OrdinalDomain{.family = lo->family,
                         .low = std::min(lo->value, hi->value),
                         .high = std::max(lo->value, hi->value),
                         .enum_key = lo->enum_key};
  }
  if (t->kind == Kind::TyEnum) {
    const auto& en = static_cast<const TyEnum&>(*t);
    return OrdinalDomain{
        .family = OrdinalFamily::Enum,
        .low = 0,
        .high =
            en.members.empty() ? 0 : static_cast<int64_t>(en.members.size() - 1),
        .enum_key = t->descriptor};
  }
  if (t->kind != Kind::TyName) return std::nullopt;

  const PrimitiveInfo* info = primitive_info_for_type(t);
  if (info && info->kind == PrimitiveKind::Boolean) {
    return OrdinalDomain{OrdinalFamily::Boolean, 0, 1, {}};
  }
  if (info && info->kind == PrimitiveKind::Char) {
    return OrdinalDomain{OrdinalFamily::Char, 0, 255, {}};
  }
  if (info && info->kind == PrimitiveKind::WideChar) {
    return OrdinalDomain{OrdinalFamily::WideChar, 0, 65535, {}};
  }
  if (info && info->int_kind != PrimitiveIntKind::None) {
    const uint8_t width = resolved_primitive_bits(*info);
    int64_t domain_low = 0;
    int64_t domain_high = 0;
    if (info->int_kind == PrimitiveIntKind::Unsigned) {
      domain_high = width >= 64
                        ? std::numeric_limits<int64_t>::max()
                        : static_cast<int64_t>(unsigned_mask_for_bits(width));
    } else {
      domain_low = signed_min_for_bits(width);
      domain_high = signed_max_for_bits(width);
    }
    return OrdinalDomain{.family = OrdinalFamily::Integer,
                         .low = domain_low,
                         .high = domain_high,
                         .enum_key = nullptr};
  }
  return std::nullopt;
}

std::optional<OrdinalDomain>
EmitAnalysis::ordinal_domain_for_set_type(const TypeExpr* t) {
  const TypeLookupContext* context =
      registry_.lookup_context_for_type(t);
  t = semantic_shape_type(t, context);
  if (const TypeLookupContext* canon_context =
          registry_.lookup_context_for_type(t)) {
    context = canon_context;
  }
  if (!t || t->kind != Kind::TySet) return std::nullopt;
  const auto& s = static_cast<const TySet&>(*t);
  auto dom = ordinal_domain_for_type(s.element.get(), context);
  if (!dom) return std::nullopt;
  if (s.has_explicit_bounds) {
    dom->low = s.explicit_low;
    dom->high = s.explicit_high;
  }
  return dom;
}

std::optional<EmitAnalysis::SetLiteralOrdinalSummary>
EmitAnalysis::extend_set_literal_ordinal_summary(
    std::optional<SetLiteralOrdinalSummary> summary, const Expr& e) {
  auto ordinal = eval_ordinal_expr(e);
  if (!ordinal) {
    return std::nullopt;
  }

  // Every element of one Pascal set literal belongs to one ordinal domain.
  // Keep that identity together with the bounds so later synthesis creates a
  // real set type instead of treating each element expression independently.
  if (!summary) {
    const TypeExpr* source_type = deduce_type(e);
    const TypeDescriptor* source_descriptor =
        registry_.descriptor_for_type(source_type);
    const TypeDescriptor* element_descriptor =
        source_descriptor
            ? source_descriptor->set_literal_element_result
            : nullptr;
    return SetLiteralOrdinalSummary{
        ordinal->family,
        ordinal->enum_key,
        element_descriptor ? element_descriptor->type : nullptr,
        ordinal->value,
        ordinal->value,
    };
  }
  if (summary->family != ordinal->family ||
      summary->enum_key != ordinal->enum_key) {
    return std::nullopt;
  }

  return SetLiteralOrdinalSummary{
      summary->family,
      summary->enum_key,
      summary->element_type,
      std::min(summary->low, ordinal->value),
      std::max(summary->high, ordinal->value),
  };
}

std::optional<EmitAnalysis::SetLiteralOrdinalSummary>
EmitAnalysis::summarize_set_literal_ordinals(const SetLit& s) {
  std::optional<SetLiteralOrdinalSummary> summary;
  for (const auto& el : s.elements) {
    if (el->kind == Kind::Range) {
      const auto& r = static_cast<const Range&>(*el);
      auto next = extend_set_literal_ordinal_summary(summary, *r.lo);
      if (!next) return std::nullopt;
      next = extend_set_literal_ordinal_summary(next, *r.hi);
      if (!next) return std::nullopt;
      summary = *next;
    } else {
      auto next = extend_set_literal_ordinal_summary(summary, *el);
      if (!next) return std::nullopt;
      summary = *next;
    }
  }
  return summary;
}

const TypeExpr* EmitAnalysis::deduce_set_literal_type(const SetLit& s,
                                                      const TypeExpr* target) {
  if (const TypeExpr* set_target = canonical_set_type(target)) {
    return set_target;
  }
  if (s.elements.empty()) return nullptr;

  auto summary = summarize_set_literal_ordinals(s);
  if (!summary) {
    const TypeExpr* element_type = nullptr;
    std::optional<OrdinalDomain> domain;
    auto add_element_type = [&](const Expr& e) {
      const TypeExpr* t = deduce_type(e);
      auto next = ordinal_domain_for_type(t);
      if (!next) return false;
      if (!domain) {
        element_type = t;
        domain = *next;
        return true;
      }
      return domain->family == next->family &&
             domain->enum_key == next->enum_key;
    };
    for (const auto& el : s.elements) {
      if (el->kind == Kind::Range) {
        const auto& r = static_cast<const Range&>(*el);
        if (!add_element_type(*r.lo) || !add_element_type(*r.hi)) {
          return nullptr;
        }
      } else if (!add_element_type(*el)) {
        return nullptr;
      }
    }
    if (!domain || !element_type) return nullptr;
    // Non-constant set elements such as `[op]` still carry an ordinal Pascal
    // type. Use that type's full domain so overload ranking can match the set
    // literal against `set of T` parameters without pretending to know which
    // runtime elements will be present.
    return synthesize_set_type(element_type,
                               std::make_pair(domain->low, domain->high));
  }

  const TypeExpr* element_type = summary->element_type;
  if (!element_type) return nullptr;
  return synthesize_set_type(element_type,
                             std::make_pair(summary->low, summary->high));
}

SetConversionKind EmitAnalysis::classify_set_conversion(
    const TypeExpr* source, const TypeExpr* target) {
  source = canonical_set_type(source);
  target = canonical_set_type(target);
  if (!(source && target)) {
    return SetConversionKind::Incompatible;
  }

  auto src_dom = ordinal_domain_for_set_type(source);
  auto dst_dom = ordinal_domain_for_set_type(target);
  if (!src_dom || !dst_dom) return SetConversionKind::Incompatible;

  bool family_compatible = false;
  switch (dst_dom->family) {
    case OrdinalFamily::Integer:
      family_compatible = src_dom->family == OrdinalFamily::Integer;
      break;
    case OrdinalFamily::Boolean:
      family_compatible = src_dom->family == OrdinalFamily::Boolean;
      break;
    case OrdinalFamily::Char:
      family_compatible = src_dom->family == OrdinalFamily::Char;
      break;
    case OrdinalFamily::WideChar:
      family_compatible = src_dom->family == OrdinalFamily::WideChar;
      break;
    case OrdinalFamily::Enum:
      family_compatible = src_dom->family == OrdinalFamily::Enum &&
                          src_dom->enum_key == dst_dom->enum_key;
      break;
    case OrdinalFamily::Invalid:
      break;
  }
  if (!family_compatible) return SetConversionKind::Incompatible;
  if (src_dom->low < dst_dom->low || src_dom->high > dst_dom->high) {
    return SetConversionKind::Incompatible;
  }

  const auto& src_set = static_cast<const TySet&>(*source);
  const auto& dst_set = static_cast<const TySet&>(*target);
  const bool same_bounds =
      src_dom->low == dst_dom->low && src_dom->high == dst_dom->high;
  if (same_bounds && same_type_ast(src_set.element.get(), dst_set.element.get())) {
    return SetConversionKind::Exact;
  }
  return SetConversionKind::Compatible;
}

std::optional<ConvertedConstInt> EmitAnalysis::convert_const_int_value(
    Location where, const ConstIntExprInfo& value, const TypeExpr* target,
    bool explicit_conversion, bool diagnose) {
  if (!target) return std::nullopt;
  const PrimitiveInfo* info = primitive_info_for_type(target);
  if (!info || info->int_kind == PrimitiveIntKind::None) return std::nullopt;

  const uint8_t width = resolved_primitive_bits(*info);
  const PrimitiveInfo* source_info = value.type;
  const bool source_is_unsigned =
      source_info && source_info->int_kind == PrimitiveIntKind::Unsigned;
  bool fits = true;
  if (info->int_kind == PrimitiveIntKind::Unsigned) {
    if (source_is_unsigned) {
      fits = width >= 64 || value.bits <= unsigned_mask_for_bits(width);
    } else if (value.value < 0) {
      fits = false;
    } else if (width < 64) {
      fits = static_cast<uint64_t>(value.value) <= unsigned_mask_for_bits(width);
    }
  } else {
    if (source_is_unsigned) {
      const uint64_t hi =
          width >= 64 ? static_cast<uint64_t>(INT64_MAX)
                      : static_cast<uint64_t>(signed_max_for_bits(width));
      fits = value.bits <= hi;
    } else {
      const int64_t lo = signed_min_for_bits(width);
      const int64_t hi = signed_max_for_bits(width);
      fits = value.value >= lo && value.value <= hi;
    }
  }
  if (!fits && diagnose && !explicit_conversion) {
    report_warning(where, "range check error while evaluating constants");
  }

  const uint64_t bits = low_bits(value.bits, width);
  if (info->int_kind == PrimitiveIntKind::Unsigned) {
    return ConvertedConstInt{.value = static_cast<int64_t>(bits),
                             .bits = bits,
                             .type = info};
  }
  if (width == 64) {
    return ConvertedConstInt{.value = static_cast<int64_t>(bits),
                             .bits = bits,
                             .type = info};
  }
  uint64_t sign_bit = uint64_t{1} << (width - 1);
  const int64_t converted_value =
      (bits & sign_bit) == 0
          ? static_cast<int64_t>(bits)
          : static_cast<int64_t>(bits | ~low_bits(UINT64_MAX, width));
  return ConvertedConstInt{
      .value = converted_value,
      .bits = bits,
      .type = info};
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_cast(
    const Call& c,
    std::unordered_set<std::string>* visiting_const_names) {
  if (c.args.size() != 1) return std::nullopt;
  const TypeSymbol* symbol = explicit_typecast_target_symbol(c);
  const TypeDescriptor* descriptor = symbol ? symbol->descriptor : nullptr;
  const PrimitiveInfo* primitive = descriptor ? descriptor->primitive : nullptr;
  const TypeExpr* cast_type =
      primitive && primitive->int_kind != PrimitiveIntKind::None
          ? descriptor->type
          : nullptr;
  if (!cast_type) return std::nullopt;
  auto arg = eval_const_int_expr(*c.args[0], visiting_const_names);
  if (!arg) return std::nullopt;
  auto converted =
      convert_const_int_value(c.loc, *arg, cast_type, true, false);
  if (!converted) return std::nullopt;
  return ConstIntExprInfo{converted->value, converted->bits,
                          converted->type};
}

const TypeExpr* EmitAnalysis::const_intrinsic_type_arg(const Expr& arg) {
  // Constant intrinsics such as `Ord(High(T))` receive their type operand
  // through expression syntax. Resolve only the Pascal type forms accepted in
  // that context; ordinary value identifiers are folded elsewhere.
  if (std::optional<const TypeSymbol*> type_operand =
          registry_.type_name_expression_result(&arg)) {
    return *type_operand && (*type_operand)->descriptor
               ? (*type_operand)->descriptor->type
               : nullptr;
  }
  if (arg.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(arg);
    if (const TypeSymbol* record_symbol =
            class_or_record_type_value_symbol(*m.base)) {
      // `low(T.field)` and `high(T.field)` ask for the declared field type of
      // a resolved record type. The base type identity is already bound; only the
      // member selection happens here because fields are not independent type
      // symbols in Pascal.
      return lookup_record_field_type_in_type(record_symbol->type, m.name);
    }
  }
  return nullptr;
}

std::optional<ConstIntExprInfo> EmitAnalysis::fold_untyped_const_decl(
    const ConstDecl& cd,
    std::unordered_set<std::string>* visiting_const_names) {
  // Untyped `const X = ...` is a compile-time constant. Typed
  // `const X: T = ...` has storage identity, so folding later references
  // through the initializer would erase aliasing/address semantics.
  if (cd.type || !cd.value) return std::nullopt;
  return eval_const_int_expr(*cd.value, visiting_const_names);
}

std::optional<ConstIntExprInfo> EmitAnalysis::fold_untyped_const_info(
    const ConstInfo& c,
    std::unordered_set<std::string>* visiting_const_names) {
  if (c.type || !c.value) return std::nullopt;
  return eval_const_int_expr(*c.value, visiting_const_names);
}

bool EmitAnalysis::type_is_string_like(const TypeExpr* t) {
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  const TypeExpr* shape = semantic_shape_type(t, context);
  if (shape && shape->kind == Kind::TyString) return true;
  const PrimitiveInfo* info = primitive_info_for_type(t);
  return info && info->is_string();
}

const TypeExpr* EmitAnalysis::canonical_set_type(const TypeExpr* t) {
  if (!t) return nullptr;
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  const TypeExpr* c = semantic_shape_type(t, context);
  return (c && c->kind == Kind::TySet) ? c : nullptr;
}

const PrimitiveInfo* EmitAnalysis::primitive_info_for_type(const TypeExpr* t) {
  if (const TypeDescriptor* descriptor = registry_.descriptor_for_type(t)) {
    if (descriptor->primitive) return descriptor->primitive;
  }
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  t = semantic_shape_type(t, context);
  if (t && t->kind == Kind::TyDistinct) {
    return primitive_info_for_type(
        static_cast<const TyDistinct&>(*t).underlying.get());
  }
  return nullptr;
}

bool EmitAnalysis::type_is_numeric_primitive(const TypeExpr* t) {
  const PrimitiveInfo* pi = primitive_info_for_type(t);
  return pi && (pi->int_kind != PrimitiveIntKind::None ||
                pi->float_tier != PrimitiveFloatTier::None);
}

bool EmitAnalysis::binop_is_comparison(BinOp op) {
  return op == BinOp::Eq || op == BinOp::NotEq || op == BinOp::Lt ||
         op == BinOp::Gt || op == BinOp::LtEq || op == BinOp::GtEq;
}

bool EmitAnalysis::binop_is_arithmetic_like(BinOp op) {
  return op == BinOp::Add || op == BinOp::Sub || op == BinOp::Mul ||
         op == BinOp::RealDiv || op == BinOp::IntDiv || op == BinOp::Mod ||
         op == BinOp::Shl || op == BinOp::Shr || op == BinOp::And ||
         op == BinOp::Or || op == BinOp::Xor;
}

bool EmitAnalysis::type_is_long_string(const TypeExpr* t) {
  const PrimitiveInfo* info = primitive_info_for_type(t);
  return info && (info->kind == PrimitiveKind::AnsiString ||
                  info->kind == PrimitiveKind::Utf8String);
}

const TypeExpr* EmitAnalysis::integer_type(const PrimitiveInfo* info) const {
  if (!info || info->int_kind == PrimitiveIntKind::None ||
      !info->descriptor) {
    return nullptr;
  }
  return info->descriptor->type;
}

const TypeExpr* EmitAnalysis::ordinal_integer_type(PrimitiveIntKind kind,
                                                   uint8_t bits) const {
  for (const PrimitiveInfo& candidate : registry_.primitive_info_storage) {
    if (candidate.is_ordinal_integer && candidate.int_kind == kind &&
        candidate.bits == bits && candidate.descriptor) {
      return candidate.descriptor->type;
    }
  }
  return nullptr;
}

const TypeExpr* EmitAnalysis::canonicalize_for_arithmetic(const TypeExpr* t) {
  t = semantic_shape_type(t);
  if (!t || t->kind != Kind::TySubrange) return t;
  auto domain = ordinal_domain_for_type(t);
  if (!domain || domain->family != OrdinalFamily::Integer) return t;
  // Map integer ordinal domain bounds to the smallest Pascal primitive
  // that can represent them. This matches the logic in
  // EmitResolution::integer_actual_domain_for_type and
  // EmitResolution::strip_conversion_wrapper so that type deduction
  // and overload ranking use the same host-type mapping for subranges.
  const int64_t low = domain->low;
  const int64_t high = static_cast<int64_t>(domain->high);
  if (low < 0) {
    if (high <= signed_max_for_bits(8))
      return ordinal_integer_type(PrimitiveIntKind::Signed, 8);
    if (high <= signed_max_for_bits(16))
      return ordinal_integer_type(PrimitiveIntKind::Signed, 16);
    if (high <= signed_max_for_bits(32))
      return ordinal_integer_type(PrimitiveIntKind::Signed, 32);
    return ordinal_integer_type(PrimitiveIntKind::Signed, 64);
  }
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(8)))
    return ordinal_integer_type(PrimitiveIntKind::Unsigned, 8);
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(16)))
    return ordinal_integer_type(PrimitiveIntKind::Unsigned, 16);
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(32)))
    return ordinal_integer_type(PrimitiveIntKind::Unsigned, 32);
  return ordinal_integer_type(PrimitiveIntKind::Unsigned, 64);
}

const TypeExpr* EmitAnalysis::deduce_binary_expr_type(const Binary& b) {
  if (b.op == BinOp::As) {
    if (std::optional<const TypeSymbol*> rhs_type =
            registry_.type_name_expression_result(b.rhs.get())) {
      return *rhs_type ? (*rhs_type)->type : nullptr;
    }
    return nullptr;
  }

  const TypeExpr* lt = deduce_type(*b.lhs);
  const TypeExpr* rt = deduce_type(*b.rhs);
  const TypeExpr* ltc = canonicalize_for_arithmetic(lt);
  const TypeExpr* rtc = canonicalize_for_arithmetic(rt);

  if (binop_is_arithmetic_like(b.op)) {
    if (b.op == BinOp::RealDiv) {
      const PrimitiveInfo* lp = primitive_info_for_type(ltc);
      const PrimitiveInfo* rp = primitive_info_for_type(rtc);
      if (!lp || !rp) return nullptr;
      const bool lreal = lp->is_real();
      const bool rreal = rp->is_real();
      const bool lint = lp->int_kind != PrimitiveIntKind::None;
      const bool rint = rp->int_kind != PrimitiveIntKind::None;
      if (lreal && rreal) {
        const TypeDescriptor* ld = registry_.descriptor_for_type(ltc);
        const TypeDescriptor* rd = registry_.descriptor_for_type(rtc);
        if (ld && ld == rd) return lt;
        return static_cast<unsigned>(lp->float_tier) >=
                       static_cast<unsigned>(rp->float_tier)
                   ? lt
                   : rt;
      }
      if (lreal && rint) return lt;
      if (lint && rreal) return rt;
      if (lint && rint) {
        const TypeDescriptor* operand =
            registry_.descriptor_for_type(ltc);
        const TypeDescriptor* result =
            operand ? operand->real_division_result : nullptr;
        return result ? result->type : nullptr;
      }
      return nullptr;
    }
    // Pascal pointer arithmetic preserves the pointer type for `p+n`,
    // `n+p`, and `p-n`. Type deduction must keep that fact here because
    // a later dereference (`(p+n)^`) needs the pointee type; without it,
    // value intrinsics such as `Ord` cannot tell that a `pchar` element is
    // a `char`.
    const bool lptr = type_is_pointer_arithmetic_operand(ltc);
    const bool rptr = type_is_pointer_arithmetic_operand(rtc);
    const PrimitiveInfo* lpi = primitive_info_for_type(ltc);
    const PrimitiveInfo* rpi = primitive_info_for_type(rtc);
    const bool lint = lpi && lpi->int_kind != PrimitiveIntKind::None;
    const bool rint = rpi && rpi->int_kind != PrimitiveIntKind::None;
    if (b.op == BinOp::Add) {
      if (lptr && rint) return lt;
      if (rptr && lint) return rt;
      if (lptr || rptr) return nullptr;
    }
    if (b.op == BinOp::Sub) {
      if (lptr && rint) return lt;
      if (lptr && rptr) {
        const TypeDescriptor* pointer =
            registry_.descriptor_for_type(ltc);
        const TypeDescriptor* result =
            pointer ? pointer->pointer_difference_result : nullptr;
        return result ? result->type : nullptr;
      }
      if (lptr || rptr) return nullptr;
    }
    if (same_type_ast(ltc, rtc)) return ltc ? ltc : rtc;
    if (type_is_numeric_primitive(ltc) && type_is_numeric_primitive(rtc)) {
      const PrimitiveInfo* lp = primitive_info_for_type(ltc);
      const PrimitiveInfo* rp = primitive_info_for_type(rtc);
      if (lp && rp && lp->int_kind != PrimitiveIntKind::None &&
          rp->int_kind != PrimitiveIntKind::None) {
        return (primitive_bits(*lp, target_) >= primitive_bits(*rp, target_)) ? ltc : rtc;
      }
      return ltc ? ltc : rtc;
    }
  }

  // String / set binary ops. Pascal's `+`/`-`/`*` are overloaded on
  // string concatenation and set operations. Typing them here gives
  // overload ranking and later lowering one Pascal result type instead
  // of leaving each call site to rediscover the operator family.
  if (b.op == BinOp::Add &&
      (type_is_string_like(lt) || type_is_string_like(rt) ||
       b.lhs->kind == Kind::StringLit || b.rhs->kind == Kind::StringLit)) {
    const TypeDescriptor* left = registry_.descriptor_for_type(lt);
    const TypeDescriptor* right = registry_.descriptor_for_type(rt);
    if (type_is_long_string(lt) && left && left->string_concat_result) {
      return left->string_concat_result->type;
    }
    if (type_is_long_string(rt) && right && right->string_concat_result) {
      return right->string_concat_result->type;
    }
    const TypeDescriptor* result =
        left && left->string_concat_result
            ? left->string_concat_result
            : (right ? right->string_concat_result : nullptr);
    return result ? result->type : nullptr;
  }
  if (b.op == BinOp::Add || b.op == BinOp::Sub || b.op == BinOp::Mul) {
    // Set union (+) / difference (-) / intersection (*). Pascal forbids
    // mixing element types, so either typed operand anchors the whole
    // expression. The other side may be a bare `[...]` literal with no
    // type of its own; once one side is a set, we still know the result.
    if (const TypeExpr* lset = canonical_set_type(lt)) {
      return b.lhs->kind == Kind::SetLit ? lset : lt;
    }
    if (const TypeExpr* rset = canonical_set_type(rt)) {
      return b.rhs->kind == Kind::SetLit ? rset : rt;
    }
  }
  return nullptr;
}

const TypeExpr* EmitAnalysis::deduce_low_high_result_type(const TypeExpr* t) {
  const TypeExpr* canon = semantic_shape_type(t);
  if (!canon) return t;
  if (canon->kind == Kind::TySet) {
    return static_cast<const TySet&>(*canon).element.get();
  }
  if (canon->kind == Kind::TyArray) {
    const auto& arr = static_cast<const TyArray&>(*canon);
    if (arr.array_kind == ArrayKind::Fixed && !arr.dims.empty()) {
      return arr.dims.front().get();
    }
    const TypeDescriptor* descriptor = registry_.descriptor_for_type(canon);
    const TypeDescriptor* result =
        descriptor ? descriptor->low_high_result : nullptr;
    return result ? result->type : nullptr;
  }
  return t;
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_expr(
    const Expr& e, const TypeLookupContext* context) {
  if (!context) return eval_const_int_expr(e);
  ScopedDeclarationLookup declaration_scope(scope_, context, context->unit);
  return eval_const_int_expr(e);
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_expr(
    const Expr& e, std::unordered_set<std::string>* visiting_const_names) {
  const auto type_for_value = [&](int64_t value) {
    return primitive_info_for_value(registry_, value);
  };
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      if (n.value > static_cast<uint64_t>(INT64_MAX)) {
        return ConstIntExprInfo{static_cast<int64_t>(n.value), n.value,
                                ordinal_integer_primitive(
                                    registry_, PrimitiveIntKind::Unsigned,
                                    64)};
      }
      int64_t value = static_cast<int64_t>(n.value);
      return ConstIntExprInfo{value, type_for_value(value)};
    }
    case Kind::Unary: {
      const auto& u = static_cast<const Unary&>(e);
      auto operand = eval_const_int_expr(*u.operand, visiting_const_names);
      if (!operand) return std::nullopt;
      if (u.op == UnOp::Plus) return operand;
      if (u.op != UnOp::Neg) return std::nullopt;
      int64_t value = 0;
      if (!checked_sub_int64(0, operand->value, &value)) return std::nullopt;
      return ConstIntExprInfo{value, type_for_value(value)};
    }
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      auto lhs = eval_const_int_expr(*b.lhs, visiting_const_names);
      auto rhs = eval_const_int_expr(*b.rhs, visiting_const_names);
      if (!lhs || !rhs) return std::nullopt;
      int64_t value = 0;
      switch (b.op) {
        case BinOp::Add:
          if (!checked_add_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Sub:
          if (!checked_sub_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Mul:
          if (!checked_mul_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::IntDiv:
          if (!checked_div_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Mod:
          if (!checked_mod_int64(lhs->value, rhs->value, &value)) return std::nullopt;
          break;
        case BinOp::Shl:
          if (!checked_pascal_shl_int64(
                  lhs->value,
                  lhs->type,
                  rhs->value, &value, target_)) {
            return std::nullopt;
          }
          if (const PrimitiveInfo* carrier =
                  shift_carrier_primitive(registry_, lhs->type, target_)) {
            return ConstIntExprInfo{value, carrier};
          }
          return std::nullopt;
        case BinOp::Shr:
          if (!checked_pascal_shr_int64(
                  lhs->value,
                  lhs->type,
                  rhs->value, &value, target_)) {
            return std::nullopt;
          }
          if (const PrimitiveInfo* carrier =
                  shift_carrier_primitive(registry_, lhs->type, target_)) {
            return ConstIntExprInfo{value, carrier};
          }
          return std::nullopt;
        case BinOp::And:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) &
                                       static_cast<uint64_t>(rhs->value));
          break;
        case BinOp::Or:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) |
                                       static_cast<uint64_t>(rhs->value));
          break;
        case BinOp::Xor:
          value = static_cast<int64_t>(static_cast<uint64_t>(lhs->value) ^
                                       static_cast<uint64_t>(rhs->value));
          break;
        default:
          return std::nullopt;
      }
      return ConstIntExprInfo{value, type_for_value(value)};
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
        const auto& callee = static_cast<const Ident&>(*c.callee);
        const std::string callee_name = ascii_lower(callee.name);
        if (callee_name == "length" && c.args[0]->kind == Kind::StringLit) {
          const auto& s = static_cast<const StringLit&>(*c.args[0]);
          if (s.value.size() <= static_cast<size_t>(INT64_MAX)) {
            int64_t value = static_cast<int64_t>(s.value.size());
            return ConstIntExprInfo{value, type_for_value(value)};
          }
        }
        if ((callee_name == "pred" || callee_name == "succ") && c.args[0]) {
          if (auto ordinal = eval_ordinal_expr(*c.args[0])) {
            int64_t value = ordinal->value;
            if (callee_name == "pred") {
              if (!checked_sub_int64(value, 1, &value)) return std::nullopt;
            } else if (!checked_add_int64(value, 1, &value)) {
              return std::nullopt;
            }
            return ConstIntExprInfo{value, type_for_value(value)};
          }
        }
        if (callee_name == "ord" && c.args[0]) {
          if (c.args[0]->kind == Kind::Call) {
            const auto& inner = static_cast<const Call&>(*c.args[0]);
            if (inner.callee && inner.callee->kind == Kind::Ident &&
                inner.args.size() == 1 && inner.args[0]) {
              const std::string inner_name =
                  ascii_lower(static_cast<const Ident&>(*inner.callee).name);
              if (inner_name == "low" || inner_name == "high") {
                // `high(T)` normally lowers to an emitted enum-bound helper.
                // Inside `ord(...)` in a type bound, the integer folder needs
                // the ordinal value instead of that helper expression.
                if (const TypeExpr* t =
                        const_intrinsic_type_arg(*inner.args[0])) {
                  if (auto dom = ordinal_domain_for_type(t)) {
                    int64_t value =
                        (inner_name == "low") ? dom->low : dom->high;
                    return ConstIntExprInfo{value, type_for_value(value)};
                  }
                }
              }
            }
          }
          if (auto ordinal = eval_ordinal_expr(*c.args[0])) {
            return ConstIntExprInfo{
                ordinal->value, type_for_value(ordinal->value)};
          }
        }
      }
      return eval_const_int_cast(c, visiting_const_names);
    }
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      if (!visiting_const_names) {
        std::unordered_set<std::string> local_visiting;
        return eval_const_int_expr(e, &local_visiting);
      }
      if (!visiting_const_names->insert(id.name).second) return std::nullopt;
      std::optional<ConstIntExprInfo> out;

      auto lit = scope_.local_consts.find(id.name);
      if (lit != scope_.local_consts.end() && lit->second && lit->second->value) {
        out = fold_untyped_const_decl(*lit->second, visiting_const_names);
      } else {
        for (const TypeLookupContext* frame = scope_.type_scope; frame;
             frame = frame->parent) {
          if (const auto* c = scope_frame_find_const(*frame, id.name)) {
            out = fold_untyped_const_info(*c, visiting_const_names);
            break;
          }
        }
      }
      visiting_const_names->erase(id.name);
      return out;
    }
    default:
      return std::nullopt;
  }
}

const TypeExpr* EmitAnalysis::deduce_type(const Expr& e) {
  if (const TypeDescriptor* result =
          registry_.expression_result_descriptor(&e);
      result && result->type) {
    return result->type;
  }
  switch (e.kind) {
    case Kind::BoolLit:
    case Kind::RealLit:
      return nullptr;
    case Kind::IntLit:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return integer_type(info->type);
      }
      return nullptr;
    case Kind::Unary:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return integer_type(info->type);
      }
      return deduce_type(*static_cast<const Unary&>(e).operand);
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      if (b.op == BinOp::Shl || b.op == BinOp::Shr) {
        const TypeExpr* lt = deduce_type(*b.lhs);
        if (lt) lt = canonicalize_for_arithmetic(lt);
        if (const PrimitiveInfo* pi = primitive_info_for_type(lt)) {
          if (const PrimitiveInfo* carrier =
                  shift_carrier_primitive(registry_, pi, target_)) {
            return integer_type(carrier);
          }
        }
      }
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return integer_type(info->type);
      }
      return deduce_binary_expr_type(b);
    }
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      // Local variables and parameters shadow everything.
      auto lit = scope_.local_value_types.find(id.name);
      if (lit != scope_.local_value_types.end()) return lit->second;
      auto lcit = scope_.local_consts.find(id.name);
      if (lcit != scope_.local_consts.end() && lcit->second) {
        return deduce_const_decl_type(*lcit->second);
      }
      // Nested functions live in `local_nested_fns`, not `local_value_types`. Type
      // deduction still needs to see their result type so boolean expressions
      // like `if ready and flag then` lower correctly before the ident emitter
      // auto-calls a parameterless `ready`.
      auto nit = scope_.local_nested_fns.find(id.name);
      if (nit != scope_.local_nested_fns.end()) {
        const ScopeStateView::NestedFn* zero_arg = nullptr;
        for (const auto& candidate : nit->second) {
          if (!candidate.is_function || !candidate.accepts_zero_args) continue;
          if (zero_arg) return nullptr;
          zero_arg = &candidate;
        }
        return zero_arg ? zero_arg->return_type : nullptr;
      }
      if (auto untyped = scope_.local_untyped_params.find(id.name);
          untyped != scope_.local_untyped_params.end()) {
        // Untyped Pascal params are raw storage slots. Treat the identifier
        // itself as Pascal `pointer` so pointer-slot assignments/casts can
        // still apply the central coercion rules instead of falling back to
        // a naked C++ `void*` assignment.
        return untyped->second ? untyped->second->type : nullptr;
      }
      // `self` is only typed when semantic binding resolved the method owner.
      // Manufacturing a TyName from the owner spelling would hide that missing
      // binding and push method-owner lookup back into emission.
      if (id.name == "self") {
        return scope_.current_class_symbol ? scope_.current_class_symbol->type
                                           : nullptr;
      }
      if (scope_.current_fn_is_function && scope_.current_fn_result_type &&
          !scope_.current_fn_name.empty() && id.name == scope_.current_fn_name) {
        return scope_.current_fn_result_type;
      }
      if (scope_.bare_result_type && is_pascal_result_ident(id.name)) {
        return scope_.bare_result_type;
      }
      if (scope_.outer_result_type && !scope_.outer_result_name.empty() &&
          id.name == scope_.outer_result_name) {
        return scope_.outer_result_type;
      }
      if (const EnumInfoReg* info =
              local_enum_info_for_member(scope_, id.name);
          info && info->type) {
        return info->type;
      }
      // `with X do` bindings contribute fields/properties/methods from their
      // target type. The innermost active `with` binding wins.
      for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
           ++it) {
        const TypeSymbol* with_symbol = it->class_symbol;
        const ClassInfo* with_class =
            with_symbol ? with_symbol->class_info() : nullptr;
        if (with_class) {
          if (auto* f = registry_.lookup_class_field(*with_class, id.name)) {
            return f->type.get();
          }
          if (auto* p = registry_.lookup_class_property(*with_class, id.name)) {
            return p->type.get();
          }
          if (const auto* methods =
                  registry_.lookup_class_methods(*with_class, id.name)) {
            if (const TypeExpr* mt = method_set_value_result_type(methods)) {
              return mt;
            }
            return nullptr;
          }
        }
        if (const TypeExpr* rf = lookup_record_field_type_in_with(*it, id.name)) {
          return rf;
        }
      }
      // Class member lookup inside a known method body.
      if (const ClassInfo* current_class = current_class_info()) {
        if (auto* f = registry_.lookup_class_field(*current_class, id.name)) {
          return f->type.get();
        }
        if (auto* p =
                registry_.lookup_class_property(*current_class, id.name)) {
          return p->type.get();
        }
        if (const auto* methods =
                registry_.lookup_class_methods(*current_class, id.name)) {
          if (const TypeExpr* mt = method_set_value_result_type(methods)) {
            return mt;
          }
          return nullptr;
        }
      }
      // Own-unit: both interface and implementation are visible. Other units:
      // only interface exports reached through the actual `uses` chain count.
      // Do not consult the registry's global last-wins maps here; two units
      // can export the same name with different meanings and Pascal only sees
      // the units actually imported by the current unit.
      for (const TypeLookupContext* frame = scope_.type_scope; frame;
           frame = frame->parent) {
        if (const auto* v = scope_frame_find_var(*frame, id.name)) {
          return v->type.get();
        }
        if (const auto* c = scope_frame_find_const(*frame, id.name)) {
          return deduce_const_info_type(*c);
        }
        if (const auto* procs = scope_frame_find_procs(*frame, id.name)) {
          if (const TypeExpr* pt = proc_set_value_result_type(procs)) {
            return pt;
          }
          return nullptr;
        }
        if (const EnumMemberInfo* member =
                scope_frame_find_enum_member(*frame, id.name)) {
          return member->owner ? member->owner->type : nullptr;
        }
      }
      return nullptr;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      const TypeExpr* operand_raw = deduce_type(*d.operand);
      if (!operand_raw) {
        return nullptr;
      }
      const TypeExpr* t = semantic_shape_type(operand_raw);
      if (t && t->kind == Kind::TyPointer) {
        return static_cast<const TyPointer&>(*t).target.get();
      }
      return nullptr;
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (auto unit_member = resolve_unit_qualified_member(m)) {
        // `Unit.name` has already resolved the base as a unit qualifier.
        // Returning here keeps type analysis on that same symbol instead of
        // re-entering ordinary member lookup on the qualifier spelling.
        auto uit = registry_.units.find(unit_member->unit_name);
        const UnitInfo* unit =
            uit == registry_.units.end() ? nullptr : &uit->second;
        const bool own_unit =
            unit_member->unit_name == scope_.current_unit_name;
        switch (unit_member->resolved.kind) {
          case ResolvedKind::UnitVar:
            if (unit) {
              const VarInfo* var =
                  own_unit ? unit->find_var(unit_member->member_name)
                           : unit->find_export_var(unit_member->member_name);
              return var ? var->type.get() : nullptr;
            }
            return nullptr;
          case ResolvedKind::UnitConst:
            if (unit) {
              const ConstInfo* c =
                  own_unit ? unit->find_const(unit_member->member_name)
                           : unit->find_export_const(unit_member->member_name);
              return c ? deduce_const_info_type(*c) : nullptr;
            }
            return nullptr;
          case ResolvedKind::UnitProc:
            return unit_member->resolved.accepts_zero_args &&
                           unit_member->resolved.proc
                       ? unit_member->resolved.proc->return_type.get()
                       : nullptr;
          case ResolvedKind::EnumMember:
            if (const EnumMemberInfo* member =
                    registry_.lookup_enum_member_in_unit(
                        unit_member->unit_name, m.name)) {
              return member->owner ? member->owner->type : nullptr;
            }
            return nullptr;
          default:
            return nullptr;
        }
      }
      const TypeExpr* base_type = deduce_type(*m.base);
      if (const TypeExpr* rf =
              lookup_record_field_type_in_type(base_type, m.name)) {
        return rf;
      }
      const TypeSymbol* class_symbol = nullptr;
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        if (id.name == "self") {
          class_symbol = scope_.current_class_symbol;
        }
        else if (const TypeSymbol* symbol =
                     class_or_record_type_value_symbol(*m.base)) {
          class_symbol = symbol;
        }
      }
      if (!class_symbol) {
        // Chained accesses like `x.sym.name` and result-slot writes like
        // `clone.next := nil` must recover the Pascal class alias from the
        // base expression, not from the canonicalized class body node.
        if (base_type) {
          class_symbol = registry_.metaclass_target_for_type(base_type);
        }
      }
      if (!class_symbol) {
        class_symbol = deduce_class_symbol(*m.base);
      }
      const ClassInfo* class_info =
          class_symbol ? class_symbol->class_info() : nullptr;
      if (class_info) {
        if (const auto* methods =
                registry_.lookup_class_methods(*class_info, m.name)) {
          if (const TypeExpr* mt = method_set_value_result_type(methods)) {
            return mt;
          }
          return nullptr;
        }
        if (auto* pf = registry_.lookup_class_field(*class_info, m.name)) {
          return pf->type.get();
        }
        if (auto* pp = registry_.lookup_class_property(*class_info, m.name)) {
          return pp->type.get();
        }
      }
      if (const InterfaceInfo* iface_info = interface_info_for_type(base_type)) {
        // Interface methods use the same expression syntax as class methods.
        // When a parameterless method is written as `i.name`, overload
        // validation needs the method result type, not the interface type of
        // the receiver.
        if (const auto* methods =
                registry_.lookup_interface_methods(*iface_info, m.name)) {
          if (const TypeExpr* mt = method_set_value_result_type(methods)) {
            return mt;
          }
          return nullptr;
        }
      }
      if (const TypeExpr* rf =
              lookup_record_field_type_in_type(base_type, m.name)) {
        return rf;
      }
      return nullptr;
    }
    case Kind::Index: {
      const auto& ix = static_cast<const Index&>(e);
      if (ix.base->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*ix.base);
        const TypeSymbol* symbol = deduce_class_symbol(*mem.base);
        const ClassInfo* cls = symbol ? symbol->class_info() : nullptr;
        if (cls) {
          if (auto* prop = registry_.lookup_class_property(*cls, mem.name);
              prop && !prop->params.empty()) {
            return prop->type.get();
          }
        }
      }
      if (ix.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*ix.base);
        if (auto found = find_implicit_class_property(id.name);
            found && found->prop && !found->prop->params.empty()) {
          return found->prop->type.get();
        }
      }
      const TypeExpr* base_type = deduce_type(*ix.base);
      if (!base_type) return nullptr;
      const TypeDescriptor* base_descriptor =
          registry_.descriptor_for_type(base_type);
      if (base_descriptor && base_descriptor->element_result) {
        return base_descriptor->element_result->type;
      }
      const TypeExpr* bt = semantic_shape_type(base_type);
      if (bt && bt->kind == Kind::TyArray) {
        return static_cast<const TyArray&>(*bt).element.get();
      }
      if (bt && bt->kind == Kind::TyPointer) {
        return static_cast<const TyPointer&>(*bt).target.get();
      }
      {
        const TypeSymbol* cls_symbol = deduce_class_symbol(*ix.base);
        const ClassInfo* cls = cls_symbol ? cls_symbol->class_info() : nullptr;
        if (cls) {
          if (auto* prop = registry_.lookup_default_property(*cls)) {
            return prop->type.get();
          }
        }
      }
      return nullptr;
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Semantic binding has already distinguished `T(expr)` from an ordinary
      // call.
      // Preserve the selected declaration type directly; primitive aliases
      // share descriptor metadata and require no spelling dispatch here.
      if (c.args.size() == 1) {
        if (const TypeExpr* cast_type = explicit_typecast_result_type(e)) {
          return cast_type;
        }
      }
      // Intrinsics that work the same whether spelled `low(t)` or
      // `system.low(t)` are dispatched through intrinsic_call_name so the
      // Ident-callee and system-unit-Member-callee spellings share one table.
      if (auto intrinsic = intrinsic_call_name(*c.callee)) {
        const std::string& n = *intrinsic;
        if (n == "ord" && c.args.size() == 1) {
          return ord_result_type_for_operand(*c.args[0]);
        }
        if ((n == "low" || n == "high") && c.args.size() == 1) {
          if (std::optional<const TypeSymbol*> type_operand =
                  registry_.type_name_expression_result(c.args[0].get())) {
            if (const TypeSymbol* symbol = *type_operand) {
              return deduce_low_high_result_type(
                  symbol->descriptor ? symbol->descriptor->type : nullptr);
            }
          }
          return deduce_low_high_result_type(deduce_type(*c.args[0]));
        }
        if ((n == "lo" || n == "hi") && c.args.size() == 1) {
          const TypeExpr* arg_type = deduce_type(*c.args[0]);
          if (const TypeExpr* ord_type = ord_result_type_for_type(arg_type)) {
            arg_type = ord_type;
          }
          const TypeDescriptor* argument =
              registry_.descriptor_for_type(arg_type);
          const TypeDescriptor* result =
              argument ? argument->lo_hi_result : nullptr;
          if (result) return result->type;
        }
        if ((n == "succ" || n == "pred" ||
             n == "abs" || n == "sqr") &&
            c.args.size() == 1) {
          return deduce_type(*c.args[0]);
        }
      }
      const TypeExpr* callee_type = deduce_type(*c.callee);
      if (callee_type) callee_type = semantic_shape_type(callee_type);
      if (callee_type && callee_type->kind == Kind::TyProcedural) {
        const auto& p = static_cast<const TyProcedural&>(*callee_type);
        if (p.is_function) return p.return_type.get();
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        auto nit = scope_.local_nested_fns.find(id.name);
        if (nit != scope_.local_nested_fns.end() && nit->second.size() == 1 &&
            nit->second.front().is_function) {
          return nit->second.front().return_type;
        }
      }
      return call_type_provider_.type_for_resolved_call(c);
    }
    case Kind::StringLit:
      return nullptr;
    case Kind::SetLit:
      return deduce_set_literal_type(static_cast<const SetLit&>(e));
    case Kind::AddrOf:
      // Pascal `@x` yields `^T` where `T` is x's type. `@arr` is therefore
      // pointer-to-array and `@arr[0]` is pointer-to-element: same runtime
      // address but distinct C++ pointer types (different stride, different
      // type-checked APIs), so deduce_type must not conflate them.
      if (const auto& a = static_cast<const AddrOf&>(e); a.operand) {
        if (a.operand->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*a.operand);
          if (auto untyped = scope_.local_untyped_params.find(id.name);
              untyped != scope_.local_untyped_params.end()) {
            return untyped->second ? untyped->second->type : nullptr;
          }
        }
        const TypeExpr* operand_ty = deduce_type(*a.operand);
        if (operand_ty) {
          return synthesize_pointer_type(operand_ty);
        }
      }
      return nullptr;
    default:
      return nullptr;
  }
  return nullptr;
}

const TypeSymbol* EmitAnalysis::deduce_class_symbol(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self") return scope_.current_class_symbol;
    if (const TypeSymbol* symbol =
            concrete_class_symbol_for_metaclass_value(e)) {
      return symbol;
    }
  } else if (e.kind == Kind::Call) {
    if (const TypeExpr* cast_type = explicit_typecast_result_type(e)) {
      const TypeSymbol* symbol = class_symbol_for_type(cast_type);
      const ClassInfo* ci = symbol ? symbol->class_info() : nullptr;
      if (ci && ci->is_reference_type) return symbol;
    }
  }
  const TypeExpr* t = deduce_type(e);
  if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
    return target;
  }
  if (const TypeSymbol* symbol = class_symbol_for_type(t)) return symbol;
  if (const TypeExpr* canon = semantic_shape_type(t)) {
    if (const TypeSymbol* target =
            registry_.metaclass_target_for_type(canon)) {
      return target;
    }
    if (const TypeSymbol* symbol = class_symbol_for_type(canon)) {
      return symbol;
    }
  }
  return nullptr;
}

const TypeSymbol* EmitAnalysis::class_or_record_type_value_symbol(
    const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (identifier_is_shadowed_value(id.name)) return nullptr;
  } else if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (!mem.base || mem.base->kind != Kind::Ident) return nullptr;
    const auto& base = static_cast<const Ident&>(*mem.base);
    if (identifier_is_shadowed_value(base.name)) return nullptr;
  } else {
    return nullptr;
  }
  if (std::optional<const TypeSymbol*> symbol =
          registry_.value_type_expression_result(&e)) {
    const TypeSymbol* payload =
        *symbol && (*symbol)->descriptor && (*symbol)->descriptor->symbol
            ? (*symbol)->descriptor->symbol
            : *symbol;
    if (payload && (payload->class_info() || payload->record_info())) {
      return payload;
    }
  }
  return nullptr;
}

const TypeSymbol* EmitAnalysis::concrete_class_symbol_for_metaclass_value(
    const Expr& e) {
  if (const TypeSymbol* symbol = class_or_record_type_value_symbol(e)) {
    const ClassInfo* ci = symbol->class_info();
    if (ci && ci->is_reference_type) return symbol;
  }
  return nullptr;
}

const TypeSymbol* EmitAnalysis::concrete_class_symbol_for_metaclass_target(
    const Expr& e, const TypeExpr* target) {
  const TypeSymbol* concrete = concrete_class_symbol_for_metaclass_value(e);
  const TypeSymbol* target_symbol = metaclass_target_symbol(target);
  const ClassInfo* concrete_class =
      concrete ? concrete->class_info() : nullptr;
  const ClassInfo* target_class =
      target_symbol ? target_symbol->class_info() : nullptr;
  if (!concrete_class || !target_class ||
      !concrete_class->is_reference_type || !target_class->is_reference_type) {
    return nullptr;
  }

  std::unordered_set<const ClassInfo*> seen;
  for (const ClassInfo* cur = concrete_class; cur;
       cur = registry_.lookup_parent_class(*cur)) {
    if (!seen.insert(cur).second) break;
    if (registry_.same_class_identity(*cur, *target_class)) {
      return concrete;
    }
  }
  return nullptr;
}

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!type) return nullptr;
  const std::string lower_field_name = ascii_lower(std::string(field_name));
  if (const RecordInfo* record =
          record_info_for_resolved_type(registry_, type)) {
    auto it = record->fields.find(lower_field_name);
    return it == record->fields.end() ? nullptr : it->second.type.get();
  }
  type = semantic_shape_type(type);
  if (!type) return nullptr;
  if (type->kind == Kind::TyPointer) {
    return lookup_record_field_type_in_type(
        static_cast<const TyPointer&>(*type).target.get(), field_name);
  }
  return nullptr;
}

bool EmitAnalysis::record_field_is_variant_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!type) return false;
  const std::string lower_field_name = ascii_lower(std::string(field_name));
  if (const RecordInfo* record =
          record_info_for_resolved_type(registry_, type)) {
    auto it = record->fields.find(lower_field_name);
    return it != record->fields.end() && it->second.is_variant;
  }
  type = semantic_shape_type(type);
  if (!type) return false;
  if (type->kind == Kind::TyPointer) {
    return record_field_is_variant_in_type(
        static_cast<const TyPointer&>(*type).target.get(), field_name);
  }
  return false;
}

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_with(
    const ScopeStateView::WithBind& wb, std::string_view field_name) {
  if (const TypeExpr* ft = lookup_record_field_type_in_type(
          wb.type, field_name)) {
    return ft;
  }
  return nullptr;
}

bool EmitAnalysis::with_bind_has_visible_member(
    const ScopeStateView::WithBind& wb, std::string_view name) {
  const TypeSymbol* symbol = wb.class_symbol;
  const ClassInfo* cls = symbol ? symbol->class_info() : nullptr;
  if (cls) {
    const std::string member(name);
    if (registry_.lookup_class_methods(*cls, member) ||
        registry_.lookup_class_field(*cls, member) ||
        registry_.lookup_class_property(*cls, member)) {
      return true;
    }
  }
  return lookup_record_field_type_in_with(wb, name) != nullptr;
}

bool EmitAnalysis::identifier_is_shadowed_value(std::string_view name_in) {
  const std::string name = ascii_lower(name_in);
  // Unit qualifiers live in the same syntactic slot as ordinary values:
  // `foo.bar` may mean a field/method of value `foo`, or symbol `bar` from
  // unit `foo`. Pascal lexical lookup gives nearer values precedence, so a
  // local, current-class member, or active `with` member named `foo` blocks the
  // unit interpretation.
  if (scope_.local_scope.count(name) > 0) return true;
  if (const ClassInfo* current_class = current_class_info()) {
    if (registry_.lookup_class_methods(*current_class, name) ||
        registry_.lookup_class_field(*current_class, name) ||
        registry_.lookup_class_property(*current_class, name)) {
      return true;
    }
  }
  for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
       ++it) {
    if (with_bind_has_visible_member(*it, name)) return true;
  }
  return false;
}

bool EmitAnalysis::is_visible_unit_qualifier(std::string_view name_in) {
  const std::string name = ascii_lower(name_in);
  // Pascal imports System implicitly. tp2cc registers that surface as the
  // synthetic `__rt__` unit so unqualified runtime lookup and `System.name`
  // qualification use the same symbol table.
  const std::string wanted = (name == "system") ? "__rt__" : name;
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (frame->unit_info && frame->unit == wanted) return true;
  }
  return false;
}

std::optional<UnitQualifiedMemberLookup>
EmitAnalysis::resolve_unit_qualified_member(const ast::Member& mem) {
  if (!mem.base || mem.base->kind != Kind::Ident) {
    return std::nullopt;
  }
  const auto& id = static_cast<const Ident&>(*mem.base);
  const std::string unit_name = ascii_lower(id.name);
  if (identifier_is_shadowed_value(unit_name) ||
      !is_visible_unit_qualifier(unit_name)) {
    return std::nullopt;
  }
  const std::string lookup_unit_name =
      (unit_name == "system") ? "__rt__" : unit_name;

  // From here the base identifier is a unit qualifier, not a value expression.
  // Call the normal qualified-name resolver once and hand that result to value,
  // call, type, and storage consumers instead of letting each consumer recurse
  // into the qualifier and rediscover the same rule.
  ResolveResult rr =
      resolve_name_provider_.resolve_name(mem.name, QualifierKind::Unit,
                                          lookup_unit_name);
  return UnitQualifiedMemberLookup{lookup_unit_name, mem.name, rr};
}

std::optional<std::string>
EmitAnalysis::intrinsic_call_name(const Expr& callee) {
  if (callee.kind == Kind::Ident) {
    return static_cast<const Ident&>(callee).name;
  }
  if (callee.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(callee);
    if (!mem.base || mem.base->kind != Kind::Ident) {
      return std::nullopt;
    }
    const auto& id = static_cast<const Ident&>(*mem.base);
    if (id.name != "system") {
      return std::nullopt;
    }
    if (const auto system_member = resolve_unit_qualified_member(mem);
        system_member && system_member->unit_name == "__rt__") {
      return mem.name;
    }
  }
  return std::nullopt;
}

const VarInfo* EmitAnalysis::find_visible_unit_var(const std::string& name) {
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (const auto* v = scope_frame_find_var(*frame, name)) return v;
  }
  return nullptr;
}

const ConstInfo* EmitAnalysis::find_visible_unit_const(const std::string& name) {
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (const auto* c = scope_frame_find_const(*frame, name)) return c;
  }
  return nullptr;
}

const EnumInfoReg* EmitAnalysis::find_enum_info_in_unit(
    std::string_view unit_name, std::string_view member_name) {
  const EnumMemberInfo* member =
      registry_.lookup_enum_member_in_unit(unit_name, member_name);
  return member ? member->owner : nullptr;
}

const EnumMemberInfo* EmitAnalysis::find_visible_enum_member(
    const std::string& name) {
  if (const EnumMemberInfo* local = local_enum_member(scope_, name)) {
    return local;
  }
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (const EnumMemberInfo* member =
            scope_frame_find_enum_member(*frame, name)) {
      return member;
    }
  }
  return nullptr;
}

const EnumInfoReg* EmitAnalysis::find_visible_enum_info_for_member(
    const std::string& name) {
  const EnumMemberInfo* member = find_visible_enum_member(name);
  return member ? member->owner : nullptr;
}

std::string EmitAnalysis::implicit_self_cxx() {
  if (const ClassInfo* ci = current_class_info()) {
    // Old object values use `(*this)` while reference classes use `this`.
    // Property lowering asks this helper instead of re-deriving that split.
    return ci->is_reference_type ? "this" : "(*this)";
  }
  return "(*this)";
}

std::string EmitAnalysis::implicit_self_access() {
  if (const ClassInfo* ci = current_class_info()) {
    return ci->is_reference_type ? "->" : ".";
  }
  return ".";
}

std::optional<ImplicitPropertyLookup> EmitAnalysis::find_implicit_class_property(
    std::string_view name) {
  if (scope_.local_scope.count(std::string(name))) return std::nullopt;

  // `with` bindings shadow the ambient class scope for property lookup just as
  // they do for fields/methods, so search them from innermost to outermost.
  for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend(); ++it) {
    const TypeSymbol* symbol = it->class_symbol;
    const ClassInfo* cls = symbol ? symbol->class_info() : nullptr;
    if (!cls) continue;
    if (auto* prop =
            registry_.lookup_class_property(*cls, std::string(name))) {
      return ImplicitPropertyLookup{prop, it->cxx_text, it->access_op, true};
    }
  }

  // Fall back to the current class only after locals and `with` scopes have
  // been ruled out; otherwise a same-named local would spuriously become a
  // property access.
  const ClassInfo* current_class = current_class_info();
  if (!current_class) return std::nullopt;
  if (auto* prop =
          registry_.lookup_class_property(*current_class, std::string(name))) {
    return ImplicitPropertyLookup{prop, implicit_self_cxx(),
                                  implicit_self_access(), false};
  }
  return std::nullopt;
}

}  // namespace tp2cc
