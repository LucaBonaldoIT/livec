# LiveC

LiveC is an experimental **C++20** runtime with an LLVM ORC native-code JIT, automatic project watching, and hot code replacement. It compiles project translation units with Clang, creates dispatch for native function symbols, and refreshes active loops at compiler-inserted safepoints. C++ code needs no LiveC listener or reload annotation.

## Build and test

Requirements: macOS, LLVM 22/Clang 22, the macOS C++ standard library headers, Make, and Python 3.

```sh
brew install llvm@22
make
make test
```

The host uses the macOS libc++ ABI; LiveC compiles application code against the matching SDK headers. `LLVM_CONFIG`, `CLANG`, and `CLANG_CXX_INCLUDE` can override the detected toolchain paths.

## Run a C++ project

Pass the C++ `main` source. LiveC discovers `.cpp`, `.cc`, and `.cxx` files beside it recursively, watches C++ sources and headers, and rebuilds the project after edits:

```sh
build/livec --clang-arg -I --clang-arg include examples/live-value/main.cpp
```

Use `--watch-root DIR` for a different project root. Pass extra Clang options with `--clang-arg ARG`; load a native dynamic library with `--link-library PATH`. Use `--quiet` or `-q` to suppress informational LiveC messages while retaining program output and compiler/runtime errors.

There is no application-side listener. Definitions from all discovered translation units are compiled automatically, and calls go through stable dispatch. A failed compile leaves the previous valid code installed.

## Live edit example

Run `make demo-value`. The loop in `examples/live-value/main.cpp` computes and prints a value. Change its local constant while the program runs; the updated `main` takes over at the next loop back-edge without restarting the process.

## Native-symbol reflection

Include `<livec/reflection.hpp>` to enumerate functions, globals, and types. Reflection handles expose native names, signatures, source locations, versions, callable entries, variable addresses, class fields, and methods.

```cpp
auto add = livec::function<int(int, int)>("add");
std::cout << add.name << " = " << add(20, 22) << '\n';

auto player = livec::type("Player");
for (const auto &field : player.fields)
    std::cout << field.name << " : " << field.type << '\n';

auto score = livec::variable("score");
std::cout << score.name << " = " << score.as<int>() << '\n';
```

Run the complete reflection example with `make demo-reflection`. A native free function remains a normal C++ function symbol; its reflection handle is the object that carries `.name` and can be called with a typed signature.

## Current boundaries

- macOS and host architecture only; LLVM 22 ORC is the JIT backend.
- The entry point must be `int main()`.
- At loop back-edges, an active function whose implementation changed tail-calls its new version with the original parameters. Its automatic locals are reinitialized; globals and compatible C++ objects persist. Functions without loop safepoints finish their current invocation on the old version.
- Functions with C++ exception cleanups or nontrivial local destructors do not receive active-loop migration yet; their current invocation finishes on the old version.
- Function signature changes get a distinct dispatch identity, so new callers compiled against the new signature use its implementation while old-ABI callers keep the old version. Update declarations in the relevant headers/translation units; live object-layout changes are rejected. New C++ static initializers are not rerun on reload.
- Translation units are rebuilt together after each change; old code versions are retained for safe active calls.

See [`livec.md`](livec.md) for the language contract and roadmap.
