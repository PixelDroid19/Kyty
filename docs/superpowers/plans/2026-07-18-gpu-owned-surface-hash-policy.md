# GPU-Owned Surface Hash Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove per-submit CPU hashing from GPU-owned tiled render and display surfaces whose update callbacks cannot upload guest memory.

**Architecture:** Express CPU-upload eligibility as a pure object-level policy and use that same policy to configure `GpuObject::check_hash` and guard the update callback. Keep hashing enabled for every CPU-backed or write-back resource so native guest writes remain observable until a safe page-dirty tracker exists.

**Tech Stack:** C++17, Vulkan object wrappers, XXH64-backed `GpuMemory`, Kyty unit-test harness, Ninja Release build.

## Global Constraints

- Work only in `/home/monasterios/Documents/PS5/Kyty`; do not create another worktree.
- Never copy GPL or third-party implementation code; use only clean-room architectural observations.
- Never put private workload paths, title identifiers, assets, screenshots, or raw runtime logs in tracked files or commit messages.
- Preserve strict behavior: no permissive GPU skips, fabricated synchronization, assumed formats, or behavioral fallbacks.
- Prove the current producer and add a deterministic failing test before editing production behavior.
- Keep hashing enabled whenever an update may consume CPU guest memory.

## Design Decision

Three realistic approaches were evaluated:

1. **Object ownership policy (selected):** disable hashing only when the existing update contract is a proven no-op: tiled `VideoOutBuffer`, and tiled `RenderTexture` without write-back. This has the smallest correctness surface and immediately removes large redundant reads.
2. **Replace XXH64 with XXH3:** lowers CPU time per byte but still scans unchanged multi-megabyte surfaces every submit and does not remove global-mutex residency. It remains a possible secondary optimization.
3. **Page-level dirty tracking:** write-protect guest pages, mark all overlapping owners dirty on first write, and hash only as a fallback. This offers the highest ceiling but requires async-signal-safe metadata, alias handling, protection restoration, and cross-platform tests; it is a separate implementation phase.

The selected change does not infer guest behavior. It derives hash eligibility from whether the existing object update callback can consume the hashed memory.

---

### Task 1: Characterize GPU-owned surface hash eligibility

**Files:**
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: `RenderTextureObject(..., bool tiled, ..., bool write_back)` and `VideoOutBufferObject(..., bool tiled, ...)`.
- Produces: tests that define `GpuObject::check_hash` for GPU-owned and CPU-backed variants.

- [x] **Step 1: Write the failing tests**

```cpp
TEST(EmulatorGraphicsState, GpuOwnedTiledRenderTextureSkipsCpuHash)
{
	const RenderTextureObject gpu_owned(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, true, false, 1280, false);
	const RenderTextureObject write_back(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, true, false, 1280, true);
	const RenderTextureObject linear(RenderTextureFormat::R8G8B8A8Unorm, 1280, 720, false, false, 1280, false);

	EXPECT_FALSE(gpu_owned.check_hash);
	EXPECT_TRUE(write_back.check_hash);
	EXPECT_TRUE(linear.check_hash);
}

TEST(EmulatorGraphicsState, TiledVideoOutBufferSkipsCpuHash)
{
	const VideoOutBufferObject tiled(VideoOutBufferFormat::R8G8B8A8Srgb, 1280, 720, true, false, 1280);
	const VideoOutBufferObject linear(VideoOutBufferFormat::R8G8B8A8Srgb, 1280, 720, false, false, 1280);

	EXPECT_FALSE(tiled.check_hash);
	EXPECT_TRUE(linear.check_hash);
}
```

- [x] **Step 2: Run the tests and verify the current constructors fail**

Run:

```bash
ninja -C _build_linux fc_script
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.GpuOwnedTiledRenderTextureSkipsCpuHash:EmulatorGraphicsState.TiledVideoOutBufferSkipsCpuHash'
```

Expected: both tests fail because both constructors currently set `check_hash = true`.

### Task 2: Centralize and implement CPU-upload eligibility

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/Objects/RenderTexture.h`
- Modify: `source/emulator/include/Emulator/Graphics/Objects/VideoOutBuffer.h`
- Modify: `source/emulator/src/Graphics/Objects/RenderTexture.cpp`

**Interfaces:**
- Consumes: `bool tiled`, `bool write_back`.
- Produces: `RenderTextureMayCpuUploadOnUpdate(bool tiled, bool write_back)` and the existing `VideoOutBufferShouldCpuUploadOnUpdate(bool tiled)` as the single policy sources.

- [x] **Step 1: Add the RenderTexture policy and use it in the constructor**

```cpp
[[nodiscard]] inline bool RenderTextureMayCpuUploadOnUpdate(bool tiled, bool write_back)
{
	return !tiled || write_back;
}
```

Set `check_hash = RenderTextureMayCpuUploadOnUpdate(tiled, write_back);`.

- [x] **Step 2: Reuse the policy in the RenderTexture update callback**

```cpp
const bool write_back = params[RenderTextureObject::PARAM_WRITE_BACK] != 0;
if (!RenderTextureMayCpuUploadOnUpdate(tiled, write_back))
{
	return;
}
```

- [x] **Step 3: Use the existing VideoOut policy in its constructor**

Set `check_hash = VideoOutBufferShouldCpuUploadOnUpdate(tiled);`.

- [x] **Step 4: Run the focused tests**

Run:

```bash
ninja -C _build_linux fc_script
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.GpuOwnedTiledRenderTextureSkipsCpuHash:EmulatorGraphicsState.TiledVideoOutBufferSkipsCpuHash:EmulatorGraphicsState.TiledVideoOutBufferUpdateDoesNotCpuUpload'
```

Expected: all three tests pass.

### Task 3: Regression and runtime evidence

**Files:**
- Temporarily modify and then restore: `source/emulator/src/Graphics/Objects/GpuMemory.cpp`
- Modify after measurement: `docs/kyty-runtime-graphics-investigation-handoff.md`

**Interfaces:**
- Consumes: aggregate hash call/byte/time counters grouped by `GpuMemoryObjectType`.
- Produces: sanitized before/after measurements and a statement of remaining dominant object types.

- [x] **Step 1: Run the deterministic graphics regression**

Run:

```bash
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
```

Expected: all focused graphics tests pass.

- [x] **Step 2: Add temporary opt-in hash counters**

Count calls, bytes, and elapsed time around `calc_hash` by object type only when an untracked diagnostic environment variable is enabled. Emit at a bounded interval and do not print guest addresses.

- [x] **Step 3: Measure a fixed Release/Silent runtime interval**

Run `fc_script` with the private root supplied only through `$KYTY_GUEST_ROOT`, fixed 1280×720 output, the same shader-cache state, and identical discovery input conditions as the baseline. Record hash calls, bytes, and time beneath ignored scratch.

Expected: tiled RenderTexture and VideoOutBuffer contribute zero hash bytes; visual progress reaches the previous gameplay checkpoint without an earlier strict failure.

- [x] **Step 4: Remove all temporary counters**

Run:

```bash
git diff -- source/emulator/src/Graphics/Objects/GpuMemory.cpp
```

Expected: no diff remains in `GpuMemory.cpp`.

- [x] **Step 5: Document results and residual risks**

Record the redundant hash producer, ownership-based solution, focused test counts, runtime conditions, measured change, and the remaining page-dirty-tracking work. Do not record the private title identity or path.

### Task 4: Final verification and publish

**Files:**
- Review every modified file listed above.

**Interfaces:**
- Consumes: green focused tests, runtime evidence, and sanitized documentation.
- Produces: one reviewable performance commit pushed to `origin/codex/graphics-runtime-fixes`.

- [x] **Step 1: Rebuild runtime and rerun tests**

```bash
ninja -C _build_linux fc_script
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
```

- [x] **Step 2: Review scope and privacy**

```bash
git diff --check
git diff --stat
git diff
git status --short
rg -n 'PPSA|Dead Cells|/home/monasterios/Documents/PS5/Games' docs source
```

Expected: no whitespace errors, no temporary probes, and no newly introduced private fixture references.

- [ ] **Step 3: Commit and push**

```bash
git add docs/superpowers/plans/2026-07-18-gpu-owned-surface-hash-policy.md \
  docs/kyty-runtime-graphics-investigation-handoff.md \
  source/emulator/include/Emulator/Graphics/Objects/RenderTexture.h \
  source/emulator/include/Emulator/Graphics/Objects/VideoOutBuffer.h \
  source/emulator/src/Graphics/Objects/RenderTexture.cpp \
  source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git commit -m "perf(graphics): skip hashes for GPU-owned surfaces"
git push origin codex/graphics-runtime-fixes
```

Expected: the push updates `origin/codex/graphics-runtime-fixes` to the new commit.
