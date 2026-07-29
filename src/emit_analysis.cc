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

static bool type_is_typed_pointer_arithmetic_operand(const TypeExpr* t) {
  return t && t->kind == Kind::TyPointer &&
         static_cast<const TyPointer&>(*t).target;
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
  const TypeSymbol* symbol =
      migration_type_symbol_for_expression(*c.callee);
  return symbol && symbol->descriptor && symbol->descriptor->symbol
             ? symbol->descriptor->symbol
             : symbol;
}

const TypeExpr* EmitAnalysis::explicit_typecast_result_type(const Expr& e) {
  if (e.kind != Kind::Call) return nullptr;
  const auto& c = static_cast<const Call&>(e);
  if (c.args.size() != 1 || !c.callee) return nullptr;

  if (const TypeSymbol* symbol =
          migration_type_symbol_for_expression(*c.callee)) {
    // Metaclass descriptors are target-keyed and deliberately own no syntax
    // node. The selected declaration still carries the bound TyMetaclass
    // expression needed by expression typing. Ordinary aliases likewise keep
    // their bound TyName here while sharing the target descriptor.
    if (symbol->type) return symbol->type;
    return symbol->descriptor ? symbol->descriptor->type : nullptr;
  }

  return nullptr;
}

const TypeSymbol* EmitAnalysis::migration_type_symbol_for_expression(
    const Expr& e) {
  return resolve_name_provider_.migration_type_symbol_for_expression(e);
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
  // A required binary constant is reified as a new Pascal integer literal;
  // its initial type follows the evaluated value, not the carrier that an
  // executable occurrence of the same operator would use. Other expression
  // forms keep their resolved declaration identity, notably Low/High of
  // distinct and subrange types.
  if (cd.value->kind == Kind::Binary) {
    auto info = eval_const_int_expr(*cd.value);
    if (info && info->type) return integer_type(info->type);
  }
  if (const TypeExpr* type = deduce_type(*cd.value)) return type;
  auto info = eval_const_int_expr(*cd.value);
  if (info && info->type) return integer_type(info->type);
  if (cd.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*cd.value));
  }
  return nullptr;
}

const TypeExpr* EmitAnalysis::deduce_const_info_type(const ConstInfo& c) {
  if (c.type) return c.type.get();
  if (!c.value) return nullptr;
  if (c.value->kind == Kind::Binary) {
    auto info = eval_const_int_expr(*c.value);
    if (info && info->type) return integer_type(info->type);
  }
  if (const TypeExpr* type = deduce_type(*c.value)) return type;
  auto info = eval_const_int_expr(*c.value);
  if (info && info->type) return integer_type(info->type);
  if (c.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*c.value));
  }
  return nullptr;
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

bool EmitAnalysis::type_is_native_pointer_value(const TypeExpr* type) {
  const TypeExpr* shape = semantic_shape_type(type);
  return type &&
         (type_is_reference_class(type) || type_is_interface(type) ||
          type_accepts_class_value(type) ||
          (shape && shape->kind == Kind::TyPointer));
}

bool EmitAnalysis::fixed_array_pointer_can_decay_to_element_pointer(
    const TypeExpr* source, const TypeExpr* target) {
  const TypeExpr* source_shape = semantic_shape_type(source);
  const TypeExpr* target_shape = semantic_shape_type(target);
  if (!source_shape || source_shape->kind != Kind::TyPointer ||
      !target_shape || target_shape->kind != Kind::TyPointer) {
    return false;
  }
  const TypeExpr* source_pointee = semantic_shape_type(
      static_cast<const TyPointer&>(*source_shape).target.get());
  const TypeExpr* target_pointee = semantic_shape_type(
      static_cast<const TyPointer&>(*target_shape).target.get());
  if (!source_pointee || source_pointee->kind != Kind::TyArray ||
      !target_pointee) {
    return false;
  }
  const auto& array = static_cast<const TyArray&>(*source_pointee);
  return array.array_kind == ArrayKind::Fixed && array.element &&
         same_type_ast(array.element.get(), target_pointee);
}

PointerConversionKind EmitAnalysis::classify_pointer_conversion(
    const TypeExpr* target, const TypeExpr* source,
    bool explicit_pascal_cast) {
  const TypeExpr* raw_target = target;
  const TypeExpr* raw_source = source;
  const TypeDescriptor* target_descriptor =
      registry_.descriptor_for_type(raw_target);
  const TypeDescriptor* source_descriptor =
      registry_.descriptor_for_type(raw_source);
  if (target_descriptor && target_descriptor == source_descriptor) {
    return PointerConversionKind::Identity;
  }

  target = semantic_shape_type(target);
  source = semantic_shape_type(source);
  if (!target || !source) return PointerConversionKind::Incompatible;

  const auto is_plain_pointer = [](const TypeExpr* type) {
    return type && type->kind == Kind::TyPointer &&
           !static_cast<const TyPointer&>(*type).target;
  };
  const auto is_plain_procedure = [](const TypeExpr* type) {
    return type && type->kind == Kind::TyProcedural &&
           !static_cast<const TyProcedural&>(*type).is_method;
  };
  const PrimitiveInfo* target_primitive = primitive_info_for_type(target);
  const PrimitiveInfo* source_primitive = primitive_info_for_type(source);
  const bool target_integer =
      target_primitive &&
      target_primitive->int_kind != PrimitiveIntKind::None;
  const bool source_integer =
      source_primitive &&
      source_primitive->int_kind != PrimitiveIntKind::None;
  const bool target_procedure = is_plain_procedure(target);
  const bool source_procedure = is_plain_procedure(source);
  const bool target_pointer = type_is_native_pointer_value(target);
  const bool source_pointer = type_is_native_pointer_value(source);
  if (explicit_pascal_cast && target_pointer && source_integer) {
    return PointerConversionKind::IntegerToPointer;
  }
  if (explicit_pascal_cast && target_integer && source_pointer) {
    return resolved_primitive_bits(*target_primitive) >= target_.pointer_bits
               ? PointerConversionKind::PointerToInteger
               : PointerConversionKind::Incompatible;
  }
  if (!(target_pointer || target_procedure) ||
      !(source_pointer || source_procedure)) {
    return PointerConversionKind::Incompatible;
  }
  if (same_type_ast(raw_target, raw_source)) {
    return PointerConversionKind::Identity;
  }

  if (fixed_array_pointer_can_decay_to_element_pointer(source, target)) {
    return PointerConversionKind::FixedArrayToElement;
  }

  auto pointer_target_class = [&](const TypeExpr* pointer) {
    if (!pointer || pointer->kind != Kind::TyPointer) {
      return static_cast<const ClassInfo*>(nullptr);
    }
    const TypeExpr* pointee =
        static_cast<const TyPointer&>(*pointer).target.get();
    if (!pointee) return static_cast<const ClassInfo*>(nullptr);
    return class_info_for_type(pointee);
  };
  auto hierarchy_conversion = [&](const ClassInfo* target_class,
                                   const ClassInfo* source_class) {
    return target_class && source_class &&
           (registry_.class_ancestor_depth(*target_class, *source_class) >= 0 ||
            (explicit_pascal_cast &&
             registry_.class_ancestor_depth(*source_class, *target_class) >=
                 0));
  };

  const ClassInfo* target_object = pointer_target_class(target);
  const ClassInfo* source_object = pointer_target_class(source);
  if (target_object && source_object && !target_object->is_reference_type &&
      !source_object->is_reference_type &&
      hierarchy_conversion(target_object, source_object)) {
    return PointerConversionKind::Hierarchy;
  }

  const ClassInfo* target_class = class_info_for_type(raw_target);
  const ClassInfo* source_class = class_info_for_type(raw_source);
  const InterfaceInfo* target_interface = interface_info_for_type(raw_target);
  if (target_interface && source_class &&
      source_class->is_reference_type &&
      registry_.class_implements_interface(*source_class,
                                            *target_interface)) {
    return PointerConversionKind::Hierarchy;
  }
  if (target_class && source_class && target_class->is_reference_type &&
      source_class->is_reference_type &&
      hierarchy_conversion(target_class, source_class)) {
    return PointerConversionKind::Hierarchy;
  }

  const TypeSymbol* target_metaclass = metaclass_target_symbol(raw_target);
  const TypeSymbol* source_metaclass = metaclass_target_symbol(raw_source);
  if (target_metaclass && source_metaclass) {
    const ClassInfo* target_metaclass_info = target_metaclass->class_info();
    const ClassInfo* source_metaclass_info = source_metaclass->class_info();
    if (hierarchy_conversion(target_metaclass_info,
                             source_metaclass_info)) {
      return PointerConversionKind::Hierarchy;
    }
  }

  const bool target_untyped = is_plain_pointer(target);
  const bool source_untyped = is_plain_pointer(source);
  if (target_procedure && source_untyped) {
    return PointerConversionKind::UntypedPointerToProcedure;
  }
  if (target_untyped && source_procedure) {
    return PointerConversionKind::ProcedureToUntypedPointer;
  }
  if (target_untyped) return PointerConversionKind::ToUntypedPointer;
  if (source_untyped) return PointerConversionKind::FromUntypedPointer;

  return explicit_pascal_cast ? PointerConversionKind::Reinterpret
                              : PointerConversionKind::Incompatible;
}

std::optional<TargetPointerArithmeticOperation>
EmitAnalysis::bind_target_pointer_arithmetic(
    const Expr& expression, const TypeExpr* target) {
  const TypeExpr* target_shape = semantic_shape_type(target);
  if (!type_is_typed_pointer_arithmetic_operand(target_shape) ||
      expression.kind != Kind::Binary) {
    return std::nullopt;
  }
  const auto& binary = static_cast<const Binary&>(expression);
  if ((binary.op != BinOp::Add && binary.op != BinOp::Sub) ||
      !binary.lhs || !binary.rhs) {
    return std::nullopt;
  }

  const TypeExpr* lhs_type = deduce_type(*binary.lhs);
  const TypeExpr* rhs_type = deduce_type(*binary.rhs);
  const PrimitiveInfo* lhs_primitive = primitive_info_for_type(lhs_type);
  const PrimitiveInfo* rhs_primitive = primitive_info_for_type(rhs_type);
  const bool lhs_integer =
      lhs_primitive &&
      lhs_primitive->int_kind != PrimitiveIntKind::None;
  const bool rhs_integer =
      rhs_primitive &&
      rhs_primitive->int_kind != PrimitiveIntKind::None;
  const bool pointer_on_lhs =
      binary.lhs->kind == Kind::AddrOf && rhs_integer;
  const bool pointer_on_rhs =
      binary.op == BinOp::Add && lhs_integer &&
      binary.rhs->kind == Kind::AddrOf;
  if (!pointer_on_lhs && !pointer_on_rhs) return std::nullopt;

  const Expr* pointer = pointer_on_lhs ? binary.lhs.get() : binary.rhs.get();
  const Expr* delta = pointer_on_lhs ? binary.rhs.get() : binary.lhs.get();
  const TypeExpr* pointer_source =
      pointer_on_lhs ? lhs_type : rhs_type;
  if (classify_pointer_conversion(target, pointer_source,
                                  /*explicit_pascal_cast=*/true) ==
      PointerConversionKind::Incompatible) {
    return std::nullopt;
  }
  const TypeDescriptor* pointer_type = registry_.descriptor_for_type(target);
  const TypeDescriptor* delta_type =
      pointer_type ? pointer_type->pointer_difference_result : nullptr;
  if (!pointer_type || !delta_type || !delta_type->type) return std::nullopt;
  return TargetPointerArithmeticOperation{
      .pointer = pointer,
      .delta = delta,
      .pointer_type = pointer_type,
      .delta_type = delta_type,
      .op = binary.op,
  };
}

const TypeDescriptor* EmitAnalysis::common_pointer_comparison_type(
    const TypeExpr* lhs, const TypeDescriptor* lhs_descriptor,
    const TypeExpr* rhs, const TypeDescriptor* rhs_descriptor) {
  if (!lhs_descriptor && !rhs_descriptor) {
    const TypeSymbol* pointer = registry_.builtin_literal("pointer");
    return pointer ? pointer->descriptor : nullptr;
  }
  if (!lhs_descriptor) return rhs_descriptor;
  if (!rhs_descriptor) return lhs_descriptor;
  if (lhs_descriptor == rhs_descriptor) return lhs_descriptor;

  const TypeSymbol* lhs_metaclass = lhs_descriptor->metaclass_target;
  const TypeSymbol* rhs_metaclass = rhs_descriptor->metaclass_target;
  if (lhs_metaclass || rhs_metaclass) {
    if (!lhs_metaclass || !rhs_metaclass) return nullptr;
    const ClassInfo* lhs_class = lhs_metaclass->class_info();
    const ClassInfo* rhs_class = rhs_metaclass->class_info();
    if (!lhs_class || !rhs_class) return nullptr;
    if (registry_.class_ancestor_depth(*lhs_class, *rhs_class) >= 0) {
      return lhs_descriptor;
    }
    if (registry_.class_ancestor_depth(*rhs_class, *lhs_class) >= 0) {
      return rhs_descriptor;
    }
    return nullptr;
  }

  if (classify_pointer_conversion(lhs, rhs,
                                  /*explicit_pascal_cast=*/false) !=
      PointerConversionKind::Incompatible) {
    return lhs_descriptor;
  }
  if (classify_pointer_conversion(rhs, lhs,
                                  /*explicit_pascal_cast=*/false) !=
      PointerConversionKind::Incompatible) {
    return rhs_descriptor;
  }
  return nullptr;
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
      if (std::optional<int64_t> ordinal = enum_member_ordinal(*member)) {
        return OrdinalExprValue{*ordinal, OrdinalFamily::Enum,
                                member->owner->descriptor};
      }
      return std::nullopt;
    }
    if (const TypeExpr* type = deduce_type(e)) {
      const std::optional<OrdinalDomain> domain =
          ordinal_domain_for_type(type);
      const std::optional<ConstIntExprInfo> value =
          domain ? eval_const_int_expr(e) : std::nullopt;
      if (domain && value) {
        // An untyped constant initialized from an enum label retains that
        // enum's Pascal type. Set bounds such as `First..LastAlias` therefore
        // have one enum domain even though the alias's stored value is an
        // integer ordinal.
        return OrdinalExprValue{value->value, domain->family,
                                domain->enum_key};
      }
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
        if (std::optional<int64_t> ordinal = enum_member_ordinal(*member)) {
          return OrdinalExprValue{*ordinal, OrdinalFamily::Enum,
                                  member->owner->descriptor};
        }
        return std::nullopt;
      }
    }
  }
  if (e.kind == Kind::Call) {
    const auto& call = static_cast<const Call&>(e);
    if (call.callee && call.args.size() == 1 && call.args[0]) {
      const std::optional<std::string> intrinsic =
          intrinsic_call_name(*call.callee);
      const std::string name = intrinsic ? *intrinsic : std::string{};
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
        const BoundIntrinsicOperation* operation =
            bind_intrinsic_operation(call);
        std::optional<ConstIntExprInfo> adjusted =
            operation ? eval_bound_ordinal_step(call, *operation, nullptr,
                                                 nullptr)
                      : std::nullopt;
        std::optional<OrdinalExprValue> source =
            eval_ordinal_expr(*call.args[0]);
        if (adjusted && source) {
          source->value = adjusted->value;
          return source;
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

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_low_high(
    const TypeExpr* type, bool want_low) {
  if (!type) return std::nullopt;
  const TypeExpr* shape = semantic_shape_type(type);
  const PrimitiveInfo* primitive = primitive_info_for_type(type);
  if (primitive) {
    if (primitive->kind == PrimitiveKind::Boolean) {
      const uint64_t bits = want_low ? 0 : 1;
      return ConstIntExprInfo{static_cast<int64_t>(bits), bits, primitive};
    }
    if (primitive->kind == PrimitiveKind::Char) {
      const uint64_t bits = want_low ? 0 : 255;
      return ConstIntExprInfo{static_cast<int64_t>(bits), bits, primitive};
    }
    if (primitive->kind == PrimitiveKind::WideChar) {
      const uint64_t bits = want_low ? 0 : 65535;
      return ConstIntExprInfo{static_cast<int64_t>(bits), bits, primitive};
    }
    if (primitive->int_kind != PrimitiveIntKind::None) {
      const uint8_t width = resolved_primitive_bits(*primitive);
      if (primitive->int_kind == PrimitiveIntKind::Unsigned) {
        const uint64_t bits =
            want_low ? 0 : unsigned_mask_for_bits(width);
        return ConstIntExprInfo{static_cast<int64_t>(bits), bits, primitive};
      }
      const int64_t value =
          want_low ? signed_min_for_bits(width) : signed_max_for_bits(width);
      return ConstIntExprInfo{value, static_cast<uint64_t>(value), primitive};
    }
  }
  if (shape && shape->kind == Kind::TySet) {
    return eval_const_low_high(
        static_cast<const TySet&>(*shape).element.get(), want_low);
  }
  if (std::optional<OrdinalDomain> domain =
          ordinal_domain_for_type(type)) {
    const int64_t value = want_low ? domain->low : domain->high;
    return ConstIntExprInfo{
        value, primitive_info_for_value(registry_, value)};
  }
  return std::nullopt;
}

std::optional<int64_t> EmitAnalysis::enum_member_ordinal(
    const EnumMemberInfo& member) {
  if (!member.owner || !member.owner->type ||
      member.member_index >= member.owner->members.size()) {
    return std::nullopt;
  }
  auto [it, inserted] = enum_ordinal_cache_.try_emplace(
      member.owner, member.owner->members.size());
  if (inserted || !it->second[member.member_index]) {
    evaluate_enum_ordinals(*member.owner);
  }
  return it->second[member.member_index];
}

void EmitAnalysis::evaluate_enum_ordinals(const EnumInfoReg& info) {
  auto found = enum_ordinal_cache_.find(&info);
  if (found == enum_ordinal_cache_.end() || !info.type) return;
  if (!enums_being_evaluated_.insert(&info).second) return;

  std::vector<std::optional<int64_t>>& values = found->second;
  const TypeLookupContext* context =
      registry_.lookup_context_for_type(info.type);
  int64_t previous = -1;
  bool have_previous = false;
  for (size_t index = 0;
       index < info.type->members.size() && index < values.size(); ++index) {
    if (!values[index]) {
      const EnumMember& member = info.type->members[index];
      if (member.value) {
        std::optional<ConstIntExprInfo> value =
            eval_const_int_expr(*member.value, context);
        if (!value || (value->type &&
                       value->type->int_kind == PrimitiveIntKind::Unsigned &&
                       value->bits >
                           static_cast<uint64_t>(
                               std::numeric_limits<int64_t>::max()))) {
          break;
        }
        values[index] = value->value;
      } else if (!have_previous) {
        values[index] = 0;
      } else {
        int64_t next = 0;
        if (!checked_add_int64(previous, 1, &next)) break;
        values[index] = next;
      }
    }
    if (!values[index]) break;
    if (have_previous && *values[index] <= previous) {
      const EnumMember& member = info.type->members[index];
      report_error(member.value ? member.value->loc : info.type->loc,
                   "enum values must be strictly ascending");
    }
    previous = *values[index];
    have_previous = true;
  }
  enums_being_evaluated_.erase(&info);
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
    const EnumInfoReg* info =
        en.descriptor ? en.descriptor->enum_info() : nullptr;
    if (info && !info->members.empty()) {
      const EnumMemberInfo first{info, 0};
      const EnumMemberInfo last{info, info->members.size() - 1};
      std::optional<int64_t> low = enum_member_ordinal(first);
      std::optional<int64_t> high = enum_member_ordinal(last);
      if (low && high) {
        return OrdinalDomain{.family = OrdinalFamily::Enum,
                             .low = *low,
                             .high = *high,
                             .enum_key = t->descriptor};
      }
      return std::nullopt;
    }
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
  const TypeExpr* set_target = canonical_set_type(target);
  if (s.result_descriptor && s.result_descriptor->type &&
      canonical_set_type(s.result_descriptor->type)) {
    const TypeExpr* inferred = s.result_descriptor->type;
    if (!set_target) return inferred;
    return classify_set_conversion(inferred, set_target) !=
                   SetConversionKind::Incompatible
               ? set_target
               : nullptr;
  }
  if (s.elements.empty()) return set_target;

  const TypeExpr* inferred = nullptr;
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
    inferred = synthesize_set_type(
        element_type, std::make_pair(domain->low, domain->high));
  } else {
    const TypeExpr* element_type = summary->element_type;
    if (!element_type) return nullptr;
    inferred = synthesize_set_type(
        element_type, std::make_pair(summary->low, summary->high));
  }
  if (!inferred) return nullptr;
  s.result_descriptor = registry_.descriptor_for_type(inferred);
  if (!set_target) return inferred;
  if (classify_set_conversion(inferred, set_target) ==
      SetConversionKind::Incompatible) {
    return nullptr;
  }
  return set_target;
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
  const bool fits = const_int_fits_primitive(value, *info);
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
    std::unordered_set<std::string>* visiting_const_names,
    bool* overflow) {
  if (c.args.size() != 1) return std::nullopt;
  const TypeSymbol* symbol = explicit_typecast_target_symbol(c);
  const TypeDescriptor* descriptor = symbol ? symbol->descriptor : nullptr;
  const PrimitiveInfo* primitive = descriptor ? descriptor->primitive : nullptr;
  const TypeExpr* cast_type =
      primitive && primitive->int_kind != PrimitiveIntKind::None
          ? descriptor->type
          : nullptr;
  if (!cast_type) return std::nullopt;
  auto arg =
      eval_const_int_expr_impl(*c.args[0], visiting_const_names, overflow);
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
  if (const TypeSymbol* type_operand =
          migration_type_symbol_for_expression(arg)) {
    return type_operand->descriptor ? type_operand->descriptor->type : nullptr;
  }
  if (arg.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(arg);
    if (const TypeSymbol* record_symbol =
            migration_class_or_record_type_value_symbol(*m.base)) {
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
    std::unordered_set<std::string>* visiting_const_names,
    bool* overflow) {
  // Untyped `const X = ...` is a compile-time constant. Typed
  // `const X: T = ...` has storage identity, so folding later references
  // through the initializer would erase aliasing/address semantics.
  if (cd.type || !cd.value) return std::nullopt;
  return eval_const_int_expr_impl(*cd.value, visiting_const_names, overflow);
}

std::optional<ConstIntExprInfo> EmitAnalysis::fold_untyped_const_info(
    const ConstInfo& c,
    std::unordered_set<std::string>* visiting_const_names,
    bool* overflow) {
  if (c.type || !c.value) return std::nullopt;
  return eval_const_int_expr_impl(*c.value, visiting_const_names, overflow);
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

const PrimitiveInfo* EmitAnalysis::default_integer_primitive(
    PrimitiveIntKind kind) const {
  return ordinal_integer_primitive(registry_, kind, target_.pointer_bits);
}

const PrimitiveInfo* EmitAnalysis::common_integer_primitive(
    const PrimitiveInfo& lhs, const PrimitiveInfo& rhs) const {
  const uint8_t lhs_bits = primitive_bits(lhs, target_);
  const uint8_t rhs_bits = primitive_bits(rhs, target_);
  if (lhs.int_kind == rhs.int_kind) {
    return ordinal_integer_primitive(
        registry_, lhs.int_kind, std::max(lhs_bits, rhs_bits));
  }

  const PrimitiveInfo& signed_type =
      lhs.int_kind == PrimitiveIntKind::Signed ? lhs : rhs;
  const PrimitiveInfo& unsigned_type =
      lhs.int_kind == PrimitiveIntKind::Unsigned ? lhs : rhs;
  const uint8_t signed_bits = primitive_bits(signed_type, target_);
  const uint8_t unsigned_bits = primitive_bits(unsigned_type, target_);
  if (signed_bits > unsigned_bits) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed,
                                     signed_bits);
  }
  if (unsigned_bits < 64) {
    const uint8_t result_bits =
        unsigned_bits < 8 ? 8
        : unsigned_bits < 16 ? 16
        : unsigned_bits < 32 ? 32
                             : 64;
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed,
                                     result_bits);
  }

  // No signed 64-bit carrier contains the whole UInt64 domain. Callers that
  // require signed-64 precedence handle it before asking for a common range.
  return ordinal_integer_primitive(registry_, PrimitiveIntKind::Unsigned, 64);
}

const PrimitiveInfo* EmitAnalysis::integer_arithmetic_primitive(
    BinOp op, const PrimitiveInfo& lhs, const PrimitiveInfo& rhs) const {
  const uint8_t lhs_bits = primitive_bits(lhs, target_);
  const uint8_t rhs_bits = primitive_bits(rhs, target_);
  if ((lhs.int_kind == PrimitiveIntKind::Signed && lhs_bits == 64) ||
      (rhs.int_kind == PrimitiveIntKind::Signed && rhs_bits == 64)) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed, 64);
  }
  if ((lhs.int_kind == PrimitiveIntKind::Unsigned && lhs_bits == 64) ||
      (rhs.int_kind == PrimitiveIntKind::Unsigned && rhs_bits == 64)) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Unsigned,
                                     64);
  }

  // On a 32-bit target, mixing the native unsigned carrier with a signed
  // operand, or subtracting native unsigned operands, requires the next
  // larger signed carrier. On a 64-bit target the native unsigned carrier was
  // handled by the 64-bit case above.
  const bool lhs_native_unsigned =
      lhs.int_kind == PrimitiveIntKind::Unsigned &&
      lhs_bits == target_.pointer_bits;
  const bool rhs_native_unsigned =
      rhs.int_kind == PrimitiveIntKind::Unsigned &&
      rhs_bits == target_.pointer_bits;
  if (target_.pointer_bits == 32 &&
      (lhs_native_unsigned || rhs_native_unsigned) &&
      (lhs.int_kind == PrimitiveIntKind::Signed ||
       rhs.int_kind == PrimitiveIntKind::Signed || op == BinOp::Sub)) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed, 64);
  }

  // Ordinary integer arithmetic promotes to the target default integer
  // carrier. Subtraction is signed even when both narrower operands are
  // unsigned; addition/multiplication stay unsigned only when both operands
  // are unsigned.
  if (lhs.int_kind == PrimitiveIntKind::Signed ||
      rhs.int_kind == PrimitiveIntKind::Signed || op == BinOp::Sub) {
    return default_integer_primitive(PrimitiveIntKind::Signed);
  }
  return default_integer_primitive(PrimitiveIntKind::Unsigned);
}

const PrimitiveInfo* EmitAnalysis::integer_bitwise_primitive(
    BinOp op, const PrimitiveInfo& lhs, const PrimitiveInfo& rhs) const {
  if (lhs.kind == PrimitiveKind::Currency ||
      rhs.kind == PrimitiveKind::Currency) {
    return nullptr;
  }
  const uint8_t lhs_bits = primitive_bits(lhs, target_);
  const uint8_t rhs_bits = primitive_bits(rhs, target_);
  if (op == BinOp::And) {
    if (lhs.int_kind != rhs.int_kind) {
      const PrimitiveInfo& signed_type =
          lhs.int_kind == PrimitiveIntKind::Signed ? lhs : rhs;
      const PrimitiveInfo& unsigned_type =
          lhs.int_kind == PrimitiveIntKind::Unsigned ? lhs : rhs;
      const uint8_t signed_bits = primitive_bits(signed_type, target_);
      const uint8_t unsigned_bits = primitive_bits(unsigned_type, target_);
      if (unsigned_bits >= signed_bits &&
          unsigned_bits >= std::min<uint8_t>(target_.pointer_bits, 32)) {
        return ordinal_integer_primitive(
            registry_, PrimitiveIntKind::Unsigned, unsigned_bits);
      }
    }
    return common_integer_primitive(lhs, rhs);
  }
  if (lhs.int_kind == rhs.int_kind && lhs_bits < 64 && rhs_bits < 64) {
    return common_integer_primitive(lhs, rhs);
  }
  return integer_arithmetic_primitive(BinOp::Add, lhs, rhs);
}

bool EmitAnalysis::const_int_fits_primitive(
    const ConstIntExprInfo& value, const PrimitiveInfo& target) const {
  if (target.int_kind == PrimitiveIntKind::None) return false;
  const uint8_t width = resolved_primitive_bits(target);
  const bool source_is_unsigned =
      value.type && value.type->int_kind == PrimitiveIntKind::Unsigned;
  if (target.int_kind == PrimitiveIntKind::Unsigned) {
    if (source_is_unsigned) {
      return width >= 64 || value.bits <= unsigned_mask_for_bits(width);
    }
    return value.value >= 0 &&
           (width >= 64 ||
            static_cast<uint64_t>(value.value) <=
                unsigned_mask_for_bits(width));
  }
  if (source_is_unsigned) {
    const uint64_t high =
        width >= 64 ? static_cast<uint64_t>(INT64_MAX)
                    : static_cast<uint64_t>(signed_max_for_bits(width));
    return value.bits <= high;
  }
  return value.value >= signed_min_for_bits(width) &&
         value.value <= signed_max_for_bits(width);
}

bool EmitAnalysis::const_expr_fits_primitive(
    const Expr& expr, const PrimitiveInfo& target) {
  const std::optional<ConstIntExprInfo> value = eval_const_int_expr(expr);
  return value && const_int_fits_primitive(*value, target);
}

const PrimitiveInfo* EmitAnalysis::integer_division_primitive(
    const Expr& lhs_expr, const PrimitiveInfo& lhs,
    const Expr& rhs_expr, const PrimitiveInfo& rhs) {
  const PrimitiveInfo* converted_lhs = &lhs;
  const PrimitiveInfo* converted_rhs = &rhs;
  const uint8_t original_lhs_bits = primitive_bits(lhs, target_);
  const uint8_t original_rhs_bits = primitive_bits(rhs, target_);
  if (rhs.int_kind == PrimitiveIntKind::Unsigned &&
      ((const_expr_fits_primitive(lhs_expr, rhs)) ||
       (lhs.int_kind == PrimitiveIntKind::Unsigned &&
        original_rhs_bits >= original_lhs_bits))) {
    converted_lhs = &rhs;
  }
  if (lhs.int_kind == PrimitiveIntKind::Unsigned &&
      ((const_expr_fits_primitive(rhs_expr, lhs)) ||
       (rhs.int_kind == PrimitiveIntKind::Unsigned &&
        original_lhs_bits >= original_rhs_bits))) {
    converted_rhs = &lhs;
  }

  const PrimitiveInfo& effective_lhs = *converted_lhs;
  const PrimitiveInfo& effective_rhs = *converted_rhs;
  const uint8_t lhs_bits = primitive_bits(effective_lhs, target_);
  const uint8_t rhs_bits = primitive_bits(effective_rhs, target_);
  if (lhs_bits == 64 || rhs_bits == 64) {
    return ordinal_integer_primitive(
        registry_,
        effective_lhs.int_kind == PrimitiveIntKind::Signed ||
                effective_rhs.int_kind == PrimitiveIntKind::Signed
            ? PrimitiveIntKind::Signed
            : PrimitiveIntKind::Unsigned,
        64);
  }

  const bool lhs_native_unsigned =
      effective_lhs.int_kind == PrimitiveIntKind::Unsigned &&
      lhs_bits == target_.pointer_bits;
  const bool rhs_native_unsigned =
      effective_rhs.int_kind == PrimitiveIntKind::Unsigned &&
      rhs_bits == target_.pointer_bits;
  if (target_.pointer_bits == 32 &&
      (lhs_native_unsigned || rhs_native_unsigned) &&
      (effective_lhs.int_kind == PrimitiveIntKind::Signed ||
       effective_rhs.int_kind == PrimitiveIntKind::Signed)) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed, 64);
  }
  if (lhs_native_unsigned || rhs_native_unsigned) {
    return default_integer_primitive(PrimitiveIntKind::Unsigned);
  }
  return default_integer_primitive(PrimitiveIntKind::Signed);
}

std::optional<bool> EmitAnalysis::compare_constant_integers(
    BinOp op, const Expr& lhs_expr, const Expr& rhs_expr) {
  const std::optional<ConstIntExprInfo> lhs =
      eval_const_int_expr(lhs_expr);
  const std::optional<ConstIntExprInfo> rhs =
      eval_const_int_expr(rhs_expr);
  if (!lhs || !rhs) return std::nullopt;
  const bool lhs_negative =
      lhs->type && lhs->type->int_kind == PrimitiveIntKind::Signed &&
      lhs->value < 0;
  const bool rhs_negative =
      rhs->type && rhs->type->int_kind == PrimitiveIntKind::Signed &&
      rhs->value < 0;
  int order = 0;
  if (lhs_negative != rhs_negative) {
    order = lhs_negative ? -1 : 1;
  } else if (lhs_negative) {
    order = lhs->value < rhs->value ? -1
            : lhs->value > rhs->value ? 1
                                      : 0;
  } else {
    order = lhs->bits < rhs->bits ? -1
            : lhs->bits > rhs->bits ? 1
                                    : 0;
  }
  switch (op) {
    case BinOp::Eq: return order == 0;
    case BinOp::NotEq: return order != 0;
    case BinOp::Lt: return order < 0;
    case BinOp::Gt: return order > 0;
    case BinOp::LtEq: return order <= 0;
    case BinOp::GtEq: return order >= 0;
    default: return std::nullopt;
  }
}

const PrimitiveInfo* EmitAnalysis::integer_comparison_primitive(
    const Expr& lhs_expr, const PrimitiveInfo& lhs,
    const Expr& rhs_expr, const PrimitiveInfo& rhs) {
  const uint8_t lhs_bits = primitive_bits(lhs, target_);
  const uint8_t rhs_bits = primitive_bits(rhs, target_);
  if (lhs.int_kind == rhs.int_kind && lhs_bits < 64 && rhs_bits < 64) {
    return common_integer_primitive(lhs, rhs);
  }
  if (lhs.int_kind != rhs.int_kind) {
    if (const_expr_fits_primitive(lhs_expr, rhs)) return &rhs;
    if (const_expr_fits_primitive(rhs_expr, lhs)) return &lhs;
  }
  return integer_arithmetic_primitive(BinOp::Add, lhs, rhs);
}

const BoundBinaryOperation* EmitAnalysis::bind_set_binary_operation(
    const Binary& b, const TypeExpr* lhs_source_type,
    const TypeExpr* rhs_source_type) {
  BoundBinaryKind kind = BoundBinaryKind::None;
  switch (b.op) {
    case BinOp::Add:
      kind = BoundBinaryKind::SetUnion;
      break;
    case BinOp::Sub:
      kind = BoundBinaryKind::SetDifference;
      break;
    case BinOp::Mul:
      kind = BoundBinaryKind::SetIntersection;
      break;
    case BinOp::SymDiff:
      kind = BoundBinaryKind::SetSymmetricDifference;
      break;
    default:
      return nullptr;
  }

  const TypeExpr* lhs_set = canonical_set_type(lhs_source_type);
  const TypeExpr* rhs_set = canonical_set_type(rhs_source_type);
  if (!lhs_set && b.lhs->kind == Kind::SetLit && rhs_set) {
    lhs_source_type = deduce_set_literal_type(
        static_cast<const SetLit&>(*b.lhs), rhs_source_type);
    lhs_set = canonical_set_type(lhs_source_type);
  }
  if (!rhs_set && b.rhs->kind == Kind::SetLit && lhs_set) {
    rhs_source_type = deduce_set_literal_type(
        static_cast<const SetLit&>(*b.rhs), lhs_source_type);
    rhs_set = canonical_set_type(rhs_source_type);
  }
  if (!lhs_set || !rhs_set) return nullptr;

  const std::optional<OrdinalDomain> lhs_domain =
      ordinal_domain_for_set_type(lhs_set);
  const std::optional<OrdinalDomain> rhs_domain =
      ordinal_domain_for_set_type(rhs_set);
  if (!lhs_domain || !rhs_domain ||
      lhs_domain->family != rhs_domain->family ||
      lhs_domain->enum_key != rhs_domain->enum_key) {
    return nullptr;
  }

  const TypeExpr* result_type = nullptr;
  if (classify_set_conversion(rhs_set, lhs_set) !=
      SetConversionKind::Incompatible) {
    result_type = lhs_source_type;
  } else if (classify_set_conversion(lhs_set, rhs_set) !=
             SetConversionKind::Incompatible) {
    result_type = rhs_source_type;
  } else {
    const int64_t result_low = std::min(lhs_domain->low, rhs_domain->low);
    const int64_t result_high = std::max(lhs_domain->high, rhs_domain->high);
    const auto& lhs = static_cast<const TySet&>(*lhs_set);
    const auto& rhs = static_cast<const TySet&>(*rhs_set);
    const TypeExpr* result_element = lhs.element.get();

    if (lhs_domain->family == OrdinalFamily::Integer) {
      const std::optional<OrdinalDomain> lhs_element_domain =
          ordinal_domain_for_type(lhs.element.get());
      const std::optional<OrdinalDomain> rhs_element_domain =
          ordinal_domain_for_type(rhs.element.get());
      if (lhs_element_domain && lhs_element_domain->low <= result_low &&
          lhs_element_domain->high >= result_high) {
        result_element = lhs.element.get();
      } else if (rhs_element_domain &&
                 rhs_element_domain->low <= result_low &&
                 rhs_element_domain->high >= result_high) {
        result_element = rhs.element.get();
      } else {
        const OrdinalDomain result_domain{
            .family = OrdinalFamily::Integer,
            .low = result_low,
            .high = result_high,
        };
        const PrimitiveInfo* primitive =
            ordinal_integer_primitive_for_domain(result_domain);
        result_element =
            primitive && primitive->descriptor
                ? primitive->descriptor->type
                : nullptr;
      }
    }
    if (!result_element) return nullptr;
    result_type = synthesize_set_type(
        result_element, std::make_pair(result_low, result_high));
  }

  const TypeDescriptor* lhs_source =
      registry_.descriptor_for_type(lhs_source_type);
  const TypeDescriptor* rhs_source =
      registry_.descriptor_for_type(rhs_source_type);
  const TypeDescriptor* result = registry_.descriptor_for_type(result_type);
  if (!lhs_source || !rhs_source || !result) return nullptr;

  b.bound_operation = BoundBinaryOperation{
      .binding_complete = true,
      .kind = kind,
      .lhs_source = lhs_source,
      .rhs_source = rhs_source,
      .lhs = result,
      .rhs = result,
      .result = result,
  };
  b.result_descriptor = result;
  return &b.bound_operation;
}

const BoundBinaryOperation* EmitAnalysis::bind_binary_operation(
    const Binary& b) {
  if (b.bound_operation.binding_complete) {
    return b.bound_operation.kind != BoundBinaryKind::None
               ? &b.bound_operation
               : nullptr;
  }
  b.bound_operation.binding_complete = true;

  const TypeExpr* lhs_source_type = deduce_type(*b.lhs);
  const TypeExpr* rhs_source_type = deduce_type(*b.rhs);
  if (const BoundBinaryOperation* set_operation =
          bind_set_binary_operation(b, lhs_source_type, rhs_source_type)) {
    return set_operation;
  }
  const TypeDescriptor* lhs_source =
      registry_.descriptor_for_type(lhs_source_type);
  const TypeDescriptor* rhs_source =
      registry_.descriptor_for_type(rhs_source_type);
  if (!lhs_source) {
    if (const TypeSymbol* symbol =
            concrete_class_symbol_for_metaclass_value(*b.lhs)) {
      lhs_source = registry_.metaclass_descriptor_for_target(symbol);
    }
  }
  if (!rhs_source) {
    if (const TypeSymbol* symbol =
            concrete_class_symbol_for_metaclass_value(*b.rhs)) {
      rhs_source = registry_.metaclass_descriptor_for_target(symbol);
    }
  }
  const TypeExpr* lhs_type =
      canonicalize_for_arithmetic(lhs_source_type);
  const TypeExpr* rhs_type =
      canonicalize_for_arithmetic(rhs_source_type);
  const PrimitiveInfo* lhs = primitive_info_for_type(lhs_type);
  const PrimitiveInfo* rhs = primitive_info_for_type(rhs_type);
  // Bind pointer operations before integer arithmetic so their legality,
  // common pointer type, and result type are fixed once. Emission uses native
  // C++ operators only for the selected same-array operations.
  const bool lhs_pointer =
      type_is_typed_pointer_arithmetic_operand(lhs_type);
  const bool rhs_pointer =
      type_is_typed_pointer_arithmetic_operand(rhs_type);
  const bool lhs_pointer_value =
      b.lhs->kind == Kind::NilLit ||
      (lhs_source && lhs_source->metaclass_target) ||
      type_is_native_pointer_value(lhs_source_type);
  const bool rhs_pointer_value =
      b.rhs->kind == Kind::NilLit ||
      (rhs_source && rhs_source->metaclass_target) ||
      type_is_native_pointer_value(rhs_source_type);
  const bool lhs_integer =
      lhs && lhs->family == PrimitiveFamily::Integer &&
      lhs->int_kind != PrimitiveIntKind::None;
  const bool rhs_integer =
      rhs && rhs->family == PrimitiveFamily::Integer &&
      rhs->int_kind != PrimitiveIntKind::None;
  const TypeDescriptor* pointer = lhs_pointer ? lhs_source : rhs_source;
  const TypeDescriptor* ptrint =
      pointer ? pointer->pointer_difference_result : nullptr;
  auto bind_pointer_operation =
      [&](BoundBinaryKind kind, const TypeDescriptor* lhs_conversion,
          const TypeDescriptor* rhs_conversion,
          const TypeDescriptor* result,
          bool pointer_on_lhs = true) -> const BoundBinaryOperation* {
    b.bound_operation = BoundBinaryOperation{
        .binding_complete = true,
        .kind = kind,
        .lhs_source = lhs_source,
        .rhs_source = rhs_source,
        .lhs = lhs_conversion,
        .rhs = rhs_conversion,
        .result = result,
        .pointer_on_lhs = pointer_on_lhs,
    };
    b.result_descriptor = result;
    return &b.bound_operation;
  };
  if ((b.op == BinOp::Eq || b.op == BinOp::NotEq ||
       b.op == BinOp::Lt || b.op == BinOp::Gt ||
       b.op == BinOp::LtEq || b.op == BinOp::GtEq) &&
      lhs_pointer_value && rhs_pointer_value) {
    const TypeDescriptor* common = common_pointer_comparison_type(
        lhs_source_type, lhs_source, rhs_source_type, rhs_source);
    const TypeSymbol* boolean = registry_.builtin_literal("boolean");
    const TypeDescriptor* result =
        boolean ? boolean->descriptor : nullptr;
    if (!common || !result) {
      return bind_pointer_operation(BoundBinaryKind::InvalidPointer,
                                    lhs_source, rhs_source, nullptr);
    }
    return bind_pointer_operation(
        b.op == BinOp::Eq || b.op == BinOp::NotEq
            ? BoundBinaryKind::PointerEqual
            : BoundBinaryKind::PointerOrder,
        common, common, result);
  }
  if ((b.op == BinOp::Eq || b.op == BinOp::NotEq ||
       b.op == BinOp::Lt || b.op == BinOp::Gt ||
       b.op == BinOp::LtEq || b.op == BinOp::GtEq) &&
      (lhs_pointer_value || rhs_pointer_value)) {
    return bind_pointer_operation(BoundBinaryKind::InvalidPointer,
                                  lhs_source, rhs_source, nullptr);
  }
  if (ptrint && b.op == BinOp::Add &&
      ((lhs_pointer && rhs_integer) || (lhs_integer && rhs_pointer))) {
    return bind_pointer_operation(
        BoundBinaryKind::PointerAdd, lhs_pointer ? lhs_source : ptrint,
        rhs_pointer ? rhs_source : ptrint, pointer, lhs_pointer);
  }
  if (ptrint && b.op == BinOp::Sub && lhs_pointer && rhs_integer) {
    return bind_pointer_operation(BoundBinaryKind::PointerSubtract, lhs_source,
                                  ptrint, lhs_source);
  }
  if (ptrint && b.op == BinOp::Sub && lhs_pointer && rhs_pointer) {
    const auto& lhs_pointer_type = static_cast<const TyPointer&>(*lhs_type);
    const auto& rhs_pointer_type = static_cast<const TyPointer&>(*rhs_type);
    const TypeDescriptor* common_pointer = lhs_source;
    if (!lhs_pointer_type.target) {
      common_pointer = rhs_source;
    } else if (rhs_pointer_type.target &&
               !registry_.bound_signature_type_exprs_match(
                   lhs_type, rhs_type)) {
      return nullptr;
    }
    return bind_pointer_operation(BoundBinaryKind::PointerDifference,
                                  common_pointer, common_pointer, ptrint);
  }
  if ((lhs_pointer_value || rhs_pointer_value) &&
      (b.op == BinOp::Add || b.op == BinOp::Sub)) {
    return bind_pointer_operation(BoundBinaryKind::InvalidPointer,
                                  lhs_source, rhs_source, nullptr);
  }
  if (!lhs || !rhs || lhs->int_kind == PrimitiveIntKind::None ||
      rhs->int_kind == PrimitiveIntKind::None) {
    return nullptr;
  }
  // ByteBool, WordBool, LongBool, QWordBool, and WideChar use integer C++
  // storage, but their Pascal operators belong to the Boolean or character
  // families. Storage representation must not make the integer binder claim
  // those operations.
  if (lhs->family != PrimitiveFamily::Integer ||
      rhs->family != PrimitiveFamily::Integer) {
    return nullptr;
  }
  // Currency uses a scaled representation and CPU-family-dependent operator
  // rules. Treating that representation as an ordinary signed integer would
  // bind the wrong Pascal operation; TargetInfo must model that distinction
  // before Currency can join this integer binder.
  if (lhs->kind == PrimitiveKind::Currency ||
      rhs->kind == PrimitiveKind::Currency) {
    return nullptr;
  }

  const PrimitiveInfo* lhs_conversion = nullptr;
  const PrimitiveInfo* rhs_conversion = nullptr;
  const PrimitiveInfo* result = nullptr;
  BoundBinaryKind kind = BoundBinaryKind::None;
  switch (b.op) {
    case BinOp::Add:
      result = integer_arithmetic_primitive(b.op, *lhs, *rhs);
      kind = BoundBinaryKind::IntegerAdd;
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::Sub:
      result = integer_arithmetic_primitive(b.op, *lhs, *rhs);
      kind = BoundBinaryKind::IntegerSub;
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::Mul:
      result = integer_arithmetic_primitive(b.op, *lhs, *rhs);
      kind = BoundBinaryKind::IntegerMul;
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::IntDiv:
      result = integer_division_primitive(*b.lhs, *lhs, *b.rhs, *rhs);
      kind = BoundBinaryKind::IntegerDiv;
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::Mod:
      result = integer_division_primitive(*b.lhs, *lhs, *b.rhs, *rhs);
      kind = BoundBinaryKind::IntegerMod;
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::And:
    case BinOp::Or:
    case BinOp::Xor:
      kind = BoundBinaryKind::IntegerBitwise;
      result = integer_bitwise_primitive(b.op, *lhs, *rhs);
      lhs_conversion = rhs_conversion = result;
      break;
    case BinOp::Eq:
    case BinOp::NotEq:
    case BinOp::Lt:
    case BinOp::Gt:
    case BinOp::LtEq:
    case BinOp::GtEq:
      kind = BoundBinaryKind::IntegerCompare;
      lhs_conversion = rhs_conversion = integer_comparison_primitive(
          *b.lhs, *lhs, *b.rhs, *rhs);
      if (const TypeSymbol* boolean = registry_.builtin_literal("boolean")) {
        result = boolean->descriptor ? boolean->descriptor->primitive : nullptr;
      }
      break;
    case BinOp::Shl:
    case BinOp::Shr:
      kind = BoundBinaryKind::IntegerShift;
      lhs_conversion = shift_carrier_primitive(registry_, lhs, target_);
      rhs_conversion =
          default_integer_primitive(PrimitiveIntKind::Signed);
      result = lhs_conversion;
      break;
    default:
      return nullptr;
  }
  if (!lhs_conversion || !rhs_conversion || !result ||
      !lhs_conversion->descriptor || !rhs_conversion->descriptor ||
      !result->descriptor) {
    return nullptr;
  }

  b.bound_operation = BoundBinaryOperation{
      .binding_complete = true,
      .kind = kind,
      .lhs_source = lhs_source,
      .rhs_source = rhs_source,
      .lhs = lhs_conversion->descriptor,
      .rhs = rhs_conversion->descriptor,
      .result = result->descriptor,
      .check_overflow = b.q_check,
  };
  if (kind == BoundBinaryKind::IntegerCompare) {
    if (std::optional<bool> constant =
            compare_constant_integers(b.op, *b.lhs, *b.rhs)) {
      b.bound_operation.has_constant_boolean = true;
      b.bound_operation.constant_boolean = *constant;
    }
  }
  if (!binop_is_comparison(b.op)) {
    b.result_descriptor = result->descriptor;
  }
  return &b.bound_operation;
}

const PrimitiveInfo* EmitAnalysis::abs_sqr_primitive(
    BoundIntrinsicKind kind, const PrimitiveInfo& operand) const {
  if (operand.is_real()) return &operand;
  if (operand.family != PrimitiveFamily::Integer ||
      operand.kind == PrimitiveKind::Currency) {
    return nullptr;
  }
  if (operand.int_kind == PrimitiveIntKind::None) return nullptr;

  const uint8_t bits = primitive_bits(operand, target_);
  if (operand.int_kind == PrimitiveIntKind::Signed) {
    return ordinal_integer_primitive(
        registry_, PrimitiveIntKind::Signed, bits <= 32 ? 32 : 64);
  }
  if (bits <= 16) {
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed, 32);
  }
  if (kind == BoundIntrinsicKind::Abs) {
    return bits == 32
               ? ordinal_integer_primitive(
                     registry_, PrimitiveIntKind::Signed, 64)
               : nullptr;
  }
  return ordinal_integer_primitive(registry_, PrimitiveIntKind::Unsigned, 64);
}

const PrimitiveInfo* EmitAnalysis::ordinal_integer_primitive_for_domain(
    const OrdinalDomain& domain) const {
  if (domain.low < 0) {
    const uint8_t bits =
        domain.low >= signed_min_for_bits(8) &&
                domain.high <= signed_max_for_bits(8)
            ? 8
        : domain.low >= signed_min_for_bits(16) &&
                  domain.high <= signed_max_for_bits(16)
            ? 16
        : domain.low >= signed_min_for_bits(32) &&
                  domain.high <= signed_max_for_bits(32)
            ? 32
            : 64;
    return ordinal_integer_primitive(registry_, PrimitiveIntKind::Signed,
                                     bits);
  }
  const uint64_t high = static_cast<uint64_t>(domain.high);
  const uint8_t bits =
      high <= unsigned_mask_for_bits(8) ? 8
      : high <= unsigned_mask_for_bits(16) ? 16
      : high <= unsigned_mask_for_bits(32) ? 32
                                           : 64;
  return ordinal_integer_primitive(registry_, PrimitiveIntKind::Unsigned,
                                   bits);
}

const PrimitiveInfo* EmitAnalysis::ordinal_storage_primitive(
    const TypeExpr* type) {
  if (const PrimitiveInfo* primitive = primitive_info_for_type(type);
      primitive && primitive->int_kind != PrimitiveIntKind::None) {
    return primitive;
  }
  const TypeExpr* shape = semantic_shape_type(type);
  const std::optional<OrdinalDomain> domain =
      ordinal_domain_for_type(type);
  if (!domain) {
    if (!shape || shape->kind != Kind::TySubrange) return nullptr;
    const auto& subrange = static_cast<const TySubrange&>(*shape);
    if (!subrange.lo || !subrange.hi) return nullptr;
    const PrimitiveInfo* low = primitive_info_for_type(
        canonicalize_for_arithmetic(deduce_type(*subrange.lo)));
    const PrimitiveInfo* high = primitive_info_for_type(
        canonicalize_for_arithmetic(deduce_type(*subrange.hi)));
    if (!low || !high || low->family != PrimitiveFamily::Integer ||
        high->family != PrimitiveFamily::Integer ||
        low->int_kind == PrimitiveIntKind::None ||
        high->int_kind == PrimitiveIntKind::None) return nullptr;
    // Required-constant bounds can remain symbolic until generated C++
    // evaluates them. Their resolved Pascal expression types still select the
    // common carrier; type emission must not invent one.
    return integer_arithmetic_primitive(BinOp::Add, *low, *high);
  }
  if (domain->family == OrdinalFamily::Enum) {
    const TypeExpr* enum_type =
        domain->enum_key ? semantic_shape_type(domain->enum_key->type) : shape;
    if (!enum_type || enum_type->kind != Kind::TyEnum) return nullptr;
    const auto& enum_decl = static_cast<const TyEnum&>(*enum_type);
    if (enum_decl.members.empty()) {
      return ordinal_integer_primitive(
          registry_, PrimitiveIntKind::Signed, 32);
    }
    const std::optional<OrdinalDomain> enum_domain =
        ordinal_domain_for_type(enum_type);
    if (!enum_domain) return nullptr;

    // Enum representation is declaration-controlled, not inferred from the
    // current subrange. This is the same decision used by enum declaration
    // emission, so arithmetic and storage cannot disagree about its width.
    uint8_t bits = 8;
    if (enum_domain->low < std::numeric_limits<int32_t>::min() ||
        enum_domain->high >
            static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      bits = 64;
    } else if (enum_decl.packenum >= 4 ||
               enum_domain->low < std::numeric_limits<int16_t>::min() ||
               enum_domain->high >
                   static_cast<int64_t>(
                       std::numeric_limits<uint16_t>::max())) {
      bits = 32;
    } else if (enum_decl.packenum >= 2 ||
               enum_domain->low < std::numeric_limits<int8_t>::min() ||
               enum_domain->high >
                   static_cast<int64_t>(
                       std::numeric_limits<uint8_t>::max())) {
      bits = 16;
    }
    return ordinal_integer_primitive(
        registry_,
        enum_domain->low < 0 ? PrimitiveIntKind::Signed
                             : PrimitiveIntKind::Unsigned,
        bits);
  }
  return ordinal_integer_primitive_for_domain(*domain);
}

const BoundIntrinsicOperation* EmitAnalysis::bind_intrinsic_operation(
    const Call& call) {
  if (call.bound_intrinsic.kind != BoundIntrinsicKind::None) {
    return &call.bound_intrinsic;
  }
  if (!call.callee) return nullptr;
  const std::optional<std::string> intrinsic =
      intrinsic_call_name(*call.callee);
  if (!intrinsic) return nullptr;

  BoundIntrinsicKind kind = BoundIntrinsicKind::None;
  if (*intrinsic == "abs" && call.args.size() == 1) {
    kind = BoundIntrinsicKind::Abs;
  } else if (*intrinsic == "sqr" && call.args.size() == 1) {
    kind = BoundIntrinsicKind::Sqr;
  } else if (*intrinsic == "succ" && call.args.size() == 1) {
    kind = BoundIntrinsicKind::Succ;
  } else if (*intrinsic == "pred" && call.args.size() == 1) {
    kind = BoundIntrinsicKind::Pred;
  } else if (*intrinsic == "inc" &&
             (call.args.size() == 1 || call.args.size() == 2)) {
    kind = BoundIntrinsicKind::Inc;
  } else if (*intrinsic == "dec" &&
             (call.args.size() == 1 || call.args.size() == 2)) {
    kind = BoundIntrinsicKind::Dec;
  } else {
    return nullptr;
  }

  const TypeExpr* operand_type = deduce_type(*call.args[0]);
  const TypeDescriptor* operand =
      registry_.descriptor_for_type(operand_type);
  if (!operand) return nullptr;
  const TypeDescriptor* source = operand;
  const TypeDescriptor* result = operand;
  const TypeDescriptor* carrier = operand;
  const TypeExpr* delta_type =
      call.args.size() == 2 ? deduce_type(*call.args[1]) : nullptr;
  const TypeDescriptor* delta_source =
      registry_.descriptor_for_type(delta_type);
  if (!delta_source &&
      (kind == BoundIntrinsicKind::Inc ||
       kind == BoundIntrinsicKind::Dec)) {
    const TypeSymbol* one = registry_.builtin_literal("shortint");
    delta_source = one ? one->descriptor : nullptr;
  }
  const TypeDescriptor* delta = delta_source;
  const PrimitiveInfo* delta_primitive = nullptr;
  if ((kind == BoundIntrinsicKind::Inc ||
       kind == BoundIntrinsicKind::Dec) &&
      call.args.size() == 2) {
    delta_primitive = primitive_info_for_type(
        canonicalize_for_arithmetic(delta_type));
    if (!delta_primitive ||
        delta_primitive->family != PrimitiveFamily::Integer ||
        delta_primitive->kind == PrimitiveKind::Currency ||
        delta_primitive->int_kind == PrimitiveIntKind::None) {
      return nullptr;
    }
  }
  if (kind == BoundIntrinsicKind::Abs ||
      kind == BoundIntrinsicKind::Sqr) {
    const PrimitiveInfo* source =
        primitive_info_for_type(operand_type);
    const PrimitiveInfo* selected =
        source ? abs_sqr_primitive(kind, *source) : nullptr;
    if (!selected || !selected->descriptor) return nullptr;
    operand = selected->descriptor;
    carrier = selected->descriptor;
    result = selected->descriptor;
  } else if (kind == BoundIntrinsicKind::Succ ||
             kind == BoundIntrinsicKind::Pred ||
             kind == BoundIntrinsicKind::Inc ||
             kind == BoundIntrinsicKind::Dec) {
    const PrimitiveInfo* storage_carrier =
        ordinal_storage_primitive(operand_type);
    if (storage_carrier && storage_carrier->descriptor) {
      const PrimitiveInfo* operation_carrier = storage_carrier;
      if (call.q_check || call.r_check) {
        const PrimitiveInfo* delta_carrier =
            delta_primitive
                ? delta_primitive
                : ordinal_integer_primitive(
                      registry_, PrimitiveIntKind::Signed, 8);
        if (!delta_carrier) return nullptr;
        operation_carrier = integer_arithmetic_primitive(
            (kind == BoundIntrinsicKind::Pred ||
             kind == BoundIntrinsicKind::Dec)
                ? BinOp::Sub
                : BinOp::Add,
            *storage_carrier, *delta_carrier);
      }
      if (!operation_carrier || !operation_carrier->descriptor) {
        return nullptr;
      }
      carrier = operation_carrier->descriptor;
      delta = operation_carrier->descriptor;
    } else {
      const TypeExpr* shape = semantic_shape_type(operand_type);
      const bool pointer =
          shape && shape->kind == Kind::TyPointer &&
          static_cast<const TyPointer&>(*shape).target;
      if (!(pointer && (kind == BoundIntrinsicKind::Inc ||
                       kind == BoundIntrinsicKind::Dec))) {
        return nullptr;
      }
      if (!delta_primitive && delta_source) {
        delta_primitive = delta_source->primitive;
      }
      if (!delta_primitive ||
          delta_primitive->int_kind == PrimitiveIntKind::None) {
        return nullptr;
      }
      const TypeSymbol* pointer_delta = registry_.builtin_literal(
          delta_primitive->int_kind == PrimitiveIntKind::Unsigned
              ? "ptruint"
              : "ptrint");
      if (!pointer_delta || !pointer_delta->descriptor) return nullptr;
      delta = pointer_delta->descriptor;
    }
  }

  call.bound_intrinsic = BoundIntrinsicOperation{
      .kind = kind,
      .source = source,
      .operand = operand,
      .delta_source = delta_source,
      .delta = delta,
      .carrier = carrier,
      .result = result,
      .check_overflow = call.q_check,
      .check_range = call.r_check,
  };
  if (kind != BoundIntrinsicKind::Inc &&
      kind != BoundIntrinsicKind::Dec) {
    call.result_descriptor = result;
  }
  return &call.bound_intrinsic;
}

const TypeExpr* EmitAnalysis::deduce_binary_expr_type(const Binary& b) {
  if (const BoundBinaryOperation* operation = bind_binary_operation(b)) {
    return operation->result ? operation->result->type : nullptr;
  }
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
    const Expr& e) {
  std::unordered_set<std::string> visiting_const_names;
  return eval_const_int_expr_impl(e, &visiting_const_names, nullptr);
}

bool EmitAnalysis::required_const_int_expr_overflows(const Expr& e) {
  std::unordered_set<std::string> visiting_const_names;
  bool overflow = false;
  (void)eval_const_int_expr_impl(e, &visiting_const_names, &overflow);
  return overflow;
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_bound_ordinal_step(
    const Call& call, const BoundIntrinsicOperation& operation,
    std::unordered_set<std::string>* visiting_const_names,
    bool* overflow) {
  if (call.args.size() != 1 || !call.args[0] || !operation.source ||
      !operation.source->type || !operation.carrier ||
      !operation.carrier->primitive || !operation.result ||
      (operation.kind != BoundIntrinsicKind::Succ &&
       operation.kind != BoundIntrinsicKind::Pred)) {
    return std::nullopt;
  }

  const PrimitiveInfo* storage =
      ordinal_storage_primitive(operation.source->type);
  const PrimitiveInfo* carrier = operation.carrier->primitive;
  if (!storage || !storage->descriptor || !carrier->descriptor ||
      storage->int_kind == PrimitiveIntKind::None ||
      carrier->int_kind == PrimitiveIntKind::None) {
    return std::nullopt;
  }

  std::optional<ConstIntExprInfo> source =
      eval_const_int_expr_impl(*call.args[0], visiting_const_names, overflow);
  if (!source) {
    const std::optional<OrdinalExprValue> ordinal =
        eval_ordinal_expr(*call.args[0]);
    if (!ordinal) return std::nullopt;
    source = ConstIntExprInfo{
        ordinal->value, static_cast<uint64_t>(ordinal->value), storage};
  }
  const std::optional<ConvertedConstInt> converted =
      convert_const_int_value(call.loc, *source, carrier->descriptor->type,
                              true, false);
  if (!converted) return std::nullopt;

  const uint8_t carrier_bits = resolved_primitive_bits(*carrier);
  const uint64_t carrier_mask = unsigned_mask_for_bits(carrier_bits);
  const auto signed_carrier_value = [&](uint64_t bits) {
    bits &= carrier_mask;
    if (carrier_bits == 64) return static_cast<int64_t>(bits);
    const uint64_t sign = uint64_t{1} << (carrier_bits - 1);
    return (bits & sign) == 0
               ? static_cast<int64_t>(bits)
               : static_cast<int64_t>(bits | ~carrier_mask);
  };

  uint64_t result_bits = converted->bits & carrier_mask;
  if (operation.check_overflow &&
      carrier->int_kind == PrimitiveIntKind::Signed) {
    const int64_t value = signed_carrier_value(result_bits);
    int64_t result = 0;
    const bool valid =
        operation.kind == BoundIntrinsicKind::Succ
            ? !__builtin_add_overflow(value, int64_t{1}, &result)
            : !__builtin_sub_overflow(value, int64_t{1}, &result);
    if (!valid ||
        (carrier_bits < 64 &&
         (result < signed_min_for_bits(carrier_bits) ||
          result > signed_max_for_bits(carrier_bits)))) {
      if (overflow) *overflow = true;
      return std::nullopt;
    }
    result_bits = static_cast<uint64_t>(result) & carrier_mask;
  } else if (operation.kind == BoundIntrinsicKind::Succ) {
    result_bits = (result_bits + 1) & carrier_mask;
  } else {
    result_bits = (result_bits - 1) & carrier_mask;
  }

  if (operation.check_range) {
    const std::optional<OrdinalDomain> domain =
        ordinal_domain_for_type(operation.source->type);
    if (!domain) return std::nullopt;
    if (carrier->int_kind == PrimitiveIntKind::Signed) {
      const int64_t value = signed_carrier_value(result_bits);
      if (value < domain->low || value > domain->high) return std::nullopt;
    } else if (domain->low < 0 ||
               result_bits > static_cast<uint64_t>(domain->high)) {
      return std::nullopt;
    }
  }

  const uint8_t storage_bits = resolved_primitive_bits(*storage);
  const uint64_t storage_mask = unsigned_mask_for_bits(storage_bits);
  result_bits &= storage_mask;
  int64_t result_value = static_cast<int64_t>(result_bits);
  if (storage->int_kind == PrimitiveIntKind::Signed &&
      storage_bits < 64) {
    const uint64_t sign = uint64_t{1} << (storage_bits - 1);
    if ((result_bits & sign) != 0) {
      result_value =
          static_cast<int64_t>(result_bits | ~storage_mask);
    }
  }
  return ConstIntExprInfo{result_value, result_bits, storage};
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_expr_impl(
    const Expr& e,
    std::unordered_set<std::string>* visiting_const_names,
    bool* overflow) {
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
      auto operand =
          eval_const_int_expr_impl(*u.operand, visiting_const_names, overflow);
      if (!operand) return std::nullopt;
      if (u.op == UnOp::Plus) return operand;
      if (u.op != UnOp::Neg) return std::nullopt;
      int64_t value = 0;
      if (!checked_sub_int64(0, operand->value, &value)) {
        if (overflow) *overflow = true;
        return std::nullopt;
      }
      return ConstIntExprInfo{value, type_for_value(value)};
    }
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      const BoundBinaryOperation* operation = bind_binary_operation(b);
      if (!operation) return std::nullopt;
      auto lhs =
          eval_const_int_expr_impl(*b.lhs, visiting_const_names, overflow);
      auto rhs =
          eval_const_int_expr_impl(*b.rhs, visiting_const_names, overflow);
      if (!lhs || !rhs) return std::nullopt;
      if (operation->kind == BoundBinaryKind::IntegerCompare) {
        return std::nullopt;
      }

      const auto is_negative = [](const ConstIntExprInfo& operand) {
        return operand.value < 0 &&
               !(operand.type &&
                 operand.type->int_kind == PrimitiveIntKind::Unsigned);
      };
      const auto magnitude = [&](const ConstIntExprInfo& operand) {
        return is_negative(operand) ? uint64_t{0} - operand.bits
                                    : operand.bits;
      };
      const auto make_result =
          [&](bool negative,
              uint64_t result) -> std::optional<ConstIntExprInfo> {
        if (negative) {
          const uint64_t min_int64_magnitude = uint64_t{1} << 63;
          if (result > min_int64_magnitude) {
            if (overflow) *overflow = true;
            return std::nullopt;
          }
          const int64_t value =
              result == min_int64_magnitude
                  ? std::numeric_limits<int64_t>::min()
                  : -static_cast<int64_t>(result);
          return ConstIntExprInfo{value, type_for_value(value)};
        }
        if (result <= static_cast<uint64_t>(
                          std::numeric_limits<int64_t>::max())) {
          const int64_t value = static_cast<int64_t>(result);
          return ConstIntExprInfo{value, type_for_value(value)};
        }
        return ConstIntExprInfo{
            static_cast<int64_t>(result), result,
            ordinal_integer_primitive(
                registry_, PrimitiveIntKind::Unsigned, 64)};
      };
      const auto add_signed_magnitudes =
          [&](bool lhs_negative, uint64_t lhs_magnitude,
              bool rhs_negative, uint64_t rhs_magnitude)
          -> std::optional<ConstIntExprInfo> {
        if (lhs_negative != rhs_negative) {
          if (lhs_magnitude >= rhs_magnitude) {
            return make_result(lhs_negative,
                               lhs_magnitude - rhs_magnitude);
          }
          return make_result(rhs_negative,
                             rhs_magnitude - lhs_magnitude);
        }
        if (lhs_magnitude > UINT64_MAX - rhs_magnitude) {
          if (overflow) *overflow = true;
          return std::nullopt;
        }
        return make_result(lhs_negative,
                           lhs_magnitude + rhs_magnitude);
      };

      const bool lhs_negative = is_negative(*lhs);
      const bool rhs_negative = is_negative(*rhs);
      const uint64_t lhs_magnitude = magnitude(*lhs);
      const uint64_t rhs_magnitude = magnitude(*rhs);
      if (operation->kind == BoundBinaryKind::IntegerAdd) {
        return add_signed_magnitudes(
            lhs_negative, lhs_magnitude, rhs_negative, rhs_magnitude);
      }
      if (operation->kind == BoundBinaryKind::IntegerSub) {
        return add_signed_magnitudes(
            lhs_negative, lhs_magnitude, !rhs_negative, rhs_magnitude);
      }
      if (operation->kind == BoundBinaryKind::IntegerMul ||
          operation->kind == BoundBinaryKind::IntegerDiv ||
          operation->kind == BoundBinaryKind::IntegerMod) {
        if (rhs_magnitude == 0 &&
            (operation->kind == BoundBinaryKind::IntegerDiv ||
             operation->kind == BoundBinaryKind::IntegerMod)) {
          return std::nullopt;
        }
        if (operation->kind == BoundBinaryKind::IntegerMul &&
            rhs_magnitude != 0 &&
            lhs_magnitude > UINT64_MAX / rhs_magnitude) {
          if (overflow) *overflow = true;
          return std::nullopt;
        }
        if (operation->kind == BoundBinaryKind::IntegerMul) {
          return make_result(lhs_negative != rhs_negative,
                             lhs_magnitude * rhs_magnitude);
        }
        if (operation->kind == BoundBinaryKind::IntegerDiv) {
          return make_result(lhs_negative != rhs_negative,
                             lhs_magnitude / rhs_magnitude);
        }
        return make_result(lhs_negative,
                           lhs_magnitude % rhs_magnitude);
      }

      int64_t value = 0;
      if (operation->kind == BoundBinaryKind::IntegerShift) {
        const bool valid =
            b.op == BinOp::Shl
                ? checked_pascal_shl_int64(
                      lhs->value, lhs->type, rhs->value, &value, target_)
                : checked_pascal_shr_int64(
                      lhs->value, lhs->type, rhs->value, &value, target_);
        if (!valid) return std::nullopt;
        if (const PrimitiveInfo* carrier =
                shift_carrier_primitive(registry_, lhs->type, target_)) {
          return ConstIntExprInfo{value, carrier};
        }
        return std::nullopt;
      }
      if (operation->kind != BoundBinaryKind::IntegerBitwise) {
        return std::nullopt;
      }
      if (b.op == BinOp::And) {
        value = static_cast<int64_t>(
            static_cast<uint64_t>(lhs->value) &
            static_cast<uint64_t>(rhs->value));
      } else if (b.op == BinOp::Or) {
        value = static_cast<int64_t>(
            static_cast<uint64_t>(lhs->value) |
            static_cast<uint64_t>(rhs->value));
      } else {
        value = static_cast<int64_t>(
            static_cast<uint64_t>(lhs->value) ^
            static_cast<uint64_t>(rhs->value));
      }
      return ConstIntExprInfo{value, type_for_value(value)};
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      if (c.args.size() == 1 && c.callee) {
        const std::optional<std::string> intrinsic =
            intrinsic_call_name(*c.callee);
        const std::string callee_name =
            intrinsic ? *intrinsic : std::string{};
        if (callee_name == "length" && c.args[0]->kind == Kind::StringLit) {
          const auto& s = static_cast<const StringLit&>(*c.args[0]);
          if (s.value.size() <= static_cast<size_t>(INT64_MAX)) {
            int64_t value = static_cast<int64_t>(s.value.size());
            return ConstIntExprInfo{value, type_for_value(value)};
          }
        }
        if (callee_name == "low" || callee_name == "high") {
          if (const TypeExpr* type =
                  const_intrinsic_type_arg(*c.args[0])) {
            return eval_const_low_high(type, callee_name == "low");
          }
        }
        if ((callee_name == "pred" || callee_name == "succ") && c.args[0]) {
          const BoundIntrinsicOperation* operation =
              bind_intrinsic_operation(c);
          if (operation) {
            return eval_bound_ordinal_step(
                c, *operation, visiting_const_names, overflow);
          }
        }
        if (callee_name == "ord" && c.args[0]) {
          if (c.args[0]->kind == Kind::Call) {
            const auto& inner = static_cast<const Call&>(*c.args[0]);
            if (inner.callee && inner.args.size() == 1 && inner.args[0]) {
              const std::optional<std::string> inner_intrinsic =
                  intrinsic_call_name(*inner.callee);
              const std::string inner_name =
                  inner_intrinsic ? *inner_intrinsic : std::string{};
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
      return eval_const_int_cast(c, visiting_const_names, overflow);
    }
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      if (const EnumMemberInfo* member =
              find_visible_enum_member(id.name);
          member && member->owner) {
        if (const std::optional<int64_t> ordinal =
                enum_member_ordinal(*member)) {
          // ConstIntExprInfo carries the value domain used by the partial
          // evaluator, not the enum's nominal identity. Callers such as
          // eval_ordinal_expr retain that identity from the bound expression
          // type while using this numeric value for aliases and arithmetic.
          return ConstIntExprInfo{
              *ordinal, static_cast<uint64_t>(*ordinal),
              type_for_value(*ordinal)};
        }
        return std::nullopt;
      }
      if (!visiting_const_names) {
        std::unordered_set<std::string> local_visiting;
        return eval_const_int_expr_impl(e, &local_visiting, overflow);
      }
      if (!visiting_const_names->insert(id.name).second) return std::nullopt;
      std::optional<ConstIntExprInfo> out;

      auto lit = scope_.local_consts.find(id.name);
      if (lit != scope_.local_consts.end() && lit->second && lit->second->value) {
        out = fold_untyped_const_decl(*lit->second, visiting_const_names,
                                      overflow);
      } else {
        for (const TypeLookupContext* frame = scope_.type_scope; frame;
             frame = frame->parent) {
          if (const auto* c = scope_frame_find_const(*frame, id.name)) {
            out = fold_untyped_const_info(*c, visiting_const_names, overflow);
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
      if (const BoundBinaryOperation* operation = bind_binary_operation(b)) {
        return operation->result ? operation->result->type : nullptr;
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
                     migration_class_or_record_type_value_symbol(*m.base)) {
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
      if (const BoundIntrinsicOperation* operation =
              bind_intrinsic_operation(c)) {
        return operation->result ? operation->result->type : nullptr;
      }
      // Semantic binding has already distinguished `T(expr)` from an ordinary
      // call.
      // Preserve the selected declaration type directly; primitive aliases
      // share descriptor metadata and require no spelling dispatch here.
      if (c.args.size() == 1) {
        if (const TypeExpr* cast_type = explicit_typecast_result_type(e)) {
          return cast_type;
        }
      }
      if (c.callee->kind == Kind::Ident && c.args.size() == 1 &&
          static_cast<const Ident&>(*c.callee).name == "unaligned") {
        // `unaligned(x)` changes how x's storage may be accessed, not its
        // Pascal value type. Keeping the operand type here lets assignment,
        // Inc/Dec, and address lowering share the storage designator without
        // inventing a second type from its emitted byte-copy representation.
        return deduce_type(*c.args[0]);
      }
      // Intrinsics that work the same whether spelled `low(t)` or
      // `system.low(t)` are dispatched through intrinsic_call_name so the
      // Ident-callee and system-unit-Member-callee spellings share one table.
      if (auto intrinsic = intrinsic_call_name(*c.callee)) {
        const std::string& n = *intrinsic;
        if (n == "ord" && c.args.size() == 1) {
          return ord_result_type_for_operand(*c.args[0]);
        }
        if (n == "chr" && c.args.size() == 1) {
          const TypeSymbol* result = registry_.builtin_literal("char");
          return result && result->descriptor ? result->descriptor->type
                                              : nullptr;
        }
        if (n == "sizeof" && c.args.size() == 1) {
          const TypeSymbol* result = registry_.builtin_literal("longint");
          return result && result->descriptor ? result->descriptor->type
                                              : nullptr;
        }
        if ((n == "low" || n == "high") && c.args.size() == 1) {
          if (const TypeSymbol* symbol =
                  migration_type_symbol_for_expression(*c.args[0])) {
            return deduce_low_high_result_type(
                symbol->descriptor ? symbol->descriptor->type : nullptr);
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

const TypeSymbol*
EmitAnalysis::migration_class_or_record_type_value_symbol(
    const Expr& e) {
  const TypeSymbol* symbol = migration_type_symbol_for_expression(e);
  const TypeSymbol* payload =
      symbol && symbol->descriptor && symbol->descriptor->symbol
          ? symbol->descriptor->symbol
          : symbol;
  if (payload && (payload->class_info() || payload->record_info())) {
    return payload;
  }
  return nullptr;
}

const TypeSymbol* EmitAnalysis::concrete_class_symbol_for_metaclass_value(
    const Expr& e) {
  if (const TypeSymbol* symbol =
          migration_class_or_record_type_value_symbol(e)) {
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

  return registry_.class_ancestor_depth(*target_class, *concrete_class) >= 0
             ? concrete
             : nullptr;
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
    // Typecasts share call syntax with intrinsics. Ask the ordered migration
    // resolver whether this spelling denotes a nearer Pascal type before
    // accepting the implicit runtime callable.
    if (migration_type_symbol_for_expression(callee)) {
      return std::nullopt;
    }
    const std::string& name = static_cast<const Ident&>(callee).name;
    ResolveResult resolved = resolve_name_provider_.resolve_name(name);
    return resolved.kind == ResolvedKind::RtBuiltin
               ? std::optional<std::string>(name)
               : std::nullopt;
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

bool EmitAnalysis::unqualified_special_form_is_shadowed(
    const Expr& callee, std::string_view name) {
  if (callee.kind != Kind::Ident ||
      static_cast<const Ident&>(callee).name != name) {
    return false;
  }
  ResolveResult resolved =
      resolve_name_provider_.resolve_name(std::string(name));
  return resolved.kind != ResolvedKind::Unknown &&
         resolved.kind != ResolvedKind::RtBuiltin;
}

}  // namespace tp2cc
