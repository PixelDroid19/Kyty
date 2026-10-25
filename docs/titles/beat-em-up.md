# Beat 'em up

Title: PPSA28311 - Absolum.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Beat 'em up: device-address write-back waits (2026-10-14, open, measured)

- Workload: the beat 'em up, strict release build, Intel Arc A770, scripted
  combat route, perf-snapshot window at 150 s. 17.8 FPS (56 ms per frame):
  command processor 44.7 ms, fence waits 5.2 per frame for 16.3 ms, GPU busy
  about 55%. The processor and the GPU take turns instead of overlapping.
- Attribution (temporary capped diagnostics, removed): in combat almost all
  fence time is the device-address draw retry
  (`RetryGuestDeviceAddressDraw` → `Gpu::WaitSubmission`,
  `GraphicsRun.cpp:2030`). Every retry comes from
  `GuestDeviceAddressPendingWriteBack`; the in-flight reader path of the
  snapshot refresh never fired (0 of 240,000 preparations).
- Update (ddd43522): the survival title's uniform depth-plane fill
  publication was not verified to apply here. The storage buffer below has
  the size of a 3840x2176 32-bit plane, but its compute shader is not known
  to write a uniform value. Combat route against the previous build: 4.0
  against 4.9 fence waits per frame, 23-29 against 23-25 ms, and about 1.03
  storage write-backs per draw in both (350-466 per frame). The write-back
  volume is the open cost.
- Pending objects, all produced and consumed on the graphics queue:
  - a 33,423,360-byte read-write storage buffer (16-byte stride, 2,088,960
    records) bound at index 0 of compute shader `0x5f385baf`; it crosses an
    older depth target. In combat it causes about 60% of the waits at about
    16 ms each;
  - 144-byte storage buffers aliased by an equal vertex buffer and contained
    in larger storage buffers, all older than the write. They dominate before
    combat and wait about once per frame.
- Ceiling experiment (diagnostic only, never a candidate): skipping every
  pending object in `GpuMemory::CollectWritableStorage` raised the window to
  23.2 FPS, but every capture was black, so the device-address readers do
  consume this GPU-written content. Skipping only objects of 16 MiB or more
  gave 17.8 FPS: the 144-byte objects then absorb the whole queue backlog, as
  in the sandbox title's exclusion below. Any fix must remove every
  write-back wait of a frame to change its frame time.
- Direction: publish pending storage results into the device-address view in
  queue order on the device (as `GuestDeviceAddressRecordPublication` does for
  uniform fills), without exempting the object from the ordinary write-back
  that its aliases rely on. Any CPU-side read of a published range, such as
  an alias reload or a hash, must wait for the publishing submission. The
  write-back side effects that other consumers depend on (alias invalidation,
  depth metadata) are what regressed the 2D action title when an exemption
  replaced them.
