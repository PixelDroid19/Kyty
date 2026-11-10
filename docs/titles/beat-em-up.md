# Beat 'em up

Title: PPSA28311 - Absolum.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Beat 'em up: the vertex container search walked every block of each stream (2026-11-10, fixed)

- **Observed** (Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.14, first area,
  `scripts/input/reach_first_gameplay.pad`, 60 fps limit, 60-115 s window;
  fps is flips per one-second sample, averaged over the window):
  - `40d8c109f` with `59e3640cf`, Release without link-time optimisation:
    39.4 fps; the command processor ran 23.9 ms per frame, 8.2 ms of it
    binding vertex buffers.
  - Both ends of the range with link-time optimisation, same host and driver:
    `26be1584d` 48.6 fps with 0.56 ms of vertex buffer binding per frame,
    `657613887` 36.4 fps with 9.43 ms. The command processor grew by 7.2 ms
    per frame.
- **Localized** with a 5 s `perf` sample of on-CPU time at about 75 s:
  `GpuMemory::FindVertexContaining` held 29.6 % of all samples, 22.4 % in its
  own body, and `GpuMemory::FindBlocks` 8.5 %. The cost is the walk, not
  lock waits.
- **Cause** (`51e107dbf`, `00558ea73`): before creating a draw's vertex
  object, `BindVertexBuffers` asks for the live vertex object that holds the
  declared stream. The search walks every block the stream overlaps and reads
  each object, to check that one vertex object covers the range. This title
  binds each pool from a fixed V# base, so the object of its exact stream
  already exists (the reuse path of "Vertex objects cover the declared
  stream", `docs/bringup/cross-title.md`), and the walk only confirmed it.
- **Fix** (`799fb76fd`, `GpuMemory::FindVertexContaining` in
  `GpuMemoryWriteback.cpp`): the exact object is looked up by start address
  first; the block walk runs only when it is missing, so a stream inside an
  older object still binds into that object.
- **Result** (Release without link-time optimisation, one run, same window):
  39.4 to 60.1 fps, the present limit; command processor 23.9 to 13.2 ms per
  frame; vertex buffer binding 8.18 to 0.45 ms. Upload and hash bytes per
  frame change by less than 4 %. With link-time optimisation, `799fb76fd`
  ran 58.7 fps against 36.4 for `657613887` (command processor 26.0 to 14.4
  ms per frame). Captures at 60 and 115 s are correct. The
  scene at 115 s varies between runs of this route: earlier runs stopped at
  the camp, in a cave or on a bridge.
- **Shared path** (`PPSA21567 - Astro Bot`, user-recorded dunes route,
  Release without link-time optimisation): vertex buffer binding stays at
  0.11-0.12 ms per frame over 60-80 s, where both runs show the same content:
  22.85 fps against 22.4. Of two runs with the change, one left the route at
  about 84 s and reached content whose compute pipelines took 3.8-4.4 s each
  to create. The other reached the dunes with correct captures. The replay
  follows wall clock, so the first run is recorded as a route desync, not
  attributed to the change.
- **Verified:** the `KytyUnitTest` suite passes; no unit test reaches this
  lookup.
- **Other titles** (earlier runs of `59e3640cf`, same window): vertex buffer
  binding is 0.92 ms per frame in `PPSA09076 - DREDGE`, 0.11 ms in
  `PPSA09809 - OCTOPATH TRAVELER II` and 0.22 ms in `PPSA21402 - Balatro`, so
  the remaining walk is not a bound there. The change was not run on them.
- **Open:**
  - The advancing-V# pool that `51e107dbf` targets still walks the blocks.
    No record names its title or its gain.
  - `26be1584d` ran 58.6 fps on 2026-11-07 (Mesa 26.2.5) and 48.6 fps here.
    The 58.6 and 58.7 fps runs stayed at the camp and the 48.6 fps run
    reached a cave, so the gap looks like scene, not driver; not measured.

## Beat 'em up: every in-place uniform fill was written twice to guest memory (2026-11-07, fixed)

- **Observed** (Intel Arc A770, Mesa 26.2.5 on Xe, first area, generic pad route,
  60 fps limit, 80-120 s): 39.6 fps, GPU busy 94-95 %, about 10.7 ms of fence
  wait per frame. Only about 5 ms per frame was guest draws and dispatches.
- **Measured** with a DIAG that timestamps each pool command buffer at begin, around
  every guest draw, dispatch and detile kernel, after every transfer, clear,
  barrier and render pass end, and at end. These marks serialize the GPU, so
  read the shares, not the absolute times. Of 26.2 ms of busy GPU time per frame:
  - 7.8 ms went to the fill transfer of proven uniform fills
    (`RecordComputeUniformFillTransfer`) plus the barrier after it;
  - 7.8 ms went to the second fill of the same range in
    `GuestDeviceAddressRecordFill` plus its after-barrier;
  - 4.7 ms went to about 1650 guest memory barriers (`GraphicsRenderMemoryBarrier`),
    an amount the marks inflate;
  - 4.0 ms went to indexed draws, and 1.1 ms to dispatches.
- **Cause**: about 480 fills per second, 7.3 GB/s, up to 33,423,360 bytes each.
  Every fill targets an object bound in place: memory flags 0xe (host-visible,
  coherent, cached, not device-local). The first fill already writes the guest
  pages. The publication then filled the same pages again through the
  publication pieces, which are a second import of the same physical memory.
  That is about 180 MB per frame over PCIe, twice.
- **Fix**: an in-place object skips the second fill and publishes through the in-place
  branch of `GuestDeviceAddressRecordPublication`, which drops overlays and
  records the barriers. The guest-published marking is unchanged.
- **Result**: one product run each.
  - PPSA28311 - Absolum: 39.6 to 58.4 fps (+47 %); p95 frame time 27.2 to
    20.2 ms; fence wait 10.7 to 2.2 ms per frame; same 335 draws per frame.
    Captures at 60, 90 and 115 s match the earlier ones.
  - PPSA09076 - DREDGE: 22.04 to 21.76 us/draw, 33.5 to 34.8 fps (within noise).
  - All 108 tests pass.
- **Open**:
  - The first fill still writes about 180 MB per frame to host memory.
  - The guest memory barriers (ALL_COMMANDS to ALL_COMMANDS, about three per guest
    event) are not sized yet without the DIAG.

## Beat 'em up: every draw scanned and copied a whole shared vertex pool (2026-10-31, fixed)

- **Observed** (Intel Arc A770, Mesa 26.2.5, first area, generic pad route, 60 fps
  limit): 13-15 fps against 37.8 fps on a 2026-10-25 build, same 335 draws and 345
  dispatches per frame. Per frame: command processor 24.7 to 74.5 ms, draw state setup
  1.4 to 24.4 ms, vertex buffer binding 1.0 to 22.2 ms, render pass begins 10 to 105.
- **Cause.** One vertex object covers a declared stream shared by many draws, and the
  fix for the black frames below treated that whole stream as read by each draw. A
  temporary counter (removed) found about 190 pending storage results per copy, about
  160 bytes each. Each draw scanned them, copied each one behind its own barrier outside
  the render pass and became a use of each, which also lengthened the device-address scan.
- **Fix** (`ResolveVertexStreamRead` in `GraphicsRenderBind.cpp`, used by
  `BindVertexBuffers` and `PrepareVertexStreamsForRead`): indexed draws with an index
  snapshot resolve their record range first and limit the seed and the copy to the bytes
  they fetch. Non-indexed draws, depth copies, geometry passes, draws without a snapshot
  and tail fetches keep the whole range, since their fetch range is unknown or was not measured.
- **Verified.** First area, three runs on the two steps: 32.7 fps with only the copy limited,
  40.5 and 40.7 fps with both; 10 render pass begins per frame; picture correct.
  Seed 19 to 1.5 us per draw, copy 64 to 0.1 us per draw. The black-frame test below, one run with
  no input and 75 s: 8 flat of 157 captures, all the boot loading screen or a white flash in the
  cinematic. `PPSA09076 - DREDGE` dock 24.4 fps, picture correct. `PPSA21567 - Astro Bot` title
  screen 21.1 fps, picture correct, no vertex object used there.
- **Open.** No source fell inside a draw's range in any of these runs: the intro's vertex ring
  now binds its storage object directly, so the copy itself did not run. It stays covered
  only by its use of the range the transient snapshots already rely on.

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
