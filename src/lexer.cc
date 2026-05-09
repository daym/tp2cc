#include "lexer.h"

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "diag.h"

namespace tp2cc {

namespace {

// Hard-reserved keywords only. Declaration directives and modifiers stay as
// identifiers and are recognized by the parser only where they are valid.
const std::pair<const char*, Tok> kKeywordTable[] = {
    {"and", Tok::KwAnd},
    {"array", Tok::KwArray},
    {"as", Tok::KwAs},
    {"asm", Tok::KwAsm},
    {"begin", Tok::KwBegin},
    {"case", Tok::KwCase},
    {"class", Tok::KwClass},
    {"const", Tok::KwConst},
    {"constructor", Tok::KwConstructor},
    {"destructor", Tok::KwDestructor},
    {"div", Tok::KwDiv},
    {"do", Tok::KwDo},
    {"downto", Tok::KwDownto},
    {"else", Tok::KwElse},
    {"end", Tok::KwEnd},
    {"except", Tok::KwExcept},
    {"exports", Tok::KwExports},
    {"false", Tok::KwFalse},
    {"file", Tok::KwFile},
    {"finalization", Tok::KwFinalization},
    {"finally", Tok::KwFinally},
    {"for", Tok::KwFor},
    {"function", Tok::KwFunction},
    {"goto", Tok::KwGoto},
    {"if", Tok::KwIf},
    {"implementation", Tok::KwImplementation},
    {"in", Tok::KwIn},
    {"inherited", Tok::KwInherited},
    {"initialization", Tok::KwInitialization},
    {"interface", Tok::KwInterface},
    {"is", Tok::KwIs},
    {"label", Tok::KwLabel},
    {"library", Tok::KwLibrary},
    {"mod", Tok::KwMod},
    {"nil", Tok::KwNil},
    {"not", Tok::KwNot},
    {"object", Tok::KwObject},
    {"of", Tok::KwOf},
    {"operator", Tok::KwOperator},
    {"or", Tok::KwOr},
    {"otherwise", Tok::KwOtherwise},
    {"packed", Tok::KwPacked},
    {"procedure", Tok::KwProcedure},
    {"program", Tok::KwProgram},
    {"raise", Tok::KwRaise},
    {"record", Tok::KwRecord},
    {"repeat", Tok::KwRepeat},
    {"resourcestring", Tok::KwResourcestring},
    {"self", Tok::KwSelf},
    {"set", Tok::KwSet},
    {"shl", Tok::KwShl},
    {"shortstring", Tok::KwShortstring},
    {"shr", Tok::KwShr},
    {"string", Tok::KwString},
    {"then", Tok::KwThen},
    {"threadvar", Tok::KwThreadvar},
    {"to", Tok::KwTo},
    {"true", Tok::KwTrue},
    {"try", Tok::KwTry},
    {"type", Tok::KwType},
    {"unit", Tok::KwUnit},
    {"until", Tok::KwUntil},
    {"uses", Tok::KwUses},
    {"var", Tok::KwVar},
    {"while", Tok::KwWhile},
    {"with", Tok::KwWith},
    {"xor", Tok::KwXor},
};

inline bool is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
inline bool is_ident_cont(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return 10 + (c - 'A');
}

}  // namespace

std::string Lexer::lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

Lexer::Lexer(std::shared_ptr<SourceFile> root,
             std::vector<std::filesystem::path> include_dirs)
    : include_dirs_(std::move(include_dirs)) {
  Input in;
  in.file = std::move(root);
  stack_.push_back(std::move(in));

  for (auto& kv : kKeywordTable) keywords_.emplace(kv.first, kv.second);
}

void Lexer::define(std::string name) { defines_.insert(lower(name)); }
void Lexer::undefine(const std::string& name) { defines_.erase(lower(name)); }

bool Lexer::at_eof_of_current() const {
  const auto& in = stack_.back();
  return in.pos >= in.file->contents.size();
}

char Lexer::peek(size_t ahead) const {
  const auto& in = stack_.back();
  size_t p = in.pos + ahead;
  if (p >= in.file->contents.size()) return 0;
  return in.file->contents[p];
}

char Lexer::get() {
  auto& in = stack_.back();
  if (in.pos >= in.file->contents.size()) return 0;
  char c = in.file->contents[in.pos++];
  if (c == '\n') {
    ++in.line;
    in.col = 1;
  } else {
    ++in.col;
  }
  return c;
}

void Lexer::unget() {
  auto& in = stack_.back();
  if (in.pos == 0) return;
  --in.pos;
  // Column rewinding is approximate; only accurate when not at a newline.
  // Good enough for diagnostics.
  if (in.col > 1) --in.col;
}

bool Lexer::pop_input_if_eof() {
  if (stack_.size() > 1 && at_eof_of_current()) {
    stack_.pop_back();
    return true;
  }
  return false;
}

Location Lexer::here() const {
  const auto& in = stack_.back();
  return Location{in.file, in.line, in.col};
}

bool Lexer::accepting() const {
  for (const auto& f : ifdef_stack_) {
    if (!f.accepting) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Directive handling

namespace {

// Split the directive body into a head (first word, lowercased) and the rest.
std::pair<std::string, std::string_view> split_directive(std::string_view body) {
  size_t i = 0;
  // skip leading spaces
  while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
  size_t start = i;
  while (i < body.size() &&
         (std::isalnum(static_cast<unsigned char>(body[i])) ||
          body[i] == '_' || body[i] == '+' || body[i] == '-')) {
    ++i;
  }
  std::string head;
  head.reserve(i - start);
  for (size_t k = start; k < i; ++k) {
    char c = body[k];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    head.push_back(c);
  }
  // skip separator
  while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) ++i;
  return {std::move(head), body.substr(i)};
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

std::optional<int> parse_packenum_value(std::string_view rest) {
  std::string value = trim(rest);
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (value.empty()) return std::nullopt;
  if (value == "default" || value == "normal") return 4;
  if (value == "1") return 1;
  if (value == "2") return 2;
  if (value == "4") return 4;
  return std::nullopt;
}

std::optional<int> mode_default_packenum(std::string_view rest) {
  std::string mode = trim(rest);
  for (char& c : mode) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (mode == "tp" || mode == "tp7" || mode == "delphi" || mode == "macpas") {
    return 1;
  }
  if (mode == "fpc" || mode == "objfpc") {
    return 4;
  }
  return std::nullopt;
}

}  // namespace

// Tiny recursive-descent evaluator for `{$if EXPR}` bodies. Comparisons are
// parsed after boolean OR/AND, so `or` binds tighter than `=` here:
//   expr        = simple_expr (relop simple_expr)?
//   simple_expr = and_expr ('or' and_expr)*
//   and_expr    = unary ('and' unary)*
//   unary       = 'not' unary | primary
//   primary     = '(' expr ')' | 'defined' '(' IDENT ')' | TRUE | FALSE | INT
//
// Unsupported syntax is a hard parser failure. The directive handler reports
// that as a diagnostic rather than silently treating it as false.
namespace {

struct IfExprParser {
  struct Value {
    enum class Kind { Bool, Int } kind = Kind::Bool;
    bool b = false;
    int64_t i = 0;

    static Value bool_value(bool v) { return Value{Kind::Bool, v, 0}; }
    static Value int_value(int64_t v) { return Value{Kind::Int, false, v}; }
  };

  std::string_view src;
  size_t pos = 0;
  bool ok = true;
  const std::unordered_set<std::string>* defines;

  void skip_ws() {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t')) ++pos;
  }
  bool eof() { skip_ws(); return pos >= src.size(); }
  bool match_char(char c) {
    skip_ws();
    if (pos < src.size() && src[pos] == c) { ++pos; return true; }
    return false;
  }
  bool match_word(std::string_view w) {
    skip_ws();
    if (pos + w.size() > src.size()) return false;
    for (size_t i = 0; i < w.size(); ++i) {
      char c = src[pos + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != w[i]) return false;
    }
    // Word boundary: next char must not be ident-continuation.
    size_t end = pos + w.size();
    if (end < src.size()) {
      char nc = src[end];
      if (std::isalnum(static_cast<unsigned char>(nc)) || nc == '_') {
        return false;
      }
    }
    pos = end;
    return true;
  }
  std::optional<int64_t> read_int() {
    skip_ws();
    size_t start = pos;
    int sign = 1;
    if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
      if (src[pos] == '-') sign = -1;
      ++pos;
    }
    size_t digits = pos;
    int64_t value = 0;
    while (pos < src.size() &&
           std::isdigit(static_cast<unsigned char>(src[pos]))) {
      value = value * 10 + (src[pos] - '0');
      ++pos;
    }
    if (digits == pos) {
      pos = start;
      return std::nullopt;
    }
    return sign < 0 ? -value : value;
  }
  std::string read_ident() {
    skip_ws();
    size_t start = pos;
    while (pos < src.size() &&
           (std::isalnum(static_cast<unsigned char>(src[pos])) ||
            src[pos] == '_')) {
      ++pos;
    }
    std::string out;
    out.reserve(pos - start);
    for (size_t i = start; i < pos; ++i) {
      char c = src[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      out.push_back(c);
    }
    return out;
  }
  Value parse_primary() {
    skip_ws();
    if (match_char('(')) {
      Value v = parse_expr();
      if (!match_char(')')) ok = false;
      return v;
    }
    // `defined(SYM)`
    size_t saved = pos;
    if (match_word("defined")) {
      if (!match_char('(')) { ok = false; return Value::bool_value(false); }
      std::string sym = read_ident();
      if (!match_char(')')) { ok = false; return Value::bool_value(false); }
      return Value::bool_value(defines && defines->count(sym) > 0);
    }
    pos = saved;
    if (match_word("true")) return Value::bool_value(true);
    if (match_word("false")) return Value::bool_value(false);
    if (auto v = read_int()) return Value::int_value(*v);
    ok = false;
    return Value::bool_value(false);
  }
  Value parse_unary() {
    if (match_word("not")) {
      Value v = parse_unary();
      if (v.kind != Value::Kind::Bool) ok = false;
      return Value::bool_value(!v.b);
    }
    return parse_primary();
  }
  Value parse_and() {
    Value v = parse_unary();
    while (match_word("and")) {
      Value r = parse_unary();
      if (v.kind != Value::Kind::Bool || r.kind != Value::Kind::Bool) {
        ok = false;
        return Value::bool_value(false);
      }
      v = Value::bool_value(v.b && r.b);
    }
    return v;
  }
  Value parse_simple_expr() {
    Value v = parse_and();
    while (match_word("or")) {
      Value r = parse_and();
      if (v.kind != Value::Kind::Bool || r.kind != Value::Kind::Bool) {
        ok = false;
        return Value::bool_value(false);
      }
      v = Value::bool_value(v.b || r.b);
    }
    return v;
  }
  std::optional<std::string_view> match_relop() {
    skip_ws();
    if (pos >= src.size()) return std::nullopt;
    if (src[pos] == '=') {
      ++pos;
      return std::string_view("=");
    }
    if (src[pos] == '<') {
      if (pos + 1 < src.size() && src[pos + 1] == '>') {
        pos += 2;
        return std::string_view("<>");
      }
      if (pos + 1 < src.size() && src[pos + 1] == '=') {
        pos += 2;
        return std::string_view("<=");
      }
      ++pos;
      return std::string_view("<");
    }
    if (src[pos] == '>') {
      if (pos + 1 < src.size() && src[pos + 1] == '=') {
        pos += 2;
        return std::string_view(">=");
      }
      ++pos;
      return std::string_view(">");
    }
    return std::nullopt;
  }
  Value parse_expr() {
    Value lhs = parse_simple_expr();
    auto op = match_relop();
    if (!op) return lhs;
    Value rhs = parse_simple_expr();
    if (lhs.kind != rhs.kind) {
      ok = false;
      return Value::bool_value(false);
    }
    if (lhs.kind == Value::Kind::Bool) {
      if (*op == "=") return Value::bool_value(lhs.b == rhs.b);
      if (*op == "<>") return Value::bool_value(lhs.b != rhs.b);
      ok = false;
      return Value::bool_value(false);
    }
    if (*op == "=") return Value::bool_value(lhs.i == rhs.i);
    if (*op == "<>") return Value::bool_value(lhs.i != rhs.i);
    if (*op == "<") return Value::bool_value(lhs.i < rhs.i);
    if (*op == ">") return Value::bool_value(lhs.i > rhs.i);
    if (*op == "<=") return Value::bool_value(lhs.i <= rhs.i);
    if (*op == ">=") return Value::bool_value(lhs.i >= rhs.i);
    ok = false;
    return Value::bool_value(false);
  }
};

}  // namespace

std::optional<bool> Lexer::eval_if_expr(std::string_view expr) {
  IfExprParser p{expr, 0, true, &defines_};
  IfExprParser::Value v = p.parse_expr();
  if (!p.ok) return std::nullopt;
  if (!p.eof()) return std::nullopt;
  if (v.kind != IfExprParser::Value::Kind::Bool) return std::nullopt;
  return v.b;
}

void Lexer::handle_directive(std::string_view body, Location where) {
  auto [head, rest] = split_directive(body);
  if (head.empty()) return;

  // Conditional directives are handled regardless of accepting() state.
  if (head == "ifdef" || head == "ifndef") {
    std::string sym = lower(trim(rest));
    bool defined = defines_.count(sym) > 0;
    bool take = (head == "ifdef") ? defined : !defined;
    bool parent_ok = accepting();
    IfdefFrame f;
    f.accepting = parent_ok && take;
    f.any_taken = f.accepting;
    f.in_else = false;
    ifdef_stack_.push_back(f);
    return;
  }
  if (head == "if") {
    bool parent_ok = accepting();
    bool cond = false;
    if (parent_ok) {
      if (std::optional<bool> v = eval_if_expr(rest)) {
        cond = *v;
      } else {
        report_error(where, "unsupported {$if} expression: " + trim(rest));
      }
    }
    IfdefFrame f;
    f.accepting = parent_ok && cond;
    f.any_taken = f.accepting;
    f.in_else = false;
    ifdef_stack_.push_back(f);
    return;
  }
  if (head == "ifopt") {
    // `{$ifopt X+}` / `{$ifopt X-}` queries a compiler switch state.
    // We track the Q (overflow-check) switch live; everything else
    // returns false so neither branch's body is required to compile,
    // matching what most callers want.
    bool parent_ok = accepting();
    bool cond = false;
    std::string opt = lower(trim(rest));
    if (opt == "q+" || opt == "overflowchecks+") cond = q_check_;
    else if (opt == "q-" || opt == "overflowchecks-") cond = !q_check_;
    else if (opt == "r+" || opt == "rangechecks+") cond = r_check_;
    else if (opt == "r-" || opt == "rangechecks-") cond = !r_check_;
    else if (opt == "h+" || opt == "longstrings+") cond = h_long_strings_;
    else if (opt == "h-" || opt == "longstrings-") cond = !h_long_strings_;
    IfdefFrame f;
    f.accepting = parent_ok && cond;
    f.any_taken = f.accepting;
    f.in_else = false;
    ifdef_stack_.push_back(f);
    return;
  }
  if (head == "elseif") {
    if (ifdef_stack_.empty()) {
      report_error(where, "{$elseif} without matching {$if}");
      return;
    }
    auto& f = ifdef_stack_.back();
    if (f.in_else) {
      report_error(where, "{$elseif} after {$else}");
      return;
    }
    bool parent_ok = true;
    for (size_t i = 0; i + 1 < ifdef_stack_.size(); ++i) {
      if (!ifdef_stack_[i].accepting) { parent_ok = false; break; }
    }
    bool cond = false;
    if (parent_ok && !f.any_taken) {
      if (std::optional<bool> v = eval_if_expr(rest)) {
        cond = *v;
      } else {
        report_error(where, "unsupported {$elseif} expression: " + trim(rest));
      }
    }
    f.accepting = parent_ok && !f.any_taken && cond;
    if (f.accepting) f.any_taken = true;
    return;
  }
  if (head == "else") {
    if (ifdef_stack_.empty()) {
      report_error(where, "{$else} without matching {$ifdef}");
      return;
    }
    auto& f = ifdef_stack_.back();
    if (f.in_else) {
      report_error(where, "duplicate {$else}");
      return;
    }
    f.in_else = true;
    // Parent state: all frames above this one must be accepting.
    bool parent_ok = true;
    for (size_t i = 0; i + 1 < ifdef_stack_.size(); ++i) {
      if (!ifdef_stack_[i].accepting) { parent_ok = false; break; }
    }
    f.accepting = parent_ok && !f.any_taken;
    if (f.accepting) f.any_taken = true;
    return;
  }
  if (head == "endif" || head == "ifend") {
    if (ifdef_stack_.empty()) {
      report_error(where, "{$endif} without matching {$ifdef}");
      return;
    }
    ifdef_stack_.pop_back();
    return;
  }

  // All other directives apply only when accepting.
  if (!accepting()) return;

  if (head == "define") {
    std::string sym = lower(trim(rest));
    // fpc allows `{$define FOO:=bar}` macro form -- we ignore the value.
    size_t eq = sym.find(":=");
    if (eq != std::string::npos) sym = sym.substr(0, eq);
    if (!sym.empty()) defines_.insert(sym);
    return;
  }
  if (head == "undef") {
    std::string sym = lower(trim(rest));
    defines_.erase(sym);
    return;
  }
  if (head == "i" || head == "include") {
    do_include(rest, where);
    return;
  }
  // Fatal / error directives -- report them.
  if (head == "fatal" || head == "error") {
    report_error(where, std::string("{$") + head + "} " + trim(rest));
    return;
  }
  if (head == "warning" || head == "message" || head == "note" || head == "hint") {
    report_warning(where, std::string("{$") + head + "} " + trim(rest));
    return;
  }

  // Pascal `{$Q+/-}` / `{$overflowchecks+/-}` toggle the integer-overflow
  // check. Track the live setting so `{$ifopt Q+}` queries above and the
  // parser's per-node snapshot both see it.
  if (head == "q+" || head == "overflowchecks+") {
    q_check_ = true;
    return;
  }
  if (head == "q-" || head == "overflowchecks-") {
    q_check_ = false;
    return;
  }
  if (head == "r+" || head == "rangechecks+") {
    r_check_ = true;
    return;
  }
  if (head == "r-" || head == "rangechecks-") {
    r_check_ = false;
    return;
  }
  if (head == "h+" || head == "longstrings+") {
    h_long_strings_ = true;
    return;
  }
  if (head == "h-" || head == "longstrings-") {
    h_long_strings_ = false;
    return;
  }
  if (head == "longstrings") {
    std::string mode = lower(trim(rest));
    if (mode == "on" || mode == "+") {
      h_long_strings_ = true;
    } else if (mode == "off" || mode == "-") {
      h_long_strings_ = false;
    } else {
      report_error(where, "illegal {$longstrings} value: " + trim(rest));
    }
    return;
  }
  // FPC's default enum storage is 4 bytes (`{$PACKENUM 4}` / `{$Z4}`), but
  // `{$PACKENUM 1}`, `{$PACKENUM 2}`, `{$MINENUMSIZE ...}`, and `{$Z1/$Z2/$Z4}`
  // all change the minimum storage width for subsequently declared enums.
  if (head == "z1") {
    packenum_ = 1;
    return;
  }
  if (head == "z2") {
    packenum_ = 2;
    return;
  }
  if (head == "z4") {
    packenum_ = 4;
    return;
  }
  if (head == "packenum" || head == "minenumsize") {
    if (std::optional<int> value = parse_packenum_value(rest)) {
      packenum_ = *value;
    } else {
      report_error(where, std::string("illegal {$") + head + "} value: " +
                              trim(rest));
    }
    return;
  }
  if (head == "interfaces") {
    std::string mode = lower(trim(rest));
    if (mode == "corba") {
      interface_mode_ = InterfaceMode::Corba;
    } else if (mode == "com") {
      interface_mode_ = InterfaceMode::Com;
    } else if (mode == "default") {
      interface_mode_ = default_interface_mode_;
    } else {
      report_error(where, "illegal {$interfaces} value: " + trim(rest));
    }
    return;
  }
  // We still ignore most mode semantics, but enum layout must follow the mode
  // default: old FPC uses packenum=1 in TP/TP7 and Delphi-compatible modes,
  // and packenum=4 in FPC/ObjFPC mode.
  if (head == "mode") {
    if (std::optional<int> value = mode_default_packenum(rest)) {
      packenum_ = *value;
    }
    return;
  }

  // Everything else (I+, F+, ASMMODE, L, linklib, appid,
  // apptype, memory, stacksize, heapsize, etc) is silently accepted.
}

bool Lexer::overflow_check_active() const {
  return q_check_;
}

bool Lexer::range_check_active() const {
  return r_check_;
}

int Lexer::packenum_active() const { return packenum_; }

InterfaceMode Lexer::interface_mode_active() const { return interface_mode_; }

bool Lexer::long_strings_active() const { return h_long_strings_; }

void Lexer::set_overflow_check_default(bool on) { q_check_ = on; }
void Lexer::set_range_check_default(bool on) { r_check_ = on; }

void Lexer::do_include(std::string_view arg, Location where) {
  std::string a = trim(arg);
  if (a.empty()) {
    report_error(where, "empty {$include ...}");
    return;
  }
  if (a.size() >= 2 && a.front() == '%' && a.back() == '%') {
    std::string macro = lower(a);
    std::string contents;
    if (macro == "%date%") contents = "'1970-01-01'";
    else if (macro == "%time%") contents = "'00:00:00'";
    else {
      report_error(where, "unsupported builtin include-macro {$I " + a + "}");
      return;
    }
    auto sf = std::make_shared<SourceFile>();
    sf->path = stack_.back().file->path + ":{$I " + a + "}";
    sf->contents = std::move(contents);
    Input in;
    in.file = std::move(sf);
    stack_.push_back(std::move(in));
    return;
  }

  // Strip surrounding quotes if present.
  if (a.size() >= 2 && a.front() == '\'' && a.back() == '\'') {
    a = a.substr(1, a.size() - 2);
  }

  // Resolve relative to the current source file's directory first, then
  // include_dirs_.
  std::filesystem::path p(a);
  if (!p.has_extension()) p += ".inc";

  std::filesystem::path resolved;
  if (p.is_absolute() && std::filesystem::exists(p)) {
    resolved = p;
  } else {
    std::filesystem::path here_dir =
        std::filesystem::path(stack_.back().file->path).parent_path();
    auto cand = here_dir / p;
    if (std::filesystem::exists(cand)) {
      resolved = cand;
    } else {
      for (const auto& d : include_dirs_) {
        auto c = d / p;
        if (std::filesystem::exists(c)) {
          resolved = c;
          break;
        }
      }
    }
  }
  if (resolved.empty()) {
    report_error(where, "cannot find include file: " + a);
    return;
  }
  auto sf = SourceFile::load(resolved);
  if (!sf) {
    report_error(where, "cannot read include file: " + resolved.string());
    return;
  }
  Input in;
  in.file = std::move(sf);
  stack_.push_back(std::move(in));
}

// ---------------------------------------------------------------------------
// Whitespace & comments

bool Lexer::handle_directive_at_brace() {
  // Assumes current char is '{' and next is '$'.
  Location where = here();
  get();  // consume '{'
  get();  // consume '$'
  // Read body until '}' (no nesting for directive bodies).
  std::string body;
  while (!at_eof_of_current()) {
    char c = peek();
    if (c == '}') { get(); break; }
    body.push_back(get());
  }
  handle_directive(body, where);
  return true;
}

bool Lexer::handle_directive_at_paren() {
  // Assumes current chars are '(' '*' '$'.
  Location where = here();
  get(); get(); get();  // '(' '*' '$'
  std::string body;
  while (!at_eof_of_current()) {
    char c = peek();
    if (c == '*' && peek(1) == ')') { get(); get(); break; }
    body.push_back(get());
  }
  handle_directive(body, where);
  return true;
}

void Lexer::skip_ws_and_comments() {
  for (;;) {
    if (pop_input_if_eof()) continue;
    char c = peek();
    if (c == 0) return;  // true EOF of root

    // Whitespace
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { get(); continue; }

    // Line comment `// ...`
    if (c == '/' && peek(1) == '/') {
      while (!at_eof_of_current() && peek() != '\n') get();
      continue;
    }

    // Brace form: `{ ... }`  or directive `{$ ... }`
    if (c == '{') {
      if (peek(1) == '$') {
        handle_directive_at_brace();
        continue;
      }
      // Brace comments can nest. Consume the opening `{` first so
      // `depth == 1` always means "inside the outermost comment".
      // Inside the body, `{$...}` is NOT a directive -- FPC only
      // recognizes a directive when `$` immediately follows the
      // *outermost* `{`. So we just count braces; an inner
      // `{$ifdef X}` is a balanced `{`/`}` pair (net zero depth).
      get();
      int depth = 1;
      while (depth > 0) {
        if (pop_input_if_eof()) continue;
        char cc = peek();
        if (cc == 0) break;
        if (cc == '{') {
          ++depth;
          get();
          continue;
        }
        if (cc == '}') {
          --depth;
          get();
          continue;
        }
        get();
      }
      continue;
    }

    // Paren-star: `(* ... *)` or directive `(*$ ... *)`
    if (c == '(' && peek(1) == '*') {
      if (peek(2) == '$') {
        handle_directive_at_paren();
        continue;
      }
      get(); get();
      while (!at_eof_of_current()) {
        if (peek() == '*' && peek(1) == ')') { get(); get(); break; }
        get();
      }
      continue;
    }

    return;  // not whitespace/comment/directive
  }
}

// ---------------------------------------------------------------------------
// Scanners

Token Lexer::scan_identifier_or_keyword() {
  Location loc = here();
  std::string text;
  while (!at_eof_of_current() && is_ident_cont(peek())) {
    text.push_back(get());
  }
  // Pascal identifiers are case-insensitive; keep lowercase for comparison
  // but keep original for emission? We'll lowercase -- emitter will preserve
  // casing by looking up a canonical form. For now, lowercase.
  std::string key = lower(text);
  auto it = keywords_.find(key);
  Token t;
  t.loc = loc;
  if (it != keywords_.end()) {
    t.kind = it->second;
    t.text = key;
  } else {
    t.kind = Tok::Ident;
    t.text = std::move(key);
  }
  return t;
}

Token Lexer::scan_number() {
  Location loc = here();
  Token t;
  t.loc = loc;
  t.kind = Tok::IntLit;

  std::string text;
  char c = peek();

  // Every integer-base path must reject literals that exceed a
  // 64-bit unsigned value -- the largest magnitude any legal Pascal
  // integer constant can denote.  `__builtin_{mul,add}_overflow'
  // sets a flag on overflow without producing UB; we keep
  // consuming digits so the whole token is absorbed (avoids
  // cascading parse errors on the tail), and emit a single
  // diagnostic.
  auto accumulate_digit = [](uint64_t& v, uint64_t base, uint64_t digit,
                             bool& ovf) {
    if (ovf) return;
    uint64_t tmp;
    if (__builtin_mul_overflow(v, base, &tmp) ||
        __builtin_add_overflow(tmp, digit, &v)) {
      ovf = true;
    }
  };

  if (c == '$') {
    // Hex.
    text.push_back(get());
    uint64_t v = 0;
    bool ovf = false;
    while (!at_eof_of_current() && is_hex(peek())) {
      char d = get();
      accumulate_digit(v, 16, static_cast<uint64_t>(hex_val(d)), ovf);
      text.push_back(d);
    }
    if (ovf) report_error(loc, "integer literal exceeds 64 bits");
    t.int_value = v;
    t.text = std::move(text);
    return t;
  }
  if (c == '%') {
    text.push_back(get());
    uint64_t v = 0;
    bool ovf = false;
    while (!at_eof_of_current() && (peek() == '0' || peek() == '1')) {
      char d = get();
      accumulate_digit(v, 2, static_cast<uint64_t>(d - '0'), ovf);
      text.push_back(d);
    }
    if (ovf) report_error(loc, "integer literal exceeds 64 bits");
    t.int_value = v;
    t.text = std::move(text);
    return t;
  }
  if (c == '&') {
    text.push_back(get());
    uint64_t v = 0;
    bool ovf = false;
    while (!at_eof_of_current() && peek() >= '0' && peek() <= '7') {
      char d = get();
      accumulate_digit(v, 8, static_cast<uint64_t>(d - '0'), ovf);
      text.push_back(d);
    }
    if (ovf) report_error(loc, "integer literal exceeds 64 bits");
    t.int_value = v;
    t.text = std::move(text);
    return t;
  }

  // Decimal, possibly real.
  uint64_t ivalue = 0;
  bool ivalue_ovf = false;
  while (!at_eof_of_current() && is_digit(peek())) {
    char d = get();
    accumulate_digit(ivalue, 10, static_cast<uint64_t>(d - '0'), ivalue_ovf);
    text.push_back(d);
  }
  bool is_real = false;
  // `..` is DotDot, not a decimal point -- careful.
  if (peek() == '.' && peek(1) != '.') {
    is_real = true;
    text.push_back(get());  // '.'
    while (!at_eof_of_current() && is_digit(peek())) text.push_back(get());
  }
  if (peek() == 'e' || peek() == 'E') {
    is_real = true;
    text.push_back(get());
    if (peek() == '+' || peek() == '-') text.push_back(get());
    while (!at_eof_of_current() && is_digit(peek())) text.push_back(get());
  }

  if (is_real) {
    t.kind = Tok::RealLit;
  } else {
    if (ivalue_ovf) report_error(loc, "integer literal exceeds 64 bits");
    t.int_value = ivalue;
  }
  t.text = std::move(text);
  return t;
}

Token Lexer::scan_string() {
  // Pascal strings: sequences of '...' pieces and #NN char codes, with
  // doubled '' being an embedded quote.
  Location loc = here();
  Token t;
  t.loc = loc;
  t.kind = Tok::StringLit;
  std::string out;
  for (;;) {
    char c = peek();
    if (c == '\'') {
      get();
      for (;;) {
        if (at_eof_of_current()) {
          report_error(loc, "unterminated string literal");
          t.text = std::move(out);
          return t;
        }
        char d = peek();
        if (d == '\n') {
          report_error(loc, "newline in string literal");
          t.text = std::move(out);
          return t;
        }
        if (d == '\'') {
          get();
          if (peek() == '\'') {
            out.push_back('\'');
            get();
            continue;
          }
          break;  // end of this piece
        }
        out.push_back(get());
      }
      continue;
    }
    if (c == '#') {
      Location esc_loc = here();
      get();
      // `#NN' character escape denotes a byte.  Any input value
      // outside [0,255] is a Pascal-level error; reject rather than
      // silently truncating.  Overflow during accumulation is
      // caught before the value is produced.
      uint32_t code = 0;
      bool ovf = false;
      auto eat_digit = [&](uint32_t base, uint32_t digit) {
        if (ovf) return;
        uint32_t tmp;
        if (__builtin_mul_overflow(code, base, &tmp) ||
            __builtin_add_overflow(tmp, digit, &code)) {
          ovf = true;
        }
      };
      if (peek() == '$') {
        get();
        while (!at_eof_of_current() && is_hex(peek())) {
          eat_digit(16, static_cast<uint32_t>(hex_val(get())));
        }
      } else {
        while (!at_eof_of_current() && is_digit(peek())) {
          eat_digit(10, static_cast<uint32_t>(get() - '0'));
        }
      }
      if (ovf || code > 0xff) {
        report_error(esc_loc,
                     "character escape `#' value out of range (expected 0..255)");
        code = 0;
      }
      out.push_back(static_cast<char>(code));
      continue;
    }
    break;
  }
  t.text = std::move(out);
  return t;
}

Token Lexer::scan_punctuation() {
  Location loc = here();
  Token t;
  t.loc = loc;
  t.kind = Tok::Error;

  char c = get();
  switch (c) {
    case '+': t.kind = Tok::Plus; t.text = "+"; break;
    case '-': t.kind = Tok::Minus; t.text = "-"; break;
    case '*':
      if (peek() == '*') { get(); t.kind = Tok::StarStar; t.text = "**"; }
      else { t.kind = Tok::Star; t.text = "*"; }
      break;
    case '/': t.kind = Tok::Slash; t.text = "/"; break;
    case '=': t.kind = Tok::Eq; t.text = "="; break;
    case '<':
      if (peek() == '=') { get(); t.kind = Tok::LtEq; t.text = "<="; }
      else if (peek() == '>') { get(); t.kind = Tok::NotEq; t.text = "<>"; }
      else if (peek() == '<') { get(); t.kind = Tok::KwShl; t.text = "<<"; }
      else { t.kind = Tok::Lt; t.text = "<"; }
      break;
    case '>':
      if (peek() == '=') { get(); t.kind = Tok::GtEq; t.text = ">="; }
      else if (peek() == '<') { get(); t.kind = Tok::SymDiff; t.text = "><"; }
      else if (peek() == '>') { get(); t.kind = Tok::KwShr; t.text = ">>"; }
      else { t.kind = Tok::Gt; t.text = ">"; }
      break;
    case ':':
      if (peek() == '=') { get(); t.kind = Tok::Assign; t.text = ":="; }
      else { t.kind = Tok::Colon; t.text = ":"; }
      break;
    case ';': t.kind = Tok::Semi; t.text = ";"; break;
    case ',': t.kind = Tok::Comma; t.text = ","; break;
    case '(': t.kind = Tok::LParen; t.text = "("; break;
    case ')': t.kind = Tok::RParen; t.text = ")"; break;
    case '[': t.kind = Tok::LBrack; t.text = "["; break;
    case ']': t.kind = Tok::RBrack; t.text = "]"; break;
    case '^': t.kind = Tok::Caret; t.text = "^"; break;
    case '@':
      if (peek() == '@') { get(); t.kind = Tok::AtAt; t.text = "@@"; }
      else { t.kind = Tok::At; t.text = "@"; }
      break;
    case '.':
      if (peek() == '.') { get(); t.kind = Tok::DotDot; t.text = ".."; }
      else { t.kind = Tok::Dot; t.text = "."; }
      break;
    default: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "unexpected character 0x%02x", (unsigned)(unsigned char)c);
      report_error(loc, buf);
      t.kind = Tok::Error;
      t.text.assign(1, c);
      break;
    }
  }
  return t;
}

// ---------------------------------------------------------------------------
// next()

Token Lexer::next() {
  for (;;) {
    skip_ws_and_comments();
    // After directives, we may have moved; check EOF.
    if (pop_input_if_eof()) continue;
    if (stack_.empty() || (stack_.size() == 1 && at_eof_of_current())) {
      Token t; t.kind = Tok::Eof; t.loc = here(); return t;
    }

    if (!accepting()) {
      // We are inside a rejected {$ifdef} branch: consume source until a
      // directive changes state. `skip_ws_and_comments()` already handles
      // directives, whitespace, and comments, but we must also swallow whole
      // Pascal string/char literals here. Otherwise a skipped literal like
      // `'{'
      // leaves the `{` behind for the next loop iteration, where
      // `skip_ws_and_comments()` misclassifies it as a comment/directive start
      // and can eat the rest of the file.
      if (at_eof_of_current()) continue;
      char c = peek();
      if (c == '\'' || c == '#') {
        (void)scan_string();
      } else {
        get();
      }
      continue;
    }

    char c = peek();
    if (is_ident_start(c)) return scan_identifier_or_keyword();
    if (is_digit(c) || c == '$' || c == '%' || c == '&') {
      // In the supported bootstrap dialect, '&' reaches only numeric
      // constants here; escaped identifiers are not accepted.
      return scan_number();
    }
    if (c == '\'' || c == '#') return scan_string();
    return scan_punctuation();
  }
}

}  // namespace tp2cc
