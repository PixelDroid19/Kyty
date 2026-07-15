# Export / NID catalog (v1 seed)

Local versioned inventory **separate from HLE registration**.

## Location

- Headers: `source/lib/DevTools/include/Kyty/DevTools/Catalog/ExportCatalog.h`
- Implementation: `source/lib/DevTools/src/Catalog/ExportCatalog.cpp`
- Tests: `source/unit_test/src/devtools/UnitTestDevToolsExportCatalog.cpp`

## Schema

- Major/minor: **1.0**
- Status: `KNOWN | REQUIRED | IMPLEMENTED | UNIMPLEMENTED | CONFLICT | UNKNOWN_ABI`
- Fields: firmware, library, module, export name, NID, source tag, symbol type

## Guarantees

- `Insert` / `SeedPublicExportCatalog` **never** install trampolines or mark imports executable.
- Unresolved statuses remain diagnostic-only.
- Conflict: same library+NID with different export names marks the existing entry `CONFLICT`.
- Seed uses only NID strings already present in-tree (comments / fixtures). No invented NIDs.

## Queries

```cpp
ExportCatalog cat;
SeedPublicExportCatalog(&cat);
cat.FindByName("sceAudioOut2Initialize", &idx);
cat.FindByNid("Q2V+iqvjgC0", &idx);
cat.FindByLibrary("libSceAudioOut", indices, max, &count);
```

## Phase scope

Seed is intentionally small (multi-source sample). Full firmware 3.20 corpus (~39k)
is research data, not auto-imported as success stubs.
