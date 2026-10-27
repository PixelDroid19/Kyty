# Beat 'em up

Title: PPSA28311 - Absolum.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Beat 'em up: black frames in logos, cinematic and loading screens (2026-10-27, fixed)

- **Observed** (Intel Arc A770, Mesa 26.2.5, strict runs at the 60 fps present
  limit, no input, 75 s, native capture about every 15 presents): whole frames
  presented black at random through the publisher logos, the intro cinematic
  and the loading screens. 131 of 206 captures were flat (4 colour bins or
  fewer) on the previous build.
- **Cause.** Each frame a compute shader writes the quads of the full-screen
  passes into a 288 KiB vertex ring (36-byte records) as writable storage, and
  the draws fetch them through a vertex V#. Writable storage in use refuses
  the transient vertex snapshot, so the bind falls back to the vertex object
  over the declared stream, which is seeded from guest bytes on the host.
  Guest memory holds the quad only after the storage write-back. When the
  dispatch had completed before the draw was prepared, the write-back ran
  first and the frame was right; when it had not, the result reached guest
  memory only through the device-address publication on the GPU, the vertex
  object kept the stale bytes (zeros), every vertex landed at the same
  position, nothing rasterized and the frame was black. Temporary
  diagnostics (removed) tied 342 of 342 black draws to the GPU-only
  publication and 190 of 190 good draws to a CPU write-back before record.
- **Fix** (general contract; `PrepareVertexStreamsForRead` in
  `GraphicsRenderDraw.cpp`, `CopyPendingStorageIntoVertices` in
  `GraphicsRenderBind.cpp`, `GpuMemoryWriteBackStorageForGuestSeed` and
  `GpuMemoryCollectStorageForGuestSeed` in `GpuMemoryWriteback.cpp`): a vertex
  object seeded from guest bytes now sees every writable storage result that
  overlaps its stream. Draw preparation writes back the completed results; a
  result whose pending uses are all on the draw's queue is copied on the
  device into the vertex object, ahead of the draw in queue order, and the
  draw becomes a use of the source so its write-back and release follow the
  copy; a result pending on another queue, or spread over several blocks, is
  a dependency the draw waits for.
- **Excluded on the way:** CPU flips and VideoOut, the video player, cross-queue
  order, fills, blending and aliases; the synchronization validation layer
  (clean); driver fast clears (`INTEL_DEBUG=nofc`); command-buffer snapshot
  clobbering; record-time draw state (identical in good and black frames);
  re-binding the draw; the device-address lookup (replays correctly); the
  draw matrices; a CPU write to the ring (a page-protection trap never fired).
  Pipeline statistics showed the black draws reach the clipper with zero
  fragment invocations.
- **Excluded fix:** making every pending result a dependency (wait, then
  write back) also removed the black frames, but cost one wait of about 7 ms
  per frame (1,700 waits and 11 s of waiting in a 60 s run).
- **Verified** (same host and settings). Pipeline statistics on the ring's
  target passes (temporary, removed): 0 of 2,114 draws without fragment
  invocations, against 342 of 532 before. Captures: 3 to 5 flat of about 190
  per run over four runs, at boot, at the scene cuts that recur at the same
  point of the intro (one run also caught one flat frame in a dark stretch of
  the cinematic; the statistics above exclude this cause for it). Frame rate in 10 s agent watch windows over the
  same stretch of the intro, previous build against the fix: 467/478/458/423
  against 429/511/505/382 presents. Draw processor cost on the
  `PPSA09076 - DREDGE` menu, two runs each: 77.2 and 86.7 against 84.1 and 72.1 us per
  draw, within run-to-run noise. Strict `games-matrix` sweep: same outcome
  for every case as the previous build (23 of 32 interactive; the
  difference from 66bee914 is the intermittent boot fault of
  `PPSA03969 - F.I.S.T.: Forged In Shadow Torch` recorded in
  `docs/bringup/cross-title.md`). Repository gates and the GPU memory,
  vertex, storage, write-back and device-address unit suites (205 tests)
  pass.

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
