# Fishing title

Title: PPSA09076 - DREDGE.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Fishing title: per-draw command-processor costs at the dock (2026-11-07, partly fixed)

`PPSA09076 - DREDGE`, first-gameplay route, Intel Arc A770, ANV Mesa 26.2.5, 60 fps limit. The dock is CP-bound:
about 1,170 draws and 27 ms of command-processor time per frame (about 23 us per draw), 34 fps, the GPU 45 % busy.
A `cpu-clock:u` profile with call graphs is flat; no single item passes 6 %. Every A/B below alternates the change
on and off every 10 s inside one run. It compares CP us per draw per window with matched draws per frame. The three
commits below cut about 7 % of CP time per draw, each measured on its own.

- **Probe selectors read on every draw (c0a31996e).** The vertex clip, pixel input and fragment tap probe
  resolvers read their environment selectors on every draw: `getenv` plus its `strncmp` cost 2.2 % of the CP thread.
  The draw paths now check once whether any selector is set and skip the resolvers otherwise; with no selector they
  returned their disabled configs anyway. The resolvers are unchanged and their tests call them directly. A/B
  together with the pipeline index key below: 23.41 ± 0.06 to 22.77 ± 0.06 us per draw (-2.7 %), 33.3 to 34.1 fps,
  10 windows per arm. The lookup statistics of the same run attribute about 0.2 % of that to the index key, so the
  selector check is worth about 2.5 %.
- **Per-draw allocations of input infos and pipeline parameters (321f64f2f).** A Kyty `Vector` allocates even when
  empty, so every vertex input info and every heap copy of the pipeline parameters allocates and clears memory on
  each draw. The draw paths now build the geometry-pass input info only for a geometry pass. Pipeline creation keeps
  the lookup parameters on the stack and copies them to the heap only on a miss. A/B: 23.59 ± 0.14 to
  22.92 ± 0.13 us per draw (-2.8 %), 33.0 to 33.9 fps. That is more than the profile share of the removed calls.
- **Pixel input info reset (7fa9b12d2).** `ShaderGetInputInfoPS` began with `*ps_info = {}`, which builds, copies and
  frees a whole temporary input info. Both draw paths now value-initialize a fresh one and the reset is gone. The
  first A/B ran with load on the host: six of its 20 windows were 1 to 6 us slower, in both arms, and the means
  showed no difference. In its clean windows the change was about 0.3 us faster. A repeat on a quiet host:
  23.85 ± 0.17 to 23.41 ± 0.15 us per draw (-1.8 %), median -0.40 us, 8 of 10 adjacent window pairs faster.
- **Pipeline index key over pass and static parameters (measured, not committed).** The pipeline index hashes only
  the shader pair, so `Find` compares every pipeline of that pair. Adding the render pass and the static parameter
  bytes to the key cut graphics lookups from 412 to 360 ns but raised compute lookups from 298 to 371 ns (hashing
  the parameters). What remains is hashing and pointer chasing, not the candidate walk; about 0.2 % of the CP thread.
- **Pixel usage memo (measured, not committed).** About half of the pixel `ShaderParseUsage2` calls repeat within a
  frame with the same program, user SGPRs and descriptor words. A memo keyed on those inputs, revalidated by
  re-reading every descriptor word the parse read, matched production output on 536,933 hits, but saved only
  about 1.3 % (under two standard errors): copying the 7.2 KB bind resources and the reads log costs half of what
  the parse saves.
- **Descriptor set misses (handed to the transient pool owner).** About 25,000 descriptor set misses per second
  against 37,000 hits. Of those misses, 97 % combine storage buffer ids that were all seen before. A recaptured
  snapshot key takes another key's oldest released entry, so the combinations seldom recur. Taking the key's own
  stale entry first changed nothing (no free stale entries exist in those chains).
- **Remaining per-draw costs (same profile, before the change).** Pixel input info 14.7 % (its parse 10.1 %),
  pipeline lookup 4.7 % (`Find` 1.8 %, shader ids 1.9 %), guest device address preparation 3.9 %, transient
  read-only uploads 3.4 %, descriptor binding 3.2 %, allocation 4.9 % (empty Kyty vectors allocate) and zeroing
  2.9 % (each input info clears about 7 KB of bind resources). The two commits above remove part of the last two.

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
  same symptom (2026-10-12 and 2026-10-13). Cause found on 2026-10-23: a
  collector context request delivered while the thread held a host lock
  (cross-title record "Collector context requests delivered only at safe
  points").
