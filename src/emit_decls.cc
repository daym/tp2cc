#include "emit_decls.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "emit_analysis.h"
#include "emit_storage.h"
#include "emit_support.h"
#include "emit_types.h"
#include "emit_values.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

EmitDecls::EmitDecls(const TypeRegistry* registry, ScopeStateView& scope,
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
  std::vector<const TyEnum*> enums;
  collect_enum_types(*t, enums);
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
  if (in_header || emit_ops_.in_block_scope() || !registry_) return true;
  auto uit = registry_->units.find(scope_.current_unit_name);
  if (uit == registry_->units.end()) return true;
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
    std::string_view class_name) {
  std::vector<MetaclassCallable> out;
  if (!registry_) return out;

  std::string cls = ascii_lower(std::string(class_name));
  std::vector<const ClassInfo*> chain;
  std::unordered_set<std::string> seen;
  const ClassInfo* ci = analysis_.class_info_for_type_name(cls);
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    chain.push_back(ci);
    ci = ci->parent.empty() ? nullptr
                            : registry_->lookup_class(ci->parent,
                                                      ci->defining_unit);
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

  if (const auto* ci = analysis_.class_info_for_type_name(class_name);
      ci && ci->is_reference_type && !pos.count("create")) {
    // Every Delphi/FPC class inherits `TObject.Create` even if the source
    // does not redeclare a constructor. Class-value calls like
    // `TDLLScannerClass(x).Create` therefore still need a zero-arg metaclass
    // entry instead of falling out as an unsupported member.
    out.push_back({"create", nullptr, true});
  }
  return out;
}

std::optional<EmitDecls::MetaclassCallableImpl>
EmitDecls::find_metaclass_callable_impl(std::string_view concrete_class,
                                        const MetaclassCallable& target) {
  if (!registry_) return std::nullopt;

  auto matches_target = [&](const MethodSig& candidate) {
    if (!candidate.decl) return false;
    if (target.implicit_root_create) {
      return candidate.kind == SymKind::Constructor &&
             candidate.param_count == 0;
    }
    if (!target.sig || !target.sig->decl) return false;
    return candidate.kind == target.sig->kind &&
           types_.procedural_param_types_to_cxx(candidate.decl->params) ==
               types_.procedural_param_types_to_cxx(target.sig->decl->params);
  };

  std::string cls = ascii_lower(std::string(concrete_class));
  std::unordered_set<std::string> seen;
  const ClassInfo* ci = analysis_.class_info_for_type_name(cls);
  while (ci) {
    const std::string identity = ci->defining_unit + "." + ci->name;
    if (seen.count(identity)) break;
    seen.insert(identity);
    auto mit = ci->methods.find(target.name);
    if (mit != ci->methods.end()) {
      for (const auto& sig : mit->second) {
        if (matches_target(sig)) {
          return MetaclassCallableImpl{identity, &sig, false};
        }
      }
    }
    ci = ci->parent.empty() ? nullptr
                            : registry_->lookup_class(ci->parent,
                                                      ci->defining_unit);
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

void EmitDecls::emit_const_decl(const ConstDecl& cd, bool in_header) {
  (void)in_header;
  const std::string name = mangle(cd.name);
  emit_enum_carrier_decls(cd.type.get());
  std::string val = values_.const_value_to_cxx(*cd.value, cd.type.get());

  // Two things drive the qualifiers:
  //   `inline` -- required on definitions at namespace scope in a header so
  //              multiple translation units that include it don't violate
  //              ODR. Invalid at block scope, so we drop it there.
  //   `const`  -- Pascal's UNTYPED const (`const X = 5;`) is immutable,
  //              TYPED const (`const X : T = 5;`) is writable.
  const bool block = emit_ops_.in_block_scope();
  const std::string linkage = block ? std::string() : std::string("inline ");
  const TypeExpr* typed_const_ty =
      cd.type ? analysis_.canonicalize_type(cd.type.get()) : nullptr;

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
        std::string lo, size_expr;
        if (!types_.array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
          goto generic_emit;
        }
        ty = "::rt::tp2cc_Array<" + ty + ", " + lo + ", " + size_expr + ">";
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
    const TypeExpr* canon = analysis_.canonicalize_type(inferred_ty);
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
  (void)in_header;
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
    auto inherited_virtual_with_same_cxx_signature =
        [&](const ProcDecl& pd) -> const MethodSig* {
      if (!registry_ || pd.is_class_method || to.parent.empty()) return nullptr;

      const std::string method_name = ascii_lower(pd.name);
      const std::string cxx_name = mangle(pd.name);
      const std::string params = param_type_list_to_cxx(pd.params);
      const ClassInfo* parent =
          registry_->lookup_class(to.parent, scope_.current_unit_name);
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
        parent = parent->parent.empty()
                     ? nullptr
                     : registry_->lookup_class(parent->parent,
                                              parent->defining_unit);
      }
      return nullptr;
    };
    std::string line = "struct " + name;
    std::vector<std::string> bases;
    if (!to.parent.empty()) {
      bases.push_back(types_.named_type_struct_cxx(to.parent));
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
      const std::string meta_name = "tp2cc_metaclass_" + name;
      const std::string value_fn = "tp2cc_metaclass_value_" + name;
      // The metaclass struct and value function implement Pascal `class of`
      // dispatch. A virtual class method can be strict protected and still
      // have an entry in that table, so the generated table code must be able
      // to call the protected C++ member without making normal Pascal calls
      // bypass visibility.
      emit_ops_.emitln("friend struct " + meta_name + ";");
      emit_ops_.emitln("friend " + meta_name + "* " + value_fn + "();");
    }
    if (!to.parent.empty()) {
      emit_ops_.emitln("using inherited = " +
                       types_.named_type_struct_cxx(to.parent) + ";");
    } else if (to.is_reference_type) {
      emit_ops_.emitln("using inherited = ::rt::t_tobject;");
    }
    if (to.is_reference_type) {
      emit_ops_.emitln("virtual ::rt::t_tclass p_classtype() const override;");
      emit_ops_.emitln("virtual int32_t p_instancesize() const override;");
    }
    bool has_virtual = false;
    std::string current_access = "public";
    auto cxx_access = [](Visibility vis) {
      switch (vis) {
        case Visibility::StrictPrivate:
          return "private";
        case Visibility::StrictProtected:
          return "protected";
        case Visibility::Public:
        case Visibility::Private:
        case Visibility::Protected:
          // Non-strict Pascal private/protected members remain public in C++:
          // Pascal still permits same-unit access, and those accesses are
          // emitted as ordinary namespace-level C++ code.
          return "public";
      }
      return "public";
    };
    auto ensure_access = [&](Visibility vis) {
      std::string wanted = cxx_access(vis);
      if (wanted != current_access) {
        emit_ops_.emitln(wanted + ":");
        current_access = std::move(wanted);
      }
    };
    bool seen_class_constructor = false;
    bool seen_class_destructor = false;
    for (const auto& m : to.members) {
      ensure_access(m.vis);
      if (m.kind == ObjectMemberKind::Field) {
        for (const auto& fn : m.field_names) {
          // FPC rejects a field or class var that reuses an inherited field
          // name. C++ would accept a derived static member with the same name,
          // so diagnose it before emission can change the Pascal program.
          if (!to.parent.empty() && registry_ &&
              registry_->lookup_class_field(to.parent, fn,
                                            scope_.current_unit_name)) {
            emit_ops_.report_error(m.loc, "duplicate inherited field `" + fn + "'");
          }
          const std::string field_name =
              registry_ ? registry_->field_cxx_name(fn) : mangle(fn);
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
          if (pd.is_virtual || pd.is_abstract || pd.is_override ||
              pd.is_final) {
            emit_ops_.report_error(
                pd.loc,
                "class constructors and destructors cannot be virtual");
          }
        }
        const MethodSig* inherited_same_signature_virtual =
            inherited_virtual_with_same_cxx_signature(pd);
        if (pd.is_final && !(pd.is_virtual || pd.is_abstract || pd.is_override)) {
          emit_ops_.report_error(pd.loc, "only virtual methods can be final");
        }
        // FPC rejects `override` on old-style Pascal `object` methods; its
        // documented spelling for overriding an inherited object virtual is to
        // redeclare the derived method as `virtual`. That still becomes a C++
        // override, so emit the C++ `override` specifier for this accepted
        // object case. For `class`, keep requiring the Pascal `override`
        // directive so a source-level hide/reintroduce does not silently become
        // an implicit C++ override.
        const bool object_virtual_override =
            inherited_same_signature_virtual && !to.is_reference_type &&
            pd.is_virtual;
        if (inherited_same_signature_virtual &&
            inherited_same_signature_virtual->is_final) {
          emit_ops_.report_error(pd.loc, "cannot override final method");
        } else if (!pd.is_override && !object_virtual_override &&
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
        } else if (pd.is_virtual || pd.is_abstract || pd.is_override) {
          prefix = "virtual ";
          has_virtual = true;
        }
        std::string suffix;
        if (!pd.is_class_method) {
          if (pd.is_override || object_virtual_override) suffix += " override";
          if (pd.is_final &&
              (pd.is_virtual || pd.is_abstract || pd.is_override ||
               object_virtual_override)) {
            suffix += " final";
          }
        }
        if (pd.is_abstract && !pd.is_class_method) {
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
      ensure_access(Visibility::Public);
      emit_ops_.emitln("virtual ~" + name + "() = default;");
    }
    emit_ops_.dedent();
    emit_ops_.emitln("};");

    if (to.is_reference_type) {
      const std::string meta_name = "tp2cc_metaclass_" + name;
      const std::string value_fn = "tp2cc_metaclass_value_" + name;
      const bool has_parent_meta =
          !to.parent.empty() && analysis_.class_info_for_type_name(to.parent) &&
          analysis_.class_info_for_type_name(to.parent)->is_reference_type;
      const std::string parent_meta =
          has_parent_meta ? types_.metaclass_struct_cxx(to.parent)
                          : std::string{};
      const std::string base_meta =
          has_parent_meta ? parent_meta
                          : std::string("::rt::tp2cc_metaclass_t_tobject");
      const auto visible_callables = collect_metaclass_callables(td.name);
      const auto parent_callables =
          has_parent_meta ? collect_metaclass_callables(to.parent)
                          : std::vector<MetaclassCallable>{};
      auto callable_param_types = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
        return types_.procedural_param_types_to_cxx(callable.sig->decl->params);
      };
      auto same_callable_surface = [&](const MetaclassCallable& lhs,
                                       const MetaclassCallable& rhs) {
        if (lhs.name != rhs.name) return false;
        if (lhs.implicit_root_create || rhs.implicit_root_create) {
          return lhs.implicit_root_create == rhs.implicit_root_create;
        }
        if (!lhs.sig || !rhs.sig) return lhs.sig == rhs.sig;
        return lhs.sig->kind == rhs.sig->kind &&
               callable_param_types(lhs) == callable_param_types(rhs);
      };
      auto is_virtual_class_callable = [](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig ||
            !callable.sig->decl) {
          return false;
        }
        const auto& pd = *callable.sig->decl;
        return callable.sig->kind == SymKind::ClassMethod &&
               (pd.is_virtual || pd.is_abstract || pd.is_override);
      };
      std::unordered_map<std::string, MetaclassCallable> parent_surface;
      for (const auto& callable : parent_callables) {
        parent_surface.emplace(callable.name, callable);
      }
      std::vector<MetaclassCallable> own_callables;
      for (const auto& callable : visible_callables) {
        auto pit = parent_surface.find(callable.name);
        const bool same_as_parent =
            pit != parent_surface.end() &&
            same_callable_surface(callable, pit->second);
        if (!same_as_parent || is_virtual_class_callable(callable)) {
          own_callables.push_back(callable);
          continue;
        }
        if (callable.implicit_root_create ||
            (callable.sig && callable.sig->kind == SymKind::Constructor)) {
          // Constructors are special: even when a derived class keeps the same
          // Pascal parameter surface as its parent, the metaclass slot still
          // needs a concrete return type (`TChild*`, not `TBase*`). Keep the
          // derived slot visible so the generated metaclass does not erase the
          // return type back to the parent's class pointer.
          own_callables.push_back(callable);
        }
      }

      auto callable_return_type = [&](std::string_view target_class,
                                      const MetaclassCallable& callable) {
        if (callable.implicit_root_create ||
            (callable.sig && callable.sig->kind == SymKind::Constructor)) {
          return types_.named_type_struct_cxx(target_class) + "*";
        }
        const auto& pd = *callable.sig->decl;
        if (pd.pkind == ProcKind::Function && pd.return_type) {
          return types_.type_to_cxx(*pd.return_type);
        }
        return std::string("void");
      };
      auto callable_param_list = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
        return param_list_to_cxx(callable.sig->decl->params);
      };
      auto callable_arg_list = [&](const MetaclassCallable& callable) {
        if (callable.implicit_root_create || !callable.sig) return std::string{};
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
      };
      auto callable_ctor_param = [&](std::string_view target_class,
                                     const MetaclassCallable& callable) {
        return callable_return_type(target_class, callable) + " (*tp2cc_" +
               mangle(callable.name) + ")(" + callable_param_types(callable) +
               ")";
      };
      auto callable_ctor_init = [&](const MetaclassCallable& callable) {
        return mangle(callable.name) + "(tp2cc_" + mangle(callable.name) + ")";
      };
      auto emit_virtual_class_callable = [&](std::string_view owner_class,
                                             const MetaclassCallable& callable,
                                             bool has_same_parent_slot) {
        const auto& pd = *callable.sig->decl;
        const std::string ret = callable_return_type(owner_class, callable);
        std::string decl = proc_attributes_to_cxx(pd) + "virtual " + ret +
                           " " + mangle(callable.name) + "(" +
                           callable_param_list(callable) + ") const";
        if (has_same_parent_slot) decl += " override";
        decl += " { ";
        if (pd.is_abstract && !pd.body) {
          decl += "::std::abort();";
          if (ret != "void") decl += " return {};";
        } else {
          if (ret != "void") decl += "return ";
          decl += types_.named_type_struct_cxx(owner_class) + "::" +
                  mangle(callable.name) + "(" + callable_arg_list(callable) +
                  ");";
        }
        decl += " }";
        emit_ops_.emitln(decl);
      };

      std::string meta_decl = "struct " + meta_name;
      meta_decl += " : public " + base_meta;
      meta_decl += " {";
      emit_ops_.emitln(meta_decl);
      emit_ops_.indent();
      for (const auto& callable : own_callables) {
        if (is_virtual_class_callable(callable)) {
          auto pit = parent_surface.find(callable.name);
          const bool same_as_parent =
              pit != parent_surface.end() &&
              same_callable_surface(callable, pit->second);
          emit_virtual_class_callable(td.name, callable, same_as_parent);
        } else {
          emit_ops_.emitln(callable_return_type(td.name, callable) + " (*" +
                           mangle(callable.name) + ")(" +
                           callable_param_types(callable) + ");");
        }
      }
      const std::string direct_parent_meta =
          has_parent_meta ? (types_.metaclass_value_fn_cxx(to.parent) + "()")
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
          if (is_virtual_class_callable(callable)) continue;
          if (!first) ctor_params += ", ";
          ctor_params += callable_ctor_param(td.name, callable);
          first = false;
        }
        std::string init_list;
        if (has_parent_meta && !parent_callables.empty()) {
          init_list = " : " + parent_meta + "(tp2cc_parent)";
        }
        for (const auto& callable : own_callables) {
          if (is_virtual_class_callable(callable)) continue;
          init_list += (init_list.empty() ? " : " : ", ") +
                       callable_ctor_init(callable);
        }
        emit_ops_.emitln(meta_name + "(" + ctor_params + ")" + init_list +
                         " {}");
      }
      emit_ops_.dedent();
      emit_ops_.emitln("};");

      std::function<std::string(std::string_view, std::string_view)>
          build_metaclass_ctor_expr =
              [&](std::string_view target_class,
                  std::string_view concrete_class) -> std::string {
        const auto target_callables = collect_metaclass_callables(target_class);
        std::string current = ascii_lower(std::string(target_class));
        std::string parent_class;
        if (const auto* ci = analysis_.class_info_for_type_name(current)) {
          parent_class = ci->parent;
        }
        const bool has_parent =
            !parent_class.empty() &&
            analysis_.class_info_for_type_name(parent_class) &&
            analysis_.class_info_for_type_name(parent_class)->is_reference_type;
        const auto parent_visible =
            has_parent ? collect_metaclass_callables(parent_class)
                       : std::vector<MetaclassCallable>{};
        std::unordered_map<std::string, MetaclassCallable> parent_surface;
        for (const auto& callable : parent_visible) {
          parent_surface.emplace(callable.name, callable);
        }
        auto ctor_member_call = [&](std::string_view owner_class,
                                    std::string_view method_name,
                                    const std::string& args) {
          if (ascii_lower(std::string(owner_class)) ==
              ascii_lower(std::string(concrete_class))) {
            return "tp2cc_ptr->" + std::string(method_name) + "(" + args + ")";
          }
          return "static_cast<" + types_.named_type_struct_cxx(owner_class) +
                 "*>(tp2cc_ptr)->" + std::string(method_name) + "(" + args +
                 ")";
        };
        std::string out = types_.metaclass_struct_cxx(target_class) + "(";
        bool first = true;
        if (has_parent && !parent_visible.empty()) {
          out += build_metaclass_ctor_expr(parent_class, concrete_class);
          first = false;
        }
        for (const auto& callable : target_callables) {
          if (is_virtual_class_callable(callable)) continue;
          auto pit = parent_surface.find(callable.name);
          if (pit != parent_surface.end() &&
              same_callable_surface(callable, pit->second)) {
            if (!(callable.implicit_root_create ||
                  (callable.sig &&
                   callable.sig->kind == SymKind::Constructor))) {
              continue;
            }
            // Keep same-signature constructors visible in the constructed
            // metaclass value too. Otherwise the static metaclass descriptor
            // erases `TChild.Create` back to the parent's create slot even
            // though the metaclass struct still exposes the derived return
            // type and constructor pointer member.
          }
          if (!first) out += ", ";
          const auto concrete_impl =
              find_metaclass_callable_impl(concrete_class, callable);
          if (!concrete_impl) {
            const std::string ret =
                callable_return_type(target_class, callable);
            out += "+[](" + callable_param_list(callable) + ") -> " + ret +
                   " { ::std::abort();";
            if (ret != "void") out += " return {};";
            out += " }";
            first = false;
            continue;
          }
          const bool use_implicit_root_create =
              concrete_impl->implicit_root_create;
          const auto* concrete_sig = concrete_impl->sig;
          if (use_implicit_root_create ||
              (concrete_sig && concrete_sig->kind == SymKind::Constructor)) {
            out += "+[](" + callable_param_list(callable) + ") -> " +
                   callable_return_type(target_class, callable) + " { auto "
                   "tp2cc_ptr = new " +
                   types_.named_type_struct_cxx(concrete_class) + "{}; ";
            const std::string ctor_name =
                use_implicit_root_create ? "p_create" : mangle(callable.name);
            out += ctor_member_call(concrete_impl->owner_class, ctor_name,
                                    callable_arg_list(callable)) +
                   "; return tp2cc_ptr; }";
          } else {
            std::string ret = callable_return_type(target_class, callable);
            out += "+[](" + callable_param_list(callable) + ") -> " + ret +
                   " { ";
            if (ret != "void") out += "return ";
            out += types_.named_type_struct_cxx(concrete_impl->owner_class) +
                   "::" + mangle(callable.name) + "(" +
                   callable_arg_list(callable) + ");";
            out += " }";
          }
          first = false;
        }
        out += ")";
        return out;
      };

      emit_ops_.emitln("inline " + meta_name + "* " + value_fn + "() {");
      emit_ops_.indent();
      if (visible_callables.empty()) {
        emit_ops_.emitln("static " + meta_name + " value{};");
      } else {
        emit_ops_.emitln("static " + meta_name + " value = " +
                         build_metaclass_ctor_expr(td.name, td.name) + ";");
      }
      emit_ops_.emitln("return &value;");
      emit_ops_.dedent();
      emit_ops_.emitln("}");
      emit_ops_.emitln("inline ::rt::t_tclass " + name +
                       "::p_classtype() const {");
      emit_ops_.indent();
      emit_ops_.emitln("return " + value_fn + "();");
      emit_ops_.dedent();
      emit_ops_.emitln("}");
      emit_ops_.emitln("inline int32_t " + name + "::p_instancesize() const {");
      emit_ops_.indent();
      emit_ops_.emitln("return sizeof(" + name + ");");
      emit_ops_.dedent();
      emit_ops_.emitln("}");
    }
    return;
  }

  std::string rhs = td.type ? types_.type_to_cxx(*td.type)
                            : std::string("int32_t");
  emit_ops_.emitln("using " + name + " = " + rhs + ";");
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
  return pd.is_noreturn ? "[[noreturn]] " : "";
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
  bool first_arg = true;
  int unnamed_index = 0;
  auto append_arg = [&](const std::string& pt, const std::string& name) {
    helper_params += ", " + pt + " " + name;
    if (!first_arg) helper_args += ", ";
    first_arg = false;
    helper_args += name;
  };

  for (const auto& par : pd.params) {
    std::string pt;
    if (!par.type) {
      pt = (par.mode == Param::Const || par.mode == Param::ConstRef)
               ? "const void*"
               : "void*";
    } else if (storage_.type_is_open_array(par.type.get())) {
      pt = types_.open_array_type_to_cxx(*par.type);
    } else if (types_.param_uses_shortstring_ref(par.type.get(), par.mode)) {
      pt = types_.shortstring_ref_type_to_cxx(par.type.get());
    } else {
      pt = types_.type_to_cxx(*par.type);
    }
    if (par.type) {
      if (types_.param_uses_shortstring_ref(par.type.get(), par.mode)) {
        // Already a mutable storage proxy value.
      } else if (par.mode == Param::ConstRef) {
        pt = "const " + pt + "&";
      } else if (par.mode == Param::Var || par.mode == Param::Out) pt += "&";
      else if (par.mode == Param::Const &&
               analysis_.const_param_needs_mutable_ref(par.type.get()))
        pt += "&";
      else if (par.mode == Param::Const &&
               analysis_.const_param_needs_const_ref(par.type.get()))
        pt = "const " + pt + "&";
    }
    if (par.names.empty()) {
      append_arg(pt, "tp2cc_arg" + std::to_string(++unnamed_index));
      continue;
    }
    for (const auto& pn : par.names) {
      append_arg(pt, mangle(pn));
    }
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
  if (pd.is_external) {
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
      } else if (pd.is_external) {
        emit_ops_.report_error(pd.loc, "external routines are unsupported");
      } else if (pd.is_forward) {
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
          emit_ops_.emitln("::std::function<" + ret + "(" + sig_params +
                           ")> " + mangle(pd.name) + ";");
          scope_.local_nested_forwards.insert(pd.name);
        } else {
          emit_proc_decl_signature(pd);
        }
      } else if (!pd.is_external && (pd.body || pd.is_abstract)) {
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
