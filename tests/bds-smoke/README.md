# BDS symbol dispatch smoke

Runs the generated symbol dispatch shim inside a real Bedrock Dedicated Server. A LeviLamina `preload-native` mod hosts the CLR through `nethost`/`hostfxr`, loads the managed projection, and calls value-type symbols resolved by module base + RVA. It also reads `ServiceLocator<AppPlatform>::mService` from PeEditor's exported data symbols and calls virtual getters on the live server object.

## Prerequisites

- Windows BDS **1.21.3.01** extracted into its own directory.
- LeviLamina **0.13.5** with PreLoader 1.9 and PeEditor 3.7 installed, so `bedrock_server_mod.exe` exists.
- A merged symbol IR for that server. The harness is exercised with the expanded class set `Actor,BlockPos,Vec3,ChunkPos,Level,ItemStack,BlockSource,AppPlatform,ServerInstance` produced by `run-symbols.ps1`, `run-levilamina-headers.ps1`, and `merge-symbol-ir.py`.
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
a2=Pos(7,2,3)
moved=Pos(7,1,3)
rebuilt=Pos(7,1,3)
seedA=972575194
seedB=972575194
chunk=(1,3,3)
vec=(16,64,48)
length=81.58431
normalized=1
absolute=81.58431
platform=0x34B482203F0
edu=True
maxRequests=0
screenX=0
addresses=0
address=
broadcast=0
userInput=0
stable=True
result=OK
```

The smoke verifies the real module base and RVAs, the MSVC member-return convention for `std::string` and by-value records, inherited field access (`x`/`y`/`z` read and `a.x = 7` write), `BlockPos::neighbor`, `ChunkPos` construction from `BlockPos`, `ChunkPos::toBlockSpaceVec3` returning `Vec3`, `Vec3` field reads and `length`/`normalized`/`abs`, CLR hosting inside the server process, the generated C# wrapper (`Create`, `randomSeed`, `Dispose`), and virtual calls on the live `AppPlatform` object (`isEduMode` slot 138, `getMaxSimultaneousServiceRequests` slot 79, `getDefaultScreenPositionX` slot 98) with stable repeat results. It also calls the `std::vector<std::string>` getters `getIPAddresses`, `getBroadcastAddresses`, and `getUserInput` through their virtual slots, exercising the hidden sret buffer and the thread-local array view; this server returns empty arrays, and two `getIPAddresses` calls compare equal.

`Level` itself is not reachable in this setup: LeviLamina's hook-based `ll::service::bedrock::getLevel()` returns an empty reference here, while BDS's own `ServiceLocator<AppPlatform>::mService` is populated. The AppPlatform object provides the live vtable/RTTI check; Level-level tests need a loader path that exposes the Level service.

This test is not part of CI because it requires the proprietary 1.21.3 server and the matching LeviLamina loader. The PE/PDB fixture in `tests/symbol-dispatch` and the Linux fixture in `tests/fixtures/symbol-dispatch` cover the dispatch mechanics in CI.
