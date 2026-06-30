#include "emit_analysis.h"

#include <algorithm>
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

static const RecordInfo* record_info_for_visible_type(
    const TypeRegistry& registry, const ScopeStateView& scope,
    const TypeExpr* type) {
  if (!type) return nullptr;
  const TypeLookupContext* context = registry.lookup_context_for_type(type);
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry, scope, type, context);
  symbol = descriptor_payload_symbol(symbol);
  return symbol ? symbol->record_info() : nullptr;
}

static const TypeExpr* descriptor_payload_type_for_expr(
    const TypeRegistry& registry, const ScopeStateView& scope,
    const TypeExpr* type, const TypeLookupContext*& context) {
  if (!type) return nullptr;
  if (const TypeLookupContext* own_context =
          registry.lookup_context_for_type(type)) {
    context = own_context;
  }
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry, scope, type, context);
  const TypeExpr* payload = descriptor_payload_type(symbol);
  if (!payload) return nullptr;
  if (const TypeLookupContext* payload_context =
          registry.lookup_context_for_type(payload)) {
    context = payload_context;
  }
  return payload;
}

static const TypeSymbol* descriptor_payload_symbol_for_expr(
    const TypeRegistry& registry, const ScopeStateView& scope,
    const TypeExpr* type, const TypeLookupContext*& context) {
  if (!type) return nullptr;
  if (const TypeLookupContext* own_context =
          registry.lookup_context_for_type(type)) {
    context = own_context;
  }
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry, scope, type, context);
  return descriptor_payload_symbol(symbol);
}

static bool is_local_type_name(const TypeRegistry& registry,
                               const ScopeStateView& scope,
                               const TypeExpr* type) {
  // The type expression already went through build() binding; locality is a
  // property of that chosen symbol, not another lookup by the source spelling.
  if (!type || type->kind != Kind::TyName) return false;
  const TypeSymbol* symbol = registry.resolved_symbol_for_type(type);
  if (!symbol) return false;
  for (const TypeLookupContext* frame = scope.type_scope; frame;
       frame = frame->parent) {
    for (const auto& [_, scoped_symbol] : frame->type_symbols) {
      if (scoped_symbol == symbol) return true;
    }
  }
  return false;
}

// Method bodies are emitted outside the Pascal class declaration, but an
// unqualified type such as `TInner' in that body can still be a lexical child
// of the method owner. Return the full source owner path so class/member lookup
// walks `TOuter.TInner' rather than treating `TInner' as a unit-level type.
static std::string local_member_owner_type_name(
    const TypeRegistry& registry, const ScopeStateView& scope,
    const TypeExpr* type) {
  if (!type || type->kind != Kind::TyName) return {};
  const TypeSymbol* symbol = registry.resolved_symbol_for_type(type);
  if (!symbol || !is_local_type_name(registry, scope, type)) return {};
  symbol = descriptor_payload_symbol(symbol);
  if (!symbol->class_info() && !symbol->record_info() &&
      !symbol->interface_info()) {
    return {};
  }
  return type_symbol_source_name(*symbol);
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
  if (!proc->return_type_name.empty()) {
    return named_pascal_type(proc->return_type_name);
  }
  return nullptr;
}

// Synthesized analysis types borrow child nodes from the parsed AST. They are
// interned by semantic identity before storage, so repeated type queries do
// not grow permanent analysis state.
static TypePtr borrowed_ast_type(const TypeExpr* type) {
  return TypePtr(const_cast<TypeExpr*>(type), [](TypeExpr*) {});
}

static const TypeExpr* record_field_type(const RecordField& field,
                                         std::string_view lower_field_name) {
  for (const auto& name : field.names) {
    if (ascii_lower(name) == lower_field_name) return field.type.get();
  }
  return nullptr;
}

static const TypeExpr* variant_part_field_type(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::string_view lower_field_name) {
  if (!vpart) return nullptr;
  if (!vpart->tag_name.empty() &&
      ascii_lower(vpart->tag_name) == lower_field_name) {
    return vpart->tag_type.get();
  }
  for (const auto& vc : vpart->cases) {
    for (const auto& field : vc.fields) {
      if (const TypeExpr* ft = record_field_type(field, lower_field_name)) {
        return ft;
      }
    }
    if (const TypeExpr* ft =
            variant_part_field_type(vc.variant_part, lower_field_name)) {
      return ft;
    }
  }
  return nullptr;
}

static bool variant_part_has_payload_field(
    const std::shared_ptr<ast::VariantPart>& vpart,
    std::string_view lower_field_name) {
  if (!vpart) return false;
  for (const auto& vc : vpart->cases) {
    for (const auto& field : vc.fields) {
      if (record_field_type(field, lower_field_name)) return true;
    }
    if (variant_part_has_payload_field(vc.variant_part, lower_field_name)) {
      return true;
    }
  }
  return false;
}

static const TypeExpr* method_result_type(const MethodSig* method) {
  if (!method) return nullptr;
  if (!method->return_type_name.empty()) {
    return named_pascal_type(method->return_type_name);
  }
  if (!method->decl || !method->decl->return_type) return nullptr;
  return method->decl->return_type.get();
}

static bool type_is_pointer_arithmetic_operand(const TypeExpr* t) {
  return t && t->kind == Kind::TyPointer;
}

static const TypeExpr* primitive_type_expr_for_name(std::string_view name) {
  const std::string low = ascii_lower(name);
  return is_primitive_type(low) ? named_pascal_type(low) : nullptr;
}

static const TypeExpr* lo_hi_result_type_for_bits(uint8_t bits) {
  if (bits <= 16) return builtin_integer_type("byte");
  if (bits <= 32) return builtin_integer_type("word");
  return builtin_integer_type("cardinal");
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
  if (const TypeExpr* payload =
          descriptor_payload_type_for_expr(registry_, scope_, t, context)) {
    return payload;
  }
  return t;
}

const TypeExpr* EmitAnalysis::ord_result_type_for_type(const TypeExpr* t) {
  t = semantic_shape_type(t);
  if (!t) return nullptr;
  if (t->kind == Kind::TyName) {
    std::string atom = builtin_atom_name_for_type(t);
    if (atom.empty()) {
      atom = ascii_lower(static_cast<const TyName&>(*t).name);
    }
    if (primitive_name_is_charish(atom) || atom == "boolean") {
      return builtin_integer_type("byte");
    }
    if (atom == "widechar") return builtin_integer_type("word");
    if (atom == "bytebool") return builtin_integer_type("shortint");
    if (atom == "wordbool") return builtin_integer_type("smallint");
    if (atom == "longbool") return builtin_integer_type("longint");
    if (atom == "qwordbool") return builtin_integer_type("int64");
    if (atom == "boolean64") return builtin_integer_type("qword");
    if (const PrimitiveInfo* info = primitive_info_for_type(t);
        info && info->int_kind != PrimitiveIntKind::None) {
      return builtin_integer_type(atom);
    }
    const TypeLookupContext* context = registry_.lookup_context_for_type(t);
    const TypeSymbol* symbol =
        resolved_type_symbol_in_context(registry_, scope_, t, context);
    symbol = descriptor_payload_symbol(symbol);
    if (symbol && symbol->enum_info()) {
      return builtin_integer_type("longint");
    }
    return nullptr;
  }
  if (t->kind == Kind::TyEnum) return builtin_integer_type("longint");
  if (t->kind == Kind::TySubrange) return t;
  if (t->kind == Kind::TyDistinct) {
    const auto& d = static_cast<const TyDistinct&>(*t);
    return ord_result_type_for_type(d.underlying.get()) ? t : nullptr;
  }
  return nullptr;
}

const TypeExpr* EmitAnalysis::ord_result_type_for_operand(
    const Expr& operand) {
  return ord_result_type_for_type(deduce_type(operand));
}

const ClassInfo* EmitAnalysis::migration_fallback_class_info_by_name(std::string_view name) const {
  std::string low = ascii_lower(name);
  const TypeSymbol* symbol =
      migration_fallback_type_symbol_by_name(registry_, scope_, low);
  symbol = descriptor_payload_symbol(symbol);
  return symbol ? symbol->class_info() : nullptr;
}

const ClassInfo* EmitAnalysis::class_info_for_type(const TypeExpr* t) {
  return class_info_for_type_in_context(t, nullptr);
}

const ClassInfo* EmitAnalysis::class_info_for_type_in_context(
    const TypeExpr* t, const TypeLookupContext* context) {
  if (!t) return nullptr;
  if (const TypeLookupContext* own_context =
          registry_.lookup_context_for_type(t)) {
    context = own_context;
  }
  if (const TypeSymbol* symbol =
          resolved_type_symbol_in_context(registry_, scope_, t, context)) {
    symbol = descriptor_payload_symbol(symbol);
    if (const ClassInfo* ci = symbol->class_info()) return ci;
  }
  return nullptr;
}

const InterfaceInfo* EmitAnalysis::interface_info_for_type(const TypeExpr* t) {
  if (!t) return nullptr;
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  if (const TypeSymbol* symbol =
          resolved_type_symbol_in_context(registry_, scope_, t, context)) {
    symbol = descriptor_payload_symbol(symbol);
    if (const InterfaceInfo* info = symbol->interface_info()) return info;
  }
  return nullptr;
}

std::string EmitAnalysis::direct_type_name(const TypeExpr* t) {
  if (!t) return {};
  if (const TypeSymbol* symbol = registry_.resolved_symbol_for_type(t)) {
    return type_symbol_source_name(*symbol);
  }
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  if (const TypeSymbol* symbol =
          resolved_type_symbol_in_context(registry_, scope_, t, context)) {
    return type_symbol_source_name(*symbol);
  }
  return {};
}

std::string EmitAnalysis::pointer_target_type_name(const TypeExpr* t) {
  t = semantic_shape_type(t);
  if (!t || t->kind != Kind::TyPointer) return {};
  const TypeExpr* target = static_cast<const TyPointer&>(*t).target.get();
  return direct_type_name(target);
}

const TypeExpr* EmitAnalysis::migration_fallback_named_type_expr_by_name(std::string_view name) {
  std::string low = ascii_lower(name);

  if (const TypeSymbol* symbol =
          migration_fallback_type_symbol_by_name(registry_, scope_, low)) {
    if (symbol->type) {
      const bool lexical =
          migration_fallback_lexical_type_symbol_by_name(registry_, scope_, low) ==
          symbol;
      if (!lexical && low.find('.') == std::string::npos &&
          !symbol->defining_unit.empty() &&
          symbol->defining_unit != scope_.current_unit_name) {
        return named_pascal_type(symbol->defining_unit + "." + symbol->name);
      }
      return named_pascal_type(name);
    }
  }

  return nullptr;
}

const TypeExpr* EmitAnalysis::explicit_typecast_result_type(const Expr& e) {
  if (e.kind != Kind::Call) return nullptr;
  const auto& c = static_cast<const Call&>(e);
  if (c.args.size() != 1 || !c.callee) return nullptr;

  if (c.callee->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*c.callee);
    if (const TypeExpr* named = migration_fallback_named_type_expr_by_name(id.name)) return named;
    if (const TyName* int_ty = builtin_integer_type(id.name)) return int_ty;
    if (const TypeExpr* primitive = primitive_type_expr_for_name(id.name)) {
      return primitive;
    }
    return nullptr;
  }

  if (c.callee->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*c.callee);
    if (auto unit_member = resolve_unit_qualified_member(mem);
        unit_member && unit_member->resolved.kind == ResolvedKind::UnitType) {
      return migration_fallback_named_type_expr_by_name(unit_member->unit_name + "." + mem.name);
    }
  }

  return nullptr;
}

std::string EmitAnalysis::metaclass_target_name(const TypeExpr* t) {
  if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
    return type_symbol_source_name(*target);
  }
  t = semantic_shape_type(t);
  if (const TypeSymbol* target = registry_.metaclass_target_for_type(t)) {
    return type_symbol_source_name(*target);
  }
  if (!t || t->kind != Kind::TyMetaclass) return {};
  return static_cast<const TyMetaclass&>(*t).class_name;
}

bool EmitAnalysis::type_is_runtime_tclass(const TypeExpr* t) {
  if (!t) return false;
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry_, scope_, t, context);
  symbol = descriptor_payload_symbol(symbol);
  return symbol && symbol->defining_unit == "__builtin__" &&
         symbol->name == "tclass";
}

bool EmitAnalysis::type_accepts_class_value(const TypeExpr* t) {
  return !metaclass_target_name(t).empty() || type_is_runtime_tclass(t);
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

bool EmitAnalysis::const_param_needs_const_ref(const TypeExpr* t) {
  // Pascal `const` is not a blanket request for C++ reference semantics.
  // Keep the default path as plain-by-value unless the source model proves we
  // need aliasing; otherwise reviewability suffers because "harmless" params
  // silently become storage-identity-preserving references.
  (void)t;
  return false;
}

const TypeExpr* EmitAnalysis::deduce_const_decl_type(const ConstDecl& cd) {
  if (cd.type) return cd.type.get();
  auto info = eval_const_int_expr(*cd.value);
  if (info && info->type) return builtin_integer_type(info->type);
  if (cd.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*cd.value));
  }
  return cd.value ? deduce_type(*cd.value) : nullptr;
}

const TypeExpr* EmitAnalysis::deduce_const_info_type(const ConstInfo& c) {
  if (c.type) return c.type.get();
  if (!c.value) return nullptr;
  auto info = eval_const_int_expr(*c.value);
  if (info && info->type) return builtin_integer_type(info->type);
  if (c.value->kind == Kind::SetLit) {
    return deduce_set_literal_type(static_cast<const SetLit&>(*c.value));
  }
  return deduce_type(*c.value);
}

const TySet* EmitAnalysis::synthesize_set_type(
    const TypeExpr* element,
    std::optional<std::pair<int64_t, int64_t>> explicit_bounds) {
  if (!element) return nullptr;
  const bool has_explicit_bounds = explicit_bounds.has_value();
  const int64_t explicit_low =
      has_explicit_bounds ? explicit_bounds->first : 0;
  const int64_t explicit_high =
      has_explicit_bounds ? explicit_bounds->second : 0;
  SynthesizedSetKey key{.element = element,
                        .has_explicit_bounds = has_explicit_bounds,
                        .low = explicit_low,
                        .high = explicit_high};
  if (auto it = synthesized_set_types_.find(key);
      it != synthesized_set_types_.end()) {
    return it->second.get();
  }
  auto tp = std::make_shared<TySet>(
      element->loc, borrowed_ast_type(element), has_explicit_bounds,
      explicit_low, explicit_high);
  synthesized_set_types_.emplace(key, tp);
  return tp.get();
}

const TyPointer* EmitAnalysis::synthesize_pointer_type(
    const TypeExpr* target) {
  if (!target) return nullptr;
  if (auto it = synthesized_pointer_types_.find(target);
      it != synthesized_pointer_types_.end()) {
    return it->second.get();
  }
  auto tp = std::make_shared<TyPointer>(
      target->loc, borrowed_ast_type(target));
  synthesized_pointer_types_.emplace(target, tp);
  return tp.get();
}

bool EmitAnalysis::same_type_ast(const TypeExpr* a, const TypeExpr* b) {
  const TypeDescriptor* initial_ad = registry_.descriptor_for_type(a);
  const TypeDescriptor* initial_bd = registry_.descriptor_for_type(b);
  if (!initial_ad) {
    const TypeLookupContext* context = registry_.lookup_context_for_type(a);
    if (const TypeSymbol* symbol =
            resolved_type_symbol_in_context(registry_, scope_, a, context)) {
      initial_ad = symbol->descriptor;
    }
  }
  if (!initial_bd) {
    const TypeLookupContext* context = registry_.lookup_context_for_type(b);
    if (const TypeSymbol* symbol =
            resolved_type_symbol_in_context(registry_, scope_, b, context)) {
      initial_bd = symbol->descriptor;
    }
  }
  if (initial_ad && initial_bd) return initial_ad == initial_bd;

  a = semantic_shape_type(a);
  b = semantic_shape_type(b);
  if (!a || !b) return false;
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case Kind::TyName: {
      const std::string an =
          ascii_lower(static_cast<const TyName&>(*a).name);
      const std::string bn =
          ascii_lower(static_cast<const TyName&>(*b).name);
      const TypeDescriptor* ad = registry_.descriptor_for_type(a);
      const TypeDescriptor* bd = registry_.descriptor_for_type(b);
      if (ad || bd) return ad && ad == bd;
      return an == bn;
    }
    case Kind::TyDistinct:
      return same_type_ast(static_cast<const TyDistinct&>(*a).underlying.get(),
                             static_cast<const TyDistinct&>(*b).underlying.get());
    case Kind::TyPointer: {
      const auto& ap = static_cast<const TyPointer&>(*a);
      const auto& bp = static_cast<const TyPointer&>(*b);
      if (!ap.target || !bp.target) return !ap.target && !bp.target;
      return same_type_ast(ap.target.get(), bp.target.get());
    }
    case Kind::TySubrange: {
      const auto& as = static_cast<const TySubrange&>(*a);
      const auto& bs = static_cast<const TySubrange&>(*b);
      auto alo = eval_ordinal_expr(*as.lo);
      auto ahi = eval_ordinal_expr(*as.hi);
      auto blo = eval_ordinal_expr(*bs.lo);
      auto bhi = eval_ordinal_expr(*bs.hi);
      return alo && ahi && blo && bhi && alo->family == ahi->family &&
             alo->family == blo->family && blo->family == bhi->family &&
             alo->enum_key == ahi->enum_key && alo->enum_key == blo->enum_key &&
             blo->enum_key == bhi->enum_key && alo->value == blo->value &&
             ahi->value == bhi->value;
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
    const std::string low = ascii_lower(static_cast<const Ident&>(e).name);
    if (low == "false" || low == "true") {
      return OrdinalExprValue{(low == "true") ? 1 : 0,
                              OrdinalFamily::Boolean, nullptr};
    }
    if (const auto* info = find_visible_enum_info_for_member(low)) {
      for (size_t i = 0; i < info->members.size(); ++i) {
        if (info->members[i] == low) {
          return OrdinalExprValue{static_cast<int64_t>(i), OrdinalFamily::Enum,
                                  info->type};
        }
      }
    }
  }
  if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (mem.base->kind == Kind::Ident) {
      const std::string unit =
          ascii_lower(static_cast<const Ident&>(*mem.base).name);
      const std::string member = ascii_lower(mem.name);
      if (const TyEnum* enum_type =
              registry_.lookup_enum_member_in_unit(unit, member)) {
        if (const EnumInfoReg* info =
                registry_.enum_info_for_type(enum_type)) {
          for (size_t i = 0; i < info->members.size(); ++i) {
            if (info->members[i] == member) {
              return OrdinalExprValue{static_cast<int64_t>(i),
                                      OrdinalFamily::Enum, info->type};
            }
          }
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
        .enum_key = &en};
  }
  if (t->kind != Kind::TyName) return std::nullopt;

  std::string atom = builtin_atom_name_for_type(t);
  if (atom.empty()) {
    atom = ascii_lower(static_cast<const TyName&>(*t).name);
  }
  if (atom == "boolean") {
    return OrdinalDomain{OrdinalFamily::Boolean, 0, 1, {}};
  }
  if (primitive_name_is_charish(atom)) {
    return OrdinalDomain{OrdinalFamily::Char, 0, 255, {}};
  }
  if (atom == "widechar") {
    return OrdinalDomain{OrdinalFamily::WideChar, 0, 65535, {}};
  }
  if (const auto* info = primitive_info_for_type(t);
      info && info->int_kind != PrimitiveIntKind::None) {
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
  const TypeSymbol* symbol =
      resolved_type_symbol_in_context(registry_, scope_, t, context);
  symbol = descriptor_payload_symbol(symbol);
  const EnumInfoReg* info = symbol ? symbol->enum_info() : nullptr;
  if (!info || !info->type) {
    return std::nullopt;
  }
  // EnumInfoReg::type points at the TyEnum AST node from the original
  // `type T = (...)` declaration. That pointer is the enum's identity: the
  // raw-TyEnum branch above and this TyName-canonicalised branch always
  // converge on the same pointer for the same Pascal enum, regardless of
  // which path the type expression took to get here.
  return OrdinalDomain{
      .family = OrdinalFamily::Enum,
      .low = 0,
      .high = info->members.empty()
                  ? 0
                  : static_cast<int64_t>(info->members.size() - 1),
      .enum_key = info->type};
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
    return SetLiteralOrdinalSummary{
        ordinal->family,
        ordinal->enum_key,
        ordinal->family == OrdinalFamily::Enum ? deduce_type(e) : nullptr,
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
      summary->enum_type,
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

  const TypeExpr* element_type = nullptr;
  switch (summary->family) {
    case OrdinalFamily::Integer:
      // Untyped integer set literals should not inherit the accidental
      // narrow C++ type of their first constant/member. Keep the Pascal
      // type broad (`longint`) and preserve the actual range separately.
      element_type = builtin_integer_type("longint");
      break;
    case OrdinalFamily::Boolean:
      element_type = builtin_boolean_type();
      break;
    case OrdinalFamily::Char:
      element_type = builtin_char_type();
      break;
    case OrdinalFamily::WideChar:
      element_type = named_pascal_type("widechar");
      break;
    case OrdinalFamily::Enum:
      element_type = summary->enum_type;
      break;
    case OrdinalFamily::Invalid:
      return nullptr;
  }
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
  const bool source_is_unsigned =
      value.type && value.type->int_kind == PrimitiveIntKind::Unsigned;
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
  return ConvertedConstInt{.value = converted_value, .bits = bits, .type = info};
}

std::optional<ConstIntExprInfo> EmitAnalysis::eval_const_int_cast(
    const Call& c,
    std::unordered_set<std::string>* visiting_const_names) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return std::nullopt;
  const auto& callee = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(callee.name)) return std::nullopt;
  auto* cast_type = builtin_integer_type(callee.name);
  if (!cast_type) return std::nullopt;
  auto arg = eval_const_int_expr(*c.args[0], visiting_const_names);
  if (!arg) return std::nullopt;
  auto converted =
      convert_const_int_value(c.loc, *arg, cast_type, true, false);
  if (!converted) return std::nullopt;
  return ConstIntExprInfo{converted->value, converted->bits, converted->type};
}

const TypeExpr* EmitAnalysis::const_intrinsic_type_arg(const Expr& arg) {
  // Constant intrinsics such as `Ord(High(T))` receive their type operand
  // through expression syntax. Resolve only the Pascal type forms accepted in
  // that context; ordinary value identifiers are folded elsewhere.
  if (arg.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(arg);
    const std::string low = ascii_lower(id.name);
    if (const TypeSymbol* symbol =
            migration_fallback_lexical_type_symbol_by_name(registry_, scope_, low)) {
      return descriptor_payload_type(symbol);
    }
    return migration_fallback_named_type_expr_by_name(id.name);
  }
  if (arg.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(arg);
    if (mem.base && mem.base->kind == Kind::Ident) {
      const auto& unit = static_cast<const Ident&>(*mem.base);
      return migration_fallback_named_type_expr_by_name(unit.name + "." + mem.name);
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
  const std::string atom = builtin_atom_name_for_type(t);
  return atom == "shortstring" || atom == "ansistring" ||
         atom == "utf8string";
}

const TypeExpr* EmitAnalysis::canonical_set_type(const TypeExpr* t) {
  if (!t) return nullptr;
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  const TypeExpr* c = semantic_shape_type(t, context);
  return (c && c->kind == Kind::TySet) ? c : nullptr;
}

const PrimitiveInfo* EmitAnalysis::primitive_info_for_type(const TypeExpr* t) {
  const std::string atom = builtin_atom_name_for_type(t);
  if (!atom.empty()) {
    if (const PrimitiveInfo* info = primitive_info(atom)) return info;
  }
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  t = semantic_shape_type(t, context);
  if (!t || t->kind != Kind::TyName) return nullptr;
  return primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
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
  const std::string atom = builtin_atom_name_for_type(t);
  return atom == "ansistring" || atom == "utf8string";
}

std::string EmitAnalysis::builtin_atom_name_for_type(const TypeExpr* t) {
  const TypeLookupContext* context = registry_.lookup_context_for_type(t);
  if (const TypeSymbol* symbol =
          descriptor_payload_symbol_for_expr(registry_, scope_, t, context)) {
    if (symbol->defining_unit == "__builtin__") {
      return ascii_lower(symbol->name);
    }
  }

  const TypeExpr* shape = semantic_shape_type(t, context);
  if (shape && shape->kind == Kind::TyName) {
    const std::string low =
        ascii_lower(static_cast<const TyName&>(*shape).name);
    if (registry_.builtin_literal(low)) return low;
  }
  return {};
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
    if (high <= signed_max_for_bits(8)) return builtin_integer_type("shortint");
    if (high <= signed_max_for_bits(16)) return builtin_integer_type("smallint");
    if (high <= signed_max_for_bits(32)) return builtin_integer_type("longint");
    return builtin_integer_type("int64");
  }
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(8)))
    return builtin_integer_type("byte");
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(16)))
    return builtin_integer_type("word");
  if (high <= static_cast<int64_t>(unsigned_mask_for_bits(32)))
    return builtin_integer_type("cardinal");
  return builtin_integer_type("qword");
}

const TypeExpr* EmitAnalysis::deduce_binary_expr_type(const Binary& b) {
  if (b.op == BinOp::Is) return builtin_boolean_type();
  if (b.op == BinOp::In) return builtin_boolean_type();
  if (b.op == BinOp::As) {
    if (b.rhs->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*b.rhs);
      if (const TypeExpr* named = migration_fallback_named_type_expr_by_name(id.name)) return named;
    }
    return nullptr;
  }

  const TypeExpr* lt = deduce_type(*b.lhs);
  const TypeExpr* rt = deduce_type(*b.rhs);
  const TypeExpr* ltc = canonicalize_for_arithmetic(lt);
  const TypeExpr* rtc = canonicalize_for_arithmetic(rt);

  if (binop_is_comparison(b.op)) return builtin_boolean_type();
  if (binop_is_arithmetic_like(b.op)) {
    if (b.op == BinOp::RealDiv) {
      if (same_type_ast(lt, rt) && type_is_numeric_primitive(lt)) {
        const PrimitiveInfo* pi = primitive_info_for_type(lt);
        if (pi && pi->int_kind == PrimitiveIntKind::None) return lt;
      }
      if (type_is_numeric_primitive(lt) && type_is_numeric_primitive(rt)) {
        const PrimitiveInfo* lp = primitive_info_for_type(lt);
        const PrimitiveInfo* rp = primitive_info_for_type(rt);
        if (lp && lp->int_kind == PrimitiveIntKind::None) return lt;
        if (rp && rp->int_kind == PrimitiveIntKind::None) return rt;
        return named_pascal_type("real");
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
      if (lptr && rptr) return named_pascal_type("ptrint");
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
    if (type_is_long_string(lt) || type_is_long_string(rt)) {
      return named_pascal_type("ansistring");
    }
    return named_pascal_type("shortstring");
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
    // Open and dynamic arrays have no declared Pascal index subrange in the
    // type. The emitted `high(a)` value is derived from runtime length, whose
    // runtime helper uses the compiler's LongInt carrier.
    return builtin_integer_type("longint");
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
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      if (n.value > static_cast<uint64_t>(INT64_MAX)) {
        return ConstIntExprInfo{static_cast<int64_t>(n.value), n.value,
                                primitive_info("qword")};
      }
      int64_t value = static_cast<int64_t>(n.value);
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
    }
    case Kind::Unary: {
      const auto& u = static_cast<const Unary&>(e);
      auto operand = eval_const_int_expr(*u.operand, visiting_const_names);
      if (!operand) return std::nullopt;
      if (u.op == UnOp::Plus) return operand;
      if (u.op != UnOp::Neg) return std::nullopt;
      int64_t value = 0;
      if (!checked_sub_int64(0, operand->value, &value)) return std::nullopt;
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
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
          if (!checked_pascal_shl_int64(lhs->value, lhs->type, rhs->value, &value, target_)) {
            return std::nullopt;
          }
          return ConstIntExprInfo{value, shift_carrier_primitive(lhs->type, target_)};
        case BinOp::Shr:
          if (!checked_pascal_shr_int64(lhs->value, lhs->type, rhs->value, &value, target_)) {
            return std::nullopt;
          }
          return ConstIntExprInfo{value, shift_carrier_primitive(lhs->type, target_)};
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
      return ConstIntExprInfo{value, primitive_info_for_value(value)};
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
            return ConstIntExprInfo{value, primitive_info_for_value(value)};
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
            return ConstIntExprInfo{value, primitive_info_for_value(value)};
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
                    return ConstIntExprInfo{value,
                                            primitive_info_for_value(value)};
                  }
                }
              }
            }
          }
          if (auto ordinal = eval_ordinal_expr(*c.args[0])) {
            return ConstIntExprInfo{ordinal->value,
                                    primitive_info_for_value(ordinal->value)};
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
  switch (e.kind) {
    case Kind::BoolLit:
      return builtin_boolean_type();
    case Kind::RealLit:
      // A real literal is target-convertible, but its standalone Pascal type is
      // `Real`; call validation then applies the normal real-widening rank for
      // `Single`/`Double`/`Extended` formals.
      return named_pascal_type("real");
    case Kind::IntLit:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      return nullptr;
    case Kind::Unary:
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
      }
      return deduce_type(*static_cast<const Unary&>(e).operand);
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(e);
      if (b.op == BinOp::Shl || b.op == BinOp::Shr) {
        const TypeExpr* lt = deduce_type(*b.lhs);
        if (lt) lt = canonicalize_for_arithmetic(lt);
        if (const PrimitiveInfo* pi = primitive_info_for_type(lt);
            pi && shift_carrier_primitive(pi, target_)) {
          return builtin_integer_type(shift_carrier_primitive(pi, target_));
        }
      }
      if (auto info = eval_const_int_expr(e); info && info->type) {
        return builtin_integer_type(info->type);
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
      if (nit != scope_.local_nested_fns.end() && nit->second.size() == 1 &&
          nit->second.front().is_function) {
        return nit->second.front().return_type;
      }
      if (scope_.local_untyped_params.count(id.name)) {
        // Untyped Pascal params are raw storage slots. Treat the identifier
        // itself as Pascal `pointer` so pointer-slot assignments/casts can
        // still apply the central coercion rules instead of falling back to
        // a naked C++ `void*` assignment.
        return named_pascal_type("pointer");
      }
      // `self` inside a method has the surrounding class as its type. Pascal
      // passes records/objects by reference and classes by implicit pointer,
      // but those subtleties are an emit-time concern; for type deduction the
      // class name is what every other path (member lookup, meta-class target,
      // etc.) already uses, so returning it here keeps deduce_type consistent
      // instead of silently dropping the type.
      if (id.name == "self" && !scope_.current_class_name.empty()) {
        return named_pascal_type(scope_.current_class_name);
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
      // Class member lookup inside a known method body.
      if (!scope_.current_class_name.empty()) {
        if (auto* f = registry_.lookup_class_field(
                scope_.current_class_name, id.name, scope_.current_unit_name)) {
          return f->type.get();
        }
        if (auto* p = registry_.lookup_class_property(
                scope_.current_class_name, id.name, scope_.current_unit_name)) {
          return p->type.get();
        }
        if (const TypeExpr* mt = method_result_type(unique_zero_arg_method(
                registry_.lookup_class_methods(scope_.current_class_name,
                                                id.name,
                                                scope_.current_unit_name)))) {
          return mt;
        }
      }
      // `with X do` bindings contribute fields/properties/methods from their
      // target type. The innermost active `with` binding wins.
      for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend();
           ++it) {
        const std::string& ac = it->class_name;
        if (!ac.empty()) {
          if (auto* f = registry_.lookup_class_field(
                  ac, id.name, scope_.current_unit_name)) {
            return f->type.get();
          }
          if (auto* p = registry_.lookup_class_property(
                  ac, id.name, scope_.current_unit_name)) {
            return p->type.get();
          }
          if (const TypeExpr* mt = method_result_type(unique_zero_arg_method(
                  registry_.lookup_class_methods(ac, id.name,
                                                  scope_.current_unit_name)))) {
            return mt;
          }
        }
        if (const TypeExpr* rf = lookup_record_field_type_in_with(*it, id.name)) {
          return rf;
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
        if (const TypeExpr* pt = proc_result_type(
                unique_zero_arg_proc(scope_frame_find_procs(*frame,
                                                            id.name)))) {
          return pt;
        }
        if (scope_frame_has_enum_member(*frame, id.name)) {
          if (const auto* t =
                  registry_.lookup_enum_member_in_unit(frame->unit, id.name)) {
            return t;
          }
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
      if (t == named_pascal_type("pchar") ||
          t == named_pascal_type("pansichar")) {
        return builtin_char_type();
      }
      if (t && t->kind == Kind::TyPointer) {
        return static_cast<const TyPointer&>(*t).target.get();
      }
      return nullptr;
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (auto unit_member = resolve_unit_qualified_member(m)) {
        if (unit_member->resolved.kind == ResolvedKind::EnumMember) {
          return registry_.lookup_enum_member_in_unit(unit_member->unit_name,
                                                       m.name);
        }
      }
      const TypeExpr* base_type = deduce_type(*m.base);
      if (const TypeExpr* rf =
              lookup_record_field_type_in_type(base_type, m.name)) {
        return rf;
      }
      std::string cls;
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        if (id.name == "self") cls = scope_.current_class_name;
        else if (const TypeSymbol* symbol = descriptor_payload_symbol(
                     migration_fallback_type_symbol_by_name(registry_, scope_,
                                                            id.name));
                 symbol && (symbol->class_info() || symbol->record_info())) {
          cls = id.name;
        }
      }
      if (cls.empty()) {
        // Chained accesses like `x.sym.name` and result-slot writes like
        // `clone.next := nil` must recover the Pascal class alias from the
        // base expression, not from the canonicalized class body node.
        if (base_type) cls = metaclass_target_name(base_type);
      }
      if (cls.empty()) cls = deduce_class_alias(*m.base);
      if (cls.empty()) {
        return nullptr;
      }
      if (const TypeExpr* mt = method_result_type(unique_zero_arg_method(
              registry_.lookup_class_methods(cls, m.name,
                                              scope_.current_unit_name)))) {
        return mt;
      }
      if (auto* pf = registry_.lookup_class_field(
              cls, m.name, scope_.current_unit_name)) {
        return pf->type.get();
      }
      if (auto* pp = registry_.lookup_class_property(
              cls, m.name, scope_.current_unit_name)) {
        return pp->type.get();
      }
      if (auto* rf = registry_.lookup_record_field(
              cls, m.name, scope_.current_unit_name)) {
        return rf->type.get();
      }
      // Procedure-local record aliases can still produce a non-empty
      // `cls` (e.g. `p^.field` through `type PDir = ^TDir;`) even though the
      // alias is not in the global registry maps above. Fall back to direct
      // structural field lookup on the deduced base type before giving up.
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
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = scope_.current_class_name;
        } else {
          cls = deduce_class_alias(*mem.base);
        }
        if (!cls.empty()) {
          if (auto* prop = registry_.lookup_class_property(
                  cls, mem.name, scope_.current_unit_name);
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
      const TypeExpr* bt = deduce_type(*ix.base);
      if (!bt) return nullptr;
      bt = semantic_shape_type(bt);
      if (bt && bt->kind == Kind::TyString) return builtin_char_type();
      if (type_is_string_like(bt)) {
        return builtin_char_type();
      }
      if (bt == named_pascal_type("pchar") ||
          bt == named_pascal_type("pansichar")) {
        return builtin_char_type();
      }
      if (bt && bt->kind == Kind::TyArray) {
        return static_cast<const TyArray&>(*bt).element.get();
      }
      if (bt && bt->kind == Kind::TyPointer) {
        return static_cast<const TyPointer&>(*bt).target.get();
      }
      {
        std::string cls = deduce_class_alias(*ix.base);
        if (!cls.empty()) {
          if (auto* prop = registry_.lookup_default_property(
                  cls, scope_.current_unit_name)) {
            return prop->type.get();
          }
        }
      }
      return nullptr;
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Intrinsics that work the same whether spelled `low(t)` or
      // `system.low(t)` are dispatched through intrinsic_call_name so the
      // Ident-callee and system-unit-Member-callee spellings share one table.
      if (auto intrinsic = intrinsic_call_name(*c.callee)) {
        const std::string& n = *intrinsic;
        if ((primitive_name_is_charish(n) || n == "chr") && c.args.size() == 1) {
          return builtin_char_type();
        }
        if (n == "ord" && c.args.size() == 1) {
          return ord_result_type_for_operand(*c.args[0]);
        }
        if ((n == "low" || n == "high") && c.args.size() == 1) {
          if (c.args[0]->kind == Kind::Ident) {
            const auto& arg_id = static_cast<const Ident&>(*c.args[0]);
            if (const TypeExpr* named = migration_fallback_named_type_expr_by_name(arg_id.name)) {
              return deduce_low_high_result_type(named);
            }
            if (const TyName* int_ty = builtin_integer_type(arg_id.name)) {
              return int_ty;
            }
          }
          return deduce_low_high_result_type(deduce_type(*c.args[0]));
        }
        if ((n == "lo" || n == "hi") && c.args.size() == 1) {
          const TypeExpr* arg_type = deduce_type(*c.args[0]);
          if (const TypeExpr* ord_type = ord_result_type_for_type(arg_type)) {
            arg_type = ord_type;
          }
          arg_type = semantic_shape_type(arg_type);
          if (const PrimitiveInfo* pi = primitive_info_for_type(arg_type);
              pi && pi->int_kind != PrimitiveIntKind::None) {
            return lo_hi_result_type_for_bits(primitive_bits(*pi, target_));
          }
        }
        if (n == "sizeof" && c.args.size() == 1) {
          return builtin_integer_type("longint");
        }
        if (n == "pointer" && c.args.size() == 1) {
          return named_pascal_type("pointer");
        }
        if ((n == "pchar" || n == "pansichar") && c.args.size() == 1) {
          return builtin_pchar_type();
        }
        if ((n == "succ" || n == "pred" ||
             n == "abs" || n == "sqr") &&
            c.args.size() == 1) {
          return deduce_type(*c.args[0]);
        }
        if (c.args.size() == 1) {
          const TypeExpr* cast_type = semantic_shape_type(named_pascal_type(n));
          if (type_is_string_like(cast_type)) {
            return cast_type;
          }
        }
      }
      const TypeExpr* callee_type = deduce_type(*c.callee);
      if (callee_type) callee_type = semantic_shape_type(callee_type);
      if (callee_type && callee_type->kind == Kind::TyProcedural) {
        const auto& p = static_cast<const TyProcedural&>(*callee_type);
        if (p.is_function) return p.return_type.get();
      }
      // Typecast `T(expr)` and alias-call forms only apply when the callee is
      // a bare identifier, since `system.T(expr)` would be a unit-qualified
      // access rather than a typecast in user code.
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        if (c.args.size() == 1) {
          // A one-argument call whose callee is a type name is a Pascal
          // typecast. The result type is the named type even when value
          // emission later decides between value-copy and storage-view
          // lowering for that cast.
          if (const TypeExpr* named = migration_fallback_named_type_expr_by_name(id.name)) {
            return named;
          }
          if (const TyName* int_ty = builtin_integer_type(id.name)) {
            return int_ty;
          }
          if (const TypeExpr* primitive = primitive_type_expr_for_name(id.name)) {
            return primitive;
          }
        }
        auto nit = scope_.local_nested_fns.find(id.name);
        if (nit != scope_.local_nested_fns.end() && nit->second.size() == 1 &&
            nit->second.front().is_function) {
          return nit->second.front().return_type;
        }
      }
      return call_type_provider_.type_for_resolved_call(c);
    }
    case Kind::StringLit: {
      const auto& sl = static_cast<const StringLit&>(e);
      return sl.value.size() == 1 ? builtin_char_type()
                                  : builtin_string_type();
    }
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
          if (scope_.local_untyped_params.count(id.name)) {
            return named_pascal_type("pointer");
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

std::string EmitAnalysis::deduce_class_alias(const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    // Fast path for `self`: the surrounding method already tells us the class.
    if (id.name == "self") return scope_.current_class_name;
  } else if (e.kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(e);
    if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*c.callee);
      const ClassInfo* ci = migration_fallback_class_info_by_name(id.name);
      if (ci && ci->is_reference_type) {
        return id.name;
      }
    }
  }
  const TypeExpr* t = deduce_type(e);
  if (!t) return {};
  // Returning the metaclass target or direct Pascal type name keeps class
  // alias recovery on the source-language path instead of canonicalized class
  // bodies. That matters for casts, `self`, and chained member accesses.
  if (auto cls = metaclass_target_name(t); !cls.empty()) return cls;
  if (auto cls = local_member_owner_type_name(registry_, scope_, t);
      !cls.empty()) {
    return cls;
  }
  if (!is_local_type_name(registry_, scope_, t)) {
    if (auto cls = direct_type_name(t);
        !cls.empty()) return cls;
  }
  const TypeExpr* canon = semantic_shape_type(t);
  if (auto cls = local_member_owner_type_name(registry_, scope_, canon);
      !cls.empty()) {
    return cls;
  }
  return direct_type_name(canon);
}

std::string EmitAnalysis::concrete_class_name_for_metaclass_value(
    const Expr& e) {
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (identifier_is_shadowed_value(id.name)) return {};
    if (const auto* ci = migration_fallback_class_info_by_name(id.name);
        ci && ci->is_reference_type) {
      // Alias declarations share the payload class descriptor. Return the
      // payload class name so the metaclass value function resolves to the
      // real class rather than the source spelling used at this site.
      return ci->name;
    }
    return {};
  }

  if (e.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(e);
    if (!mem.base || mem.base->kind != Kind::Ident) return {};
    const auto& base = static_cast<const Ident&>(*mem.base);
    const std::string qualified = base.name + "." + mem.name;
    if (const auto* ci = migration_fallback_class_info_by_name(qualified);
        ci && ci->is_reference_type) {
      return qualified;
    }
    if (auto unit_member = resolve_unit_qualified_member(mem);
        unit_member && unit_member->resolved.kind == ResolvedKind::UnitType) {
      if (const TypeExpr* named =
              migration_fallback_named_type_expr_by_name(unit_member->unit_name + "." + mem.name)) {
        const std::string direct = direct_type_name(named);
        if (!direct.empty()) {
          const ClassInfo* ci = class_info_for_type(named);
          if (ci && ci->is_reference_type) {
            return direct;
          }
        }
      }
    }
  }

  return {};
}

std::string EmitAnalysis::canonical_method_owner_type_name(
    std::string_view owner) {
  const std::string original = ascii_lower(owner);
  if (original.empty()) return original;

  const TypeSymbol* symbol =
      migration_fallback_type_symbol_by_name(registry_, scope_, original);
  if (!symbol) return original;

  // Pascal permits method implementations to name a class type alias:
  // `TAlias = TReal; procedure TAlias.M;`. C++ cannot define methods on a
  // `using`/typedef alias, so use the descriptor's owning class symbol for
  // out-of-class method definitions.
  if (const TypeSymbol* canonical = descriptor_payload_symbol(symbol);
      canonical && canonical->class_info()) {
    return type_symbol_source_name(*canonical);
  }
  return original;
}

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!type) return nullptr;
  const std::string lower_field_name = ascii_lower(std::string(field_name));
  if (const RecordInfo* record =
          record_info_for_visible_type(registry_, scope_, type)) {
    auto it = record->fields.find(lower_field_name);
    return it == record->fields.end() ? nullptr : it->second.type.get();
  }
  type = semantic_shape_type(type);
  if (!type) return nullptr;
  if (type->kind == Kind::TyPointer) {
    return lookup_record_field_type_in_type(
        static_cast<const TyPointer&>(*type).target.get(), field_name);
  }
  if (type->kind == Kind::TyName) {
    if (const RecordInfo* record =
            record_info_for_visible_type(registry_, scope_, type)) {
      auto it = record->fields.find(lower_field_name);
      return it == record->fields.end() ? nullptr : it->second.type.get();
    }
  }
  if (type->kind != Kind::TyRecord) return nullptr;

  const auto& rec = static_cast<const TyRecord&>(*type);

  for (const auto& rf : rec.fields) {
    if (const TypeExpr* ft = record_field_type(rf, lower_field_name)) {
      return ft;
    }
  }

  return variant_part_field_type(rec.variant_part, lower_field_name);
}

bool EmitAnalysis::record_field_is_variant_in_type(
    const TypeExpr* type, std::string_view field_name) {
  if (!type) return false;
  const std::string lower_field_name = ascii_lower(std::string(field_name));
  if (const RecordInfo* record =
          record_info_for_visible_type(registry_, scope_, type)) {
    auto it = record->fields.find(lower_field_name);
    return it != record->fields.end() && it->second.is_variant;
  }
  type = semantic_shape_type(type);
  if (!type) return false;
  if (type->kind == Kind::TyPointer) {
    return record_field_is_variant_in_type(
        static_cast<const TyPointer&>(*type).target.get(), field_name);
  }
  if (type->kind == Kind::TyName) {
    if (const RecordInfo* record =
            record_info_for_visible_type(registry_, scope_, type)) {
      auto it = record->fields.find(lower_field_name);
      return it != record->fields.end() && it->second.is_variant;
    }
  }
  if (type->kind != Kind::TyRecord) return false;

  const auto& rec = static_cast<const TyRecord&>(*type);
  return variant_part_has_payload_field(rec.variant_part, lower_field_name);
}

const TypeExpr* EmitAnalysis::lookup_record_field_type_in_with(
    const ScopeStateView::WithBind& wb, std::string_view field_name) {
  if (const TypeExpr* ft = lookup_record_field_type_in_type(
          wb.type, field_name)) {
    return ft;
  }
  if (wb.class_name.empty()) return nullptr;
  if (auto* rf = registry_.lookup_record_field(
          wb.class_name, std::string(field_name), scope_.current_unit_name)) {
    return rf->type.get();
  }
  return nullptr;
}

bool EmitAnalysis::with_bind_has_visible_member(
    const ScopeStateView::WithBind& wb, std::string_view name) {
  if (!wb.class_name.empty()) {
    if (registry_.lookup_class_methods(wb.class_name, std::string(name),
                                        scope_.current_unit_name) ||
        registry_.lookup_class_field(wb.class_name, std::string(name),
                                      scope_.current_unit_name) ||
        registry_.lookup_class_property(wb.class_name, std::string(name),
                                         scope_.current_unit_name)) {
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
  if (!scope_.current_class_name.empty() &&
      (registry_.lookup_class_methods(scope_.current_class_name, name,
                                       scope_.current_unit_name) ||
       registry_.lookup_class_field(scope_.current_class_name, name,
                                     scope_.current_unit_name) ||
       registry_.lookup_class_property(scope_.current_class_name, name,
                                        scope_.current_unit_name))) {
    return true;
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
    const bool system_unit_qualifier =
        resolve_unit_qualified_member(mem).has_value();
    if (system_unit_qualifier) return mem.name;
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
  if (const TyEnum* type =
          registry_.lookup_enum_member_in_unit(unit_name, member_name)) {
    return registry_.enum_info_for_type(type);
  }
  return nullptr;
}

const EnumInfoReg* EmitAnalysis::find_visible_enum_info_for_member(
    const std::string& name) {
  if (const EnumInfoReg* local =
          local_enum_info_for_member(scope_, name)) {
    return local;
  }

  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    if (!scope_frame_has_enum_member(*frame, name)) continue;
    if (const auto* info = find_enum_info_in_unit(frame->unit, name)) {
      return info;
    }
  }
  return nullptr;
}

std::string EmitAnalysis::implicit_self_cxx() {
  if (!scope_.current_class_name.empty()) {
    if (const auto* ci = migration_fallback_class_info_by_name(scope_.current_class_name)) {
      // Old object values use `(*this)` while reference classes use `this`.
      // Property lowering asks this helper instead of re-deriving that split.
      return ci->is_reference_type ? "this" : "(*this)";
    }
  }
  return "(*this)";
}

std::string EmitAnalysis::implicit_self_access() {
  if (!scope_.current_class_name.empty()) {
    if (const auto* ci =
            migration_fallback_class_info_by_name(scope_.current_class_name)) {
      return ci->is_reference_type ? "->" : ".";
    }
  }
  return ".";
}

std::optional<ImplicitPropertyLookup> EmitAnalysis::find_implicit_class_property(
    std::string_view name) {
  if (scope_.local_scope.count(std::string(name))) return std::nullopt;

  // `with` bindings shadow the ambient class scope for property lookup just as
  // they do for fields/methods, so search them from innermost to outermost.
  for (auto it = scope_.with_stack.rbegin(); it != scope_.with_stack.rend(); ++it) {
    const std::string& cls = it->class_name;
    if (cls.empty()) continue;
    if (auto* prop = registry_.lookup_class_property(
            cls, std::string(name), scope_.current_unit_name)) {
      return ImplicitPropertyLookup{prop, cls, it->cxx_text, it->access_op,
                                    true};
    }
  }

  if (scope_.current_class_name.empty()) return std::nullopt;
  // Fall back to the current class only after locals and `with` scopes have
  // been ruled out; otherwise a same-named local would spuriously become a
  // property access.
  if (auto* prop = registry_.lookup_class_property(
          scope_.current_class_name, std::string(name),
          scope_.current_unit_name)) {
    return ImplicitPropertyLookup{prop, scope_.current_class_name,
                                  implicit_self_cxx(), implicit_self_access(),
                                  false};
  }
  return std::nullopt;
}

}  // namespace tp2cc
