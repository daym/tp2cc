#include "emit.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diag.h"
#include "typereg.h"

namespace tp2cc {

using namespace ast;

namespace {

const ast::TyName* builtin_char_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "char";
    return n;
  }();
  return &t;
}

const ast::TyName* builtin_string_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "string";
    return n;
  }();
  return &t;
}

const ast::TyName* builtin_pchar_type() {
  static const ast::TyName t = [] {
    ast::TyName n;
    n.name = "pchar";
    return n;
  }();
  return &t;
}

// ---------------------------------------------------------------------------
// Name mangling

// Identifiers already start with `p_` in the output. Pascal built-in type
// names map directly to C++ types below without the prefix.
std::string mangle(std::string_view name) {
  std::string s("p_");
  s.append(name);
  return s;
}

std::string ascii_lower(std::string_view text) {
  std::string s(text);
  for (char& ch : s) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return s;
}

bool tyname_is(const TypeExpr* t, std::string_view expected) {
  return t && t->kind == Kind::TyName &&
         ascii_lower(static_cast<const TyName&>(*t).name) == expected;
}

// Pascal primitive type names recognised by name lookup during type emission.
// Keys are lowercased; values are the C++ expansion.
const std::unordered_map<std::string, std::string>& primitive_type_map() {
  static const std::unordered_map<std::string, std::string> m = {
      {"integer",   "int32_t"},
      {"longint",   "int32_t"},
      {"cardinal",  "uint32_t"},
      {"longword",  "uint32_t"},
      {"smallint",  "int16_t"},
      {"word",      "uint16_t"},
      {"shortint",  "int8_t"},
      {"byte",      "uint8_t"},
      {"char",      "::rt::p_char"},
      {"boolean",   "bool"},
      {"bytebool",  "uint8_t"},
      {"wordbool",  "uint16_t"},
      {"longbool",  "uint32_t"},
      {"single",    "float"},
      {"double",    "double"},
      {"real",      "double"},
      {"extended",  "long double"},
      {"comp",      "long double"},
      {"pointer",   "void*"},
      {"pchar",     "::rt::p_char*"},
      {"ppchar",    "::rt::p_char**"},
      {"text",      "::rt::TextFile"},
      {"int64",     "int64_t"},
      {"qword",     "uint64_t"},
      {"dword",     "uint32_t"},
      {"string",    "::rt::ShortString<>"},
      {"shortstring", "::rt::ShortString<>"},
  };
  return m;
}

bool is_primitive_type(std::string_view lowname) {
  return primitive_type_map().count(std::string(lowname)) > 0;
}

std::string primitive_type_cxx(std::string_view lowname) {
  auto it = primitive_type_map().find(std::string(lowname));
  if (it == primitive_type_map().end()) return {};
  return it->second;
}

// ---------------------------------------------------------------------------
// Emitter state

struct Emitter {
  std::string header;
  std::string impl;
  // Current sink pointer.
  std::string* out = &header;
  int indent_level = 0;
  // Depth of proc bodies we're currently emitting. >0 means block scope,
  // which means C++ `inline` is invalid for local decls.
  int block_depth = 0;

  // Name of the Pascal class whose method body we're currently emitting
  // (if any). Empty when emitting a free function or at namespace scope.
  std::string current_class_name;

  // Name of the Pascal unit we are emitting (lowercased). Used to
  // decide whether a cross-unit reference needs explicit qualification
  // to disambiguate it from a same-named symbol brought in by another
  // `uses` clause.
  std::string current_unit_name;

  // Suppresses the "bare method reference -> append ()" rewrite. Set
  // while emitting (a) the CALLEE of a Call (else `foo(args)` would
  // become `foo()(args)`), and (b) the operand of AddrOf (else `@foo`
  // would become `&foo()`).
  bool is_callee_context_ = false;

  // When emitting an LHS expression, if set, any bare Ident whose name
  // equals this value is rewritten to `p_result`. Used so that Pascal's
  // `funcname := x`, `funcname[i] := x`, `funcname.field := x` all route
  // to the result slot rather than trying to mutate the function.
  std::string lhs_fn_rewrite;

  // Names bound in the current function's scope (parameters + locals).
  // `obj` resolved bare at block scope that hits this set must be a
  // variable, so auto-call (`name()`) is suppressed. Prevents false
  // auto-call on local vars whose names happen to coincide with a
  // parameterless method in another unit.
  std::unordered_set<std::string> local_scope;

  // Variable-to-declared-type map for the current scope (parameters +
  // locals). Populated at proc-body entry so expression-type deduction
  // can answer "what class does this variable belong to?" and the
  // Member-access emitter can auto-call only actual methods.
  std::unordered_map<std::string, const ast::TypeExpr*> local_types;

  // Names of the current scope's parameters that are Pascal's untyped
  // `var X` / `const X` / `X` form. Their C++ type is `void*` (not
  // `void*&`); the callee receives the caller's storage address.
  // `@X` on one of these emits as the ident itself -- no `&`.
  std::unordered_set<std::string> local_untyped_params;

  // Nested procs/functions declared in the current scope. For each
  // name, store the parameter count and whether it returns a value.
  // Used so bare references to a parameterless nested `function`
  // auto-call (the lambda itself is `std::function<T()>`, not a `T`).
  struct NestedFn {
    size_t param_count = 0;
    bool is_function = false;
    const ast::TypeExpr* return_type = nullptr;
  };
  std::unordered_map<std::string, NestedFn> local_nested_fns;

  // Function-local enum types: name -> the TyEnum AST node. Pascal
  // lets a `type T = (a, b, c)` and `const X : array[T] of ... = ...`
  // live inside a proc's declaration section. These aren't in the
  // unit-wide TypeRegistry (which only indexes interface/impl top-
  // level decls), so we layer them on here while emitting the proc.
  std::unordered_map<std::string, const ast::TyEnum*> local_enums;
  // Function-local type aliases: `type pi = ^integer;` style.
  std::unordered_map<std::string, const ast::TypeExpr*>
      local_type_aliases_scoped;

  // `with X do` bindings: for every `with target`, push the target's
  // expression text (already emitted) and its deduced type. Bare idents
  // inside the body that resolve as fields of one of the targets get
  // rewritten to `target.name`. For auto-call decisions on bare idents,
  // consult these types.
  struct WithBind {
    std::string cxx_text;
    const ast::TypeExpr* type = nullptr;
  };
  std::vector<WithBind> with_stack;

  // Reified type/symbol tree spanning all parsed units. Set by the
  // driver. Drives member-access and ident-call decisions.
  const TypeRegistry* registry = nullptr;

  // Topologically-sorted unit names whose `__unit_init()` must run at
  // program start, before the program's `begin..end.` body. Set by
  // the driver only when emitting the `program` unit.
  const std::vector<std::string>* unit_init_order = nullptr;

  void set_header() { out = &header; }
  void set_impl()   { out = &impl; }

  void emit(std::string_view s) { out->append(s); }
  void emitln(std::string_view s) {
    for (int i = 0; i < indent_level; ++i) out->append("  ");
    out->append(s);
    out->push_back('\n');
  }
  void nl() { out->push_back('\n'); }
  void indent() { ++indent_level; }
  void dedent() { if (indent_level > 0) --indent_level; }

  // Top-level drivers.
  void emit_unit(const UnitNode& u);
  void emit_decl(const Decl& d, bool in_header);
  void emit_const_decl(const ConstDecl& cd, bool in_header);
  void emit_type_decl(const TypeDecl& td, bool in_header);
  void emit_var_decl(const VarDecl& vd, bool in_header);
  void emit_proc_decl_signature(const ProcDecl& pd);

  // Types -> C++ type string.
  std::string type_to_cxx(const TypeExpr& t);
  std::string type_name_to_cxx(const TyName& n);
  std::string array_type_to_cxx(const TyArray& a);
  const ast::TypeExpr* canonicalize_type(const ast::TypeExpr* t);
  bool array_dim_bounds_to_cxx(const ast::TypeExpr& dim,
                               std::string* lo,
                               std::string* size_expr);
  std::string set_type_to_cxx(const TySet& s);
  std::string enum_type_to_cxx(const TyEnum& e, const std::string& context);
  std::string subrange_type_to_cxx(const TySubrange& r);
  std::string string_type_to_cxx(const TyString& s);
  std::string pointer_type_to_cxx(const TyPointer& p);
  std::string procedural_type_to_cxx(const TyProcedural& p);

  // Expressions -> C++ expression.
  std::string expr_to_cxx(const Expr& e);
  std::string const_value_to_cxx(const Expr& e);

  // Small helpers.
  bool const_param_needs_mutable_ref(const ast::TypeExpr* t);
  std::string primitive_cast_lvalue_ref(const ast::Call& c);
  std::string param_list_to_cxx(const std::vector<Param>& params);
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'

  // Expression-type deduction. Returns the Pascal TypeExpr that the
  // expression has, or nullptr when unknown. Consults the TypeRegistry
  // for globals and the current scope tables for locals/self-class.
  const ast::TypeExpr* deduce_type(const ast::Expr& e);

  // Class/record alias name ("tfoo") of `e`, lowercased, if detectable.
  // Empty if the type can't be narrowed to a named object/record type.
  std::string deduce_class_alias(const ast::Expr& e);
  const ast::Expr* peel_primitive_casts(const ast::Expr* e);
  bool expr_is_storage_lvalue(const ast::Expr& e);
  bool expr_is_charish(const ast::Expr& e);
  bool type_is_stringish(const ast::TypeExpr* t);
  bool type_is_open_array(const ast::TypeExpr* t);
  std::string open_array_type_to_cxx(const ast::TypeExpr& t);
  void mark_call_param_info(const ast::ProcDecl* decl,
                            std::vector<bool>& untyped_arg,
                            std::vector<const ast::TypeExpr*>& param_types);
  void collect_call_param_info(const ast::Expr& callee,
                               std::vector<bool>& untyped_arg,
                               std::vector<const ast::TypeExpr*>& param_types);
  std::string lower_call_arg(const ast::Expr& arg,
                             const ast::TypeExpr* param_type,
                             bool untyped_arg);

  // ---------------------------------------------------------------------
  // Pascal name resolution (one function, every emit path goes through
  // it). Given a name and optional qualifier, model the full Pascal
  // lookup:
  //   - unqualified: `with` -> locals -> enclosing nested fns ->
  //                  class+ancestors (in method body) -> current unit ->
  //                  `uses` chain -> rt:: builtins.
  //   - `Unit.name`: symbols exported by `Unit` (which must be in the
  //                  current unit's `uses` list).
  //   - `Class.name` / `obj.name`: class's members walking ancestors.
  //
  // The resolved result tells the emitter:
  //   - how to spell the access in C++,
  //   - whether it's a parameterless callable (value context -> auto-call),
  //   - the ProcDecl* for call-site untyped-var arg wrapping,
  //   - whether it's a field/var/const/enum-member (never auto-call).
  enum class ResolvedKind {
    Unknown,          // emit the mangled name; let C++ lookup sort it out
    ResultSlot,       // Pascal fn's name-as-read inside its own body
    Local,            // param/local/typed-const/nested-fn-name
    NestedFn,         // a parameterless nested function value
    WithField,        // field under a `with X do` binding
    WithMethod,       // method under a `with X do` binding
    ClassField,       // member of current class (or ancestor)
    ClassMethod,      // method of current class (or ancestor)
    UnitVar,
    UnitConst,
    UnitProc,
    UnitType,
    EnumMember,
    RtBuiltin,
  };
  struct ResolveResult {
    ResolvedKind kind = ResolvedKind::Unknown;
    std::string cxx;              // the full C++ expression text
    bool is_parameterless = false;
    bool is_callable = false;
    const ast::ProcDecl* proc = nullptr;   // for call-site analysis
  };
  // Qualifier: empty means unqualified lookup.  Otherwise it's a
  // unit name or a class/record alias name (both lowercased).
  enum class QualifierKind { None, Unit, Class };
  ResolveResult resolve_name(const std::string& name,
                             QualifierKind qk = QualifierKind::None,
                             const std::string& qualifier = {});

  // State: the Pascal identifier of the current function whose body we are
  // emitting (not mangled). Used by `exit`/`exit(v)` translation so we
  // know which result slot to fill.
  std::string current_fn_name;
  bool current_fn_is_function = false;
  bool current_fn_is_ctor = false;
  // Stack of loop-exit labels. Pascal `break` inside a `case` arm must
  // exit the enclosing loop, but C++ `break` inside `switch` exits the
  // switch -- so we emit Pascal `break` as `goto` to a fresh label
  // placed right after each loop. Also used by `continue` -> a separate
  // label placed at the loop's re-test/re-increment point.
  std::vector<std::string> loop_break_labels;
  std::vector<std::string> loop_continue_labels;
  int loop_label_counter = 0;

  void emit_tpexcept_unit(const UnitNode& u);
};

// ---------------------------------------------------------------------------
// Types

std::string Emitter::type_name_to_cxx(const TyName& n) {
  if (is_primitive_type(n.name)) return primitive_type_cxx(n.name);
  // Qualified name `unitname.Type` -> `p_unitname::p_type`
  auto dot = n.name.find('.');
  if (dot != std::string::npos) {
    return mangle(std::string_view(n.name).substr(0, dot)) +
           "::" + mangle(std::string_view(n.name).substr(dot + 1));
  }
  return mangle(n.name);
}

const TypeExpr* Emitter::canonicalize_type(const TypeExpr* t) {
  int hops = 0;
  while (t && t->kind == Kind::TyName && hops++ < 32) {
    const auto& n = static_cast<const TyName&>(*t);
    auto lit = local_type_aliases_scoped.find(n.name);
    if (lit != local_type_aliases_scoped.end() && lit->second &&
        lit->second != t) {
      t = lit->second;
      continue;
    }
    if (registry) {
      const TypeExpr* next = registry->canonicalize(t);
      if (next && next != t) {
        t = next;
        continue;
      }
    }
    break;
  }
  return t;
}

bool Emitter::const_param_needs_mutable_ref(const TypeExpr* t) {
  t = canonicalize_type(t);
  if (t && t->kind == Kind::TyName && registry) {
    const auto& name = static_cast<const TyName&>(*t).name;
    if (registry->classes.count(name) || registry->records.count(name)) {
      return true;
    }
  }
  // In old Turbo Pascal/FPC object-style code, `const` on records and
  // objects is mainly a calling-convention hint. Treating it as C++
  // deep immutability breaks method calls and container updates.
  return t && (t->kind == Kind::TyRecord || t->kind == Kind::TyObject);
}

std::string Emitter::primitive_cast_lvalue_ref(const Call& c) {
  if (c.args.size() != 1 || c.callee->kind != Kind::Ident) return {};
  const auto& id = static_cast<const Ident&>(*c.callee);
  if (!is_primitive_type(id.name)) return {};
  const Expr* peeled = peel_primitive_casts(c.args[0].get());
  if (!peeled || !expr_is_storage_lvalue(*c.args[0])) return {};
  if (peeled->kind == Kind::Ident) {
    ResolveResult rr = resolve_name(static_cast<const Ident&>(*peeled).name);
    if (rr.kind == ResolvedKind::UnitConst || rr.kind == ResolvedKind::EnumMember ||
        rr.kind == ResolvedKind::UnitType || rr.is_callable) {
      return {};
    }
  }
  // Pascal `T(lv)` used as an lvalue aliases the same storage with a
  // different type. Emit that reinterpretation directly.
  return "::rt::p_reinterpret_ref<" + primitive_type_cxx(id.name) + ">(" +
         expr_to_cxx(*peeled) + ")";
}

bool Emitter::array_dim_bounds_to_cxx(const TypeExpr& dim_in,
                                      std::string* lo,
                                      std::string* size_expr) {
  auto expr_is_char = [&](const Expr& e) -> bool {
    const TypeExpr* t = deduce_type(e);
    if (t) t = canonicalize_type(t);
    return tyname_is(t, "char");
  };
  auto ordinal_bound = [&](const Expr& e) -> std::string {
    std::string text = const_value_to_cxx(e);
    return expr_is_char(e) ? "::rt::p_ord(" + text + ")" : text;
  };
  const TypeExpr* dim = canonicalize_type(&dim_in);
  if (!dim) return false;
  *lo = "0";
  size_expr->clear();
  if (dim->kind == Kind::TySubrange) {
    const auto& sr = static_cast<const TySubrange&>(*dim);
    *lo = const_value_to_cxx(*sr.lo);
    *size_expr = "((" + ordinal_bound(*sr.hi) + ") - (" + ordinal_bound(*sr.lo) +
                 ") + 1)";
    return true;
  }
  if (dim->kind == Kind::TyEnum) {
    const auto& en = static_cast<const TyEnum&>(*dim);
    *size_expr = std::to_string(en.members.size());
    return true;
  }
  if (dim->kind != Kind::TyName) return false;
  const auto& tn = static_cast<const TyName&>(*dim);
  auto leit = local_enums.find(tn.name);
  if (leit != local_enums.end()) {
    *size_expr = std::to_string(leit->second->members.size());
    return true;
  }
  if (registry) {
    auto eit = registry->enums.find(tn.name);
    if (eit != registry->enums.end()) {
      *size_expr = std::to_string(eit->second.members.size());
      return true;
    }
  }
  if (tn.name == "boolean" || tn.name == "bytebool") {
    *size_expr = "2";
    return true;
  }
  if (tn.name == "byte" || tn.name == "char" || tn.name == "shortint") {
    *size_expr = "256";
    return true;
  }
  if (tn.name == "word" || tn.name == "smallint" || tn.name == "wordbool") {
    *size_expr = "65536";
    return true;
  }
  return false;
}

std::string Emitter::subrange_type_to_cxx(const TySubrange& r) {
  // If both bounds are enum members of the same enum, the subrange's
  // base type IS that enum -- and we want to keep that typing so
  // things like `Set of R_EAX..R_BL` get `Set<tregister>` rather
  // than `Set<int32_t>`.
  auto bound_enum = [&](const Expr* e) -> std::string {
    if (!e || e->kind != Kind::Ident || !registry) return {};
    auto it = registry->enum_members.find(
        static_cast<const Ident&>(*e).name);
    if (it == registry->enum_members.end()) return {};
    // enum_members maps member-name -> defining unit. Find which
    // enum within that unit the member belongs to.
    auto uit = registry->units.find(it->second);
    if (uit == registry->units.end()) return {};
    for (const auto& [en, info] : registry->enums) {
      if (info.defining_unit == it->second) {
        for (const auto& m : info.members) {
          if (m == static_cast<const Ident&>(*e).name) return en;
        }
      }
    }
    return {};
  };
  std::string le = bound_enum(r.lo.get());
  std::string he = bound_enum(r.hi.get());
  if (!le.empty() && le == he) return mangle(le);
  // Without further info we can only represent subranges as their
  // base type; pick int32_t as a safe default.
  return "int32_t";
}

std::string Emitter::string_type_to_cxx(const TyString& s) {
  if (s.max_length) {
    // `string[N]`
    return "::rt::ShortString<" + const_value_to_cxx(*s.max_length) + ">";
  }
  return "::rt::ShortString<>";
}

std::string Emitter::pointer_type_to_cxx(const TyPointer& p) {
  return type_to_cxx(*p.target) + "*";
}

std::string Emitter::set_type_to_cxx(const TySet& s) {
  // For enum element types we can use the enum itself to parameterise the
  // set; for primitives we'd use a bounded-integer Set. Keep it coarse for
  // now: element type tagged into the template.
  return "::rt::Set<" + type_to_cxx(*s.element) + ">";
}

std::string Emitter::enum_type_to_cxx(const TyEnum& e, const std::string&) {
  // An anonymous enum inside a type alias is unusual. Emit an inline
  // anonymous enum with unprefixed C++ syntax. This function is called
  // when the enum appears at the RHS of `type X = (...)` -- emission of
  // that decl handles the full `enum class X { ... }` form.
  // For nested anonymous positions, just return int32_t.
  return "int32_t";
}

std::string Emitter::array_type_to_cxx(const TyArray& a) {
  // `array[D1, D2, ...] of T` emits as nested ::rt::Array<T, Lo, N>
  // wrappers. Open arrays (no dims) stay as a plain pointer.
  if (a.dims.empty()) {
    return type_to_cxx(*a.element) + "*";
  }
  std::string ty = type_to_cxx(*a.element);
  // Wrap from innermost to outermost.
  for (auto it = a.dims.rbegin(); it != a.dims.rend(); ++it) {
    std::string lo, size_expr;
    if (!array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
      // Can't compute dimension statically; fall back to pointer.
      return type_to_cxx(*a.element) + "*";
    }
    ty = "::rt::Array<" + ty + ", " + lo + ", " + size_expr + ">";
  }
  return ty;
}

std::string Emitter::procedural_type_to_cxx(const TyProcedural& p) {
  std::string ret = p.is_function ? type_to_cxx(*p.return_type) : std::string("void");
  std::string params;
  bool first = true;
  for (const auto& pp : p.params) {
    std::string pt = pp.type ? type_to_cxx(*pp.type) : std::string("void*");
    if (pp.mode == Param::Var || pp.mode == Param::Out) pt += "&";
    else if (pp.mode == Param::Const) {
      if (const_param_needs_mutable_ref(pp.type.get())) pt += "&";
      else pt = std::string("const ") + pt + "&";
    }
    for (const auto& n : pp.names) {
      (void)n;
      if (!first) params += ", ";
      first = false;
      params += pt;
    }
    if (pp.names.empty()) {
      if (!first) params += ", ";
      first = false;
      params += pt;
    }
  }
  return ret + " (*)(" + params + ")";
}

std::string Emitter::type_to_cxx(const TypeExpr& t) {
  switch (t.kind) {
    case Kind::TyName:       return type_name_to_cxx(static_cast<const TyName&>(t));
    case Kind::TyPointer:    return pointer_type_to_cxx(static_cast<const TyPointer&>(t));
    case Kind::TySet:        return set_type_to_cxx(static_cast<const TySet&>(t));
    case Kind::TyArray:      return array_type_to_cxx(static_cast<const TyArray&>(t));
    case Kind::TySubrange:   return subrange_type_to_cxx(static_cast<const TySubrange&>(t));
    case Kind::TyString:     return string_type_to_cxx(static_cast<const TyString&>(t));
    case Kind::TyEnum:       return enum_type_to_cxx(static_cast<const TyEnum&>(t), "");
    case Kind::TyProcedural: return procedural_type_to_cxx(static_cast<const TyProcedural&>(t));
    case Kind::TyFile: {
      // Pascal `text`, `file`, `file of T`.
      const auto& tf = static_cast<const TyFile&>(t);
      if (tf.is_text || !tf.element) return "::rt::TextFile";
      return "::rt::TypedFile<" + type_to_cxx(*tf.element) + ">";
    }
    case Kind::TyRecord:
    case Kind::TyObject:
      // Anonymous record/object in-place -- rare; emit a stub. Named
      // records/objects come via TyName above.
      return "/* inline-record */ int32_t";
    default:                 return "/* unsupported-type */ int32_t";
  }
}

// ---------------------------------------------------------------------------
// Expression-type deduction. Used by the Member / Ident emitters so
// decisions like "is `obj.name` a method call or a field read?" come
// from the actual type tree, not name-matching heuristics.

const TypeExpr* Emitter::deduce_type(const Expr& e) {
  if (!registry) return nullptr;
  switch (e.kind) {
    case Kind::Ident: {
      const auto& id = static_cast<const Ident&>(e);
      // Local variables and parameters shadow everything.
      auto lit = local_types.find(id.name);
      if (lit != local_types.end()) return lit->second;
      // Nested functions live in `local_nested_fns`, not `local_types`.
      // Type deduction still needs to see their result type so boolean
      // expressions like `if ready and flag then` lower to `&&` even
      // before the ident emitter auto-calls a parameterless `ready`.
      auto nit = local_nested_fns.find(id.name);
      if (nit != local_nested_fns.end() && nit->second.is_function)
        return nit->second.return_type;
      // Self -- canonically the current class's type.
      if (id.name == "self" && !current_class_name.empty()) {
        // We don't track a direct TypeExpr for the class here. Returning
        // nullptr is fine; Member access will fall through to class-name
        // handling via `current_class_name` in the caller.
        return nullptr;
      }
      // Class member (inside method body of a known class).
      if (!current_class_name.empty()) {
        auto* m = registry->lookup_class_member(current_class_name, id.name);
        if (m && !m->is_method) return m->field.type;
      }
      // `with X do` bindings contribute fields of their target type --
      // the ident might name such a field.
      for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
        std::string ac = registry ? registry->direct_type_name(
                                        registry->canonicalize(it->type))
                                  : std::string{};
        if (ac.empty()) continue;
        auto* m = registry->lookup_class_member(ac, id.name);
        if (m && !m->is_method) return m->field.type;
        auto* rf = registry->lookup_record_field(ac, id.name);
        if (rf) return rf->type;
      }
      // Unit-level lookup: own unit first, then each `uses` entry
      // (right-to-left). The global last-wins maps on TypeRegistry
      // are NOT consulted here -- two units can share a name with
      // different types and the only right answer is to find the
      // one exported from a unit the current unit actually uses.
      // Own-unit: both interface and impl visible. Other units:
      // interface-exports only.
      auto lookup_own = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_var(id.name)) return v->type;
        if (auto* c = u.find_const(id.name); c && c->type) return c->type;
        if (auto* p = u.find_proc(id.name);
            p && p->decl && p->decl->return_type)
          return p->decl->return_type.get();
        return nullptr;
      };
      auto lookup_export = [&](const UnitInfo& u) -> const TypeExpr* {
        if (auto* v = u.find_export_var(id.name)) return v->type;
        if (auto* c = u.find_export_const(id.name); c && c->type) return c->type;
        if (auto* p = u.find_export_proc(id.name);
            p && p->decl && p->decl->return_type)
          return p->decl->return_type.get();
        return nullptr;
      };
      auto cur = registry->units.find(current_unit_name);
      if (cur != registry->units.end()) {
        if (const auto* t = lookup_own(cur->second)) return t;
        for (auto it = cur->second.uses.rbegin();
             it != cur->second.uses.rend(); ++it) {
          auto uit = registry->units.find(*it);
          if (uit == registry->units.end()) continue;
          if (const auto* t = lookup_export(uit->second)) return t;
        }
      }
      return nullptr;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      const TypeExpr* t = deduce_type(*d.operand);
      if (!t) return nullptr;
      t = registry->canonicalize(t);
      if (tyname_is(t, "pchar")) return builtin_char_type();
      if (tyname_is(t, "ppchar")) return builtin_pchar_type();
      if (t && t->kind == Kind::TyPointer) {
        const auto& p = static_cast<const TyPointer&>(*t);
        return p.target.get();
      }
      return nullptr;
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      // If base is `self`, use current_class_name directly.
      std::string cls;
      if (m.base->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*m.base);
        if (id.name == "self") cls = current_class_name;
      }
      if (cls.empty()) {
        const TypeExpr* bt = deduce_type(*m.base);
        if (bt) cls = registry->direct_type_name(registry->canonicalize(bt));
      }
      if (cls.empty()) return nullptr;
      if (auto* cm = registry->lookup_class_member(cls, m.name)) {
        if (cm->is_method) {
          if (cm->method.decl && cm->method.decl->return_type)
            return cm->method.decl->return_type.get();
          return nullptr;
        }
        return cm->field.type;
      }
      if (auto* rf = registry->lookup_record_field(cls, m.name))
        return rf->type;
      return nullptr;
    }
    case Kind::Index: {
      const auto& ix = static_cast<const Index&>(e);
      const TypeExpr* bt = deduce_type(*ix.base);
      if (!bt) return nullptr;
      bt = registry->canonicalize(bt);
      if (bt && bt->kind == Kind::TyString) return builtin_char_type();
      if (tyname_is(bt, "pchar")) return builtin_char_type();
      if (tyname_is(bt, "ppchar")) return builtin_pchar_type();
      if (bt && bt->kind == Kind::TyArray)
        return static_cast<const TyArray&>(*bt).element.get();
      return nullptr;
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      const TypeExpr* callee_type = deduce_type(*c.callee);
      if (callee_type) callee_type = canonicalize_type(callee_type);
      if (callee_type && callee_type->kind == Kind::TyProcedural) {
        const auto& p = static_cast<const TyProcedural&>(*callee_type);
        if (p.is_function) return p.return_type.get();
      }
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        if ((id.name == "char" || id.name == "chr") && c.args.size() == 1)
          return builtin_char_type();
        if ((id.name == "succ" || id.name == "pred" || id.name == "upcase") &&
            c.args.size() == 1)
          return deduce_type(*c.args[0]);
        auto nit = local_nested_fns.find(id.name);
        if (nit != local_nested_fns.end() && nit->second.is_function)
          return nit->second.return_type;
        // Type cast `T(expr)` -- target type is the alias's own type.
        auto ait = registry->aliases.find(id.name);
        if (ait != registry->aliases.end() && c.args.size() == 1)
          return ait->second.target;
        // Function call -> return type. Per-unit resolution avoids
        // the last-wins global-map pitfall.
        ResolveResult rr = resolve_name(id.name);
        if (rr.proc && rr.proc->return_type)
          return rr.proc->return_type.get();
      } else if (c.callee->kind == Kind::Member) {
        const auto& mem = static_cast<const Member&>(*c.callee);
        std::string cls;
        if (mem.base->kind == Kind::Ident &&
            static_cast<const Ident&>(*mem.base).name == "self") {
          cls = current_class_name;
        } else {
          const TypeExpr* bt = deduce_type(*mem.base);
          if (bt) cls = registry->direct_type_name(registry->canonicalize(bt));
        }
        if (!cls.empty()) {
          if (auto* cm = registry->lookup_class_member(cls, mem.name)) {
            if (cm->is_method && cm->method.decl && cm->method.decl->return_type)
              return cm->method.decl->return_type.get();
          }
        }
      }
      return nullptr;
    }
    case Kind::StringLit: {
      const auto& sl = static_cast<const StringLit&>(e);
      return sl.value.size() == 1 ? builtin_char_type()
                                  : builtin_string_type();
    }
    case Kind::AddrOf: {
      // Returning a pointer type would be ideal, but synthesising it on
      // the fly requires owning a TypeExpr we don't have. Unused today.
      return nullptr;
    }
    default:
      return nullptr;
  }
}

std::string Emitter::deduce_class_alias(const Expr& e) {
  if (!registry) return {};
  // Fast path for `self` -- we already know the class.
  if (e.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(e);
    if (id.name == "self") return current_class_name;
  }
  const TypeExpr* t = deduce_type(e);
  if (!t) return {};
  return registry->direct_type_name(registry->canonicalize(t));
}

// Strip a chain of primitive casts like `pointer(longint(x))` down to the
// underlying storage expression. Pascal uses these casts to satisfy type
// checking before reinterpreting bytes, so emit-time lvalue analysis has to
// look through them.
const Expr* Emitter::peel_primitive_casts(const Expr* e) {
  while (e && e->kind == Kind::Call) {
    const auto& c = static_cast<const Call&>(*e);
    if (c.args.size() != 1 || c.callee->kind != Kind::Ident) break;
    if (!is_primitive_type(static_cast<const Ident&>(*c.callee).name)) break;
    e = c.args[0].get();
  }
  return e;
}

// Decide whether an expression names mutable storage we can legally
// reinterpret in-place. This is stricter than "AST looks like an lvalue":
// parameterless methods auto-call in value context, and `inherited.name`
// can also lower to a call, so those must not be treated as addressable
// storage here.
bool Emitter::expr_is_storage_lvalue(const Expr& e) {
  const Expr* peeled = peel_primitive_casts(&e);
  const Expr& root = peeled ? *peeled : e;
  bool is_inherited_member = false;
  if (root.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(root);
    if (m.base->kind == Kind::Ident &&
        static_cast<const Ident&>(*m.base).name == "inherited") {
      is_inherited_member = true;
    }
  }
  bool is_lvalue_shape =
      !is_inherited_member &&
      (root.kind == Kind::Ident || root.kind == Kind::Member ||
       root.kind == Kind::Index || root.kind == Kind::Deref);
  if (root.kind == Kind::Ident &&
      local_untyped_params.count(static_cast<const Ident&>(root).name)) {
    is_lvalue_shape = true;
  }
  if (!is_lvalue_shape || !registry) return is_lvalue_shape;

  if (root.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(root);
    ResolveResult rr = resolve_name(id.name);
    if (rr.is_callable && rr.is_parameterless) return false;
  } else if (root.kind == Kind::Member) {
    const auto& m = static_cast<const Member&>(root);
    std::string cls = deduce_class_alias(*m.base);
    if (!cls.empty()) {
      if (auto* mem = registry->lookup_class_member(cls, m.name)) {
        if (mem->is_method && mem->method.param_count == 0) return false;
      }
    }
  }
  return true;
}

bool Emitter::expr_is_charish(const Expr& e) {
  const TypeExpr* t = deduce_type(e);
  if (!t) return false;
  t = canonicalize_type(t);
  return tyname_is(t, "char");
}

bool Emitter::type_is_stringish(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  if (!t) return false;
  if (t->kind == Kind::TyString) return true;
  return tyname_is(t, "string") || tyname_is(t, "shortstring");
}

bool Emitter::type_is_open_array(const TypeExpr* t) {
  if (!t) return false;
  t = canonicalize_type(t);
  return t && t->kind == Kind::TyArray &&
         static_cast<const TyArray&>(*t).dims.empty();
}

std::string Emitter::open_array_type_to_cxx(const TypeExpr& t) {
  const TypeExpr* canon = canonicalize_type(&t);
  const auto& a = static_cast<const TyArray&>(*canon);
  return "::rt::OpenArray<" +
         (a.element ? type_to_cxx(*a.element) : std::string("int32_t")) + ">";
}

void Emitter::mark_call_param_info(
    const ProcDecl* decl, std::vector<bool>& untyped_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (!decl) return;
  size_t ai = 0;
  for (const auto& p : decl->params) {
    for (size_t k = 0; k < p.names.size(); ++k) {
      if (ai < untyped_arg.size() && !p.type) untyped_arg[ai] = true;
      if (ai < param_types.size()) param_types[ai] = p.type.get();
      ++ai;
    }
  }
}

// Call emission only cares about parameter metadata for a narrow set of
// bootstrap-sensitive rewrites: untyped `var` arguments stay as raw storage
// pointers, `char` actuals may need string wrapping, and open arrays need the
// adapter type. Keep the lookup in one helper instead of restating it inside
// the giant call-expression path.
void Emitter::collect_call_param_info(
    const Expr& callee, std::vector<bool>& untyped_arg,
    std::vector<const TypeExpr*>& param_types) {
  if (!registry) return;
  if (callee.kind == Kind::Ident) {
    const auto& id = static_cast<const Ident&>(callee);
    if (!current_class_name.empty()) {
      if (auto* m = registry->lookup_class_member(current_class_name,
                                                  id.name)) {
        if (m->is_method) {
          mark_call_param_info(m->method.decl, untyped_arg, param_types);
          return;
        }
      }
    }
    ResolveResult rr = resolve_name(id.name);
    mark_call_param_info(rr.proc, untyped_arg, param_types);
    return;
  }
  if (callee.kind != Kind::Member) return;
  const auto& mem = static_cast<const Member&>(callee);
  std::string cls;
  if (mem.base->kind == Kind::Ident &&
      static_cast<const Ident&>(*mem.base).name == "self") {
    cls = current_class_name;
  } else {
    cls = deduce_class_alias(*mem.base);
  }
  if (cls.empty()) return;
  if (auto* m = registry->lookup_class_member(cls, mem.name)) {
    if (m->is_method) {
      mark_call_param_info(m->method.decl, untyped_arg, param_types);
    }
  }
}

std::string Emitter::lower_call_arg(const Expr& arg, const TypeExpr* param_type,
                                    bool untyped_arg) {
  std::string arg_text = expr_to_cxx(arg);
  if (type_is_stringish(param_type) && expr_is_charish(arg)) {
    arg_text = "::rt::ShortString<>(" + arg_text + ")";
  }
  if (type_is_open_array(param_type)) {
    const TypeExpr* at = deduce_type(arg);
    if (at) at = canonicalize_type(at);
    if (!(at && at->kind == Kind::TyArray &&
          static_cast<const TyArray&>(*at).dims.empty())) {
      arg_text = open_array_type_to_cxx(*param_type) + "(" + arg_text + ")";
    }
  }
  if (!untyped_arg) return arg_text;

  // Untyped Pascal params are already lowered as "pointer to caller storage".
  // Forwarding one of them must preserve the pointer value; taking `&` here
  // would pass the address of the local pointer slot instead.
  if (arg.kind == Kind::AddrOf &&
      !static_cast<const AddrOf&>(arg).double_addr) {
    return "((void*)(" + arg_text + "))";
  }
  if (arg.kind == Kind::Ident &&
      local_untyped_params.count(static_cast<const Ident&>(arg).name)) {
    return arg_text;
  }
  return "((void*)&(" + arg_text + "))";
}

size_t procedural_param_count(const TyProcedural& p) {
  size_t count = 0;
  for (const auto& pp : p.params) {
    count += pp.names.empty() ? 1 : pp.names.size();
  }
  return count;
}

// ---------------------------------------------------------------------------
// Single-point Pascal name resolution. `resolve_name` walks the real
// Pascal lookup order and returns a `ResolveResult` every emit site
// consumes uniformly; this avoids having the same "is it a method?
// is it a unit-qualified proc? should we auto-call?" logic grow in
// three different places in the emitter.

Emitter::ResolveResult Emitter::resolve_name(
    const std::string& name, QualifierKind qk, const std::string& qualifier) {
  ResolveResult r;

  // ----- Qualified lookups first: `Unit.name` / `Class.name`. -----
  if (qk == QualifierKind::Unit) {
    r.cxx = mangle(qualifier) + "::" + mangle(name);
    if (registry) {
      auto uit = registry->units.find(qualifier);
      if (uit != registry->units.end()) {
        const UnitInfo& u = uit->second;
        if (auto* pi = u.find_export_proc(name)) {
          r.kind = ResolvedKind::UnitProc;
          r.proc = pi->decl;
          r.is_callable = true;
          r.is_parameterless = (pi->param_count == 0);
          return r;
        }
        if (u.find_export_var(name)) { r.kind = ResolvedKind::UnitVar; return r; }
        if (u.find_export_const(name)) { r.kind = ResolvedKind::UnitConst; return r; }
        if (u.has_export_enum_member(name)) { r.kind = ResolvedKind::EnumMember; return r; }
        if (u.has_export_type(name)) { r.kind = ResolvedKind::UnitType; return r; }
      }
    }
    // RTL unit we don't parse (e.g. `dos.getenv` when dos.pas isn't
    // in our source tree). Leave the name unresolved and let the
    // stub header's `namespace alias = rt` do the final lookup.
    r.kind = ResolvedKind::Unknown;
    return r;
  }
  if (qk == QualifierKind::Class) {
    if (registry) {
      if (auto* m = registry->lookup_class_member(qualifier, name)) {
        if (m->is_method) {
          r.kind = ResolvedKind::ClassMethod;
          r.proc = m->method.decl;
          r.is_callable = true;
          r.is_parameterless = (m->method.param_count == 0);
        } else {
          r.kind = ResolvedKind::ClassField;
        }
        r.cxx = mangle(name);  // caller emits the `base.` prefix
        return r;
      }
    }
    r.cxx = mangle(name);
    r.kind = ResolvedKind::Unknown;
    return r;
  }

  // ----- Unqualified lookup. -----

  // 1. Function-name-as-read inside its own body -> `result`.
  if (current_fn_is_function && !current_fn_name.empty() &&
      name == current_fn_name) {
    r.cxx = "result";
    r.kind = ResolvedKind::ResultSlot;
    return r;
  }
  // 2. `with X do` bindings (inside-out). Fields and methods of X's
  //    class (walking ancestors) shadow outer scopes.
  if (registry) {
    for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
      std::string cls = it->type
                            ? registry->direct_type_name(
                                  registry->canonicalize(it->type))
                            : std::string{};
      if (cls.empty()) continue;
      if (auto* m = registry->lookup_class_member(cls, name)) {
        r.cxx = it->cxx_text + "." + mangle(name);
        if (m->is_method) {
          r.kind = ResolvedKind::WithMethod;
          r.proc = m->method.decl;
          r.is_callable = true;
          r.is_parameterless = (m->method.param_count == 0);
        } else {
          r.kind = ResolvedKind::WithField;
        }
        return r;
      }
      if (auto* f = registry->lookup_record_field(cls, name)) {
        (void)f;
        r.cxx = it->cxx_text + "." + mangle(name);
        r.kind = ResolvedKind::WithField;
        return r;
      }
    }
  }
  // 3. Nested parameterless function in the current scope -- stored
  //    as `std::function<T()>`, so a bare reference is NOT the value.
  {
    auto nit = local_nested_fns.find(name);
    if (nit != local_nested_fns.end()) {
      r.kind = ResolvedKind::NestedFn;
      r.cxx = mangle(name);
      r.is_callable = true;
      r.is_parameterless = (nit->second.param_count == 0);
      return r;
    }
  }
  // 4. Procedure-local (param, var, typed const, nested-proc-name).
  if (local_scope.count(name)) {
    r.kind = ResolvedKind::Local;
    r.cxx = mangle(name);
    return r;
  }
  // 5. Current class's members (chain).
  if (!current_class_name.empty() && registry) {
    if (auto* m = registry->lookup_class_member(current_class_name, name)) {
      r.cxx = mangle(name);
      if (m->is_method) {
        r.kind = ResolvedKind::ClassMethod;
        r.proc = m->method.decl;
        r.is_callable = true;
        r.is_parameterless = (m->method.param_count == 0);
      } else {
        r.kind = ResolvedKind::ClassField;
      }
      return r;
    }
  }
  // 6. Unit-level -- own unit first, then cross-unit (`uses` chain).
  if (registry) {
    auto uit = registry->units.find(current_unit_name);
    const UnitInfo* ui = (uit != registry->units.end())
                            ? &uit->second : nullptr;
    bool own = ui && ui->has(name);
    // Current unit's own symbols shadow everything from `uses`.
    // Emit bare (C++ picks them up in the current namespace).
    if (ui) {
      if (auto* pi = ui->find_proc(name)) {
        r.cxx = mangle(name);
        r.kind = ResolvedKind::UnitProc;
        r.proc = pi->decl;
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        return r;
      }
      if (ui->find_var(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitVar; return r;
      }
      if (ui->find_const(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitConst; return r;
      }
      if (ui->has_enum_member(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::EnumMember; return r;
      }
      if (ui->has_type(name)) {
        r.cxx = mangle(name); r.kind = ResolvedKind::UnitType; return r;
      }
    }
    // Cross-unit lookup: walk the current unit's `uses` list and pick
    // the first match in a unit that actually exports this name.
    // Ambiguity between same-named symbols in two `using namespace`'d
    // units is resolved by emitting the fully-qualified form.
    auto check_unit = [&](const std::string& un) -> bool {
      auto it = registry->units.find(un);
      if (it == registry->units.end()) return false;
      const UnitInfo& u = it->second;
      // Synthetic `__rt__` unit holds rt:: builtins. They live in
      // namespace `::rt` and every emitted unit injects
      // `using namespace ::rt;`, so emit the bare mangled name.
      const std::string prefix =
          (un == "__rt__") ? std::string() : (mangle(un) + "::");
      // Other units contribute only their interface-exported names.
      if (auto* pi = u.find_export_proc(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = (un == "__rt__") ? ResolvedKind::RtBuiltin
                                  : ResolvedKind::UnitProc;
        r.proc = pi->decl;
        r.is_callable = true;
        r.is_parameterless = (pi->param_count == 0);
        return true;
      }
      if (u.find_export_var(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitVar; return true;
      }
      if (u.find_export_const(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitConst; return true;
      }
      if (u.has_export_enum_member(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::EnumMember; return true;
      }
      if (u.has_export_type(name)) {
        r.cxx = prefix + mangle(name);
        r.kind = ResolvedKind::UnitType; return true;
      }
      return false;
    };
    if (ui) {
      // Right-to-left is Pascal's uses resolution order.
      for (auto it = ui->uses.rbegin(); it != ui->uses.rend(); ++it) {
        if (check_unit(*it)) return r;
      }
    }
    (void)own;  // already handled by the per-unit lookup above.
  }
  // 7. Fallback: emit the mangled name and let C++ lookup sort it out
  //    (catches rt:: free functions we haven't enumerated and any
  //    name that's in the current namespace).
  r.cxx = mangle(name);
  r.kind = ResolvedKind::Unknown;
  return r;
}

// ---------------------------------------------------------------------------
// Expressions (coarse -- just enough for constant values)

std::string Emitter::expr_to_cxx(const Expr& e) {
  switch (e.kind) {
    case Kind::IntLit: {
      const auto& n = static_cast<const IntLit&>(e);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", (long long)n.value);
      return buf;
    }
    case Kind::RealLit: {
      const auto& n = static_cast<const RealLit&>(e);
      return n.text;
    }
    case Kind::StringLit: {
      const auto& n = static_cast<const StringLit&>(e);
      // Single-character Pascal string literals are semantically chars.
      // Emit them as C++ character literals so they can appear as
      // subrange bounds (`'A'..'Z'`), case labels, and set-elements.
      // Multi-character literals are emitted as ShortString so that `+`
      // resolves to concatenation (not pointer arithmetic).
      auto escape_char_body = [](char c, bool in_char_literal) {
        std::string o;
        switch (c) {
          case '\\': o += "\\\\"; return o;
          case '\n': o += "\\n"; return o;
          case '\r': o += "\\r"; return o;
          case '\t': o += "\\t"; return o;
          case '\0': o += "\\0"; return o;
          default: break;
        }
        if (in_char_literal && c == '\'') { o += "\\'"; return o; }
        if (!in_char_literal && c == '"') { o += "\\\""; return o; }
        if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7f) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\x%02x", (unsigned char)c);
          o += esc;
          return o;
        }
        o.push_back(c);
        return o;
      };
      if (n.value.size() == 1) {
        return "::rt::p_char_of('" + escape_char_body(n.value[0], true) + "')";
      }
      // C++ `\xHH` escapes are greedy: any following hex digit gets
      // pulled into the escape ("\x01" + "7" would parse as "\x017"
      // which overflows). We close and reopen the string literal
      // between a non-printable (emitted as hex) and any subsequent
      // hex-digit character.
      auto is_hex_digit = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
      };
      std::string out = "::rt::ShortString<>(\"";
      bool prev_was_hex_escape = false;
      for (char c : n.value) {
        bool is_printable = (unsigned char)c >= 0x20 && (unsigned char)c < 0x7f
                            && c != '"' && c != '\\';
        if (prev_was_hex_escape && is_hex_digit(c)) {
          out += "\" \"";  // split into adjacent literals
        }
        out += escape_char_body(c, false);
        prev_was_hex_escape = !is_printable;
      }
      out += "\")";
      return out;
    }
    case Kind::NilLit: return "nullptr";
    case Kind::BoolLit: {
      const auto& n = static_cast<const BoolLit&>(e);
      return n.value ? "true" : "false";
    }
    case Kind::Ident: {
      const auto& n = static_cast<const Ident&>(e);
      if (n.name == "inherited") return "inherited{}";
      if (n.name == "self") return "(*this)";
      // LHS rewrite for `funcname := ...` assignments during Assign target
      // emission. We handle this BEFORE resolve_name so recursive
      // calls using `funcname(...)` still see the function name.
      if (!lhs_fn_rewrite.empty() && n.name == lhs_fn_rewrite) {
        return "result";
      }
      // The function-name-as-read rewrite is already in resolve_name
      // (only fires outside is_callee_context_), but we need to
      // suppress it in callee context to keep recursive call sites
      // spelled with the function's name.
      if (is_callee_context_ && current_fn_is_function &&
          !current_fn_name.empty() && n.name == current_fn_name) {
        return mangle(n.name);
      }
      ResolveResult rr = resolve_name(n.name);
      // At namespace scope (block_depth == 0) we leave callable
      // names bare: Pascal typed-const initialisers reference
      // function names as procedural-pointer values.
      bool want_call = !is_callee_context_ && block_depth > 0 &&
                       rr.is_callable && rr.is_parameterless;
      return want_call ? rr.cxx + "()" : rr.cxx;
    }
    case Kind::Binary: {
      const auto& n = static_cast<const Binary&>(e);
      // Pascal operators that don't map cleanly to a C++ infix operator
      // need dedicated lowering.
      if (n.op == BinOp::In) {
        // `elem in set` -> `set.contains(elem)`
        return "(" + expr_to_cxx(*n.rhs) + ").contains(" +
               expr_to_cxx(*n.lhs) + ")";
      }
      if (n.op == BinOp::SymDiff) {
        // Set symmetric difference `a >< b` -> `(a + b) - (a * b)` on our
        // Set<> type (rt::Set has union/intersect/subtract overloads).
        std::string a = expr_to_cxx(*n.lhs);
        std::string b = expr_to_cxx(*n.rhs);
        return "((" + a + " + " + b + ") - (" + a + " * " + b + "))";
      }
      if (n.op == BinOp::Is) {
        // `x is T` -> dynamic-cast-based check. Requires T as a type name.
        return "(dynamic_cast<" + expr_to_cxx(*n.rhs) + "*>(&(" +
               expr_to_cxx(*n.lhs) + ")) != nullptr)";
      }
      if (n.op == BinOp::As) {
        return "(*dynamic_cast<" + expr_to_cxx(*n.rhs) + "*>(&(" +
               expr_to_cxx(*n.lhs) + ")))";
      }
      // Pascal `+` on `char` operands means string concatenation
      // (produces a 2-char string). C++ `char + char` is int
      // arithmetic, so wrap a char-side in ShortString<> to force
      // the ShortString `operator+` overload.
      if (n.op == BinOp::Add) {
        bool l_char = expr_is_charish(*n.lhs);
        bool r_char = expr_is_charish(*n.rhs);
        if (l_char || r_char) {
          auto wrap = [&](const Expr& x, bool want) {
            return want ? "::rt::ShortString<>(" + expr_to_cxx(x) + ")"
                        : expr_to_cxx(x);
          };
          return "(" + wrap(*n.lhs, l_char) + " + " + wrap(*n.rhs, r_char) + ")";
        }
      }
      // Pascal `and` / `or` are polymorphic: bool operands get
      // short-circuit `&&` / `||` (crucial for `assigned(p) and
      // (p^.x = y)` idioms), integer/set operands get bitwise `&` /
      // `|`. Be strict here: treating a nested flag expression like
      // `(IF_SM or IF_SM2)` as "boolean because it is an `or`" silently
      // miscompiles bitmask code into `&&`/`||`.
      std::function<bool(const Expr&)> is_bool = [&](const Expr& x) -> bool {
        // Calls to rt:: builtins and user procs: consult the registry's
        // recorded return type (registry stores rt builtins under the
        // synthetic `__rt__` unit alongside user procs).
        if (x.kind == Kind::Call && registry) {
          const auto& c = static_cast<const Call&>(x);
          if (c.callee->kind == Kind::Ident) {
            const std::string& nm =
                static_cast<const Ident&>(*c.callee).name;
            auto pit = registry->procs.find(nm);
            if (pit != registry->procs.end() &&
                pit->second.return_type_name == "boolean")
              return true;
          }
        }
        // Comparisons always yield bool.
        if (x.kind == Kind::Binary) {
          const auto& bx = static_cast<const Binary&>(x);
          auto bop = bx.op;
          if (bop == BinOp::Eq || bop == BinOp::NotEq ||
              bop == BinOp::Lt || bop == BinOp::Gt ||
              bop == BinOp::LtEq || bop == BinOp::GtEq ||
              bop == BinOp::In || bop == BinOp::Is)
            return true;
          if (bop == BinOp::And || bop == BinOp::Or)
            return is_bool(*bx.lhs) && is_bool(*bx.rhs);
        }
        if (x.kind == Kind::Unary &&
            static_cast<const Unary&>(x).op == UnOp::Not)
          return is_bool(*static_cast<const Unary&>(x).operand);
        if (x.kind == Kind::BoolLit) return true;
        if (!registry) return false;
        const TypeExpr* t = deduce_type(x);
        if (!t) return false;
        t = registry->canonicalize(t);
        if (!t || t->kind != Kind::TyName) return false;
        std::string nm = static_cast<const TyName&>(*t).name;
        for (auto& c : nm)
          if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return nm == "boolean" || nm == "bytebool" ||
               nm == "wordbool" || nm == "longbool";
      };
      bool logical_bool = (n.op == BinOp::And || n.op == BinOp::Or) &&
                          is_bool(*n.lhs) && is_bool(*n.rhs);
      const char* op = "?";
      switch (n.op) {
        case BinOp::Add:    op = "+"; break;
        case BinOp::Sub:    op = "-"; break;
        case BinOp::Mul:    op = "*"; break;
        case BinOp::RealDiv:op = "/"; break;
        case BinOp::IntDiv: op = "/"; break;
        case BinOp::Mod:    op = "%"; break;
        case BinOp::Shl:    op = "<<"; break;
        case BinOp::Shr:    op = ">>"; break;
        case BinOp::And:    op = logical_bool ? "&&" : "&"; break;
        case BinOp::Or:     op = logical_bool ? "||" : "|"; break;
        case BinOp::Xor:    op = "^"; break;
        case BinOp::Eq:     op = "=="; break;
        case BinOp::NotEq:  op = "!="; break;
        case BinOp::Lt:     op = "<"; break;
        case BinOp::Gt:     op = ">"; break;
        case BinOp::LtEq:   op = "<="; break;
        case BinOp::GtEq:   op = ">="; break;
        default:            op = "/*?*/"; break;
      }
      return "(" + expr_to_cxx(*n.lhs) + " " + op + " " + expr_to_cxx(*n.rhs) + ")";
    }
    case Kind::Unary: {
      const auto& n = static_cast<const Unary&>(e);
      if (n.op == UnOp::Not) {
        // Pascal `not` is logical for bool, bitwise for int. Dispatch
        // at compile time via a runtime helper.
        return "::rt::p_not(" + expr_to_cxx(*n.operand) + ")";
      }
      const char* op = (n.op == UnOp::Neg) ? "-" : "+";
      return std::string(op) + expr_to_cxx(*n.operand);
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      // Classify the base into one of the qualifier kinds that
      // `resolve_name` understands. The base cases are:
      //   - `inherited.name`    -> class-qualified on parent alias
      //   - `Unit.name`         -> unit-qualified (Unit must be a
      //                            known unit or in the current
      //                            unit's `uses` list)
      //   - `expr.name` where   -> class-qualified on deduced type
      //     expr's type is a
      //     named class/record
      //   - otherwise           -> unknown: emit `base.name` and let
      //                            C++ member lookup do its thing.
      auto base_is_ident = [&](std::string& out) -> bool {
        if (m.base->kind != Kind::Ident) return false;
        out = static_cast<const Ident&>(*m.base).name;
        return true;
      };

      std::string base_name;
      // `inherited.foo` -- treat as class-qualified on the parent
      // alias (C++ `inherited::foo` via the in-struct `using
      // inherited = Parent;` alias).
      if (base_is_ident(base_name) && base_name == "inherited") {
        std::string parent;
        if (registry && !current_class_name.empty()) {
          auto cit = registry->classes.find(current_class_name);
          if (cit != registry->classes.end()) parent = cit->second.parent;
        }
        std::string text = "inherited::" + mangle(m.name);
        if (parent.empty()) return text;
        ResolveResult rr =
            resolve_name(m.name, QualifierKind::Class, parent);
        bool want_call = !is_callee_context_ &&
                         rr.is_callable && rr.is_parameterless;
        return want_call ? text + "()" : text;
      }

      // Pascal's `System` unit is implicitly used everywhere. Route
      // `System.x` straight to `::rt::x` so every builtin (delete,
      // length, copy, pos, ...) resolves without a per-method stub
      // on some `p_system` object.
      if (base_is_ident(base_name) && base_name == "system") {
        return "::rt::" + mangle(m.name);
      }
      // `Unit.name` -- only when the base ident names a known unit
      // AND isn't shadowed by any nearer binding.
      if (registry && base_is_ident(base_name)) {
        bool shadowed = local_scope.count(base_name) > 0;
        if (!shadowed && !current_class_name.empty() &&
            registry->lookup_class_member(current_class_name, base_name))
          shadowed = true;
        if (!shadowed) {
          for (auto it = with_stack.rbegin(); it != with_stack.rend(); ++it) {
            std::string cls = it->type
                                  ? registry->direct_type_name(
                                        registry->canonicalize(it->type))
                                  : std::string{};
            if (!cls.empty() &&
                (registry->lookup_class_member(cls, base_name) ||
                 registry->lookup_record_field(cls, base_name))) {
              shadowed = true; break;
            }
          }
        }
        if (!shadowed) {
          bool is_unit = registry->units.count(base_name) > 0;
          if (!is_unit) {
            auto uit = registry->units.find(current_unit_name);
            if (uit != registry->units.end()) {
              for (const auto& nm : uit->second.uses) {
                if (nm == base_name) { is_unit = true; break; }
              }
            }
          }
          if (is_unit) {
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Unit, base_name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.is_parameterless;
            return want_call ? rr.cxx + "()" : rr.cxx;
          }
          // `TClass.method` -- Pascal's way to call a specific
          // class's method (typically the parent's version from
          // inside an override). Emit `TClass::method`.
          if (registry->classes.count(base_name) ||
              registry->records.count(base_name)) {
            ResolveResult rr =
                resolve_name(m.name, QualifierKind::Class, base_name);
            std::string text = mangle(base_name) + "::" + mangle(m.name);
            bool want_call = !is_callee_context_ &&
                             rr.is_callable && rr.is_parameterless;
            return want_call ? text + "()" : text;
          }
        }
      }

      // Otherwise: object/record field/method access. Emit `base.name`
      // and auto-call if the deduced class has `name` as a
      // parameterless method.
      std::string base_cxx = expr_to_cxx(*m.base);
      std::string text = base_cxx + "." + mangle(m.name);
      if (is_callee_context_ || !registry) return text;
      std::string bcls = deduce_class_alias(*m.base);
      if (bcls.empty()) return text;
      ResolveResult rr = resolve_name(m.name, QualifierKind::Class, bcls);
      if (rr.is_callable && rr.is_parameterless) text += "()";
      return text;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      // `::rt::p_deref(p)` is equivalent to `*p` for typed pointers and
      // yields `char&` for `void*` so Pascal `ptr^` on untyped pointers
      // still compiles.
      return "::rt::p_deref(" + expr_to_cxx(*d.operand) + ")";
    }
    case Kind::AddrOf: {
      const auto& a = static_cast<const AddrOf&>(e);
      // `@TClass.method` is an unbound-method pointer in Pascal; the
      // C++ spelling uses `::` instead of `.`. Detect the AST pattern
      // `AddrOf(Member(Ident=TypeName, method))` where TypeName is a
      // known class/record alias in the registry.
      if (a.operand && a.operand->kind == Kind::Member) {
        const auto& m = static_cast<const Member&>(*a.operand);
        if (m.base && m.base->kind == Kind::Ident) {
          const auto& id = static_cast<const Ident&>(*m.base);
          if (registry &&
              (registry->classes.count(id.name) ||
               registry->records.count(id.name))) {
            return "(&" + mangle(id.name) + "::" + mangle(m.name) + ")";
          }
        }
      }
      // `@X` where X is a Pascal untyped-var parameter: X is already
      // `void*` holding the caller's storage address, so the
      // address-of-X is just X itself.
      if (a.operand && a.operand->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*a.operand);
        if (local_untyped_params.count(id.name)) {
          return "(" + mangle(id.name) + ")";
        }
      }
      bool saved = is_callee_context_;
      is_callee_context_ = true;
      std::string inner = expr_to_cxx(*a.operand);
      is_callee_context_ = saved;
      // Pascal `@arr` where `arr` is a flat byte-array (`array of
      // char` / `array of byte`) typically lands in a `pchar` or
      // `pointer` context -- the fpc compiler's fill buffers and
      // inline byte tables do exactly this. For that narrow case
      // emit `(::rt::p_char*)arr` using `rt::Array<byte>`'s pointer
      // decay. Anything deeper than one array level (e.g.
      // `array of array of char`) stays as `&arr` and the source
      // is expected to use a flatter spelling -- we do not paper
      // over nested-array type-punning at the translator level.
      if (registry) {
        const TypeExpr* ot = deduce_type(*a.operand);
        if (ot) ot = registry->canonicalize(ot);
        if (ot && ot->kind == Kind::TyArray) {
          const auto& ar = static_cast<const TyArray&>(*ot);
          const TypeExpr* elem = ar.element.get();
          if (elem) elem = registry->canonicalize(elem);
          if (elem && elem->kind == Kind::TyName) {
            std::string en = ascii_lower(static_cast<const TyName&>(*elem).name);
            if (en == "byte" || en == "char" || en == "uint8_t" ||
                en == "shortint") {
              return "((::rt::p_char*)(" + inner + "))";
            }
          }
        }
      }
      return "(&" + inner + ")";
    }
    case Kind::Call: {
      const auto& c = static_cast<const Call&>(e);
      // Only three Pascal builtins need special emit-time handling -- the
      // rest (length, ord, chr, assigned, odd, abs, sqr, sqrt, sin, cos,
      // ln, exp, arctan, trunc, round, int, frac, inc, dec, succ, pred,
      // ...) live in `rt::` under their exact Pascal names and pass through
      // the `using namespace ::rt;` injection. No translation table.
      //
      // Special cases below:
      //   * `low(T)` / `high(T)` when T is a type name  -> emitted constant
      //   * `sizeof(x)`                                 -> C++ `sizeof`
      //   * `TypeName(expr)` function-style cast        -> paren-cast when
      //                                                   the C++ type is
      //                                                   compound
      //   * `new(...)` / `dispose(...)`                 -> placement form
      if (c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        const std::string& n = id.name;
        auto arg0 = [&] {
          return c.args.empty() ? std::string("0") : expr_to_cxx(*c.args[0]);
        };

        // `low(T)`/`high(T)` on a type-name arg -> emitted enum constant.
        // `low(arr)`/`high(arr)` on an array value -> call `arr.low()` /
        // `arr.high()` (rt::Array exposes those as static methods).
        bool is_low_high_type = false;
        std::string low_high_rewrite;
        if ((n == "low" || n == "high") && c.args.size() == 1 &&
            c.args[0]->kind == Kind::Ident) {
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          // Function-local enum: the `__low`/`__high` constants live
          // right alongside us in the current block scope.
          if (local_enums.count(a.name)) {
            is_low_high_type = true;
            low_high_rewrite = mangle(a.name) + "__" + n;
          } else if (registry) {
            auto eit = registry->enums.find(a.name);
            if (eit != registry->enums.end()) {
              // Enum's `__low` / `__high` constants live in its
              // defining unit's namespace; qualify unless it's ours.
              is_low_high_type = true;
              const std::string& def = eit->second.defining_unit;
              if (def == current_unit_name) {
                low_high_rewrite = mangle(a.name) + "__" + n;
              } else {
                low_high_rewrite = mangle(def) + "::" + mangle(a.name) +
                                   "__" + n;
              }
            }
          }
          if (!is_low_high_type) {
            // Not a type name: may be an array-valued expression.
            // Resolve via type deduction and use rt::Array's low()/
            // high() static methods on the deduced array type.
            const TypeExpr* at = deduce_type(*c.args[0]);
            if (at) at = registry ? registry->canonicalize(at) : at;
            if (at && at->kind == Kind::TyArray) {
              is_low_high_type = true;
              low_high_rewrite = mangle(a.name) + "." + n + "()";
            }
          }
        }

        if (is_low_high_type) {
          return low_high_rewrite;
        } else if (n == "sizeof" && c.args.size() == 1) {
          // `sizeof(T)` on a type name -- map primitives to their C++
          // expansion, type aliases to their mangled form. `sizeof(expr)`
          // on any value expression stays as C++ `sizeof(expr)`.
          if (c.args[0]->kind == Kind::Ident) {
            const auto& tn = static_cast<const Ident&>(*c.args[0]);
            if (is_primitive_type(tn.name)) {
              return "sizeof(" + primitive_type_cxx(tn.name) + ")";
            }
            if (registry &&
                (registry->classes.count(tn.name) ||
                 registry->records.count(tn.name) ||
                 registry->enums.count(tn.name) ||
                 registry->aliases.count(tn.name))) {
              return "sizeof(" + mangle(tn.name) + ")";
            }
          }
          return "sizeof(" + expr_to_cxx(*c.args[0]) + ")";
        } else if (n == "typeof" && c.args.size() == 1 &&
                   c.args[0]->kind == Kind::Ident && registry) {
          // Pascal `typeof(T)` takes a TYPE NAME, not a value. In C++
          // we have no VMT-by-type-name runtime object; stub as
          // `nullptr` with a dummy template-arg tag so the expression
          // at least compiles. Users of this value compare it for
          // equality/inequality at runtime only.
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          if (registry->classes.count(a.name) ||
              registry->records.count(a.name)) {
            return "((void*)nullptr)";
          }
        } else if (c.args.size() == 1 && is_primitive_type(n)) {
          // Function-style type cast in expression context.
          // Only the explicit lvalue forms handled elsewhere
          // (`T(lv) := ...`, `inc(T(lv))`, `dec(T(lv))`) reinterpret
          // storage. Plain `T(expr)` remains a value conversion.
          if (n == "char") {
            return "::rt::p_chr(" + arg0() + ")";
          }
          if (n == "pointer" || n == "pchar" || n == "ppchar") {
            const Expr* peeled = peel_primitive_casts(c.args[0].get());
            if (peeled && expr_is_storage_lvalue(*c.args[0])) {
              return "::rt::p_reinterpret_ref<" + primitive_type_cxx(n) +
                     ">(" + expr_to_cxx(*peeled) + ")";
            }
          }
          if (expr_is_charish(*c.args[0])) {
            return "((" + primitive_type_cxx(n) + ")(::rt::p_ord(" +
                   arg0() + ")))";
          }
          return "((" + primitive_type_cxx(n) + ")(" + arg0() + "))";
        } else if (c.args.size() == 1 && n != "inc" && n != "dec") {
          const TypeExpr* cast_ty = nullptr;
          auto lit = local_type_aliases_scoped.find(n);
          if (lit != local_type_aliases_scoped.end()) {
            cast_ty = canonicalize_type(lit->second);
          } else if (registry) {
            auto ait = registry->aliases.find(n);
            if (ait != registry->aliases.end() && ait->second.target) {
              cast_ty = canonicalize_type(ait->second.target);
            }
          }
          if (cast_ty && cast_ty->kind == Kind::TyArray) {
            const auto& arr = static_cast<const TyArray&>(*cast_ty);
            const Expr* peeled = peel_primitive_casts(c.args[0].get());
            if (peeled && expr_is_storage_lvalue(*c.args[0])) {
              return "::rt::p_reinterpret_ref<" + expr_to_cxx(*c.callee) +
                     ">(" + expr_to_cxx(*peeled) + ")";
            }
            const TypeExpr* elem =
                arr.element ? canonicalize_type(arr.element.get()) : nullptr;
            if (arr.dims.size() == 1 &&
                (tyname_is(elem, "byte") || tyname_is(elem, "char"))) {
              return "::rt::p_reinterpret_bytes<" +
                     expr_to_cxx(*c.callee) + ">(" + arg0() + ")";
            }
          }
        } else if ((n == "inc" || n == "dec") &&
                   (c.args.size() == 1 || c.args.size() == 2) &&
                   c.args[0]->kind == Kind::Call) {
          // Pascal `inc(T(lv))` / `dec(T(lv))` mutate the storage behind
          // `lv` as type T. Emit that reinterpreting lvalue explicitly.
          const auto& inner = static_cast<const Call&>(*c.args[0]);
          if (std::string ref = primitive_cast_lvalue_ref(inner);
              !ref.empty()) {
            std::string op = (n == "inc") ? "::rt::p_inc" : "::rt::p_dec";
            if (c.args.size() == 2) {
              return op + "(" + ref + ", " + expr_to_cxx(*c.args[1]) + ")";
            }
            return op + "(" + ref + ")";
          }
          // Fall through to generic emission.
        } else if (n == "new" && !c.args.empty()) {
          // Expression-form `new(T)` or `new(T, Ctor(args))`. The first
          // arg is the *pointer-type name* (an Ident), which we already
          // emit as `p_T` -- the underlying struct is
          // `std::remove_pointer_t<p_T>`.
          // STUB: if the type is one of our stub target-back-end
          // aliases (t_win32 / t_os2 / t_go32v* classes that got
          // skipped), emit `nullptr` -- the call site is inside an
          // unreachable `case target_info.target of` arm.
          if (c.args[0]->kind == Kind::Ident) {
            const std::string& tname =
                static_cast<const Ident&>(*c.args[0]).name;
            static const std::unordered_set<std::string> stub_targets = {
                "pimportlibwin32", "timportlibwin32",
                "pimportlibos2",   "timportlibos2",
                "pimportlibgo32v2","timportlibgo32v2",
                "pexportlibwin32", "texportlibwin32",
                "pexportlibos2",   "texportlibos2",
                "pexportlibgo32v2","texportlibgo32v2",
                "plinkerwin32",    "tlinkerwin32",
                "plinkeros2",      "tlinkeros2",
                "plinkergo32v1",   "tlinkergo32v1",
                "plinkergo32v2",   "tlinkergo32v2",
            };
            if (stub_targets.count(tname)) return "nullptr";
          }
          std::string t = expr_to_cxx(*c.args[0]);
          std::string make =
              "new ::std::remove_pointer_t<" + t + ">{}";
          if (c.args.size() == 1) return make;
          // c.args[1] is either Call(Ctor, args) or Ident(Ctor).
          std::string method;
          std::string margs;
          const auto& second = *c.args[1];
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            for (size_t i = 0; i < cc.args.size(); ++i) {
              if (i) margs += ", ";
              margs += expr_to_cxx(*cc.args[i]);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          return "([&]{ auto __p = " + make + "; __p->" + method + "(" +
                 margs + "); return __p; }())";
        }
      }
      // Pointer cast `T(lv)` where T resolves to a pointer type AND
      // the argument is an addressable expression (Ident, Member,
      // Index, Deref): emit `(*(T*)&(lv))` so the result is an lvalue
      // and can bind to a `var`-parameter reference. Pascal routinely
      // casts pointer storage this way
      // (e.g. `resolvederef(pderef(def), ...)`). If the argument is
      // an rvalue (a call result, arithmetic, another cast, or a
      // parameterless-method access like `inherited.name` which the
      // emitter silently calls), a plain functional cast works and
      // an address-of would not compile.
      if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
        const auto& id = static_cast<const Ident&>(*c.callee);
        bool cast_to_pointer = false;
        std::string cast_type_cxx;
        if (id.name == "pointer") {
          cast_to_pointer = true;
          cast_type_cxx = "void*";
        } else if (registry) {
          auto ait = registry->aliases.find(id.name);
          if (ait != registry->aliases.end() && ait->second.target) {
            const TypeExpr* tgt = registry->canonicalize(ait->second.target);
            if (tgt && tgt->kind == Kind::TyPointer) {
              cast_to_pointer = true;
              cast_type_cxx = mangle(id.name);
            }
          }
        }
        if (cast_to_pointer) {
          const Expr* peeled = peel_primitive_casts(c.args[0].get());
          if (peeled && expr_is_storage_lvalue(*c.args[0])) {
            return "(*(" + cast_type_cxx + "*)&(" +
                   expr_to_cxx(*peeled) + "))";
          }
        }
      }
      is_callee_context_ = true;
      std::string callee_text = expr_to_cxx(*c.callee);
      is_callee_context_ = false;
      bool is_tpexcept_setjmp = false;
      if (c.args.size() == 1) {
        if (c.callee->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*c.callee);
          if (id.name == "setjmp") {
            ResolveResult rr = resolve_name(id.name);
            is_tpexcept_setjmp = (rr.cxx == "p_tpexcept::p_setjmp");
          }
        } else if (c.callee->kind == Kind::Member) {
          const auto& mem = static_cast<const Member&>(*c.callee);
          if (mem.name == "setjmp" && mem.base->kind == Kind::Ident &&
              static_cast<const Ident&>(*mem.base).name == "tpexcept") {
            is_tpexcept_setjmp = true;
          }
        }
      }
      if (is_tpexcept_setjmp) {
        return "setjmp(p_tpexcept::p_detail::p_state_for(&(" +
               expr_to_cxx(*c.args[0]) + ")).p_env)";
      }
      // Collect per-arg "untyped-var" flags from the callee's proc
      // decl so we can wrap those args with `(void*)&(arg)`. Handles
      // two shapes: Ident callees (unit-level procs) and Member
      // callees (methods whose class we can resolve).
      std::vector<bool> untyped_arg(c.args.size(), false);
      std::vector<const TypeExpr*> param_types(c.args.size(), nullptr);
      collect_call_param_info(*c.callee, untyped_arg, param_types);
      std::string out = callee_text + "(";
      for (size_t i = 0; i < c.args.size(); ++i) {
        if (i) out += ", ";
        out += lower_call_arg(*c.args[i], param_types[i], untyped_arg[i]);
      }
      out += ")";
      return out;
    }
    case Kind::Index: {
      const auto& i = static_cast<const Index&>(e);
      std::string out = expr_to_cxx(*i.base);
      for (const auto& idx : i.indices) out += "[" + expr_to_cxx(*idx) + "]";
      return out;
    }
    case Kind::SetLit: {
      const auto& s = static_cast<const SetLit&>(e);
      // Fast path when there are no range-elements: the deduction-friendly
      // set_of helper works.
      bool has_range = false;
      for (const auto& el : s.elements) {
        if (el->kind == Kind::Range) { has_range = true; break; }
      }
      if (s.elements.empty()) {
        // `[]` in Pascal: untyped empty set. EmptySet converts to any
        // `Set<T>` implicitly, so the value is usable in any set
        // context.
        return "::rt::EmptySet{}";
      }
      if (!has_range) {
        // Variadic-pack form so the element types don't have to
        // match exactly (Pascal set literals freely mix e.g. a
        // CharConst `p_newline` with plain char literals like
        // `'\r'`, `';'`). The Set's element type is deduced from
        // the first argument.
        std::string out = "::rt::set_of(";
        for (size_t i = 0; i < s.elements.size(); ++i) {
          if (i) out += ", ";
          out += expr_to_cxx(*s.elements[i]);
        }
        out += ")";
        return out;
      }
      // Slow path: mixed scalar + range elements. Build a Set in an IIFE
      // whose element type is deduced from the first element (either a
      // scalar value or a range low-bound). Use `[&]` inside function
      // bodies (may reference outer locals); use `[]` at namespace scope
      // where `[&]` is invalid.
      std::string first;
      if (s.elements.front()->kind == Kind::Range) {
        first = expr_to_cxx(
            *static_cast<const Range&>(*s.elements.front()).lo);
      } else {
        first = expr_to_cxx(*s.elements.front());
      }
      std::string body =
          "::rt::Set<decltype(" + first + ")> __s;";
      for (const auto& el : s.elements) {
        if (el->kind == Kind::Range) {
          const auto& r = static_cast<const Range&>(*el);
          std::string lo = expr_to_cxx(*r.lo);
          std::string hi = expr_to_cxx(*r.hi);
          body += " for (int64_t __v = (int64_t)(" + lo +
                  "); __v <= (int64_t)(" + hi +
                  "); ++__v) __s.add(static_cast<decltype(" + first +
                  ")>(__v));";
        } else {
          body += " __s.add(" + expr_to_cxx(*el) + ");";
        }
      }
      const char* cap = (block_depth > 0) ? "[&]" : "[]";
      return std::string("(") + cap + "{ " + body + " return __s; }())";
    }
    case Kind::Range: {
      const auto& r = static_cast<const Range&>(e);
      return "::rt::range(" + expr_to_cxx(*r.lo) + ", " + expr_to_cxx(*r.hi) + ")";
    }
    case Kind::ArrayConst: {
      const auto& a = static_cast<const ArrayConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < a.elements.size(); ++i) {
        if (i) out += ", ";
        out += expr_to_cxx(*a.elements[i]);
      }
      out += "}";
      return out;
    }
    case Kind::RecordConst: {
      const auto& r = static_cast<const RecordConst&>(e);
      std::string out = "{";
      for (size_t i = 0; i < r.fields.size(); ++i) {
        if (i) out += ", ";
        out += "." + mangle(r.fields[i].first) + " = " +
               expr_to_cxx(*r.fields[i].second);
      }
      out += "}";
      return out;
    }
    default:
      return "/* unsupported-expr */ 0";
  }
}

std::string Emitter::const_value_to_cxx(const Expr& e) { return expr_to_cxx(e); }

// ---------------------------------------------------------------------------
// Declarations

void Emitter::emit_const_decl(const ConstDecl& cd, bool in_header) {
  const std::string name = mangle(cd.name);
  std::string val = const_value_to_cxx(*cd.value);

  // Two things drive the qualifiers:
  //   `inline` -- required on definitions at namespace scope in a header so
  //              multiple translation units that include it don't violate
  //              ODR. Invalid at block scope, so we drop it there.
  //   `const`  -- Pascal's UNTYPED const (`const X = 5;`) is immutable,
  //              TYPED const (`const X : T = 5;`) is writable.
  const bool block = block_depth > 0;
  const std::string linkage = block ? std::string() : std::string("inline ");
  const TypeExpr* typed_const_ty =
      cd.type ? canonicalize_type(cd.type.get()) : nullptr;

  // Typed array (or named alias ultimately resolving to one) with an
  // array-constant initialiser emits an `rt::Array<T, Lo, N>` so
  //   (a) the size is known even when the index is an enum (Pascal),
  //   (b) the array has value-copy semantics on pass (Pascal),
  //   (c) `arr[Lo]` picks the first element (Pascal arbitrary low bound).
  if (cd.type && cd.value->kind == Kind::ArrayConst) {
    // Chase through named aliases (cross-unit-aware) until we see the
    // underlying TyArray.
    const TypeExpr* t = typed_const_ty;
    if (t && t->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*t);
      // Wrap the element type in `Array<..., Lo, N>` for each dim
      // from innermost to outermost.
      std::string ty = arr.element ? type_to_cxx(*arr.element)
                                   : std::string("int32_t");
      for (auto it = arr.dims.rbegin(); it != arr.dims.rend(); ++it) {
        std::string lo, size_expr;
        if (!array_dim_bounds_to_cxx(**it, &lo, &size_expr)) {
          // Still unknown -- fall through to the generic emit below.
          goto generic_emit;
        }
        ty = "::rt::Array<" + ty + ", " + lo + ", " + size_expr + ">";
      }
      emitln(linkage + ty + " " + name + " = " + val + ";");
      return;
    }
  }
generic_emit:;

  if (cd.type) {
    // Typed Pascal const -- writable.
    if (typed_const_ty && typed_const_ty->kind == Kind::TyArray &&
        cd.value->kind != Kind::StringLit) {
      emitln(linkage + type_to_cxx(*cd.type) + " " + name + " = {" + val +
             "};");
      return;
    }
    emitln(linkage + type_to_cxx(*cd.type) + " " + name + " = " + val + ";");
    return;
  }

  // Untyped Pascal const -- immutable.
  //   - Single-char string literal: wrap in `rt::CharConst` so it's
  //     usable as both `p_char` (Pascal char) and `ShortString<>`
  //     (Pascal string) by context, matching Pascal's polymorphic
  //     `const X = 'c';` semantics.
  //   - Multi-char string literal: plain `ShortString<>` so `+`
  //     resolves to concatenation.
  if (cd.value->kind == Kind::StringLit) {
    const auto& sl = static_cast<const StringLit&>(*cd.value);
    if (sl.value.size() == 1) {
      // Direct-init (`{...}`) because CharConst's ctor is explicit
      // -- see prelude.h for why.
      emitln(linkage + "constexpr ::rt::CharConst " + name + "{" +
             val + "};");
    } else {
      emitln(linkage + "const ::rt::ShortString<> " + name + " = " +
             val + ";");
    }
    return;
  }
  emitln(linkage + "const auto " + name + " = " + val + ";");
}

void Emitter::emit_type_decl(const TypeDecl& td, bool) {
  const std::string name = mangle(td.name);

  // Pascal enums are unscoped: members leak into the enclosing namespace
  // and are referenced directly. We emit a plain `enum` (not `enum class`)
  // with an explicit underlying type so the members are usable bare.
  //
  // We also emit per-enum `__low` / `__high` constants -- Pascal's
  // `low(T)` / `high(T)` take a type name as argument; we rewrite those
  // calls to the constants at emit time.
  if (td.type && td.type->kind == Kind::TyEnum) {
    const auto& te = static_cast<const TyEnum&>(*td.type);
    emitln("enum " + name + " : int32_t {");
    indent();
    for (size_t i = 0; i < te.members.size(); ++i) {
      std::string m = mangle(te.members[i]);
      if (i + 1 < te.members.size()) m += ",";
      emitln(m);
    }
    dedent();
    emitln("};");
    if (!te.members.empty()) {
      // `inline` is not permitted on block-scope variables, so omit it
      // when we're emitting inside a function body (local type decl).
      const char* lin = (block_depth > 0) ? "" : "inline ";
      emitln(std::string(lin) + "constexpr " + name + " " + name +
             "__low = " + mangle(te.members.front()) + ";");
      emitln(std::string(lin) + "constexpr " + name + " " + name +
             "__high = " + mangle(te.members.back()) + ";");
    }
    return;
  }

  if (td.type && td.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*td.type);
    // Packed compiler records are often byte-for-byte file layouts that still
    // need ordinary field access in emitted C++. `[[gnu::packed]]` preserves
    // the layout, but GCC then treats those fields as potentially misaligned
    // packed members and rejects many non-const reference bindings. `#pragma
    // pack` gives the byte layout we need here while keeping the generated
    // field expressions usable.
    if (tr.is_packed) emitln("#pragma pack(push, 1)");
    emitln("struct " + name + " {");
    indent();
    for (const auto& f : tr.fields) {
      std::string ft = f.type ? type_to_cxx(*f.type) : std::string("int32_t");
      for (const auto& fn : f.names) {
        emitln(ft + " " + mangle(fn) + ";");
      }
    }
    if (tr.has_variant) {
      // Pascal variant records expose their case-fields directly on the
      // outer record -- `rec.fieldOfCase1` works without saying which case.
      // We match that by emitting one anonymous struct per case inside an
      // anonymous union. GCC accepts this as an extension.
      if (!tr.variant_tag_name.empty() && tr.variant_tag_type) {
        emitln(type_to_cxx(*tr.variant_tag_type) + " " +
               mangle(tr.variant_tag_name) + ";");
      }
      emitln("union {");
      indent();
      for (const auto& vc : tr.variant_cases) {
        if (vc.fields.empty()) continue;
        emitln("struct {");
        indent();
        for (const auto& f : vc.fields) {
          std::string ft = f.type ? type_to_cxx(*f.type) : std::string("int32_t");
          for (const auto& fn : f.names) {
            emitln(ft + " " + mangle(fn) + ";");
          }
        }
        dedent();
        emitln("};");
      }
      dedent();
      emitln("};");
    }
    dedent();
    emitln("};");
    if (tr.is_packed) emitln("#pragma pack(pop)");
    return;
  }

  if (td.type && td.type->kind == Kind::TyObject) {
    const auto& to = static_cast<const TyObject&>(*td.type);
    std::string line = "struct " + name;
    if (!to.parent.empty()) line += " : public " + mangle(to.parent);
    line += " {";
    emitln(line);
    indent();
    // Pascal `inherited X` is unambiguous under single inheritance: it
    // refers to the parent object's X. Emit `using inherited = Parent;`
    // so the method-body translator can rewrite `inherited X` to
    // `inherited::p_X`. The alias is named `inherited` (bare, no p_
    // prefix) because `inherited` is a Pascal keyword -- user code cannot
    // declare a field or variable with that name, so the alias is
    // guaranteed collision-free.
    if (!to.parent.empty()) {
      emitln("using inherited = " + mangle(to.parent) + ";");
    }
    for (const auto& m : to.members) {
      if (m.is_field) {
        std::string ft = m.field_type ? type_to_cxx(*m.field_type)
                                      : std::string("int32_t");
        for (const auto& fn : m.field_names) {
          emitln(ft + " " + mangle(fn) + ";");
        }
      } else {
        // Method signature. We do NOT emit the body here -- bodies live in
        // the implementation .cc, emitted as `ret Class::method(...) { }`.
        const auto& pd = static_cast<const ProcDecl&>(*m.method);
        std::string ret;
        if (pd.pkind == ProcKind::Function && pd.return_type) {
          ret = type_to_cxx(*pd.return_type);
        } else if (pd.pkind == ProcKind::Constructor) {
          ret = "bool";  // Pascal constructors return failure status
        } else {
          ret = "void";
        }
        std::string prefix;
        if (pd.is_virtual || pd.is_abstract || pd.is_override) {
          prefix = "virtual ";
        }
        std::string suffix;
        if (pd.is_abstract) suffix = " = 0";
        else if (pd.is_override) suffix = " override";
        emitln(prefix + ret + " " + mangle(pd.name) + "(" +
               param_list_to_cxx(pd.params) + ")" + suffix + ";");
      }
    }
    dedent();
    emitln("};");
    return;
  }

  // Ordinary alias.
  std::string rhs = td.type ? type_to_cxx(*td.type) : std::string("int32_t");
  emitln("using " + name + " = " + rhs + ";");
}

void Emitter::emit_var_decl(const VarDecl& vd, bool in_header) {
  if (vd.is_absolute) {
    report_error(vd.loc, "absolute variables are unsupported");
    return;
  }
  if (vd.is_external) {
    report_error(vd.loc, "external variables are unsupported");
    return;
  }
  std::string ty = vd.type ? type_to_cxx(*vd.type) : std::string("int32_t");
  for (const auto& n : vd.names) {
    std::string name = mangle(n);
    if (in_header) {
      emitln("extern " + ty + " " + name + ";");
    } else {
      if (vd.init) {
        emitln(ty + " " + name + " = " + expr_to_cxx(*vd.init) + ";");
      } else {
        emitln(ty + " " + name + ";");
      }
    }
  }
}

std::string Emitter::param_list_to_cxx(const std::vector<Param>& params) {
  std::string out;
  bool first = true;
  for (const auto& p : params) {
    // Untyped `var X` / `const X` / `X` in Pascal means "pass the
    // storage, any type". The closest C++ is a raw address: `void*`
    // (no reference). Inside the body, Pascal's `@X` becomes just
    // `X` (the pointer itself). At call sites we wrap the argument
    // with `(void*)&(arg)` to pass the caller's storage address.
    std::string pt;
    if (!p.type) {
      pt = "void*";
    } else {
      if (p.type->kind == Kind::TyArray &&
          static_cast<const TyArray&>(*p.type).dims.empty()) {
        pt = open_array_type_to_cxx(*p.type);
      } else {
        pt = type_to_cxx(*p.type);
      }
      if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
      else if (p.mode == Param::Const) {
        if (const_param_needs_mutable_ref(p.type.get())) pt += "&";
        else pt = std::string("const ") + pt + "&";
      }
    }
    for (const auto& n : p.names) {
      if (!first) out += ", ";
      first = false;
      out += pt + " " + mangle(n);
    }
    if (p.names.empty()) {
      if (!first) out += ", ";
      first = false;
      out += pt;
    }
  }
  return out;
}

void Emitter::emit_proc_decl_signature(const ProcDecl& pd) {
  if (pd.is_external) {
    report_error(pd.loc, "external routines are unsupported");
    return;
  }
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  std::string params = param_list_to_cxx(pd.params);
  emitln(ret + " " + mangle(pd.name) + "(" + params + ");");
}

void Emitter::emit_decl(const Decl& d, bool in_header) {
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
        report_error(pd.loc, "external routines are unsupported");
      } else if (pd.is_forward) {
        // Pascal `forward;` in the impl section means "the body
        // comes later in this same unit". C++ needs a prototype
        // up-front so calls earlier in the file resolve.
        emit_proc_decl_signature(pd);
      } else if (!pd.is_external && !pd.is_abstract && pd.body) {
        if (block_depth > 0) {
          // Nested proc. C++ forbids nested function definitions; emit
          // as a lambda captured by reference so it sees the enclosing
          // routine's locals. std::function so the lambda name is in
          // scope inside its own body (required for recursion).
          emit_nested_proc_lambda(pd);
        } else {
          emit_proc_body(pd);
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

// ---------------------------------------------------------------------------
// Statements

void Emitter::emit_stmt(const Stmt& s) {
  switch (s.kind) {
    case Kind::Compound: {
      const auto& c = static_cast<const Compound&>(s);
      emitln("{");
      indent();
      for (const auto& sub : c.body) emit_stmt(*sub);
      dedent();
      emitln("}");
      break;
    }
    case Kind::EmptyStmt: {
      emitln(";");
      break;
    }
    case Kind::Assign: {
      const auto& a = static_cast<const Assign&>(s);
      // Pascal `T(lv) := rhs` writes through a cast view of the same
      // storage. Emit that storage reinterpret explicitly.
      if (a.target->kind == Kind::Call) {
        const auto& c = static_cast<const Call&>(*a.target);
        if (c.args.size() == 1 && c.callee->kind == Kind::Ident) {
          if (std::string ref = primitive_cast_lvalue_ref(c); !ref.empty()) {
            emitln(ref + " = " + expr_to_cxx(*a.value) + ";");
            break;
          }
          const auto& id = static_cast<const Ident&>(*c.callee);
          if (registry) {
            auto ait = registry->aliases.find(id.name);
            if (ait != registry->aliases.end() && ait->second.target) {
              const TypeExpr* tgt = registry->canonicalize(ait->second.target);
              if (tgt && tgt->kind == Kind::TyPointer) {
                std::string lv = expr_to_cxx(*c.args[0]);
                std::string rhs = expr_to_cxx(*a.value);
                emitln("(*(" + mangle(id.name) + "*)&(" + lv + ")) = " +
                       rhs + ";");
                break;
              }
            }
          }
        }
      }
      // Enable LHS-rewrite for the function name so that Pascal
      // `funcname := x`, `funcname[i] := x`, `funcname.field := x` etc.
      // all route to the result slot. We only scope the rewrite to the
      // target emission so the RHS still sees the function for recursive
      // calls.
      lhs_fn_rewrite = current_fn_name;
      std::string target_cxx = expr_to_cxx(*a.target);
      lhs_fn_rewrite.clear();
      std::string rhs_cxx = expr_to_cxx(*a.value);
      if (type_is_stringish(deduce_type(*a.target)) &&
          expr_is_charish(*a.value)) {
        rhs_cxx = "::rt::ShortString<>(" + rhs_cxx + ")";
      }
      emitln(target_cxx + " = " + rhs_cxx + ";");
      break;
    }
    case Kind::ExprStmt: {
      const auto& es = static_cast<const ExprStmt&>(s);
      // Pascal builtin control-flow statements (break / continue / exit)
      // and allocation builtins (new / dispose) need special lowering.
      // Classify once, then handle in a single if/else chain.
      std::string name;
      const Call* call_expr = nullptr;
      if (es.expr->kind == Kind::Ident) {
        name = static_cast<const Ident&>(*es.expr).name;
      } else if (es.expr->kind == Kind::Call) {
        call_expr = &static_cast<const Call&>(*es.expr);
        if (call_expr->callee->kind == Kind::Ident) {
          name = static_cast<const Ident&>(*call_expr->callee).name;
        }
      }

      if (name == "break") {
        // Pascal break exits the enclosing loop even from inside a case.
        // Emit as goto so switch nesting can't swallow it.
        if (!loop_break_labels.empty()) {
          emitln("goto " + loop_break_labels.back() + ";");
        } else {
          emitln("break;");  // outside any loop -- let C++ diagnose
        }
      } else if (name == "continue") {
        if (!loop_continue_labels.empty()) {
          emitln("goto " + loop_continue_labels.back() + ";");
        } else {
          emitln("continue;");
        }
      } else if (name == "exit") {
        // exit or exit(v). In a Function, fill the result slot and return;
        // in a Procedure, return; in a Constructor, return the status.
        if (call_expr && !call_expr->args.empty() && current_fn_is_function) {
          emitln("result = " + expr_to_cxx(*call_expr->args[0]) + ";");
          emitln("return result;");
        } else if (current_fn_is_function || current_fn_is_ctor) {
          emitln("return result;");
        } else {
          emitln("return;");
        }
      } else if (name == "fail") {
        if (current_fn_is_ctor) {
          emitln("result = false;");
          emitln("return result;");
        } else {
          report_error(es.loc, "`fail` outside constructors is unsupported");
        }
      } else if (name == "new" && call_expr && !call_expr->args.empty()) {
        // new(p) or new(p, Ctor(args)). `p` might be `arr[i]` whose
        // `decltype` is a reference (`T&`); strip it before computing
        // the pointee so `new remove_pointer_t<T&>` doesn't arise.
        std::string p = expr_to_cxx(*call_expr->args[0]);
        emitln(p + " = new ::std::remove_pointer_t<"
                   "::std::remove_reference_t<decltype(" + p + ")>>{};");
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          std::string args;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
            for (size_t i = 0; i < cc.args.size(); ++i) {
              if (i) args += ", ";
              args += expr_to_cxx(*cc.args[i]);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) {
            emitln("(*" + p + ")." + method + "(" + args + ");");
          }
        }
      } else if (name == "dispose" && call_expr && !call_expr->args.empty()) {
        // dispose(p) or dispose(p, Done)
        std::string p = expr_to_cxx(*call_expr->args[0]);
        if (call_expr->args.size() >= 2) {
          const auto& second = *call_expr->args[1];
          std::string method;
          if (second.kind == Kind::Call) {
            const auto& cc = static_cast<const Call&>(second);
            if (cc.callee->kind == Kind::Ident) {
              method = mangle(static_cast<const Ident&>(*cc.callee).name);
            }
          } else if (second.kind == Kind::Ident) {
            method = mangle(static_cast<const Ident&>(second).name);
          }
          if (!method.empty()) emitln("(*" + p + ")." + method + "();");
        }
        emitln("delete " + p + ";");
        emitln(p + " = nullptr;");
      } else {
        // `expr_to_cxx` auto-calls parameterless procs/methods in
        // value context via `resolve_name`. The one extra case we
        // handle here: Pascal statement-form `writeln;` / `readln;`
        // / `halt;` -- rt variadic builtins where 0 args is a
        // legitimate call. We don't try to auto-call parameterful
        // callables -- those are either emitted as `Call` (handled
        // above with args) or they're real source bugs (like an
        // `inherited init;` whose parent wants an arg) that should
        // be fixed in the Pascal, not papered over.
        std::string text = expr_to_cxx(*es.expr);
        if (es.expr->kind == Kind::Ident && registry) {
          const auto& id = static_cast<const Ident&>(*es.expr);
          auto pit = registry->procs.find(id.name);
          if (pit != registry->procs.end() &&
              pit->second.accepts_zero_args &&
              !text.empty() && text.back() != ')') {
            text += "()";
          }
        }
        // Parameterless procedural variables are callable in statement
        // position (`olddo_stop;`) but must stay as plain values in
        // assignments like `do_stop := olddo_stop;`. Detect that only here.
        if ((es.expr->kind == Kind::Ident || es.expr->kind == Kind::Member) &&
            !text.empty() && text.back() != ')') {
          if (const TypeExpr* t = deduce_type(*es.expr);
              t && (t = canonicalize_type(t)) &&
              t->kind == Kind::TyProcedural) {
            const auto& p = static_cast<const TyProcedural&>(*t);
            if (procedural_param_count(p) == 0) text += "()";
          }
        }
        emitln(text + ";");
      }
      break;
    }
    case Kind::If: {
      const auto& i = static_cast<const If&>(s);
      emitln("if (" + expr_to_cxx(*i.cond) + ") {");
      indent();
      if (i.then_branch) emit_stmt(*i.then_branch);
      dedent();
      if (i.else_branch) {
        emitln("} else {");
        indent();
        emit_stmt(*i.else_branch);
        dedent();
      }
      emitln("}");
      break;
    }
    case Kind::While: {
      const auto& w = static_cast<const While&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "__ploop_brk_" + n;
      std::string cont = "__ploop_cnt_" + n;
      emitln("while (" + expr_to_cxx(*w.cond) + ") {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (w.body) emit_stmt(*w.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::Repeat: {
      const auto& r = static_cast<const Repeat&>(s);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "__ploop_brk_" + n;
      std::string cont = "__ploop_cnt_" + n;
      emitln("do {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      for (const auto& sub : r.body) emit_stmt(*sub);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      dedent();
      emitln("} while (!(" + expr_to_cxx(*r.cond) + "));");
      emitln(brk + ":;");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      std::string var = mangle(f.var);
      std::string from = expr_to_cxx(*f.from);
      std::string to = expr_to_cxx(*f.to);
      std::string n = std::to_string(++loop_label_counter);
      std::string brk = "__ploop_brk_" + n;
      std::string cont = "__ploop_cnt_" + n;
      // Pascal `for X := A to B do S` is NOT `for (X=A; X<=B; ++X)`:
      // when X's type is `byte` and B is 255, ++X wraps to 0 and the
      // condition never fails. True semantics: body runs for each X in
      // [A,B]; terminate by equality after the body. Snapshot the end
      // bound so mid-body assignments to B don't alter the loop count.
      emitln("{");
      indent();
      emitln("auto __pfrom = (" + from + ");");
      emitln("auto __pto = (" + to + ");");
      const char* cmp = f.downto ? ">=" : "<=";
      const char* step = f.downto ? "::rt::p_dec" : "::rt::p_inc";
      emitln(std::string("if (__pfrom ") + cmp + " __pto) {");
      indent();
      emitln(var + " = __pfrom;");
      emitln("while (true) {");
      indent();
      loop_break_labels.push_back(brk);
      loop_continue_labels.push_back(cont);
      if (f.body) emit_stmt(*f.body);
      emitln(cont + ":;");
      loop_continue_labels.pop_back();
      loop_break_labels.pop_back();
      emitln("if (" + var + " == __pto) break;");
      emitln(step + std::string("(") + var + ");");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      dedent();
      emitln("}");
      emitln(brk + ":;");
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      auto selector_is_charish = [&]() -> bool {
        const TypeExpr* t = deduce_type(*cs.selector);
        if (!t) return false;
        t = canonicalize_type(t);
        return tyname_is(t, "char");
      };
      auto case_expr = [&](const Expr& e) -> std::string {
        std::string text = expr_to_cxx(e);
        return selector_is_charish() ? "::rt::p_ord(" + text + ")" : text;
      };
      emitln("switch (" + case_expr(*cs.selector) + ") {");
      indent();
      for (const auto& arm : cs.arms) {
        for (const auto& lab : arm.labels) {
          if (lab->kind == Kind::Range) {
            // GCC case-range extension: `case lo ... hi:`. Acceptable here;
            // the gnu profile compiler supports it. TODO: iterate label
            // values for strict standard C++.
            const auto& r = static_cast<const Range&>(*lab);
            emitln("case " + case_expr(*r.lo) + " ... " +
                   case_expr(*r.hi) + ":");
          } else {
            emitln("case " + case_expr(*lab) + ":");
          }
        }
        indent();
        if (arm.body) emit_stmt(*arm.body);
        emitln("break;");
        dedent();
      }
      if (cs.else_branch) {
        emitln("default:");
        indent();
        emit_stmt(*cs.else_branch);
        emitln("break;");
        dedent();
      }
      dedent();
      emitln("}");
      break;
    }
    case Kind::With: {
      // Pascal `with A, B do S` opens A's and B's fields (and methods)
      // as unqualified names inside S. We alias each target as
      // `__with<i>` and push its deduced type onto `with_stack`; bare
      // idents inside S that match a field of any stacked type are
      // rewritten by the expression emitter to `__with<i>.name`.
      const auto& w = static_cast<const With&>(s);
      emitln("{");
      indent();
      size_t pushed = 0;
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        const TypeExpr* ty = deduce_type(*w.exprs[i]);
        std::string nm = "__with" + std::to_string(with_stack.size());
        emitln("auto& " + nm + " = " + expr_to_cxx(*w.exprs[i]) + ";");
        with_stack.push_back({nm, ty});
        ++pushed;
      }
      if (w.body) emit_stmt(*w.body);
      for (size_t i = 0; i < pushed; ++i) with_stack.pop_back();
      dedent();
      emitln("}");
      break;
    }
    case Kind::Goto: {
      const auto& g = static_cast<const Goto&>(s);
      emitln("goto p_" + g.label + ";");
      break;
    }
    case Kind::Labeled: {
      const auto& lb = static_cast<const Labeled&>(s);
      emitln("p_" + lb.label + ":");
      if (lb.body) emit_stmt(*lb.body);
      break;
    }
    case Kind::AsmStmt: {
      report_error(s.loc, "asm blocks are unsupported");
      emitln("/* unsupported asm */");
      break;
    }
    default:
      emitln("/* unsupported-stmt */;");
      break;
  }
}

// Forward decl so emit_proc_body / emit_nested_proc_lambda can call it
// to forward-declare record/object types in local type-decls before
// pointer aliases that reference them.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<ast::DeclPtr>& decls);

void Emitter::emit_proc_body(const ProcDecl& pd) {
  // Header line: ret ClassName::Method(args) or ret Method(args).
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else if (pd.pkind == ProcKind::Constructor) {
    ret = "bool";
  } else {
    ret = "void";
  }
  std::string qname = mangle(pd.name);
  if (!pd.of_type.empty()) qname = mangle(pd.of_type) + "::" + qname;
  emitln(ret + " " + qname + "(" + param_list_to_cxx(pd.params) + ") {");
  indent();

  // Save outer state and set for this body.
  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  std::string saved_class = current_class_name;
  auto saved_locals = local_scope;
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = (pd.pkind == ProcKind::Constructor);
  current_class_name = pd.of_type;  // empty for free functions
  ++block_depth;

  // Populate local-scope set so the expression emitter won't auto-call
  // identifiers that happen to name a parameterless method in another
  // unit (e.g. a local `typename: string;` shadowing a method). Also
  // record declared types so `.field` / `.method` access on those
  // locals can be resolved from the type registry.
  auto saved_types = local_types;
  auto saved_nested = local_nested_fns;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_aliases = local_type_aliases_scoped;
  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      local_scope.insert(nm);
      if (p.type) {
        local_types[nm] = p.type.get();
      } else {
        local_untyped_params.insert(nm);
      }
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        local_scope.insert(nm);
        if (vd.type) local_types[nm] = vd.type.get();
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      local_scope.insert(cd.name);
      if (cd.type) local_types[cd.name] = cd.type.get();
    } else if (l->kind == Kind::TypeDecl) {
      // Pascal's local `type` section is statically visible to the
      // translator too -- record enums (for array-dim sizing and
      // `low(T)`/`high(T)` rewrites) and aliases (for canonicalize).
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      local_scope.insert(npd.name);
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      local_nested_fns[npd.name] = nf;
    }
  }

  // Forward-declare any record/object types in locals so a pointer
  // alias that textually precedes its target still compiles inside
  // the function body.
  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  // Pascal result variable. We use an internal slot named `result`
  // (unprefixed) so it won't collide with a Pascal-level variable
  // named `result` (which would emit as `p_result`). Shadowing the
  // function itself is avoided because we don't use the function's
  // name for the slot.
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " result{};");
  } else if (pd.pkind == ProcKind::Constructor) {
    emitln("bool result = true;");
  }
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function ||
      pd.pkind == ProcKind::Constructor) {
    emitln("return result;");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  current_class_name = std::move(saved_class);
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_nested_fns = std::move(saved_nested);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("}");
}

void Emitter::emit_nested_proc_lambda(const ProcDecl& pd) {
  std::string ret;
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    ret = type_to_cxx(*pd.return_type);
  } else {
    ret = "void";
  }
  // Build the param-type list for the std::function signature.
  std::string sig_params;
  {
    bool first = true;
    for (const auto& p : pd.params) {
      std::string pt;
      if (!p.type) {
        pt = "void*";
      } else {
        pt = type_to_cxx(*p.type);
        if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
        else if (p.mode == Param::Const) {
          if (const_param_needs_mutable_ref(p.type.get())) pt += "&";
          else pt = std::string("const ") + pt + "&";
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
  }

  const std::string lname = mangle(pd.name);
  // Forward-declare the std::function so the lambda can recurse by name.
  emitln("::std::function<" + ret + "(" + sig_params + ")> " + lname + ";");
  emitln(lname + " = [&](" + param_list_to_cxx(pd.params) + ") -> " + ret +
         " {");
  indent();

  std::string saved_name = current_fn_name;
  bool saved_fn = current_fn_is_function;
  bool saved_ctor = current_fn_is_ctor;
  auto saved_locals = local_scope;
  auto saved_types = local_types;
  auto saved_nested = local_nested_fns;
  auto saved_untyped = local_untyped_params;
  auto saved_local_enums = local_enums;
  auto saved_local_aliases = local_type_aliases_scoped;
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = false;
  ++block_depth;

  for (const auto& p : pd.params) {
    for (const auto& nm : p.names) {
      local_scope.insert(nm);
      if (p.type) local_types[nm] = p.type.get();
      else local_untyped_params.insert(nm);
    }
  }
  for (const auto& l : pd.locals) {
    if (l->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*l);
      for (const auto& nm : vd.names) {
        local_scope.insert(nm);
        if (vd.type) local_types[nm] = vd.type.get();
      }
    } else if (l->kind == Kind::ConstDecl) {
      const auto& cd = static_cast<const ConstDecl&>(*l);
      local_scope.insert(cd.name);
      if (cd.type) local_types[cd.name] = cd.type.get();
    } else if (l->kind == Kind::TypeDecl) {
      const auto& td = static_cast<const TypeDecl&>(*l);
      if (td.type) {
        if (td.type->kind == Kind::TyEnum) {
          local_enums[td.name] =
              static_cast<const ast::TyEnum*>(td.type.get());
        } else {
          local_type_aliases_scoped[td.name] = td.type.get();
        }
      }
    } else if (l->kind == Kind::ProcDecl) {
      const auto& npd = static_cast<const ProcDecl&>(*l);
      local_scope.insert(npd.name);
      NestedFn nf;
      for (const auto& p : npd.params) nf.param_count += p.names.size();
      nf.is_function = (npd.pkind == ProcKind::Function);
      nf.return_type = npd.return_type.get();
      local_nested_fns[npd.name] = nf;
    }
  }

  emit_forward_struct_decls(*this, pd.locals);
  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " result{};");
  }
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function) {
    emitln("return result;");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
  local_scope = std::move(saved_locals);
  local_types = std::move(saved_types);
  local_nested_fns = std::move(saved_nested);
  local_untyped_params = std::move(saved_untyped);
  local_enums = std::move(saved_local_enums);
  local_type_aliases_scoped = std::move(saved_local_aliases);
  --block_depth;

  dedent();
  emitln("};");
}

// ---------------------------------------------------------------------------
// Unit

// Scan the decl list and emit forward declarations for every record/object
// type, so a pointer type that textually precedes its target still compiles.
static void emit_forward_struct_decls(Emitter& e,
                                      const std::vector<DeclPtr>& decls) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    if (td.type->kind == Kind::TyRecord || td.type->kind == Kind::TyObject) {
      e.emitln("struct " + std::string("p_") + td.name + ";");
    }
  }
}

// Collect every TyName (lowercased) mentioned in a TypeExpr. Recurses into
// records/objects so that a record's field types contribute dependencies.
static void collect_type_refs(const TypeExpr& t,
                              std::unordered_set<std::string>& out) {
  switch (t.kind) {
    case Kind::TyName:
      out.insert(static_cast<const TyName&>(t).name);
      return;
    case Kind::TyPointer:
      if (static_cast<const TyPointer&>(t).target)
        collect_type_refs(*static_cast<const TyPointer&>(t).target, out);
      return;
    case Kind::TyArray: {
      const auto& a = static_cast<const TyArray&>(t);
      for (const auto& d : a.dims) if (d) collect_type_refs(*d, out);
      if (a.element) collect_type_refs(*a.element, out);
      return;
    }
    case Kind::TySet:
      if (static_cast<const TySet&>(t).element)
        collect_type_refs(*static_cast<const TySet&>(t).element, out);
      return;
    case Kind::TyFile:
      if (static_cast<const TyFile&>(t).element)
        collect_type_refs(*static_cast<const TyFile&>(t).element, out);
      return;
    case Kind::TyRecord: {
      const auto& r = static_cast<const TyRecord&>(t);
      for (const auto& f : r.fields) if (f.type) collect_type_refs(*f.type, out);
      for (const auto& vc : r.variant_cases)
        for (const auto& f : vc.fields)
          if (f.type) collect_type_refs(*f.type, out);
      return;
    }
    case Kind::TyObject: {
      const auto& o = static_cast<const TyObject&>(t);
      if (!o.parent.empty()) out.insert(o.parent);
      for (const auto& m : o.members) {
        if (m.is_field && m.field_type) collect_type_refs(*m.field_type, out);
      }
      return;
    }
    case Kind::TySubrange:
    case Kind::TyString:
    case Kind::TyEnum:
    case Kind::TyProcedural:
    default:
      return;
  }
}

// Reorder type decls so every alias (non-record, non-object) appears after
// the types it references by name. Record/object types are already
// forward-declared by emit_forward_struct_decls, so aliases that point to
// them via `^T` always work; this function only needs to handle aliases
// that depend on other aliases (e.g. `pfoo = ^tfoo` where `tfoo` is itself
// an alias to an array type).
//
// `in` must contain only type decls (checked by the caller); this runs
// against a single contiguous Pascal `type` section.
static std::vector<const Decl*> ordered_type_decls(
    const std::vector<const Decl*>& in) {
  std::vector<const Decl*> type_decls(in);

  // Map name -> index for quick lookup.
  std::unordered_map<std::string, int> index_of;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    index_of[static_cast<const TypeDecl*>(type_decls[i])->name] = i;
  }

  // For each type decl, which in-unit types does it reference?
  std::vector<std::unordered_set<int>> deps(type_decls.size());
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    const auto& td = *static_cast<const TypeDecl*>(type_decls[i]);
    if (!td.type) continue;
    std::unordered_set<std::string> refs;
    collect_type_refs(*td.type, refs);
    for (const auto& r : refs) {
      auto it = index_of.find(r);
      if (it == index_of.end()) continue;  // external / primitive
      int j = it->second;
      if (j == i) continue;
      const auto& rd = *static_cast<const TypeDecl*>(type_decls[j]);
      // Pointer-to-record aliases don't need the record body before them:
      // `using p_pfoo = p_tfoo*;` only needs the struct forward declaration
      // (emitted by emit_forward_struct_decls). This break lets cycles
      // like `Pfoo = ^Tfoo; Tfoo = record next: Pfoo; end;` remain a DAG.
      if (rd.type && (rd.type->kind == Kind::TyRecord ||
                      rd.type->kind == Kind::TyObject) &&
          td.type->kind == Kind::TyPointer) {
        continue;
      }
      deps[i].insert(j);
    }
  }

  // Kahn topological sort (stable: ties broken by original order).
  std::vector<int> indeg(type_decls.size(), 0);
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    for (int j : deps[i]) (void)j, ++indeg[i];
  }
  std::vector<int> ready;
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (indeg[i] == 0) ready.push_back(i);
  }
  std::vector<const Decl*> out;
  std::unordered_set<int> emitted_set;
  while (!ready.empty()) {
    int n = ready.front();
    ready.erase(ready.begin());
    out.push_back(type_decls[n]);
    emitted_set.insert(n);
    for (int i = 0; i < (int)type_decls.size(); ++i) {
      if (!deps[i].count(n)) continue;
      if (--indeg[i] == 0) ready.push_back(i);
    }
  }
  // Anything left has a cycle among non-pointer aliases. Emit in source
  // order as a fallback -- probably won't compile, but we don't silently
  // drop declarations.
  for (int i = 0; i < (int)type_decls.size(); ++i) {
    if (!emitted_set.count(i)) out.push_back(type_decls[i]);
  }
  return out;
}

void Emitter::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;  // used for the #include stem
  current_unit_name = ascii_lower(u.name);
  if (current_unit_name == "tpexcept") {
    emit_tpexcept_unit(u);
    return;
  }

  // Header.
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <array>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  // Emitted headers are prefixed `p_` so the filename never collides with
  // a C/C++ standard header (e.g. Pascal unit `strings` vs libc strings.h).
  for (const auto& uu : u.interface_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  // Runtime helpers (p_fillchar, p_writeln, p_getmem, ...) live in
  // namespace ::rt. Pull them in so Pascal builtins emit as bare calls.
  emitln("using namespace ::rt;");
  for (const auto& uu : u.interface_uses) {
    emitln("using namespace " + mangle(uu) + ";");
  }
  nl();
  emit_forward_struct_decls(*this, u.interface_decls);
  // Walk source order. Types are reordered topologically only within a
  // single contiguous run (a Pascal `type` section); any intervening
  // const/var/proc breaks the run. This respects Pascal's rule that
  // forward references are only allowed within the same type section.
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/true);
      }
      run.clear();
    };
    for (const auto& d : u.interface_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, true);
      }
    }
    flush();
  }
  // Forward-declare __unit_init() in the header so the program's
  // init chain in main() can call it across translation units.
  if (!u.is_program) {
    nl();
    emitln("void __unit_init();");
  }
  nl();
  emitln("}  // namespace " + ns);

  // Implementation.
  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_" + hguard + ".h\"");
  for (const auto& uu : u.impl_uses) {
    emitln("#include \"p_" + uu + ".h\"");
  }
  // The program emits a `__unit_init()` call chain over every
  // parsed unit; include all of their headers so the declarations
  // are visible.
  if (u.is_program && unit_init_order) {
    for (const auto& uu : *unit_init_order) {
      if (uu == u.name) continue;
      emitln("#include \"p_" + uu + ".h\"");
    }
  }
  nl();
  emitln("namespace " + ns + " {");
  nl();
  emitln("using namespace ::rt;");
  for (const auto& uu : u.impl_uses) {
    emitln("using namespace " + mangle(uu) + ";");
  }
  nl();
  emit_forward_struct_decls(*this, u.impl_decls);
  // Emit definitions (not just extern declarations) for interface
  // vars in the .cc so external references resolve at link time.
  for (const auto& d : u.interface_decls) {
    if (d->kind == Kind::VarDecl) {
      const auto& vd = static_cast<const VarDecl&>(*d);
      if (vd.is_absolute || vd.is_external) continue;
      emit_decl(*d, /*in_header=*/false);
    }
  }
  {
    std::vector<const Decl*> run;
    auto flush = [&] {
      if (run.empty()) return;
      for (const auto* td : ordered_type_decls(run)) {
        emit_decl(*td, /*in_header=*/false);
      }
      run.clear();
    };
    for (const auto& d : u.impl_decls) {
      if (d->kind == Kind::TypeDecl) {
        run.push_back(d.get());
      } else {
        flush();
        emit_decl(*d, false);
      }
    }
    flush();
  }
  // Emit the unit/program `begin..end.` body.
  //
  //  - program:  generate `int main(int argc, char** argv)` that
  //    stashes argv for ParamStr/ParamCount, then runs the program
  //    body inside an exception-catching IIFE. Pascal `Halt(n)` is
  //    rt::p_halt, which longjmps back to main.
  //  - unit:     emit a free `__init()` function holding the body.
  //    TODO: chain these in a proper startup init list. For now,
  //    unreferenced init bodies are dead-stripped by the linker.
  if (!u.is_program) {
    // Always emit a `__unit_init()` so the program's startup init
    // chain can call every unit unconditionally (without checking
    // whether this unit has a begin..end. body). Empty body if
    // Pascal had none.
    nl();
    emitln("void __unit_init() {");
    indent();
    ++block_depth;
    if (u.init_body) emit_stmt(*u.init_body);
    --block_depth;
    dedent();
    emitln("}");
    nl();
    emitln("}  // namespace " + ns);
  } else if (u.init_body) {
    nl();
    if (u.is_program) {
      // Program entry point -- the one unprefixed name we emit.
      emitln("}  // namespace " + ns);
      nl();
      emitln("int main(int argc, char** argv) {");
      indent();
      emitln("::rt::init_argv(argc, argv);");
      // Run each uses'd unit's init body in topological order
      // (dependencies before dependents). Pascal's unit init
      // semantics: every unit's `begin..end.` at its tail fires
      // exactly once, before the program body.
      if (unit_init_order) {
        for (const auto& uu : *unit_init_order) {
          if (uu == u.name) continue;  // don't self-init the program
          emitln(mangle(uu) + "::__unit_init();");
        }
      }
      // Re-enter the namespace so the body sees in-unit names
      // unqualified.
      emitln("using namespace " + ns + ";");
      emitln("using namespace ::rt;");
      ++block_depth;
      if (u.init_body) emit_stmt(*u.init_body);
      --block_depth;
      emitln("return 0;");
      dedent();
      emitln("}");
    } else {
      emitln("void __unit_init() {");
      indent();
      ++block_depth;
      emit_stmt(*u.init_body);
      --block_depth;
      dedent();
      emitln("}");
      nl();
      emitln("}  // namespace " + ns);
    }
  } else {
    nl();
    emitln("}  // namespace " + ns);
  }
}

void Emitter::emit_tpexcept_unit(const UnitNode& u) {
  (void)u;
  set_header();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <setjmp.h>");
  emitln("#include \"tp2cc_rt/prelude.h\"");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("using namespace ::rt;");
  nl();
  emitln("struct p_jmp_buf {");
  indent();
  emitln("int32_t p_eax;");
  emitln("int32_t p_ebx;");
  emitln("int32_t p_ecx;");
  emitln("int32_t p_edx;");
  emitln("int32_t p_esi;");
  emitln("int32_t p_edi;");
  emitln("int32_t p_ebp;");
  emitln("int32_t p_esp;");
  emitln("int32_t p_eip;");
  emitln("int32_t p_flags;");
  emitln("uint16_t p_cs;");
  emitln("uint16_t p_ds;");
  emitln("uint16_t p_es;");
  emitln("uint16_t p_fs;");
  emitln("uint16_t p_gs;");
  emitln("uint16_t p_ss;");
  dedent();
  emitln("};");
  emitln("using p_pjmp_buf = p_jmp_buf*;");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("struct p_jump_state {");
  indent();
  emitln("::jmp_buf p_env;");
  dedent();
  emitln("};");
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec);");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("int32_t p_setjmp(p_jmp_buf& p_rec) = delete;");
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value);");
  emitln("inline p_pjmp_buf p_recoverpospointer = nullptr;");
  emitln("inline bool p_longjump_used = false;");
  nl();
  emitln("void __unit_init();");
  nl();
  emitln("}  // namespace p_tpexcept");

  set_impl();
  emitln("// Generated by tp2cc. Do not edit.");
  emitln("#include \"p_tpexcept.h\"");
  emitln("#include <cstdlib>");
  emitln("#include <unordered_map>");
  nl();
  emitln("namespace {");
  indent();
  emitln("std::unordered_map<const p_tpexcept::p_jmp_buf*,");
  emitln("                   p_tpexcept::p_detail::p_jump_state> p_jump_states;");
  dedent();
  emitln("}  // namespace");
  nl();
  emitln("namespace p_tpexcept {");
  nl();
  emitln("using namespace ::rt;");
  nl();
  emitln("namespace p_detail {");
  indent();
  emitln("p_jump_state& p_state_for(p_jmp_buf* p_rec) {");
  indent();
  emitln("return ::p_jump_states[p_rec];");
  dedent();
  emitln("}");
  dedent();
  emitln("}  // namespace p_detail");
  nl();
  emitln("[[noreturn]] void p_longjmp(const p_jmp_buf& p_rec, int32_t p_return_value) {");
  indent();
  emitln("auto it = ::p_jump_states.find(&p_rec);");
  emitln("if (it == ::p_jump_states.end()) std::abort();");
  emitln("p_longjump_used = true;");
  emitln("::longjmp(it->second.p_env, p_return_value == 0 ? 1 : p_return_value);");
  dedent();
  emitln("}");
  nl();
  emitln("void __unit_init() {");
  emitln("}");
  nl();
  emitln("}  // namespace p_tpexcept");
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u, const TypeRegistry* registry,
                      const std::vector<std::string>* unit_init_order) {
  Emitter e;
  e.registry = registry;
  if (unit_init_order) e.unit_init_order = unit_init_order;
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace tp2cc
