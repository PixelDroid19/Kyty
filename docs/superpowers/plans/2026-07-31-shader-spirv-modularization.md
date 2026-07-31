# Shader SPIR-V Modularization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 14,375-line `ShaderSpirv.cpp` monolith with focused translation units while preserving generated SPIR-V and the public API.

**Architecture:** Keep the public facade stable, introduce one private state contract, and group instruction emitters by ISA responsibility. Retain one dispatch table so instruction selection remains data-driven and behaviorally identical.

**Tech Stack:** C++17, CMake globbed emulator sources, GoogleTest, SPIR-V text generation.

## Global Constraints

- Work in the current checkout and branch.
- Preserve all existing local modifications.
- Do not stash, reset, discard, or rewrite unrelated files.
- Perform a mechanical refactor only; do not add emulator capability.
- Treat the existing arrayed-image test abort as a baseline failure.

---

### Task 1: Establish private contracts and extract static templates

**Files:**
- Create: `source/emulator/src/Graphics/ShaderSpirvInternal.h`
- Create: `source/emulator/src/Graphics/ShaderSpirvEmitters.h`
- Create: `source/emulator/src/Graphics/ShaderSpirvTemplates.h`
- Create: `source/emulator/src/Graphics/ShaderSpirvTemplates.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirv.cpp`

**Interfaces:**
- Produces: `ShaderSpirvInternal::Spirv`, `RecompilerFunc`, shared operand helpers, and externally declared SPIR-V snippets.
- Preserves: `SpirvGenerateSource`, `SpirvGetEmbeddedVs`, and `SpirvGetEmbeddedPs`.

- [x] Extract the generator state class and internal enums without changing member layout or inline behavior.
- [x] Move raw SPIR-V snippets and embedded shader strings to the templates module.
- [x] Build `fc_script` and `unit_test` to catch linkage or CMake-discovery errors.

Run:

```bash
cmake --build _build_linux --target fc_script unit_test -j"$(nproc)"
```

Expected: exit code 0.

### Task 2: Extract generator and shared operand services

**Files:**
- Create: `source/emulator/src/Graphics/ShaderSpirvGenerator.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvOperands.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirv.cpp`

**Interfaces:**
- Consumes: internal state and template declarations from Task 1.
- Produces: generator section writers and shared `operand_load_int`, `operand_load_uint`, and `operand_load_float`.

- [x] Move module orchestration, header/type/global/local/function writers, constant discovery, and variable discovery.
- [x] Move operand classification, constant lookup, SDWA materialization, and load helpers.
- [x] Build both affected targets.
- [x] Run the focused control-flow, interpolation, buffer, and image shader tests that pass in the baseline.

Run:

```bash
_build_linux/fc_script --gtest_filter='EmulatorGraphicsPackets.StructuresBackwardSBranchAsLoopHeader:EmulatorGraphicsPackets.CanonicalizesAliasedPixelInterpolators:EmulatorGraphicsPackets.ParsesImageSampleLzDmaskF:EmulatorGraphicsPackets.Gen5DsRead2B32UsesDwordScaledWorkgroupOffsets' '{kyty_run_tests()}'
```

Expected: four passing tests.

### Task 3: Extract domain emitters and central dispatch

**Files:**
- Create: `source/emulator/src/Graphics/ShaderSpirvControlFlow.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvBuffer.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvImage.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvScalar.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvVector.cpp`
- Create: `source/emulator/src/Graphics/ShaderSpirvDispatch.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirv.cpp`

**Interfaces:**
- Consumes: `KYTY_RECOMPILER_ARGS`, `Spirv`, shared operand helpers, and templates.
- Produces: declared `Recompile_*` emitters and `RecompFunc(type, format)`.

- [x] Move CFG analysis and conditional/unconditional branch emitters.
- [x] Move buffer, scalar-buffer, typed-buffer, barrier, and LDS emitters.
- [x] Move all image helpers and emitters.
- [x] Move scalar ALU emitters.
- [x] Move vector ALU, interpolation, export, fetch, and debug emitters.
- [x] Move the dispatch table without editing its entries.
- [x] Build both affected targets.

Run:

```bash
cmake --build _build_linux --target fc_script unit_test -j"$(nproc)"
```

Expected: exit code 0.

### Task 4: Verify behavior and review scope

**Files:**
- Review all `ShaderSpirv*` files and existing local changes in `Shader.cpp`.

**Interfaces:**
- Verifies the unchanged public facade and instruction registry.

- [x] Run all shader-focused tests that are known to pass in the baseline.
- [x] Re-run `MaterializesArrayedGen5ImageLoadAndStoreCoordinates` and confirm it has the same recorded abort.
- [x] Review `git diff --check`, diff statistics, public header changes, and new file sizes.
- [x] Report exact commands, results, and the known pre-existing limitation.

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors; only intended modularization files plus the user's existing `Shader.cpp` change.
