# GPU dirty-page tracking for graphics uploads

## Context and root cause

`GpuMemory::Update` currently hashes every block belonging to a CPU-visible
resource on each new submission. The GPU-owned surface policy removed this
work for tiled RenderTexture and VideoOut resources, but gameplay still spends
roughly 100-140 ms/s hashing Texture ranges (about 1.0-1.6 GiB/s while active).
The remaining cost is serialized by the global GPU-memory mutex and is large
enough to explain the low gameplay frame rate. The fix must preserve the
existing hash path whenever dirty tracking cannot prove a write.

SharpEmu provides a useful clean-room architectural reference: protect guest
pages read-only, mark the first write, and explicitly notify writes performed
by host code. Kyty must not copy implementation details, and must address
physical aliases, platform page sizes, signal-handler safety, and the race
between upload and re-arming.

## Goals

- Skip full-range XXH64 for CPU-visible resources when every page generation is
  unchanged since the last successful upload.
- Preserve visual correctness for direct guest stores, host/HLE writes, DMA
  destinations, labels, and GPU writeback.
- Keep GPU-owned resources on the existing no-hash path.
- Use actual host page size and preserve the original guest protection mode.
- Fall back to the current XXH64 behavior for unsupported ranges, failed
  protection, metadata exhaustion, platform limitations, or upload failure.
- Make the tracker independent of Vulkan submission/fence lifetime.

## Non-goals

- No change to guest virtual-memory semantics or allocation policy.
- No attempt to make Darwin ARM64 write-fault classification work in this phase;
  those ranges remain hash fallback until the platform signal context exposes a
  reliable write bit.
- No copy of SharpEmu code or private implementation details.
- No submit batching or fence redesign in this phase.

## Architecture

### Platform-neutral tracker

Add a small `GpuDirtyPageTracker` module with fixed-capacity, preallocated
metadata. Each registered range has a `DirtyResourceId`, a mode
(`PageFault` or `HashFallback`), and page records containing an atomic dirty
generation. A resource keeps its own observed generation snapshot; dirty state
is never consumed globally, so two resources or aliases cannot clear one
another's evidence.

The public operations are:

```text
Register(resource, virtual range) -> PageFault or HashFallback
Unregister(resource)
Arm(resource)
HandleWriteFault(fault address) -> handled or untracked
NotifyWrite(address, length)
Collect(resource, spans)
Rearm(spans)
Mode(resource)
Stats()
```

Registration resolves the direct-memory backing identity and offset. All mapped
virtual aliases for a physical page are armed, and a fault through any alias
increments the same page generation. Non-backed or flexible ranges use stable
range chunks and are eligible only when their protection can be restored
without changing guest-visible behavior.

### Fault-handler boundary

The POSIX/Windows exception handler calls only `HandleWriteFault`. The handler
may perform bounded atomic operations, fixed-table lookup, generation increment,
and signal-safe restoration of write access for the faulting page. It must not
take a mutex, allocate, log, hash, call Vulkan, invoke callbacks, or touch
`std::string`, `std::vector`, or unordered containers.

The existing `VirtualMemory::ProtectWriteSignalSafe` primitive is used only to
let the faulting store complete. The tracker stores the original protection and
re-arms it outside the handler with the normal protection API; it never assumes
that forcing read-write is a complete lifecycle operation.

### Upload and re-arm protocol

For a PageFault resource, `Collect` first identifies dirty pages and captures
their current generations. Before reading/uploading those pages, the runtime
re-arms them read-only. The upload then reads the protected snapshot. A page is
recorded clean only after the upload succeeds and only if its generation still
matches the captured value. A write racing with the upload faults after the
re-arm and increments the generation, so it remains dirty for the next submit.
An upload failure leaves all affected pages dirty and selects hash fallback for
that resource until it is safely re-registered.

### Explicit write notifications

Host writes cannot rely on a guest fault. The following destination paths call
`NotifyWrite` (or enter a scoped write-notification helper): libc/HLE
memcpy/memset, file reads into guest memory, CP WriteData, DMA destinations,
constant-RAM dumps, KernelRead output, labels, and GPU-to-guest writeback.
DMA sources are not notified. Notifications happen before a protected range is
read or while a writeback scope temporarily permits writes, and never recurse
through a lock held by the writer.

### Failure policy

If registration, alias enumeration, capacity, page-size handling, or any
protection operation fails, the affected resource is marked `HashFallback` and
is left writable with its prior protection restored. The existing XXH64 update
and check path remains authoritative. A fallback counter and reason are exposed
in debug stats so performance regressions are diagnosable rather than silent.

## Test-first acceptance criteria

Add deterministic tests under `EmulatorGraphicsDirtyTracking.*` covering:

- native write fault marks a page, completes the store, and faults again after
  re-arm;
- original page protection is preserved;
- cross-page writes mark both pages;
- concurrent faults do not deadlock and produce monotonic generations;
- aliases of one physical backing share dirtiness while unmapping one alias
  keeps the other valid;
- unregister/free/reuse does not accept a stale fault (no ABA);
- every host destination path notifies, while DMA source does not;
- writeback under a held graphics lock cannot recurse into the tracker;
- partial and unaligned ranges mark all overlapping owners;
- unsupported platform/capacity/protection failure selects hash fallback;
- failed upload does not mark pages clean;
- observation mode reports no mismatch between watcher dirtiness and XXH64.

The implementation is not considered enabled by default until the tests pass
and a subprocess test exercises a real POSIX write fault. Untracked faults must
continue through the normal exception path.

## Runtime measurement and rollout

Expose counters for hash calls/bytes/time, write faults, explicit notifications,
dirty and re-armed pages, fallback ranges, protection failures, upload bytes,
and alias fanout. Compare baseline and tracker builds with the same Release +
Silent configuration, resolution, cache state, and input: 30 seconds warm-up,
then three 180-second runs. The feature is accepted only if it produces no
false-clean upload or visual regression and improves median gameplay frame time
without worsening p99 frame time by more than 10 percent. If it does not meet
that bar, keep the tracker available for observation but leave XXH64 as the
effective policy.

## Implementation order

1. Add the platform-neutral fixed metadata tracker and unit tests.
2. Add POSIX/Linux and Windows fault-handler adapters; keep Darwin ARM64 in
   fallback mode.
3. Integrate registration and per-resource generations into `GpuMemory`.
4. Add explicit notifications at host write destinations.
5. Run observation mode against the existing hash path, then enable omission
   only for resources with zero mismatches.
6. Measure Dead Cells and the graphics test suite; document results and retain
   fallback diagnostics.
