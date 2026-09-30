# Live OS: Vision and Design Considerations

## The idea

The long-term goal is an operating system whose running program can continue to evolve while it is in use. Developers edit C++ source, the runtime compiles the change to native code, and the updated behavior appears in the existing process without a stop/build/restart cycle.

The source project remains the authoritative description of the system. The running OS is a persistent environment for that source—not merely the output of a one-time build.

That could enable workflows such as:

- Edit a desktop panel and see the new behavior in the next frame.
- Add a module and immediately inspect its functions, types, fields, and globals.
- Change a service implementation while keeping the service process and compatible state alive.
- Evolve parts of the kernel while the rest of the machine continues running.

This is more than compiling C++ with a JIT. It combines native C++ semantics with stable runtime identities, automatic project watching, versioned function dispatch, persistent state, reflection, and safe publication of code changes.

## Language and reflection model

The language direction is C++20. LiveC aims to preserve ordinary C++ source, overload resolution, classes, templates, virtual dispatch, and the platform C++ ABI. C++ remains the language; LiveC adds a runtime model around it.

A native free function does not acquire a `.name` member in standard C++. Reflection therefore uses callable handles:

```cpp
auto add = livec::function<int(int, int)>("add");
std::cout << add.name << " = " << add(20, 22) << '\n';

auto player = livec::type("Player");
for (const auto &field : player.fields)
    std::cout << field.name << " : " << field.type << '\n';
```

The handle refers to a native symbol through stable dispatch. Types can expose fields and methods, and variable handles can inspect compatible globals. Overloads are selected by signature. Class/member-function invocation, richer type metadata, and object-aware value access can grow from this foundation.

## What “fully JITted” can mean

A practical OS could ship its kernel implementation as C++ source or an intermediate representation and JIT-compile it to native machine code during boot. In that sense, the kernel would have no ahead-of-time compiled kernel image: its native kernel code would be produced by the runtime.

There must still be a bootstrap path. The processor starts in firmware, then a bootloader or a small stable bootstrap must establish enough execution environment to start the JIT. Some seed code is unavoidable; the kernel proper can still be JIT-generated.

The JIT must be available before any code that depends on it. A plausible bootstrap sequence is:

```text
Firmware / bootloader
        ↓
Small native bootstrap
        ↓
CPU mode, page tables, memory allocator, console
        ↓
LiveC runtime and native JIT
        ↓
JIT-compiled kernel subsystems and services
```

The bootstrap can remain intentionally small and stable. Drivers, service logic, desktop components, and eventually large portions of the kernel can be compiled and replaced by LiveC.

## Safe live-update semantics

“Immediate” should mean that new calls use the new implementation as soon as a successful candidate is published, with active loops switching at compiler-inserted safe points. It cannot mean overwriting instructions while another CPU is executing them or changing an active stack frame arbitrarily.

Important rules include:

- Compile and validate a candidate away from execution-critical paths.
- Publish compatible function versions atomically; retain old code while active frames or pointers can still reach it.
- Give different ABI signatures distinct dispatch identities. New callers compiled against an updated declaration use the new signature; old callers retain the old ABI version until they are rebuilt or finish.
- Preserve existing global and heap state when its layout remains compatible.
- Reject incompatible object-layout changes unless an explicit migration is available. Reflection alone cannot update raw pointers to an old object layout.
- At a loop safepoint, a refreshed function starts with its original parameters; automatic locals are reinitialized. General stack-frame/local migration is a separate, much harder capability.
- Keep compilation, memory allocation, and code reclamation out of interrupt and other hard real-time paths.
- Keep the previous valid version active if compilation or validation fails.

Kernel updates need additional care around interrupts, callbacks, virtual dispatch, exceptions, thread-local storage, static initialization/destruction, and concurrent execution. A desktop UI can tolerate more flexible refresh points than a scheduler or interrupt handler.

## A sensible development path

1. **C++ project JIT:** run `main`, discover project translation units, JIT native code, and watch source/header changes automatically.
2. **Runtime reflection:** inspect functions, overloads, globals, classes, fields, methods, source locations, and versions through C++ handles.
3. **State/version rules:** expand compatibility checks, class layout metadata, explicit migrations, and safe code reclamation.
4. **Live desktop environment:** use the runtime in user-space UI and services, where a fast edit/observe loop proves the workflow.
5. **Freestanding target:** add a kernel target, linker/runtime support, and a minimal native bootstrap. Avoid hosted OS dependencies after bootstrap.
6. **JIT kernel subsystems:** move suitable drivers and services to JIT-managed code; keep low-level entry paths constrained and explicitly safe.
7. **Concurrent kernel updates:** add stop-the-world or per-thread safepoints, stack/frame policies, rollback, and multi-core code reclamation.

The current prototype is at the project-JIT and initial-reflection stages. It targets macOS with LLVM ORC; it is not yet a freestanding compiler/runtime or a kernel.

## North star

```text
The OS keeps running.
The C++ source remains its live definition.
Reflection describes native code and state.
Edits become new versions at safe points.
Compatible state survives those edits.
```

The ambitious payoff is an OS that can be reshaped from the inside while its process, services, and compatible state remain alive.
