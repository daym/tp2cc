# TP2CC Compiler Design

This document describes the current compiler and runtime behavior as implemented today in `src/emit.cc`, `src/typereg.cc`, the parser/AST code in `src/`, and `tp2cc_rt/prelude.h`.

It is intentionally written from a user's perspective:

- what Pascal constructs are accepted
- what they lower to in generated C++
- which conversions are implicit vs explicit
- which workarounds and limitations still exist
- what the prelude runtime library provides

Examples show the shape of the generated code, not exact whitespace or every helper name in every context.

## 1. Lowering Model

At a high level, tp2cc does this:

1. Parse Pascal into an AST.
2. Build a cross-unit type and member registry.
3. Resolve names using Pascal rules.
4. Lower expressions, statements, declarations, and types into explicit C++.
5. Rely on `tp2cc_rt/prelude.h` for Pascal carrier types and helper routines.

### 1.1 Unit and symbol naming

Each Pascal unit becomes a C++ namespace, typically `p_<unit>`. User-visible symbols also get `p_` prefixes.

Pascal:

```pascal
unit Foo;
var Counter: Integer;
```

Representative C++:

```cpp
namespace p_foo {
  int32_t p_counter;
}
```

### 1.2 Name resolution order

The emitter does not leave most lookup to C++. It resolves names itself using Pascal-style precedence. The current order is roughly:

1. function result / function-name-as-result
2. active `with` scopes
3. nested routines
4. locals and parameters
5. implicit properties
6. current class members
7. current unit
8. used units
9. runtime fallback in `::rt`, only for names starting with `p_`

Every unit implicitly sees `System`; internally the type registry models that with a synthetic runtime unit.

### 1.3 Scalars, literals, and ordinary operators

Plain arithmetic and comparisons lower mostly directly. Some Pascal operators become runtime helpers.

Pascal:

```pascal
x := not y;
```

Representative C++:

```cpp
p_x = ::rt::p_not(p_y);
```

Character and string literals are explicit runtime objects:

Pascal:

```pascal
c := 'A';
s := 'Hello';
```

Representative C++:

```cpp
p_c = ::rt::tp2cc_char_of('A');
p_s = ::rt::tp2cc_shortstring_literal<255>("Hello");
```

### 1.4 Strings

Pascal strings are not lowered as raw `char *`. They use explicit runtime carriers:

- `string[n]` -> `::rt::tp2cc_ShortString<n>`
- unsized `string` -> `::rt::tp2cc_ShortString<>`
- `ansistring` and related types -> `::rt::tp2cc_AnsiString`

Concatenation and comparisons are done through runtime operators/helpers on those carrier types.

### 1.5 Arrays

Fixed arrays preserve their lower bounds in the type:

Pascal:

```pascal
var A: array[1..3] of Integer;
```

Representative C++:

```cpp
::rt::tp2cc_Array<int32_t, 1, 3> p_a;
```

Open-array actual arguments are lowered explicitly with runtime helpers.

Pascal:

```pascal
Proc([A, B, C]);
```

Representative C++:

```cpp
p_proc(::rt::tp2cc_open_array_of<int32_t>(p_a, p_b, p_c));
```

Dynamic arrays lower to `::rt::tp2cc_DynArray<T>`.

### 1.6 Sets

Pascal sets use `::rt::tp2cc_Set<T>`.

Pascal:

```pascal
if X in S then ...
T := [1, 2, 5..8];
U := A >< B;
```

Representative C++:

```cpp
if (p_s.contains(p_x)) { ... }
p_t = ::rt::tp2cc_Set<int32_t>::from_list(...);
p_u = ((p_a + p_b) - (p_a * p_b));
```

`><` is lowered as symmetric difference via union minus intersection.

### 1.7 Pointers and dereference

Pascal dereference uses a runtime helper:

Pascal:

```pascal
x := p^;
```

Representative C++:

```cpp
p_x = ::rt::tp2cc_deref(p_p);
```

Address-of is emitted explicitly. For methods it may become a generated thunk.

### 1.8 Records, objects, and classes

The compiler distinguishes three important cases:

- `record`: value type C++ struct
- Turbo Pascal `object`: value type C++ struct with methods
- Delphi/FPC `class`: reference type lowered as pointer to struct

Pascal `class` types are heap-allocated and generally lowered as pointers. `object` values stay inline.

`self` lowers differently:

- class method body: `this`
- value object method body: `(*this)`

Class declarations follow the Delphi/FPC split between true forwards and
empty class bodies:

- `TFoo = class;` is a forward declaration
- `TBar = class(TFoo);` is a complete empty inherited class declaration,
  equivalent to `class(TFoo) end`

### 1.9 Constructors and `Create`

Reference-class construction is explicit. The emitter allocates the object and then calls the Pascal constructor helper.

Pascal:

```pascal
o := TFoo.Create(1, 2);
```

Representative C++:

```cpp
p_o = [&]() {
  auto *p_tmp = new p_tfoo{};
  p_tmp->p_create(1, 2);
  return p_tmp;
}();
```

### 1.10 Metaclasses and `class of`

tp2cc has real support for Pascal metaclass values (`class of T`), but the supported surface is narrow and operational rather than reflective.

Today this means:

- `class of T` parses as a distinct metaclass type
- a class identifier used as a value lowers to a metaclass descriptor, not an instance pointer
- metaclass values can be compared for equality
- constructors and class methods can be called through metaclass values
- instance-side `ClassType` and `InstanceSize` are supported on translated classes

Representative shape:

```cpp
using p_tbaseclass = const tp2cc_metaclass_p_tbase *;

p_cls = tp2cc_metaclass_value_p_tchild();
p_inst = p_cls->p_create(1);
p_same = (p_x->p_classtype() == tp2cc_metaclass_value_p_tchild());
```

What tp2cc does not currently model as a general metaclass surface is RTTI/reflection-style members such as arbitrary `.ClassName` access on metaclass values. The implemented metaclass member surface is essentially:

- constructors
- class methods

plus instance-side `ClassType` / `InstanceSize` on class instances.

### 1.11 `Free`

`obj.Free` lowers through the runtime `TObject` helper so that Pascal's null-safe `Free` behavior is preserved.

Pascal:

```pascal
o.Free;
```

Representative C++:

```cpp
::rt::p_tobject::p_free(p_o);
```

### 1.12 Procedure variables and `of object`

Plain procedural types lower to function pointers or `std::function`-like helpers depending on context. `procedure/function ... of object` lowers to `::rt::tp2cc_MethodPtr<...>`.

Method pointers are represented as code-plus-self pairs. The emitter generates thunks where needed so a Pascal method can be called through that runtime carrier.

### 1.13 Properties

Properties are not magical storage in the generated code. They lower to:

- direct field access
- getter calls
- setter calls
- default/indexed property reads and writes on the property result, when a
  non-indexed property returns an indexable/container object

depending on the property declaration.

Pascal:

```pascal
x := Obj.Value;
Obj.Value := 42;
```

Representative C++:

```cpp
p_x = p_obj->p_get_value();
p_obj->p_set_value(42);
```

or direct field access when the property maps to a field.

Indexed properties lower in the obvious way:

Pascal:

```pascal
p := Obj[1];
Obj[2] := p;
```

Representative C++:

```cpp
p_p = p_obj->p_get(1);
p_obj->p_put(2, p_p);
```

And when a non-indexed property returns a container object with a default
indexed property, tp2cc lowers through the property result rather than
treating the outer property as the write target:

Pascal:

```pascal
p := Box.List[1];
Box.List[2] := p;
```

Representative C++:

```cpp
p_p = p_box->p_flist->p_get(1);
p_box->p_flist->p_put(2, p_p);
```

### 1.14 `with`

`with` is implemented by binding the lowered target into a temporary C++ name and resolving later identifiers against that stack before ordinary lexical lookup.

The compiler tries to preserve lvalue behavior:

- true lvalue `with` targets become `auto &`
- rvalue/cast-like cases become `auto`

### 1.15 `absolute`

`absolute` declarations lower to storage aliases, not copies. The emitter uses explicit reinterpretation helpers to bind the alias to the target bytes.

### 1.16 `is` and `as`

Pascal runtime type checks on classes lower to C++ RTTI operations.

Pascal:

```pascal
if x is TFoo then ...
y := x as TFoo;
```

Representative C++:

```cpp
if (dynamic_cast<p_tfoo *>(p_x) != nullptr) { ... }
p_y = dynamic_cast<p_tfoo *>(p_x);
```

### 1.17 Nested procedures

Nested procedures/functions lower to capturing lambdas and `std::function`-style wrappers when needed. This is how tp2cc preserves lexical capture and nested-scope behavior in C++.

## 2. Type Conversions

The emitter prefers explicit helper calls over implicit C++ conversion machinery. This is a deliberate runtime design rule: Pascal carrier types in the prelude avoid user-defined C++ constructors and implicit conversion tricks where possible.

### 2.1 Implicit conversions

The compiler currently performs these kinds of implicit conversions when the target type is known:

- constant integer conversion to the target integer type
- shortstring capacity narrowing/widening
- shortstring to ansistring
- parameterless callable to call result in value context
- some property reads through implicit member/property resolution

For constant integers:

- implicit assignment-style conversion checks range
- explicit Pascal casts do not report the same narrowing errors

### 2.2 Explicit conversions

Pascal typecasts generally lower to explicit runtime helpers or explicit C++ casts.

Examples:

- `char(x)` / `chr(x)` -> `::rt::p_chr(...)`
- explicit set cast -> `::rt::tp2cc_set_cast<Dst>(src)`
- class cast via `as` -> `dynamic_cast`
- byte reinterpretation of storage -> reinterpret helpers from the runtime

### 2.3 String conversions

When a concrete target string type is known, the emitter converts explicitly.

Examples:

- to `string[20]` -> `::rt::tp2cc_shortstring_of<20>(...)`
- to unsized shortstring -> `::rt::tp2cc_shortstring_of<255>(...)`
- to `ansistring` -> `::rt::tp2cc_ansistring_of(...)`

Important rule: the emitter tries to preserve string lvalues when passing mutable `var`/`out`-style arguments. It does not intentionally wrap those in temporaries.

### 2.4 Untyped `var` storage views

Untyped `var` is treated as raw storage.

For example, code like:

```pascal
l := longint(b);
longint(b) := l;
inc(longint(b));
```

does not become a typed C++ reference into arbitrary storage anymore. tp2cc now lowers these cases as byte load/store operations:

- read -> `::rt::tp2cc_reinterpret_load<T>(...)`
- write -> `::rt::tp2cc_reinterpret_store<T>(...)`
- read/modify/write -> helper built from load/store

This preserves Pascal's storage-reinterpretation semantics while avoiding misaligned typed-reference UB in C++.

### 2.5 Typed storage reinterprets

When the source really is typed storage being re-viewed as another Pascal type, the emitter can still lower to reference-like reinterpret helpers such as:

- `::rt::tp2cc_reinterpret_storage_ref<T>(...)`
- `::rt::tp2cc_reinterpret_ref<T>(...)`

The distinction is whether the input is already storage of interest or a pointer to it.

### 2.6 Method pointer conversions

Converting a method designator to a `... of object` value is explicit in the lowered code. The emitter produces the method thunk and object pointer pair needed by `::rt::tp2cc_MethodPtr`.

## 3. Workarounds and Limitations

This section describes current behavior, not ideal future behavior.

### 3.1 Packed aggregate restrictions

tp2cc intentionally rejects some accesses through packed aggregate fields because the natural C++ lowering would create misaligned typed references.

Currently rejected:

- nested member access through a packed aggregate field
- indexing through a packed aggregate field

except for narrow byte-aligned cases such as some character and byte-like carriers.

These checks happen before property lowering as well. If reaching a field,
container, or property target would first require misaligned packed-member
access, tp2cc reports that statically instead of emitting a setter call or
field write through a misaligned base.

This is why some old FPC sources needed source-side depacking of in-memory backend tables.

### 3.2 Anonymous inline record/object types

Anonymous inline `record`/`object` types in arbitrary type positions are not fully modeled yet. In some code paths they still lower to a placeholder C++ type rather than a real emitted local struct. This is a known weak spot.

### 3.3 Distinct types are not fully nominal yet

The parser and AST have a notion of distinct types, but the emitter still mostly lowers them as their underlying type. So today they are not enforced as strong nominal wrapper types throughout generated C++.

### 3.4 Externals

Current status:

- external variables: unsupported
- external procedures/functions: unsupported

These parse, but the emitter reports them as unsupported.

### 3.5 `asm`

`asm ... end` is recognized but not meaningfully translated. The parser mainly drains the body, and the emitter treats it as unsupported output.

### 3.6 Class method feature subset

The parser currently rejects several Delphi/FPC class-method features such as:

- `virtual`
- `abstract`
- `override`
- `dynamic`
- `message`

This is an intentional current limitation.

For ordinary class declarations, tp2cc does support both:

- true forward declarations like `TFoo = class;`
- empty inherited class declarations like `TBar = class(TFoo);`

### 3.7 Metaclass reflection subset

Metaclass support exists, but it is not a full Delphi/FPC RTTI surface.

Supported today:

- `class of T`
- class identifiers as metaclass values
- constructor/class-method dispatch through metaclass values
- `ClassType`
- `InstanceSize`

Not generally supported today:

- arbitrary reflective metaclass members such as `.ClassName`

### 3.8 Open-array constructor edge cases

Open-array actuals from simple list syntax are supported. Some more complex forms, especially with range-like syntax in those contexts, are still limited.

### 3.9 Bare `raise` and constructor-only behavior

Some context-sensitive Pascal behavior is enforced directly by the emitter. For example:

- bare `raise` is only valid where the compiler tracks an active exception context
- some constructor-only control flow such as `fail` is restricted

### 3.10 Runtime and backend stubs

The prelude still contains some stubbed or placeholder functionality, especially for low-level platform glue and backend-specific edges. Some old bootstrap paths work by avoiding those code paths rather than by having a complete final runtime model.

## 4. The Prelude Runtime Library

`tp2cc_rt/prelude.h` is the runtime layer that makes the generated C++ look Pascal-shaped. It is not just a bag of helper functions; it defines the core carrier types the emitter targets.

### 4.1 Core carrier types

High-level runtime types include:

- `p_char`
- `tp2cc_ShortString<N>`
- `tp2cc_AnsiString`
- `tp2cc_Array<T, Lo, N>`
- `tp2cc_DynArray<T>`
- `tp2cc_OpenArray<T>`
- `tp2cc_Set<T>`
- `tp2cc_TextFile`
- `tp2cc_TypedFile<T>`
- `tp2cc_MethodPtr<...>`
- `p_tmethod`
- `p_tobject`
- exception and metaclass support types

### 4.2 Strings

The runtime provides:

- shortstring construction and narrowing helpers
- ansistring conversion helpers
- concatenation
- comparison
- insertion/deletion/copying
- pointer/string bridge helpers such as `StrPas`-style behavior

The important design choice is that emitted code calls named helpers explicitly instead of relying on C++ implicit conversion operators.

### 4.3 Arrays and open arrays

The runtime models:

- Pascal lower-bounded fixed arrays
- dynamic arrays
- borrowed open-array views
- value-backed open-array temporaries

This is why the emitter can preserve Pascal lower bounds and open-array call behavior.

### 4.4 Sets

`tp2cc_Set<T>` provides the set operations needed by the language:

- contains
- union
- intersection
- difference
- typed construction helpers

### 4.5 Object model support

The runtime carries the common object/class support needed by generated code:

- `TObject`-like base support
- null-safe `Free`
- method-pointer carriers
- metaclass descriptors
- metaclass value constructors/class-method thunks
- `ClassType` / `InstanceSize`
- RTTI-driven `is` / `as` support through generated C++ RTTI usage

### 4.6 Reinterpretation and raw storage helpers

This is one of the most important parts of the runtime for old Pascal code.

The prelude provides helpers for:

- byte reinterpretation
- storage aliasing
- untyped-`var` load/store
- typed storage re-viewing
- read/modify/write helpers for reinterpreted primitive storage

These helpers exist specifically so tp2cc does not need to form unsafe C++ references in many raw-storage cases.

### 4.7 Files, memory, process, and misc helpers

The runtime also carries higher-level helpers for:

- Pascal text and typed file operations
- memory allocation and disposal
- environment and process helpers
- numeric/string formatting helpers
- scope-exit support for `try .. finally`
- various old RTL compatibility entry points

## 5. Practical Reading Guide

If you want to understand a generated translation, the most useful mental model is:

- names are resolved by Pascal rules first
- types lower to explicit runtime carriers
- conversions are usually explicit helper calls
- raw-storage tricks are modeled deliberately, not left to accidental C++ aliasing
- when tp2cc cannot lower something safely, it increasingly prefers an explicit diagnostic over silent UB

In practice, the most important files are:

- `src/parser.cc` and `src/ast.h`: what syntax and AST shapes exist
- `src/typereg.cc`: what cross-unit/type information is remembered
- `src/emit.cc`: how actual lowering is done
- `tp2cc_rt/prelude.h`: what generated code is allowed to target
