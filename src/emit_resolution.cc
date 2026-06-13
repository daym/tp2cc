#include "emit_resolution.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
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

}  // namespace

EmitResolution::EmitResolution(const TypeRegistry* registry,
                               ScopeStateView& scope, EmitAnalysis& analysis,
                               ResolutionTypeOps& type_ops,
                               OverloadTypeProvider& overload_types)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      type_ops_(type_ops),
      overload_types_(overload_types) {}

std::vector<EmitResolution::AnyCand> EmitResolution::class_method_cands(
    const std::string& cls, const std::string& name) {
  std::vector<AnyCand> candidates;
  if (!registry_ || cls.empty()) return candidates;
  auto* set = registry_->lookup_class_methods(
      cls, name, scope_.current_unit_name);
  if (!set) return candidates;
  for (const auto& ms : *set) {
    if (!ms.decl) continue;
    // Pascal constructor calls are expressions whose result is the constructed
    // class reference. The emitted constructor method body itself returns
    // bool, so call-type deduction needs this Pascal-facing result metadata.
    std::string return_type_name =
        ms.kind == SymKind::Constructor ? cls : std::string{};
    candidates.push_back({ms.decl.get(), ms.param_count, ms.accepts_zero_args,
                          {}, ms.defining_unit, ms.declaring_type,
                          std::move(return_type_name)});
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand> EmitResolution::metaclass_method_cands(
    const std::string& cls, const std::string& name) {
  std::vector<AnyCand> candidates;
  if (!registry_ || cls.empty()) return candidates;
  auto* set = registry_->lookup_class_methods(
      cls, name, scope_.current_unit_name);
  if (!set) return candidates;
  for (const auto& ms : *set) {
    if (!ms.decl) continue;
    if (ms.kind != SymKind::Constructor && ms.kind != SymKind::ClassMethod) {
      continue;
    }
    // Pascal constructor calls are expressions whose result is the constructed
    // class reference. The emitted constructor method body itself returns
    // bool, so call-type deduction needs this Pascal-facing result metadata.
    std::string return_type_name =
        ms.kind == SymKind::Constructor ? cls : std::string{};
    candidates.push_back({ms.decl.get(), ms.param_count, ms.accepts_zero_args,
                          {}, ms.defining_unit, ms.declaring_type,
                          std::move(return_type_name)});
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand> EmitResolution::unit_export_proc_cands(
    const std::string& unit, const std::string& name) {
  std::vector<AnyCand> candidates;
  if (!registry_) return candidates;
  auto it = registry_->units.find(unit);
  if (it == registry_->units.end()) return candidates;
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
  if (!registry_) return candidates;

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
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return candidates;
  // A decl in the current unit shadows same-named decls reached through
  // `uses`. Without this stop, local overload sets and imported overload sets
  // would get merged even though Pascal lexical lookup never reaches the
  // imports once the current unit contributes the name.
  if (auto* local = cur->second.find_procs(name); local && !local->empty()) {
    for (const auto& pi : *local) {
      candidates.push_back(
          {pi.decl.get(), pi.param_count, pi.accepts_zero_args,
           scope_.current_unit_name, pi.defining_unit, {},
           pi.return_type_name});
    }
    return candidates;
  }
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
       ++it) {
    std::vector<AnyCand> unit_candidates = unit_export_proc_cands(*it, name);
    candidates.insert(candidates.end(),
                      std::make_move_iterator(unit_candidates.begin()),
                      std::make_move_iterator(unit_candidates.end()));
  }
  return candidates;
}

std::vector<EmitResolution::AnyCand>
EmitResolution::gather_operator_in_pascal_scope(
    const std::string& op) {
  std::vector<AnyCand> candidates;
  if (!registry_) return candidates;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return candidates;
  if (auto* local = cur->second.find_operators(op); local && !local->empty()) {
    for (const auto& pi : *local) {
      candidates.push_back(
          {pi.decl.get(), pi.param_count, pi.accepts_zero_args,
           scope_.current_unit_name, pi.defining_unit, {}, {}});
    }
    return candidates;
  }
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
       ++it) {
    auto uit = registry_->units.find(*it);
    if (uit == registry_->units.end()) continue;
    auto* ops = uit->second.find_export_operators(op);
    if (!ops) continue;
    for (const auto& pi : *ops) {
      candidates.push_back({pi.decl.get(), pi.param_count, pi.accepts_zero_args,
                            *it, pi.defining_unit, {}, {}});
    }
  }
  return candidates;
}

std::vector<FlatCallParamInfo> EmitResolution::flatten_call_param_info(
    const ProcDecl* decl) {
  std::vector<FlatCallParamInfo> flat_params;
  if (!decl) return flat_params;
  for (const auto& p : decl->params) {
    size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) {
      flat_params.emplace_back(
          p.type.get(), !p.type,
          p.mode == Param::Var || p.mode == Param::Out ||
              (p.mode == Param::Const &&
               analysis_.const_param_needs_mutable_ref(p.type.get())),
          p.default_value.get());
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
      t = analysis_.canonicalize_type(
          static_cast<const TyDistinct&>(*t).underlying.get());
    } else {
      return builtin_integer_type("longint");
    }
  }
  return t;
}

ConvScore EmitResolution::class_hierarchy_conversion_score(
    const TypeExpr* arg, const TypeExpr* param) {
  if (!registry_) return {};
  if (!arg || !param || arg->kind != Kind::TyName ||
      param->kind != Kind::TyName) {
    return {};
  }
  const auto& arg_name = static_cast<const TyName&>(*arg).name;
  const auto& param_name = static_cast<const TyName&>(*param).name;
  const auto* arg_class = analysis_.class_info_for_type_name(arg_name);
  const auto* param_class = analysis_.class_info_for_type_name(param_name);
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
    cur = cur->parent.empty()
              ? nullptr
              : registry_->lookup_class(cur->parent, cur->defining_unit);
    ++depth;
  }
  return {};
}

const PrimitiveInfo* EmitResolution::primitive_for_type(const TypeExpr* t) {
  if (!t || t->kind != Kind::TyName) return nullptr;
  return primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
}

std::optional<EmitResolution::IntegerActualDomain>
EmitResolution::integer_actual_domain_for_type(const TypeExpr* t) {
  t = analysis_.canonicalize_type(t);
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
  if (!pi || pi->int_kind == PrimitiveIntKind::None || pi->bits == 0) {
    return std::nullopt;
  }
  if (pi->int_kind == PrimitiveIntKind::Unsigned) {
    return IntegerActualDomain{
        .low = 0,
        .high = pi->bits >= 64 ? std::numeric_limits<uint64_t>::max()
                               : ((uint64_t{1} << pi->bits) - 1),
        .preferred_kind = PrimitiveIntKind::Unsigned};
  }
  return IntegerActualDomain{
      .low = pi->bits >= 64
                 ? std::numeric_limits<int64_t>::min()
                 : -(int64_t{1} << (pi->bits - 1)),
      .high = pi->bits >= 64
                  ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                  : ((uint64_t{1} << (pi->bits - 1)) - 1),
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
  if (formal.int_kind == PrimitiveIntKind::None || formal.bits == 0) {
    return false;
  }
  if (formal.int_kind == PrimitiveIntKind::Unsigned) {
    if (domain.low < 0) return false;
    const uint64_t max =
        formal.bits >= 64 ? std::numeric_limits<uint64_t>::max()
                          : ((uint64_t{1} << formal.bits) - 1);
    return domain.high <= max;
  }
  const int64_t min =
      formal.bits >= 64 ? std::numeric_limits<int64_t>::min()
                        : -(int64_t{1} << (formal.bits - 1));
  const uint64_t max =
      formal.bits >= 64
          ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
          : ((uint64_t{1} << (formal.bits - 1)) - 1);
  return domain.low >= min && domain.high <= max;
}

bool EmitResolution::set_literal_can_construct_open_array(
    const SetLit& literal, const TypeExpr* param) const {
  const TypeExpr* p = analysis_.canonicalize_type(param);
  if (!p || p->kind != Kind::TyArray ||
      static_cast<const TyArray&>(*p).array_kind != ArrayKind::Open) {
    return false;
  }
  for (const auto& element : literal.elements) {
    if (element && element->kind == Kind::Range) return false;
  }
  return true;
}

ConvScore EmitResolution::rank_integer_domain_conversion(
    const IntegerActualDomain& domain, const TypeExpr* param, bool var_param) {
  if (var_param) return {};
  const TypeExpr* p = analysis_.canonicalize_type(param);
  const PrimitiveInfo* formal = primitive_for_type(p);
  if (!formal || formal->int_kind == PrimitiveIntKind::None ||
      formal->bits == 0) {
    return {};
  }
  if (!integer_domain_fits_primitive(domain, *formal)) return {};
  // FPC chooses the smallest integer formal that can represent the Pascal
  // source domain, then uses signedness only to break equal-width ties. This is
  // why `byte` binds to `longint` rather than `qword` when no `cardinal`
  // overload exists, while `cardinal` still binds to `qword` over `int64`.
  const int sign_mismatch =
      domain.preferred_kind == formal->int_kind ? 0 : 1;
  return {ConvRank::IntDomainCompatible,
          static_cast<int>(formal->bits) * 2 + sign_mismatch};
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
        flatten_call_param_info(candidate.decl);
    if (args.size() > flat.size()) continue;

    ScoredCandidate scored{candidate.decl, candidate.scores};
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
      const TypeExpr* formal_type = analysis_.canonicalize_type(flat[i].type);
      const PrimitiveInfo* formal = primitive_for_type(formal_type);
      if (!formal || formal->int_kind == PrimitiveIntKind::None ||
          formal->bits == 0) {
        // Integer actuals may also fit non-primitive overloads through
        // assignment operators. FPC still ranks the primitive integer candidates
        // as their own set first; operator candidates remain available only if
        // no primitive integer candidate wins.
        fits_all = false;
        break;
      }
      ConvScore score =
          rank_integer_domain_conversion(*domains[i], flat[i].type, false);
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

int EmitResolution::real_conversion_rank(std::string_view name) const {
  if (name == "single") return 1;
  if (name == "double" || name == "real") return 2;
  if (name == "extended" || name == "comp") return 3;
  return 0;
}

bool EmitResolution::type_is_shortstring_family(const TypeExpr* t) const {
  if (!t) return false;
  if (t->kind == Kind::TyString) return true;
  if (t->kind != Kind::TyName) return false;
  const auto& n = ascii_lower(static_cast<const TyName&>(*t).name);
  return n == "shortstring";
}

bool EmitResolution::type_is_ansistring(const TypeExpr* t) const {
  return t && t->kind == Kind::TyName &&
         ascii_lower(static_cast<const TyName&>(*t).name) == "ansistring";
}

bool EmitResolution::type_is_char_type(const TypeExpr* t) const {
  return t && t->kind == Kind::TyName &&
         ascii_lower(static_cast<const TyName&>(*t).name) == "char";
}

ConvScore EmitResolution::rank_conversion(const TypeExpr* arg,
                                          const TypeExpr* param,
                                          bool var_param) {
  if (!arg || !param) return {};
  const TypeExpr* a = analysis_.canonicalize_type(arg);
  const TypeExpr* p = analysis_.canonicalize_type(param);
  if (!a || !p) return {};

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
  // 1. Exact identity after canonicalization.
  if (!a_cxx.empty() && a_cxx == p_cxx) return {ConvRank::Exact, 0};

  const TypeExpr* a_under = strip_conversion_wrapper(a);
  const TypeExpr* p_under = strip_conversion_wrapper(p);
  // 2. Equal modulo distinct/subrange wrappers. Distinct types still lower to
  // the same underlying storage for overload ranking, and subranges adopt
  // their base integer type here.
  if (a_under && p_under &&
      type_cxx_or_empty(a_under) == type_cxx_or_empty(p_under)) {
    return {ConvRank::Equal, 0};
  }

  if (var_param) {
    // `var`/`out` params only accept identity/equal-or-wrapper matches, plus
    // class-hierarchy aliasing for reference types. Anything else would pass a
    // temporary or layout-incompatible slot by reference.
    if (ConvScore score = class_hierarchy_conversion_score(a, p);
        score.viable()) {
      return score;
    }
    return {};
  }

  // 3. Class hierarchy: a derived class may pass where an ancestor is expected.
  // Fewer parent hops means the closer Pascal match.
  if (ConvScore score = class_hierarchy_conversion_score(a, p); score.viable()) {
    return score;
  }

  // 4. Integer widening with unchanged signedness. Prefer the smallest target
  // that still contains the source by using the bit-width gap as distance.
  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = primitive_for_type(p);
        pi && pi->int_kind == ai->int_kind && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::IntWideningSameSign,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  // 5. Real widening follows Pascal's precision ladder. Again, smaller rank
  // gaps are better fits.
  if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
    int ar =
        real_conversion_rank(ascii_lower(static_cast<const TyName&>(*a).name));
    int pr =
        real_conversion_rank(ascii_lower(static_cast<const TyName&>(*p).name));
    if (ar > 0 && pr > 0 && pr >= ar) return {ConvRank::RealWidening, pr - ar};
  }

  // 6. Same-family ShortString widening.
  if (type_is_shortstring_family(a) && type_is_shortstring_family(p)) {
    return {ConvRank::StringSameTagWiden, 0};
  }

  const bool param_is_shortstring = type_is_shortstring_family(p);
  const bool param_is_ansistring = type_is_ansistring(p);
  const bool arg_is_shortstring = type_is_shortstring_family(a);
  const bool arg_is_ansistring = type_is_ansistring(a);
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
        pi && pi->int_kind != PrimitiveIntKind::None && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::OrdinalSignChange,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  if (const auto* ai = primitive_for_type(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    // 10. Integer narrowing. Pascal still permits this for value/const params
    // with range checking; the picker must rank it as viable but worse than
    // widening or sign-preserving matches.
    if (const auto* pi = primitive_for_type(p);
        pi && pi->int_kind != PrimitiveIntKind::None &&
        pi->bits != 0 && ai->bits != 0 && pi->bits < ai->bits) {
      return {ConvRank::IntNarrowing,
              static_cast<int>(ai->bits) - static_cast<int>(pi->bits)};
    }
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
  return type_ops_.procedural_param_types_to_cxx(decl.params) ==
         type_ops_.procedural_param_types_to_cxx(proc.params);
}

EmitResolution::InstanceMethodLookup
EmitResolution::pick_instance_method_decl(
    const std::string& cls, const std::string& name,
    const TyProcedural& proc) {
  const auto* methods =
      registry_->lookup_class_methods(cls, name, scope_.current_unit_name);
  if (!methods) return InstanceMethodLookup::no_instance_method();
  bool saw_instance = false;
  for (const auto& method : *methods) {
    if (!method.decl || method.decl->is_class_method) continue;
    saw_instance = true;
    if (procedural_signatures_match(*method.decl, proc)) {
      return InstanceMethodLookup::match(method.decl.get());
    }
  }
  if (saw_instance) return InstanceMethodLookup::signature_mismatch();
  return InstanceMethodLookup::no_instance_method();
}

std::optional<MethodValueBinding> EmitResolution::resolve_method_value_binding(
    const Expr& arg, const TyProcedural& proc) {
  if (!proc.is_method || !registry_) return std::nullopt;

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
        proc);
    if (lookup.kind == InstanceMethodLookup::Kind::NoInstanceMethod) {
      return std::nullopt;
    }
    if (lookup.kind == InstanceMethodLookup::Kind::SignatureMismatch) {
      return MethodValueBinding::signature_mismatch(scope_.current_class_name,
                                                    nullptr);
    }
    return MethodValueBinding::via_self(lookup.decl, scope_.current_class_name);
  }

  if (value->kind != Kind::Member) return std::nullopt;
  const auto& member = static_cast<const Member&>(*value);
  if (!member.base) return std::nullopt;

  // `Klass.method` (a metaclass-qualified reference) doesn't bind a Self -
  // it would name a class function, which is not a valid value for a
  // procedure-of-object target.
  if (!analysis_.metaclass_target_name(analysis_.deduce_type(*member.base))
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
                 const TypeSymbol* symbol = registry_->lookup_type_symbol(
                     id.name, scope_.current_unit_name);
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

  InstanceMethodLookup lookup = pick_instance_method_decl(cls, member.name, proc);
  if (lookup.kind == InstanceMethodLookup::Kind::NoInstanceMethod) {
    return std::nullopt;
  }
  if (lookup.kind == InstanceMethodLookup::Kind::SignatureMismatch) {
    return MethodValueBinding::signature_mismatch(std::move(cls), method_base);
  }
  return MethodValueBinding::via_member(lookup.decl, std::move(cls),
                                        method_base);
}

std::optional<ConvScore> EmitResolution::score_procedural_argument_conversion(
    const Expr& arg, const TyProcedural& proc) {
  if (proc.is_method && arg.kind == Kind::NilLit) {
    return ConvScore{ConvRank::Exact, 0};
  }
  if (proc.is_method) {
    auto bind = resolve_method_value_binding(arg, proc);
    if (!bind) return std::nullopt;
    return bind->has_matching_decl() ? ConvScore{ConvRank::Exact, 0}
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
  return std::nullopt;
}

ConvScore EmitResolution::score_argument_conversion(
    const Expr& arg, const FlatCallParamInfo& param,
    bool allow_assignment_operator_conversions) {
  const TypeExpr* canon_param =
      param.type ? analysis_.canonicalize_type(param.type) : nullptr;
  // Pascal's empty set literal is context-typed: once the parameter is known
  // to be a set, bare `[]` is an exact fit even though it has no standalone
  // element type.
  if (arg.kind == Kind::SetLit && canon_param &&
      canon_param->kind == Kind::TySet &&
      static_cast<const SetLit&>(arg).elements.empty()) {
    return {ConvRank::Exact, 0};
  }
  if (arg.kind == Kind::SetLit &&
      set_literal_can_construct_open_array(
          static_cast<const SetLit&>(arg), param.type)) {
    // Bracket syntax is target-typed in Pascal calls: `[a, b]` is an
    // open-array constructor when the selected formal is `array of T`, not a
    // set value. Score it here so overload selection reaches the existing
    // open-array argument lowering instead of emitting a set literal too early.
    return {ConvRank::Exact, 0};
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
  // Pascal's `pointer` is the universal pointer type: any typed-pointer
  // value (including `@var` results and reference-class values) is freely
  // assignable to a `pointer` parameter without a cast. rank_conversion
  // doesn't model this implicit narrowing, so the picker rejects e.g.
  // `foo(@s, ...)` against a `pointer` slot when there's a competing
  // overload. Score it Exact here so the picker sees the call as viable.
  //
  // `@expr` always yields a pointer in Pascal, but deduce_type intentionally
  // returns null for `@array`: the emitter chooses pointer-to-array versus
  // pointer-to-first-element at the use site. Recognize AddrOf directly so the
  // picker doesn't reject `foo(@arr, ...)` against a pointer slot.
  if (canon_param && canon_param->kind == Kind::TyName &&
      static_cast<const TyName&>(*canon_param).name == "pointer") {
    if (arg.kind == Kind::AddrOf) {
      return {ConvRank::Exact, 0};
    }
    if (const TypeExpr* arg_type =
            overload_types_.type_for_overload(arg)) {
      const TypeExpr* canon_arg = analysis_.canonicalize_type(arg_type);
      if (canon_arg && type_ops_.type_is_pointerish(canon_arg)) {
        return {ConvRank::Exact, 0};
      }
    }
  }
  if (canon_param && canon_param->kind == Kind::TyProcedural) {
    if (auto score = score_procedural_argument_conversion(
            arg, static_cast<const TyProcedural&>(*canon_param))) {
      return *score;
    }
  }
  const TypeExpr* arg_type = overload_types_.type_for_overload(arg);
  ConvScore direct = rank_conversion(arg_type, param.type, param.mutable_ref);
  if (direct.rank == ConvRank::Exact) return direct;
  if (auto domain = integer_actual_domain_for_expr(arg)) {
    if (ConvScore ordinal =
            rank_integer_domain_conversion(*domain, param.type,
                                           param.mutable_ref);
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

PickResult EmitResolution::pick_overload(
    const std::vector<const ProcDecl*>& candidates,
    const std::vector<const Expr*>& args,
    bool allow_assignment_operator_conversions) {
  if (candidates.empty()) return {};

  std::vector<ScoredCandidate> viable;
  for (const ProcDecl* decl : candidates) {
    if (!decl) continue;
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(decl);
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

    ScoredCandidate s{decl, {}};
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
    if (registry_) {
      if (auto cls = registry_->direct_type_name(t, scope_.current_unit_name);
          !cls.empty()) {
        return cls;
      }
      if (const TypeExpr* canon = analysis_.canonicalize_type(t)) {
        if (auto cls =
                registry_->direct_type_name(canon, scope_.current_unit_name);
            !cls.empty()) {
          return cls;
        }
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
    if (!analysis_.identifier_is_shadowed_value(id.name) &&
        [&] {
          const TypeSymbol* symbol =
              signature_type_symbol_for(registry_, scope_, id.name);
          return symbol && (symbol->class_info() || symbol->record_info());
        }()) {
      const TypeSymbol* symbol =
          signature_type_symbol_for(registry_, scope_, id.name);
      return symbol ? type_symbol_pascal_path(*symbol) : id.name;
    }
    return value_class_alias(*member.base);
  }
  return value_class_alias(*member.base);
}

ResolvedCall EmitResolution::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  const std::string member_name = callable_member_name(callee);
  if (!registry_) return unresolved_call(member_name);
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
            registry_->lookup_class(scope_.current_class_name,
                                    scope_.current_unit_name);
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
      std::vector<FlatCallParamInfo> flat = flatten_call_param_info(a.decl);
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
    // overload, but we still record the defining unit so the printer spells
    // the callee through the same Pascal lookup path.
    return resolved_call_from_candidate(member_name, arity_ok[0], false);
  }

  if (arity_ok.size() > 1) {
    // Multiple arity-viable candidates: run the Pascal conversion-rank picker
    // on the decl-backed subset. We do not silently pick among tied
    // incomparables; the caller reports the ambiguity as a Pascal error.
    std::vector<const ProcDecl*> with_decl;
    for (const auto& a : arity_ok) {
      if (a.decl) with_decl.push_back(a.decl);
    }
    if (with_decl.empty()) {
      return unresolved_call(member_name);
    }
    PickResult pr = pick_overload(
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
  if (!registry_ || !pointer_type || ctor_callee.kind != Kind::Ident) {
    return unresolved_call();
  }
  std::string pointee = registry_->pointer_target_type_name(pointer_type);
  if (pointee.empty()) return unresolved_call();
  const auto& ctor_ident = static_cast<const Ident&>(ctor_callee);
  Member member(ctor_callee.loc,
                std::make_shared<Ident>(ctor_callee.loc, std::move(pointee)),
                ctor_ident.name);
  return resolve_call(member, args);
}

bool EmitResolution::operand_type_allows_operator_lookup(const TypeExpr* t) {
  t = analysis_.canonicalize_type(t);
  if (!t) return false;
  if (type_ops_.type_is_stringish(t)) return true;
  if (t->kind == Kind::TyName) {
    const auto& name = ascii_lower(static_cast<const TyName&>(*t).name);
    if (primitive_info(name)) return false;
    const TypeSymbol* symbol =
        registry_->lookup_type_symbol(name, scope_.current_unit_name);
    if (!symbol || symbol->enum_info()) return false;
    return symbol->record_info() || symbol->class_info() ||
           symbol->interface_info() || symbol->alias_info();
  }
  return t->kind == Kind::TyRecord || t->kind == Kind::TyObject ||
         t->kind == Kind::TyInterface || t->kind == Kind::TyPointer ||
         t->kind == Kind::TyArray || t->kind == Kind::TyMetaclass;
}

BinaryOperatorResult EmitResolution::find_binary_operator(
    const std::string& op, const Expr& lhs, const Expr& rhs) {
  if (!registry_) return {};

  const TypeExpr* lhs_type = overload_types_.type_for_overload(lhs);
  const TypeExpr* rhs_type = overload_types_.type_for_overload(rhs);
  const bool overloadable_context =
      operand_type_allows_operator_lookup(lhs_type) ||
      operand_type_allows_operator_lookup(rhs_type);
  if (!overloadable_context) return {};

  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(op);
  if (cands.empty()) return {};

  std::vector<const ProcDecl*> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(c.decl);
    if (flat.size() == 2) arity_ok.push_back(c.decl);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&lhs, &rhs};
  PickResult pr = pick_overload(
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
  if (!registry_) return {};
  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(op);
  if (cands.empty()) return {};

  std::vector<const ProcDecl*> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(c.decl);
    if (flat.size() == 1) arity_ok.push_back(c.decl);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&operand};
  PickResult pr = pick_overload(
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
  if (!registry_ || !source || !target) return {};
  const TypeExpr* canon_target = analysis_.canonicalize_type(target);
  if (!canon_target) return {};
  std::string target_cxx = type_ops_.type_to_cxx(*canon_target);
  if (target_cxx.empty()) return {};

  std::vector<AnyCand> cands = gather_operator_in_pascal_scope(":=");
  std::vector<const ProcDecl*> viable;
  for (const auto& c : cands) {
    const ProcDecl* pd = c.decl;
    if (!pd || pd->params.size() != 1 || !pd->return_type) continue;
    const TypeExpr* ret = analysis_.canonicalize_type(pd->return_type.get());
    if (!ret || type_ops_.type_to_cxx(*ret) != target_cxx) continue;
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(pd);
    if (flat.size() != 1) continue;
    if (rank_conversion(source, flat[0].type, flat[0].mutable_ref).viable()) {
      viable.push_back(pd);
    }
  }
  if (viable.empty()) return {};
  if (viable.size() == 1) {
    for (const auto& c : cands) {
      if (c.decl == viable.front()) return {viable.front(), c.callee_unit};
    }
    return {viable.front(), {}};
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
  for (const ProcDecl* pd : viable) {
    std::vector<FlatCallParamInfo> flat = flatten_call_param_info(pd);
    ConvScore score = rank_conversion(source, flat[0].type, flat[0].mutable_ref);
    if (!score.viable()) continue;
    if (!have_best || score.rank < best_score.rank ||
        (score.rank == best_score.rank && score.distance < best_score.distance)) {
      best = pd;
      for (const auto& c : cands) {
        if (c.decl == pd) {
          best_unit = c.callee_unit;
          break;
        }
      }
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
