# libTooling frontend

The native frontend parses the three header sets and emits the versioned IR defined in `Hosihikari/Design` (`docs/ir-and-coverage.md`). It is a separate CMake target because libTooling has no stable C++ API and cannot be hosted in-process by .NET 10.

## Toolchain

Pin LLVM/Clang **23.1.3**.

- **Windows**: the LLVM MSI is toolchain-only and does not contain `clang/Tooling` headers, `clangTooling.lib` or `ClangConfig.cmake`. Use the official `clang+llvm-23.1.3-x86_64-pc-windows-msvc.tar.xz` (or build LLVM), and point `CMAKE_PREFIX_PATH` at the extracted tree.
- **Linux**: use the official `LLVM-23.1.3-Linux-X64.tar.xz`. Distribution `libclang-23-dev` may be a different patch level; do not mix.

## Build

```powershell
./frontend/build.ps1 -LlvmRoot <llvm-root>
```

```shell
./frontend/build.sh --llvm-root <llvm-root>
```

The Windows script expects the extracted `clang+llvm-23.1.3-x86_64-pc-windows-msvc` tree and uses the Visual Studio 18 generator. The Linux script uses Ninja when available and builds `build/frontend-linux/hosihikari-ir-frontend`. A manual CMake invocation works too:

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

For the Endstone Linux configuration:

```shell
./frontend/run-endstone.sh \
  --endstone-root <checkout> \
  --llvm-root <llvm-root> \
  --expected-lite-include <expected-lite/include> \
  --nlohmann-include <nlohmann-json/include> \
  --output artifacts/endstone-linux-ir
```

The script emits `linux-x64-server` IR and validates it with the .NET CLI. On libstdc++, `--compat-include` can force-include headers such as the Endstone `std::floorf` compatibility shim from `Hosihikari/Bindings.Endstone`.

## Symbol prototype (Bedrock 1.21.3)

`hosihikari-symbol-ir-frontend` reads a publics-only PDB and an optional ELF symbol table, demangles a selected set of classes, merges Windows return types into the Linux records, and reconstructs vtable slots. `--output` writes a prototype JSON; `--ir-output` writes formal IR 1.5.1 shards and a manifest with `symbol`, `address`, `rva`, and per-configuration vtable records. Omitting `--elf` emits the Windows configuration alone, which the synthetic fixture uses. See `symbols/README.md`.

```powershell
./frontend/run-symbols.ps1 -Pdb <pdb> -Exe <pe> -Elf <debug> -Classes Actor,BlockPos
```

`frontend/run-levilamina-headers.ps1` generates a header supplement from a LeviLamina v0.13.5 checkout targeting BDS 1.21.3.01. It passes `--flatten-anonymous-fields`, which emits fields nested in anonymous unions/structs with absolute bit offsets; the flag is off by default so existing IRs stay unchanged. `frontend/merge-symbol-ir.py` then merges fields, layouts, inheritance, and inline methods from that IR into the symbol IR.

The generator's `emit --symbol-dispatch` mode turns that IR into a shim that does not include game headers: direct calls use the module base plus `rva`/`address` (or `GetProcAddress`/`dlsym` on the recorded symbol), and virtual calls use the per-configuration `vtableIndex`. `tests/symbol-dispatch/run.ps1` exercises the Windows PE/PDB path end-to-end with a synthetic fixture; the `cli` CI job runs the Linux fixture and resolver smoke.

## Current state

The frontend parses the full `src/ll/api` umbrella plus the `src-server/ll/api` overlays (1,530 declarations / 6,145 members in the current full IR) and the smaller `ll/api/event` module. It emits JSONL IR plus `ir-manifest.json` and validates the result through the .NET CLI. The visitor covers classes/structs/unions, fields, static fields, methods, constructors, destructors, operators, enums, namespace-scope functions, and concrete template instantiations. Public record types referenced by value from filtered signatures are emitted with their constructors, destructor, and fields even when declared outside the module filter. Macro provenance is inferred from export/deprecated/nodiscard attributes; uninstantiated template declarations and partial specializations are skipped.

Type canonical strings are platform-neutral for identity purposes. On targets where `long` is 64-bit, canonical `long`/`unsigned long` spellings are rewritten to `long long`/`unsigned long long`, matching the Windows spelling of `std::int64_t`, `std::uint64_t`, and `size_t`. Declaration ids, table field hashes, and coverage therefore stay identical between `win-x64-server` and `linux-x64-server` IR.

## Full IR

```powershell
./frontend/run-ll-full.ps1 -Output artifacts/llapi-full-ir
```

`run-ll-full.ps1` wraps `run-umbrella.ps1 -Module ll/api`, which parses the complete `src/ll/api` plus `src-server/ll/api` tree in one translation unit and validates the result. Dependency enums declared outside the filter are emitted automatically.

## Dependency enums

Enums are often declared in headers outside a module filter while still appearing in filtered signatures. The visitor records such enums as dependency declarations after the main traversal, so the ABI planner can map their underlying integer type. When an IR is assembled from separately parsed modules, merge the missing declarations with:

```powershell
python frontend/merge-ir.py --base <base-ir> --supplement <module-ir> --output <merged-ir> --record-type declaration --kind enum
```

`merge-ir.py` copies only records matching the requested type/kind, skips ids already present, and recomputes the shard hash, record count, and manifest counts.

The frontend is not part of the .NET fixture job; it requires the pinned LLVM toolchain, zlib/zstd, and the MSVC/SDK or libstdc++ headers. The `endstone-linux-ir` CI job builds it on Linux, generates `linux-x64-server` IR from Endstone 0.11.14, emits from that IR, and compiles the resulting shim.

`build.ps1`, `run-local.ps1`, and `run-umbrella.ps1` cover the local Windows slice; `build.sh` and `run-endstone.sh` cover the Linux slice.
