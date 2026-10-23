# Anime fighter

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Anime fighter: excluded, submit without waiting for decode (2026-10-23)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit,
  fight at ~75 s).** The submitting guest thread spends 47.6% of the wall
  clock in `GraphicsRing::Submit`, waiting until the command processor has
  decoded its command buffer. The command-processor thread itself is idle
  53-59% of the time, waiting for the next batch.
- **Experiment (not kept).** Submit kept the command-buffer snapshot but
  returned without waiting for decode. The ring queue never held more than
  one buffer.
- **Result.** Fight windows fell from 43.5 / 35.6 fps to 34.3 / 26.3 fps.
  The guest now writes the next frame's data while the previous buffer is
  being decoded. Those writes dirty the host snapshots behind
  guest-device-address reads (`PendingReaderForSnapshotRefresh`). Any
  dirty tracked chunk makes the next device-address draw wait for every
  active reader. That reader is often the buffer being recorded, so the
  draw submits it and waits for its fence. `DrawIndexOffset` spent 32.7% of
  the command-processor thread in `CommandBuffer::WaitForFence`, over 15,000
  short waits in 6 s. Two guest threads also spun at 97-99% CPU.
- **Direction.** The decode wait cannot be removed while one host snapshot
  serves every device-address reader. Snapshot refreshes would need their own
  versions, so that a refresh never waits for a reader of the old bytes.
  Until then, the decode cost itself is the lever.

## Anime fighter: write-back restores protection once per run (2026-10-23)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit,
  generic route into a fight).** Temporary counters (removed) over 58,000
  write-backs: the changed pages of one write-back averaged 13.4, and in
  every write-back they formed a single run of consecutive pages. The
  storage range lies in direct memory. The write-back notified each changed
  page separately, so each page took its own `mprotect` back to writable,
  with a TLB shootdown on every core; the rearm after the write-back already
  protected the whole run with one call.
- **Change.** The write-back cache notifies each run of consecutive changed
  pages once. `GpuDirtyPageTracker::NotifyWritePages` claims consecutive
  armed pages with the same guest protection under one restorer announcement,
  records the write on each, and restores the run with one call. A page in
  any other state, or whose guest protection denies writes, keeps the
  per-page path; the restorer is left before that path announces itself.
- **Measured (same route and capture windows, one run per build).**

  | Build | 53-79 s | 79-105 s (fight) |
  | --- | --- | --- |
  | before | 40.9 fps | 34.2 fps |
  | per-run restore | 42.5 fps | 36.2 fps |

  One run per build, so this is an indication, not a measured change. A
  write-back now changes protection twice: one restore and one rearm.
- **Shared-path check (Intel, strict, 75 s, 60 fps present limit).** The card
  title and racing hold the 60 fps limit with no stop. The beat 'em up ran
  25.8 fps in its 27-53 s window (16.0 fps in the previous run's same
  window). The fishing title hit its known intermittent frame-3 boot stall
  (1 draw, 1 dispatch, 2 presents), the signature recorded before this
  change; its write-back ran once and changed no page.

## Anime fighter: async round-trip latency (2026-10-23)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit,
  story scenes).** Temporary per-batch timestamps (removed) showed the
  eight async compute queues strictly serial: never two batches in flight,
  about 29 round trips per frame. Each round trip spent ~6 us dequeued,
  105-140 us decoding, 150-460 us on the GPU and 110-125 us publishing its
  completion. A stack sampler on the ring thread found three host costs that
  do not depend on the guest's work:
  - `GpuDirtyPageTracker::Rearm` walked all 3,840 pages of the 15.7 MB
    storage range after every write-back (17% of the thread). The whole-range
    armed proof fails as soon as one block changes, and the walk that re-arms
    the written pages cannot record a new proof, so every write-back cost two
    full walks.
  - Mapping and unmapping dedicated host-visible allocations around each copy
    changed the process page tables, with a TLB shootdown on every core.
  - Each command stream ran on a helper thread while the ring thread waited
    for it, so every batch paid two thread hand-offs and gained no overlap.
- **Changes.**
  1. A registered range keeps one armed proof per 64-page block: the block's
     change count read before a rearm found every page of the range in it
     armed. A rearm skips proven blocks and walks only blocks whose count
     moved. A page still leaves Armed only through a write, fallback or
     authority change that advances its block, so the proof keeps the
     guarantee of the whole-range proof it replaces.
  2. Dedicated host-visible allocations are mapped once, whole, and stay
     mapped until they are freed, like pooled blocks.
  3. The ring thread runs the constant and draw streams itself; the two
     helper threads per ring are removed.
- **Measured (same route and window, per async round trip).**

  | Build | Decode (even / odd queues) | Completion publish | Batches per 5 s per queue |
  | --- | --- | --- | --- |
  | before | 125 / 170 us | 112-124 us | 607 |
  | + persistent mappings | - | 104-111 us | - |
  | + per-block armed proofs | 105 / 141 us | 84-89 us | 670 |
  | + streams on the ring thread | 80 / 113 us | 83-89 us | 682 |

  `Rearm` falls from 17% to about 3% of the ring thread. Graphics-queue
  frames in the first four 5 s story windows went from 211/206/200/192 to
  220/221/198/193; story scenes differ between runs, so this is a small gain,
  not a measured frame-rate change.
- **Shared-path check (Intel, strict runs).** The card title (262 fps in
  play, no present limit), racing (101 fps), beat 'em up (18.2 fps, was
  13.1) and fishing (22.6 fps, was 21.5) run their routes with no stop. The
  sandbox reaches its world faster and then stopped on a depth slice range,
  a refusal added the same day by the depth-slice change, not by these
  changes (fixed, see "Sandbox title: shadow cascades bound as slice
  ranges").
- **Remaining frame cost.** The graphics queue decodes ~8 ms per frame while
  the submitting guest thread waits, its GPU work runs ~7.5 ms after the last
  flush, and the async chain follows it. The ring thread's largest remaining
  cost is the write-back itself: every changed page is made writable, copied
  and protected again, two `mprotect` calls with TLB shootdowns per page
  (one restore per run since 2026-10-23, see above).

## Anime fighter: write-back copies only GPU-flagged pages (2026-10-22)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit).**
  In the story scenes the frame p50 was 56 ms. About 22 write-backs per
  frame each compared the 15.7 MB storage buffer against its snapshot
  (31 MB read, ~1.2 ms), while only about 13 pages (~53 KB) had changed.
  Write-back took 12.2 s of a 25 s window.
- **Contract used.**
  - A storage `VkBuffer` is created with only storage and transfer-source
    usage, so only shaders write it.
  - Host detile shaders read storage objects and write scratch only.
  - Write-back runs only after `GpuMemoryCanWriteBackAtSubmission`, so no
    writer is in flight.
- **Change.**
  - Every storage object owns a host-visible flag array with one `uint`
    per 4 KiB write-back page.
  - Translated shaders bind the arrays as one more storage binding. The
    SPIR-V pass in `ShaderSpirvStorageWriteMask.cpp` sets the page's flag
    before each store or atomic through `%buf`/`%buf_uint`.
  - The pass stops translation when a storage pointer has a use it cannot
    follow: a copy, a phi, a call argument, or a store through a partial
    access chain.
  - Write-back compares and copies only flagged pages, and clears their
    flags. Adopting a uniform-buffer copy clears all flags.
  - This replaces change 2 below: the three helper compare threads are
    removed.
- **Measured (story scenes, same route, 60 fps limit).**

  | Build | Frame p50 | Write-back time | Per call | Copied per call |
  | --- | --- | --- | --- | --- |
  | before | 56 ms | 12.2 s per 25 s | ~1.2 ms | ~53 KB |
  | flagged pages | 28 ms | 1.2 s per 20 s | 79 us | ~52 KB |

  The copied bytes per call match, so the flags select the same changed
  pages the full compare found. The title screen and the menus run at the
  60 fps limit with no write-back. Captures are visually unchanged.
- **Open (attributed 2026-10-23, see above).** At 28 ms the story scenes
  are still below 60 fps.

## Anime fighter: storage write-back on the label path (2026-10-17)

- **Observation (Intel, native 3840x2160 output).** ~8.3 fps, frame p50
  118 ms. Two guest threads spin at 93% CPU on single loops. The GPU is idle
  ~75% of the time, and rendering at fixed 1080p gives the same 8.4 fps, so the
  title is not pixel-bound. Per 10 s: ~2,000 dispatches, and each writes back
  one ~15.7 MB storage buffer (about 22 per frame).
- **Mechanism.** The end-of-pipe label the guest waits on is written only
  after the completion write-back. Each write-back cost three things:
  - a page compare of 15.7 MB against the write-back snapshot (1.84 ms);
  - a full XXH3 hash of the range (0.46 ms);
  - another full hash at the next use, because the write-back's own write
    notifications advanced the dirty generation and nothing re-baselined it
    (`hash_tracked_unchanged` = 1,871 of 1,871).

  That came to 58.9 GB hashed and 3.45 s of write-back per 10 s, serialized
  with the guest's waits.
- **Changes.**
  1. After a write-back with changed content, a tracked range re-baselines
     its dirty generation, as `Update` already does after an unchanged compare.
     The range is not hashed: a later guest write compares against the unknown
     (zero) hash and uploads its changed pages. Page generations stay stale,
     so label-fence holes are still uploaded.
  2. Ranges of 256 pages or more compare their pages on up to three helper
     threads. The copy and its write notifications stay on the calling thread,
     in page order.
- **Measured (same 10 s window at 55 s).**

  | Build | Frame p50 | fps | Hashed per 10 s | Write-back per call |
  | --- | --- | --- | --- | --- |
  | before | 118 ms | 8.3 | 58.9 GB | 1.84 ms |
  | + generation baseline | 100 ms | 9.9 | 1.2 GB | 1.84 ms |
  | + parallel compare | 83 ms | 11.6 | 1.4 GB | 1.26 ms |

  Captures are visually unchanged.
- **Completion polling.** The compute and graphics rings noticed a finished
  submission only on their next 1 ms condition-variable timeout, so every
  label round trip paid up to 1 ms of idle latency. A 100 us poll (experiment,
  not kept) moved p50 from 83 to 64 ms, which confirmed the cost.
- **Change 3.** Every queue owns a timeline semaphore; each command-processor
  submission signals the next value in submission order. A ring with pending
  work arms a per-ring waiter thread on the oldest submission's value; the
  thread blocks in `vkWaitSemaphores` and wakes the ring as soon as the GPU
  finishes. Completion is still pumped by the ring itself, under its own
  locks, and the 1 ms timeout stays as the backstop.

  | Build | Frame p50 | Frame p99 | fps |
  | --- | --- | --- | --- |
  | parallel compare | 83 ms | 108 ms | 11.6 |
  | + timeline wake | 66 ms | 88 ms | 15.3 |

  Sandbox (terrain, 6-7.5 fps in world), racing (7 captures) and the puzzle
  title (cold-compile window) show the same states and captures as before.
  Verified on Intel only; RADV is untested.
- **Open (resolved 2026-10-22, see above).** The compare reads 31 MB per
  call and is now close to memory bandwidth. Removing the compare needs
  knowledge of which pages the dispatch wrote.
