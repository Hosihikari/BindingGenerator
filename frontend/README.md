# libTooling frontend

The native frontend parses the three header sets and emits the versioned IR defined in `Hosihikari/Design` (`docs/ir-and-coverage.md`). It is a separate CMake target because libTooling has no stable C++ API and cannot be hosted in-process by .NET 10.

## Toolchain

Pin LLVM/Clang **23.1.3**.

- **Windows**: the LLVM MSI is toolchain-only and does not contain `clang/Tooling` headers, `clangTooling.lib` or `ClangConfig.cmake`. Use the official `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz` (or build LLVM), and point `CMAKE_PREFIX_PATH` at the extracted tree.
- **Linux**: use the official 23.1.3 tarball or build LLVM with `LLVM_ENABLE_PROJECTS=clang`. Distribution `libclang-23-dev` may be a different patch level; do not mix.

## Build

```powershell
./frontend/build.ps1 -LlvmRoot <llvm-root>
```

The script expects the extracted `clang+llvm-23.1.3-x86_64-pc-windows-msvc` tree and uses the Visual Studio 18 generator. A manual CMake invocation works too:

```shell
cmake -S frontend -B build/frontend -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH=<llvm-root>
cmake --build build/frontend --config Release
```

## Run

```powershell
./frontend/run-local.ps1 -LlvmRoot <llvm-root> -LeviLaminaRoot <checkout>
```

The script locates MSVC and the Windows SDK, runs the frontend on `ll/api/event/Event.h`, and validates the produced IR with the .NET CLI.

## Current state

The frontend parses `ll/api/event/Event.h`, emits JSONL IR plus `ir-manifest.json`, and validates the result through the .NET CLI. The visitor covers classes/structs/unions, fields, static fields, methods, enums, and namespace-scope functions; constructors, destructors, operators, template instantiations, macro provenance, and the full umbrella are not emitted yet.

The frontend is not part of the .NET CI job; it requires the pinned LLVM toolchain, zlib/zstd, and the MSVC/SDK headers. `build.ps1` and `run-local.ps1` cover the local Windows slice.
