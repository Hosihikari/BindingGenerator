# Hosihikari.Bindings.Generator

[English](README.md) | [简体中文](README.zh_hans.md)

The generator turns the versioned IR into the C ABI **BindingShim** and the native-style C# **Interop**. It does not produce the hand-written .NET Idiomatic layer.

## Boundary

- **Input**: IR shards produced by the libTooling frontend, plus the IR library contract.
- **Output**: generated C++ (BindingShim) and C# (Interop) sources. Both are committed in the binding repositories and verified by a CI diff.
- **Not here**: the hand-written Idiomatic wrappers, the HostingShim, or any CLR hosting.
- The IR library is a sibling checkout (`../Ir`); in CI it is pulled with `actions/checkout` using a pinned `repository`/`path`, following `HosihikariFramework/PluginManager/.github/workflows/verify.yml`.
- Design rationale: the internal `Hosihikari/Design` repository (private), ADRs 0001/0003/0004/0008/0013.

## Projects

| Path | Purpose |
|---|---|
| `src/Hosihikari.Bindings.Generator` | .NET CLI: version, IR validation, coverage validation |
| `frontend/` | libTooling CMake target (LLVM 23.1.3); currently a scaffold |
| `tests/fixtures/` | Minimal IR fixture used by the CLI contract job |

## Commands

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- version
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- validate-ir tests/fixtures/basic/ir-manifest.json tests/fixtures/basic
```

`validate-ir` reads the manifest and every `*.jsonl` under the shard directory, then runs the IR validator. `validate-coverage` does the same for `coverage/summary.json` + `coverage/manifest.jsonl`.

## Next steps

1. Implement the libTooling AST consumer and JSONL writer (`frontend/`).
2. Add the LeviLamina first-slice umbrella/chunk TUs and xmake `compile_commands.json` ingestion.
3. Add the C++ BindingShim emitter, then the C# Interop emitter.
4. Wire the generated output into the LeviLamina binding repository and the coverage gate.

## Licence

GPL-3.0-or-later, see `LICENSE.md`.
