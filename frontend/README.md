# libTooling frontend

The native frontend parses the three header sets and emits the versioned IR defined in `Hosihikari/Design` (`docs/ir-and-coverage.md`). It is a separate CMake target because libTooling has no stable C++ API and cannot be hosted in-process by .NET 10.

## Toolchain

Pin LLVM/Clang **23.1.3**.

- **Windows**: the LLVM MSI is toolchain-only and does not contain `clang/Tooling` headers, `clangTooling.lib` or `ClangConfig.cmake`. Use the official `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz` (or build LLVM), and point `CMAKE_PREFIX_PATH` at the extracted tree.
- **Linux**: use the official 23.1.3 tarball or build LLVM with `LLVM_ENABLE_PROJECTS=clang`. Distribution `libclang-23-dev` may be a different patch level; do not mix.

## Build

```shell
cmake -S . -B ../../build/frontend -DCMAKE_PREFIX_PATH=<llvm-root>
cmake --build ../../build/frontend --config Release
```

## Current state

`src/main.cpp` is a scaffold: it proves the CMake/libTooling link shape and prints a diagnostic. The next step is to add an `ASTFrontendAction` + `ASTConsumer` + `RecursiveASTVisitor` with `shouldVisitTemplateInstantiations()` and a `PPCallbacks`/`MacroExpansionContext` sidecar, then write JSONL shards.

The frontend is not part of the .NET CI job yet; it requires the pinned LLVM toolchain and the upstream header checkouts.
