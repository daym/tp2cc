#include "emit_decls.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "emit_analysis.h"
#include "emit_signature_scope.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "emit_values.h"

namespace tp2cc {

using namespace ast;

namespace {

std::string nested_proc_cxx_name(const ScopeStateView& scope,
                                 const ProcDecl& pd) {
  auto it = scope.local_nested_fns.find(pd.name);
  if (it != scope.local_nested_fns.end()) {
    for (const auto& overload : it->second) {
      if (overload.decl == &pd) return overload.cxx_name;
    }
  }
  return mangle(pd.name);
}

std::string type_symbol_source_name(const TypeSymbol& symbol) {
  std::string out;
  for (const auto& owner : symbol.owner_path) {
    if (!out.empty()) out += ".";
    out += owner;
  }
  if (!out.empty()) out += ".";
  out += symbol.name;
  return out;
}

bool has_builtin_metaclass_descriptor_slot(std::string_view name) {
  const std::string low = ascii_lower(std::string(name));
  return low == "classname" || low == "classtype" ||
         low == "inheritsfrom" || low == "instancesize";
}

class ScopedTypeExprContext {
 public:
  ScopedTypeExprContext(ScopeStateView& scope, const TypeRegistry& registry,
                        const TypeExpr* type)
      : scope_(scope), saved_type_scope_(scope.type_scope) {
    if (!type) return;
    const TypeLookupContext* context = registry.lookup_context_for_type(type);
    if (!context) return;
    scope_.type_scope = context;
    active_ = true;
  }

  ScopedTypeExprContext(const ScopedTypeExprContext&) = delete;
  ScopedTypeExprContext& operator=(const ScopedTypeExprContext&) = delete;

  ~ScopedTypeExprContext() {
    if (active_) scope_.type_scope = saved_type_scope_;
  }

 private:
  ScopeStateView& scope_;
  const TypeLookupContext* saved_type_scope_ = nullptr;
  bool active_ = false;
};

}  // namespace

EmitDecls::EmitDecls(const TypeRegistry& registry, ScopeStateView& scope,
                     EmitAnalysis& analysis,
                     EmitTypes& types, EmitStorage& storage,
                     EmitValues& values, EmitDeclOps& emit_ops)
    : registry_(registry),
      scope_(scope),
      analysis_(analysis),
      types_(types),
      storage_(storage),
      values_(values),
      emit_ops_(emit_ops) {}

std::string EmitDecls::cxx_access_for_pascal_visibility(Visibility vis) {
  switch (vis) {
    case Visibility::StrictPrivate:
      return "private";
    case Visibility::StrictProtected:
      return "protected";
    case Visibility::Public:
    case Visibility::Private:
    case Visibility::Protected:
      // Non-strict Pascal private/protected members remain public in C++:
      // Pascal still permits same-unit access, and those accesses are emitted
      // as ordinary namespace-level C++ code.
      return "public";
  }
  return "public";
}

void EmitDecls::emit_enum_carrier(const TyEnum& te, std::string_view cxx_name,
                                  std::string_view bound_name) {
  emit_ops_.emitln("enum " + std::string(cxx_name) + " : " +
                   types_.enum_underlying_type_to_cxx(te) + " {");
  emit_ops_.indent();
  auto members = types_.enum_members_to_cxx(te);
  for (size_t i = 0; i < members.size(); ++i) {
    std::string m = members[i];
    if (i + 1 < members.size()) m += ",";
    emit_ops_.emitln(m);
  }
  emit_ops_.dedent();
  emit_ops_.emitln("};");
  if (!bound_name.empty() && !te.members.empty()) {
    const char* lin = emit_ops_.in_block_scope() ? "" : "inline ";
    emit_ops_.emitln(std::string(lin) + "constexpr " +
                     std::string(cxx_name) + " " +
                     enum_bound_name(bound_name, "low") + " = " +
                     mangle(te.members.front().name) + ";");
    emit_ops_.emitln(std::string(lin) + "constexpr " +
                     std::string(cxx_name) + " " +
                     enum_bound_name(bound_name, "high") + " = " +
                     mangle(te.members.back().name) + ";");
  }
}

void EmitDecls::emit_enum_carrier_decls(const TypeExpr* t,
                                        const TyEnum* skip) {
  if (!t) return;
  std::vector<const TyEnum*> enums = collect_enum_types(*t);
  std::unordered_set<const TyEnum*> emitted;
  for (const TyEnum* te : enums) {
    if (!te || te == skip || !emitted.insert(te).second) continue;
    auto cxx_name = types_.enum_carrier_type_to_cxx(*te);
    // Imported enum carriers are declared by the included unit header; this
    // declaration site only owns carriers whose name is local to the scope.
    if (!cxx_name || cxx_name->find("::") != std::string::npos) continue;
    emit_enum_carrier(*te, *cxx_name, {});
  }
}

bool EmitDecls::should_emit_var_type_helpers(const VarDecl& vd,
                                             bool in_header) {
  if (in_header || emit_ops_.in_block_scope()) return true;
  auto uit = registry_.units.find(scope_.current_unit_name);
  if (uit == registry_.units.end()) return true;
  for (const auto& n : vd.names) {
    if (!uit->second.iface_vars.count(ascii_lower(n))) return true;
  }
  return false;
}

void EmitDecls::emit_packed_record_asserts(
    const std::string& type_text,
    const std::vector<std::pair<std::string, std::string>>& field_offsets,
    std::string_view size_expr, std::string_view label) {
  for (const auto& [field_name, offset_expr] : field_offsets) {
    emit_ops_.emitln("static_assert(offsetof(" + type_text + ", " + field_name +
                     ") == " + offset_expr + ", "
                     "\"packed record '" + std::string(label) +
                     "' must place field '" + field_name +
                     "' at the exact Pascal byte offset with no inserted "
                     "padding.\");");
  }
  emit_ops_.emitln("static_assert(sizeof(" + type_text + ") == " +
                   std::string(size_expr) + ", \"packed record '" +
                   std::string(label) +
                   "' must have the exact packed Pascal size.\");");
}

std::vector<EmitDecls::MetaclassCallable> EmitDecls::collect_metaclass_callables(
    const ClassInfo& root_info) {
  std::vector<MetaclassCallable> out;
  std::vector<const ClassInfo*> chain;
  std::unordered_set<std::string> seen;
  const ClassInfo* root = &root_info;
  const ClassInfo* ci = root;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    chain.push_back(ci);
    ci = registry_.lookup_parent_class(*ci);
  }
  std::reverse(chain.begin(), chain.end());

  std::unordered_map<std::string, size_t> pos;
  for (const ClassInfo* current : chain) {
    std::vector<std::string> names;
    names.reserve(current->methods.size());
    // Pascal does not let overloads of one name disagree on
    // Constructor/ClassMethod-ness, so the representative overload's kind
    // classifies the whole slot for the metaclass-callable test.
    for (const auto& [name, sigs] : current->methods) {
      const MethodSig* sig = ::tp2cc::representative_method(sigs);
      if (!sig) continue;
      if (sig->decl && sig->decl->is_class_method &&
          (sig->decl->pkind == ProcKind::Constructor ||
           sig->decl->pkind == ProcKind::Destructor)) {
        // The metaclass method table contains callable constructor/class-method
        // entries; class lifecycle methods run from unit init/fini emission.
        continue;
      }
      if (sig->kind == SymKind::Constructor ||
          sig->kind == SymKind::ClassMethod) {
        if (has_builtin_metaclass_descriptor_slot(name)) continue;
        names.push_back(name);
      }
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
      const MethodSig* sig =
          ::tp2cc::representative_method(current->methods.at(name));
      auto pit = pos.find(name);
      if (pit == pos.end()) {
        pos[name] = out.size();
        out.push_back({name, sig, false});
      } else {
        out[pit->second].sig = sig;
        out[pit->second].implicit_root_create = false;
      }
    }
  }

  if (root && root->is_reference_type && !pos.count("create")) {
    // Every Delphi/FPC class inherits `TObject.Create` even if the source
    // does not redeclare a constructor. Class-value calls like
    // `TDLLScannerClass(x).Create` therefore still need a zero-arg metaclass
    // entry instead of falling out as an unsupported member.
    out.push_back({"create", nullptr, true});
  }
  return out;
}

std::optional<EmitDecls::MetaclassCallableImpl>
EmitDecls::find_metaclass_callable_impl(const ClassInfo& concrete_class,
                                        const MetaclassCallable& target) {
  std::unordered_set<std::string> seen;
  const ClassInfo* ci = &concrete_class;
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    auto mit = ci->methods.find(target.name);
    if (mit != ci->methods.end()) {
      for (const auto& sig : mit->second) {
        if (metaclass_callable_matches_impl(target, sig)) {
          const std::string owner =
              sig.declaring_type.empty() ? concrete_class.name
                                         : sig.declaring_type;
          return MetaclassCallableImpl{owner, &sig, false};
        }
      }
    }
    ci = registry_.lookup_parent_class(*ci);
  }

  if (target.implicit_root_create) {
    // `TLinkedListItemClass(x).Create` and similar base-typed class refs still
    // inherit `TObject.Create` even when a derived class also declares
    // `Create(...)` with a different signature. Keep that zero-arg slot alive
    // through the concrete metaclass instead of treating the derived
    // declaration as if it erased the inherited constructor.
    return MetaclassCallableImpl{"tobject", nullptr, true};
  }
  return std::nullopt;
}

bool EmitDecls::metaclass_callable_matches_impl(
    const MetaclassCallable& target, const MethodSig& candidate) {
  if (!candidate.decl) return false;
  if (target.implicit_root_create) {
    return candidate.kind == SymKind::Constructor && candidate.param_count == 0;
  }
  if (!target.sig || !target.sig->decl) return false;
  return candidate.kind == target.sig->kind &&
         method_sig_param_types(candidate) ==
             method_sig_param_types(*target.sig);
}

std::string EmitDecls::method_sig_param_types(const MethodSig& sig) {
  if (!sig.decl) return {};
  ScopedSignatureLookupUnit lookup_unit(scope_, registry_, &sig);
  return types_.formal_param_types_to_cxx(sig.decl->params);
}

std::string EmitDecls::method_sig_param_list(const MethodSig& sig) {
  if (!sig.decl) return {};
  ScopedSignatureLookupUnit lookup_unit(scope_, registry_, &sig);
  return param_list_to_cxx(sig.decl->params);
}

std::string EmitDecls::metaclass_callable_param_types(
    const MetaclassCallable& callable) {
  if (callable.implicit_root_create || !callable.sig) return {};
  return method_sig_param_types(*callable.sig);
}

bool EmitDecls::same_metaclass_callable_surface(
    const MetaclassCallable& lhs, const MetaclassCallable& rhs) {
  if (lhs.name != rhs.name) return false;
  if (lhs.implicit_root_create || rhs.implicit_root_create) {
    return lhs.implicit_root_create == rhs.implicit_root_create;
  }
  if (!lhs.sig || !rhs.sig) return lhs.sig == rhs.sig;
  return lhs.sig->kind == rhs.sig->kind &&
         metaclass_callable_param_types(lhs) ==
             metaclass_callable_param_types(rhs);
}

bool EmitDecls::is_virtual_metaclass_callable(
    const MetaclassCallable& callable) {
  if (callable.implicit_root_create || !callable.sig || !callable.sig->decl) {
    return false;
  }
  const auto& pd = *callable.sig->decl;
  return callable.sig->kind == SymKind::ClassMethod &&
         (pd.modifiers.is_virtual || pd.modifiers.is_abstract ||
          pd.modifiers.is_override);
}

bool EmitDecls::has_same_parent_metaclass_slot(
    const MetaclassCallable& callable,
    const std::vector<MetaclassCallable>& parent_callables) {
  for (const auto& parent_callable : parent_callables) {
    if (same_metaclass_callable_surface(callable, parent_callable)) {
      return true;
    }
  }
  return false;
}

std::vector<EmitDecls::MetaclassCallable> EmitDecls::own_metaclass_callables(
    const std::vector<MetaclassCallable>& visible_callables,
    const std::vector<MetaclassCallable>& parent_callables) {
  std::vector<MetaclassCallable> out;
  for (const auto& callable : visible_callables) {
    const bool same_as_parent =
        has_same_parent_metaclass_slot(callable, parent_callables);
    if (!same_as_parent || is_virtual_metaclass_callable(callable)) {
      out.push_back(callable);
      continue;
    }
    if (callable.implicit_root_create ||
        (callable.sig && callable.sig->kind == SymKind::Constructor)) {
      // Constructor slots include the allocated class in their function-pointer
      // return type, so a derived metaclass needs its own slot even when the
      // Pascal parameter list matches the parent constructor.
      out.push_back(callable);
    }
  }
  return out;
}

std::string EmitDecls::metaclass_callable_return_type(
    std::string_view target_class, const MetaclassCallable& callable) {
  if (callable.implicit_root_create ||
      (callable.sig && callable.sig->kind == SymKind::Constructor)) {
    return types_.named_type_struct_cxx(target_class) + "*";
  }
  ScopedSignatureLookupUnit lookup_unit(scope_, registry_, callable.sig);
  const auto& pd = *callable.sig->decl;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    return types_.type_to_cxx(*pd.return_type);
  }
  return "void";
}

std::string EmitDecls::metaclass_callable_return_type(
    const ClassInfo& target_info, const MetaclassCallable& callable) {
  return metaclass_callable_return_type(target_info, target_info.name, callable);
}

std::string EmitDecls::metaclass_callable_return_type(
    const ClassInfo& target_info, std::string_view target_class,
    const MetaclassCallable& callable) {
  if (callable.implicit_root_create ||
      (callable.sig && callable.sig->kind == SymKind::Constructor)) {
    return class_struct_cxx(target_info, target_class) + "*";
  }
  ScopedSignatureLookupUnit lookup_unit(scope_, registry_, callable.sig);
  const auto& pd = *callable.sig->decl;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    return types_.type_to_cxx(*pd.return_type);
  }
  return "void";
}

std::string EmitDecls::metaclass_callable_param_list(
    const MetaclassCallable& callable) {
  if (callable.implicit_root_create || !callable.sig) return {};
  return method_sig_param_list(*callable.sig);
}

std::string EmitDecls::metaclass_callable_arg_list(
    const MetaclassCallable& callable) {
  if (callable.implicit_root_create || !callable.sig) return {};
  std::string out;
  bool first = true;
  size_t unnamed_index = 0;
  for (const auto& par : callable.sig->decl->params) {
    if (par.names.empty()) {
      if (!first) out += ", ";
      out += "tp2cc_arg" + std::to_string(unnamed_index++);
      first = false;
      continue;
    }
    for (const auto& pn : par.names) {
      if (!first) out += ", ";
      out += mangle(pn);
      first = false;
    }
  }
  return out;
}

std::string EmitDecls::metaclass_callable_ctor_param(
    std::string_view target_class, const MetaclassCallable& callable) {
  return metaclass_callable_return_type(target_class, callable) +
         " (*tp2cc_" + mangle(callable.name) + ")(" +
         metaclass_callable_param_types(callable) + ")";
}

std::string EmitDecls::metaclass_callable_ctor_param(
    const ClassInfo& target_info, std::string_view target_class,
    const MetaclassCallable& callable) {
  return metaclass_callable_return_type(target_info, target_class, callable) +
         " (*tp2cc_" + mangle(callable.name) + ")(" +
         metaclass_callable_param_types(callable) + ")";
}

std::string EmitDecls::metaclass_callable_ctor_init(
    const MetaclassCallable& callable) {
  return mangle(callable.name) + "(tp2cc_" + mangle(callable.name) + ")";
}

void EmitDecls::emit_virtual_metaclass_callable(
    std::string_view owner_class, const MetaclassCallable& callable,
    bool has_same_parent_slot) {
  const auto& pd = *callable.sig->decl;
  const std::string ret = metaclass_callable_return_type(owner_class, callable);
  std::string decl = proc_attributes_to_cxx(pd) + "virtual " + ret + " " +
                     mangle(callable.name) + "(" +
                     metaclass_callable_param_list(callable) + ") const";
  if (has_same_parent_slot) decl += " override";
  decl += " { ";
  if (pd.modifiers.is_abstract && !pd.body) {
    decl += "::std::abort();";
    if (ret != "void") decl += " return {};";
  } else {
    if (ret != "void") decl += "return ";
    decl += types_.named_type_struct_cxx(owner_class) + "::" +
            mangle(callable.name) + "(" +
            metaclass_callable_arg_list(callable) + ");";
  }
  decl += " }";
  emit_ops_.emitln(decl);
}

void EmitDecls::emit_virtual_metaclass_callable(
    const ClassInfo& owner_info, std::string_view owner_class,
    const MetaclassCallable& callable, bool has_same_parent_slot) {
  const auto& pd = *callable.sig->decl;
  const std::string ret =
      metaclass_callable_return_type(owner_info, owner_class, callable);
  std::string decl = proc_attributes_to_cxx(pd) + "virtual " + ret + " " +
                     mangle(callable.name) + "(" +
                     metaclass_callable_param_list(callable) + ") const";
  if (has_same_parent_slot) decl += " override";
  decl += " { ";
  if (pd.modifiers.is_abstract && !pd.body) {
    decl += "::std::abort();";
    if (ret != "void") decl += " return {};";
  } else {
    if (ret != "void") decl += "return ";
    decl += class_struct_cxx(owner_info, owner_class) + "::" +
            mangle(callable.name) + "(" +
            metaclass_callable_arg_list(callable) + ");";
  }
  decl += " }";
  emit_ops_.emitln(decl);
}

const TypeSymbol* EmitDecls::class_symbol(const ClassInfo& info) const {
  const TypeSymbol* symbol =
      registry_.lookup_type_symbol_exact(info.defining_unit, info.name);
  return descriptor_payload_symbol(symbol);
}

const TypeSymbol* EmitDecls::class_symbol(
    const ClassInfo& info, std::string_view source_name) const {
  if (!source_name.empty()) {
    if (const TypeSymbol* symbol =
            descriptor_payload_symbol(registry_.lookup_type_symbol_exact(
                info.defining_unit, source_name))) {
      return symbol;
    }
  }
  return class_symbol(info);
}

std::string EmitDecls::class_struct_cxx(const ClassInfo& info) const {
  return class_struct_cxx(info, info.name);
}

std::string EmitDecls::class_struct_cxx(
    const ClassInfo& info, std::string_view fallback_name) const {
  if (const TypeSymbol* symbol = class_symbol(info, fallback_name)) {
    return types_.type_symbol_struct_cxx(*symbol);
  }
  return types_.named_type_struct_cxx(fallback_name);
}

std::string EmitDecls::class_struct_cxx(std::string_view class_name,
                                        const MethodSig* sig) const {
  if (sig && !sig->defining_unit.empty()) {
    const std::string owner =
        sig->declaring_type.empty() ? ascii_lower(class_name)
                                    : sig->declaring_type;
    if (const TypeSymbol* symbol =
            descriptor_payload_symbol(registry_.lookup_type_symbol_exact(
                sig->defining_unit, owner))) {
      return types_.type_symbol_struct_cxx(*symbol);
    }
  }
  if (ascii_lower(class_name) == "tobject") {
    if (const TypeSymbol* symbol =
            descriptor_payload_symbol(
                registry_.lookup_type_symbol_exact("__rt__", "tobject"))) {
      return types_.type_symbol_struct_cxx(*symbol);
    }
  }
  return types_.named_type_struct_cxx(class_name);
}

std::string EmitDecls::metaclass_struct_cxx(const ClassInfo& info) const {
  return metaclass_struct_cxx(info, info.name);
}

std::string EmitDecls::metaclass_struct_cxx(
    const ClassInfo& info, std::string_view fallback_name) const {
  if (const TypeSymbol* symbol = class_symbol(info, fallback_name)) {
    return types_.metaclass_struct_cxx(*symbol);
  }
  return types_.metaclass_struct_cxx(fallback_name);
}

std::string EmitDecls::metaclass_value_fn_cxx(const ClassInfo& info) const {
  return metaclass_value_fn_cxx(info, info.name);
}

std::string EmitDecls::metaclass_value_fn_cxx(
    const ClassInfo& info, std::string_view fallback_name) const {
  if (const TypeSymbol* symbol = class_symbol(info, fallback_name)) {
    return types_.metaclass_value_fn_cxx(*symbol);
  }
  return types_.metaclass_value_fn_cxx(fallback_name);
}

std::string EmitDecls::build_metaclass_ctor_expr(
    const ClassInfo& target_info, std::string_view concrete_class) {
  return build_metaclass_ctor_expr(target_info, target_info.name,
                                   target_info, concrete_class);
}

std::string EmitDecls::build_metaclass_ctor_expr(
    const ClassInfo& target_info, std::string_view target_class,
    std::string_view concrete_class) {
  return build_metaclass_ctor_expr(target_info, target_class, target_info,
                                   concrete_class);
}

std::string EmitDecls::build_metaclass_ctor_expr(
    const ClassInfo& target_info, std::string_view target_class,
    const ClassInfo& concrete_info, std::string_view concrete_class) {
  const auto target_callables = collect_metaclass_callables(target_info);
  const ClassInfo* parent_info =
      registry_.lookup_parent_class(target_info);
  const bool has_parent = parent_info && parent_info->is_reference_type;
  const auto parent_visible =
      has_parent ? collect_metaclass_callables(*parent_info)
                 : std::vector<MetaclassCallable>{};

  // A metaclass value stores the implementations visible on the concrete class,
  // but its C++ constructor arguments must match the target class's slot types.
  std::string out = metaclass_struct_cxx(target_info, target_class) + "(";
  bool first = true;
  if (has_parent && !parent_visible.empty()) {
    out += build_metaclass_ctor_expr(*parent_info, parent_info->name,
                                     concrete_info, concrete_class);
    first = false;
  }
  for (const auto& callable : target_callables) {
    if (is_virtual_metaclass_callable(callable)) continue;
    if (has_same_parent_metaclass_slot(callable, parent_visible) &&
        !(callable.implicit_root_create ||
          (callable.sig && callable.sig->kind == SymKind::Constructor))) {
      continue;
    }
    if (!first) out += ", ";
    const auto concrete_impl =
        find_metaclass_callable_impl(concrete_info, callable);
    if (!concrete_impl) {
      const std::string ret =
          metaclass_callable_return_type(target_info, target_class, callable);
      out += "+[](" + metaclass_callable_param_list(callable) + ") -> " + ret +
             " { ::std::abort();";
      if (ret != "void") out += " return {};";
      out += " }";
      first = false;
      continue;
    }
    const bool use_implicit_root_create = concrete_impl->implicit_root_create;
    const auto* concrete_sig = concrete_impl->sig;
    if (use_implicit_root_create ||
        (concrete_sig && concrete_sig->kind == SymKind::Constructor)) {
      out += "+[](" + metaclass_callable_param_list(callable) + ") -> " +
             metaclass_callable_return_type(target_info, target_class,
                                            callable) +
             " { auto tp2cc_ptr = new " +
             class_struct_cxx(concrete_info, concrete_class) + "{}; ";
      const std::string ctor_name =
          use_implicit_root_create ? "p_create" : mangle(callable.name);
      if (ascii_lower(concrete_impl->owner_class) ==
          ascii_lower(std::string(concrete_class))) {
        out += "tp2cc_ptr->" + ctor_name + "(" +
               metaclass_callable_arg_list(callable) + ")";
      } else {
        out += "static_cast<" +
               class_struct_cxx(concrete_impl->owner_class, concrete_sig) +
               "*>(tp2cc_ptr)->" + ctor_name + "(" +
               metaclass_callable_arg_list(callable) + ")";
      }
      out += "; return tp2cc_ptr; }";
    } else {
      std::string ret =
          metaclass_callable_return_type(target_info, target_class, callable);
      out += "+[](" + metaclass_callable_param_list(callable) + ") -> " + ret +
             " { ";
      if (ret != "void") out += "return ";
      out += class_struct_cxx(concrete_impl->owner_class, concrete_sig) + "::" +
             mangle(callable.name) + "(" +
             metaclass_callable_arg_list(callable) + ");";
      out += " }";
    }
    first = false;
  }
  out += ")";
  return out;
}

void EmitDecls::emit_const_decl(const ConstDecl& cd, bool in_header) {
  (void)in_header;
  const std::string name = mangle(cd.name);
  emit_enum_carrier_decls(cd.type.get());
  std::string val = cd.type
                        ? values_.typed_const_value_to_cxx(*cd.value,
                                                           cd.type.get())
                        : values_.const_value_to_cxx(*cd.value);

  // Two things drive the qualifiers:
  //   `inline` -- required on definitions at namespace scope in a header so
  //              multiple translation units that include it don't violate
  //              ODR. Invalid at block scope, so we drop it there.
  //   `const`  -- Pascal's UNTYPED const (`const X = 5;`) is immutable,
  //              TYPED const (`const X : T = 5;`) is writable.
  const bool block = emit_ops_.in_block_scope();
  const std::string linkage = block ? std::string() : std::string("inline ");
  const TypeExpr* typed_const_ty =
      cd.type ? analysis_.semantic_shape_type(cd.type.get()) : nullptr;

  // Typed array (or named alias ultimately resolving to one) with an
  // array-constant initialiser emits an `rt::tp2cc_Array<T, Lo, N>` so
  //   (a) the size is known even when the index is an enum (Pascal),
  //   (b) the array has value-copy semantics on pass (Pascal),
  //   (c) `arr[Lo]` picks the first element (Pascal arbitrary low bound).
  if (cd.type && cd.value->kind == Kind::ArrayConst) {
    const TypeExpr* t = typed_const_ty;
    if (t && t->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*t);
      std::string ty =
          arr.element ? types_.type_to_cxx(*arr.element) : std::string("int32_t");
      for (auto it = arr.dims.rbegin(); it != arr.dims.rend(); ++it) {
        auto bounds = types_.array_dim_bounds_to_cxx(**it);
        if (!bounds) {
          goto generic_emit;
        }
        ty = "::rt::tp2cc_Array<" + ty + ", " + bounds->low + ", " +
             bounds->size_expr + ">";
      }
      emit_ops_.emitln(linkage + ty + " " + name + " = " + val + ";");
      return;
    }
  }
generic_emit:;

  if (cd.type) {
    emit_ops_.emitln(linkage + types_.type_to_cxx(*cd.type) + " " + name +
                     " = " + val + ";");
    return;
  }

  if (cd.value->kind == Kind::StringLit) {
    const auto& sl = static_cast<const StringLit&>(*cd.value);
    if (sl.value.size() == 1) {
      emit_ops_.emitln(linkage + "constexpr ::rt::CharConst " + name + "{" +
                       val + "};");
    } else {
      emit_ops_.emitln(linkage + "const ::rt::tp2cc_ShortString<> " + name +
                       " = " + val + ";");
    }
    return;
  }
  if (const TypeExpr* inferred_ty = analysis_.deduce_const_decl_type(cd)) {
    const TypeExpr* canon = analysis_.semantic_shape_type(inferred_ty);
    if (canon && canon->kind == Kind::TyName) {
      std::string nm = ascii_lower(static_cast<const TyName&>(*canon).name);
      if (auto* info = primitive_info(nm);
          info && info->int_kind != PrimitiveIntKind::None) {
        val = values_.const_value_to_cxx(*cd.value, inferred_ty);
        emit_ops_.emitln(linkage + "const " + types_.type_to_cxx(*inferred_ty) +
                         " " + name + " = " + val + ";");
        return;
      }
    }
  }
  emit_ops_.emitln(linkage + "const auto " + name + " = " + val + ";");
}

void EmitDecls::emit_type_decl(const TypeDecl& td, bool in_header) {
  std::vector<PendingReferenceClassSupport> pending_support;
  emit_type_decl_impl(td, in_header, pending_support);
  for (const auto& pending : pending_support) {
    emit_pending_reference_class_support(pending);
  }
}

void EmitDecls::emit_nested_type_decl(
    const TypeDecl& td,
    std::vector<PendingReferenceClassSupport>& pending_support) {
  emit_type_decl_impl(td, /*in_header=*/false, pending_support);
}

void EmitDecls::emit_type_decl_impl(const TypeDecl& td, bool in_header,
                                    std::vector<PendingReferenceClassSupport>&
                                        pending_support) {
  (void)in_header;
  ScopedTypeExprContext type_context(scope_, registry_, td.type.get());
  const std::string name = type_mangle(td.name);

  if (td.type && td.type->kind == Kind::TyEnum) {
    const auto& te = static_cast<const TyEnum&>(*td.type);
    emit_enum_carrier(te, name, td.name);
    return;
  }

  emit_enum_carrier_decls(td.type.get());

  if (td.type && td.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*td.type);
    const auto layout = types_.compute_record_layout(tr);
    std::string open = "struct ";
    if (tr.is_packed) open += "[[gnu::packed]] ";
    emit_ops_.emitln(open + name + " {");
    
    emit_ops_.indent();
    std::vector<PendingReferenceClassSupport> nested_pending;
    for (const auto& nested : tr.nested_types) {
      if (nested) emit_nested_type_decl(*nested, nested_pending);
    }
    for (const auto& line : layout.decl_lines) {
      if (line.find('}') != std::string::npos) emit_ops_.dedent();
      emit_ops_.emitln(line);
      if (line.find('{') != std::string::npos) emit_ops_.indent();
    }
    emit_ops_.dedent();
    
    emit_ops_.emitln("};");
    if (tr.is_packed) {
      emit_packed_record_asserts(name, layout.packed_layout.field_offsets, layout.packed_layout.size_expr,
                                 name);
    }
    pending_support.insert(pending_support.end(), nested_pending.begin(),
                           nested_pending.end());
    return;
  }

  if (td.type && td.type->kind == Kind::TyInterface) {
    const auto& ti = static_cast<const TyInterface&>(*td.type);
    emit_ops_.emitln("struct " + name + " {");
    emit_ops_.indent();
    emit_ops_.emitln("virtual ~" + name + "() = default;");
    for (const auto& m : ti.members) {
      if (m.kind != ObjectMemberKind::Method || !m.method) continue;
      const auto& pd = *m.method;
      emit_ops_.emitln(proc_attributes_to_cxx(pd) + "virtual " +
                       proc_return_type_to_cxx(pd) + " " +
                       mangle(pd.name) + "(" + param_list_to_cxx(pd.params) +
                       ") = 0;");
    }
    emit_ops_.dedent();
    emit_ops_.emitln("};");
    return;
  }

  if (td.type && td.type->kind == Kind::TyObject) {
    const auto& to = static_cast<const TyObject&>(*td.type);
    if (to.is_forward) {
      return;
    }
    const TypeSymbol* symbol = registry_.canonical_symbol_for_type(td.type.get());
    const TypeSymbol* payload_symbol = descriptor_payload_symbol(symbol);
    const ClassInfo* current_class_info =
        payload_symbol ? payload_symbol->class_info() : nullptr;
    const ClassInfo* parent_info =
        current_class_info ? registry_.lookup_parent_class(*current_class_info)
                           : nullptr;
    const std::string source_type_name =
        symbol ? type_symbol_source_name(*symbol) : ascii_lower(td.name);
    const std::string qualified_cxx_name =
        symbol ? types_.type_symbol_struct_cxx(*symbol) : name;
    auto inherited_virtual_with_same_cxx_signature =
        [&](const ProcDecl& pd) -> const MethodSig* {
      if (pd.is_class_method || !parent_info) return nullptr;

      const std::string method_name = ascii_lower(pd.name);
      const std::string cxx_name = mangle(pd.name);
      const std::string params = param_type_list_to_cxx(pd.params);
      const ClassInfo* parent = parent_info;
      std::unordered_set<std::string> seen;
      while (parent) {
        const std::string identity = parent->defining_unit + "." + parent->name;
        if (seen.count(identity)) break;
        seen.insert(identity);

        auto mit = parent->methods.find(method_name);
        if (mit != parent->methods.end()) {
          for (const auto& inherited : mit->second) {
            if (!inherited.decl || !inherited.is_virtual ||
                inherited.kind == SymKind::ClassMethod) {
              continue;
            }
            if (mangle(inherited.decl->name) == cxx_name &&
                param_type_list_to_cxx(inherited.decl->params) == params) {
              return &inherited;
            }
          }
        }
        parent = registry_.lookup_parent_class(*parent);
      }
      return nullptr;
    };
    std::string line = "struct " + name;
    std::vector<std::string> bases;
    if (parent_info) {
      bases.push_back(class_struct_cxx(*parent_info));
    } else if (to.is_reference_type) {
      bases.push_back("::rt::t_tobject");
    }
    for (const auto& iface : to.interfaces) {
      bases.push_back(types_.named_type_struct_cxx(iface));
    }
    if (!bases.empty()) {
      line += " : ";
      for (size_t i = 0; i < bases.size(); ++i) {
        if (i) line += ", ";
        line += "public " + bases[i];
      }
    }
    line += " {";
    emit_ops_.emitln(line);
    emit_ops_.indent();
    if (to.is_reference_type) {
      const std::string meta_name = current_class_info
                                        ? metaclass_struct_cxx(*current_class_info,
                                                               source_type_name)
                                        : types_.metaclass_struct_cxx(source_type_name);
      const std::string value_fn =
          current_class_info ? metaclass_value_fn_cxx(*current_class_info,
                                                       source_type_name)
                             : types_.metaclass_value_fn_cxx(source_type_name);
      // The metaclass struct and value function implement Pascal `class of`
      // dispatch. A virtual class method can be strict protected and still
      // have an entry in that table, so the generated table code must be able
      // to call the protected C++ member without making normal Pascal calls
      // bypass visibility.
      emit_ops_.emitln("friend struct " + meta_name + ";");
      emit_ops_.emitln("friend " + meta_name + "* " + value_fn + "();");
    }
    if (parent_info) {
      emit_ops_.emitln("using inherited = " +
                       class_struct_cxx(*parent_info) + ";");
    } else if (to.is_reference_type) {
      emit_ops_.emitln("using inherited = ::rt::t_tobject;");
    }
    if (to.is_reference_type) {
      emit_ops_.emitln("virtual ::rt::t_tclass p_classtype() const override;");
      emit_ops_.emitln("virtual int32_t p_instancesize() const override;");
    }
    bool has_virtual = false;
    std::string current_access = "public";
    bool seen_class_constructor = false;
    bool seen_class_destructor = false;
    std::vector<PendingReferenceClassSupport> nested_pending;
    for (const auto& m : to.members) {
      std::string wanted_access = cxx_access_for_pascal_visibility(m.vis);
      if (wanted_access != current_access) {
        emit_ops_.emitln(wanted_access + ":");
        current_access = std::move(wanted_access);
      }
      if (m.kind == ObjectMemberKind::Type) {
        if (m.type_decl) emit_nested_type_decl(*m.type_decl, nested_pending);
      } else if (m.kind == ObjectMemberKind::Field) {
        for (const auto& fn : m.field_names) {
          // FPC rejects a field or class var that reuses an inherited field
          // name. C++ would accept a derived static member with the same name,
          // so diagnose it before emission can change the Pascal program.
          bool inherited_field = false;
          for (const ClassInfo* parent = parent_info; parent && !inherited_field;
               parent = registry_.lookup_parent_class(*parent)) {
            inherited_field = parent->fields.count(ascii_lower(fn)) != 0;
          }
          if (inherited_field) {
            emit_ops_.report_error(m.loc, "duplicate inherited field `" + fn + "'");
          }
          const std::string field_name = registry_.field_cxx_name(fn);
          const std::string decl =
              types_.named_type_to_cxx(m.field_type.get(), field_name);
          emit_ops_.emitln(m.is_class_var ? ("inline static " + decl + "{};")
                                           : (decl + ";"));
        }
      } else if (m.kind == ObjectMemberKind::Method) {
        const auto& pd = *m.method;
        const bool is_class_lifecycle =
            pd.is_class_method &&
            (pd.pkind == ProcKind::Constructor ||
             pd.pkind == ProcKind::Destructor);
        if (is_class_lifecycle) {
          bool& seen = pd.pkind == ProcKind::Constructor
                           ? seen_class_constructor
                           : seen_class_destructor;
          if (seen) {
            emit_ops_.report_error(
                pd.loc,
                pd.pkind == ProcKind::Constructor
                    ? "duplicate class constructor"
                    : "duplicate class destructor");
          }
          seen = true;
          if (pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override ||
              pd.modifiers.is_final) {
            emit_ops_.report_error(
                pd.loc,
                "class constructors and destructors cannot be virtual");
          }
        }
        const MethodSig* inherited_same_signature_virtual =
            inherited_virtual_with_same_cxx_signature(pd);
        if (pd.modifiers.is_final && !(pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override)) {
          emit_ops_.report_error(pd.loc, "only virtual methods can be final");
        }
        // FPC rejects `override` on old-style Pascal `object` methods; the
        // accepted Pascal source syntax for overriding an inherited object
        // virtual is to redeclare the derived method as `virtual`. That still
        // becomes a C++ override, so emit the C++ `override` specifier for this
        // accepted object case. For `class`, keep requiring the Pascal `override`
        // directive so a source-level hide/reintroduce does not silently become
        // an implicit C++ override.
        const bool object_virtual_override =
            inherited_same_signature_virtual && !to.is_reference_type &&
            pd.modifiers.is_virtual;
        if (inherited_same_signature_virtual &&
            inherited_same_signature_virtual->is_final) {
          emit_ops_.report_error(pd.loc, "cannot override final method");
        } else if (!pd.modifiers.is_override && !object_virtual_override &&
            inherited_same_signature_virtual) {
          emit_ops_.report_error(
              pd.loc,
              "method matches an inherited virtual method; use `override' "
              "to override.  Note: `reintroduce' is not supported.");
        }
        std::string ret = proc_return_type_to_cxx(pd);
        std::string prefix;
        if (pd.is_class_method) {
          prefix = "static ";
        } else if (pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override) {
          prefix = "virtual ";
          has_virtual = true;
        }
        std::string suffix;
        if (!pd.is_class_method) {
          if (pd.modifiers.is_override || object_virtual_override) suffix += " override";
          if (pd.modifiers.is_final &&
              (pd.modifiers.is_virtual || pd.modifiers.is_abstract || pd.modifiers.is_override ||
               object_virtual_override)) {
            suffix += " final";
          }
        }
        if (pd.modifiers.is_abstract && !pd.is_class_method) {
          emit_ops_.emitln(proc_attributes_to_cxx(pd) + prefix + ret + " " +
                           mangle(pd.name) + "(" +
                           param_list_to_cxx(pd.params) + ")" + suffix +
                           " { ::std::abort(); }");
        } else {
          emit_ops_.emitln(proc_attributes_to_cxx(pd) + prefix + ret + " " +
                           mangle(pd.name) + "(" +
                           param_list_to_cxx(pd.params) + ")" + suffix + ";");
        }
        if (!pd.is_class_method) emit_method_pointer_thunk(name, pd, ret);
      }
    }
    if (has_virtual) {
      std::string wanted_access =
          cxx_access_for_pascal_visibility(Visibility::Public);
      if (wanted_access != current_access) {
        emit_ops_.emitln(wanted_access + ":");
        current_access = std::move(wanted_access);
      }
      emit_ops_.emitln("virtual ~" + name + "() = default;");
    }
    emit_ops_.dedent();
    emit_ops_.emitln("};");

    if (to.is_reference_type) {
      pending_support.push_back(PendingReferenceClassSupport{
          .decl = &td,
          .class_info = current_class_info,
          .source_type_name = source_type_name,
          .qualified_cxx_name = qualified_cxx_name});
    }
    pending_support.insert(pending_support.end(), nested_pending.begin(),
                           nested_pending.end());
    return;
  }

  std::string rhs = td.type ? types_.type_to_cxx(*td.type)
                            : std::string("int32_t");
  // MIGRATION: this preserves Pascal alias spelling as a C++ `using`
  // declaration. Keep only if a later compatibility decision explicitly wants
  // alias declarations in the generated C++ surface; semantic consumers must
  // not depend on this alias existing.
  emit_ops_.emitln("using " + name + " = " + rhs + ";");
}

void EmitDecls::emit_reference_class_support(
    const TypeDecl& td, const ClassInfo& class_info,
    std::string_view qualified_cxx_name,
    std::string_view source_type_name) {
  const std::string meta_name =
      metaclass_struct_cxx(class_info, source_type_name);
  const std::string value_fn =
      metaclass_value_fn_cxx(class_info, source_type_name);
  const ClassInfo* parent_info =
      registry_.lookup_parent_class(class_info);
  const bool has_parent_meta = parent_info && parent_info->is_reference_type;
  const std::string parent_meta =
      has_parent_meta ? metaclass_struct_cxx(*parent_info) : std::string{};
  const std::string base_meta =
      has_parent_meta ? parent_meta
                      : std::string("::rt::tp2cc_metaclass_t_tobject");
  const auto visible_callables = collect_metaclass_callables(class_info);
  const auto parent_callables =
      has_parent_meta ? collect_metaclass_callables(*parent_info)
                      : std::vector<MetaclassCallable>{};
  const std::vector<MetaclassCallable> own_callables =
      own_metaclass_callables(visible_callables, parent_callables);

  std::string meta_decl = "struct " + meta_name;
  meta_decl += " : public " + base_meta;
  meta_decl += " {";
  emit_ops_.emitln(meta_decl);
  emit_ops_.indent();
  for (const auto& callable : own_callables) {
    if (is_virtual_metaclass_callable(callable)) {
      const bool same_as_parent =
          has_same_parent_metaclass_slot(callable, parent_callables);
      emit_virtual_metaclass_callable(class_info, source_type_name, callable,
                                      same_as_parent);
    } else {
      const std::string ret =
          metaclass_callable_return_type(class_info, source_type_name,
                                         callable);
      emit_ops_.emitln(ret + " (*" + mangle(callable.name) + ")(" +
                       metaclass_callable_param_types(callable) + ");");
    }
  }
  const std::string direct_parent_meta =
      has_parent_meta ? (metaclass_value_fn_cxx(*parent_info) + "()")
                      : std::string("::rt::tp2cc_metaclass_value_t_tobject()");
  emit_ops_.emitln("::rt::t_tclass tp2cc_parentclass() const override { "
                   "return " +
                   direct_parent_meta + "; }");
  emit_ops_.emitln("::rt::tp2cc_ShortString<> p_classname() const override { "
                   "return ::rt::tp2cc_shortstring_of<>(\"" +
                   td.name + "\"); }");
  if (visible_callables.empty()) {
    emit_ops_.emitln(meta_name + "() = default;");
  } else {
    std::string ctor_params;
    bool first = true;
    if (has_parent_meta && !parent_callables.empty()) {
      ctor_params += parent_meta + " tp2cc_parent";
      first = false;
    }
    for (const auto& callable : own_callables) {
      if (is_virtual_metaclass_callable(callable)) continue;
      if (!first) ctor_params += ", ";
      ctor_params += metaclass_callable_ctor_param(class_info, source_type_name,
                                                   callable);
      first = false;
    }
    std::string init_list;
    if (has_parent_meta && !parent_callables.empty()) {
      init_list = " : " + parent_meta + "(tp2cc_parent)";
    }
    for (const auto& callable : own_callables) {
      if (is_virtual_metaclass_callable(callable)) continue;
      init_list += (init_list.empty() ? " : " : ", ") +
                   metaclass_callable_ctor_init(callable);
    }
    emit_ops_.emitln(meta_name + "(" + ctor_params + ")" + init_list + " {}");
  }
  emit_ops_.dedent();
  emit_ops_.emitln("};");

  // Keep metaclass objects out of C++ global initialization order. Pascal
  // class metadata is immutable, so a function-local static constructs each
  // metaclass on first use and recursively constructs its parent first.
  emit_ops_.emitln("inline " + meta_name + "* " + value_fn + "() {");
  emit_ops_.indent();
  if (visible_callables.empty()) {
    emit_ops_.emitln("static " + meta_name + " value{};");
  } else {
    emit_ops_.emitln("static " + meta_name + " value = " +
                     build_metaclass_ctor_expr(class_info, source_type_name,
                                               class_info, source_type_name) +
                     ";");
  }
  emit_ops_.emitln("return &value;");
  emit_ops_.dedent();
  emit_ops_.emitln("}");
  emit_ops_.emitln("inline ::rt::t_tclass " +
                   std::string(qualified_cxx_name) + "::p_classtype() const {");
  emit_ops_.indent();
  emit_ops_.emitln("return " + value_fn + "();");
  emit_ops_.dedent();
  emit_ops_.emitln("}");
  emit_ops_.emitln("inline int32_t " + std::string(qualified_cxx_name) +
                   "::p_instancesize() const {");
  emit_ops_.indent();
  emit_ops_.emitln("return sizeof(" + std::string(qualified_cxx_name) + ");");
  emit_ops_.dedent();
  emit_ops_.emitln("}");
}

void EmitDecls::emit_pending_reference_class_support(
    const PendingReferenceClassSupport& pending) {
  const TypeDecl* td = pending.decl;
  const ClassInfo* class_info = pending.class_info;
  if (!td || !td->type || td->type->kind != Kind::TyObject) return;
  const auto& to = static_cast<const TyObject&>(*td->type);
  if (!to.is_reference_type || to.is_forward) return;
  if (!class_info) {
    emit_ops_.report_error(td->loc,
                           "missing resolved class metadata for `" +
                               pending.source_type_name + "`");
    return;
  }

  const std::string source_name = ascii_lower(pending.source_type_name);
  // Pascal reference classes need namespace-scope C++ helper definitions for
  // `class of' values and TObject virtual metadata. The declaration pass
  // records the build-resolved ClassInfo while class scopes are open. The
  // containing type drains the records after its closing brace, when qualified
  // out-of-class definitions are legal C++; re-looking up the class name here
  // would hide failed build binding and reintroduce emission-time name lookup.
  emit_reference_class_support(*td, *class_info, pending.qualified_cxx_name,
                               source_name);
}

void EmitDecls::emit_var_decl(const VarDecl& vd, bool in_header) {
  if (!vd.is_external && should_emit_var_type_helpers(vd, in_header)) {
    emit_enum_carrier_decls(vd.type.get());
  }
  if (vd.is_absolute) {
    auto target = storage_.resolve_absolute_target(vd);
    if (!target) return;
    std::string ty =
        vd.type ? types_.type_to_cxx(*vd.type) : std::string("int32_t");
    bool pointee_view =
        target->is_pointerish && !storage_.type_is_pointerish(vd.type.get());
    for (const auto& n : vd.names) {
      std::string name = mangle(n);
      std::string decl = attach_named_cxx_type(
          ty, name, target->is_const_storage ? "const &" : "&");
      if (in_header) {
        emit_ops_.emitln("extern " + decl + ";");
      } else {
        emit_ops_.emitln(decl + " = " +
                         storage_.reinterpret_ref_text(ty, target->cxx,
                                                       pointee_view) +
                         ";");
      }
    }
    return;
  }
  if (vd.is_external) {
    emit_ops_.report_error(vd.loc, "external variables are unsupported");
    return;
  }
  const TyRecord* inline_packed_record = nullptr;
  if (vd.type && vd.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*vd.type);
    if (tr.is_packed) inline_packed_record = &tr;
  }
  for (const auto& n : vd.names) {
    std::string name = mangle(n);
    std::string decl = types_.named_type_to_cxx(vd.type.get(), name);
    if (in_header) {
      emit_ops_.emitln("extern " + decl + ";");
    } else if (vd.init) {
      std::string rhs = values_.const_value_to_cxx(*vd.init, vd.type.get());
      emit_ops_.emitln(decl + " = " + rhs + ";");
    } else if (emit_ops_.in_block_scope()) {
      emit_ops_.emitln(decl + "{};");
    } else {
      emit_ops_.emitln(decl + ";");
    }
    if (inline_packed_record) {
      const auto layout = types_.compute_record_layout(*inline_packed_record);
      emit_packed_record_asserts("decltype(" + name + ")", layout.packed_layout.field_offsets,
                                 layout.packed_layout.size_expr, n);
    }
  }
}

std::string EmitDecls::param_list_to_cxx(const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& p : params) {
    std::string pt;
    std::string name_prefix;
    if (!p.type) {
      pt = (p.mode == Param::Const || p.mode == Param::ConstRef)
               ? "const void*"
               : "void*";
    } else {
      if (storage_.type_is_open_array(p.type.get())) {
        pt = types_.open_array_type_to_cxx(*p.type);
      } else if (types_.param_uses_shortstring_ref(p.type.get(), p.mode)) {
        pt = types_.shortstring_ref_type_to_cxx(p.type.get());
      } else {
        pt = types_.type_to_cxx(*p.type);
      }
      if (types_.param_uses_shortstring_ref(p.type.get(), p.mode)) {
        name_prefix.clear();
      } else if (p.mode == Param::ConstRef) {
        name_prefix = "const &";
      } else if (p.mode == Param::Var || p.mode == Param::Out) {
        name_prefix = "&";
      } else if (p.mode == Param::Const &&
                 analysis_.const_param_needs_mutable_ref(p.type.get())) {
        name_prefix = "&";
      } else if (p.mode == Param::Const &&
                 analysis_.const_param_needs_const_ref(p.type.get())) {
        name_prefix = "const &";
      }
    }
    for (const auto& n : p.names) {
      if (!first) out += ", ";
      first = false;
      if (!p.type) out += pt + " " + mangle(n);
      else out += attach_named_cxx_type(pt, mangle(n), name_prefix);
    }
    if (p.names.empty()) {
      if (!first) out += ", ";
      first = false;
      if (!p.type) out += pt;
      else out += attach_named_cxx_type(pt, "", name_prefix);
    }
  }
  return out;
}

std::string EmitDecls::param_type_list_to_cxx(
    const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& p : params) {
    std::string pt;
    if (!p.type) {
      pt = (p.mode == Param::Const || p.mode == Param::ConstRef)
               ? "const void*"
               : "void*";
    } else {
      if (storage_.type_is_open_array(p.type.get())) {
        pt = types_.open_array_type_to_cxx(*p.type);
      } else if (types_.param_uses_shortstring_ref(p.type.get(), p.mode)) {
        pt = types_.shortstring_ref_type_to_cxx(p.type.get());
      } else {
        pt = types_.type_to_cxx(*p.type);
      }
      if (types_.param_uses_shortstring_ref(p.type.get(), p.mode)) {
        // Already a mutable storage proxy value.
      } else if (p.mode == Param::ConstRef) {
        pt = "const " + pt + "&";
      } else if (p.mode == Param::Var || p.mode == Param::Out) {
        pt += "&";
      } else if (p.mode == Param::Const &&
                 analysis_.const_param_needs_mutable_ref(p.type.get())) {
        pt += "&";
      } else if (p.mode == Param::Const &&
                 analysis_.const_param_needs_const_ref(p.type.get())) {
        pt = "const " + pt + "&";
      }
    }
    size_t count = p.names.empty() ? 1 : p.names.size();
    for (size_t i = 0; i < count; ++i) {
      if (!first) out += ", ";
      first = false;
      out += pt;
    }
  }
  return out;
}

std::string EmitDecls::proc_return_type_to_cxx(const ProcDecl& pd) {
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    return types_.type_to_cxx(*pd.return_type);
  }
  if (pd.pkind == ProcKind::Constructor && !pd.is_class_method) {
    return "bool";
  }
  return "void";
}

std::string EmitDecls::proc_attributes_to_cxx(const ProcDecl& pd) {
  return pd.modifiers.is_noreturn ? "[[noreturn]] " : "";
}

std::string EmitDecls::method_pointer_thunk_param_type(const Param& par) {
  return types_.procedural_param_type_to_cxx(par);
}

std::vector<EmitDecls::MethodPointerThunkParam>
EmitDecls::method_pointer_thunk_params(const ProcDecl& pd) {
  std::vector<MethodPointerThunkParam> out;
  int unnamed_index = 0;
  for (const auto& par : pd.params) {
    std::string pt = method_pointer_thunk_param_type(par);
    auto call_arg_for = [&](const std::string& name) {
      if (!types_.procedural_param_uses_pointer_carrier(par)) return name;
      std::string actual = types_.type_to_cxx(*par.type);
      if (actual == pt) return name;
      return "static_cast<" + actual + ">(" + name + ")";
    };
    if (par.names.empty()) {
      std::string name = "tp2cc_arg" + std::to_string(++unnamed_index);
      out.push_back(MethodPointerThunkParam{
          .type = pt, .name = name, .call_arg = call_arg_for(name)});
      continue;
    }
    for (const auto& pn : par.names) {
      std::string name = mangle(pn);
      out.push_back(MethodPointerThunkParam{
          .type = pt, .name = name, .call_arg = call_arg_for(name)});
    }
  }
  return out;
}

void EmitDecls::emit_method_pointer_thunk(const std::string& owner_name,
                                          const ProcDecl& pd,
                                          const std::string& ret) {
  if (pd.is_class_method) return;
  if (pd.pkind != ProcKind::Procedure && pd.pkind != ProcKind::Function) {
    return;
  }

  std::string helper_params = "void* tp2cc_self";
  std::string helper_args;
  std::vector<MethodPointerThunkParam> thunk_params =
      method_pointer_thunk_params(pd);
  // `tp2cc_MethodPtr<Sig>::operator()` casts the stored code slot to exactly
  // `Ret (*)(void*, Sig...)` before calling it. Therefore the helper's C++
  // signature is the procvar carrier signature, and pointer-like Pascal
  // formals are cast back to the method's real formal types inside the helper.
  for (size_t i = 0; i < thunk_params.size(); ++i) {
    const auto& param = thunk_params[i];
    helper_params += ", " + param.type + " " + param.name;
    if (i != 0) helper_args += ", ";
    helper_args += param.call_arg;
  }

  std::string call = "static_cast<" + owner_name + "*>(tp2cc_self)->" +
                     mangle(pd.name) + "(" + helper_args + ")";
  if (pd.pkind == ProcKind::Function) {
    emit_ops_.emitln("static " + ret + " " +
                     types_.method_pointer_helper_name(pd) + "(" +
                     helper_params + ") { return " + call + "; }");
  } else {
    emit_ops_.emitln("static void " + types_.method_pointer_helper_name(pd) +
                     "(" + helper_params + ") { " + call + "; }");
  }
}

void EmitDecls::emit_proc_decl_signature(const ProcDecl& pd) {
  if (pd.modifiers.is_external) {
    emit_ops_.report_error(pd.loc, "external routines are unsupported");
    return;
  }
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = types_.type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  std::string params = param_list_to_cxx(pd.params);
  emit_ops_.emitln(proc_attributes_to_cxx(pd) + ret + " " +
                   pascal_operator_decl_name_to_cxx(pd) + "(" + params + ");");
}

void EmitDecls::emit_decl(const Decl& d, bool in_header) {
  switch (d.kind) {
    case Kind::ConstDecl:
      emit_const_decl(static_cast<const ConstDecl&>(d), in_header);
      break;
    case Kind::TypeDecl:
      emit_type_decl(static_cast<const TypeDecl&>(d), in_header);
      break;
    case Kind::VarDecl:
      emit_var_decl(static_cast<const VarDecl&>(d), in_header);
      break;
    case Kind::ProcDecl: {
      const auto& pd = static_cast<const ProcDecl&>(d);
      if (in_header) {
        emit_proc_decl_signature(pd);
      } else if (pd.modifiers.is_external) {
        emit_ops_.report_error(pd.loc, "external routines are unsupported");
      } else if (pd.modifiers.is_forward) {
        if (emit_ops_.in_block_scope()) {
          std::string ret =
              (pd.pkind == ProcKind::Function && pd.return_type)
                  ? types_.type_to_cxx(*pd.return_type)
                  : std::string("void");
          std::string sig_params;
          bool first = true;
          for (const auto& p : pd.params) {
            std::string pt;
            if (!p.type) {
              pt = (p.mode == Param::Const || p.mode == Param::ConstRef)
                       ? "const void*"
                       : "void*";
            } else if (types_.param_uses_shortstring_ref(p.type.get(), p.mode)) {
              pt = types_.shortstring_ref_type_to_cxx(p.type.get());
            } else {
              pt = types_.type_to_cxx(*p.type);
              if (p.mode == Param::ConstRef) {
                pt = "const " + pt + "&";
              } else if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
              else if (p.mode == Param::Const &&
                       analysis_.const_param_needs_mutable_ref(p.type.get())) {
                pt += "&";
              } else if (p.mode == Param::Const &&
                         analysis_.const_param_needs_const_ref(p.type.get())) {
                pt = "const " + pt + "&";
              }
            }
            for (const auto& n : p.names) {
              (void)n;
              if (!first) sig_params += ", ";
              first = false;
              sig_params += pt;
            }
            if (p.names.empty()) {
              if (!first) sig_params += ", ";
              first = false;
              sig_params += pt;
            }
          }
          const std::string lname = nested_proc_cxx_name(scope_, pd);
          emit_ops_.emitln("::std::function<" + ret + "(" + sig_params +
                           ")> " + lname + ";");
          scope_.local_nested_forwards.insert(lname);
        } else {
          emit_proc_decl_signature(pd);
        }
      } else if (!pd.modifiers.is_external && (pd.body || pd.modifiers.is_abstract)) {
        if (emit_ops_.in_block_scope()) {
          emit_ops_.emit_nested_proc_lambda(pd);
        } else {
          emit_ops_.emit_proc_body(pd);
        }
      }
      break;
    }
    case Kind::LabelDecl:
      break;
    default:
      break;
  }
}

}  // namespace tp2cc
