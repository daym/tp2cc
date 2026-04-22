#include "lexer.h"

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "diag.h"

namespace tp2cc {

namespace {

// Hard-reserved keywords. Other Pascal "keywords" (name, index, read, write,
// stored, default, message, alias, cvar, result, operator, on) are
// context-sensitive and are lexed as Ident; the parser recognizes them by
// text at the specific points where they matter.
const std::pair<const char*, Tok> kKeywordTable[] = {
    {"absolute", Tok::KwAbsolute},
    {"abstract", Tok::KwAbstract},
    {"and", Tok::KwAnd},
    {"array", Tok::KwArray},
    {"as", Tok::KwAs},
    {"asm", Tok::KwAsm},
    {"assembler", Tok::KwAssembler},
    {"begin", Tok::KwBegin},
    {"case", Tok::KwCase},
    {"cdecl", Tok::KwCdecl},
    {"class", Tok::KwClass},
    {"const", Tok::KwConst},
    {"constructor", Tok::KwConstructor},
    {"destructor", Tok::KwDestructor},
    {"div", Tok::KwDiv},
    {"do", Tok::KwDo},
    {"downto", Tok::KwDownto},
    // `dynamic' is a method directive, not a reserved keyword -- per
    // Pascal standard and FPC ref/refse4.html -- so it's not in this
    // table.  Recognised at directive positions via
    // is_directive("dynamic") in parse_proc_modifiers.
    {"else", Tok::KwElse},
    {"end", Tok::KwEnd},
    {"except", Tok::KwExcept},
    {"export", Tok::KwExport},
    {"exports", Tok::KwExports},
    {"external", Tok::KwExternal},
    {"false", Tok::KwFalse},
    {"far", Tok::KwFar},
    {"file", Tok::KwFile},
    {"finalization", Tok::KwFinalization},
    {"finally", Tok::KwFinally},
    {"for", Tok::KwFor},
    {"forward", Tok::KwForward},
    {"function", Tok::KwFunction},
    {"goto", Tok::KwGoto},
    {"if", Tok::KwIf},
    {"implementation", Tok::KwImplementation},
    {"in", Tok::KwIn},
    {"inherited", Tok::KwInherited},
    {"initialization", Tok::KwInitialization},
    {"inline", Tok::KwInline},
    {"interface", Tok::KwInterface},
    {"interrupt", Tok::KwInterrupt},
    {"is", Tok::KwIs},
    {"label", Tok::KwLabel},
    {"library", Tok::KwLibrary},
    {"mod", Tok::KwMod},
    {"near", Tok::KwNear},
    {"nil", Tok::KwNil},
    {"not", Tok::KwNot},
    {"object", Tok::KwObject},
    {"of", Tok::KwOf},
    {"or", Tok::KwOr},
    {"otherwise", Tok::KwOtherwise},
    {"override", Tok::KwOverride},
    {"packed", Tok::KwPacked},
    {"pascal", Tok::KwPascal},
    {"popstack", Tok::KwPopstack},
    {"private", Tok::KwPrivate},
    {"procedure", Tok::KwProcedure},
    {"program", Tok::KwProgram},
    {"property", Tok::KwProperty},
    {"protected", Tok::KwProtected},
    {"public", Tok::KwPublic},
    {"published", Tok::KwPublished},
    {"raise", Tok::KwRaise},
    {"record", Tok::KwRecord},
    {"register", Tok::KwRegister},
    {"repeat", Tok::KwRepeat},
    {"resident", Tok::KwResident},
    {"resourcestring", Tok::KwResourcestring},
    {"safecall", Tok::KwSafecall},
    {"self", Tok::KwSelf},
    {"set", Tok::KwSet},
    {"shl", Tok::KwShl},
    {"shortstring", Tok::KwShortstring},
    {"shr", Tok::KwShr},
    {"static", Tok::KwStatic},
    {"stdcall", Tok::KwStdcall},
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
    {"virtual", Tok::KwVirtual},
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

char Lexer::peek(int ahead) const {
  const auto& in = stack_.back();
  size_t p = in.pos + static_cast<size_t>(ahead);
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

}  // namespace

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
  if (head == "if" || head == "ifopt") {
    // Treat unknown `if` / `ifopt` conditions as FALSE conservatively.
    // This matches what most bootstrap use-cases need; we can refine later.
    bool parent_ok = accepting();
    IfdefFrame f;
    f.accepting = false;
    f.any_taken = false;
    f.in_else = false;
    (void)parent_ok;
    ifdef_stack_.push_back(f);
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

  // Everything else (mode, I+, R-, S-, H-, F+, ASMMODE, L, linklib, appid,
  // apptype, memory, stacksize, heapsize, etc) is silently accepted.
}

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
      // Regular brace comment.
      get();
      while (!at_eof_of_current() && peek() != '}') get();
      if (!at_eof_of_current()) get();  // consume '}'
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
    t.int_value = static_cast<int64_t>(v);
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
    t.int_value = static_cast<int64_t>(v);
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
    t.int_value = static_cast<int64_t>(v);
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
    t.int_value = static_cast<int64_t>(ivalue);
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
      else { t.kind = Tok::Lt; t.text = "<"; }
      break;
    case '>':
      if (peek() == '=') { get(); t.kind = Tok::GtEq; t.text = ">="; }
      else if (peek() == '<') { get(); t.kind = Tok::SymDiff; t.text = "><"; }
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
      // We are inside a rejected {$ifdef} branch: consume characters until
      // we hit a directive that changes state. skip_ws_and_comments already
      // processes directives, so we just need to eat one non-directive
      // character and loop.
      if (at_eof_of_current()) continue;
      get();
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
