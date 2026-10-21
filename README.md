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
| `tests/fixtures/` | Minimal IR fixtures used by the CLI contract and real-shim jobs |
| `tests/BindingShimSmoke` | End-to-end Windows smoke for the LLAPI slice |
| `tests/RealShimSmoke` | CI smoke for the link-carrier fixture |

## Commands

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- version
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- validate-ir tests/fixtures/basic/ir-manifest.json tests/fixtures/basic
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit tests/fixtures/basic/ir-manifest.json tests/fixtures/basic artifacts/emitted --abi-header ../Abi/native/hosihikari_abi.h
```

`validate-ir` reads the manifest and every `*.jsonl` under the shard directory, then runs the IR validator. `validate-coverage` does the same for `coverage/summary.json` + `coverage/manifest.jsonl`.

`emit` writes `interop/` (C#), `shim/` (C++ BindingShim), and `coverage/` (manifest + summary). Pass `--native-root <path>` to emit the real BindingShim for the IR's source files:

```shell
dotnet run --project src/Hosihikari.Bindings.Generator/Hosihikari.Bindings.Generator.csproj -c Release -- emit \
  artifacts/llapi-full-ir/ir-manifest.json artifacts/llapi-full-ir artifacts/llapi-real-shim \
  --native-root C:/path/to/LeviLamina --abi-header ../Abi/native/hosihikari_abi.h
```

`--abi-header` points at `Hosihikari/Bindings.Abi`'s `native/hosihikari_abi.h`; the emitter copies it into every shim directory so the shim build stays self-contained. Generated C# references `Hosihikari.Bindings.Abi` for `StringView`, `ArrayView`, `SharedHandle`, and the version-checking `ShimLibrary` loader.

Without `--native-root`, `emit` writes the contract stubs used by the fixture CI.

### Real BindingShim v1

- The C ABI is one versioned `HosihikariBindingTableV1` per carrier surface. Handles are `void*`; owned handles come from constructor factories and are released by the generated disposer. Table fields are named from the stable function identity (for example `hs_method_1a2b3c4d`), not from the slot ordinal; the ordinal remains only the physical array position, so adding a function does not rename existing call sites. Field strings are returned as borrowed `HosihikariStringView` values that the managed side copies immediately; string parameters are `(const char*, size_t)` UTF-8 pairs.
- Constructors, destructors, public fields, and supported methods are projected. Members that the generator deliberately refuses are emitted as `NotSupportedException` stubs marked with `[Obsolete]` carrying the same reason; calling one produces compiler warning CS0618 and an IDE strikethrough, and projects can escalate it with `<WarningsAsErrors>CS0618</WarningsAsErrors>`. Constructors and methods accept multiple parameters. Constructors may take mutable and rvalue references to public declarations that have generated wrappers; the shim passes those parameters by dereferenced address. A method may return `void`, a fundamental, an enum, an opaque pointer, a string, or an eligible by-value record; string results are copied into a thread-local shim buffer and the managed wrapper copies them immediately. Multiple string parameters are pinned independently per call. Enums use their underlying integer ABI. By-value record arguments are dereferenced from caller handles; by-value record results are copied into shim-owned objects that the managed wrapper can dispose. Record and container fields are exposed through borrowed address views; array fields are getter-only. `std::vector<T>` results use a borrowed `HosihikariArrayView` and surface as `T[]`/`string[]`/`nint[]`, including pointer elements; vector parameters expand to a hidden `(element pointer, count)` pair and surface as `T[]`/`string[]`/`Variant[]`, including record and variant elements, with multiple vector parameters per call; each vector gets its own pinned element view; `std::initializer_list<T>` parameters stay unsupported because C++ cannot construct a runtime-length initializer list, but the affected classes expose equivalent vector overloads; `std::vector<std::pair<string, uint64>>` parameters surface as `KeyValuePair<string, ulong>[]`; `std::optional<T>` parameters and results use a hidden presence flag; managed values are nullable scalars, `string?`, or owned `T?`; `std::unique_ptr<T>` results release ownership as an owned `T` handle; `unique_ptr` parameters transfer ownership from the wrapper, which releases its handle before the call and throws if reused; abstract interfaces with a public virtual destructor receive a disposer when returned this way. `std::shared_ptr<T>` results return a `SharedHandle` that keeps one shared reference alive and releases it on dispose; `shared_ptr`/`weak_ptr` parameters accept a `SharedHandle`, and the shim recovers the typed reference through a type-checked holder. `optional_ref<T>` parameters and results use the optional presence flag with borrowed references; managed values are `T?`/`nint?`. `nonstd::expected_lite::expected<T, E>` results return `T` and surface failures as `InvalidOperationException` carrying the native error message; `E` may be `ll::Error` or a string. `std::variant<T...>` values with string, scalar/enum, pointer, and copyable-record alternatives receive a generated owned wrapper with per-alternative `Create`/`As`/`Is`, `Index`, and `Dispose`; by-value parameters copy, `const&` parameters borrow, and by-value results copy into an owned handle. A public copyable class whose members are inherited (no recorded fields, constructors, or destructor) is accepted as a record alternative; the generator emits the destroy function needed by its `Dispose`. `endstone::NotNull<T>`/`Nullable<T>` returns use `SharedHandle`; sequence returns distinguish borrowed pointer elements (`nint[]`), owned elements (`T[]`/`T?[]` for records and `unique_ptr`), and shared elements (`SharedHandle[]` for `shared_ptr`/`NotNull`/`Nullable`); `unordered_set<T*>` returns borrowed address arrays. `std::chrono::time_point<std::chrono::system_clock>` maps to Unix milliseconds; other clocks map to their native tick count; `std::chrono::duration` maps to its native count; `optional` time values use `long?`. Returned `std::function<R(Args...)>` values become generated callable wrappers with `Invoke` and `Dispose`. Reference returns of variant, callable, chrono, `typeid_t`, `strong_ordering`, and filesystem-path values are copied through the same projection instead of surfacing as opaque handles; pointers to those types remain borrowed handles. `Bedrock::typeid_t<C>` maps to `ushort`, `std::strong_ordering` to `sbyte` (-1/0/1), and `std::filesystem::path` to a UTF-8 string; optional forms follow the scalar/string rules. `std::unordered_map<K, V>` parameters and results use a managed `Dictionary<K, V>`; keys support string/scalar/pointer and values support string/scalar/pointer, record copies, and variant copies. Reference and pointer results keep a borrowed handle when the entry types cannot be projected.
- The default link-safe mode projects public non-template classes that are complete in the IR source root and have only inline methods and link-safe bases. `--link-carrier` also admits public concrete template instances and links the carrier import library, fmt, and prelinked Bedrock imports. Template arguments must be accessible; incomplete arguments are admitted only for standard or supported third-party types. Member templates, rvalue-qualified members, and variable templates are not projected, and a template destructor must have a visible definition before a disposer is emitted. Static methods and static fields are projected without `self`. Abstract classes do not receive factories. Classes that store `string_view` fields receive holder-backed factories: the shim copies each string argument into a holder and constructs the object against those copies, so the views stay valid until the wrapper is disposed.
- Every table function is a C++ function-try-block. Native exceptions are captured into a thread-local message exposed by `hosihikari_get_last_error`; no C++ exception crosses into managed code. Factory failures return a null handle and the generated `Create` methods throw `InvalidOperationException` with that message.
- `std::function<R(Args...)>`, `brstd::move_only_function<R(Args...)>`, and `brstd::function_ref<R(Args...)>` parameters are exposed as `(invoke, userData, release)` triples. The shim wraps a shared callback state in the requested callable type (`std::function`, `brstd::move_only_function`, or `brstd::function_ref`); the last copy invokes `release(userData)`. v1 supports scalar and enum results with scalar, enum, string, pointer, reference, and by-value record arguments; const scalar, enum, and string references cross as values.
- `--ll-allocator` routes the shim's global `operator new`/`delete` through `ll::memory::getDefaultAllocator()`. Use it with carriers such as LeviLamina that replace the global allocator; mock carrier fixtures omit it.
- `NativeMethods.Load(path)` validates the exported table version and `sizeof(Table)` before use. The table's `version` and `size` are the first two fields.

`frontend/build-shim.ps1` compiles the generated shim with clang-cl and `/MD` against the LeviLamina include environment. Pass `-AdditionalLinkLibrary` for carrier/package libs and `-DelayLoadLibrary LeviLamina.dll` for a standalone smoke build. `frontend/build-carrier-shim.ps1` compiles the object, runs `prelink` to generate the Bedrock runtime import library, and links the carrier shim. `frontend/smoke-shim.ps1` emits, builds, and runs the end-to-end smoke test in `tests/BindingShimSmoke`; add `-LinkCarrier` to use the carrier build with delay-loading.

## Local LLAPI slice

`frontend/run-local.ps1` parses `ll/api/event/Event.h`, writes IR under `artifacts/llapi-ir`, and validates it. `frontend/run-umbrella.ps1 -Module ll/api` parses the full `src/ll/api` plus `src-server/ll/api` umbrella, reuses the LeviLamina `compile_commands.json`, and currently yields 1,530 declarations / 6,145 members at IR 1.2.0. Public record types referenced by value from filtered signatures are added with their constructors, destructor, and fields even when their headers sit outside the module filter. The IR records each method's `inline`/`defined`/`consteval` facts, member templates, ref qualifiers, enum underlying types, and class traits (`abstract`, `copyConstructible`), which the BindingShim planner uses for eligibility and ownership.

```shell
powershell -File frontend/run-umbrella.ps1 -Module ll/api -Output artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir
powershell -File frontend/smoke-shim.ps1 -IrDirectory artifacts/llapi-full-ir -LinkCarrier
```

The complete LLAPI umbrella currently selects 214 link-safe functions, or 3,219 functions when built with `--link-carrier` against `LeviLamina.lib`. The slice covers `ll::event::EventId`, `ll::event::EventIdView`, `ll::hash_utils::HashedIdBase`, `ll::hash_utils::HashCombiner`, `ll::data::Version`, and the other eligible classes. The smoke test creates a native `EventId` from a managed string, reads its name and hash, compares two `HashedIdBase` views, calls `HashCombiner::hash()`, and round-trips `ll::data::Version` through string construction, `to_string()`, field getters, `valid(string)`, and `isIdenticalTo(Version)`. The remaining members keep their Interop stubs and are recorded in the coverage manifest with the `interop` emitter only.

## Endstone surface

`frontend/run-endstone.ps1` parses the Endstone public umbrella (`include/endstone/endstone.hpp`) with expected-lite and nlohmann_json headers. `--virtual-dispatch` emits a link-free shim for the header-only virtual interfaces: inline members and virtual methods are called through the host-provided vtables, while non-virtual out-of-line members remain stubs. `frontend/build-endstone-shim.ps1` compiles the generated table against the Endstone headers. The Endstone 0.11.14 surface currently selects 1,838 functions, including eligible template instances and inline/constexpr static fields.

## Related repositories

- `Hosihikari/Bindings.Ir`: schema, models, reader/writer, validator.
- `Hosihikari/Bindings.Abi`: shared ABI value types, loader, and native header.
- `Hosihikari/Bindings.Endstone`: the Endstone surface.
- `Hosihikari/Bindings.LeviLamina`: the LeviLamina surface.

## Licence

GPL-3.0-or-later, see `LICENSE.md`.
