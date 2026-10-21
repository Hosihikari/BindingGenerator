# Symbol IR prototype (Bedrock 1.21.3)

Extracts a prototype symbol-level IR from a publics-only PDB and an ELF symbol table. It exists to establish the third API surface without header assumptions; the formal IR schema is not changed yet.

## Inputs

- `--pdb`: the 1.21.3 `bedrock_server.pdb` (publics/globals, vtable and RTTI symbols; the TPI type stream is empty).
- `--exe`: the matching `bedrock_server.exe`, used for section RVAs and vtable bytes.
- `--elf`: `bedrock_server_symbols.debug`, used for the game `.symtab` and `.rela.dyn` vtable relocations.
- `--classes`: comma-separated class names, currently `Actor,BlockPos`.

## Run

```powershell
./frontend/run-symbols.ps1 `
  -Pdb ../../BDS/bin/bedrock_server.pdb `
  -Exe ../../BDS/bin/bedrock_server.exe `
  -Elf ../../BDS/bin/bedrock_server_symbols.debug `
  -Output artifacts/symbol-ir-1.21.3/prototype.json `
  -Classes Actor,BlockPos
```

The script builds `hosihikari-symbol-ir-frontend` and writes a single JSON document. The JSON is a prototype, not `hosihikari.bindings.ir/1`.

## What the symbols provide

- Mangled public symbols and addresses from both images.
- Demangled method names, parameter types, `const`, `static`, and MSVC access specifiers.
- Windows return types (MSVC mangling) merged into Linux records by `qualified name + parameters + const`.
- Vtable membership: Windows slots are read from the PE as image-base VAs; Linux slots are reconstructed from `R_X86_64_RELATIVE` addends in `.rela.dyn`.
- No field layout, class size, field types, or `inline`/`defined` facts. The ELF DWARF covers only NetherNet/webrtc, and the PDB type stream is stripped.

## Current slice

| Class | Windows methods | Linux methods | Merged | Windows vtable slots | Linux vtable slots |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Actor` | 635 | 823 | 634 | 153 | 151 |
| `BlockPos` | 14 | 15 | 12 | 0 | 0 |

Linux includes local/inline functions that the Windows publics omit, which explains most Linux-only entries. The two vtable sizes differ by one slot; stores the per-configuration index rather than assuming a shared layout.

## Output shape

```json
{
  "prototype": "hosihikari.bedrock.symbols/1",
  "version": "1.21.3",
  "classes": [
    {
      "name": "Actor",
      "vtables": { "win-x64-server": { "symbol": "??_7Actor@@6B@", "address": "0x2da38f0" } },
      "methods": [
        {
          "key": "Actor::canAttack(Actor*,bool)const",
          "returnType": "bool",
          "parameters": ["Actor*", "bool"],
          "virtual": true,
          "win-x64-server": { "symbol": "?...", "rva": "0x...", "vtableIndex": 12 },
          "linux-x64-server": { "symbol": "_ZNK5Actor9canAttack...", "address": "0x...", "vtableIndex": 12 }
        }
      ]
    }
  ]
}
```

## Next steps

- Validate class/field facts against LiteLoaderBDS headers as a supplement; symbols remain the primary input.
- Decide whether the formal IR gets `symbol`, `address`, `vtableIndex`, and optional `layout`/`returnType` fields, or whether symbol records are a separate schema.
- Add a symbol-dispatch shim mode that calls by RVA/vtable rather than by generated C++ declarations.
