# Fishing title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Fishing title: gray rectangles behind the first dialogue portrait (2026-10-23, checked)

- **Observation.** In the first character dialogue, a tan panel behind the
  character shows flat gray rectangles.
- **Checked.**
  - The rectangles are opaque in the flip image (alpha 255), so the capture
    blend fixed in the anime fighter record does not explain them.
  - They have different sizes and are each rotated by a different angle
    inside an axis-aligned panel. Corrupted texture blocks or render-target
    tiles would form an axis-aligned grid, so this does not look like
    corruption; it matches drawn content such as pinned paper notes.
- **Open.** Not compared with a reference image of the same scene. Compare
  before spending a run on it.

## Fishing title: command-processor CPU costs (2026-10-14, partly fixed)

- Context: on Intel the fishing title runs its dock scenes (the scripted
  route ends at the dock dialogue and the undock prompt) at 10-11 FPS with host CPU and GPU
  both far from saturated. The emulated command processor is one thread per
  guest queue, as on hardware, and that thread is the limit: about 100 ms of
  command-processor time per presented frame with a stack sampler attached.
- Flat profile of that thread at the dock (release build, strict run, the
  scripted route): `GpuMemory::FindBlocks` 9.3%, `DescriptorCache::UnindexSet`
  8.9%, driver descriptor-set allocation 6% plus 1.3% free,
  `GpuMemoryClassifyRangeSets` 3.7%, `ResetHash` 3.6%,
  `LabelFenceRegistry::Snapshot` 3% (`LabelStoragePublication::Upload`, one
  linear pass over every registered fence hole per storage upload), dirty-page
  `Rearm` 2.9%.
- Fixed: the per-resource index of live descriptor sets held a vector per
  resource and removed a set with a linear erase, so a frequently bound image
  or buffer made every unindex O(sets referencing it). The index is now a hash
  set per resource; `FreeSetsReferencing` still frees in ascending index order.
  `UnindexSet` disappears from the line profile. ABBA on Intel, 200 s per run,
  the last four capture intervals: baseline 11.0 / 9.3 FPS, change 11.25 /
  11.25 FPS. Regression runs with the change, scripted routes, the last four
  captures checked by index: the racing title on Intel 38-42 FPS racing; the
  beat 'em up on Intel renders its intro, dialogue and walking scenes correctly
  (its route desynchronized between runs, so its frame rates are not
  comparable); the 2D action title on AMD reaches gameplay with no HTILE trails;
  the fishing title on AMD 7.3-7.7 FPS at the dock dialogue, which the slower
  host reaches later (5.0-7.3 FPS there before the change). No fatal stop.
  `scripts/kyty_playable_regression.py` cannot run on this host (no private
  profile or guest root).
- Excluded: remembering up to eight recently scanned clean ranges in the
  materialization cache so that a repeated `InvalidateRange` returns before its
  2048-span scan. ABBA on Intel against the fix above: 11.73 / 11.97 FPS with it
  and 11.83 / 11.67 FPS without it. The scan is not on the critical path at this
  frame rate.
- Ten-second agent window after the fix (Intel, at the dock): 11.3 FPS, 82.5 ms of
  command-processor time per frame, that thread at 73% of one core (3% of it in
  the kernel), 1157 draws per frame (about 71 us each). Of the 82.5 ms, 15.7 ms
  are fence waits (32.6 per frame) and 11.1 ms `WAIT_REG_MEM` (212 per frame):
  a third of the frame is the command processor waiting for the GPU, which is
  why neither the host CPU nor the GPU shows high utilization. Resource binding
  takes 39 ms (texture descriptors 17 ms), object lookups 19.7 ms for 6768
  calls (nearly all cached or fast reuse, not object churn), and uploads move
  47 MB per frame; the slowest creates are 8.3 MB textures re-uploaded on fast
  reuse at 15-20 ms each. On AMD the same thread is 60% busy, 19% of it in the
  kernel.
- Remaining ranked costs and directions, not yet changed: `FindBlocks` from
  `CanSnapshotReadOnlyBuffer` and `CaptureSnapshotReadOnlyBuffer`
  (`GpuMemoryFind.cpp:371` and `:413`) spends most of its time sorting long
  overlap lists from 1 MiB page buckets that hold many small objects; measure the
  overlap count per query before choosing an index. Descriptor-set recycling is
  now done (see the per-draw section below). The fence-hole registry is
  unsorted (`Objects/Label.cpp:113-252`); a sorted index with the longest hole
  length bounds each overlap query.
- Fixed in part: write-back waits. A `WAIT_REG_MEM` on a label that the
  current submission writes was consolidated into one barrier only for a plain
  label store; a store whose `RELEASE_MEM` also writes back caches (about five
  per frame, ~2 ms each) still submitted and waited. Ceiling, a diagnostic build
  that consolidated every current-producer wait regardless of effects: 11.3 to
  13.1 FPS, `WAIT_REG_MEM` 11.1 to 0.03 ms and fence waits 15.7 to 7.1 ms per
  frame, the dock captures unchanged. Producer effects over that run: 284672
  plain stores, 20328 store plus write-back, no notification. A second
  diagnostic snapshotted the pending writable storage ranges at each forced
  write-back consolidation and counted later object lookups in the same batch
  over them: only depth-stencil objects (in a quarter of the consolidations);
  no texture, vertex, index or render target. Guest-memory readers (memory
  reads, GDS transfers, device-address views) already wait for pending storage
  themselves, so the synchronous wait only protects GPU objects that alias the
  storage, which the write-back refreshes or invalidates. The rule now
  consolidates a store plus write-back when no pending writable storage has an
  alias (`GraphicsWaitRegMemCanConsolidateCurrentProducer`,
  `GpuMemoryPendingStorageHasAliases`); aliased cases and notifications stay
  synchronous. Window with the rule: `WAIT_REG_MEM` 1.6 ms and fence waits
  13.5 ms per frame; the last capture intervals 10.8 / 12.7 / 11.7 / 12.3 and
  11.7 / 13.2 FPS against 10.5 / 11.8 / 10.9 / 11.8 and 10.7 / 12.1 / 11.5 /
  10.7 FPS for the previous build, a gain of about 0.7 FPS that one ten-second
  window (11.0 FPS, slower uploads) did not show. Regression with the rule:
  the beat 'em up reaches gameplay in a 300 s run and renders its combat scenes
  correctly (no gain there, its waits are device-address write-backs); the
  racing title 41.3 / 40.6 FPS against 41.3 / 38.7; the fishing dock captures
  match by index. Unrelated and pre-existing: some fishing-title launches never
  present a first frame. The build before the rule failed in 2 of 12 launches
  and presented in 5 of 5 short boot checks; three earlier builds showed the
  same symptom (2026-10-12 and 2026-10-13). Cause unknown.
