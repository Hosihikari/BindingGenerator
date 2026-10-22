# BDS symbol dispatch smoke

Runs the generated symbol dispatch shim inside a real Bedrock Dedicated Server. A LeviLamina `preload-native` mod hosts the CLR through `nethost`/`hostfxr`, loads the managed projection, and calls `BlockPos` symbols resolved by module base + RVA.

## Prerequisites

- Windows BDS **1.21.3.01** extracted into its own directory.
- LeviLamina **0.13.5** with PreLoader 1.9 and PeEditor 3.7 installed, so `bedrock_server_mod.exe` exists.
- A merged symbol IR for that server (the `Actor`/`BlockPos` slice produced by `run-symbols.ps1`, `run-levilamina-headers.ps1`, and `merge-symbol-ir.py`).
- .NET 10 and the MSVC toolchain.

The server should use a spare port and a local world, for example `server-port=19139`, `online-mode=false`, `level-name=symbol-smoke-world`. The run script starts the server, waits for the result file, and stops the process.

## Build

```powershell
./tests/bds-smoke/build.ps1 `
  -ServerRoot C:\path\to\bds-1.21.3 `
  -IrManifest artifacts/symbol-ir-1.21.3/merged-levi-final/ir-manifest.json
```

The script emits the `win-x64-server` dispatch shim with `--target-module bedrock_server_mod.exe`, compiles the shim and the preload plugin, builds `Hosihikari.SymbolSmoke` against the generated Interop, and deploys the plugin directory to `plugins/hosihikari-symbol-smoke`.

## Run

```powershell
./tests/bds-smoke/run.ps1 -ServerRoot C:\path\to\bds-1.21.3
```

Expected output:

```text
load=managed
a=Pos(1,2,3)
b=Pos(1,2,3)
c=Pos(1,2,4)
seedA=972575194
seedB=972575194
result=OK
```

The smoke verifies the real module base and RVAs, the MSVC member-return convention for `std::string`, CLR hosting inside the server process, and the generated C# wrapper (`Create`, `toString_*`, `randomSeed`, `Dispose`).

This test is not part of CI because it requires the proprietary 1.21.3 server and the matching LeviLamina loader. The PE/PDB fixture in `tests/symbol-dispatch` and the Linux fixture in `tests/fixtures/symbol-dispatch` cover the dispatch mechanics in CI.
