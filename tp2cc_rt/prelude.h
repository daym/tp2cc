#pragma once

// Minimal runtime for tp2cc-emitted code.
//
// This header is deliberately small -- we only provide the pieces the
// translator currently emits references to.
//
// Target: Linux 32-bit. Translated Pascal primitive types map to fixed-width
// C++ types (see emit.cc's primitive_type_map); short strings, sets, and a
// few I/O helpers are implemented here.
//
// Runtime design rule: do not add user-declared C++ constructors or
// constructor-based implicit conversions for the Pascal carrier types in this
// header.
// tp2cc models Pascal initialization/conversion explicitly in emitted code and
// named helpers (`tp2cc_shortstring_of`, `tp2cc_ansistring_of`, `tp2cc_open_array`,
// `p_method_ptr`, ...), not through hidden C++ construction hooks.
// User-declared constructors change the aggregate/POD surface that GCC uses
// for packed-layout decisions.

#include <algorithm>
#include <array>
#include <bit>
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
#include <memory>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ctime>
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

inline constexpr p_char tp2cc_char_of(char c) {
  return static_cast<p_char>(static_cast<uint8_t>(c));
}
inline constexpr p_char tp2cc_char_of(p_char c) {
  return c;
}
inline constexpr p_char tp2cc_char_of(uint8_t c) {
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
  for (size_t i = 0; i < n; ++i) buf[i] = tp2cc_char_of(s[i]);
  buf[n] = tp2cc_char_of('\0');
  return buf.data();
}

template <typename Fn>
inline void* p_funptr_bits(Fn fn) {
  static_assert(std::is_pointer_v<Fn>,
                "method thunks must use plain function pointers");
  static_assert(sizeof(Fn) == sizeof(void*),
                "function-pointer and pointer sizes must match");
  return std::bit_cast<void*>(fn);
}

template <typename Fn>
inline Fn p_funptr_from_bits(void* bits) {
  static_assert(std::is_pointer_v<Fn>,
                "method thunks must use plain function pointers");
  static_assert(sizeof(Fn) == sizeof(void*),
                "function-pointer and pointer sizes must match");
  return std::bit_cast<Fn>(bits);
}

template <typename Signature>
struct tp2cc_MethodPtr;

// Raw `{Code,Data}` storage used by Pascal code that reinterprets method
// pointers as plain records instead of invoking them through `... of object`.
struct p_tmethod {
  void* p_code = nullptr;
  void* p_data = nullptr;
};

template <typename Ret, typename... Args>
struct tp2cc_MethodPtr<Ret(Args...)> {
  using Thunk = Ret (*)(void*, Args...);

  // Keep the layout as two pointer-sized slots so Pascal code that
  // reinterprets a method pointer as `record(pointer, pointer)` still
  // sees the expected bytes.
  void* code = nullptr;
  void* self = nullptr;

  constexpr tp2cc_MethodPtr& operator=(std::nullptr_t) {
    code = nullptr;
    self = nullptr;
    return *this;
  }

  constexpr explicit operator bool() const { return code != nullptr; }

  Ret operator()(Args... args) const {
    if (!code) std::abort();
    Thunk thunk = p_funptr_from_bits<Thunk>(code);
    if constexpr (std::is_void_v<Ret>) {
      thunk(self, std::forward<Args>(args)...);
      return;
    } else {
      return thunk(self, std::forward<Args>(args)...);
    }
  }

  friend constexpr bool operator==(const tp2cc_MethodPtr& a, const tp2cc_MethodPtr& b) {
    return a.code == b.code && a.self == b.self;
  }
  friend constexpr bool operator!=(const tp2cc_MethodPtr& a, const tp2cc_MethodPtr& b) {
    return !(a == b);
  }
  friend constexpr bool operator==(const tp2cc_MethodPtr& a, std::nullptr_t) {
    return a.code == nullptr;
  }
  friend constexpr bool operator==(std::nullptr_t, const tp2cc_MethodPtr& a) {
    return a == nullptr;
  }
  friend constexpr bool operator!=(const tp2cc_MethodPtr& a, std::nullptr_t) {
    return !(a == nullptr);
  }
  friend constexpr bool operator!=(std::nullptr_t, const tp2cc_MethodPtr& a) {
    return !(a == nullptr);
  }
};

template <typename Sig>
constexpr tp2cc_MethodPtr<Sig> p_method_ptr(void* code, void* self) {
  return {code, self};
}

template <auto Fn>
inline void* tp2cc_method_code() {
  return p_funptr_bits(Fn);
}

template <int N> struct tp2cc_ShortString;
template <int N> struct tp2cc_ShortStringPtrValue;
template <int N> struct tp2cc_ShortStringPtrRef;
class tp2cc_AnsiString;
template <int N = 255> constexpr tp2cc_ShortString<N> tp2cc_shortstring_of();
template <int N = 255> constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const char* s);
template <int N = 255> constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const p_char* s);
template <int N = 255> constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(p_char c);
template <int N = 255> constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(char c);
template <int N = 255, int M>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const tp2cc_ShortString<M>& o);
template <int N, typename Src>
inline void tp2cc_shortstring_assign(tp2cc_ShortString<N>& dest, const Src& src);
template <int N, typename Src>
inline void tp2cc_shortstring_assign(tp2cc_ShortStringPtrRef<N> dest, const Src& src);
struct p_tobject;
struct p_exception;
struct tp2cc_metaclass_p_tobject;
using p_tclass = const tp2cc_metaclass_p_tobject*;

struct tp2cc_metaclass_p_tobject {
  p_tobject* (*p_create)() = nullptr;

  tp2cc_metaclass_p_tobject(p_tobject* (*tp2cc_p_create)() = nullptr)
      : p_create(tp2cc_p_create) {}

  // `TClass` / `InheritsFrom` only need the direct class parent, not full RTTI.
  // Each generated metaclass descriptor overrides this hook with the exact
  // singleton for its Pascal parent class.
  virtual p_tclass tp2cc_parentclass() const { return nullptr; }
};

inline const tp2cc_metaclass_p_tobject* tp2cc_metaclass_value_p_tobject();

struct tp2cc_metaclass_p_exception : public tp2cc_metaclass_p_tobject {
  using tp2cc_metaclass_p_tobject::tp2cc_metaclass_p_tobject;

  // Generated subclasses of `Exception` reuse the root metaclass thunk but
  // still need a concrete `tp2cc_metaclass_p_exception` object so their
  // `ClassType` value stays convertible to `TClass` and keeps the exception
  // parent hook.
  tp2cc_metaclass_p_exception(tp2cc_metaclass_p_tobject tp2cc_parent)
      : tp2cc_metaclass_p_tobject(tp2cc_parent) {}

  p_tclass tp2cc_parentclass() const override;
};

inline const tp2cc_metaclass_p_exception* tp2cc_metaclass_value_p_exception();

// Delphi/FPC `class` types are references to heap objects whose base
// contract is `TObject`. Keep the runtime base explicit rather than
// letting each translated unit invent its own ad-hoc root.
struct p_tobject {
  // Pascal class construction starts at TObject.Create. The default root
  // implementation just succeeds; translated derived constructors chain to
  // it via `inherited Create`.
  virtual bool p_create() { return true; }
  // Old Delphi/FPC object code queries the dynamic class descriptor and
  // instance byte size via TObject.ClassType / InstanceSize. Derived
  // translated classes override these with concrete answers.
  virtual p_tclass p_classtype() const { return tp2cc_metaclass_value_p_tobject(); }
  virtual int32_t p_instancesize() const { return static_cast<int32_t>(sizeof(*this)); }
  virtual void p_destroy() {}
  // `FreeInstance` is the raw storage-release hook underneath `Free`.
  // Old compiler code overrides it directly (for refcounted symbol-table
  // nodes, for example), so keep it virtual and separate from `p_destroy`.
  virtual void p_freeinstance() { delete this; }
  virtual ~p_tobject() = default;
  // Pascal `obj.Free` is null-safe. That cannot be a normal C++ member
  // call on a pointer, because `obj->p_free()` is already UB when `obj`
  // is null. The emitter therefore lowers Pascal `Free` to this static
  // helper, which owns the null check and only then dispatches the
  // virtual Pascal destruction sequence.
  template <typename T>
  static void p_free(T* p) {
    static_assert(std::is_base_of_v<p_tobject, T>,
                  "p_tobject::p_free expects a translated Pascal class type");
    if (!p) return;
    p->p_destroy();
    p->p_freeinstance();
  }

  // `obj.InheritsFrom(TClassVar)` is the object-side subset of Delphi/FPC
  // metaclass RTTI that the bootstrap compiler actually uses. Walk the direct
  // metaclass parent hooks instead of requiring a larger reflection surface.
  bool p_inheritsfrom(p_tclass p_aclass) const {
    if (!p_aclass) return false;
    for (p_tclass p_meta = p_classtype(); p_meta;
         p_meta = p_meta->tp2cc_parentclass()) {
      if (p_meta == p_aclass) return true;
    }
    return false;
  }
};

inline void p_assert(bool ok) {
  if (!ok) std::abort();
}

template <int N>
inline void p_assert(bool ok, const tp2cc_ShortString<N>&) {
  if (!ok) std::abort();
}

// GNU `feenableexcept` / `fedisableexcept` are public in glibc's C headers,
// but this toolchain's libstdc++ `<fenv.h>` wrapper does not surface them to
// C++ consistently. Keep the backend in a tiny C translation unit and expose
// just the Pascal-level mask bits here.
extern "C" uint8_t tp2cc_get_exception_mask_bits(void);
extern "C" uint8_t tp2cc_set_exception_mask_bits(uint8_t bits);
extern "C" uint16_t tp2cc_get_8087_control_word(void);
extern "C" void tp2cc_set_8087_control_word(uint16_t cw);

// Keep the old Pascal-visible low-level control-word hooks available for the
// bootstrap compiler sources. That compatibility path only carries the six
// exception-mask bits that `globals.pas` still edits; higher-level code
// should prefer `GetExceptionMask` / `SetExceptionMask`.
inline uint16_t p_get8087cw() { return tp2cc_get_8087_control_word(); }
inline void p_set8087cw(uint16_t cw) { tp2cc_set_8087_control_word(cw); }

using p_ppointer = void**;

// The translated `sysutils` stub aliases into `rt::`, and compiler units
// declare exception subclasses against that alias. Keep a minimal base
// available here so those classes compile before a full SysUtils exists.
// `p_exception`'s definition lives lower down -- the `p_message` field is
// `tp2cc_AnsiString`, whose full type isn't yet visible here. The forward
// declaration earlier in this header is enough for the metaclass
// machinery; subclasses and `new p_exception` users land below
// `tp2cc_AnsiString` so the field has a complete type.

inline const tp2cc_metaclass_p_tobject* tp2cc_metaclass_value_p_tobject() {
  static const tp2cc_metaclass_p_tobject value{+[]() -> p_tobject* {
    auto* tp2cc_ptr = new p_tobject{};
    tp2cc_ptr->p_create();
    return tp2cc_ptr;
  }};
  return &value;
}

// Pascal `try .. finally` runs the cleanup block on every exit path:
// ordinary fallthrough, `Exit`, loop control, and exception unwinding.
// Keep that behaviour explicit with a small C++ scope guard instead of
// duplicating the finally-body at each translated exit site.
template <typename F>
class tp2cc_scope_exit {
 public:
  ~tp2cc_scope_exit() {
    if (active_ && *active_) {
      *active_ = false;
      fn_();
    }
  }

  F fn_;
  std::shared_ptr<bool> active_;
};

template <typename F>
tp2cc_scope_exit<F> tp2cc_make_scope_exit(F fn) {
  return {std::move(fn), std::make_shared<bool>(true)};
}

struct CharConst;

struct tp2cc_ShortStringCharValue {
  const uint8_t* byte = nullptr;

  constexpr explicit operator uint8_t() const { return *byte; }
  constexpr operator p_char() const { return tp2cc_char_of(*byte); }

  const p_char* operator&() const {
    return reinterpret_cast<const p_char*>(byte);
  }
};

struct tp2cc_ShortStringCharRef {
  uint8_t* byte = nullptr;

  constexpr explicit operator uint8_t() const { return *byte; }
  constexpr operator p_char() const { return tp2cc_char_of(*byte); }

  constexpr tp2cc_ShortStringCharRef& operator=(const tp2cc_ShortStringCharRef& other) {
    *byte = *other.byte;
    return *this;
  }

  template <typename T>
  requires std::is_convertible_v<T, p_char>
  constexpr tp2cc_ShortStringCharRef& operator=(T x) {
    *byte = p_char_byte(static_cast<p_char>(x));
    return *this;
  }

  constexpr tp2cc_ShortStringCharRef& operator=(uint8_t x) {
    *byte = x;
    return *this;
  }

  p_char* operator&() const {
    return reinterpret_cast<p_char*>(byte);
  }
};

// --- tp2cc_ShortString<N> --------------------------------------------------------
//
// Pascal-compatible short-string layout: 1 length byte followed by N content
// bytes. Fixed size. Default capacity is 255 (classic `string`).

template <int N = 255>
struct tp2cc_ShortString {
  static_assert(N >= 1 && N <= 255, "tp2cc_ShortString capacity must be 1..255");

  // No default member initialisers, so `std::is_trivial_v<tp2cc_ShortString>`
  // holds; that lets a tp2cc_ShortString live as a field of a packed record
  // without GCC silently dropping the `[[gnu::packed]]` attribute.
  // Consumers always read no further than `length`, and the emitter
  // value-inits locals (`tp2cc_ShortString<> s{};`) so Pascal `var s : string;`
  // still starts empty.
  uint8_t length;
  p_char data[N];

  constexpr uint8_t size() const { return length; }
  constexpr bool empty() const { return length == 0; }

  template <int M, typename = std::enable_if_t<M != N>>
  constexpr operator tp2cc_ShortString<M>() const {
    return tp2cc_shortstring_of<M>(*this);
  }

  operator tp2cc_AnsiString() const;

  constexpr tp2cc_ShortString& operator=(p_char c) {
    length = 1;
    data[0] = c;
    return *this;
  }

  constexpr tp2cc_ShortString& operator=(char c) {
    return (*this = tp2cc_char_of(c));
  }

  // Cross-capacity equality -- declared as a friend template so
  // `s<255> == s<10>` is unambiguous.
  template <int M>
  friend constexpr bool operator==(const tp2cc_ShortString& a,
                                   const tp2cc_ShortString<M>& b) {
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; ++i)
      if (a.data[i] != b.data[i]) return false;
    return true;
  }
  template <int M>
  friend constexpr bool operator!=(const tp2cc_ShortString& a,
                                   const tp2cc_ShortString<M>& b) {
    return !(a == b);
  }

  // Pascal string ordering is per-byte, length-then-content isn't
  // right: compare characters up to min-length, shorter is smaller on
  // tie. Emit the full four relational ops so sort-like code works.
  template <int M>
  friend constexpr bool operator<(const tp2cc_ShortString& a,
                                  const tp2cc_ShortString<M>& b) {
    int n = a.length < b.length ? a.length : b.length;
    for (int i = 0; i < n; ++i) {
      if (a.data[i] < b.data[i]) return true;
      if (a.data[i] > b.data[i]) return false;
    }
    return a.length < b.length;
  }
  template <int M>
  friend constexpr bool operator>(const tp2cc_ShortString& a,
                                  const tp2cc_ShortString<M>& b) {
    return b < a;
  }
  template <int M>
  friend constexpr bool operator<=(const tp2cc_ShortString& a,
                                   const tp2cc_ShortString<M>& b) {
    return !(b < a);
  }
  template <int M>
  friend constexpr bool operator>=(const tp2cc_ShortString& a,
                                   const tp2cc_ShortString<M>& b) {
    return !(a < b);
  }

  // Pascal occasionally writes `s := +t;` -- a unary `+` on a string,
  // which is a no-op. Provide the operator so the emitted C++ mirror
  // (`s = +t;`) type-checks.
  friend constexpr tp2cc_ShortString operator+(const tp2cc_ShortString& a) { return a; }

  // Pascal `s1 + s2` concatenation across any two tp2cc_ShortString capacities.
  // Declared as a non-member friend with explicit template parameters so
  // mixing `tp2cc_ShortString<N1>` with `tp2cc_ShortString<N2>` is unambiguous -- each
  // instantiation produces exactly one best viable overload (result type
  // is whichever side has the larger capacity).
  template <int M>
  friend constexpr auto operator+(const tp2cc_ShortString& a,
                                  const tp2cc_ShortString<M>& b) {
    constexpr int R = (N > M ? N : M);
    tp2cc_ShortString<R> out{};
    int n = a.length + b.length;
    if (n > R) n = R;
    for (int i = 0; i < a.length && i < R; ++i) out.data[i] = a.data[i];
    int off = a.length;
    for (int i = 0; i + off < R && i < b.length; ++i)
      out.data[off + i] = b.data[i];
    out.length = static_cast<uint8_t>(n);
    return out;
  }

  friend constexpr tp2cc_ShortString operator+(const tp2cc_ShortString& a, const char* b) {
    return a + tp2cc_shortstring_of<N>(b);
  }
  friend constexpr tp2cc_ShortString operator+(const char* a, const tp2cc_ShortString& b) {
    return tp2cc_shortstring_of<N>(a) + b;
  }
  friend constexpr auto operator+(const tp2cc_ShortString& a, p_char c) {
    return a + tp2cc_shortstring_of<>(c);
  }
  friend constexpr auto operator+(p_char c, const tp2cc_ShortString& b) {
    return tp2cc_shortstring_of<>(c) + b;
  }

  // Pascal `s[i]` is 1-based. We model the access: index 0 gives the
  // length byte (as in TP memory layout), 1..length give the characters.
  constexpr tp2cc_ShortStringCharRef operator[](int i) {
    return tp2cc_ShortStringCharRef{
        i == 0 ? &length : reinterpret_cast<uint8_t*>(&data[i - 1])};
  }
  constexpr tp2cc_ShortStringCharValue operator[](int i) const {
    return tp2cc_ShortStringCharValue{
        i == 0 ? &length : reinterpret_cast<const uint8_t*>(&data[i - 1])};
  }
};

// Turbo Pascal / Delphi `^string` storage is a live prefix: callers often
// allocate only `length(s) + 1` bytes and still expect `p^`, `length(p^)`,
// and `p^[i]` to work. A typed-pointer dereference therefore cannot be a
// normal `tp2cc_ShortString&`, because that would pretend a full `tp2cc_ShortString<N>`
// object exists in storage that may be much smaller.
template <int N>
struct tp2cc_ShortStringPtrValue {
  const uint8_t* storage = nullptr;

  constexpr int32_t size() const {
    return storage ? static_cast<int32_t>(*storage) : 0;
  }
  constexpr bool empty() const { return size() == 0; }
  constexpr const p_char* bytes() const {
    return storage ? reinterpret_cast<const p_char*>(storage + 1) : nullptr;
  }
  constexpr tp2cc_ShortStringCharValue operator[](int i) const {
    return tp2cc_ShortStringCharValue{storage + i};
  }

  template <int M = N>
  constexpr operator tp2cc_ShortString<M>() const {
    tp2cc_ShortString<M> out{};
    const int32_t n = std::min<int32_t>(size(), M);
    const p_char* src = bytes();
    out.length = static_cast<uint8_t>(n);
    for (int32_t i = 0; i < n; ++i) out.data[i] = src[i];
    return out;
  }

  operator tp2cc_AnsiString() const;
};

template <int N>
struct tp2cc_ShortStringPtrRef {
  uint8_t* storage = nullptr;

  constexpr int32_t size() const {
    return storage ? static_cast<int32_t>(*storage) : 0;
  }
  constexpr bool empty() const { return size() == 0; }
  constexpr p_char* bytes() const {
    return storage ? reinterpret_cast<p_char*>(storage + 1) : nullptr;
  }
  constexpr operator tp2cc_ShortStringPtrValue<N>() const { return {storage}; }
  template <int M = N>
  constexpr operator tp2cc_ShortString<M>() const {
    return static_cast<tp2cc_ShortString<M>>(tp2cc_ShortStringPtrValue<N>{storage});
  }
  operator tp2cc_AnsiString() const;
  constexpr tp2cc_ShortStringCharRef operator[](int i) const {
    return tp2cc_ShortStringCharRef{storage + i};
  }
};

template <typename T>
struct tp2cc_shortstring_capacity;

template <int N>
struct tp2cc_shortstring_capacity<tp2cc_ShortString<N>>
    : std::integral_constant<int, N> {};

template <int N>
struct tp2cc_shortstring_capacity<tp2cc_ShortStringPtrValue<N>>
    : std::integral_constant<int, N> {};

template <int N>
struct tp2cc_shortstring_capacity<tp2cc_ShortStringPtrRef<N>>
    : std::integral_constant<int, N> {};

template <typename T>
inline constexpr int tp2cc_shortstring_capacity_v =
    tp2cc_shortstring_capacity<std::remove_cvref_t<T>>::value;

template <typename T>
struct tp2cc_is_shortstring_proxy : std::false_type {};

template <int N>
struct tp2cc_is_shortstring_proxy<tp2cc_ShortStringPtrValue<N>> : std::true_type {};

template <int N>
struct tp2cc_is_shortstring_proxy<tp2cc_ShortStringPtrRef<N>> : std::true_type {};

template <typename T>
inline constexpr bool tp2cc_is_shortstring_proxy_v =
    tp2cc_is_shortstring_proxy<std::remove_cvref_t<T>>::value;

template <typename T>
struct tp2cc_is_fixed_shortstring_like : std::false_type {};

template <int N>
struct tp2cc_is_fixed_shortstring_like<tp2cc_ShortString<N>> : std::true_type {};

template <int N>
struct tp2cc_is_fixed_shortstring_like<tp2cc_ShortStringPtrValue<N>>
    : std::true_type {};

template <int N>
struct tp2cc_is_fixed_shortstring_like<tp2cc_ShortStringPtrRef<N>>
    : std::true_type {};

template <typename T>
inline constexpr bool tp2cc_is_fixed_shortstring_like_v =
    tp2cc_is_fixed_shortstring_like<std::remove_cvref_t<T>>::value;

template <int N>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of() {
  return {};
}

template <int N>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const char* s) {
  tp2cc_ShortString<N> out{};
  int n = 0;
  if (s) {
    while (s[n] && n < N) ++n;
  }
  out.length = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) out.data[i] = tp2cc_char_of(s[i]);
  return out;
}

template <int N>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const p_char* s) {
  tp2cc_ShortString<N> out{};
  int n = 0;
  if (s) {
    while (p_char_byte(s[n]) != 0 && n < N) ++n;
  }
  out.length = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) out.data[i] = s[i];
  return out;
}

template <int N>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(p_char c) {
  tp2cc_ShortString<N> out{};
  out.length = 1;
  out.data[0] = c;
  return out;
}

template <int N>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(char c) {
  return tp2cc_shortstring_of<N>(tp2cc_char_of(c));
}

template <int N, int M>
constexpr tp2cc_ShortString<N> tp2cc_shortstring_of(const tp2cc_ShortString<M>& o) {
  tp2cc_ShortString<N> out{};
  int n = o.length;
  if (n > N) n = N;
  out.length = static_cast<uint8_t>(n);
  for (int i = 0; i < n; ++i) out.data[i] = o.data[i];
  return out;
}

// Pascal `const X = 'c';` declares a constant that is BOTH a char
// (assignable into `s[i] : char` contexts) and a 1-element string
// (usable in string concatenations). C++ can't have one type that
// plays both roles, so the emitter wraps such consts in this tag
// struct with implicit conversions in both directions. Scoped to
// the const decl -- ordinary tp2cc_ShortString variables are unaffected.
struct CharConst {
  p_char c;
  // Conversions: `p_char` for Pascal char contexts, and
  // `tp2cc_ShortString<N>` for string-concatenation contexts.
  constexpr operator p_char() const { return c; }
  template <int N = 255>
  constexpr operator tp2cc_ShortString<N>() const {
    return tp2cc_shortstring_of<N>(c);
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
inline constexpr tp2cc_ShortString<> operator+(CharConst a, CharConst b) {
  tp2cc_ShortString<> r{};
  r.data[0] = a.c;
  r.data[1] = b.c;
  r.length = 2;
  return r;
}
inline constexpr tp2cc_ShortString<> operator+(CharConst a, p_char b) {
  return tp2cc_shortstring_of<>(a.c) + b;
}
inline constexpr tp2cc_ShortString<> operator+(p_char a, CharConst b) {
  return a + tp2cc_shortstring_of<>(b.c);
}
template <int N>
inline constexpr auto operator+(CharConst a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<>(a.c) + b;
}
template <int N>
inline constexpr auto operator+(const tp2cc_ShortString<N>& a, CharConst b) {
  return a + tp2cc_shortstring_of<>(b.c);
}

// --- tp2cc_AnsiString ------------------------------------------------------------
//
// Generated compiler code treats an ansistring value in two ways at once:
// as a Pascal string value, and as "the variable whose first storage slot
// contains the payload pointer". Keep the runtime wrapper to exactly one
// pointer data member so low-level emitted operations like
// `reinterpret_storage_ref<void*>(s)` still see the expected bytes.

struct AnsiStringHeader {
  int32_t len;
};

inline p_char* p_ansistring_empty_bytes() {
  static p_char empty[1] = {tp2cc_char_of('\0')};
  return empty;
}

inline bool p_ansistring_is_empty_bytes(const p_char* data) {
  return data == p_ansistring_empty_bytes();
}

inline AnsiStringHeader* p_ansistring_header(p_char* data) {
  return reinterpret_cast<AnsiStringHeader*>(
      reinterpret_cast<unsigned char*>(data) - sizeof(AnsiStringHeader));
}

inline const AnsiStringHeader* p_ansistring_header(const p_char* data) {
  return reinterpret_cast<const AnsiStringHeader*>(
      reinterpret_cast<const unsigned char*>(data) - sizeof(AnsiStringHeader));
}

inline std::vector<void*>& p_ansistring_allocations() {
  static auto* allocations = new std::vector<void*>();
  return *allocations;
}

inline void p_ansistring_release_all() {
  auto& allocations = p_ansistring_allocations();
  for (void* raw : allocations) std::free(raw);
  allocations.clear();
}

inline void p_ansistring_track_allocation(void* raw) {
  static bool registered = false;
  if (!registered) {
    std::atexit(p_ansistring_release_all);
    registered = true;
  }
  p_ansistring_allocations().push_back(raw);
}

inline p_char* p_ansistring_alloc_bytes(int32_t len) {
  if (len <= 0) return p_ansistring_empty_bytes();
  auto* raw = static_cast<unsigned char*>(
      std::malloc(sizeof(AnsiStringHeader) +
                  static_cast<size_t>(len + 1) * sizeof(p_char)));
  if (!raw) std::abort();
  p_ansistring_track_allocation(raw);
  auto* hdr = reinterpret_cast<AnsiStringHeader*>(raw);
  hdr->len = len;
  auto* data = reinterpret_cast<p_char*>(raw + sizeof(AnsiStringHeader));
  std::memset(data, 0, static_cast<size_t>(len + 1) * sizeof(p_char));
  return data;
}

inline p_char* p_ansistring_alloc_owned_empty() {
  auto* raw = static_cast<unsigned char*>(
      std::malloc(sizeof(AnsiStringHeader) + sizeof(p_char)));
  if (!raw) std::abort();
  p_ansistring_track_allocation(raw);
  auto* hdr = reinterpret_cast<AnsiStringHeader*>(raw);
  hdr->len = 0;
  auto* data = reinterpret_cast<p_char*>(raw + sizeof(AnsiStringHeader));
  data[0] = tp2cc_char_of('\0');
  return data;
}

class tp2cc_AnsiString;

struct tp2cc_AnsiStringCharValue {
  const p_char* byte = nullptr;

  constexpr operator p_char() const { return *byte; }

  const p_char* operator&() const { return byte; }
};

struct tp2cc_AnsiStringCharRef {
  tp2cc_AnsiString* owner = nullptr;
  int index = 0;  // zero-based byte index within the payload

  operator p_char() const;
  // Mirror tp2cc_ShortStringCharRef: Pascal `byte(s[i])` lowers as
  // `(uint8_t)(s[i])`, and we need the same explicit conversion path
  // for AnsiString as for ShortString or the cast at the call site
  // fails to chain through the proxy. Without this, `byte(ansi[i])`
  // builds for ShortString and breaks for AnsiString -- the kind of
  // type-asymmetry that hides p_char-vs-uint8_t mistakes.
  explicit operator uint8_t() const;
  tp2cc_AnsiStringCharRef& operator=(p_char value);
  tp2cc_AnsiStringCharRef& operator=(const tp2cc_AnsiStringCharRef& other);
  p_char* operator&();
};

class tp2cc_AnsiString {
 public:
  p_char* data = p_ansistring_empty_bytes();

  template <int N>
  tp2cc_AnsiString& operator=(const tp2cc_ShortString<N>& s) {
    assign_bytes(s.data, s.length);
    return *this;
  }

  tp2cc_AnsiString& operator=(const char* s) {
    assign_c_str(s);
    return *this;
  }

  tp2cc_AnsiString& operator=(const p_char* s) {
    assign_pascal_c_str(s);
    return *this;
  }

  tp2cc_AnsiString& operator=(p_char c) {
    set_length(1);
    data[0] = c;
    return *this;
  }

  tp2cc_AnsiString& operator=(std::nullptr_t) {
    clear();
    return *this;
  }

  int32_t length() const {
    return p_ansistring_is_empty_bytes(data) ? 0 : p_ansistring_header(data)->len;
  }

  bool empty() const { return length() == 0; }

  const p_char* bytes() const { return data; }

  p_char* mutable_bytes() {
    ensure_unique();
    return data;
  }

  void clear() {
    data = p_ansistring_empty_bytes();
  }

  void ensure_unique() {
    if (p_ansistring_is_empty_bytes(data)) {
      data = p_ansistring_alloc_owned_empty();
      return;
    }
    int32_t len = p_ansistring_header(data)->len;
    p_char* fresh = p_ansistring_alloc_bytes(len);
    std::memcpy(fresh, data, static_cast<size_t>(len + 1) * sizeof(p_char));
    data = fresh;
  }

  void set_length(int32_t new_len) {
    if (new_len <= 0) {
      clear();
      return;
    }

    int32_t old_len = length();
    p_char* fresh = p_ansistring_alloc_bytes(new_len);
    int32_t keep = old_len < new_len ? old_len : new_len;
    if (keep > 0) {
      std::memcpy(fresh, data, static_cast<size_t>(keep) * sizeof(p_char));
    }
    data = fresh;
  }

  explicit operator int32_t() const {
    return static_cast<int32_t>(reinterpret_cast<std::uintptr_t>(data));
  }

  explicit operator uint32_t() const {
    return static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(data));
  }

  operator const p_char*() const { return data; }

  operator p_char*() { return data; }

  // Pascal freely passes ansistrings to `string` parameters when that
  // means materialising a shortstring temporary, so keep this conversion
  // implicit and let the destination capacity truncate as Pascal does.
  template <int N = 255>
  operator tp2cc_ShortString<N>() const {
    tp2cc_ShortString<N> out{};
    int32_t copy = length();
    if (copy > N) copy = N;
    out.length = static_cast<uint8_t>(copy);
    for (int32_t i = 0; i < copy; ++i) out.data[i] = data[i];
    return out;
  }

  tp2cc_AnsiStringCharRef operator[](int i) {
    return {this, i - 1};
  }

  tp2cc_AnsiStringCharValue operator[](int i) const {
    return tp2cc_AnsiStringCharValue{data + (i - 1)};
  }

 private:
  void assign_bytes(const p_char* src, int32_t len) {
    if (!src || len <= 0) {
      clear();
      return;
    }
    p_char* fresh = p_ansistring_alloc_bytes(len);
    std::memcpy(fresh, src, static_cast<size_t>(len) * sizeof(p_char));
    data = fresh;
  }

  void assign_c_str(const char* s) {
    if (!s || *s == '\0') {
      clear();
      return;
    }
    assign_bytes(reinterpret_cast<const p_char*>(s),
                 static_cast<int32_t>(std::strlen(s)));
  }

  void assign_pascal_c_str(const p_char* s) {
    if (!s || p_char_byte(*s) == 0) {
      clear();
      return;
    }
    int32_t len = 0;
    while (p_char_byte(s[len]) != 0) ++len;
    assign_bytes(s, len);
  }
};

// `p_exception`'s field `p_message` needs `tp2cc_AnsiString` to be a
// complete type, so the struct lives here rather than next to
// `p_tobject`. Forward declarations earlier in the header let the
// metaclass machinery refer to it ahead of this point.
struct p_exception : p_tobject {
  using inherited = p_tobject;
  using inherited::p_create;

  // Pascal `Exception.Message` -- sysutils declares this as `string`
  // under `{$H+}`, which is `AnsiString`. Stored on the base so every
  // derived exception (`EOSError`, `EIntError`, ...) has it for free.
  tp2cc_AnsiString p_message;

  p_tclass p_classtype() const override;
  int32_t p_instancesize() const override;

  bool p_create(const tp2cc_ShortString<255>& msg) {
    p_message = msg;
    return true;
  }
  bool p_create(const tp2cc_AnsiString& msg) {
    p_message = msg;
    return true;
  }
};

inline const tp2cc_metaclass_p_exception* tp2cc_metaclass_value_p_exception() {
  static const tp2cc_metaclass_p_exception value{
      +[]() -> p_tobject* {
        auto* tp2cc_ptr = new p_exception{};
        tp2cc_ptr->p_create();
        return tp2cc_ptr;
      }};
  return &value;
}

inline p_tclass tp2cc_metaclass_p_exception::tp2cc_parentclass() const {
  return tp2cc_metaclass_value_p_tobject();
}

inline p_tclass p_exception::p_classtype() const {
  return tp2cc_metaclass_value_p_exception();
}

inline int32_t p_exception::p_instancesize() const {
  return static_cast<int32_t>(sizeof(*this));
}

// Pascal sysutils exception hierarchy used by `{$Q+}` / `{$R+}` /
// `try ... except on E:EIntOverflow do ...`. The sysutils-stub
// generator (src/main.cc) aliases these names into `p_sysutils` so
// translated code in either configuration sees the same C++ classes.
struct p_eexternal : p_exception {
  using inherited = p_exception;
  using inherited::p_create;
};
struct p_einterror : p_eexternal {
  using inherited = p_eexternal;
  using inherited::p_create;
};
struct p_eintoverflow : p_einterror {
  using inherited = p_einterror;
  using inherited::p_create;
};
struct p_erangeerror : p_einterror {
  using inherited = p_einterror;
  using inherited::p_create;
};
struct p_edivbyzero : p_einterror {
  using inherited = p_einterror;
  using inherited::p_create;
};

[[noreturn]] inline void tp2cc_throw_int_overflow() {
  auto* e = new p_eintoverflow{};
  e->p_create();
  throw static_cast<p_tobject*>(e);
}

// Checked integer arithmetic for `{$Q+}`. Signed types use
// `__builtin_*_overflow` so the compiler emits an INTO-equivalent
// path; unsigned types match Pascal's wraparound semantics under
// Q+ (Pascal does not overflow-check unsigned).
template <typename T>
inline std::enable_if_t<std::is_signed_v<T>, T> tp2cc_add_checked(T a, T b) {
  T r;
  if (__builtin_add_overflow(a, b, &r)) tp2cc_throw_int_overflow();
  return r;
}
template <typename T>
inline std::enable_if_t<std::is_signed_v<T>, T> tp2cc_sub_checked(T a, T b) {
  T r;
  if (__builtin_sub_overflow(a, b, &r)) tp2cc_throw_int_overflow();
  return r;
}
template <typename T>
inline std::enable_if_t<std::is_signed_v<T>, T> tp2cc_mul_checked(T a, T b) {
  T r;
  if (__builtin_mul_overflow(a, b, &r)) tp2cc_throw_int_overflow();
  return r;
}
template <typename T>
inline std::enable_if_t<std::is_signed_v<T>, T> tp2cc_negate_checked(T x) {
  if (x == std::numeric_limits<T>::min()) tp2cc_throw_int_overflow();
  return static_cast<T>(-x);
}
template <typename T>
inline std::enable_if_t<std::is_unsigned_v<T>, T> tp2cc_add_checked(T a, T b) {
  return static_cast<T>(a + b);
}
template <typename T>
inline std::enable_if_t<std::is_unsigned_v<T>, T> tp2cc_sub_checked(T a, T b) {
  return static_cast<T>(a - b);
}
template <typename T>
inline std::enable_if_t<std::is_unsigned_v<T>, T> tp2cc_mul_checked(T a, T b) {
  return static_cast<T>(a * b);
}
template <typename T>
inline std::enable_if_t<std::is_unsigned_v<T>, T> tp2cc_negate_checked(T x) {
  return static_cast<T>(-x);
}

inline tp2cc_AnsiString tp2cc_ansistring_of(std::nullptr_t) {
  return tp2cc_AnsiString{};
}

inline tp2cc_AnsiString tp2cc_ansistring_of(const tp2cc_AnsiString& s) {
  tp2cc_AnsiString out{};
  out.data = s.data;
  return out;
}

inline tp2cc_AnsiString tp2cc_ansistring_of(const char* s) {
  tp2cc_AnsiString out{};
  out = s;
  return out;
}

inline tp2cc_AnsiString tp2cc_ansistring_of(const p_char* s) {
  tp2cc_AnsiString out{};
  out = s;
  return out;
}

template <int N>
inline tp2cc_AnsiString tp2cc_ansistring_of(const tp2cc_ShortString<N>& s) {
  tp2cc_AnsiString out{};
  out = s;
  return out;
}

template <int N>
inline tp2cc_AnsiString tp2cc_ansistring_of(tp2cc_ShortStringPtrValue<N> s) {
  return tp2cc_ansistring_of(static_cast<tp2cc_ShortString<N>>(s));
}

template <int N>
inline tp2cc_AnsiString tp2cc_ansistring_of(tp2cc_ShortStringPtrRef<N> s) {
  return tp2cc_ansistring_of(static_cast<tp2cc_ShortString<N>>(s));
}

template <int N>
inline tp2cc_ShortString<N> tp2cc_shortstring_of(const tp2cc_AnsiString& s) {
  return static_cast<tp2cc_ShortString<N>>(s);
}

inline tp2cc_AnsiString tp2cc_ansistring_of(p_char c) {
  tp2cc_AnsiString out{};
  out = c;
  return out;
}

template <int N>
inline tp2cc_ShortString<N>::operator tp2cc_AnsiString() const {
  return tp2cc_ansistring_of(*this);
}

template <int N>
inline tp2cc_ShortStringPtrValue<N>::operator tp2cc_AnsiString() const {
  return tp2cc_ansistring_of(*this);
}

template <int N>
inline tp2cc_ShortStringPtrRef<N>::operator tp2cc_AnsiString() const {
  return tp2cc_ansistring_of(*this);
}

inline tp2cc_AnsiStringCharRef::operator p_char() const {
  return owner->data[index];
}

inline tp2cc_AnsiStringCharRef::operator uint8_t() const {
  return p_char_byte(owner->data[index]);
}

inline tp2cc_AnsiStringCharRef& tp2cc_AnsiStringCharRef::operator=(p_char value) {
  owner->ensure_unique();
  owner->data[index] = value;
  return *this;
}

inline tp2cc_AnsiStringCharRef& tp2cc_AnsiStringCharRef::operator=(
    const tp2cc_AnsiStringCharRef& other) {
  return (*this = static_cast<p_char>(other));
}

inline p_char* tp2cc_AnsiStringCharRef::operator&() {
  owner->ensure_unique();
  return owner->data + index;
}

template <int N>
inline const p_char* p_string_bytes(const tp2cc_ShortString<N>& s) {
  return s.data;
}
template <int N>
inline const p_char* p_string_bytes(const tp2cc_ShortStringPtrValue<N>& s) {
  return s.bytes();
}
template <int N>
inline const p_char* p_string_bytes(const tp2cc_ShortStringPtrRef<N>& s) {
  return s.bytes();
}

inline const p_char* p_string_bytes(const tp2cc_AnsiString& s) {
  return s.bytes();
}

template <int N>
inline int32_t p_string_length(const tp2cc_ShortString<N>& s) {
  return s.length;
}
template <int N>
inline int32_t p_string_length(const tp2cc_ShortStringPtrValue<N>& s) {
  return s.size();
}
template <int N>
inline int32_t p_string_length(const tp2cc_ShortStringPtrRef<N>& s) {
  return s.size();
}

inline int32_t p_string_length(const tp2cc_AnsiString& s) {
  return s.length();
}

template <typename A, typename B>
inline int p_string_compare(const A& a, const B& b) {
  const int32_t a_len = p_string_length(a);
  const int32_t b_len = p_string_length(b);
  const p_char* a_bytes = p_string_bytes(a);
  const p_char* b_bytes = p_string_bytes(b);
  int32_t limit = a_len < b_len ? a_len : b_len;
  for (int32_t i = 0; i < limit; ++i) {
    uint8_t av = p_char_byte(a_bytes[i]);
    uint8_t bv = p_char_byte(b_bytes[i]);
    if (av < bv) return -1;
    if (av > bv) return 1;
  }
  if (a_len < b_len) return -1;
  if (a_len > b_len) return 1;
  return 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator==(const A& a, const B& b) {
  return p_string_compare(a, b) == 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator!=(const A& a, const B& b) {
  return !(a == b);
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator<(const A& a, const B& b) {
  return p_string_compare(a, b) < 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator>(const A& a, const B& b) {
  return p_string_compare(a, b) > 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator<=(const A& a, const B& b) {
  return p_string_compare(a, b) <= 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr bool operator>=(const A& a, const B& b) {
  return p_string_compare(a, b) >= 0;
}

template <typename A, typename B>
requires (tp2cc_is_fixed_shortstring_like_v<A> &&
          tp2cc_is_fixed_shortstring_like_v<B> &&
          (tp2cc_is_shortstring_proxy_v<A> ||
           tp2cc_is_shortstring_proxy_v<B>))
inline constexpr auto operator+(const A& a, const B& b) {
  constexpr int AN = tp2cc_shortstring_capacity_v<A>;
  constexpr int BN = tp2cc_shortstring_capacity_v<B>;
  return static_cast<tp2cc_ShortString<AN>>(a) + static_cast<tp2cc_ShortString<BN>>(b);
}

template <typename A, typename B>
inline tp2cc_AnsiString p_concat_to_ansistring(const A& a, const B& b) {
  const int32_t a_len = p_string_length(a);
  const int32_t b_len = p_string_length(b);
  tp2cc_AnsiString out{};
  out.set_length(a_len + b_len);
  if (a_len > 0) {
    std::memcpy(out.data, p_string_bytes(a),
                static_cast<size_t>(a_len) * sizeof(p_char));
  }
  if (b_len > 0) {
    std::memcpy(out.data + a_len, p_string_bytes(b),
                static_cast<size_t>(b_len) * sizeof(p_char));
  }
  return out;
}

inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& s) { return tp2cc_ansistring_of(s); }

template <int N>
inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_concat_to_ansistring(a, b);
}

template <int N>
inline tp2cc_AnsiString operator+(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_concat_to_ansistring(a, b);
}

inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_concat_to_ansistring(a, b);
}

inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& a, const char* b) {
  return a + tp2cc_ansistring_of(b);
}

inline tp2cc_AnsiString operator+(const char* a, const tp2cc_AnsiString& b) {
  return tp2cc_ansistring_of(a) + b;
}

inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& a, const p_char* b) {
  return a + tp2cc_ansistring_of(b);
}

inline tp2cc_AnsiString operator+(const p_char* a, const tp2cc_AnsiString& b) {
  return tp2cc_ansistring_of(a) + b;
}

inline tp2cc_AnsiString operator+(const tp2cc_AnsiString& a, p_char c) {
  return a + tp2cc_ansistring_of(c);
}

inline tp2cc_AnsiString operator+(p_char c, const tp2cc_AnsiString& b) {
  return tp2cc_ansistring_of(c) + b;
}

inline bool operator==(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) == 0;
}

inline bool operator!=(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return !(a == b);
}

inline bool operator<(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) < 0;
}

inline bool operator>(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) > 0;
}

inline bool operator<=(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) <= 0;
}

inline bool operator>=(const tp2cc_AnsiString& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) >= 0;
}

template <int N>
inline bool operator==(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_string_compare(a, b) == 0;
}

template <int N>
inline bool operator==(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) == 0;
}

// Pascal compares strings against a single `Char` literal by lifting
// the char to a one-character string of the same family, then doing
// a string compare. C++ doesn't have an implicit `p_char -> AnsiString`
// conversion (deliberately -- that path causes ambiguity in
// overload resolution elsewhere), so wire the lifted equality
// directly. True iff the AnsiString has length 1 and its first byte
// matches.
inline bool operator==(const tp2cc_AnsiString& a, p_char b) {
  return a.length() == 1 && a.bytes()[0] == b;
}
inline bool operator==(p_char a, const tp2cc_AnsiString& b) {
  return b == a;
}
inline bool operator!=(const tp2cc_AnsiString& a, p_char b) {
  return !(a == b);
}
inline bool operator!=(p_char a, const tp2cc_AnsiString& b) {
  return !(a == b);
}

template <int N>
inline bool operator==(const p_char* a, const tp2cc_ShortString<N>& b) {
  if (!a) return b.size() == 0;
  for (int i = 0; i < b.size(); ++i) {
    if (a[i] != b[i + 1]) return false;
  }
  return a[b.size()] == tp2cc_char_of('\0');
}

template <int N>
inline bool operator==(const tp2cc_ShortString<N>& a, const p_char* b) {
  return b == a;
}

template <typename S>
requires tp2cc_is_shortstring_proxy_v<S>
inline bool operator==(const p_char* a, const S& b) {
  if (!a) return b.size() == 0;
  for (int i = 0; i < b.size(); ++i) {
    if (a[i] != b[i + 1]) return false;
  }
  return a[b.size()] == tp2cc_char_of('\0');
}

template <typename S>
requires tp2cc_is_shortstring_proxy_v<S>
inline bool operator==(const S& a, const p_char* b) {
  return b == a;
}

template <int N>
inline bool operator==(const tp2cc_ShortString<N>& a, p_char b) {
  return a == tp2cc_shortstring_of<1>(b);
}

template <int N>
inline bool operator==(p_char a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<1>(a) == b;
}

template <int N>
inline bool operator!=(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return !(a == b);
}

template <int N>
inline bool operator!=(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return !(a == b);
}

template <int N>
inline bool operator!=(const tp2cc_ShortString<N>& a, p_char b) {
  return !(a == b);
}

template <int N>
inline bool operator!=(p_char a, const tp2cc_ShortString<N>& b) {
  return !(a == b);
}

template <int N>
inline bool operator<(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_string_compare(a, b) < 0;
}

template <int N>
inline bool operator<(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) < 0;
}

template <int N>
inline bool operator<(const tp2cc_ShortString<N>& a, p_char b) {
  return a < tp2cc_shortstring_of<1>(b);
}

template <int N>
inline bool operator<(p_char a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<1>(a) < b;
}

template <int N>
inline bool operator>(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_string_compare(a, b) > 0;
}

template <int N>
inline bool operator>(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) > 0;
}

template <int N>
inline bool operator>(const tp2cc_ShortString<N>& a, p_char b) {
  return a > tp2cc_shortstring_of<1>(b);
}

template <int N>
inline bool operator>(p_char a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<1>(a) > b;
}

template <int N>
inline bool operator<=(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_string_compare(a, b) <= 0;
}

template <int N>
inline bool operator<=(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) <= 0;
}

template <int N>
inline bool operator<=(const tp2cc_ShortString<N>& a, p_char b) {
  return a <= tp2cc_shortstring_of<1>(b);
}

template <int N>
inline bool operator<=(p_char a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<1>(a) <= b;
}

template <int N>
inline bool operator>=(const tp2cc_AnsiString& a, const tp2cc_ShortString<N>& b) {
  return p_string_compare(a, b) >= 0;
}

template <int N>
inline bool operator>=(const tp2cc_ShortString<N>& a, const tp2cc_AnsiString& b) {
  return p_string_compare(a, b) >= 0;
}

template <int N>
inline bool operator>=(const tp2cc_ShortString<N>& a, p_char b) {
  return a >= tp2cc_shortstring_of<1>(b);
}

template <int N>
inline bool operator>=(p_char a, const tp2cc_ShortString<N>& b) {
  return tp2cc_shortstring_of<1>(a) >= b;
}

// --- Common Pascal RTL type aliases ----------------------------------------
// Exposed in the `rt` namespace so emitted units pick them up via
// `using namespace ::rt;`.

// dos unit
using p_dirstr  = tp2cc_ShortString<255>;
using p_namestr = tp2cc_ShortString<255>;
using p_extstr  = tp2cc_ShortString<255>;
using p_pathstr = tp2cc_ShortString<255>;
using p_comstr  = tp2cc_ShortString<255>;
// The current tp2cc bootstrap runtime targets 32-bit hosts only. Match
// FPC's CPU32 aliases here so translated compiler code sees pointer-sized
// integers as `longint`/`dword` equivalents.
using p_longint  = int32_t;
using p_dword    = uint32_t;
using p_sizeint  = p_longint;
using p_sizeuint = p_dword;
using p_ptrint   = p_longint;
using p_ptruint  = p_dword;

// objects unit
using p_sw_integer = int32_t;
using p_sw_word    = uint32_t;

// linux / file descriptors
using p_thandle   = int32_t;
using p_tfiletime = int64_t;
using p_tdatetime = double;
using p_currency  = int64_t;
using p_pansistring = tp2cc_AnsiString*;

using p_pcardinal = uint32_t*;
using p_pcurrency = p_currency*;
using p_pint64    = int64_t*;

// Only `vtAnsiString` is needed: ncgld.pas locally redeclares all the
// other vt* tags in a procedure-scoped `const` block, so the bare name
// `vtAnsiString` is the only one that falls through to the system unit.
inline constexpr int32_t p_vtansistring = 11;
// Win32 share-mode flag, no-op on POSIX. The compiler ORs it into open
// modes for .ppu/.res files; the value just has to round-trip without
// colliding with the real fmOpen* bits.
inline constexpr int32_t p_fmsharedenynone = 0x40;
// Variant type tag for "string argument" used by IDispatch dispatch
// descriptors. The bootstrap compiler tags each translate_disp_call arg
// with these `varXxx` codes; only the literal `varStrArg` appears in
// the source, so this is the single tag we need to expose. The compiler
// itself never invokes IDispatch -- the constant just has to be present
// so ncal.pas's `translate_disp_call` body translates cleanly.
inline constexpr int32_t p_varstrarg = 0x48;

struct p_tsystemtime {
  uint16_t p_year = 0;
  uint16_t p_month = 0;
  uint16_t p_dayofweek = 0;
  uint16_t p_day = 0;
  uint16_t p_hour = 0;
  uint16_t p_minute = 0;
  uint16_t p_second = 0;
  uint16_t p_milliseconds = 0;
};

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
inline T p_swapendian(T value) {
  if constexpr (std::is_enum_v<T>) {
    using Plain = std::underlying_type_t<T>;
    using Unsigned = std::make_unsigned_t<Plain>;
    Unsigned bits = static_cast<Unsigned>(value);
    if constexpr (sizeof(Unsigned) == 1) {
      return value;
    } else if constexpr (sizeof(Unsigned) == 2) {
      return static_cast<T>(static_cast<Plain>(__builtin_bswap16(bits)));
    } else if constexpr (sizeof(Unsigned) == 4) {
      return static_cast<T>(static_cast<Plain>(__builtin_bswap32(bits)));
    } else if constexpr (sizeof(Unsigned) == 8) {
      return static_cast<T>(static_cast<Plain>(__builtin_bswap64(bits)));
    } else {
      static_assert(sizeof(Unsigned) <= 8,
                    "p_swapendian only supports 1/2/4/8-byte ordinal values");
    }
  } else {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned bits = static_cast<Unsigned>(value);
    if constexpr (sizeof(Unsigned) == 1) {
      return value;
    } else if constexpr (sizeof(Unsigned) == 2) {
      return static_cast<T>(__builtin_bswap16(bits));
    } else if constexpr (sizeof(Unsigned) == 4) {
      return static_cast<T>(__builtin_bswap32(bits));
    } else if constexpr (sizeof(Unsigned) == 8) {
      return static_cast<T>(__builtin_bswap64(bits));
    } else {
      static_assert(sizeof(Unsigned) <= 8,
                    "p_swapendian only supports 1/2/4/8-byte ordinal values");
    }
  }
}

template <typename T>
inline constexpr int tp2cc_ordinal_value(T x) {
  if constexpr (std::is_convertible_v<T, p_char>)
    return static_cast<int>(p_char_byte(static_cast<p_char>(x)));
  else
    return static_cast<int>(x);
}

// --- tp2cc_Array<T, Lo, N> -------------------------------------------------------
// Pascal `array[Lo..Hi] of T`. Value-semantics (copied on pass, like
// Pascal), arbitrary lower bound, 1- or 0-based or whatever Pascal said.
//
// We DO NOT inherit from std::array: that adds an extra aggregate layer
// which breaks brace-elision for designated initialisers of element
// records (the fpc sources' typed consts use `(field: value; ...)` a
// lot).  Holding a bare C-array as the single member keeps `tp2cc_Array` a
// simple one-member aggregate, so `tp2cc_Array<R, Lo, N> a = {{.f=1},{.f=2}};`
// initialises exactly as expected.
template <typename T, auto Lo, int N>
struct tp2cc_Array {
  // `T data[N];` -- DELIBERATELY NOT `T data[N]{};`. A default member
  // initialiser makes `std::is_trivial_v<tp2cc_Array>` false even when `T` is
  // trivial, which in turn makes GCC silently ignore `[[gnu::packed]]`
  // on any packed record containing this tp2cc_Array. Leaving the array
  // uninitialised in the raw declaration is fine because the tp2cc
  // emitter's `emit_var_decl` adds `{}` at every local declaration
  // site, static-storage globals zero-init by C++ rules, and struct
  // fields get zeroed by their enclosing aggregate's `{...}` init.
  //
  // For `T` with virtuals, plain `T data[N];` still default-inits each
  // element (C++ rule for arrays of class type with a default ctor),
  // so vtables are set up. Matches Pascal `array of TFoo` semantics.
  T data[N];

  template <typename Ix>
  constexpr T& operator[](Ix i) {
    return data[tp2cc_ordinal_value(i) - tp2cc_ordinal_value(Lo)];
  }
  template <typename Ix>
  constexpr const T& operator[](Ix i) const {
    return data[tp2cc_ordinal_value(i) - tp2cc_ordinal_value(Lo)];
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
    return static_cast<decltype(Lo)>(tp2cc_ordinal_value(Lo) + N - 1);
  }
};

template <typename T, auto Lo, int N, int M>
constexpr tp2cc_Array<T, Lo, N> tp2cc_array_literal(const tp2cc_ShortString<M>& s) {
  tp2cc_Array<T, Lo, N> out{};
  const int n = s.length < N ? s.length : N;
  for (int i = 0; i < n; ++i) {
    if constexpr (std::is_same_v<T, p_char>)
      out.data[i] = tp2cc_char_of(static_cast<uint8_t>(s.data[i]));
    else
      out.data[i] = static_cast<T>(s.data[i]);
  }
  return out;
}

template <typename T, auto Lo, int N>
constexpr tp2cc_Array<T, Lo, N> tp2cc_array_literal(p_char c) {
  tp2cc_Array<T, Lo, N> out{};
  if (N > 0) {
    if constexpr (std::is_same_v<T, p_char>)
      out.data[0] = c;
    else
      out.data[0] = static_cast<T>(p_char_to_c(c));
  }
  return out;
}

template <typename T, auto Lo, int N>
constexpr tp2cc_Array<T, Lo, N> tp2cc_array_literal(char c) {
  return tp2cc_array_literal<T, Lo, N>(tp2cc_char_of(c));
}

template <auto Lo, int ArrN, int StrN>
inline bool operator==(const tp2cc_Array<p_char, Lo, ArrN>& a,
                       const tp2cc_ShortString<StrN>& b) {
  int logical_len = ArrN;
  while (logical_len > 0 &&
         a.data[static_cast<size_t>(logical_len - 1)] == tp2cc_char_of('\0')) {
    --logical_len;
  }
  if (logical_len != b.size()) return false;
  for (int i = 0; i < logical_len; ++i) {
    if (a.data[static_cast<size_t>(i)] != b[i + 1]) return false;
  }
  return true;
}

template <int StrN, auto Lo, int ArrN>
inline bool operator==(const tp2cc_ShortString<StrN>& a,
                       const tp2cc_Array<p_char, Lo, ArrN>& b) {
  return b == a;
}

// Pascal has two distinct `array of T` forms:
//   * open arrays   -> procedure parameter view `(ptr,count)`
//   * dynamic arrays -> heap-backed value with shared ownership
// Keep those separate in the rt so the emitted C++ does not blur
// "parameter view" with "owning variable-length object".
template <typename T>
struct tp2cc_DynArray {
  std::shared_ptr<T[]> data{};
  int32_t count = 0;

  tp2cc_DynArray& operator=(std::nullptr_t) {
    data.reset();
    count = 0;
    return *this;
  }

  template <typename Ix>
  T& operator[](Ix i) {
    return data[static_cast<size_t>(tp2cc_ordinal_value(i))];
  }
  template <typename Ix>
  const T& operator[](Ix i) const {
    return data[static_cast<size_t>(tp2cc_ordinal_value(i))];
  }

  T* ptr() { return data.get(); }
  const T* ptr() const { return data.get(); }

  T* begin() { return ptr(); }
  T* end() { return ptr() + count; }
  const T* begin() const { return ptr(); }
  const T* end() const { return ptr() + count; }

  constexpr int32_t low() const { return 0; }
  constexpr int32_t high() const { return count - 1; }

  explicit operator bool() const { return data != nullptr; }
};

template <typename T>
inline bool operator==(const tp2cc_DynArray<T>& a, std::nullptr_t) {
  return a.data == nullptr;
}
template <typename T>
inline bool operator==(std::nullptr_t, const tp2cc_DynArray<T>& a) {
  return a == nullptr;
}
template <typename T>
inline bool operator!=(const tp2cc_DynArray<T>& a, std::nullptr_t) {
  return !(a == nullptr);
}
template <typename T>
inline bool operator!=(std::nullptr_t, const tp2cc_DynArray<T>& a) {
  return !(a == nullptr);
}

template <typename T>
struct tp2cc_OpenArray {
  T* data = nullptr;
  int32_t count = 0;

  constexpr T& operator[](int32_t i) { return data[i]; }
  constexpr const T& operator[](int32_t i) const { return data[i]; }

  constexpr T* begin() { return data; }
  constexpr T* end() { return data + count; }
  constexpr const T* begin() const { return data; }
  constexpr const T* end() const { return data + count; }

  constexpr int32_t low() const { return 0; }
  constexpr int32_t high() const { return count - 1; }
};

template <typename T>
constexpr tp2cc_OpenArray<T> tp2cc_open_array() {
  return {};
}

template <typename T>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(T* p, int32_t n) {
  return {p, n};
}

template <typename T, typename U, auto Lo, int N>
requires std::is_convertible_v<U*, T*>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(tp2cc_Array<U, Lo, N>& a) {
  return {a.data, N};
}

template <typename T, typename U, auto Lo, int N>
requires std::is_convertible_v<const U*, T*>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(const tp2cc_Array<U, Lo, N>& a) {
  return {a.data, N};
}

template <typename T, typename U>
requires std::is_convertible_v<U*, T*>
inline tp2cc_OpenArray<T> tp2cc_open_array(tp2cc_DynArray<U>& a) {
  return {a.ptr(), a.count};
}

template <typename T, typename U>
requires std::is_convertible_v<const U*, T*>
inline tp2cc_OpenArray<T> tp2cc_open_array(const tp2cc_DynArray<U>& a) {
  return {const_cast<U*>(a.ptr()), a.count};
}

template <typename T, int N>
requires std::is_convertible_v<p_char*, T*>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(tp2cc_ShortString<N>& s) {
  return {s.data, s.length};
}

template <typename T, int N>
requires std::is_convertible_v<const p_char*, T*>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(const tp2cc_ShortString<N>& s) {
  return {s.data, s.length};
}

// A bracketed Pascal open-array constructor (`foo([a, b, c])`) needs
// temporary storage that survives the whole call expression. This wrapper
// owns that storage and converts to `tp2cc_OpenArray<T>` by pointing at it.
template <typename T, std::size_t N>
struct tp2cc_OpenArrayValue {
  std::array<T, N> storage{};

  constexpr operator tp2cc_OpenArray<T>() {
    return tp2cc_open_array<T>(storage.data(), static_cast<int32_t>(N));
  }
  constexpr operator tp2cc_OpenArray<T>() const {
    return tp2cc_open_array<T>(const_cast<T*>(storage.data()),
                           static_cast<int32_t>(N));
  }
};

template <typename T, std::size_t N>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(tp2cc_OpenArrayValue<T, N>& value) {
  return tp2cc_open_array<T>(value.storage.data(), static_cast<int32_t>(N));
}

template <typename T, std::size_t N>
constexpr tp2cc_OpenArray<T> tp2cc_open_array(const tp2cc_OpenArrayValue<T, N>& value) {
  return tp2cc_open_array<T>(const_cast<T*>(value.storage.data()),
                         static_cast<int32_t>(N));
}

template <typename T, typename... Args>
constexpr auto tp2cc_open_array_of(Args&&... args) {
  tp2cc_OpenArrayValue<T, sizeof...(Args)> out{};
  if constexpr (sizeof...(Args) > 0) {
    std::size_t i = 0;
    ((out.storage[i++] = static_cast<T>(std::forward<Args>(args))), ...);
  }
  return out;
}

template <typename Arr>
struct ByteReinterpreter;

template <typename Elem, auto Lo, int N>
struct ByteReinterpreter<tp2cc_Array<Elem, Lo, N>> {
  template <typename Src>
  static tp2cc_Array<Elem, Lo, N> cast(const Src& src) {
    static_assert(std::is_same_v<Elem, uint8_t> ||
                      std::is_same_v<Elem, p_char>,
                  "byte reinterpretation only supports byte-sized arrays");
    tp2cc_Array<Elem, Lo, N> out{};
    const auto* raw = reinterpret_cast<const uint8_t*>(&src);
    const int bytes = static_cast<int>(
        std::min<std::size_t>(sizeof(src), sizeof(out.data)));
    for (int i = 0; i < bytes; ++i) {
      if constexpr (std::is_same_v<Elem, p_char>)
        out.data[i] = tp2cc_char_of(raw[i]);
      else
        out.data[i] = raw[i];
    }
    return out;
  }
};

// Pascal typecasts like `array[0..9] of byte(x)` reinterpret raw
// storage bytes; they are not element-wise numeric conversions.
template <typename Arr, typename Src>
inline Arr tp2cc_reinterpret_bytes(const Src& src) {
  return ByteReinterpreter<Arr>::cast(src);
}

// Pascal also uses typed casts like `double(bits)` to mean "take this
// aggregate object's bytes and interpret them as a floating-point value".
// Do that with a byte copy rather than a C++ cast so the translation stays
// defined with respect to aliasing and alignment.
template <typename T, typename Src>
inline T tp2cc_reinterpret_copy(const Src& src) {
  static_assert(std::is_trivially_copyable_v<T>,
                "byte reinterpretation target must be trivially copyable");
  static_assert(std::is_trivially_copyable_v<Src>,
                "byte reinterpretation source must be trivially copyable");
  static_assert(sizeof(T) == sizeof(Src),
                "byte reinterpretation requires equal object size");
  T out{};
  std::memcpy(&out, &src, sizeof(T));
  return out;
}

template <typename T>
inline T tp2cc_reinterpret_load(const void* p) {
  static_assert(std::is_trivially_copyable_v<T>,
                "byte reinterpretation target must be trivially copyable");
  T out{};
  std::memcpy(&out, p, sizeof(T));
  return out;
}

template <typename T>
inline void tp2cc_reinterpret_store(void* p, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>,
                "byte reinterpretation source must be trivially copyable");
  std::memcpy(p, &value, sizeof(T));
}

inline void* tp2cc_byte_offset(void* p, std::ptrdiff_t n) {
  return static_cast<void*>(static_cast<uint8_t*>(p) + n);
}

inline const void* tp2cc_byte_offset(const void* p, std::ptrdiff_t n) {
  return static_cast<const void*>(static_cast<const uint8_t*>(p) + n);
}

// View the bytes of the source object itself as a different type.
// This is the helper used for Pascal `absolute` aliases and typed lvalue
// casts, where the source object already is the storage being re-viewed.
template <typename T, typename Src>
inline T& tp2cc_reinterpret_storage_ref(Src& src) {
  return *reinterpret_cast<T*>(&src);
}
template <typename T, typename Src>
inline const T& tp2cc_reinterpret_storage_ref(const Src& src) {
  return *reinterpret_cast<const T*>(&src);
}
template <typename T>
inline T& tp2cc_reinterpret_storage_ref(tp2cc_ShortStringCharRef src) {
  return *reinterpret_cast<T*>(src.byte);
}
template <typename T>
inline const T& tp2cc_reinterpret_storage_ref(tp2cc_ShortStringCharValue src) {
  return *reinterpret_cast<const T*>(src.byte);
}

// View the storage pointed at by `src` as a different type. This is a
// different Pascal operation from tp2cc_reinterpret_storage_ref even though the
// current implementation uses the same cast sequence for non-pointer inputs.
template <typename T, typename Src>
inline T& tp2cc_reinterpret_ref(Src& src) {
  return *reinterpret_cast<T*>(&src);
}
template <typename T, typename Src>
inline const T& tp2cc_reinterpret_ref(const Src& src) {
  return *reinterpret_cast<const T*>(&src);
}
template <typename T>
inline T& tp2cc_reinterpret_ref(tp2cc_ShortStringCharRef src) {
  return *reinterpret_cast<T*>(src.byte);
}
template <typename T>
inline const T& tp2cc_reinterpret_ref(tp2cc_ShortStringCharValue src) {
  return *reinterpret_cast<const T*>(src.byte);
}
template <typename T>
inline T& tp2cc_reinterpret_ref(void* p) {
  return *reinterpret_cast<T*>(p);
}
template <typename T>
inline const T& tp2cc_reinterpret_ref(const void* p) {
  return *reinterpret_cast<const T*>(p);
}

// --- tp2cc_Set<Elem> --------------------------------------------------------------
//
// A 256-bit set, wide enough for `set of byte`, `set of char`, and every
// enum-backed Pascal set we encounter (emitted code may cast enum values to
// integers that exceed 63, so a wider mask is essential).
//
// The storage is a bare `unsigned char[32]` with alignment 1 -- intentionally,
// NOT a `uint64_t[4]`. Reason: a Pascal `packed record` maps to a C++ struct
// wrapped in `#pragma pack(push, 1)`, which places all fields at byte
// granularity. A `tp2cc_Set` member of such a record then lands at an arbitrary
// byte offset within the record (and, once placed in an array, most elements
// have the field at an address that isn't 4- or 8-aligned). Calling any
// member function on that misaligned `tp2cc_Set` -- e.g. `rec.p_flags.add(x)` --
// forms a `tp2cc_Set* this` pointer that has lost the "I came from a packed
// struct" information; inside the method the compiler assumes the pointer
// has the type's natural alignment and emits aligned loads/stores through it.
// If the internal storage were `uint64_t[4]`, those loads/stores would be
// unaligned and are UB under the C++ abstract machine. UBSan's
// `-fsanitize=alignment` catches exactly this, and on strict-aligning
// architectures it would fault outright. Using a 1-byte-aligned element
// type makes every access on `this` trivially aligned regardless of where
// the `tp2cc_Set` actually sits, so packed-record membership is safe. See the
// tp2cc codegen -- it emits `#pragma pack(push, 1)` whenever the Pascal
// source says `packed record` (e.g. `ttargetinfo` in `compiler/systems.pas`
// which has a `set of ttargetflags` field).
//
// All bit operations below are written byte-wise to preserve that property.

template <typename Elem>
struct tp2cc_Set {
  static constexpr int Nb = 32;  // 32 bytes == 256 bits.
  // No default member initialiser: keeps `is_trivial_v<tp2cc_Set>` so `tp2cc_Set`
  // can live inside a packed record without GCC dropping the packing.
  // `tp2cc_Set` helpers (`from_list`, `set_of`, the emitter's set-range
  // lambda) value-init with `tp2cc_Set s{};` before calling `add` so the
  // unset bits are zeroed; otherwise `.contains()` would return true
  // for arbitrary values.
  unsigned char bits[Nb];

  static constexpr int idx(Elem e) {
    if constexpr (std::is_same_v<Elem, p_char>)
      return static_cast<int>(p_char_byte(e));
    else
      return static_cast<int>(static_cast<int64_t>(e));
  }

  static tp2cc_Set from_list(std::initializer_list<Elem> xs) {
    // Value-init; `tp2cc_Set` has no default member initialisers, so a bare
    // `tp2cc_Set s;` would leave the bitmask uninitialised and the
    // subsequent `s.add(x)` calls would only set specific bits on top
    // of stack garbage.
    tp2cc_Set s{};
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
  friend constexpr tp2cc_Set operator+(tp2cc_Set a, tp2cc_Set b) {
    tp2cc_Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] | b.bits[i]; return r;
  }
  friend constexpr tp2cc_Set operator-(tp2cc_Set a, tp2cc_Set b) {
    tp2cc_Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] & ~b.bits[i]; return r;
  }
  friend constexpr tp2cc_Set operator*(tp2cc_Set a, tp2cc_Set b) {
    tp2cc_Set r{}; for (int i = 0; i < Nb; ++i) r.bits[i] = a.bits[i] & b.bits[i]; return r;
  }
  friend constexpr bool operator==(tp2cc_Set a, tp2cc_Set b) {
    for (int i = 0; i < Nb; ++i) if (a.bits[i] != b.bits[i]) return false;
    return true;
  }
  friend constexpr bool operator!=(tp2cc_Set a, tp2cc_Set b) { return !(a == b); }
  friend constexpr bool operator<=(tp2cc_Set a, tp2cc_Set b) {
    // subset test
    for (int i = 0; i < Nb; ++i) if ((a.bits[i] & ~b.bits[i]) != 0) return false;
    return true;
  }
  friend constexpr bool operator>=(tp2cc_Set a, tp2cc_Set b) {
    // Pascal `a >= b` on sets is a superset test.
    return b <= a;
  }
};

template <typename Elem>
tp2cc_Set<Elem> set_of(std::initializer_list<Elem> xs) {
  return tp2cc_Set<Elem>::from_list(xs);
}

// compiler.pas masks FPU exceptions around the main compile so constant
// folding does not raise stray FP traps. The actual mask read/write
// happens via tp2cc_get/set_exception_mask_bits below; the enum/set here
// just gives the source-level names that compiler.pas references.
enum p_tfpuexception : uint8_t {
  p_exinvalidop,
  p_exdenormalized,
  p_exzerodivide,
  p_exoverflow,
  p_exunderflow,
  p_exprecision,
};
using p_tfpuexceptionmask = tp2cc_Set<p_tfpuexception>;

// Pascal `Math.GetExceptionMask` / `SetExceptionMask` use the same six-bit
// exception-mask layout across targets. Keep that API in the runtime so the
// translated compiler does not need the full `Math` unit just to mask FP
// traps before constant folding.
inline p_tfpuexceptionmask p_getexceptionmask() {
  p_tfpuexceptionmask mask{};
  mask.bits[0] = static_cast<unsigned char>(tp2cc_get_exception_mask_bits() & 0x3Fu);
  return mask;
}

inline p_tfpuexceptionmask p_setexceptionmask(p_tfpuexceptionmask mask) {
  p_tfpuexceptionmask previous{};
  previous.bits[0] = static_cast<unsigned char>(
      tp2cc_set_exception_mask_bits(static_cast<uint8_t>(mask.bits[0] & 0x3Fu)));
  return previous;
}

template <typename DstSet, typename SrcElem>
constexpr DstSet tp2cc_set_cast(const tp2cc_Set<SrcElem>& src) {
  // Pascal `TDstSet(x)` is an explicit set typecast. Keep that as an
  // explicit helper rather than an implicit cross-tp2cc_Set conversion.
  DstSet dst{};
  constexpr int bytes =
      (DstSet::Nb < tp2cc_Set<SrcElem>::Nb) ? DstSet::Nb : tp2cc_Set<SrcElem>::Nb;
  for (int i = 0; i < bytes; ++i) dst.bits[i] = src.bits[i];
  return dst;
}

template <typename DstSet, typename Elem, auto Lo, int N>
constexpr DstSet tp2cc_set_cast(const tp2cc_Array<Elem, Lo, N>& src) {
  static_assert(N == DstSet::Nb,
                "set casts from raw array carriers require exactly 32 bytes");
  static_assert(sizeof(Elem) == 1,
                "set casts from raw array carriers require byte-sized elements");
  DstSet dst{};
  for (int i = 0; i < DstSet::Nb; ++i) {
    if constexpr (std::is_same_v<Elem, p_char>)
      dst.bits[i] = static_cast<unsigned char>(p_char_byte(src.data[i]));
    else
      dst.bits[i] = static_cast<unsigned char>(src.data[i]);
  }
  return dst;
}

// Pascal `Tprim(set)` packs the set's bit array into an integer:
// element i ends up at bit i of the result. Doing this by memcpy
// would inherit host endianness; spell out the bytes so the result
// is the same on big-endian hosts.
template <typename T, typename E>
constexpr T tp2cc_set_to_int(const tp2cc_Set<E>& s) {
  static_assert(std::is_integral_v<T>);
  static_assert(sizeof(T) <= tp2cc_Set<E>::Nb);
  T out = 0;
  for (size_t i = 0; i < sizeof(T); ++i)
    out |= static_cast<T>(s.bits[i]) << (i * 8);
  return out;
}

// Mixed-type variadic `set_of` -- Pascal set literals like
// `[newline, #13, '{', ';']` mix a CharConst (our wrapper for Pascal
// `const X = 'c'`) with plain char literals. A single
// `initializer_list<Elem>` can't deduce Elem across distinct argument
// types, so take them as a variadic pack and add each explicitly.
// The first argument's type drives the tp2cc_Set's element type.
namespace detail {
template <typename T> struct set_elem_type { using type = T; };
template <> struct set_elem_type<char> { using type = p_char; };
template <> struct set_elem_type<CharConst> { using type = p_char; };
}
template <typename T, typename... Rest>
inline auto set_of(T first, Rest... rest) {
  using E = typename detail::set_elem_type<T>::type;
  tp2cc_Set<E> s{};  // value-init: zero the bits[] -- see note on from_list
  s.add(static_cast<E>(first));
  (s.add(static_cast<E>(rest)), ...);
  return s;
}

// Empty set-literal sentinel. Pascal `[]` has no element type on its own
// (the type is inferred from use context). We emit it as `EmptySet{}`
// which implicitly converts to any tp2cc_Set<T>.
struct EmptySet {
  template <typename T>
  constexpr operator tp2cc_Set<T>() const { return {}; }
};
inline tp2cc_Set<int> set_of(std::initializer_list<EmptySet>) { return {}; }
inline EmptySet set_of() { return {}; }

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr tp2cc_Set<A> operator+(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  for (int i = 0; i < tp2cc_Set<A>::Nb; ++i) a.bits[i] |= b.bits[i];
  return a;
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr tp2cc_Set<A> operator-(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  for (int i = 0; i < tp2cc_Set<A>::Nb; ++i) a.bits[i] &= static_cast<unsigned char>(~b.bits[i]);
  return a;
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr tp2cc_Set<A> operator*(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  for (int i = 0; i < tp2cc_Set<A>::Nb; ++i) a.bits[i] &= b.bits[i];
  return a;
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr bool operator==(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  for (int i = 0; i < tp2cc_Set<A>::Nb; ++i) {
    if (a.bits[i] != b.bits[i]) return false;
  }
  return true;
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr bool operator!=(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  return !(a == b);
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr bool operator<=(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  for (int i = 0; i < tp2cc_Set<A>::Nb; ++i) {
    if ((a.bits[i] & static_cast<unsigned char>(~b.bits[i])) != 0) return false;
  }
  return true;
}

template <typename A, typename B>
requires (!std::is_same_v<A, B>)
constexpr bool operator>=(tp2cc_Set<A> a, tp2cc_Set<B> b) {
  return b <= a;
}

// tp2cc_Set-literal element: either a single value or a range `lo..hi`.  We
// model heterogeneous set literals with a type-erased element, then
// construct the tp2cc_Set by walking and adding each element (ranges expand).
template <typename Elem>
struct SetElem {
  bool is_range = false;
  Elem lo{}, hi{};
};

template <typename Elem>
tp2cc_Set<Elem> set_of_range(std::initializer_list<SetElem<Elem>> xs) {
  tp2cc_Set<Elem> s{};  // value-init; see note on tp2cc_Set::from_list
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

struct tp2cc_TextFile {
  std::FILE* f = nullptr;
  // `{}` needed because `tp2cc_ShortString` no longer carries a default
  // member initialiser on its own fields (would disqualify it from
  // `is_trivial` and break its use as a packed-record member).
  tp2cc_ShortString<> name{};
  int32_t iores = 0;  // last IOResult
};

// Pascal `file of T` - minimal stub; behaviour added as needed.
template <typename T>
struct tp2cc_TypedFile {
  std::FILE* f = nullptr;
  tp2cc_ShortString<> name{};
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

template <int N> inline int p_length(const tp2cc_ShortString<N>& s) { return s.length; }
template <int N> inline int p_length(const tp2cc_ShortStringPtrValue<N>& s) {
  return s.size();
}
template <int N> inline int p_length(const tp2cc_ShortStringPtrRef<N>& s) {
  return s.size();
}
inline int p_length(const tp2cc_AnsiString& s) { return s.length(); }
template <typename T> inline int p_length(const tp2cc_DynArray<T>& a) { return a.count; }
template <typename T> inline int p_length(const tp2cc_OpenArray<T>& a) { return a.count; }
template <typename T> inline int p_length(const std::array<T, 0>&) { return 0; }

template <int N, typename Src>
inline void tp2cc_shortstring_assign(tp2cc_ShortString<N>& dest, const Src& src) {
  const int32_t n = std::min<int32_t>(p_string_length(src), N);
  dest.length = static_cast<uint8_t>(n);
  if (n > 0) {
    std::memmove(dest.data, p_string_bytes(src),
                 static_cast<size_t>(n) * sizeof(p_char));
  }
}

template <int N, typename Src>
inline void tp2cc_shortstring_assign(tp2cc_ShortStringPtrRef<N> dest, const Src& src) {
  const int32_t n = std::min<int32_t>(p_string_length(src), N);
  *dest.storage = static_cast<uint8_t>(n);
  if (n > 0) {
    std::memmove(dest.bytes(), p_string_bytes(src),
                 static_cast<size_t>(n) * sizeof(p_char));
  }
}

template <int N>
inline void p_setlength(tp2cc_ShortString<N>& s, int new_len) {
  if (new_len < 0) new_len = 0;
  if (new_len > N) new_len = N;
  for (int i = s.length; i < new_len; ++i) {
    s.data[i] = tp2cc_char_of('\0');
  }
  s.length = static_cast<uint8_t>(new_len);
}

inline void p_setlength(tp2cc_AnsiString& s, int new_len) {
  s.set_length(new_len);
}

template <typename T>
inline void p_setlength(tp2cc_DynArray<T>& a, int new_len) {
  if (new_len <= 0) {
    a = nullptr;
    return;
  }

  auto new_data = std::shared_ptr<T[]>(
      new T[static_cast<size_t>(new_len)](), std::default_delete<T[]>());
  const int32_t copy_count = std::min(a.count, static_cast<int32_t>(new_len));
  for (int32_t i = 0; i < copy_count; ++i) {
    new_data[static_cast<size_t>(i)] = a.data[static_cast<size_t>(i)];
  }
  a.data = std::move(new_data);
  a.count = new_len;
}

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
inline constexpr p_char p_chr(int x) { return tp2cc_char_of(static_cast<uint8_t>(x)); }

// Pascal `Lo` / `Hi` return the lower / upper half of an ordinal value's
// storage width: 32-bit -> 16-bit halves, 64-bit -> 32-bit halves, etc.
template <typename T>
inline constexpr auto p_lo(T value) {
  using U = std::make_unsigned_t<T>;
  if constexpr (sizeof(U) == 8) {
    return static_cast<uint32_t>(static_cast<U>(value) & 0xffffffffu);
  } else if constexpr (sizeof(U) == 4) {
    return static_cast<uint16_t>(static_cast<U>(value) & 0xffffu);
  } else if constexpr (sizeof(U) == 2) {
    return static_cast<uint8_t>(static_cast<U>(value) & 0xffu);
  } else {
    return static_cast<U>(value);
  }
}

template <typename T>
inline constexpr auto p_hi(T value) {
  using U = std::make_unsigned_t<T>;
  if constexpr (sizeof(U) == 8) {
    return static_cast<uint32_t>(static_cast<U>(value) >> 32);
  } else if constexpr (sizeof(U) == 4) {
    return static_cast<uint16_t>(static_cast<U>(value) >> 16);
  } else if constexpr (sizeof(U) == 2) {
    return static_cast<uint8_t>(static_cast<U>(value) >> 8);
  } else {
    return static_cast<U>(0);
  }
}

// Pascal `swap` -- byte-swap the two halves of a word or longint.
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

// Pascal `ptr^` becomes `tp2cc_deref(ptr)`. For typed pointers this is just
// `*ptr`. For untyped (`pointer` -> `void*`), expose the first byte so
// code that writes through the deref still compiles; the translated units
// using this pattern (settextbuf buffers, heap-trace hooks) only touch
// these values behind stubbed helpers.
template <typename T> inline T& tp2cc_deref(T* p) { return *p; }
template <int N> inline tp2cc_ShortStringPtrRef<N> tp2cc_deref(tp2cc_ShortString<N>* p) {
  return {reinterpret_cast<uint8_t*>(p)};
}
template <int N>
inline tp2cc_ShortStringPtrValue<N> tp2cc_deref(const tp2cc_ShortString<N>* p) {
  return {reinterpret_cast<const uint8_t*>(p)};
}
inline char& tp2cc_deref(void* p) { return *static_cast<char*>(p); }
inline const char& tp2cc_deref(const void* p) { return *static_cast<const char*>(p); }

template <typename T> inline bool p_assigned(T* p) { return p != nullptr; }
template <typename T> inline bool p_assigned(const tp2cc_DynArray<T>& a) {
  return a.data != nullptr;
}
template <typename Sig> inline bool p_assigned(const tp2cc_MethodPtr<Sig>& p) {
  return static_cast<bool>(p);
}
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
inline void p_inc(tp2cc_ShortStringCharRef x) { ++(*x.byte); }
template <typename T, typename N> inline void p_inc(T& x, N n) {
  if constexpr (std::is_pointer_v<T>) x += n;
  else x = static_cast<T>(static_cast<int64_t>(x) + n);
}
template <typename N>
inline void p_inc(tp2cc_ShortStringCharRef x, N n) {
  *x.byte = static_cast<uint8_t>(static_cast<int64_t>(*x.byte) + n);
}
template <typename T> inline void p_dec(T& x) {
  if constexpr (std::is_enum_v<T>)
    x = static_cast<T>(static_cast<int64_t>(x) - 1);
  else --x;
}
inline void p_dec(tp2cc_ShortStringCharRef x) { --(*x.byte); }
template <typename T, typename N> inline void p_dec(T& x, N n) {
  if constexpr (std::is_pointer_v<T>) x -= n;
  else x = static_cast<T>(static_cast<int64_t>(x) - n);
}
template <typename N>
inline void p_dec(tp2cc_ShortStringCharRef x, N n) {
  *x.byte = static_cast<uint8_t>(static_cast<int64_t>(*x.byte) - n);
}

template <typename T> inline void tp2cc_reinterpret_inc(void* p) {
  T x = tp2cc_reinterpret_load<T>(p);
  p_inc(x);
  tp2cc_reinterpret_store<T>(p, x);
}
template <typename T, typename N> inline void tp2cc_reinterpret_inc(void* p, N n) {
  T x = tp2cc_reinterpret_load<T>(p);
  p_inc(x, n);
  tp2cc_reinterpret_store<T>(p, x);
}
template <typename T> inline void tp2cc_reinterpret_dec(void* p) {
  T x = tp2cc_reinterpret_load<T>(p);
  p_dec(x);
  tp2cc_reinterpret_store<T>(p, x);
}
template <typename T, typename N> inline void tp2cc_reinterpret_dec(void* p, N n) {
  T x = tp2cc_reinterpret_load<T>(p);
  p_dec(x, n);
  tp2cc_reinterpret_store<T>(p, x);
}

// No rvalue `p_inc`/`p_dec` overloads here: typed-storage casted lvalues like
// `inc(longint(p))` on a real pointer slot are emitted as
// `p_inc(tp2cc_reinterpret_storage_ref<int32_t>(p))`. Untyped-storage byte views
// use `tp2cc_reinterpret_inc` / `tp2cc_reinterpret_dec` above instead of manufacturing
// a potentially misaligned C++ reference.

// --- Missing small RTL procedures ------------------------------------------

inline int32_t p_memavail() { return 1 << 30; }   // stub: "lots of memory"
//inline int32_t p_heapavail() { return 1 << 30; }
//inline int32_t p_maxavail()  { return 1 << 30; }

inline int32_t p_last_ioresult = 0;

template <typename File>
inline void p_set_ioresult(File& f, int32_t code) {
  f.iores = code;
  p_last_ioresult = code;
}

inline tp2cc_ShortString<> p_file_name_to_string(const tp2cc_ShortString<>& name) {
  return name;
}

template <typename File>
inline void p_file_name_to_buf(const File& f, char (&buf)[260]) {
  int n = f.name.length < 255 ? f.name.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = p_char_to_c(f.name.data[i]);
  buf[n] = '\0';
}

template <int N>
inline std::string p_to_std_string(const tp2cc_ShortString<N>& s) {
  std::string out;
  out.reserve(s.length);
  for (int i = 0; i < s.length; ++i) out.push_back(p_char_to_c(s.data[i]));
  return out;
}

inline std::string p_to_std_string(const tp2cc_AnsiString& s) {
  std::string out;
  out.reserve(static_cast<size_t>(s.length()));
  for (int i = 0; i < s.length(); ++i) out.push_back(p_char_to_c(s.data[i]));
  return out;
}

inline std::string p_to_std_string(const char* s) {
  return s ? std::string(s) : std::string();
}
inline std::string p_to_std_string(const p_char* s) {
  return s ? std::string(p_c_str(s)) : std::string();
}

inline int32_t p_strtoint(const tp2cc_ShortString<>& s) {
  char buf[260]{};
  for (int i = 0; i < s.length; ++i) buf[i] = p_char_to_c(s.data[i]);
  return std::atoi(buf);
}

inline int32_t p_strtoint(const tp2cc_AnsiString& s) {
  return std::atoi(p_to_std_string(s).c_str());
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
      case '&':
        base = 8;
        ++i;
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
                   uint8_t p_attr = 0; tp2cc_ShortString<> p_name{};
                   std::vector<std::string> p_matches;
                   std::size_t p_index = 0; };
using p_searchrec = SearchRec;
using p_tsearchrec = SearchRec;
inline constexpr int32_t p_fadirectory = 0x10;
inline constexpr int32_t p_faarchive = 0x20;
inline void p_searchrec_fill(SearchRec& rec, const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return;
  rec.p_time = static_cast<int32_t>(st.st_mtime);
  rec.p_size = static_cast<int32_t>(st.st_size > INT32_MAX ? INT32_MAX : st.st_size);
  rec.p_attr = 0;
  if (S_ISDIR(st.st_mode)) rec.p_attr |= 0x10;
  std::size_t sep = path.find_last_of("/\\");
  rec.p_name = tp2cc_shortstring_of<>((sep == std::string::npos ? path : path.substr(sep + 1)).c_str());
}
inline int32_t p_findfirst(const tp2cc_ShortString<>& pattern, int attrs, SearchRec& rec) {
  rec.p_matches.clear();
  rec.p_index = 0;
  rec.p_attr = 0;
  rec.p_name = tp2cc_shortstring_of<>("");
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
inline void p_getfattr(tp2cc_TextFile& f, uint16_t& attr) {
  struct stat st{};
  if (::stat(p_to_std_string(f.name).c_str(), &st) != 0) {
    p_doserror = errno;
    attr = 0;
    return;
  }
  attr = static_cast<uint16_t>(S_ISDIR(st.st_mode) ? 0x10 : 0);
  p_doserror = 0;
}
template <typename T>
inline void p_getfattr(tp2cc_TypedFile<T>& f, uint16_t& attr) {
  struct stat st{};
  if (::stat(p_to_std_string(f.name).c_str(), &st) != 0) {
    p_doserror = errno;
    attr = 0;
    return;
  }
  attr = static_cast<uint16_t>(S_ISDIR(st.st_mode) ? 0x10 : 0);
  p_doserror = 0;
}
inline void p_mkdir(const tp2cc_ShortString<>& path) {
  p_last_ioresult = ::mkdir(p_to_std_string(path).c_str(), 0777) == 0 ? 0 : 5;
}
inline void p_rmdir(const tp2cc_ShortString<>& path) {
  p_last_ioresult = ::rmdir(p_to_std_string(path).c_str()) == 0 ? 0 : 5;
}
inline void p_chdir(const tp2cc_ShortString<>& path) {
  p_last_ioresult = ::chdir(p_to_std_string(path).c_str()) == 0 ? 0 : 3;
}
template <int N>
inline void p_getdir(int, tp2cc_ShortString<N>& out) {
  char buf[PATH_MAX > 0 ? PATH_MAX : 4096]{};
  if (::getcwd(buf, sizeof(buf)) == nullptr) out = tp2cc_shortstring_of<N>("");
  else out = tp2cc_shortstring_of<N>(buf);
}
inline void p_getdir(int, tp2cc_AnsiString& out) {
  char buf[PATH_MAX > 0 ? PATH_MAX : 4096]{};
  if (::getcwd(buf, sizeof(buf)) == nullptr) out = nullptr;
  else out = buf;  // AnsiString::operator=(const char*)
}
inline void p_erase(const tp2cc_ShortString<>& path) {
  p_last_ioresult = std::remove(p_to_std_string(path).c_str()) == 0 ? 0 : 2;
}
inline void p_erase(tp2cc_TextFile& f) {      // `erase(f)` after assign(f, name)
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  p_set_ioresult(f, std::remove(buf) == 0 ? 0 : 2);
}
inline void p_rename(const tp2cc_ShortString<>& old_name, const tp2cc_ShortString<>& new_name) {
  p_last_ioresult =
      std::rename(p_to_std_string(old_name).c_str(), p_to_std_string(new_name).c_str()) == 0 ? 0 : 5;
}
inline void p_rename(tp2cc_TextFile& f, const tp2cc_ShortString<>& new_name) {
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  p_set_ioresult(f, std::rename(buf, p_to_std_string(new_name).c_str()) == 0 ? 0 : 5);
}
inline tp2cc_ShortString<> p_fsearch(const tp2cc_ShortString<>& name, const tp2cc_ShortString<>&) { return name; }
inline void p_fsplit(const tp2cc_ShortString<>& input, tp2cc_ShortString<>& dir,
                     tp2cc_ShortString<>& name, tp2cc_ShortString<>& ext) {
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

  dir = tp2cc_shortstring_of<>(dir_part.c_str());
  name = tp2cc_shortstring_of<>(name_part.c_str());
  ext = tp2cc_shortstring_of<>(ext_part.c_str());
}
inline void tp2cc_split_path(const std::string& input, std::string& dir,
                             std::string& name, std::string& ext) {
  std::string path = input;
  std::string prefix;
  if (path.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    prefix = path.substr(0, 2);
    path.erase(0, 2);
  }

  size_t last_sep = path.find_last_of("/\\");
  std::string leaf = path;
  dir = prefix;
  if (last_sep != std::string::npos) {
    dir += path.substr(0, last_sep + 1);
    leaf = path.substr(last_sep + 1);
  }

  size_t dot = leaf.find_last_of('.');
  if (dot != std::string::npos && dot != 0) {
    name = leaf.substr(0, dot);
    ext = leaf.substr(dot);
  } else {
    name = leaf;
    ext.clear();
  }
}

template <typename Str>
inline tp2cc_AnsiString p_extractfilepath(const Str& input) {
  std::string dir, name, ext;
  tp2cc_split_path(p_to_std_string(input), dir, name, ext);
  return tp2cc_ansistring_of(dir.c_str());
}

template <typename Str>
inline tp2cc_AnsiString p_extractfiledir(const Str& input) {
  std::string dir, name, ext;
  tp2cc_split_path(p_to_std_string(input), dir, name, ext);
  // ExtractFilePath returns "/tmp/", ExtractFileDir returns "/tmp" --
  // same path, no trailing separator. Root ("/") keeps its slash so
  // "ExtractFileDir('/foo.txt')" still resolves to "/".
  if (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) {
    dir.pop_back();
  }
  return tp2cc_ansistring_of(dir.c_str());
}

template <typename Str>
inline tp2cc_AnsiString p_extractfilename(const Str& input) {
  std::string dir, name, ext;
  tp2cc_split_path(p_to_std_string(input), dir, name, ext);
  return tp2cc_ansistring_of((name + ext).c_str());
}

template <typename Str>
inline tp2cc_AnsiString p_extractfileext(const Str& input) {
  std::string dir, name, ext;
  tp2cc_split_path(p_to_std_string(input), dir, name, ext);
  return tp2cc_ansistring_of(ext.c_str());
}

template <typename Path, typename Ext>
inline tp2cc_AnsiString p_changefileext(const Path& input, const Ext& ext) {
  std::string dir, name, old_ext;
  tp2cc_split_path(p_to_std_string(input), dir, name, old_ext);
  return tp2cc_ansistring_of((dir + name + p_to_std_string(ext)).c_str());
}

template <typename Str>
inline tp2cc_AnsiString p_expandfilename(const Str& input) {
  return tp2cc_ansistring_of(p_to_std_string(p_fexpand(tp2cc_shortstring_of<>(p_to_std_string(input).c_str()))).c_str());
}

template <typename Str>
inline tp2cc_AnsiString p_getenvironmentvariable(const Str& name) {
  std::string key = p_to_std_string(name);
  const char* value = std::getenv(key.c_str());
  return tp2cc_ansistring_of(value ? value : "");
}

template <typename Str>
inline bool p_deletefile(const Str& name) {
  return std::remove(p_to_std_string(name).c_str()) == 0;
}

template <typename Str>
inline bool p_fileexists(const Str& name) {
  struct stat st{};
  return ::stat(p_to_std_string(name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

template <typename Str>
inline bool p_directoryexists(const Str& name) {
  struct stat st{};
  return ::stat(p_to_std_string(name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

template <typename OldName, typename NewName>
inline bool p_renamefile(const OldName& old_name, const NewName& new_name) {
  return std::rename(p_to_std_string(old_name).c_str(),
                     p_to_std_string(new_name).c_str()) == 0;
}

// FExpand: produce a fully-qualified, lexically normalised form of
// `path`.  This is a string operation: GetDir is read once for the
// cwd, and otherwise the filesystem is not touched.  Non-existent
// paths round-trip unchanged; symlinks are NOT resolved.  Matches
// the semantic of Kylix/Delphi SysUtils.ExpandFileName, GNU Pascal
// FExpand, and FPC rtl/inc/fexpand.inc: the point is to produce a
// qualified name the caller can feed to Assign/Rewrite before the
// target necessarily exists.  Callers that need symlink resolution
// should use a separate routine (realpath(3), not FExpand).
//
// Rules:
//  - Empty input returns the current working directory.
//  - Leading `~/` or bare `~` expands to $HOME (matches FPC's
//    FPC_FEXPAND_TILDE on Linux).  If $HOME is unset or empty, the
//    `~` is left in place.
//  - Relative inputs are resolved against GetDir(0).
//  - `.` components are dropped; `..` pops the previous component;
//    `..` at the root is a no-op.
//  - Result is capped to 255 chars by the tp2cc_ShortString carrier size
//    (silent truncation, Pascal shortstring semantics).
inline tp2cc_ShortString<> p_fexpand(const tp2cc_ShortString<>& s) {
  std::string in = p_to_std_string(s);

  // Read cwd up-front.  getcwd(3) with a too-small buffer returns
  // NULL and sets errno=ERANGE; grow and retry.  Cap the growth so a
  // broken cwd doesn't blow up memory.
  std::string cwd;
  for (size_t sz = 4096; sz <= (1u << 20); sz *= 2) {
    std::string buf(sz, '\0');
    if (::getcwd(buf.data(), buf.size())) {
      cwd.assign(buf.c_str());
      break;
    }
    if (errno != ERANGE) break;
  }

  if (in.empty()) return tp2cc_shortstring_of<>(cwd.c_str());

  std::string path = in;

  // Tilde expansion.
  if (path[0] == '~' && (path.size() == 1 || path[1] == '/')) {
    if (const char* home = std::getenv("HOME"); home && *home) {
      std::string h = home;
      while (h.size() > 1 && h.back() == '/') h.pop_back();
      path = h + path.substr(1);
    }
  }

  // Make absolute if still relative after tilde expansion.
  if (path.empty() || path[0] != '/') {
    std::string base = cwd.empty() ? std::string("/") : cwd;
    if (base.back() != '/') base += '/';
    path = base + path;
  }

  // Lexical collapse of `.` and `..` components.  Everything between
  // `/`s is a component; empty components (from `//`) are ignored.
  std::vector<std::string> comps;
  for (size_t i = 0; i < path.size(); ) {
    while (i < path.size() && path[i] == '/') ++i;
    size_t j = i;
    while (j < path.size() && path[j] != '/') ++j;
    if (j > i) {
      std::string c = path.substr(i, j - i);
      if (c == ".") {
        // drop
      } else if (c == "..") {
        if (!comps.empty()) comps.pop_back();
      } else {
        comps.push_back(std::move(c));
      }
    }
    i = j;
  }

  std::string out = "/";
  for (size_t k = 0; k < comps.size(); ++k) {
    if (k > 0) out += '/';
    out += comps[k];
  }

  return tp2cc_shortstring_of<>(out.c_str());
}

inline constexpr int64_t tp2cc_tdatetime_unix_epoch_days = 25569;

inline int64_t tp2cc_days_from_civil(int32_t year, uint32_t month, uint32_t day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(year - era * 400);
  const uint32_t doy = (153 * (month + (month > 2 ? static_cast<uint32_t>(-3) : 9)) + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline void tp2cc_civil_from_days(int64_t days, int32_t& year, uint32_t& month,
                                  uint32_t& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(days - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int32_t>(yoe) + static_cast<int32_t>(era) * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2;
}

inline p_tdatetime tp2cc_make_tdatetime(int32_t year, uint32_t month,
                                        uint32_t day, uint32_t hour = 0,
                                        uint32_t minute = 0,
                                        uint32_t second = 0,
                                        uint32_t millisecond = 0) {
  const int64_t days =
      tp2cc_days_from_civil(year, month, day) + tp2cc_tdatetime_unix_epoch_days;
  const uint64_t msec =
      ((static_cast<uint64_t>(hour) * 60 + minute) * 60 + second) * 1000 + millisecond;
  return static_cast<p_tdatetime>(days) +
         static_cast<p_tdatetime>(msec) / 86400000.0;
}

inline void tp2cc_decode_tdatetime(p_tdatetime value, int32_t& year,
                                   uint32_t& month, uint32_t& day,
                                   uint32_t& hour, uint32_t& minute,
                                   uint32_t& second, uint32_t& millisecond) {
  double day_part = std::floor(value);
  double frac = value - day_part;
  if (frac < 0) {
    frac += 1.0;
    day_part -= 1.0;
  }
  int64_t days = static_cast<int64_t>(day_part) - tp2cc_tdatetime_unix_epoch_days;
  tp2cc_civil_from_days(days, year, month, day);

  uint64_t total_msec = static_cast<uint64_t>(std::llround(frac * 86400000.0));
  if (total_msec >= 86400000ull) {
    total_msec = 0;
    tp2cc_civil_from_days(days + 1, year, month, day);
  }
  hour = static_cast<uint32_t>(total_msec / 3600000ull);
  total_msec %= 3600000ull;
  minute = static_cast<uint32_t>(total_msec / 60000ull);
  total_msec %= 60000ull;
  second = static_cast<uint32_t>(total_msec / 1000ull);
  millisecond = static_cast<uint32_t>(total_msec % 1000ull);
}

inline p_tdatetime p_filedatetodatetime(int32_t filedate) {
  const uint32_t sec2 = static_cast<uint32_t>(filedate & 31u);
  const uint32_t minute = static_cast<uint32_t>((filedate >> 5) & 63u);
  const uint32_t hour = static_cast<uint32_t>((filedate >> 11) & 31u);
  const uint32_t day = static_cast<uint32_t>((filedate >> 16) & 31u);
  const uint32_t month = static_cast<uint32_t>((filedate >> 21) & 15u);
  const uint32_t year = static_cast<uint32_t>(((filedate >> 25) & 127u) + 1980u);
  return tp2cc_make_tdatetime(static_cast<int32_t>(year), month, day, hour,
                              minute, sec2 * 2u, 0);
}

inline void p_decodedate(p_tdatetime value, uint16_t& year, uint16_t& month,
                         uint16_t& day) {
  int32_t y = 0;
  uint32_t m = 0, d = 0, hh = 0, mm = 0, ss = 0, ms = 0;
  tp2cc_decode_tdatetime(value, y, m, d, hh, mm, ss, ms);
  year = static_cast<uint16_t>(y);
  month = static_cast<uint16_t>(m);
  day = static_cast<uint16_t>(d);
}

inline void p_decodetime(p_tdatetime value, uint16_t& hour, uint16_t& minute,
                         uint16_t& second, uint16_t& msec) {
  int32_t y = 0;
  uint32_t m = 0, d = 0, hh = 0, mm = 0, ss = 0, ms = 0;
  tp2cc_decode_tdatetime(value, y, m, d, hh, mm, ss, ms);
  hour = static_cast<uint16_t>(hh);
  minute = static_cast<uint16_t>(mm);
  second = static_cast<uint16_t>(ss);
  msec = static_cast<uint16_t>(ms);
}

inline p_tdatetime p_date() {
  std::time_t now = std::time(nullptr);
  std::tm lt{};
  ::localtime_r(&now, &lt);
  return tp2cc_make_tdatetime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

inline p_tdatetime p_time() {
  struct timeval tv{};
  ::gettimeofday(&tv, nullptr);
  std::tm lt{};
  std::time_t now = static_cast<std::time_t>(tv.tv_sec);
  ::localtime_r(&now, &lt);
  return static_cast<p_tdatetime>(
      ((lt.tm_hour * 60 + lt.tm_min) * 60 + lt.tm_sec) * 1000 + tv.tv_usec / 1000) /
         86400000.0;
}

inline void p_getlocaltime(p_tsystemtime& out) {
  struct timeval tv{};
  ::gettimeofday(&tv, nullptr);
  std::time_t now = static_cast<std::time_t>(tv.tv_sec);
  std::tm lt{};
  if (!::localtime_r(&now, &lt)) {
    out = {};
    return;
  }
  out.p_year = static_cast<uint16_t>(lt.tm_year + 1900);
  out.p_month = static_cast<uint16_t>(lt.tm_mon + 1);
  out.p_dayofweek = static_cast<uint16_t>(lt.tm_wday);
  out.p_day = static_cast<uint16_t>(lt.tm_mday);
  out.p_hour = static_cast<uint16_t>(lt.tm_hour);
  out.p_minute = static_cast<uint16_t>(lt.tm_min);
  out.p_second = static_cast<uint16_t>(lt.tm_sec);
  out.p_milliseconds = static_cast<uint16_t>(tv.tv_usec / 1000);
}

inline int32_t tp2cc_stat_to_filedatetime(const struct stat& st) {
  std::tm lt{};
  std::time_t when = st.st_mtime;
  if (!::localtime_r(&when, &lt)) return -1;
  int32_t packed = ((lt.tm_year + 1900 - 1980) & 127);
  packed = (packed << 4) | ((lt.tm_mon + 1) & 15);
  packed = (packed << 5) | (lt.tm_mday & 31);
  packed = (packed << 5) | (lt.tm_hour & 31);
  packed = (packed << 6) | (lt.tm_min & 63);
  packed = (packed << 5) | ((lt.tm_sec / 2) & 31);
  return packed;
}

inline int32_t p_filegetdate(p_thandle handle) {
  struct stat st{};
  return ::fstat(handle, &st) == 0 ? tp2cc_stat_to_filedatetime(st) : -1;
}

inline int32_t p_filesetdate(p_thandle handle, int32_t age) {
  std::tm tmv{};
  tmv.tm_year = static_cast<int>(((age >> 25) & 127) + 80);
  tmv.tm_mon = static_cast<int>(((age >> 21) & 15) - 1);
  tmv.tm_mday = static_cast<int>((age >> 16) & 31);
  tmv.tm_hour = static_cast<int>((age >> 11) & 31);
  tmv.tm_min = static_cast<int>((age >> 5) & 63);
  tmv.tm_sec = static_cast<int>(age & 31) * 2;
  std::time_t when = std::mktime(&tmv);
  if (when == static_cast<std::time_t>(-1)) return -1;
  timespec ts[2]{};
  ts[0].tv_sec = when;
  ts[1].tv_sec = when;
  return ::futimens(handle, ts) == 0 ? 0 : -1;
}

template <typename Str>
inline int32_t p_fileage(const Str& name) {
  struct stat st{};
  return ::stat(p_to_std_string(name).c_str(), &st) == 0
             ? tp2cc_stat_to_filedatetime(st)
             : -1;
}

inline p_thandle p_getfilehandle(tp2cc_TextFile& f) {
  return f.f ? ::fileno(f.f) : -1;
}

template <typename T>
inline p_thandle p_getfilehandle(tp2cc_TypedFile<T>& f) {
  return f.f ? ::fileno(f.f) : -1;
}

template <typename Path, typename Cmd>
inline int32_t p_executeprocess(const Path& path, const Cmd& command_line) {
  std::vector<std::string> args;
  std::string exe = p_to_std_string(path);
  if (!exe.empty()) {
    args.push_back(exe);
  }
  auto rest = p_split_commandline(p_to_std_string(command_line));
  args.insert(args.end(), rest.begin(), rest.end());
  p_spawn_process(args);
  return p_last_dosexitcode;
}

inline int32_t p_executeprocess(const tp2cc_AnsiString& path,
                                const tp2cc_OpenArray<tp2cc_AnsiString>& args) {
  std::vector<std::string> argv;
  std::string exe = p_to_std_string(path);
  if (!exe.empty()) argv.push_back(exe);
  for (const auto& arg : args) argv.push_back(p_to_std_string(arg));
  p_spawn_process(argv);
  return p_last_dosexitcode;
}

inline tp2cc_AnsiString p_stringofchar(p_char c, p_sizeint count) {
  tp2cc_AnsiString out{};
  if (count <= 0) return out;
  out.set_length(count);
  p_char* bytes = out.mutable_bytes();
  for (int32_t i = 0; i < count; ++i) bytes[i] = c;
  return out;
}

template <typename A, typename B>
inline int32_t p_ansicomparefilename(const A& lhs, const B& rhs) {
  auto normalize = [](std::string s) {
    for (char& c : s) {
      if (c == '\\') c = '/';
    }
    return s;
  };
  std::string a = normalize(p_to_std_string(lhs));
  std::string b = normalize(p_to_std_string(rhs));
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

// `EpochToLocal(epoch, var year,month,day,hour,minute,second)`
// breaks a Unix epoch second-count into local-time calendar fields.
// Out-of-range epochs (libc returning nullptr) zero every field
// rather than leaving them undefined.
inline void p_epochtolocal(int32_t epoch, uint16_t& year, uint16_t& month,
                           uint16_t& day, uint16_t& hour, uint16_t& minute,
                           uint16_t& second) {
  std::time_t t = static_cast<std::time_t>(epoch);
  std::tm lt{};
  if (!::localtime_r(&t, &lt)) {
    year = month = day = hour = minute = second = 0;
    return;
  }
  year   = static_cast<uint16_t>(lt.tm_year + 1900);
  month  = static_cast<uint16_t>(lt.tm_mon + 1);
  day    = static_cast<uint16_t>(lt.tm_mday);
  hour   = static_cast<uint16_t>(lt.tm_hour);
  minute = static_cast<uint16_t>(lt.tm_min);
  second = static_cast<uint16_t>(lt.tm_sec);
}

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
//inline void p_truncate(tp2cc_TextFile&) {}
inline void p_flush(const tp2cc_TextFile& f) {
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
template <typename File, typename Count>
inline void p_blockread(File& f, tp2cc_ShortStringCharRef value, int32_t count,
                        Count& transferred) {
  p_blockread(f, static_cast<void*>(value.byte), count, transferred);
}
template <typename File, typename Count>
inline void p_blockread(File& f, tp2cc_AnsiStringCharRef value, int32_t count,
                        Count& transferred) {
  p_blockread(f, static_cast<void*>(value.owner->mutable_bytes() + value.index),
              count, transferred);
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
inline void p_truncate(File& f) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  long pos = std::ftell(f.f);
  if (pos < 0) {
    p_set_ioresult(f, 103);
    return;
  }
  std::fflush(f.f);
  int rc = ::ftruncate(::fileno(f.f), static_cast<off_t>(pos));
  p_set_ioresult(f, rc == 0 ? 0 : 103);
}

template <typename File, typename Count>
inline void p_blockwrite(File& f, const void* value, int32_t count,
                         Count& transferred) {
  if (!f.f) {
    transferred = static_cast<Count>(0);
    p_set_ioresult(f, 103);
    return;
  }
  transferred = static_cast<Count>(std::fwrite(value, 1, count, f.f));
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}

template <typename File, typename T, typename Count>
inline void p_blockwrite(File& f, const T& value, int32_t count,
                         Count& transferred) {
  if (!f.f) {
    transferred = static_cast<Count>(0);
    p_set_ioresult(f, 103);
    return;
  }
  transferred = static_cast<Count>(
      std::fwrite(static_cast<const void*>(std::addressof(value)), 1, count,
                  f.f));
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
template <typename File, typename Count>
inline void p_blockwrite(File& f, tp2cc_ShortStringCharRef value, int32_t count,
                         Count& transferred) {
  p_blockwrite(f, static_cast<const void*>(value.byte), count, transferred);
}
template <typename File, typename Count>
inline void p_blockwrite(File& f, tp2cc_ShortStringCharValue value, int32_t count,
                         Count& transferred) {
  p_blockwrite(f, static_cast<const void*>(value.byte), count, transferred);
}
template <typename File, typename Count>
inline void p_blockwrite(File& f, tp2cc_AnsiStringCharRef value, int32_t count,
                         Count& transferred) {
  p_blockwrite(f,
               static_cast<const void*>(value.owner->data + value.index),
               count, transferred);
}
template <typename File, typename Count>
inline void p_blockwrite(File& f, tp2cc_AnsiStringCharValue value, int32_t count,
                         Count& transferred) {
  p_blockwrite(f, static_cast<const void*>(value.byte), count, transferred);
}

template <typename File>
inline void p_blockwrite(File& f, const void* value, int32_t count) {
  int32_t transferred = 0;
  p_blockwrite(f, value, count, transferred);
}
template <typename File, typename T>
inline void p_blockwrite(File& f, const T& value, int32_t count) {
  int32_t transferred = 0;
  p_blockwrite(f, value, count, transferred);
}

inline void p_fillword(void* dest, int32_t count, uint16_t value) {
  auto* p = static_cast<uint16_t*>(dest);
  for (int32_t i = 0; i < count; ++i) p[i] = value;
}
template <typename T>
requires (!std::is_pointer_v<T>)
inline void p_fillword(T& dest, int32_t count, uint16_t value) {
  p_fillword(static_cast<void*>(std::addressof(dest)), count, value);
}
inline void p_filldword(void* dest, int32_t count, uint32_t value) {
  auto* p = static_cast<uint32_t*>(dest);
  for (int32_t i = 0; i < count; ++i) p[i] = value;
}
template <typename T>
requires (!std::is_pointer_v<T>)
inline void p_filldword(T& dest, int32_t count, uint32_t value) {
  p_filldword(static_cast<void*>(std::addressof(dest)), count, value);
}

inline int32_t p_compareword(const void* a, const void* b, int32_t count) {
  auto* pa = static_cast<const uint16_t*>(a);
  auto* pb = static_cast<const uint16_t*>(b);
  for (int32_t i = 0; i < count; ++i) {
    if (pa[i] < pb[i]) return -1;
    if (pa[i] > pb[i]) return 1;
  }
  return 0;
}
template <typename A, typename B>
inline int32_t p_compareword(const A& a, const B& b, int32_t count) {
  return p_compareword(static_cast<const void*>(std::addressof(a)),
                       static_cast<const void*>(std::addressof(b)), count);
}
inline int32_t p_comparebyte(const void* a, const void* b, int32_t count) {
  auto* pa = static_cast<const uint8_t*>(a);
  auto* pb = static_cast<const uint8_t*>(b);
  for (int32_t i = 0; i < count; ++i) {
    if (pa[i] < pb[i]) return -1;
    if (pa[i] > pb[i]) return 1;
  }
  return 0;
}
template <typename A, typename B>
inline int32_t p_comparebyte(const A& a, const B& b, int32_t count) {
  return p_comparebyte(static_cast<const void*>(std::addressof(a)),
                       static_cast<const void*>(std::addressof(b)), count);
}
inline int32_t p_comparechar(const void* a, const void* b, int32_t count) {
  return p_comparebyte(a, b, count);
}
template <typename A, typename B>
inline int32_t p_comparechar(const A& a, const B& b, int32_t count) {
  return p_comparebyte(a, b, count);
}

inline int32_t p_indexbyte(const void* data, int32_t count, uint8_t needle) {
  auto* p = static_cast<const uint8_t*>(data);
  for (int32_t i = 0; i < count; ++i) {
    if (p[i] == needle) return i;
  }
  return -1;
}
template <typename T, typename Needle>
inline int32_t p_indexbyte(const T& data, int32_t count, Needle needle) {
  return p_indexbyte(static_cast<const void*>(std::addressof(data)), count,
                     static_cast<uint8_t>(needle));
}

template <typename Elem, typename Needle>
inline int32_t p_indexword(const Elem* data, int32_t count, Needle needle) {
  for (int32_t i = 0; i < count; ++i) {
    if (data[i] == needle) return i;
  }
  return -1;
}

// `indexword` walks a word array and returns the first matching ordinal.
// Keep array values as first-class Pascal arrays here; callers that already
// have an indexable array object use this overload directly instead of
// relying on any array-to-pointer decay.
template <typename Arr, typename Needle>
inline int32_t p_indexword(const Arr& arr, int32_t count, Needle needle) {
  for (int32_t i = 0; i < count; ++i) {
    if (arr[static_cast<std::size_t>(i)] == needle) return i;
  }
  return -1;
}
// `readln(f, s)` reads a line into `s`. `readln` (no args) reads and
// discards a line. `readln(f)` reads/discards a line from `f`.
inline void p_readln(tp2cc_TextFile& f) {
  if (!f.f) return;
  int c;
  while ((c = std::fgetc(f.f)) != EOF && c != '\n') {}
}
template <int N>
inline void p_readln(tp2cc_TextFile& f, tp2cc_ShortString<N>& s) {
  s.length = 0;
  if (!f.f) return;
  int c;
  while ((c = std::fgetc(f.f)) != EOF && c != '\n') {
    if (s.length < N) { s.data[s.length] = tp2cc_char_of(static_cast<char>(c)); ++s.length; }
  }
}
inline void p_readln(tp2cc_TextFile& f, tp2cc_AnsiString& s) {
  if (!f.f) {
    s.clear();
    return;
  }
  std::vector<p_char> bytes;
  int c;
  while ((c = std::fgetc(f.f)) != EOF && c != '\n') {
    bytes.push_back(tp2cc_char_of(static_cast<char>(c)));
  }
  s.set_length(static_cast<int32_t>(bytes.size()));
  if (!bytes.empty()) {
    std::memcpy(s.data, bytes.data(),
                bytes.size() * sizeof(p_char));
  }
}
template <typename... A> inline void p_readln(A&&...) {}
template <typename... A> inline void p_read(A&&...) {}
// `settextbuf(f, buf, size)` is a stub -- we don't buffer. Take the buffer
// as fully variadic so callers can pass anything (void*, opaque arrays,
// lvalue derefs of untyped pointers) without type-checking fuss.
template <typename... A>
inline void p_settextbuf(tp2cc_TextFile&, A&&...) {}
template <typename T> inline int32_t p_ioresult_of(T&&) { return 0; }

// PChar utilities (strings unit).
inline int p_strlen(const char* s) { return s ? (int)std::strlen(s) : 0; }
inline int p_strlen(const p_char* s) {
  if (!s) return 0;
  int n = 0;
  while (p_char_byte(s[n]) != 0) ++n;
  return n;
}
inline int p_strlen(const tp2cc_AnsiString& s) { return s.length(); }
inline tp2cc_ShortString<> p_strpas(const char* s) { return tp2cc_shortstring_of<>(s); }
inline tp2cc_ShortString<> p_strpas(const p_char* s) { return tp2cc_shortstring_of<>(s); }
template <int N>
inline tp2cc_ShortString<N> p_strpas_s(const char* s) {
  return tp2cc_shortstring_of<N>(s);
}
template <int N>
inline tp2cc_ShortString<N> p_strpas_s(const p_char* s) {
  return tp2cc_shortstring_of<N>(s);
}
template <int N, typename... Cs>
requires ((std::is_same_v<std::remove_cvref_t<Cs>, p_char>) && ...)
constexpr tp2cc_ShortString<N> tp2cc_shortstring_literal(Cs... chars) {
  tp2cc_ShortString<N> out{};
  constexpr std::size_t literal_len = sizeof...(Cs);
  constexpr std::size_t copy_len =
      literal_len < static_cast<std::size_t>(N) ? literal_len
                                                : static_cast<std::size_t>(N);
  const p_char src[] = {chars..., tp2cc_char_of('\0')};
  out.length = static_cast<uint8_t>(copy_len);
  for (std::size_t i = 0; i < copy_len; ++i) out.data[i] = src[i];
  return out;
}
inline char* p_strpcopy(char* dest, const tp2cc_ShortString<>& src) {
  for (int i = 0; i < src.length; ++i) dest[i] = p_char_to_c(src.data[i]);
  dest[src.length] = 0;
  return dest;
}
inline p_char* p_strpcopy(p_char* dest, const tp2cc_ShortString<>& src) {
  for (int i = 0; i < src.length; ++i) dest[i] = src.data[i];
  dest[src.length] = tp2cc_char_of('\0');
  return dest;
}
inline char* p_strpcopy(char* dest, const tp2cc_AnsiString& src) {
  for (int i = 0; i < src.length(); ++i) dest[i] = p_char_to_c(src.data[i]);
  dest[src.length()] = 0;
  return dest;
}
inline p_char* p_strpcopy(p_char* dest, const tp2cc_AnsiString& src) {
  for (int i = 0; i < src.length(); ++i) dest[i] = src.data[i];
  dest[src.length()] = tp2cc_char_of('\0');
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
inline void p_insert(const tp2cc_ShortString<N>& src, tp2cc_ShortString<M>& dest, int pos) {
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

template <int N, int M>
inline void p_insert(tp2cc_ShortStringPtrValue<N> src, tp2cc_ShortString<M>& dest, int pos) {
  p_insert(static_cast<tp2cc_ShortString<N>>(src), dest, pos);
}

template <int N, int M>
inline void p_insert(tp2cc_ShortStringPtrRef<N> src, tp2cc_ShortString<M>& dest, int pos) {
  p_insert(static_cast<tp2cc_ShortString<N>>(src), dest, pos);
}
// Pascal `insert(c, s, pos)` -- insert a single character.
template <int M>
inline void p_insert(p_char c, tp2cc_ShortString<M>& dest, int pos) {
  char src[2] = {p_char_to_c(c), 0};
  p_insert(src, dest, pos);
}
inline void p_insert(const char* src, tp2cc_ShortString<>& dest, int pos) {
  p_insert(tp2cc_shortstring_of<>(src), dest, pos);
}
inline void p_insert(const p_char* src, tp2cc_ShortString<>& dest, int pos) {
  p_insert(tp2cc_shortstring_of<>(src), dest, pos);
}

template <typename Src>
inline void p_insert_bytes(const Src& src, tp2cc_AnsiString& dest, int pos) {
  int32_t src_len = p_string_length(src);
  if (src_len <= 0) return;
  const p_char* src_bytes = p_string_bytes(src);
  std::vector<p_char> owned_src;
  if (src_bytes == dest.bytes()) {
    owned_src.assign(src_bytes, src_bytes + src_len);
    src_bytes = owned_src.data();
  }

  int32_t old_len = dest.length();
  if (pos < 1) pos = 1;
  if (pos > old_len + 1) pos = old_len + 1;

  dest.set_length(old_len + src_len);
  p_char* bytes = dest.data;
  std::memmove(bytes + (pos - 1) + src_len, bytes + (pos - 1),
               static_cast<size_t>(old_len - (pos - 1)) * sizeof(p_char));
  std::memcpy(bytes + (pos - 1), src_bytes,
              static_cast<size_t>(src_len) * sizeof(p_char));
}

template <int N>
inline void p_insert(const tp2cc_ShortString<N>& src, tp2cc_AnsiString& dest, int pos) {
  p_insert_bytes(src, dest, pos);
}

template <int N>
inline void p_insert(tp2cc_ShortStringPtrValue<N> src, tp2cc_AnsiString& dest, int pos) {
  p_insert(static_cast<tp2cc_ShortString<N>>(src), dest, pos);
}

template <int N>
inline void p_insert(tp2cc_ShortStringPtrRef<N> src, tp2cc_AnsiString& dest, int pos) {
  p_insert(static_cast<tp2cc_ShortString<N>>(src), dest, pos);
}

inline void p_insert(const tp2cc_AnsiString& src, tp2cc_AnsiString& dest, int pos) {
  p_insert_bytes(src, dest, pos);
}

inline void p_insert(p_char c, tp2cc_AnsiString& dest, int pos) {
  p_insert_bytes(tp2cc_ansistring_of(c), dest, pos);
}

inline void p_insert(const char* src, tp2cc_AnsiString& dest, int pos) {
  p_insert_bytes(tp2cc_ansistring_of(src), dest, pos);
}

inline void p_insert(const p_char* src, tp2cc_AnsiString& dest, int pos) {
  p_insert_bytes(tp2cc_ansistring_of(src), dest, pos);
}

// --- Memory / bytewise utilities -------------------------------------------

inline void p_fillchar(void* dest, int count, int value) {
  std::memset(dest, value & 0xff, static_cast<size_t>(count));
}
inline void p_fillchar(void* dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
inline void p_fillchar(tp2cc_ShortStringCharRef dest, int count, int value) {
  std::memset(dest.byte, value & 0xff, static_cast<size_t>(count));
}
inline void p_fillchar(tp2cc_ShortStringCharRef dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
inline void p_fillchar(tp2cc_AnsiStringCharRef dest, int count, int value) {
  std::memset(&dest, value & 0xff, static_cast<size_t>(count));
}
inline void p_fillchar(tp2cc_AnsiStringCharRef dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
template <typename T>
requires (!std::is_pointer_v<T>)
inline void p_fillchar(T& dest, int count, int value) {
  std::memset(&dest, value & 0xff, static_cast<size_t>(count));
}
template <typename T>
requires (!std::is_pointer_v<T>)
inline void p_fillchar(T& dest, int count, p_char value) {
  p_fillchar(dest, count, p_ord(value));
}
inline void p_move(const void* src, void* dest, int count) {
  std::memmove(dest, src, static_cast<size_t>(count));
}
inline void p_move(tp2cc_ShortStringCharRef src, void* dest, int count) {
  std::memmove(dest, src.byte, static_cast<size_t>(count));
}
inline void p_move(tp2cc_ShortStringCharValue src, void* dest, int count) {
  std::memmove(dest, src.byte, static_cast<size_t>(count));
}
inline void p_move(tp2cc_AnsiStringCharRef src, void* dest, int count) {
  std::memmove(dest, &src, static_cast<size_t>(count));
}
inline void p_move(tp2cc_AnsiStringCharValue src, void* dest, int count) {
  std::memmove(dest, src.byte, static_cast<size_t>(count));
}
inline void p_move(const void* src, tp2cc_ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, src, static_cast<size_t>(count));
}
inline void p_move(const void* src, tp2cc_AnsiStringCharRef dest, int count) {
  std::memmove(&dest, src, static_cast<size_t>(count));
}
inline void p_move(tp2cc_ShortStringCharValue src, tp2cc_ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, src.byte, static_cast<size_t>(count));
}
inline void p_move(tp2cc_AnsiStringCharValue src, tp2cc_AnsiStringCharRef dest, int count) {
  std::memmove(&dest, src.byte, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(tp2cc_ShortStringCharRef src, D& dest, int count) {
  std::memmove(std::addressof(dest), src.byte, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(tp2cc_AnsiStringCharRef src, D& dest, int count) {
  std::memmove(std::addressof(dest), &src, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(tp2cc_ShortStringCharValue src, D& dest, int count) {
  std::memmove(std::addressof(dest), src.byte, static_cast<size_t>(count));
}
template <typename D>
inline void p_move(tp2cc_AnsiStringCharValue src, D& dest, int count) {
  std::memmove(std::addressof(dest), src.byte, static_cast<size_t>(count));
}
template <typename S, typename D>
inline void p_move(const S& src, D& dest, int count) {
  std::memmove(&dest, &src, static_cast<size_t>(count));
}
template <typename S>
inline void p_move(const S& src, tp2cc_ShortStringCharRef dest, int count) {
  std::memmove(dest.byte, &src, static_cast<size_t>(count));
}
template <typename S>
inline void p_move(const S& src, tp2cc_AnsiStringCharRef dest, int count) {
  std::memmove(&dest, &src, static_cast<size_t>(count));
}
inline void p_getmem(void*& p, int size) {
  p = std::malloc(static_cast<size_t>(size));
}
inline void* p_allocmem(int size) {
  return std::calloc(1, static_cast<size_t>(size));
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
  // Pascal `GetMem(p, n)` allocates exactly `n` bytes even when `p` is a
  // typed pointer. The bootstrap compiler relies on that for giant array
  // pointer views like `^tasmsymbolidxarr`, where only a live prefix is
  // allocated and later passed around as `p^`.
  p = static_cast<P*>(std::malloc(static_cast<size_t>(size)));
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

template <typename P>
inline void p_new(P*& p) {
  // Pascal typed-pointer allocation must stay in the same malloc/realloc/free
  // family as getmem/reallocmem/freemem. Plain typed storage is sometimes
  // grown with reallocmem and later released with dispose, so lowering
  // new/dispose to C++ new/delete would make those paths mismatch. Use
  // placement construction here so managed fields still start life in a
  // proper value-initialized object.
  void* raw = std::malloc(sizeof(P));
  if (!raw) std::abort();
  p = static_cast<P*>(raw);
  ::new (raw) P{};
}

// Pascal `Dispose(p)` -- destroy the pointed-to object and free its
// heap storage. ISO Pascal, Turbo Pascal, and FPC all leave the
// pointer value undefined after the call; we take the pointer by
// value so the same overload handles real `var p : ^T` lvalues and
// typed-cast rvalues like `dispose(PEntry(list[i]))`. The caller's
// slot, if any, is unchanged -- that matches Pascal's contract.
template <typename P>
inline void p_dispose(P* p) {
  if (!p) return;
  std::destroy_at(p);
  std::free(static_cast<void*>(p));
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
inline p_signalhandler p_fpsignal(int32_t sig, p_signalhandler h) {
  return p_signal(sig, h);
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
inline tp2cc_ShortString<> p_paramstr(int i) {
  if (i < 0 || i >= rt_argc || !rt_argv || !rt_argv[i]) return {};
  return tp2cc_shortstring_of<>(rt_argv[i]);
}

inline int32_t p_ioresult() {
  int32_t result = p_last_ioresult;
  p_last_ioresult = 0;
  return result;
}

// --- String intrinsics ------------------------------------------------------

template <int N>
inline int p_pos(const char* needle, const tp2cc_ShortString<N>& hay) {
  int nl = 0; while (needle[nl]) ++nl;
  for (int i = 0; i + nl <= hay.length; ++i) {
    bool ok = true;
    for (int j = 0; j < nl; ++j) {
      if (hay.data[i + j] != tp2cc_char_of(needle[j])) { ok = false; break; }
    }
    if (ok) return i + 1;
  }
  return 0;
}
template <int N>
inline int p_pos(const char* needle, const tp2cc_ShortStringPtrValue<N>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<N>>(hay));
}
template <int N>
inline int p_pos(const char* needle, const tp2cc_ShortStringPtrRef<N>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<N>>(hay));
}
template <int N>
inline int p_pos(const p_char* needle, const tp2cc_ShortString<N>& hay) {
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
template <int N>
inline int p_pos(const p_char* needle, const tp2cc_ShortStringPtrValue<N>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<N>>(hay));
}
template <int N>
inline int p_pos(const p_char* needle, const tp2cc_ShortStringPtrRef<N>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<N>>(hay));
}
template <int N, int M>
inline int p_pos(const tp2cc_ShortString<N>& needle, const tp2cc_ShortString<M>& hay) {
  for (int i = 0; i + needle.length <= hay.length; ++i) {
    bool ok = true;
    for (int j = 0; j < needle.length; ++j) {
      if (hay.data[i + j] != needle.data[j]) { ok = false; break; }
    }
    if (ok) return i + 1;
  }
  return 0;
}
template <int N, int M>
inline int p_pos(const tp2cc_ShortString<N>& needle,
                 const tp2cc_ShortStringPtrValue<M>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<M>>(hay));
}
template <int N, int M>
inline int p_pos(const tp2cc_ShortString<N>& needle, const tp2cc_ShortStringPtrRef<M>& hay) {
  return p_pos(needle, static_cast<tp2cc_ShortString<M>>(hay));
}

// Pascal `pos(c, s)` with a single-char needle. Very common in compiler.
template <int N>
inline int p_pos(p_char c, const tp2cc_ShortString<N>& hay) {
  for (int i = 0; i < hay.length; ++i) {
    if (hay.data[i] == c) return i + 1;
  }
  return 0;
}
template <int N>
inline int p_pos(p_char c, const tp2cc_ShortStringPtrValue<N>& hay) {
  return p_pos(c, static_cast<tp2cc_ShortString<N>>(hay));
}
template <int N>
inline int p_pos(p_char c, const tp2cc_ShortStringPtrRef<N>& hay) {
  return p_pos(c, static_cast<tp2cc_ShortString<N>>(hay));
}

inline int p_pos(const char* needle, const tp2cc_AnsiString& hay) {
  int nl = needle ? static_cast<int>(std::strlen(needle)) : 0;
  int32_t hay_len = hay.length();
  for (int32_t i = 0; i + nl <= hay_len; ++i) {
    bool ok = true;
    for (int j = 0; j < nl; ++j) {
      if (hay.data[i + j] != tp2cc_char_of(needle[j])) {
        ok = false;
        break;
      }
    }
    if (ok) return i + 1;
  }
  return 0;
}

inline int p_pos(const p_char* needle, const tp2cc_AnsiString& hay) {
  int nl = p_strlen(needle);
  int32_t hay_len = hay.length();
  for (int32_t i = 0; i + nl <= hay_len; ++i) {
    bool ok = true;
    for (int j = 0; j < nl; ++j) {
      if (hay.data[i + j] != needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return i + 1;
  }
  return 0;
}

template <int N>
inline int p_pos(const tp2cc_ShortString<N>& needle, const tp2cc_AnsiString& hay) {
  int32_t needle_len = needle.length;
  int32_t hay_len = hay.length();
  for (int32_t i = 0; i + needle_len <= hay_len; ++i) {
    bool ok = true;
    for (int32_t j = 0; j < needle_len; ++j) {
      if (hay.data[i + j] != needle.data[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return i + 1;
  }
  return 0;
}

inline int p_pos(const tp2cc_AnsiString& needle, const tp2cc_AnsiString& hay) {
  int32_t needle_len = needle.length();
  int32_t hay_len = hay.length();
  for (int32_t i = 0; i + needle_len <= hay_len; ++i) {
    bool ok = true;
    for (int32_t j = 0; j < needle_len; ++j) {
      if (hay.data[i + j] != needle.data[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return i + 1;
  }
  return 0;
}

inline int p_pos(p_char c, const tp2cc_AnsiString& hay) {
  for (int32_t i = 0; i < hay.length(); ++i) {
    if (hay.data[i] == c) return i + 1;
  }
  return 0;
}

// Pascal `val(S, real_var, code_var)` overload.
template <int N>
inline void p_val(const tp2cc_ShortString<N>& s, double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  double v = std::strtod(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
inline void p_val(const tp2cc_AnsiString& s, double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  double v = std::strtod(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_ShortString<N>& s, double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_AnsiString& s, double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <int N>
inline void p_val(const tp2cc_ShortString<N>& s, long double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  long double v = std::strtold(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
inline void p_val(const tp2cc_AnsiString& s, long double& out, int32_t& code) {
  std::string buf = p_to_std_string(s);
  char* end = nullptr;
  long double v = std::strtold(buf.c_str(), &end);
  if (end && *end == '\0') { out = v; code = 0; }
  else { code = static_cast<int32_t>(end - buf.c_str()) + 1; }
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_ShortString<N>& s, long double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_AnsiString& s, long double& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <int N>
inline void p_val(const tp2cc_ShortString<N>& s, float& out, int32_t& code) {
  double v = 0.0;
  p_val(s, v, code);
  if (code == 0) out = static_cast<float>(v);
}
inline void p_val(const tp2cc_AnsiString& s, float& out, int32_t& code) {
  double v = 0.0;
  p_val(s, v, code);
  if (code == 0) out = static_cast<float>(v);
}
template <int N, typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_ShortString<N>& s, float& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}
template <typename Code>
requires (std::is_integral_v<Code> && !std::is_same_v<Code, bool> &&
          !std::is_same_v<Code, int32_t>)
inline void p_val(const tp2cc_AnsiString& s, float& out, Code& code) {
  int32_t parsed_code = 0;
  p_val(s, out, parsed_code);
  code = static_cast<Code>(parsed_code);
}

template <int N>
inline tp2cc_ShortString<> p_copy(const tp2cc_ShortString<N>& s, int start, int count) {
  tp2cc_ShortString<> r{};
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
inline tp2cc_ShortString<> p_copy(const tp2cc_ShortStringPtrValue<N>& s,
                            int start,
                            int count) {
  return p_copy(static_cast<tp2cc_ShortString<N>>(s), start, count);
}
template <int N>
inline tp2cc_ShortString<> p_copy(const tp2cc_ShortStringPtrRef<N>& s,
                            int start,
                            int count) {
  return p_copy(static_cast<tp2cc_ShortString<N>>(s), start, count);
}

inline tp2cc_AnsiString p_copy(const tp2cc_AnsiString& s, int start, int count) {
  if (start < 1) start = 1;
  int avail = s.length() - (start - 1);
  if (avail < 0) avail = 0;
  if (count > avail) count = avail;
  if (count < 0) count = 0;
  tp2cc_AnsiString r{};
  r.set_length(count);
  if (count > 0) {
    std::memcpy(r.data, s.data + (start - 1),
                static_cast<size_t>(count) * sizeof(p_char));
  }
  return r;
}

template <int N>
inline void p_delete(tp2cc_ShortString<N>& s, int start, int count) {
  if (start < 1 || start > s.length) return;
  int tail = s.length - (start - 1) - count;
  if (tail < 0) { s.length = static_cast<uint8_t>(start - 1); return; }
  for (int i = 0; i < tail; ++i) s.data[start - 1 + i] = s.data[start - 1 + count + i];
  s.length = static_cast<uint8_t>(s.length - count);
}

inline void p_delete(tp2cc_AnsiString& s, int start, int count) {
  int32_t len = s.length();
  if (start < 1 || start > len || count <= 0) return;
  if (count > len - (start - 1)) count = len - (start - 1);
  s.ensure_unique();
  p_char* bytes = s.data;
  std::memmove(bytes + (start - 1), bytes + (start - 1 + count),
               static_cast<size_t>(len - (start - 1 + count) + 1) *
                   sizeof(p_char));
  s.set_length(len - count);
}

template <int N>
inline void p_insert(const char* src, tp2cc_ShortString<N>& s, int pos) {
  p_insert(tp2cc_shortstring_of<N>(src), s, pos);
}

template <int N>
inline void p_insert(const tp2cc_AnsiString& src, tp2cc_ShortString<N>& s, int pos) {
  p_insert(static_cast<tp2cc_ShortString<N>>(src), s, pos);
}

template <typename Int>
inline tp2cc_ShortString<> p_octstr(Int value, int width) {
  using U = std::make_unsigned_t<Int>;
  U bits = static_cast<U>(value);
  char buf[65];
  int len = 0;
  do {
    buf[len++] = static_cast<char>('0' + (bits & 7));
    bits >>= 3;
  } while (bits != 0);
  while (len < width) buf[len++] = '0';
  tp2cc_ShortString<> out{};
  out.length = static_cast<uint8_t>(len);
  for (int i = 0; i < len; ++i) out.data[i] = tp2cc_char_of(buf[len - 1 - i]);
  return out;
}

inline p_char p_upcase(p_char c) {
  uint8_t b = p_char_byte(c);
  if (b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - 32);
  return tp2cc_char_of(b);
}
template <int N>
inline tp2cc_ShortString<N> p_upcase(const tp2cc_ShortString<N>& s) {
  tp2cc_ShortString<N> r = s;
  for (int i = 0; i < r.length; ++i) r.data[i] = p_upcase(r.data[i]);
  return r;
}

inline tp2cc_AnsiString p_upcase(const tp2cc_AnsiString& s) {
  tp2cc_AnsiString r = tp2cc_ansistring_of(s);
  for (int i = 1; i <= r.length(); ++i) r[i] = p_upcase(r[i]);
  return r;
}

// --- Write / Writeln --------------------------------------------------------
// Variadic emit: each call translates to a sequence of one-arg writes.

// Single-value writers to stdout.
template <int N>
inline void p_write_one(const tp2cc_ShortString<N>& s) {
  for (int i = 0; i < s.length; ++i) std::fputc(p_char_to_c(s.data[i]), stdout);
}
inline void p_write_one(const tp2cc_AnsiString& s) {
  for (int i = 0; i < s.length(); ++i) std::fputc(p_char_to_c(s.data[i]), stdout);
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
inline void p_write_one(const tp2cc_TextFile&)  {}  // first arg of `write(f, ...)`
template <typename T> inline void p_write_one(T* p) {
  std::fprintf(stdout, "%p", (void*)p);
}

template <int N>
inline void p_write_file_one(std::FILE* out, const tp2cc_ShortString<N>& s) {
  if (!out) return;
  for (int i = 0; i < s.length; ++i) std::fputc(p_char_to_c(s.data[i]), out);
}
inline void p_write_file_one(std::FILE* out, const tp2cc_AnsiString& s) {
  if (!out) return;
  for (int i = 0; i < s.length(); ++i) std::fputc(p_char_to_c(s.data[i]), out);
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
inline void p_write_file_one(std::FILE*, const tp2cc_TextFile&) {}
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
inline void p_write(tp2cc_TextFile& f, Args&&... args) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  (p_write_file_one(f.f, std::forward<Args>(args)), ...);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
inline void p_write(tp2cc_TextFile& f) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
template <typename... Args>
inline void p_writeln(tp2cc_TextFile& f, Args&&... args) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  (p_write_file_one(f.f, std::forward<Args>(args)), ...);
  std::fputc('\n', f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}
inline void p_writeln(tp2cc_TextFile& f) {
  if (!f.f) {
    p_set_ioresult(f, 103);
    return;
  }
  std::fputc('\n', f.f);
  p_set_ioresult(f, std::ferror(f.f) ? 101 : 0);
}

// --- File-IO placeholders ---------------------------------------------------
// Real behaviour is added as units are translated that need them.
template <int N>
inline void p_assign(tp2cc_TextFile& f, const tp2cc_ShortString<N>& n) {
  f.name = n;
  f.f = nullptr;
  p_set_ioresult(f, 0);
}
template <int N>
inline void p_assign(tp2cc_TextFile& f, const tp2cc_ShortStringPtrValue<N>& n) {
  p_assign(f, static_cast<tp2cc_ShortString<N>>(n));
}
template <int N>
inline void p_assign(tp2cc_TextFile& f, const tp2cc_ShortStringPtrRef<N>& n) {
  p_assign(f, static_cast<tp2cc_ShortString<N>>(n));
}
inline void p_assign(tp2cc_TextFile& f, const tp2cc_AnsiString& n) {
  p_assign(f, tp2cc_shortstring_of<>(n.bytes()));
}
inline void p_reset(tp2cc_TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "rb");
  p_set_ioresult(f, f.f ? 0 : 2);  // 2 = file-not-found per fpc convention
}
inline void p_reset(tp2cc_TextFile& f, int32_t) { p_reset(f); }  // rec size form
inline void p_rewrite(tp2cc_TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "wb");
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_rewrite(tp2cc_TextFile& f, int32_t) { p_rewrite(f); }
inline void p_append(tp2cc_TextFile& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "ab");
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_close(tp2cc_TextFile& f) {
  if (f.f) { std::fclose(f.f); f.f = nullptr; }
  p_set_ioresult(f, 0);
}
inline bool p_eof(tp2cc_TextFile& f) {
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
inline void p_assign(tp2cc_TypedFile<T>& f, const tp2cc_ShortString<>& n) {
  f.name = n;
  f.f = nullptr;
  p_set_ioresult(f, 0);
}
template <typename T, int N>
inline void p_assign(tp2cc_TypedFile<T>& f, const tp2cc_ShortStringPtrValue<N>& n) {
  p_assign(f, static_cast<tp2cc_ShortString<>>(n));
}
template <typename T, int N>
inline void p_assign(tp2cc_TypedFile<T>& f, const tp2cc_ShortStringPtrRef<N>& n) {
  p_assign(f, static_cast<tp2cc_ShortString<>>(n));
}
template <typename T>
inline void p_assign(tp2cc_TypedFile<T>& f, const tp2cc_AnsiString& n) {
  p_assign(f, tp2cc_shortstring_of<>(n.bytes()));
}
template <typename T>
inline void p_reset(tp2cc_TypedFile<T>& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "rb");
  p_set_ioresult(f, f.f ? 0 : 2);
}
template <typename T> inline void p_reset(tp2cc_TypedFile<T>& f, int32_t) { p_reset(f); }
template <typename T>
inline void p_rewrite(tp2cc_TypedFile<T>& f) {
  if (f.f) {
    std::fclose(f.f);
    f.f = nullptr;
  }
  char buf[260]{};
  p_file_name_to_buf(f, buf);
  f.f = std::fopen(buf, "wb");
  p_set_ioresult(f, f.f ? 0 : 5);
}
template <typename T> inline void p_rewrite(tp2cc_TypedFile<T>& f, int32_t) { p_rewrite(f); }
template <typename T>
inline void p_append(tp2cc_TypedFile<T>& f) {
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
inline void p_close(tp2cc_TypedFile<T>& f) {
  if (f.f) { std::fclose(f.f); f.f = nullptr; }
  p_set_ioresult(f, 0);
}
template <typename T>
inline bool p_eof(const tp2cc_TypedFile<T>& f) {
  if (!f.f) return true;
  int c = std::fgetc(f.f);
  if (c == EOF) return true;
  std::ungetc(c, f.f);
  return false;
}

// --- Val / Str --------------------------------------------------------------

template <int N>
inline void p_val(const tp2cc_ShortString<N>& s, int32_t& out, int32_t& code) {
  p_parse_pascal_integer(p_to_std_string(s), out, code);
}
template <int N, typename UInt>
requires (std::is_integral_v<UInt> && std::is_unsigned_v<UInt> &&
          !std::is_same_v<UInt, bool> && !std::is_same_v<UInt, uint8_t>)
inline void p_val(const tp2cc_ShortString<N>& s, UInt& out, int32_t& code) {
  // Pascal keeps a leading-zero decimal literal decimal. Reusing the shared
  // parser here avoids C's base-0 rule turning `01012` into octal `522`.
  using ParseUInt =
      std::conditional_t<(sizeof(UInt) <= sizeof(uint32_t)), uint32_t, uint64_t>;
  ParseUInt parsed = 0;
  p_parse_pascal_integer(p_to_std_string(s), parsed, code);
  if (code != 0 ||
      parsed > static_cast<ParseUInt>(std::numeric_limits<UInt>::max())) {
    out = 0;
    if (code == 0) code = 1;
    return;
  }
  out = static_cast<UInt>(parsed);
}
inline void p_val(const tp2cc_AnsiString& s, int32_t& out, int32_t& code) {
  p_parse_pascal_integer(p_to_std_string(s), out, code);
}
template <int N>
inline void p_str(int32_t v, tp2cc_ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(int32_t v, tp2cc_AnsiString& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", v);
  out = buf;
}
template <int N>
inline void p_str(uint32_t v, tp2cc_ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u", v);
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(uint32_t v, tp2cc_AnsiString& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u", v);
  out = buf;
}
template <int N>
inline void p_str(int64_t v, tp2cc_ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(int64_t v, tp2cc_AnsiString& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out = buf;
}
template <int N>
inline void p_str(uint64_t v, tp2cc_ShortString<N>& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(uint64_t v, tp2cc_AnsiString& out) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  out = buf;
}
template <int N>
inline void p_str(float v, tp2cc_ShortString<N>& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .9g", static_cast<double>(v));
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(float v, tp2cc_AnsiString& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .9g", static_cast<double>(v));
  out = buf;
}
template <int N>
inline void p_str(double v, tp2cc_ShortString<N>& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .17g", v);
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(double v, tp2cc_AnsiString& out) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "% .17g", v);
  out = buf;
}
template <int N>
inline void p_str(long double v, tp2cc_ShortString<N>& out) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "% .21Lg", v);
  out = tp2cc_shortstring_of<N>(buf);
}
inline void p_str(long double v, tp2cc_AnsiString& out) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "% .21Lg", v);
  out = buf;
}

// Pascal `HexStr(val, cnt)` -- uppercase, zero-padded to `cnt` digits, no
// leading `$`. Truncates the high digits if `cnt` is smaller than the
// natural width (matches fpc behaviour: callers like ogmap.pas pass
// `sizeof(aint)*2` so width tracks the integer size explicitly).
inline tp2cc_ShortString<> p_hexstr(int64_t val, uint8_t cnt) {
  uint64_t bits = static_cast<uint64_t>(val);
  if (cnt > 16) cnt = 16;
  char buf[17];
  for (int i = cnt - 1; i >= 0; --i) {
    uint8_t nyb = static_cast<uint8_t>(bits & 0xF);
    buf[i] = static_cast<char>(nyb < 10 ? '0' + nyb : 'A' + (nyb - 10));
    bits >>= 4;
  }
  buf[cnt] = '\0';
  return tp2cc_shortstring_of<>(buf);
}
inline tp2cc_ShortString<> p_hexstr(int32_t val, uint8_t cnt) {
  return p_hexstr(static_cast<int64_t>(static_cast<uint32_t>(val)), cnt);
}
inline tp2cc_ShortString<> p_hexstr(uint32_t val, uint8_t cnt) {
  return p_hexstr(static_cast<int64_t>(val), cnt);
}
inline tp2cc_ShortString<> p_hexstr(uint64_t val, uint8_t cnt) {
  return p_hexstr(static_cast<int64_t>(val), cnt);
}
// Pointer overload uses the natural pointer width; fpc returns a string
// whose length matches `sizeof(pointer)*2`.
inline tp2cc_ShortString<> p_hexstr(const void* p) {
  return p_hexstr(static_cast<int64_t>(reinterpret_cast<uintptr_t>(p)),
                  static_cast<uint8_t>(sizeof(void*) * 2));
}

// `FreeAndNil(var obj)` -- null-safe Free, then clear obj to nil. The
// Pascal var parameter is untyped; the emitter passes a real reference so
// we get the pointer's storage back to overwrite. Templated to keep the
// obj-pointer type intact for the Pascal class destruction sequence.
template <typename T>
inline void p_freeandnil(T*& obj) {
  T* tmp = obj;
  obj = nullptr;
  p_tobject::p_free(tmp);
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
inline constexpr p_sizeint p_maxint = p_maxlongint;
inline constexpr double  p_pi         = 3.141592653589793;

// Pascal `typeof(T)` returns a pointer to T's virtual method table. We
// don't model VMT values at emit time, so stub it as nullptr. Sites
// that use it (stream registration, runtime class-id lookup) only need
// the value to be comparable to other nullptrs, which holds.
template <typename T> inline void* p_typeof(const T&) {
  return nullptr;
}
inline void* p_typeof(...) {
  return nullptr;
}

// Pascal `ofs(x)` / `seg(x)` return the 16-bit offset/segment of a
// far pointer. On flat-model targets both are stubbed to 0.
template <typename... A> inline int32_t p_ofs(A&&...) { return 0; } // FPC 1: for browse info streaming
template <typename... A> inline int32_t p_seg(A&&...) { return 0; } // FPC 1: for browse info streaming

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
  for (std::size_t i = 0; i < n; ++i) out[i] = tp2cc_char_of(s[i]);
  out[n] = tp2cc_char_of('\0');
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
inline bool p_chmod(const tp2cc_ShortString<>& path, int32_t newmode) {
  return ::chmod(p_to_std_string(path).c_str(),
                 static_cast<mode_t>(newmode)) == 0;
}
inline void p_gettime(uint16_t& hour, uint16_t& minute, uint16_t& second,
                      uint16_t& msec, uint16_t& usec) {
  struct timeval tv{};
  ::gettimeofday(&tv, nullptr);
  std::time_t t = static_cast<std::time_t>(tv.tv_sec);
  std::tm lt{};
  ::localtime_r(&t, &lt);
  hour   = static_cast<uint16_t>(lt.tm_hour);
  minute = static_cast<uint16_t>(lt.tm_min);
  second = static_cast<uint16_t>(lt.tm_sec);
  msec   = static_cast<uint16_t>(tv.tv_usec / 1000);
  usec   = static_cast<uint16_t>(tv.tv_usec % 1000);
}
inline void p_gettime(uint16_t& hour, uint16_t& minute, uint16_t& second,
                      uint16_t& sec100) {
  uint16_t msec = 0, usec = 0;
  p_gettime(hour, minute, second, msec, usec);
  sec100 = static_cast<uint16_t>(msec / 10);  // ms -> hundredths
}
inline void p_gettime(uint16_t& hour, uint16_t& minute, uint16_t& second) {
  uint16_t msec = 0, usec = 0;
  p_gettime(hour, minute, second, msec, usec);
}
inline void p_getdate(uint16_t& year, uint16_t& month, uint16_t& mday,
                      uint16_t& wday) {
  std::time_t t = std::time(nullptr);
  std::tm lt{};
  ::localtime_r(&t, &lt);
  year = static_cast<uint16_t>(lt.tm_year + 1900);
  month = static_cast<uint16_t>(lt.tm_mon + 1);
  mday = static_cast<uint16_t>(lt.tm_mday);
  wday = static_cast<uint16_t>(lt.tm_wday);
}
// FPC's DOS unit uses a packed DOS timestamp record here, not Unix epoch
// seconds. Compiler units decode those bitfields directly via UnpackTime.
struct DateTime {
  uint16_t p_year = 0;
  uint16_t p_month = 0;
  uint16_t p_day = 0;
  uint16_t p_hour = 0;
  uint16_t p_min = 0;
  uint16_t p_sec = 0;
};
using p_datetime = DateTime;
inline void p_unpacktime(int32_t p, DateTime& t) {
  t.p_sec = static_cast<uint16_t>((p & 31) * 2);
  p >>= 5;
  t.p_min = static_cast<uint16_t>(p & 63);
  p >>= 6;
  t.p_hour = static_cast<uint16_t>(p & 31);
  p >>= 5;
  t.p_day = static_cast<uint16_t>(p & 31);
  p >>= 5;
  t.p_month = static_cast<uint16_t>(p & 15);
  p >>= 4;
  t.p_year = static_cast<uint16_t>(p + 1980);
}
inline void p_packtime(DateTime& t, int32_t& p) {
  int32_t zs = t.p_hour;
  p = ((static_cast<int32_t>(t.p_year) - 1980) & 127);
  p = (p << 4) + t.p_month;
  p = (p << 5) + t.p_day;
  p <<= 16;
  zs = (zs << 6) + t.p_min;
  zs = (zs << 5) + (t.p_sec / 2);
  p += (zs & 0xffff);
}
inline constexpr p_char p_directoryseparator = tp2cc_char_of('/');
inline constexpr p_char p_driveseparator = tp2cc_char_of(':');
// PATH-list separator (the one between entries in $PATH-style strings),
// not the drive-letter colon.
inline constexpr p_char p_pathseparator = tp2cc_char_of(':');

// Append the platform directory separator if the path does not already
// end with one (and is non-empty). On Linux this is `/`. The empty
// string stays empty -- Pascal's sysutils returns the input unchanged
// in that case.
template <typename Str>
inline tp2cc_AnsiString p_includetrailingpathdelimiter(const Str& input) {
  std::string s = p_to_std_string(input);
  if (s.empty() || s.back() == p_char_to_c(p_directoryseparator)) {
    return tp2cc_ansistring_of(s.c_str());
  }
  s.push_back(p_char_to_c(p_directoryseparator));
  return tp2cc_ansistring_of(s.c_str());
}
// `set of char` so cross-platform path code can recognise foreign
// separators (e.g. accept '\' even on unix); cfileutils.pas tests `s[i] in
// AllowDirectorySeparators` against arbitrary input paths.
inline tp2cc_Set<p_char> p_allowdirectoryseparators =
    set_of<p_char>({tp2cc_char_of('/'), tp2cc_char_of('\\')});
inline int32_t p_extraoptions = 0;
inline int32_t p_moduleindex = 0;
template <int N, int M>
inline void p_exec(const tp2cc_ShortString<N>& command, const tp2cc_ShortString<M>& para) {
  std::vector<std::string> args;
  args.push_back(p_to_std_string(command));
  auto rest = p_split_commandline(p_to_std_string(para));
  args.insert(args.end(), rest.begin(), rest.end());
  p_spawn_process(args);
}
// Pascal `include(set, elem)` / `exclude(set, elem)` add/remove a
// single element. Not stubs -- these are real Pascal set builtins.
template <typename E1, typename E2>
inline void p_include(tp2cc_Set<E1>& s, E2 v) { s.add(static_cast<E1>(v)); }
template <typename E1, typename E2>
inline void p_exclude(tp2cc_Set<E1>& s, E2 v) {
  int i = tp2cc_Set<E1>::idx(static_cast<E1>(v));
  if (i >= 0 && i < 8 * tp2cc_Set<E1>::Nb) {
    s.bits[i >> 3] &= static_cast<unsigned char>(~(1u << (i & 7)));
  }
}

inline void p_popen(tp2cc_TextFile& f, const tp2cc_ShortString<>& cmd, p_char mode) {
  char m[2] = {static_cast<char>(std::tolower(p_char_to_c(mode))), '\0'};
  f.f = ::popen(p_to_std_string(cmd).c_str(), m);
  p_set_ioresult(f, f.f ? 0 : 5);
}
inline void p_popen(tp2cc_TextFile& f, const tp2cc_ShortString<>& cmd, char mode) {
  p_popen(f, cmd, tp2cc_char_of(mode));
}
template <typename F>
inline void p_popen(F&, const tp2cc_ShortString<>&, char) {}
template <typename F>
inline void p_popen(F&, const tp2cc_ShortString<>&, p_char) {}
inline void p_pclose(tp2cc_TextFile& f) {
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
// STUB: stderr is the fpc standard error tp2cc_TextFile.
inline tp2cc_TextFile p_stderr;
inline tp2cc_TextFile p_output = [] {
  tp2cc_TextFile f;
  f.f = stdout;
  return f;
}();
inline tp2cc_TextFile p_input = [] {
  tp2cc_TextFile f;
  f.f = stdin;
  return f;
}();
// (No global `tprocdefcoll` -- it's a function-local record type in
// tccal.pas and gets emitted there. An earlier stub here was taking
// name precedence over the real thing via `using namespace ::rt`.)

// STUB: target-platform import/export/linker types from back-ends we still
// skip entirely. Keep the aliases only for units that are not translated at
// all; once a real unit exists (e.g. `t_win32.pas`), a duplicate runtime
// alias would collide with the translated class name through `using
// namespace ::rt`.
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
using p_timportlibos2 = StubTargetLib;
using p_pimportlibos2 = StubTargetLib*;
using p_timportlibgo32v2 = StubTargetLib;
using p_pimportlibgo32v2 = StubTargetLib*;
using p_texportlibos2 = StubTargetLib;
using p_pexportlibos2 = StubTargetLib*;
using p_texportlibgo32v2 = StubTargetLib;
using p_pexportlibgo32v2 = StubTargetLib*;
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
inline bool p_fstat(const tp2cc_ShortString<N>& path, p_stat& info) {
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

// Return value of `getenv`. fpc's `dos.getenv` returns tp2cc_ShortString,
// `linux.getenv` returns pchar -- same lowered name, different
// types. The proxy converts to both, so one `rt::p_getenv` serves
// both `Dos.Getenv` and `Linux.Getenv` call sites.
struct GetEnvResult {
  const char* raw;  // null-terminated env value, or nullptr if unset
  operator const p_char*() const { return raw ? p_from_c_str_copy(raw) : nullptr; }
  operator p_char*() const { return raw ? p_from_c_str_copy(raw) : nullptr; }
  operator tp2cc_ShortString<>() const {
    return raw ? tp2cc_shortstring_of<>(raw) : tp2cc_shortstring_of<>("");
  }
};
template <int N>
inline auto operator+(const tp2cc_ShortString<N>& a, const GetEnvResult& b) {
  return a + static_cast<tp2cc_ShortString<>>(b);
}
template <int N>
inline auto operator+(const GetEnvResult& a, const tp2cc_ShortString<N>& b) {
  return static_cast<tp2cc_ShortString<>>(a) + b;
}
inline GetEnvResult p_getenv(const tp2cc_ShortString<>& name) {
  char buf[260]{};
  int n = name.length < 255 ? name.length : 255;
  for (int i = 0; i < n; ++i) buf[i] = p_char_to_c(name.data[i]);
  return {std::getenv(buf)};
}
// Pascal `Linux.Shell(cmd)` -- run a command via `/bin/sh -c`, i.e.
// POSIX `system(3)`. Used by the compiler for wildcard expansion.
template <int N>
inline int32_t p_shell(const tp2cc_ShortString<N>& cmd) {
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
