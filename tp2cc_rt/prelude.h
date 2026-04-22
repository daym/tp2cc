#pragma once

// Minimal runtime for tp2cc-emitted code.
//
// This header is deliberately small -- we only provide the pieces the
// translator currently emits references to. It will grow alongside M3/M4/M5
// as more Pascal features get translated.
//
// Target: Linux/i386. Translated Pascal primitive types map to fixed-width
// C++ types (see emit.cc's primitive_type_map); short strings, sets, and a
// few I/O helpers are implemented here.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <functional>
#include <glob.h>
#include <initializer_list>
#include <limits.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace rt {

// --- Pascal Char -----------------------------------------------------------
//
// Pascal `Char` is an ordinal type, but it is not a C++ arithmetic
// character. Keep it distinct so accidental integer promotion does not
// silently change semantics. `Ord` / `Chr` are the explicit boundary.

enum class p_char : uint8_t {};

inline constexpr p_char p_char_of(char c) {
  return static_cast<p_char>(static_cast<uint8_t>(c));
}
inline constexpr p_char p_char_of(p_char c) {
  return c;
}
inline constexpr p_char p_char_of(uint8_t c) {
  return static_cast<p_char>(c);
}
inline constexpr uint8_t p_char_byte(p_char c) {
  return static_cast<uint8_t>(c);
}
inline constexpr char p_char_to_c(p_char c) {
  return static_cast<char>(p_char_byte(c));
}
inline const char* p_c_str(const p_char* s) {
  return reinterpret_cast<const char*>(s);
}
inline char* p_c_str(p_char* s) {
  return reinterpret_cast<char*>(s);
}
inline p_char* p_from_c_str_copy(const char* s) {
  if (!s) return nullptr;
  thread_local std::vector<p_char> buf;
  size_t n = std::strlen(s);
  buf.resize(n + 1);
  for (size_t i = 0; i < n; ++i) buf[i] = p_char_of(s[i]);
  buf[n] = p_char_of('\0');
  return buf.data();
}

struct CharConst;

struct ShortStringCharValue {
  const uint8_t* byte = nullptr;

  constexpr explicit operator uint8_t() const { return *byte; }
  constexpr operator p_char() const { return p_char_of(*byte); }

  const p_char* operator&() const {
    return reinterpret_cast<const p_char*>(byte);
  }
};

struct ShortStringCharRef {
  uint8_t* byte = nullptr;

  constexpr ShortStringCharRef() = default;
  constexpr ShortStringCharRef(uint8_t* p) : byte(p) {}
  constexpr ShortStringCharRef(const ShortStringCharRef&) = default;
  constexpr explicit operator uint8_t() const { return *byte; }
  constexpr operator p_char() const { return p_char_of(*byte); }

  constexpr ShortStringCharRef& operator=(const ShortStringCharRef& other) {
    *byte = *other.byte;
    return *this;
  }

  template <typename T>
  requires std::is_convertible_v<T, p_char>
  constexpr ShortStringCharRef& operator=(T x) {
    *byte = p_char_byte(static_cast<p_char>(x));
    return *this;
  }

  constexpr ShortStringCharRef& operator=(uint8_t x) {
    *byte = x;
    return *this;
  }

  p_char* operator&() const {
    return reinterpret_cast<p_char*>(byte);
  }
};

// --- ShortString<N> --------------------------------------------------------
//
// Pascal-compatible short-string layout: 1 length byte followed by N content
// bytes. Fixed size. Default capacity is 255 (classic `string`).

template <int N = 255>
struct ShortString {
  static_assert(N >= 1 && N <= 255, "ShortString capacity must be 1..255");

  // No default member initialisers, so `std::is_trivial_v<ShortString>`
  // holds; that lets a ShortString live as a field of a packed record
  // without GCC silently dropping the `[[gnu::packed]]` attribute.
  // Consumers always read no further than `length`, and the emitter
  // value-inits locals (`ShortString<> s{};`) so Pascal `var s : string;`
  // still starts empty.
  uint8_t length;
  p_char data[N];

  constexpr ShortString() = default;

  // `: data{}` mem-init zeroes the tail past what we copy in -- load-
  // bearing now that `data` has no default member initialiser, or callers
  // that memcpy/bytewise-compare a returned ShortString would see stack
  // garbage in positions >= length.
  constexpr ShortString(const char* s) : data{} {
    int n = 0;
    if (s) {
      while (s[n] && n < N) ++n;
    }
    length = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) data[i] = p_char_of(s[i]);
  }
  constexpr ShortString(const p_char* s) : data{} {
    int n = 0;
    if (s) {
      while (p_char_byte(s[n]) != 0 && n < N) ++n;
    }
    length = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) data[i] = s[i];
  }

  constexpr ShortString(p_char c) : length(1), data{} { data[0] = c; }


  // Copy from a ShortString of any capacity (Pascal assigns freely
  // between different `string[N]` sizes; target capacity truncates).
  template <int M>
  constexpr ShortString(const ShortString<M>& o) : data{} {
    int n = o.length;
    if (n > N) n = N;
    length = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) data[i] = o.data[i];
  }

  template <int M>
  constexpr ShortString& operator=(const ShortString<M>& o) {
    int n = o.length;
    if (n > N) n = N;
    length = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) data[i] = o.data[i];
    return *this;
  }

  constexpr uint8_t size() const { return length; }
  constexpr bool empty() const { return length == 0; }

  // Cross-capacity equality -- declared as a friend template so
  // `s<255> == s<10>` is unambiguous.
  template <int M>
  friend constexpr bool operator==(const ShortString& a,
                                   const ShortString<M>& b) {
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; ++i)
      if (a.data[i] != b.data[i]) return false;
    return true;
  }
  template <int M>
  friend constexpr bool operator!=(const ShortString& a,
                                   const ShortString<M>& b) {
    return !(a == b);
  }

  // Pascal string ordering is per-byte, length-then-content isn't
  // right: compare characters up to min-length, shorter is smaller on
  // tie. Emit the full four relational ops so sort-like code works.
  template <int M>
  friend constexpr bool operator<(const ShortString& a,
                                  const ShortString<M>& b) {
    int n = a.length < b.length ? a.length : b.length;
    for (int i = 0; i < n; ++i) {
      if (a.data[i] < b.data[i]) return true;
      if (a.data[i] > b.data[i]) return false;
    }
    return a.length < b.length;
  }
  template <int M>
  friend constexpr bool operator>(const ShortString& a,
                                  const ShortString<M>& b) {
    return b < a;
  }
  template <int M>
  friend constexpr bool operator<=(const ShortString& a,
                                   const ShortString<M>& b) {
    return !(b < a);
  }
  template <int M>
  friend constexpr bool operator>=(const ShortString& a,
                                   const ShortString<M>& b) {
    return !(a < b);
  }

  // Pascal occasionally writes `s := +t;` -- a unary `+` on a string,
  // which is a no-op. Provide the operator so the emitted C++ mirror
  // (`s = +t;`) type-checks.
  friend constexpr ShortString operator+(const ShortString& a) { return a; }

  // Pascal `s1 + s2` concatenation across any two ShortString capacities.
  // Declared as a non-member friend with explicit template parameters so
  // mixing `ShortString<N1>` with `ShortString<N2>` is unambiguous -- each
  // instantiation produces exactly one best viable overload (result type
  // is whichever side has the larger capacity).
  template <int M>
  friend constexpr auto operator+(const ShortString& a,
                                  const ShortString<M>& b) {
    constexpr int R = (N > M ? N : M);
    ShortString<R> out{};
    int n = a.length + b.length;
    if (n > R) n = R;
    for (int i = 0; i < a.length && i < R; ++i) out.data[i] = a.data[i];
    int off = a.length;
    for (int i = 0; i + off < R && i < b.length; ++i)
      out.data[off + i] = b.data[i];
    out.length = static_cast<uint8_t>(n);
    return out;
  }

  friend constexpr ShortString operator+(const ShortString& a, const char* b) {
    return a + ShortString(b);
  }
  friend constexpr ShortString operator+(const char* a, const ShortString& b) {
    return ShortString(a) + b;
  }
  friend constexpr auto operator+(const ShortString& a, p_char c) {
    return a + ShortString<>(c);
  }
  friend constexpr auto operator+(p_char c, const ShortString& b) {
    return ShortString<>(c) + b;
  }

  // Pascal `s[i]` is 1-based. We model the access: index 0 gives the
  // length byte (as in TP memory layout), 1..length give the characters.
  constexpr ShortStringCharRef operator[](int i) {
    return ShortStringCharRef{
        i == 0 ? &length : reinterpret_cast<uint8_t*>(&data[i - 1])};
  }
  constexpr ShortStringCharValue operator[](int i) const {
    return ShortStringCharValue{
        i == 0 ? &length : reinterpret_cast<const uint8_t*>(&data[i - 1])};
  }
};

// Pascal `const X = 'c';` declares a constant that is BOTH a char
// (assignable into `s[i] : char` contexts) and a 1-element string
// (usable in string concatenations). C++ can't have one type that
// plays both roles, so the emitter wraps such consts in this tag
// struct with implicit conversions in both directions. Scoped to
// the const decl -- ordinary ShortString variables are unaffected.
struct CharConst {
  p_char c;
  // `explicit` so `p_char -> CharConst` is not viable silently; the
  // only way a CharConst is constructed is from an explicit `'c'` in
  // the emitted const decl.
  explicit constexpr CharConst(p_char x) : c(x) {}
  // Conversions: `p_char` for Pascal char contexts, and
  // `ShortString<N>` for string-concatenation contexts.
  constexpr operator p_char() const { return c; }
  template <int N = 255>
  constexpr operator ShortString<N>() const {
    return ShortString<N>(c);
  }
};

template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator==(A a, B b) {
  return p_char_byte(static_cast<p_char>(a)) ==
         p_char_byte(static_cast<p_char>(b));
}
template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator!=(A a, B b) {
  return !(a == b);
}
template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator<(A a, B b) {
  return p_char_byte(static_cast<p_char>(a)) <
         p_char_byte(static_cast<p_char>(b));
}
template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator<=(A a, B b) {
  return !(b < a);
}
template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator>(A a, B b) {
  return b < a;
}
template <typename A, typename B>
requires (std::is_convertible_v<A, p_char> &&
          std::is_convertible_v<B, p_char>)
inline constexpr bool operator>=(A a, B b) {
  return !(a < b);
}

// `+` on a CharConst / p_char chooses string concatenation: the only
// reason a Pascal 1-char value appears under `+` is to build a string.
inline constexpr ShortString<> operator+(CharConst a, CharConst b) {
  ShortString<> r{};
  r.data[0] = a.c;
  r.data[1] = b.c;
  r.length = 2;
  return r;
}
inline constexpr ShortString<> operator+(CharConst a, p_char b) {
  return ShortString<>(a.c) + b;
}
inline constexpr ShortString<> operator+(p_char a, CharConst b) {
  return a + ShortString<>(b.c);
}
template <int N>
inline constexpr auto operator+(CharConst a, const ShortString<N>& b) {
  return ShortString<>(a.c) + b;
}
template <int N>
inline constexpr auto operator+(const ShortString<N>& a, CharConst b) {
  return a + ShortString<>(b.c);
}

// --- Common Pascal RTL type aliases ----------------------------------------
// Exposed in the `rt` namespace so emitted units pick them up via
// `using namespace ::rt;`.

// dos unit
using p_dirstr  = ShortString<255>;
using p_namestr = ShortString<255>;
using p_extstr  = ShortString<255>;
using p_pathstr = ShortString<255>;
using p_comstr  = ShortString<255>;

// objects unit
using p_sw_integer = int32_t;
using p_sw_word    = uint32_t;

// linux / file descriptors
using p_thandle   = int32_t;
using p_tfiletime = int64_t;

// signal handler (syslinux) + POSIX signal numbers used by catch.pas.
using p_signalhandler = void (*)(int32_t);
inline constexpr int32_t p_sighup  = 1;
inline constexpr int32_t p_sigint  = 2;
inline constexpr int32_t p_sigquit = 3;
inline constexpr int32_t p_sigill  = 4;
inline constexpr int32_t p_sigtrap = 5;
inline constexpr int32_t p_sigabrt = 6;
inline constexpr int32_t p_sigbus  = 7;
inline constexpr int32_t p_sigfpe  = 8;
inline constexpr int32_t p_sigkill = 9;
inline constexpr int32_t p_sigsegv = 11;
inline constexpr int32_t p_sigterm = 15;

template <typename T>
inline constexpr int p_ordinal_value(T x) {
  if constexpr (std::is_convertible_v<T, p_char>)
    return static_cast<int>(p_char_byte(static_cast<p_char>(x)));
  else
    return static_cast<int>(x);
}

// --- Array<T, Lo, N> -------------------------------------------------------
// Pascal `array[Lo..Hi] of T`. Value-semantics (copied on pass, like
// Pascal), arbitrary lower bound, 1- or 0-based or whatever Pascal said.
//
// We DO NOT inherit from std::array: that adds an extra aggregate layer
// which breaks brace-elision for designated initialisers of element
// records (the fpc sources' typed consts use `(field: value; ...)` a
// lot).  Holding a bare C-array as the single member keeps `Array` a
// simple one-member aggregate, so `Array<R, Lo, N> a = {{.f=1},{.f=2}};`
// initialises exactly as expected.
template <typename T, auto Lo, int N>
struct Array {
  // `T data[N];` -- DELIBERATELY NOT `T data[N]{};`. A default member
  // initialiser makes `std::is_trivial_v<Array>` false even when `T` is
  // trivial, which in turn makes GCC silently ignore `[[gnu::packed]]`
  // on any packed record containing this Array. Leaving the array
  // uninitialised in the raw declaration is fine because the tp2cc
  // emitter's `emit_var_decl` adds `{}` at every local declaration
  // site, static-storage globals zero-init by C++ rules, and struct
  // fields get zeroed by their enclosing aggregate's `{...}` init.
  //
  // For `T` with virtuals, plain `T data[N];` still default-inits each
  // element (C++ rule for arrays of class type with a default ctor),
  // so vtables are set up. Matches Pascal `array of TFoo` semantics.
  T data[N];

  // Defaulted (not a user-provided body) so the default ctor is
  // trivial -- required for `is_trivial_v<Array>`, which the emitter's
  // per-field static_assert demands on packed-record members. The
  // non-default ctors below are user-*declared* but not-user-*provided*
  // for purposes of the trivial-default-ctor rule: only the default
  // ctor has to be trivial, and only non-copy/non-move ctors have to
  // be absent for trivial copy/move.
  constexpr Array() = default;

  // `Array<T, Lo, N> a = {e0, e1, ...};` -- each element converts to T,
  // so `{.field=v}` designated aggregate inits of element records work.
  // `: data{}` zeroes the tail when fewer than N initializers are given.
  constexpr Array(std::initializer_list<T> il) : data{} {
    int i = 0;
    for (auto& v : il) {
      if (i < N) data[i++] = v;
    }
  }

  // Convert from a ShortString. Matches Pascal's `array[1..N] of char =
  // 'some string'` idiom: characters fill the first part of the array,
  // remaining bytes stay zero. The `: data{}` mem-init is load-bearing
  // -- without it (and now that `data` has no default member initialiser)
  // the tail after the copied prefix would be uninitialised stack
  // garbage, breaking code that treats the array as a fixed-size char
  // buffer.
  template <int M>
  constexpr Array(const ShortString<M>& s) : data{} {
    int n = s.length < N ? s.length : N;
    for (int i = 0; i < n; ++i) {
      if constexpr (std::is_same_v<T, p_char>)
        data[i] = p_char_of(static_cast<uint8_t>(s.data[i]));
      else
        data[i] = static_cast<T>(s.data[i]);
    }
  }

  // Pascal single-char string literal as init for `array of char`. The
  // emitter turns a one-character Pascal string into a plain `char`, so
  // we need a ctor that accepts it and places the byte at position 0.
  // `: data{}` zeroes the rest -- see note on the ShortString ctor above.
  constexpr Array(char c) : data{} {
    if (N > 0) data[0] = static_cast<T>(c);
  }
  constexpr Array(p_char c) : data{} {
    if (N > 0) {
      if constexpr (std::is_same_v<T, p_char>)
        data[0] = c;
      else
        data[0] = static_cast<T>(p_char_to_c(c));
    }
  }

  template <typename Ix>
  constexpr T& operator[](Ix i) {
    return data[p_ordinal_value(i) - p_ordinal_value(Lo)];
  }
  template <typename Ix>
  constexpr const T& operator[](Ix i) const {
    return data[p_ordinal_value(i) - p_ordinal_value(Lo)];
  }

  constexpr T* begin()             { return data; }
  constexpr T* end()               { return data + N; }
  constexpr const T* begin() const { return data; }
  constexpr const T* end()   const { return data + N; }

  // Decay to pointer for `char*` / `uint8_t*` Pascal idioms like `@buf`
  // assigned to a pchar. Only active for byte-sized T.
  template <typename U = T,
            typename = std::enable_if_t<sizeof(U) == 1>>
  constexpr operator const p_char*() const {
    return reinterpret_cast<const p_char*>(data);
  }
  template <typename U = T,
            typename = std::enable_if_t<sizeof(U) == 1>>
  constexpr operator p_char*() {
    return reinterpret_cast<p_char*>(data);
  }

  static constexpr auto low()  { return Lo; }
  static constexpr auto high() {
    return static_cast<decltype(Lo)>(p_ordinal_value(Lo) + N - 1);
  }
};

template <typename T>
struct OpenArray {
  T* data = nullptr;
  int32_t count = 0;

  constexpr OpenArray() = default;
  constexpr OpenArray(T* p, int32_t n) : data(p), count(n) {}

  template <typename U, auto Lo, int N>
  requires std::is_convertible_v<U*, T*>
  constexpr OpenArray(Array<U, Lo, N>& a) : data(a.data), count(N) {}

  template <typename U, auto Lo, int N>
  requires std::is_convertible_v<const U*, T*>
  constexpr OpenArray(const Array<U, Lo, N>& a) : data(a.data), count(N) {}

  template <int N>
  requires std::is_convertible_v<p_char*, T*>
  constexpr OpenArray(ShortString<N>& s) : data(s.data), count(s.length) {}

  template <int N>
  requires std::is_convertible_v<const p_char*, T*>
  constexpr OpenArray(const ShortString<N>& s) : data(s.data), count(s.length) {}

  constexpr T& operator[](int32_t i) { return data[i]; }
  constexpr const T& operator[](int32_t i) const { return data[i]; }

  constexpr T* begin() { return data; }
  constexpr T* end() { return data + count; }
  constexpr const T* begin() const { return data; }
  constexpr const T* end() const { return data + count; }

  constexpr int32_t low() const { return 0; }
  constexpr int32_t high() const { return count - 1; }
};

template <typename Arr>
struct ByteReinterpreter;

template <typename Elem, auto Lo, int N>
struct ByteReinterpreter<Array<Elem, Lo, N>> {
  template <typename Src>
  static Array<Elem, Lo, N> cast(const Src& src) {
    static_assert(std::is_same_v<Elem, uint8_t> ||
                      std::is_same_v<Elem, p_char>,
                  "byte reinterpretation only supports byte-sized arrays");
    Array<Elem, Lo, N> out{};
    const auto* raw = reinterpret_cast<const uint8_t*>(&src);
    const int bytes = static_cast<int>(
        std::min<std::size_t>(sizeof(src), sizeof(out.data)));
    for (int i = 0; i < bytes; ++i) {
      if constexpr (std::is_same_v<Elem, p_char>)
        out.data[i] = p_char_of(raw[i]);
      else
        out.data[i] = raw[i];
    }
    return out;
  }
};

// Pascal typecasts like `array[0..9] of byte(x)` reinterpret raw
// storage bytes; they are not element-wise numeric conversions.
template <typename Arr, typename Src>
inline Arr p_reinterpret_bytes(const Src& src) {
  return ByteReinterpreter<Arr>::cast(src);
}

template <typename T, typename Src>
inline T& p_reinterpret_ref(Src& src) {
  return *reinterpret_cast<T*>(&src);
}
template <typename T, typename Src>
inline const T& p_reinterpret_ref(const Src& src) {
  return *reinterpret_cast<const T*>(&src);
}
template <typename T>
inline T& p_reinterpret_ref(ShortStringCharRef src) {
  return *reinterpret_cast<T*>(src.byte);
}
template <typename T>
inline const T& p_reinterpret_ref(ShortStringCharValue src) {
  return *reinterpret_cast<const T*>(src.byte);
}
template <typename T>
inline T& p_reinterpret_ref(void* p) {
  return *reinterpret_cast<T*>(p);
}
template <typename T>
inline const T& p_reinterpret_ref(const void* p) {
  return *reinterpret_cast<const T*>(p);
}

// --- Set<Elem> --------------------------------------------------------------
//
// A 256-bit set, wide enough for `set of byte`, `set of char`, and every
// enum-backed Pascal set we encounter (emitted code may cast enum values to
// integers that exceed 63, so a wider mask is essential).
//
// The storage is a bare `unsigned char[32]` with alignment 1 -- intentionally,
// NOT a `uint64_t[4]`. Reason: a Pascal `packed record` maps to a C++ struct
// wrapped in `#pragma pack(push, 1)`, which places all fields at byte
// granularity. A `Set` member of such a record then lands at an arbitrary
// byte offset within the record (and, once placed in an array, most elements
// have the field at an address that isn't 4- or 8-aligned). Calling any
// member function on that misaligned `Set` -- e.g. `rec.p_flags.add(x)` --
// forms a `Set* this` pointer that has lost the "I came from a packed
// struct" information; inside the method the compiler assumes the pointer
// has the type's natural alignment and emits aligned loads/stores through it.
// If the internal storage were `uint64_t[4]`, those loads/stores would be
// unaligned and are UB under the C++ abstract machine. UBSan's
// `-fsanitize=alignment` catches exactly this, and on strict-aligning
// architectures it would fault outright. Using a 1-byte-aligned element
// type makes every access on `this` trivially aligned regardless of where
// the `Set` actually sits, so packed-record membership is safe. See the
// tp2cc codegen -- it emits `#pragma pack(push, 1)` whenever the Pascal
// source says `packed record` (e.g. `ttargetinfo` in `compiler/systems.pas`
// which has a `set of ttargetflags` field).
//
// All bit operations below are written byte-wise to preserve that property.

template <typename Elem>
struct Set {
  static constexpr int Nb = 32;  // 32 bytes == 256 bits.
  // No default member initialiser: keeps `is_trivial_v<Set>` so `Set`
  // can live inside a packed record without GCC dropping the packing.
  // `Set` helpers (`from_list`, `set_of`, the emitter's set-range
  // lambda) value-init with `Set s{};` before calling `add` so the
  // unset bits are zeroed; otherwise `.contains()` would return true
  // for arbitrary values.
  unsigned char bits[Nb];

  constexpr Set() = default;

  // Cross-element conversion: same bit-layout, different `Elem`
  // type. Pascal treats `set of char`, `set of byte`, `set of
  // 0..255` etc. as interchangeable at the value level; C++ templates
  // instantiate these as distinct types, so we provide an implicit
  // conversion constructor that just copies the bitmask.
  template <typename Other>
  constexpr Set(const Set<Other>& o) {
    for (int i = 0; i < Nb; ++i) bits[i] = o.bits[i];
  }

  // Adopt the byte-layout of a 32-byte `array[0..31] of byte` as the
  // bitmask. Pascal's in-memory `set` layout matches this for
  // `set of 0..255` / `set of byte`, so an explicit cast
  // `byteset(arr)` is just a reinterpretation. `: bits{}` zeroes the
  // tail when `N < Nb`.
  template <auto Lo, int N,
            typename = std::enable_if_t<(N * sizeof(uint8_t) <= Nb)>>
  constexpr Set(const Array<uint8_t, Lo, N>& a) : bits{} {
    const int n = (N < Nb) ? N : Nb;
    for (int i = 0; i < n; ++i) bits[i] = a.data[i];
  }

  static constexpr int idx(Elem e) {
    if constexpr (std::is_same_v<Elem, p_char>)
      return static_cast<int>(p_char_byte(e));
    else
      return static_cast<int>(static_cast<int64_t>(e));
  }

  static Set from_list(std::initializer_list<Elem> xs) {
    // Value-init; `Set` has no default member initialisers, so a bare
    // `Set s;` would leave the bitmask uninitialised and the
    // subsequent `s.add(x)` calls would only set specific bits on top
    // of stack garbage.
    Set s{};
    for (auto x : xs) s.add(x);
    return s;
  }
  constexpr void add(Elem e) {
    int i = idx(e);
    if (i >= 0 && i < 8 * Nb)
      bits[i >> 3] |= static_cast<unsigned char>(1u << (i & 7));
  }
  constexpr bool contains(Elem e) const {
    int i = idx(e);
    if (i < 0 || i >= 8 * Nb) return false;
    return (bits[i >> 3] & (1u << (i & 7))) != 0;
  }
  friend constexpr Set operator+(Set a, Set b) {
    Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] | b.bits[i]; return r;
  }
  friend constexpr Set operator-(Set a, Set b) {
    Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] & ~b.bits[i]; return r;
  }
  friend constexpr Set operator*(Set a, Set b) {
    Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] & b.bits[i]; return r;
  }
  friend constexpr bool operator==(Set a, Set b) {
    for (int i = 0; i < Nb; ++i) if (a.bits[i] != b.bits[i]) return false;
    return true;
  }
  friend constexpr bool operator!=(Set a, Set b) { return !(a == b); }
  friend constexpr bool operator<=(Set a, Set b) {
    // subset test
    for (int i = 0; i < Nb; ++i) if ((a.bits[i] & ~b.bits[i]) != 0) return false;
    return true;
  }
};

template <typename Elem>
Set<Elem> set_of(std::initializer_list<Elem> xs) {
  return Set<Elem>::from_list(xs);
}

// Mixed-type variadic `set_of` -- Pascal set literals like
// `[newline, #13, '{', ';']` mix a CharConst (our wrapper for Pascal
// `const X = 'c'`) with plain char literals. A single
// `initializer_list<Elem>` can't deduce Elem across distinct argument
// types, so take them as a variadic pack and add each explicitly.
// The first argument's type drives the Set's element type.
namespace detail {
template <typename T> struct set_elem_type { using type = T; };
template <> struct set_elem_type<char> { using type = p_char; };
template <> struct set_elem_type<CharConst> { using type = p_char; };
}
template <typename T, typename... Rest>
inline auto set_of(T first, Rest... rest) {
  using E = typename detail::set_elem_type<T>::type;
  Set<E> s{};  // value-init: zero the bits[] -- see note on from_list
  s.add(static_cast<E>(first));
  (s.add(static_cast<E>(rest)), ...);
  return s;
}

// Empty set-literal sentinel. Pascal `[]` has no element type on its own
// (the type is inferred from use context). We emit it as `EmptySet{}`
// which implicitly converts to any Set<T>.
struct EmptySet {
  template <typename T>
  constexpr operator Set<T>() const { return {}; }
};
inline Set<int> set_of(std::initializer_list<EmptySet>) { return {}; }
inline EmptySet set_of() { return {}; }

// Set-literal element: either a single value or a range `lo..hi`.  We
// model heterogeneous set literals with a type-erased element, then
// construct the Set by walking and adding each element (ranges expand).
template <typename Elem>
struct SetElem {
  bool is_range = false;
  Elem lo{}, hi{};
  SetElem(Elem v) : is_range(false), lo(v), hi(v) {}
  SetElem(Elem a, Elem b) : is_range(true), lo(a), hi(b) {}
};

template <typename Elem>
Set<Elem> set_of_range(std::initializer_list<SetElem<Elem>> xs) {
  Set<Elem> s{};  // value-init; see note on Set::from_list
  for (const auto& x : xs) {
    if (x.is_range) {
      for (int64_t v = static_cast<int64_t>(x.lo);
           v <= static_cast<int64_t>(x.hi); ++v) {
        s.add(static_cast<Elem>(v));
      }
    } else {
      s.add(x.lo);
    }
  }
  return s;
}

// --- Text I/O stub ----------------------------------------------------------

struct TextFile {
  std::FILE* f = nullptr;
  // `{}` needed because `ShortString` no longer carries a default
  // member initialiser on its own fields (would disqualify it from
  // `is_trivial` and break its use as a packed-record member).
  ShortString<> name{};
  int32_t iores = 0;  // last IOResult
};

// Pascal `file of T` - minimal stub; behaviour added as needed.
template <typename T>
struct TypedFile {
  std::FILE* f = nullptr;
  ShortString<> name{};
  int32_t iores = 0;
};

// --- Range helper (placeholder for `a..b` in set literals) ------------------

struct Range {
  int64_t lo, hi;
};
inline Range range(int64_t a, int64_t b) { return {a, b}; }

// --- Pascal value-builtins named exactly as Pascal calls them -------------
// Keeping these in sync with Pascal's names means the emitter passes
// calls through verbatim -- no translation table needed.

template <int N> inline int p_length(const ShortString<N>& s) { return s.length; }
template <typename T> inline int p_length(const std::array<T, 0>&) { return 0; }

template <typename T>
requires std::is_convertible_v<T, p_char>
inline constexpr int32_t p_ord(T x) {
  return static_cast<int32_t>(p_char_byte(static_cast<p_char>(x)));
}
template <typename T>
requires (!std::is_convertible_v<T, p_char>)
inline constexpr int32_t p_ord(T x) {
  return static_cast<int32_t>(x);
}
inline constexpr p_char p_chr(int x) { return p_char_of(static_cast<uint8_t>(x)); }

// Pascal `swap` -- byte-swap the two halves of a word or longint.
// Real fpc emits it for endianness handling in .ppu file I/O.
inline uint16_t p_swap(uint16_t w) {
  return static_cast<uint16_t>((w >> 8) | (w << 8));
}
inline uint32_t p_swap(uint32_t l) {
  return (l >> 16) | (l << 16);
}
inline int16_t p_swap(int16_t w) {
  return static_cast<int16_t>(p_swap(static_cast<uint16_t>(w)));
}
inline int32_t p_swap(int32_t l) {
  return static_cast<int32_t>(p_swap(static_cast<uint32_t>(l)));
}

// Pascal `ptr^` becomes `p_deref(ptr)`. For typed pointers this is just
// `*ptr`. For untyped (`pointer` -> `void*`), expose the first byte so
// code that writes through the deref still compiles; the translated units
// using this pattern (settextbuf buffers, heap-trace hooks) only touch
// these values behind stubbed helpers.
template <typename T> inline T& p_deref(T* p) { return *p; }
inline char& p_deref(void* p) { return *static_cast<char*>(p); }
inline const char& p_deref(const void* p) { return *static_cast<const char*>(p); }

template <typename T> inline bool p_assigned(T* p) { return p != nullptr; }
template <typename T> inline bool p_odd(T x) { return (static_cast<int64_t>(x) & 1) != 0; }

// Pascal `not` is polymorphic: logical-not for bool, bit-not for int.
inline constexpr bool p_not(bool b) { return !b; }
template <typename T>
inline constexpr T p_not(T x) {
  if constexpr (std::is_same_v<T, bool>) return !x;
  else return static_cast<T>(~static_cast<int64_t>(x));
}

template <typename T> inline T p_abs(T x) { return x < 0 ? -x : x; }
template <typename T> inline T p_sqr(T x) { return x * x; }
// Pascal `sqrt` coerces integer args via the float overload; express
// that with a generic template so `sqrt(int)` resolves without
// ambiguity between `double` and `long double` overloads.
template <typename T> inline long double p_sqrt(T x) {
  return std::sqrt(static_cast<long double>(x));
}
inline double      p_sin(double x)          { return std::sin(x); }
inline double      p_cos(double x)          { return std::cos(x); }
inline double      p_ln(double x)           { return std::log(x); }
inline double      p_exp(double x)          { return std::exp(x); }
inline double      p_arctan(double x)       { return std::atan(x); }
inline int32_t     p_trunc(double x)        { return static_cast<int32_t>(x); }
inline int32_t     p_round(double x)        { return static_cast<int32_t>(std::lround(x)); }
inline double      p_int(double x)          { return std::trunc(x); }
inline double      p_frac(double x)         { return x - std::trunc(x); }

template <typename T> inline T p_succ(T x) { return static_cast<T>(static_cast<int64_t>(x) + 1); }
template <typename T> inline T p_pred(T x) { return static_cast<T>(static_cast<int64_t>(x) - 1); }

// Pascal `inc`/`dec` work on ordinal types, including enums, and on
// typed pointers (pointer arithmetic in units of `sizeof(*ptr)`).
// Integers use builtin ++/--, enums go through int64_t so unscoped
// enum operands work, pointers stay in pointer arithmetic.
template <typename T> inline void p_inc(T& x) {
  if constexpr (std::is_enum_v<T>)
    x = static_cast<T>(static_cast<int64_t>(x) + 1);
  else ++x;
}
template <typename T, typename N> inline void p_inc(T& x, N n) {
  if constexpr (std::is_pointer_v<T>) x += n;
  else x = static_cast<T>(static_cast<int64_t>(x) + n);
}
template <typename T> inline void p_dec(T& x) {
  if constexpr (std::is_enum_v<T>)
    x = static_cast<T>(static_cast<int64_t>(x) - 1);
  else --x;
}
template <typename T, typename N> inline void p_dec(T& x, N n) {
  if constexpr (std::is_pointer_v<T>) x -= n;
  else x = static_cast<T>(static_cast<int64_t>(x) - n);
}

// No rvalue `p_inc`/`p_dec` overloads here: casted-lvalue forms like
// `inc(longint(p))` are emitted as `p_inc(p_reinterpret_ref<int32_t>(p))`,
// so callers still reach `p_inc` / `p_dec` with a true lvalue.

// --- Missing small RTL procedures ------------------------------------------

inline int32_t p_memavail() { return 1 << 30; }   // stub: "lots of memory"
inline int32_t p_heapavail() { return 1 << 30; }
inline int32_t p_maxavail()  { return 1 << 30; }

inline int32_t p_last_ioresult = 0;

template <typename File>
inline void p_set_ioresult(File& f, int32_t code) {
  f.iores = code;
  p_last_ioresult = code;
}

inline ShortString<> p_file_name_to_string(const ShortString<>& name) {
  return name;
}

template <typename File>
inline void p_file_name_to_buf(const File& f, char (&buf)[260]) {
  int n = f.name.length < 255 ? f.name.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = p_char_to_c(f.name.data[i]);
  buf[n] = '\0';
}

template <int N>
inline std::string p_to_std_string(const ShortString<N>& s) {
  std::string out;
  out.reserve(s.length);
  for (int i = 0; i < s.length; ++i) out.push_back(p_char_to_c(s.data[i]));
  return out;
}

inline std::string p_to_std_string(const char* s) {
  return s ? std::string(s) : std::string();
}
inline std::string p_to_std_string(const p_char* s) {
  return s ? std::string(p_c_str(s)) : std::string();
}

inline int32_t p_strtoint(const ShortString<>& s) {
  char buf[260]{};
  for (int i = 0; i < s.length; ++i) buf[i] = p_char_to_c(s.data[i]);
  return std::atoi(buf);
}

inline int p_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

template <typename Int>
inline void p_parse_pascal_integer(const std::string& buf, Int& out,
                                   int32_t& code) {
  static_assert(std::is_integral_v<Int> && !std::is_same_v<Int, bool>);

  using UInt = std::make_unsigned_t<Int>;
  auto fail = [&](std::size_t pos) {
    out = 0;
    code = static_cast<int32_t>(pos + 1);
  };

  std::size_t i = 0;
  while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t')) ++i;

  bool neg = false;
  if (i < buf.size() && (buf[i] == '+' || buf[i] == '-')) {
    neg = buf[i] == '-';
    ++i;
  }

  int base = 10;
  if (i < buf.size()) {
    switch (buf[i]) {
      case '$':
        base = 16;
        ++i;
        while ((i + 1) < buf.size() && buf[i] == '0') ++i;
        break;
      case '%':
        base = 2;
        ++i;
        break;
    }
  }

  if (i >= buf.size()) {
    fail(i);
    return;
  }

  if constexpr (std::is_unsigned_v<Int>) {
    if (neg) {
      fail(i);
      return;
    }
  }

  UInt max_value = std::numeric_limits<UInt>::max();
  if constexpr (std::is_signed_v<Int>) {
    if (base == 10) {
      const UInt max_signed = static_cast<UInt>(std::numeric_limits<Int>::max());
      max_value = neg ? (max_signed + UInt{1}) : max_signed;
    }
  }

  UInt value = 0;
  bool any = false;
  for (; i < buf.size(); ++i) {
    int d = p_digit_value(buf[i]);
    if (d < 0 || d >= base) {
      fail(i);
      return;
    }
    any = true;

    const UInt ub = static_cast<UInt>(base);
    const UInt ud = static_cast<UInt>(d);
    if (value > (max_value / ub)) {
      fail(i);
      return;
    }

    const UInt next = value * ub;
    if (next > (max_value - ud)) {
      fail(i);
      return;
    }
    value = next + ud;
  }

  if (!any) {
    fail(i);
    return;
  }

  if constexpr (std::is_signed_v<Int>) {
    if (neg) value = UInt(0) - value;
  }
  out = static_cast<Int>(value);
  code = 0;
}

inline std::vector<std::string> p_split_commandline(const std::string& s) {
  std::vector<std::string> args;
  std::string cur;
  char quote = '\0';
  bool escape = false;

  for (char c : s) {
    if (escape) {
      cur += c;
      escape = false;
      continue;
    }
    if (quote != '\'') {
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (quote == '"' && c == '"') {
        quote = '\0';
        continue;
      }
    }
    if (quote == '\'' && c == '\'') {
      quote = '\0';
      continue;
    }
    if (quote == '\0' && (c == '\'' || c == '"')) {
      quote = c;
      continue;
    }
    if (quote == '\0' &&
        std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!cur.empty()) {
        args.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }

  if (escape) cur += '\\';
  if (!cur.empty()) args.push_back(cur);
  return args;
}

inline int32_t p_doserror = 0;
inline int32_t p_last_dosexitcode = 0;

inline void p_store_wait_status(int status) {
  p_doserror = 0;
  if (WIFEXITED(status)) {
    p_last_dosexitcode = WEXITSTATUS(status);
    // glibc's posix_spawnp may defer exec failures to the child instead of reporting them synchronously, and in qemu transparent emulation this does happen.
    if (p_last_dosexitcode == 127) {
      p_doserror = 8;
      p_last_dosexitcode = 0;
    }
  } else if (WIFSIGNALED(status)) {
    p_last_dosexitcode = 128 + WTERMSIG(status);
  } else {
    p_last_dosexitcode = 1;
  }
}

inline void p_spawn_process(const std::vector<std::string>& args) {
  p_last_dosexitcode = 0;
  if (args.empty() || args[0].empty()) {
    p_doserror = ENOENT;
    return;
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  int spawn_err = ::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(),
                                 ::environ);
  if (spawn_err != 0) {
    p_doserror = spawn_err;
    return;
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      p_doserror = errno;
      p_last_dosexitcode = 0;
      return;
    }
  }

  p_store_wait_status(status);
}

// Dos/file procedures -- stubbed; real behaviour added as needed.
struct SearchRec { int32_t p_time = 0; int32_t p_size = 0;
                   uint8_t p_attr = 0; ShortString<> p_name{};
                   std::vector<std::string> p_matches;
                   std::size_t p_index = 0; };
using p_searchrec = SearchRec;
using p_tsearchrec = SearchRec;
inline void p_searchrec_fill(SearchRec& rec, const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return;
  rec.p_time = static_cast<int32_t>(st.st_mtime);
  rec.p_size = static_cast<int32_t>(st.st_size > INT32_MAX ? INT32_MAX : st.st_size);
  rec.p_attr = 0;
  if (S_ISDIR(st.st_mode)) rec.p_attr |= 0x10;
  std::size_t sep = path.find_last_of("/\\");
  rec.p_name = ShortString<>((sep == std::string::npos ? path : path.substr(sep + 1)).c_str());
}
inline int32_t p_findfirst(const ShortString<>& pattern, int attrs, SearchRec& rec) {
  rec.p_matches.clear();
  rec.p_index = 0;
  rec.p_attr = 0;
  rec.p_name = ShortString<>("");
  glob_t matches{};
  int rc = ::glob(p_to_std_string(pattern).c_str(), GLOB_NOSORT, nullptr, &matches);
  if (rc != 0) {
    ::globfree(&matches);
    p_doserror = 18;
    return p_doserror;
  }
  for (std::size_t i = 0; i < matches.gl_pathc; ++i) {
    const char* path = matches.gl_pathv[i];
    struct stat st{};
    if (::stat(path, &st) != 0) continue;
    if (attrs == 0x10 && !S_ISDIR(st.st_mode)) continue;
    rec.p_matches.emplace_back(path);
  }
  ::globfree(&matches);
  if (rec.p_matches.empty()) {
    p_doserror = 18;
    return p_doserror;
  }
  p_searchrec_fill(rec, rec.p_matches[0]);
  p_doserror = 0;
  return 0;
}
inline int32_t p_findnext(SearchRec& rec) {
  if (rec.p_index + 1 >= rec.p_matches.size()) {
    p_doserror = 18;
    return p_doserror;
  }
  ++rec.p_index;
  p_searchrec_fill(rec, rec.p_matches[rec.p_index]);
  p_doserror = 0;
  return 0;
}
inline void p_findclose(SearchRec& rec) {
  rec.p_matches.clear();
  rec.p_index = 0;
}
inline void p_mkdir(const ShortString<>& path) {
  p_last_ioresult = ::mkdir(p_to_std_string(path).c_str(), 0777) == 0 ? 0 : 5;
}
inline void p_rmdir(const ShortString<>& path) {
  p_last_ioresult = ::rmdir(p_to_std_string(path).c_str()) == 0 ? 0 : 5;
}
inline void p_chdir(const ShortString<>& path) {
  p_last_ioresult = ::chdir(p_to_std_string(path).c_str()) == 0 ? 0 : 3;
}
template <int N>
inline void p_getdir(int, ShortString<N>& out) {
  char buf[PATH_MAX > 0 ? PATH_MAX : 4096]{};
  if (::getcwd(buf, sizeof(buf)) == nullptr) out = ShortString<N>("");
  else out = ShortString<N>(buf);
}
inline void p_erase(const ShortString<>& path) {
  p_last_ioresult = std::remove(p_to_std_string(path).c_str()) == 0 ? 0 : 2;
}
inline void p_erase(TextFile& f) {      // `erase(f)` after assign(f, name)
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  p_set_ioresult(f, std::remove(buf) == 0 ? 0 : 2);
}
inline void p_rename(const ShortString<>& old_name, const ShortString<>& new_name) {
  p_last_ioresult =
      std::rename(p_to_std_string(old_name).c_str(), p_to_std_string(new_name).c_str()) == 0 ? 0 : 5;
}
inline void p_rename(TextFile& f, const ShortString<>& new_name) {
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  p_set_ioresult(f, std::rename(buf, p_to_std_string(new_name).c_str()) == 0 ? 0 : 5);
}
inline ShortString<> p_fsearch(const ShortString<>& name, const ShortString<>&) { return name; }
inline void p_fsplit(const ShortString<>& input, ShortString<>& dir,
                     ShortString<>& name, ShortString<>& ext) {
  std::string path;
  path.reserve(input.length);
  for (int i = 0; i < input.length; ++i) {
    char c = p_char_to_c(input.data[i]);
    if (c == '\\') c = '/';
    path.push_back(c);
  }

  std::string dir_part;
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    dir_part = path.substr(0, 2);
    path.erase(0, 2);
  }

  size_t last_sep = path.find_last_of('/');
  std::string leaf = path;
  if (last_sep != std::string::npos) {
    dir_part += path.substr(0, last_sep + 1);
    leaf = path.substr(last_sep + 1);
  }

  size_t dot = leaf.find_last_of('.');
  std::string name_part = leaf;
  std::string ext_part;
  if (dot != std::string::npos) {
    name_part = leaf.substr(0, dot);
    ext_part = leaf.substr(dot);
  }

  dir = ShortString<>(dir_part.c_str());
  name = ShortString<>(name_part.c_str());
  ext = ShortString<>(ext_part.c_str());
}
inline ShortString<> p_fexpand(const ShortString<>& s) { return s; }

inline void p_epochtolocal(int32_t, uint16_t&, uint16_t&, uint16_t&,
                           uint16_t&, uint16_t&, uint16_t&) {}

template <typename File>
inline int32_t p_filepos(const File& f) {
  if (!f.f) return 0;
  long pos = std::ftell(f.f);
  return pos < 0 ? 0 : static_cast<int32_t>(pos);
}
template <typename File>
inline int32_t p_filesize(const File& f) {
  if (!f.f) return 0;
  long cur = std::ftell(f.f);
  if (cur < 0) return 0;
  if (std::fseek(f.f, 0, SEEK_END) != 0) return 0;
  long size = std::ftell(f.f);
  std::fseek(f.f, cur, SEEK_SET);
  return size < 0 ? 0 : static_cast<int32_t>(size);
}
template <typename File>
inline void p_seek(File& f, int32_t pos) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  p_set_ioresult(f, std::fseek(f.f, pos, SEEK_SET) == 0 ? 0 : 103);
}
inline void p_truncate(TextFile&) {}
inline void p_flush(const TextFile& f) {
  if (f.f) std::fflush(f.f);
}
// `blockread` / `blockwrite` are stubs and callers in fpc use either
// the 3-arg or 4-arg form depending on whether they care about the
// actually-transferred count. Accept both shapes variadically.
template <typename File, typename Count>
inline void p_blockread(File& f, void* value, int32_t count, Count& transferred) {
  if (!f.f) {
    transferred = static_cast<Count>(0);
    p_set_ioresult(f, 103);
    return;
  }
  transferred = static_cast<Count>(std::fread(value, 1, count, f.f));
  p_set_ioresult(f, std::ferror(f.f) ? 100 : 0);
}
template <typename File, typename T, typename Count>
inline void p_blockread(File& f, T& value, int32_t count, Count& transferred) {
  if (!f.f) {
    transferred = static_cast<Count>(0);
    p_set_ioresult(f, 103);
    return;
  }
  transferred = static_cast<Count>(
      std::fread(static_cast<void*>(std::addressof(value)), 1, count, f.f));
  p_set_ioresult(f, std::ferror(f.f) ? 100 : 0);
}
template <typename File, typename T>
inline void p_blockread(File& f, T& value, int32_t count) {
  int32_t transferred = 0;
  p_blockread(f, value, count, transferred);
}
template <typename File>
inline void p_blockread(File& f, void* value, int32_t count) {
  int32_t transferred = 0;
  p_blockread(f, value, count, transferred);
}
template <typename File>
inline void p_blockwrite(File& f, const void* value, int32_t count) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  std::fwrite(value, 1, count, f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
template <typename File, typename T>
inline void p_blockwrite(File& f, const T& value, int32_t count) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  std::fwrite(static_cast<const void*>(std::addressof(value)), 1, count, f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
// `readln(f, s)` reads a line into `s`. `readln` (no args) reads and
// discards a line. `readln(f)` reads/discards a line from `f`.
inline void p_readln(TextFile& f) {
  if (!f.f) return;
  int c;
  while ((c = std::fgetc(f.f)) != EOF && c != '\n') {}
}
template <int N>
inline void p_readln(TextFile& f, ShortString<N>& s) {
  s.length = 0;
  if (!f.f) return;
  int c;
  while ((c = std::fgetc(f.f)) != EOF && c != '\n') {
    if (s.length < N) { s.data[s.length] = p_char_of(static_cast<char>(c)); ++s.length; }
  }
}
template <typename... A> inline void p_readln(A&&...) {}
template <typename... A> inline void p_read(A&&...) {}
// `settextbuf(f, buf, size)` is a stub -- we don't buffer. Take the buffer
// as fully variadic so callers can pass anything (void*, opaque arrays,
// lvalue derefs of untyped pointers) without type-checking fuss.
template <typename... A>
inline void p_settextbuf(TextFile&, A&&...) {}
template <typename T> inline int32_t p_ioresult_of(T&&) { return 0; }

// PChar utilities (strings unit).
inline int p_strlen(const char* s) { return s ? (int)std::strlen(s) : 0; }
inline int p_strlen(const p_char* s) {
  if (!s) return 0;
  int n = 0;
  while (p_char_byte(s[n]) != 0) ++n;
  return n;
}
inline const char* p_strpas(const char* s) { return s; }
inline const p_char* p_strpas(const p_char* s) { return s; }
template <int N>
inline ShortString<N> p_strpas_s(const char* s) {
  return ShortString<N>(s);
}
template <int N>
inline ShortString<N> p_strpas_s(const p_char* s) {
  return ShortString<N>(s);
}
inline char* p_strpcopy(char* dest, const ShortString<>& src) {
  for (int i = 0; i < src.length; ++i) dest[i] = p_char_to_c(src.data[i]);
  dest[src.length] = 0;
  return dest;
}
inline p_char* p_strpcopy(p_char* dest, const ShortString<>& src) {
  for (int i = 0; i < src.length; ++i) dest[i] = src.data[i];
  dest[src.length] = p_char_of('\0');
  return dest;
}
inline int p_strcomp(const char* a, const char* b) { return std::strcmp(a, b); }
inline int p_strcomp(const p_char* a, const p_char* b) {
  int i = 0;
  while (true) {
    uint8_t av = p_char_byte(a[i]);
    uint8_t bv = p_char_byte(b[i]);
    if (av != bv) return av < bv ? -1 : 1;
    if (av == 0) return 0;
    ++i;
  }
}

// Insert (string manipulation).
template <int N, int M>
inline void p_insert(const ShortString<N>& src, ShortString<M>& dest, int pos) {
  if (pos < 1) pos = 1;
  if (pos > dest.length + 1) pos = dest.length + 1;
  int need = dest.length + src.length;
  if (need > M) need = M;
  int shift = need - dest.length;
  if (shift < 0) shift = 0;
  for (int i = need - 1; i >= pos - 1 + src.length; --i) {
    dest.data[i] = dest.data[i - shift];
  }
  for (int i = 0; i < src.length && pos - 1 + i < M; ++i) {
    dest.data[pos - 1 + i] = src.data[i];
  }
  dest.length = static_cast<uint8_t>(need);
}
// Pascal `insert(c, s, pos)` -- insert a single character.
template <int M>
inline void p_insert(p_char c, ShortString<M>& dest, int pos) {
  char src[2] = {p_char_to_c(c), 0};
  p_insert(src, dest, pos);
}
inline void p_insert(const char* src, ShortString<>& dest, int pos) {
  p_insert(ShortString<>(src), dest, pos);
}
inline void p_insert(const p_char* src, ShortString<>& dest, int pos) {
  p_insert(ShortString<>(src), dest, pos);
}

// --- Memory / bytewise utilities -------------------------------------------

inline void p_fillchar(void* dest, int count, int value) {
  std::memset(dest, value & 0xff, static_cast<size_t>(count));
}
inline void p_fillchar(void* dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
inline void p_fillchar(ShortStringCharRef dest, int count, int value) {
  std::memset(dest.byte, value & 0xff, static_cast<size_t>(count));
}
inline void p_fillchar(ShortStringCharRef dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
template <typename T>
inline void p_fillchar(T& dest, int count, int value) {
  std::memset(&dest, value & 0xff, static_cast<size_t>(count));
}
template <typename T>
inline void p_fillchar(T& dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
inline void p_move(const void* src, void* dest, int count) {
  std::memmove(dest, src, static_cast<size_t>(count));
}
inline void p_move(ShortStringCharRef src, void* dest, int count) {
  std::memmove(dest, src.byte, static_cast<size_t>(count));
}
inline void p_move(ShortStringCharValue src, void* dest, int count) {
  std::memmove(dest, src.byte, static_cast<size_t>(count));
}
inline void p_move(const void* src, ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, src, static_cast<size_t>(count));
}
inline void p_move(ShortStringCharValue src, ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, src.byte, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(ShortStringCharRef src, D& dest, int count) {
  std::memmove(std::addressof(dest), src.byte, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(ShortStringCharValue src, D& dest, int count) {
  std::memmove(std::addressof(dest), src.byte, static_cast<size_t>(count));
}
template <typename S, typename D>
inline void p_move(const S& src, D& dest, int count) {
  std::memmove(&dest, &src, static_cast<size_t>(count));
}
template <typename S>
inline void p_move(const S& src, ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, &src, static_cast<size_t>(count));
}
inline void p_getmem(void*& p, int size) {
  p = std::malloc(static_cast<size_t>(size));
}
inline void p_freemem(void*& p, int = 0) {
  std::free(p);
  p = nullptr;
}
inline void p_reallocmem(void*& p, int size) {
  if (size <= 0) {
    std::free(p);
    p = nullptr;
    return;
  }
  void* q = std::realloc(p, static_cast<size_t>(size));
  if (q) p = q;
}
template <typename P>
inline void p_getmem(P*& p, int size) {
  // Pascal idiom: `getmem(p, length(s)+1)` then `p^ := s` where p is
  // `^string`. In Pascal that packs to length+1 bytes because strings
  // are length-prefixed and stored tightly. In C++ our ShortString<N>
  // is fixed size, so the assignment would overrun a length+1 buffer.
  // Round allocations up to at least `sizeof(P)` so any later
  // store-through via `*p` stays in bounds.
  size_t n = static_cast<size_t>(size);
  if (n < sizeof(P)) n = sizeof(P);
  p = static_cast<P*>(std::malloc(n));
}
template <typename P>
inline void p_freemem(P*& p, int = 0) {
  std::free(p);
  p = nullptr;
}
template <typename P>
inline void p_reallocmem(P*& p, int size) {
  if (size <= 0) {
    std::free(static_cast<void*>(p));
    p = nullptr;
    return;
  }
  void* q = std::realloc(static_cast<void*>(p), static_cast<size_t>(size));
  if (q) p = static_cast<P*>(q);
}

// --- Program control --------------------------------------------------------

[[noreturn]] inline void p_halt() { std::exit(0); }
[[noreturn]] inline void p_halt(int code) { std::exit(code); }
// Pascal `RunError(n)` -- abort with a runtime-error code. Same effect
// as Halt(n) for our purposes.
[[noreturn]] inline void p_runerror(int code = 0) { std::exit(code); }

// POSIX `signal(sig, handler)` -- fpc's catch unit installs signal
// handlers for SIGSEGV/SIGBUS/SIGILL to turn them into RunErrors.
inline p_signalhandler p_signal(int32_t sig, p_signalhandler h) {
  return reinterpret_cast<p_signalhandler>(
      std::signal(sig, reinterpret_cast<void (*)(int)>(h)));
}

// `FindFirst` attribute mask -- any file (every attribute bit set).
inline constexpr int32_t p_faanyfile = 0x3F;

// Pascal's system-level `exitproc` points to a procedure run at program
// exit. We don't implement exit-chaining yet; provide the variable so
// code that saves/restores it compiles.
using ExitProc = void (*)();
inline ExitProc p_exitproc = nullptr;
inline int32_t p_exitcode = 0;

// ParamCount / ParamStr -- argv machinery. `init_argv` is called by
// the emitted `main(argc, argv)` stub, then Pascal code uses the
// builtin accessors.
inline int rt_argc = 0;
inline char** rt_argv = nullptr;
inline void init_argv(int argc, char** argv) { rt_argc = argc; rt_argv = argv; }
inline int p_paramcount() { return rt_argc > 0 ? rt_argc - 1 : 0; }
inline ShortString<> p_paramstr(int i) {
  if (i < 0 || i >= rt_argc || !rt_argv || !rt_argv[i]) return {};
  return ShortString<>(rt_argv[i]);
}

inline int32_t p_ioresult() {
  int32_t result = p_last_ioresult;
  p_last_ioresult = 0;
  return result;
}

// --- String intrinsics ------------------------------------------------------

template <int N>
inline int p_pos(const char* needle, const ShortString<N>& hay) {
  int nl = 0; while (needle[nl]) ++nl;
  for (int i = 0; i + nl <= hay.length; ++i) {
    bool ok = true;
    for (int j = 0; j < nl; ++j) {
      if (hay.data[i + j] != p_char_of(needle[j])) { ok = false; break; }
    }
    if (ok) return i + 1;
  }
  return 0;
}
template <int N>
inline int p_pos(const p_char* needle, const ShortString<N>& hay) {
  int nl = p_strlen(needle);
  for (int i = 0; i + nl <= hay.length; ++i) {
    bool ok = true;
    for (int j = 0; j < nl; ++j) {
      if (hay.data[i + j] != needle[j]) { ok = false; break; }
    }
    if (ok) return i + 1;
  }
  return 0;
}
template <int N, int M>
inline int p_pos(const ShortString<N>& needle, const ShortString<M>& hay) {
  for (int i = 0; i + needle.length <= hay.length; ++i) {
    bool ok = true;
    for (int j = 0; j < needle.length; ++j) {
      if (hay.data[i + j] != needle.data[j]) { ok = false; break; }
    }
    if (ok) return i + 1;
  }
  return 0;
}

// Pascal `pos(c, s)` with a single-char needle. Very common in compiler.
template <int N>
inline int p_pos(p_char c, const ShortString<N>& hay) {
  for (int i = 0; i < hay.length; ++i) {
    if (hay.data[i] == c) return i + 1;
  }
  return 0;
}

// Pascal `val(S, real_var, code_var)` overload.
template <int N>
inline void p_val(const ShortString<N>& s, double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  double v = std::strtod(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const ShortString<N>& s, double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <int N>
inline void p_val(const ShortString<N>& s, long double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  long double v = std::strtold(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const ShortString<N>& s, long double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <int N>
inline void p_val(const ShortString<N>& s, float& out, int32_t& code) {
  double v = 0.0;
  p_val(s, v, code);
  if (code == 0) out = static_cast<float>(v);
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const ShortString<N>& s, float& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}

template <int N>
inline ShortString<> p_copy(const ShortString<N>& s, int start, int count) {
  ShortString<> r{};
  if (start < 1) start = 1;
  int avail = s.length - (start - 1);
  if (avail < 0) avail = 0;
  if (count > avail) count = avail;
  if (count < 0) count = 0;
  r.length = static_cast<uint8_t>(count);
  for (int i = 0; i < count; ++i) r.data[i] = s.data[start - 1 + i];
  return r;
}

template <int N>
inline void p_delete(ShortString<N>& s, int start, int count) {
  if (start < 1 || start > s.length) return;
  int tail = s.length - (start - 1) - count;
  if (tail < 0) { s.length = static_cast<uint8_t>(start - 1); return; }
  for (int i = 0; i < tail; ++i) s.data[start - 1 + i] = s.data[start - 1 + count + i];
  s.length = static_cast<uint8_t>(s.length - count);
}

template <int N>
inline void p_insert(const char* src, ShortString<N>& s, int pos) {
  ShortString<N> in(src);
  (void)pos; (void)in;
  // Minimal stub; flesh out later.
}

inline p_char p_upcase(p_char c) {
  uint8_t b = p_char_byte(c);
  if (b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - 32);
  return p_char_of(b);
}
template <int N>
inline ShortString<N> p_upcase(const ShortString<N>& s) {
  ShortString<N> r = s;
  for (int i = 0; i < r.length; ++i) r.data[i] = p_upcase(r.data[i]);
  return r;
}

// --- Write / Writeln --------------------------------------------------------
// Variadic emit: each call translates to a sequence of one-arg writes.

// Single-value writers to stdout.
template <int N>
inline void p_write_one(const ShortString<N>& s) {
  for (int i = 0; i < s.length; ++i) std::fputc(p_char_to_c(s.data[i]), stdout);
}
inline void p_write_one(const char* s)    { if (s) std::fputs(s, stdout); }
inline void p_write_one(const p_char* s)  { if (s) std::fputs(p_c_str(s), stdout); }
inline void p_write_one(p_char* s)        { p_write_one(const_cast<const p_char*>(s)); }
inline void p_write_one(int32_t v)        { std::fprintf(stdout, "%d", v); }
inline void p_write_one(uint32_t v)       { std::fprintf(stdout, "%u", v); }
inline void p_write_one(int64_t v)        { std::fprintf(stdout, "%lld", (long long)v); }
inline void p_write_one(uint64_t v)       { std::fprintf(stdout, "%llu", (unsigned long long)v); }
inline void p_write_one(double v)         { std::fprintf(stdout, "%g", v); }
inline void p_write_one(long double v)    { std::fprintf(stdout, "%Lg", v); }
inline void p_write_one(p_char c)         { std::fputc(p_char_to_c(c), stdout); }
inline void p_write_one(bool b)           { std::fputs(b ? "TRUE" : "FALSE", stdout); }
inline void p_write_one(const TextFile&)  {}  // first arg of `write(f, ...)`
template <typename T> inline void p_write_one(T* p) {
  std::fprintf(stdout, "%p", (void*)p);
}

template <int N>
inline void p_write_file_one(std::FILE* out, const ShortString<N>& s) {
  if (!out) return;
  for (int i = 0; i < s.length; ++i) std::fputc(p_char_to_c(s.data[i]), out);
}
inline void p_write_file_one(std::FILE* out, const char* s) {
  if (out && s) std::fputs(s, out);
}
inline void p_write_file_one(std::FILE* out, const p_char* s) {
  if (out && s) std::fputs(p_c_str(s), out);
}
inline void p_write_file_one(std::FILE* out, p_char* s) {
  p_write_file_one(out, const_cast<const p_char*>(s));
}
inline void p_write_file_one(std::FILE* out, int32_t v) {
  if (out) std::fprintf(out, "%d", v);
}
inline void p_write_file_one(std::FILE* out, uint32_t v) {
  if (out) std::fprintf(out, "%u", v);
}
inline void p_write_file_one(std::FILE* out, int64_t v) {
  if (out) std::fprintf(out, "%lld", (long long)v);
}
inline void p_write_file_one(std::FILE* out, uint64_t v) {
  if (out) std::fprintf(out, "%llu", (unsigned long long)v);
}
inline void p_write_file_one(std::FILE* out, double v) {
  if (out) std::fprintf(out, "%g", v);
}
inline void p_write_file_one(std::FILE* out, long double v) {
  if (out) std::fprintf(out, "%Lg", v);
}
inline void p_write_file_one(std::FILE* out, p_char c) {
  if (out) std::fputc(p_char_to_c(c), out);
}
inline void p_write_file_one(std::FILE* out, bool b) {
  if (out) std::fputs(b ? "TRUE" : "FALSE", out);
}
inline void p_write_file_one(std::FILE*, const TextFile&) {}
template <typename T>
inline void p_write_file_one(std::FILE* out, T* p) {
  if (out) std::fprintf(out, "%p", (void*)p);
}

// Variadic write / writeln -- Pascal `write(a, b, c)` and
// `writeln(f, a, b, c)`. Every arg is emitted via p_write_one.
template <typename... Args>
inline void p_write(Args&&... args) {
  (p_write_one(std::forward<Args>(args)), ...);
}
template <typename... Args>
inline void p_writeln(Args&&... args) {
  (p_write_one(std::forward<Args>(args)), ...);
  std::fputc('\n', stdout);
}
template <typename... Args>
inline void p_write(TextFile& f, Args&&... args) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  (p_write_file_one(f.f, std::forward<Args>(args)), ...);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
inline void p_write(TextFile& f) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
template <typename... Args>
inline void p_writeln(TextFile& f, Args&&... args) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  (p_write_file_one(f.f, std::forward<Args>(args)), ...);
  std::fputc('\n', f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
inline void p_writeln(TextFile& f) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  std::fputc('\n', f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}

// --- File-IO placeholders ---------------------------------------------------
// Real behaviour is added as units are translated that need them.
inline void p_assign(TextFile& f, const ShortString<>& n) {
  f.name = n;
  f.f = nullptr;
  p_set_ioresult(f, 0);
}
inline void p_reset(TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "rb");
  p_set_ioresult(f, f.f ? 0 : 2);  // 2 = file-not-found per fpc convention
}
inline void p_reset(TextFile& f, int32_t) { p_reset(f); }  // rec size form
inline void p_rewrite(TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "wb");
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_rewrite(TextFile& f, int32_t) { p_rewrite(f); }
inline void p_append(TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "ab");
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_close(TextFile& f) {
  if (f.f) { std::fclose(f.f); f.f = nullptr; }
  p_set_ioresult(f, 0);
}
inline bool p_eof(TextFile& f) {
  if (!f.f) return true;
  // Peek one char to update feof state after consuming previous
  // bytes -- glibc sets EOF flag lazily otherwise.
  int c = std::fgetc(f.f);
  if (c == EOF) return true;
  std::ungetc(c, f.f);
  return false;
}
// typed-file variants
template <typename T>
inline void p_assign(TypedFile<T>& f, const ShortString<>& n) {
  f.name = n;
  f.f = nullptr;
  p_set_ioresult(f, 0);
}
template <typename T>
inline void p_reset(TypedFile<T>& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "rb");
  p_set_ioresult(f, f.f ? 0 : 2);
}
template <typename T> inline void p_reset(TypedFile<T>& f, int32_t) { p_reset(f); }
template <typename T>
inline void p_rewrite(TypedFile<T>& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "wb");
  p_set_ioresult(f, f.f ? 0 : 5);
}
template <typename T> inline void p_rewrite(TypedFile<T>& f, int32_t) { p_rewrite(f); }
template <typename T>
inline void p_append(TypedFile<T>& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "ab");
  p_set_ioresult(f, f.f ? 0 : 5);
}
template <typename T>
inline void p_close(TypedFile<T>& f) {
  if (f.f) { std::fclose(f.f); f.f = nullptr; }
  p_set_ioresult(f, 0);
}
template <typename T>
inline bool p_eof(const TypedFile<T>& f) {
  if (!f.f) return true;
  int c = std::fgetc(f.f);
  if (c == EOF) return true;
  std::ungetc(c, f.f);
  return false;
}

// --- Val / Str --------------------------------------------------------------

template <int N>
inline void p_val(const ShortString<N>& s, int32_t& out, int32_t& code) {
  p_parse_pascal_integer(p_to_std_string(s), out, code);
}
template <int N>
inline void p_str(int32_t v, ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(uint32_t v, ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u", v);
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(int64_t v, ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(uint64_t v, ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(float v, ShortString<N>& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .9g", static_cast<double>(v));
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(double v, ShortString<N>& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .17g", v);
  out = ShortString<N>(buf);
}
template <int N>
inline void p_str(long double v, ShortString<N>& out) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "% .21Lg", v);
  out = ShortString<N>(buf);
}

// --- high / low intrinsics --------------------------------------------------
//
// Pascal `High(x)` and `Low(x)` return the highest/lowest index of an array
// or the max/min value of an ordinal type. We only need a small subset here.

template <typename T, std::size_t N>
constexpr std::size_t high(const T (&)[N]) { return N - 1; }
template <typename T, std::size_t N>
constexpr std::size_t low(const T (&)[N])  { return 0; }
template <typename T, std::size_t N>
constexpr std::size_t high(const std::array<T, N>&) { return N - 1; }
template <typename T, std::size_t N>
constexpr std::size_t low(const std::array<T, N>&)  { return 0; }

// Pascal numeric constants.
inline constexpr int32_t p_maxlongint = 2147483647;
inline constexpr double  p_pi         = 3.141592653589793;

// Pascal `typeof(T)` returns a pointer to T's virtual method table. We
// don't model VMT values at emit time, so stub it as nullptr. Sites
// that use it (stream registration, runtime class-id lookup) only need
// the value to be comparable to other nullptrs, which holds.
template <typename T> inline void* p_typeof(const T&) { return nullptr; }
inline void* p_typeof(...) { return nullptr; }

// Pascal `ofs(x)` / `seg(x)` return the 16-bit offset/segment of a
// far pointer. On flat-model targets both are stubbed to 0.
template <typename... A> inline int32_t p_ofs(A&&...) { return 0; }
template <typename... A> inline int32_t p_seg(A&&...) { return 0; }

// Stubs for DOS/system symbols referenced by the compiler. They are
// invoked in defensive paths (error reporting, cross-check prints,
// trace hooks) that don't need to do real work for bootstrap purposes.
inline int32_t p_filemode = 0;
// STUB: `erroraddr` is the RT's last-error address -- a mutable
// global pointer the runtime sets when a runtime error occurs; fpc's
// sources check it and clear it via assignment (`erroraddr := nil;`).
inline void* p_erroraddr = nullptr;
inline void p_swapvectors() {}
inline p_char* p_strnew(const char* s) {
  if (!s) return nullptr;
  std::size_t n = std::strlen(s);
  auto* out = static_cast<p_char*>(std::malloc((n + 1) * sizeof(p_char)));
  if (!out) return nullptr;
  for (std::size_t i = 0; i < n; ++i) out[i] = p_char_of(s[i]);
  out[n] = p_char_of('\0');
  return out;
}
inline p_char* p_strnew(const p_char* s) {
  if (!s) return nullptr;
  std::size_t n = 0;
  while (p_char_byte(s[n]) != 0) ++n;
  auto* out = static_cast<p_char*>(std::malloc((n + 1) * sizeof(p_char)));
  if (!out) return nullptr;
  for (std::size_t i = 0; i <= n; ++i) out[i] = s[i];
  return out;
}
inline void p_strdispose(p_char*& p) {
  if (!p) return;
  std::free(static_cast<void*>(p));
  p = nullptr;
}
template <typename... A> inline int32_t p_execmd(A&&...) { return 0; }
template <typename... A> inline void p_gettime(A&&...) {}
inline bool p_chmod(const ShortString<>& path, int32_t newmode) {
  return ::chmod(p_to_std_string(path).c_str(),
                 static_cast<mode_t>(newmode)) == 0;
}
template <typename... A> inline void p_getdate(A&&...) {}
template <typename... A> inline void p_settime(A&&...) {}
template <typename... A> inline void p_setdate(A&&...) {}
// STUB: `datetime` is a record type in fpc's dos unit. Accept common
// field accesses (year, month, day, hour, min, sec) since owar.pas
// names them unprefixed.
struct DateTime { uint16_t p_year = 0, p_month = 0, p_day = 0,
                  p_hour = 0, p_min = 0, p_sec = 0, p_sec100 = 0; };
using p_datetime = DateTime;
inline int32_t p_extraoptions = 0;
inline int32_t p_moduleindex = 0;
template <int N, int M>
inline void p_exec(const ShortString<N>& command, const ShortString<M>& para) {
  std::vector<std::string> args;
  args.push_back(p_to_std_string(command));
  auto rest = p_split_commandline(p_to_std_string(para));
  args.insert(args.end(), rest.begin(), rest.end());
  p_spawn_process(args);
}
// Pascal `include(set, elem)` / `exclude(set, elem)` add/remove a
// single element. Not stubs -- these are real Pascal set builtins.
template <typename E1, typename E2>
inline void p_include(Set<E1>& s, E2 v) { s.add(static_cast<E1>(v)); }
template <typename E1, typename E2>
inline void p_exclude(Set<E1>& s, E2 v) {
  int i = Set<E1>::idx(static_cast<E1>(v));
  if (i >= 0 && i < 8 * Set<E1>::Nb) {
    s.bits[i >> 3] &= static_cast<unsigned char>(~(1u << (i & 7)));
  }
}

// STUB: misc small dos/system builtins used by the compiler sources.
inline int32_t p_winstackpagesize = 4096;
inline void p_popen(TextFile& f, const ShortString<>& cmd, p_char mode) {
  char m[2] = {static_cast<char>(std::tolower(p_char_to_c(mode))), '\0'};
  f.f = ::popen(p_to_std_string(cmd).c_str(), m);
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_popen(TextFile& f, const ShortString<>& cmd, char mode) {
  p_popen(f, cmd, p_char_of(mode));
}
template <typename F>
inline void p_popen(F&, const ShortString<>&, char) {}
template <typename F>
inline void p_popen(F&, const ShortString<>&, p_char) {}
inline void p_pclose(TextFile& f) {
  int rc = 0;
  if (f.f) {
    rc = ::pclose(f.f);
    f.f = nullptr;
  }
  p_set_ioresult(f, rc == -1 ? errno : 0);
}
template <typename F>
inline void p_pclose(F&) {}
// STUB: file timestamp get/set used by assembler/link bookkeeping.
template <typename F, typename T> inline void p_getftime(F&&, T&) {}
template <typename F, typename T> inline void p_setftime(F&&, T) {}
// STUB: stderr is the fpc standard error TextFile.
inline TextFile p_stderr;
// (No global `tprocdefcoll` -- it's a function-local record type in
// tccal.pas and gets emitted there. An earlier stub here was taking
// name precedence over the real thing via `using namespace ::rt`.)

// STUB: target-platform import/export/linker types from the skipped
// t_win32.pas / t_os2.pas / t_go32v*.pas back-ends. The call sites
// that reference them are inside `case target_info.target of` arms
// guarded for non-linux targets and therefore unreachable at
// bootstrap runtime. We alias them to a tag struct with an implicit
// conversion to any pointer type so `importlib := new(pimportlibwin32,
// Init)` assignments into base-class pointer variables type-check.
struct StubTargetLib {
  void p_init() {}
  // Explicit conversion of the tag struct's address to any pointer
  // type. We only need the operator defined on the pointer; a
  // non-template overload isn't viable because the operand side of
  // `T* = StubTargetLib*` is already a pointer type. Instead we
  // rely on the call-site emitter wrapping `new(T,Init)` in a
  // reinterpret_cast to the receiver type -- which it does when the
  // target alias resolves here (see emit.cc `new(T,Init)` lowering).
};
using p_timportlibwin32 = StubTargetLib;
using p_pimportlibwin32 = StubTargetLib*;
using p_timportlibos2 = StubTargetLib;
using p_pimportlibos2 = StubTargetLib*;
using p_timportlibgo32v2 = StubTargetLib;
using p_pimportlibgo32v2 = StubTargetLib*;
using p_texportlibwin32 = StubTargetLib;
using p_pexportlibwin32 = StubTargetLib*;
using p_texportlibos2 = StubTargetLib;
using p_pexportlibos2 = StubTargetLib*;
using p_texportlibgo32v2 = StubTargetLib;
using p_pexportlibgo32v2 = StubTargetLib*;
using p_tlinkerwin32 = StubTargetLib;
using p_plinkerwin32 = StubTargetLib*;
using p_tlinkeros2 = StubTargetLib;
using p_plinkeros2 = StubTargetLib*;
using p_tlinkergo32v1 = StubTargetLib;
using p_plinkergo32v1 = StubTargetLib*;
using p_tlinkergo32v2 = StubTargetLib;
using p_plinkergo32v2 = StubTargetLib*;
// DOS file-attribute flags (from fpc's dos unit). Used in
// FindFirst(mask, attrs, rec) calls for file enumeration.
inline constexpr int32_t p_readonly  = 0x01;
inline constexpr int32_t p_hidden    = 0x02;
inline constexpr int32_t p_sysfile   = 0x04;
inline constexpr int32_t p_volumeid  = 0x08;
inline constexpr int32_t p_directory = 0x10;
inline constexpr int32_t p_archive   = 0x20;
inline constexpr int32_t p_anyfile   = 0x3F;
// STUB: linux unit's `stat` record returned by FStat. Real fpc reads
// the matching POSIX stat fields; we expose the subset the compiler
// touches (mtime for timestamp comparisons).
struct LinuxStat { int32_t p_mtime = 0; int32_t p_mode = 0; int32_t p_size = 0; };
using p_stat = LinuxStat;
template <int N>
inline bool p_fstat(const ShortString<N>& path, p_stat& info) {
  struct stat st{};
  if (::stat(p_to_std_string(path).c_str(), &st) != 0) return false;
  info.p_mtime = static_cast<int32_t>(st.st_mtime);
  info.p_mode = static_cast<int32_t>(st.st_mode);
  info.p_size = static_cast<int32_t>(st.st_size > INT32_MAX ? INT32_MAX : st.st_size);
  return true;
}
template <typename File>
inline bool p_fstat(const File& f, p_stat& info) {
  if (!f.f) return false;
  struct stat st{};
  if (::fstat(::fileno(f.f), &st) != 0) return false;
  info.p_mtime = static_cast<int32_t>(st.st_mtime);
  info.p_mode = static_cast<int32_t>(st.st_mode);
  info.p_size = static_cast<int32_t>(st.st_size > INT32_MAX ? INT32_MAX : st.st_size);
  return true;
}

// Return value of `getenv`. fpc's `dos.getenv` returns ShortString,
// `linux.getenv` returns pchar -- same lowered name, different
// types. The proxy converts to both, so one `rt::p_getenv` serves
// both `Dos.Getenv` and `Linux.Getenv` call sites.
struct GetEnvResult {
  const char* raw;  // null-terminated env value, or nullptr if unset
  operator const p_char*() const { return raw ? p_from_c_str_copy(raw) : nullptr; }
  operator p_char*() const { return raw ? p_from_c_str_copy(raw) : nullptr; }
  operator ShortString<>() const {
    return raw ? ShortString<>(raw) : ShortString<>("");
  }
};
template <int N>
inline auto operator+(const ShortString<N>& a, const GetEnvResult& b) {
  return a + static_cast<ShortString<>>(b);
}
template <int N>
inline auto operator+(const GetEnvResult& a, const ShortString<N>& b) {
  return static_cast<ShortString<>>(a) + b;
}
inline GetEnvResult p_getenv(const ShortString<>& name) {
  char buf[260]{};
  int n = name.length < 255 ? name.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = p_char_to_c(name.data[i]);
  return {std::getenv(buf)};
}
// Pascal `Linux.Shell(cmd)` -- run a command via `/bin/sh -c`, i.e.
// POSIX `system(3)`. Used by the compiler for wildcard expansion.
template <int N>
inline int32_t p_shell(const ShortString<N>& cmd) {
  // Resolve `sh` via PATH so this keeps working in build chroots that
  // intentionally do not provide a `/bin/sh` path.
  p_spawn_process({"sh", "-c", p_to_std_string(cmd)});
  return p_last_dosexitcode;
}
inline int32_t p_dosexitcode() { return p_last_dosexitcode; }
// Used by fpc 1.0.6 as an existence check for the drive.
inline int32_t p_disksize(uint8_t drive_number) {
  struct statvfs st{};
  if (::statvfs(".", &st) != 0) return -1;
  unsigned long long total =
      static_cast<unsigned long long>(st.f_blocks) * st.f_frsize;
  if (total > static_cast<unsigned long long>(
                  std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
  }
  return static_cast<int32_t>(total);
}

// `System.heapsize` appears as a plain value in the compiler's
// status prints. Real fpc sets this during startup; we expose a
// constant close-enough placeholder.
inline int32_t p_heapsize = 1 << 20;

// Pascal `val(s, n, code)` -- string-to-number parser. The emitter
// routes `System.Val(...)` to `::rt::p_val(...)`.
template <typename S, typename N, typename Code,
          typename = std::enable_if_t<std::is_integral_v<N> &&
                                      !std::is_same_v<N, bool> &&
                                      std::is_integral_v<Code> &&
                                      !std::is_same_v<Code, bool>>>
inline void p_val(const S& s, N& n, Code& code) {
  int32_t parsed_code = 0;
  p_parse_pascal_integer(p_to_std_string(s), n, parsed_code);
  code = static_cast<Code>(parsed_code);
}

}  // namespace rt
