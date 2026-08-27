# Gen5 Vertex Clip Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure the selected Gen5 vertex shader's post-transform clip-space `w` and `x/w`, `y/w`, `z/w` ranges after `Pos0`, without changing guest-visible state or rendered output.

**Architecture:** Resolve one checksum/count-scoped diagnostic variant at the draw boundary. Give that variant distinct VS shader-module and graphics-pipeline identities. The generated VS writes fixed-size aggregate statistics through core unsigned SPIR-V atomics into a separate host-owned descriptor set. The renderer binds that set only for the selected draw and reads it only after the command buffer's submission fence completes.

**Tech Stack:** C++17, Vulkan, generated SPIR-V assembly, SPIRV-Tools validation, CMake/CTest integration binary, native `kyty_agent` runtime diagnostics.

**Spec:** `docs/kyty-runtime-graphics-investigation-handoff.md`

## Global Constraints

- Work only in the active checkout on `feature/gen5-3d-world`.
- Preserve every unrelated dirty change. Do not reset, restore, clean, stash, checkout, create another worktree, commit, or push.
- Keep the feature opt-in, checksum/count-scoped, bounded to one pending draw, host-owned, and disabled by default.
- Never key behavior by title, guest address, local path, or captured content.
- Do not alter `gl_Position`, varyings, guest descriptor sets, depth state, draw parameters, or ordinary shader/pipeline identities.
- Use only core uint atomics. Do not require float atomic extensions or unverified Vulkan device features.
- Prefer the existing consolidated graphics integration executable; do not add one unit test per helper.
- Build at `-j1` or `-j2` inside a dedicated cgroup with a hard memory limit and no swap. Never build while a guest is running.
- Run at most one guest, with a systemd cgroup, watchdog, native agent timeouts, `PrintfDirection = Silent`, and the exact two-tap route. A boot, window, menu, present, or capture is not playability evidence.

---

### Task 1: Lock the selector, cache identity, and stats contracts in one integration mode

**Files:**
- Modify: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Shader.h`
- Modify: `source/emulator/include/Emulator/Graphics/ShaderTranslationCache.h`

- [x] Add one isolated integration mode named `--vertex-clip-probe-contract-only` to `GraphicsDiagnosticsIntegration.cpp`; keep all assertions in that mode so repeated implementation builds do not run the unrelated consolidated suite.

- [x] In that mode, test strict environment parsing through this public contract:

```cpp
struct ShaderVertexClipProbeConfig
{
	bool     enabled             = false;
	bool     draw_scoped         = false;
	uint64_t diagnostic_identity = 0;
};

[[nodiscard]] ShaderVertexClipProbeConfig ShaderResolveVertexClipProbeConfig(
    uint64_t code_id, bool indexed, uint32_t guest_count);
```

Use `KYTY_VS_CLIP_PROBE=<16-hex-checksum>` and `KYTY_VS_CLIP_PROBE_DRAW=indexed:<count>|auto:<count>`. Assert exact checksum match, exact kind/count match, rejection of trailing input and overflow, disabled-by-default behavior, and a nonzero identity only for the selected draw.

- [x] Test `ShaderModuleKey::Create` with the same `ShaderId` and two different nonzero diagnostic identities. Vertex keys must remain unequal; a zero-identity ordinary Vertex key must remain unchanged.

- [x] Test the fixed raw statistics contract and ordered-float conversion with representative negative, negative zero, positive zero, positive, infinity-rejection, and empty sentinel cases. The public pure helpers are:

```cpp
struct VertexClipProbeRawStats
{
	uint32_t invocations = 0;
	uint32_t nonfinite   = 0;
	uint32_t min_w       = UINT32_MAX;
	uint32_t max_w       = 0;
	uint32_t min_x_w     = UINT32_MAX;
	uint32_t max_x_w     = 0;
	uint32_t min_y_w     = UINT32_MAX;
	uint32_t max_y_w     = 0;
	uint32_t min_z_w     = UINT32_MAX;
	uint32_t max_z_w     = 0;
};

[[nodiscard]] uint32_t VertexClipProbeEncodeOrderedFloat(float value);
[[nodiscard]] float VertexClipProbeDecodeOrderedFloat(uint32_t value);
[[nodiscard]] bool VertexClipProbeHasFiniteExtrema(const VertexClipProbeRawStats& stats);
```

- [x] Build only `KytyGraphicsDiagnosticsIntegration` under a memory-limited cgroup and run the isolated mode. Record the expected RED result before implementation; missing declarations or unresolved symbols are acceptable RED evidence, while an unrelated compile failure is not.

### Task 2: Implement pure selector, identity, and raw-stat behavior

**Files:**
- Create: `source/emulator/include/Emulator/Graphics/VertexClipProbe.h`
- Create: `source/emulator/src/Graphics/VertexClipProbe.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Shader.h`
- Modify: `source/emulator/src/Graphics/Shader.cpp`
- Modify: `source/emulator/src/Graphics/ShaderTranslationCache.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderDraw.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp`
- Modify: the emulator source list that explicitly enumerates graphics sources, if required by the active CMake target

- [x] Put `ShaderVertexClipProbeConfig clip_probe;` in `ShaderVertexInputInfo`, adjacent to other immutable per-draw shader input.

- [x] Parse the two environment variables with complete-consumption integer parsing. Derive `diagnostic_identity` from a fixed revision tag and never from a guest address or title. A supplied draw selector makes the configuration draw-scoped and leaves `enabled=false` unless kind and count both match.

- [x] Resolve the config immediately after `ShaderGetInputInfoVS` in both indexed and auto draw paths, using `gs_regs.chksum` through the existing Gen5 VS checksum surface.

- [x] Make only non-embedded Gen5 VSs eligible. Reject an enabled probe defensively at both embedded VS early-return seams so an uninstrumented embedded shader can never produce false zero statistics.

- [x] Append a fixed marker to `ShaderGetIdVS` only when `clip_probe.draw_scoped && clip_probe.enabled`; ordinary VS identities must remain byte-for-byte equivalent.

- [x] Retain nonzero diagnostic identity for `ShaderModuleStage::Vertex` as well as Pixel in `ShaderModuleKey::Create`, and pass `vs_input_info->clip_probe.diagnostic_identity` into the VS `GetOrCompile` key.

- [x] Implement ordered-float mapping exactly as:

```cpp
uint32_t bits = 0;
static_assert(sizeof(bits) == sizeof(value));
std::memcpy(&bits, &value, sizeof(bits));
return (bits & 0x80000000u) != 0 ? ~bits : (bits ^ 0x80000000u);
```

Reverse it with the inverse bit transform and `std::memcpy` back to `float`. Treat `min == UINT32_MAX` or `max == 0` as no finite extrema; never decode those sentinels as observations.

- [x] Rebuild and run only `--vertex-clip-probe-contract-only`; require GREEN. Review `git diff --check` and the relevant diff before proceeding.

### Task 3: Add a synthetic VS SPIR-V integration gate

**Files:**
- Modify: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`
- Modify: `source/integration_test/CMakeLists.txt`

- [x] Extend the same isolated mode with a minimal parsed Vertex `ShaderCode` that exports `Pos0` and a `ShaderVertexInputInfo` whose `clip_probe.enabled` is true.

- [x] Generate source with `SpirvGenerateSource`, then assert all of these properties:

```text
the existing OpStore to %outPerVertex is present
the probe descriptor is DescriptorSet <host-only-set> and Binding 0
OpAtomicIAdd is present for invocation/nonfinite counters
OpAtomicUMin and OpAtomicUMax are present for extrema
OpIsNan and OpIsInf guard the extrema path
no OpAtomicFMinEXT or OpAtomicFMaxEXT appears
no ordinary vertex source without clip_probe contains the probe symbols
```

- [x] Compile the generated source through `ShaderRecompileVS` and validate the binary with the existing SPIRV-Tools validation helper used by the integration executable.

- [x] Run the isolated mode and record the expected RED assertion before editing the generator.

- [x] Register `KytyGraphicsDiagnosticsIntegration.VertexClipProbe` in CTest with the integration-binary fixture and bounded timeout.

### Task 4: Generate a non-mutating clip-stat instrumentation variant

**Files:**
- Modify: `source/emulator/src/Graphics/ShaderSpirvInternal.h`
- Modify: `source/emulator/src/Graphics/ShaderSpirvGenerator.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirvVector.cpp`

- [x] Add generator predicates that return true only for a Vertex shader with `clip_probe.enabled`.

- [x] When enabled, declare one host diagnostic storage block with ten uint members at binding 0 in a descriptor set passed through `ShaderVertexInputInfo`. Keep it completely outside `ShaderBindResources`, so guest binding arithmetic and descriptor layouts do not move.

- [x] Initialize the buffer on the host, not in the shader: counters to zero, minima to `UINT32_MAX`, maxima to zero. This is implemented with the Task 5 renderer-owned buffer.

- [x] Immediately after the existing `OpStore` of `%t4_<index>` to `%outPerVertex`, load `x`, `y`, `z`, `w`; increment invocation count; test `w == 0`, NaN, and Inf; calculate ratios only for finite nonzero `w`; and update ordered uint extrema with core atomics.

- [x] Count one nonfinite observation for an invocation if `w` or any ratio is invalid. Skip all extrema for that invocation. Do not write any guest output or change control flow that reaches ordinary shader stores.

- [x] Rebuild and run `--vertex-clip-probe-contract-only`; require generated source assertions, recompilation, and SPIR-V validation all GREEN.

### Task 5: Add the host-only descriptor and fence-correct readback lifecycle

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsRender.h`
- Modify: `source/emulator/src/Graphics/GraphicsRenderInternal.h`
- Modify: `source/emulator/src/Graphics/GraphicsRenderContext.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderDraw.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderCommandBuffer.cpp`
- Modify: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`

- [x] Add one renderer-owned `VertexClipProbe` with explicit `Init` and `Done` lifecycle. Allocate a fixed host-visible, coherent storage buffer using `VulkanCreateBuffer`; do not reuse `GdsBuffer` because it is guest-visible.

- [x] Create a dedicated one-binding `VkDescriptorSetLayout`, one-set descriptor pool, and descriptor set. Increase the graphics pipeline's local set-layout capacity from two to three. Append the probe layout after ordinary guest VS/PS layouts only for a selected diagnostic pipeline.

- [x] Compute the probe set index as the final ordinary guest set-layout count only after both VS and PS binding requirements are known. Store that index in `ShaderVertexInputInfo` before `ShaderGetIdVS`, `ShaderModuleKey::Create`, or VS translation. Do not assume a fixed set number: it can be 0, 1, or 2.

- [x] After assigning the final set index, include probe enablement, revision, and set index in both the Vertex `ShaderModuleKey` and graphics-pipeline/`ShaderGetIdVS` identities. The same VS combined with different PS descriptor requirements must compile distinct SPIR-V set decorations. A normal pipeline must not acquire another set layout.

- [x] Before the selected draw, claim at most one pending probe for that `CommandBuffer`, clear/map the raw stats to their sentinels while no GPU use is pending, bind the host-only descriptor set at the diagnostic set index, record a host-write → vertex-shader read/write buffer dependency, and record the draw normally.

- [x] After the render pass containing that draw ends, record a `VK_PIPELINE_STAGE_VERTEX_SHADER_BIT` shader-write to `VK_PIPELINE_STAGE_HOST_BIT` host-read buffer barrier for the fixed range.

- [x] Attach the pending probe to the exact `CommandBuffer` slot. Consume it only after successful fence completion and before the pending record can be reused in both completion paths: blocking `CommandBuffer::WaitForFence` and nonblocking `TryCompleteFenceAndResetWithoutLabelCallbacks`. Never read the mapped data from the draw-recording function.

- [x] Emit one bounded machine-readable native-agent event containing checksum, indexed/auto, guest count, invocations, nonfinite, and decoded min/max ranges. Do not emit per vertex or per frame; keep the optional log as a secondary non-Silent sink.

- [x] Add integration assertions for the one-shot state transition: idle → reserved → recording → pending-fence → completed; reject a second claim and never rearm after completion. Exercise real Vulkan Init/Done plus blocking/nonblocking CommandBuffer fence paths without mocks.

- [x] Build `KytyGraphicsDiagnosticsIntegration` under the cgroup and run only the isolated mode. Then build `fc_script` at `-j1` under the same safety envelope. Require both builds GREEN and `git diff --check` clean.

### Task 6: Independently review and run one bounded strict diagnostic

**Files:**
- Modify only if evidence requires a correction: files changed by Tasks 1–5
- Modify: `docs/kyty-runtime-graphics-investigation-handoff.md`

- [x] Have an independent reviewer inspect selector strictness, cache/pipeline separation, guest binding preservation, Vulkan lifetime, barrier/fence ordering, SPIR-V validity, bounded logging, and teardown.

- [x] Resolve every material review finding before runtime. Rebuild only the affected targets at `-j1`; do not add broad unit suites.

- [x] Verify no build/compiler/test processes remain. Record RAM, zram, and pressure state before starting the guest.

- [x] Start one strict Silent guest inside a bounded systemd cgroup with a watchdog. Select only the exact Gen5 VS checksum and `indexed:3564`; do not enable FS taps, permissive flags, fallback paths, or address filters.

- [x] Use the native agent route exactly: `wait-ready`, `doctor`, wait for `present >= 8000`, lowercase `cross`, `delta 40`, lowercase `cross`, `delta 40`, then status/events and at most one capture. Do not send a third tap.

- [x] Require one fence-completed probe record tied to the selected draw. If it is absent, stop and diagnose selection/lifetime; do not infer values from a capture.

- [x] Classify the result before changing semantics:

```text
nonfinite > 0 or extreme |x/w|, |y/w|, |z/w|: post-VS clip output remains the active producer hypothesis
finite bounded clip output: close post-VS clip as producer and trace source vertex/index/transform data to Pos0
menu or non-gameplay capture: diagnostic routing evidence only, never 3D/playability evidence
```

- [x] Record the exact hypothesis, observation, exclusion, commit, cgroup limits, commands, and remaining uncertainty in `docs/kyty-runtime-graphics-investigation-handoff.md`. Keep private guest paths and protected data out of tracked files.

- [x] Run `git diff --check`, inspect the complete scoped diff, confirm no generated dumps/logs/captures entered the repository, and leave all unrelated dirty changes intact.

## Acceptance Boundary

The probe is complete when its isolated integration contract passes, generated SPIR-V validates, the affected emulator target builds safely, independent review finds no material issue, and one strict selected draw yields a fence-completed aggregate record. That result is diagnostic evidence only. The 3D issue is not fixed, and the private workload is not playable, until a separately validated semantic correction reaches and sustains real gameplay under the repository's capture/playability gate.
