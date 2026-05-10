#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "source.h"

namespace tp2cc {

enum class Tok : uint16_t {
  // Control
  Eof = 0,
  Error,

  // Literals / ids
  Ident,
  IntLit,
  RealLit,
  StringLit,  // includes char literals; a single-char string is also valid
              // as a char -- parser decides.

  // Punctuation
  Plus,        // +
  Minus,       // -
  Star,        // *
  Slash,       // /
  Eq,          // =
  Lt,          // <
  Gt,          // >
  LtEq,        // <=
  GtEq,        // >=
  NotEq,       // <>
  SymDiff,     // ><    (set symmetric difference -- rare)
  StarStar,    // **    (rare)
  Assign,      // :=
  Caret,       // ^
  LBrack,      // [
  RBrack,      // ]
  Dot,         // .
  DotDot,      // ..
  Comma,       // ,
  LParen,      // (
  RParen,      // )
  Colon,       // :
  Semi,        // ;
  At,          // @
  AtAt,        // @@

  // Keywords (case-insensitive). Order is not significant.
  KwAbsolute, KwAbstract, KwAnd, KwArray, KwAs, KwAsm, KwAssembler,
  KwBegin, KwBreak,
  KwCase, KwCdecl, KwClass, KwConst, KwConstructor, KwContinue,
  KwDefault, KwDestructor, KwDispose, KwDiv, KwDo, KwDownto,
  KwElse, KwEnd, KwExcept, KwExit, KwExport, KwExports, KwExternal,
  KwFail, KwFalse, KwFar, KwFile, KwFinalization, KwFinally, KwFor,
    KwForward, KwFunction,
  KwGoto,
  KwIf, KwImplementation, KwIn, KwIndex, KwInherited, KwInitialization,
    KwInline, KwInterface, KwInterrupt, KwIs,
  KwLabel, KwLibrary,
  KwMessage, KwMod,
  KwName, KwNear, KwNew, KwNil, KwNot,
  KwObject, KwOf, KwOn, KwOperator, KwOr, KwOtherwise, KwOverride,
  KwPacked, KwPopstack, KwPascal, KwPrivate, KwProcedure, KwProgram,
    KwProperty, KwProtected, KwPublic, KwPublished,
  KwRaise, KwRead, KwRecord, KwRegister, KwRepeat, KwResident,
    KwResourcestring, KwResult,
  KwSafecall, KwSelf, KwSet, KwShl, KwShortstring, KwShr, KwStatic,
    KwStdcall, KwStored, KwString, KwSystem,
  KwThen, KwThreadvar, KwTo, KwTrue, KwTry, KwType,
  KwUnit, KwUntil, KwUses,
  KwVar, KwVirtual,
  KwWhile, KwWith, KwWrite,
  KwXor,
  // Pseudo-keyword: `alias` (in external 'foo' alias: 'bar'), `cvar`.
  KwAlias, KwCvar,
  // 'asmname', 'iocheck', 'syscall', 'saveregisters', 'nodefault',
  //  'internproc', 'internconst', 'openstring' are recognised only as
  //  identifiers by us -- not worth dedicated tokens unless needed.
};

struct Token {
  Tok kind = Tok::Eof;
  Location loc;
  // For Ident / StringLit / IntLit / RealLit: the literal text (unescaped for
  // strings, lowercased for Ident -- Pascal identifiers are case-insensitive).
  std::string text;
  // For IntLit only: the parsed magnitude of the literal.  Pascal
  // integer literals are always written unsigned; a leading `-' is
  // a unary operator applied to the literal, not part of it.  Full
  // 64-bit unsigned range is required for legitimate Pascal
  // constants such as `$ffffffffffffffff'.
  uint64_t int_value = 0;
  Token() = default;
  Token(Tok kind_in, Location loc_in, std::string text_in = {},
        uint64_t int_value_in = 0)
      : kind(kind_in),
        loc(loc_in),
        text(std::move(text_in)),
        int_value(int_value_in) {}
};

const char* tok_name(Tok t);

}  // namespace tp2cc
