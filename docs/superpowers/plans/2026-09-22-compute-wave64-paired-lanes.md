# Compute wave64 paired-lane implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Root owns integration, builds and final acceptance; subagents use Luna Max, at most four active, and do not spawn descendants.

**Goal:** Implement and numerically validate a strict compute wave64 execution strategy using two guest lanes per full native32 invocation, preserving the original workgroup and LDS.

**Architecture:** Separate checked execution layout, architectural packed masks and banked VGPR access from host Vulkan capability/pipeline handling. Reuse existing arithmetic/resource lowering through explicit typed interfaces. No extra workgroup barriers are introduced for wave collectives.

**Tech Stack:** Existing C++17 emulator, SPIR-V tools, Vulkan 1.2 plus `VK_EXT_subgroup_size_control`, GoogleTest and existing Vulkan integration probes.

**Spec:** `docs/superpowers/specs/2026-09-22-compute-wave64-paired-lanes-design.md` (scope confirmed by the user).

## Global constraints

- Work on the existing dirty `main`; preserve unrelated changes and unpushed commits. No reset, stash, clean, branch switch, worktree creation, commit or push.
- Root alone builds in its existing task-owned build directory with `-j2`; never reconfigure another build.
- No title/address selectors, fabricated resources, opcode skips or permissive execution.
- Compute-only; first paired mode requires full guest waves (`N % 64 == 0`) and group-count dispatches. Reject partial/thread-count controls before division.
- One host workgroup and one LDS allocation per original guest workgroup; physical group counts remain unchanged.
- Keep evidence and private fixtures outside Git. Tracked tests use synthetic programs, not private binary excerpts.
- Only root integrates shared headers/generator/dispatch tables. Assign nonoverlapping implementation modules to agents and serialize dependent edits.
- Do not activate a private workload until lane-order, barrier and opcode eligibility gates are satisfied. The exact aliased U64 comparison remains unsupported without stronger evidence.

## Review focus

1. Initial EXEC and later EXEC writes must not fabricate semantics for partial guest waves; reject those layouts in Task 1.
2. Lane 63 and cross-half exchanges must not collapse to a host32-only result; test both banks in Tasks 3 and 4.
3. Group-count bounds and thread-count controls must be checked before conversion/submission; test direct and indirect admission in Tasks 1 and 5.
4. A second bank must not read first-bank writes from the same instruction, and atomics must not collapse into one effect; snapshot sources and test alias/atomic outputs in Tasks 3 and 4.
5. Cache reuse must not mix native and paired physical layouts or retain old boolean-mask semantics; test identity and invalidate translator entries in Task 5.

## Task 1: Pure checked execution layout

**Files:** create `source/emulator/include/Emulator/Graphics/ShaderComputeWaveLayout.h`, `source/emulator/src/Graphics/ShaderComputeWaveLayout.cpp`, and `source/unit_test/src/emulator/UnitTestEmulatorComputeWaveLayout.cpp`; root adds the unit-link registration and a default-native `wave_layout` field to `ShaderComputeInputInfo` in `Shader.h`.

**Interfaces:** define Vulkan-independent request/capability/result records in the new header. Use these names consistently in later tasks:

```cpp
enum class ShaderGuestLaneOrder { Unverified, LinearXFirst };
enum class ShaderComputeWaveStrategy { Native, Paired64On32 };
enum class ShaderComputeWaveLayoutStatus {
    Supported, NoWork, UnsupportedWidth, UnsupportedDispatchMode,
    InvalidLocalSize, UnverifiedLaneOrder, MissingHostCapability,
    HostLimitExceeded, InvalidArgument
};
struct ShaderComputeWaveRequest {
    uint32_t local[3];
    uint32_t groups[3];
    uint32_t dispatch_mode;
    uint32_t lds_dwords;
    ShaderGuestLaneOrder lane_order;
};
struct ShaderComputeWaveCapabilities {
    bool size_control_enabled;
    bool full_subgroups_enabled;
    bool compute_required_size_supported;
    bool compute_ballot_shuffle_supported;
    uint32_t min_subgroup_size, max_subgroup_size;
    uint32_t max_local_size[3], max_group_count[3];
    uint32_t max_invocations, max_subgroups, max_shared_bytes;
};
struct ShaderComputeWaveLayout {
    ShaderComputeWaveStrategy strategy = ShaderComputeWaveStrategy::Native;
    uint32_t guest_local[3] {}, physical_local[3] {};
    uint32_t guest_wave_size = 0, native_subgroup_size = 0, banks = 1, waves = 0;
    uint32_t lds_dwords = 0;
};
ShaderComputeWaveLayoutStatus ShaderBuildPairedComputeWaveLayout(
    const ShaderComputeWaveRequest& request,
    const ShaderComputeWaveCapabilities& capabilities,
    ShaderComputeWaveLayout* layout);
```

The focused suite name is `EmulatorComputeWaveLayout`. A minimal supported
case uses explicit synthetic capabilities, not live device defaults:

```cpp
TEST(EmulatorComputeWaveLayout, PreservesGuestGroupAndPairsFourWaves)
{
    ShaderComputeWaveRequest request {{16, 16, 1}, {2, 3, 1}, 0x41, 1664,
                                     ShaderGuestLaneOrder::LinearXFirst};
    ShaderComputeWaveCapabilities caps {true, true, true, true, 8, 32,
                                       {1024, 1024, 64}, {65535, 65535, 65535},
                                       1024, 128, 49152};
    ShaderComputeWaveLayout layout {};
    ASSERT_EQ(ShaderBuildPairedComputeWaveLayout(request, caps, &layout),
              ShaderComputeWaveLayoutStatus::Supported);
    EXPECT_EQ(layout.physical_local[0], 128u);
    EXPECT_EQ(layout.guest_local[1], 16u);
    EXPECT_EQ(layout.waves, 4u);
    EXPECT_EQ(layout.banks, 2u);
}
```

Run red and green with:

```bash
cmake --build "$TASK_BUILD" --target fc_script -j2
GTEST_FILTER='EmulatorComputeWaveLayout.*' "$TASK_BUILD/fc_script" scripts/run_unit_tests.lua
```

- [ ] Add parameterized red cases: `(64,1,1)` maps to `(32,1,1)`; `(256,1,1)` maps to `(128,1,1)`; explicit verified `(16,16,1)` also maps to `(128,1,1)`. Reject zero/overflow, totals 1/32/33/65, unknown lane order, wave32 request, unsupported features and every relevant host limit. Zero group counts return `NoWork` without changing the output record.
- [ ] Explicitly test bit-5 and partial-threadgroup rejection before any ceil division; test per-axis group limits at limit and limit+1. Keep a sentinel output and assert rejected calls do not partially write it.
- [ ] Build root-owned `fc_script` with two jobs and run the focused layout filter, preserving the first real red result (not an authoring compile error).
- [ ] Implement validation using checked multiplication before committing a local result. The core supported-layout arithmetic is:

```cpp
const uint32_t waves = guest_invocations / 64u;
candidate.strategy = ShaderComputeWaveStrategy::Paired64On32;
candidate.guest_wave_size = 64u;
candidate.native_subgroup_size = 32u;
candidate.banks = 2u;
candidate.waves = waves;
candidate.physical_local[0] = waves * 32u;
candidate.physical_local[1] = 1u;
candidate.physical_local[2] = 1u;
```

Check multiplication against its destination bound before performing it, including the three guest dimensions, subgroup capacity and LDS bytes. Decode width from initiator bit 15. Unknown or deferred dispatch controls are not silently discarded.

- [ ] Re-run the focused test and request independent review of arithmetic, rejection ordering and no-partial-output behavior. Do not wire this helper into runtime selection yet.

## Task 2: Capability enablement and bounded GPU probe support

**Files:** focused edits in `GraphicContext.h`, `Window.cpp`, `GraphicsRenderPipeline.cpp`; add `source/integration_test/src/shader_compute/VulkanWaveProbe.cpp` and a focused header; reuse `VulkanComputeProbeInternal` resource lifetime helpers. Root updates the explicit integration CMake source list.

The shared host feature adapter lives in `source/emulator/include/Emulator/Graphics/ShaderComputeWaveVulkan.h`
and `source/emulator/src/Graphics/ShaderComputeWaveVulkan.cpp`, with focused coverage in
`source/unit_test/src/emulator/UnitTestEmulatorComputeWaveVulkan.cpp`.

**Interfaces:** adapt enabled device features/properties to `ShaderComputeWaveCapabilities`. Add a wave probe dispatch method accepting production SPIR-V, `ShaderComputeWaveLayout`, group counts and bounded `std::vector<uint32_t>` input/output; retain the existing 4-word scalar and 8-word GETPC APIs unchanged.

```cpp
// VulkanComputeProbe method. Reject empty or >4096-word output buffers.
Result DispatchWave(const uint32_t* spirv, size_t word_count,
                    const ShaderComputeWaveLayout& layout,
                    const std::array<uint32_t, 3>& groups,
                    const std::vector<uint32_t>& initial_words,
                    std::vector<uint32_t>* result_words,
                    std::string* message) const;
```

Create a separate `kyty_shader_wave64_integration` executable with a small
`ShaderWaveIntegration.cpp` runner, linking the shared existing probe helpers.
Register `KytyShaderComputeIntegration.PairedWave64`; do not grow the scalar
case runner with all wave fixtures.

- [ ] Add a failing capability-selection case where extension presence is true but feature enablement is false; it must not admit paired execution. Add a probe output-size rejection case before allocating resources.
- [ ] Query revision-2 EXT features; enable only queried-supported features in the device chain. Store enabled state separately from mere advertised support. Do not raise the application's Vulkan API requirement.

Missing optional wave features must not break ordinary device initialization
or the existing scalar probes. Only paired admission/dispatch reports the
missing capability; never substitute native32 semantics for wave64.
- [ ] Build the compute stage with this exact state when the layout is paired:

```cpp
VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required {};
required.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
required.requiredSubgroupSize = 32u;
stage.pNext = &required;
stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
stage.flags &= ~VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT;
```

The structure must live through pipeline creation. Preserve other required stage-chain structures rather than overwriting an existing chain. Reject unsupported compute stages, size range, X divisibility, subgroup count or local/shared-memory limits before creating a pipeline.

- [ ] Extend the existing fenced probe helpers with explicit buffer byte bounds and dispatch dimensions. Keep the five-second fence deadline and safe in-flight lifetime handling. Export one result record per logical guest lane, avoiding the old scalar probe's fixed-output-slot write pattern.
- [ ] Run all 13 existing scalar/GETPC GPU cases unchanged, then the new feature/layout probe. A capability skip is not a numeric pass. Independent reviewer inspects actual feature and lifetime changes before integration.

## Task 3: Explicit banked registers and packed masks

**Files:** create `ShaderSpirvComputeWave.cpp` and `ShaderSpirvWaveMask.cpp`; root integrates declarations in `ShaderSpirvInternal.h` and focused prolog/variable generation changes in `ShaderSpirvGenerator.cpp`, consuming the layout field created by Task 1. Tests belong in `UnitTestEmulatorComputeWaveMasks.cpp` and a small wave-mask GPU case module.

**Interfaces:** reuse existing `SpirvValue`, `ShaderOperand` and `String8`; introduce an explicit bank enum and register-access methods:

```cpp
enum class ShaderWaveBank : uint32_t { Low = 0u, High = 1u };
// Methods on Spirv; declarations are integrated by root.
SpirvValue GetComputeWaveRegister(ShaderOperand operand, ShaderWaveBank bank, int word) const;
bool EmitComputeWaveMaskBit(const ShaderOperand& mask, ShaderWaveBank bank,
                            const String8& result_id, String8* output) const;
bool EmitComputeWaveBallot(const String8& low_predicate,
                           const String8& high_predicate,
                           const String8& low_result,
                           const String8& high_result, String8* output) const;
```

- [ ] Write one red production-lowering GPU case with a predicate true only at logical lane 63. The complete mask oracle is `{0u, 0x80000000u}` in every invocation of that guest wave; lane-local booleans and native-low-bank-only ballots must fail it.
- [ ] Generate two VGPR banks and architectural two-word EXEC/VCC storage. SGPR storage remains wave-uniform. Derive logical IDs from subgroup ID/lane plus bank; do not read physical local XYZ as guest XYZ.
- [ ] Implement mask-bit access using the appropriate 32-bit word and native lane shift. Emit two subgroup ballots and extract each low ballot word into the corresponding architectural mask word:

```text
%wave_ballot_low = OpGroupNonUniformBallot %v4uint %uint_3 %predicate_low
%wave_ballot_high = OpGroupNonUniformBallot %v4uint %uint_3 %predicate_high
%wave_mask_low = OpCompositeExtract %uint %wave_ballot_low 0
%wave_mask_high = OpCompositeExtract %uint %wave_ballot_high 0
```

Actual identifiers must be unique per instruction. Predicates include the proper incoming EXEC bit. All host subgroup invocations execute the collectives, including guest-inactive lanes; guard effects, not collective participation.

- [ ] Add all-zero, low-only, high-only, both-half and partial-EXEC numerical cases. Assert SCC/EXEC preservation where required. For source/destination overlap, load all sources before destination commits; do not declare the specifically unresolved VCC-alias instruction supported.
- [ ] Recompile real generated SPIR-V through the existing validator, run numeric probes and obtain independent mask-representation review. No generic string replacement or overloaded boolean/packed-mask interpretation is allowed.

## Task 4: Wave operations, two-bank effects and control flow

**Files:** create `ShaderSpirvWaveLane.cpp`, `ShaderComputeWaveAnalysis.h/.cpp`; focused edits in existing vector/scalar/control-flow/buffer/image emitter modules. Use separate small lane and shared-memory integration case modules. Root owns shared dispatcher and enum/parser integration.

**Interfaces:** analysis reports the first unsupported instruction PC and contract reason. Every admitted opcode/format explicitly identifies scalar-only behavior, two-bank lane effects, packed-mask behavior or a wave collective; there is no default “ordinary supported” category.

```cpp
struct ShaderComputeWaveAnalysisResult {
    bool supported = false;
    uint32_t unsupported_pc = 0;
    Kyty::Core::String8 reason;
};
ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveCode(
    const ShaderCode& code, const ShaderComputeInputInfo& input);
```

- [ ] Add red cross-half read/write-lane cases for lanes 31, 32 and 63. Add first-active-lane cases with active lanes only in the upper half. For a logical lane `j`, bank is `j / 32` and native source lane is `j % 32`; choose the source bank before the native exchange.
- [ ] Route ordinary VGPR arithmetic through the explicit bank interface, capturing both banks' sources before committing results. Route comparison/carry/CMPX and scalar EXEC operations through the shared packed-mask implementation. Numeric mask consumers must not read a lane predicate as an integer mask.
- [ ] Route memory/image/LDS operations through bank-specific operands and guest EXEC guards. Preserve descriptor/lifetime/bounds checks. Do not duplicate scalar-only guest side effects merely because there are two banks.
- [ ] Use a shared LDS atomic-add fixture: initialize counter zero; all 128 logical lanes of two waves increment once; each lane stores the returned value; after a genuine guest barrier, counter must be 128 and sorted returned values must equal `0..127`. This detects collapsed banks and lost/duplicated side effects without requiring an arbitrary atomic ordering.
- [ ] Add a shared-memory producer/consumer fixture and a two-wave branch fixture that diverges only after a real barrier. Use different lane-63 predicates per wave and verify results do not mix waves. No extra workgroup barrier is inserted for a wave operation.
- [ ] Admit only supported barrier/reconvergence shapes with stable scalar inputs; reject a synthetic barrier bypass before Vulkan submission. Add deterministic rejection for the unresolved aliased U64 compare. Placeholder `SBarrier/Unknown` is never classified as a real barrier.
- [ ] Have an independent reviewer inspect the actual opcode eligibility set and emitted control flow. Run focused shader/toolchain and GPU cases; record unsupported opcodes individually instead of introducing a permissive fallback.

## Task 5: Runtime admission, guest identity and cache integration

**Files:** focused edits in `Shader.h`, `Shader.cpp`, `GraphicsRenderDraw.cpp`, `GraphicsRenderPipeline.cpp`, and `ShaderTranslationCache.h`; focused cache/admission unit module. Preserve existing GETPC and resource metadata contracts.

**Interfaces:** pass the raw dispatch initiator to compute input analysis explicitly; keep logical `threads_num` distinct from the new execution layout. Populate capability inputs from enabled host state. Apply one common admission result to direct and indirect dispatch paths.

```cpp
void ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs,
                         const HW::ShaderRegisters* sh,
                         uint32_t dispatch_mode,
                         ShaderComputeInputInfo* info);
```

Update its single production caller explicitly; do not add a default initiator
that would silently select an unknown width.

- [ ] Add red admission/cache cases: native and paired strategies differ; guest/physical local-shape changes differ; runtime program addresses and group counts do not alter translator identity; zero-group work remains a no-op. Thread-count/partial controls reject before the existing conversion.
- [ ] Capture and preserve dispatch mode end-to-end. The current HLE encoder mask `0xA038` preserves bit 15, and command-processor direct/indirect paths pass the mode unchanged; do not infer width from workgroup size or host subgroup size.
- [ ] Run the layout helper before unsafe conversion/submission. Validate per-axis dispatch group counts and local dimensions. Keep descriptor preparation and metadata-fill analysis on original guest counts; only physical `LocalSize` changes in paired mode.
- [ ] Key execution strategy and both shapes in shader/pipeline identity and increment the translator version once for the integrated semantic change. Per-dispatch GETPC base stays metadata, not a cache key.
- [ ] Activate the paired strategy only when instruction eligibility, proven guest lane order and enabled capabilities all hold. Unknown lane order, aliased VCC behavior or barrier shape produces a precise first unsupported-contract error, not native32 substitution.
- [ ] Re-run cache, resource-pointer, scalar/GETPC and new wave tests. Independently review the common direct/indirect path and verify no writes to unrelated worktree files.

## Task 6: Integrated strict validation and remaining-frontier handoff

**Files:** update only verified frontier/evidence notes in `docs/BRINGUP.md`; do not add private logs or fixtures to the repository.

- [ ] Root reviews the complete task-relative diff and resolves all material findings. Serialize all writers before the build.
- [ ] Reconfigure only the owned build if new globbed sources require it, then build `fc_script`, `kyty_shader_compute_integration`, `kyty_shader_wave64_integration` and `kyty_graphics_diagnostics_integration` with `-j2`.
- [ ] Run focused unit filters followed by the relevant full regression command. Run both integration contracts with the existing CTest build fixture excluded:

```bash
ctest --test-dir "$TASK_BUILD" \
  -R '^(KytyShaderComputeIntegration.(ScalarContracts|PairedWave64)|KytyGraphicsDiagnosticsIntegration.SerializationAndBounds)$' \
  -FA KytyIntegrationBinaries --output-on-failure
python3 scripts/check_emulator_boundaries.py --strict source
python3 scripts/check_graphics_tables.py source/emulator/src/Graphics/Tables/manifest.sha256
git diff --check
```

`TASK_BUILD` is the root's already-owned build, not a new or shared directory.
The wave unit modules use suite names `EmulatorComputeWaveLayout`,
`EmulatorComputeWaveMasks`, `EmulatorComputeWaveLane` and
`EmulatorComputeWaveAdmission`. Run them and the existing shader cases with:

```bash
GTEST_FILTER='EmulatorComputeWave*.*:EmulatorShader*.*' "$TASK_BUILD/fc_script" scripts/run_unit_tests.lua
"$TASK_BUILD/fc_script" scripts/run_unit_tests.lua
```

Never report capability skips as numeric passes.

- [ ] Launch the private workload in the existing bounded Silent/native runner, collect native wait-ready/doctor/watch/events/error/thread/sync/diagnostic evidence and stop at the first failure. Do not bypass unresolved alias or memory/ray contracts to produce a frame.
- [ ] If the original first failure remains, state why and retain the strict error. If it advances, record the new exact contract and repeat the project's single-frontier loop. Gameplay remains incomplete until controlled play and capture/playability gates pass.

## Execution and approval status

The user explicitly approved execution with Luna Max subagents, superseding
the earlier supplied Terra role policy. Root builds and validates; agents
implement only assigned nonoverlapping modules and independently review
material changes. Execution is authorized without further inter-task approval.
No commits are part of this workflow.
