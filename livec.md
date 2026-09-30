# LiveC

LiveC is an experimental C++20 runtime and native-code JIT built around one idea:

> **Edit the program. Keep the process alive.**

LiveC uses Clang to compile C++ translation units to LLVM IR and LLVM ORC to execute native code. The runtime discovers functions and types from compiler metadata, routes function calls through stable dispatch, and watches the project for edits automatically.

The compatibility goal is **ordinary C++ source and C++ ABI behavior** on documented target platforms. LiveC adds runtime reflection and live replacement; it does not define a separate C-like language.

---

## 1. Language and ABI contract

- C++20 is the initial language target. Projects use `.cpp`, `.cc`, or `.cxx` translation units and ordinary C++ headers.
- Clang owns C++ parsing, overload resolution, template instantiation, name mangling, and ABI lowering.
- The selected Clang, standard library, target triple, C++ ABI, exception/RTTI settings, and link options must be explicit and consistent with the host runtime.
- Undefined C++ behavior remains undefined. Hot reload does not make invalid object lifetimes, data races, or ODR violations safe.
- C++ classes and native function symbols keep their ordinary C++ layouts, names, and calling conventions. Reflection is provided by handles that describe those symbols.

Full C++20 compatibility is a staged goal. Current support is an experimental macOS/LLVM 22 prototype, verified by focused tests rather than a claim of complete standards conformance.

---

## 2. Native symbols and reflection handles

A native free function cannot have an attached data member such as `add.name` in standard C++. LiveC represents native symbols with reflection handles:

```cpp
auto add = livec::function<int(int, int)>("add");
std::cout << add.name << " " << add.signature << '\n';
std::cout << add(20, 22) << '\n';
```

The handle is a typed callable facade over the stable native dispatch stub. The underlying `add` function remains an ordinary C++ function.

Classes and types have handles too:

```cpp
auto player = livec::type("Player");
std::cout << player.name << " size=" << player.size << '\n';

for (const auto &field : player.fields)
    std::cout << field.name << " : " << field.type << '\n';

for (const auto &method : player.methods)
    std::cout << method.name << " " << method.signature << '\n';
```

Globals are queryable by name and expose type, source location, size, constness, and address:

```cpp
auto score = livec::variable("score");
std::cout << score.name << " = " << score.as<int>() << '\n';
```

The reflection API lives in `include/livec/reflection.hpp`. Metadata strings are copied into handles; function and variable addresses refer to runtime-managed symbols/storage. Respect `const` and C++ object lifetime rules when using addresses.

---

## 3. Runtime architecture

```text
C++ project files and headers
             │
             ▼
       Clang C++20 frontend
             │
             ▼
          LLVM IR + debug metadata
             │
             ▼
      LLVM ORC native JIT
       ┌─────┴─────────┐
       ▼               ▼
 function dispatch   persistent objects
 and versions        and globals
       │               │
       └──── reflection registry
```

Every discovered defined function receives a stable dispatch stub. A source edit recompiles the project into a candidate version; compatible implementations are then published to their stubs. Failed compilation retains the last valid version. Function signatures, type layouts, and existing object storage are validated before publication.

The runner watches C++ source and header files under the project root. No application-specific file watcher, reload target, or listener is required.

---

## 4. Live refresh semantics

- New calls use the currently installed implementation.
- The compiler inserts version checks at loop back-edges. An active loop whose function changed tail-calls the new implementation at its next back-edge, passing the original parameters.
- Automatic local variables are reinitialized when that function refreshes. Globals and compatible heap objects remain alive.
- An invocation without a loop safepoint can finish on its old version. Variadic, `noreturn`, `naked`, exception-cleanup, and nontrivial-local-destructor functions do not receive the loop-refresh transformation yet.
- Existing function ABI changes and live object-layout changes are rejected. Reflection does not make it safe to reinterpret existing objects with a different layout.
- C++ static initialization runs for the initial JIT load. Existing global objects are not reconstructed on reload; newly added globals with dynamic initializers are not yet supported.

This is live code replacement, not arbitrary stack-frame migration. Local state migration, virtual/member-function pointer adapters, and safe reclamation of old code versions remain future work.

---

## 5. Examples

### Refresh code in `main`

`examples/live-value/main.cpp` contains a loop and a local constant. Run it with `make demo-value`, then edit the constant and save. The JIT recompiles the project and the active loop enters the new `main` at its next back-edge.

### Reflect a C++ class and native functions

Run `make demo-reflection`. The example reflects and calls a native `add` function, inspects `Player` fields and methods, and reads a global `std::string` and integer global through reflection handles.

---

## 6. Current implementation limits

- macOS and the host architecture only; LLVM 22 ORC is the JIT backend.
- C++20 is compiled using Clang 22 and the macOS libc++ headers/runtime ABI.
- The entry point must be `int main()`.
- Projects are rebuilt as a unit after source/header edits; dependency-aware incremental compilation is future work.
- Function/type/global reflection is based on Clang-emitted debug metadata. Template-heavy standard-library metadata may be large; public reflection filters to definitions originating under the project root.
- Existing ABI/layout changes are rejected. New dynamic C++ global initializers, full exception/RTTI behavior across reloads, arbitrary `std::function` invocation, and object migration need further work.
- Old code versions are retained while the runtime is alive, so repeated reloads currently use additional memory.

---

## 7. Roadmap

### Implemented prototype foundation

- C++20 Clang frontend and LLVM ORC JIT.
- Automatic project source/header watching.
- Stable dispatch for discovered native functions.
- Transactional compile failures, persistent compatible globals, and loop safepoints.
- C++ reflection handles for functions, globals, classes, fields, and methods.

### Next runtime work

- Expand C++20 conformance, overload/ODR tests, standard-library interoperability, and target coverage.
- Make overload selection and typed invocation more ergonomic, including member-function invocation.
- Improve C++ initializer/destructor handling and constructor-safe state evolution.
- Add explicit migration for managed object layouts.
- Add concurrent execution safety, code-version reclamation, and incremental compilation.

---

## 8. Core principle

```text
C++ source retains its C++ meaning.
Native symbols remain native C++ symbols.
Reflection handles attach runtime metadata to those symbols.
Code versions can change while compatible process state remains alive.
```
