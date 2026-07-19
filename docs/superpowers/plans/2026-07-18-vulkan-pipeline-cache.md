# Vulkan Pipeline Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the multi-hundred-millisecond Vulkan pipeline compilation hitches observed during strict runtime runs by safely reusing driver pipeline cache data across processes.

**Architecture:** A small renderer-owned cache store validates Vulkan's standard cache header against the selected physical device, loads only bounded compatible data, and atomically persists refreshed data. `GraphicsRender` remains the owner of `VkPipelineCache`; successful pipeline misses mark it dirty and trigger rate-limited snapshots because the current window shutdown ends with `_Exit` and cannot rely on destructors. Invalid, stale, oversized, or unreadable cache files are ignored without changing guest behavior.

**Tech Stack:** C++17, Vulkan 1.2, `std::filesystem` with error codes, GoogleTest through `fc_script`, CMake/Ninja.

## Constraints and decision

- Keep all work in the current worktree and branch.
- Never store private workload paths, identifiers, assets, or shader/pipeline keys in Git.
- Treat SharpEmu only as clean-room architectural evidence because its implementation is GPL.
- Option 1, persistent driver `VkPipelineCache`: chosen because captures directly show 200–500 ms cold pipeline misses and Vulkan provides a device-qualified opaque cache.
- Option 2, persist Kyty's complete SPIR-V and application pipeline keys: potentially larger benefit but substantially broader serialization/versioning work.
- Option 3, asynchronous compilation with a provisional pipeline: rejected because drawing with a substitute pipeline is a behavioral fallback and violates rendering correctness.
- Cache failure is a performance miss, never a guest-visible failure or fabricated rendering success.
- Cap input blobs, validate the Vulkan cache header, and derive policy from device properties rather than vendor-specific branches.

### Task 1: Establish the validation contract with a red test

**Files:**
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`
- Create later: `source/emulator/include/Emulator/Graphics/PipelineCacheStore.h`

- [x] Add tests proving that a standard Vulkan cache header is accepted only when header version, vendor ID, device ID, UUID, declared header length, and total bounded size match.
- [x] Add tests proving truncated, foreign-device, malformed-version, and oversized data is rejected.
- [x] Build and run `EmulatorGraphicsState.PipelineCache*`; record the expected missing-header compile failure.

### Task 2: Implement bounded cache loading and atomic storage

**Files:**
- Create: `source/emulator/include/Emulator/Graphics/PipelineCacheStore.h`
- Create: `source/emulator/src/Graphics/PipelineCacheStore.cpp`
- Modify: `source/emulator/CMakeLists.txt`

- [x] Implement the minimal pure header validator required by the tests.
- [x] Resolve an explicit `KYTY_VULKAN_PIPELINE_CACHE` override, otherwise a per-user cache directory, and include device identity plus pipeline UUID in the filename.
- [x] Read at most the configured maximum, ignore incompatible data, and write via a sibling temporary file plus atomic rename.
- [x] Run the focused tests green and `git diff --check`.

### Task 3: Connect Vulkan ownership and rate-limited persistence

**Files:**
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/GraphicContext.h`

- [x] Load validated initial data before `vkCreatePipelineCache`.
- [x] After each successful graphics/compute pipeline miss, mark the cache dirty.
- [x] Persist immediately for the first successful miss and at most once per five seconds thereafter; serialize under the renderer mutex that already owns pipeline creation.
- [x] Log only cache state and byte counts outside Silent mode; never log private workload data.
- [x] Build `fc_script` and run GraphicsState/GraphicsPackets regressions.

### Task 4: Prove cold/warm runtime behavior

**Files:**
- Create/update: ignored `_scratch_playable/` evidence only
- Modify: `docs/kyty-runtime-graphics-investigation-handoff.md`
- Modify: `docs/graphics-troubleshooting.md`

- [x] Run a cold strict Silent capture with an empty isolated cache path and record pipeline compile counts/times.
- [x] Run the same scenario warm with the generated cache and compare first-gameplay latency, compile stalls, presents, FPS, resolution, and cache state.
- [x] Verify correct gameplay pixels and no earlier strict failure.
- [x] Document the root cause, cache invalidation behavior, measurement method, remaining steady-state bottlenecks, and recovery steps without private fixture details.
- [ ] Commit and push only after focused tests and cold/warm evidence pass.
