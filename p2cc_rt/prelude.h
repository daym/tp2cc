#pragma once

// Minimal runtime for p2cc-emitted code.
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>

namespace rt {

// --- ShortString<N> --------------------------------------------------------
//
// Pascal-compatible short-string layout: 1 length byte followed by N content
// bytes. Fixed size. Default capacity is 255 (classic `string`).

template <int N = 255>
struct ShortString {
  static_assert(N >= 1 && N <= 255, "ShortString capacity must be 1..255");

  uint8_t length = 0;
  char data[N] = {};

  constexpr ShortString() = default;

  constexpr ShortString(const char* s) {
    int n = 0;
    if (s) {
      while (s[n] && n < N) ++n;
    }
    length = static_cast<uint8_t>(n);
    for (int i = 0; i < n; ++i) data[i] = s[i];
  }

  constexpr ShortString(char c) : length(1) { data[0] = c; }


  // Copy from a ShortString of any capacity (Pascal assigns freely
  // between different `string[N]` sizes; target capacity truncates).
  template <int M>
  constexpr ShortString(const ShortString<M>& o) {
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
    ShortString<R> out;
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
  friend constexpr ShortString operator+(const ShortString& a, char c) {
    ShortString r = a;
    if (r.length < N) { r.data[r.length] = c; ++r.length; }
    return r;
  }
  friend constexpr ShortString operator+(char c, const ShortString& b) {
    ShortString r;
    r.data[0] = c;
    r.length = 1;
    return r + b;
  }
  friend constexpr ShortString operator+(const ShortString& a, uint8_t c) {
    return a + static_cast<char>(c);
  }
  friend constexpr ShortString operator+(uint8_t c, const ShortString& b) {
    return static_cast<char>(c) + b;
  }

  // Pascal `s[i]` is 1-based. We model the access: index 0 gives the
  // length byte (as in TP memory layout), 1..length give the characters.
  constexpr uint8_t& operator[](int i) {
    return i == 0 ? *reinterpret_cast<uint8_t*>(&length)
                  : reinterpret_cast<uint8_t&>(data[i - 1]);
  }
  constexpr const uint8_t& operator[](int i) const {
    return i == 0 ? *reinterpret_cast<const uint8_t*>(&length)
                  : reinterpret_cast<const uint8_t&>(data[i - 1]);
  }
};

// Pascal `const X = 'c';` declares a constant that is BOTH a char
// (assignable into `s[i] : char` contexts) and a 1-element string
// (usable in string concatenations). C++ can't have one type that
// plays both roles, so the emitter wraps such consts in this tag
// struct with implicit conversions in both directions. Scoped to
// the const decl -- ordinary ShortString variables are unaffected.
struct CharConst {
  uint8_t c;
  // `explicit` so `uint8_t -> CharConst` is not viable silently; the
  // only way a CharConst is constructed is from an explicit `'c'` in
  // the emitted const decl.
  explicit constexpr CharConst(char x) : c(static_cast<uint8_t>(x)) {}
  // Conversions: `uint8_t` for Pascal `s[i] := X` (char contexts),
  // `char` so CharConst can sit in an initializer_list<char> next to
  // regular char literals without template-deduction conflicts, and
  // `ShortString<N>` for string-concatenation contexts.
  constexpr operator uint8_t() const { return c; }
  template <int N = 255>
  constexpr operator ShortString<N>() const {
    return ShortString<N>(static_cast<char>(c));
  }
  friend constexpr bool operator==(CharConst a, CharConst b) { return a.c == b.c; }
  friend constexpr bool operator!=(CharConst a, CharConst b) { return a.c != b.c; }
};

// `+` on a CharConst chooses string concatenation: the only reason
// a Pascal 1-char const appears under `+` is to build a ShortString.
// Without these explicit overloads the compiler sees two conversion
// paths (CharConst -> uint8_t and CharConst -> ShortString) and
// marks `+` ambiguous.
inline constexpr ShortString<> operator+(CharConst a, CharConst b) {
  ShortString<> r;
  r.data[0] = a.c; r.data[1] = b.c; r.length = 2;
  return r;
}
inline constexpr ShortString<> operator+(CharConst a, char b) {
  return ShortString<>(static_cast<char>(a.c)) + b;
}
inline constexpr ShortString<> operator+(char a, CharConst b) {
  return a + ShortString<>(static_cast<char>(b.c));
}
template <int N>
inline constexpr auto operator+(CharConst a, const ShortString<N>& b) {
  return ShortString<>(static_cast<char>(a.c)) + b;
}
template <int N>
inline constexpr auto operator+(const ShortString<N>& a, CharConst b) {
  return a + ShortString<>(static_cast<char>(b.c));
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
template <typename T, int Lo, int N>
struct Array {
  T data[N]{};

  // User-provided (not =default) so the class is NOT an aggregate. This
  // matters because `Array<R, Lo, N> a = {{.f=1}, ...}` must dispatch
  // to the initializer_list<T> ctor and aggregate-init each element as
  // a T -- not try to aggregate-init the Array (which would route the
  // designator through `data[N]` and hit GNU-array-designator errors).
  constexpr Array() {}

  // `Array<T, Lo, N> a = {e0, e1, ...};` -- each element converts to T,
  // so `{.field=v}` designated aggregate inits of element records work.
  constexpr Array(std::initializer_list<T> il) {
    int i = 0;
    for (auto& v : il) {
      if (i < N) data[i++] = v;
    }
  }

  // Convert from a ShortString. Matches Pascal's `array[1..N] of char =
  // 'some string'` idiom: characters fill the first part of the array,
  // remaining bytes stay zero. Typed for byte-sized T in practice.
  template <int M>
  constexpr Array(const ShortString<M>& s) {
    int n = s.length < N ? s.length : N;
    for (int i = 0; i < n; ++i) data[i] = static_cast<T>(s.data[i]);
  }

  // Pascal single-char string literal as init for `array of char`. The
  // emitter turns a one-character Pascal string into a plain `char`, so
  // we need a ctor that accepts it and places the byte at position 0.
  constexpr Array(char c) {
    if (N > 0) data[0] = static_cast<T>(c);
  }

  template <typename Ix>
  constexpr T& operator[](Ix i) {
    return data[static_cast<int>(i) - Lo];
  }
  template <typename Ix>
  constexpr const T& operator[](Ix i) const {
    return data[static_cast<int>(i) - Lo];
  }

  constexpr T* begin()             { return data; }
  constexpr T* end()               { return data + N; }
  constexpr const T* begin() const { return data; }
  constexpr const T* end()   const { return data + N; }

  // Decay to pointer for `char*` / `uint8_t*` Pascal idioms like `@buf`
  // assigned to a pchar. Only active for byte-sized T.
  template <typename U = T,
            typename = std::enable_if_t<sizeof(U) == 1>>
  constexpr operator const char*() const {
    return reinterpret_cast<const char*>(data);
  }
  template <typename U = T,
            typename = std::enable_if_t<sizeof(U) == 1>>
  constexpr operator char*() {
    return reinterpret_cast<char*>(data);
  }

  static constexpr int low()  { return Lo; }
  static constexpr int high() { return Lo + N - 1; }
};

// --- Set<Elem> --------------------------------------------------------------
//
// A very small set type parameterised by element type. For enum-backed sets
// we use a uint64_t bitmask assuming enum fits in 64 values (enough for all
// uses in the compiler proper). For byte-backed sets (`set of byte`,
// `set of char`) we use a 256-bit std::array<uint64_t, 4>.

template <typename Elem>
struct Set {
  // 256-bit bitmask (enough for `set of byte`, `set of char`, and most
  // enum-backed Pascal sets). Emitted code may cast enum values to
  // integers that exceed 63 so a wider mask is essential.
  static constexpr int W = 4;
  uint64_t bits[W] = {0, 0, 0, 0};

  constexpr Set() = default;

  // Cross-element conversion: same bit-layout, different `Elem`
  // type. Pascal treats `set of char`, `set of byte`, `set of
  // 0..255` etc. as interchangeable at the value level; C++ templates
  // instantiate these as distinct types, so we provide an implicit
  // conversion constructor that just copies the bitmask.
  template <typename Other>
  constexpr Set(const Set<Other>& o) {
    for (int i = 0; i < W; ++i) bits[i] = o.bits[i];
  }

  // Adopt the byte-layout of a 32-byte `array[0..31] of byte` as the
  // bitmask. Pascal's in-memory `set` layout matches this for
  // `set of 0..255` / `set of byte`, so an explicit cast
  // `byteset(arr)` is just a reinterpretation.
  template <int Lo, int N,
            typename = std::enable_if_t<(N * sizeof(uint8_t) <= sizeof(bits))>>
  constexpr Set(const Array<uint8_t, Lo, N>& a) {
    const int bytes = (N < static_cast<int>(sizeof(bits)))
                          ? N
                          : static_cast<int>(sizeof(bits));
    auto* dst = reinterpret_cast<uint8_t*>(bits);
    for (int i = 0; i < bytes; ++i) dst[i] = a.data[i];
  }

  static constexpr int idx(Elem e) {
    return static_cast<int>(static_cast<int64_t>(e));
  }

  static Set from_list(std::initializer_list<Elem> xs) {
    Set s;
    for (auto x : xs) s.add(x);
    return s;
  }
  constexpr void add(Elem e) {
    int i = idx(e);
    if (i >= 0 && i < 64 * W) bits[i >> 6] |= (uint64_t{1} << (i & 63));
  }
  constexpr bool contains(Elem e) const {
    int i = idx(e);
    if (i < 0 || i >= 64 * W) return false;
    return (bits[i >> 6] & (uint64_t{1} << (i & 63))) != 0;
  }
  friend constexpr Set operator+(Set a, Set b) {
    Set r; for (int i = 0; i < W; ++i) r.bits[i] = a.bits[i] | b.bits[i]; return r;
  }
  friend constexpr Set operator-(Set a, Set b) {
    Set r; for (int i = 0; i < W; ++i) r.bits[i] = a.bits[i] & ~b.bits[i]; return r;
  }
  friend constexpr Set operator*(Set a, Set b) {
    Set r; for (int i = 0; i < W; ++i) r.bits[i] = a.bits[i] & b.bits[i]; return r;
  }
  friend constexpr bool operator==(Set a, Set b) {
    for (int i = 0; i < W; ++i) if (a.bits[i] != b.bits[i]) return false;
    return true;
  }
  friend constexpr bool operator!=(Set a, Set b) { return !(a == b); }
  friend constexpr bool operator<=(Set a, Set b) {
    // subset test
    for (int i = 0; i < W; ++i) if ((a.bits[i] & ~b.bits[i]) != 0) return false;
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
template <> struct set_elem_type<char> { using type = uint8_t; };
// CharConst is our wrapper for Pascal `const X = 'c'` -- treat as
// uint8_t for set-of-char purposes so `contains(p_c)` with a
// uint8_t byte matches without needing a CharConst conversion.
template <> struct set_elem_type<CharConst> { using type = uint8_t; };
}
template <typename T, typename... Rest>
inline auto set_of(T first, Rest... rest) {
  using E = typename detail::set_elem_type<T>::type;
  Set<E> s;
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
  Set<Elem> s;
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
};

// Pascal `file of T` - minimal stub; behaviour added as needed.
template <typename T>
struct TypedFile {
  std::FILE* f = nullptr;
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

template <typename T> inline int32_t p_ord(T x) { return static_cast<int32_t>(x); }
inline uint8_t p_chr(int x) { return static_cast<uint8_t>(x); }

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

// No rvalue `p_inc`/`p_dec` overloads here: the `inc(T(lv))` /
// `dec(T(lv))` idiom in the fpc sources is translated at emit time
// into `lv = (decltype(lv))((T)lv +/- step)`, so callers only ever
// reach `p_inc` / `p_dec` with a true lvalue argument.

// --- Missing small RTL procedures ------------------------------------------

inline int32_t p_memavail() { return 1 << 30; }   // stub: "lots of memory"
inline int32_t p_heapavail() { return 1 << 30; }
inline int32_t p_maxavail()  { return 1 << 30; }

inline int32_t p_strtoint(const ShortString<>& s) {
  char buf[260]{};
  for (int i = 0; i < s.length; ++i) buf[i] = s.data[i];
  return std::atoi(buf);
}

// Dos/file procedures -- stubbed; real behaviour added as needed.
struct SearchRec { int32_t p_time = 0; int32_t p_size = 0;
                   uint8_t p_attr = 0; ShortString<> p_name; };
using p_searchrec = SearchRec;
using p_tsearchrec = SearchRec;
inline void p_findfirst(const ShortString<>&, int, SearchRec&) {}
inline void p_findnext(SearchRec&) {}
inline void p_findclose(SearchRec&) {}
inline void p_mkdir(const ShortString<>&) {}
inline void p_rmdir(const ShortString<>&) {}
inline void p_chdir(const ShortString<>&) {}
inline void p_getdir(int, ShortString<>& out) { out = ShortString<>("/"); }
inline void p_erase(const ShortString<>&) {}
inline void p_erase(TextFile&) {}      // `erase(f)` after assign(f, name)
inline void p_rename(const ShortString<>&, const ShortString<>&) {}
inline void p_rename(TextFile&, const ShortString<>&) {}
inline ShortString<> p_fsearch(const ShortString<>& name, const ShortString<>&) { return name; }
inline void p_fsplit(const ShortString<>&, ShortString<>&, ShortString<>&, ShortString<>&) {}
inline ShortString<> p_fexpand(const ShortString<>& s) { return s; }

inline void p_epochtolocal(int32_t, uint16_t&, uint16_t&, uint16_t&,
                           uint16_t&, uint16_t&, uint16_t&) {}

inline int32_t p_filepos(const TextFile&) { return 0; }
inline int32_t p_filesize(const TextFile&) { return 0; }
inline void p_seek(TextFile&, int32_t) {}
inline void p_truncate(TextFile&) {}
inline void p_flush(const TextFile&) {}
// `blockread` / `blockwrite` are stubs and callers in fpc use either
// the 3-arg or 4-arg form depending on whether they care about the
// actually-transferred count. Accept both shapes variadically.
template <typename... A> inline void p_blockread(TextFile&, A&&...) {}
template <typename... A> inline void p_blockwrite(TextFile&, A&&...) {}
template <typename... A> inline void p_readln(A&&...) {}
template <typename... A> inline void p_read(A&&...) {}
// `settextbuf(f, buf, size)` is a stub -- we don't buffer. Take the buffer
// as fully variadic so callers can pass anything (void*, opaque arrays,
// lvalue derefs of untyped pointers) without type-checking fuss.
template <typename... A>
inline void p_settextbuf(TextFile&, A&&...) {}
template <typename T> inline int32_t p_ioresult_of(T&&) { return 0; }
inline bool p_eoln(const TextFile&) { return false; }

// PChar utilities (strings unit).
inline int p_strlen(const char* s) { return s ? (int)std::strlen(s) : 0; }
inline const char* p_strpas(const char* s) { return s; }
template <int N>
inline ShortString<N> p_strpas_s(const char* s) {
  return ShortString<N>(s);
}
inline char* p_strpcopy(char* dest, const ShortString<>& src) {
  for (int i = 0; i < src.length; ++i) dest[i] = src.data[i];
  dest[src.length] = 0;
  return dest;
}
inline int p_strcomp(const char* a, const char* b) { return std::strcmp(a, b); }

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
inline void p_insert(char c, ShortString<M>& dest, int pos) {
  char src[2] = {c, 0};
  p_insert(src, dest, pos);
}
template <int M>
inline void p_insert(uint8_t c, ShortString<M>& dest, int pos) {
  p_insert(static_cast<char>(c), dest, pos);
}
inline void p_insert(const char* src, ShortString<>& dest, int pos) {
  p_insert(ShortString<>(src), dest, pos);
}

// --- Memory / bytewise utilities -------------------------------------------

inline void p_fillchar(void* dest, int count, int value) {
  std::memset(dest, value & 0xff, static_cast<size_t>(count));
}
template <typename T>
inline void p_fillchar(T& dest, int count, int value) {
  std::memset(&dest, value & 0xff, static_cast<size_t>(count));
}
inline void p_move(const void* src, void* dest, int count) {
  std::memmove(dest, src, static_cast<size_t>(count));
}
template <typename S, typename D>
inline void p_move(const S& src, D& dest, int count) {
  std::memmove(&dest, &src, static_cast<size_t>(count));
}
inline void p_getmem(void*& p, int size) {
  p = std::malloc(static_cast<size_t>(size));
}
inline void p_freemem(void*& p, int = 0) {
  std::free(p);
  p = nullptr;
}
template <typename P>
inline void p_getmem(P*& p, int size) {
  p = static_cast<P*>(std::malloc(static_cast<size_t>(size)));
}
template <typename P>
inline void p_freemem(P*& p, int = 0) {
  std::free(p);
  p = nullptr;
}

// --- Program control --------------------------------------------------------

[[noreturn]] inline void p_halt() { std::exit(0); }
[[noreturn]] inline void p_halt(int code) { std::exit(code); }

// Pascal's system-level `exitproc` points to a procedure run at program
// exit. We don't implement exit-chaining yet; provide the variable so
// code that saves/restores it compiles.
using ExitProc = void (*)();
inline ExitProc p_exitproc = nullptr;
inline int32_t p_exitcode = 0;

inline int p_paramcount() { return 0; }    // set by main()
inline ShortString<> p_paramstr(int) { return {}; }  // set by main()

inline int32_t p_ioresult() { return 0; }

// --- String intrinsics ------------------------------------------------------

template <int N>
inline int p_pos(const char* needle, const ShortString<N>& hay) {
  int nl = 0; while (needle[nl]) ++nl;
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
inline int p_pos(char c, const ShortString<N>& hay) {
  for (int i = 0; i < hay.length; ++i) {
    if (hay.data[i] == c) return i + 1;
  }
  return 0;
}
template <int N>
inline int p_pos(uint8_t c, const ShortString<N>& hay) {
  return p_pos(static_cast<char>(c), hay);
}

// Pascal `val(S, real_var, code_var)` overload.
template <int N>
inline void p_val(const ShortString<N>& s, double& out, int32_t& code) {
  std::string buf(s.data, s.data + s.length);
  char* end = nullptr;
  double v = std::strtod(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N>
inline void p_val(const ShortString<N>& s, long double& out, int32_t& code) {
  std::string buf(s.data, s.data + s.length);
  char* end = nullptr;
  long double v = std::strtold(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}

template <int N>
inline ShortString<> p_copy(const ShortString<N>& s, int start, int count) {
  ShortString<> r;
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

inline uint8_t p_upcase(uint8_t c) {
  return (c >= 'a' && c <= 'z') ? static_cast<uint8_t>(c - 32) : c;
}
template <int N>
inline ShortString<N> p_upcase(const ShortString<N>& s) {
  ShortString<N> r = s;
  for (int i = 0; i < r.length; ++i) r.data[i] = static_cast<char>(p_upcase(static_cast<uint8_t>(r.data[i])));
  return r;
}

// --- Write / Writeln --------------------------------------------------------
// Variadic emit: each call translates to a sequence of one-arg writes.

// Single-value writers to stdout.
template <int N>
inline void p_write_one(const ShortString<N>& s) {
  std::fwrite(s.data, 1, s.length, stdout);
}
inline void p_write_one(const char* s)    { if (s) std::fputs(s, stdout); }
inline void p_write_one(int32_t v)        { std::fprintf(stdout, "%d", v); }
inline void p_write_one(uint32_t v)       { std::fprintf(stdout, "%u", v); }
inline void p_write_one(int64_t v)        { std::fprintf(stdout, "%lld", (long long)v); }
inline void p_write_one(uint64_t v)       { std::fprintf(stdout, "%llu", (unsigned long long)v); }
inline void p_write_one(double v)         { std::fprintf(stdout, "%g", v); }
inline void p_write_one(long double v)    { std::fprintf(stdout, "%Lg", v); }
inline void p_write_one(char c)           { std::fputc(c, stdout); }
inline void p_write_one(uint8_t c)        { std::fputc(c, stdout); }
inline void p_write_one(bool b)           { std::fputs(b ? "TRUE" : "FALSE", stdout); }
inline void p_write_one(const TextFile&)  {}  // first arg of `write(f, ...)`
template <typename T> inline void p_write_one(T* p) {
  std::fprintf(stdout, "%p", (void*)p);
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

// --- File-IO placeholders ---------------------------------------------------
// Real behaviour is added as units are translated that need them.
inline void p_assign(TextFile&, const ShortString<>&) {}
inline void p_reset(TextFile&) {}
inline void p_reset(TextFile&, int32_t) {}          // rec size form
inline void p_rewrite(TextFile&) {}
inline void p_rewrite(TextFile&, int32_t) {}         // rec size form
inline void p_close(TextFile&) {}
inline bool p_eof(const TextFile&) { return true; }
// typed-file variants
template <typename T> inline void p_assign(TypedFile<T>&, const ShortString<>&) {}
template <typename T> inline void p_reset(TypedFile<T>&) {}
template <typename T> inline void p_reset(TypedFile<T>&, int32_t) {}
template <typename T> inline void p_rewrite(TypedFile<T>&) {}
template <typename T> inline void p_rewrite(TypedFile<T>&, int32_t) {}
template <typename T> inline void p_close(TypedFile<T>&) {}
template <typename T> inline bool p_eof(const TypedFile<T>&) { return true; }

// --- Val / Str --------------------------------------------------------------

template <int N>
inline void p_val(const ShortString<N>& s, int32_t& out, int32_t& code) {
  std::string buf(s.data, s.data + s.length);
  char* end = nullptr;
  long v = std::strtol(buf.c_str(), &end, 10);
  if (end && *end == '\0') { out = static_cast<int32_t>(v); code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N>
inline void p_str(int32_t v, ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
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
inline void p_ovrgetbuf(int32_t&) {}
inline int32_t p_strnew(const char* s) { return 0; (void)s; }
inline void p_chmod(const ShortString<>&, int32_t) {}
template <typename... A> inline int32_t p_execmd(A&&...) { return 0; }
template <typename... A> inline void p_gettime(A&&...) {}
template <typename... A> inline void p_getdate(A&&...) {}
template <typename... A> inline void p_settime(A&&...) {}
template <typename... A> inline void p_setdate(A&&...) {}
// STUB: `datetime` is a record type in fpc's dos unit. Accept common
// field accesses (year, month, day, hour, min, sec) since owar.pas
// names them unprefixed.
struct DateTime { uint16_t p_year = 0, p_month = 0, p_day = 0,
                  p_hour = 0, p_min = 0, p_sec = 0, p_sec100 = 0; };
using p_datetime = DateTime;
inline int32_t p_segment(const void*) { return 0; }
inline int32_t p_offset(const void*) { return 0; }
inline int32_t p_extraoptions = 0;
inline int32_t p_moduleindex = 0;
// STUB: loadunit is a compiler hook for loading precompiled units.
// Accept any shape of arguments so overloaded calls in pmodules type-
// check. Always returns 0 ("not loaded from cache").
template <typename... A> inline int32_t p_loadunit(A&&...) { return 0; }
// STUB: dos.exec -- launches external process. No-op here means the
// bootstrap assembler/linker step won't actually run; the compiler
// path that invokes external tools has to be audited before release.
template <typename... A> inline void p_exec(A&&...) {}
// Pascal `include(set, elem)` / `exclude(set, elem)` add/remove a
// single element. Not stubs -- these are real Pascal set builtins.
template <typename E1, typename E2>
inline void p_include(Set<E1>& s, E2 v) { s.add(static_cast<E1>(v)); }
template <typename E1, typename E2>
inline void p_exclude(Set<E1>& s, E2 v) {
  int i = Set<E1>::idx(static_cast<E1>(v));
  if (i >= 0 && i < 64 * Set<E1>::W) {
    s.bits[i >> 6] &= ~(uint64_t{1} << (i & 63));
  }
}

// STUB: misc small dos/system builtins used by the compiler sources.
inline int32_t p_winstackpagesize = 4096;
// STUB: `popen(f, cmd, mode)` opens a pipe to a process.
template <typename F>
inline void p_popen(F&, const ShortString<>&, char) {}
// STUB: heap-trace hook.
template <typename F> inline void p_setheaptraceoutput(F&&) {}
template <typename F> inline void p_setlocaltime(F&&) {}
// STUB: file timestamp get/set used by assembler/link bookkeeping.
template <typename F, typename T> inline void p_getftime(F&&, T&) {}
template <typename F, typename T> inline void p_setftime(F&&, T) {}
// STUB: heap-trace extra-info hook. Accept any arity (the real one
// takes a size + function pointer).
template <typename... A> inline void p_setextrainfo(A&&...) {}
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
template <typename... A> inline int32_t p_getversion(A&&...) { return 0; }
// STUB: `doserror` is the DOS unit's global error code after a call.
inline int32_t p_doserror = 0;
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
template <typename... A> inline bool p_fstat(A&&...) { return false; }

// Return value of `getenv`. fpc's `dos.getenv` returns ShortString,
// `linux.getenv` returns pchar -- same lowered name, different
// types. The proxy converts to both, so one `rt::p_getenv` serves
// both `Dos.Getenv` and `Linux.Getenv` call sites.
struct GetEnvResult {
  const char* raw;  // null-terminated env value, or nullptr if unset
  constexpr operator const char*() const { return raw; }
  constexpr operator char*() const { return const_cast<char*>(raw); }
  operator ShortString<>() const {
    return raw ? ShortString<>(raw) : ShortString<>("");
  }
};
inline GetEnvResult p_getenv(const ShortString<>& name) {
  char buf[260]{};
  int n = name.length < 255 ? name.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = name.data[i];
  return {std::getenv(buf)};
}
// Pascal `Linux.Shell(cmd)` -- run a command via `/bin/sh -c`, i.e.
// POSIX `system(3)`. Used by the compiler for wildcard expansion.
inline int32_t p_shell(const ShortString<>& cmd) {
  char buf[260]{};
  int n = cmd.length < 255 ? cmd.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = cmd.data[i];
  return std::system(buf);
}
// STUB: `dosexitcode` is a function in fpc's dos unit that returns
// the exit code of the last `exec`-launched child. Since our `exec`
// is stubbed, return 0 ("success").
inline int32_t p_dosexitcode() { return 0; }

// `System.heapsize` appears as a plain value in the compiler's
// status prints. Real fpc sets this during startup; we expose a
// constant close-enough placeholder.
inline int32_t p_heapsize = 1 << 20;

// Pascal `val(s, n, code)` -- string-to-number parser. The emitter
// routes `System.Val(...)` to `::rt::p_val(...)`.
template <typename S, typename N>
inline void p_val(const S& s, N& n, int32_t& code) {
  code = 0;
  n = static_cast<N>(0);
  int i = 0;
  int len = p_length(s);
  int sign = 1;
  if (i < len && (s[i + 1] == '+' || s[i + 1] == '-')) {
    if (s[i + 1] == '-') sign = -1;
    ++i;
  }
  int64_t v = 0;
  bool any = false;
  while (i < len) {
    char c = static_cast<char>(s[i + 1]);
    if (c < '0' || c > '9') { code = i + 1; return; }
    v = v * 10 + (c - '0');
    any = true;
    ++i;
  }
  if (!any) { code = 1; return; }
  n = static_cast<N>(sign * v);
}

}  // namespace rt
