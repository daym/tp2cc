#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "source.h"
#include "token.h"

namespace tp2cc {

enum class InterfaceMode {
  Com,
  Corba,
};

// A Lexer that owns a stack of input sources (so {$include FOO} can push a new
// file) and a conditional-compilation state (IfdefStack).
class Lexer {
 public:
  explicit Lexer(std::shared_ptr<SourceFile> root,
                 std::vector<std::filesystem::path> include_dirs = {});

  // Predefine a symbol for {$ifdef}.
  void define(std::string name);
  void undefine(const std::string& name);

  // Produce the next token. Returns Tok::Eof when all sources exhausted.
  Token next();

  // Pascal `{$Q+}` / `{$OVERFLOWCHECKS+}` enable runtime overflow
  // detection on integer arithmetic. The fpc compiler's own source
  // is built with Q+ (see compiler/ppc.cfg) and uses the resulting
  // `EIntOverflow` exceptions as compile-time control flow (see
  // nadd.pas's `try ... except on E:EIntOverflow do ...` blocks).
  // Tracked here so `{$ifopt Q+}` queries return the live state and
  // the parser can stamp each arithmetic AST node with the active
  // setting.
  bool overflow_check_active() const;

  // Pascal `{$R+}` / `{$rangechecks+}` enables runtime range checks
  // (subrange/enum bounds, array indices, narrowing assignments).
  // Tracked alongside `{$Q+}`; the parser snapshots into Assign /
  // Index nodes that the emitter then routes through checked helpers.
  bool range_check_active() const;
  int packenum_active() const;
  InterfaceMode interface_mode_active() const;

  // Initial `{$Q+}` / `{$R+}` state supplied by `-Co` / `-Cr` command-line
  // flags. Source-level `{$Q+/-}` / `{$R+/-}` directives still override
  // forward from their position.
  void set_overflow_check_default(bool on);
  void set_range_check_default(bool on);

 private:
  struct Input {
    std::shared_ptr<SourceFile> file;
    size_t pos = 0;
    uint32_t line = 1;
    uint32_t col = 1;
  };

  // Read one char, advance; returns 0 on EOF of current input.
  char peek(size_t ahead = 0) const;
  char get();
  void unget();  // step back one (within current input; line/col rewinds)

  bool at_eof_of_current() const;
  bool pop_input_if_eof();   // pop input stack if at end; returns true if popped
  Location here() const;

  void skip_ws_and_comments();
  // Returns true if a directive was consumed (and produced no token).
  bool handle_directive_at_brace();   // call when peek()=='{' and peek(1)=='$'
  bool handle_directive_at_paren();   // call when peek()=='(' and peek(1)=='*' and peek(2)=='$'

  void handle_directive(std::string_view body, Location where);
  void do_include(std::string_view arg, Location where);
  // Evaluate a `{$if EXPR}` / `{$elseif EXPR}` body. Supports the
  // subset the bootstrap actually uses: `defined(SYM)`, `not`, `and`,
  // `or`, parens. Anything outside this subset evaluates to false --
  // unrecognised predicates skip both branches, which is the safer
  // default for our purposes.
  bool eval_if_expr(std::string_view expr);

  Token scan_identifier_or_keyword();
  Token scan_number();
  Token scan_string();      // '...'  with '' escape, concatenated with #NN
  Token scan_punctuation();

  // Conditional compilation.
  struct IfdefFrame {
    bool accepting;    // are we currently including tokens?
    bool any_taken;    // has any branch in this if-chain been taken?
    bool in_else;      // saw an {$else} already
  };
  bool accepting() const;           // top of stack says yes (or stack empty)
  void skip_until_matching_cond();  // used when a branch is rejected

  // Helpers.
  static std::string lower(std::string_view s);

  std::vector<Input> stack_;
  // SourceFiles whose Input has already been popped off stack_.  We
  // keep them alive so that `Location`s in the AST stay valid -- see
  // release_sources().
  std::vector<std::unique_ptr<SourceFile>> retired_;
  std::vector<IfdefFrame> ifdef_stack_;
  std::unordered_set<std::string> defines_;           // lowercased
  std::unordered_map<std::string, Tok> keywords_;     // lowercased -> kind
  std::vector<std::filesystem::path> include_dirs_;

  // Live `{$Q+/-}` / `{$OVERFLOWCHECKS+/-}` state. Pascal directives are
  // forward-only by file but units commonly save/restore via
  // `{$ifopt Q+}{$define ...}{$Q-}{$endif} ... {$ifdef ...}{$Q+}{$endif}`
  // patterns; the lexer just tracks the current value, the parser
  // snapshots it onto each arithmetic node.
  bool q_check_ = false;
  bool r_check_ = false;
  // FPC defaults to `{$PACKENUM 4}` outside TP mode. We don't track mode
  // directives yet, but we do honor explicit `{$PACKENUM ...}`,
  // `{$MINENUMSIZE ...}`, and `{$Z1/$Z2/$Z4}` switches at the point where an
  // enum type is parsed.
  int packenum_ = 4;

  // FPC's default is COM interfaces. That implies IUnknown-style
  // refcounting; p2cc currently only lowers explicit CORBA interfaces.
  InterfaceMode default_interface_mode_ = InterfaceMode::Com;
  InterfaceMode interface_mode_ = InterfaceMode::Com;

  // For peek() we keep the current input on the back of stack_.
};

}  // namespace tp2cc
