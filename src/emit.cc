#include "emit.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "diag.h"

namespace p2cc {

using namespace ast;

namespace {

// ---------------------------------------------------------------------------
// Name mangling

// Identifiers already start with `p_` in the output. Pascal built-in type
// names map directly to C++ types below without the prefix.
std::string mangle(std::string_view name) {
  std::string s("p_");
  s.append(name);
  return s;
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
      {"char",      "uint8_t"},
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
      {"pchar",     "const char*"},
      {"ppchar",    "const char**"},
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

  // Pascal lets you call a parameterless function/method without parens:
  // `x := obj.size` means `x := obj.size()`. In C++ that yields a member
  // pointer, which is a compile error in most contexts. We scan the
  // current unit for parameterless method names and append `()` to bare
  // member accesses. Same-unit only -- cross-unit callers still need
  // explicit parens.
  std::unordered_set<std::string> parameterless_methods;

  // Map from Pascal type-alias name (lowercased) to its type expression
  // within the currently-emitted unit. Used to resolve named aliases so
  // a typed-constant whose declared type is a named `array[...] of T`
  // can still emit a C-style array initialiser.
  std::unordered_map<std::string, const TypeExpr*> local_type_aliases;

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
  std::string param_list_to_cxx(const std::vector<Param>& params);
  void emit_proc_body(const ProcDecl& pd);
  void emit_nested_proc_lambda(const ProcDecl& pd);
  void emit_stmt(const Stmt& s);
  void emit_stmt_line(const Stmt& s);  // prepends indent + trailing ';'

  // State: the Pascal identifier of the current function whose body we are
  // emitting (not mangled). Used by `exit`/`exit(v)` translation so we
  // know which result slot to fill.
  std::string current_fn_name;
  bool current_fn_is_function = false;
  bool current_fn_is_ctor = false;
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

std::string Emitter::subrange_type_to_cxx(const TySubrange& r) {
  // Without further info we can only represent subranges as their base
  // type; pick int32_t as a safe default.
  // TODO: derive a narrower type from the bounds when both are IntLit.
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
  // `array[D1, D2, ...] of T` -> nested std::array<std::array<T, ...>, ...>
  // For open arrays (no dims) we emit a pointer + length convention later;
  // for now, a plain pointer.
  if (a.dims.empty()) {
    return type_to_cxx(*a.element) + "*";
  }
  // Start from innermost to outermost. We need the dim size; for a named
  // index type we can't compute it cheaply here without the type table.
  // For ordinal subrange `lo..hi`, we can compute `hi - lo + 1`.
  std::vector<std::string> size_strs;
  for (const auto& d : a.dims) {
    if (d->kind == Kind::TySubrange) {
      const auto& sr = static_cast<const TySubrange&>(*d);
      size_strs.push_back("(" + const_value_to_cxx(*sr.hi) +
                          " - " + const_value_to_cxx(*sr.lo) + " + 1)");
    } else if (d->kind == Kind::TyName) {
      // Index by a named ordinal type -- use an opaque helper whose size
      // the runtime knows. Conservative: treat as an int-indexed array
      // using a sentinel size constant that the emitter (later) will
      // resolve via the symbol table. For now, leave as a pointer.
      size_strs.clear();
      break;
    } else {
      size_strs.clear();
      break;
    }
  }
  if (size_strs.empty()) {
    // Fall back to raw pointer; callers will index via bracket anyway.
    return type_to_cxx(*a.element) + "*";
  }
  std::string t = type_to_cxx(*a.element);
  // Outermost-first: wrap innermost first.
  for (auto it = size_strs.rbegin(); it != size_strs.rend(); ++it) {
    t = "::std::array<" + t + ", " + *it + ">";
  }
  return t;
}

std::string Emitter::procedural_type_to_cxx(const TyProcedural& p) {
  std::string ret = p.is_function ? type_to_cxx(*p.return_type) : std::string("void");
  std::string params;
  bool first = true;
  for (const auto& pp : p.params) {
    std::string pt = pp.type ? type_to_cxx(*pp.type) : std::string("void*");
    if (pp.mode == Param::Var || pp.mode == Param::Out) pt += "&";
    else if (pp.mode == Param::Const) pt = std::string("const ") + pt + "&";
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
    default:                 return "/* unsupported-type */ int32_t";
  }
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
        return "'" + escape_char_body(n.value[0], true) + "'";
      }
      std::string out = "::rt::ShortString<>(\"";
      for (char c : n.value) out += escape_char_body(c, false);
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
      if (n.name == "inherited") {
        // Bare `inherited;` (rare) -- `inherited{}` default-constructs the
        // parent via the in-struct `using inherited = Parent;` alias.
        return "inherited{}";
      }
      if (n.name == "self") return "(*this)";
      // Intentionally do NOT substitute primitive type names here.
      // `text` might be a Pascal field name as easily as the `text`
      // file-type, and we have no symbol table at expression emission.
      // Type-context substitution lives in type_to_cxx; type-casts are
      // handled as a special form in the Call case below.
      return mangle(n.name);
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
        case BinOp::And:    op = "&"; break;
        case BinOp::Or:     op = "|"; break;
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
      const char* op = "+";
      switch (n.op) {
        case UnOp::Neg: op = "-"; break;
        case UnOp::Plus: op = "+"; break;
        case UnOp::Not: op = "~"; break;
      }
      return std::string(op) + expr_to_cxx(*n.operand);
    }
    case Kind::Member: {
      const auto& m = static_cast<const Member&>(e);
      if (m.base->kind == Kind::Ident &&
          static_cast<const Ident&>(*m.base).name == "inherited") {
        return "inherited::" + mangle(m.name);
      }
      std::string text = expr_to_cxx(*m.base) + "." + mangle(m.name);
      // Pascal auto-calls a parameterless function/method written without
      // parens. Append `()` if the name is a known parameterless method
      // in this unit. The parent Call node (if any) handles its own
      // parens and bypasses this path through parse_postfix.
      if (parameterless_methods.count(m.name)) text += "()";
      return text;
    }
    case Kind::Deref: {
      const auto& d = static_cast<const Deref&>(e);
      return "(*" + expr_to_cxx(*d.operand) + ")";
    }
    case Kind::AddrOf: {
      const auto& a = static_cast<const AddrOf&>(e);
      return "(&" + expr_to_cxx(*a.operand) + ")";
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
        bool is_low_high_type = false;
        std::string low_high_rewrite;
        if ((n == "low" || n == "high") && c.args.size() == 1 &&
            c.args[0]->kind == Kind::Ident) {
          const auto& a = static_cast<const Ident&>(*c.args[0]);
          auto it = local_type_aliases.find(a.name);
          if (it != local_type_aliases.end() && it->second &&
              it->second->kind == Kind::TyEnum) {
            is_low_high_type = true;
            low_high_rewrite = mangle(a.name) + "__" + n;
          }
        }

        if (is_low_high_type) {
          return low_high_rewrite;
        } else if (n == "sizeof" && c.args.size() == 1) {
          return "sizeof(" + arg0() + ")";
        } else if (c.args.size() == 1 && is_primitive_type(n)) {
          // Function-style type cast. Compound C++ type expansions like
          // `const char*` or `long double` can't appear as `T(expr)`; use
          // a paren cast.
          return "((" + primitive_type_cxx(n) + ")(" + arg0() + "))";
        } else if (n == "new" && !c.args.empty()) {
          // Expression-form `new(T)` or `new(T, Ctor(args))`. The first
          // arg is the *pointer-type name* (an Ident), which we already
          // emit as `p_T` -- the underlying struct is
          // `std::remove_pointer_t<p_T>`.
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
      std::string out = expr_to_cxx(*c.callee) + "(";
      for (size_t i = 0; i < c.args.size(); ++i) {
        if (i) out += ", ";
        out += expr_to_cxx(*c.args[i]);
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
      // Construct an ::rt::set_of(..) helper; element type inferred.
      std::string out = "::rt::set_of({";
      for (size_t i = 0; i < s.elements.size(); ++i) {
        if (i) out += ", ";
        out += expr_to_cxx(*s.elements[i]);
      }
      out += "})";
      return out;
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

  // Typed array (or named alias ultimately resolving to one) with an
  // array-constant initialiser emits an `rt::Array<T, Lo, N>` so
  //   (a) the size is known even when the index is an enum (Pascal),
  //   (b) the array has value-copy semantics on pass (Pascal),
  //   (c) `arr[Lo]` picks the first element (Pascal arbitrary low bound).
  if (cd.type && cd.value->kind == Kind::ArrayConst) {
    const TypeExpr* t = cd.type.get();
    while (t && t->kind == Kind::TyName) {
      const auto& tn = static_cast<const TyName&>(*t);
      auto it = local_type_aliases.find(tn.name);
      if (it == local_type_aliases.end()) break;
      t = it->second;
    }
    if (t && t->kind == Kind::TyArray) {
      const auto& arr = static_cast<const TyArray&>(*t);
      // Wrap the element type in `Array<..., Lo, N>` for each dim from
      // innermost to outermost.
      std::string ty = arr.element ? type_to_cxx(*arr.element)
                                   : std::string("int32_t");
      for (auto it = arr.dims.rbegin(); it != arr.dims.rend(); ++it) {
        std::string lo = "0", size_expr;
        const auto& dim = **it;
        if (dim.kind == Kind::TySubrange) {
          const auto& r = static_cast<const TySubrange&>(dim);
          lo = const_value_to_cxx(*r.lo);
          size_expr = "((" + const_value_to_cxx(*r.hi) + ") - (" + lo +
                      ") + 1)";
        } else if (dim.kind == Kind::TyName) {
          const auto& tn = static_cast<const TyName&>(dim);
          auto at = local_type_aliases.find(tn.name);
          if (at != local_type_aliases.end() && at->second &&
              at->second->kind == Kind::TyEnum) {
            const auto& en = static_cast<const TyEnum&>(*at->second);
            lo = "0";
            size_expr = std::to_string(en.members.size());
          }
        }
        if (size_expr.empty()) {
          // Unknown size -- leave as-is; fall through to the generic
          // emit below.
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
    emitln(linkage + type_to_cxx(*cd.type) + " " + name + " = " + val + ";");
    return;
  }

  // Untyped Pascal const -- immutable. Multi-char string literals wrap in
  // ShortString<> so `+` means concatenation.
  if (cd.value->kind == Kind::StringLit) {
    emitln(linkage + "const ::rt::ShortString<> " + name + " = " + val + ";");
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
      emitln("inline constexpr " + name + " " + name + "__low = " +
             mangle(te.members.front()) + ";");
      emitln("inline constexpr " + name + " " + name + "__high = " +
             mangle(te.members.back()) + ";");
    }
    return;
  }

  if (td.type && td.type->kind == Kind::TyRecord) {
    const auto& tr = static_cast<const TyRecord&>(*td.type);
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
    std::string pt = p.type ? type_to_cxx(*p.type) : std::string("void*");
    if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
    else if (p.mode == Param::Const) pt = std::string("const ") + pt + "&";
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
      } else if (!pd.is_forward && !pd.is_external && !pd.is_abstract &&
                 pd.body) {
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
      // Pascal: inside a function body, `funcname := value` assigns the
      // return value. We rewrite to the result slot so the function's
      // own name still refers to the function (enabling recursion).
      std::string target_cxx;
      if (!current_fn_name.empty() && a.target->kind == Kind::Ident &&
          static_cast<const Ident&>(*a.target).name == current_fn_name) {
        target_cxx = "p_result";
      } else {
        target_cxx = expr_to_cxx(*a.target);
      }
      emitln(target_cxx + " = " + expr_to_cxx(*a.value) + ";");
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
        emitln("break;");
      } else if (name == "continue") {
        emitln("continue;");
      } else if (name == "exit") {
        // exit or exit(v). In a Function, fill the result slot and return;
        // in a Procedure, return; in a Constructor, return the status.
        if (call_expr && !call_expr->args.empty() && !current_fn_name.empty()) {
          emitln(mangle(current_fn_name) + " = " +
                 expr_to_cxx(*call_expr->args[0]) + ";");
          emitln("return " + mangle(current_fn_name) + ";");
        } else if (current_fn_is_function && !current_fn_name.empty()) {
          emitln("return " + mangle(current_fn_name) + ";");
        } else if (current_fn_is_ctor) {
          emitln("return p_result;");
        } else {
          emitln("return;");
        }
      } else if (name == "new" && call_expr && !call_expr->args.empty()) {
        // new(p) or new(p, Ctor(args))
        std::string p = expr_to_cxx(*call_expr->args[0]);
        emitln(p + " = new ::std::remove_pointer_t<decltype(" + p + ")>{};");
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
        emitln(expr_to_cxx(*es.expr) + ";");
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
      emitln("while (" + expr_to_cxx(*w.cond) + ") {");
      indent();
      if (w.body) emit_stmt(*w.body);
      dedent();
      emitln("}");
      break;
    }
    case Kind::Repeat: {
      const auto& r = static_cast<const Repeat&>(s);
      emitln("do {");
      indent();
      for (const auto& sub : r.body) emit_stmt(*sub);
      dedent();
      emitln("} while (!(" + expr_to_cxx(*r.cond) + "));");
      break;
    }
    case Kind::For: {
      const auto& f = static_cast<const For&>(s);
      std::string var = mangle(f.var);
      std::string from = expr_to_cxx(*f.from);
      std::string to = expr_to_cxx(*f.to);
      if (f.downto) {
        emitln("for (" + var + " = " + from + "; " + var + " >= " + to + "; --" + var + ") {");
      } else {
        emitln("for (" + var + " = " + from + "; " + var + " <= " + to + "; ++" + var + ") {");
      }
      indent();
      if (f.body) emit_stmt(*f.body);
      dedent();
      emitln("}");
      break;
    }
    case Kind::CaseStmt: {
      const auto& cs = static_cast<const CaseStmt&>(s);
      emitln("switch (" + expr_to_cxx(*cs.selector) + ") {");
      indent();
      for (const auto& arm : cs.arms) {
        for (const auto& lab : arm.labels) {
          if (lab->kind == Kind::Range) {
            // GCC case-range extension: `case lo ... hi:`. Acceptable here;
            // the gnu profile compiler supports it. TODO: iterate label
            // values for strict standard C++.
            const auto& r = static_cast<const Range&>(*lab);
            emitln("case " + expr_to_cxx(*r.lo) + " ... " +
                   expr_to_cxx(*r.hi) + ":");
          } else {
            emitln("case " + expr_to_cxx(*lab) + ":");
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
      // Minimal codegen: introduce a reference alias per `with` expression
      // and emit the body. Pascal's name-resolution rewrite is a future
      // refinement; for now we rely on the emitted identifier names being
      // qualified by the parser/user where needed.
      const auto& w = static_cast<const With&>(s);
      emitln("{");
      indent();
      for (size_t i = 0; i < w.exprs.size(); ++i) {
        emitln("auto& __with" + std::to_string(i) + " = " +
               expr_to_cxx(*w.exprs[i]) + ";");
      }
      emitln("(void)0;  // with-body follows");
      if (w.body) emit_stmt(*w.body);
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
      emitln("/* asm..end -- not translated; use p2cc_rt shim */");
      break;
    }
    default:
      emitln("/* unsupported-stmt */;");
      break;
  }
}

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
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = (pd.pkind == ProcKind::Constructor);
  ++block_depth;

  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  // Pascal result variable. We deliberately don't create a local with
  // the function's name here: that would shadow the function itself and
  // silently break recursive calls `funcname(args)`. Instead we use
  // `p_result` as the sole slot, and rewrite Pascal-style `funcname :=
  // value` assignments at emit-time (see Kind::Assign in emit_stmt).
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " p_result{};");
  } else if (pd.pkind == ProcKind::Constructor) {
    emitln("bool p_result = true;");
  }
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function ||
      pd.pkind == ProcKind::Constructor) {
    emitln("return p_result;");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
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
      std::string pt = p.type ? type_to_cxx(*p.type) : std::string("void*");
      if (p.mode == Param::Var || p.mode == Param::Out) pt += "&";
      else if (p.mode == Param::Const) pt = std::string("const ") + pt + "&";
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
  current_fn_name = pd.name;
  current_fn_is_function = (pd.pkind == ProcKind::Function);
  current_fn_is_ctor = false;
  ++block_depth;

  for (const auto& l : pd.locals) emit_decl(*l, /*in_header=*/false);
  if (pd.pkind == ProcKind::Function && pd.return_type) {
    emitln(ret + " p_result{};");
  }
  if (pd.body) emit_stmt(*pd.body);
  if (pd.pkind == ProcKind::Function) {
    emitln("return p_result;");
  }

  current_fn_name = std::move(saved_name);
  current_fn_is_function = saved_fn;
  current_fn_is_ctor = saved_ctor;
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

// Scan a decl list for:
//   - object-type parameterless methods (for auto-call in Member emission)
//   - type aliases (name -> underlying TypeExpr*) so a typed const whose
//     declared type is a named alias to an array can still emit as a
//     C-style array.
static void collect_unit_tables(
    const std::vector<DeclPtr>& decls,
    std::unordered_set<std::string>& methods,
    std::unordered_map<std::string, const TypeExpr*>& aliases) {
  for (const auto& d : decls) {
    if (d->kind != Kind::TypeDecl) continue;
    const auto& td = static_cast<const TypeDecl&>(*d);
    if (!td.type) continue;
    aliases[td.name] = td.type.get();
    if (td.type->kind == Kind::TyObject) {
      const auto& to = static_cast<const TyObject&>(*td.type);
      for (const auto& m : to.members) {
        if (m.is_field || !m.method) continue;
        const auto& pd = static_cast<const ProcDecl&>(*m.method);
        if (pd.params.empty()) methods.insert(pd.name);
      }
    }
  }
}

void Emitter::emit_unit(const UnitNode& u) {
  const std::string ns = mangle(u.name);
  const std::string hguard = u.name;  // used for the #include stem

  collect_unit_tables(u.interface_decls, parameterless_methods,
                      local_type_aliases);
  collect_unit_tables(u.impl_decls, parameterless_methods,
                      local_type_aliases);

  // Header.
  set_header();
  emitln("// Generated by p2cc. Do not edit.");
  emitln("#pragma once");
  emitln("#include <cstdint>");
  emitln("#include <cstddef>");
  emitln("#include <array>");
  emitln("#include \"p2cc_rt/prelude.h\"");
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
  nl();
  emitln("}  // namespace " + ns);

  // Implementation.
  set_impl();
  emitln("// Generated by p2cc. Do not edit.");
  emitln("#include \"p_" + hguard + ".h\"");
  for (const auto& uu : u.impl_uses) {
    emitln("#include \"p_" + uu + ".h\"");
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
  // Unit init body isn't emitted yet (M5 concern -- main() generation).
  nl();
  emitln("}  // namespace " + ns);
}

}  // namespace

EmittedUnit emit_unit(const UnitNode& u) {
  Emitter e;
  e.emit_unit(u);
  return {std::move(e.header), std::move(e.impl)};
}

}  // namespace p2cc
