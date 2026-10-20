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
./frontend/run-umbrella.ps1 -LlvmRoot <llvm-root> -LeviLaminaRoot <checkout> -Module ll/api
```

`run-local.ps1` runs the frontend on `ll/api/event/Event.h`. `run-umbrella.ps1` builds an umbrella from the module's headers, reuses the include paths and defines from the LeviLamina `compile_commands.json`, and validates the produced IR with the .NET CLI.

## Current state

The frontend parses the full `src/ll/api` umbrella plus the `src-server/ll/api` overlays (1,514 declarations / 6,114 members in the current full IR) and the smaller `ll/api/event` module. It emits JSONL IR plus `ir-manifest.json` and validates the result through the .NET CLI. The visitor covers classes/structs/unions, fields, static fields, methods, constructors, destructors, operators, enums, namespace-scope functions, and concrete template instantiations. Macro provenance is inferred from export/deprecated/nodiscard attributes; uninstantiated template declarations and partial specializations are skipped.

## Dependency enums

Enums are often declared in headers outside a module filter while still appearing in filtered signatures. The visitor records such enums as dependency declarations after the main traversal, so the ABI planner can map their underlying integer type. Run the module that references them and merge the new declarations into the assembled IR:

```powershell
./frontend/run-umbrella.ps1 -Module ll/api/command -Output artifacts/llapi-enum-supplement
python frontend/merge-ir.py --base artifacts/llapi-full-ir-v15 --supplement artifacts/llapi-enum-supplement --output artifacts/llapi-full-ir-v16 --record-type declaration --kind enum
```

`merge-ir.py` copies only records matching the requested type/kind, skips ids already present, and recomputes the shard hash, record count, and manifest counts.

The frontend is not part of the .NET CI job; it requires the pinned LLVM toolchain, zlib/zstd, and the MSVC/SDK headers. `build.ps1`, `run-local.ps1`, and `run-umbrella.ps1` cover the local Windows slice.
