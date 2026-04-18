#pragma once

#include <cstdint>
#include <string>

#include "source.h"

namespace p2cc {

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
  KwDefault, KwDestructor, KwDispose, KwDiv, KwDo, KwDownto, KwDynamic,
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
  // For IntLit only: the parsed numeric value.
  int64_t int_value = 0;
};

const char* tok_name(Tok t);

}  // namespace p2cc
