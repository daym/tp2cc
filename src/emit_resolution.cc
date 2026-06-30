#include "emit_resolution.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include "emit_signature_scope.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

std::string callable_member_name(const Expr& callee) {
  if (callee.kind == Kind::Ident) {
    return static_cast<const Ident&>(callee).name;
  }
  if (callee.kind == Kind::Member) {
    return static_cast<const Member&>(callee).name;
  }
  return {};
}

ResolvedCall unresolved_call(std::string member_name = {}) {
  return ResolvedCall{.decl = nullptr,
                      .callee_kind = ResolvedCalleeKind::Default,
                      .defining_unit = {},
                      .member_name = std::move(member_name),
                      .default_arg_unit = {},
                      .signature_declaring_type = {},
                      .return_type_name = {},
                      .needs_arg_casts = false,
                      .ambiguous = false};
}

std::vector<const Param*> flatten_proc_param_slots(
    const std::vector<Param>& params) {
  std::vector<const Param*> out;
  for (const Param& param : params) {
    size_t count = param.names.empty() ? 1 : param.names.size();
    for (size_t i = 0; i < count; ++i) out.push_back(&param);
  }
  return out;
}

std::vector<Param> single_slot_param_vector(const Param& param) {
  Param copy = param;
  copy.names = {"tp2cc_slot"};
  return {std::move(copy)};
}

bool proc_set_allows_outer_lookup(const std::vector<ProcInfo>& procs) {
  // Keep walking the unit stack when the procsym found in the current
  // frame contains at least one `overload` procdef.
  return std::any_of(procs.begin(), procs.end(),
                     [](const ProcInfo& proc) { return proc.is_overload; });
}

}  // namespace

EmitResolution::EmitResolution(const TypeRegistry& registry,
                               ScopeStateView& scope, EmitAnalysis& analysis,
                               ResolutionTypeOps& type_ops,
                               OverloadTypeProvider& overload_types,
                               TargetInfo target)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      type_ops_(type_ops),
      overload_types_(overload_types),
      target_(target) {}

std::vector<EmitResolution::AnyCand> EmitResolution::class_method_cands(
    const std::string& cls, const std::string& name) {
  std::vector<AnyCand> candidates;
  if (cls.empty()) return candidates;
  auto* set = registry_.lookup_class_methods(
      cls, name, scope_.current_unit_name);
  if (!set) return candidates;
  for (const auto& ms : *set) {
    if (!ms.decl) continue;
    candidates.push_back({ms.decl.get(), ms.param_count, ms.accepts_zero_args,
                          {}, ms.defining_unit, ms.declaring_type,
                          ms.return_type_name});
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand> EmitResolution::metaclass_method_cands(
    const std::string& cls, const std::string& name) {
  std::vector<AnyCand> candidates;
  if (cls.empty()) return candidates;
  auto* set = registry_.lookup_class_methods(
      cls, name, scope_.current_unit_name);
  if (!set) return candidates;
  for (const auto& ms : *set) {
    if (!ms.decl) continue;
    if (ms.kind != SymKind::Constructor && ms.kind != SymKind::ClassMethod) {
      continue;
    }
    candidates.push_back({ms.decl.get(), ms.param_count, ms.accepts_zero_args,
                          {}, ms.defining_unit, ms.declaring_type,
                          ms.return_type_name});
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand> EmitResolution::unit_export_proc_cands(
    const std::string& unit, const std::string& name) {
  std::vector<AnyCand> candidates;
  auto it = registry_.units.find(unit);
  if (it == registry_.units.end()) return candidates;
  auto* v = (unit == scope_.current_unit_name)
                ? it->second.find_procs(name)
                : it->second.find_export_procs(name);
  if (!v) return candidates;
  for (const auto& pi : *v) {
    candidates.push_back({pi.decl.get(), pi.param_count, pi.accepts_zero_args,
                          unit, pi.defining_unit, {},
                          pi.return_type_name});
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand>
EmitResolution::gather_callable_in_pascal_scope(
    const std::string& name) {
  std::vector<AnyCand> candidates;

  for (auto wit = scope_.with_stack.rbegin(); wit != scope_.with_stack.rend();
       ++wit) {
    std::vector<AnyCand> with_candidates =
        class_method_cands(wit->class_name, name);
    if (!with_candidates.empty()) return with_candidates;
  }
  if (auto nit = scope_.local_nested_fns.find(name);
      nit != scope_.local_nested_fns.end()) {
    for (const auto& nested : nit->second) {
      if (!nested.decl) continue;
      candidates.push_back(
          {nested.decl, nested.param_count, nested.accepts_zero_args, {},
           scope_.current_unit_name, {}, {}});
    }
    return candidates;
  }
  std::vector<AnyCand> class_candidates =
      class_method_cands(scope_.current_class_name, name);
  if (!class_candidates.empty()) return class_candidates;
  // A decl in the current unit shadows same-named decls reached through
  // `uses`. Without this stop, local overload sets and imported overload sets
  // would get merged even though Pascal lexical lookup never reaches the
  // imports once the current unit contributes the name.
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    const std::vector<ProcInfo>* procs = scope_frame_find_procs(*frame, name);
    if (!procs || procs->empty()) continue;
    std::vector<AnyCand> frame_candidates;
    for (const auto& pi : *procs) {
      frame_candidates.push_back(
          {pi.decl.get(), pi.param_count, pi.accepts_zero_args,
           frame->unit, pi.defining_unit, {}, pi.return_type_name});
    }
    if (!scope_frame_is_import(*frame)) {
      return frame_candidates;
    }
    candidates.insert(candidates.end(),
                      std::make_move_iterator(frame_candidates.begin()),
                      std::make_move_iterator(frame_candidates.end()));
    if (!proc_set_allows_outer_lookup(*procs)) break;
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand>
EmitResolution::gather_operator_in_pascal_scope(
    const std::string& op) {
  std::vector<AnyCand> candidates;
  for (const TypeLookupContext* frame = scope_.type_scope; frame;
       frame = frame->parent) {
    const auto* ops = scope_frame_find_operators(*frame, op);
    if (!ops) continue;
    std::vector<AnyCand> frame_candidates;
    for (const auto& pi : *ops) {
      frame_candidates.push_back({pi.decl.get(), pi.param_count,
                                  pi.accepts_zero_args, frame->unit,
                                  pi.defining_unit, {}, {}});
    }
    if (!scope_frame_is_import(*frame)) {
      return frame_candidates;
    }
    candidates.insert(candidates.end(),
                      std::make_move_iterator(frame_candidates.begin()),
                      std::make_move_iterator(frame_candidates.end()));
    if (!proc_set_allows_outer_lookup(*ops)) break;
  }
  return candidates;
}

std::vector<FlatCallParamInfo> EmitResolution::flatten_call_param_info(
    const ProcDecl* decl, std::string_view param_unit,
    std::string_view param_declaring_type) {
  std::vector<FlatCallParamInfo> flat_params;
  if (!decl) return flat_params;
  const TypeLookupContext* signature_context =
      registry_.lookup_proc_signature_context(decl);
  for (const auto& p : decl->params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) {
      const TypeLookupContext* type_context =
          registry_.lookup_context_for_type(p.type.get());
      if (!type_context) type_context = signature_context;
      flat_params.emplace_back(
          p.type.get(), !p.type,
          p.mode == Param::Var || p.mode == Param::Out ||
              (p.mode == Param::Const &&
               analysis_.const_param_needs_mutable_ref(p.type.get())),
          p.default_value.get(), std::string(param_unit),
          std::string(param_declaring_type),
          std::shared_ptr<const TypeExpr>{}, type_context);
    }
  }
  return flat_params;
}

std::string EmitResolution::type_cxx_or_empty(const TypeExpr* t) {
  return t ? type_ops_.type_to_cxx(*t) : std::string{};
}

const TypeExpr* EmitResolution::strip_conversion_wrapper(const TypeExpr* t) {
  while (t && (t->kind == Kind::TyDistinct || t->kind == Kind::TySubrange)) {
    if (t->kind == Kind::TyDistinct) {
      t = analysis_.semantic_shape_type(
          static_cast<const TyDistinct&>(*t).underlying.get());
    } else {
      // Resolve subrange to its host integer primitive using the same
      // domain-to-primitive mapping as canonicalize_for_arithmetic so
      // type deduction and overload ranking stay consistent.
      return analysis_.canonicalize_for_arithmetic(t);
    }
  }
  return t;
}

ConvScore EmitResolution::class_hierarchy_conversion_score(
    const TypeExpr* arg, const TypeLookupContext* arg_context,
    const TypeExpr* param, const TypeLookupContext* param_context) {
  const auto* arg_class =
      analysis_.class_info_for_type_in_context(arg, arg_context);
  const auto* param_class =
      analysis_.class_info_for_type_in_context(param, param_context);
  if (!arg_class || !param_class) return {};

  std::unordered_set<std::string> seen;
  const ClassInfo* cur = arg_class;
  int depth = 0;
  while (cur) {
    const std::string identity = cur->defining_unit + "." + cur->name;
    if (seen.count(identity)) break;
    if (cur->name == param_class->name &&
        cur->defining_unit == param_class->defining_unit) {
      return {ConvRank::ClassHierarchy, depth};
    }
    seen.insert(identity);
    cur = registry_.lookup_parent_class(*cur);
    ++depth;
  }
  return {};
}

ConvScore EmitResolution::object_pointer_hierarchy_conversion_score(
    const TypeExpr* arg, const TypeExpr* param) {
  arg = analysis_.semantic_shape_type(arg);
  param = analysis_.semantic_shape_type(param);
  if (!arg || !param || arg->kind != Kind::TyPointer ||
      param->kind != Kind::TyPointer) {
    return {};
  }
  const auto* arg_class = analysis_.class_info_for_type(
      static_cast<const TyPointer&>(*arg).target.get());
  const auto* param_class = analysis_.class_info_for_type(
      static_cast<const TyPointer&>(*param).target.get());
  if (!arg_class || !param_class) return {};
  if (arg_class->is_reference_type || param_class->is_reference_type) {
    return {};
  }

  std::unordered_set<std::string> seen;
  const ClassInfo* cur = arg_class;
  int depth = 0;
  while (cur) {
    const std::string identity = cur->defining_unit + "." + cur->name;
    if (seen.count(identity)) break;
    if (cur->name == param_class->name &&
        cur->defining_unit == param_class->defining_unit) {
      return {ConvRank::ClassHierarchy, depth};
    }
    seen.insert(identity);
    cur = registry_.lookup_parent_class(*cur);
    ++depth;
  }
  return {};
}

const PrimitiveInfo* EmitResolution::primitive_for_type(const TypeExpr* t) {
  return analysis_.primitive_info_for_type(t);
}

std::optional<EmitResolution::IntegerActualDomain>
EmitResolution::integer_actual_domain_for_type(const TypeExpr* t) {
  t = analysis_.semantic_shape_type(t);
  if (!t) return std::nullopt;
  if (t->kind == Kind::TyDistinct) {
    return integer_actual_domain_for_type(
        static_cast<const TyDistinct&>(*t).underlying.get());
  }
  if (t->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*t);
    if (!sr.lo || !sr.hi) return std::nullopt;
    auto lo = analysis_.eval_const_int_expr(*sr.lo);
    auto hi = analysis_.eval_const_int_expr(*sr.hi);
    if (!lo || !hi) return std::nullopt;
    int64_t low = std::min(lo->value, hi->value);
    int64_t high = std::max(lo->value, hi->value);
    return IntegerActualDomain{
        .low = low,
        .high = high < 0 ? 0 : static_cast<uint64_t>(high),
        .preferred_kind = low < 0 ? PrimitiveIntKind::Signed
                                  : PrimitiveIntKind::Unsigned};
  }
  const PrimitiveInfo* pi = primitive_for_type(t);
  if (!pi || pi->int_kind == PrimitiveIntKind::None) return std::nullopt;
  const uint8_t width = primitive_bits(*pi, target_);
  if (pi->int_kind == PrimitiveIntKind::Unsigned) {
    return IntegerActualDomain{
        .low = 0,
        .high = width >= 64
                    ? std::numeric_limits<uint64_t>::max()
                    : unsigned_mask_for_bits(width),
        .preferred_kind = PrimitiveIntKind::Unsigned};
  }
  return IntegerActualDomain{
      .low = signed_min_for_bits(width),
      .high = width >= 64
                  ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                  : static_cast<uint64_t>(signed_max_for_bits(width)),
      .preferred_kind = PrimitiveIntKind::Signed};
}

bool EmitResolution::is_untyped_integer_constant_expr(const Expr& arg) {
  switch (arg.kind) {
    case Kind::IntLit:
      return true;
    case Kind::Unary: {
      const auto& u = static_cast<const Unary&>(arg);
      return u.operand && is_untyped_integer_constant_expr(*u.operand);
    }
    case Kind::Binary: {
      const auto& b = static_cast<const Binary&>(arg);
      return b.lhs && b.rhs && is_untyped_integer_constant_expr(*b.lhs) &&
             is_untyped_integer_constant_expr(*b.rhs);
    }
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(arg);
      auto local = scope_.local_consts.find(id.name);
      if (local != scope_.local_consts.end()) {
        return local->second && !local->second->type && local->second->value;
      }
      if (const ConstInfo* c = analysis_.find_visible_unit_const(id.name)) {
        return !c->type && c->value;
      }
      return false;
    }
    default:
      return false;
  }
}

std::optional<EmitResolution::IntegerActualDomain>
EmitResolution::integer_actual_domain_for_expr(const Expr& arg) {
  if (is_untyped_integer_constant_expr(arg)) {
    auto c = analysis_.eval_const_int_expr(arg);
    if (!c) return std::nullopt;
    if (c->type && c->type->int_kind == PrimitiveIntKind::Unsigned) {
      if (c->bits <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        // FPC overload resolution treats ordinary untyped integer constants as
        // Integer/LongInt candidates, not as the smallest storage type that
        // would hold their current value.
        return integer_actual_domain_for_type(builtin_integer_type("longint"));
      }
      return IntegerActualDomain{.low = 0,
                                 .high = c->bits,
                                 .preferred_kind =
                                     PrimitiveIntKind::Unsigned};
    }
    if (c->value >= std::numeric_limits<int32_t>::min() &&
        c->value <= std::numeric_limits<int32_t>::max()) {
      // FPC overload resolution treats ordinary untyped integer constants as
      // Integer/LongInt candidates, not as the smallest storage type that would
      // hold their current value. That is why `pair(cardinal_value, 1)` chooses
      // the Int64 overload: the second argument still carries a signed LongInt
      // domain, so QWord is not a common formal for the whole call.
      return integer_actual_domain_for_type(builtin_integer_type("longint"));
    }
    return IntegerActualDomain{
        .low = c->value < 0 ? c->value : 0,
        .high = c->value < 0 ? 0 : static_cast<uint64_t>(c->value),
        .preferred_kind = c->value < 0 ? PrimitiveIntKind::Signed
                                       : PrimitiveIntKind::Unsigned};
  }
  return integer_actual_domain_for_type(overload_types_.type_for_overload(arg));
}

bool EmitResolution::integer_domain_fits_primitive(
    const IntegerActualDomain& domain, const PrimitiveInfo& formal) const {
  if (formal.int_kind == PrimitiveIntKind::None) return false;
  const uint8_t width = primitive_bits(formal, target_);
  if (formal.int_kind == PrimitiveIntKind::Unsigned) {
    if (domain.low < 0) return false;
    const uint64_t max = unsigned_mask_for_bits(width);
    return domain.high <= max;
  }
  const int64_t min = signed_min_for_bits(width);
  const uint64_t max =
      width >= 64 ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                  : static_cast<uint64_t>(signed_max_for_bits(width));
  return domain.low >= min && domain.high <= max;
}

bool EmitResolution::set_literal_can_construct_open_array(
    const SetLit& literal, const TypeExpr* param,
    const TypeLookupContext* param_context) const {
  const TypeExpr* p =
      analysis_.semantic_shape_type_in_context(param, param_context);
  if (!p || p->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*p).array_kind != ArrayKind::Open) {
    return false;
  }
  for (const auto& element : literal.elements) {
    if (element && element->kind == Kind::Range) return false;
  }
  return true;
}

bool EmitResolution::target_pointer_arithmetic_can_convert(
    const Expr& arg, const TypeExpr* param,
    const TypeLookupContext* param_context,
    bool allow_assignment_operator_conversions) {
  const TypeExpr* canon_param =
      analysis_.semantic_shape_type_in_context(param, param_context);
  if (!canon_param || !type_ops_.type_is_pointerish(canon_param) ||
      arg.kind != Kind::Binary) {
    return false;
  }

  const auto& b = static_cast<const Binary&>(arg);
  if (b.op != BinOp::Add && b.op != BinOp::Sub) return false;
  if (!b.lhs || !b.rhs) return false;

  const TypeExpr* lhs_type =
      analysis_.semantic_shape_type(argument_source_type_for_conversion(*b.lhs));
  const TypeExpr* rhs_type =
      analysis_.semantic_shape_type(argument_source_type_for_conversion(*b.rhs));
  auto integer_type = [this](const TypeExpr* t) {
    const PrimitiveInfo* pi = primitive_for_type(t);
    return pi && pi->int_kind != PrimitiveIntKind::None;
  };

  FlatCallParamInfo pointer_operand(param, /*untyped_in=*/false,
                                    /*mutable_ref_in=*/false,
                                    /*default_value_in=*/nullptr,
                                    /*param_unit_in=*/{},
                                    /*param_declaring_type_in=*/{},
                                    /*owned_type_in=*/{},
                                    param_context);
  if (type_ops_.type_is_pointerish(lhs_type) && integer_type(rhs_type)) {
    return score_argument_conversion(*b.lhs, pointer_operand,
                                     allow_assignment_operator_conversions)
        .viable();
  }
  if (b.op == BinOp::Add && integer_type(lhs_type) &&
      type_ops_.type_is_pointerish(rhs_type)) {
    return score_argument_conversion(*b.rhs, pointer_operand,
                                     allow_assignment_operator_conversions)
        .viable();
  }
  return false;
}

const TypeExpr* EmitResolution::argument_source_type_for_conversion(
    const Expr& arg) {
  if (const TypeExpr* cast_type =
          analysis_.explicit_typecast_result_type(arg)) {
    return cast_type;
  }
  if (const TypeExpr* overload_type = overload_types_.type_for_overload(arg)) {
    return overload_type;
  }
  return analysis_.deduce_type(arg);
}

ConvScore EmitResolution::rank_integer_domain_conversion(
    const IntegerActualDomain& domain, const TypeExpr* param, bool var_param,
    const TypeLookupContext* param_context) {
  if (var_param) return {};
  const TypeExpr* p =
      analysis_.semantic_shape_type_in_context(param, param_context);
  const PrimitiveInfo* formal = primitive_for_type(p);
  if (!formal || formal->int_kind == PrimitiveIntKind::None) return {};
  if (!integer_domain_fits_primitive(domain, *formal)) return {};
  const uint8_t width = primitive_bits(*formal, target_);
  // FPC chooses the smallest integer formal that can represent the Pascal
  // source domain, then uses signedness only to break equal-width ties. This is
  // why `byte` binds to `longint` rather than `qword` when no `cardinal`
  // overload exists, while `cardinal` still binds to `qword` over `int64`.
  const int sign_mismatch =
      domain.preferred_kind == formal->int_kind ? 0 : 1;
  return {ConvRank::IntDomainCompatible,
          static_cast<int>(width) * 2 + sign_mismatch};
}

std::optional<PickResult> EmitResolution::pick_integer_domain_overload(
    const std::vector<ScoredCandidate>& viable,
    const std::vector<const Expr*>& args) {
  if (args.empty()) return std::nullopt;

  std::vector<std::optional<IntegerActualDomain>> domains;
  domains.reserve(args.size());
  bool saw_integer_domain = false;
  for (const Expr* arg : args) {
    if (!arg) return std::nullopt;
    auto domain = integer_actual_domain_for_expr(*arg);
    if (domain) saw_integer_domain = true;
    domains.push_back(std::move(domain));
  }
  if (!saw_integer_domain) return std::nullopt;

  std::vector<ScoredCandidate> domain_viable;
  for (const ScoredCandidate& candidate : viable) {
    if (!candidate.decl) continue;
    std::vector<FlatCallParamInfo> flat =
        flatten_call_param_info(candidate.decl, candidate.declaration_unit,
                                candidate.declaring_type);
    if (args.size() > flat.size()) continue;

    ScoredCandidate scored{candidate.decl, candidate.declaration_unit,
                           candidate.declaring_type, candidate.scores};
    bool fits_all = true;
    for (size_t i = 0; i < args.size(); ++i) {
      if (!domains[i]) continue;
      if (scored.scores[i].rank == ConvRank::Exact ||
          scored.scores[i].rank == ConvRank::Equal) {
        continue;
      }
      if (flat[i].mutable_ref) {
        fits_all = false;
        break;
      }
      const TypeExpr* formal_type = analysis_.semantic_shape_type(flat[i].type);
      const PrimitiveInfo* formal = primitive_for_type(formal_type);
      if (!formal || formal->int_kind == PrimitiveIntKind::None) {
        // Integer actuals may also fit non-primitive overloads through
        // assignment operators. FPC still ranks the primitive integer candidates
        // as their own set first; operator candidates remain available only if
        // no primitive integer candidate wins.
        fits_all = false;
        break;
      }
      ConvScore score =
          rank_integer_domain_conversion(*domains[i], flat[i].type, false,
                                         flat[i].type_context);
      if (!score.viable()) {
        fits_all = false;
        break;
      }
      scored.scores[i] = score;
    }
    if (fits_all) domain_viable.push_back(std::move(scored));
  }

  if (domain_viable.empty()) return std::nullopt;
  if (domain_viable.size() == 1) return PickResult{domain_viable[0].decl, false};

  size_t best = 0;
  for (size_t i = 1; i < domain_viable.size(); ++i) {
    if (conversion_candidate_dominates(domain_viable[i], domain_viable[best])) {
      best = i;
    }
  }
  for (size_t i = 0; i < domain_viable.size(); ++i) {
    if (i == best) continue;
    if (!conversion_candidate_dominates(domain_viable[best], domain_viable[i])) {
      return PickResult{nullptr, true};
    }
  }
  return PickResult{domain_viable[best].decl, false};
}

bool EmitResolution::type_is_shortstring_family(const TypeExpr* t) {
  if (!t) return false;
  const TypeExpr* shape = analysis_.semantic_shape_type(t);
  if (shape && shape->kind == Kind::TyString) return true;
  return analysis_.builtin_atom_name_for_type(t) == "shortstring";
}

bool EmitResolution::type_is_longstring_family(const TypeExpr* t) const {
  return analysis_.type_is_long_string(t);
}

bool EmitResolution::type_is_char_type(const TypeExpr* t) {
  const PrimitiveInfo* info = primitive_for_type(t);
  return info && info->is_char();
}

ConvScore EmitResolution::rank_conversion(const TypeExpr* arg,
                                          const TypeExpr* param,
                                          bool var_param,
                                          const TypeLookupContext* param_context) {
  if (!arg || !param) return {};
  const TypeExpr* raw_arg = arg;
  const TypeExpr* raw_param = param;
  const TypeDescriptor* arg_descriptor = registry_.descriptor_for_type(raw_arg);
  if (!arg_descriptor) {
    if (const TypeSymbol* symbol = registry_.resolved_symbol_for_type(raw_arg)) {
      arg_descriptor = symbol->descriptor;
    }
  }
  if (!arg_descriptor) {
    if (const TypeSymbol* symbol =
            resolved_type_symbol_in_context(registry_, scope_, raw_arg)) {
      arg_descriptor = symbol->descriptor;
    }
  }
  const TypeDescriptor* param_descriptor =
      registry_.descriptor_for_type(raw_param);
  if (!param_descriptor) {
    if (const TypeSymbol* symbol =
            registry_.resolved_symbol_for_type(raw_param)) {
      param_descriptor = symbol->descriptor;
    }
  }
  if (!param_descriptor) {
    if (const TypeSymbol* symbol = resolved_type_symbol_in_context(
            registry_, scope_, raw_param, param_context)) {
      param_descriptor = symbol->descriptor;
    }
  }
  if (arg_descriptor && param_descriptor) {
    if (arg_descriptor == param_descriptor) return {ConvRank::Exact, 0};
  }
  const TypeExpr* a = analysis_.semantic_shape_type(arg);
  const TypeExpr* p =
      analysis_.semantic_shape_type_in_context(param, param_context);
  if (!a || !p) return {};

  if (a == p || (!param_context && analysis_.same_type_ast(raw_arg, raw_param))) {
    return {ConvRank::Exact, 0};
  }

  if (a->kind == Kind::TySet || p->kind == Kind::TySet) {
    switch (analysis_.classify_set_conversion(a, p)) {
      case SetConversionKind::Exact:
        return {ConvRank::Exact, 0};
      case SetConversionKind::Compatible:
        return var_param ? ConvScore{} : ConvScore{ConvRank::SetCompatible, 0};
      case SetConversionKind::Incompatible:
        return {};
    }
  }

  std::string a_cxx = type_cxx_or_empty(a);
  std::string p_cxx = type_cxx_or_empty(p);
  // 1. Exact identity after canonicalization. Do not use emitted C++ carrier
  // equality for pointer-like Pascal values: `Pointer`, typed pointers,
  // reference-class values, and callback carriers may share a representation
  // while still having distinct Pascal conversion ranks.
  const bool pointer_like_pair =
      type_ops_.type_is_pointerish(a) || type_ops_.type_is_pointerish(p);
  if (!pointer_like_pair && !a_cxx.empty() && a_cxx == p_cxx) {
    return {ConvRank::Exact, 0};
  }

  const TypeExpr* a_under = strip_conversion_wrapper(a);
  const TypeExpr* p_under = strip_conversion_wrapper(p);
  // 2. Equal modulo distinct/subrange wrappers. Distinct types still lower to
  // the same underlying storage for overload ranking, and subranges adopt
  // their base integer type here.
  if (!pointer_like_pair && a_under && p_under &&
      type_cxx_or_empty(a_under) == type_cxx_or_empty(p_under)) {
    return {ConvRank::Equal, 0};
  }

  if (var_param) {
    // `var`/`out` params only accept identity/equal-or-wrapper matches, plus
    // class-hierarchy aliasing for reference types. Anything else would pass a
    // temporary or layout-incompatible slot by reference.
    if (ConvScore score = class_hierarchy_conversion_score(
            raw_arg, nullptr, raw_param, param_context);
        score.viable()) {
      return score;
    }
    if (ConvScore score =
            class_hierarchy_conversion_score(a, nullptr, p, param_context);
        score.viable()) {
      return score;
    }
    return {};
  }

  // Pointer aliases to TP-style `object` values participate in the same
  // source-language parent chain as the object values themselves. This permits
  // passing `^ChildObject` to a value formal of type `^BaseObject`, while
  // keeping unrelated typed pointers distinct.
  if (ConvScore score = object_pointer_hierarchy_conversion_score(raw_arg,
                                                                  raw_param);
      score.viable()) {
    return score;
  }
  if (ConvScore score = object_pointer_hierarchy_conversion_score(a, p);
      score.viable()) {
    return score;
  }

  // 3. Class hierarchy: a derived class may pass where an ancestor is expected.
  // Fewer parent hops means the closer Pascal match.
  if (ConvScore score = class_hierarchy_conversion_score(
          raw_arg, nullptr, raw_param, param_context);
      score.viable()) {
    return score;
  }
  if (ConvScore score =
          class_hierarchy_conversion_score(a, nullptr, p, param_context);
      score.viable()) {
    return score;
  }

  // 4. Integer widening with unchanged signedness. Prefer the smallest target
  // that still contains the source by using the bit-width gap as distance.
  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = primitive_for_type(p);
        pi && pi->int_kind == ai->int_kind) {
      const uint8_t aw = primitive_bits(*ai, target_);
      const uint8_t pw = primitive_bits(*pi, target_);
      if (pw >= aw) {
      return {ConvRank::IntWideningSameSign,
                static_cast<int>(pw) - static_cast<int>(aw)};
      }
    }
  }

  // 5. Real widening follows Pascal's precision ladder. Again, smaller rank
  // gaps are better fits.
  if (const PrimitiveInfo* ai = primitive_for_type(a)) {
    const PrimitiveInfo* pi = primitive_for_type(p);
    int ar = ai->float_rank();
    int pr = pi ? pi->float_rank() : 0;
    if (ar > 0 && pr > 0 && pr >= ar) return {ConvRank::RealWidening, pr - ar};
    if (ar > 0 && pr > 0) return {ConvRank::RealNarrowing, ar - pr};
  }

  // 6. Same-family ShortString widening.
  if (type_is_shortstring_family(a) && type_is_shortstring_family(p)) {
    return {ConvRank::StringSameTagWiden, 0};
  }

  const bool param_is_shortstring = type_is_shortstring_family(p);
  const bool param_is_ansistring = type_is_longstring_family(p);
  const bool arg_is_shortstring = type_is_shortstring_family(a);
  const bool arg_is_ansistring = type_is_longstring_family(a);
  // 7-8. Cross-family string conversions stay split because under `{$H-}`
  // Pascal prefers ShortString-targeted overloads over AnsiString-targeted
  // ones when both otherwise accept the same source.
  if (arg_is_ansistring && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (type_is_char_type(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (type_ops_.type_is_pcharish(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (arg_is_shortstring && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (type_is_char_type(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (type_ops_.type_is_pcharish(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if ((arg_is_shortstring || arg_is_ansistring) && type_ops_.type_is_pcharish(p)) {
    return {ConvRank::StringToAnsiString, 0};
  }

  // 9. Signedness change with sufficient width.
  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = primitive_for_type(p);
        pi && pi->int_kind != PrimitiveIntKind::None) {
      const uint8_t aw = primitive_bits(*ai, target_);
      const uint8_t pw = primitive_bits(*pi, target_);
      if (pw >= aw) {
      return {ConvRank::OrdinalSignChange,
                static_cast<int>(pw) - static_cast<int>(aw)};
      }
    }
  }

  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    // 10. Integer narrowing. Pascal still permits this for value/const params
    // with range checking; the picker must rank it as viable but worse than
    // widening or sign-preserving matches.
    if (const auto* pi = primitive_for_type(p);
        pi && pi->int_kind != PrimitiveIntKind::None) {
      const uint8_t aw = primitive_bits(*ai, target_);
      const uint8_t pw = primitive_bits(*pi, target_);
      if (pw < aw) {
      return {ConvRank::IntNarrowing,
                static_cast<int>(aw) - static_cast<int>(pw)};
      }
    }
  }

  // 11. Integer value expressions can feed real value formals. Keep this worse
  // than every integer-formal conversion so integer overloads still win for
  // integer actuals; within real formals, prefer the lower-precision target.
  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    const PrimitiveInfo* pi = primitive_for_type(p);
    int pr = pi ? pi->float_rank() : 0;
    if (pr > 0) return {ConvRank::IntegerToReal, pr};
  }

  return {};
}

ConvScore EmitResolution::score_conversion(
    const TypeExpr* arg, const TypeExpr* param, bool var_param,
    bool allow_assignment_operator_conversions) {
  ConvScore score = rank_conversion(arg, param, var_param);
  if (score.viable()) return score;
  if (!allow_assignment_operator_conversions || var_param) return {};
  if (find_assignment_operator(arg, param).decl) {
    return {ConvRank::Operator, 0};
  }
  return {};
}

bool EmitResolution::procedural_signatures_match(const ProcDecl& decl,
                                                 const TyProcedural& proc) {
  if ((decl.pkind == ProcKind::Function) != proc.is_function) return false;
  if (proc.is_function) {
    if (!proc.return_type || !decl.return_type) return false;
    if (type_ops_.type_to_cxx(*proc.return_type) !=
        type_ops_.type_to_cxx(*decl.return_type)) {
      return false;
    }
  }
  return type_ops_.formal_param_types_to_cxx(decl.params) ==
         type_ops_.formal_param_types_to_cxx(proc.params);
}

bool EmitResolution::procedural_types_match(const TyProcedural& source,
                                            const TyProcedural& target) {
  if (source.is_function != target.is_function ||
      source.is_method != target.is_method || source.is_cdecl != target.is_cdecl) {
    return false;
  }
  if (source.is_function) {
    if (!source.return_type || !target.return_type) return false;
    if (type_ops_.type_to_cxx(*source.return_type) !=
        type_ops_.type_to_cxx(*target.return_type)) {
      return false;
    }
  }
  // Compare the Pascal formal surface, not the procvar call carrier. Procvar
  // calls deliberately normalize pointer-like value parameters to `void*` for
  // defined indirect C++ calls, but Pascal still distinguishes
  // `procedure(TItem; Pointer)` from `procedure(Pointer; Pointer)` unless the
  // source writes an explicit procedural cast.
  return type_ops_.formal_param_types_to_cxx(source.params) ==
         type_ops_.formal_param_types_to_cxx(target.params);
}

std::optional<int> EmitResolution::procedural_value_signature_distance(
    const ProcDecl& decl, const TyProcedural& proc,
    bool allow_pointer_carrier_adapters) {
  // Procedural-value binding needs more information than exact/not-exact when
  // the caller allows explicit pointer-like callback casts. Given overloads
  // accepting `procedure(TItem; Pointer)` and `procedure(Pointer; Pointer)`, a
  // `procedure(TItem; Pointer)` value must prefer the exact overload; the
  // pointer overload is adapter-viable only for an explicit procedural cast.
  // Return the number of adapter-needed parameter slots so exact callbacks rank
  // before adapted callbacks and equally adapted candidates stay ambiguous.
  if ((decl.pkind == ProcKind::Function) != proc.is_function) return std::nullopt;
  if (proc.is_function) {
    if (!proc.return_type || !decl.return_type) return std::nullopt;
    if (type_ops_.type_to_cxx(*proc.return_type) !=
        type_ops_.type_to_cxx(*decl.return_type)) {
      return std::nullopt;
    }
  }
  if (procedural_signatures_match(decl, proc)) return 0;
  if (!allow_pointer_carrier_adapters) return std::nullopt;

  const std::vector<const Param*> source_params =
      flatten_proc_param_slots(decl.params);
  const std::vector<const Param*> target_params =
      flatten_proc_param_slots(proc.params);
  if (source_params.size() != target_params.size()) return std::nullopt;

  int distance = 0;
  for (size_t i = 0; i < source_params.size(); ++i) {
    const Param& source = *source_params[i];
    const Param& target = *target_params[i];
    const std::string source_formal =
        type_ops_.formal_param_types_to_cxx(single_slot_param_vector(source));
    const std::string target_formal =
        type_ops_.formal_param_types_to_cxx(single_slot_param_vector(target));
    if (source_formal == target_formal) continue;

    if (!type_ops_.procedural_param_uses_pointer_carrier(source) ||
        !type_ops_.procedural_param_uses_pointer_carrier(target)) {
      return std::nullopt;
    }
    if (type_ops_.procedural_param_type_to_cxx(source) !=
        type_ops_.procedural_param_type_to_cxx(target)) {
      return std::nullopt;
    }
    ++distance;
  }
  return distance;
}

EmitResolution::InstanceMethodLookup
EmitResolution::pick_instance_method_decl(
    const std::string& cls, const std::string& name,
    const TyProcedural& proc, bool allow_pointer_carrier_adapters) {
  const auto* methods =
      registry_.lookup_class_methods(cls, name, scope_.current_unit_name);
  if (!methods) return InstanceMethodLookup::no_instance_method();
  bool saw_instance = false;
  const ProcDecl* match = nullptr;
  int best_distance = std::numeric_limits<int>::max();
  bool ambiguous = false;
  for (const auto& method : *methods) {
    if (!method.decl || method.decl->is_class_method) continue;
    saw_instance = true;
    auto distance = procedural_value_signature_distance(
        *method.decl, proc, allow_pointer_carrier_adapters);
    if (!distance) continue;
    if (*distance < best_distance) {
      match = method.decl.get();
      best_distance = *distance;
      ambiguous = false;
    } else if (*distance == best_distance) {
      ambiguous = true;
    }
  }
  if (match && !ambiguous) {
    return InstanceMethodLookup::match(match, best_distance);
  }
  if (saw_instance) return InstanceMethodLookup::signature_mismatch();
  return InstanceMethodLookup::no_instance_method();
}

std::optional<MethodValueBinding> EmitResolution::resolve_method_value_binding(
    const Expr& arg, const TyProcedural& proc,
    bool allow_pointer_carrier_adapters) {
  if (!proc.is_method) return std::nullopt;

  // Strip an optional address-of (`@method`); both bare ident-form (`method`)
  // and addressed form lower the same way for procedure-of-object targets.
  const Expr* value = &arg;
  if (arg.kind == Kind::AddrOf) {
    const auto& addr = static_cast<const AddrOf&>(arg);
    if (addr.double_addr || !addr.operand) return std::nullopt;
    value = addr.operand.get();
  }

  if (value->kind == Kind::Ident) {
    if (scope_.current_class_name.empty()) return std::nullopt;
    InstanceMethodLookup lookup = pick_instance_method_decl(
        scope_.current_class_name, static_cast<const Ident&>(*value).name,
        proc, allow_pointer_carrier_adapters);
    if (lookup.kind == InstanceMethodLookup::Kind::NoInstanceMethod) {
      return std::nullopt;
    }
    if (lookup.kind == InstanceMethodLookup::Kind::SignatureMismatch) {
      return MethodValueBinding::signature_mismatch(scope_.current_class_name,
                                                    nullptr);
    }
    return MethodValueBinding::via_self(lookup.decl, scope_.current_class_name,
                                        lookup.distance);
  }

  if (value->kind != Kind::Member) return std::nullopt;
  const auto& member = static_cast<const Member&>(*value);
  if (!member.base) return std::nullopt;

  // `Klass.method` (a metaclass-qualified reference) doesn't bind a Self -
  // it would name a class function, which is not a valid value for a
  // procedure-of-object target.
  if (!analysis_.metaclass_target_name(
           method_value_member_base_type(*member.base))
           .empty()) {
    return std::nullopt;
  }

  std::string cls;
  if (member.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*member.base);
    if (id.name == "self") {
      cls = scope_.current_class_name;
    } else if (!analysis_.identifier_is_shadowed_value(id.name) &&
               [&] {
                 const TypeSymbol* symbol =
                     migration_fallback_type_symbol_by_name(registry_, scope_,
                                                    id.name);
                 symbol = descriptor_payload_symbol(symbol);
                 return symbol &&
                        (symbol->class_info() || symbol->record_info());
               }()) {
      // `Klass.method` where Klass is a type name (not a value) - same as
      // the metaclass case above; not a method-value binding.
      return std::nullopt;
    }
  }
  const Expr* method_base = member.base.get();
  // Type-cast base like `t_obj(expr).method`: the cast target determines the
  // method's owning class, but the receiver is still the cast argument.
  if (cls.empty() && member.base->kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(*member.base);
    if (c.args.size() == 1 && c.callee && c.callee->kind == Kind::Ident) {
      cls = analysis_.deduce_class_alias(*member.base);
      if (!cls.empty()) {
        method_base = c.args[0].get();
      }
    }
  }
  if (cls.empty()) cls = analysis_.deduce_class_alias(*member.base);
  if (cls.empty()) return std::nullopt;

  InstanceMethodLookup lookup = pick_instance_method_decl(
      cls, member.name, proc, allow_pointer_carrier_adapters);
  if (lookup.kind == InstanceMethodLookup::Kind::NoInstanceMethod) {
    return std::nullopt;
  }
  if (lookup.kind == InstanceMethodLookup::Kind::SignatureMismatch) {
    return MethodValueBinding::signature_mismatch(std::move(cls), method_base);
  }
  return MethodValueBinding::via_member(lookup.decl, std::move(cls), method_base,
                                        lookup.distance);
}

std::optional<PlainProcValueBinding>
EmitResolution::resolve_plain_proc_value_binding(const Expr& arg,
                                                 const TyProcedural& proc,
                                                 bool allow_pointer_carrier_adapters) {
  if (proc.is_method) return std::nullopt;

  const Expr* value = &arg;
  if (arg.kind == Kind::AddrOf) {
    const auto& addr = static_cast<const AddrOf&>(arg);
    if (addr.double_addr || !addr.operand) return std::nullopt;
    value = addr.operand.get();
  }

  std::vector<AnyCand> candidates;
  if (value->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*value);
    if (scope_.local_scope.count(id.name) || scope_.local_value_types.count(id.name) ||
        scope_.local_consts.count(id.name)) {
      return std::nullopt;
    }
    candidates = gather_callable_in_pascal_scope(id.name);
  } else if (value->kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(*value);
    if (auto unit_member = analysis_.resolve_unit_qualified_member(mem)) {
      auto uit = registry_.units.find(unit_member->unit_name);
      if (uit == registry_.units.end()) return std::nullopt;
      const bool own_unit = unit_member->unit_name == scope_.current_unit_name;
      const std::vector<ProcInfo>* procs =
          own_unit ? uit->second.find_procs(mem.name)
                   : uit->second.find_export_procs(mem.name);
      if (procs) {
        for (const auto& pi : *procs) {
          candidates.push_back({pi.decl.get(), pi.param_count,
                                pi.accepts_zero_args, unit_member->unit_name,
                                pi.defining_unit, {}, pi.return_type_name});
        }
      }
    }
  } else {
    return std::nullopt;
  }

  const ProcDecl* match = nullptr;
  int best_distance = std::numeric_limits<int>::max();
  bool ambiguous = false;
  for (const auto& candidate : candidates) {
    if (!candidate.decl) continue;
    // An instance method needs a Self slot and cannot be represented by a plain
    // procedure variable. Procedure-of-object binding handles that path.
    if (!candidate.decl->of_type.empty() && !candidate.decl->is_class_method) {
      continue;
    }
    auto distance = procedural_value_signature_distance(
        *candidate.decl, proc, allow_pointer_carrier_adapters);
    if (!distance) continue;
    if (*distance < best_distance) {
      match = candidate.decl;
      best_distance = *distance;
      ambiguous = false;
    } else if (*distance == best_distance) {
      ambiguous = true;
    }
  }
  if (!match || ambiguous) return std::nullopt;
  return PlainProcValueBinding{match, best_distance};
}

const TypeExpr* EmitResolution::method_value_member_base_type(
    const Expr& base) {
  // Method-value binding first classifies the receiver expression. A metaclass
  // value is rejected for procedure-of-object binding before normal member
  // lookup because it has no instance Self to capture.
  return analysis_.deduce_type(base);
}

std::optional<ConvScore> EmitResolution::score_procedural_argument_conversion(
    const Expr& arg, const TyProcedural& proc) {
  if (proc.is_method && arg.kind == Kind::NilLit) {
    return ConvScore{ConvRank::Exact, 0};
  }
  if (proc.is_method) {
    auto bind = resolve_method_value_binding(arg, proc);
    if (!bind) return std::nullopt;
    return bind->has_matching_decl() ? ConvScore{ConvRank::Exact, bind->distance}
                                     : ConvScore{};
  }
  // Plain `procedure(...)` slot: an instance-method value is not admissible
  // because it needs a `Self` pointer. Reuse the method-value resolver with a
  // temporary of-object target so this rejection and the successful binding path
  // use the same Pascal lookup and shadowing rules.
  TyProcedural method_view = proc;
  method_view.is_method = true;
  if (resolve_method_value_binding(arg, method_view)) {
    return ConvScore{};
  }
  if (auto bind = resolve_plain_proc_value_binding(arg, proc)) {
    return ConvScore{ConvRank::Exact, bind->distance};
  }
  return std::nullopt;
}

ConvScore EmitResolution::score_argument_conversion(
    const Expr& arg, const FlatCallParamInfo& param,
    bool allow_assignment_operator_conversions) {
  const TypeLookupContext* param_context =
      param.type_context ? param.type_context
                         : registry_.lookup_context_for_type(param.type);
  const TypeExpr* canon_param =
      param.type ? analysis_.semantic_shape_type_in_context(param.type,
                                                            param_context)
                 : nullptr;
  // Pascal's empty set literal is context-typed: once the parameter is known
  // to be a set, bare `[]` is an exact fit even though it has no standalone
  // element type.
  if (arg.kind == Kind::SetLit && canon_param &&
      canon_param->kind == Kind::TySet &&
      static_cast<const SetLit&>(arg).elements.empty()) {
    return {ConvRank::Exact, 0};
  }
  if (arg.kind == Kind::SetLit && canon_param &&
      canon_param->kind == Kind::TySet) {
    const TypeExpr* literal_type =
        analysis_.deduce_set_literal_type(static_cast<const SetLit&>(arg));
    switch (analysis_.classify_set_conversion(literal_type, canon_param)) {
      case SetConversionKind::Exact:
        return {ConvRank::Exact, 0};
      case SetConversionKind::Compatible:
        return param.mutable_ref ? ConvScore{} : ConvScore{ConvRank::SetCompatible, 0};
      case SetConversionKind::Incompatible:
        return {};
    }
  }
  if (arg.kind == Kind::SetLit &&
      set_literal_can_construct_open_array(
          static_cast<const SetLit&>(arg), param.type, param_context)) {
    // Bracket syntax is target-typed in Pascal calls: `[a, b]` is an
    // open-array constructor when the selected formal is `array of T`, not a
    // set value. Score it here so overload selection reaches the existing
    // open-array argument lowering instead of emitting a set literal too early.
    return {ConvRank::Exact, 0};
  }
  if (canon_param && canon_param->kind == Kind::TyArray &&
      static_cast<const TyArray&>(*canon_param).array_kind == ArrayKind::Open) {
    const TypeExpr* arg_type = argument_source_type_for_conversion(arg);
    const TypeExpr* canon_arg = analysis_.semantic_shape_type(arg_type);
    if (canon_arg && canon_arg->kind == Kind::TyArray) {
      return {ConvRank::Exact, 0};
    }
  }
  if (target_pointer_arithmetic_can_convert(
          arg, param.type, param_context,
          allow_assignment_operator_conversions)) {
    return {ConvRank::Exact, 0};
  }
  if (!param.mutable_ref && canon_param &&
      type_ops_.expr_is_storage_lvalue(arg)) {
    if (const TypeExpr* arg_type = argument_source_type_for_conversion(arg);
        type_ops_.fixed_char_array_value_can_decay_to_pchar(arg_type,
                                                            param.type)) {
      return {ConvRank::PointerValueConversion, 0};
    }
  }
  if (arg.kind == Kind::AddrOf && canon_param &&
      type_ops_.type_is_pointerish(canon_param)) {
    return {ConvRank::PointerValueConversion, 0};
  }
  // `nil` has no standalone type; deduce_type(NilLit) returns null, so
  // rank_conversion bails with Not-Viable. Without this case the picker
  // would reject `nil` for every pointer-compatible slot, killing overload
  // resolution on calls like `foo(@method, nil)`. Pascal accepts `nil`
  // wherever a pointer-compatible value goes - typed pointers, procedural
  // variables (including procedure-of-object), reference-class instances,
  // pchar-family aliases - so reuse the same `type_is_pointerish` predicate
  // the storage layer uses to classify those targets, instead of
  // rebuilding the list of accepting kinds here.
  if (arg.kind == Kind::NilLit && canon_param &&
      type_ops_.type_is_pointerish(canon_param)) {
    return {ConvRank::Exact, 0};
  }
  // Pascal's `Pointer` is the universal pointer type: typed-pointer values
  // (including `@var` results and reference-class values) can pass to a
  // `Pointer` formal without an explicit cast. FPC ranks that as a conversion,
  // not as identity, so an exact typed-pointer formal still wins.
  //
  // `@expr` always yields a pointer in Pascal, but deduce_type intentionally
  // returns null for `@array`: the emitter chooses pointer-to-array versus
  // pointer-to-first-element at the use site. Recognize AddrOf directly so the
  // picker doesn't reject `foo(@arr, ...)` against a pointer slot.
  if (canon_param && canon_param->kind == Kind::TyPointer &&
      !static_cast<const TyPointer&>(*canon_param).target) {
    if (const TypeExpr* arg_type = argument_source_type_for_conversion(arg);
        analysis_.same_type_ast(arg_type, param.type)) {
      return {ConvRank::Exact, 0};
    }
    if (arg.kind == Kind::AddrOf) {
      return {ConvRank::PointerValueConversion, 0};
    }
    if (const TypeExpr* arg_type = argument_source_type_for_conversion(arg)) {
      const TypeExpr* canon_arg = analysis_.semantic_shape_type(arg_type);
      if (canon_arg && type_ops_.type_is_pointerish(canon_arg)) {
        return {ConvRank::PointerValueConversion, 0};
      }
    }
  }
  if (canon_param && canon_param->kind == Kind::TyProcedural) {
    const auto& proc = static_cast<const TyProcedural&>(*canon_param);
    if (const TypeExpr* arg_type = argument_source_type_for_conversion(arg)) {
      const TypeExpr* canon_arg = analysis_.semantic_shape_type(arg_type);
      if (canon_arg && canon_arg->kind == Kind::TyProcedural) {
        return procedural_types_match(
                   static_cast<const TyProcedural&>(*canon_arg), proc)
                   ? ConvScore{ConvRank::Exact, 0}
                   : ConvScore{};
      }
    }
    if (auto score = score_procedural_argument_conversion(
            arg, proc)) {
      return *score;
    }
    // Do not fall through to rank_conversion here: procedural values can share
    // the same erased C++ callback carrier even when their Pascal signatures are
    // not assignment-compatible.  The procedural scorer above is the Pascal
    // type rule for this formal.
    return {};
  }
  const TypeExpr* arg_type = argument_source_type_for_conversion(arg);
  if (!param.mutable_ref && canon_param) {
    if ((analysis_.type_accepts_class_value(param.type) ||
         analysis_.type_accepts_class_value(canon_param)) &&
        !analysis_.concrete_class_name_for_metaclass_value(arg).empty()) {
      return {ConvRank::ClassValueConversion, 0};
    }
  }
  ConvScore direct = rank_conversion(arg_type, param.type, param.mutable_ref,
                                     param_context);
  if (direct.rank == ConvRank::Exact) return direct;
  if (!param.mutable_ref && arg_type &&
      type_ops_.pointer_value_conversion_is_valid(
          param.type, arg_type,
          /*explicit_pascal_cast=*/analysis_.explicit_typecast_result_type(arg) !=
              nullptr)) {
    return {ConvRank::PointerValueConversion, 0};
  }
  if (auto domain = integer_actual_domain_for_expr(arg)) {
    if (ConvScore ordinal =
            rank_integer_domain_conversion(*domain, param.type,
                                           param.mutable_ref, param_context);
        ordinal.viable()) {
      return ordinal;
    }
  }
  if (direct.viable()) return direct;
  if (!allow_assignment_operator_conversions || param.mutable_ref) return {};
  if (find_assignment_operator(arg_type, param.type).decl) {
    return {ConvRank::Operator, 0};
  }
  return {};
}

bool EmitResolution::conversion_score_less(const ConvScore& a,
                                           const ConvScore& b) const {
  // Lexicographic compare on (rank, distance): better rank wins; inside one
  // rank, the smaller distance is the tighter Pascal fit.
  if (a.rank != b.rank) return a.rank < b.rank;
  return a.distance < b.distance;
}

bool EmitResolution::conversion_candidate_dominates(
    const ScoredCandidate& a, const ScoredCandidate& b) const {
  // A candidate dominates another iff it is no worse at every explicit arg
  // position and strictly better at least once. Incomparable overload
  // candidates stay ambiguous.
  size_t n = std::min(a.scores.size(), b.scores.size());
  bool any_strict = false;
  for (size_t i = 0; i < n; ++i) {
    if (conversion_score_less(b.scores[i], a.scores[i])) return false;
    if (conversion_score_less(a.scores[i], b.scores[i])) any_strict = true;
  }
  return any_strict;
}

PickResult EmitResolution::pick_method_overload(
    const std::vector<MethodSig>& candidates,
    const std::vector<const Expr*>& args,
    bool allow_assignment_operator_conversions) {
  std::vector<AnyCand> wrapped;
  wrapped.reserve(candidates.size());
  for (const MethodSig& method : candidates) {
    if (!method.decl) continue;
    wrapped.push_back({method.decl.get(), method.param_count,
                       method.accepts_zero_args, {}, method.defining_unit,
                       method.declaring_type, method.return_type_name});
  }
  return pick_overload_from_candidates(
      wrapped, args, allow_assignment_operator_conversions);
}

PickResult EmitResolution::pick_overload_from_candidates(
    const std::vector<AnyCand>& candidates,
    const std::vector<const Expr*>& args,
    bool allow_assignment_operator_conversions) {
  if (candidates.empty()) return {};

  std::vector<ScoredCandidate> viable;
  for (const AnyCand& candidate : candidates) {
    const ProcDecl* decl = candidate.decl;
    if (!decl) continue;
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(
        decl, candidate.declaration_unit, candidate.declaring_type);
    // First filter by arity plus default-argument slack.
    if (args.size() > flat.size()) continue;
    bool ok = true;
    for (size_t i = args.size(); i < flat.size(); ++i) {
      if (!flat[i].default_value) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    ScoredCandidate s{decl, candidate.declaration_unit,
                      candidate.declaring_type, {}};
    s.scores.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
      ConvScore r = score_argument_conversion(
          *args[i], flat[i], allow_assignment_operator_conversions);
      if (!r.viable()) {
        ok = false;
        break;
      }
      s.scores.push_back(r);
    }
    if (ok) viable.push_back(std::move(s));
  }
  if (viable.empty()) return {};
  if (viable.size() == 1) return {viable[0].decl, false};

  if (auto integer_pick = pick_integer_domain_overload(viable, args)) {
    return *integer_pick;
  }

  size_t best = 0;
  for (size_t i = 1; i < viable.size(); ++i) {
    if (conversion_candidate_dominates(viable[i], viable[best])) best = i;
  }
  for (size_t i = 0; i < viable.size(); ++i) {
    if (i == best) continue;
    if (!conversion_candidate_dominates(viable[best], viable[i])) {
      // No strict winner: keep this as a Pascal ambiguity instead of silently
      // letting C++ overload resolution pick whichever conversion sequence it
      // prefers.
      return {nullptr, true};
    }
  }
  return {viable[best].decl, false};
}

ResolvedCall EmitResolution::resolved_call_from_candidate(
    const std::string& member_name, const AnyCand& chosen,
    bool ran_type_picker) const {
  const bool free_unit = !chosen.callee_unit.empty();
  return ResolvedCall{
      .decl = chosen.decl,
      .callee_kind = free_unit ? ResolvedCalleeKind::FreeFunctionInUnit
                               : ResolvedCalleeKind::Default,
      .defining_unit = chosen.callee_unit,
      .member_name = member_name,
      .default_arg_unit = chosen.declaration_unit,
      .signature_declaring_type = chosen.declaring_type,
      .return_type_name = chosen.return_type_name,
      .needs_arg_casts = ran_type_picker,
      .ambiguous = false};
}

std::string EmitResolution::value_class_alias(const Expr& e) {
  if (e.kind == Kind::Ident &&
      static_cast<const Ident&>(e).name == "self") {
    return scope_.current_class_name;
  }
  const bool produced_value =
      e.kind == Kind::Call || e.kind == Kind::Binary || e.kind == Kind::Unary;
  if (!produced_value) {
    if (auto cls = analysis_.deduce_class_alias(e); !cls.empty()) return cls;
  }
  if (const TypeExpr* t = overload_types_.type_for_overload(e)) {
    if (auto cls = analysis_.metaclass_target_name(t); !cls.empty()) return cls;
    if (auto cls = analysis_.direct_type_name(t);
        !cls.empty()) {
      return cls;
    }
    if (const TypeExpr* canon = analysis_.semantic_shape_type(t)) {
      if (auto cls = analysis_.direct_type_name(canon);
          !cls.empty()) {
        return cls;
      }
    }
  }
  return analysis_.deduce_class_alias(e);
}

std::string EmitResolution::value_metaclass_target(const Expr& e) {
  if (const TypeExpr* t = overload_types_.type_for_overload(e)) {
    return analysis_.metaclass_target_name(t);
  }
  return {};
}

std::string EmitResolution::receiver_class_for_member_call(const Expr& callee) {
  if (callee.kind != Kind::Member) return {};
  const auto& member = static_cast<const Member&>(callee);
  if (member.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*member.base);
    if (id.name == "self") return scope_.current_class_name;
    if (!analysis_.identifier_is_shadowed_value(id.name)) {
      const TypeSymbol* symbol = descriptor_payload_symbol(
          signature_type_symbol_for(registry_, scope_, id.name));
      if (symbol && (symbol->class_info() || symbol->record_info())) {
        return type_symbol_pascal_path(*symbol);
      }
    }
    return value_class_alias(*member.base);
  }
  return value_class_alias(*member.base);
}

ResolvedCall EmitResolution::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  const std::string member_name = callable_member_name(callee);
  // Method overloads and free-function overloads share the same picker.
  // Methods come from class-chain lookup; free functions come from the current
  // unit plus the visible uses chain. `inherited foo(...)` is still a separate
  // AST node kind here, but once candidate gathering is done the same dominance
  // rules decide the winner.
  std::vector<AnyCand> all_cands;
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    all_cands = gather_callable_in_pascal_scope(id.name);
  } else if (callee.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(callee);
    bool unit_qualified = false;
    bool inherited_call = false;
    if (mem.base->kind == Kind::Ident) {
      const auto& id = static_cast<const Ident&>(*mem.base);
      if (id.name == "inherited" && !scope_.current_class_name.empty()) {
        // `inherited Foo(args)` looks up `Foo` in the parent class chain,
        // skipping the current class. The later picker still runs exactly the
        // same type-based disambiguation as any other method call.
        inherited_call = true;
        const ClassInfo* ci =
            analysis_.migration_fallback_class_info_by_name(
                scope_.current_class_name);
        if (ci) {
          std::string parent = ci->parent;
          if (parent.empty() && ci->is_reference_type) {
            parent = "tobject";
          }
          all_cands = class_method_cands(parent, mem.name);
        }
      }
      if (!inherited_call) {
        // `Unit.proc(args)` uses the same Member AST node kind as
        // `receiver.method(args)`. Ask analysis for the qualified-unit
        // classification so overload resolution does not duplicate the
        // shadowing/uses checks.
        if (auto unit_member = analysis_.resolve_unit_qualified_member(mem)) {
          unit_qualified = true;
          all_cands =
              unit_export_proc_cands(unit_member->unit_name, mem.name);
        }
      }
    }
    if (!inherited_call && !unit_qualified) {
      const std::string metaclass = value_metaclass_target(*mem.base);
      if (!metaclass.empty()) {
        all_cands = metaclass_method_cands(metaclass, mem.name);
      } else {
        std::string cls = receiver_class_for_member_call(callee);
        if (!cls.empty()) all_cands = class_method_cands(cls, mem.name);
      }
    }
  }

  std::vector<AnyCand> arity_ok;
  for (const auto& a : all_cands) {
    // Arity-filter first, including default-argument slack. Synthetic runtime
    // builtins do not carry AST ProcDecls, so they must still participate
    // here through the cached `param_count` / `accepts_zero_args` metadata;
    // otherwise a visible imported decl with the same name but wrong arity
    // can steal the final emitted callee text.
    if (args.size() > a.param_count) continue;
    if (args.size() < a.param_count && !a.accepts_zero_args) {
      if (!a.decl) continue;
      std::vector<FlatCallParamInfo> flat = flatten_call_param_info(
          a.decl, a.declaration_unit, a.declaring_type);
      bool ok = true;
      for (size_t i = args.size(); i < flat.size(); ++i) {
        if (!flat[i].default_value) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
    }
    arity_ok.push_back(a);
  }

  if (arity_ok.size() == 1) {
    // Single arity-viable candidate. C++ does not need help choosing the
    // overload, but arity is not enough to make a Pascal call valid. Defer
    // actual/formal conversion validation to call lowering so the accepted
    // conversions and the emitted conversions stay one model.
    ResolvedCall resolved =
        resolved_call_from_candidate(member_name, arity_ok[0], false);
    resolved.needs_arg_validation = arity_ok[0].decl != nullptr;
    return resolved;
  }

  if (arity_ok.size() > 1) {
    // Multiple arity-viable candidates: run the Pascal conversion-rank picker
    // on the decl-backed subset. We do not silently pick among tied
    // incomparables; the caller reports the ambiguity as a Pascal error.
    std::vector<AnyCand> with_decl;
    for (const auto& a : arity_ok) {
      if (a.decl) with_decl.push_back(a);
    }
    if (with_decl.empty()) {
      return unresolved_call(member_name);
    }
    PickResult pr = pick_overload_from_candidates(
        with_decl, args, /*allow_assignment_operator_conversions=*/true);
    if (pr.ambiguous) {
      return ResolvedCall{.decl = nullptr,
                          .callee_kind = ResolvedCalleeKind::Default,
                          .defining_unit = {},
                          .member_name = member_name,
                          .default_arg_unit = {},
                          .signature_declaring_type = {},
                          .return_type_name = {},
                          .needs_arg_casts = false,
                          .ambiguous = true};
    }
    if (!pr.decl) {
      return unresolved_call(member_name);
    }
    for (const auto& a : arity_ok) {
      if (a.decl == pr.decl) {
        return resolved_call_from_candidate(member_name, a, true);
      }
    }
    // `pr.decl` came from the arity-filtered set, so reaching this path means
    // the ranking result and candidate metadata disagree. Keep the declaration
    // attached so the later diagnostic names the chosen Pascal callable.
    return ResolvedCall{.decl = pr.decl,
                        .callee_kind = ResolvedCalleeKind::Default,
                        .defining_unit = {},
                        .member_name = member_name,
                        .default_arg_unit = {},
                        .signature_declaring_type = {},
                        .return_type_name = {},
                        .needs_arg_casts = true,
                        .ambiguous = false};
  }

  return unresolved_call(member_name);
}

ResolvedCall EmitResolution::resolve_pointer_target_constructor(
    const TypeExpr* pointer_type, const Expr& ctor_callee,
    const std::vector<const Expr*>& args) {
  if (!pointer_type || ctor_callee.kind != Kind::Ident) {
    return unresolved_call();
  }
  std::string pointee = analysis_.pointer_target_type_name(pointer_type);
  if (pointee.empty()) return unresolved_call();
  const auto& ctor_ident = static_cast<const Ident&>(ctor_callee);
  Member member(ctor_callee.loc,
                std::make_shared<Ident>(ctor_callee.loc, std::move(pointee)),
                ctor_ident.name);
  return resolve_call(member, args);
}

bool EmitResolution::operand_type_allows_operator_lookup(const TypeExpr* t) {
  if (!t) return false;
  if (type_ops_.type_is_stringish(t)) return true;
  if (t->kind == Kind::TyName) {
    if (analysis_.primitive_info_for_type(t)) return false;
    const TypeSymbol* symbol =
        resolved_type_symbol_in_context(registry_, scope_, t);
    const TypeSymbol* payload = descriptor_payload_symbol(symbol);
    if (payload) {
      if (payload->enum_info()) return false;
      return payload->record_info() || payload->class_info() ||
             payload->interface_info();
    }
  }
  t = analysis_.semantic_shape_type(t);
  if (!t) return false;
  return t->kind == Kind::TyRecord || t->kind == Kind::TyObject ||
         t->kind == Kind::TyInterface || t->kind == Kind::TyPointer ||
         t->kind == Kind::TyArray || t->kind == Kind::TyMetaclass;
}

bool EmitResolution::operands_are_both_pcharish(const TypeExpr* lhs,
                                               const TypeExpr* rhs) {
  return type_ops_.type_is_pcharish(lhs) && type_ops_.type_is_pcharish(rhs);
}

bool EmitResolution::operands_are_pointer_nil_comparison(
    const Expr& lhs, const TypeExpr* lhs_type, const Expr& rhs,
    const TypeExpr* rhs_type) {
  return (lhs.kind == Kind::NilLit && type_ops_.type_is_pointerish(rhs_type)) ||
         (rhs.kind == Kind::NilLit && type_ops_.type_is_pointerish(lhs_type));
}

BinaryOperatorResult EmitResolution::find_binary_operator(
    const std::string& op, const Expr& lhs, const Expr& rhs) {
  const TypeExpr* lhs_type = overload_types_.type_for_overload(lhs);
  const TypeExpr* rhs_type = overload_types_.type_for_overload(rhs);
  // `nil` is the built-in null value for pointer-compatible operands. It should
  // not make overload resolution target-type the other operand through unrelated
  // pointer/string conversions.
  if (operands_are_pointer_nil_comparison(lhs, lhs_type, rhs, rhs_type)) {
    return {};
  }
  // PChar is pointer storage. Explicit/string-targeted contexts may convert it
  // by reading a NUL-terminated string, but binary operators on two PChar-like
  // operands must not use that conversion as an overload-resolution bridge:
  // `a < b` compares pointer values, not the strings at those addresses.
  if (operands_are_both_pcharish(lhs_type, rhs_type)) return {};
  const bool overloadable_context =
      operand_type_allows_operator_lookup(lhs_type) ||
      operand_type_allows_operator_lookup(rhs_type);
  if (!overloadable_context) return {};

  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(op);
  if (cands.empty()) return {};

  std::vector<AnyCand> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat =
        flatten_call_param_info(c.decl, c.declaration_unit, c.declaring_type);
    if (flat.size() == 2) arity_ok.push_back(c);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&lhs, &rhs};
  PickResult pr = pick_overload_from_candidates(
      arity_ok, args, /*allow_assignment_operator_conversions=*/true);
  if (pr.ambiguous) return {nullptr, {}, true};
  if (!pr.decl) return {};
  for (const auto& c : cands) {
    if (c.decl == pr.decl) return {pr.decl, c.callee_unit, false};
  }
  return {pr.decl, {}, false};
}

UnaryOperatorResult EmitResolution::find_unary_operator(
    const std::string& op, const Expr& operand) {
  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(op);
  if (cands.empty()) return {};

  std::vector<AnyCand> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat =
        flatten_call_param_info(c.decl, c.declaration_unit, c.declaring_type);
    if (flat.size() == 1) arity_ok.push_back(c);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&operand};
  PickResult pr = pick_overload_from_candidates(
      arity_ok, args, /*allow_assignment_operator_conversions=*/true);
  if (pr.ambiguous) return {nullptr, {}, true};
  if (!pr.decl) return {};
  for (const auto& c : cands) {
    if (c.decl == pr.decl) return {pr.decl, c.callee_unit, false};
  }
  return {pr.decl, {}, false};
}

AssignmentOperatorResult EmitResolution::find_assignment_operator(
    const TypeExpr* source, const TypeExpr* target) {
  if (!source || !target) return {};
  const TypeExpr* canon_target = analysis_.semantic_shape_type(target);
  if (!canon_target) return {};
  std::string target_cxx = type_ops_.type_to_cxx(*canon_target);
  if (target_cxx.empty()) return {};

  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(":=");
  std::vector<AnyCand> viable;
  for (const auto& c : cands) {
    const ProcDecl* pd = c.decl;
    if (!pd || pd->params.size() != 1 || !pd->return_type) continue;
    std::shared_ptr<TyName> qualified_ret = qualified_signature_type_name(
        registry_, scope_, pd->return_type.get(), c.declaration_unit,
        c.declaring_type);
    const TypeExpr* ret_type =
        qualified_ret ? qualified_ret.get() : pd->return_type.get();
    const TypeExpr* ret = analysis_.semantic_shape_type(ret_type);
    if (!ret || type_ops_.type_to_cxx(*ret) != target_cxx) continue;
    std::vector<FlatCallParamInfo> flat =
        flatten_call_param_info(pd, c.declaration_unit, c.declaring_type);
    if (flat.size() != 1) continue;
    if (rank_conversion(source, flat[0].type, flat[0].mutable_ref,
                        flat[0].type_context)
            .viable()) {
      viable.push_back(c);
    }
  }
  if (viable.empty()) return {};
  if (viable.size() == 1) {
    return {viable.front().decl, viable.front().callee_unit};
  }

  // Reuse the normal picker by synthesizing the source type as an expression
  // is not possible here, so choose only exact/equal-ranked matches. This is
  // enough for the FPC compiler's explicit one-parameter conversion operators
  // and avoids selecting among lossy numeric conversions.
  const ProcDecl* best = nullptr;
  std::string best_unit;
  ConvScore best_score{};
  bool have_best = false;
  bool ambiguous = false;
  for (const AnyCand& candidate : viable) {
    const ProcDecl* pd = candidate.decl;
    std::vector<FlatCallParamInfo> flat =
        flatten_call_param_info(pd, candidate.declaration_unit,
                                candidate.declaring_type);
    ConvScore score = rank_conversion(source, flat[0].type,
                                      flat[0].mutable_ref,
                                      flat[0].type_context);
    if (!score.viable()) continue;
    if (!have_best || score.rank < best_score.rank ||
        (score.rank == best_score.rank && score.distance < best_score.distance)) {
      best = pd;
      best_unit = candidate.callee_unit;
      best_score = score;
      have_best = true;
      ambiguous = false;
    } else if (score.rank == best_score.rank &&
               score.distance == best_score.distance) {
      ambiguous = true;
    }
  }
  return ambiguous ? AssignmentOperatorResult{} : AssignmentOperatorResult{best, best_unit};
}

}  // namespace tp2cc
