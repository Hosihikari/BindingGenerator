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

`emit` writes `interop/` (C#), `shim/` (C++ BindingShim), and `coverage/` (manifest + summary). Pass `--native-root <path>` to emit the real BindingShim for the IR's source files:

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit \
  artifacts/llapi-full-ir/ir-manifest.json artifacts/llapi-full-ir artifacts/llapi-real-shim \
  --native-root C:/path/to/LeviLamina
```

Without `--native-root`, `emit` writes the contract stubs used by the fixture CI.

### Real BindingShim v1

- The C ABI is one versioned `HosihikariBindingTableV1` per carrier surface. Handles are `void*`; owned handles come from constructor factories and are released by the generated disposer. Field strings are returned as borrowed `HosihikariStringView` values that the managed side copies immediately; string parameters are `(const char*, size_t)` UTF-8 pairs.
- Constructors, destructors, public fields, and supported methods are projected. A method is eligible when it is inline, public, non-variadic, has at most one parameter, and returns `void`, a fundamental, or an opaque pointer. String-returning methods and string method parameters stay in the Interop stub set because their lifetime contract is not part of v1.
- Classes are projected only when they are non-template, public, complete in the IR source root, and link-safe: every method is inline and every base is link-safe. This keeps the shim linkable for header-only classes without pulling carrier symbols into the ABI. Abstract classes and classes with borrowed-string constructors do not receive factories.
- Every table function is a C++ function-try-block. Native exceptions are captured into a thread-local message exposed by `hosihikari_get_last_error`; no C++ exception crosses into managed code. Factory failures return a null handle and the generated `Create` methods throw `InvalidOperationException` with that message.
- `NativeMethods.Load(path)` validates the exported table version and `sizeof(Table)` before use. The table's `version` and `size` are the first two fields.

`frontend/build-shim.ps1` compiles the generated shim with clang-cl and `/MD` against the LeviLamina include environment. `frontend/smoke-shim.ps1` emits, builds, and runs the end-to-end smoke test in `tests/BindingShimSmoke` against the current IR.

## Local LLAPI slice

`frontend/run-local.ps1` parses `ll/api/event/Event.h`, writes IR under `artifacts/llapi-ir`, and validates it. `frontend/run-umbrella.ps1 -Module ll/api` parses the full `src/ll/api` plus `src-server/ll/api` umbrella, reuses the LeviLamina `compile_commands.json`, and currently yields 461 declarations / 1,394 members. The IR records each method's `inline`/`defined` facts, which the BindingShim planner uses for link safety.

```shell
powershell -File frontend/run-umbrella.ps1 -Module ll/api -Output artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir
```

The complete LLAPI umbrella currently selects 159 real BindingShim functions. It covers `ll::event::EventId`, `ll::event::EventIdView`, `ll::hash_utils::HashedIdBase`, `ll::hash_utils::HashCombiner`, and the other link-safe classes in the slice. The smoke test creates a native `EventId` from a managed string, reads its name and hash, compares two `HashedIdBase` views, and calls `HashCombiner::hash()` through the table. The remaining members keep their Interop stubs and are recorded in the coverage manifest with the `interop` emitter only.

## Licence

GPL-3.0-or-later, see `LICENSE.md`.
