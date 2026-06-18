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

size_t initial_input_pos(const SourceFile& file) {
  const std::string& s = file.contents;
  // Skip a leading UTF-8 BOM
  if (s.size() >= 3 &&
      static_cast<unsigned char>(s[0]) == 0xef &&
      static_cast<unsigned char>(s[1]) == 0xbb &&
      static_cast<unsigned char>(s[2]) == 0xbf) {
    return 3;
  }
  return 0;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b &&
         (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) {
    ++a;
  }
  while (b > a &&
         (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
          s[b - 1] == '\n')) {
    --b;
  }
  return std::string(s.substr(a, b - a));
}

struct MacroDefinition {
  std::string name;
  std::optional<std::string> text;
};

MacroDefinition parse_macro_definition(std::string_view rest) {
  std::string body = trim(rest);
  size_t eq = body.find(":=");
  if (eq == std::string::npos) {
    return MacroDefinition{std::move(body), std::nullopt};
  }
  std::string name = trim(std::string_view(body).substr(0, eq));
  std::string text = trim(std::string_view(body).substr(eq + 2));
  return MacroDefinition{std::move(name), std::move(text)};
}

}  // namespace

std::string Lexer::lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

// Numeric tokens and #NN string escapes keep consuming after overflow so one
// malformed literal produces one diagnostic instead of a cascade.
void Lexer::accumulate_digit(uint64_t& value, uint64_t base, uint64_t digit,
                             bool& overflow) {
  if (overflow) return;
  uint64_t tmp;
  if (__builtin_mul_overflow(value, base, &tmp) ||
      __builtin_add_overflow(tmp, digit, &value)) {
    overflow = true;
  }
}

void Lexer::accumulate_digit(uint32_t& value, uint32_t base, uint32_t digit,
                             bool& overflow) {
  if (overflow) return;
  uint32_t tmp;
  if (__builtin_mul_overflow(value, base, &tmp) ||
      __builtin_add_overflow(tmp, digit, &value)) {
    overflow = true;
  }
}

Lexer::Lexer(std::shared_ptr<SourceFile> root,
             std::vector<std::filesystem::path> include_dirs)
    : include_dirs_(std::move(include_dirs)) {
  const size_t pos = initial_input_pos(*root);
  stack_.push_back(Input{.file = std::move(root),
                         .pos = pos,
                         .line = 1,
                         .col = 1});

  for (auto& kv : kKeywordTable) keywords_.emplace(kv.first, kv.second);
}

void Lexer::define(std::string name) {
  MacroDefinition def = parse_macro_definition(name);
  std::string normalized = lower(def.name);
  if (!normalized.empty()) {
    defines_[std::move(normalized)] = std::move(def.text);
  }
}

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
  std::string error;
  const std::unordered_map<std::string, std::optional<std::string>>* defines;

  IfExprParser(
      std::string_view source,
      const std::unordered_map<std::string, std::optional<std::string>>*
          active_defines)
      : src(source), defines(active_defines) {}

  void fail(std::string msg) {
    ok = false;
    if (error.empty()) error = std::move(msg);
  }

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
  std::optional<Value> parse_macro_value(std::string_view text) {
    IfExprParser p{text, nullptr};
    if (p.match_word("true") && p.eof()) return Value::bool_value(true);
    if (p.match_word("false") && p.eof()) return Value::bool_value(false);
    if (auto v = p.read_int(); v && p.eof()) return Value::int_value(*v);
    return std::nullopt;
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
      if (!match_char('(')) {
        fail("expected `(' after `defined' in {$if} expression");
        return Value::bool_value(false);
      }
      std::string sym = read_ident();
      if (sym.empty()) {
        fail("expected symbol name in `defined(...)' in {$if} expression");
        return Value::bool_value(false);
      }
      if (!match_char(')')) {
        fail("expected `)' after symbol name in {$if} expression");
        return Value::bool_value(false);
      }
      return Value::bool_value(defines && defines->count(sym) > 0);
    }
    pos = saved;
    if (match_word("true")) return Value::bool_value(true);
    if (match_word("false")) return Value::bool_value(false);
    if (auto v = read_int()) return Value::int_value(*v);
    std::string ident = read_ident();
    if (!ident.empty()) {
      if (defines) {
        auto it = defines->find(ident);
        if (it != defines->end() && it->second) {
          if (auto v = parse_macro_value(*it->second)) return *v;
          fail("symbol `" + ident + "' has unsupported {$if} value `" +
               *it->second + "'");
          return Value::bool_value(false);
        }
      }
      fail("undefined symbol `" + ident + "' in {$if} expression");
      return Value::bool_value(false);
    }
    fail("expected operand in {$if} expression");
    return Value::bool_value(false);
  }
  Value parse_unary() {
    if (match_word("not")) {
      Value v = parse_unary();
      if (v.kind != Value::Kind::Bool) {
        fail("operator `not' requires a boolean operand in {$if} expression");
      }
      return Value::bool_value(!v.b);
    }
    return parse_primary();
  }
  Value parse_and() {
    Value v = parse_unary();
    while (match_word("and")) {
      Value r = parse_unary();
      if (v.kind != Value::Kind::Bool || r.kind != Value::Kind::Bool) {
        fail("operator `and' requires boolean operands in {$if} expression");
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
        fail("operator `or' requires boolean operands in {$if} expression");
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
      fail("cannot compare boolean and integer values in {$if} expression");
      return Value::bool_value(false);
    }
    if (lhs.kind == Value::Kind::Bool) {
      if (*op == "=") return Value::bool_value(lhs.b == rhs.b);
      if (*op == "<>") return Value::bool_value(lhs.b != rhs.b);
      fail("only `=' and `<>' are valid for boolean {$if} values");
      return Value::bool_value(false);
    }
    if (*op == "=") return Value::bool_value(lhs.i == rhs.i);
    if (*op == "<>") return Value::bool_value(lhs.i != rhs.i);
    if (*op == "<") return Value::bool_value(lhs.i < rhs.i);
    if (*op == ">") return Value::bool_value(lhs.i > rhs.i);
    if (*op == "<=") return Value::bool_value(lhs.i <= rhs.i);
    if (*op == ">=") return Value::bool_value(lhs.i >= rhs.i);
    fail("unsupported operator in {$if} expression");
    return Value::bool_value(false);
  }
};

}  // namespace

Lexer::IfEvalResult Lexer::eval_if_expr(std::string_view expr) {
  IfExprParser p{expr, &defines_};
  IfExprParser::Value v = p.parse_expr();
  if (!p.ok) {
    return IfEvalResult{false, false, std::move(p.error)};
  }
  if (!p.eof()) {
    return IfEvalResult{false, false,
                        "unexpected text after {$if} expression: `" +
                            trim(p.src.substr(p.pos)) + "`"};
  }
  if (v.kind != IfExprParser::Value::Kind::Bool) {
    return IfEvalResult{false, false,
                        "{$if} expression does not produce a boolean value"};
  }
  return IfEvalResult{true, v.b, {}};
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
    const bool accepting_branch = parent_ok && take;
    ifdef_stack_.push_back(IfdefFrame{.accepting = accepting_branch,
                                      .any_taken = accepting_branch,
                                      .in_else = false});
    return;
  }
  if (head == "if") {
    bool parent_ok = accepting();
    bool cond = false;
    if (parent_ok) {
      IfEvalResult v = eval_if_expr(rest);
      if (v.ok) {
        cond = v.value;
      } else {
        report_error(where, "cannot evaluate {$if}: " + v.error);
      }
    }
    const bool accepting_branch = parent_ok && cond;
    ifdef_stack_.push_back(IfdefFrame{.accepting = accepting_branch,
                                      .any_taken = accepting_branch,
                                      .in_else = false});
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
    else if (opt == "t+") cond = t_typed_addresses_;
    else if (opt == "t-") cond = !t_typed_addresses_;
    const bool accepting_branch = parent_ok && cond;
    ifdef_stack_.push_back(IfdefFrame{.accepting = accepting_branch,
                                      .any_taken = accepting_branch,
                                      .in_else = false});
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
      IfEvalResult v = eval_if_expr(rest);
      if (v.ok) {
        cond = v.value;
      } else {
        report_error(where, "cannot evaluate {$elseif}: " + v.error);
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
    MacroDefinition def = parse_macro_definition(rest);
    std::string normalized = lower(def.name);
    if (!normalized.empty()) {
      defines_[std::move(normalized)] = std::move(def.text);
    }
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
  if (head == "t-" || head == "typedaddress-") {
    t_typed_addresses_ = false;
    return;
  }
  if (head == "t+" || head == "typedaddress+") {
    report_error(where, "{$T+} typed address operator mode is not supported");
    return;
  }
  if (head == "typedaddress") {
    std::string mode = lower(trim(rest));
    if (mode == "-") {
      t_typed_addresses_ = false;
    } else if (mode == "+") {
      report_error(where,
                   "{$TYPEDADDRESS+} typed address operator mode is not supported");
    } else {
      report_error(where, "illegal {$typedaddress} value: " + trim(rest));
    }
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
    auto sf = std::make_shared<SourceFile>(
        stack_.back().file->path + ":{$I " + a + "}", std::move(contents));
    const size_t pos = initial_input_pos(*sf);
    stack_.push_back(Input{.file = std::move(sf),
                           .pos = pos,
                           .line = 1,
                           .col = 1});
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
  const size_t pos = initial_input_pos(*sf);
  stack_.push_back(Input{.file = std::move(sf),
                         .pos = pos,
                         .line = 1,
                         .col = 1});
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
  if (it != keywords_.end()) {
    return Token(it->second, loc, key);
  }
  return Token(Tok::Ident, loc, std::move(key));
}

Token Lexer::scan_number() {
  Location loc = here();

  std::string text;
  char c = peek();

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
    return Token(Tok::IntLit, loc, std::move(text), v);
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
    return Token(Tok::IntLit, loc, std::move(text), v);
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
    return Token(Tok::IntLit, loc, std::move(text), v);
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

  if (is_real) return Token(Tok::RealLit, loc, std::move(text));
  if (ivalue_ovf) report_error(loc, "integer literal exceeds 64 bits");
  return Token(Tok::IntLit, loc, std::move(text), ivalue);
}

Token Lexer::scan_string() {
  // Pascal strings: sequences of '...' pieces and #NN char codes, with
  // doubled '' being an embedded quote.
  Location loc = here();
  std::string out;
  for (;;) {
    char c = peek();
    if (c == '\'') {
      get();
      for (;;) {
        if (at_eof_of_current()) {
          report_error(loc, "unterminated string literal");
          return Token(Tok::StringLit, loc, std::move(out));
        }
        char d = peek();
        if (d == '\n') {
          report_error(loc, "newline in string literal");
          return Token(Tok::StringLit, loc, std::move(out));
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
      if (peek() == '$') {
        get();
        while (!at_eof_of_current() && is_hex(peek())) {
          accumulate_digit(code, 16, static_cast<uint32_t>(hex_val(get())),
                           ovf);
        }
      } else {
        while (!at_eof_of_current() && is_digit(peek())) {
          accumulate_digit(code, 10, static_cast<uint32_t>(get() - '0'), ovf);
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
  return Token(Tok::StringLit, loc, std::move(out));
}

Token Lexer::scan_punctuation() {
  Location loc = here();
  char c = get();
  switch (c) {
    case '+': return Token(Tok::Plus, loc, "+");
    case '-': return Token(Tok::Minus, loc, "-");
    case '*':
      if (peek() == '*') { get(); return Token(Tok::StarStar, loc, "**"); }
      return Token(Tok::Star, loc, "*");
    case '/': return Token(Tok::Slash, loc, "/");
    case '=': return Token(Tok::Eq, loc, "=");
    case '<':
      if (peek() == '=') { get(); return Token(Tok::LtEq, loc, "<="); }
      if (peek() == '>') { get(); return Token(Tok::NotEq, loc, "<>"); }
      if (peek() == '<') { get(); return Token(Tok::KwShl, loc, "<<"); }
      return Token(Tok::Lt, loc, "<");
    case '>':
      if (peek() == '=') { get(); return Token(Tok::GtEq, loc, ">="); }
      if (peek() == '<') { get(); return Token(Tok::SymDiff, loc, "><"); }
      if (peek() == '>') { get(); return Token(Tok::KwShr, loc, ">>"); }
      return Token(Tok::Gt, loc, ">");
    case ':':
      if (peek() == '=') { get(); return Token(Tok::Assign, loc, ":="); }
      return Token(Tok::Colon, loc, ":");
    case ';': return Token(Tok::Semi, loc, ";");
    case ',': return Token(Tok::Comma, loc, ",");
    case '(': return Token(Tok::LParen, loc, "(");
    case ')': return Token(Tok::RParen, loc, ")");
    case '[': return Token(Tok::LBrack, loc, "[");
    case ']': return Token(Tok::RBrack, loc, "]");
    case '^': return Token(Tok::Caret, loc, "^");
    case '@':
      if (peek() == '@') { get(); return Token(Tok::AtAt, loc, "@@"); }
      return Token(Tok::At, loc, "@");
    case '.':
      if (peek() == '.') { get(); return Token(Tok::DotDot, loc, ".."); }
      return Token(Tok::Dot, loc, ".");
    default: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "unexpected character 0x%02x", (unsigned)(unsigned char)c);
      report_error(loc, buf);
      return Token(Tok::Error, loc, std::string(1, c));
    }
  }
}

// ---------------------------------------------------------------------------
// next()

Token Lexer::next() {
  for (;;) {
    skip_ws_and_comments();
    // After directives, we may have moved; check EOF.
    if (pop_input_if_eof()) continue;
    if (stack_.empty() || (stack_.size() == 1 && at_eof_of_current())) {
      return Token(Tok::Eof, here());
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
