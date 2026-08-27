# Gen5 PARAM0 Export Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine whether the selected Gen5 vertex shader emits malformed `PARAM0.xy`, the producer values that feed the exact pixel shader's first implicit texture sample.

**Architecture:** Extend the already reviewed one-shot vertex clip probe buffer and event with an independent `PARAM0.xy` aggregate. Instrument only the selected vertex module's existing `EXP PARAM0` store, keep ordinary modules byte-identical, and reuse the current descriptor, barriers, fence ownership, and native Agent event.

**Tech Stack:** C++17, Vulkan, generated SPIR-V assembly, SPIRV-Tools validation, the existing graphics integration executable, native `kyty_agent` runtime diagnostics.

**Spec:** `docs/kyty-runtime-graphics-investigation-handoff.md`

## Global Constraints

- Work only in the active checkout; preserve all overlapping and unrelated dirty changes.
- No title, address, content, permissive, fallback, or semantic rendering behavior. The probe stays disabled by default and selected only by the existing checksum/kind/count contract.
- Do not change the guest `PARAM0`, position, descriptor bindings, interpolation, sample coordinates, draw, or pipeline state.
- Reuse the current renderer-owned buffer and one-shot fence lifecycle. No new logger, descriptor set, runner, or per-frame output.
- Extend the existing integration mode; do not add a unit-test suite. Build at `-j1` with no guest active, a hard memory cap, and zero cgroup swap.
- At most one data-bearing corrected strict guest run. A corrected launch that is stopped before the first input because cold-cache compilation prevents the required 8000-present precondition does not count as an occurrence; allow one cache-warm relaunch, with the same hard limits and an early rate check. If its capture is not the damaged 3D occurrence, treat values as occurrence-local evidence only.

---

### Task 1: Add the failing aggregate contract to the existing integration mode

**Files:**
- Modify: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`

- [x] Change the expected raw layout from 10 to 16 uints and require host initialization of `param0_exports`, `param0_nonfinite`, `min/max_param0_x`, and `min/max_param0_y`.
- [x] Extend the synthetic VS fixture with full-mask and XY-only `EXP PARAM0` before `EXP POS0`; require the selected source to preserve the original `%param0` store and contain PARAM0-specific uint atomic count/non-finite/extrema operations whenever X and Y are both enabled. Reject a mask that omits either component.
- [x] Require the compact event format to include `p0n`, `p0nf`, `p0fin`, `p0x`, and `p0y` without exceeding the Agent message bound.
- [x] Run only `--vertex-clip-probe-contract-only` and retain the expected RED result before implementation.

### Task 2: Extend the host raw contract and selected SPIR-V

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/VertexClipProbe.h`
- Modify: `source/emulator/src/Graphics/VertexClipProbe.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirvGenerator.cpp`
- Modify: `source/emulator/src/Graphics/ShaderSpirvVector.cpp`
- Modify only if a helper is required: `source/emulator/src/Graphics/ShaderSpirvInternal.h`

- [x] Append six uints to `VertexClipProbeRawStats`: export count, non-finite count, X min/max, Y min/max. Keep the original first ten offsets unchanged.
- [x] Add explicit SPIR-V member offsets 40 through 60 and expand the struct type to 16 uints.
- [x] After the existing selected `EXP PARAM0` value is assembled and stored, count the export once per invocation when enable-mask bits X and Y are both set. Reject NaN/Inf X or Y as one non-finite observation; otherwise update ordered-float X/Y extrema with core `OpAtomicUMin/UMax`.
- [x] Do not instrument PARAM1+ or PARAM0 exports missing X or Y, and do not change the stored value or control flow visible after the export merge.
- [x] Extend the bounded native event and secondary Silent-safe log with the PARAM0 aggregate. A missing PARAM0 export must remain explicit as `p0fin=0`, not appear as zeros.

### Task 3: Validate once, review, then run one bounded strict occurrence

**Files:**
- Modify only for evidence: files from Tasks 1–2
- Modify: `docs/kyty-runtime-graphics-investigation-handoff.md`

- [x] Build the existing graphics integration target at `-j1` under `MemoryHigh=1G`, `MemoryMax=1536M`, `MemorySwapMax=0`, `CPUQuota=100%`; run only its vertex probe contract and real blocking/nonblocking fence CTests.
- [x] Have an independent reviewer check the preserved first ten offsets, PARAM0-only targeting, NaN/Inf counting, ordered extrema, compact message bound, and absence of guest-visible changes.
- [x] Build `fc_script` once at `-j1` under the established 2 GiB/2.5 GiB zero-swap envelope. Never overlap it with a guest.
- [x] Run one strict Silent checksum/`indexed:3564` occurrence with the existing two-edge route and require one fence-completed event. If the event is empty, stop, prove the selection/export mismatch offline, correct only that diagnostic contract, then allow one bounded corrected retry. If that launch cannot reach the pre-input 8000-present condition because it is still compiling the revised diagnostic cache, stop it before input and allow one cache-warm relaunch under identical hard limits. Capture at most once across successful data-bearing runs.
- [x] Classify before changing semantics: malformed PARAM0 keeps the VS varying producer active; finite plausible PARAM0 moves the aggregate to PS logical input zero immediately before the implicit sample.
- [x] Record exact evidence and limitations, run `git diff --check`, and confirm no generated artifacts entered the repository. Do not claim restored 3D or playability from the probe.
