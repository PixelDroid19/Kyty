# Scalar resource mappings implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for this bounded migration; retain the user's explicit Luna Max collaboration protocol.

**Goal:** Preserve more than 64 instruction-specific resource mappings without increasing a fixed resource limit or dropping consumers.

**Architecture:** Store one mapping record per distinct scalar-load PC in a copyable vector. Bound additions by the parsed instruction count. Keep actual texture, sampler and storage-buffer limits separate, and preserve mapping order and shader-key field order.

**Tech Stack:** C++, Kyty Core Vector, Google Test, CMake/Ninja, native runtime diagnostics.

**Spec:** `AGENTS.md`; `docs/BRINGUP.md`, current verified frontier and strict investigation invariants.

## Global Constraints

- Work in the existing dirty `main` worktree; preserve all unrelated modifications.
- No commits, pushes, branch switches, new worktrees, stashing or destructive cleanup.
- Subagents use Luna Max, no descendants, with a single writer for overlapping files.
- Root alone compiles in its existing owned build directory, with `-j2`.
- No guest-specific constants, permissive flags, truncated mappings or fabricated descriptors.
- Keep private workloads and runtime evidence outside versioned paths.
- Keep new tests in a focused module; only adapt existing large-file fixtures mechanically.

## Review Focus

- Distinct PCs sharing one resource must retain separate producer/consumer lifetimes.
- Same-PC replay must retain consistency checks and must not allocate duplicate records.
- Copy-on-write bind copies must not mutate each other's mapping entries.
- Shader identity must retain the existing mapping field order and values.
- The allocation bound must derive from parsed instructions, not another arbitrary ceiling.

### Task 1: Bounded mapping storage and regression

**Files:**

- Create `source/unit_test/src/emulator/UnitTestEmulatorShaderDynamicMappings.cpp`.
- Modify only mapping declarations in `source/emulator/include/Emulator/Graphics/Shader.h`.
- Migrate mapping consumers in `ShaderResources.cpp`, `ShaderSpirvInternal.h`, `ShaderSpirvImage.cpp`, and `Shader.cpp` under `source/emulator/src/Graphics/`.
- Mechanically adapt mapping assertions/fixtures in `UnitTestEmulatorGraphicsState.cpp` and `UnitTestEmulatorGraphicsPackets.cpp`; root adds the unit link in `source/unit_test/src/UnitTest.cpp`.

**Interfaces:** Existing resource collection and resource-index lookup APIs retain their signatures. The internal record replaces indexed parallel-array fields:

```cpp
struct ShaderDynamicSLoadMapping
{
    ShaderDynamicSLoadResourceKind kind = ShaderDynamicSLoadResourceKind::StorageBuffer;
    int resource_index = 0;
    int destination_register = 0;
    uint32_t instruction_pc = 0;
    int offset_dw = 0;
    int dword_count = 0;
    int resource_field_offset = 0;
    uint32_t last_consumer_pc = 0;
    bool raw_vmem_oob_guarded = false;
};
```

- [x] Add a synthetic shader with 65 distinct scalar-load PCs, each consumed before the next overwrite; all loads refer to one valid descriptor. Assert one resource, 65 distinct mappings, preserved destination and last-consumer PC. Copy the bind and ensure mutation of the copy does not change the original.
- [x] Root builds the test against the existing table and runs it. Expected failure is the 64-record ceiling, not an invalid descriptor or fixture setup error.
- [x] Replace the parallel arrays with `Vector<ShaderDynamicSLoadMapping>`. Iterate stable record order and use `Size()` instead of a separately mutable count.
- [x] Pass `code.GetInstructions().Size()` through the private insertion helpers. Check existing PC consistency first, then reject new additions when `Size() >= instruction_count`; retain the existing maximum consumer-PC update for a consistent duplicate. Do not reserve the entire shader size unnecessarily.
- [x] Update readers mechanically: `old.field[index]` becomes `records.At(index).field`. Use `records[index].field` for mutations so Vector performs copy-on-write; do not retain a reference across insertion or copy-on-write. Keep shader-key emission order unchanged.
- [x] Root runs the new test, existing dynamic scalar/split-texture fixtures, and shader regressions. Preserve the known order-dependent retirement failure as separate evidence.
- [x] Independent Luna Max reviewer checks the actual task-only diff, vector copying, count bounds, mapping lookup, and unchanged key field order. Root resolves material findings.
- [x] Root builds affected targets and performs a strict native-agent launch. Acceptance for this task is passing the previous mapping ceiling with a precise new frontier; it is not gameplay acceptance.

Root's reproducible validation commands use its already-owned build path:

```bash
cmake --build "$task_build" --target fc_script kyty_shader_compute_integration kyty_graphics_diagnostics_integration -j2
GTEST_FILTER='EmulatorShaderDynamicMappings.*:EmulatorGraphicsState.*Dynamic*:EmulatorGraphicsPackets.*Dynamic*' "$task_build/fc_script" scripts/run_unit_tests.lua
```

The broader user task remains active after this migration: actual shader execution, presentation and sustained interactive gameplay must be separately demonstrated.
