# Hosihikari.Bindings.Generator

[English](README.md) | [简体中文](README.zh_hans.md)

The generator turns the versioned IR into the C ABI **BindingShim** and the native-style C# **Interop**. It does not produce the hand-written .NET Idiomatic layer.

## Boundary

- **Input**: IR shards produced by the libTooling frontend, plus the IR library contract.
- **Output**: generated C++ (BindingShim) and C# (Interop) sources. Both are committed in the binding repositories and verified by a CI diff.
- **Not here**: the hand-written Idiomatic wrappers, the HostingShim, or any CLR hosting.
- The IR library is a sibling checkout (`../Ir`); in CI it is pulled with `actions/checkout` using a pinned `repository`/`path`.

## Projects

| Path | Purpose |
|---|---|
| `src/Hosihikari.Bindings.Generator` | .NET CLI: version, IR validation, coverage validation |
| `frontend/` | libTooling CMake target (LLVM 23.1.3) |
| `tests/fixtures/` | Minimal IR fixture used by the CLI contract job |

## Commands

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- version
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- validate-ir tests/fixtures/basic/ir-manifest.json tests/fixtures/basic
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit tests/fixtures/basic/ir-manifest.json tests/fixtures/basic artifacts/emitted
```

`validate-ir` reads the manifest and every `*.jsonl` under the shard directory, then runs the IR validator. `validate-coverage` does the same for `coverage/summary.json` + `coverage/manifest.jsonl`.

`emit` writes `interop/` (C# stubs), `shim/` (C++ binding-table stubs), and `coverage/` (manifest + summary). The stubs establish the IR-to-output contract; the BindingShim calls are not wired yet.

## Local LLAPI slice

`frontend/run-local.ps1` parses `ll/api/event/Event.h`, writes IR under `artifacts/llapi-ir`, and validates it. `frontend/run-umbrella.ps1 -Module ll/api` parses the full `src/ll/api` plus `src-server/ll/api` umbrella, reuses the LeviLamina `compile_commands.json`, and currently yields 461 declarations / 1,394 members. `emit` then writes the Interop, BindingShim and coverage stubs under `artifacts/`; the generated C# for this slice compiles.

## Licence

GPL-3.0-or-later, see `LICENSE.md`.
