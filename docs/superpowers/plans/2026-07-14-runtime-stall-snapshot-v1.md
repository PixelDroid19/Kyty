# Runtime Stall Snapshot v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a passive native Kyty supervisor that identifies which observable execution domain stopped, preserves the last coherent state across a worker hang or crash, and writes a deterministic privacy-safe diagnostic bundle.

**Architecture:** A dependency-free `kyty_devtools_core` library owns fixed telemetry records, SPSC writer rings, keyed progress endpoints, pure classification, and the explicit wire protocol. The emulator is only a provider adapter and publishes through an inherited shared mapping; the separate `kyty_devtools` parent process owns launch, sampling, classification, durable bundle output, and child exit status. Version 1 has no control queue, attach mode, arbitrary stack suspension, stall-triggered or production automatic termination, overlay, shader reload, or general C++ hot reload; `measure` is a separate, explicit duration-bounded workflow.

**Tech Stack:** C++17 without exceptions, CMake/Ninja, GoogleTest through `fc_script`, POSIX `shm_open`/`mmap`/`posix_spawn`, Windows `CreateFileMappingW`/`CreateProcessW`, platform lock-free 64-bit atomics, CRC-64/ECMA-182, structured JSON written by the supervisor.

## Global Constraints

- Execute only after `docs/superpowers/plans/2026-07-14-curated-gen5-integration.md` reaches its history and strict/visual gates.
- Freeze the exact accepted curated commit before execution. `/home/monasterios/Documents/PS5/Kyty-gen5-curated` is read-only evidence while this plan runs; all diagnostic implementation occurs in `/home/monasterios/Documents/PS5/Kyty-devtools`.
- Treat `docs/superpowers/specs/2026-07-14-native-runtime-diagnostics-design.md` as the accepted protocol authority; changing its ABI or behavior requires a reviewed spec amendment first.
- Hot-path recording performs no allocation, formatting, file I/O, emulator-owned locking, scheduler pumping, recovery, signaling, or waiting.
- Automatic artifacts are allowlist-only and contain no arbitrary argv/environment/log text, private paths, workload identifiers, guest-provided names, stable shader/pipeline hashes, binaries, textures, screenshots, or full guest memory.
- `kyty_devtools_core` links no `core`, `emulator`, SDL, Vulkan, or third-party target.
- `kyty_devtools` and `kyty_devtools_supervisor` never link the `emulator` static library.
- Every new source is enumerated explicitly with `target_sources`; do not broaden legacy recursive globs.
- Linux/macOS builds use `-fno-exceptions`; filesystem operations use `std::error_code` or explicit platform return values.
- Shared wire data uses fixed little-endian offsets and platform atomics, never raw C++ struct or `std::atomic` ABI.
- Startup filters are immutable for version 1. There is no `capture-now`, attach,
  or supervisor-to-worker queue; notification acknowledgement is parent-local.
- A loss/capacity/rejected-sample gap overlapping decisive evidence prevents high-confidence classification.
- The supervisor captures on stall but never terminates the worker automatically.
- General code changes use rebuild plus controlled restart/replay; overlay and live shader generation require separate specifications.
- One contract per short commit; no `Co-authored-by` trailer.
- Before every emulator-provider commit, run the complete host build, confirm the task's exact test suite appears in `--gtest_list_tests`, run that suite, run a lightweight enabled/disabled benchmark, run the same bounded strict scenario directly and through the supervisor, and run `git diff --check`. A zero-test match or earlier strict frontier blocks the commit.
- Execute binaries from `/home/monasterios/Documents/PS5/Kyty-devtools`, route emulator artifacts to `/home/monasterios/Documents/PS5/Kyty-devtools-scratch`, and require `--output-dir` for every supervisor bundle. The primary and curated source worktrees remain byte-for-byte untouched.

---

### Task 0: Fork a dedicated DevTools worktree from the frozen curated frontier

**Files:**
- Read-only source: `/home/monasterios/Documents/PS5/Kyty-gen5-curated`
- Create through worktree skill: `/home/monasterios/Documents/PS5/Kyty-devtools/`

**Interfaces:**
- Consumes: one clean, fully gated `codex/gen5-curated-integration` commit recorded in the required untracked frontier report.
- Produces: branch `codex/runtime-stall-snapshot-v1`, exact base commit recorded outside Git, and exclusive build directory `_build_linux_devtools`.

- [ ] **Step 1: Prove and freeze the source commit**

```bash
set -euo pipefail
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
cd "$curated"
primary=/home/monasterios/Documents/PS5/Kyty
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/baseline"
test "$(git -C "$curated" branch --show-current)" = codex/gen5-curated-integration
curated_status=$(git -C "$curated" status --short)
test -z "$curated_status"
curated_tip=$(git -C "$curated" rev-parse HEAD)
main_tip=$(git -C "$curated" rev-parse main)
test "$main_tip" != "$curated_tip"
printf '%s\n' "$curated_tip"
git -C "$curated" rev-parse HEAD > "$scratch/baseline/curated-head"
git -C "$curated" status --porcelain=v2 -z > "$scratch/baseline/curated-status"
git -C "$curated" stash list > "$scratch/baseline/curated-stashes"
git -C "$primary" rev-parse HEAD > "$scratch/baseline/primary-head"
git -C "$primary" status --porcelain=v2 -z > "$scratch/baseline/primary-status"
git -C "$primary" stash list > "$scratch/baseline/primary-stashes"
```

Expected: a clean accepted tip. Record `curated_tip` in the untracked execution report; any later source movement requires re-running the curated final gate and restarting this plan from a newly frozen commit.

- [ ] **Step 2: Create the isolated worktree through the required skill**

Invoke `superpowers:using-git-worktrees` with the exact recorded `curated_tip`, branch `codex/runtime-stall-snapshot-v1`, and destination `/home/monasterios/Documents/PS5/Kyty-devtools`. Do not reuse the curated worktree or its build directory.

- [ ] **Step 3: Prove the DevTools baseline**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
test "$(git -C "$devtools" branch --show-current)" = codex/runtime-stall-snapshot-v1
devtools_status=$(git -C "$devtools" status --short)
test -z "$devtools_status"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
test_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*:EmulatorKernelMemory.*:EmulatorPad.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
```

Expected: a clean, green build at exactly the frozen curated commit. No DevTools commit exists yet.

---

### Task 1: Establish the DevTools targets and fixed event schema

**Files:**
- Modify: `source/lib/CMakeLists.txt`
- Create: `source/lib/DevTools/CMakeLists.txt`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Telemetry/Event.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Time/MonotonicClock.h`
- Create: `source/lib/DevTools/src/Telemetry/Event.cpp`
- Create: `source/lib/DevTools/src/Time/MonotonicClockPosix.cpp`
- Create: `source/lib/DevTools/src/Time/MonotonicClockWindows.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsEventRing.cpp`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: C++17 and existing `UT_BEGIN`/`UT_LINK` registration.
- Produces: target `kyty_devtools_core`, the exact v1 domain/event/operation/
  role/result/wait dictionaries, `WriterKey`, schema validation, and 72-byte
  `EventRecord`, plus the shared `MonotonicNowNs` platform adapter.

- [ ] **Step 1: Add the schema test and test registration**

Use this contract in `UnitTestDevToolsEventRing.cpp`:

```cpp
UT_BEGIN(DevToolsEventRing);

TEST(DevToolsEventRing, EventRecordHasWireCompatibleFields)
{
    EXPECT_EQ(sizeof(DevTools::EventRecord), 72u);
    EXPECT_TRUE(std::is_trivially_copyable_v<DevTools::EventRecord>);
    EXPECT_EQ(DevTools::WriterKey {7, 11}.Pack(), (uint64_t {11} << 32u) | 7u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::Domain::Unknown), 0u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::Domain::GuestThread), 1u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::Domain::Synchronization), 8u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::Domain::Count), 9u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::EventId::Unknown), 0u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::EventId::ThreadStart), 1u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::EventId::WaitEnd), 7u);
    EXPECT_EQ(static_cast<uint16_t>(DevTools::EventId::Count), 8u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::TimelineFlag::TimedWait), 0x40u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::ThreadRole::SnapshotPublisher), 11u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::ResultCategory::Unsupported), 6u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::WaitOutcome::Error), 4u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::HleCallKind::WaitEventFlag), 1u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::HleCallKind::TriggerEventQueue), 6u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::OperationCode::CommandPacket), 0x0302u);
    EXPECT_EQ(static_cast<uint32_t>(DevTools::OperationCode::RegisterMemoryWait), 0x0804u);
    EXPECT_EQ((static_cast<uint32_t>(DevTools::OperationCode::CommandPacket) >> 8u) & 0xffu,
              static_cast<uint16_t>(DevTools::Domain::CommandProcessor));
    EXPECT_EQ(static_cast<uint32_t>(DevTools::OperationCode::RegisterMemoryWait) & 0xffff0000u, 0u);
}

TEST(DevToolsEventRing, EventSchemaRejectsInvalidFlagAndDomainCombinations)
{
    DevTools::EventRecord valid {};
    valid.domain      = static_cast<uint16_t>(DevTools::Domain::Hle);
    valid.event       = static_cast<uint16_t>(DevTools::EventId::OperationSubmit);
    valid.flags       = static_cast<uint32_t>(DevTools::TimelineFlag::CorrelationValid) |
                        static_cast<uint32_t>(DevTools::TimelineFlag::Payload0Valid) |
                        static_cast<uint32_t>(DevTools::TimelineFlag::Payload1Valid) |
                        static_cast<uint32_t>(DevTools::TimelineFlag::Payload3Valid);
    valid.correlation = (uint64_t {1} << 56u) | 7u;
    valid.payload[0]  = static_cast<uint32_t>(DevTools::OperationCode::HleCall);
    valid.payload[1]  = 11;
    valid.payload[3]  = static_cast<uint32_t>(DevTools::HleCallKind::WaitEventFlag) |
                        (uint64_t {1} << 32u);
    EXPECT_TRUE(DevTools::ValidateEventSchema(valid));
    auto wrong_domain = valid;
    wrong_domain.payload[0] = static_cast<uint32_t>(DevTools::OperationCode::CommandPacket);
    EXPECT_FALSE(DevTools::ValidateEventSchema(wrong_domain));
    auto reserved_flag = valid;
    reserved_flag.flags |= 0x80u;
    EXPECT_FALSE(DevTools::ValidateEventSchema(reserved_flag));
}

UT_END();
```

Add `UT_LINK(DevToolsEventRing);` to `source/unit_test/src/UnitTest.cpp` and explicitly add the test source in `source/unit_test/CMakeLists.txt`. Add `MonotonicConversionIsChecked` with injected POSIX timespec and Windows counter/frequency conversion values, including remainder and overflow cases; production code selects `CLOCK_MONOTONIC` or QPC in CMake and never uses `steady_clock` or wall time.

Add `ResultErrorFlagsAreEventSpecific`: ThreadExit/OperationComplete require the
bit exactly for GuestError, HostError, Timeout, and Unsupported; WaitEnd requires
it exactly for TimedOut and Error; every other result/outcome and event rejects
an incorrectly set or cleared bit.

- [ ] **Step 2: Run the test and verify red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/event-schema.log"
set +e
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1 && \
  ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q 'fatal error: .*Kyty/DevTools/Telemetry/Event\.h.*(No such file|file not found)' "$red_log"
```

Expected: compile failure because the DevTools target/types do not exist.

- [ ] **Step 3: Create the target and exact event types**

`Event.h` defines fixed-width fields in this order:

```cpp
#include <cstdint>
#include <type_traits>

namespace Kyty::DevTools {

enum class Domain: uint16_t {
    Unknown = 0, GuestThread = 1, Hle = 2, CommandProcessor = 3, Renderer = 4,
    GpuQueue = 5, VideoOut = 6, Presentation = 7, Synchronization = 8, Count = 9
};
enum class EventId: uint16_t {
    Unknown = 0, ThreadStart = 1, ThreadExit = 2, OperationSubmit = 3,
    OperationProgress = 4, OperationComplete = 5, WaitBegin = 6, WaitEnd = 7, Count = 8
};
enum class TimelineFlag: uint32_t {
    CorrelationValid = 0x01, Payload0Valid = 0x02, Payload1Valid = 0x04,
    Payload2Valid = 0x08, Payload3Valid = 0x10, ResultError = 0x20, TimedWait = 0x40
};
enum class ThreadRole: uint32_t {
    Unknown = 0, MainGuest = 1, GuestPthread = 2, GraphicsBatch = 3,
    GraphicsDraw = 4, GraphicsConstant = 5, GraphicsCompute = 6, GraphicsLabel = 7,
    GraphicsRender = 8, VideoOut = 9, Presentation = 10, SnapshotPublisher = 11, Count = 12
};
enum class ResultCategory: uint32_t {
    Unknown = 0, Success = 1, GuestError = 2, HostError = 3,
    Timeout = 4, Cancelled = 5, Unsupported = 6, Count = 7
};
enum class WaitOutcome: uint32_t {
    Unknown = 0, Satisfied = 1, TimedOut = 2, Cancelled = 3, Error = 4, Count = 5
};
enum class HleCallKind: uint32_t {
    Unknown = 0, WaitEventFlag = 1, SetEventFlag = 2, WaitSemaphore = 3,
    SignalSemaphore = 4, WaitEventQueue = 5, TriggerEventQueue = 6, Count = 7
};
enum class OperationCode: uint32_t {
    Unknown = 0, ExecutionBoundary = 0x0101, HleCall = 0x0201,
    CommandSubmit = 0x0301, CommandPacket = 0x0302, RenderFrame = 0x0401,
    RenderDraw = 0x0402, RenderDispatch = 0x0403, RenderCommandBuffer = 0x0404,
    QueueSubmit = 0x0501, FenceWait = 0x0502, FencePoll = 0x0503,
    LabelPoll = 0x0504, VideoOutFlip = 0x0601, PresentationAcquire = 0x0701,
    PresentationPresent = 0x0702, SwapchainRecreate = 0x0703,
    PresentationDeviceIdle = 0x0704, EventFlagWait = 0x0801,
    SemaphoreWait = 0x0802, EventQueueWait = 0x0803, RegisterMemoryWait = 0x0804
};

struct WriterKey {
    uint32_t slot       = 0;
    uint32_t generation = 0;
    [[nodiscard]] constexpr uint64_t Pack() const noexcept { return (uint64_t {generation} << 32u) | slot; }
};

struct EventRecord {
    uint64_t sequence     = 0;
    uint64_t monotonic_ns = 0;
    uint64_t writer_key   = 0;
    uint16_t domain       = 0;
    uint16_t event        = 0;
    uint32_t flags        = 0;
    uint64_t correlation  = 0;
    uint64_t payload[4]   = {};
};

static_assert(sizeof(EventRecord) == 72);
static_assert(std::is_trivially_copyable_v<EventRecord>);
[[nodiscard]] bool ValidateEventSchema(const EventRecord& record) noexcept;

} // namespace Kyty::DevTools
```

`source/lib/DevTools/CMakeLists.txt` creates static `kyty_devtools_core` with explicitly listed `src/Telemetry/Event.cpp`, which implements the closed flag/payload/domain validation above, and publishes only its `include` directory. It has no linked target. Add `target_link_libraries(unit_test kyty_devtools_core)` using the repository's existing non-keyword signature so focused tests resolve the schema without linking the emulator.

- [ ] **Step 4: Build and run the focused test**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsEventRing\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  EventRecordHasWireCompatibleFields$' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsEventRing.*'
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
git -C "$devtools" add source/lib/CMakeLists.txt source/lib/DevTools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsEventRing.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): define telemetry event schema'
```

### Task 2: Implement the bounded SPSC writer registry

**Files:**
- Create: `source/lib/DevTools/include/Kyty/DevTools/Telemetry/EventRing.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Telemetry/WriterRegistry.h`
- Create: `source/lib/DevTools/src/Telemetry/EventRing.cpp`
- Create: `source/lib/DevTools/src/Telemetry/WriterRegistry.cpp`
- Modify: `source/lib/DevTools/CMakeLists.txt`
- Modify: `source/unit_test/src/devtools/UnitTestDevToolsEventRing.cpp`

**Interfaces:**
- Consumes: `EventRecord`, 512 writer slots, 256 events per writer.
- Produces:

```cpp
enum class WriterState: uint32_t { Free, Reserved, Active, Closing };
struct TelemetryWriterToken { uint32_t slot; uint32_t generation; };
struct WriterLossCounter {
    uint64_t total;
    uint64_t last_attempted_sequence;
    uint64_t last_loss_monotonic_ns;
};
struct GlobalLossCounter {
    uint64_t total;
    uint64_t last_loss_monotonic_ns;
};
struct WriterLossSnapshot {
    WriterLossCounter writer[512];
    GlobalLossCounter aggregate_ring;
    GlobalLossCounter registration_capacity;
    GlobalLossCounter inactive_writer_attempts;
    uint64_t max_loss_monotonic_ns;
};
struct WriterInventoryEntry {
    uint64_t writer_key;
    uint64_t diagnostic_thread_instance;
    ThreadRole role;
    WriterState state;
};
struct WriterInventorySnapshot {
    WriterInventoryEntry entries[512];
    uint64_t inventory_generation;
};
class TimelineHistory {
public:
    void AppendDrained(const EventRecord* records, uint32_t count) noexcept;
    uint32_t SnapshotNewest(EventRecord* out, uint32_t capacity) const noexcept;
};
class WriterRegistry {
public:
    bool Reserve(ThreadRole role, uint64_t diagnostic_thread_instance,
                 TelemetryWriterToken* out) noexcept;
    bool Activate(TelemetryWriterToken token) noexcept;
    void Abandon(TelemetryWriterToken token) noexcept;
    bool TryRecord(TelemetryWriterToken token, EventRecord record) noexcept;
    void Close(TelemetryWriterToken token, EventRecord exit_record) noexcept;
    uint32_t Drain(EventRecord* out, uint32_t capacity) noexcept;
    WriterLossSnapshot SnapshotLoss() const noexcept;
    bool SnapshotInventory(WriterInventorySnapshot* out) const noexcept;
};
```

- [ ] **Step 1: Add red tests for full/drop and lifecycle handoff**

`SnapshotInventoryIsIndependentOfTimelineRetention` proves active writer key,
diagnostic instance, role, and lifecycle remain available after lifecycle
events age out. A bounded whole-inventory retry returns generation zero rather
than a mixed snapshot.

Add exact tests headed by `ReservesActivatesAndDrainsWriter`. Creator reserves before native thread creation; only the future producer activates; failed create abandons `Reserved -> Free`. Exactly 512 writer reservations succeed and the 513th updates `registration_capacity`. `AllConfiguredWritersFitWithoutRegistrationLoss` concurrently reserves 256 guest-thread, 67 CP-worker, LabelManager, Window, and publisher owners. Exactly 256 event pushes succeed and the 257th updates that slot plus process-lifetime aggregate loss without overwrite. Cover lossy close/reuse without decreasing slot/aggregate sequence or loss, invalid/reserved/closing/stale-token attempts as `inactive_writer_attempts`, invalid domain/event values through attempted-sequence loss, ordered drain, producer reuse after consumer release, abrupt owner loss, and unsigned ring wrap. `WriterGenerationWrapRetiresSlot` permanently retires a slot before 32-bit generation wrap while retaining cumulative loss. `TryRecordOverwritesCallerSequenceAndWriterKey` requires registry-assigned values. `WriterLossSurvivesSlotReuseAndRetainedHistory` proves an older retained event keeps its loss provenance. Single-producer slot counters expose sequence; global counters expose only total/time. Add `TimelineHistoryRetainsAcrossEmptyPublications`, `TimelineHistoryWrapKeepsNewest4096`, and `DrainMergesWritersDeterministically`, including fair progress and ordering by monotonic time/writer key/sequence. `max_loss_monotonic_ns` is an atomic maximum. Progress capacity and protocol read rejection remain Tasks 3 and 5 owners. Compile-only stubs reject operations so the named red test executes and fails.

- [ ] **Step 2: Run and confirm red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/event-ring.log"
ninja -C "$devtools/_build_linux_devtools" fc_script >"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsEventRing\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ReservesActivatesAndDrainsWriter$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsEventRing.ReservesActivatesAndDrainsWriter' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsEventRing\.ReservesActivatesAndDrainsWriter' "$red_log"
```

- [ ] **Step 3: Implement the exact memory-order contract**

Creator: `Reserve` claims only `Free`, increments generation, initializes role and ring indices but preserves the slot's process-lifetime attempted-event sequence and loss record, then release-publishes `Reserved`; capacity failure updates the registration counter and atomic maximum loss time. A failed create may `Abandon` only that matching reserved generation. Producer: `Activate` release-publishes `Active` before its first event; every `TryRecord` ignores the caller's sequence and writer-key fields, assigns the next slot-owned attempted-event sequence, and assigns `WriterKey {token.slot, token.generation}`. It first validates the closed domain/event ranges. An invalid active-writer record advances that attempted sequence and both slot/aggregate loss but is never stored. For a valid record it acquire-loads `read_sequence`; if distance equals capacity, it advances only the attempted-event sequence and updates the owning slot's total/attempted-sequence/time, process-lifetime aggregate ring total/time, and atomic maximum time; otherwise it writes that assigned sequence/key into the exclusively owned plain slot and release-stores `write_sequence+1`. Thus event sequences expose drops while ring indices count only published storage. Invalid or non-active token attempts update only `inactive_writer_attempts`. Consumer: acquire-load write sequence, fairly multiway-drain published heads in deterministic `(monotonic_ns, writer_key, sequence)` order, then release-store each read sequence. `TimelineHistory` appends all drained records into its publisher-owned fixed circular window and never clears on an empty cycle. Close assigns the exit event through the same attempted-sequence/key path, release-stores `Closing`, and drain acquire-loads it and alone publishes `Free` after the final published record. Neither `Abandon` nor drain may recycle an `Active` slot. Per-slot attempted sequence and writer loss never reset; `aggregate_ring` is likewise never reset. `SnapshotLoss` preserves slot provenance plus the process aggregate; the supervisor later composes it with progress/protocol loss before classification.

- [ ] **Step 4: Run tests and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsEventRing\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsEventRing.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/lib/DevTools source/unit_test/src/devtools/UnitTestDevToolsEventRing.cpp
git -C "$devtools" commit -m 'feat(devtools): record bounded thread events'
```

### Task 3: Implement fixed keyed progress endpoints

**Files:**
- Create: `source/lib/DevTools/include/Kyty/DevTools/Telemetry/Progress.h`
- Create: `source/lib/DevTools/src/Telemetry/Progress.cpp`
- Modify: `source/lib/DevTools/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsProgress.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: domain keys and lifecycle rules from Tasks 1–2.
- Produces:

```cpp
enum class ProgressState: uint16_t {
    Unknown = 0, Idle = 1, Active = 2, Waiting = 3, Closed = 4, Unavailable = 5, Count = 6
};
enum class ProgressFlag: uint16_t {
    OperationValid = 0x01, CorrelationValid = 0x02, Auxiliary0Valid = 0x04,
    Auxiliary1Valid = 0x08, TimedWait = 0x10
};
struct ProgressKey {
    Domain domain;
    uint64_t instance;
    [[nodiscard]] bool TryPack(uint64_t* out) const noexcept;
    [[nodiscard]] static bool TryUnpack(uint64_t wire, ProgressKey* out) noexcept;
};
struct ProgressToken { uint16_t domain; uint16_t slot; uint32_t generation; };
struct ProgressRecord { uint64_t instance_key, last_change_ns, submitted, completed; uint32_t operation; uint16_t state, flags; uint64_t correlation, auxiliary[2]; };
struct ProgressUpdate {
    uint64_t epoch;
    OperationCode operation;
    ProgressState state;
    uint16_t flags;
    uint64_t correlation;
    uint64_t auxiliary[2];
    uint64_t monotonic_ns;
};
inline constexpr uint32_t MaxProgressSnapshotEntries = 1504;
struct ProgressSnapshotEntry { ProgressKey key; ProgressRecord record; };
struct ProgressSnapshot {
    ProgressSnapshotEntry entries[MaxProgressSnapshotEntries];
    uint32_t count;
    uint32_t unavailable_count;
    uint64_t inventory_generation;
};
struct ProgressLossSnapshot {
    GlobalLossCounter capacity[static_cast<uint32_t>(Domain::Count)];
    GlobalLossCounter rejected_update[static_cast<uint32_t>(Domain::Count)];
    uint64_t max_loss_monotonic_ns;
};
struct WaitEdge { uint64_t wait_ref; uint64_t waiter_ref; uint64_t producer_ref; };
struct WaitGraphSnapshot {
    WaitEdge edges[512];
    uint32_t count;
    uint32_t unknown_producer_count;
    uint32_t rejected_reference_count;
    uint32_t reserved;
};
[[nodiscard]] bool BuildWaitGraph(const ProgressSnapshot&, WaitGraphSnapshot*) noexcept;
class ProgressRegistry {
public:
    bool Register(ProgressKey key, ProgressToken* out) noexcept;
    bool Submit(ProgressToken token, const ProgressUpdate& update) noexcept;
    bool Advance(ProgressToken token, const ProgressUpdate& update) noexcept;
    bool Complete(ProgressToken token, uint64_t epoch, uint64_t now_ns) noexcept;
    void Close(ProgressToken token) noexcept;
    bool Snapshot(ProgressToken token, ProgressRecord* out) const noexcept;
    bool SnapshotAll(ProgressSnapshot* out) const noexcept;
    ProgressLossSnapshot SnapshotLoss() const noexcept;
};
struct MeasurementSnapshot {
    uint32_t mode;
    uint32_t flags;
    uint64_t frame_count;
    uint64_t completed_flip_count;
    uint64_t first_presentation_ns;
    uint64_t last_presentation_ns;
    uint64_t overflow_count;
    uint64_t frame_interval_ms[1024];
};
class MeasurementRegistry {
public:
    void Initialize(uint32_t mode) noexcept;
    void RecordPresentation(bool flip_completed, uint64_t monotonic_ns) noexcept;
    bool Snapshot(MeasurementSnapshot* out) const noexcept;
};
enum class GpuFaultState: uint32_t {
    CapabilityPending = 0,
    NotObserved = 1,
    ExtensionUnavailable = 2,
    CountsSucceeded = 3,
    CountsFailed = 4,
};
inline constexpr int32_t GpuFaultResultSuccess = 0;
inline constexpr int32_t GpuFaultResultDeviceLost = -4;
struct GpuFaultSnapshot {
    GpuFaultState state;
    uint32_t flags;
    int32_t device_lost_result;
    int32_t query_result;
    uint64_t capture_monotonic_ns;
    uint64_t gpu_submission_id;
    uint32_t address_info_count;
    uint32_t vendor_info_count;
    uint64_t vendor_binary_size;
};
[[nodiscard]] bool ValidateGpuFaultSnapshot(const GpuFaultSnapshot&) noexcept;
class GpuFaultRegistry {
public:
    bool InitializeCapability(bool available) noexcept;
    bool TryBeginCapture() noexcept;
    bool Complete(const GpuFaultSnapshot& final_snapshot) noexcept;
    bool Snapshot(GpuFaultSnapshot* out) const noexcept;
};
```

- [ ] **Step 1: Add tests for keyed concurrency and incomplete coverage**

Add `KeepsConcurrentInstancesDistinct` with two HLE instances and two GPU queues; advancing one cannot replace another. Exhaust every fixed table and verify only its domain capacity total/time changes. `ProgressRefRoundTripsDomainAnd56BitInstance` rejects zero, Count, and oversized instances. `ProgressWireIdentityIsCanonical` requires successful `TryPack` to equal `record.instance_key`, permits the same low instance in different domains, and rejects duplicate packed refs. `RejectsInvalidProgressSchemaWithoutMutatingEndpoint` covers wrong-domain operation, Unknown/Count/provider-Unavailable states, reserved flags, missing OperationValid, and invalid timed-wait combinations as rejected-update only. `ProgressEpochTransitionsAreExact` covers Idle 0/0; strictly newer Submit including Active supersession; same-epoch/same-operation/correlation Advance; exact Complete retaining metadata; replacement by later Submit; and regression/double-complete/overflow rejection. `ProgressSlotLifecycleRejectsStaleReuse` reuses a Closed slot only with a fresh never-issued key, rejects duplicate live keys, increments generation, retires before wrap, replaces key+record coherently, and rejects stale tokens. Provider allocator tests prove domain/instance pairs are never reused, so stale refs cannot alias; the registry does not promise historical-key storage. `SnapshotAllRejectsInventoryMutation` uses barriers and requires coherent even inventory generation or zero/unavailable after bounded retries/overflow. `WaitGraphRequiresKnownResolvedProducer` requires resolved owner/producer refs for an edge/cycle and counts unknown/rejected references separately. Per-token snapshots reject odd/changing versions after bounded retries. `SnapshotAll` preserves all keys and emits stable-ref-only Unavailable records on copy failure. Add coherent measurement and the exact one-shot GPU-fault initialization/contention/noncanonical-failure tests from the design. `SnapshotLoss` owns only domain capacity/rejected-update reasons. Compile-only stubs make `KeepsConcurrentInstancesDistinct` execute and fail before implementation.

`GpuFaultStateCombinationsAreClosed` validates the accepted state table from
the spec at registry completion and snapshot. Pending/not-observed/unavailable
require canonical zero results/time/submission/counts and exact flags;
success/failure require flag 1, `VK_ERROR_DEVICE_LOST`, nonzero capture time,
and the matching query result. Vendor-binary size is always zero. Submission ID
0 is explicitly unassociated; generated submit IDs begin at 1. Reserved flag
bits, unknown states, and every noncanonical combination are rejected.

- [ ] **Step 2: Reconfigure and prove the named tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/progress.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsProgress\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  KeepsConcurrentInstancesDistinct$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsProgress.KeepsConcurrentInstancesDistinct' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsProgress\.KeepsConcurrentInstancesDistinct' "$red_log"
```

Expected: the freshly registered suite fails for the missing progress contract. Any unrelated configure/compiler failure is fixed before implementation.

- [ ] **Step 3: Implement fixed tables and atomic local publication**

Use the accepted capacities: guest 256, HLE 512, CP 80, renderer 32, GPU 32, VideoOut 64, presentation 16, waits 512. CP 80 covers the current three graphics workers plus all 64 configured ComputeRings without making ordinary configuration incomplete, with bounded headroom; a focused test registers all 67 current owners concurrently and rejects only the 81st CP endpoint. Each endpoint has one declared writer and atomic fixed-width key/payload words guarded by an odd/even local version; publisher acquire-validates identical even versions. Registration computes the packed ref once and stores that exact value in `ProgressRecord.instance_key`; snapshots reject any key/record divergence and never reconstruct a domain from array position alone. A short internal inventory mutation guard serializes Register/Close and advances one global even inventory generation; SnapshotAll validates the same generation around the entire scan and returns generation zero on bounded failure. Free/Live/Closed state and token generation are separate from the public progress state. Closed slots are reusable, duplicate live keys fail, and generation/inventory wrap fail closed. Validate every `ProgressUpdate` and exact epoch transition against the accepted state/flag/operation-domain tables before changing the endpoint; invalid input updates only `rejected_update`. A newer Submit may supersede an Active/Waiting marker while completed remains unchanged; `Advance` is a heartbeat/state/auxiliary update for the one current epoch, never implicit completion; `Complete` retains typed identity metadata in Idle. `MeasurementRegistry` uses the same rule for mode, flags, counters, times, overflow, and all 1,024 buckets. `GpuFaultRegistry` uses one atomic capability initialization followed, only when available, by one `NotObserved -> Capturing` claim (with `Capturing` private and never serialized), one claimant-owned fixed payload, and release publication of exactly one accepted terminal state; acquire readers return the prior coherent `NotObserved` snapshot while capture is incomplete. The registry rejects noncanonical failure counts; the Vulkan boundary in Task 12 zeroes driver-written partial output before constructing the snapshot. No raw plain payload is read concurrently.

- [ ] **Step 4: Run and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsProgress\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsProgress.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/lib/DevTools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsProgress.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): track keyed runtime progress'
```

### Task 4: Implement pure virtual-time stall classification

**Files:**
- Create: `source/lib/DevTools/include/Kyty/DevTools/Diagnostics/ProcessStatus.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Diagnostics/StallClassifier.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Diagnostics/Checksum.h`
- Create: `source/lib/DevTools/src/Diagnostics/StallClassifier.cpp`
- Create: `source/lib/DevTools/src/Diagnostics/Checksum.cpp`
- Modify: `source/lib/DevTools/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsClassifier.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: immutable local progress/loss snapshots, explicit heartbeat time, portable child status, virtual sample time, and caller-owned classifier state.
- Produces:

```cpp
enum class ProcessLiveness: uint8_t { Unknown = 0, Running = 1, Terminated = 2 };
enum class ProcessTermination: uint8_t { None = 0, ExitCode = 1, Signal = 2, UnhandledException = 3, OpaquePlatformStatus = 4 };
enum class ProcessStatusError: uint8_t { None = 0, WaitFailed = 1, QueryFailed = 2, MalformedTerminalStatus = 3 };
struct ProcessStatus {
    ProcessLiveness liveness;
    ProcessTermination termination;
    ProcessStatusError error;
    uint8_t code_valid;
    uint32_t code;
    uint32_t platform_status;
    uint32_t platform_error;
};
[[nodiscard]] bool ValidateProcessStatus(const ProcessStatus&) noexcept;
enum class StallCategory: uint16_t { None, HealthyIdle, HleStall, GuestDeadlock, CommandProcessorStall, GpuStall, PresentationStall, WorkerUnresponsive, ProcessExited, ProcessCrashed, ProcessTerminated, UnknownStall };
enum class Confidence: uint8_t { Low, Medium, High };
struct StallFact { Domain domain; uint16_t code; uint32_t flags; uint64_t instance; uint64_t correlation; };
struct LossSnapshot {
    WriterLossSnapshot writers;
    ProgressLossSnapshot progress;
    GlobalLossCounter unregistered_writers;
    GlobalLossCounter skipped_publications;
    GlobalLossCounter disconnects;
    GlobalLossCounter rejected_samples;
    uint64_t max_loss_monotonic_ns;
};
struct ObservationInput {
    ProgressSnapshot progress;
    WaitGraphSnapshot wait_graph;
    LossSnapshot loss;
    ProcessStatus process;
    uint64_t heartbeat_ns;
    uint64_t sample_time_ns;
};
struct CausalProgressFact {
    uint64_t progress_ref;
    uint64_t submitted;
    uint64_t completed;
    uint64_t correlation;
    uint64_t auxiliary[2];
    uint32_t operation;
    uint16_t state;
    uint16_t flags;
};
struct CausalStateKey {
    uint32_t stream_version;
    StallCategory category;
    uint8_t heartbeat_stale;
    uint8_t inventory_complete;
    ProcessStatus process;
    CausalProgressFact progress[MaxProgressSnapshotEntries];
    uint32_t progress_count;
    WaitGraphSnapshot wait_graph;
    LossSnapshot loss;
    uint8_t complete;
    uint8_t reserved[7];
};
struct SuspectedEvidence {
    uint64_t causal_fingerprint;
    uint64_t sample_time_ns;
    uint64_t heartbeat_ns;
    uint64_t max_loss_monotonic_ns;
    StallCategory category;
    Confidence confidence;
    uint16_t evidence_total;
    uint16_t evidence_stored;
    uint16_t contradiction_total;
    uint16_t contradiction_stored;
    uint8_t evidence_truncated;
    uint8_t contradiction_truncated;
    uint8_t reserved[6];
    StallFact evidence[16];
    StallFact contradictions[16];
};
struct ClassifierState {
    uint64_t causal_fingerprint;
    CausalStateKey causal_key;
    uint64_t first_observed_ns;
    uint64_t suspected_ns;
    uint64_t confirmed_ns;
    bool terminal_finalized;
    SuspectedEvidence suspected;
};
struct StallSettings;
struct StallResult;
StallResult Observe(const ObservationInput& input, const StallSettings& settings, ClassifierState* state) noexcept;
```

`ClassifierState` is the complete repeated-observation memory: CRC-64 is only a diagnostic index; repetition requires the same complete canonical `CausalStateKey` by every scalar, count, and array field. Progress facts are sorted by packed ref, wait edges lexicographically, and the full fixed capacities prevent key truncation. A changed key resets suspicion and replaces the bounded `SuspectedEvidence`; identical causal evidence advances suspicion/confirmation without rewriting that first snapshot. No hidden clock, process polling, or static mutable state exists. The bundle receives both `state.suspected` and the confirmed result.

- [ ] **Step 1: Add virtual-time classification tests**

Add `CausalFingerprintUsesEcmaCheckValue` for ASCII `123456789`; the protocol
must reuse this same core implementation rather than introducing another CRC.

Add an exact `ClassifiesHleStallAfterVirtualThreshold` test, then cover `HealthyIdle`, explicit-cycle-only `GuestDeadlock`, `CommandProcessorStall`, `GpuStall`, `PresentationStall`, `WorkerUnresponsive`, `ProcessExited`, `ProcessCrashed`, `ProcessTerminated`, and `UnknownStall`. Advance sample time directly; do not sleep. `HleStallJoinsOnlyItsOwningGuestThread` proves unrelated guest progress does not clear a stalled call while progress by its correlation owner does. `GuestDeadlockRequiresResolvedWaitGraphCycle` builds two waiting progress records, derives the typed graph, round-trips it through the classifier input, and proves that a missing/unknown/unresolved producer prevents the category. `CausalKeyIgnoresTimeAndInputOrder` proves time-only resampling and record reordering do not reset; `CausalKeyChangesForEpochCorrelationOrWaitEdge` proves each semantic change does; `FingerprintCollisionDoesNotMergeStates` injects equal hashes with unequal typed fact arrays and requires reset. Prove confirmed evidence cannot mutate the stored first suspected snapshot. `EvidenceTruncationIsExplicit` supplies 17 contradictions, requires total 17/stored 16/truncated, deterministic selection, and confidence below High. An incomplete canonical key likewise prevents High confidence. Assert incomplete inventory, unavailable progress, progress capacity/rejected-update loss, registration loss, inactive-writer attempts, unregistered writers, rejected snapshot samples, skipped publications, or an overlapping event-loss interval prevents high confidence; a skipped publication affects every shared section for that interval. A disconnect is reported as a transport fact and never silently interpreted as healthy progress. Assert apparent all-thread blocking without an explicit wait cycle remains unknown. Add a compiled operation-threshold override test showing the existing `OperationCode::PresentationDeviceIdle` operation uses its explicit larger virtual-time window while `OperationCode::RenderCommandBuffer` uses the default; invalid/duplicate override keys are rejected in settings validation. Add `ProcessStatusCombinationsAreClosed`: Unknown and Running require `termination=None`, no code, and no platform error; a terminal ExitCode/Signal/UnhandledException requires a portable code; OpaquePlatformStatus forbids one; any observation error requires Unknown/None and no code, WaitFailed/QueryFailed require nonzero platform error, and MalformedTerminalStatus permits zero; all other combinations fail validation. Feed the four valid terminal forms and assert exact `ProcessExited`, `ProcessCrashed`, and `ProcessTerminated` categories. `OpaquePlatformStatusNeverBecomesExitOrCrash` and `StatusErrorNeverEntersTerminalClassifier` are mandatory: a non-`None` error returns nonterminal `UnknownStall`/Low if accidentally passed, while the supervisor normally bypasses `Observe` for that value. A live confirmed stall remains nonterminal and continues sampling. Add the public types plus a compile-only `Observe` stub that always returns `UnknownStall`/`Low`, so the exact HLE test executes and fails.

- [ ] **Step 2: Rebuild and prove classifier tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/classifier.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsClassifier\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ClassifiesHleStallAfterVirtualThreshold$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsClassifier.ClassifiesHleStallAfterVirtualThreshold' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsClassifier\.ClassifiesHleStallAfterVirtualThreshold' "$red_log"
```

- [ ] **Step 3: Implement settings and categories**

```cpp
struct OperationThreshold {
    Domain domain;
    uint32_t operation;
    uint64_t suspected_after_ns;
    uint64_t confirmed_after_ns;
};
struct StallSettings {
    uint64_t suspected_after_ns = 5'000'000'000ull;
    uint64_t confirmed_after_ns = 15'000'000'000ull;
    OperationThreshold overrides[16] = {};
    uint8_t override_count = 0;
};
struct StallResult {
    StallCategory category;
    Confidence confidence;
    uint8_t terminal;
    uint16_t evidence_total;
    uint16_t evidence_stored;
    uint16_t contradiction_total;
    uint16_t contradiction_stored;
    uint8_t evidence_truncated;
    uint8_t contradiction_truncated;
    uint8_t reserved[5];
    uint64_t suspected_ns;
    uint64_t confirmed_ns;
    ProcessStatus process;
    StallFact evidence[16];
    StallFact contradictions[16];
};
```

Classification uses only explicit progress/loss/typed-wait-graph/heartbeat/process facts. It canonicalizes category, process state, heartbeat-stale/inventory-complete booleans, every progress record without last-change time, sorted wait edges, and complete loss provenance into the typed key; sample time, absolute ages, and inventory generation are excluded. The exposed fingerprint is CRC-64/ECMA-182 over the exact versioned little-endian field order, but equality always compares the complete typed key. `CanonicalKeyCoversMaximumSnapshot` fills all 1,504 progress, 512 edge, and 512 writer-loss entries, mutates the final entry of each in turn, and requires suspicion reset without key truncation. The override table is fixed-capacity and populated only from compiled allowlisted domain/operation pairs; it cannot suppress unrelated lanes. It reports repeated boundary events as a fact but has no general livelock category. Validate the exact `ProcessStatus` combination before classification. `ProcessExited` is derived only from `ExitCode`; `ProcessCrashed` only from `Signal` or explicitly observed `UnhandledException`; `OpaquePlatformStatus` yields `ProcessTerminated`. A non-`None` observation error cannot finalize a terminal classifier result. A terminal observation sets `terminal_finalized`; subsequent calls cannot emit a second terminal bundle. A live stall never sets it. More than 16 evidence or contradiction facts records the exact total and truncation and prevents High confidence.

- [ ] **Step 4: Run and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsClassifier\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsClassifier.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/lib/DevTools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsClassifier.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): classify runtime stalls'
```

### Task 5: Implement the fixed shared-memory protocol

**Files:**
- Create: `source/lib/DevTools/include/Kyty/DevTools/Transport/Protocol.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Transport/SharedAtomic.h`
- Create: `source/lib/DevTools/src/Transport/Protocol.cpp`
- Create: `source/lib/DevTools/src/Transport/SharedAtomicPosix.cpp`
- Create: `source/lib/DevTools/src/Transport/SharedAtomicWindows.cpp`
- Modify: `source/lib/DevTools/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsProtocol.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: exact wire layout in the accepted spec.
- Consumes the one core `Crc64Ecma` implementation introduced for canonical classifier keys in Task 4.
- Produces `InitializeProtocol`, one `ProgressPublication` aggregate containing progress/writer inventory/writer/domain loss plus coherent measurement and GPU-fault snapshots, `PublishProgress`, `PublishTimeline`, one atomic `ReadProgressPublication`, `ReadTimeline`, `ReadProtocolHealth`, header/section validation, `RecordingMode { MetricsOnly=1, Full=2 }`, `LoggingMode { Unknown=0, Silent=1, Console=2, File=3, Directory=4 }`, `ShaderCacheState { Unknown=0, NoPersistentCache=1, PersistentCacheCold=2, PersistentCacheWarm=3, PersistentCacheDisabled=4 }`, caller-owned `ProtocolReadLossState`, and the exact health type below. Measurement and GPU-fault views come only from the returned `ProgressPublication`; there are no independent mapping reads that could mix generations.

```cpp
struct ProtocolHealthSnapshot {
    GlobalLossCounter aggregate_ring;
    GlobalLossCounter unregistered_writers;
    GlobalLossCounter registration_capacity;
    GlobalLossCounter instance_capacity;
    GlobalLossCounter skipped_publications;
    GlobalLossCounter disconnects;
    GlobalLossCounter rejected_samples;
    GlobalLossCounter inactive_token_attempts;
    uint64_t max_loss_monotonic_ns;
};
enum class RecordingMode: uint32_t { MetricsOnly=1, Full=2 };
enum class LoggingMode: uint32_t {
    Unknown=0, Silent=1, Console=2, File=3, Directory=4
};
enum class ShaderCacheState: uint32_t {
    Unknown=0, NoPersistentCache=1, PersistentCacheCold=2,
    PersistentCacheWarm=3, PersistentCacheDisabled=4
};
struct ParentProtocolInit {
    uint64_t supervisor_pid;
    uint64_t supervisor_start_token;
    uint8_t nonce[16];
    RecordingMode requested_mode;
};
struct WorkerHandshake {
    uint64_t worker_pid;
    uint64_t worker_start_token;
    uint8_t nonce[16];
    RecordingMode accepted_mode;
    LoggingMode logging_mode;
    ShaderCacheState shader_cache_state;
    char revision[40];
    uint32_t dirty;
    uint32_t validation_enabled;
    uint32_t resolution_width;
    uint32_t resolution_height;
    uint64_t capabilities[2];
};
struct ProgressPublication {
    ProgressSnapshot progress;
    WriterLossSnapshot writer_loss;
    WriterInventorySnapshot writer_inventory;
    ProgressLossSnapshot progress_loss;
    MeasurementSnapshot measurement;
    GpuFaultSnapshot gpu_fault;
};
struct TimelineSnapshot {
    EventRecord events[4096];
    uint32_t count;
    uint32_t reserved;
    uint64_t generation;
};
struct MutableMappingView { uint8_t* data; uint64_t size; };
struct ConstMappingView { const uint8_t* data; uint64_t size; };
enum class ProtocolResult: uint8_t {
    Ok=0, InvalidArgument=1, Incompatible=2, InvalidLayout=3,
    Corrupt=4, Busy=5, Rejected=6
};
enum class ControlCell: uint8_t {
    AggregateRing=0, UnregisteredWriters=1, RegistrationCapacity=2,
    InstanceCapacity=3, SkippedPublications=4, Disconnects=5,
    RejectedSamples=6, InactiveTokenAttempts=7
};
struct ProtocolReadLossState {
    GlobalLossCounter rejected_samples;
};
ProtocolResult InitializeProtocolOwner(MutableMappingView,
                                       const ParentProtocolInit&) noexcept;
ProtocolResult PublishWorkerHandshake(MutableMappingView,
                                      const WorkerHandshake&) noexcept;
ProtocolResult AcceptWorkerHandshake(MutableMappingView,
                                     const ParentProtocolInit&,
                                     WorkerHandshake*) noexcept;
ProtocolResult RejectWorkerHandshake(MutableMappingView,
                                     ProtocolResult reason) noexcept;
ProtocolResult PublishWorkerHeartbeat(MutableMappingView,
                                      uint64_t monotonic_ns) noexcept;
ProtocolResult PublishWorkerControl(MutableMappingView,
                                    ControlCell,
                                    const GlobalLossCounter&) noexcept;
ProtocolResult PublishProgress(MutableMappingView,
                               const ProgressPublication&) noexcept;
ProtocolResult PublishTimeline(MutableMappingView,
                               const TimelineSnapshot&) noexcept;
ProtocolResult ReadProgressPublication(ConstMappingView,
                                       ProtocolReadLossState*,
                                       ProgressPublication*) noexcept;
ProtocolResult ReadTimeline(ConstMappingView, ProtocolReadLossState*,
                            TimelineSnapshot*) noexcept;
ProtocolResult ReadProtocolHealth(ConstMappingView,
                                  ProtocolHealthSnapshot*) noexcept;
uint64_t Crc64Ecma(const uint8_t* bytes, uint64_t size) noexcept;
```

A progress generation is assembled once and release-published once;
measurement and device-fault data are never patched into an already active
buffer. Header controls are independent monotonic notifications, not a second
causal generation: when a control duplicates detailed publication data it may
be newer, and the classifier conservatively uses the greater total/time rather
than requiring equality or replacing detailed provenance.

- [ ] **Step 1: Add byte-offset and CRC tests**

Add the exact test `Crc64MatchesEcmaCheckValue`, then assert magic `KYTYDVT1`, major/minor `1/0`, byte-order/word-size tags `0x01020304/8`, total mapping `0x141000`, header `0x1000`, progress buffers at `0x001000/0x021000` sized `0x020000`, timeline buffers at `0x041000/0x0c1000` sized `0x080000`, event size 72, progress size 64, active descriptor packing, handshake enum values, requested/accepted recording modes at `0x0d4/0x0d8`, logging values `0..4`, shader-cache values `0..4`, protocol capability bit 0 for the counts-only device-fault block, and all section descriptor offsets. Assert progress/timeline schema IDs `0x31475250/0x314e4c54`, flags zero, capacities `1504/4096`, fixed progress payload size `0x1f100`, and timeline payload size exactly `count * 72` with maximum `0x48000`. Assert the eight 64-byte control cells at `0x300..0x4c0`, with total/time at `+0x00/+0x08`, zero reserved bytes, child-publisher-only writes for ring/unregistered/registration/instance/skipped/disconnect/inactive-token cells, and parent-reader-only writes for rejected samples at `0x480`. Within the progress payload, assert eight domain descriptors at `0x000`, coherent nonzero even progress generation at `0x0c0`, independent writer generation at `0x0c8`, either zero meaning that inventory is unavailable, and `0x0d0..0x0ff` reserved zero, separate unsequenced 24-byte domain capacity-loss summaries at `0x100` with a zero reserved middle word, the 512-entry/`0x3000`-byte cumulative slot-loss table at `0x200`, progress arrays beginning at `0x3200` and ending at `0x1aa00`, eight rejected-update summaries at `0x1aa00`, zero padding through `0x1ac00`, the `0x2400`-byte measurement block at `0x1ac00` with schema `0x3154454d`, flags/reserved at `+0x08/+0x0c`, exact counter/histogram offsets, and the `0x100`-byte device-fault block at `0x1d000` with schema `0x31465047`, the exact spec fields, and zero reserved bytes. Assert writer records preserve cumulative total/last-attempted-sequence/time across slot generation reuse, every v1-reserved byte is written zero and rejected if nonzero, and the entire fixed payload fits the existing `0x020000` buffer. Verify CRC-64/ECMA-182 check value `0x6c40df5f0b497347` for ASCII `123456789`.

`ProgressWireDomainLayoutIsExact` asserts record arrays at
GuestThread `0x3200/256`, Hle `0x7200/512`, CommandProcessor `0xf200/80`,
Renderer `0x10600/32`, GpuQueue `0x10e00/32`, VideoOut `0x11600/64`,
Presentation `0x12600/16`, and Synchronization `0x12a00/512`. It requires
section item count to equal the checked descriptor-count sum, record offset zero
to equal the packed entry key, packed domain to equal descriptor domain, and
all refs to be unique. It accepts the same low instance in two domains, then
rejects key/record, descriptor-domain, duplicate-ref, count, offset, order, and
capacity mutations before any wait-graph or classifier call.

`WriterInventoryWireLayoutIsExact` asserts a 512-entry writer inventory at
`0x1d100`, with 16-byte records containing canonical `WriterKey` followed by
one packed identity word: diagnostic thread instance in bits 0..55,
`ThreadRole` in 56..59, lifecycle state in 60..61, and zero in 62..63. The
table ends at the fixed payload boundary `0x1f100`. Empty slots are all zero;
active entries must agree with the key's registry generation and the writer-
inventory generation. The atomic publication binds that coherent snapshot to
the independently coherent progress generation. Reject duplicate active keys, unknown
roles/states, instance mismatch, and reserved-bit mutations. `threads.json`
is generated from this inventory and uses timeline lifecycle events only as
history, never as the source of the active set.

The header test also asserts exact little-endian scalar widths: major/minor are
u16; header, byte-order, word-size, dirty, log/cache, validation, resolution,
and modes are u32; total size, identities, and start token are u64. It mutates
dirty/validation to 2 and mutates every reserved neighboring byte to prove
canonical-boolean and padding rejection.

- [ ] **Step 2: Add concurrency protocol tests**

Cover reader pin/recheck, late-reader generation rejection, ABA prevention, inactive-buffer skip when pinned, interrupted publisher retaining prior active snapshot, checksum/size/schema rejection, and bounded retry reporting. Add exact major/minor compatibility tests: only `1/0` succeeds; any major or minor mismatch enters `WorkerRejected` and permits no section read, even if unknown capability bits are set. A missing required capability or mutated descriptor is also rejected. Test MetricsOnly/Full request/echo, invalid zero/unknown mode, and parent rejection of a mismatched child echo. A pinned inactive buffer increments publisher-owned skipped publications exactly once without replacing the active snapshot. A final rejected `ReadProgressPublication`/`ReadTimeline` sample updates the parent-owned rejected-sample total/time exactly once and never claims a sequence; a retry that eventually succeeds does not. `ReadProtocolHealth` returns all eight named monotonic control totals and preserves unregistered-writer versus inactive-token provenance; duplicate quick controls are permitted to be newer than the detailed publication and never erase its slot/domain detail. A timeline record with a future numeric domain/event is copied unchanged and receives reader-owned unsupported metadata; it is never remapped, used as an index, or written back with changed flags. `ProgressUnavailableRoundTripsAndLowersConfidence` publishes a stable-key-only `Unavailable` item, reads the same state with every other field zero, and feeds it to the classifier as incomplete evidence. `ProgressLossReasonsRoundTripSeparately` preserves each domain's capacity and rejected-update total/time, verifies the descriptor total is their sum, and never merges either with instance-capacity aggregate control. One `PublishProgress`/`ReadProgressPublication` round trip preserves both recording modes, measurement counts/overflow/all 1,024 buckets, and all five GPU-fault states/flags/counts from one descriptor generation while every reserved byte remains zero. `ProgressPublicationCannotMixEmbeddedGenerations` publishes N+1 between attempted consumer views and proves the API returns either all of N or all of N+1, never mixed progress/measurement/fault. A failed/interrupted publication must leave all three prior embedded snapshots coherent. Use only process-local threads and deterministic barriers; no sleeps. Add the exact API declarations plus compile-only stubs that reject every read/publish and return a zero CRC so the suite builds, lists, and fails before protocol implementation.

`ReadProgressPublication` and `PublishProgress` both call
`ValidateGpuFaultSnapshot`; protocol tests mutate each state field and require
rejection of reserved flags, wrong result/time/count combinations, nonzero
vendor-binary size, and unknown states before the snapshot reaches a bundle.

- [ ] **Step 3: Reconfigure and prove protocol tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/protocol.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsProtocol\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  Crc64MatchesEcmaCheckValue$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsProtocol.Crc64MatchesEcmaCheckValue' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsProtocol\.Crc64MatchesEcmaCheckValue' "$red_log"
```

- [ ] **Step 4: Implement platform atomic words**

POSIX uses `__atomic` sequentially consistent operations on validated aligned 64-bit wire cells. Windows uses `Interlocked*`. Build-time/runtime checks refuse transport when eight-byte lock-free behavior is unavailable; they never substitute a process-local mutex.

- [ ] **Step 5: Implement fixed encoding, double buffers, and CRC**

Encode every field by offset; do not `reinterpret_cast` the mapping to a public struct. Validate exact major/minor/capability/descriptor compatibility before exposing any section. `PublishProgress` serializes one immutable `ProgressPublication`: `ProgressSnapshot`, `WriterLossSnapshot`, `WriterInventorySnapshot`, separate progress capacity/rejected-update arrays, coherent `MeasurementSnapshot`, and coherent `GpuFaultSnapshot`. It validates every packed ref against its entry key and domain descriptor, uniqueness, the exact fixed per-domain offsets/capacities/order, and item-count sum before writing. It writes each descriptor's total incompleteness as the checked sum of its two reason totals, then copies the seven child-owned local loss/health aggregates to their exact wire cells; only the child publisher touches those cells. It writes every fixed block into the same inactive buffer before one descriptor release. `ReadProgressPublication` performs one descriptor load, reader-count increment, descriptor recheck, whole-payload copy/validation including the same identity/count invariants, and reader-count decrement; callers obtain measurement/fault only as fields of that local copy. After bounded retries fail, the sole parent reader updates its unsequenced rejected-sample control and caller state; no shared multi-writer sequence is fabricated. The sole child publisher checks inactive count; when pinned it increments its local skipped-publication total/time and copies it to the wire while keeping the prior active snapshot, otherwise it writes payload/header and publishes a strictly newer descriptor. It likewise owns wire copies of local disconnect state. `ReadProtocolHealth` copies all eight monotonic controls without assigning cross-owner sequences. Decode retains future numeric domain/event values with reader-owned unsupported metadata and never indexes fixed tables with them. All CRC call sites use the Task 4 implementation with polynomial `0x42f0e1eba9ea3693`, init/xor zero, and no reflection.

- [ ] **Step 6: Run and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsProtocol\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsProtocol.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/lib/DevTools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsProtocol.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): publish coherent stall snapshots'
```

### Task 6: Generate sanitized build provenance

**Files:**
- Create: `source/KytyBuildInfo.h.in`
- Delete: `source/KytyGitVersion.h.in`
- Modify: `source/generate_version.cmake`
- Modify: `source/CMakeLists.txt`
- Modify: `source/KytyScripts.cpp`
- Modify: `source/launcher/CMakeLists.txt`
- Modify: `source/emulator/CMakeLists.txt`
- Create: `source/cmake/tests/TestBuildInfo.cmake`

**Interfaces:**
- Consumes: Git only at build time.
- Produces `Kyty::BuildInfo::Revision`, `Dirty`, and `RevisionKnown`; no branch/path/status text.

- [ ] **Step 1: Add a deterministic build-info generator test**

`TestBuildInfo.cmake` creates temporary sanitized source directories under its supplied binary scratch path: a clean one-commit Git repository, the same repository with one untracked file, and a non-Git directory. It invokes `generate_version.cmake` and asserts exact 40-hex/known/clean, known/dirty, and zero-hash/unknown/dirty outputs. It also rejects embedded temp paths, branch names, and raw status text. No host repository state is mutated. Contract mismatches must end with the exact diagnostic `BUILD_INFO_CONTRACT_MISSING`; setup or Git-fixture failures use `BUILD_INFO_TEST_INFRA_ERROR` instead, so the red gate cannot accept broken test infrastructure.

- [ ] **Step 2: Run the generator test and prove red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/build-info.log"
set +e
cmake -DTEST_BINARY_DIR="$scratch/build-info-test" -DPROJECT_SOURCE_DIR="$devtools/source" \
  -P "$devtools/source/cmake/tests/TestBuildInfo.cmake" >"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q 'BUILD_INFO_CONTRACT_MISSING' "$red_log"
set +e
rg -q 'BUILD_INFO_TEST_INFRA_ERROR' "$red_log"
infra_status=$?
set -e
test "$infra_status" -eq 1
```

- [ ] **Step 3: Define the generated header contract**

```cpp
#pragma once
namespace Kyty::BuildInfo {
inline constexpr char Revision[] = "@KYTY_BUILD_REVISION@";
inline constexpr bool Dirty = @KYTY_BUILD_DIRTY@;
inline constexpr bool RevisionKnown = @KYTY_BUILD_REVISION_KNOWN@;
}
```

- [ ] **Step 4: Update generation**

Pass `SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}` explicitly from the custom target. Run `git -C "${SOURCE_DIR}" rev-parse --verify HEAD`; require exactly 40 lowercase hex characters. Define dirty to include tracked changes and untracked files via `git status --porcelain --untracked-files=normal`, but never embed that output. Render booleans as lowercase C++ tokens `true`/`false`, never CMake `TRUE`/`FALSE` or `0`/`1`. Without Git, emit forty zeroes, `Dirty=true`, `RevisionKnown=false`. Declare `KytyBuildInfo.h` as a custom-target byproduct.

- [ ] **Step 5: Update all target dependencies and the script banner**

Replace `KytyGitVersion` references with `KytyBuildInfo`; make `fc_script`, `emulator`, and the actual consuming launcher target depend on it. Preserve user-facing version output by appending a literal `-dirty` only from the boolean. Delete the superseded `KytyGitVersion.h.in` in the same commit so there is one source of build provenance.

- [ ] **Step 6: Re-run the generator test, build, inspect, and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/build-info-test"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
cmake -DTEST_BINARY_DIR="$scratch/build-info-test" -DPROJECT_SOURCE_DIR="$devtools/source" -P "$devtools/source/cmake/tests/TestBuildInfo.cmake"
rg -n 'Revision|Dirty|RevisionKnown' "$devtools/_build_linux_devtools/KytyBuildInfo.h"
set +e
rg -n '/home/|codex/|PPSA' "$devtools/_build_linux_devtools/KytyBuildInfo.h"
privacy_status=$?
set -e
test "$privacy_status" -eq 1
git -C "$devtools" diff --check
git -C "$devtools" add -A source/KytyBuildInfo.h.in source/KytyGitVersion.h.in source/generate_version.cmake source/CMakeLists.txt source/KytyScripts.cpp source/launcher/CMakeLists.txt source/emulator/CMakeLists.txt source/cmake/tests/TestBuildInfo.cmake
git -C "$devtools" commit -m 'build: generate sanitized build provenance'
```

### Task 7: Build the testable supervisor process boundary

**Files:**
- Modify: `source/CMakeLists.txt`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Transport/Bootstrap.h`
- Create: `source/lib/DevTools/include/Kyty/DevTools/Transport/ProcessIdentity.h`
- Create: `source/lib/DevTools/src/Transport/Bootstrap.cpp`
- Create: `source/lib/DevTools/src/Transport/ProcessIdentity.cpp`
- Modify: `source/lib/DevTools/CMakeLists.txt`
- Create: `source/devtools/CMakeLists.txt`
- Create: `source/devtools/include/Kyty/DevTools/Supervisor/ProcessLauncher.h`
- Create: `source/devtools/include/Kyty/DevTools/Supervisor/SharedMapping.h`
- Create: `source/devtools/src/ProcessLauncherPosix.cpp`
- Create: `source/devtools/src/ProcessLauncherWindows.cpp`
- Create: `source/devtools/src/SharedMappingPosix.cpp`
- Create: `source/devtools/src/SharedMappingWindows.cpp`
- Create: `source/devtools/src/SecureRandomPosix.cpp`
- Create: `source/devtools/src/SecureRandomWindows.cpp`
- Create: `source/devtools/src/ProcessIdentityLinux.cpp`
- Create: `source/devtools/src/ProcessIdentityMacos.cpp`
- Create: `source/devtools/src/ProcessIdentityWindows.cpp`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsSupervisor.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: protocol mapping size and handshake state.
- Produces static `kyty_devtools_supervisor`, `SharedMapping::CreateOwnerOnly`,
  `ProcessLauncher::Launch`, `Poll`, `Wait`, and `ForwardSignal`, plus:

```cpp
enum class ProcessOperationError: uint8_t {
    None = 0, Unsupported = 1, InvalidArgument = 2, EntropyUnavailable = 3,
    MappingFailed = 4, SpawnFailed = 5, HandleFailed = 6
};
struct ProcessUsage { uint64_t user_ns; uint64_t system_ns; uint8_t valid; };
struct ProcessObservation { ProcessStatus status; ProcessUsage usage; };
class ProcessHandle;
struct ProcessIdentity { uint64_t pid; uint64_t start_token; };
enum class ProcessIdentityError: uint8_t { None = 0, Unavailable = 1, Malformed = 2, Overflow = 3 };
ProcessIdentityError QueryProcessIdentity(const ProcessHandle&, ProcessIdentity*) noexcept;
enum class ProcessIdentityProbe: uint8_t {
    Dead = 0, AliveMatch = 1, AliveDifferentStart = 2,
    Unreadable = 3, Malformed = 4, Overflow = 5
};
ProcessIdentityProbe ProbeProcessIdentity(uint64_t pid,
                                          uint64_t expected_start_token) noexcept;
struct BootstrapNonce { uint8_t bytes[16]; };
enum class BootstrapPlatform: uint8_t { Posix = 1, Windows = 2 };
enum class BootstrapParseResult: uint8_t { Missing = 0, Valid = 1, Malformed = 2 };
struct BootstrapMetadata {
    BootstrapPlatform platform;
    uint64_t mapping_handle;
    uint64_t liveness_handle;
    BootstrapNonce nonce;
};
struct BootstrapText { char bytes[80]; uint32_t size; };
BootstrapParseResult ParseBootstrapMetadata(const char* value, BootstrapMetadata*) noexcept;
bool EncodeBootstrapMetadata(const BootstrapMetadata&, BootstrapText*) noexcept;
class ProcessHandle {
public:
    ProcessHandle() noexcept;
    ProcessHandle(ProcessHandle&&) noexcept;
    ProcessHandle& operator=(ProcessHandle&&) noexcept;
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    bool IsValid() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
struct LaunchResult {
    ProcessOperationError error;
    uint32_t platform_error;
    ProcessHandle process;
};
class SharedMapping {
public:
    SharedMapping() noexcept;
    SharedMapping(SharedMapping&&) noexcept;
    SharedMapping& operator=(SharedMapping&&) noexcept;
    ~SharedMapping();
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;
    static ProcessOperationError CreateOwnerOnly(uint64_t size,
                                                 SharedMapping* out) noexcept;
    MutableMappingView MutableView() noexcept;
    ConstMappingView View() const noexcept;
    uint64_t InheritableHandle() const noexcept;
    void Close() noexcept;
};
struct LaunchOptions {
    const char* executable;
    const char* const* argv;
    uint32_t argc;
    BootstrapText bootstrap;
};
class ProcessLauncher {
public:
    static LaunchResult Launch(const LaunchOptions&) noexcept;
    static ProcessOperationError Poll(ProcessHandle*,
                                      ProcessObservation*) noexcept;
    static ProcessOperationError Wait(ProcessHandle*,
                                      ProcessObservation*) noexcept;
    static ProcessOperationError ForwardSignal(ProcessHandle*,
                                               uint32_t signal) noexcept;
};
```

`BootstrapNonce`, bootstrap grammar/parser/encoder, the `ProcessIdentity`
value/error, and pure checked normalization/parsing helpers live in
`kyty_devtools_core`. The overload above is supervisor-only and queries an
independently owned child `ProcessHandle`; Task 10 adds a worker-side
`QueryCurrentProcessIdentity` adapter without linking the supervisor.
`Poll`/`Wait` return `ProcessObservation`; they never expose an undecoded
integer as an exit/crash. `Launch` compile stubs return the explicit
`Unsupported` error, never a magic boolean/status.

- [ ] **Step 1: Add mockable lifecycle tests**

Add exact tests named `CreatesOwnerOnlyMapping`, `BootstrapMetadataRoundTripsExactly`, `MissingBootstrapSelectsStandalone`, `MalformedBootstrapOpensNoHandle`, `BootstrapIsScrubbedBeforeWorkerInit`, `ProcessIdentityMatchesIndependentQueries`, `LinuxProcessStatParsesParenthesizedCommand`, `ProcessIdentityRejectsUnreadableMalformedOrOverflow`, `ReusedPidWithDifferentStartTokenIsRejected`, `RejectsNonceMismatch`, `RejectsChildIdentityMismatch`, `RejectsMajorVersionMismatch`, `RejectsAnyMinorMismatch`, `RejectsMissingCapabilityOrMutatedDescriptor`, `HandshakeTimeoutIsStructured`, `ParentAndChildShareMonotonicEpoch`, `PosixExitDecodesExitCode`, `PosixSignalDecodesCrash`, `PosixMalformedTerminalStatusIsDecodeError`, `TerminalPollPreservesUsageExactlyOnce`, `PosixWaitBeforeTerminalBlocksThenCaches`, `PosixWaitRetriesEintr`, `PosixWaitHardErrorIsStructured`, `WindowsWaitFailureIsObservationError`, `WindowsSignaledStatusIsOpaqueTerminal`, `WindowsStatusQueryFailureIsDecodeError`, `WindowsSignaledStillActiveIsMalformed`, `DisconnectDisablesTransport`, `InheritsOnlyMappingAndLiveness`, `LinuxEntropyHandlesPartialAndEintr`, `LinuxEntropyFailsOnZeroOrHardError`, and `MappingCreationRejectsCollisionOrWrongSize`. Bootstrap and pure identity-normalization tests link only `kyty_devtools_core`; bootstrap tests enforce the exact `KYTY_DEVTOOLS_BOOTSTRAP_V1` grammar from the spec, missing/malformed distinction, fixed POSIX descriptors, nonzero bounded Windows handles, independent mapping nonce comparison, child-only environment replacement, immediate erase before script/guest initialization, and exclusion from every log/artifact. Process identity tests use Linux start ticks, checked macOS start microseconds, or Windows creation FILETIME, require independent parent/child nonzero equality, and reuse the same normalized value for stale-directory ownership; unreadable identity always retains evidence. `ParentAndChildShareMonotonicEpoch` launches the synthetic child through the real platform adapter, publishes one worker time, samples parent time through the same `MonotonicNowNs`, and proves checked 5 s/15 s ages and an overlapping loss interval without wall-clock conversion. On POSIX, inject terminal raw statuses for `WIFEXITED`, `WIFSIGNALED`, and neither; only the first two decode. The terminal poll uses one injected `wait4` result with rusage, then proves repeated Poll/Wait return the cached observation without another reap. Wait without a cached terminal observation calls the same helper without `WNOHANG`; both paths retry `EINTR`, and every other error returns `WaitFailed` with errno. On Windows, inject timeout, wait failure, signaled plus terminal DWORD, failed `GetExitCodeProcess`, and signaled plus `STILL_ACTIVE`. A terminal DWORD is always `OpaquePlatformStatus`: nonzero is not a crash heuristic. Failures preserve the platform error and no portable code. Major/minor/protocol rejection must observe `WorkerRejected` and no section read. The inherited-descriptor test opens an unrelated non-CLOEXEC sentinel and proves the synthetic child receives only standard streams plus fixed mapping/liveness descriptors. `ProcessHandle` and `SharedMapping` are move-only RAII owners with out-of-line platform state; each closes exactly once and no public API exposes a native status as a portable exit/crash. Add mockable interface declarations plus platform compile stubs that return `ProcessOperationError::Unsupported`; register the suite so it builds and fails its success-path expectations before implementation.

- [ ] **Step 2: Prove the registered supervisor tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/supervisor-boundary.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsSupervisor\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  CreatesOwnerOnlyMapping$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsSupervisor.CreatesOwnerOnlyMapping' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsSupervisor\.CreatesOwnerOnlyMapping' "$red_log"
```

- [ ] **Step 3: Implement secure random and mappings**

Linux loops until all 16 nonce bytes are filled, preserving partial reads and retrying only `EINTR`; zero or any other error returns `EntropyUnavailable` and never falls back. macOS uses `arc4random_buf`; Windows uses `BCryptGenRandom` and links `bcrypt`. POSIX mapping generates a fresh random name for each bounded attempt and uses `shm_open(O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0600)`, exact-size `ftruncate` plus `fstat` validation, `mmap(MAP_SHARED)`, and immediate `shm_unlink`. A collision retries with a new name; wrong owner/mode/size or exhausted retries fails closed. Windows uses restricted-DACL `CreateFileMappingW`, explicit `MapViewOfFile`, and one duplicated inheritable handle.

- [ ] **Step 4: Implement launchers**

POSIX normalizes mapping/liveness to fixed descriptors 3/4 before spawn. On Linux a compile-checked `posix_spawn_file_actions_addclosefrom_np` closes descriptor 5 and above; on macOS compile-checked `POSIX_SPAWN_CLOEXEC_DEFAULT` plus explicit inherit exposes only 3/4. Absence of the required primitive returns `Unsupported`; there is no leaky fallback. The launcher generates `KYTY_DEVTOOLS_BOOTSTRAP_V1` from its independent nonce and normalized handles, replaces any same-named caller value only in the child environment, and never emits it. Windows encodes only the two restricted inherited handle values; POSIX encodes only 3/4. The worker parser distinguishes missing from malformed, erases the variable before any other subsystem, opens no handle on malformed input, and compares the parsed nonce with the mapping nonce before `WorkerReady`. One synchronized reap helper owns the terminal cache: Poll calls `wait4(..., WNOHANG, ..., &rusage)`, Wait calls `wait4(..., 0, ..., &rusage)` only when the cache is empty, both retry `EINTR`, and any other error returns `WaitFailed` with errno. The first terminal result atomically caches both `ProcessStatus` and `ProcessUsage`; every later caller reads it without another wait. Status is decoded only through `WIFEXITED`/`WEXITSTATUS` or `WIFSIGNALED`/`WTERMSIG`; a terminal status matching neither sets `MalformedTerminalStatus`. Windows uses `CreateProcessW` with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` containing exactly the restricted mapping and parent-liveness handles. `WAIT_TIMEOUT` is Running; `WAIT_FAILED` sets `WaitFailed`; after a signaled handle, process times are queried before handle close, `GetExitCodeProcess` failure sets `QueryFailed`, `STILL_ACTIVE` sets `MalformedTerminalStatus`, and every other DWORD is `Terminated/OpaquePlatformStatus`. No numeric range or high-bit test calls it a crash. Parent and child close unused pipe/handle ends immediately after launch/handshake. Neither bundle nor log path records child argv/environment/bootstrap. Attach mode is absent.

- [ ] **Step 5: Build library/tests and commit**

Link the focused test with `target_link_libraries(unit_test kyty_devtools_supervisor)` using the existing non-keyword signature. The supervisor library may depend on `kyty_devtools_core`; neither target may depend on `emulator`. Keep `kyty_devtools_core` dependency-free. Link supervisor/process-thread consumers with the project's `Threads::Threads` target after `find_package(Threads REQUIRED)`, and link `bcrypt` only in the Windows branch that builds `SecureRandomWindows.cpp`.

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsSupervisor\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsSupervisor.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/CMakeLists.txt source/lib/DevTools source/devtools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsSupervisor.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): launch supervised workers'
```

### Task 8: Write deterministic privacy-safe bundles

**Files:**
- Create: `source/devtools/include/Kyty/DevTools/Supervisor/BundleWriter.h`
- Create: `source/devtools/include/Kyty/DevTools/Supervisor/DurableFile.h`
- Create: `source/devtools/src/BundleWriter.cpp`
- Create: `source/devtools/src/DurableFilePosix.cpp`
- Create: `source/devtools/src/DurableFileWindows.cpp`
- Modify: `source/devtools/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsBundle.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: validated suspected/confirmed snapshot copies, `ClassifierState::suspected`, `StallResult`, `WriterLossSnapshot`, `ProgressLossSnapshot`, `ProtocolHealthSnapshot`, `GpuFaultSnapshot`, and the typed supervisor trigger/status even when no terminal classifier result exists.
- Produces an atomic directory containing `progress.json`, `threads.json`, `wait_graph.json`, `gpu.json`, `timeline.bin`, `manifest.json`, and final `complete.marker`.

`complete.marker` is exactly 64 bytes and little-endian: magic `KYTDBND1` at
0x00, schema major/minor u16 values `1/0` at 0x08/0x0a, flags u32 zero at
0x0c, manifest byte size u64 at 0x10, manifest CRC-64/ECMA-182 at 0x18, bundle
generation u64 at 0x20, and zero reserved bytes through 0x3f. It is written
only after the durable manifest and every checksum named by that manifest.
Readers reject wrong size, reserved bytes, checksum, schema, or generation.

```cpp
enum class BundleWriteResult: uint8_t {
    Ok=0, InvalidInput=1, Conflict=2, IoError=3, DurabilityError=4
};
struct BundleInput {
    const ProgressPublication* publication;
    const TimelineSnapshot* timeline;
    const WaitGraphSnapshot* wait_graph;
    const ClassifierState* classifier;
    const StallResult* result;
    ProcessStatus process;
    ProtocolHealthSnapshot health;
    uint64_t bundle_generation;
};
struct BundlePath { char bytes[1024]; uint32_t size; };
BundleWriteResult WriteBundle(const char* absolute_output_dir,
                              const BundleInput&,
                              BundlePath* completed_path) noexcept;
```

- [ ] **Step 1: Add schema/durability/privacy tests**

Add exact tests named `WritesCompleteBundleAtomically`, `CompleteMarkerWireFormatIsExact`, `TimelineHeaderWireFormatIsExact`, `TimelineHeaderRejectsSizeCountAndChecksumMismatch`, `RejectsLiveOwnerTemporaryDirectory`, `RetainsYoungDeadOwnerTemporaryDirectory`, `CleansOldDeadOwnerTemporaryDirectory`, `RejectsReusedPidOwnerToken`, `ManifestPrecedesCompleteMarker`, `ManifestPreservesSuspectedAndConfirmedEvidence`, `ManifestPreservesAllLossOwners`, `RejectsInvalidProcessStatusCombination`, `StatusDecodeErrorPreservesLastCoherentEvidence`, `GpuJsonContainsOnlySanitizedFaultCounts`, and `AutomaticArtifactsExcludeCanaries`. Inject canary secrets into child argv, environment, fake paths, guest thread name, shader hash, raw log, a mock device-fault description, GPU addresses, vendor-info text/codes, and vendor-binary bytes. Recursively scan every automatic artifact byte-for-byte and assert none appears. Use an injected clock to prove cleanup requires a dead matching PID/start token and age of at least 24 hours; live, young, unreadable, or PID-reused owners remain untouched. Assert artifact checksums match, rename stays on one filesystem, first-suspected evidence survives later state mutation, and the manifest reports aggregate ring, unregistered-writer, inactive-token, registration-capacity, instance-capacity, skipped-publication, disconnect, and rejected-sample counters without merging their provenance. `RejectsInvalidProcessStatusCombination` feeds every invalid closed-schema combination and requires a structured validation error before any temporary directory is created. `progress.json` preserves each domain's capacity and rejected-update total/time separately and reports unavailable endpoints; `wait_graph.json` contains only the typed bounded resolved/unknown-producer facts. A status-decode-error bundle retains the prior progress/timeline/fault generations, records the raw platform status/error only as a numeric field, and contains no fabricated child exit code or crash classification. `gpu.json` emits only the fixed state/flags/results/time/session ID/counts from `GpuFaultSnapshot`. Add the public writer interfaces plus compile-only stubs that return an explicit I/O failure without creating files; the suite must build and fail success-path assertions.

- [ ] **Step 2: Prove the registered bundle tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/bundle.log"
ninja -C "$devtools/_build_linux_devtools" fc_script >"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsBundle\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  WritesCompleteBundleAtomically$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsBundle.WritesCompleteBundleAtomically' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsBundle\.WritesCompleteBundleAtomically' "$red_log"
```

- [ ] **Step 3: Implement a bounded JSON writer**

Write only compiled keys and allowlisted numeric/enumerated values. Thread roles are emulator-owned enums and synchronization object IDs are per-run opaque numbers; shader/pipeline identity is entirely deferred from version 1. Do not reuse `Core::Json`, which is reader-only and would link the heavy Core graph.

- [ ] **Step 4: Implement durable platform I/O**

POSIX uses explicit write loops, `fsync` each artifact, writes manifest then marker, `fsync`s the temp directory, renames, then `fsync`s the parent. Windows uses `WriteFile`, `FlushFileBuffers`, and `MoveFileExW`. No throwing filesystem overload is used.
Before creating a new temporary directory, scan only the tool's own fixed naming pattern. Cleanup requires a parseable owner PID/start token, proof that exact owner is dead, and age at least 24 hours from an injected/testable clock; otherwise return a structured retained/conflict result. Never recursively remove an arbitrary caller path.

- [ ] **Step 5: Run and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsBundle\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsBundle.*'
git -C "$devtools" diff --check
git -C "$devtools" add source/devtools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsBundle.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): write sanitized stall bundles'
```

### Task 9: Implement supervisor sampling and synthetic worker modes

**Files:**
- Create: `source/devtools/include/Kyty/DevTools/Supervisor/Supervisor.h`
- Create: `source/devtools/src/Supervisor.cpp`
- Create: `source/devtools/src/SyntheticWorker.cpp`
- Create: `source/devtools/src/Main.cpp`
- Create: `source/devtools/src/Measurement.cpp`
- Modify: `source/devtools/CMakeLists.txt`
- Modify: `source/devtools/include/Kyty/DevTools/Supervisor/ProcessLauncher.h`
- Modify: `source/devtools/src/ProcessLauncherPosix.cpp`
- Modify: `source/devtools/src/ProcessLauncherWindows.cpp`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsLifecycle.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`
- Create: `docs/devtools/runtime-stall-snapshot.md`

**Interfaces:**
- Consumes: launcher, protocol reader, classifier, bundle writer.
- Produces executable commands `kyty_devtools run [options] -- WORKER [ARG ...]`, `measure --recording=metrics-only|full --duration-s=N --output-dir DIR -- WORKER [ARG ...]`, `validate-metrics`, `compare-metrics`, `checkpoint-run --recording=metrics-only|full --checkpoint-output FILE --`, `new-invocation-id`, `tree-id --root DIR`, `attest-frontier --interactive --checkpoint FILE`, `verify-frontier --checkpoint FILE` with explicit binding and requirement switches, a bounded `self-test` harness, internal synthetic modes `progress`, `blocked-lane`, `publication-stop`, `parent-disconnect`, `privacy-canary`, `normal-exit`, `crash`, and:

```cpp
enum class SupervisorOutcome: uint8_t { ChildExited, ChildCrashed, ChildTerminated, StatusDecodeError, LaunchError, WorkerHandshakeFailed, ProtocolError, BundleError, UserInterrupted };
struct SupervisorResult {
    SupervisorOutcome outcome;
    ProcessStatus process;
};
struct SupervisorOptions {
    const char* absolute_output_dir;
    const char* worker;
    const char* const* worker_argv;
    uint32_t worker_argc;
    RecordingMode mode;
    uint64_t sample_period_ns;
    uint64_t suspicion_ns;
    uint64_t confirmation_ns;
};
SupervisorResult RunSupervisor(const SupervisorOptions&) noexcept;
SupervisorResult RunMeasurement(const SupervisorOptions&,
                                uint64_t duration_ns) noexcept;
int RunSelfTest(const char* synthetic_mode,
                const char* absolute_output_dir) noexcept;
```

The library returns `SupervisorResult`. `run`, `measure`, and `self-test` require an explicit absolute `--output-dir`; there is no current-working-directory default. Production `run` returns the proven normal exit code, conventional `128 + signal` for a POSIX signal, `125` for a supervisor-owned failure, and an opaque terminal Windows DWORD subject to the invoking shell's documented truncation; the complete typed status remains in the sanitized manifest. A Windows DWORD is reported as `ChildTerminated`, never inferred to be `ChildExited` or `ChildCrashed`. `self-test` returns zero only when its own selected synthetic mode produced the platform-specific expected outcome: the POSIX `crash` mode must decode `Signal/SIGABRT`, while the Windows fail-fast mode is accepted only as the exact opaque status emitted by that controlled synthetic binary and is not generalized to arbitrary workers. `measure` is explicitly duration-bounded: it forwards an orderly interrupt at the deadline, allows a fixed 10-second cleanup window, marks forced cleanup invalid, and writes sanitized process CPU plus the fixed measurement snapshot. It never serves as production stall recovery.

`verify-frontier` validates a sanitized, untracked schema-1 checkpoint containing
a fresh 128-bit invocation ID, exact Git tree ID, recording mode, child
PID/start token, observed process status, forced-cleanup boolean,
strict/diagnostic flags, first-failure category, frame/completed-flip counts,
visual comparison (`Equivalent`, `Later`, or `Earlier`), manual-review boolean,
and capture start/end monotonic times. It rejects fixture paths,
identifiers, argv, logs, screenshots, and guest data. The combined requirement
switches used by provider gates succeed only when strict is true, every
behavior-changing flag is false, counts are nonzero, manual review is true,
visual comparison is Equivalent/Later, and no earlier EXIT, device loss, or
relevant validation failure is recorded. `attest-frontier --interactive` binds
manual review to the file produced for that invocation; verification requires
the expected invocation/tree/mode, matching child identity, freshness, and
`forced_cleanup=false`. Status 124 alone is never acceptance; malformed,
replayed, stale, or privacy-unsafe checkpoint fields fail closed.
`tree-id` hashes relative path, mode, and content for tracked plus untracked
implementation files under the explicit source/CMake/docs allowlist, including
dirty pre-commit changes; it rejects symlinks escaping the root and excludes
build, scratch, fixture, dump, shader, log, and attachment paths. Thus a later
edit cannot reuse an earlier checkpoint even before the commit exists.

`checkpoint-run` is the sole process owner for provider gates. Before launch it
durably creates a seed containing invocation/tree/mode and `forced_cleanup=false`;
after launch it adds the queried child PID/start token before allowing the
worker handshake. `recording=metrics-only` launches `fc_script` with only the
fixed counts/strict-facts measurement channel and no ordinary timeline or
progress providers; it is the behavior-neutral semantic baseline. `full`
enables the normal supervised transport. At the duration boundary it sends one orderly interrupt,
waits ten seconds, and records whether escalation was required before its sole
wait/finalization. Exit 124 means an orderly duration boundary, 125 means
forced cleanup or wrapper failure. It writes counts/strict flags/process result
to that same invocation checkpoint and never accepts a pre-existing file.
`attest-frontier` may fill only manual-review and visual-comparison fields; it
cannot alter identity, mode, cleanup, counters, or strict facts.

- [ ] **Step 1: Add deterministic orchestration tests**

Use a fake clock/process launcher for 250 ms samples, 5 s suspicion, and 15 s confirmation with no sleeps. Add exact tests named `LiveStallCapturesOnceAndContinues`, `CheckpointRunOwnsIdentityAndCleanup`, `AttestationCannotMutateSeed`, `TreeIdChangesWithDirtyImplementation`, `RejectsMissingOrRelativeOutputDirectory`, `RejectsCaptureNowAndAttachCommands`, `NormalExitFinalizesOnce`, `CrashFinalizesOnce`, `OpaqueTerminalFinalizesWithoutCrashClaim`, `StatusDecodeErrorFinalizesFromLastSnapshot`, `HandshakeFailureHasDistinctOutcome`, `HandshakeTimeoutLeavesNoOrphanOrDoubleWait`, `MeasureCollectsCpuAndHistogram`, `MeasureRejectsForcedCleanup`, `MetricsJsonIsAllowlistOnly`, and `CompareMetricsUsesRunVariation`. Verify notification acknowledgement is parent-local, `capture-now`/attach are rejected by the parser, and no worker command exists. A decode error after one valid publication writes one bundle from that immutable copy, never calls `Observe` with a terminal process state, finalizes once, and returns `StatusDecodeError`; an opaque termination returns `ChildTerminated` and a `ProcessTerminated` bundle without exit/crash wording. On handshake timeout, close diagnostic ownership but retain the process owner, forward explicit signals, perform exactly one wait, and return `WorkerHandshakeFailed` only after the synthetic child exits; no production auto-kill or live orphan is allowed. The metrics comparison uses two MetricsOnly and two Full summaries, rejects schema/mode/duration mismatch, and blocks only when Full regression exceeds both 3% and baseline run-to-run variation. Inject canaries into fake argv/environment/path/log inputs and assert `Measurement.cpp` writes only fixed schema/mode/duration/process-CPU/histogram fields to `metrics.json`. Add the supervisor/measurement interfaces, CLI parser, and compile-only `Run`/synthetic/measure stubs that return `ProtocolError`; register the lifecycle suite so its success paths execute and fail.

- [ ] **Step 2: Prove lifecycle orchestration tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/lifecycle.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script kyty_devtools >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsLifecycle\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  LiveStallCapturesOnceAndContinues$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsLifecycle.LiveStallCapturesOnceAndContinues' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsLifecycle\.LiveStallCapturesOnceAndContinues' "$red_log"
```

- [ ] **Step 3: Implement the parent loop**

Create mapping/nonce plus a one-way parent-liveness primitive, launch child, validate bounded handshake, sample coherent sections, poll the OS child handle, derive `WaitGraphSnapshot` from the same validated `ProgressPublication`, and compose `ObservationInput.loss` from its `WriterLossSnapshot`/`ProgressLossSnapshot` plus `ProtocolHealthSnapshot` (including the parent reader's rejected-sample state). Classify, emit a bundle once, and print its sanitized path. A skipped-publication or rejected-sample delta inside the decisive interval must reach the classifier and bundle; it cannot disappear between the wire reader and `Observe`. After a live stall bundle, keep sampling the production worker. After a proven exit, proven crash, or opaque termination, finalize exactly once, close mapping/owner resources, and return the matching `SupervisorResult`; never remain alive without the child. If `ProcessStatus.error != None`, bypass terminal classification, retain the last validated progress/timeline/measurement/fault publication, write one decode-error bundle, close resources, and return `StatusDecodeError` with raw platform status/error and no invented child code. On bounded handshake timeout, record the outcome and close diagnostics, but keep owning/observing the child until its one terminal wait; production `run` never returns a live orphan or terminates it. Ctrl+C forwarding is explicit user termination, not stall recovery. POSIX parent liveness uses an inherited read end whose peer closes with the supervisor; Windows uses the inherited supervisor process/synchronization handle already restricted by the launch handle list. This is observation only, not a control queue. The measurement path consumes the cached `ProcessUsage` returned with the one terminal `ProcessObservation` from Task 7 and combines it with `ProgressPublication.measurement`; it never performs a second `wait4`, queries a closed Windows handle, performs a separate mapping read, parses Console text, or reads window titles. An unavailable usage or measurement invalidates the run instead of silently reusing prior values. `Measurement.cpp` serializes from a fixed allowlist and cannot access the child argv, environment, raw logs, or working path.

- [ ] **Step 4: Add synthetic executable modes**

`blocked-lane` keeps publisher heartbeat alive while one lane stops; `publication-stop` leaves the process alive after all publications stop; `normal-exit` returns zero; `crash` raises `SIGABRT` on POSIX or invokes a fixed fail-fast path on Windows. The former proves `ChildCrashed`; the latter proves only the controlled mode's exact `ChildTerminated/OpaquePlatformStatus`. `parent-disconnect` makes the worker observe liveness loss nonblockingly, increment one disconnect transition counter, disable further transport writes, and continue its synthetic workload. `privacy-canary` places one caller-supplied value into synthetic argv/environment/fake-log/path/guest-name/shader-hash inputs while expecting it in no automatic artifact. Add a deterministic synthetic measurement producer with known bucket/count values for `validate-metrics`/`compare-metrics` tests. `self-test` owns an explicit cleanup deadline and terminates only its synthetic live child after asserting the expected capture; production `run` never does so automatically.

Document the exact `run`, `measure`, `validate-metrics`, `compare-metrics`, `checkpoint-run --recording=metrics-only|full --checkpoint-output FILE --`, `new-invocation-id`, `tree-id --root DIR`, `attest-frontier --interactive --checkpoint FILE`, `verify-frontier --checkpoint FILE` with explicit binding and requirement switches, `self-test`, output-directory, privacy, exit-status, and controlled-restart workflow in `docs/devtools/runtime-stall-snapshot.md`. State explicitly that `capture-now`, arbitrary C++/shader hot reload, and attach/control are not v1 features.

- [ ] **Step 5: Build, run synthetic modes, and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsLifecycle\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsLifecycle.*:DevToolsSupervisor.*:DevToolsClassifier.*'
"$devtools/_build_linux_devtools/devtools/kyty_devtools" self-test --output-dir "$bundle_dir" --mode=blocked-lane --suspected-ms=20 --confirmed-ms=50 --cleanup-ms=2000
git -C "$devtools" diff --check
git -C "$devtools" add source/devtools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsLifecycle.cpp source/unit_test/src/UnitTest.cpp docs/devtools/runtime-stall-snapshot.md
git -C "$devtools" commit -m 'feat(devtools): capture supervised worker stalls'
```

Expected: synthetic bundle is complete and classifier names the blocked lane without terminating it.

### Task 9A: Establish concurrency and overhead gates before emulator instrumentation

**Files:**
- Create: `source/devtools/src/Benchmark.cpp`
- Create: `source/devtools/tests/StressEventRing.cpp`
- Modify: `source/devtools/CMakeLists.txt`
- Modify: `source/devtools/src/Main.cpp`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsBenchmark.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: core ring/protocol and the supervisor CLI from Task 9.
- Produces `kyty_devtools benchmark --mode=disabled|enabled
  --warmup-batches=W --batches=N --events-per-batch=E` and optional
  `kyty_devtools_stress` built with ThreadSanitizer on supported Linux
  toolchains.

- [ ] **Step 1: Add the benchmark output contract**

Emit one structured line containing mode, warmup-batch count, measured-batch count, events per batch, total events, median and nearest-rank p95 ns/event, median events/s, aggregate loss count, and allocation count. Require at least 5 measured batches; the acceptance command uses 31. Disabled and enabled paths execute identical warmup and measured loops; only the record operation changes. `NearestRank95` sorts a fixed preallocated sample array and selects `ceil(0.95*N)-1`; median uses the middle sample for odd N and overflow-safe average for even N. Register `DevToolsBenchmark.BenchmarkQuantilesUseFixedSamples` and `DevToolsBenchmark.RejectsInvalidBatchContract` before implementation, with exact known samples and no wall-clock assertion. In the DevTools executable only, define a process-global `operator new/delete` counter guarded by an atomic measurement flag. Warm up all CLI/output state and the requested warmup batches, reset the counter, enable it only around all measured loops, then disable it before sorting/formatting. The counter itself performs no allocation. The enabled hot path must report zero allocations and the no-overflow case must report zero loss. Output contains no machine path, CPU name, argv, or environment.

- [ ] **Step 2: Add the deterministic stress target**

Stress producer/consumer full/drop/reuse/close, registration exhaustion, invalid-token loss, atomic maximum loss time, and shared-snapshot pin/publication with deterministic barriers. `KYTY_DEVTOOLS_BUILD_TSAN_STRESS` defaults OFF and builds only the small DevTools targets with `-fsanitize=thread` on supported Clang/GCC Linux. Link the stress executable to `Threads::Threads`; do not add any dependency to `kyty_devtools_core` itself.

- [ ] **Step 3: Run the pre-provider gate and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKYTY_DEVTOOLS_BUILD_TSAN_STRESS=ON
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsBenchmark\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  BenchmarkQuantilesUseFixedSamples$' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsBenchmark.*'
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
"$devtools/_build_linux_devtools/devtools/kyty_devtools_stress"
git -C "$devtools" diff --check
git -C "$devtools" add source/devtools source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsBenchmark.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'test(devtools): gate telemetry overhead'
```

Expected: TSan reports no race, enabled recording allocates nothing, and the benchmark becomes available for every following provider commit. The final workload-level 3% gate remains in Task 15.

### Task 10: Add the emulator runtime adapter and publisher

**Files:**
- Create: `source/emulator/include/Emulator/DevTools/Runtime.h`
- Create: `source/emulator/include/Emulator/DevTools/RuntimeSubsystem.h`
- Create: `source/emulator/include/Emulator/DevTools/Providers.h`
- Create: `source/emulator/src/DevTools/Runtime.cpp`
- Create: `source/emulator/src/DevTools/RuntimeSubsystem.cpp`
- Create: `source/emulator/src/DevTools/SharedMemoryTransportPosix.cpp`
- Create: `source/emulator/src/DevTools/SharedMemoryTransportWindows.cpp`
- Create: `source/emulator/src/DevTools/CurrentProcessIdentityLinux.cpp`
- Create: `source/emulator/src/DevTools/CurrentProcessIdentityMacos.cpp`
- Create: `source/emulator/src/DevTools/CurrentProcessIdentityWindows.cpp`
- Create: `source/emulator/src/DevTools/PublisherThreadPosix.cpp`
- Create: `source/emulator/src/DevTools/PublisherThreadWindows.cpp`
- Create: `source/emulator/include/Emulator/DevTools/PublisherThread.h`
- Modify: `source/emulator/CMakeLists.txt`
- Modify: `source/emulator/src/Kyty.cpp`
- Modify: `source/KytyScripts.cpp`
- Modify: `source/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsRuntime.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: inherited transport metadata and core registries.
- Produces:

```cpp
namespace Kyty::Emulator::DevTools {
enum class ProviderMode: uint8_t { Disabled = 0, MetricsOnly = 1, Full = 2 };
enum class EarlyBootstrapResult: uint8_t {
    Standalone = 0, Prepared = 1, Malformed = 2, TransportRejected = 3
};
class ProviderEnableGate {
public:
    ProviderMode Mode() const noexcept;
    bool AllowsMeasurement() const noexcept;
    bool AllowsFullTelemetry() const noexcept;
};
struct ThreadWriterReservation {
    uint64_t instance;
    Kyty::DevTools::ThreadRole role;
    Kyty::DevTools::TelemetryWriterToken token;
};
EarlyBootstrapResult PrepareInheritedTransportEarly() noexcept;
bool InitializeFromPreparedTransport() noexcept;
void Shutdown() noexcept;
bool ReserveThread(Kyty::DevTools::ThreadRole, ThreadWriterReservation*) noexcept;
bool ActivateThread(const ThreadWriterReservation&, uint64_t monotonic_ns) noexcept;
void AbandonThread(const ThreadWriterReservation&) noexcept;
void CloseThread(const ThreadWriterReservation&, Kyty::DevTools::ResultCategory,
                 uint64_t monotonic_ns) noexcept;
void Record(const Kyty::DevTools::TelemetryWriterToken* optional_token,
            Kyty::DevTools::EventRecord) noexcept;
Kyty::DevTools::ProgressRegistry& GetProgressRegistry() noexcept;
Kyty::DevTools::MeasurementRegistry& GetMeasurementRegistry() noexcept;
Kyty::DevTools::GpuFaultRegistry& GetGpuFaultRegistry() noexcept;
const ProviderEnableGate& GetProviderEnableGate() noexcept;
}
```

- [ ] **Step 1: Add disabled/invalid/valid transport tests**

`BootstrapScrubPrecedesAnySubsystem` invokes the real fc_script entry seam with
recording fake subsystem constructors. It requires
`PrepareInheritedTransportEarly` to parse, erase, open, and nonce/identity-
validate a well-formed bootstrap before `SubsystemsList`, Scripts, or Emulator
initialization; malformed input returns `Malformed`, opens no handle, and also
precedes every subsystem. `WorkerUsesCoreBootstrapAndCurrentIdentity` proves the
parser/value contract comes from `kyty_devtools_core`, and
`EmulatorRuntimeDoesNotLinkSupervisor` checks the configured target graph.

Add exact tests named `ValidTransportPublishesAndShutsDown`, `MetricsOnlyPublishesNoTimelineOrProgressOnShutdown`, `StandaloneModeIsDisabledWithoutError`, `InvalidHandshakeIsRejected`, `AdapterPreservesWriterLifecycle`, `ThreadLifecycleEventsAreOwnedExactlyOnce`, `ThreadRolesUseCoreAbiValues`, `ThreadInstanceAllocatorStartsAtOneAndNeverReuses`, `ThreadInstanceAllocatorRejectsProgressRefOverflow`, `MissingTokenAndInactiveTokenLossStayDistinct`, `BuildDescriptorMapsLoggingAndCacheExactly`, `WorkerUsesCoreBootstrapAndCurrentIdentity`, `EmulatorRuntimeDoesNotLinkSupervisor`, `RuntimeInitializesBeforePthreadAndGraphics`, `RuntimeOutlivesGraphicsDuringReverseShutdown`, `DestroyAllFinalizesRuntimeOnce`, `ShutdownAllFinalizesRuntimeOnce`, `DestroyThenAtexitShutdownIsIdempotent`, `ParentDisconnectDisablesTransportOnce`, `ShutdownAfterParentDisconnectJoinsWithoutTransportWrite`, `StartFailureCleanupIsIdempotent`, `ShutdownWhileInactiveBufferPinnedDoesNotBlock`, and `ShutdownWithActiveNonJoinableProvider`. A valid synthetic mapping starts one publisher, writes handshake/build info, advances heartbeat, and shuts down cleanly. Full mode reserves/emits the publisher timeline-writer lifecycle and protocol heartbeat; it has no progress endpoint; MetricsOnly starts the same publication/heartbeat thread but reserves no writer token, emits no timeline record even at shutdown, and publishes only a valid measurement-bearing progress generation with zero progress items. Intentional mode gating creates no loss. The adapter consumes the Task 1 `ThreadRole` directly: `MainGuest=1` through `SnapshotPublisher=11`, with no shifted duplicate enum. A process-lifetime monotonic allocator owned by the runtime creates diagnostic thread instances before any native create, starts at 1, is bounded to the 56-bit `ProgressRef` instance field, never derives identity from a pointer, host TID, or the later guest `unique_id`, and never reuses an instance after abandon/failure. Refusing the exhausted namespace disables telemetry for that new thread without failing guest creation. The allocator has injected fresh and near-limit states in unit tests. `ThreadWriterReservation` preserves the allocator's instance, role, and token through reserve, activate, close, or abandon. `ActivateThread(reservation, now)` is the sole owner of exactly one `ThreadStart`; `CloseThread(reservation, result, now)` passes exactly one `ThreadExit` directly to `WriterRegistry::Close`; `AbandonThread` emits neither. A null token increments only `unregistered_writers`; a supplied stale/inactive token increments only `inactive_writer_attempts`; calls gated by Disabled/MetricsOnly are intentional no-ops and increment neither. Reserve/activate, reserve/abandon, active/close/drain, stale-token, and native-create-failure paths preserve the Task 2 lifecycle through the adapter. The four registry/gate accessors return the one process-lifetime instances owned by Runtime; provider tests compare addresses so Graphics/Window/publisher cannot create private copies. Worker bootstrap parsing/erasure and normalized identity types come from `kyty_devtools_core`; only the worker-side platform adapter queries its current process. A CMake dependency test inspects the configured target graph and fails if `emulator` or `kyty_devtools_core` links `kyty_devtools_supervisor`. Build-info publication maps `Log::Direction` explicitly to `Unknown=0/Silent=1/Console=2/File=3/Directory=4` and reports current shader-cache state as `NoPersistentCache=1`, never from enum order or shader-dump directory presence. Build a real `Core::SubsystemsList` with recording fake dependencies plus `RuntimeDiagnosticsSubsystem` and assert initialization precedes Pthread/Graphics while reverse shutdown keeps diagnostics alive until both have shut down. Exercise `DestroyAll` and `ShutdownAll` separately; `Destroy` and `UnexpectedShutdown` call the same idempotent finalizer exactly once. Then model a normal destroy followed by the atexit shutdown and require the second call to be a no-op, covering ordinary teardown plus the existing kyty_close/DbgAssert/Window `_Exit` paths without invoking `_Exit` in a unit test. Simulate parent-liveness loss and assert one disconnect counter transition, no later section/mapping write after its dedicated control store, no retry loop, and continued emulator-side work. Then call orderly `Shutdown`: it joins the already exited publisher without a final mapping write or `WorkerClosing` transition. Handshake/start failure leaves no active token or joinable thread and every cleanup call is idempotent. Final connected publication is bounded: if the inactive buffer remains pinned, increment skipped once, preserve the last generation, publish `WorkerClosing`, close/drain the publisher reservation locally, and join without waiting. The active-provider shutdown test leaves a synthetic detached-provider token active, disables the runtime, calls record/close afterward, and proves bounded no-op behavior with no freed storage access. Add the public adapter/subsystem/thread declarations plus compile stubs that remain disabled and reject `PublisherThread::Start`; register the suite so valid-transport/lifecycle expectations execute and fail.

- [ ] **Step 2: Prove runtime-adapter tests are red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red"
red_log="$scratch/red/runtime-adapter.log"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$red_log" 2>&1
ninja -C "$devtools/_build_linux_devtools" fc_script >>"$red_log" 2>&1
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsRuntime\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ValidTransportPublishesAndShutsDown$' <<<"$listed_tests"; then exit 1; fi
set +e
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsRuntime.ValidTransportPublishesAndShutsDown' >>"$red_log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsRuntime\.ValidTransportPublishesAndShutsDown' "$red_log"
```

- [ ] **Step 3: Implement initialization at the emulator subsystem boundary**

At the start of `fc_script` main, before `SubsystemsList::Instance()`, Scripts,
or Emulator initialization, call `PrepareInheritedTransportEarly`. It uses the
core parser, erases `KYTY_DEVTOOLS_BOOTSTRAP_V1` immediately, opens only a
well-formed inherited transport, validates mapping nonce plus current process
identity, and stores a move-only pending transport in process-lifetime runtime
storage. `Standalone` continues normally; `Malformed` or `TransportRejected`
prints one sanitized numeric initialization error and returns failure before
scripts can observe the environment. `RuntimeDiagnosticsSubsystem::Init` later
calls `InitializeFromPreparedTransport` to publish the handshake/start provider
threads; it never reparses the environment or opens a second handle. Build-tools
configuration compiles an explicit standalone no-op branch and never links the
supervisor.

Implement `RuntimeDiagnosticsSubsystem` as the only emulator lifecycle owner. In `source/emulator/src/Kyty.cpp`, add it before the second `InitAll`: it depends only on already initialized Core/Config, Pthread depends on it, and Graphics depends on it in addition to its current dependencies. This makes diagnostics active before any instrumented Pthread/Graphics worker is created and, because `SubsystemsList` shuts down in reverse initialization order, keeps diagnostics alive until those producers finish their subsystem shutdown. Both `Destroy` and `UnexpectedShutdown` call the same idempotent `Shutdown`; the existing `kyty_close` atexit path and `WindowRun -> ShutdownAll() -> _Exit` path therefore reach the final publisher flush without relying on C++ destructors. Do not add a second direct atexit hook or call shutdown after `_Exit`.

Implement a narrow platform-boundary `PublisherThread` whose POSIX `pthread_create` and Windows `_beginthreadex`/handle paths return an explicit start result and expose orderly join; it carries no emulator policy. In Full mode, allocate/reserve the publisher's `ThreadWriterReservation` before `PublisherThread::Start`, activate that same reservation at thread entry, abandon only when that explicit start call fails, and close it at exit. In MetricsOnly, start the publisher without any writer/progress reservation; it publishes heartbeat and measurement but no event/progress item. The publisher owns `TimelineHistory`, fairly drains every Full-mode writer ring into it, publishes the retained 4,096-event window even on later empty cycles, copies local ring, unregistered-writer, registration-capacity, instance-capacity, skipped-publication, disconnect, and inactive-token totals into the seven child-owned wire controls, and copies one coherent `ProgressPublication` (including separate per-domain loss reasons, measurement, and device-fault snapshots) at 250 ms. It performs every mapping write outside emulation hot paths. It checks the inherited parent-liveness primitive nonblockingly; the first disconnect increments local state, stores that total/time once to the dedicated disconnect control while the mapping is still valid, atomically disables external provider recording and all further transport publication, closes transport without retry, closes/drains its optional local writer reservation, and returns from the publisher entry point while emulation continues. It never recreates the mapping or signals the worker. Standalone `fc_script` never waits for it.

Every runtime-owned activation supplies `MonotonicNowNs()` to
`ActivateThread`; every orderly exit supplies its actual `ResultCategory` and
time to `CloseThread`. No caller emits a separate lifecycle record around those
APIs. Publisher disconnect/close likewise uses this single path before local
drain; an abandoned reservation records no start or exit.

Recorder registries, the thread-instance allocator, and provider-enabled state use process-lifetime storage. On an orderly connected Full path, `Shutdown` first disables external providers, then requests publisher stop without invalidating transport. The publisher drains external records, assigns and records its own exit, transitions its reservation to `Closing`, self-drains that final record to `Free`, then attempts one final coherent snapshot whose timeline includes the publisher exit. If the inactive buffer is pinned, it does not wait or retry indefinitely: it increments/stores skipped-publication once and preserves the prior active generation. It then publishes `WorkerClosing`, disables transport, returns, and `Shutdown` joins it. MetricsOnly follows the same bounded final publication but has no publisher exit event/reservation and its timeline/progress count remains zero. After parent disconnect, the publisher is already joinably exited with transport invalid: later `Shutdown` only requests stop idempotently and joins, and must not publish another snapshot or `WorkerClosing`. If handshake validation or thread start failed, `Shutdown` sees no joinable publisher and no active reservation and remains idempotent. Registries are not destroyed while detached LabelManager or Window/Flip paths might still reference them, and every later provider call is a bounded no-op. Process teardown reclaims that storage. `ValidTransportPublishesAndShutsDown` asserts the connected Full order, while MetricsOnly, pinned-buffer, disconnect, and start-failure tests assert their distinct cleanup orders and prove no publisher slot remains `Active` after join.

- [ ] **Step 4: Enumerate adapter sources explicitly**

Use `target_sources(emulator PRIVATE ...)`, link `kyty_devtools_core` with the existing non-keyword `target_link_libraries` signature, and depend on `KytyBuildInfo`. Select POSIX/Windows transport source in CMake, not guest code.

- [ ] **Step 5: Build, test enabled/disabled, and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsRuntime\.' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsRuntime.*:DevToolsEventRing.*:DevToolsProtocol.*'
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() {
  invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id)
  tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools")
  checkpoint="$scratch/frontier-$invocation_id.json"
  set +e
  "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run \
    --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 \
    --cleanup-s=10 --checkpoint-output "$checkpoint" \
    --invocation "$invocation_id" --tree "$tree_id" \
    --output-dir "$bundle_dir" -- "$@"
  status=$?
  set -e
  test "$status" -eq 0 || test "$status" -eq 124
  "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier \
    --interactive --checkpoint "$checkpoint"
  "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier \
    --checkpoint "$checkpoint" --expected-invocation "$invocation_id" \
    --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" \
    --require-fresh --require-strict --reject-forced-cleanup \
    --require-frames --require-flips --require-visual-equivalence \
    --require-no-earlier-exit
}
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/DevTools source/emulator/src/DevTools source/emulator/CMakeLists.txt source/emulator/src/Kyty.cpp source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsRuntime.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): publish emulator snapshots'
```

### Task 11A: Instrument guest-thread lifecycle

**Files:**
- Modify: `source/emulator/include/Emulator/Kernel/Pthread.h`
- Modify: `source/emulator/src/Kernel/Pthread.cpp`
- Modify: `source/emulator/src/Loader/RuntimeLinker.cpp`
- Modify: `source/emulator/include/Emulator/DevTools/Providers.h`
- Create: `source/emulator/src/DevTools/Providers/KernelProvider.cpp`
- Modify: `source/emulator/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsKernelProvider.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: `TelemetryWriterToken`, actual pthread creation/cleanup paths, and guest-entry boundaries.
- Produces reserve-before-create, one matched GuestThread progress endpoint/ref,
  activate-at-entry, abandon/close-on-create-failure, close-on-real-exit, and
  bounded guest entry/return events. It does not claim a central scheduler,
  arbitrary RIP, or universal HLE coverage.

```cpp
namespace Kyty::Emulator::DevTools {
struct GuestThreadTelemetryContext {
    ThreadWriterReservation writer;
    Kyty::DevTools::ProgressToken progress;
    uint64_t progress_ref;
    uint64_t epoch;
    uint64_t boundary_ordinal;
    bool valid;
};
bool ReserveGuestThread(Kyty::DevTools::ThreadRole, GuestThreadTelemetryContext*) noexcept;
bool ActivateGuestThread(GuestThreadTelemetryContext*, uint64_t monotonic_ns) noexcept;
bool AdvanceGuestThread(GuestThreadTelemetryContext*, Kyty::DevTools::ProgressState,
                        uint64_t monotonic_ns) noexcept;
void AbandonGuestThread(GuestThreadTelemetryContext*, uint64_t monotonic_ns) noexcept;
void CloseGuestThread(GuestThreadTelemetryContext*, Kyty::DevTools::ResultCategory,
                      uint64_t monotonic_ns) noexcept;
bool AdvanceCurrentGuestThread(Kyty::DevTools::ProgressState,
                               uint64_t monotonic_ns) noexcept;
uint64_t CurrentGuestThreadProgressRef() noexcept;
bool RecordCurrentGuestThread(Kyty::DevTools::EventRecord) noexcept;
}
```

- [ ] **Step 1: Register lifecycle tests against compile-only provider stubs**

Add `UT_LINK(DevToolsKernelProvider)` and explicitly add `UnitTestDevToolsKernelProvider.cpp` to the unit-test target. Declare the narrow lifecycle provider API in `Providers.h` and add compile-only no-op/invalid-result definitions in `KernelProvider.cpp`; the stubs exist only to make the new tests link and must not touch `Pthread.cpp` or `RuntimeLinker.cpp`. Add exact tests named `ReservationActivatesOnce`, `ReservationAndActivationKeepThreadInstance`, `GuestThreadProgressLifecycleUsesOneEndpoint`, `GuestThreadBoundaryAdvancesWithoutChangingEpoch`, `AbandonedReservationCannotActivate`, `FailedCreateDoesNotReuseThreadInstance`, `StaleTokenRejected`, `MainThreadOrderPreserved`, `NativeCreateFailureAbandonsReservation`, and `ExitClosesWriterAndProgress`. The lifecycle tests require the writer instance, progress key instance, and packed ref instance to match; register starts Idle `0/0`, activation submits epoch 1 `ExecutionBoundary/Active` and emits exactly one ThreadStart, boundary/wait transitions only advance epoch 1, exit completes then closes it and emits exactly one ThreadExit, and native-create failure closes Idle plus abandons the writer with zero lifecycle events. `MainThreadOrderPreserved` asserts start, module/entry/return boundaries, and exit exactly once; the same counts are asserted for a guest pthread and publisher. Do not implement HLE/wait instrumentation yet.

- [ ] **Step 2: Prove the lifecycle test is red for the intended missing behavior**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsKernelProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  ReservationActivatesOnce$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsKernelProvider.ReservationActivatesOnce' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsKernelProvider\.ReservationActivatesOnce' <<<"$red_output"
```

The build and registration must succeed. A compiler/linker failure or failure from any other test is not an acceptable red gate.

- [ ] **Step 3: Bind the exact lifecycle to `PthreadPrivate`**

Replace the compile-only provider behavior with the minimum lifecycle implementation. Add one private `GuestThreadTelemetryContext` to `PthreadPrivate` in `Pthread.cpp`; it contains the stable writer reservation, GuestThread `ProgressToken`, packed GuestThread `ProgressRef`, fixed epoch 1, boundary ordinal, and validity bit. `Pthread.h` exposes only the two narrow current-thread helper declarations above, backed by the existing `g_pthread_self`; they perform no allocation, registration, ID lookup, or lock acquisition and change no guest-visible struct/ABI. `ReserveGuestThread` first allocates/reserves the diagnostic instance and writer, then registers `ProgressKey {GuestThread, same_instance}`; a progress-registration failure abandons the writer, leaves the context invalid, and never changes guest create behavior. `PthreadCreate` completes that reservation before native `pthread_create`; `run_thread` calls `ActivateGuestThread`, which calls the runtime's sole `ActivateThread(reservation, now)` start emitter and submits epoch 1 `ExecutionBoundary/Active` immediately on entry, independently of the later guest `unique_id` assignment. Guest/HLE boundaries and explicit-wait entry/exit call `AdvanceCurrentGuestThread`, which keeps operation/epoch/correlation fixed, increments the boundary ordinal, and selects only `Active` or `Waiting`. `cleanup_thread` completes epoch 1, closes the progress endpoint, and calls the runtime's sole `CloseThread(reservation, result, now)` exit emitter. Every attribute-copy or native-create failure closes the still-idle endpoint and abandons the matching writer with no lifecycle event; the consumed diagnostic instance is never reused. `PthreadInitSelfForMainThread` reserves and activates a fresh diagnostic context after its guest unique ID is initialized, and the existing main-thread cleanup owner closes it. No new guest `thread_local` is introduced. `RuntimeLinker::Execute` emits only module-start and guest entry/return boundaries through `AdvanceCurrentGuestThread`; it never emits a second main-thread start or exit. HLE and wait providers obtain the owner only from `CurrentGuestThreadProgressRef()` and use the current-thread advance helper; they do not mutate the context directly or reconstruct identity from submit correlations.

- [ ] **Step 4: Run the per-provider gate and commit lifecycle only**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsKernelProvider\.' <<<"$listed_tests"; then exit 1; fi
test_filter='DevToolsKernelProvider.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/Kernel/Pthread.h source/emulator/src/Kernel/Pthread.cpp source/emulator/src/Loader/RuntimeLinker.cpp source/emulator/include/Emulator/DevTools/Providers.h source/emulator/src/DevTools/Providers/KernelProvider.cpp source/emulator/CMakeLists.txt source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsKernelProvider.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): trace guest thread lifecycle'
```

### Task 11B: Instrument allowlisted HLE calls and explicit waits

**Files:**
- Modify: `source/emulator/include/Emulator/DevTools/Providers.h`
- Modify: `source/emulator/src/DevTools/Providers/KernelProvider.cpp`
- Create: `source/emulator/src/DevTools/Providers/HleProvider.cpp`
- Modify: `source/emulator/src/Kernel/EventFlag.cpp`
- Modify: `source/emulator/src/Kernel/Semaphore.cpp`
- Modify: `source/emulator/src/Kernel/EventQueue.cpp`
- Modify: `source/emulator/CMakeLists.txt`
- Modify: `source/unit_test/src/devtools/UnitTestDevToolsKernelProvider.cpp`

**Interfaces:**
- Consumes: the committed lifecycle provider, explicit blocking HLE exports, and real wait contracts.
- Produces allowlisted HLE entry/return, wait begin/end, timeout/cancel/signal outcome, and an optional validated `known_producer_ref` without changing synchronization semantics.

```cpp
namespace Kyty::Emulator::DevTools {
enum class ProviderRecordResult: uint8_t {
    Recorded = 0, Disabled = 1, CapacityLost = 2, InvalidArgument = 3
};
enum class WaitObjectKind: uint8_t {
    Unknown = 0, EventFlag = 1, Semaphore = 2, EventQueue = 3, Count = 4
};
enum class WaitObservationFlag: uint16_t {
    KnownProducer = 0x01, Timed = 0x02
};
struct OpaqueObjectToken {
    uint16_t slot;
    uint16_t kind;
    uint32_t generation;
    uint64_t id;
};
struct HleCallScope {
    Kyty::DevTools::ProgressToken progress;
    uint64_t invocation_id;
    uint64_t hle_ref;
    uint64_t owner_ref;
    uint32_t depth;
    Kyty::DevTools::HleCallKind kind;
    bool active;
};
struct WaitObservation {
    Kyty::DevTools::OperationCode operation;
    uint16_t flags;
    uint16_t reserved;
    uint32_t reserved2;
    uint64_t object_id;
    uint64_t owner_ref;
    uint64_t producer_ref;
    uint64_t deadline_ns;
    uint64_t predicate;
    uint64_t mask;
};
struct WaitScope {
    Kyty::DevTools::ProgressToken progress;
    uint64_t wait_ref;
    uint64_t epoch;
    uint64_t owner_ref;
    uint64_t object_id;
    bool active;
};
ProviderRecordResult BeginHleCall(Kyty::DevTools::HleCallKind kind,
                                  uint64_t monotonic_ns, HleCallScope* out) noexcept;
void CompleteHleCall(HleCallScope*, Kyty::DevTools::ResultCategory,
                     uint64_t monotonic_ns) noexcept;
bool ReserveWaitObject(WaitObjectKind, OpaqueObjectToken*) noexcept;
void RetireWaitObject(OpaqueObjectToken) noexcept;
uint64_t WaitObjectId(OpaqueObjectToken) noexcept;
void RecordWaitObjectSignal(OpaqueObjectToken, uint64_t caller_ref,
                            uint64_t monotonic_ns) noexcept;
bool AllocateWaitObservationId(uint64_t* out) noexcept;
ProviderRecordResult BeginWait(const WaitObservation&, uint64_t monotonic_ns,
                               WaitScope* out) noexcept;
void CompleteWait(WaitScope*, Kyty::DevTools::WaitOutcome, uint64_t observed,
                  uint64_t monotonic_ns) noexcept;
}
```

- [ ] **Step 1: Add focused HLE/wait tests against compile-only seams**

Extend `Providers.h` with the exact interfaces above, reusing the core `HleCallKind` ABI rather than defining another ID enum. `known_producer_ref` is the validated `producer_ref`; it never substitutes an operation correlation such as submit ID. Add compile-only no-op/invalid-result definitions in `KernelProvider.cpp` and `HleProvider.cpp`, then extend the already registered suite with exact tests named `NestedHleIdentityPreserved`, `HleTimelineRetainsOwnerAfterProgressReuse`, `WaitGraphCycleIsExplicit`, `UnknownProducerRemainsUnknown`, `LastSignalerDoesNotBecomeProducer`, `MultipleSetsWithMasksRemainUnknown`, `MultipleSemaphoreSignalsRemainUnknown`, `ReorderedQueueEventsRemainUnknown`, `RejectsMalformedProducerRef`, `TimedWaitDeadlineFlagIsCanonical`, `TimeoutOutcomeIsClassified`, `ObjectIdGenerationRejectsReuse`, `WaitObservationIdsNeverContainAddressBits`, and `ResultErrorFlagsMatchWaitOutcome`. HLE progress key and epoch both equal the monotonic invocation ID; progress correlation is the owning GuestThread ref, auxiliaries are call kind/depth, while timeline correlation is the owner, payload 1 the invocation ID, and payload 3 the packed kind/depth. The deadline test requires `Timed` and nonzero deadline together, and requires an untimed wait to have both absent/zero. A wait progress record puts owner ref in auxiliary 0 and optional producer ref in auxiliary 1; object ID exists only in WaitBegin/WaitEnd timeline payload. The tests feed only sanitized records and do not duplicate generic EventFlag/Semaphore/EventQueue behavior tests. Do not modify the real HLE or wait call sites yet.

- [ ] **Step 2: Prove the HLE/wait test is red for the intended missing behavior**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsKernelProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  NestedHleIdentityPreserved$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsKernelProvider.NestedHleIdentityPreserved' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsKernelProvider\.NestedHleIdentityPreserved' <<<"$red_output"
```

The red gate is invalid if the build fails, the suite is absent, zero tests run, or another test is the only failure.

- [ ] **Step 3: Implement the closed HLE and object-ID seams**

Replace the compile-only definitions with the allocation-free scopes above. A process-lifetime nonzero 56-bit HLE invocation allocator never reuses IDs; the fixed 512 HLE progress slots are the concurrency bound and their normal capacity counter reports omitted observations. The private pthread telemetry context holds only a checked nesting-depth scalar; successful Begin increments it, Complete requires LIFO depth and decrements it, and failure changes neither. Use the scope only at the six selected exports; never instrument `PRINT_NAME`, registration macros, or a universal HLE dispatch seam. A fixed 1,024-slot object registry assigns monotonic nonzero IDs to EventFlag, semaphore, and equeue lifetimes, validates slot generation on producer/read/retire, and never derives IDs from pointers or guest names. `AllocateWaitObservationId` is a separate monotonic 56-bit allocator for one WaitRegMem observation; it makes no unsupported same-address lifetime claim.

- [ ] **Step 4: Add explicit wait scopes**

Instrument `KernelWaitEventFlag`/`KernelSetEventFlag`, `KernelWaitSema`/`KernelSignalSema`, and `KernelWaitEqueue`/`KernelTriggerEvent`. Object creation reserves a token and destruction retires it; reserve failure only records incompleteness. Each Set/Signal/Trigger records its current validated GuestThread ref as timeline history after the real operation. No current primitive exposes exact consumed-bit, permit, or event provenance, so these waits publish no producer edge; a future implementation may provide one only when it returns the exact consumed token. Record only waiter ID, wait kind, session-local object ID, sanitized predicate/mask/reference, finite deadline when present, optional validated producer ref, and outcome. Never record guest names, pointers, equeue `ident`, trigger data, or user data. Scopes only observe and their trivial destructors emit nothing. Every existing normal return branch calls `CompleteHleCall`/`CompleteWait` with the actual translated result/outcome before returning; focused branch-coverage tests reject a normal path that leaves an active endpoint. An abrupt thread/process loss remains visible as incomplete work instead of receiving a fabricated outcome. `CommandProcessor::WaitRegMem32/64` is deferred to Task 12 where a real CP owner context exists. Generic pthread mutex, condition-variable, and rwlock waits remain deferred.

- [ ] **Step 5: Run the per-provider gate and commit waits only**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsKernelProvider\.' <<<"$listed_tests"; then exit 1; fi
test_filter='DevToolsKernelProvider.*:EmulatorKernelMemory.*:EmulatorGraphicsPackets.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/DevTools/Providers.h source/emulator/src/Kernel/EventFlag.cpp source/emulator/src/Kernel/Semaphore.cpp source/emulator/src/Kernel/EventQueue.cpp source/emulator/src/DevTools/Providers/KernelProvider.cpp source/emulator/src/DevTools/Providers/HleProvider.cpp source/emulator/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsKernelProvider.cpp
git -C "$devtools" commit -m 'feat(devtools): trace guest waits passively'
```

### Task 12: Instrument command processor, renderer, and GPU completion

**Files:**
- Modify: `source/emulator/include/Emulator/DevTools/Providers.h`
- Create: `source/emulator/src/DevTools/Providers/GraphicsProvider.cpp`
- Modify: `source/emulator/src/DevTools/Runtime.cpp`
- Modify: `source/3rdparty/vulkan/include/vulkan/vulkan_core.h`
- Modify: `source/emulator/include/Emulator/Graphics/AsyncJob.h`
- Create: `source/emulator/include/Emulator/Graphics/DeviceFault.h`
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsRender.h`
- Create: `source/emulator/src/Graphics/DeviceFault.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRun.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRender.cpp`
- Modify: `source/emulator/src/Graphics/Objects/Label.cpp`
- Modify: `source/emulator/src/Graphics/Window.cpp`
- Modify: `source/emulator/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsGraphicsProvider.cpp`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsDeviceFault.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: graphics CP identity (graphics plus compute rings), submit IDs, PM4 offsets/opcodes, renderer frame/draw/dispatch IDs, Vulkan queue/fence results, and capability-gated device-fault counts after device loss.
- Produces one CP instance per graphics worker, one serialized renderer-context projection, and publisher-owned submit/completion projections per logical GPU queue. Hot callers remain distinct SPSC timeline writers and never mutate shared progress. A focused `DeviceFaultContext` at the Vulkan host boundary owns extension feature enablement, the loaded function entry point, and the one-shot counts query; DevTools receives only the fixed sanitized snapshot.

The command seam is explicit and single-writer:

```cpp
struct ReservedCommandWorkerTelemetry {
    ThreadWriterReservation writer;
    Kyty::DevTools::ProgressToken progress;
    uint64_t worker_instance;
};
struct ActiveCommandTelemetry {
    Kyty::Emulator::DevTools::ThreadWriterReservation writer;
    Kyty::DevTools::ProgressToken progress;
    uint64_t command_session_id;
    uint64_t epoch;
    bool active;
};
bool ReserveCommandWorker(Kyty::DevTools::ThreadRole role,
                          ReservedCommandWorkerTelemetry* out) noexcept;
bool ActivateCommandWorker(ReservedCommandWorkerTelemetry*, uint64_t now,
                           ActiveCommandTelemetry* out) noexcept;
ProviderRecordResult BeginCommand(ActiveCommandTelemetry*, uint64_t size_dw,
                                  uint64_t now) noexcept;
ProviderRecordResult RecordCommandPacket(ActiveCommandTelemetry*,
                                         uint64_t offset_dw,
                                         uint32_t opcode,
                                         uint64_t now) noexcept;
ProviderRecordResult BeginRegisterMemoryWait(ActiveCommandTelemetry*,
                                             const WaitObservation&,
                                             uint64_t now,
                                             WaitScope* out) noexcept;
void CompleteCommand(ActiveCommandTelemetry*, Kyty::DevTools::ResultCategory,
                     uint64_t now) noexcept;
void CloseCommandWorker(ActiveCommandTelemetry*, Kyty::DevTools::ResultCategory,
                        uint64_t now) noexcept;
```

There is exactly one CP endpoint per worker: three graphics workers plus 64
configured compute rings fit the capacity of 80. `BeginCommand` assigns a
process-monotonic nonzero `command_session_id` independent of legacy
`m_sumbit_id`, starts a newer epoch with `CommandSubmit`, and stores size in
auxiliary 0. Every packet starts a strictly newer epoch on the same endpoint
with `CommandPacket`, the same correlation/session ID, and offset/opcode in
auxiliaries 0/1; this supersedes the prior active marker without pretending it
completed. WaitRegMem advances that current epoch to Waiting and back to Active
and also owns one distinct Synchronization endpoint/observation ID. End records
Complete for the current epoch. The active context is passed explicitly into
`CommandProcessor::Run`; no shared processor token or TLS lookup is allowed.

`DeviceFaultContext::Initialize` is called exactly once after selected-device
feature negotiation and entry-point loading. It publishes available only when
extension, `deviceFault`, and function pointer are all present; every other
combination publishes unavailable. `CaptureAfterResult(VkResult, time,
submission_id)` returns its input result unchanged, and `Snapshot` exposes only
`GpuFaultSnapshot`.

```cpp
struct DeviceFaultDispatch {
    PFN_vkGetDeviceFaultInfoEXT get_device_fault_info = nullptr;
};
enum class DeviceFaultInitResult: uint8_t {
    Available = 0, Unavailable = 1, AlreadyInitialized = 2, InvalidArgument = 3
};
class DeviceFaultContext {
public:
    DeviceFaultContext(Kyty::DevTools::GpuFaultRegistry& registry,
                       const Kyty::Emulator::DevTools::ProviderEnableGate& gate) noexcept;
    DeviceFaultInitResult Initialize(VkDevice device, bool extension_advertised,
                                     bool device_fault_feature_enabled,
                                     DeviceFaultDispatch dispatch) noexcept;
    VkResult CaptureAfterResult(VkResult result, uint64_t monotonic_ns,
                                uint64_t gpu_submission_id) noexcept;
    bool Snapshot(Kyty::DevTools::GpuFaultSnapshot* out) const noexcept;
};
DeviceFaultContext& GetDeviceFaultContext() noexcept;
```

`GetDeviceFaultContext` returns one process-lifetime context constructed from
the exact addresses returned by `GetGpuFaultRegistry()` and
`GetProviderEnableGate()`. `GraphicContext` remains unchanged and stores no
owner; existing aggregate/default construction is preserved. Initialization
runs once on the device-setup thread before worker access. Duplicate
initialization returns `AlreadyInitialized` without mutation. Every capture
checks `AllowsFullTelemetry()` before registry claim or Vulkan dispatch and
otherwise returns the input result. Version 1 defines no fake device teardown:
the current Graphics shutdown hooks do not join the detached/infinite users or
destroy `VkDevice`, so the context is reclaimed only at process exit and has no
destructor-side Vulkan call. The one-shot registry arbitrates concurrent
device-loss callers, so the context stores no second query state.

- [ ] **Step 1: Add command-processing tests against compile-only provider stubs**

First add only command-processing tests named `CommandWorkersRemainDistinct`, `IndependentRingProgressIsPreserved`, `CommandLossCapsConfidence`, and `CommandSessionIdsAreOpaque`. Declare the narrow graphics-provider API in `Providers.h` and add compile-only no-op/invalid-result definitions in `GraphicsProvider.cpp`; do not instrument graphics owners yet. Register `UT_LINK(DevToolsGraphicsProvider)` and explicitly add the test source to `source/unit_test/CMakeLists.txt`. Do not add GPU submit/completion expectations until the command-processing commit is complete.

Also add `PacketProgressPreservesSubmitOffsetAndOpcode`: packet progress uses
`correlation=command_session_id`, auxiliary 0 for the dword offset, and auxiliary 1 for
the opcode; submit progress uses the command size and zero auxiliary 1.

- [ ] **Step 2: Prove command-processing identity is red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsGraphicsProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  CommandWorkersRemainDistinct$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsGraphicsProvider.CommandWorkersRemainDistinct' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsGraphicsProvider\.CommandWorkersRemainDistinct' <<<"$red_output"
```

- [ ] **Step 3: Instrument CP boundaries**

The owning `GraphicsRing` reserves tokens for its batch, draw, and constant workers; each `ComputeRing` reserves its own worker token. Add a plain `ReservedCommandWorkerTelemetry` value and an `AsyncJob(const char* name, ReservedCommandWorkerTelemetry telemetry)` overload. Declare the optional telemetry context member before the existing `Core::Thread*` member and initialize the context before allocating the native thread wrapper, so the already-reserved state is visible at entry. Preserve the existing uninstrumented `AsyncJob(const char* name)` constructor, including `Tile.cpp` call sites that pass `nullptr`. At thread entry the instrumented overload activates, and on actual return it emits exit then closes. Do not promise an abandon path for `Core::Thread`: its current constructor exposes no recoverable create-failure result. Do not store a writer token on `CommandProcessor`, because independently scheduled workers can reach the same processor. The actual worker passes its context into `CommandProcessor::Run`, which records submit start/end and updates current command offset/opcode through that worker's single-writer progress endpoint. Do not emit every packet into the bounded history ring; use progress updates for high-rate offsets and ring events only for submit/wait transitions.

- [ ] **Step 4: Build and commit command-processing progress**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsGraphicsProvider\.' <<<"$listed_tests"; then exit 1; fi
test_filter='DevToolsGraphicsProvider.*:EmulatorGraphicsPackets.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/DevTools/Providers.h source/emulator/include/Emulator/Graphics/AsyncJob.h source/emulator/src/Graphics/GraphicsRun.cpp source/emulator/src/DevTools/Providers/GraphicsProvider.cpp source/emulator/src/DevTools/Runtime.cpp source/emulator/CMakeLists.txt source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsGraphicsProvider.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): trace command processor progress'
```

- [ ] **Step 5: Refresh the pinned Vulkan C declarations in one isolated commit**

The curated base carries Vulkan C header version 176, which predates `VK_EXT_device_fault`, while the already vendored SDL 2.32.10 Khronos header is version 275 and contains the official extension declarations. Do not hand-declare extension ABI and do not change the Vulkan loader, SPIR-V tools, MoltenVK, or C++ wrapper in this commit. Prove the old include path lacks the declaration, then replace only `source/3rdparty/vulkan/include/vulkan/vulkan_core.h` byte-for-byte with the already vendored version-275 Khronos header, preserving its Apache-2.0 notice. Recompile the complete tree and rerun the strict/visual frontier before accepting the dependency declaration refresh.

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
mkdir -p "$scratch/red" "$scratch/Bundles"
main_header="$devtools/source/3rdparty/vulkan/include/vulkan/vulkan_core.h"
sdl_header="$devtools/source/3rdparty/sdl2/sdl2/src/video/khronos/vulkan/vulkan_core.h"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
rg -q '^#define VK_HEADER_VERSION 176$' "$main_header"
rg -q '^#define VK_HEADER_VERSION 275$' "$sdl_header"
probe=$'#include <vulkan/vulkan_core.h>\n#ifndef VK_EXT_device_fault\n#error DEVICE_FAULT_DECLARATIONS_MISSING\n#endif\nVkPhysicalDeviceFaultFeaturesEXT f{};\nPFN_vkGetDeviceFaultInfoEXT p = nullptr;'
set +e
c++ -std=c++17 -I"$devtools/source/3rdparty/vulkan/include" -x c++ -c -o /dev/null - <<<"$probe" >"$scratch/red/vulkan-device-fault-header.log" 2>&1
red_status=$?
set -e
test "$red_status" -ne 0
rg -q 'DEVICE_FAULT_DECLARATIONS_MISSING' "$scratch/red/vulkan-device-fault-header.log"
install -m 0644 "$sdl_header" "$main_header"
cmp -s "$sdl_header" "$main_header"
c++ -std=c++17 -I"$devtools/source/3rdparty/vulkan/include" -x c++ -c -o /dev/null - <<<"$probe"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/3rdparty/vulkan/include/vulkan/vulkan_core.h
git -C "$devtools" commit -m 'build: refresh Vulkan API declarations'
```

- [ ] **Step 6: Add renderer/GPU tests against compile-only seams**

Use the closed provider boundary below. Queue aliases are registered only at
device setup; all later arguments are opaque IDs and result categories.

```cpp
struct LogicalQueueToken { uint64_t session_id; };
enum class GpuTimelineKind: uint8_t { Submitted=1, Completed=2 };
bool RegisterLogicalQueue(uint64_t native_queue_alias_key,
                          LogicalQueueToken* out) noexcept;
bool ReserveGpuQueueProjection(LogicalQueueToken) noexcept;
bool AllocateGpuSubmissionId(uint64_t* out) noexcept;
ProviderRecordResult RecordGpuTimeline(GpuTimelineKind,
                                       LogicalQueueToken,
                                       uint64_t submission_id,
                                       Kyty::DevTools::ResultCategory,
                                       uint64_t monotonic_ns) noexcept;
void ProjectGpuTimeline(const EventRecord&) noexcept;
```

`native_queue_alias_key` is consumed during registration and never serialized;
production passes a locally derived equality key, while tests use inert values.
`ProjectGpuTimeline` is callable only by the publisher thread and updates the
pre-reserved submit/completion endpoints. The renderer provider similarly
reserves one endpoint for the single serialized `RenderContext`; callers emit
timeline events and never create per-caller renderer progress records.
`Runtime.cpp` invokes `ProjectGpuTimeline` while draining each validated event,
before appending it to retained history; projection is allocation-free and a
malformed/future event is ignored while its original record remains retained.

After the command-processing and header commits, extend the graphics-provider API/tests without touching the renderer or LabelManager implementation. Add `AllLogicalQueuesFitGpuProgressCapacity`, `LogicalQueueSubmitAndCompletionOwnersAreDistinct`, `SharedNativeQueueUsesOneProjection`, `TwoQueuesCompleteOutOfOrderWithoutCrossing`, `FenceWaitRetainsSubmissionJoin`, `LaterQueueCompletionImpliesEarlierSubmission`, `GpuSubmissionIdsStartAtOne`, `QueueResultDoesNotChangeVulkanResult`, and `LabelPollingRemainsSeparate` to `DevToolsGraphicsProvider`. In the separate `DevToolsDeviceFault` suite add `CapabilityPendingBeforeInitialization`, `CapabilityRequiresExtensionFeatureAndEntrypoint`, `AvailableInitializesNotObservedWithFlag`, `UnavailableInitializesStructuredState`, `VendorBinaryFeatureRemainsDisabled`, `DoesNotQueryBeforeDeviceLost`, `IgnoresTimeoutOutOfDateAndSuboptimal`, `DeviceLostZeroInitializesCountsAndUsesNullFaultInfoOnce`, `ConcurrentDeviceLostQueriesOnce`, `UnavailableCapabilityNeverCallsVulkan`, `CountsFailurePublishesZeroCounts`, `RepeatedDeviceLostDoesNotQueryTwice`, `ObservationDoesNotChangeVulkanResult`, and `DeviceFaultSnapshotContainsCountsOnly`. The mock asserts `VkDeviceFaultCountsEXT` is fully zero-initialized except exact `sType=VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT` and `pNext=nullptr` before dispatch. On query failure it deliberately writes partial nonzero outputs and proves the published address/vendor/vendor-binary counts are all zero. Use an injected Vulkan dispatch function and inert fake device value; no unit test requires a GPU. The privacy test API exposes no description/address/vendor-info/binary parameters at all. Add only the minimum compile-only no-op/invalid-result provider and device-fault definitions required to link them. Register both exact source files and `UT_LINK` entries.

The same suite adds `DuplicateInitializationDoesNotMutateCapability`,
`RuntimeGatePreventsClaimAndDispatch`, `ProviderAndPublisherShareFaultRegistry`,
`MissingDeviceFaultExtensionDoesNotChangeDeviceSelectionOrCreation`,
`NoDestructorSideVulkanCall`, `UnassociatedDeviceLossUsesSubmissionZero`, and
`GeneratedSubmissionIdsStartAtOne`. It calls the exact public context above,
not a test-only helper, and validates every resulting snapshot with
`ValidateGpuFaultSnapshot`.

- [ ] **Step 7: Prove GPU queue projection is red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsGraphicsProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  AllLogicalQueuesFitGpuProgressCapacity$' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^DevToolsDeviceFault\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  DeviceLostZeroInitializesCountsAndUsesNullFaultInfoOnce$' <<<"$listed_tests"; then exit 1; fi
for exact_test in \
  DevToolsGraphicsProvider.AllLogicalQueuesFitGpuProgressCapacity \
  DevToolsDeviceFault.DeviceLostZeroInitializesCountsAndUsesNullFaultInfoOnce; do
  set +e
  red_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_color=no --gtest_filter="$exact_test" 2>&1)
  red_status=$?
  set -e
  printf '%s\n' "$red_output"
  test "$red_status" -ne 0
  rg -Fqx "[ RUN      ] $exact_test" <<<"$red_output"
  test "$(rg -Fxc "[  FAILED  ] $exact_test" <<<"$red_output")" -eq 1
done
```

- [ ] **Step 8: Instrument renderer/GPU boundaries and bounded device-fault counts**

Replace the compile-only GPU behavior and give the serialized `RenderContext` one Renderer progress endpoint. During device setup, map every logical queue to an opaque queue-session ID; aliases of the same native queue share one session without publishing the raw handle. Reserve exactly two GpuQueue progress endpoints per logical queue (latest successfully enqueued submission and latest observed completion), so the current eleven logical queues require 22 of 32 slots. Actual submit/wait/status callers emit caller-owned SPSC timeline facts containing queue-session and monotonic nonzero `gpu_submission_id`; they never write these progress endpoints. The existing publisher is the sole writer that drains those facts and projects them in order. A failed enqueue never advances submitted progress; completion can advance only the matching queue/session and ID. Classifier comparison is within one queue session, and overlapping timeline loss prevents High confidence. No queue lock, polling thread, wait, or guest timing is added for telemetry. `LabelManager` only owns a distinct label-event lane for its existing `VkEvent` polling. It reserves before creating its detached worker and activates at `ThreadRun` entry; because the current worker has no orderly shutdown path, do not invent one merely to close telemetry—the active slot ends with the process. Observe existing `vkQueueSubmit`, `vkWaitForFences`, event/fence-status results, timeout, and device loss without recording Vulkan handles or changing `UINT64_MAX`, retry, wait, or completion semantics.

Keep extension policy in `Graphics/DeviceFault.*`, not in the dependency-free core or bundle writer. `VulkanFindPhysicalDevice` first chooses a device using only its existing baseline required-extension list; do not append `VK_EXT_device_fault` to the list that currently rejects devices for a missing entry. After selection, query only that selected device. When it advertises the extension and a chained `VkPhysicalDeviceFaultFeaturesEXT` query reports `deviceFault`, append it to the device-create extension list and chain an enabled feature with `deviceFault=VK_TRUE` and `deviceFaultVendorBinary=VK_FALSE`; otherwise device selection and creation remain the baseline path. After device creation, load `vkGetDeviceFaultInfoEXT` through `vkGetDeviceProcAddr`; call `GetDeviceFaultContext().Initialize` exactly once and publish `NotObserved/flag=1` only when extension, feature, and entry point are all enabled, otherwise `ExtensionUnavailable/flag=0`. Every existing checked Vulkan boundary may call `CaptureAfterResult`, but it first returns unless Full telemetry is still allowed and then returns unless the result is exactly `VK_ERROR_DEVICE_LOST`. The first claimant uses the shared `GpuFaultRegistry`, zero-initializes `VkDeviceFaultCountsEXT`, sets only its exact `sType` and null `pNext`, calls `vkGetDeviceFaultInfoEXT(device, &counts, nullptr)` once, and publishes counts only on `VK_SUCCESS`. Failure publishes `CountsFailed` with all counts zero even if the driver touched partial outputs. It never allocates or supplies `VkDeviceFaultInfoEXT`, so it never retrieves descriptions, addresses, vendor records, or binaries. Later concurrent device-loss observations preserve the first completed snapshot. Parent disconnect and runtime shutdown close the gate before any later observation, so neither registry claim nor Vulkan dispatch occurs afterward.

`DeviceFault.cpp` contains `static_assert(GpuFaultResultSuccess == VK_SUCCESS)`
and `static_assert(GpuFaultResultDeviceLost == VK_ERROR_DEVICE_LOST)`; the
dependency-free core never includes Vulkan headers or duplicates an unnamed
numeric literal.

- [ ] **Step 9: Build, test, replay disabled/enabled, and commit GPU progress**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsGraphicsProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^DevToolsDeviceFault\.' <<<"$listed_tests"; then exit 1; fi
test_filter='DevToolsGraphicsProvider.*:DevToolsDeviceFault.*:EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/DevTools/Providers.h source/emulator/include/Emulator/Graphics/DeviceFault.h source/emulator/include/Emulator/Graphics/GraphicsRender.h source/emulator/src/Graphics/DeviceFault.cpp source/emulator/src/Graphics/GraphicsRender.cpp source/emulator/src/Graphics/Objects/Label.cpp source/emulator/src/Graphics/Window.cpp source/emulator/src/DevTools/Providers/GraphicsProvider.cpp source/emulator/src/DevTools/Runtime.cpp source/emulator/CMakeLists.txt source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsDeviceFault.cpp source/unit_test/src/devtools/UnitTestDevToolsGraphicsProvider.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): trace GPU progress passively'
```

Expected: same strict/visual frontier; neither mode fabricates completion.

### Task 13: Instrument VideoOut and presentation

**Files:**
- Modify: `source/emulator/include/Emulator/DevTools/Providers.h`
- Create: `source/emulator/src/DevTools/Providers/VideoOutProvider.cpp`
- Modify: `source/emulator/src/DevTools/Runtime.cpp`
- Modify: `source/emulator/src/Graphics/VideoOut.cpp`
- Modify: `source/emulator/src/Graphics/Window.cpp`
- Modify: `source/emulator/CMakeLists.txt`
- Create: `source/unit_test/src/devtools/UnitTestDevToolsVideoOutProvider.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: opaque per-run VideoOut instance, submitting `WriterKey`, bounded buffer index, generated flip-request ID, swapchain generation, acquire/blit/present results, and negotiated `RecordingMode`.
- Produces one publisher-owned submitted projection and one Window-owned completion projection per VideoOut session, presentation-owner progress, and fixed frame/flip measurement counters/histogram. Submit callers remain distinct timeline writers; GPU completion can be distinguished from flip/present stall without recording `flip_arg`, guest pointers, or Vulkan handles.

```cpp
struct VideoOutSessionToken { uint64_t session_id; uint64_t generation; };
bool ReserveVideoOutSession(VideoOutSessionToken* out) noexcept;
void RetireVideoOutSession(VideoOutSessionToken) noexcept;
bool RecordAcceptedFlip(Kyty::DevTools::TelemetryWriterToken submitter,
                        VideoOutSessionToken, uint32_t buffer_index,
                        uint64_t monotonic_ns,
                        uint64_t* request_id) noexcept;
ProviderRecordResult RecordCompletedFlip(
                                         Kyty::DevTools::TelemetryWriterToken window,
                                         VideoOutSessionToken,
                                         uint64_t request_id,
                                         uint32_t buffer_index,
                                         Kyty::DevTools::ResultCategory,
                                         uint64_t monotonic_ns) noexcept;
void ProjectAcceptedFlip(const EventRecord&) noexcept;
```

`RecordAcceptedFlip` is called only after the real depth-two queue accepts the
request, allocates a monotonic nonzero request ID, and writes a caller-owned
timeline event. Queue-full paths return false and publish no accepted progress.
Only the publisher calls `ProjectAcceptedFlip`; only Window updates completion.
Retirement closes both projections, increments the session generation, and
causes every old token/request completion to fail without mutation.
`Runtime.cpp` calls `ProjectAcceptedFlip` from the same validated drain loop as
GPU projection, before retained-history append; no second inbox or polling
thread exists.

- [ ] **Step 1: Add provider tests against compile-only seams**

Declare the narrow VideoOut-provider API in `Providers.h` and add compile-only no-op/invalid-result definitions in `VideoOutProvider.cpp`; do not modify `VideoOut.cpp` or `Window.cpp` yet. Register `UT_LINK(DevToolsVideoOutProvider)` and explicitly add the test source to the unit-test target. Add exact tests named `TwoPendingFlipsRetainBacklog`, `SubmitAndCompletionJoinByRequestId`, `CrossThreadSubmittersProjectToOneSession`, `SubmitterWriterKeyStaysTimelineOnly`, `QueueFullDoesNotPublishAcceptedProgress`, `FlipArgCanaryIsNeverRecorded`, `CompletionCannotAdvanceAnotherSession`, `SessionReuseRejectsOldRequest`, `PresentationStallSurvivesGpuProgress`, `WindowUsesRuntimeMeasurementRegistry`, `MetricsOnlyAndFullShareMeasurementPath`, and `FrameHistogramUsesBoundedBuckets`. Model two opaque VideoOut instances, a guest-call submitter and LabelManager-callback submitter with distinct `WriterKey` values, distinct generated flip-request IDs, and a swapchain generation change. Compare the provider registry address with `GetMeasurementRegistry()`, feed identical presentation timestamps through MetricsOnly and Full, and require identical counts/buckets; MetricsOnly must emit no ordinary event/progress record.

- [ ] **Step 2: Prove submit-writer isolation is red**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsVideoOutProvider\.' <<<"$listed_tests"; then exit 1; fi
if ! rg -q '^  TwoPendingFlipsRetainBacklog$' <<<"$listed_tests"; then exit 1; fi
set +e
red_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevToolsVideoOutProvider.TwoPendingFlipsRetainBacklog' 2>&1)
red_status=$?
set -e
printf '%s\n' "$red_output"
test "$red_status" -ne 0
rg -q '\[[[:space:]]*FAILED[[:space:]]*\][[:space:]]+DevToolsVideoOutProvider\.TwoPendingFlipsRetainBacklog' <<<"$red_output"
```

- [ ] **Step 3: Instrument `FlipQueue` without changing its waits**

Map each live VideoOut configuration to a session-local opaque ID/generation and reserve one submitted projection plus one completion projection. Guest calls and LabelManager callbacks keep distinct writer tokens and emit timeline events only. Assign `flip_request_id` only after `FlipQueue::Submit` accepts the request; the publisher projects the latest accepted ID while preserving the depth-two backlog in timeline history. `FlipQueue::Wait` uses the waiter thread's synchronization endpoint; the Window/Flip thread executing `Flip` updates only its completion projection. Join by opaque VideoOut session/generation, request ID, and bounded buffer index. Queue-full attempts publish neither accepted timeline nor progress, and session reuse rejects old completions. Never copy `flip_arg`, configuration pointers, guest callback data, or names into telemetry. The observer never wakes `FlipQueue`, changes timeouts, or submits a flip.

- [ ] **Step 4: Instrument acquire/present**

Store the presentation writer/progress tokens, incrementing swapchain generation, and a non-owning pointer to the one process-lifetime `GetMeasurementRegistry()` instance in private `WindowContext`; never store or mutate a `MeasurementSnapshot` DTO as an owner. Reserve before `WindowRun` creation and activate at its entry. The current `WindowRun` terminates through `_Exit` and has no orderly context teardown, so do not fabricate a close path; process-lifetime runtime storage makes later shutdown calls safe. Record image-acquire begin/result, present submission, `vkQueuePresentKHR` result, swapchain generation, and existing `vkDeviceWaitIdle` begin/result around recreation. On every completed presentation, the sole Window/presentation writer calls that registry's `RecordPresentation`, updating frame/flip totals and exactly one 1 ms histogram bucket from the prior presentation timestamp in both recording modes. MetricsOnly performs this measurement update and returns before ordinary event/progress writes; Full continues them. Keep Vulkan ownership in `Window.cpp`; the provider receives only result enums, timestamps, and opaque IDs, never handles.

- [ ] **Step 5: Build, test, replay, and commit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"; test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"; test -z "${KYTY_AUTO_CROSS:-}"
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevToolsVideoOutProvider\.' <<<"$listed_tests"; then exit 1; fi
test_filter='DevToolsVideoOutProvider.*:EmulatorGraphicsState.*:EmulatorPad.*'
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=3 --batches=11 --events-per-batch=100000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=3 --batches=11 --events-per-batch=100000
run_bounded() { invocation_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" new-invocation-id); tree_id=$("$devtools/_build_linux_devtools/devtools/kyty_devtools" tree-id --root "$devtools"); checkpoint="$scratch/frontier-$invocation_id.json"; set +e; "$devtools/_build_linux_devtools/devtools/kyty_devtools" checkpoint-run --recording="$KYTY_EXPECTED_RECORDING_MODE" --duration-s=90 --cleanup-s=10 --checkpoint-output "$checkpoint" --invocation "$invocation_id" --tree "$tree_id" --output-dir "$bundle_dir" -- "$@"; status=$?; set -e; test "$status" -eq 0 || test "$status" -eq 124; "$devtools/_build_linux_devtools/devtools/kyty_devtools" attest-frontier --interactive --checkpoint "$checkpoint"; "$devtools/_build_linux_devtools/devtools/kyty_devtools" verify-frontier --checkpoint "$checkpoint" --expected-invocation "$invocation_id" --expected-tree "$tree_id" --expected-mode "$KYTY_EXPECTED_RECORDING_MODE" --require-fresh --require-strict --require-frames --require-flips --require-visual-equivalence --require-no-earlier-exit --reject-forced-cleanup; }
KYTY_EXPECTED_RECORDING_MODE=metrics-only run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
KYTY_EXPECTED_RECORDING_MODE=full run_bounded "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
git -C "$devtools" diff --check
git -C "$devtools" add source/emulator/include/Emulator/DevTools/Providers.h source/emulator/src/DevTools/Providers/VideoOutProvider.cpp source/emulator/src/DevTools/Runtime.cpp source/emulator/src/Graphics/VideoOut.cpp source/emulator/src/Graphics/Window.cpp source/emulator/CMakeLists.txt source/unit_test/CMakeLists.txt source/unit_test/src/devtools/UnitTestDevToolsVideoOutProvider.cpp source/unit_test/src/UnitTest.cpp
git -C "$devtools" commit -m 'feat(devtools): trace presentation progress'
```

### Task 14: Add cross-platform CI build and concurrency gates

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: the committed Task 9A benchmark/stress targets and complete provider suite.
- Produces Linux build/test/TSan, macOS compile/test, and Windows MinGW compile gates without claiming unavailable runtime validation.

- [ ] **Step 1: Extend CI without weakening existing jobs**

Add a Linux job that performs a full Release build, lists/runs `DevTools*.*`, runs enabled/disabled benchmarks, and runs the small TSan stress target. Add a macOS job that builds `kyty_devtools`, the unit-test runner, and runs deterministic DevTools tests. Retain the current Windows MinGW compile and explicitly build both DevTools targets there; Windows runtime remains a manual/CI-host gate. Replace retired `actions/checkout@v3` with the existing reviewed `actions/checkout@v6` major and `actions/upload-artifact@v3` with `actions/upload-artifact@v7`; do not name a nonexistent shared major. Upgrade `jurplel/install-qt-action@v2` to `@v4` while preserving the repository's explicit Qt 5.15.2 and `win64_mingw81` inputs; do not accept the action's newer default Qt as an implicit dependency bump.

- [ ] **Step 2: Verify the workflow command set locally**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKYTY_DEVTOOLS_BUILD_TSAN_STRESS=ON
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
if ! rg -q '^DevTools' <<<"$listed_tests"; then exit 1; fi
"$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter='DevTools*.*'
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
"$devtools/_build_linux_devtools/devtools/kyty_devtools_stress"
git -C "$devtools" diff --check
git -C "$devtools" add .github/workflows/ci.yml
git -C "$devtools" commit -m 'ci: validate native diagnostics'
```

Expected: local Linux gates are green. macOS runtime and Windows runtime remain explicitly unverified until an appropriate host executes them; a compile-only job is never reported as runtime validation.

### Task 15: Prove privacy, non-interference, crash-safe capture, and strict frontier

**Files:**
- No tracked private evidence.
- Update only generic public DevTools usage documentation if commands changed during implementation.

**Interfaces:**
- Consumes: complete v1 executable and emulator providers.
- Produces acceptance evidence for Runtime Stall Snapshot v1 and the first real rendering-frontier bundle.

- [ ] **Step 1: Run all deterministic DevTools and owner regressions**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
cmake -S "$devtools/source" -B "$devtools/_build_linux_devtools" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$devtools/_build_linux_devtools"
listed_tests=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_list_tests)
for required_suite in DevToolsEventRing DevToolsProgress DevToolsClassifier DevToolsProtocol DevToolsSupervisor DevToolsBundle DevToolsLifecycle DevToolsBenchmark DevToolsRuntime DevToolsKernelProvider DevToolsGraphicsProvider DevToolsDeviceFault DevToolsVideoOutProvider; do
  if ! rg -q "^${required_suite}\\." <<<"$listed_tests"; then exit 1; fi
done
test_filter='DevTools*.*:EmulatorGraphicsPackets.*:EmulatorGraphicsState.*:EmulatorKernelMemory.*:EmulatorPad.*'
test_output=$("$devtools/_build_linux_devtools/fc_script" '{kyty_run_tests()}' --gtest_filter="$test_filter")
printf '%s\n' "$test_output"
if ! rg -q '\[[[:space:]]+PASSED[[:space:]]+\][[:space:]]+[1-9][0-9]* tests?' <<<"$test_output"; then exit 1; fi
git -C "$devtools" diff --check
```

- [ ] **Step 2: Run all five synthetic lifecycle modes**

Verify blocked lane with live publisher, whole publication stop while process stays alive, parent disconnect, normal exit, and abnormal crash produce distinct results. For a live blocked/publication-stop child, the supervisor captures once per causal state and keeps sampling without terminating it. For parent disconnect, the worker disables transport once and continues. For normal exit/crash, the supervisor finalizes once, closes ownership, and returns the documented child result; it does not remain alive after the child.

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/SyntheticBundles"
mkdir -p "$bundle_dir"
for mode in blocked-lane publication-stop parent-disconnect normal-exit crash; do
  "$devtools/_build_linux_devtools/devtools/kyty_devtools" self-test \
    --output-dir "$bundle_dir/$mode" --mode="$mode" \
    --suspected-ms=20 --confirmed-ms=50 --cleanup-ms=2000
done
```

- [ ] **Step 3: Run recursive canary scan**

Launch a synthetic worker with canary values in argv, environment, fake log, path, guest name, and shader hash. Search every completed automatic artifact as bytes. Any match fails acceptance; raw local-only attachment is not generated.

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
canary_dir="$scratch/CanaryBundles"
canary='KYTY_PRIVACY_CANARY_7f1e2a90'
mkdir -p "$canary_dir"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" self-test \
  --output-dir "$canary_dir" --mode=privacy-canary --canary="$canary" \
  --suspected-ms=20 --confirmed-ms=50 --cleanup-ms=2000
set +e
rg -a -F "$canary" "$canary_dir"
canary_status=$?
set -e
test "$canary_status" -eq 1
```

- [ ] **Step 4: Measure MetricsOnly/Full workload overhead**

Run the fixed synthetic enabled/disabled benchmark after warmup. Then reproduce the private strict workload twice through native `measure --recording=metrics-only` and twice through `measure --recording=full`, using identical 180-second duration, resolution, Silent logging, shader-cache state, and real input sequence. The measurement path—not Console text—produces frame/flip counts, the fixed frame-interval histogram, and OS process CPU. Its deliberate deadline is measurement infrastructure, not emulator recovery; a forced cleanup invalidates the run.

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
bundle_dir="$scratch/Bundles"
mkdir -p "$scratch/Shaders" "$scratch/Logs" "$scratch/Buffers" "$scratch/Pipelines" "$bundle_dir"
metrics_root=$(mktemp -d "$scratch/MetricsAcceptance.XXXXXX")
export KYTY_SHADER_LOG_FOLDER="$scratch/Shaders"
export KYTY_PRINTF_OUTPUT_FOLDER="$scratch/Logs"
export KYTY_COMMAND_BUFFER_DUMP_FOLDER="$scratch/Buffers"
export KYTY_PIPELINE_DUMP_FOLDER="$scratch/Pipelines"
test -n "${KYTY_GUEST_ROOT:-}"
test -z "${KYTY_STUB_MISSING:-}"
test -z "${KYTY_GFX_PERMISSIVE:-}"
test -z "${KYTY_SKIP_UD2:-}"
test -z "${KYTY_AUTO_CROSS:-}"
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=disabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
"$devtools/_build_linux_devtools/devtools/kyty_devtools" benchmark --mode=enabled --warmup-batches=5 --batches=31 --events-per-batch=1000000
for recording_mode in metrics-only full; do
  for run in 1 2; do
    run_dir="$metrics_root/${recording_mode}-${run}"
    "$devtools/_build_linux_devtools/devtools/kyty_devtools" measure \
      --recording="$recording_mode" --duration-s=180 --output-dir "$run_dir" -- \
      "$devtools/_build_linux_devtools/fc_script" "$devtools/scripts/run_guest.lua" "$KYTY_GUEST_ROOT"
    "$devtools/_build_linux_devtools/devtools/kyty_devtools" validate-metrics \
      --input "$run_dir/metrics.json" --expected-mode="$recording_mode" --expected-duration-s=180
  done
done
"$devtools/_build_linux_devtools/devtools/kyty_devtools" compare-metrics \
  --baseline "$metrics_root/metrics-only-1/metrics.json" \
  --baseline "$metrics_root/metrics-only-2/metrics.json" \
  --full "$metrics_root/full-1/metrics.json" \
  --full "$metrics_root/full-2/metrics.json" \
  --max-regression-percent=3
```

Record the four validated output summaries, median/p95 frame interval, user/system process CPU, frame/flip counts, exact resolution, Silent logging, shader-cache state, real press/release input sequence, and every loss/capacity counter outside Git. A Full regression above 3% that also exceeds MetricsOnly run-to-run variation blocks acceptance.

- [ ] **Step 5: Verify the same strict/visual frontier**

MetricsOnly and Full must reach the same correctly rendered or identically first-bad state; no earlier EXIT, corrupted frame, input regression, device loss, relevant Vulkan validation error, unavailable measurement, or new loss gap is accepted. Passive telemetry may remain only after this equivalence is proven.

- [ ] **Step 6: Capture the current real rendering frontier**

Use the supervisor bundle to identify the earliest observable producer/consumer divergence behind the visual corruption. The bundle is evidence for the next one-hypothesis compatibility cycle; it is not itself a visual fix or playability claim.

- [ ] **Step 7: Write and validate the sanitized acceptance report**

Using `apply_patch`, create `/home/monasterios/Documents/PS5/Kyty-devtools-scratch/acceptance-report.md` outside Git from the actual outputs. Give it these exact second-level headings: `Identity`, `Build and tests`, `Synthetic lifecycle`, `Privacy`, `Overhead`, `Strict equivalence`, `Visual frontier`, `Platform gates`, and `Remaining work`. Record the frozen curated base and DevTools commit, host/GPU capability summary, nonzero test count, outcome of every synthetic mode, recursive canary result, enabled/disabled median and p95 plus run-to-run variation, process CPU, all loss/capacity counters, exact resolution, Silent logging, shader-cache state, real press/release input sequence, four strict run results with frame/flip counts, Vulkan validation outcome, visual equivalence, and earliest observable divergence. Write `not verified` for any unavailable platform/runtime gate; never infer a pass. Do not include private fixture names, product IDs, absolute private paths, raw logs, captures, binaries, shaders, textures, or screenshots.

```bash
set -euo pipefail
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
report="$scratch/acceptance-report.md"
test -s "$report"
for heading in 'Identity' 'Build and tests' 'Synthetic lifecycle' 'Privacy' 'Overhead' 'Strict equivalence' 'Visual frontier' 'Platform gates' 'Remaining work'; do
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

- [ ] **Step 8: Freeze public usage documentation**

Re-run every documented command in `docs/devtools/runtime-stall-snapshot.md` against the built CLI help/schema. If implementation changed syntax, update only this generic document and commit it separately:

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
test -s docs/devtools/runtime-stall-snapshot.md
set +e
git -C "$devtools" diff --quiet -- docs/devtools/runtime-stall-snapshot.md
docs_status=$?
set -e
if test "$docs_status" -eq 1; then
  git -C "$devtools" add docs/devtools/runtime-stall-snapshot.md
  git -C "$devtools" diff --cached --check
  git -C "$devtools" commit -m 'docs(devtools): document stall snapshots'
else
  test "$docs_status" -eq 0
fi
```

- [ ] **Step 9: Final history and commit audit**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
primary=/home/monasterios/Documents/PS5/Kyty
curated=/home/monasterios/Documents/PS5/Kyty-gen5-curated
scratch=/home/monasterios/Documents/PS5/Kyty-devtools-scratch
test -n "${KYTY_GUEST_ROOT:-}"
private_fixture_name=$(basename "$KYTY_GUEST_ROOT")
git -C "$devtools" log --format=fuller --stat main..HEAD
git -C "$devtools" log -p --format='%H%n%B' main..HEAD
commit_messages=$(git -C "$devtools" log main..HEAD --format='%B')
history_patch=$(git -C "$devtools" log -p --format='%H%n%B' main..HEAD)
main_diff=$(git -C "$devtools" diff --unified=0 main..HEAD -- .)
main_added_lines=$(awk 'substr($0, 1, 1) == "+" && substr($0, 1, 3) != "+++" { print }' <<<"$main_diff")
frozen_base=$(<"$scratch/baseline/curated-head")
frozen_diff=$(git -C "$devtools" diff --unified=0 "$frozen_base"..HEAD -- .)
added_lines=$(awk 'substr($0, 1, 1) == "+" && substr($0, 1, 3) != "+++" { print }' <<<"$frozen_diff")
set +e
rg -ni '^Co-authored-by:' <<<"$commit_messages"
coauthor_status=$?
rg -F "$private_fixture_name" <<<"$history_patch"
history_private_status=$?
rg -F "$private_fixture_name" <<<"$main_added_lines"
diff_private_status=$?
rg -ni 'Co-authored-by:' <<<"$main_added_lines"
diff_coauthor_status=$?
rg -n 'KYTY_STUB_MISSING|KYTY_GFX_PERMISSIVE|KYTY_SKIP_UD2|KYTY_AUTO_CROSS' <<<"$added_lines"
flag_status=$?
rg -F "$private_fixture_name" <<<"$added_lines"
added_private_status=$?
set -e
test "$coauthor_status" -eq 1
test "$history_private_status" -eq 1
test "$diff_private_status" -eq 1
test "$diff_coauthor_status" -eq 1
test "$flag_status" -eq 1
test "$added_private_status" -eq 1
git -C "$primary" rev-parse HEAD >"$scratch/final-primary-head"
git -C "$primary" status --porcelain=v2 -z >"$scratch/final-primary-status"
git -C "$primary" stash list >"$scratch/final-primary-stashes"
git -C "$curated" rev-parse HEAD >"$scratch/final-curated-head"
git -C "$curated" status --porcelain=v2 -z >"$scratch/final-curated-status"
git -C "$curated" stash list >"$scratch/final-curated-stashes"
cmp "$scratch/baseline/primary-head" "$scratch/final-primary-head"
cmp "$scratch/baseline/primary-status" "$scratch/final-primary-status"
cmp "$scratch/baseline/primary-stashes" "$scratch/final-primary-stashes"
cmp "$scratch/baseline/curated-head" "$scratch/final-curated-head"
cmp "$scratch/baseline/curated-status" "$scratch/final-curated-status"
cmp "$scratch/baseline/curated-stashes" "$scratch/final-curated-stashes"
test -s "$scratch/acceptance-report.md"
devtools_status=$(git -C "$devtools" status --short)
test -z "$devtools_status"
```

Expected: no prohibited trailer/flag dependency; only explicitly reviewed tracked changes. Overlay, live shader reload, C-ABI plugins, and broader modularization remain separate future specifications after the visual/gameplay frontier is frozen.

- [ ] **Step 10: Push only the reviewed DevTools branch**

```bash
set -euo pipefail
devtools=/home/monasterios/Documents/PS5/Kyty-devtools
cd "$devtools"
git -C "$devtools" push -u origin codex/runtime-stall-snapshot-v1
```

Do not merge to `main` until Linux acceptance, macOS/Windows compile gates, strict 2x2 equivalence, privacy scan, history audit, and the required untracked acceptance report all pass.
