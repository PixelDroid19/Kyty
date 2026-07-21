# GPU Dirty Page Tracking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce CPU upload overhead by skipping XXH64 scans for CPU-visible GPU resources whose tracked pages have not been written, while retaining the existing hash path for every unsupported or uncertain range.

**Architecture:** A fixed-capacity `GpuDirtyPageTracker` owns page generations and a signal-safe fault path. `GpuMemory::ObjectInfo` stores per-block generation snapshots, so aliases/resources never consume one another's dirtiness. The first rollout is Linux/POSIX plus explicit host notifications; failures and unsupported platforms remain `HashFallback`.

**Tech Stack:** C++17, Kyty `Core::VirtualMemory`, POSIX `mprotect`/Windows `VirtualProtect` seam, XXH64 fallback, fc_script GoogleTest.

## Global Constraints

- Same worktree: `/home/monasterios/Documents/PS5/Kyty`; no additional worktrees.
- The exception handler must not lock, allocate, log, hash, call Vulkan, or use containers.
- Use `Core::VirtualMemory::GetPageSize()`; never assume 4 KiB.
- Preserve existing XXH64 behavior on registration/protection/capacity/upload failure.
- Darwin ARM64 write-fault classification remains fallback in this phase.
- Do not copy third-party emulator code; use only clean-room architectural concepts.

### Task 1: Fixed-capacity dirty-page tracker

**Files:**
- Create: `source/emulator/include/Emulator/Graphics/GpuDirtyPageTracker.h`
- Create: `source/emulator/src/Graphics/GpuDirtyPageTracker.cpp`
- Create: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsDirtyTracking.cpp`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- `bool RegisterRange(uint64_t address, uint64_t size)`
- `void UnregisterRange(uint64_t address, uint64_t size) noexcept`
- `bool PrepareForRead(uint64_t address, uint64_t size) noexcept`
- `bool HandleWriteFault(uint64_t fault_address) noexcept`
- `bool NotifyWrite(uint64_t address, uint64_t size) noexcept`
- `uint64_t SnapshotGeneration(uint64_t address, uint64_t size) const noexcept`
- `bool ChangedSince(uint64_t address, uint64_t size, uint64_t snapshot) const noexcept`
- `bool Enabled() noexcept`

- [ ] **Step 1: Write failing tracker tests** for host page size, fault/rearm, cross-page ranges, explicit notification, unregister, and concurrent generation increments.
- [ ] **Step 2: Run the focused suite and confirm it fails because the tracker API is absent.**
- [ ] **Step 3: Implement a fixed open-addressed table.** Store page key, atomic generation, atomic armed state, refcount, and original protection. Normal registration uses the virtual-memory mutex; `HandleWriteFault` performs only bounded atomic lookup, generation increment, state transition, and `ProtectWriteSignalSafe`.
- [ ] **Step 4: Implement rearm and snapshots.** `PrepareForRead` calls normal `Protect(..., Mode::Read)` only for pages marked writable, then sets armed. `NotifyWrite` increments generation and write-enables tracked destination pages; source-only/untracked ranges are no-ops.
- [ ] **Step 5: Run the focused suite and the existing `CoreVirtualMemory.*` protection tests.**
- [ ] **Step 6: Commit:** `perf(graphics): add signal-safe dirty page tracker`.

### Task 2: Integrate generations into GpuMemory

**Files:**
- Modify: `source/emulator/src/Graphics/Objects/GpuMemory.cpp`

**Interfaces:** consumes the tracker API from Task 1; produces `GpuMemoryCheckAccessViolation` as the exception-handler entry point.

- [ ] **Step 1: Add a failing characterization test** asserting that a tracked object skips the second hash scan when no page generation changed, but uploads after a write notification.
- [ ] **Step 2: Run the test and confirm it fails because `Update` always calls XXH64.**
- [ ] **Step 3: Add `dirty_generation[VADDR_BLOCKS_MAX]` and `dirty_registered` to `ObjectInfo`.** Register only `info.check_hash` ranges; if any registration fails, leave the object on XXH64.
- [ ] **Step 4: Change `Update` to call `PrepareForRead`, compare generations, and hash only changed/fallback blocks.** Set a snapshot only after a successful `update_func`; if upload fails or a generation changes during upload, keep the previous snapshot.
- [ ] **Step 5: Register on object creation and unregister before private `Free` recycles the object slot.** Keep GPU-owned `check_hash=false` objects out of the tracker.
- [ ] **Step 6: Implement `GpuMemoryCheckAccessViolation` as the lock-free tracker fault/notify seam and return `GpuMemoryWatcherEnabled` only when the backend is active.**
- [ ] **Step 7: Run `EmulatorGraphicsDirtyTracking.*`, `EmulatorGraphicsState.*`, and `EmulatorGraphicsPackets.*`.**
- [ ] **Step 8: Commit:** `perf(graphics): skip clean CPU uploads with page generations`.

### Task 3: Host write notifications

**Files:**
- Modify: `source/emulator/src/Graphics/GraphicsRun.cpp`
- Modify: `source/emulator/src/Libs/LibC.cpp`
- Modify: `source/emulator/src/Kernel/FileSystem.cpp`
- Modify: `source/emulator/src/Graphics/Objects/Label.cpp`

- [ ] **Step 1: Add tests that distinguish destination and source notifications.**
- [ ] **Step 2: Run them red.**
- [ ] **Step 3: Notify immediately before CP `WriteData`, const-RAM dumps, custom DMA destinations, libc memcpy/memmove/memset destinations, file reads, label writes, and GPU writeback. Do not notify DMA sources.
- [ ] **Step 4: Ensure notification never takes `GpuMemory::m_mutex`; writeback paths must use the fixed tracker only.**
- [ ] **Step 5: Run the focused suites and a non-faulted kernel-memory integration test.**
- [ ] **Step 6: Commit:** `perf(graphics): notify host writes for dirty tracking`.

### Task 4: Observation and fallback diagnostics

**Files:**
- Modify: `source/emulator/src/Graphics/GpuDirtyPageTracker.cpp`
- Modify: `source/emulator/src/Graphics/Objects/GpuMemory.cpp`
- Modify: `docs/kyty-runtime-graphics-investigation-handoff.md`

- [ ] **Step 1: Add counters for faults, explicit notifications, re-arms, skipped hashes, fallback ranges, protection failures, and observation mismatches.**
- [ ] **Step 2: Run observation mode with XXH64 retained and assert watcher/hash decisions match.**
- [ ] **Step 3: Enable hash omission only for resources with zero observation mismatches; force fallback on any mismatch.
- [ ] **Step 4: Document controls, counters, and failure reasons in the handoff.**
- [ ] **Step 5: Commit:** `perf(graphics): add dirty tracking diagnostics`.

### Task 5: Dead Cells measurement and rollout

**Files:**
- Modify: `docs/kyty-runtime-graphics-investigation-handoff.md`
- Modify: `docs/superpowers/specs/2026-07-18-gpu-dirty-page-tracking-design.md`

- [ ] **Step 1: Build Release + Silent and run the focused tests.**
- [ ] **Step 2: Run the Dead Cells workload with the same resolution/cache/input as the baseline: 30 s warm-up plus three 180 s runs.**
- [ ] **Step 3: Compare hash bytes/time, skipped scans, upload time, median FPS, median frame time, p99 frame time, stalls, and visual correctness.
- [ ] **Step 4: Keep the feature enabled by default only if there are zero false-clean uploads/crashes and median frame time improves without p99 worsening by more than 10 percent.**
- [ ] **Step 5: Push the commits on `codex/graphics-runtime-fixes` and document the exact result and fallback path.**
