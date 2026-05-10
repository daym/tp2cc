#include "emit_resolution.h"

#include <algorithm>

#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitResolution::EmitResolution(const TypeRegistry* registry,
                               ScopeStateView& scope, EmitAnalysis& analysis,
                               ResolutionTypeOps& type_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      type_ops_(type_ops) {}

void EmitResolution::append_class_method_cands(
    const std::string& cls, const std::string& name,
    std::vector<AnyCand>& cands) {
  if (!registry_ || cls.empty()) return;
  auto* set = registry_->lookup_class_methods(
      cls, name, scope_.current_unit_name);
  if (!set) return;
  for (const auto& ms : *set) {
    if (!ms.decl) continue;
    cands.push_back({ms.decl.get(), ms.param_count, ms.accepts_zero_args, {}});
  }
}

void EmitResolution::append_unit_export_proc_cands(
    const std::string& unit, const std::string& name,
    std::vector<AnyCand>& cands) {
  if (!registry_) return;
  auto it = registry_->units.find(unit);
  if (it == registry_->units.end()) return;
  auto* v = (unit == scope_.current_unit_name)
                ? it->second.find_procs(name)
                : it->second.find_export_procs(name);
  if (!v) return;
  for (const auto& pi : *v) {
    cands.push_back({pi.decl.get(), pi.param_count, pi.accepts_zero_args, unit});
  }
}

void EmitResolution::gather_callable_in_pascal_scope(
    const std::string& name, std::vector<AnyCand>& cands) {
  if (!registry_) return;
  auto try_class = [&](const std::string& cls) -> bool {
    size_t before = cands.size();
    append_class_method_cands(cls, name, cands);
    return cands.size() != before;
  };

  for (auto wit = scope_.with_stack.rbegin(); wit != scope_.with_stack.rend();
       ++wit) {
    if (try_class(wit->class_name)) return;
  }
  if (auto nit = scope_.local_nested_fns.find(name);
      nit != scope_.local_nested_fns.end()) {
    if (nit->second.decl) {
      cands.push_back(
          {nit->second.decl, nit->second.param_count,
           nit->second.accepts_zero_args, {}});
    }
    return;
  }
  if (try_class(scope_.current_class_name)) return;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return;
  // A decl in the current unit shadows same-named decls reached through
  // `uses`. Without this stop, local overload sets and imported overload sets
  // would get merged even though Pascal lexical lookup never reaches the
  // imports once the current unit contributes the name.
  if (auto* local = cur->second.find_procs(name); local && !local->empty()) {
    for (const auto& pi : *local) {
      cands.push_back(
          {pi.decl.get(), pi.param_count, pi.accepts_zero_args,
           scope_.current_unit_name});
    }
    return;
  }
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
       ++it) {
    append_unit_export_proc_cands(*it, name, cands);
  }
}

void EmitResolution::gather_operator_in_pascal_scope(
    const std::string& op, std::vector<AnyCand>& cands) {
  if (!registry_) return;
  auto cur = registry_->units.find(scope_.current_unit_name);
  if (cur == registry_->units.end()) return;
  if (auto* local = cur->second.find_operators(op); local && !local->empty()) {
    for (const auto& pi : *local) {
      cands.push_back(
          {pi.decl.get(), pi.param_count, pi.accepts_zero_args,
           scope_.current_unit_name});
    }
    return;
  }
  for (auto it = cur->second.uses.rbegin(); it != cur->second.uses.rend();
       ++it) {
    auto uit = registry_->units.find(*it);
    if (uit == registry_->units.end()) continue;
    auto* ops = uit->second.find_export_operators(op);
    if (!ops) continue;
    for (const auto& pi : *ops) {
      cands.push_back({pi.decl.get(), pi.param_count, pi.accepts_zero_args,
                       *it});
    }
  }
}

const ProcDecl* EmitResolution::resolve_call_decl(const Expr& callee) {
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    if (!registry_) return nullptr;
    std::vector<AnyCand> cands;
    gather_callable_in_pascal_scope(id.name, cands);
    for (const auto& c : cands) {
      if (c.decl) return c.decl;
    }
    return nullptr;
  }
  if (callee.kind != Kind::Member || !registry_) return nullptr;
  const auto& mem = static_cast<const Member&>(callee);
  if (auto unit_member = analysis_.resolve_unit_qualified_member(mem)) {
    // Unit-qualified calls still need the underlying ProcDecl here so call
    // lowering can materialize trailing defaults and parameter modes before
    // the printer spells the final C++ call.
    auto uit = registry_->units.find(unit_member->unit_name);
    if (uit != registry_->units.end()) {
      const ProcInfo* pi =
          (unit_member->unit_name == scope_.current_unit_name)
              ? uit->second.find_proc(mem.name)
              : uit->second.find_export_proc(mem.name);
      if (pi) {
        return pi->decl.get();
      }
    }
  }
  std::string cls;
  if (mem.base->kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(*mem.base);
    if (id.name == "self") {
      cls = scope_.current_class_name;
    } else if (!analysis_.identifier_is_shadowed_value(id.name) &&
               (registry_->has_class(id.name, scope_.current_unit_name) ||
                registry_->records.count(id.name))) {
      cls = id.name;
    } else {
      // Method calls through a variable or parameter receiver (`source.read`)
      // still need the receiver's declared Pascal type here so later lowering
      // can recover parameter modes and trailing defaults from the right decl.
      cls = analysis_.deduce_class_alias(*mem.base);
    }
  } else {
    cls = analysis_.deduce_class_alias(*mem.base);
  }
  if (cls.empty()) return nullptr;
  if (auto* m = registry_->lookup_class_method(
          cls, mem.name, scope_.current_unit_name)) {
    return m->decl.get();
  }
  return nullptr;
}

void EmitResolution::flatten_call_param_info(
    const ProcDecl* decl, std::vector<FlatCallParamInfo>& flat_params) {
  flat_params.clear();
  if (!decl) return;
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
}

ConvScore EmitResolution::rank_conversion(const TypeExpr* arg,
                                          const TypeExpr* param,
                                          bool var_param) {
  if (!arg || !param) return {};
  const TypeExpr* a = analysis_.canonicalize_type(arg);
  const TypeExpr* p = analysis_.canonicalize_type(param);
  if (!a || !p) return {};

  auto type_text = [&](const TypeExpr* t) {
    return t ? type_ops_.type_to_cxx(*t) : std::string{};
  };

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

  std::string a_cxx = type_text(a);
  std::string p_cxx = type_text(p);
  // 1. Exact identity after canonicalization.
  if (!a_cxx.empty() && a_cxx == p_cxx) return {ConvRank::Exact, 0};

  auto strip_wrap = [&](const TypeExpr* t) {
    while (t && (t->kind == Kind::TyDistinct || t->kind == Kind::TySubrange)) {
      if (t->kind == Kind::TyDistinct) {
        t = analysis_.canonicalize_type(
            static_cast<const TyDistinct&>(*t).underlying.get());
      } else {
        t = builtin_integer_type("longint");
        break;
      }
    }
    return t;
  };
  const TypeExpr* a_under = strip_wrap(a);
  const TypeExpr* p_under = strip_wrap(p);
  // 2. Equal modulo distinct/subrange wrappers. Distinct types still lower to
  // the same underlying storage for overload ranking, and subranges adopt
  // their base integer type here.
  if (a_under && p_under && type_text(a_under) == type_text(p_under)) {
    return {ConvRank::Equal, 0};
  }

  if (var_param) {
    // `var`/`out` params only accept identity/equal-or-wrapper matches, plus
    // class-hierarchy aliasing for reference types. Anything else would pass a
    // temporary or layout-incompatible slot by reference.
    if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
      const auto& an = static_cast<const TyName&>(*a).name;
      const auto& pn = static_cast<const TyName&>(*p).name;
      const auto* aci = analysis_.class_info_for_type_name(an);
      const auto* pci = analysis_.class_info_for_type_name(pn);
      if (aci && pci) {
        std::unordered_set<std::string> seen;
        const ClassInfo* cur = aci;
        int depth = 0;
        while (cur) {
          const std::string identity = cur->defining_unit + "." + cur->name;
          if (seen.count(identity)) break;
          if (cur->name == pci->name &&
              cur->defining_unit == pci->defining_unit) {
            return {ConvRank::ClassHierarchy, depth};
          }
          seen.insert(identity);
          cur = cur->parent.empty()
                    ? nullptr
                    : registry_->lookup_class(cur->parent, cur->defining_unit);
          ++depth;
        }
      }
    }
    return {};
  }

  if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
    const auto* aci =
        analysis_.class_info_for_type_name(static_cast<const TyName&>(*a).name);
    const auto* pci =
        analysis_.class_info_for_type_name(static_cast<const TyName&>(*p).name);
    // 3. Class hierarchy: a derived class may pass where an ancestor is
    // expected. Fewer parent hops means the closer Pascal match.
    if (aci && pci) {
      std::unordered_set<std::string> seen;
      const ClassInfo* cur = aci;
      int depth = 0;
      while (cur) {
        const std::string identity = cur->defining_unit + "." + cur->name;
        if (seen.count(identity)) break;
        if (cur->name == pci->name &&
            cur->defining_unit == pci->defining_unit) {
          return {ConvRank::ClassHierarchy, depth};
        }
        seen.insert(identity);
        cur = cur->parent.empty()
                  ? nullptr
                  : registry_->lookup_class(cur->parent, cur->defining_unit);
        ++depth;
      }
    }
  }

  auto prim_of = [&](const TypeExpr* t) -> const PrimitiveInfo* {
    if (!t || t->kind != Kind::TyName) return nullptr;
    return primitive_info(ascii_lower(static_cast<const TyName&>(*t).name));
  };

  // 4. Integer widening with unchanged signedness. Prefer the smallest target
  // that still contains the source by using the bit-width gap as distance.
  if (const auto* ai = prim_of(a); ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = prim_of(p);
        pi && pi->int_kind == ai->int_kind && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::IntWideningSameSign,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  auto real_rank = [](std::string_view name) -> int {
    if (name == "single") return 1;
    if (name == "double" || name == "real") return 2;
    if (name == "extended" || name == "comp") return 3;
    return 0;
  };
  // 5. Real widening follows Pascal's precision ladder. Again, smaller rank
  // gaps are better fits.
  if (a->kind == Kind::TyName && p->kind == Kind::TyName) {
    int ar = real_rank(ascii_lower(static_cast<const TyName&>(*a).name));
    int pr = real_rank(ascii_lower(static_cast<const TyName&>(*p).name));
    if (ar > 0 && pr > 0 && pr >= ar) return {ConvRank::RealWidening, pr - ar};
  }

  auto is_shortstring_param = [&](const TypeExpr* t) {
    if (!t) return false;
    if (t->kind == Kind::TyString) return true;
    if (t->kind != Kind::TyName) return false;
    const auto& n = ascii_lower(static_cast<const TyName&>(*t).name);
    return n == "shortstring";
  };
  auto is_ansistring = [&](const TypeExpr* t) {
    return t && t->kind == Kind::TyName &&
           ascii_lower(static_cast<const TyName&>(*t).name) == "ansistring";
  };
  auto is_char = [&](const TypeExpr* t) {
    return t && t->kind == Kind::TyName &&
           ascii_lower(static_cast<const TyName&>(*t).name) == "char";
  };

  // 6. Same-family ShortString widening.
  if (is_shortstring_param(a) && is_shortstring_param(p)) {
    return {ConvRank::StringSameTagWiden, 0};
  }

  const bool param_is_shortstring = is_shortstring_param(p);
  const bool param_is_ansistring = is_ansistring(p);
  const bool arg_is_shortstring = is_shortstring_param(a);
  const bool arg_is_ansistring = is_ansistring(a);
  // 7-8. Cross-family string conversions stay split because under `{$H-}`
  // Pascal prefers ShortString-targeted overloads over AnsiString-targeted
  // ones when both otherwise accept the same source.
  if (arg_is_ansistring && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (is_char(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (type_ops_.type_is_pcharish(a) && param_is_shortstring) {
    return {ConvRank::StringToShortString, 0};
  }
  if (arg_is_shortstring && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (is_char(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if (type_ops_.type_is_pcharish(a) && param_is_ansistring) {
    return {ConvRank::StringToAnsiString, 0};
  }
  if ((arg_is_shortstring || arg_is_ansistring) && type_ops_.type_is_pcharish(p)) {
    return {ConvRank::StringToAnsiString, 0};
  }

  // 9. Signedness change with sufficient width.
  if (const auto* ai = prim_of(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    if (const auto* pi = prim_of(p);
        pi && pi->int_kind != PrimitiveIntKind::None && pi->bits >= ai->bits &&
        pi->bits != 0 && ai->bits != 0) {
      return {ConvRank::OrdinalSignChange,
              static_cast<int>(pi->bits) - static_cast<int>(ai->bits)};
    }
  }

  if (const auto* ai = prim_of(a);
      ai && ai->int_kind != PrimitiveIntKind::None) {
    // 10. Integer narrowing. Pascal still permits this for value/const params
    // with range checking; the picker must rank it as viable but worse than
    // widening or sign-preserving matches.
    if (const auto* pi = prim_of(p);
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
  return score_conversion(analysis_.deduce_type(arg), param.type,
                          param.mutable_ref,
                          allow_assignment_operator_conversions);
}

PickResult EmitResolution::pick_overload(
    const std::vector<const ProcDecl*>& candidates,
    const std::vector<const Expr*>& args,
    bool allow_assignment_operator_conversions) {
  if (candidates.empty()) return {};

  struct Scored {
    const ProcDecl* decl;
    std::vector<ConvScore> scores;
  };
  std::vector<Scored> viable;
  for (const ProcDecl* decl : candidates) {
    if (!decl) continue;
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(decl, flat);
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

    Scored s{decl, {}};
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

  auto score_less = [](const ConvScore& a, const ConvScore& b) {
    // Lexicographic compare on (rank, distance): better rank wins; inside one
    // rank, the smaller distance is the tighter Pascal fit.
    if (a.rank != b.rank) return a.rank < b.rank;
    return a.distance < b.distance;
  };
  auto score_greater = [&](const ConvScore& a, const ConvScore& b) {
    return score_less(b, a);
  };
  auto dominates = [&](const Scored& a, const Scored& b) {
    // A candidate dominates another iff it is no worse at every explicit arg
    // position and strictly better at least once. Incomparable overload
    // candidates stay ambiguous.
    size_t n = std::min(a.scores.size(), b.scores.size());
    bool any_strict = false;
    for (size_t i = 0; i < n; ++i) {
      if (score_greater(a.scores[i], b.scores[i])) return false;
      if (score_less(a.scores[i], b.scores[i])) any_strict = true;
    }
    return any_strict;
  };
  size_t best = 0;
  for (size_t i = 1; i < viable.size(); ++i) {
    if (dominates(viable[i], viable[best])) best = i;
  }
  for (size_t i = 0; i < viable.size(); ++i) {
    if (i == best) continue;
    if (!dominates(viable[best], viable[i])) {
      // No strict winner: keep this as a Pascal ambiguity instead of silently
      // letting C++ overload resolution pick whichever conversion sequence it
      // prefers.
      return {nullptr, true};
    }
  }
  return {viable[best].decl, false};
}

ResolvedCall EmitResolution::resolve_call(
    const Expr& callee, const std::vector<const Expr*>& args) {
  ResolvedCall out;
  if (callee.kind == Kind::Ident) {
    out.member_name = static_cast<const Ident&>(callee).name;
  } else if (callee.kind == Kind::Member) {
    out.member_name = static_cast<const Member&>(callee).name;
  }
  if (!registry_) {
    out.decl = resolve_call_decl(callee);
    return out;
  }
  // Method overloads and free-function overloads share the same picker.
  // Methods come from class-chain lookup; free functions come from the current
  // unit plus the visible uses chain. `inherited foo(...)` is still a separate
  // AST node kind here, but once candidate gathering is done the same dominance
  // rules decide the winner.
  std::vector<AnyCand> all_cands;
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    gather_callable_in_pascal_scope(id.name, all_cands);
  } else if (callee.kind == Kind::Member) {
    const auto& mem = static_cast<const Member&>(callee);
    auto receiver_class = [&](const Expr& c) -> std::string {
      if (c.kind != Kind::Member) return {};
      const auto& member = static_cast<const Member&>(c);
      if (member.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*member.base);
        if (id.name == "self") return scope_.current_class_name;
        if (!analysis_.identifier_is_shadowed_value(id.name) &&
            (registry_->has_class(id.name, scope_.current_unit_name) ||
             registry_->records.count(id.name))) {
          return id.name;
        }
        return analysis_.deduce_class_alias(*member.base);
      }
      return analysis_.deduce_class_alias(*member.base);
    };
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
          append_class_method_cands(parent, mem.name, all_cands);
        }
      }
      if (!inherited_call) {
        // `Unit.proc(args)` uses the same Member AST node kind as
        // `receiver.method(args)`. Ask analysis for the qualified-unit
        // classification so overload resolution does not duplicate the
        // shadowing/uses checks.
        if (auto unit_member = analysis_.resolve_unit_qualified_member(mem)) {
          unit_qualified = true;
          append_unit_export_proc_cands(unit_member->unit_name, mem.name,
                                        all_cands);
        }
      }
    }
    if (!inherited_call && !unit_qualified) {
      std::string cls = receiver_class(callee);
      if (!cls.empty()) append_class_method_cands(cls, mem.name, all_cands);
    }
  }

  std::vector<AnyCand> arity_ok;
  for (const auto& a : all_cands) {
    // Arity-filter first, including default-argument slack. Synthetic runtime
    // builtins do not carry AST ProcDecls, so they must still participate
    // here through the cached `param_count` / `accepts_zero_args` metadata;
    // otherwise a visible imported decl with the same name but wrong arity
    // can steal the final callee spelling.
    if (args.size() > a.param_count) continue;
    if (args.size() < a.param_count && !a.accepts_zero_args) {
      if (!a.decl) continue;
      std::vector<FlatCallParamInfo> flat;
      flatten_call_param_info(a.decl, flat);
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

  auto adopt = [&](const AnyCand& chosen, bool ran_type_picker) {
    // The chosen candidate's defining unit determines whether the printer must
    // force `<unit_ns>::name` spelling to stay aligned with Pascal lookup.
    out.decl = chosen.decl ? chosen.decl : resolve_call_decl(callee);
    out.needs_arg_casts = ran_type_picker;
    if (!chosen.unit.empty()) {
      out.callee_kind = ResolvedCalleeKind::FreeFunctionInUnit;
      out.defining_unit = chosen.unit;
    }
  };

  if (arity_ok.size() == 1) {
    // Single arity-viable candidate. C++ does not need help choosing the
    // overload, but we still record the defining unit so the printer spells
    // the callee through the same Pascal lookup path.
    adopt(arity_ok[0], false);
    return out;
  }

  if (arity_ok.size() > 1) {
    // Multiple arity-viable candidates: run the Pascal conversion-rank picker
    // on the decl-backed subset. We do not silently pick among tied
    // incomparables; ambiguity is surfaced to the caller as a Pascal error.
    std::vector<const ProcDecl*> with_decl;
    for (const auto& a : arity_ok) {
      if (a.decl) with_decl.push_back(a.decl);
    }
    if (with_decl.empty()) {
      out.decl = resolve_call_decl(callee);
      return out;
    }
    PickResult pr = pick_overload(
        with_decl, args, /*allow_assignment_operator_conversions=*/true);
    if (pr.ambiguous) {
      out.decl = nullptr;
      out.ambiguous = true;
      return out;
    }
    if (!pr.decl) {
      out.decl = resolve_call_decl(callee);
      return out;
    }
    for (const auto& a : arity_ok) {
      if (a.decl == pr.decl) {
        adopt(a, true);
        return out;
      }
    }
    // Defensive fallback: `pr.decl` came from the arity-filtered set, so the
    // loop above should have found it. Keep the decl anyway if metadata got
    // out of sync so later emit-time diagnostics stay anchored.
    out.decl = pr.decl;
    out.needs_arg_casts = true;
    return out;
  }

  out.decl = resolve_call_decl(callee);
  return out;
}

BinaryOperatorResult EmitResolution::find_binary_operator(
    const std::string& op, const Expr& lhs, const Expr& rhs) {
  if (!registry_) return {};
  auto operand_allows_operator_lookup = [&](const TypeExpr* t) {
    t = analysis_.canonicalize_type(t);
    if (!t) return false;
    if (t->kind == Kind::TyName) {
      const auto& name = ascii_lower(static_cast<const TyName&>(*t).name);
      if (primitive_info(name)) return false;
      if (registry_->enums.count(name)) return false;
      return registry_->records.count(name) ||
             registry_->has_class(name, scope_.current_unit_name) ||
             registry_->aliases.count(name);
    }
    return t->kind == Kind::TyRecord || t->kind == Kind::TyObject ||
           t->kind == Kind::TyInterface || t->kind == Kind::TyPointer ||
           t->kind == Kind::TyArray || t->kind == Kind::TyMetaclass;
  };

  const TypeExpr* lhs_type = analysis_.deduce_type(lhs);
  const TypeExpr* rhs_type = analysis_.deduce_type(rhs);
  const bool overloadable_context =
      operand_allows_operator_lookup(lhs_type) ||
      operand_allows_operator_lookup(rhs_type);
  if (!overloadable_context) return {};

  std::vector<AnyCand> cands;
  gather_operator_in_pascal_scope(op, cands);
  if (cands.empty()) return {};

  std::vector<const ProcDecl*> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(c.decl, flat);
    if (flat.size() == 2) arity_ok.push_back(c.decl);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&lhs, &rhs};
  PickResult pr = pick_overload(
      arity_ok, args, /*allow_assignment_operator_conversions=*/true);
  if (pr.ambiguous) return {nullptr, {}, true};
  if (!pr.decl) return {};
  for (const auto& c : cands) {
    if (c.decl == pr.decl) return {pr.decl, c.unit, false};
  }
  return {pr.decl, {}, false};
}

UnaryOperatorResult EmitResolution::find_unary_operator(
    const std::string& op, const Expr& operand) {
  if (!registry_) return {};
  std::vector<AnyCand> cands;
  gather_operator_in_pascal_scope(op, cands);
  if (cands.empty()) return {};

  std::vector<const ProcDecl*> arity_ok;
  for (const auto& c : cands) {
    if (!c.decl) continue;
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(c.decl, flat);
    if (flat.size() == 1) arity_ok.push_back(c.decl);
  }
  if (arity_ok.empty()) return {};

  std::vector<const Expr*> args{&operand};
  PickResult pr = pick_overload(
      arity_ok, args, /*allow_assignment_operator_conversions=*/true);
  if (pr.ambiguous) return {nullptr, {}, true};
  if (!pr.decl) return {};
  for (const auto& c : cands) {
    if (c.decl == pr.decl) return {pr.decl, c.unit, false};
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

  std::vector<AnyCand> cands;
  gather_operator_in_pascal_scope(":=", cands);
  std::vector<const ProcDecl*> viable;
  for (const auto& c : cands) {
    const ProcDecl* pd = c.decl;
    if (!pd || pd->params.size() != 1 || !pd->return_type) continue;
    const TypeExpr* ret = analysis_.canonicalize_type(pd->return_type.get());
    if (!ret || type_ops_.type_to_cxx(*ret) != target_cxx) continue;
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(pd, flat);
    if (flat.size() != 1) continue;
    if (rank_conversion(source, flat[0].type, flat[0].mutable_ref).viable()) {
      viable.push_back(pd);
    }
  }
  if (viable.empty()) return {};
  if (viable.size() == 1) {
    for (const auto& c : cands) {
      if (c.decl == viable.front()) return {viable.front(), c.unit};
    }
    return {viable.front(), {}};
  }

  // Reuse the normal picker by synthesizing the source type as an expression
  // is not possible here, so choose only exact/equal-ranked matches. This is
  // enough for the FPC compiler's explicit one-parameter conversion operators
  // and avoids guessing among lossy numeric conversions.
  const ProcDecl* best = nullptr;
  std::string best_unit;
  ConvScore best_score{};
  bool have_best = false;
  bool ambiguous = false;
  for (const ProcDecl* pd : viable) {
    std::vector<FlatCallParamInfo> flat;
    flatten_call_param_info(pd, flat);
    ConvScore score = rank_conversion(source, flat[0].type, flat[0].mutable_ref);
    if (!score.viable()) continue;
    if (!have_best || score.rank < best_score.rank ||
        (score.rank == best_score.rank && score.distance < best_score.distance)) {
      best = pd;
      for (const auto& c : cands) {
        if (c.decl == pd) {
          best_unit = c.unit;
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
