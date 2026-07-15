# Curated Gen5 Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconstruct the current Gen5 compatibility work on a clean branch from `main`, preserving evidenced behavior while excluding fabricated IPC, unsafe trap/dialog assumptions, diagnostic probes from semantic commits, and every `Co-authored-by` trailer.

**Architecture:** Treat the existing branches as read-only evidence sources, not merge targets. Reimplement or cherry-pick one net contract at a time into a third worktree, validate each batch with focused tests and the strict/visual fixture, then reconcile the primary dirty patch file by file. `main` remains untouched until the curated branch reaches the same or later frontier twice.

**Tech Stack:** Git worktrees, CMake/Ninja, C++17, GoogleTest through `fc_script`, Vulkan validation when available, strict private-fixture replay through `$KYTY_GUEST_ROOT`.

## Global Constraints

- Read `/home/monasterios/Documents/PS5/Kyty/AGENTS.md` completely before every execution session.
- Source worktrees `/home/monasterios/Documents/PS5/Kyty` and `/home/monasterios/Documents/PS5/Kyty-gen5-compositor` are read-only evidence during curation.
- Preserve all existing stashes, untracked scratch, and dirty files; do not stash, reset, clean, rebase, checkout, or force-push either source worktree.
- Create `/home/monasterios/Documents/PS5/Kyty-gen5-curated` on `codex/gen5-curated-integration` from `main` only through the `superpowers:using-git-worktrees` skill.
- Never use `KYTY_STUB_MISSING`, `KYTY_GFX_PERMISSIVE`, or `KYTY_SKIP_UD2` for acceptance.
- Never copy private fixture paths, identifiers, assets, screenshots, binaries, raw logs, or generated shader dumps into Git.
- Every semantic commit has one contract, a short subject, no `Co-authored-by` trailer, focused tests, a successful build, and a strict/visual replay.
- Do not preserve an intermediate commit known to be semantically wrong merely because a later commit repairs it; integrate the correct net behavior once.
- Do not copy `TexProbe` workload-specific dimensions/CRCs, `KYTY_*_EVIDENCE` hot-path dumps, auto-input, fabricated dialog state, trap skipping, or assumed guest success into a semantic commit.
- Use `apply_patch` for manual source edits. Git cherry-pick is allowed only for the exact commits identified below and only in the clean curated worktree.
- Execute every emulator/test binary from the curated worktree and route shader/log/buffer/pipeline output to `/home/monasterios/Documents/PS5/Kyty-curated-scratch`; never let relative output land in the dirty source worktree.
- After adding a red test, rebuild `fc_script` before running it. A result from the pre-edit binary or a GoogleTest filter that matched zero tests is invalid evidence.

---

### Task 1: Create and prove the isolated baseline

**Files:**
- Read: `/home/monasterios/Documents/PS5/Kyty/AGENTS.md`
- Read: `/home/monasterios/Documents/PS5/Kyty/docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md`
- Create through worktree skill: `/home/monasterios/Documents/PS5/Kyty-gen5-curated/`

**Interfaces:**
- Consumes: immutable audited code frontier `15e1552e46b27e928b33e869111ca9d521702612`, `main=0b708016142cf8ce304675fd8f9f3cd547e2362b`, and `compositor-fix=b9abd4553e8766ef60fd555a649ca2102096f3e7`.
- Produces: clean branch `codex/gen5-curated-integration` and exclusive build directory `_build_linux_curated`.

- [ ] **Step 1: Verify the audited graph has not drifted**

```bash
set -euo pipefail
source_repo=/home/monasterios/Documents/PS5/Kyty
compositor_repo=/home/monasterios/Documents/PS5/Kyty-gen5-compositor
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/baseline"
test "$(git -C "$source_repo" rev-parse main)" = 0b708016142cf8ce304675fd8f9f3cd547e2362b
test "$(git -C "$compositor_repo" rev-parse HEAD)" = b9abd4553e8766ef60fd555a649ca2102096f3e7
test "$(git -C "$source_repo" merge-base codex/gen5-render-frontier codex/gen5-compositor-fix)" = 15e1552e46b27e928b33e869111ca9d521702612
git -C "$source_repo" merge-base --is-ancestor 15e1552e46b27e928b33e869111ca9d521702612 codex/gen5-render-frontier
git -C "$source_repo" status --short
git -C "$compositor_repo" status --short
git -C "$source_repo" stash list
git -C "$source_repo" rev-parse HEAD > "$scratch/baseline/source-head"
git -C "$source_repo" status --porcelain=v2 -z > "$scratch/baseline/source-status"
git -C "$source_repo" stash list > "$scratch/baseline/source-stashes"
git -C "$compositor_repo" rev-parse HEAD > "$scratch/baseline/compositor-head"
git -C "$compositor_repo" status --porcelain=v2 -z > "$scratch/baseline/compositor-status"
git -C "$compositor_repo" stash list > "$scratch/baseline/compositor-stashes"
```

Expected: the hashes/merge-base match; source dirty state and all stashes are recorded but unchanged.

- [ ] **Step 2: Create the new worktree through the required skill**

Invoke `superpowers:using-git-worktrees` with base `main`, branch `codex/gen5-curated-integration`, and destination `/home/monasterios/Documents/PS5/Kyty-gen5-curated`.

- [ ] **Step 3: Configure and build the trusted baseline**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
cmake -S "$curated/source" -B "$curated/_build_linux_curated" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*:EmulatorKernelMemory.*:EmulatorPad.*'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
```

Expected: build succeeds and the focused baseline is green. Record any pre-existing missing suite instead of hiding it with an unfiltered run.

- [ ] **Step 4: Reproduce and classify the first strict frontier twice**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
```

Expected: both runs produce the same first structural failure or the same first bad rendered state. Save complete evidence outside Git. If an HLE/ABI failure occurs before graphics, execute the evidence-driven HLE cycle in Task 9 before applying graphics Tasks 3–8; do not use a later visual symptom as the baseline.

- [ ] **Step 5: Record the baseline without committing private evidence**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
git -C "$curated" status --short
git -C "$curated" log -1 --oneline --decorate
```

Expected: clean worktree at `main`; no commit is created in this task.

### Task 2: Import the reviewed engineering documentation

**Files:**
- Modify by cherry-pick: `AGENTS.md`
- Create by cherry-pick: `docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md`
- Create by cherry-pick: `docs/superpowers/plans/2026-07-14-curated-gen5-integration.md`
- Create by cherry-pick: `docs/superpowers/plans/2026-07-14-runtime-stall-snapshot-v1.md`

**Interfaces:**
- Consumes: reviewed commits `210c239` and `b30bdcc`, the latest reviewed single-file amendment of the diagnostics specification, plus the latest reviewed single-file commit for each execution plan discovered from the immutable source worktree. The amendment and both plan files must already exist as reviewed, single-file commits before Task 1 starts; dirty or untracked documentation is an execution blocker.
- Produces: curated branch governed by the same reviewed rules, accepted diagnostics specification, and executable plans.

- [ ] **Step 1: Resolve and verify the specification amendment and two plan commits**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
source_repo=/home/monasterios/Documents/PS5/Kyty
spec_amendment_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md)
curated_plan_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/plans/2026-07-14-curated-gen5-integration.md)
runtime_plan_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/plans/2026-07-14-runtime-stall-snapshot-v1.md)
test "$spec_amendment_commit" != b30bdcc
git -C "$source_repo" merge-base --is-ancestor b30bdcc "$spec_amendment_commit"
test "$(git -C "$source_repo" diff-tree --no-commit-id --name-only -r "$spec_amendment_commit")" = docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md
test "$(git -C "$source_repo" diff-tree --no-commit-id --name-only -r "$curated_plan_commit")" = docs/superpowers/plans/2026-07-14-curated-gen5-integration.md
test "$(git -C "$source_repo" diff-tree --no-commit-id --name-only -r "$runtime_plan_commit")" = docs/superpowers/plans/2026-07-14-runtime-stall-snapshot-v1.md
test "$spec_amendment_commit" != "$curated_plan_commit"
test "$spec_amendment_commit" != "$runtime_plan_commit"
test "$curated_plan_commit" != "$runtime_plan_commit"
```

- [ ] **Step 2: Cherry-pick only the five reviewed documentation commits**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
source_repo=/home/monasterios/Documents/PS5/Kyty
spec_amendment_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md)
curated_plan_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/plans/2026-07-14-curated-gen5-integration.md)
runtime_plan_commit=$(git -C "$source_repo" log -1 --format='%H' -- docs/superpowers/plans/2026-07-14-runtime-stall-snapshot-v1.md)
git -C "$curated" cherry-pick 210c239 b30bdcc "$spec_amendment_commit" "$curated_plan_commit" "$runtime_plan_commit"
git -C "$curated" diff --check main..HEAD
```

Expected: five documentation commits, no source changes.

- [ ] **Step 3: Prove full bodies and patches contain no prohibited trailer or private fixture name**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test -n "${KYTY_GUEST_ROOT:-}"
private_fixture_name=$(basename "$KYTY_GUEST_ROOT")
commit_messages=$(git -C "$curated" log main..HEAD --format='%B')
history_patch=$(git -C "$curated" log -p --format='%H%n%B' main..HEAD)
set +e
rg -ni '^Co-authored-by:' <<<"$commit_messages"
coauthor_status=$?
rg -F "$private_fixture_name" <<<"$history_patch"
private_status=$?
set -e
test "$coauthor_status" -eq 1
test "$private_status" -eq 1
```

Expected: both searches return no matches.

### Task 3: Freeze existing tile-27 layout with characterization tests

**Files:**
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`
- Do not create: `source/emulator/include/Emulator/Graphics/Objects/TexProbe.h`
- Do not create: `source/emulator/src/Graphics/Objects/TexProbe.cpp`

**Interfaces:**
- Consumes: `TileGetSw64kRxOffset` and `TileConvertSw64kRxToLinear`, already present on `main`; only the additional deterministic tests come from `84e855a`.
- Produces: characterization coverage for x-ramp detile, macro-pitch block advance, and the observed format-56 size calculation. This task makes no runtime change.

- [ ] **Step 1: Add only the three missing deterministic layout tests**

Add `Sw64kRx4bppDetileRampPreservesX`, `Sw64kRx4bppMacroPitchAdvancesBlock`, and `SizesGen5RotatedXGbuffer642x362Rgba8MatchesSample56`. `Sw64kRx4bppWithinBlockIsBijective` and the implementation already exist on `main`; do not duplicate or edit them. Do not add fixed workload CRCs, shader IDs, or evidence environment variables.

- [ ] **Step 2: Build and confirm the characterization is green on `main` behavior**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsPackets.Sw64kRx4bppDetileRampPreservesX:EmulatorGraphicsPackets.Sw64kRx4bppMacroPitchAdvancesBlock:EmulatorGraphicsPackets.SizesGen5RotatedXGbuffer642x362Rgba8MatchesSample56'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
```

Expected: all three tests pass without editing `Tile.h` or `Tile.cpp`. A failure reopens one layout hypothesis; do not copy the historical implementation over `main`.

- [ ] **Step 3: Prove the commit is test-only and commit it**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test "$(git -C "$curated" diff --name-only)" = source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git -C "$curated" add source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git -C "$curated" commit -m 'test(graphics): characterize Gen5 tile 27 layout'
```

Expected: one test-only commit; runtime sources remain byte-identical to `main` at this seam.

### Task 4: Rebuild viewport and depth-clear contracts sequentially

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicContext.h`
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/GraphicsState.cpp`
- Create: `source/emulator/include/Emulator/Graphics/Objects/DepthMeta.h`
- Create: `source/emulator/src/Graphics/Objects/DepthMeta.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Objects/DepthStencilBuffer.h`
- Modify: `source/emulator/src/Graphics/Objects/DepthStencilBuffer.cpp`
- Modify: `source/emulator/src/Graphics/Objects/StorageBuffer.cpp`
- Modify: `source/emulator/src/Graphics/Objects/GpuMemory.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Utils.h`
- Modify: `source/emulator/src/Graphics/Utils.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/emulator/src/Graphics/Window.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: observed depth/HTILE evidence isolated from `84e855a`.
- Produces three independently committed contracts: `State::ResolveViewportDepth`, explicit depth clear/load decisions, and one-use HTILE clear tracking keyed by exact range.

- [ ] **Step 1: Add and confirm red for viewport depth resolution**

Add only `ResolvesViewportDepthForClipSpaceAndHostLimits`, covering OpenGL/DX clip space with and without unrestricted host depth range. Add a compile-only `ViewportDepthRange` declaration and `ResolveViewportDepth` stub returning the neutral zero-initialized range so the complete build succeeds without implementing the contract.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
cmake -S "$curated/source" -B "$curated/_build_linux_curated" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^EmulatorGraphicsState\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ResolvesViewportDepthForClipSpaceAndHostLimits$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.ResolvesViewportDepthForClipSpaceAndHostLimits' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+EmulatorGraphicsState\.ResolvesViewportDepthForClipSpaceAndHostLimits' <<<"$red_output"
```

Expected: the complete build succeeds and the exact named assertion is red. A compiler/linker failure is not accepted as the red signal. Step 2 replaces the stub; it does not add a second implementation.

- [ ] **Step 2: Implement, validate, and commit viewport depth**

Use an explicit return struct. `ResolveViewportDepth` maps guest clip-space state and clamps only when the host lacks unrestricted depth range. In `Window.cpp`, enumerate and enable `VK_EXT_depth_clip_control`/`VK_EXT_depth_range_unrestricted` only when supported, chain the depth-clip-control feature query/enablement, and store the resulting booleans in `GraphicContext`. Guard compatibility declarations for the vendored header with extension macros. Select behavior from actual extensions/features, never vendor IDs.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.ResolvesViewportDepthForClipSpaceAndHostLimits'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicContext.h source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/src/Graphics/GraphicsState.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/emulator/src/Graphics/Window.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): resolve Gen5 viewport depth range'
```

- [ ] **Step 3: Add and confirm red for explicit depth clear/load decisions**

Add only `SeparatesHtileMetaClearFromRegisterDepthClear` and `DepthAttachmentLoadOpsClearWhenGuestDepthClear`. Add compile-only declarations/stubs for the two decision seams that return their existing neutral load/no-clear defaults; do not change renderer call sites yet.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  SeparatesHtileMetaClearFromRegisterDepthClear$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.SeparatesHtileMetaClearFromRegisterDepthClear' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+EmulatorGraphicsState\.SeparatesHtileMetaClearFromRegisterDepthClear' <<<"$red_output"
```

Expected: red at the missing decision seam or named assertions, never a zero-match green run.

- [ ] **Step 4: Implement and commit explicit depth clear/load decisions**

`ResolveDepthClearActions` distinguishes guest register clear from an observed pending HTILE clear. `ResolveDepthAttachmentLoadOps` selects `CLEAR` only from those explicit actions and known attachment state; it never invents a clear for an unknown layout.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsState.SeparatesHtileMetaClearFromRegisterDepthClear:EmulatorGraphicsState.DepthAttachmentLoadOpsClearWhenGuestDepthClear'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/include/Emulator/Graphics/Utils.h source/emulator/src/Graphics/GraphicsState.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): preserve explicit depth clears'
```

- [ ] **Step 5: Add `DepthMeta` tests against compile-only exact-range stubs**

Add `RecognizesObservedHtileClearPattern`, `ConsumesTrackedHtileClearOnce`, `HtilePendingClearDoesNotSuppressDepthWrite`, and `MatchesOnlyExactHtileStorageRange`. Create `DepthMeta.h/.cpp` with only compile/link stubs that reject every pattern/range and never retain a mark; do not integrate any owner yet. Reconfigure because the legacy source glob does not discover the new `.cpp` automatically, build the complete host, then prove the exact pattern test fails:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
cmake -S "$curated/source" -B "$curated/_build_linux_curated" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  RecognizesObservedHtileClearPattern$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.RecognizesObservedHtileClearPattern' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+EmulatorGraphicsState\.RecognizesObservedHtileClearPattern' <<<"$red_output"
```

After red, replace the stubs with the single `DepthMeta` owner. It owns only observed pending-clear records and exact storage-range matching. Document ownership and locking. A record is inserted by an evidenced HTILE clear pattern and consumed once at render-pass begin; partial/zero ranges do not match.

- [ ] **Step 6: Integrate, validate, and commit HTILE metadata tracking**

Thread HTILE address and size through `DepthStencilBufferObject`. Extend the private `StorageVulkanBuffer` state in `GraphicContext.h`, and let `GpuMemory.cpp` associate it only when the StorageBuffer range exactly matches the HTILE range. Recognize the observed clear pattern in `StorageBuffer`, centralize depth aspect-mask handling in `Utils.cpp`, and consume only a matching pending record in `GraphicsRender`. Keep guest state in `GraphicsState`; keep Vulkan structs/barriers in `GraphicsRender.cpp`.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
cmake -S "$curated/source" -B "$curated/_build_linux_curated" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" diff --check
```

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicContext.h source/emulator/include/Emulator/Graphics/Objects/DepthMeta.h source/emulator/src/Graphics/Objects/DepthMeta.cpp source/emulator/include/Emulator/Graphics/Objects/DepthStencilBuffer.h source/emulator/src/Graphics/Objects/DepthStencilBuffer.cpp source/emulator/src/Graphics/Objects/GpuMemory.cpp source/emulator/src/Graphics/Objects/StorageBuffer.cpp source/emulator/src/Graphics/Utils.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): preserve observed Gen5 depth clears'
```

Expected: green build/tests, same or later strict/visual frontier, and no whitespace errors. Inspect each shared-file commit to prove later-contract hunks were not staged early.

### Task 5: Rebuild multi-render-target and clear behavior

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/GraphicsState.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Utils.h`
- Modify: `source/emulator/src/Graphics/ShaderSpirv.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: net contracts from `84e855a`, `fb614d7`, `38bf9db`, and `b15e1fc`.
- Produces four independently committed contracts: up to eight explicit color attachment states with per-RT nibble masks/blend state, packed-half export preservation, correct attachment clear/load behavior, and exec-mask guarded color exports.

- [ ] **Step 1: Add exact red tests for normalized multi-RT layout**

Add `ResolvesContiguousMultiRenderTargetLayout`, `RejectsGappedMultiRenderTargetLayout`, and `RejectsPartialChannelMultiRenderTargetLayout` around a new pure `State::ResolveColorTargetLayout(uint32_t mask)` result containing bounded count, eight nibbles, and an explicit error enum. Cover zero targets, one target, four contiguous `0xf` nibbles, all eight full nibbles, a hole, and a partial nibble. Existing `DecodesBlendControl` already characterizes all eight blend slots; do not duplicate it. Exclude color-clear, shader export, screenshot, CRC, shader-ID, and private-dimension assertions from this first commit.

Declare the typed result and add a compile-only implementation that preserves
the baseline single-target behavior: mask zero yields zero targets; any nonzero
mask yields one target containing only its low nibble and no typed error. It
does not recognize holes or partial masks. The new suite must build and list
before the production decoder exists; all three exact contracts are then
classified independently.

- [ ] **Step 2: Confirm red with the focused filter**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
for exact_name in ResolvesContiguousMultiRenderTargetLayout RejectsGappedMultiRenderTargetLayout RejectsPartialChannelMultiRenderTargetLayout; do
  if ! rg -q "^  ${exact_name}$" <<<"$listed_tests"; then exit 1; fi
  exact_test="EmulatorGraphicsState.${exact_name}"
  set +e
  red_output=$("$curated/_build_linux_curated/fc_script" --gtest_color=no --gtest_filter="$exact_test" '{kyty_run_tests()}' 2>&1)
  red_status=$?
  set -e
  printf '%s\n' "$red_output"
  test "$red_status" -ne 0
  rg -Fqx "[ RUN      ] $exact_test" <<<"$red_output"
  rg -Fq '[==========] 1 test from 1 test suite ran.' <<<"$red_output"
  test "$(rg -Fxc "[  FAILED  ] $exact_test" <<<"$red_output")" -eq 1
done
```

Expected: build and registration succeed, then the exact success-path test fails
against the explicit `Unsupported` stub. Configure/compiler errors and a
zero-test run are invalid red evidence.

- [ ] **Step 3: Implement normalized per-RT state**

Decode direct and indirect state through the same `GraphicsState` functions. `ResolveColorTargetLayout` stores exactly eight bounded slots with explicit active count. The observed 32-bit contract accepts only contiguous full-channel (`0xf`) nibbles; holes or partial nibbles return a typed error that the renderer reports structurally with register/submit evidence.

- [ ] **Step 4: Implement Vulkan attachment arrays and commit only multi-RT state**

Build render-pass attachments, image views, blend attachments, and invalidation loops from the normalized active count. Do not index slot zero as a proxy for every attachment. Preserve the existing attachment load behavior in this commit; Task 5 Step 6 owns the separate clear/load decision.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/src/Graphics/GraphicsState.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): preserve Gen5 multi-target output'
```

Expected: same or later strict/visual frontier; no evidence-only environment variable is required.

- [ ] **Step 5: Add, implement, and commit packed-half exports**

Port the sanitized tests from `fb614d7`, confirm red, and implement only the packed-half MRT export translation in `ShaderSpirv.cpp`.

Before implementation, the ported test must compile and fail exactly:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^EmulatorGraphicsPackets\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  CompressedMrtExportReadsPackedHalfFromUintShadow$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsPackets.CompressedMrtExportReadsPackedHalfFromUintShadow' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsPackets.CompressedMrtExportReadsPackedHalfFromUintShadow' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsPackets.CompressedMrtExportReadsPackedHalfFromUintShadow' <<<"$red_output")" -eq 1
```

If the exact test is already green before any packed-half implementation hunk,
classify `fb614d7` as already integrated and omit this semantic commit; do not
manufacture a red test by weakening current behavior.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsPackets.CompressedMrtExportReadsPackedHalfFromUintShadow'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/src/Graphics/ShaderSpirv.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git -C "$curated" commit -m 'fix(graphics): preserve packed half MRT exports'
```

- [ ] **Step 6: Add, implement, and commit color attachment clears**

Port the pure load/clear tests from `38bf9db`, confirm red, then implement the explicit color attachment load/clear decision without default clears. Stage only the clear-related hunks in shared files.

Declare the typed `ResolveColorAttachmentLoadOps` seam used by the three tests
with a compile-only `Unsupported` result before touching renderer behavior.
Prove the first success path is red while build and registration are green:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
for exact_name in ColorAttachmentLoadOpsClearOnUndefinedFirstUse ColorAttachmentLoadOpsLoadOnOptimalSubsequentPass ColorAttachmentLoadOpsClearUsesGuestClearWords; do
  if ! rg -q "^  ${exact_name}$" <<<"$listed_tests"; then exit 1; fi
done
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsState.ColorAttachmentLoadOpsClearOnUndefinedFirstUse' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsState.ColorAttachmentLoadOpsClearOnUndefinedFirstUse' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsState.ColorAttachmentLoadOpsClearOnUndefinedFirstUse' <<<"$red_output")" -eq 1
```

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsState.ColorAttachmentLoadOpsClearOnUndefinedFirstUse:EmulatorGraphicsState.ColorAttachmentLoadOpsLoadOnOptimalSubsequentPass:EmulatorGraphicsState.ColorAttachmentLoadOpsClearUsesGuestClearWords'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/Utils.h source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): honor Gen5 color attachment clears'
```

- [ ] **Step 7: Add, implement, and commit exec-mask guarded exports**

Port the sanitized tests from `b15e1fc`, confirm red, and guard only color exports with the shader execution mask.

Run the exact ported test before implementation. If it is already green, omit
the commit as already integrated; otherwise require attributed red evidence:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  CompressedMrtExportIsGuardedByExecMask$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsPackets.CompressedMrtExportIsGuardedByExecMask' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsPackets.CompressedMrtExportIsGuardedByExecMask' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsPackets.CompressedMrtExportIsGuardedByExecMask' <<<"$red_output")" -eq 1
```

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsPackets.CompressedMrtExportIsGuardedByExecMask'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/src/Graphics/ShaderSpirv.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git -C "$curated" commit -m 'fix(graphics): guard color exports with exec mask'
```

After each commit, inspect `git show --stat --oneline HEAD` and `git status --short`; a shared-file hunk from a later contract must remain unstaged until its own step.

### Task 6: Require exact sampled render-target backing without `TexProbe`

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/GraphicsState.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: the final exact-backing correction `bd90f69` and the tile-27 4-byte guest-memory uploader already present on `main`; the dimension-based promotion in `84e855a` is negative evidence and must never be recreated.
- Produces: `Gen5SampleBacking` plus `ResolveGen5SampleBacking`, which reuses only an exact live render-target object, permits the evidenced format-56 guest-memory detile path, and rejects unsupported tile/format backing without workload-specific probes.

- [ ] **Step 1: Add the final exact-backing decision test**

Port only `Gen5SampleBackingRequiresExactLiveRenderTarget` from `bd90f69`. Assert exact live RT reuse, format-56 tile-27 guest-memory upload when no RT exists, unsupported format-14/71 tile-27 without an exact RT, and ordinary guest-memory texture handling for a supported non-tile-27 case. Do not add the superseded size-registry tests.

Declare `Gen5SampleBacking { Unsupported, ExactRenderTarget,
GuestMemoryTexture }` and `ResolveGen5SampleBacking` first. Its compile-only
baseline returns `ExactRenderTarget` when the caller supplies an exact live
object and otherwise returns `GuestMemoryTexture`; it deliberately does not yet
reject unsupported tile-27 format 14/71. This is the only accepted red stub and
must not enter the implementation commit.

- [ ] **Step 2: Confirm red**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test_filter='EmulatorGraphicsState.Gen5SampleBackingRequiresExactLiveRenderTarget'
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^EmulatorGraphicsState\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  Gen5SampleBackingRequiresExactLiveRenderTarget$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter="$test_filter" 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsState.Gen5SampleBackingRequiresExactLiveRenderTarget' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsState.Gen5SampleBackingRequiresExactLiveRenderTarget' <<<"$red_output")" -eq 1
```

Expected: green build/listing and one exact failed test against the
`Unsupported` stub; no unrelated build or suite failure is accepted.

- [ ] **Step 3: Implement the exact-object decision at the existing lookup seam**

Use the existing live-object lookup result in `PrepareTextures` as the sole render-target identity proof. An exact object yields `ExactRenderTarget`; absent backing with tile 27 and format 56 yields `GuestMemoryTexture`; absent tile-27 backing for formats without an evidenced CPU uploader yields `Unsupported` and a structured failure. Never create a render target from matching dimensions, size, format class, CRC, or prior observation. The GPU-owned render-texture layout preservation already on `main` remains unchanged.

- [ ] **Step 4: Prove no probe code entered the curated tree**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
set +e
git -C "$curated" grep -n 'TexProbe\|RecordGen5RenderTargetSize\|HasGen5RenderTargetSize\|Gen5Tile27SamplePrefersRenderTarget\|KYTY_VERTEX_EVIDENCE\|KYTY_BLEND_CONSTANT_EVIDENCE'
grep_status=$?
set -e
test "$grep_status" -eq 1
```

Expected: no matches.

- [ ] **Step 5: Build, test, replay, and commit**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/src/Graphics/GraphicsState.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): match sampled render-target backing'
```

### Task 7: Integrate label completion and final sampled-color semantics sequentially

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/Objects/Label.h`
- Modify: `source/emulator/src/Graphics/Objects/Label.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/Objects/Texture.h`
- Modify: `source/emulator/src/Graphics/Objects/Texture.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/GraphicsState.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: `c87e4d0`, the correct net of `d14f547+1281d5f`, and `b9abd45`.
- Produces three independently committed contracts: one callback per completed label, sampler-controlled RGBA8 degamma, and disabled comparison for the evidenced regular image-sample path. Depth-reference operation plumbing is deferred and receives no acceptance claim.

- [ ] **Step 1: Add a deterministic label transition test**

Introduce a test seam that feeds completed label records through worker drain and synchronous fence drain. Assert `Active -> Completed -> callback exactly once`, including both drains observing the same record.

Name the exact test
`EmulatorGraphicsState.LabelCompletionCallbackFiresOnceAcrossWorkerAndFenceDrains`.
Expose a small typed transition helper used under the existing LabelManager
mutex. Its compile-only baseline reports callbacks for every signaled
observation without changing state, so the worker-plus-fence test sees two
claims. Build, list, and attribute the red result before changing either real
drain:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  LabelCompletionCallbackFiresOnceAcrossWorkerAndFenceDrains$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsState.LabelCompletionCallbackFiresOnceAcrossWorkerAndFenceDrains' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsState.LabelCompletionCallbackFiresOnceAcrossWorkerAndFenceDrains' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsState.LabelCompletionCallbackFiresOnceAcrossWorkerAndFenceDrains' <<<"$red_output")" -eq 1
```

- [ ] **Step 2: Implement idempotent completion**

Centralize the transition in one `Label` function called by both paths. The function atomically claims completion before invoking the callback; the second caller observes the completed state and performs no callback.

- [ ] **Step 3: Validate and commit only label completion**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsState.*Label*'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/Objects/Label.h source/emulator/src/Graphics/Objects/Label.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): complete labels once after fences'
curated_status=$(git -C "$curated" status --short)
test -z "$curated_status"
```

Expected: label tests pass, strict replay reaches the same or later frontier, and the commit contains no sampler changes.

- [ ] **Step 4: Add and port only final RGBA8 degamma semantics**

Do not commit the intermediate “all format 56 is sRGB” state from `d14f547`. Add the final `Gen5SampledRgba8FormatUsesUnormByDefault` expectations from `1281d5f`; format 56 selects sRGB only from the evidenced `ForceDegamma/SkipDegamma` sampler contract.

Add the four-argument `TextureResolveSampledVkFormat(..., bool force_degamma)`
overload with a compile-only implementation that ignores the boolean and
returns the existing UNORM decision. The existing three-argument call remains
unchanged. Require the exact new test to compile, list, and fail on its forced
degamma assertion before implementation:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  Gen5SampledRgba8FormatUsesUnormByDefault$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsState.Gen5SampledRgba8FormatUsesUnormByDefault' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsState.Gen5SampledRgba8FormatUsesUnormByDefault' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsState.Gen5SampledRgba8FormatUsesUnormByDefault' <<<"$red_output")" -eq 1
```

- [ ] **Step 5: Validate and commit RGBA8 degamma**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test_filter='EmulatorGraphicsState.Gen5SampledRgba8FormatUsesUnormByDefault'
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/Objects/Texture.h source/emulator/src/Graphics/Objects/Texture.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): honor Gen5 sampler degamma'
```

- [ ] **Step 6: Disable comparison only for the evidenced regular sample path**

Add `RegularImageSamplingDisablesSamplerComparison`, confirm red, and make the sampler creation seam request the regular operation explicitly. Do not claim Dref support: the shader operation is not yet threaded into this cache key/call site, so depth-reference enablement remains a separate test-first contract when observed.

Declare `ImageSampleOperation` and `ResolveSamplerComparison` first with a
compile-only implementation that preserves the raw sampler comparison enable.
The regular-operation expectation must then be the sole red result:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^  RegularImageSamplingDisablesSamplerComparison$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='EmulatorGraphicsState.RegularImageSamplingDisablesSamplerComparison' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorGraphicsState.RegularImageSamplingDisablesSamplerComparison' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorGraphicsState.RegularImageSamplingDisablesSamplerComparison' <<<"$red_output")" -eq 1
```

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsState.RegularImageSamplingDisablesSamplerComparison'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/src/Graphics/GraphicsState.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" commit -m 'fix(graphics): disable comparison for regular samples'
```

Expected: the three commits contain no cross-contract hunks; Dref is neither silently advertised nor accepted by these tests.

### Task 8: Rebuild direct-memory lifetime and coherent aliases

**Files:**
- Modify: `source/emulator/src/Kernel/Memory.cpp`
- Modify: `source/include/Kyty/Core/VirtualMemory.h`
- Modify: `source/include/Kyty/Sys/Linux/SysLinuxVirtual.h`
- Modify: `source/include/Kyty/Sys/Windows/SysWindowsVirtual.h`
- Modify: `source/lib/Core/src/VirtualMemory.cpp`
- Modify: `source/lib/Sys/src/SysLinuxVirtual.cpp`
- Modify: `source/lib/Sys/src/SysWindowsVirtual.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorKernelMemory.cpp`
- Create: `source/unit_test/src/core/UnitTestCoreVirtualMemory.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: net behavior from `88a3827` and `a80cf54`, not their fabricated-history parents.
- Produces: allocation lifetime separated from mappings and coherent aliases mapped through platform boundary APIs.

- [ ] **Step 1: Port the focused lifetime/alias tests**

First port only `ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap` from
`88a3827`; it exercises the existing kernel API and needs no new production
declaration. After that behavior is committed, add the platform-neutral alias
test described in Step 4. Do not depend on dialog IPC state or use a broad suite
failure as red evidence.

- [ ] **Step 2: Confirm red on the curated branch**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test_filter='EmulatorKernelMemory.ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap'
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^EmulatorKernelMemory\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter="$test_filter" 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] EmulatorKernelMemory.ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] EmulatorKernelMemory.ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap' <<<"$red_output")" -eq 1
```

Expected: the runner builds and lists the exact test, then only that behavior
fails. A compiler error, process abort before `[ RUN ]`, or zero-test result is
not accepted.

- [ ] **Step 3: Implement and commit allocation/mapping lifetime**

Keep guest allocation/mapping policy in `Kernel/Memory.cpp`. Unmapping one view must not free the allocation while another live mapping exists. Run `EmulatorKernelMemory.*` and strict replay, then commit `Memory.cpp` and its lifetime tests as `fix(kernel): separate direct memory lifetimes` before adding alias APIs.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
"$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorKernelMemory.*'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" add source/emulator/src/Kernel/Memory.cpp source/unit_test/src/emulator/UnitTestEmulatorKernelMemory.cpp
git -C "$curated" commit -m 'fix(kernel): separate direct memory lifetimes'
```

- [ ] **Step 4: Add and implement coherent aliases**

Add `CoreVirtualMemory.SharedBackingPreservesAliasCoherence` only after Step 3
is committed. Declare the narrow shared-backing/create-alias API in `Core` and
platform headers with compile-only implementations that return an explicit
unsupported result. Register the new core suite, build successfully, list the
exact test, run only it, and require one `[  FAILED  ]` line against that stub
before implementing. This avoids the historical kernel fixture that aborts
before GTest can attribute the alias failure. Put policy in `Core` and OS
mapping calls in `SysLinuxVirtual.cpp`/`SysWindowsVirtual.cpp`; alias writes
remain coherent without guest-specific policy in platform files.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}')
if ! rg -q '^CoreVirtualMemory\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  SharedBackingPreservesAliasCoherence$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$curated/_build_linux_curated/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter='CoreVirtualMemory.SharedBackingPreservesAliasCoherence' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fqx '[ RUN      ] CoreVirtualMemory.SharedBackingPreservesAliasCoherence' <<<"$red_output"
test "$(rg -Fxc '[  FAILED  ] CoreVirtualMemory.SharedBackingPreservesAliasCoherence' <<<"$red_output")" -eq 1
```

- [ ] **Step 5: Run Linux tests and supported-host compile gates**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter='CoreVirtualMemory.SharedBackingPreservesAliasCoherence:EmulatorKernelMemory.ReusedDirectMemoryKeepsVirtualAliasesCoherent' '{kyty_run_tests()}')
rg -Fqx 'CoreVirtualMemory.' <<<"$listed_tests"
test "$(rg -Fxc '  SharedBackingPreservesAliasCoherence' <<<"$listed_tests")" -eq 1
rg -Fqx 'EmulatorKernelMemory.' <<<"$listed_tests"
test "$(rg -Fxc '  ReusedDirectMemoryKeepsVirtualAliasesCoherent' <<<"$listed_tests")" -eq 1
test_filter='CoreVirtualMemory.*:EmulatorKernelMemory.ReusedDirectMemoryKeepsVirtualAliasesCoherent'
"$curated/_build_linux_curated/fc_script" --gtest_filter="$test_filter" '{kyty_run_tests()}'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
```

Expected: Linux green. Before `main`, CI or available builders must compile macOS and Windows platform files; absence of those hosts is recorded as an unmet gate, not silently accepted.

- [ ] **Step 6: Commit the alias contract**

Commit only the alias APIs, platform adapters, and alias tests:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
git -C "$curated" add source/emulator/src/Kernel/Memory.cpp source/include/Kyty/Core/VirtualMemory.h source/include/Kyty/Sys/Linux/SysLinuxVirtual.h source/include/Kyty/Sys/Windows/SysWindowsVirtual.h source/lib/Core/src/VirtualMemory.cpp source/lib/Sys/src/SysLinuxVirtual.cpp source/lib/Sys/src/SysWindowsVirtual.cpp source/unit_test/src/emulator/UnitTestEmulatorKernelMemory.cpp
git -C "$curated" commit -m 'fix(kernel): preserve coherent direct memory aliases'
```

### Task 9: Interruptible strict HLE/ABI frontier gate

**Files:**
- Candidate files are limited per cycle to the owning seam among `source/emulator/src/Libs/`, `source/emulator/src/Loader/`, `source/emulator/src/Dialog.cpp`, `source/emulator/src/Kernel/Memory.cpp`, `source/lib/Core/src/MSpace.cpp`, and matching focused tests.
- Evidence sources: commits `1149171`, `5fda290`, `2ced411`, `2f9305f`, `ad1c150`, and `c9615dd`.

**Interfaces:**
- Consumes: the current first strict HLE/ABI failure whenever it appears—immediately after Task 1 or between any later tasks—plus local call-site evidence and sanitized unit inputs.
- Produces: one evidenced HLE/ABI contract per commit; no assumed success or automatic dialog state.

This is an interruptible gate, not a deferred sweep. If any earlier task's strict replay stops first in HLE/ABI, pause that task sequence, execute one Task 9 cycle, and resume only after the old first failure is gone. If the current first frontier is graphics or a bad rendered state, do not import historical HLE changes speculatively.

- [ ] **Step 1: Reproduce the first missing HLE/ABI contract twice**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
```

Expected: the same first failure or an explicitly documented race. Capture evidence outside Git.

- [ ] **Step 2: Classify the existing candidate changes before using them**

Allowed candidate seams are bounded ApplicationHeap PT_LOAD search, independently evidenced libc/CxaGuard behavior, ProcParam malloc wiring, real filesystem removal, independently evidenced SaveData unmount validation, sysmodule registry state, and an evidenced NID mapping. Explicitly reject trap skipping, GetGPI default zero, default SaveData success, auto-initialized dialogs, fabricated result structures, auto-finished dialog state, and zero-resource “clear all” unless the current strict capture proves that exact contract.

Strong historical characterization names include
`CapturedV2TablePassesFullValidation`, `RejectsHeaderOnlyFalsePositive`,
`AcquireClaimsGuardByteOnce`, `ReleaseMarksDoneAndAbortResets`, paired NID plus
behavior tests for `rand`/`strtok`, `NativeUmountNidResolves` with
`UmountRejectsNullMountPoint`, and explicit sysmodule registry transitions.
`MallocReplaceSlotOffsets`, retail-zero GPI, headless dialog auto-completion,
and auto-initialized dialog status are not admissible evidence. If the captured
seam has no strong existing test, add a new behavior-named test; never substitute
one of these candidates merely because it is nearby.

- [ ] **Step 3: Write one focused red test for the captured contract**

Name the test after the behavior, not the private workload. The test uses sanitized ABI arguments and asserts both success and failure behavior. A constant-offset test that only repeats the implementation is insufficient.

Set `CAPTURED_HLE_TEST` in the untracked session shell to the one fully
qualified test just added. Prove only that behavior is red; an already-green
test means the historical contract is present and the strict frontier must be
recaptured instead of patched:

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test -n "${CAPTURED_HLE_TEST:-}"
case "$CAPTURED_HLE_TEST" in *.*) ;; *) exit 1 ;; esac
suite=${CAPTURED_HLE_TEST%%.*}
case_name=${CAPTURED_HLE_TEST#*.}
ninja -C "$curated/_build_linux_curated" fc_script
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter="$CAPTURED_HLE_TEST" '{kyty_run_tests()}' 2>&1)
rg -Fqx "${suite}." <<<"$listed_tests"
test "$(rg -Fxc "  ${case_name}" <<<"$listed_tests")" -eq 1
set +e
red_output=$("$curated/_build_linux_curated/fc_script" --gtest_filter="$CAPTURED_HLE_TEST" '{kyty_run_tests()}' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fq "[ RUN      ] $CAPTURED_HLE_TEST" <<<"$red_output"
rg -Fq '[==========] 1 test from 1 test suite ran.' <<<"$red_output"
test "$(rg -Fxc "[  FAILED  ] $CAPTURED_HLE_TEST" <<<"$red_output")" -eq 1
```

- [ ] **Step 4: Implement only that owner seam and verify**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
test -n "${CAPTURED_HLE_TEST:-}"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter="$CAPTURED_HLE_TEST" '{kyty_run_tests()}' 2>&1)
suite=${CAPTURED_HLE_TEST%%.*}
case_name=${CAPTURED_HLE_TEST#*.}
rg -Fqx "${suite}." <<<"$listed_tests"
test "$(rg -Fxc "  ${case_name}" <<<"$listed_tests")" -eq 1
"$curated/_build_linux_curated/fc_script" --gtest_filter="$CAPTURED_HLE_TEST" '{kyty_run_tests()}'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
```

Expected: the old failure is gone and the next frontier is captured. If it is unchanged, revert the live experiment before another hypothesis.

- [ ] **Step 5: Commit and repeat the evidenced cycle**

Commit only the touched owner module and focused test with a short behavior subject. Repeat Steps 1–5 for the next first failure; do not batch the six historical HLE commits. Record every rejected historical assumption in untracked research notes.

### Task 10: Integrate input and wait-width contracts

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/Objects/VideoOutBuffer.h`
- Modify: `source/emulator/src/Graphics/Objects/VideoOutBuffer.cpp`
- Create: `source/emulator/include/Emulator/Graphics/KeyboardInput.h`
- Create: `source/emulator/src/Graphics/KeyboardInput.cpp`
- Modify: `source/emulator/src/Graphics/Window.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorPad.cpp`
- Modify: `source/emulator/src/Graphics/Graphics.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`

**Interfaces:**
- Consumes: `5fcd98b`, `2a31059`, and `b6a4e5e` as research inputs only; none enters branch ancestry.
- Produces: GPU-owned tiled VideoOut buffers, real keyboard left-stick press/release mapping, and 32-bit wait predicate preservation.

- [ ] **Step 1: Establish an attributable red gate for GPU-owned VideoOut buffers**

Inspect the historical diff and current production path, then reimplement only
the sanitized test `EmulatorGraphicsState.TiledVideoOutBufferUpdateDoesNotCpuUpload`.
If the exact test already exists, classify it before editing: green is evidence
to inspect the production call and omit this historical behavior; red proceeds
directly to Step 2. If the test is absent, add the narrow
`VideoOutBufferShouldCpuUploadOnUpdate(bool tiled)` seam, connect it to the real
update path, and preserve baseline behavior with a stub that always returns
`true`. The exact test must then fail once under GTest; a build error, crash,
missing test, or additional selected test is not an acceptable red gate.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
test_name=EmulatorGraphicsState.TiledVideoOutBufferUpdateDoesNotCpuUpload
classify_one_test() {
  local fc=$1
  local exact_name=$2
  local suite=${exact_name%%.*}
  local case_name=${exact_name#*.}
  local listed output status
  listed=$("$fc" --gtest_list_tests --gtest_color=no --gtest_filter="$exact_name" '{kyty_run_tests()}' 2>&1)
  rg -Fqx "${suite}." <<<"$listed"
  test "$(rg -Fxc "  ${case_name}" <<<"$listed")" -eq 1
  set +e
  output=$("$fc" --gtest_color=no --gtest_filter="$exact_name" '{kyty_run_tests()}' 2>&1)
  status=$?
  set -e
  printf '%s\n' "$output"
  rg -Fq "[ RUN      ] $exact_name" <<<"$output"
  rg -Fq '[==========] 1 test from 1 test suite ran.' <<<"$output"
  if test "$status" -eq 0; then
    rg -Fq "[       OK ] $exact_name" <<<"$output"
    rg -Fq '[  PASSED  ] 1 test.' <<<"$output"
    printf 'green\n'
    return 0
  fi
  rg -Fq "[  FAILED  ] $exact_name" <<<"$output"
  test "$(rg -Fxc "[  FAILED  ] $exact_name" <<<"$output")" -eq 1
  rg -Fq '[  FAILED  ] 1 test' <<<"$output"
  printf 'red\n'
}
ninja -C "$curated/_build_linux_curated" fc_script
classification=$(classify_one_test "$curated/_build_linux_curated/fc_script" "$test_name" | tee /dev/stderr | tail -n 1)
case "$classification" in red|green) ;; *) exit 1 ;; esac
```

Do not implement after a green classification until inspection proves that the
test reaches `VideoOutBufferObject::GetUpdateFunc()` behavior rather than a dead
helper. When red, change only the helper to return `!tiled`, retain the
production call, and prove exact green plus the owner suite and strict replay.

- [ ] **Step 2: Implement and verify GPU-owned VideoOut ownership when red**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter='EmulatorGraphicsState.TiledVideoOutBufferUpdateDoesNotCpuUpload' '{kyty_run_tests()}')
rg -Fqx 'EmulatorGraphicsState.' <<<"$listed_tests"
test "$(rg -Fxc '  TiledVideoOutBufferUpdateDoesNotCpuUpload' <<<"$listed_tests")" -eq 1
"$curated/_build_linux_curated/fc_script" --gtest_filter='EmulatorGraphicsState.TiledVideoOutBufferUpdateDoesNotCpuUpload' '{kyty_run_tests()}'
"$curated/_build_linux_curated/fc_script" --gtest_filter='EmulatorGraphicsState.*VideoOut*' '{kyty_run_tests()}'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" diff --check
git -C "$curated" add source/emulator/include/Emulator/Graphics/Objects/VideoOutBuffer.h source/emulator/src/Graphics/Objects/VideoOutBuffer.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git -C "$curated" diff --cached --check
git -C "$curated" commit -m 'fix(graphics): keep tiled VideoOut buffers GPU-owned'
```

- [ ] **Step 3: Establish an attributable red gate for keyboard movement**

The existing `KeyboardAxesAreReturnedByPadReadState` test injects controller
axes directly and does not prove Window key mapping. Add the exact test
`EmulatorPad.KeyboardMovementKeyPressReleaseMapsLeftStick` and a focused pure
input seam in `KeyboardInput.h/.cpp`:

- `KeyboardLeftStickState` owns `left`, `right`, `up`, and `down` booleans.
- `KeyboardLeftStickAxes` defaults both axes to `128`.
- `KeyboardLeftStickUpdate` reports `handled`, `changed`, and the resulting axes.
- `ApplyKeyboardLeftStickKey(state, key_code, down)` handles only A/D/W/S.
- `ResetKeyboardLeftStick(state)` clears state and reports neutral axes.

The compile-clean baseline implementation returns `{}`. The test must cover
press and release for A/D/W/S, simultaneous opposites resolving to `128`, and
reset to `128/128`; require the exact test to fail once before implementing.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
test_name=EmulatorPad.KeyboardMovementKeyPressReleaseMapsLeftStick
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_color=no --gtest_filter="$test_name" '{kyty_run_tests()}' 2>&1)
rg -Fqx 'EmulatorPad.' <<<"$listed_tests"
test "$(rg -Fxc '  KeyboardMovementKeyPressReleaseMapsLeftStick' <<<"$listed_tests")" -eq 1
set +e
red_output=$("$curated/_build_linux_curated/fc_script" --gtest_color=no --gtest_filter="$test_name" '{kyty_run_tests()}' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fq "[ RUN      ] $test_name" <<<"$red_output"
test "$(rg -Fxc "[  FAILED  ] $test_name" <<<"$red_output")" -eq 1
rg -Fq '[==========] 1 test from 1 test suite ran.' <<<"$red_output"
rg -Fq '[  FAILED  ] 1 test' <<<"$red_output"
```

- [ ] **Step 4: Implement and verify real keyboard movement**

Implement the pure state transition. Keep SDL event decoding in `Window.cpp`;
it passes the key code into the helper and calls `ControllerAxis` only when the
returned update says the state changed. Focus loss/reset uses the same helper.
This is a testability seam, not permission for broader Window refactoring.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter='EmulatorPad.KeyboardMovementKeyPressReleaseMapsLeftStick' '{kyty_run_tests()}')
rg -Fqx 'EmulatorPad.' <<<"$listed_tests"
test "$(rg -Fxc '  KeyboardMovementKeyPressReleaseMapsLeftStick' <<<"$listed_tests")" -eq 1
"$curated/_build_linux_curated/fc_script" --gtest_filter='EmulatorPad.KeyboardMovementKeyPressReleaseMapsLeftStick:EmulatorPad.KeyboardAxesAreReturnedByPadReadState' '{kyty_run_tests()}'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" diff --check
git -C "$curated" add source/emulator/include/Emulator/Graphics/KeyboardInput.h source/emulator/src/Graphics/KeyboardInput.cpp source/emulator/src/Graphics/Window.cpp source/unit_test/src/emulator/UnitTestEmulatorPad.cpp
git -C "$curated" diff --cached --check
git -C "$curated" commit -m 'fix(input): map keyboard movement to left stick'
```

During strict replay, verify actual key press and release, movement in both
directions, neutralization after release, and no AUTO_CROSS acceptance claim.

- [ ] **Step 5: Establish an attributable red gate for 32-bit wait predicates**

Port only `EmulatorGraphicsPackets.Encodes32BitWaitWithInactiveUpperPredicate`
from the historical research diff. There is no new helper seam: the test calls
the real `GraphicsDcbWaitRegMem` encoder with `size=0` and upper bits present in
reference/mask. Build, list the exact test, and require one attributed failure
before changing production code. If the exact test already exists and is green,
inspect the encoder path and omit the historical behavior instead of rewriting
it.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated" fc_script
test_name=EmulatorGraphicsPackets.Encodes32BitWaitWithInactiveUpperPredicate
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_color=no --gtest_filter="$test_name" '{kyty_run_tests()}' 2>&1)
rg -Fqx 'EmulatorGraphicsPackets.' <<<"$listed_tests"
test "$(rg -Fxc '  Encodes32BitWaitWithInactiveUpperPredicate' <<<"$listed_tests")" -eq 1
set +e
red_output=$("$curated/_build_linux_curated/fc_script" --gtest_color=no --gtest_filter="$test_name" '{kyty_run_tests()}' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -Fq "[ RUN      ] $test_name" <<<"$red_output"
test "$(rg -Fxc "[  FAILED  ] $test_name" <<<"$red_output")" -eq 1
rg -Fq '[==========] 1 test from 1 test suite ran.' <<<"$red_output"
rg -Fq '[  FAILED  ] 1 test' <<<"$red_output"
```

- [ ] **Step 6: Implement and verify 32-bit wait predicates when red**

Change only the encoder width contract proved by the test. Keep the normalized
packet envelope and parser contract unchanged unless the red evidence points to
their producer. Then prove exact green, the owner filter, and strict replay.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
ninja -C "$curated/_build_linux_curated"
listed_tests=$("$curated/_build_linux_curated/fc_script" --gtest_list_tests --gtest_filter='EmulatorGraphicsPackets.Encodes32BitWaitWithInactiveUpperPredicate' '{kyty_run_tests()}')
rg -Fqx 'EmulatorGraphicsPackets.' <<<"$listed_tests"
test "$(rg -Fxc '  Encodes32BitWaitWithInactiveUpperPredicate' <<<"$listed_tests")" -eq 1
"$curated/_build_linux_curated/fc_script" --gtest_filter='EmulatorGraphicsPackets.Encodes32BitWaitWithInactiveUpperPredicate' '{kyty_run_tests()}'
"$curated/_build_linux_curated/fc_script" --gtest_filter='EmulatorGraphicsPackets.*Wait*' '{kyty_run_tests()}'
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$curated" diff --check
git -C "$curated" add source/emulator/src/Graphics/Graphics.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git -C "$curated" diff --cached --check
git -C "$curated" commit -m 'fix(graphics): preserve 32-bit wait predicates'
```

Expected: every behavior is independently red, implemented, green, and replayed.
No historical commit is cherry-picked, and a pre-existing exact green test is
accepted only after inspection proves it reaches the production seam.

### Task 11: Reconcile the dirty primary patch file by file

**Files:**
- Read-only source diff: `/home/monasterios/Documents/PS5/Kyty`
- Candidate tracked files are the current dirty list reported by `git -C /home/monasterios/Documents/PS5/Kyty status --short`.
- Candidate untracked files: `docs/graphics-captures.md`, `scripts/kyty_capture.py`, and `scripts/test_kyty_capture.py`.
- Always exclude: `scripts/__pycache__/`, scratch, raw captures, generated shaders, and private runner values.

**Interfaces:**
- Consumes: dirty delta relative to the source worktree HEAD after the four documentation commits.
- Produces: reviewed net behaviors or explicit omissions; source worktree remains byte-for-byte untouched.

- [ ] **Step 1: Regenerate the overlap inventory**

```bash
set -euo pipefail
source_repo=/home/monasterios/Documents/PS5/Kyty
git -C "$source_repo" status --short
git -C "$source_repo" diff --check
git -C "$source_repo" diff --name-only
```

- [ ] **Step 2: Compare each candidate against the curated implementation**

For every tracked file, classify each hunk as already integrated, a focused semantic improvement with evidence/test, passive diagnostic behavior belonging to the accepted diagnostics spec, generated/private evidence, or unexplained. Only the second class enters a semantic commit; diagnostic code waits for the separate diagnostics plan.

- [ ] **Step 3: Reimplement accepted hunks with `apply_patch`**

Do not apply the whole dirty diff. Add or extend a focused test first, implement the smallest net behavior in the curated worktree, then run the owner filter and strict replay. Leave the source file unchanged.

- [ ] **Step 4: Audit the Python capture tool separately**

Run the following against the source worktree so cwd is explicit and no new bytecode is written. If the tool contains only generic sanitized capture logic, port it in a dedicated tooling commit after removing private/default paths. Do not port `__pycache__`, raw captures, or workload-specific selectors.

```bash
set -euo pipefail
source_repo=/home/monasterios/Documents/PS5/Kyty
(cd "$source_repo" && PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest scripts/test_kyty_capture.py)
```

### Task 12: Prove the curated frontier and history gate

**Files:**
- No new tracked evidence files.
- A sanitized frontier report is required and remains untracked until the frozen-frontier gate is complete.

**Interfaces:**
- Consumes: all curated commits and reconciled dirty behavior.
- Produces: branch eligible for push and later `main` review.

- [ ] **Step 1: Run the complete focused set and build**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
ninja -C "$curated/_build_linux_curated"
listed_tests=$(
  "$curated/_build_linux_curated/fc_script" --gtest_list_tests '{kyty_run_tests()}'
)
for required_suite in CoreVirtualMemory EmulatorGraphicsPackets EmulatorGraphicsState EmulatorKernelMemory EmulatorPad; do
  if ! rg -q "^${required_suite}\\." <<<"$listed_tests"; then exit 1; fi
done
test_filter='CoreVirtualMemory.*:EmulatorGraphicsPackets.*:EmulatorGraphicsState.*:EmulatorKernelMemory.*:EmulatorPad.*:EmulatorApplicationHeap.*:EmulatorProcParamMalloc.*:EmulatorLibcGuard.*:EmulatorLibcMspace.*:EmulatorLibcRand.*:EmulatorLibcStrtok.*:EmulatorDialog.*:EmulatorSaveData.*:EmulatorSysmodule.*'
test_output=$("$curated/_build_linux_curated/fc_script" --gtest_filter="$test_filter" '{kyty_run_tests()}')
printf '%s\n' "$test_output"
if ! rg -q '\[[[:space:]]+PASSED[[:space:]]+\][[:space:]]+[1-9][0-9]* tests?' <<<"$test_output"; then exit 1; fi
git -C "$curated" diff --check
```

If Task 9 added an HLE suite, verify that exact suite name is listed before adding it to `test_filter`; a legitimately skipped Task 9 does not create a phantom suite.

- [ ] **Step 2: Reproduce the strict/visual frontier twice**

Use identical resolution, Silent logging, shader-cache state, and real input sequence. A controlled external timeout is measurement infrastructure, not emulator recovery: exit `0` or timeout `124` after the full window is acceptable only when no earlier crash/EXIT occurred.

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
run_bounded() {
  set +e
  timeout --foreground --signal=INT --kill-after=10s 180s "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
}
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
run_bounded "$curated/_build_linux_curated/fc_script" "$curated/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
```

Both runs must reach the same or later frontier than the source branch; HUD-only or corrupted world rendering fails.

- [ ] **Step 3: Write and validate the sanitized frontier report**

Using `apply_patch`, create `/home/monasterios/Documents/PS5/Kyty-curated-scratch/frontier-report.md` outside Git after reading the actual test/run evidence. Give it these exact second-level headings: `Identity`, `Build and tests`, `Strict replay`, `Input`, `Visual result`, `Validation`, `Performance`, and `Frontier`. Record the exact commit, host/GPU capability summary, nonzero focused-test count, both run durations/results, exact resolution, `PrintfDirection=Silent`, shader-cache state, real press/release input sequence, frame/flip counts, geometry/color assessment, Vulkan validation result, performance conditions, and first bad producer or stable checkpoint. Write `not verified` for a gate that could not be run; never infer a pass. Do not include the fixture name, title/product ID, private path, raw logs, captures, or assets.

```bash
set -euo pipefail
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
report="$scratch/frontier-report.md"
test -s "$report"
for heading in 'Identity' 'Build and tests' 'Strict replay' 'Input' 'Visual result' 'Validation' 'Performance' 'Frontier'; do
  rg -q "^## ${heading}$" "$report"
done
test -n "${KYTY_GUEST_ROOT:-}"
private_fixture_name=$(basename "$KYTY_GUEST_ROOT")
set +e
rg -F "$private_fixture_name" "$report"
private_status=$?
rg -n '/home/|PPSA[0-9]|CUSA[0-9]|VALUE_REQUIRED' "$report"
path_status=$?
set -e
test "$private_status" -eq 1
test "$path_status" -eq 1
```

- [ ] **Step 4: Prove bad history is absent**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
source_repo=/home/monasterios/Documents/PS5/Kyty
compositor_repo=/home/monasterios/Documents/PS5/Kyty-gen5-compositor
scratch=/home/monasterios/Documents/PS5/Kyty-curated-scratch
test -n "${KYTY_GUEST_ROOT:-}"
private_fixture_name=$(basename "$KYTY_GUEST_ROOT")
git -C "$curated" log --format=fuller --stat main..HEAD
git -C "$curated" log -p --format='%H%n%B' main..HEAD
commit_messages=$(git -C "$curated" log main..HEAD --format='%B')
history_patch=$(git -C "$curated" log -p --format='%H%n%B' main..HEAD)
set +e
rg -ni '^Co-authored-by:' <<<"$commit_messages"
coauthor_status=$?
rg -F "$private_fixture_name" <<<"$history_patch"
private_status=$?
set -e
test "$coauthor_status" -eq 1
test "$private_status" -eq 1
for omitted in 1149171 5fda290 2ced411 2f9305f ad1c150 c9615dd c98ef19 e4815d2 88fd991 155f48c c44e6c0 dbce682 15e1552 4f185a9; do
  set +e
  git -C "$curated" merge-base --is-ancestor "$omitted" HEAD
  ancestor_status=$?
  set -e
  test "$ancestor_status" -eq 1
done
git -C "$curated" range-diff main..codex/gen5-render-frontier main..HEAD
git -C "$curated" range-diff main..codex/gen5-compositor-fix main..HEAD
git -C "$source_repo" rev-parse HEAD >"$scratch/final-source-head"
git -C "$source_repo" status --porcelain=v2 -z >"$scratch/final-source-status"
git -C "$source_repo" stash list >"$scratch/final-source-stashes"
git -C "$compositor_repo" rev-parse HEAD >"$scratch/final-compositor-head"
git -C "$compositor_repo" status --porcelain=v2 -z >"$scratch/final-compositor-status"
git -C "$compositor_repo" stash list >"$scratch/final-compositor-stashes"
cmp "$scratch/baseline/source-head" "$scratch/final-source-head"
cmp "$scratch/baseline/source-status" "$scratch/final-source-status"
cmp "$scratch/baseline/source-stashes" "$scratch/final-source-stashes"
cmp "$scratch/baseline/compositor-head" "$scratch/final-compositor-head"
cmp "$scratch/baseline/compositor-status" "$scratch/final-compositor-status"
cmp "$scratch/baseline/compositor-stashes" "$scratch/final-compositor-stashes"
curated_status=$(git -C "$curated" status --short)
test -z "$curated_status"
test -s "$scratch/frontier-report.md"
```

Expected: no trailer matches, no omitted commit is an ancestor, and the range-diff explains every kept/reworked/dropped behavior.

- [ ] **Step 5: Push only the curated branch**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
git -C "$curated" push -u origin codex/gen5-curated-integration
```

Do not merge to `main` until supported-host builds, strict/visual replay, input, validation, history review, and frozen-frontier evidence all pass.
