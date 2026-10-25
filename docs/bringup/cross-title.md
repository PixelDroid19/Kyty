## GDS is shared across queue families (2026-10-24)

- **Observed.** After compute queues moved to a compute-only family, a title
  that initializes GDS on a compute queue and later binds it on the graphics
  queue stopped at boot with `GDS queue-family migration is unsupported`.
- **Cause and fix.** The GDS buffer is created through the common buffer path,
  which already applies concurrent sharing across every used family; the
  first-use family check still treated it as exclusively owned. The check and
  the owner field are removed.
- **Verified.** A strict boot on Intel A770 passes the publisher card, the
  studio logo and the title, and loads the intro level.

# Cross-title records (2026-10-10 onward)

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Render-target scaling on the AMD integrated GPU (2026-10-25)

- **Question:** the AMD Radeon 610M (RADV, 2 compute units) ran the slowest
  titles at 9-19 fps in strict runs, which force `Native`. The preschool
  Unity title's command processor spent 43 ms per frame in fence waits,
  almost all from `RetryGuestDeviceAddressDraw` waiting for a 192 KiB
  HTILE-linked storage buffer that overlaps a device-address range
  (`StorageIsWholeGuestContent` refuses depth metadata, so it is not
  published on the device).
- **Not the bound:** a removed diagnostic that published those buffers
  anyway removed the waits but left the title at 17.1 fps against 18.0. The
  wait only moved the command processor's idle time; the GPU is the bound.
  The exclusion stays.
- **Measured** at `Fixed` 1280x720 with `RenderResolutionScope =
  RenderTargets`, one user-recorded route per title, against the strict
  `Native` runs:
  - preschool Unity title, outdoor gameplay: 20.6 fps against 17.8;
  - dungeon crawler gameplay: 30.9 fps against 14.5;
  - well platformer gameplay: 24.3 fps against 19;
  - fishing title: 11.2 fps against 11, CPU-bound (see its title record);
  - first-person puzzle, first room: 11-12 fps against 9, and 12-13 fps at
    640x360, so its bound is GPU work that the display extent does not
    scale.
- Captures stay correct and no run stopped. The scope stays opt-in; the
  launcher's Quality, Balanced and Performance presets select it.

## Vertex objects cover the declared stream (2026-10-24)

- **Observed** (beat 'em up gameplay, Intel A770, strict runs at the 60 fps
  present limit, replaying a user-recorded route that reaches combat at
  about 79 s): vertex bindings over GPU-written storage cannot use a
  transient snapshot and create a GPU vertex object instead. Temporary
  counters (removed) over 100,000 such binds: two vertex V#s, each a
  288 KiB pool, gave about 5,700 distinct windows each, because every draw
  shifts the stream base by its vertex offset and ends it at its own last
  record. Each window was a new object that reclaimed the crossing peer:
  25,582 objects for 100,000 binds.
- **Fix** (`BindVertexBuffers`, `GraphicsRenderBind.cpp`): when the declared
  stream (stride times records) is at most the read-only transient limit
  (4 MiB), the vertex object covers the whole declared stream from the V#
  base, and the draw's shifted start becomes the bind offset. Every draw of
  the pool then takes the exact-match reuse path. Fetches stay inside the
  records the V# declares. Larger streams keep the per-draw window.
- **Result:** 239 vertex objects for 100,000 binds. Over the same replayed
  route, the gameplay window between the 60 s and 120 s status reports
  presented 1,824 frames against 1,674 (+9%), and the run ended at 35.3 fps
  against 31.8. Captures match frame for frame in content.
- **Verified:** all 113 tests pass.

## In-memory database name ends with an empty parameter list (2026-10-24)

- **Observed** (Linux, executables linked with LLD): `GpuMemory::DbgInit`
  stopped at `GpuMemoryDbg.cpp:42` because `Connection::CreateInMemory`
  failed. `sqlite3_open_v2(":memory:")` returned `SQLITE_ERROR` with
  "unknown cipher 'textkey'". `sqlite3PagerFilename` returned a `""` literal
  for an in-memory database, and `CodecConfigureFromUri` passed it to
  `sqlite3_uri_parameter`, which reads the URI parameters after the name.
  The bytes after the literal belong to whatever string the linker placed
  next: with bfd it was a zero, with LLD's merged literals it was a
  `cipher`/`textkey` pair. 14 of 113 tests failed this way.
- **Fix** (`3rdparty/sqlite/src/sqlite3.c`): the in-memory name is a
  zero-filled static array, so the parameter list after it is empty
  whatever the linker does with literals.
- **Verified:** with LLD all 113 tests pass, including the graphics
  diagnostics integration.
- **Open (build):** the executables rely on bfd exporting every symbol that
  a linked shared library also defines (4471 dynamic symbols in
  `fc_script`). Kyty replaces `operator new`/`delete`
  (`lib/Core/src/MemoryAlloc.cpp:784`) and links libstdc++ statically; LLD
  exports 19 symbols by default, so a block allocated on one side was freed
  on the other and 62 unit tests stopped at the `mem_block_magic` check
  (`MemoryAlloc.cpp:91`). Passing a `--dynamic-list` of the dependencies'
  defined symbols restores the same export set and every test passes.
  Direction: make the export explicit in the build (for example a
  generated dynamic list) before LLD becomes a supported linker.

## Range-cache span index links slots in place (2026-10-24)

- **Observed** (anime fighter, Intel A770, strict run at the 60 fps present
  limit): with the ordered-set span index of the record below, the two range
  caches held 4.1% of the CP thread inclusive, and the set's tree insert and
  rebalance 5.5% as a leaf, because every store and invalidation allocated or
  moved a tree node.
- **Fix** (`GpuMemoryRangeQueryCache.h`): each length class hashes span
  starts into 1,024 buckets as wide as the class bound, and slots are linked
  into their bucket in place (one link per slot, no allocation). A range
  visits the buckets that can hold an overlapping start, at most 1,024 per
  class, plus the single list of spans longer than 256 MiB; the caller still
  checks every candidate span as before.
- **Result:** in the fighter the range caches fell to 1.3% inclusive and the
  tree leaf to 1.5% (what remains belongs to other users of `std::set`).
- **Verified:** unit tests pass (2465, including the two span-index cases of
  the record below), all 113 tests pass. Sweep with no stop: racing 60 fps,
  anime fighter 53.1 fps in the tutorial (45.7-48.9 before), isometric
  roguelike 60 fps, insect platformer sequel 60 fps, fishing dock 21.6 fps
  (inside its 19.1-23.6 band), beat 'em up intro cutscene 25.8 fps
  (video-paced, 23-29).

## Storage page generations are read only for an upload (2026-10-24)

- **Observed** (beat 'em up, Intel A770, strict runs at the 60 fps present
  limit): `GpuDirtyPageTracker::PageGenerations` held 4.3% of the CP thread
  in gameplay and 23.9% in a window of the intro cutscene, always under
  `GpuMemory::Update` from `CreateObject`. `Update` read one generation per
  4 KiB page of every tracked storage buffer it checked, before knowing
  whether the buffer changed; a clean buffer returned without using them.
  The upload itself (`update_func`) took 0.5% of the gameplay profile.
- **Fix** (`GpuMemoryCreate.cpp`): the generations are read after the
  unchanged-buffer return, still after `BeginRead` armed the pages and before
  the upload, so the changed-page runs and the stored generations are the
  same as before.
- **Result:** `PageGenerations` no longer appears in the cutscene profile. In
  that cutscene the CP thread is mostly idle and the frame rate (23-29 fps)
  did not move; no gameplay window was reached in the measured runs.
- **Verified:** unit tests pass (2465), graphics diagnostics integration
  passes (15). Sweep with no stop: racing 60 fps, anime fighter 45.7 fps in
  the tutorial (45.7-48.9 in earlier runs), fishing dock 20.3-20.4 fps (19.1-23.6
  across today's runs on earlier builds).

## Range-query caches invalidate only overlapping spans (2026-10-24)

- **Observed** (beat 'em up gameplay, Intel A770, strict run at the 60 fps
  present limit): after the record below, `BindVertexBuffers` held 28% of the
  CP thread and its vertex-buffer `CreateObject` 19.5%, with `Free` 7.4% and
  `CreateBlock` 4.3%. Temporary counters (removed) showed about 90% of vertex
  bindings refusing the transient snapshot because the range overlaps a
  writable storage buffer (compute writes the vertex data), so the binding
  creates and reclaims objects every draw. Each block create or delete
  invalidated the overlap-snapshot cache (4,096 slots) and the per-heap
  adaptive cache (up to 4,096) by scanning every slot: about 3.94 us per call
  for the two together, plus 1.02 us in the materialization cache.
- **Fix** (`GpuMemoryRangeQueryCache.h`): `GpuMemoryRangeSpanIndex` keeps the
  slots of both range caches in ordered sets by span start, one set per span
  length class (4 KiB, 64 KiB, 1 MiB, 16 MiB, 256 MiB, longer). A class of
  spans at most `bound` bytes long can only overlap a range from a start
  within `bound` bytes below it, so invalidation visits only those slots.
  Store, invalidation and `Reserve` keep the index in step with the slots.
- **Result:** the two range caches fell from 3.94 to 0.18 us per block call
  in the intro cutscene; in gameplay (three times the block churn) all three
  caches together cost about 1.35 us per call, against about 5 us before.
- **Excluded:** the same index in `GpuMemoryMaterializationCache`, replacing
  its clean-range replay of the store log. Recording invalidates the same
  attachment ranges every draw, and the writer's own entries survive those
  invalidations, so every call walked all of them: in the racing title the
  index walk took about 42% of the CP thread and the run fell from 60 to
  12 fps (anime fighter 49 -> 32, isometric roguelike 60 -> 48). The
  materialization cache keeps its clean-range replay.
- **Verified:** unit tests pass (2465, with two new range-cache cases: a long
  query invalidated from its last byte in every length class, and a replaced
  query invalidated only by its own range), graphics diagnostics integration
  passes (15). Regression sweep with no stop: racing 60 fps, anime fighter
  46.7 fps in the tutorial (more frames than the previous sweep), isometric
  roguelike 60 fps; the insect platformer sequel held 60 fps.
- **Open:** vertex bindings over GPU-written storage still create and reclaim
  objects every draw (`multi_vertex_mixed` peer reclaim); that churn, not the
  invalidation, is the remaining cost.

## Dirty-page tracker admits 4 GiB of live ranges (2026-10-24)

- **Observed** (beat 'em up intro cutscene and gameplay, Intel A770, strict
  runs at the 60 fps present limit): after the record below, XXH3 still held
  13.6% of the CP thread in the cutscene and 20.7% in gameplay, under
  `PrepareStorageBuffers` -> `GpuMemory::CreateObject` -> `Update`. Temporary
  counters (removed) showed about 500 storage-buffer hashes per 2 s window
  over ranges of 2.5-32 MiB (about 11 MiB on average), 5-6.6 GB hashed per
  window, every one unchanged and none of them registered with the dirty-page
  tracker. `RegisterRange` refused them in `AdmitPages`: all 524,288 page
  identities were live (no unreferenced, retired or revoked identity to
  reuse) across about 11,600-13,400 ranges. The title keeps more than 2 GiB
  of tracked ranges live, and a refused range is hashed whole on every use.
- **Fix** (`GpuDirtyPageTracker.h`): the page table grows from 1,048,576 to
  2,097,152 slots, so the admission budget is 1,048,576 pages (4 GiB on a
  4 KiB host). The table costs 56 MiB more host memory; full-table scans
  (large write spans, revoked-identity release) visit twice the slots.
- **Result:** with the larger table the same cutscene showed no refusal and
  storage hashing fell from 5-6.6 GB to almost nothing per window. In the
  gameplay profile XXH3 fell below 0.6% of the CP thread (from 20.7%), and
  `GpuDirtyPageTracker::PageGenerations` (4.3%) took the place of the hash;
  the camp area of gameplay held 35.8-36.7 fps (27-31 fps in gameplay before;
  one run, a foreign build overlapped its start). The intro cutscene read
  23.6-23.8 fps (21-24 before).
- **Verified:** unit tests pass (2463; the lookup tests that mirror the table
  size were updated), graphics diagnostics integration passes (15).
  Regression sweep with no stop: racing 60 fps (56.8 at the lowest sample),
  anime fighter 48.9 fps in the tutorial (about 48 before), isometric
  roguelike 60 fps, insect platformer sequel 60 fps (ends on the black load
  after the prologue, as the previous run did).
- **Open:** a title that keeps more than 4 GiB of tracked ranges live is
  refused again; the refusal is not reported outside temporary counters.

## Physical device-address scans visit only partly imported ranges (2026-10-24)

- **Observed** (beat 'em up intro cutscene, Intel A770, strict runs at the
  60 fps present limit): after the two records below, `std::_Rb_tree_increment`
  still held about 21% of the CP thread under `GuestDeviceAddressPrepareForRead`.
  Temporary counters (removed) over a window of heavy churn showed 419 full
  physical scans visiting 4.02 million ranges, 99.6% of them already fully
  imported (328 ms of walking); in steady cutscene windows about 700 scans per
  window, each started by a change of the physical-backing population. A list
  of ranges not scanned since the last population was tried first and did not
  help, because the population changes on almost every registration.
- **Fix** (`GuestDeviceAddress.cpp`): the registry keeps the set of physical
  ranges with pages not imported yet. A registration, a release survivor or a
  chunk drop that clears imported pages adds the range; a release removes it;
  `ImportPhysicalRanges` walks only that set and removes the ranges it leaves
  fully imported. A non-physical range in the set stops the emulator.
- **Result:** in the same cutscene window `_Rb_tree_increment` went from
  20.9% to 1.8% of the CP thread and XXH3 hashing from 34% to 13.6% of the
  profile. The import itself (about 135 µs per imported span) is unchanged.
- **Verified:** unit tests pass (2463). Regression sweep with no stop: racing
  57.8 fps, anime fighter controls menu about 48 fps between captures (48.2
  before), isometric roguelike 58 fps, insect platformer sequel 60 fps,
  beat 'em up 27-28 fps in combat (one sample each; gameplay read 31 in an
  earlier run at another moment).
- **Open:** each imported span still costs about 135 µs; the sorted table
  insert, the window import and the populated-page query have not been
  measured apart.

## Object range queries indexed by block size (2026-10-24)

- **Observed** (beat 'em up gameplay, Intel A770, strict runs at the 60 fps
  present limit, 18-23 fps): 37% of the CP thread was `GpuMemory::FindBlocks`
  under the vertex-snapshot admission check
  (`TransientReadOnlyOverlapsAllowedLocked`). Temporary counters (removed)
  showed, per window of about 2 s, about 11,400 admission queries missing the
  overlap cache; each miss took every object of the 1 MiB index buckets the
  range touched as a candidate, on average about 5,700, to find about 11
  overlapping blocks (about 400 ms per window). The heap held about 8,400
  objects, most of them small.
- **Fix** (`GpuMemoryInternal.h`, `GpuMap2`): blocks are indexed in six size
  classes (up to 256 B, 4 KiB, 64 KiB, 1 MiB, 16 MiB and larger), each an
  ordered set by start address with the sizes of its live blocks. A query
  visits, per class, only the blocks starting between the range end and the
  range start minus the class's largest live block, and keeps those that
  reach the range. `FindBlocks` still classifies each candidate exactly.
- **Result:** candidates per miss went from about 5,700 to about 14 (the
  overlapping blocks themselves) and the miss time from about 400 ms to about
  48 ms per window; other `FindBlocks` callers went from about 31 ms to 9 ms.
  The agent's gameplay fps read 31 against 19-23 before (one sample each, in
  different runs). In the regression sweep the anime fighter's controls menu
  ran at 52.5 fps between the same captures where the previous sweep read
  48.2.
- **Verified:** unit tests pass (2463) and the graphics diagnostics
  integration suite passes (15), whose index case now expects exact
  candidates and covers a large block reaching a range from far before it.
  Regression sweep with no stop: racing 59 fps, anime fighter 52.5 fps,
  isometric roguelike 58 fps, insect platformer sequel 60 fps, beat 'em up
  32 fps in a dark intro scene.

## Device-address registry changes without whole-registry walks (2026-10-24)

- **Observed** (beat 'em up, Intel A770, strict runs at the 60 fps present
  limit): on the title screen after the intro, `std::_Rb_tree_increment` held
  28% of the CP thread, reached from `GuestDeviceAddressPrepareForRead` and
  `GuestDeviceAddressPendingWriteBack`. Temporary counters (removed) over
  each 3,000 preparations showed about 11,300 registered ranges, about 1,000
  registrations and 1,000 releases, about 360 rebuilds of the merged range
  snapshot (about 1.0 ms each), about 360 device-table rebuilds (about
  0.9 ms each, of which the Vulkan allocation was about 40 µs) and about 700
  physical import passes (about 1.4 ms each). Every registration or release
  dropped the merged snapshot and dirtied the table, so each change walked the
  whole registry twice.
- **Fix** (`GuestDeviceAddress.cpp`, `GpuMemoryWriteback.cpp`): the storage
  write-back paths ask the registry whether a storage block overlaps a
  registered range (one ordered-map lookup under the registry lock) instead of
  scanning a merged copy of every range. GPU memory calls it under its own
  lock and the registry never calls GPU memory while it holds its lock, so the
  lock order stays one way. The device table's entries are kept as a vector
  sorted by guest base, updated when a chunk is imported or destroyed; a table
  rebuild copies that vector after the zero prefix instead of collecting and
  sorting every chunk. Chunks cover disjoint guest bytes, so an entry is keyed
  by its guest base and a duplicate or missing entry stops the emulator.
- **Verified:** unit tests pass (2463). In a 130 s beat 'em up run the
  registry functions and tree iteration no longer appear in a 25 s CP profile;
  that run reached gameplay at 79 s instead of the title screen, so the same
  scene was not measured before and after. Regression sweep with no stop on
  five titles (racing 59 fps, anime fighter 45 fps in a fight, isometric
  roguelike 58 fps, insect platformer sequel 60 fps, beat 'em up 19 fps in
  the intro).
- **Open:** a population change or a new registration still sends every
  physical range through `ImportPhysicalRanges`. The calls of
  `GuestDeviceAddressReleaseRangeQuiesced` from `release_direct_memory`
  (`Kernel/Memory.cpp`) grew from 178,000 to 918,000 per window on the title
  screen; whether the invalidation list accumulates was not checked. In
  beat 'em up gameplay (18-23 fps) 37% of the CP thread is
  `GpuMemory::FindBlocks` under the vertex-snapshot admission check
  (`TransientReadOnlyOverlapsAllowedLocked`).

## Restored retired tracker pages keep no identity (2026-10-24)

- **Observed** (beat 'em up, Intel A770, strict runs at the 60 fps present
  limit): in the intro cutscene `GpuMemoryCreateObject` held 44% of the CP
  thread, and about half of that was content hashing in `GpuMemory::Update`
  for ranges the dirty tracker had refused. A temporary counter (removed) showed about
  91,000 `RegisterRange` refusals per 60 s, all for the page-identity budget
  (524,288 pages), already on the title screen and with only about 5,750
  registered ranges. A table dump showed about 295,000 referenced pages and
  about 229,000 retired pages with a live token; nothing was revoked, so the
  2026-10-11 sweep of revoked identities freed nothing. The title allocates
  new resources at fresh addresses and does not unmap the old ones, so
  retired identities only accumulated.
- **Why retired identities were kept:** a guest write that faulted while
  the page was still armed can reach the handler after unregistration
  restored the page. The retired identity let that late fault restore its
  token and run again; without it the fault was reported as the guest's.
- **Fix** (`GpuDirtyPageTracker.cpp`, `VirtualMemory.cpp`): the protection
  operations gain an optional signal-safe host query, "does the host let this
  process write these pages now". On Linux it is
  `madvise(MADV_POPULATE_WRITE)`, which fails without a signal wherever a
  write would fault and is probed once at start (kernels before 5.14 reject
  it). A write fault that the tracked path does not claim runs again when the
  host grants the write: whatever removed the write is gone, and the access
  either succeeds or faults again for a real reason. Write faults are
  classified from the hardware error code on Linux; hosts without the query
  keep the old behaviour. With that rule, unregistration releases the
  identities of a run whose batched restore succeeded. A run whose restore
  failed keeps its identities and tokens, so a later write still restores the
  page. Released slots become tombstones; a tombstone followed by an empty slot
  (and the tombstones before it) becomes empty, in the page and block tables,
  so absent lookups do not lengthen with churn. That is safe because no probe
  chain continues past an empty slot.
- **Result:** registration refusals went from about 91,000 per 60 s to 0.
  Hashed bytes on the title screen fell from 8.8 GB to 3.1 GB per 60 s run
  (79,800 vs 117,500 calls). In the intro cutscene the same-binary A/B
  (release on vs off) gave 22.1 vs 20.6 fps between the same captures; this
  is one sample per side.
- **Verified:** unit tests pass (2463; one devtools process test failed once
  and passed on three reruns), including a focused case for released
  identities, failed restores and host-denied late faults; the existing
  late-fault case now runs through the real VM query. Regression sweep
  with no stop on five titles (racing 60 fps, anime fighter 48 fps between
  its last captures, isometric roguelike 57 fps, insect platformer sequel
  60 fps, beat 'em up title screen 56 fps), in line with the previous sweep.
- **Open:** storage textures are never retired while they hold writes
  (`GpuMemoryWriteback.cpp`, `FrameDone`): the beat 'em up creates about 18
  sprite-sized storage textures per cutscene frame at new addresses, and about
  11,000 stay live after 52 s, with 2.3 GB hashed at creation. The guest
  device-address registry follows them (about 11,000 ranges), and walks of
  that map (`MergedRanges`, `RebuildTable`, `ImportPhysicalRanges`) cost
  about 10% of the CP thread in tree iteration. Both need a proof that the
  storage contents are dead before anything can be released.

## Release-ordered snapshot lists instead of trees (2026-10-24)

- **Observed** (fishing title in game, Intel A770, CP-thread profile): the
  transient snapshot pool's free index cost about 4% of the CP thread.
  Every recording releases the snapshots it leased (`IndexFree`, 2.0%) and
  leases most of them again during the same recording, and each step
  inserted into and erased from two ordered maps (the per-usage
  size-and-release map and the pool-wide release map; tree erase 1.9%, plus
  node allocations).
- **Fix** (`TransientBufferPool`, `GraphicsRenderCommandBuffer.cpp`): release
  order only grows, so both release orders are intrusive lists. Free
  snapshots sit in one list per size, under a map from size to list; the
  first non-empty size at or above a request holds the answer at its head,
  which is the entry the previous size-and-release key chose. Empty sizes
  are dropped when a lookup passes them or when their last entry is
  destroyed.
- **Result:** in the same window of the scene (90-120 s) the tree erase
  share fell 1.86% → 0.33%, `IndexFree` 1.99% → 1.16% and `Capture`
  11.2% → 10.3% of the CP thread, about 2.4 points in total.
- **Verified:** unit tests pass (2462); regression sweep with no stop on
  five titles (racing 60 fps, anime fighter 47 fps, isometric roguelike
  60 fps, insect platformer sequel 60 fps, beat 'em up 18 fps in the intro cutscene),
  frame counts in line with the previous binary for the same scene.

## Attachment writes keep their own materialization (2026-10-24)

- **Observed** (fishing title in game, Intel A770, strict runs at the 60 fps
  present limit): render targets and depth buffers never reused the
  materialization cache; temporary counters (removed) showed 0 hits for
  about 0.9 million stores each. Every draw calls `ResetHash` on its
  attachment ranges, which invalidated every overlapping entry, including
  the attachment's own, so each of its about 1.8 attachment creates per
  draw went through `FindFast` and `Update` again.
- **Fix** (`GpuMemoryMaterializationCache.h`, `GpuMemory::ResetHash`): the
  invalidation names the writer by type and exact ranges and keeps only its
  entry; every other overlapping entry (a texture over the same memory, an
  object of the same type over different ranges) is still dropped. For the
  writer in the same submission `Update` does nothing, so the cached path
  does the same work as the fast path. The repeated-range shortcut records
  the writer, so a later plain invalidation of the range still scans every
  slot.
- **Result:** render target hits 0 → 83%, depth 0 → 88%, with no rejected
  hit. CP cost per draw, alternating the two behaviours every 5 s inside
  one run (another instance overlapped every sample): about 52.7 against
  54.0 µs per draw, about 2% and within noise.
- **Verified:** unit test for the writer entry, the dropped overlapping
  entry and a later plain invalidation; racing, fighter, roguelike, insect sequel and beat 'em up
  sweeps finish without a stop and with their captures; the fighter repeated
  without overlap holds its baseline (4394 frames in 100 s against 4405).
  Beat 'em up frame counts are bimodal on both binaries: runs whose boot
  lets the route reach gameplay render about 900 frames in 60 s, runs left
  on the title screen about 2100, so frame counts are compared only for the
  same scene.

## Descriptor set trim order and descriptor-miss attribution (2026-10-24)

- **Observed** (fishing title in game, Intel A770, strict runs at the 60 fps
  present limit, about 31.6-32.1 µs per draw): temporary counters (removed)
  showed about 37% descriptor-set hits per 100k lookups; about 62% of lookups
  missed only because a bound buffer had a different identity (textures,
  ranges and other fields about 0%). Most of those buffers are shared-pool
  snapshots (reused or newly captured), far fewer per-recording uploads.
  `TrimSets` took 4.1% of the CP thread, about 0.7% of it in a scan and
  partial sort over every live set.
- **Fix** (`DescriptorCache`, `GraphicsRenderDescriptor.cpp`): sets whose
  every use is tracked by a submission sit in a list ordered by last use,
  and a trim releases from its oldest end. The policy is unchanged; the scan
  and sort are gone.
- **Result:** `TrimSets` 4.1% → 3.5% of the CP thread (what remains is
  unindexing and deferred deletion of the released sets); 31.1-32.1 µs per
  draw against 31.6-32.1, within noise.
- **Excluded:**
  - keeping snapshots below the tracked-snapshot size in the release-ordered
    free index so their buffers keep their identity: hits 37% → 65%, but
    32.8 against 31.8 µs per draw, no gain;
  - binding read-only storage snapshots with the full entry range: hits
    unchanged, range differences were not the cause of misses.
  Descriptor-set misses are therefore not the dominant CP cost; binding work
  per draw (texture preparation, pixel-shader input analysis, snapshot
  capture) is.
- **Excluded, pixel-shader resource parse memo:** reusing the last eight
  `ShaderParseUsage2` results of a pixel program when user data, user SGPRs
  and layout seeds match and every recorded descriptor-table read returns the
  same words. In game it hit 46% of draws; 37% of misses differ only in the
  extended-user-data pointer SGPR pair, which the guest moves every draw.
  Gain about 0.5 µs per draw (31.3 against 31.6-32.1), within noise, for
  about 7 KiB per entry. Reusing a result across a moved pointer would need
  proof that the parse consumes the pointer only through that table read.
- **Measurement caveat:** a second emulator instance started during a run
  raised every per-draw cost of the fishing title by about 15% at once
  (22 → 15 fps, same draws per frame). Perf windows that overlap another
  instance are discarded.

## Transient snapshot pool: covering vertex snapshots and expiry (2026-10-24)

- **Observed** (fishing title in game, Intel A770, strict runs at the 60 fps
  present limit): the transient snapshot pool sat at its 512 MiB cap (64 MiB
  per recording, eight recordings) with about 770 entries. Temporary counters
  (removed) showed that 99.9% of new captures found no previous snapshot for
  their key, although 96% of those keys had been seen before: entries were
  recycled before their key came back. About 3 million captures in 130 s;
  vertex captures 33% (keys about 865 KB, about 19 KB read per draw), index
  27%, storage 40%.
- **Cause:** a vertex draw keys its snapshot by (base, size), and the size is
  the end of the records that draw reads. Per 1,500 binds there were 250-300
  distinct bases but 770-826 distinct (base, size) pairs over the same bytes,
  so one buffer held several overlapping snapshots, and 88% of the windows
  repeated in the next frame. Shifting the vertex offset instead is not
  possible: the vertex index feeds guest VGPRs directly.
- **Fix** (`TransientBufferPool`, `GraphicsRenderCommandBuffer.cpp`):
  - Vertex snapshots without a zero tail are keyed by base and usage only. A
    probe reuses the newest snapshot that covers its read window, compares
    the whole snapshot, and binds the snapshot's full range. A miss captures
    the union with the previous snapshot of that base when the union stays
    within the read-only transient limit and the admission check accepts it.
  - Free entries that still hold a snapshot sit in their own index ordered
    by release. A new capture takes, in order: an empty free entry, the stale
    entry of its own chain, an aged entry, a new entry while the pool is
    below its cap, and only then the least recently released snapshot.
  - Free snapshots not reused for 256 recordings are destroyed, at most four
    per capture, so the pool shrinks back after a scene change.
- **Result** (fishing title in game): 34.5-36.6 → 30.6 µs per draw
  (35.4 ms for 1,157 draws), about 24.8 fps, pool 420-512 MiB → 51 MB,
  uploads 1.3-2 → 0.34 ms per frame. Racing, anime fighter, isometric
  roguelike, insect platformer sequel and beat 'em up strict runs reach the
  same frame counts and fps as the same morning's baseline, captures match.
- **Excluded:** least-recently-released recycling alone (37.2 µs per draw:
  keys still did not cover each other); capturing whole vertex buffers (up
  to 818 MB per window); keeping released snapshots for 1,024 recordings
  instead of 64 (about 32 µs per draw, pool 71 MB: storage ring constants
  that come back hold new bytes, so the extra hits are mismatches).
- **Open:** storage captures still find no previous snapshot in about 96% of
  probes (ring constants); descriptor binding and pixel-shader input analysis
  remain the largest CP costs. 60 fps needs about 11 µs per draw.

## Fishing title CP cost per draw and an excluded usage memo (2026-10-24)

- **Observed** (fishing title in game, Intel A770, strict runs at the 60 fps
  present limit): 17-23 fps, about 1,150-1,490 draws per frame, command
  processor busy 40-55 ms per frame, 34.5-36.6 µs per draw. 60 fps needs
  about 11 µs per draw. The frame is bound by the command processor.
- **Profile** (frame-pointer sampler on the CP thread, in game; inclusive):
  descriptor binding 33% (descriptor-set lookup and creation 12%, texture
  preparation 10%, storage-buffer preparation 7%), transient snapshot capture
  16% (vertex buffers 8%, mostly copies and compares of guest bytes),
  pixel-shader input analysis 12% of which `ShaderParseUsage2` 9%, pipeline
  lookup 6.5%, GPU object creation 6.4%, the Vulkan driver 12%.
- **Counters** (temporary, removed): descriptor-set lookups hit 77-81%; the
  live-set count stays at the 65,536 limit, so the cache trims 8,192 sets at
  a time. The exact `ShaderParseUsage2` input (shader, all user SGPRs) repeats
  in 89% of pixel-shader calls over the whole run; the EUD pointer SGPR
  changes in 44% of consecutive calls of the same shader, so a key recurs
  about one frame later.
- **Excluded: exact memo of `ShaderParseUsage2`.** The result was stored with
  every input (SGPRs, user-data metadata, output seeds) and the guest
  descriptor words read through the reader seam, re-read and compared on
  reuse. Its output copies SGPR values, so the key cannot drop the EUD
  pointer. Hit rates: 45% with 16 results per shader, 61% with 128, 62% with
  a global LRU of 8,192. With the global LRU the in-game cost stayed at
  36 µs per draw (41.6 ms for 1,158 draws), against 34.5-36.6 µs before:
  recording and copying the 7 KB bind output on each miss cancels the hits.
  Not adopted.
- **Open:** the remaining levers are per-draw state that does not change
  between frames (descriptor sets, texture and vertex snapshots, pipeline
  lookup), not the usage parse alone.

## Compute queues on a compute-only family (2026-10-24)

- **Observed** (anime fighter, Intel A770, Mesa ANV, strict runs at the 60 fps
  present limit, about 38 fps in a menu scene):
  - The guest chains about 30 async compute submissions per frame. Each
    one is a single small dispatch followed by a label write that the guest
    polls before it builds the next one.
  - Temporary Vulkan timestamps (removed) put the CP and the GPU each about
    half of the frame busy, and both at once only 28% of it. An async
    submission made while a graphics batch was queued waited 4.7 ms for
    that batch to finish.
  - Cause: the A770 exposes one queue in its only graphics family (the AMD
    iGPU tested exposes one as well). Queue selection put every Kyty queue
    in that family, so all eight compute queues and the graphics queue were
    the same `VkQueue`, and the GPU ran their batches strictly in order.
    The record "Anime fighter: async round-trip latency" measured the chain
    but not this cause.
- **Change.**
  - When the common graphics family has a single queue and the device has a
    compute-only family, the compute queues are created there, up to eight.
    Hosts whose graphics family has several queues keep the old layout.
  - `GraphicContext` records the distinct families its queues use. Queues
    never transfer ownership, so with more than one family every image and
    buffer is created `VK_SHARING_MODE_CONCURRENT` across them. Cross-queue
    order still comes from the existing host waits on labels and fences.
  - A command buffer on a queue without graphics support records no graphics
    pipeline stage or access type. Layout transitions and the guest barrier
    helpers drop graphics access types there and wait on all of that queue's
    commands instead of graphics stages; those accesses ran on the graphics
    queue and are ordered by the host waits.
  - Dispatches no longer set graphics dynamic state: a compute pipeline has
    none, and `vkCmdSetLineWidth` is invalid on a compute-only queue.
  - Compute fills that would clear aliasing images still require the
    physical graphics queue and are refused on a compute-only queue, as on
    any host with distinct queues. No run below reached that refusal.
- **Measured** (Intel A770, strict runs, same route):
  - Anime fighter, agent counters over the same 40 s window 50-90 s into
    the run: 1,654 presented frames with the old layout, 2,010 with the new
    one (+21.5%); the last three 5 s windows moved from about 35 to about
    43 fps. An earlier experiment with the same layout read 49.3 fps against
    about 38 fps in a menu scene, with CP and GPU both busy 41% of the frame
    instead of 28%.
  - Fishing title (no async compute): 23.0 fps with the old layout, 22.1 fps
    with the new one, at a single in-game reading each. That is within run
    noise; `CONCURRENT` sharing showed no GPU cost there.
- **Validation** (Vulkan validation layer, errors only, 60-90 s each): no
  error on the anime fighter, racing, insect platformer sequel, fishing, beat
  'em up, isometric roguelike and 2D Unity action titles. The role-playing
  title uses async compute; its 20 `VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819`
  errors (color-attachment access with compute-queue stages) led to dropping
  graphics access types, after which its run reports none. Only the fighter
  and the role-playing title recorded on the compute family.
- **AMD host** (Radeon iGPU, RADV, validation layer errors only, 90 s): the
  anime fighter reached its consent screen at 14.4 fps with correct output
  and no validation error. That run did not confirm that a compute-family
  queue recorded work before it ended.
- **Not verified:** the robot platformer; async compute on the AMD host.

## Early submissions at least half a millisecond apart (2026-10-24)

- **Observed** (sandbox title in the world, Intel A770, Mesa ANV, strict
  runs at the 60 fps present limit, agent counters 245-290 s into the run):
  - About 4300 draws per frame and about 380 submissions per frame: the GPU
    finished each eight-operation batch before the CP thread recorded the
    next one, so the idle check (see "Recorded work submitted while the GPU
    queue is idle") submitted every eight draws.
  - In CP-thread stack samples the submissions the idle check starts took
    about 7% of the thread, and the command-buffer `Begin` that follows each
    one (driver time and lease release) about 6% more.
- **Change.** An early submission also needs 0.5 ms since the processor's
  previous submission. At about 30 µs per submission that bounds their
  cost to about 6% of the CP thread, while the GPU waits at most 0.5 ms for
  the next batch.
- **Results** (same route and window, one run each):

  | Binary | CP per frame | Draws per frame | fps | Submissions per frame |
  | --- | --- | --- | --- | --- |
  | Before | 87-95 ms | 4200-4390 | 9.0-11.3 | about 380 |
  | After | 68-74 ms | 4260-4400 | 10.5-12.2 | 113 |

  - Regression runs at the 60 fps limit, no stops: the pixel cave
    platformer holds 60 fps and reaches frame 4446 at 100 s (4399 in the
    2026-10-22 record); the fishing title shows 20.2 fps at the dock (17.4
    before); the racing, card, beat 'em up and insect platformer sequel
    titles reach their usual screens at 58-60 fps.
- **Verification.** Unit tests (2459 passed) and the 74 graphics
  diagnostics integration tests pass.

## A draw after a dispatch starts a new command buffer (2026-10-23)

- **Observed** (sandbox title, boot, Intel A770, Mesa ANV 26.2.5, Xe kernel
  driver):
  - About 11 s into boot (presented frame 85-150), when the menu's
    post-process chain first runs, the graphics queue's job timed out in
    roughly half of the 30 s runs. The kernel logged a GuC engine reset of
    the render engine after its 640 ms preemption timeout, then
    `Timedout job`, and the next fence wait returned
    `VK_ERROR_DEVICE_LOST`.
  - The hung submission held the same chain each time: post-process draws,
    two dispatches, six draws and sometimes one more dispatch. Every earlier
    submission had completed. The validation layer reported no error.
- **Excluded.**
  - Loop bounds of the chain's pixel shaders. Their constant words were the
    same in every frame, including the hung one. The translated loops are
    bounded, and `V_CVT_I32_F32` saturates as on the guest GPU.
  - A missing dependency between recorded commands. A full
    `ALL_COMMANDS` memory barrier at every draw/dispatch transition still
    hung (frame 546).
  - Color compression. `INTEL_DEBUG=noccs` still hung (second run).
  - New command buffers at draw-to-dispatch transitions only. Still hung
    (frame 89).
- **Cause.** The hang needs a draw recorded after a dispatch in the same
  host command buffer:
  - ending the command buffer before such a draw avoided it in 8 of 8 runs;
  - `INTEL_DEBUG=reemit`, which makes the driver emit all graphics state at
    every draw, avoided it in 6 of 6 runs without any other change.

  Within one command buffer the driver keeps graphics state across its
  switch from the compute pipeline back to 3D and emits only the state that
  changed. On this host part of that state is not valid after the switch.
  The packet involved was not isolated; that needs a driver build.
- **Change.** The command processor submits its recording before a draw
  when a guest dispatch was recorded since its last submission. The next
  command buffer emits every graphics state again.
- **Verified.**
  - Sandbox title: 8 of 8 runs of 30 s without a hang (3 of 4 hung before).
  - Unit tests (2,459) and the integration and diagnostics tests (74).
  - 30 s strict runs at the 60 fps present limit, frames before and after:
    fishing title 1,442 / 1,438, insect platformer sequel 1,345 / 1,341,
    card title 1,479 / 1,473, racing title 1,455 / 1,453, beat 'em up
    493 / 524.
  - Racing title on AMD Raphael (RADV), 30 s: 1,456 / 1,462 frames.
  - Robot platformer, Intel, warm caches, 100 s: 986 frames at 26-33 fps
    with the change; 156 and 1,171 frames at 12-28 fps in the two runs
    without it. No measurable cost.
  - Full scripted routes of 22 titles on Intel (200-240 s each, 60 fps
    present limit): every title ran its whole route without a stop and
    reached the same gameplay captures as the previous sweep.
  - Beat 'em up, 110 s, perf snapshots every 26 s: 3,427 frames with the
    change against 3,340 without it. In combat the submissions per frame
    rose from 10.7 to 12.3 and the median frame time stayed at 41-42 ms.
- **Not verified.** Other Intel generations, and drivers other than Mesa
  ANV and RADV. The robot platformer's full 700 s route and the 2D Unity
  action title were not run with the change.

## Guest-window searches skip occupied spans (2026-10-23)

- **Observed** (role-playing title, boot, Intel A770): the first frame
  appeared after about 17.6 s, and a 30 s run presented 3 frames. One
  thread spent about 11.5 s of CPU in `mmap` under
  `mmap_in_guest_window`. With `KYTY_DEBUG_FLEX_ALLOC=1`, 78 of 79 window
  searches succeeded on their first attempt; one, a 64 MiB request with a
  16 KiB alignment and an occupied hint, took 33,554,433 attempts and
  landed 512 GiB above its hint.
- **Cause.** On Linux both window searches (private and shared mappings)
  stepped by the alignment and issued one refused fixed mapping per step,
  so an occupied interval cost one system call per aligned address inside
  it. The Apple shared search already skipped to the end of the occupied
  region.
- **Change.** When a fixed mapping is refused because the span is
  occupied, the search reads the kernel's sorted mapping list and resumes
  at the first aligned address whose whole span is free. Any other refusal
  ends the search, since it would repeat at every address. Requests larger
  than the window are refused up front.
- **Verified.**
  - The same request now succeeds on its second attempt, at the same
    address as before.
  - Role-playing title: 339 frames in 40 s (38 in 45 s before), and it
    reaches its initial settings screen within 30 s.
  - Unit tests (2,459) and the integration and diagnostics tests (74).
  - 30 s strict runs at the 60 fps present limit: fishing title 1,442
    frames, insect platformer sequel 1,345, card title 1,479, racing title
    1,455, beat 'em up 493. No stop.
  - The sandbox title's intermittent GPU hang about 11 s into its boot
    also reproduces on the previous binary, without this change.

## Proven one-word buffer fills recorded as transfer fills (2026-10-23)

- **Observed** (beat 'em up, title screen, Intel A770, Mesa ANV, Xe kernel
  driver, `INTEL_MEASURE` over about 440 frames): the GPU was busy for about
  20.8 ms per frame and the title presented about 45 fps.
  - 13.3 ms per frame went to five dispatches of the generic uniform-fill
    kernel (the 16-byte record form, one 32-bit word repeated), 2.6 to 3.7 ms
    each, four of them over 33 MB storage buffers.
  - 6.3 ms per frame went to the transfer fills that publish the same fills
    into the guest device-address view.
- **Cause.** Storage objects live in host-cached system memory, so the
  kernel's 16-byte stores cross PCIe on a discrete GPU, and every store also
  sets its page's flag in the write mask, in system memory as well. Temporary
  experiment (removed, never a candidate): with storage objects of 16 MiB or
  more in device-local, host-visible memory, the kernel took about 1.3 ms, so
  the page-flag stores alone cost about as much as the data.
- **Change.** When a dispatch is a proven full uniform fill whose words are one
  repeated 32-bit value, and the exact storage object of the range exists, the
  dispatch is recorded as a transfer fill of that object instead. A second fill
  sets every flag of the object's write mask, as the kernel's stores would, so
  the write-back that follows is unchanged. Barriers on both sides order the
  fill against earlier shader and transfer writes and every later access. Fills
  of other word patterns, partial thread groups or ranges without an exact
  storage object still execute the dispatch. Alias propagation and the
  device-address publication that follow the dispatch are unchanged.
- **Verified.**
  - Same route and frames: the fill dispatches disappeared, and transfer fills
    took 13.3 ms per frame (storage objects 0.75 to 0.86 ms each, device-address
    view 1.2 to 1.8 ms each). The GPU frame fell to about 16 ms, and the title
    screen holds the 60 fps present limit (frame 439 at 60.0 fps against
    frame 409 at 45.2 fps). The capture matches the earlier build.
  - Unit tests (2,459) and the integration and diagnostics tests (74).
  - Regression sweep, 30 s strict runs at the 60 fps present limit, frames
    presented: fishing title 1,434, insect platformer sequel 1,341, 2D Unity
    action title 1,233, card title 1,472, racing title 1,254, anime fighter
    1,490, isometric roguelike 1,152, preschool title 1,447, well platformer
    1,355. No stop.
  - The role-playing title presented 3 frames in 30 s: one boot frame took
    17.6 s, as on the earlier build. A 45 s repeat reached 38 frames at
    51 fps on the same loading screen as the earlier build.
  - The two titles that fill targets with this kernel every frame, 90 s strict
    runs with their recorded routes: the 2D Unity action title in gameplay
    (4,331 frames, 60 fps) without trails or a white scene, and the isometric
    roguelike in its first room (4,035 frames, 60 fps) with correct lighting.
- **Open.** Both fills still write the range to system memory over PCIe.
  Removing that cost needs storage objects in device-local memory with an
  explicit read-back for the CPU write-back.

## Crash evidence read only for a reported fault (2026-10-23)

- **Observed** (isometric roguelike, boot into its first load, Intel A770,
  warm shader caches): a kernel call-chain sample of the guest worker thread
  that writes most write-tracked pages put 341 samples in `getpid` and 313 in
  system-call entry for `process_vm_readv`. The POSIX signal handler read
  1 KiB of the faulting thread's stack for every SIGSEGV before offering the
  fault to the write-tracking and demand-mapping handlers, which handle it
  and resume. The Windows exception handlers copied the stack the same way.
- **Change.** The signal and exception contexts hold the registers only. The
  stack, and with crash memory enabled the memory windows its words point
  to, are read when a fault is reported: in `FatalFault`, on a copy of the
  context, and on the SIGILL path. A context that already carries a stack
  keeps it.
- **Verified.** The crash-report unit test (stack and memory windows), the
  unit suite (2,459 tests) and the integration and diagnostics tests (74).
  Same route: neither call appears in the worker's profile any more, and its
  CPU time fell from 4,317 ms to 3,953 ms. The number of tracked-page faults
  changes between runs, so the total kernel samples (12,276 and 11,282
  before, 11,331 after) do not compare.
- **Not verified.** The Windows change was not built or run on Windows.

## Direct-memory windows imported as pinned dma-bufs (2026-10-23)

- **Observed** (isometric roguelike, boot into its first load, Intel A770,
  Mesa ANV, Xe kernel driver, warm shader caches):
  - The title stopped at its first load in 4 of 4 runs (and in most runs
    before them), with a guest-thread write to address 0x40. The guest
    allocates its staging pool (2 x 256 MiB) and the allocation failed:
    its list of pending uploads kept growing because the render thread's
    frame took too long, while the loader queued bursts of about 533 MiB
    in 320 ms.
  - The render thread waited in the graphics submit for the command
    processor to decode its batch. The submit contract (above, "A graphics
    submit returns after its batch is decoded") requires that wait.
  - A kernel call-chain sample of the command-processor thread over the
    whole run: 2,579 samples (250 us each, about 0.65 s) in the Xe exec
    ioctl, 2,233 of them re-validating host-pointer imports
    (`xe_vm_userptr_pin` > `drm_gpusvm_get_pages` > `hmm_range_fault`,
    then IOMMU mapping). Creating the imports took 204 samples.
- **Cause.** Direct-memory windows (up to 64 MiB each) were imported as host
  pointers. Their pages stay movable, so every host migration or reclaim of
  one page invalidates the whole import, and the next submission walks and
  maps the whole window again. During the load the host migrated 137,509
  pages (537 MiB) in 20 s through proactive compaction
  (`compaction_proactiveness` 20) while it was also swapping.
- **Change.** The import probe prefers a dma-buf of the backing (through
  `/dev/udmabuf`), which pins the window's pages, and keeps the host pointer
  for hosts that cannot export one. Devices that refuse shared host pointers
  already took the dma-buf path. Guest releases inside a pinned window were
  already zero-filled instead of discarded.
- **Verified.**
  - Command-processor thread, same route: 302 samples in the exec ioctl and
    none re-validating imports; kernel time 2,310 samples against 5,411.
  - The roguelike ran 5 of 5 runs without stopping. A 40 s run presented
    1,611 frames, at 60 fps (limited) after the load, with p99 frame time
    falling to about 45 ms, and its capture shows the title screen.
  - Unit tests (2,459, including 14 device-address tests that now import
    through dma-bufs on this host) and the integration and diagnostics
    tests (74).
  - Regression sweep, 30 s strict runs at the 60 fps present limit, frames
    presented: fishing title 1,430, insect platformer sequel 1,328, 2D Unity
    action title 1,237, card title 1,471, racing title 1,243, anime fighter
    1,488, beat 'em up 318 (still loading at 30 s). No stop and no import
    failure.
  - Beat 'em up, 40 s from boot, host pointer against dma-buf: 542 against
    761 frames presented; frame time p50 72 against 25 ms, p95 130 against
    117 ms, p99 363 against 188 ms.
- **Excluded earlier for the same stop:** a higher cgroup memory ceiling,
  `MADV_HUGEPAGE` on the backing, a lost condition-variable wake-up,
  populating all GPU-visible memory up front (the import blocked decode and
  grew memory by 3.6 GiB), populating the CPU staging memory, userfaultfd,
  and a faster 64 KiB detile on its own. A warm shader cache alone did not
  remove it either.
- **Open.**
  - The guest's own tiler thread spends most of its time in first-touch
    faults on direct memory (`shmem_fault`, page clearing) and in the
    write-tracking signal path (`mprotect` TLB shootdowns); the crash
    evidence reads in that path were removed (record above).
  - Hosts without access to `/dev/udmabuf` keep the host-pointer path and
    its re-validation cost.

## Populated direct-memory pages read from the backing's extents (2026-10-23)

- **Observed** (isometric roguelike, boot into the first load, Intel A770,
  Mesa ANV, Xe kernel driver, warm shader caches):
  - Preparing the guest device address table took about 3.6 ms per call
    while the title loaded, and 68 ms for the first call.
  - A kernel call-chain sample of the command-processor thread put about
    40% of its CPU in `mincore` over the guest's physical views
    (`filemap_get_entry`, `xas_load`, `__mincore_unmapped_range`): each
    call asked the page cache, page by page, which pages of the views were
    present.
- **Change.** Physical ranges ask the direct-memory backing itself which
  pages hold data: `SEEK_DATA`/`SEEK_HOLE` over the backing walks its
  extents instead of every page. A page counts as populated once any view
  touched it and stays so until the guest releases it, which is the same
  set `mincore` reported for the views. Flexible memory keeps `mincore`.
  Windows hosts report every page populated, as section views commit
  their pages when mapped.
  - A physical range is now resolved before the query. A range without
    direct memory behind it fails the import with a message instead of
    importing nothing.
- **Verified.**
  - Unit test for the extent query (populated pages 1, 2, 5, 6; queries
    inside, between and past them), the unit suite (2459 tests), the
    integration and diagnostics tests (74) and the emulator boundary gate.
  - Isometric roguelike, same route: preparation fell to about 1.3 ms per
    call and the first call to 45 ms; command-processor time over the load
    was 173 ms for 129 batches against 264 ms for 99 before.
  - Regression sweep, 30 s strict runs at the 60 fps present limit, frames
    presented: fishing title 1,349, insect platformer sequel 1,296, 2D Unity
    action title 1,198, card title 1,443, racing title 1,178, anime fighter
    1,494, beat 'em up 357. No stop and no import failure.

## `sceKernelSleep` sleeps whole seconds (2026-10-23, found by inspection)

- **Contract.** The published SDK header declares
  `sceKernelSleep(uint32_t seconds)`, and two independent implementations
  sleep that many seconds.
- **Defect.** The HLE passed the count to a millisecond sleep, so a guest
  that slept one second resumed after one millisecond.
- **Change.** The sleep converts seconds to nanoseconds in 64 bits and
  records a `Sleep` wait for the thread diagnostics, as `sceKernelUsleep`
  does.
- **Verified.** Kernel unit tests pass. No run has been observed calling
  this export, so no title result changes are claimed.

## Stencil test without a stencil format (2026-10-23)

- **Observed.** The insect platformer sequel (one draw per run) and the robot
  platformer (8 per run, limited by the log cap) left
  `DB_DEPTH_CONTROL.STENCIL_ENABLE` set while `DB_STENCIL_INFO.FORMAT` was 0
  (`STENCIL_INVALID`). An earlier read-only capture recorded both depth and
  stencil formats as zero, with both stencil base addresses zero. The validator
  classified this as a missing stencil read base and logged a warning per draw,
  while the backend already bound no stencil plane
  (`ResolveEffectiveStencilFormat` returns 0 for format 0), and Vulkan leaves
  coverage unmodified without a stencil attachment.
- **Evidence.** The register enum names format 0 `STENCIL_INVALID`. A host GL
  driver for the same GPU family programs `STENCIL_ENABLE` from the
  depth-stencil state alone and writes `STENCIL_INVALID` when the framebuffer
  has no stencil, relying on the depth block to ignore the test (GL: "as if the
  stencil test always passes").
- **Change.** `ValidateStencilPlane` returns `Inactive` for format 0 regardless
  of enables and base addresses. The separate "stencil format 0" warning was
  removed as unreachable. The unit test
  `IgnoresStencilControlWithoutStencilFormat` covers an enabled test, a clear, a
  stray write base, and format 1 still reporting a missing read base.
- **Also in this change set.** Warnings removed or narrowed for supported
  states: 48-bit vertex-stream storage bases (the shader consumes the full
  base), `Z_16` depth format (mapped to D16), and mipmapped 64 KiB-tiled color
  targets (described as a mip chain). Untiled mipmapped targets and unmapped
  depth formats still warn.
- **Verified.** Emulator unit suite 2457 passed; integration and diagnostics
  tests 74 of 74; `check_emulator_boundaries.py` clean.
- **Sweep.** Strict, 45 s per title, 60 fps present limit, Intel A770 host:
  fishing title 1321 frames at 60 fps, insect platformer sequel 1315 at 60,
  card title 1467 at 60, 2D Unity action title 1169 at 47 (a point sample
  during its level load). All stop=none, and the stencil, 48-bit base and
  depth-format warnings no longer appear.

## Collector context requests delivered only at safe points (2026-10-23)

- **Observed** (fishing title, the intermittent frame-3 boot hang recorded
  under "Index snapshots over linked surfaces", host stacks at the stall):
  - The title's IL2CPP collector installs a handler for exception 30
    (`sceKernelInstallExceptionHandler`) and stops the world by raising it
    on each thread (`sceKernelRaiseException`, reached through the platform
    utility's thread-context request). The handler stores the interrupted
    stack pointer, posts the suspend semaphore and waits on an event flag
    until the world restarts.
  - The host signal that carried the request could land while the thread
    ran emulator code holding a host lock. In the captured stall the thread
    held the virtual-memory lock inside a guest-memory copy; the handler's
    event-flag wait took the same non-recursive lock to record its guest
    callers, so the thread deadlocked with itself and the collector waited
    for an acknowledgement that never came.
  - Any host lock has the same problem: the handler keeps it for the whole
    collection, and every thread that needs it stops.
- **Approach.** Collected runtimes suspend a thread only at a safe point:
  while it runs managed code, or while it blocks in a wait the runtime
  declared safe. A suspension that lands anywhere else is deferred until the
  thread reaches one. Kyty applies the same rule to guest code and host waits.
- **Change.**
  - The loader and the kernel record executable guest memory in
    `GuestCodeRanges`: the segments of every loaded program and the mappings
    the kernel makes executable. It is a page-interval set with a lock-free
    `Contains` that a signal handler can read. Adding a range merges it with
    the ranges it touches; removing one trims or splits them. Map, protect,
    unmap, cut, decommit, reserve and the destructors keep it current, and the
    runtime linker's executable-address check uses it.
  - `PthreadContext.cpp` owns the handshake. A request sets a per-thread
    pending bit and signals the thread. The signal runs the guest handler
    only when the interrupted instruction is guest code, or when the thread
    is stopped at the boundary of a blocking system call inside a
    `PthreadInterruptibleScope` whose first argument names the declared wait
    object. Scopes that hold no host lock at all accept any system call.
    Otherwise the handler records the address (`context_deferred_rip`) and
    arms a one-shot 100 µs per-thread timer that sends the signal again.
  - Declared safe waits: guest semaphores, event flags, event queues, POSIX
    semaphores, condition-variable sleeps and the mutex reacquisition after
    them, contended mutex and rwlock acquisitions, address waits,
    static-initialisation waits, sleeps, joins, blocking socket calls and
    the socket-close drain.
  - On Linux `Core::CondVar` sleeps on a sequence word that every signal
    advances, so a signal never waits for a waiter. glibc's condition
    variable before 2.41 can make a signal wait until earlier waiters drop
    their group reference, and a waiter stopped inside its wait keeps that
    reference for the whole collection. The address waits, the
    static-initialisation and once waits and the socket and epoll close
    drains moved from `std::condition_variable` to `Core::CondVar`; an
    address wake signals at most the registered waiters.
  - Guest condition variables no longer use glibc's either. Each one has a
    state with a `Core::Mutex`, a `Core::CondVar` and a generation that
    every signal advances under that lock. A signal therefore wakes only
    threads that were already waiting, never waits for a waiter, and takes
    no lock when nobody waits. The lock is never held across a sleep, so no
    stopped thread holds it.
  - Waiters keep the state through a `shared_ptr`. Destroying a condition
    right after a broadcast, which POSIX allows, frees nothing the woken
    threads still use, so destroy returns at once. Destroying one that still
    has blocked waiters is undefined; glibc's destroy blocked on them for
    good.
  - Timed waits read the condition's clock between sleeps. The monotonic
    clock therefore works on every host, and the host `pthread_condattr_t`
    is gone.
  - Requests coalesce: one raised while another is pending or running is
    answered by that delivery, or by a new delivery right after the handler
    returns. A request for a created thread that has not started is
    delivered when it starts; a thread that exits takes its pending request
    first; a thread that has exited answers `ESRCH`.
  - The guest handler runs on a trampoline stack and returns through
    `rt_sigreturn`, so an interrupted restartable system call resumes as it
    would without the request. One trampoline stack and one retry timer per
    pooled thread replace the 1 MiB stack allocated per request, so the
    stack retention noted under "Host context requests" in the history is
    gone.
  - The wait scopes record guest callers by reading the thread's own guest
    stack, without the virtual-memory lock.
  - `kyty_agent threads` reports `context_pending`, `context_running` and
    `context_deferred_rip` (`docs/agent-tools.md`).
- **Verified.**
  - Three real-signal unit tests replace the overlap test: requests reach
    sleeping threads (two raisers, 200 requests each, exact handler counts,
    `ESRCH` after the join); a request waits for a safe point (no delivery
    during 20 ms of host-code spinning, delivered once the thread sleeps);
    a request interrupts guest code (a page registered as guest code). Each
    passed 8 of 8 runs.
  - Emulator unit suite 2449 passed, 11 skipped; integration and
    diagnostics tests pass, with the archive check taught the new source.
  - Strict runs at the 60 fps present limit: the fishing title reached the
    dock in 3 of 3 boots (1790-1884 frames in 45 s, 22-26 fps). Its end
    snapshot shows no thread pending and one deferred request that was
    delivered later. The insect platformer, its sequel, the preschool title
    and the 2D Unity action title, all IL2CPP, held the 60 fps limit for
    45 s each with no stop.
  - Condition signals: a standalone reproduction holds a waiter in a signal
    handler inside its wait, signals once, adds a second waiter and signals
    again. With glibc 2.36 the second `pthread_cond_signal` stayed blocked
    until the held waiter resumed (1000 ms); the sequence-word design
    returned after 1.1 ms; with glibc 2.44 both returned. The unit test
    `ConditionSignalDoesNotWaitForStoppedWaiter` runs that sequence on
    `Core::CondVar` through a held context handler (20 of 20 repeats); CI
    runs it on Ubuntu 24.04 (glibc 2.39). Emulator unit suite 2450 passed;
    the fishing title was interactive at 60 fps after 45 s and the four
    IL2CPP titles ran 45 s each with no stop.
  - Guest condition variables:
    - `GuestConditionSignalDoesNotWaitForStoppedWaiter` runs the same
      sequence on guest conditions, with the collector's signal coming from
      another thread. It passed 20 of 20 repeats.
    - The recursive-mutex wait and timeout tests pass, and the emulator
      unit suite passed 2451 tests.
    - Strict 45 s runs at the 60 fps limit, none of which stopped:

      | Title | Frames | fps |
      | --- | --- | --- |
      | Fishing title | 1293 | 60 |
      | Insect platformer sequel | 1338 | 60 |
      | 2D Unity action title | 1229 | 43.8 |

      The 2D Unity action title's fps is a point sample. Earlier runs of the
      same build read 45-60 at the same frame count.
- **Second pass** (same day): the guest-reachable waits and host locks
  found by a review of every wait site are now safe points.
  - Host locks that a thread may keep while it runs guest code are taken
    through `PthreadLockAtSafePoint` or its shared variant, which tries the
    lock and otherwise blocks inside a scope covering the mutex object: the
    NGS2 system and dispatch locks, the locale facet lock, the AV player
    decoder lock and the video-out registration lock.
  - The module linker no longer runs initializers under its lock. A provider
    loaded for a lazy import is relocated and marked pending under the
    linker lock. Its `module_start` runs outside the lock at the next module
    start or lazy bind into it, and a concurrent caller waits at a safe point
    until the starter finishes. Admission now takes the linker lock instead
    of a second lock that relocation took in the opposite order. A failed
    start is logged and recorded as a rejected lazy load; the provider stays
    loaded, as after `sceKernelLoadStartModule`.
  - NGS2 callbacks produced while a thread holds any system lock are queued
    per thread and run once it releases its last one, so a callback never
    runs under a lock that another system's renderer may wait for.
  - The locale facet lock is recursive: a facet destructor that re-enters
    the copy or add-facet operation on the same thread no longer deadlocks.
  - Declared waits: GPU ring submit, done and idle waits, decode completion,
    submission publication, window initialization, the video-out access,
    materialization, registration and flip lifecycle gates, `WaitFlipDone`,
    the application heap's startup wait, Videodec2 decoder removal, the AV
    player decoder joins and the 3D audio port push.
  - The GPU admission gate stays held across the drain of an unmap or
    protection change, because the drain must finish before the mapping
    changes. Taking the gate and the ring waits under it are safe points,
    because GPU work may wait on memory that a stopped guest thread writes
    later. A thread stopped in such a drain keeps the gate, so guest code
    that runs while the world is stopped must not submit GPU work or unmap
    memory.
  - POSIX semaphores are shared with every caller. Destroy wakes blocked
    waiters, which fail with `EINVAL` as a call on an unknown semaphore
    does, instead of freeing the object under them.
  - Verified: `ContextRequestReachesLockWaiter` (std and recursive
    mutexes), `StandardSamplerDefersCallbacksWhileAnotherSystemIsLocked`
    and `PosixSemDestroyWakesBlockedWaiter` pass; emulator unit suite 2453
    passed, integration and diagnostics tests 74 of 74. Strict 45 s runs at
    the 60 fps limit, none of which stopped:

    | Title | Frames | fps | End state |
    | --- | --- | --- | --- |
    | Fishing title | 1169 | 23.0 | 3D title menu (earlier build: autosave notice) |
    | Insect platformer sequel | 1323 | 60 | interactive |
    | 2D Unity action title | 1203 | 46.8 | interactive |
    | Card title | 1461 | 60 | interactive |
- **Third pass** (same day): guest-code tracking, audio and thread fixes,
  guest exit and one shader translation.
  - Executable guest mappings are tracked as guest code (above).
  - Audio output waits for host pause at a collector safe point before taking
    the AudioOut2 mutex, and AudioOut2 sleeps are interruptible.
  - AudioOut2 context pushes queue the mixed grain under the context lock and
    pace to the host sink after releasing it (`HostAudio::AudioOutQueue` queues
    without waiting; `AudioOutOutputs` queues and then waits in an interruptible
    scope), so a thread stopped at that safe point no longer holds the AudioOut2
    lock; the audio-input pacing wait is a safe point too.
  - pthread mutex, cond and rwlock calls return `EINVAL` for unusable objects
    instead of dereferencing null. Hard-coded address traces and a fixed 50 ms
    sleep in the async memory thread were removed; the blocked-waiter record
    that feeds `kyty_agent sync-waits` stays.
  - pthread rwlock attribute calls return `EINVAL` for a null or uninitialized
    attribute.
  - AudioOut2 pushes that fail after host audio shutdown no longer warn. The
    "host submit failed" lines at the end of every run came from the process
    shutdown closing the host sinks, not from a host sink fault.
  - Wait records name the first return address inside guest code as the
    caller.
  - `v_cvt_pk_u8_f32` is translated: NaN to 0, saturate to [0, 255], truncate,
    with the byte selected by `S1[1:0]`. Clamp, omod and op_sel are refused.
  - Guest exit runs the guest `__cxa_atexit` list (`atexit` shares it, newest
    first), flushes streams, shuts subsystems down and ends with `quick_exit`.
    The kernel `_exit` NID and window close end the same way without guest
    handlers. Host `atexit` handlers and static destructors no longer run while
    guest threads are alive.
  - Verified: emulator unit suite 2457 passed; integration and diagnostics
    tests 74 of 74; `check_emulator_boundaries.py` clean. Strict sweep, 45 s
    per title, at the 60 fps present limit on an Intel A770 host:

    | Title | Frames | fps | Stop |
    | --- | --- | --- | --- |
    | Fishing title | 1298 | 60 | none |
    | Insect platformer sequel | 1325 | 60 | none |
    | 2D Unity action title | 1217 | 60 | none |
    | Card title | 1468 | 60 | none |

    The 2D Unity action title's first sample read 8.2 fps, a load hitch with
    249 ms since the last present; its rerun held 60 fps. The fishing title was
    23 fps before this pass at the same point.
    The AudioOut2 pacing, rwlock-attribute and shutdown items landed after
    this table; the sweep in "Stencil test without a stencil format" covers
    them.

## Transient snapshots shared by a command pool's command buffers (2026-10-23)

- **Observed** (fishing title, dock in gameplay, Intel A770, Mesa ANV,
  strict runs at the 60 fps present limit, segments perf-4 and perf-5):
  - Each command buffer owned its transient snapshot pool. Since the early
    submission of 2026-10-22 a frame spans 130-150 command buffers, so a
    guest range that many of them bind was captured and uploaded again in
    each one. 61% of snapshot probes reused an entry.
- **Change.**
  - The command buffers of one command pool, one per submission slot, share
    one snapshot pool behind a mutex. A recording leases every entry it
    binds; the entry returns to the free index once the last recording that
    leased it is reset or freed.
  - Among free entries of the same size the earliest-created one is handed
    out, so a frame that repeats the previous frame's captures binds the
    same buffers and hits the descriptor sets keyed by them.
  - The shared pool's byte and entry limits are the per-recording limits
    times the eight recordings that can be in flight, the same total as
    eight private pools. Host-only uploads stay in each command buffer's own
    pool, so the reserve that kept room for them next to the snapshots is
    removed.
- **Results** (per draw, segments perf-4 and perf-5; descriptor-set misses
  from a temporary counter, not committed):

  | Snapshot pool | Reuse | Uploads | Upload bytes | Upload time | Set misses | Resource binding |
  | --- | --- | --- | --- | --- | --- | --- |
  | Per command buffer (before) | 61% | 3.65 | 22-23 KB | 2.5-2.6 µs | 33% | 13.1-13.5 µs |
  | Shared, release order | 71% | 3.0 | 15-17 KB | 1.9-2.0 µs | 53% | 13.2-13.6 µs |
  | Shared, earliest-created first | 71-72% | 2.9-3.0 | 15-19 KB | 1.8-2.1 µs | 10% | 10.9-13.8 µs |

  - Snapshot compares rose from 0.3 to 0.5-0.65 µs per draw, because more
    probes find an entry to compare.
  - End to end the fishing dock stayed within run-to-run noise: 26-32 µs
    per draw in three runs with the change, 28-32 in three runs before.
    Segments of different runs land on different game states.
  - Card title (gameplay, single runs without the limit): reuse went from
    24-32% to 28-35% and uploads from 1.73 to 1.61 per draw, at 15 µs per
    draw either way. It has about 5 submissions per frame, so little to
    share. At the 60 fps limit it holds 60 fps at 13-16 µs per draw, with
    descriptor-set creates at 1.2% of lookups (same temporary counter).
- **Excluded.**
  - Handing out free entries in release order (the first shared version):
    a recaptured range landed on an arbitrary buffer, so descriptor-set
    misses rose from 33% to 53% and the descriptor finalize from 3.4 to
    4.1 µs per draw.
  - Handing a recaptured range the free entry that last held it: also 53%
    misses; the guest rotates its constant-buffer addresses (see the
    transient-pool affinity exclusion of 2026-10-22).
  - Limiting early submission to one per millisecond, with private pools:
    27-29 µs per draw and 74-80 submissions per frame, against 28-30 and
    146 with every early submission. Turning early submission off gave
    24-26 µs per draw with 54-56 submissions, but the cave platformer needs
    it (see "Recorded work submitted while the GPU queue is idle").
- **Open.** A submission costs the CP thread about 30 µs: `Begin` with its
  kernel time (about 2% of the thread), the queue submit (about 1.3%) and
  the lease release (about 1.1%). At 120-150 submissions per frame the
  submissions the idle check starts are about 10% of the fishing dock's CP
  thread.
- **Verification.** Unit tests (2447 passed, 11 skipped), the 74 graphics
  diagnostics integration tests and `check_emulator_boundaries.py` pass.
  Strict runs: the fishing title reaches the dock and its capture renders
  correctly; the cave platformer holds 59-60 fps at the 60 fps limit; the
  card title plays at the limit.

## A graphics submit returns after its batch is decoded (2026-10-23)

- **Contract.** A graphics command-buffer submit returns to the guest only
  once the command processor has decoded the batch. The ring snapshot covers
  the command words only. Titles rewrite memory those commands point to,
  such as descriptor tables embedded next to the commands, as soon as the
  submit returns.
- **Evidence (card title, Intel A770, Mesa ANV, strict run).**
  - An experiment returned from submit at once. Each queue took an order
    ticket instead, and decode honoured the guest's order across queues.
  - The card title then stopped at boot every time, in the vertex input
    layout check (`unsupported vertex input layout: resources=3 buffers=2`).
    The vertex-buffer descriptors it read held floating-point constants
    (`0xbf800000`, `0x3f800000`): the guest had already reused that memory.
  - With the decode wait, the same build boots and plays at about 320 fps.
- **Consequence.** Overlapping guest work and decode needs a snapshot of the
  memory the commands reference, not a submit that returns early. The order
  gate is excluded; `docs/titles/anime-fighter.md` has its frame-rate result.

## Compute ring runs copied in bytes across the ring end (2026-10-23)

- **Defect.** When a run of compute packets crossed the end of the ring, the
  copy into the internal buffer counted bytes instead of dwords. Three
  quarters of the run then decoded stale words from an earlier wrap.
- **Found by reading** the ring code during the order-gate experiment. No
  run was seen to fail from it.
- **Change.** Both halves are copied as dwords.
- **Verified.** Build, unit and integration tests, and 10 fishing-title
  boots on the fixed build.

## Decoded video frames delivered after a decode error (2026-10-22)

- **Found by reading the decoder** while a logo video of the 2D Unity action
  title stayed on one frame. That freeze did not repeat, so this path is not
  confirmed as its cause.
- **Defect.**
  - A decode error marks the decoding as done but keeps the queued frames.
  - The non-blocking and blocking reads refused every frame once the decoder
    status was an error.
  - End of stream needs an empty queue, so the stream could not end: the
    player stayed on its last frame and neither looped nor stopped.
- **Change.** The reads drain frames decoded before the error. The stream
  then ends as on a normal end of file: a looping player tries to rewind,
  which fails on a failed decoder, and the player stops.
- **Verified.** A strict run of the 2D Unity action title still plays its
  logo video and loads the first level. A decode error was not injected.

## Recorded work submitted while the GPU queue is idle (2026-10-22)

- **Observed** (pixel cave platformer, gameplay room, Intel A770, Mesa ANV,
  Xe kernel driver):
  - A frame is about 4.7 ms of guest CPU, then about 3.5 ms of command
    processing, then about 15 ms of GPU work. The main command buffer was
    submitted only at the end of the batch, so the GPU stayed idle while
    the batch was decoded.
  - GPU timestamps (temporary, not committed) put the 3840x2160 uniform
    fill at about 4.5 ms of the frame and the final 4K render pass at
    1-2 ms. The GPU clock averaged about 1840 MHz, so it was not
    downclocking.
  - Excluded: skipping the per-draw full barriers (temporary, invalid as a
    fix) moved gameplay only from about 52 to 58 fps; render passes still
    split once per draw by another path.
- **Change.**
  - Once eight draws or completed dispatches are recorded since the last
    submission, the graphics queue checks after every further operation
    whether all its submissions have completed. If they have, it submits
    what is recorded so far and keeps recording into the next slot.
  - A plain label store that a later `WAIT_REG_MEM` waits on may now sit in
    an already submitted command buffer of the same queue. The consolidation
    predicate accepts any producer on the same queue, since queue order and
    its fence publish the store either way.
- **Results** (strict runs, `KYTY_RUN_PERF=1`, segment metrics):

  | Title (segment) | Before | After |
  | --- | --- | --- |
  | Pixel cave platformer (gameplay) | 52 fps, 19 ms median | 62-64 fps, 16 ms median |
  | Insect platformer sequel (26-52 s) | 158 µs/draw, 4.8 ms/frame | 121 µs/draw, 3.7 ms/frame |
  | Racing title (race) | 39.4 µs/draw | 39.9 µs/draw |
  | Card title (gameplay) | 22.4-22.8 µs/draw | 24.2-27.4 µs/draw |

  - The racing title, the roguelike platformer and the preschool Unity title
    reach their menus earlier, so routes timed by wall clock land on other
    screens. The racing title still reaches the race and the roguelike its
    first room; compare game state, not frame counts, across this change.
  - The card title pays 6-20% per draw; it stays near 180 fps. Stack samples
    spread the cost over transient snapshot churn per command buffer
    (`TransientBufferPool::UnindexSnapshot`, `Acquire`, tree erases).
  - Excluded: probing every 32 operations inside an open render pass kept
    the card title at its old cost but returned the cave platformer to
    about 50 fps, with more fence waits per frame.
  - No run hit the extended consolidation (`wait_reg_mem_queued_producer`
    stayed 0).
- **Correction (2026-10-23).** The first commit checked only on every
  eighth operation, while the results above were measured with the check
  after every operation. With the committed check the cave platformer stayed
  at 44-50 fps (21-22 ms median): 5 submissions and 4.3 ms of fence waits
  per frame, against 7 and 1.0 ms with the measured variant. The check now
  runs after every operation once eight are recorded. A strict run at the
  60 fps present limit holds 59-60 fps with a 17-18 ms median, 1.4-2.1 ms
  of fence waits and 5.7 submissions per frame, and reaches frame 4399 at
  100 s against 3899. The check costs a fence query per operation only
  while the GPU is busy: in a 20 s CP-thread sample of the fishing title's
  dock it took about 0.1% of the thread. The submissions it starts took
  about 10%, about 30 µs each at 120-150 submissions per frame.
- **Open.**
  - VSYNC flips are not paced to vblank: `VideoOutSubmitFlip`
    (`VideoOut.cpp:2565`) validates `flip_mode` and never uses it, so
    titles that request VSYNC run uncapped. Pacing would cap them at the
    display rate.
  - Closed (2026-10-23): transient snapshots were indexed per command
    buffer, so each early submission re-captured them. See "Transient
    snapshots shared by a command pool's command buffers".
- **Verification.** 635 focused unit tests (state, packets, submission) and
  the graphics diagnostics integration test pass;
  `check_emulator_boundaries.py` passes.

## Per-draw shader resource scans memoized by layout and instruction class (2026-10-22)

- **Observed** (frame-pointer stack samples of the command-processor thread,
  robot platformer intro, Intel A770, warm caches, 12 s at 64 s):
  - `ShaderParseUsage2` held 32.5% of the thread. The sampled
    texture/sampler association took 38.6% of it, almost all in the
    copied-image descriptor flow, which re-ran a whole-program analysis on
    every draw although only the descriptor words change between draws.
  - Vertex programs carried no `ShaderCodeResourceMemo`, so their EUD
    descriptor-load and dynamic scalar-load scans ran uncached per draw.
  - The redefined and assembled buffer-descriptor scans, the scalar-buffer
    loop, the EUD span and the sampler mask each walked every instruction of
    the program per draw to reach a few buffer, sampler or descriptor loads.
    The zero-image check of each dynamically loaded S# did the same once per
    sampler load.
- **Change.**
  - The copied-image flow is split into a layout-only part and the
    value-dependent part. The program memo keeps the layout-only reads
    (consumer PC, texture descriptor, sampler index) per binding-layout
    signature, which lists every bind field that flow reads; at most 64
    layouts, cleared on overflow. Descriptor words are still applied per draw.
  - Vertex programs own two memos: one for the front-stage EUD code and one
    for the program code.
  - The memo keeps ascending instruction indices per class (buffer accesses,
    image-sampler users, 4- and 8-dword descriptor loads). The per-draw scans
    and the zero-image sampler check visit only those indices and keep their
    original checks. Pointer-table and fused-GS user-data descriptors pass
    their program's memo too.
  - The same index lists cover image reads, GDS or M0 writes and whether the
    program touches GDS memory. The GDS pointer source register is resolved
    once per draw instead of once per direct resource, and the zero-image
    check visits only image reads.
- **Measured** (same route, binary and window):
  - Memos bypassed by a temporary switch versus enabled: `ShaderParseUsage2`
    33.3% -> 22.3% of the thread, the association 14.1% -> 3.3%; 52-78 s
    window 400 -> 327 us/draw and 244 -> 209 us/dispatch; frame 1004 -> 1101
    at 100 s.
  - Instruction classes on top: `ShaderParseUsage2` 22.3% -> 17.8% of the
    thread; the redefined and assembled scans fell from 13.1% and 8.1% of it
    to 1.8% and 1.3%, and per-instruction vector-buffer classification left
    the samples.
  - The zero-image sampler check on top: `ShaderParseUsage2` 17.8% ->
    14.7% of the thread; the dynamic scalar-resource collection fell from
    29.7% to 15.7% of it. Per-window us/draw stays within scene variation
    (327-414 us at 33,000-48,000 draws per window).
  - GDS and image-read indices on top: `ShaderParseUsage2` 14.7% -> 10.4%
    of the thread (14 s at 72 s).
- **Verified.**
  - Unit suites `EmulatorGraphicsState*` (301), `EmulatorGraphicsPackets*`
    (284) and `EmulatorShader*` (316) pass; the graphics diagnostics
    integration test passes.
  - Strict sweep of the card title, roguelike platformer, racing title,
    preschool Unity title, insect platformer and fishing title (60-75 s
    each, before the zero-image change): no stop. The robot platformer's
    100 s runs pass frame 1,050 without a stop.
- **Open.** Every context switch of the thread sampled with its user stack
  (8 s at 58 s): blocked 74% of the window, on the CPU 26%. About 54% is
  spent in fence waits (rotating into a recording slot still in flight,
  `WAIT_REG_MEM` producers, indirect-dispatch and draw dependencies) and 20%
  idle waiting for guest submissions. Its next CPU costs:
  `PrepareStorageBuffers` 25% (transient snapshot compare and capture,
  `GpuMemoryCalcHash`).

## Host-only UCONFIG codes in indirect register blocks (2026-10-22)

- **Observed** (agent events of strict runs): the fishing title, the
  preschool Unity title and the racing title send indirect UCONFIG register
  blocks whose pairs 2 and 4 hold offset 0x80003FFD with value 0. That offset
  is not a hardware register. It is one of the host-only codes
  (0x80003FF4..0x80003FFF in `Pm4.h`) that the emulator's UC register-defaults
  tables hand to the guest, each with value 0. The guest copies the entry
  into its block unchanged. The command processor dropped those pairs with an
  `indirect_register_dropped` warning. Across about 400 recent runs this was
  the only dropped pair.
- **Change.** The indirect UCONFIG parser consumes a host-only code that
  holds its default value 0 without a state change, and stops on any other
  value, since no register models one.
- **Verified.** Strict 45 s runs of the fishing and preschool titles on the
  Intel host: no dropped pair, no stop.
- **Open.** Which hardware register, if any, the console library's defaults
  name for this entry.

## Depth array slices addressed and sampled as arrays (2026-10-22)

- **Observed** (bounded trace of the robot platformer):
  - Its shadow passes render a 1536x1536 16-bit depth target with
    `DB_DEPTH_VIEW` slices 0..0, 1..1 and 2..2. The depth and HTILE bases
    stay the same for every slice. Before them come three 0x30000-byte HTILE
    clears, at the HTILE base, +0x30000 and +0x60000. The next HTILE the
    guest allocates starts three strides after the base.
  - The shadow lookup samples a three-layer 2D-array T# at the depth base.
  - A second pool of 32-bit float depth is sampled as a 16-layer array. The
    title renders only some of its layers.
  - At boot the guest declares a 2048x2048 depth target over memory that
    still holds two of the 1536x1536 slices.
  - The renderer ignored the slice, which caused three faults:
    - every cascade rendered into one image;
    - the HTILE clears of slices 1 and 2 matched no target;
    - the sampled array view had one layer, so every cascade read the one
      rendered last.
- **Contract.**
  - The DB finds slice N of each plane (depth, stencil, HTILE) at the plane's
    base plus N times its slice size. For a single-level surface that slice
    size is the plane size the renderer already computes. The guest's HTILE
    clear stride (0x30000 for 1536x1536) matches it.
  - A sampled array layer reads the memory of that layer. A layer that no
    slice rendered holds what guest memory holds.
- **Change.**
  - `DescribeRenderDepthInfo` offsets the three plane bases by the slice.
    Each slice becomes its own depth image and takes its own HTILE clears.
  - Each of the following stops with a message: layered rendering (a slice
    range), slices on the previous generation, and slices of a mipmapped
    depth target.
  - A sampled depth array with several layers reads an array image owned by
    its first slice. A layer is copied from its slice's image when the slice
    was bound or written after its last copy. Depth binds, compute depth fills
    and storage-plane store-backs stamp the image's content.
  - A layer without a slice image is detiled from guest memory into the
    array. It is read only when the view and the range pass the checks of
    the guest-memory depth sample path: a 64 KiB tiled view with one level,
    no MSAA and no metadata (HTILE would change what the bytes mean),
    physical mapping, and no overlapping object that holds newer bytes. It is read again after a CPU write to its
    pages, and at every bind when those pages are not write-tracked. The
    staging buffer belongs to the recording and is freed when its command
    buffer is next used.
  - Each of the following stops with a message: a slice image that was never
    rendered, slices that differ from the first, a layer whose guest bytes
    another object owns, and a nonzero base array.
  - A depth target declared over another depth target's memory reclaims it,
    whatever the bases. Before, only a target at the same base did. The guest
    reuses array memory for targets of another extent.
- **Verified.** Strict runs on the Intel host (console warnings,
  7 GiB memory limit), and `EmulatorGraphicsState` (301 tests) passes.
  - Robot platformer: a 130 s run reaches frame 681 at about 20 fps with no
    stop. Before the guest-layer read, a longer run stopped at 143 s on an
    unrendered layer of the 16-layer pool. With it, runs of 190 s and 171 s
    pass that point with no stop. Under the 7 GiB limit they then stall near
    frame 850: the title peaks at 12.4 GiB, so the run is throttled while
    cold compute compiles run. A warm-cache run with a 14 GiB limit passes
    frame 1,400 with no stop and renders the space intro at 5-8 fps.
  - Sweep of the card title, the roguelike platformer, the racing title,
    the preschool Unity title, the insect platformer sequel and the fishing
    title: no stops. Frame counts stay within the spread between runs. The
    racing route ends in a race or in a menu depending on route timing,
    which also happened before this change.
- **Not verified.** The look of the shadow cascades in gameplay.

## Color CMASK fast clears and the eliminate pass (2026-10-22)

- **Observed** (bounded trace of the robot platformer):
  - Single-sample color targets with `CB_COLOR_INFO.FAST_CLEAR` set and no
    DCC have their CMASK filled with zero by the uniform compute fill. Their
    `CB_COLOR_CLEAR_WORD0/1` hold the clear color (for example the FP16 pair
    0.5, 0.5, 101.0, 0 on a 1024x1024 target).
  - Targets with DCC get CMASK 0xff and carry the clear in their DCC keys
    instead.
  - Draws with `CB_COLOR_CONTROL.MODE` 2 (eliminate fast clear, ROP3 0xcc)
    followed. They ran as ordinary draws, so the bound pixel shader's color
    landed on the target.
  - A target already materialized ignored the CMASK clear, so its previous
    contents showed through every pixel the following draws did not cover.
- **Contract.**
  - A CMASK nibble of 0 marks a fast-cleared 8x8 tile, which the color block
    reads as the clear words. A zero fill from the CMASK base that reaches
    every tile's nibble clears the surface.
  - The eliminate pass writes the clear words into the cleared tiles and
    discards the pixel shader's color.
  - Basis: the trace (zero CMASK only on fast-clear targets without DCC, the
    expanded value on DCC targets) and two independent implementations that
    agree on the zero code and on dropping the eliminate draw.
- **Change.**
  - The pending metadata-clear store, which already held DCC key fills,
    also serves CMASK. A single-level, single-slice, single-sample
    fast-clear target bound after a covering zero fill of its CMASK is
    cleared to its clear words.
  - A mode-2 draw binds each fast-clear or DCC target, which applies the
    pending CMASK or DCC clear, and records no draw. It refuses depth writes,
    which it does not model.
  - MSAA CMASK (FMASK compression states) is not decoded.
- **Verified.** Unit tests 2440/2440. In a strict 130-second run the robot
  platformer applied CMASK clears on seven targets (1024x1024 to 2432x1368)
  and the eliminate pass with no refusal, at an unchanged frame rate. A
  strict sweep (card title, roguelike platformer, racing title, preschool
  title, second insect platformer, fishing title; 60-75 s each) reached
  the same states with no stop and unchanged frame rates.

## Shader decoder refuses unimplemented encodings (2026-10-22)

- **Observed.** The Gen5 decoder turned unimplemented opcodes into `SBarrier`
  records ("treated as SBarrier (continuing)") and encodings used outside
  their generation into warnings. The translator emits nothing for such a
  record, so a shader that used one ran without that instruction's effect.
  This closes the placeholder debt recorded in `docs/bringup/history.md`.
- **Change.** Every unimplemented opcode stops translation with its
  instruction name, and so does an encoding used outside its generation. No
  placeholder record remains.
- **Verified.**
  - A temporary census decoded every opcode value of each encoding: 959
    decode and 4879 stop by name.
  - Strict sweep on the final binary (Intel Arc A770, 60 s per title,
    130 s for the robot platformer): the card title, the roguelike
    platformer, the fishing title, the racing title, the preschool title,
    both insect platformers, the sandbox title, the watercolor platformer
    and the isometric action title reach the same states as before, and
    none stops on a refused encoding.

## Output modifiers on vector results (2026-10-22)

- **Observed.** The robot platformer runs `v_cvt_f32_u32` with OMOD `mul:2`
  and `div:2`. The translator ignored OMOD and CLAMP on every conversion and
  integer emitter, so those results were off by a factor of two.
- **Rules** (RDNA2 ISA 6.2 and 6.5): OMOD scales floating-point results only.
  It is ignored for integer and bit results, in IEEE mode, and when output
  denormals are enabled. CLAMP saturates integer results and clamps float
  results to [0, 1]; on float-to-integer conversions it only enables the
  inexact exception.
- **Change.**
  - Conversions to f32 (`v_cvt_f32_*`, `v_cvt_off_f32_i4`, `v_cvt_f32_f64`)
    apply OMOD and CLAMP through the shared helpers.
  - Integer and bit emitters drop the OMOD warning and refuse CLAMP, whose
    saturation is not translated.
  - Half- and double-precision results refuse CLAMP, and refuse OMOD when the
    double/half denormal field (`FP_DENORM` bit 7) lets it take effect.
  - `v_cndmask_b32`, whose modifiers apply when the selected values are
    floats, refuses either modifier.
  - Float-to-integer conversions accept CLAMP as the exception switch it is.
- **Verified.** Translator version 150, geometry translator version 91.
  The robot platformer reaches its logo sequence (frame 347 at 130 s)
  with no output-modifier warning left in its log.

## Command processor cache events and acquires decoded generally (2026-10-22)

- **Observed** (bounded census of the CP packets five titles emit):
  - `EVENT_WRITE` `FLUSH_AND_INV_CB_PIXEL_DATA` (0x31) and
    `CACHE_FLUSH_AND_INV_EVENT` (0x16) with event index 0 as well as 7, and
    `DB_CACHE_FLUSH_AND_INV` (0x2a) with index 0 from the robot platformer.
  - `WRITE_DATA` with cache policy 3 from the watercolor platformer and the
    preschool title, and with dst 5 from the robot platformer.
  - `ACQUIRE_MEM` with ranged color and depth targets outside the old table
    of observed words (the robot platformer, the card title).
- **Change.**
  - `TriggerEvent` treats 0x16 and 0x31 at index 0 or 7, and 0x2a at
    index 0, as full barriers.
  - `WRITE_DATA` accepts every cache policy, and dst 5 as well as 4: dst
    bit 0 selects the PM4 engine (micro engine or prefetch parser) and
    bits 1-4 hold `DST_SEL`, so both are memory writes through L2. The
    software CP stores immediately, so neither the engine nor the GL2 policy
    hint has a visible effect. Basis: the PM4 field layout, independent
    implementations that agree on the dst encoding, and the title's writes
    landing on valid guest memory.
  - `ACQUIRE_MEM` decodes its size, base and target bits instead of matching
    whole words: no size or no target is a full barrier, a ranged color or
    depth target barriers the images in that range, and `TC_WB_ACTION_ENA`
    writes back.
- **Verified.** The general decode reproduces every case of the old table.
  On the final binary the robot platformer logs no `WRITE_DATA` warning,
  and no sweep title logs an event or acquire refusal.

## SW_64KB_R_X addressed by surface coordinates, 16-byte elements (2026-10-22)

- **Observed.** The robot platformer logged "unsupported SW_64KB_R_X element
  size": a 16-byte storage texture in the render-target swizzle fell through
  to the 8-byte equation, so texels overlapped.
- **Cause.** The 16-pipe `SW_64K_R_X` patterns of the published GFX10
  address library XOR x6 and y6 into bits 10 and 11. For 8-byte elements
  (128x64 block) y6 lies above the block; for 16-byte elements (64x64 block)
  x6 and y6 both do. The address library evaluates the pattern with surface
  coordinates, but `TileGetSw64kRxOffset` passed in-block coordinates, so
  every odd block row of an 8-byte surface had its 32-column strips swapped,
  and 16-byte elements had no equation.
- **Change.**
  - `Tile.cpp`: the pattern takes surface coordinates and gains the 16-byte
    equation (`x0 y0 x1 y1 | x3^y3 x4^y4 y5^x6 x5^y6 | y2 x2 y3 x4`). The
    1-, 2- and 4-byte equations mask their own bits and are unchanged.
  - `gen5_detile.comp` (and its generated include): the same coordinates in
    the detile and raw render scatter kernels.
  - `TileDetile.cpp`: the CPU detile accepts every 64 KiB element size.
  - `StorageTexture.cpp`: a render-target alias whose block sits on rows of
    opposite parity in the two images copies its 32-column strips swapped.
- **Verified.** Unit tests pin the published 16-byte pattern and the second
  block row of the 8-byte one; the existing alias test caught the strip
  swap. The robot platformer no longer logs the
  element-size warning.

## HLE trace lines formatted only when emitted (2026-10-22)

- **Symptom.** With console logging at the warning level, the card title
  ran gameplay at about 60 fps instead of 200-300 fps, and the roguelike
  platformer at about 75 fps.
- **Cause.** Every guest library call runs `PRINT_NAME()`
  (`Hle/Registration.h`, `Kernel/Trace.h`). The macro skipped silent
  output but, for console or file output, formatted the call time
  (`HH24:MI:SS.FFF`) before the level gate dropped the Info line. That
  formatting dominated the host profile. Silent runs never paid it, which
  is why wall-clock runs did not show the drop.
- **Change.** The macro checks the Info gate before it evaluates its
  arguments.
- **Verified** (Intel Arc A770, same scripted segment, frames counted at
  +26 s and +52 s):
  - Card title: about 70 fps before and 196 fps after with console logging
    at Warn; about 260 fps silent, unchanged.
  - Roguelike platformer: 71 fps before and 107 fps after with console
    logging; 92-113 fps silent. Its levels are generated per run, so
    single-run rates vary.
- **Open.**
  - The card title still runs about 25% slower with console logging than
    silent; the remaining cost was not attributed.
  - The roguelike platformer is bound by about 2.2 GPU fence waits per
    frame (1.1 ms each) with the command processor busy 77% of the time.

## Relocation protection round trips at module load (2026-10-22)

- **Observed.** A 150 s strict run of the preschool Unity title logged
  every guest protection change. About 572,000 changes of 8 bytes came
  during boot, two per relocated slot, from `PatchReplace`. It set the
  slot's page to read-write, wrote the slot and restored the old mode,
  even when the page was already writable.
- **Measured.** Timers around each relocation pass (Intel Arc A770, same
  title, 45 s run): 14 passes took 440 ms. 325 ms of it was inside
  `PatchReplace` for 286,201 slots, about 1.1 us each. The largest module
  patched 230,371 slots in 262 ms.
- **Change.** `PatchReplace` leaves a slot that already holds the value
  and writes a slot whose tracked protection allows writes directly.
  Read-only slots keep the protect, write and restore path. A direct
  write to a page under a GPU write lease faults into the existing lease
  handler, like other host writes into guest memory.
- **Verified.**
  - Same title, timers and run length: `PatchReplace` time fell from
    325 ms to about 12 ms (largest module 262 ms to 9.6 ms), and
    relocation as a whole from 440 ms to about 115 ms. The run reached
    the interactive phase with no stop.
  - 2440 unit tests pass. The boundary and graphics table gates pass.
  - The racing title, 75 s strict on the same binary: no stop, race
    reached at 52-80 FPS, clean capture.
- **Open.** The remaining 100 ms or so is spent outside `PatchReplace`
  (symbol lookup and relocation decoding) and was not profiled. Not
  measured on AMD.

## Depth-reference samples over a storage-detiled view (2026-10-22)

- **Symptom.** The racing title stopped now and then on
  `unsupported depth-reference image binding ... format=7 tile=24
  materialize=depth-unsupported ... 1x1 pitch=256 ... overlaps=2`
  (`GraphicsRenderBind.cpp`). It happened in 3 of 55 strict runs since
  2026-10-17. Two of them (2026-10-20, 2026-10-22) shared one overlap set:
  - a writable storage buffer containing the range (written by the GPU,
    write-back capable);
  - a texture equal to the range whose content origin was unknown.
- **Cause.**
  - A depth-reference sample whose range lies in a writable storage
    buffer becomes a texture detiled from that buffer (`depth-storage`).
  - That texture skips the guest upload. Since 2026-10-07, a texture
    that skips its guest upload records an unknown content origin.
  - When the same image was bound again while the texture still lived,
    the D16 source classifier counted it as a texture owner that held no
    CPU-uploaded bytes, and refused the bind.
  - The unit test for that case still modeled the texture as
    CPU-uploaded.
  - The only other way to get a texture that skips the upload is the
    tile-9/27 live-cover path, which never handles tile 24.
- **Change.**
  - Range provenance marks storage depth views: depth-view textures that
    skip the guest upload.
  - The classifier accepts them only while a writable storage buffer
    holds the range. That bind detiles the buffer into the view again.
  - Every other texture over the range still needs a CPU-uploaded origin.
  - The refusal message prints the flag for each overlapping object, so
    a recurrence shows whether a storage view was involved.
- **Verified.**
  - The unit test covers four cases: a storage view with its writable
    buffer is accepted; an unknown-origin texture without the flag, a
    storage view over read-only storage, and a lone storage view are all
    refused. Unit tests pass (2440).
  - A 120 s strict racing run on Intel A770 reached the race with correct
    visuals and about 58 fps, and did not stop.
  - The stop is too rare (about 1 in 25 runs) for one run to prove the
    fix.
- **Open.** The 2026-10-18 occurrence had a different overlap set: a
  texture containing the range, with an alias-invalidation origin, plus
  an equal CPU-uploaded texture. The classifier still refuses that case.
  Direction: decide whether an alias-invalidated texture that contains
  the range can still own content newer than guest memory.

## Shader translator: GDS writes in split and native compute waves (2026-10-22)

- **Regression.** A strict five-title Intel sweep after the paired
  removal stopped both insect platformers at 61 s on
  `shader emitter missing ... inst=DsWriteB32 [VaddrVdataOffsetGds]`.
- **Cause.** The runtime had already been choosing the split layout for
  wave64 compute (native when proven equivalent) before the removal. The
  GDS write lowering existed only in the paired bank emitter, which
  nothing selected any more. The stop therefore dates from the split
  default, not from the removal. Native equivalence still refuses GDS
  access, because GDS results depend on lane order, so these shaders run
  split.
- **Contract (RDNA2 ISA 3.6.1, 3.7, 10.4.3, 10.5).**
  - M0[15:0] is the size in bytes and M0[31:16] the base in bytes.
  - A store with ADDR + offset at or past the size is discarded.
  - A misaligned address is a memory violation.
- **Change.** Each invocation is one guest lane. It stores at
  M0.base + ADDR + offset when its EXEC bit is set, ADDR and
  ADDR + offset are below M0.size, the byte address is dword-aligned and
  the dword lies in the host GDS window. Any other store is dropped. The
  bounds are those of the removed paired emitter.
- **Verified.**
  - The GDS EXEC integration suite gained five write cases: base, size
    bound, single lane, misaligned base, and a split-wave64 high lane.
    They pass on Intel Arc A770 and on the AMD Raphael iGPU (RADV), both
    Mesa 26.2.5.
  - Unit tests pass (2440), the other shader integration suites pass,
    and the boundary and graphics-table gates pass.
  - Strict Intel runs: the void platformer ran 120 s and 200 s, and the
    sequel 120 s, with no stop. The void platformer reaches gameplay
    with a correct scene and HUD.
  - The first run after the fix compiled the new split modules cold and
    stayed black from about 79 s to past 105 s while loading. With warm
    caches, gameplay starts at about 105 s.
- **Open.**
  - Void platformer gameplay runs at about 68 fps (frames between agent
    samples), against about 81 fps on 89e3b892 (paired layout, many
    commits earlier). The route also reaches gameplay about 26 s later
    than on that build. Neither difference is attributed yet.

### Host driver: Mesa 26.2.5 RADV loses a narrowed EXEC

- **Seen** while building a write case whose M0 size bound dropped only
  lane 31 of a 32-lane wave.
- **What happened.**
  - NIR folded the constant bound into an inverse ballot of 0x7fffffff.
  - ACO emitted `s_bitset0_b32 vcc_lo, 31` on the `v_cmpx` mask, then
    copied the result into EXEC.
  - Post-RA copy elimination rewrote this as `s_bitset0_b32 exec_lo, 31`.
    `s_bitset0` also reads its destination, so the instruction now
    cleared the bit in the unnarrowed EXEC.
  - Lanes 0-30 all stored, instead of only the lanes `v_cmpx` selected.
- **Not a translator defect.** Adding any later read of the value hid
  the miscompile.
- **Integration suite.** The cases avoid this shape.
- **Production impact unknown.** A title is exposed only when a
  constant-folded mask has exactly one bit clear.

## Shader translator: paired wave64 layout removed (2026-10-22)

- **Why.** Compute Wave64 dispatch on 32-lane hosts builds only the
  split layout (each guest wave64 spans two 32-wide host subgroups, one
  guest lane per invocation) or the native layout. Nothing in the
  runtime selected the paired low/high bank layout any more; only tests
  built it.
- **Change.** The paired generator, its bank emitters, the paired
  fragment-compute backend, the paired admission analyses and their
  tests are removed. The instruction classifiers that native fragment
  proofs use are kept.
- **Verified.** Unit tests 2440 passed (23 skipped by design), the
  four shader and diagnostics integration suites passed on Intel Arc
  A770 (Mesa 26.2.5), and the boundary and graphics-table gates pass.
  Robot title, warm caches, strict, generic route, 150 s: no fatal stop,
  present 1374 at 137 s (1434 at 146 s before), longest hold after boot
  1.7 s.
- **One-time recompile.** Removing the enum value moved `Split64On32`
  from 2 to 1, and the strategy is part of the shader ID, so the 11 split
  modules met in that run got new keys and were translated and compiled
  again (11 SPIR-V compiles, 5.9 s in total, during boot). Their stored
  SPIR-V binaries are byte-identical to the entries stored under the old
  keys, so the GPU work is unchanged, and the old-key recipes still
  precompile to the same driver-cached pipelines (55 recipes, 49 ms).
- **Generator A/B.** A second robot run with the SPIR-V store set aside
  (Mesa cache warm) translated every module again: all 196 module entries
  it wrote (vertex, pixel, geometry and native and split compute) are
  byte-identical to the entries the previous translator stored under the
  same keys.

## Shader caches: whole-title SPIR-V store and versioned compute recipes (2026-10-22)

- **SPIR-V store.** Toolchain entries (`.spvbin`, keyed by the SPIR-V
  source) stored the full source text as identity, 0.3-2.9 MB each,
  against a 64 MiB store, a 16 MiB session write budget and eviction by
  write time. The robot title's store sat at 63 MiB, 82 % of it toolchain
  entries, and every run exhausted the budget (one run: 12 toolchain
  entries of 12 MiB and 35 modules of 3.7 MiB), so most translations were
  never persisted. The 128-bit source key is now the whole identity, the
  store is bounded at 1 GiB with a 256 MiB session budget, and its size is
  scanned once per session.
- **Compute recipes.** Recipes recorded by older translator versions were
  never invalidated (132 recipes, 34 MB). The two precompile workers start
  with the largest recipes, so they compiled stale 1.5-1.7 MB paired
  kernels for the whole session (minutes each, never finished), while the
  dispatching thread compiled the current kernels itself. A warm-run
  hold profiled at frame 872 showed both workers at 100 % in the driver.
  The recipe directory now carries the shader and geometry translator
  versions (`<title>-<version>-t<translator>-g<geometry>`), and starting
  the precompile removes the title's directories of other versions.
- **Measured** (Intel A770, Mesa 26.2.5, strict, robot title, generic
  route, 150 s, agent diagnostics at about 105 s). With the store fix
  only: 358 translation hits, 2 misses, two workers still on stale
  recipes, frame-time p99 2.57 s, longest `WAIT_REG_MEM` 1.25 s, longest
  flip wait 1.88 s, frame 1187 at 116 s. With both fixes, first run
  (recipes re-recorded): p99 0.38 s, longest `WAIT_REG_MEM` 22 ms, frame
  1415 at 137 s. Second run: 54 recipes compiled in the background (8.7 ms
  at most, driver cache hits), no translation miss, no SPIR-V compile, p99
  0.33 s, frame 1434 at 146 s; the longest hold (5.8 s) is the next level
  load, which the title itself reports as 4.1 s.
- **Open.** The Vulkan pipeline cache file (74 MB for this title) is
  rewritten whole at each checkpoint against a 128 MiB session budget, so
  one checkpoint per session fits; not shown to cost a warm run while the
  driver's own disk cache is enabled.

## Shader translator: split geometry pass and compute row DPP (2026-10-22)

- **Geometry pass contract.** A merged ES+GS draw run as a compute pass
  now uses the split wave64 layout (`Split64On32`, required subgroup 32,
  one guest lane per invocation, workgroup of the guest subgroup's lanes).
  The paired geometry analysis, its classifier fallback to the compute
  classifier and its `+8` EUD rebase are removed: non-export instructions
  go through the split compute admission, exports and `GS_ALLOC_REQ`
  through the geometry classifier. The native exec init no longer writes
  the NGG `s3` prologue for geometry compute, whose own prologue sets it.
- **Row DPP in compute waves.** Row shifts, rotates and mirrors (DPP16
  controls 0x101-0x12f, 0x140, 0x141, full row and bank masks) were
  translated only in pixel shaders. A native wave32 compute wave and each
  half of a split wave64 hold whole 16-lane rows and no ghost lanes, so
  they now use the same per-row shuffle. The ISA DPP16 table states that
  BOUND_CTRL=1 writes zero for an out-of-row source and FI=0 reads zero
  from an inactive lane; the lowering substitutes exactly that zero, so
  BOUND_CTRL=1 shifts are admitted outside the proven fragment region.
  BOUND_CTRL=0 still needs the destination-preserving or/xor/add shape,
  and the proven fragment region, whose subgroup is narrower than the
  guest wave, still refuses BOUND_CTRL=1 shifts.
- **Verification.** Unit and shader integration suites pass; the robot
  platformer's logo stretch renders its particle dissolve (title record
  of the same date). The role-playing title's traveler map was blue
  before and after this change; the cause was the scalar-load base
  defect recorded in that title's file.

## Shader translator: float contraction and coherent loads (2026-10-22)

- **NoContraction on guest float adds and multiplies: no effect, not
  merged.** RDNA2 rounds `v_mul_f32` and `v_add_f32` separately, and SPIR-V
  lets a driver fuse an `OpFMul` that feeds an `OpFAdd` unless the result
  is decorated `NoContraction`. A translator change decorated every guest
  float add and multiply result. In the pixel path each guest result passes
  through a run-time `OpSelect` on the lane's EXEC bit, recomputed from the
  native ballot (quad broadcast plus helper invocation), so no guest
  multiply feeds a guest add directly and the driver cannot fuse the pair.
  No visible or measured change on Intel Arc A770. The change alters every
  shader hash, so it waits for a translator version bump that needs it.
- **Defect found, not fixed: GLC and DLC loads are not coherent.**
  `ShaderParseMUBUF.cpp:30` logs and ignores GLC on buffer loads, storage
  buffers are not decorated `Coherent` (only GDS is,
  `ShaderSpirvGenerator.cpp:1179`), and the translator emits no atomic
  loads. On RDNA2 a GLC=1 load misses L0 and reads L2, and DLC also misses
  L1: a kernel that polls a word another workgroup of the same dispatch
  publishes relies on this. Trigger: a cross-workgroup flag or counter
  poll; no title has been seen to need it. It does not explain the
  role-playing title's GPU hang, which stops in fixed-function state with
  the execution units idle. Direction: translate GLC or DLC word loads as
  device-scope atomic loads with acquire semantics.
- **Defect found, not fixed: barriers in lane-conditional regions of the
  native wave64 route.** The native-equivalence proof refuses scalar
  memory side effects inside a region that `s_cbranch_execz` or
  `s_cbranch_vccz` can skip (`ShaderComputeWaveNativeEquivalence.cpp:960`),
  but `HasScalarSideEffect` (`:179`) does not include `s_barrier`. The
  native route needs no particular host subgroup size, so one guest
  wave64 can span several host subgroups, and each subgroup takes the skip
  from its own ballot. A host subgroup whose lanes are all inactive skips
  the region and its `OpControlBarrier` (`ShaderSpirvBuffer.cpp:3102`)
  while the rest of the guest wave executes it, which is undefined in
  Vulkan and can hang the GPU. On the guest the whole wave executes the
  barrier as long as any lane is active. Trigger: a barrier inside such a
  region whose condition differs between the 32-lane halves of a guest
  wave; no admitted kernel has been seen to do this. Direction: refuse
  `s_barrier` inside lane-conditional regions in the proof, or keep the
  skip off for regions that contain one.

## Depth sampling: integer reads and dropped pending clears (2026-10-22, found by inspection)

- **Defect found, not fixed: integer reads of a live depth target.** A
  Gen5 texture with tile mode 24 is looked up as a depth target whatever
  its format: `GraphicsRenderBind.cpp:3542` only logs a format other than
  22, and `:3680` then binds the depth target's depth view. For a regular
  sample the descriptor follows the guest format, so an unsigned-integer
  format lands in the integer sampled set (`:4435`) over a float depth
  view. Vulkan requires the view's numeric format to match the shader's
  sampled type (VUID-vkCmdDraw-format-07753), and the guest expects the
  raw depth bits. The depth-reference check (`:4270`) runs only for
  depth-reference and mixed sampling. Trigger: an integer T# over a depth
  target that is still live; no title has been seen to do this. Only 2 of
  1055 recorded run logs keep ignored-condition warnings, so their absence
  is not evidence. Direction: refuse an integer format over a depth
  target, or copy the depth bits into an integer image of the guest
  format.
- **Defect found, not fixed: pending depth clears are dropped.**
  `RetainPendingBounded` (`Objects/DepthMeta.cpp:43`) keeps at most 64
  pending HTILE clears, keyed by address, and erases the oldest when a
  65th address is cleared before any is consumed. The depth bind that
  should consume the erased clear then finds none
  (`GraphicsRenderAttachments.cpp:762`) and keeps the previous host depth.
  The stale-depth ghosts fixed on 2026-10-19 had this symptom from another
  cause. Trigger: more than 64 distinct depth metadata ranges cleared
  between their binds; not observed. Direction: stop with the pending
  count instead of dropping a clear, since the drop changes guest-visible
  depth.

## EXECZ skip over a join shared with an outer branch (2026-10-20)

- **Symptom.** The well platformer stopped at 77-122 s on the new-game
  route: a pixel shader (crc d0354e07) failed SPIR-V validation ("branches
  to the selection construct, but not to the selection header").
- **Shape.** A uniform `s_cbranch_scc0` at 0x38 targets 0x98. Its
  fallthrough masks EXEC, and `s_cbranch_execz` at 0x50 skips to 0xe0, past
  0x98. Rule 1 of `FindReconvergence` merges the uniform branch at 0x98, but
  the skip can bypass 0x98. Block 0x98 is shared by two regions that are
  not nested, so no structured selection expresses the program.
- **Fix.** A parse pass (`ShaderExecSkip.cpp`) removes an
  `s_cbranch_execz` when three conditions hold:
  - another branch, from outside the skipped range, lands inside it;
  - every instruction in the range only has EXEC-masked effects, or
    defines scalar words (no EXEC writes, no memory writes, no exports,
    no `v_writelane`);
  - none of those scalar words (including implicit SCC and VCC) is read
    on any path from the target before being redefined.

  Under these conditions, running the range with an empty EXEC leaves the
  same state as the skip. Here the range writes only `vcc`, which is dead
  after 0xe0. A nested skip, where nothing enters the range from outside,
  is kept, so empty waves still skip their work. The parser's fallthrough
  label for the removed branch is disabled with it. Translator version 141.
- **Tests.** One unit test reproduces the validator error without the
  pass and validates the SPIR-V with it. A second test keeps the skip when
  the target reads a scalar the range defines. The compute-wave equivalence
  and fragment mask-flow suites still pass (2480 tests).
- **Verified.** Intel Arc A770, Mesa ANV 26.2.5, new-game route. Before
  the fix the well platformer stopped at 85 s. A first version of the pass
  (without the entered-from-outside condition) ran 181 s and reached
  gameplay (frame 6827). The final version ran 101 s in the ten-title sweep
  without a stop, and all ten titles ended with status 1. A 180 s run of the
  final version passed the 122 s stop seen before and ended in gameplay
  (frame 6837, about 40 fps).

## Descriptor churn and frame hitches on Intel (2026-10-19)

- **Hosts and method.** Intel Arc A770, Mesa ANV 26.2.5, Xe kernel driver,
  strict runs of 150-200 s with agent perf snapshots every 30 s. Compare
  µs per draw per segment. Do not compare fps across segments with
  different content.
- **Descriptor churn (racing title, gameplay).**
  - About 40% of descriptor-set lookups miss, and the cache sits at its
    65536 live-set cap.
  - A miss usually differs from a cached set in one storage-buffer slot,
    or in the vertex-sharp uniform spill.
  - That spill is bound in about 35% of calls and nearly always misses,
    because it is uploaded into a new transient buffer on every draw.
  - About 30% of misses re-create a set the cap had evicted.
  - Cost per miss before the fix:

    | Step | Cost |
    | --- | --- |
    | Index the set | 1.4 µs |
    | Unindex an evicted set | 1.7 µs |
    | Trim batch | about 18 ms (about 1.2 µs per call, amortized) |
    | `vkUpdateDescriptorSets` | 0.37 µs |
    | Allocate | 0.25 µs |

- **Fix (d3206e17): positional reverse index.**
  - Each set records its position in every resource's reverse-index list,
    so unindexing swap-removes its entries in constant time.
  - Descriptor finalize per draw dropped from 5.6-7.0 µs to 4.0-4.7 µs in
    comparable racing segments.
  - Index and unindex left the top of the CP profile.
  - A 200 s fishing-title run with an invariant check on every index and
    unindex (temporary diagnostic) found no inconsistency.
  - The card title ran at 302-318 fps in two 150 s runs. These are single
    runs, not an attributed gain.
- **Earlier CP changes on this branch.**
  - Label holes in an ordered map, scanned from the first candidate (06d216af).
  - Table-driven instruction classifiers (d6a2cb42).
  - Together they removed about 11% of CP self time. In the heavy racing
    segment that is only about 1% of µs per draw.
- **Excluded: transient-pool affinity.** Reusing a transient snapshot from
  the same pool when its guest address repeats did not move the miss rate
  (40-50%). The guest rotates its constant-buffer addresses, so the keys
  differ anyway. Reverted.
- **Per-draw shader resource scans (fishing title, gameplay).**
  - In a 30 s user-space stack sample of the CP thread, scans of the shader
    instruction list on every draw took about 20%. The largest single cost
    was the EUD descriptor scan in the pixel path, about 5% with its
    classifiers. It walked every instruction, and walked back from every
    image consumer to its block start.
  - Excluded: memoizing the whole resource parse. A temporary counter over
    20,000-call windows saw about 55 programs but 12,000-14,000 distinct
    inputs (user SGPR values plus EUD table words). Even an ideal cache
    would hit at most about a third of calls.
  - Fix (a11c3ce9): the per-program resource memo records the code-only
    candidates once. These are the whole-descriptor S_LOADs at a constant
    offset, and the two S_LOAD_DWORDX4 halves of each split T#. Each draw
    applies only the pointer, entry-value and EUD span checks to them.
  - Equivalence: a temporary dual evaluation compared the old and new
    scans call by call. Results: fishing title 700,000 calls, role-playing
    title 80,000, puzzle title 32,000, robot platformer 40,000, unit suite.
    No mismatches. None of these titles reached a split T#, so new
    contract tests cover that path; a mutation check confirmed they fail
    without it.
  - Result: the scan dropped from about 3.6% to 0.3% of CP self time.
    Gameplay µs per draw went from 32.4-34.5 to 31.2-33.3, and p50 from
    42-43 ms to 41 ms. These are single runs with the sampler attached,
    within run-to-run noise.
  - Fix (c10b9cb4): the pixel path's device-address decision scanned every
    instruction three times per draw (about 2.5% of CP time). The immutable
    pixel analysis now lists the buffer, typed buffer and scalar loads, and
    each draw evaluates the same predicates on them only. A temporary
    old/new comparison over 4.85 million fishing-title draws found no
    mismatch; 3,027 of those draws needed the table.
  - Racing title after both fixes: 28.0-29.6 µs per draw in the late
    segments, against 30.6-32.4 in an earlier run of the same day. Single
    runs whose route phases differ, so this is indicative only. Captures
    show correct rendering.
  - Still per draw: redefined and assembled buffer descriptors (about
    1.5%), the EUD span scan (about 0.7%), and vertex-path scans without a
    memo.
  - Fix (93765196): texture binding read two diagnostic environment
    switches per bound texture; they are now read once per process.
    Environment reads were about 1.5% of CP time in total; the remaining
    ones are switches that tests set at runtime.
  - Fix (2026-10-23): with console logging at the Warn level, as in the
    sweep runs, every pipeline lookup still built the shader input-info
    dumps and every draw ran `hw_print`, because both checked only that
    logging was not silent; the level filter then dropped every line. In a
    CP-thread sample of the fishing title's dock they took about 2.5% of
    the thread (input-info dumps 1.8% with their level checks, `hw_print`
    0.6%). They now also require the Debug level, and a later sample of
    the same scene no longer shows them. Three more per-draw switches
    (`KYTY_DUMP_VIDEO_BIND`, `KYTY_DUMP_STORAGE_IMAGE`, `KYTY_DUMP_AA_REGS`)
    that no test sets are now read once per process too.
  - Excluded: caching the shared-backing population query that guest
    address preparation issues once per draw (one `fstat`, about 0.7%).
    It is the check that detects newly populated physical pages, so a
    cached value would miss them.
  - Always-on CP stage timers (`DebugStatsScopedTimer`, clock reads) cost
    about 6% of CP time. They feed the `kyty_agent` performance `_ns`
    fields, so making them opt-in is a contract change and is not done.
  - Fix (a0cf04b2): the elapsed-time statistics read `DebugStatsClock`
    instead of `steady_clock`. It reads the time-stamp counter (6.8 ns
    against 17.5 ns per read on the A770 host) and scales it to
    nanoseconds with a rate calibrated once against `steady_clock`. The
    `_ns` fields keep their unit. Hosts without an invariant counter, or
    whose kernel clock source is not the counter, keep `steady_clock`.
    A unit test checks the clock against `steady_clock` over 50 ms.
    Fishing title, warm caches, gameplay segments: p50 frame 38-39 ms to
    37 ms, 28.7-29.7 to 27.7-28.5 µs per draw, 670-693 to 695-716 flips
    per segment. Single runs; the gain is about 3%. A seven-title sweep
    on a0cf04b2 reached the same states as the previous sweep, with fps
    within run-to-run noise.
  - Fix: a disabled easy_profiler block still reads the clock twice for
    its frame counter (`[vdso]` and `profiler::Block` in the CP profile).
    Profiler blocks are now opened only when the configuration selects a
    profiler output. The draw and binding stage timers and the driver
    pipeline-cache save check use `DebugStatsClock`, and a clean pipeline
    cache skips its clock read. A unit test checks that blocks are
    recorded only when enabled. Fishing title, warm caches: 28.5-29.4 to
    27.2-28.1 µs per draw, CP time per flip 33-34.6 to 31.6-32.7 ms;
    single runs.
- **Frame hitches (fishing title, gameplay, steady segments).**
  - **One frame of about 120 ms per 30 s segment is the harness's native
    capture.** Its flip sequence equals the capture's frame number in every
    segment of two runs. It is not a runtime hitch. Do not count
    `frame_time_max_us` or `frames_over_100ms` in a segment that holds a
    capture.
  - **`vkQueueSubmit` sometimes takes 10-124 ms.**
    - Seen on the graphics and the utility queue alike, a few times per
      200 s run. A later 200 s run at 93765196 had none in its four
      gameplay segments: apart from the capture frame, no frame exceeded
      67 ms (p50 40 ms, p99 43-52 ms).
    - The queue mutex wait is 0. Thread CPU time equals wall time, and all
      of it is system time (`getrusage`).
    - The submit therefore spins in the kernel's exec path; it is not
      waiting on the GPU.
    - Kernel proactive compaction (`vm.compaction_proactiveness=20`) runs
      every 32 s. One pass migrated 5966 pages inside the same 0.5 s window
      as a 39 ms submit, while three other passes in that run caused no
      slow submit. In another run the slow-submit bursts were 31.8 s and
      64.7 s apart, one and two compaction periods. The trigger is likely,
      not proven.
    - Guest physical windows are imported as host pointers. Xe implements
      those as userptrs that it must revalidate after their pages move.
    - Importing the windows as dma-bufs (`udmabuf`, pinned pages; temporary
      diagnostic switch) removed every submit over 10 ms in one 200 s run.
      That run's steady p50 frame time was 46-47 ms, against 42-45 ms with
      host pointers. A second dma-buf run confirmed the cost: steady p50
      46-48 ms (about 21.3 fps) and 33.6-35.9 µs per draw, against 43 ms
      (about 23.5 fps) and 32.5-33.5 µs per draw in two host-pointer runs
      on the same warm caches. Pinned imports trade the rare stall for
      about 9% fps on every frame, so they are excluded as the default.
    - Not fixed. Direction: the host setting
      `vm.compaction_proactiveness=0`, which is the host owner's choice.
      An A/B with it is still needed to prove the trigger.
    - Two later 200 s runs on a quiet host (93765196) had no slow frame in
      gameplay apart from the capture frame. A 4 Hz poll of `/proc/vmstat`
      over the second run saw no compaction and no page migration at all
      (`compact_*`, `pgmigrate_success` unchanged). The stalls therefore
      need a fragmented host; a stall-free run without compaction is
      consistent with the trigger but does not prove it.
    - Owned snapshots of flexible memory are host-pointer imports too.
      They stayed host pointers in the dma-buf run, which had no slow
      submit, so whether they stall is untested.
  - **Excluded: uploads waiting behind graphics work.**
    - Small synchronous uploads (`UtilFillImage` and `UtilCopyBuffer` in
      `Utils.cpp`) took up to 176 ms.
    - The A770 family exposes one graphics queue, so the graphics, utility
      and present roles share one `VkQueue`.
    - The time was not the fence wait (0.1-3.7 ms). It was the submit
      described above.
  - **Still open.**
    - Pipeline misses of 50-180 ms with cold caches.
    - A run on warm caches still compiled 75 SPIR-V modules during
      loading. The route diverged in that run, so unstable cache keys are
      not established.
    - Area transitions take one 0.4-1.0 s frame.
    - Entering gameplay from the load (fishing title, warm caches, a0cf04b2)
      takes one frame of 0.1-0.9 s, different in each of four runs. In the
      0.9 s frame there was no pipeline miss and no SPIR-V compile, no
      wait above 50 ms, and no command-processor run above 42 ms. The delay
      is therefore upstream of the command processor: guest work, or a
      guest thread inside a runtime call or the host kernel. Two runs with
      a 100 ms poll of per-thread CPU and read calls (reads through the
      FUSE NTFS mount that holds the game files appear only in `rchar`)
      had no gameplay frame over 166 ms apart from frames next to the
      harness capture. The cause is open.
    - On cold caches, which the harness uses unless a run overrides them,
      entering gameplay compiles 16 pipelines in one 464 ms frame. The
      SPIR-V cache key includes the shader translator version, so every
      translator version change makes existing caches cold again.
- **Audio after the SDL 3 migration.** The playback stream is named after
  the executable instead of "SDL Application". A per-application mute that
  the session's audio policy had stored for that name silenced the
  harness's RMS tap. The SDL disk driver showed the emulator's output was
  audible. This is host state, not a code defect.
- **Defect seen once.** `DevToolsLifecycle.LiveStallCapturesOnceAndContinues`
  (`source/unit_test/src/devtools/UnitTestDevToolsLifecycle.cpp`) ran for
  more than 20 minutes at full CPU in one full-suite run, then passed in
  750 ms on the rerun. Direction: give the stall wait a bounded deadline
  and find the missed wake.
- **Fixed (ad8d4c76).**
  `EmulatorAudio.StandardSamplerRackDestroyWaitsForACallbackRunningOnAnotherThread`
  (`source/unit_test/src/emulator/UnitTestEmulatorAudio.cpp:8715`) hit its
  10 s barrier timeout in one full-suite run. The render thread did not
  miss the end callback: a rack destroy keeps one pin and waits for the
  other leases to go, but a lease release woke it only at zero pins, so a
  lookup released after the destroy began left it blocked. A release now
  wakes the destroy when at most its pin remains (`docs/AUDIO.md`: 24 of
  400 repeats failed before, 0 of 1,000 after). A test process that hung
  for hours at exit predates the fix; its core showed the main thread
  destroying a condition variable in `exit` while the blocked destroy still
  waited on it. On 2026-10-23 the test passed 15 of 15 runs on an idle host
  and 30 of 30 under a 12-core load.

## Open: WAIT_REG_MEM poll address 1 crashes the command processor (2026-10-17)

- Seen once in 11 strict Intel runs of the penitent platformer (new-game
  route), about 60 s in. The two runs after it on the same binary were clean.
- The CP thread faulted reading address `0x1` in
  `CommandProcessor::WaitRegMem32`
  (`source/emulator/src/Graphics/GraphicsRun.cpp`, first dereference of
  `*addr`). It was reached from `cp_op_wait_reg_mem_32` for a 7-dword
  `R_WAIT_MEM_32` packet (header `0xc0051028`).
- Neither emulator encoder can produce that address: both clear the low two
  bits (`GraphicsDcbWaitRegMem` and `GraphicsAgcWaitRegMemPatchAddress`). The
  packet was either encoded by the guest itself or read before the guest
  finished writing it.
- The existing guard only catches `nullptr`. Skipping the wait would hide the
  producer; the manual's rule ("prefer producers over symptoms") applies.
- Suggested direction: when the address is not a mapped guest page, stop with
  the packet dwords, the submission id and the command-buffer offset instead
  of faulting. Then use that context to find the producer: guest encoding, or
  a CP read that runs ahead of the guest write.

## Index snapshots over linked surfaces (2026-10-17)

- **Symptom.** After the device publication below, the sequel's next fence
  cost was the synchronous index upload. `update_func` → `UtilCopyBuffer`
  on the utility queue showed 23k waits and 6.5 s per ~80 s of play
  (symbolized fence stacks; `GraphicsRenderDrawIndex` →
  `GpuMemoryCreateObject` → IndexBuffer `create_func`).
- **Cause.** An indexed draw first tries a per-command-buffer snapshot of
  the index bytes (`TryUploadTransientReadOnlyBuffer`). The snapshot
  accepted only buffer overlaps. A refusal census (opt-in counter, one
  150 s run) showed that nearly every refused index range, all ≤ 4 KiB,
  lay inside a live surface:
  - Texture Contains: 36.7k.
  - RenderTexture Contains: 12.1k.
  - StorageTexture Contains: 2.5k.
  - Mixes of these with read-only buffers make up the rest.
  
  The refused range then became an IndexBuffer object, uploaded and waited
  on every time its guest bytes changed.
- **Why the snapshot is equivalent.** The create path links an IndexBuffer
  under these surfaces (`GpuMemoryAllowsIndexContainedInSurface`) and
  uploads it from guest memory. It is never seeded from the surface: render
  alias materialization applies only to incoming storage images, and the
  index object has no create-from-objects function. The snapshot copies
  the same guest bytes at the same bind point.
- **Change.** The index admission (renamed `IdleWritableStorage` →
  `IndexView`) also accepts the surfaces that rule links, with the same
  relations; a depth target qualifies only when crossed. Buffers keep their
  rules: read-only, or idle writable storage. Vertex and storage snapshots
  are unchanged.
- **Boot stall check (fishing title, 10 boots of 45 s on this build).**
  - 8 of 10 boots were clean.
  - The other 2 stopped at frame 3 with 2 presents. The `stall-diagnostics`
    counters showed 1 draw, 1 dispatch, no index binding
    (`draw_index_buffer_binding_ns` 0), 3 transient probes (all hits) and
    no device-address preparation.
  - The changed admission never ran before those stalls, so this frame-3
    boot stall is an independent, intermittent defect. It also weakens the
    attribution in "Index buffer objects through the mapping: excluded
    (2026-10-11)": that stall had the same signature.
  - It predates both of today's graphics changes. The run history holds the
    same stall on builds f91e7106 (2026-10-14) and 4175df97 (2026-10-16):
    6 of 85 recorded runs of the fishing title.
  - It is a hang, not a slow boot. In 200 s runs, status still read frame 3
    and 2 presents 168-204 s after the last present.
  - Open defect: the stall is at frame 3 with every guest thread waiting,
    and `sync-waits` is disabled by default.
  - Direction: enable sync-wait tracking and take host stacks of the
    command-processor thread at the stall. The 2026-10-11 record saw it
    runnable inside the driver's graphics pipeline creation.
  - 2026-10-23: the stall recurred once on an earlier build of the day.
    - In 2 of the 3 stall snapshots of 2026-10-22/23, an IL2CPP-started
      guest thread was inside emulator code (host return address, no
      recorded wait type). The third snapshot did not show it.
    - The next 28 boots in a row were clean (8 sweep boots, then 20
      boots stopped at 16 s once past frame 3), up to the build with the
      compute ring wrap fix. At the historical rate of about 7%, 28 clean
      boots make a remaining stall unlikely (about 13%) but do not show
      that it is gone.
    - It stays open. The next stall needs host stacks, not only agent
      snapshots.
  - Cause found later on 2026-10-23 from host stacks: a collector context
    request delivered under the virtual-memory lock. Fixed by "Collector
    context requests delivered only at safe points".
- **Evidence (Intel, strict, one run each, capture-method fps).** Every run
  ended in status 1 with no error, and the final captures show intact
  geometry.
  - Sequel, gameplay windows: 85.6/90.2 fps, against 73.0/77.9/73.2 on the
    previous build.
  - Sequel, perf snapshots: 8.1k-9.0k fence waits and 8.8-9.6 s per 26 s
    window, against 15k and 11.5-12.0 s.
  - Insect platformer: 115-127 fps in play, against 90-91.
  - Card title: 183-234 fps.
  - Fishing title: 21-22 fps, unchanged.
  - Beat 'em up: 14-17 fps, unchanged.
  - Sandbox title: same timeline as the previous build, about 12k frames,
    then the same late drop that main18 shows.
  - Emulator unit suite: 2245 passed, 9 skipped.

## Same-queue storage published on the device before guest-address reads (2026-10-17)

- **Symptom.** In the sequel's gameplay on Intel, about 46% of each 26 s
  window was spent in CPU fence waits (12.0-12.2 s, 21.8k-22.5k waits).
  Symbolized fence stacks (~80 s window) attributed them to:
  - `RetryGuestDeviceAddressDraw` (`GraphicsRun.cpp`): 12.4k waits, 16.9 s.
  - Synchronous create/update uploads through a temporary command buffer:
    23k waits, 6.5 s.
  - Present: 4.4k waits, 1.6 s.
  
  The GPU render engine was 42.9% busy (xe fdinfo cycles), so the frame is
  bound by CPU waits, not by the GPU.
- **Cause.** `PrepareGuestDeviceAddressForRead` refuses a draw while any
  writable storage in the registered device-address ranges has an incomplete
  use. The draw then waits for the CPU write-back. A census showed both the
  producer and the consumer of every such storage on queue 8, so the guest
  only needs device order, not a CPU round trip.
- **Change.** `PublishPendingStorageToGuestAddress`
  (`GraphicsRenderAttachments.cpp`) handles the case where every incomplete
  use is on the consumer's queue and `StorageIsWholeGuestContent` holds:
  - Conditions: one block, no linked objects, no depth metadata, hashed and
    page-fault tracked, no guest write since the last sync, and no label
    fence in the range.
  - It records a buffer-to-import copy between barriers in the consumer's
    command buffer and marks the object published by that submission.
  - The submission becomes a use of the object (`RecordUse`), so release and
    the completion write-back follow the copy.
  - That later write-back adopts the device result (page snapshot and
    write notification) without writing guest memory again
    (`StorageBufferWriteBackPublishedDevice`), so a CPU store made after the
    copy is not overwritten. The unit test
    `EmulatorLabelPublication.DevicePublishedWriteBackKeepsLaterCpuStores`
    covers this.
  - The rest keeps the CPU wait: any other queue, aliases or depth metadata.
- **Evidence (Intel, strict).** Frame rates use the main18 method: frames
  between two native captures divided by the seconds between them.
  - Sequel, A/B on one build. With the change: 67.6/76.8/71.5 fps. Without
    it: 58.7/71.5/65.2 fps. main18: 54.8/58.5/50.9 fps.
  - Sequel, perf snapshots per 26 s window. With the change: 15.1k/14.7k
    waits, 11.5-12.0 s. Without it: 22.5k/21.8k waits, 12.0-12.2 s.
  - Sequel, final build: 68.5-77.9 fps.
  - Card title: 181-210 fps.
  - Beat 'em up: 14-17 fps (main18 15-20).
  - Fishing title: 21-22 fps (main18 16-23).
  - Sandbox title: 49-79 fps, then 10 and 8 in the last windows. main18
    shows the same late drop: 64-66, then 37.5, 2.0, 0.6 and 6.6.
  - Insect platformer, first run: a strict stop at 182 s (see below).
  - Insect platformer, rerun on the same binary: passed 200 s at 90.0/91.3
    fps in play (main18: 62.9/53.4/50.7).
- **Not tested.**
  - AMD: the iGPU is wedged by gfx ring timeouts and needs a host reboot.
  - `scripts/kyty_playable_regression.py` was not run. The emulator unit
    suite (2245 passed, 9 skipped), `check_emulator_boundaries.py --strict`
    and `check_graphics_tables.py` were run.
- **No repeated copies.** `CollectWritableStorage` skips a storage that is
  already published on the consumer queue at its current `write_uses`. A
  later device-address draw in the same command buffer therefore neither
  copies again nor waits for the publishing submission.
- **Excluded or left open.**
  - The largest remaining refusal is a 32 KiB storage that is exactly the
    HTILE block of a live depth target. A compute clear rewrites it every
    frame. It is depth metadata, so its write-back keeps its side effects and
    it still waits. Do not widen the gate to it without a depth-metadata
    contract.
  - A 49 KiB storage contained in a storage image also still waits.
  - Which storages alias depends on where the guest allocator places them in
    each run, so the hot set and the gain vary between runs.
  - Next lever: the synchronous create/update uploads (6.5 s per 80 s).
- **Open defect: `Texture - Contains - StorageTexture` (insect platformer,
  1 of 2 runs).**
  - Stop at `GpuMemoryCreate.cpp:1545`, 182 s into play.
  - A new 256 KiB storage image (0x40000) lies inside a 2 MiB CPU-uploaded
    Texture that is in use, not read-only, written by the GPU, and has 272
    linked parents.
  - `create_texel_bits_seed` only considers `Equals` relations, and no
    single-parent rule covers `Contains` for this pair.
  - Not attributed. The change does not touch this create path directly,
    but it does change storage lifetimes and timing.
  - The rerun on the same binary passed. Earlier runs of this title never
    hit it: the main18 Intel and AMD sweeps and the AMD platformer rerun,
    200 s each.
  - A second occurrence, `Texture - Crosses - StorageTexture`: a 256 KiB
    storage image at 22 s, in a run with console logging (2026-10-17). The
    console output changed the timing. The same relation family appears at
    a different time, which points to allocation placement rather than one
    fixed scene.
  - Direction: decide from the guest's descriptors whether the storage image
    reinterprets the texture's bytes (seed from guest memory and link, as
    `GpuMemoryAllowsStorageTextureContainedInRenderTarget` does) or only
    reuses freed memory (reclaim).

## Review findings recorded for later (2026-10-17)

An automated review of the main-sync pull request raised these. Each was
checked against the code; none is hit by a title in the current strict runs.

- `NetSocketAbort` (`Network.cpp:2111`): an abort with flags 0 while another
  thread is blocked in a blocking recv or accept stops on
  `EXIT_NOT_IMPLEMENTED`. This is a deliberate refusal, not a fake success:
  a real abort must wake the blocked host call and make it return the abort
  error. Direction: wake it (for example `shutdown` of the host descriptor
  or a self-pipe in the wait), map the result to the guest's abort error
  from a trace, and keep the pending-abort flags refused until they are
  evidenced.
- NGS2 sampler append (`AudioNgs2.cpp:2097-2105`, `Ngs2AppendSamplerPieces`)
  is not transactional. When a later piece of a multi-piece APPEND is
  unreadable or reaches `kMaxStreamPieces`, the earlier pieces are already
  appended when the call returns `kNgs2InvalidControl`, so a retry would
  duplicate audio. Direction: decode every piece first and commit them with
  one sampler call that checks the length and piece limits for the whole
  set.
- NGS2 stream budget (`AudioNgs2.cpp:2088`): `Ngs2SampleBudgetFits` counts
  the frames already played in the front streaming block, because
  `Playback::DropPlayedFrames` only runs inside `AppendPiece`, after the
  check. A stream that reaches the per-voice cap could then stall even
  though most of its frames were consumed. Direction: release the played
  frames before the budget check. The card title's 330 s run stays far
  below the cap.

## Uncompressed standard 64 KiB mip chains (2026-10-16)

- Observed (Intel, strict, card title, `generic` route): cards, jokers, the deck and the blind chips
  showed a mosaic of their sprite atlas, and the title-screen logo showed small copies of itself
  among large displaced blocks. UI frames and text were correct.
- Evidence: the sample-bind catalog shows every sprite atlas as format 56, tile 9
  (`SW_64KB_S`), 8 levels (`last_level=7`), with non-power-of-two sizes such as 1420x3040 at pitch
  1536. Level 0 as uploaded to the host image held the smaller levels at its top and the full-size
  sprites shifted below. `TileGetTextureSize` sent tile-9 chains of uncompressed formats through
  the precomputed-table lookup. The table has no tile-9 entries, so the fallback logged
  `levels != 1` and sized one level at offset 0. The upload then detiled level 0 from the start of
  the allocation, where the chain keeps its mip tail and smaller levels.
- Change: a tile-9 texture with more than one level takes the standard 64 KiB mip layout for
  its size and its upload, whatever the format. Before, only block-compressed formats did. GFX10
  places thin 64 KiB levels by block size and element size alone. That layout already served the
  block-compressed and render-target chains. Single-level uncompressed textures keep the one-level
  path.
- Verified (Intel, strict, card title, 60 s): the uploaded joker atlas level 0 is a regular grid
  of 142x190 cards from the origin. The tutorial joker, the deck back, the blind chips and the
  button glyphs render correctly. The racing (200 s) and fishing (240 s) strict runs bind no tile-9
  texture with mips, reach their usual states, and render as before.

## Pixel ADD_TID spill slots across control flow (2026-10-16)

- Observed (Intel, strict, world-entry): in a newly created sandbox-title world, the run
  stopped at 285 s with `fragment wave transport is required by the pixel stage ... reason=memory
  write: helper lanes would be observable` at `pc=0xc78`, guest wave64, no 64-lane host subgroup.
- Offline evidence: the title's shipped pixel programs were parsed and analysed one by one with the
  production admission (178 programs longer than the stop pc). The stop is the forward terrain
  pixel shader. At `0xc78` it spills one VGPR with `DS_WRITE_ADDTID_B32` (M0 = 0) and reloads it
  about 2,000 instructions later. Between the store and the reload it has EXEC changes, joins, a
  loop back edge, and an M0 save/restore around an interpolation. The previous proof accepted only
  a spill interval with no join, no EXEC write and no M0 write. It also lost the launch-parameter
  origin of M0 at every label, so the restore before `V_INTERP` could not be proved.
- Contract: the host gives each invocation a private array and omits TID. That is exact when
  every ADD_TID access has an exact aligned M0 base and the planes are equal or at least 256 bytes apart
  inside the declared extra LDS. With no other LDS instruction, a lane then only reads dwords it
  alone stores, whatever EXEC, branches or loops come between. A lane that did not store reads
  undefined bytes both from guest LDS and from its private array. The proof is now a forward
  dataflow over basic blocks: scalar origins keep what every path agrees on, and the planes stored
  on every path are intersected at joins. Each reload needs a store to its plane on every path, and
  each interpolation (anywhere in the program) must see the launch-parameter M0. Widening EXEC
  between a store and its reload is therefore no longer refused. A branch over the stores, paths
  that disagree on M0, aliasing planes and mixed DS accesses still are. The dead-store rule (every
  store read later) is gone: an unread private slot is unobservable.
- The new rule accepts a superset of the old one (same checks, path-precise). Offline scan of the
  178 programs: 132 admitted before, 146 after, and no program lost.
- Run (Intel, strict, world-entry, 300 s, with the compare fix below): the world loaded and
  gameplay held with textured terrain; no shader refusal. That run reopened the earlier world, so
  the refused shader's draw was not reproduced live; the offline proof on the shipped program is
  the evidence for it. The run stopped at 285 s in the recipe-book search (see the IME entry).

## Compares that overwrite their own scalar source (2026-10-16)

- The same terrain shader then refused at `pc=0x2b30`: "scalar written under divergent control flow
  is read after the join". A VCC branch on `V_CMP_NEQ_F32 s[8:9], 0, s9`, a compare of a scalar
  number, was treated as lane divergent. The uniform-compare check read the taint of `s9` after
  the instruction had already written its destination pair, so it saw a mask instead of a number.
- Fix: the compare's uniformity is judged on its sources before its destination is written, like
  the existing snapshot of uniform words for scalar mask algebra.
- Offline scan after both fixes: 168 of 178 programs admitted, including every earlier one. Found,
  not fixed: four banner forward shaders still refuse with "scalar written under divergent control
  flow" (`pc` 0x330 to 0x3b0), and six volume-population pixel shaders write device memory under
  helper lanes. Neither has appeared in a run yet, and neither is analysed. One lead for the first:
  a divergent region counts every instruction between the branch and its target as written,
  including blocks that leave by `S_BRANCH` past the join, so path-precise region writes may admit
  more.

## IME panel text without an open panel (2026-10-16)

- Observed (Intel, strict, sandbox title): opening the inventory's recipe-book search during
  gameplay stopped the run with `can't resolve lazy PLT import: ieCNrVrzKd4[Ime_v1]`.
- The NID is `sceImeSetText` (hash of the name). It replaces the text of the IME panel that
  `sceImeOpen` starts and takes `(const char16_t* text, uint32_t length)`. Independent
  implementations agree that it returns `0x80BC0002` (not opened) when no panel is open. Kyty
  implements no `sceImeOpen` panel, and the title had made no unresolved `sceImeOpen` call before,
  so no panel is open. `sceImeSetText` now returns that error.

## Four-byte render-target micro tile (2026-10-16)

- Observed (Intel, strict, world-entry route): in sandbox-title gameplay, plants and leaves
  showed red and green blocky patches, and grass block sides had stray blue dots.
- The block atlas is a 2048x2048 four-level render target (tile mode 27, four bytes per texel). The
  guest draws only level 0 on the GPU and samples the whole chain; levels 1 to 3 come from guest
  CPU-written tiled bytes that the host detiles when it creates the target. A gated dump of every
  level, compared with a box downsample of the host-rendered level 0, showed that each 8x8 micro
  tile of levels 1 to 3 was permuted. Swapping x1 with y0 and x2 with y2 in the detile matched the
  guest content. Mean absolute error per channel, before and after: level 1 8.12 to 4.93, level 2
  10.21 to 2.98, level 3 25.12 to 0.45. Two dumps from separate runs agree. The level 1 residual
  sits in regions where the guest filter or animated tiles differ; rows without them match exactly,
  so the bits above the micro tile were already right.
- Contract: inside a 64 KiB block, the four-byte `SW_64KB_R_X` layout orders the micro tile
  `x0 x1 y0 y1 y2 x2` (offset bits 2 to 7). Bits 8 to 15 are unchanged. The previous equation used
  the depth (`Z_X`) order `x0 y0 x1 y1 x2 y2`. Round trips through the host's own detile hid the
  error, because the tests built their input with the same helper. The GPU detile shader and its
  inverse for raw aliases take the same order; the checked-in SPIR-V is regenerated.
- Consequence for depth and colour aliases: the four-byte render-target and depth layouts now
  differ in the micro tile. The raw texel copy of 2026-10-13 copies host texels at equal (x, y), so
  it is exact only between views in the same layout. A D32 depth target is therefore seeded only
  from a mode 24 storage view (and the reverse); render targets and mode 27 storage views no longer
  seed it. Defect found, not fixed: a depth target that reads a mode 27 target's bytes now starts
  without them. Suggested direction: copy through the buffer stage with a compute pass that applies
  the micro-tile permutation (`x' = x0 | y0<<1 | y2<<2`, `y' = x1 | y1<<1 | x2<<2` inside each 8x8
  tile) instead of refusing.
- Verified (Intel, strict, world-entry, 300 s): the run opened a newly created world and
  spawned by frame 3,457. Captures show grass sides, sea grass and sugar cane with correct textures
  and no red or green blocks. At 285 s it stopped on a pixel shader that writes memory under a
  64-lane wave (`fragment wave transport ... helper lanes would be observable`); that shader had
  not appeared in earlier runs on the previous world and is recorded separately. Unit tests: 1,449
  graphics tests, the graphics diagnostics integration test and the checked-in detile SPIR-V check.

## Unread vertex snapshot tails (2026-10-16)

- In the fishing title's dock gameplay, transient capture cost 5.26 us per draw, but the copy itself
  only 1.63 us. A temporary counter showed `Commit` taking about 1.2 us per capture. The tail clear
  zeroed about 47 KB per capture against 8 KB copied. Over 300,000 captures, the vertex tails added
  up to about 13.5 GB (about 97,000 vertex captures, about 139 KB each); index tails took about
  35 MB and storage tails about 80 MB. `Acquire` hands a small capture the smallest free entry that
  fits, so small vertex captures land in large entries whose mark sits high. The pool peaked at
  63.0 MB in 402 entries, at its 64 MiB cap.
- The clear exists for reads past the logical size (see "Transient tail clearing" below). Vertex
  input reads record `r` at `r * stride + offset`, for the attribute's format size, and the draw
  fetches only records `[first, records)`. When the draw's record range is known and fits the
  declared records, and every attribute ends within its stride, nothing reads past
  `records * stride`. Only about 0.8 % of vertex captures did not meet that condition, and their
  tails were already zero.
- Change: the vertex-input format table records the bytes one fetch reads, and
  `VulkanVertexInputBindingFetchEnd` gives the end of a binding's attributes within a record.
  `BindVertexBuffers` asks for a zero tail only when the record range is unknown or clamped, or an
  attribute reaches past its stride. Without that request, `Commit` leaves the tail and its mark as
  they are. A snapshot reused by a consumer that needs the zero tail clears it first. Uploads, zero
  carriers, index and storage captures keep clearing.
- An indexed draw's record range comes from scanning the guest indexes on the host. The GPU reads
  the same values only when the indexes are bound from a snapshot; an `IndexBuffer` object can
  hold GPU writes the scan did not see. The indexed draw therefore binds its indexes before its
  vertex buffers, and skips vertex tails only when the indexes were snapshotted. The
  depth/stencil copy path always keeps the clear. Linear draws use the packet's vertex count, and
  indirect draws read their arguments after the pending write-backs, so their ranges are exact.
- Measured (Intel, strict, 240 s, dock route), against the same tree without the change:
  - transient capture: 5.26 to 2.70 us/draw;
  - command processing: 38.7 to 39.0 to 36.0 to 36.3 us/draw in steady windows;
  - draws per window: about 610,000 to about 650,000.

  The dock captures show the same scene. Live StorageBuffers ranged from 711 to 3,058, against 247
  to 406 in the comparison run. Two runs of the unchanged tree with timing counters also ranged
  from 357 to 2,316, so this count varies between runs and is not attributed to the change.
- Sandbox title (Intel, strict, 300 s, same route), against the index-snapshot run above:
  - transient capture: 1.54 to 1.43 and 1.45 us/draw;
  - command processing: 35.8 and 36.7 to 33.4 and 33.6 us/draw;
  - frame rate: 6.18 to 6.26 fps.

  Captures 30 s and 45 s after the spawn show textured terrain, trees and the HUD. The dark patch
  seen in late frames predates the change (see "Sandbox title: drifting dark patch on terrain").
- Racing title (Intel, strict, 200 s, `formula-drive`), with the indexed-draw rule above: command
  processing stayed at 43.3 to 45.2 us/draw in the race (42.7 to 44.8 before), and the street
  captures render lanes and tyre marks as before. The run stopped at 197 s on the open,
  intermittent depth-reference refusal recorded on 2026-10-11, outside the vertex path.

## Index snapshots over idle writable storage (2026-10-16)

- In the sandbox title's gameplay, index binding cost 5.2 us of about 29 us of command processing per
  draw outside fence waits. A temporary per-reason counter showed that 199,9xx of every 200,000
  index binds refused the transient snapshot and created an `IndexBuffer` object instead, at 3.6 to
  6.6 us each. Indexes averaged about 3 KB, over about 2,200 distinct addresses. Every refusal came
  from a writable StorageBuffer overlap. For the same ranges, `GpuMemoryAcquireGdsSource` always
  reported `GuestBytesCurrent`: no unpublished GPU write covered them.
- Change: the snapshot API takes an admission (`GpuMemorySnapshotAdmission`). The index-buffer
  capture asks for `IdleWritableStorage`. It also accepts writable StorageBuffer overlaps when no
  overlapping StorageBuffer is still `in_use` with GPU writes. That state is read at validation time
  from the object blocks, not from the cached overlap snapshot, and only after the read-only
  classification alone has refused the range. Every other snapshot consumer keeps the strict
  read-only admission.
- Broad variant, excluded: the same idle rule applied to every snapshot consumer (vertex, read-only
  storage views, storage-seed and gate captures). It reproduced the 2026-10-15 regression below.
  - Gameplay ran at 1.67 fps.
  - There were 114,036 live StorageBuffers (91,247 newly linked, 76,944 with truncated link
    traversal).
  - Transient validation took 14 s per 26 s window.

  Evaluating the write-back state at validation time does not prevent that regression, so its
  trigger lies in the non-index consumers.
- Measured (Intel, strict, 240 s, same route, gameplay window), against 4175df97:
  - index binding: 5.23 to 1.17 us/draw; no `IndexBuffer` objects are created;
  - command processing: 42.0 to 35.8 and 36.7 us/draw, of which fence waits 13.1 to 8.7 and 9.8;
  - frame rate: 5.57 to 6.66 and 6.54 fps; draws per window: 566k to 650k and 637k;
  - storage objects: 45 live StorageBuffers.

  A native capture 45 s after the spawn shows textured terrain, trees and the HUD; the one at 30 s
  caught the sky-only frames while the first chunks appear.
- Fishing title (Intel, strict, 240 s, dock route), which also covers 4175df97. Command processing
  ran at 38.7 to 40.2 us/draw in steady windows, against 39.7 to 41.1 on 88a1c297. Index binding stayed
  at 1.1 to 1.3 us/draw: its indexes were already snapshotted. The dock captures render as before.
  The first attempt hit the known intermittent boot stall at frame 3, which also reproduced on
  aa09403c that morning; the repeat ran clean.

## Materialization-cache clean ranges per draw (2026-10-16)

- `GpuMemory::ResetHash` took 5.0 % self time of the sandbox title's command-processor thread in
  gameplay. Each draw invalidates its colour targets and its depth ranges (depth, stencil and HTILE)
  in the materialization cache. A range invalidated before replays only the stores made since then;
  any other range scans all 2,048 slots. The cache remembered 4 clean ranges in FIFO order, so a draw
  with more than 4 attachment ranges cycled through them and each invalidation scanned every slot.
- Fix: remember 16 clean ranges.
- Measured (Intel, strict, 240 s, temporary counters): over the whole gameplay span, about 98.1 % of
  invalidations replay recent stores and 1.9 % scan every slot (98.7 % replay over the last ~600k
  invalidations); 0.05 % overflow the 256-store log. `ResetHash` fell out of the profile's top
  entries (below 1.2 %). Gameplay command processing went from 38.1 to 42.0 us/draw, with fence waits
  from 8.3 to 13.1 us/draw: without the waits, about 29.8 to 28.9 us/draw. The scenes differ between
  runs (619k against 566k draws per window), so the gain is within noise at window level.

## Descriptor-set recycling and deferred-release drain in dense gameplay (2026-10-16)

- Sandbox title gameplay is bound by the command processor. About 4,300 draws per frame, the thread is
  about 62 % running and about 28 % in GPU fence waits (kernel wait-channel sampling). A user-space
  profile of that thread put 18 % in the driver under `DescriptorCache::Allocate` and 11 % under
  `DescriptorCache::Free`, plus 4 % self time in `GpuDeferredDeletionQueue::DrainReady`.
- Temporary counters over 200,000 set requests showed:
  - 54,000 recycled hits;
  - 146,000 `vkAllocateDescriptorSets` calls;
  - 78,000 `vkFreeDescriptorSets` calls, every one because the recycle list was full;
  - no free of an untracked or in-flight set.

  About 61,000 live sets sat near `LIVE_SETS_MAX`. Each `TrimSets` retires 8,192 sets at once, which
  overflowed the 8,192-set recycle limit. The overflow went back to the driver, and the next sets of
  the same layouts had to be allocated again from pools whose free lists had grown long.
- Changes:
  - The recycle limit is now four trim batches (`RECYCLED_SETS_MAX`, 32,768). Pools never give memory
    back to the system, so holding a set only reserves pool space.
  - A full recycle list frees its oldest set, not the incoming one. With the incoming set freed, sets
    of layouts no longer in use stayed in the list until it was full of them, and driver frees per
    200,000 requests rose over the session from 3,400 to 29,000. Released sets are kept in release
    order with per-layout positions, so a layout still takes its newest set and eviction is constant
    time.
  - The deferred-release queue pops from a `std::deque`. Popping each ready entry from the front of a
    vector was quadratic when a trim enqueued 8,192 releases.
- Verified (Intel, strict, warm cache, same route):
  - With the larger limit alone, recycled hits rose to 53--85 % of set requests, and driver frees fell
    from 78,000 to 3,400 per 200,000 requests at first, rising to 29,000 as stale layouts filled the
    list. With oldest-first eviction, a 300 s run measured 53 % and then 98.5 % recycled hits,
    2,400 and 4,100 frees per 200,000 requests, and 25,000--30,000 sets held, below the limit.
  - Neither descriptor function nor `DrainReady` appears in the profile's top entries any more.
  - In the comparable gameplay window, resource binding fell from 23.9 to 15.0 us/draw and command
    processing from 52 to 40 us/draw, with 593k draws against 462k.
  - Regression check: the racing title (200 s) ends on its usual time-over screen. Its race windows
    improved too: resource binding fell from 20.5--20.9 to 16.0--16.5 us/draw, and command processing
    from 47--49 to 43--45 us/draw.
  - After the eviction change, the sandbox title's steady gameplay windows measured resource binding at
    11.6--11.7 us/draw and command processing at 33.9--34.4 us/draw, with 690k--700k draws per window.
  - Verified on Intel only. ANV ignores the pool `maxSets`; RADV enforces it, so the held sets there
    need about 64 more 512-set pools.
- Excluded:
  - Deferring the release of in-flight sets: the counters showed none, so that change was dropped.
- Regression check after the eviction change (Intel, strict, built from the 88a1c297 tree, 240 s): the
  fishing title docks and renders the harbor correctly. Its steady windows measured command processing
  at 39.7--41.1 us/draw (47.9--49.6 in the previous run), resource binding at 17.4--18.2 (27.0--28.1),
  and 19.6--19.9 fps (12.6--16.8).
- Open: the sandbox frame is still bound by command processing (about 34 us/draw) and GPU fence waits
  (about 5 s per 26-second window). Removing every fence wait would leave about 27 us/draw, so per-draw
  CPU cost stays the larger lever. The next profile entries are read-only snapshot comparison,
  `ResetHash`, timer reads, allocation, and per-draw shader re-analysis.
- Fence-wait attribution in sandbox gameplay (temporary per-call-site counters):
  - The longest waits come from indexed and auto-index draws whose shaders read guest memory through
    device addresses. Before such a draw, `GuestDeviceAddressPendingWriteBack` finds a writable storage
    buffer in a registered range with an incomplete use, waits for that submission, and writes the
    buffer back to guest memory.
  - The waited submissions belong to the compute ring (queue 8). The buffers are 48 B to 30 KiB, each
    with one recorded use and a write count that grows by about one per dispatch. Waits of 1 ms or more
    took a median of 2.1 ms (90th percentile 8.7 ms), three to four per frame. These per-wait details
    were sampled during world loading; the aggregate counters kept the same rate of slow waits through
    gameplay, but the waited objects were not sampled there.
  - By code reading, the compute ring already submits at the end of every ring run, so these waits are
    probably for compute work that is submitted and still running. The dependency state at the wait
    was not measured.
  - Excluded:
    - Snapshot refresh of tracked flexible-memory chunks, and the retired-table budget: 0, 0 and 2
      events against about 55,000 write-back waits. Only one 32 KiB chunk is tracked.
    - Retries repeating draw preparation: 2 retries in 54,000 waits. Device-address preparation runs
      before descriptor binding.
    - Waiting on read-only uses: every waited buffer had pending writes and one recorded use.
  - Direction, not yet taken: skipping the wait would need write-backs that become visible in guest
    memory on the device timeline, ordered before every guest-visible compute signal (label events fire
    mid-submission). Copying the whole storage object into the guest window is not equivalent, because
    a dispatch may write only part of the object. Binding storage in physical ranges directly to the
    imported window is the sound form, and it is a structural change.

## Vertex shader identity limited to attribute dwords the program loads (2026-10-16)

- After the transient-entry fix below, sandbox title gameplay still compiled 210--300 SPIR-V modules
  per 26-second window (about 10 s of compile time) with a warm cache, and the 512-entry graphics
  pipeline cache evicted 500--600 pipelines per window. Translation misses kept arriving with zero
  translation-cache evictions, so new shader keys were being created, not old ones lost.
- A temporary log of every translation-miss key showed that the seven busiest vertex programs differed
  only in raw attribute-table dwords (`fetch_attrib_data`), each taking hundreds of distinct values.
  In every case, the varying dword was a hole in the program's input semantics: for example, semantics
  0, 1, 3 and 5 declared, with dwords 2 and 4 varying. The guest disassembly of these programs loads
  exactly the declared dwords from the attribute-table register (for example, `s_load_dwordx2` at 0,
  `s_load_dword` at 12 and 20). The hole dwords hold unrelated memory that changes per draw. Kyty
  copied every dword up to the highest semantic into the shader identity and into the SPIR-V constants,
  so the same program was translated and compiled once per hole value.
- Change: the cached vertex-stream scan of each program now records which attribute-table dwords its
  scalar loads read (all dwords when an offset is not constant). Dwords the program never loads are
  cleared before the identity is built. Loaded dwords and the decoded per-semantic resources are
  unchanged, so code generation for every dword the program reads is unchanged.
- Verified (Intel, strict, same warm cache, 300 s, same route), comparing the gameplay windows before
  and after:

  | Metric | Before | After |
  | --- | --- | --- |
  | SPIR-V compiles per window | 185--297 | 0--1 |
  | Pipeline evictions per window | 494--596 | 0 |
  | Command processing (us/draw) | 87--112 | 36--38 |
  | Draws per window | 225k--286k | 630k--655k |
  | Frame rate | 2.9--4.0 fps | 5.7--6.5 fps |

  The world also appears sooner. The route's last input is at 139 s. Gameplay was first captured at
  about 157 s after the change, against 208 s before; the earlier run still showed its loading screen
  at 182 s. Captures are about 25 s apart, so the gain is 25--50 s. Gameplay captures show the same textured
  terrain, trees and HUD. Regression check: the racing title (200 s) matches its previous run window
  for window (command processing 47--65 us/draw, at most 110 live objects, 0--2 compiles) and ends on
  its usual time-over screen.
- Note: the local warm SPIR-V cache had filled to its 64 MiB store limit, mostly with per-hole
  variants. It should be regenerated after this change.

## Transient snapshot entry ceiling in dense passes (2026-10-16)

- Sandbox title gameplay slowed from about 3 fps to about 1 fps within two minutes. Live GPU objects grew
  from 2.9k to 78k, almost all new read-only `StorageBuffer` objects. Per-draw transient validation rose
  from 9 to 51 us, and command processing from 112 to 170 us per draw.
- A temporary census of each persistent fallback recorded the refusal reason and pool state. The new
  objects were 272- and 320-byte ring constants. The command-buffer pool had refused them because the
  storage usage had reached its 4,096-entry ceiling, while it held only 8.8 MB of its 63 MiB snapshot
  budget. A pass of about 3,300 draws needs more views than that. Each refused view became a persistent
  object. Frame-age retirement (120 frames, 4,096 visits every 30 frames) cannot drain those at 1--3 fps,
  so every later overlap query walked a larger set.
- Excluded:
  - unstable copies (a guest writer racing the capture; 0 observed);
  - unallocated ranges (0);
  - overlap refusals (about 470 for these sizes, mostly views contained in a compute-written storage
    buffer, correctly refused).
- Change: the per-usage entry ceiling is now 16,384 (`kGpuMemoryTransientBufferMaxEntriesPerUsage`). The
  total-entry ceiling stays at three usages' worth. The byte budget is unchanged. Vulkan memory is
  already suballocated from pool blocks, so the ceiling only bounds `VkBuffer` objects. Entries persist
  across resets of the same command buffer.
- Verified (Intel, strict, warm shader cache, 300 s, same route), comparing the last two 26-second windows
  before and after:

  | Metric | Before | After |
  | --- | --- | --- |
  | Live objects | 33k, then 78k | 3.6k, then 4.0k |
  | New storage objects | 29.5k, then 44.5k | 10, then 5 |
  | Validation (us/draw) | 9.4, then 51 | 1.8, then 1.8 |
  | Resource binding (us/draw) | 32, then 91 | 18, then 20 |
  | Command processing (us/draw) | 112, then 170 | 89, then 87 |
  | Draws per window | 227k, then 153k | 280k, then 286k |

  Gameplay terrain renders as before. The remaining gameplay cost was shader work: 210--240 SPIR-V
  compiles per window (about 9.5 s) even with a warm cache. The entry above resolves it. Regression check: the racing title (200 s)
  matches its previous run window for window (command processing 48--65 us/draw, at most 125 live
  objects) and ends on its usual time-over screen.
- Open defect, not fixed: small read-only persistent buffers still ratchet upward. The pressure trigger
  `kGpuMemoryBufferPressureRetireBytes` (256 MiB, `GpuMemory.h`) never fires for tens of thousands of
  sub-KiB objects. `GpuMemory::FrameDone` (`GpuMemoryWriteback.cpp`) ages by presented frames, so a slow
  title retires almost nothing. Suggested direction: add an object-count trigger next to the byte
  trigger. Any future pool refusal would show up here as a climbing `live_objects`.

## Service plugins identified by their declared module name (2026-10-16)

- The preschool Unity title crashed about one second into module start with a guest read of address 4.
  Its package ships the same network-service plugin twice, byte-identical, once under a file name with
  a " 1" suffix. Package discovery sorts that copy first, so it is the one kept. Kyty then called its
  `module_start` with no arguments, because the native start descriptor was keyed by file name. The
  plugin printed its "old version" error and then dereferenced the null descriptor. The guest's own
  disassembly shows the check: argument size at least 0x10, `size == 0x10`, `version == 0x200`, and the
  callback stored from offset 8.
- The player passes that same descriptor at each of its four module load sites, so the descriptor
  belongs to the module, not to a path. Service plugins are now identified by the module name their ELF
  declares (`DT_OS_MODULE_INFO`). A scan of all 196 package modules in the local library
  (`Media/Plugins`, `Media/Modules`, `sce_module`) compared the old file-name rule with the new
  declared-name rule. They disagree only on this title's suffixed copy, so every other title gets the
  same start arguments as before.
- Open defect, predating this change and affecting all six plugins: the player's own descriptor carries
  a real callback, but Kyty passes `callback = 0`. Kyty also starts package plugins before the player
  does. When the player later calls `sceKernelLoadStartModule`, it gets the existing handle from the
  `FindProgramByFile` early return in `KernelLoadStartModule` (`LibKernel.cpp`), so its descriptor never
  reaches `module_start`. Suggested direction: defer `module_start` of package plugins to the guest's
  load call, so the guest's own arguments are used. A null callback call on a network-service path
  would point here.
- Verified (Intel, 150 s, strict): the title passes module start and the menus, and reaches its first
  playable scene. Still open: in that scene, sprite edges show light-blue fringes and the characters look
  point-sampled at low resolution; the menu frames are clean.

## Image reads through an all-zero descriptor (2026-10-16)

- The card title stopped before its first frame on `shader emitter missing:
  ... sampled=0/0/0 inst=ImageSample ... s[0:7], s[8:11]`. A resource-fold
  capture of that pixel program shows its metadata declares a read-only
  image slot at user data 0 and a sampler at 8, and all twelve user SGPRs
  are zero at the draw. The type nibble of a zero T# is not an image, so
  nothing was bound and the emitter had no image to sample.
- Contract: the RDNA2 ISA states that a resource set to all zeros acts as an
  unbound texture or buffer and returns (0, 0, 0, 0).
- Change: the usage parse records a sample or load whose T# is still the
  shader's entry user SGPRs and all eight dwords are zero
  (`zero_image_resources`, part of the module key). The emitter stores zero
  in each DMASK component of such a read; the image destination guard
  applies EXEC as for any image result. Gather, resinfo, stores and atomics
  through a zero T# keep refusing.
- Verified on Intel: the card title passes the stop (120 s, generic route)
  and stops at the next one, `fragment wave transport is required by the
  pixel stage` (a wave64 pixel program writes a scalar under divergent
  control flow and reads it after the join; the host has no native 64-lane
  subgroup), open. Roguelike control unchanged (4 captures, time limit).
  Unit test `SamplesThroughUnboundDescriptorReturnZero` covers the parse,
  the emitted zero stores and a T# rewritten before the read.
- Named-DMASK sample formats leave `mimg_dmask` unset; the zero read uses
  the decoded destination size, which every MIMG format sets to the DMASK
  component count.

## Alpha-only stores to one-channel storage images (2026-10-16)

- The insect platformer stopped at boot on `swizzle is not supported:
  format=1 swizzle=0x800` (`StorageTexture.cpp`). The descriptor is an R8
  storage image with `DST_SEL = (0, 0, 0, R)`. The dispatch is a copy:
  `image_load v[0:3]` from a sampled texture, then `image_store v[0:3]`
  with `dmask 0xf` to the R8 image.
- Under the existing contract (shader component i goes to memory channel
  `DST_SEL[i]`), memory red receives component 3 and the constant selects
  store nothing. The store emitter now skips constant selects, and the
  descriptor gets an identity view when the stored components cover the
  view: all four distinct channels, or red alone on a one-channel format
  (`VulkanStorageHasSingleChannel`). A four-channel view with a constant
  select still refuses, because one memory channel would have no source.
- Verified on Intel (180 s, generic route): the title passes the copy and
  reaches its brightness calibration screen and runs to the limit. Its
  text draws as solid glyph blocks from the first capture (open visual
  defect). The run does not establish which image the copy fills, nor
  whether the blocks relate to the menu glyph investigation recorded
  below.

## Physical ranges withdrawn while device addresses are prepared (2026-10-16)

- A survey of the library on Intel (Arc A770, 120 s strict runs) stopped five
  titles on `guest memory device addressing is unavailable for a shader that
  dereferences guest pointers`: the dream platformer, the insect platformer,
  its sequel, the gothic action platformer and the first-person puzzle title.
  Each logged `guest import failed: ... has no direct memory` just before.
  Three of them reached their time limit on 2026-10-10/11; the failure is
  timing-dependent (one earlier insect-platformer run passed it).
- A temporary trace in `PhysicalMemory::ResolvePhysical` (not committed)
  showed the dream platformer's failing span covered exactly by one view with
  `unmap_pending=1`, not released and with no release pending. The guest was
  unmapping the range. Until the quiesced unmap transaction drops the range
  from the device-address registry, a preparation on the command-processor
  thread still saw its resident pages, the resolution refused the view and the
  whole preparation failed.
- Change: `KernelResolvePhysical` returns `KernelPhysicalResolution`.
  `Withdrawing` means one view holds the span and is being unmapped, or its
  physical range is released or pending release. The importer leaves such a
  span unimported, as the release path already states that a released range
  must not be imported again; its quiesced release removes the range. Any
  other refusal still stops the run.
- Verified on Intel, one 120 s run each with the generic or title routes: the
  dream platformer (in gameplay), the gothic platformer (intro cinematic)
  and the puzzle title run to the limit; the insect sequel reaches its
  "Act 1" card and then stops on a new
  `unknown relation: DepthStencilBuffer - Crosses - IndexBuffer` (a 12-byte
  index read inside a live 1920x1080 depth buffer), open; the insect
  platformer reaches its next refusal, the alpha-only R8 storage selector
  (`format=1 swizzle=0x800`), open. The roguelike control run is unchanged
  (4 captures, time limit).
- The kernel-process test for `sendmsg`/`recvmsg` still expected `ENOSYS`
  from before both calls were implemented; it now expects `EBADF` for an
  invalid descriptor.

## Read-only snapshots over written-back writable buffers (2026-10-15, excluded)

- Question: in the sandbox title's world, about half of the draw-time
  read-only buffer snapshots (`GpuMemory::CaptureSnapshotReadOnlyBuffer`) are
  refused. Is the refusal a broken contract, and does it cost frame time?
- Observation on HEAD (Intel, in world, 40 s window): 9.1 fps, 2,554 draws per
  present (36.6 us per draw), 3.2M probes, 46% accepted, validation 0.44 us
  per probe. A temporary, uncommitted per-reason counter showed:
  - every refusal comes from an overlap group that is not read-only;
  - a 1-in-16 sample of those found the writable object always quiescent
    (write-back capable, `in_use=0`);
  - zero truncated or overflowing groups, zero non-buffer groups, zero
    unstable copies.
  The overlap entry tests `read_only`, which `GpuMemoryMergeReadOnlyUse` leaves
  false after the write-back cleared `in_use`.
- Change tried: count a quiescent write-back-capable object as current guest
  memory, and invalidate the overlap-snapshot cache whenever an object enters
  or leaves the pending write-back index. A unit test covered the
  transitions.
- Result: a regression, so it was not committed.
  - In world: 1.4 fps instead of 9.1.
  - 20% of probes accepted, at 47 us of validation each.
  - 173,779 live StorageBuffers (165k newly linked, 139k with truncated link
    traversal). On HEAD there are 21.
  - The mechanism was not isolated. The cache invalidation on every in-use
    change turns each refusal into a full overlap query. Accepted snapshots
    also change when the writable buffer's write-back runs. Retrying the idea
    needs a design that keeps the cached snapshot static and evaluates the
    write-back state at validation time, measured on the same route.
- Storage binding is not the frame-time lever there. In the HEAD window:
  - storage descriptors 2.2 s and transient validation 1.4 s;
  - command processor 36.5 s;
  - pipeline setup 9.2 s, of which 6.1 s is first-visit SPIR-V compilation;
  - resource binding 8.9 s;
  - fence waits 8.8 s;
  - state setup 4.9 s.
- Also excluded with temporary counters:
  - page-granular write-back overwriting CPU-written bytes: 96k pages checked,
    0 CPU-written bytes, 0 overwrites;
  - snapshot copy instability: 0 unstable copies in 4.58M.
- Earlier "unstable" counts were `Capture` returning false, mostly this
  overlap refusal, not a racing CPU writer.
- Object retirement (`GpuMemory::FrameDone`, 120-frame age, one pass every 30
  frames, 4,096 slot visits): on HEAD the in-world population stays flat.
  - 21 StorageBuffers, 979 index buffers, 774 vertex buffers.
  - 994 creates against 1,382 frees in 40 s.
  - No object-count trigger is needed for this title. The 85k-173k live
    StorageBuffers seen earlier came only from the build above and from
    diagnostic builds.

## Save data directory search patterns (2026-10-15)

- Symptom: the sandbox title never listed a saved world. Every boot showed the
  first-run flow, and the PLAY screen listed no worlds.
- Evidence: the guest searches its save directories with the patterns
  `<prefix>%@P%` and `<prefix>%@O%` (strings in the executable). The
  search compared the pattern with each directory name literally, so nothing
  matched. Two independent secondary implementations agree that `%` matches any
  run of characters (including none) and `_` exactly one.
- Change: `SaveDataDirectoryNameMatches` in `SaveDataPaths` implements those
  two wildcards (other characters literal, case-sensitive) and the directory
  search uses it.
- Test: `EmulatorSaveData.MatchesDirectorySearchPatterns`.
- Verified live: with a saved world the title skips the first-run flow, the
  PLAY screen lists the world and it loads in about 60 s.

## Guest thread-pointer loads without the TLS handler lock (2026-10-15)

- Symptom: the sandbox title took 60-110 s from its "ready to play" screen to
  the first world frame (world on screen at 183-209 s on the seeded route).
- Attribution: a guest-IP stack sampler over the world-creation threads put
  42% of their samples at one address, the TLS handler page
  (`handler_vaddr`, image base + aligned image size + the 0x1000 exception
  handler page). Every guest `mov rax, fs:[0]` is patched by
  `LoaderPatchTlsFsBaseLoads` into a call to that page, which held
  `Jit::SafeCall`: a process-wide spinlock, a 2,688-byte XSAVE into a shared
  buffer, a switch to a shared host stack, then `RuntimeLinker::TlsGetAddr`
  with the linker mutex, the static-TLS mutex and a hash-map lookup by thread.
  Worker threads of the title read their thread pointer that way many times
  per job, so they serialized on that lock.
- Change: the handler page now starts with `Jit::TlsCachedCall`. It reads the
  guest thread pointer from a host `thread_local` slot through its
  `fs`-relative offset, returns it when the slot is filled, and keeps flags and
  every register but `rax`. An empty slot (the first load on a thread) jumps to
  `SafeCall`, now at `handler_vaddr + 0x40`, whose `TlsMainGetAddr` fills the
  slot. The slot never goes stale: the main TLS block of a thread is fixed for
  its lifetime and static TLS is not resized while guest threads run. Linux
  x86-64 only; other hosts keep `SafeCall` at the handler address.
- Test: `EmulatorLoaderTls*.CachedCallReturnsThreadPointerOrTakesMissPath`
  runs the stub in an executable page (hit returns the cached pointer with
  `rcx` and the carry flag preserved; an empty slot takes the miss path).
- A/B on the same seeded sandbox route (Intel A770, strict, 300 s): loading
  screen at 132 s and world on screen at 145 s, against 183-209 s before;
  the handler page no longer appears in the sampler. In-world rate is
  unchanged (about 8.4 FPS); the world is now bound by Kyty's graphics thread
  (SPIR-V optimization, allocation, descriptor cache, memory hashing).
- Regression runs (same routes and environments as the 2026-10-15 baselines):
  - Racing title (200 s, render-target scope): the route started the race one
    checkpoint earlier and ended in TIME OVER; 22.8-28.8 FPS against
    21.0-27.8 FPS of an earlier run that took the same route branch. The
    in-race window of the usual route branch was not covered.
  - Fishing title (240 s, RADV): open sea 7.6-9.0 FPS against 7.6-8.1, dock
    9.1-9.5 FPS; gameplay reached.
  - Beat 'em up (300 s): gameplay reached, 12.2-18.4 FPS against 10.7-12.1
    (this route varies by about 5 FPS between runs of one binary). Reaching
    gameplay later (after the intro cinematics) in the beat 'em up and fishing
    runs also happens with four older binaries: route variance, not this change.
- Leads not taken in this change (measure before acting): the module-relative
  `TlsGetAddr(module, offset)` path still takes the linker mutex and scans the
  programs; `mmap_in_guest_window` took 9-25% and
  `GpuDirtyPageTracker::VisitWritePages` 12% of the loading threads' samples.

## Conditional command-buffer branch (2026-10-14)

- The role-playing title and the mechanical-fist platformer stopped on the
  unresolved `sceAgcCbBranch` import (`w1KFAHVqpaU`). The earlier held candidate
  was a jump that ignored the compare and the not-taken target.
- Contracts used:
  - Argument order: the published `CommandBuffer::branch` signature (branch mode,
    compare function, 64-bit compare address, mask and reference, then cache
    policy, target and size of each buffer). Waits use the same
    `CompareFunction` type.
  - Packet: the published `COND_INDIRECT_BUFFER` layout (opcode 0x3F, 14 dwords;
    mode 1 if-then, 2 if-then-else; functions 0-6 as `WAIT_REG_MEM`).
- Change: `sceAgcCbBranch` and `sceAgcCbBranchGetSize` (56 bytes) build that
  packet. The command processor tells it from `INDIRECT_BUFFER` by length,
  evaluates the 64-bit compare and runs the selected buffer as a nested call.
  Before reading the word, any write to it still pending in a recorded or queued
  submission is published, so the compare observes stream order.
- Live argument trace (temporary, removed), role-playing title, 55 calls: all use
  mode 1, function 0, mask and reference 0, a 64 KiB-aligned then-target of
  77-4129 dwords and a null else-target. This is the guest's command-buffer
  segment chaining.
- Result: that title now runs its whole window. The route reaches the initial
  settings, title, New Game and traveler selection at 6-9 fps.
- Open on that title:
  - The deferred menu defect is unchanged: the title menu background is solid
    magenta and the traveler-selection scene is black (see "Role-playing title
    menu defect").
  - The stick pans the selection scene and Circle returns to the title menu,
    but Cross with the quill on a traveler does not select it, so gameplay is
    not reached.
  - Its frame time is dominated by GPU waits: about 126 s of fence waits and
    108 s of `WAIT_REG_MEM` in a 231 s window.
- The mechanical-fist platformer still faults first on its startup weak-pointer
  race (27 s, guest mutex at 0x58), before any branch is built.

## Command-processor per-draw costs: armed ranges and descriptor reuse (2026-10-14)

- Method: the same scripted route does not reach the same scene in the same
  frame. Over the same 12 s window of the racing title the draws per frame were
  419, 363 and 779 in three runs, so frame rates of single windows are not
  comparable. The figures below are command-processor time per draw from two
  agent perf snapshots around a stack-sampler window (Intel, release build,
  strict run, render-target scope, the racing route at 150 s).
- Racing title, us per draw:

  | Build | Command processor | Object creation | Texture descriptors | Descriptor finalize |
  | --- | --- | --- | --- | --- |
  | Before | 55.9 | 5.73 | 7.28 | 5.83 |
  | Prefix compare, cached switches, armed proof | 45.3 | 1.89 | 3.12 | 6.86 |
  | Plus descriptor reuse (two runs) | 49.7 / 45.0 | 2.11 / 2.03 | 3.59 / 3.58 | 4.56 / 5.00 |

- Armed proof: dirty-page `Rearm` walked every page of a bound range on every
  bind (8.3% of the thread). A registered range whose pages were all armed now
  records the total change count of its blocks and returns early while it is
  unchanged. Writes and fallbacks already advance the block count; authority
  changes (unmap, protect) now advance it before and after the page update.
  Unit tests: a proven range is protected again after a write, and an unmapped
  range is recaptured after a second rearm; both fail with the proof compare
  removed.
- Prefix compare and cached switches: instruction-name prefix tests compared
  through library calls for every instruction of every draw, and opt-in dump
  switches read the environment per draw. Both disappear from the profile.
- Descriptor reuse: with the cheaper bind path the driver's descriptor-set
  allocate and free calls rose to 5.3% and 3.0% of the thread. A released set
  whose recorded submissions all completed is now kept for the next set of the
  same layout (at most 8192); the caller rewrites every binding. A set released
  with a resource while one of its submissions is pending returns to its pool
  as before. The driver free call leaves the profile; allocation drops to 1.2%.
- Fishing title at the dock, same route, the last windows: before 82.7 / 78.6 /
  95.3 us per draw at 1067-1203 draws per frame and 111.6 us at 845; after
  67.9 / 65.3 / 81.2 us at 1099-1207 and 101.8 / 103.2 us at 837-844 (about
  11.6 against 10.6 FPS in the last window). The new build reached the dock
  sooner. Captures render the boat, water and HUD correctly.
- Open: one racing run of an earlier descriptor-reuse build, which also reused
  sets released with a resource whatever their pending submissions, stopped
  presenting after 78 s (55.9 FPS until then) with every thread idle, no error
  and no kernel GPU reset. Three later runs of that build and one of the final
  build did not reproduce it. Cause unknown; the final build only reuses sets
  whose submissions completed.
- Excluded: memoizing per-draw input analysis by a key of the draw's shader
  and register inputs. A counting diagnostic on the fishing route found the
  cumulative share of draws matching a recent key falling from 77% over the
  first 50000 draws (loading screens) to 6.0% over 1.8 million, with almost no
  new matches over the last 500000 (gameplay), so the memo would only add a
  compare.
- Transient snapshot index: each snapshot capture walked every transient entry
  of the command buffer for an earlier snapshot of the same range and usage
  (4.6% of the racing thread). Entries holding a valid snapshot are now kept in
  a map by range and usage, updated where a snapshot is stored or overwritten;
  reuse still requires a byte match with guest memory. The capture drops to
  1.5%. Racing title 43.2 us per draw in one window (45.0 before). Fishing title
  at the dock, 1157-1163 draws per frame: 52.6-54.4 us per draw against
  65.3-67.9 with the previous commit and 78.6-82.7 before this section, about
  15 FPS against 11; the dock capture renders correctly.
- Regression runs on `6caf6261`, scripted routes: the fishing title on AMD
  holds the dock at 9.1-9.2 FPS (7.3-7.7 FPS recorded before), 69-72 us per draw
  at 1161-1163 draws per frame, and its dock capture matches the Intel one; the
  robot platformer on Intel reaches the studio logo and the loading phase at
  the same frame counts as `2696e271` (1120 against 1091 and 1118 presents in
  200 s). Its black rectangle over the nebula fade-in is pre-existing (the
  `2696e271` capture at frame 928 shows it). One 4.5 s gap between presents in
  its loading phase made the agent report `stalled` once; presents resumed.
- Excluded for the racing title: `ResetHash` self time is the inlined
  materialization-cache `InvalidateRange` scan (the only loop before
  `GetHeapId` in its disassembly). Scanning only live slots showed no gain in
  one window of a different scene (its share 8.0% against 5.9%, at 550 against
  780 draws per frame, so the shares are not comparable; cache occupancy was not
  measured). Remembering recently scanned clean ranges, already excluded
  for the fishing title above, left it at 6.9%: a store overlapping the
  attachment, most likely the attachment's own materialization, clears the memo
  every draw. A spatial index of the spans is the remaining direction.
- Remaining ranked costs on the racing title after these changes: `ResetHash`
  5.9-8.0%, `steady_clock::now` 3.6-4.1% (debug
  statistics timers, about a hundred per draw), instruction-prefix scans still
  3.1% (per-draw shader scans in `ShaderGetInputInfoPS` and the compute path,
  which reparses per dispatch), `mem_alloc` 2.9%, `ShaderParseUsage2` 2.7%,
  `ShaderCollectDynamicScalarResources` 2.3%.

## Static pointer-table resource plans: no measured gain (2026-10-14, excluded)

- Change under test: `travel/static-resource-plans` (memoized pointer-table
  resource plans, `6f5c0e90`) merged with this branch at `4c54c61d`; baseline is
  the same tree without the plans. Release builds, strict runs, the same
  scripted route, the same warmed SPIR-V cache, fixed 1280x720 RenderTargets.
  The plans build passes 2425 unit tests (2419 plus its six plan tests).
- Fishing title, the same scripted dock route, Intel Arc A770, ABBA order over three
  processes per variant (200 s each, the last four capture intervals): the
  baseline averaged 10.20 / 10.57 / 10.50 FPS (10.42) and the plans 8.70 /
  10.47 / 10.00 FPS (9.72). Without the lowest plans run, 10.5 versus 10.2.
  AMD, one pair at the same route: 6.2 versus 7.0 FPS, inside the run-to-run
  spread. The racing title pair desynchronized its route (different scenes) and
  is not comparable.
- Conclusion: no measurable frame-rate benefit on either host; this agrees with
  the earlier pilot, whose own state-setup timers did not decrease. The branch
  is kept unmerged. Revisit only with a counter that shows plan invocations on
  a CP-bound workload.

## Font library: guest outline rasterization (2026-10-14)

- Observed: on the robot platformer's title screen the "NEW GAME" pill drew
  its text as solid squares.
- Cause: glyph rendering filled every glyph box with full coverage and
  reported synthetic metrics.
- Change (8630816f): fonts opened from guest memory decode their TrueType
  or CFF outlines with the public-domain `stb_truetype` header. Metrics,
  horizontal layout and coverage come from the face; missing glyphs,
  unsupported surfaces and unreadable fonts return the library error codes.
- Verified: Intel robot platformer title screen shows "NEW GAME 1" and
  "NEW GAME 2" as text. Unit tests: 2,419 pass.
- Open: system font sets are opened by type and carry no guest bytes, so
  their handles have no face. Their glyph queries report NO_SUPPORT_GLYPH and
  `FontGetHorizontalLayout` returns placeholder values (`LibFont.cpp:491`).
  `FontGetVerticalLayout` (`LibFont.cpp:519`) is synthetic for every font.
  Real values need the system font set's face, which would come from a
  user-provided font directory (never committed).

## Runtime-built typed buffer descriptors (2026-10-12)

- Cause of the AMD fishing-title device loss at about 182 s: one pixel
  shader builds a V# from `s_getpc` (base = program address + a constant
  offset, 64 records, a format word written by the shader) and reads it with
  `TBUFFER_LOAD_FORMAT_X ... idxen` (32_FLOAT). The Gen5 MTBUF lowering used
  the V#'s first dword, the low half of a code address, as the index into the
  storage-buffer descriptor array. RADV computes that descriptor address
  modulo 2^32, so the fetch lands on an arbitrary page in the descriptor
  window. When the page is unmapped, the kernel reports an `SQC (data)`
  fault. Guest addresses differ between runs, which explains why the loss is
  intermittent. The fault page and the set bases logged for the faulting
  pipeline give an index that matches a 256-aligned code base.
- Contract: a vector buffer load whose V# no discovered resource describes
  holds a guest base. That means no bound storage-buffer register and no
  mapped descriptor `S_LOAD` whose destination reaches this consumer. Such a
  load is read through guest device addressing
  (`ShaderVectorBufferUsesRuntimeDescriptor`). The MUBUF paths already did
  this; the five Gen5 MTBUF recompilers now do too, outside the vertex stage.
  The supported typed formats are unconverted 32-bit floats, so the dwords
  are read as stored. The instruction's format sizes the element for the
  range check ("the MTBUF derives the data format from the instruction").
  Pixel and compute shaders with such a load request the device-address
  table.
- Limit: vertex-stage MTBUF loads keep the stream resolver, which owns the
  fetched stream V#s. A vertex shader that builds a typed V# from `s_getpc`
  is still clamped to zero by that resolver.
- Excluded on the way:
  - The vertex shader of the faulting pipeline indexes the descriptor array
    only through metadata blocks 0 and 1. Its layout was stable for a whole
    200 s run.
  - The pixel shader's metadata blocks were always slots.
  - Transient V# uniform uploads: each entry is its own buffer, keyed by
    unique id plus range.
  - Buffer-object use after free, the guest device-address table and its
    imports, and image and sampler indices were all excluded. The fault also
    reproduced with serialized submits and RADV shader sync.
- Verified:
  - AMD Raphael (RADV), strict 200 s fishing-title runs:
    - A run with the final binary reached gameplay (mayor dialogue in the
      town) with no device loss, at 7.2-7.4 fps in the hold windows.
    - Two earlier runs with the same fix plus layout logging also kept the
      device; their fps (3.4-7.1) is not representative.
    - Before the fix, most baseline runs lost the device at 180-183 s. No
      baseline fps exists for the hold windows, so the AMD fps is a first
      measurement and not a comparison.
  - Intel Arc A770: a strict 200 s run reaches gameplay with no error, at
    10.3-10.9 fps in the hold windows. A baseline run on the same route
    measured 9.3-12.5 fps, so the difference is within the window spread.
    Draws that use this pixel shader now pay a guest write-back before the
    table is read. On Intel, this load used to read through an arbitrary
    descriptor, so capture differences in those draws are corrections.
  - Intel strict sweep (`Fixed` + RenderTargets): the watercolour
    platformer, the gothic action platformer, the ninja platformer, the
    isometric roguelike, the pixel exploration title, the artillery title,
    the roguelite action platformer and the racing title ended with status 1
    and the same capture counts as the previous binary, with coherent last
    captures. The racing title's window 4 ran at 96 fps against 84 fps
    before, which is within the spread between windows. The robot
    platformer still produces no capture on Intel, as before.
  - AMD strict sweep (`Fixed` + RenderTargets): the same eight titles ended
    with status 1 and the same capture counts. The racing title ran at
    24.5 fps against about 24 before. The robot platformer still stops at
    about 61 s on `!create_all_the_same`, as before.
  - Unit suites: 997 shader and graphics tests, including two new getpc
    typed-load cases. Gate scripts: boundaries, graphics tables.

## Command-processor costs after render pass continuation (2026-10-12)

Intel Arc A770, `Fixed` 1280x720 + RenderTargets, the Unity fishing title in
gameplay (about 1,160 draws per frame, 11.6 fps). A line-level CPU profile of
the command-processor thread over 15 s puts 6.7% on the span scan of
`GpuMemoryMaterializationCache::InvalidateRange` and 4.7% on the probe loop of
`GpuDirtyPageTracker::FindPage`. No other source line takes more than 2%.

- Excluded: long probe chains in the dirty-page table. Diagnostic counters
  on the same route measured 79-90 k live pages and 75-79 k tombstones in the
  262,144-slot table, with an average of 1.3 probe steps per lookup and 2.6-3.3
  per miss. The cost is volume instead. Each `Rearm` performs about 400 page
  lookups, and `GpuMemory::Update` re-arms every bound object once per guest
  command batch, which gets a new submit id. The title submits about 78
  batches per frame.
- Inconclusive, not merged: a memo of recently invalidated clean ranges in
  `InvalidateRange`. The fps windows were 11.9 / 11.5 / 10.9 against
  11.2-11.6 before, and the phase counters were unchanged. The spread between
  windows of one binary (about 9%) exceeds the largest gain the change could
  give (about 6%). A repeat must compare the line share, not fps.
- Excluded for the beat 'em up: emulator-inserted dispatches. Its roughly 300
  dispatches per frame all come from guest `DISPATCH_DIRECT` and
  `DISPATCH_INDIRECT` packets.
- The remaining cost is spread across per-draw binding with no single item
  above about 8%. Reaching 60 fps on this title needs a cheaper contract for
  revalidating resources per guest batch, not another local optimization.

## Render pass continuation across draws (2026-10-12)

- Contract: a draw continues the render pass instance that the previous draw
  left open when its framebuffer and render pass are the same, and when
  beginning a new instance would neither clear, discard nor transition an
  attachment. Those conditions are: every attachment uses `LOAD`, its tracked
  layout equals the framebuffer's initial layout, there is no depth or stencil
  clear, and there are no custom sample locations. Skipping the begin
  therefore skips no barrier or load operation.
- How the pass is closed:
  - `CommandBuffer` owns the open instance. Every other recording path
    obtains its handle through `RecordOutsideRenderPass`, which ends the
    instance first and emits the attachment-write to shader/attachment-read
    dependency that used to follow every draw.
  - Sampled-image and storage-image barriers take the `CommandBuffer`, so a
    draw that samples a live attachment closes the instance before its
    layout transition.
  - Bindings, dynamic state and query scopes use `RecordState`, and draws use
    `RecordInsideRenderPass`.
  - `End()` closes the open instance.
- Cause found: the Unity fishing title records about 1,160 draws per frame, and
  each one began and ended its own render pass followed by a full memory
  barrier. Of 1.5 M passes in a census, 86% followed a pass on the same
  framebuffer with `LOAD` everywhere. GPU time was a fixed cost per draw:
  `rcs` busy measured 49% at both 1280x720 and 640x360.
- Verified on Intel Arc A770 (Xe KMD), `Fixed` 1280x720 + RenderTargets:
  - Unity fishing title, perf windows 5-7:
    - fps: 11.0 / 10.3 / 8.9 before, 11.4 / 11.2 / 11.6 after;
    - fence wait per draw: 17.7 / 18.1 / 20.5 µs before, 15.4 / 15.2 /
      15.1 µs after;
    - `rcs` busy over a 15 s sample in window 5: 29.2% after. The 49.3%
      before was sampled on the preceding build of the same branch, not on
      the immediate parent.
  - Window 5 counted 30,292 render pass begins and 316,174 continuations. The
    title is now bound by the command processor's CPU time.
  - Vulkan validation layer, with four errors filtered that also reproduce at
    the same point on the previous binary or come from pipeline creation:
    - `VUID-vkCmdDraw*-imageLayout-00344`: a draw samples its own color
      attachment;
    - `VUID-vkCmdDraw*-None-09600`: a depth layout mismatch at submit;
    - `VUID-RuntimeSpirv-OpEntryPoint-08743`.
    With those filtered, a 150 s run of the fishing title and a 115 s run of
    the racing title report no error. Because the emulator exits at the first
    validation error, a second pair of 150 s fishing-title runs logged every
    error to a file without exiting and without a filter:
    - before: 503 k draws, 6,374 `imageLayout-00344` (12.7 per 1,000 draws),
      5,900 `None-09600` (11.7), 20 `OpEntryPoint-08743`;
    - after: 579 k draws, 5,344 (9.2), 6,424 (11.1), 26;
    - no other VUID in either run. The continuation adds no layout error.
  - Strict sweep:
    - Status 1 with captures matching the previous binary: the watercolour
      platformer, the gothic action platformer, the ninja platformer, the
      pixel exploration title, the artillery title, the roguelite action
      platformer and the racing title.
    - The isometric roguelike stopped once with status 139: a guest-thread
      write to address 0x40 inside a host copy. This matches the signature of
      the intermittent null upload-buffer copy recorded below, but was not
      symbolized. Its rerun reached status 1 with matching captures.
    - The robot platformer is unchanged (no capture, as before).
- AMD Raphael (RADV):
  - The racing title's window 4 went from 20.0 fps (611 draws/f) to
    24.0 fps (572 draws/f).
  - The fishing title stops at 182-183 s with `VK_ERROR_DEVICE_LOST` on both
    the previous and the new binary.
- Graphics diagnostics (16 integration tests) pass, as do the focused unit
  suites (615 tests).
- Open defects:
  - AMD fishing-title device loss, reproducible at about 182 s. Fixed by
    "Runtime-built typed buffer descriptors" above.
    - The kernel reports a gfxhub page fault from `fc_script` on ring 24,
      client `SQC (data)` (a scalar data read) with permission faults,
      followed by a timeout on the `gfx_0.0.0` ring. RADV marks the graphics
      context "innocent", so the faulting submission is the emulator's own
      work on the other ring.
    - Direction: map the faulting address against the guest mappings and
      identify the last submission on that ring.
  - The beat 'em up title runs at 11.1-14.2 fps on Intel and 9.4-11.6 fps on
    AMD, with about 300 compute dispatches per frame, one per draw. It is not
    render-pass bound.
  - The fishing title's CP thread spends 7% in `GpuMemory::ResetHash` and 7% in
    `GpuDirtyPageTracker::Rearm` (through `BeginRead`), per draw.

## Depth-only compute HTILE clears (2026-10-12)

- Contract: the proven zero-word `ComputeMetadataFill` HTILE event (see the
  graphics handoff, "Compute HTILE fill restores coherent geometry") now
  becomes a Vulkan depth `CLEAR` for a depth-only target too. Whether the
  target has a stencil plane comes from the guest registers
  (`ResolveEffectiveStencilFormat`), not from the host format. A combined
  target still qualifies only as D32S8 with a guest stencil-zero clear in the
  same first use; other combined formats stay refused.
- Cause found: the racing title clears its 2048x1024 D16 shadow atlas each
  frame with this event. It was discarded, so the atlas kept earlier depth and
  reverse-Z `GEQUAL` rejected the new casters; the cars' shadows smeared over
  the track. The handoff records the viewport, in-shader bias and hardware
  depth-bias exclusions.
- Verified on Intel Arc A770, `Fixed` 1280x720 + RenderTargets, one strict
  run per title: the watercolour platformer, the gothic action platformer, the
  ninja platformer, the isometric roguelike, the pixel exploration title, the
  artillery title, the roguelite action platformer, the robot platformer and
  the Unity fishing title keep status 1; the racing title keeps status 1 on
  Intel and on AMD Raphael (RADV). Race captures show correct car shadows. The
  graphics diagnostics integration (17) and the graphics-state unit tests pass.
  Each title's last captures match its previous RenderTargets sweep, apart
  from scene timing and the artillery title's random map.
- Performance: unchanged. Two same-session strict runs at the harness
  default resolution, before and after the change, sampled the race every
  4 s while a user GPU workload was running: 25.6-52.5 fps before and
  25.9-55.6 fps after. A perf window at a fixed time offset measured 29.0 fps
  on Intel and 20.0 fps on AMD against 83-89 and 40.4 fps earlier, because a
  slower menu load moved that window to the race start, where the whole field
  is on screen (`draws/f` 575 against 132). That window does not compare runs
  whose load times differ.
- Repeat on the build with runtime typed descriptors: one 125 s strict
  racing-title run per host (`Fixed` + RenderTargets, a capture every 4 s,
  29 captures each). Every race capture on Intel (10) and on AMD (8) shows the
  car's shadow attached under the car. The wing and wheel shapes are stable
  between speeds and frames, and no smear trails behind the car. The edges
  show only ordinary shadow-map stepping. In the sweep on that build, the
  fixed perf window measured 96 fps on Intel and 24.5 fps on AMD, back at
  or above the levels before this change.
- Open defect found while excluding the viewport: `ResolveViewportDepth`
  (`GraphicsState.cpp`, the overload taking ZMIN/ZMAX) clamps the endpoints of
  the Vulkan depth range. That remaps depth linearly instead of clamping it:
  with range [0, 1] and ZMIN 0.5, depth 0.75 becomes 0.875 instead of 0.75.
  It is exact only when ZMIN/ZMAX contain the range, which holds for every
  target traced so far. Direction: keep the viewport range unclamped and
  apply ZMIN/ZMAX as a depth clamp range (depth clamp with a custom range
  where the device supports it; otherwise refuse narrower ranges).

## Render-target demotion under the RenderTargets scope (2026-10-12)

- Contract: scaling a color target is speculative. The first use that needs
  guest texels demotes it: its range is recorded, so it never scales again,
  and the scaled image is resampled into a guest-extent image that replaces
  it. `docs/render-resolution.md` lists the demoting uses and the remaining
  refusals. Depth is not demoted.
- Pieces:
  - a bounded demotion set (`RenderResolutionDemotion.cpp`, 256 ranges, then
    saturated: every range counts as demoted);
  - a scaled color candidate must also support blit source and destination
    for its format;
  - `GpuMemoryCreate.cpp` treats a render texture requested at the same range
    with only another host extent as create-from-objects plus retire, and the
    render-texture create path blits every layer (linear when the format
    filters linearly);
  - the render-target cohort demotes its committed scaled color members and
    keeps new members native instead of returning an unsupported plan;
  - the texture binding path demotes a scaled render texture selected for a
    storage view or for a stage whose sampling is refused, unless it is a
    color attachment of the same pass.
- Cross-queue ordering: the resample is recorded into the command buffer of
  the consumer, before its use, so it inherits the ordering the guest already
  gives that consumer's direct read.
- Measured with a temporary counter (removed) on Intel Arc A770 and AMD
  Raphael (RADV), `Fixed` 1280x720 + RenderTargets:
  - the watercolour platformer demotes four 0x870000-byte ranges, once each,
    at the binding path, and runs to the end of its 90 s route; captures match
    the `Display` scope run of the same scene. Intel 174 and 138 fps, AMD 53,
    64 and 54 fps.
  - the racing title demotes two ranges, once each, at the cohort and drives:
    Intel 83.3 and 89.1 fps in the drive window (two runs), AMD 32.6 fps.
    Captures show the same defective car shadow quads as `Native`.
  - the beat 'em up demotes nothing. AMD: 12.0, 11.6, 10.2 and 12.5, 10.8,
    10.2 fps against 13.0, 11.7 and 10.0 for a build of `5e6a9216` in the
    same session; p50 frame times match (77, 80-82 and 94-96 ms). Every color
    format RADV exposes as an attachment supports blit in both directions, so
    the new capability requirement does not change which targets scale there.
- `Fixed` + RenderTargets on Intel keeps status 1 for the watercolour
  platformer, the gothic action platformer, the ninja platformer, the
  isometric roguelike, the pixel exploration title, the artillery title, the
  roguelite action platformer, the racing title and the robot platformer.
  On AMD the same set keeps status 1 except the robot platformer; the racing
  title drives at 40.4 fps there.
- Open, found while verifying: the robot platformer on AMD stops at about
  78 s with `!create_all_the_same` (`GpuMemoryCreate.cpp:2186`): a 960-wide
  StorageTexture at 0x53bcf0000 crosses and contains eleven storage-buffer,
  storage-texture and render-texture parents. It stops the same way with a
  build of `5e6a9216` under `Fixed` + RenderTargets and with this change at
  `Native`, so it predates the change and does not depend on the scope.
  Direction: classify which parents the dispatch overwrites and which it
  reads, from a bounded dump of the parents' guest ranges and formats.
- Not done: depth demotion, and making RenderTargets the default (that needs
  full sweeps on both hosts and repeated runs of the roguelite action
  platformer on AMD).

## Offscreen render-target scaling, opt-in (2026-10-12)

- `RenderResolutionScope = RenderTargets` (`KYTY_RENDER_RESOLUTION_SCOPE`)
  extends `Fixed` from the display cohort to offscreen render textures and
  their depth when they match the registered display extent. `Display` stays
  the default. `docs/render-resolution.md` has the cohort, alias and read
  rules.
- Cause it targets. On AMD the beat 'em up draws about 320 sprites per frame
  into two 3840x2160 render textures and about 31 into a third with depth.
  Every consumer samples them with normalized coordinates. Limiting those
  draws to one quadrant in a removed diagnostic raised it from 4.1 to 8-11
  fps and cut GPU busy from 82% to 48%, so fill dominates.
- Alias evidence, from code reading:
  - GPU-owned render textures and depth targets never load from or write back
    to guest memory.
  - Storage-buffer and texture views of the same range are linked without a
    copy, and a storage buffer updates only from guest bytes.
  - So one exact buffer or texture view cannot observe or change the scaled
    texels. Depth accepts storage views of its planes for the same reason.
  - Anything else keeps the target native.
- Measured on AMD Raphael (RADV) at the beat 'em up gameplay windows, `Fixed`
  1280x720, with a build of the tree at `29047d27`:
  - RenderTargets: 13.4, 11.8 and 10.2 fps at 300, 335 and 364 draws per
    frame, 260 s, status 1.
  - Display: 6.9, 4.0 and 3.7 fps at 243, 316 and 321 draws per frame.
  - Captures stay healthy, with no seams or offsets.
- On Intel Arc A770 the beat 'em up is CPU-bound and stays flat under
  RenderTargets: 13.9, 12.5 and 10.9 fps against 14.2, 12.7 and 11.6 at
  `Native`. Its captures are correct.
- Refusals at this commit, on both hosts; both are resolved by the demotion
  entry below:
  - The watercolour platformer stopped at a compute storage access to a scaled
    render texture (`GraphicsRenderBind.cpp`, "storage access to a
    resolution-scaled image").
  - The racing title stopped at a cohort that pairs a committed scaled color
    target with a committed native depth ("mismatched_host_extent",
    `attachment_index=1`). A bounded diagnostic showed the depth is
    HTILE-compressed, so it was never a scaling candidate.
- `Fixed` + RenderTargets keeps status 1:
  - on Intel: the gothic action platformer, the ninja platformer, the
    isometric roguelike, the pixel exploration title, the artillery title,
    the roguelite action platformer, the robot platformer and the Unity
    fishing title;
  - on AMD: the isometric roguelike and the roguelite action platformer
    (three of three runs).
  - The other frontier titles were not run under this scope on AMD.
- Regression evidence for this change and the descriptor-cache bound below.
  Builds of the uncommitted tree, whose content matches `ba2288f3` and
  `29047d27`, keep status 1:
  - `Fixed` + `Display` on Intel for the watercolour platformer, the gothic
    action platformer, the ninja platformer, the isometric roguelike, the
    pixel exploration title, the artillery title, the roguelite action
    platformer, the racing title, the robot platformer and the Unity fishing
    title;
  - `Fixed` + `Display` on AMD for the racing title, the roguelite action
    platformer, the watercolour platformer, the isometric roguelike and the
    beat 'em up;
  - `Native` on both hosts for the same sets.
  - The mechanical-fist platformer still stops at the unresolved `Agc` import
    it hit before these changes.
  - Intel A/B at `Native` is neutral. The baseline was a dirty tree at
    `a45f6a70` carrying the indexed-lookup change:
    - racing title: 84.3, 90.1 and 85.9 fps against 90.7 and 87.6;
    - Unity fishing title: 10.5, 9.8 and 10.5 against 10.6, 9.5 and 10.6.
- The sweep wrapper used for frontier runs sets
  `KYTY_RENDER_RESOLUTION_MODE=Native`, while `scripts/run_guest.lua` defaults
  to `Fixed`. Earlier frontier numbers measured `Native` unless they name
  `Fixed`.
- Open, found while verifying:
  - The beat 'em up draws large white quads over a forest scene at native
    extent on AMD. They already appear in a dirty-tree build at `a45f6a70`,
    which predates these changes.
  - The roguelite action platformer on AMD stopped at about 93 s with
    "indirect register packet references invalid guest storage: class=0x13
    address=0x0000002000000001 count=7" (`GraphicsRun.cpp:1263`). It happened
    in two of three runs at `Native` with the RenderTargets scope set, before
    `29047d27` made the scope inert at `Native`. That earlier code recorded
    extra submission uses for its extent lookups.
    - Without the scope it passed six of six runs, and under `Fixed` +
      RenderTargets it passed three of three. The cause is not established.
    - The identical value in both failures points to a deterministic parse of
      stale or misaligned command words.
    - Next step: dump the packet window around the offset in a bounded
      diagnostic.

## Bounded descriptor-set cache (2026-10-12)

- Cause. Descriptor sets were freed only with a resource they reference.
  Under render-target scaling the beat 'em up ran fast enough to grow the
  cache to 261,632 sets in 512 pools. The trigger was compute sets binding
  two long-lived storage buffers in many combinations, about 13.7k resources
  in all. Then:
  - `vkCreateDescriptorPool` returned `VK_ERROR_OUT_OF_HOST_MEMORY`;
  - the failure was ignored;
  - RADV read offset 0xb4 of the null pool in `radv_AllocateDescriptorSets`
    and exited with status 139.
  This also explains the 0xb4 fault in the earlier quadrant experiment.
- Fix:
  - Each set records the submissions that used it.
  - Past 65,536 live sets, the 8,192 least recently used tracked sets leave
    the cache. They are freed through the deferred deletion queue once those
    submissions complete.
  - A failed pool creation now stops with its `VkResult`.
- Verified with a removed diagnostic. The run went 260 s with status 1 and 40
  trims. Every candidate set was tracked, pools settled at 129 instead of
  512, and peak RSS fell from 11.4 GB to 9.3 GB.

## FSR 1.0 presentation upscaling (2026-10-12)

- `PresentationFilter = Fsr` upscales the final image to the swapchain with
  AMD FidelityFX Super Resolution 1.0 (EASU, then RCAS). The headers are
  vendored unmodified under `source/3rdparty/fidelityfx_fsr1` (MIT).
  `PresentationFsrSharpness` (0 to 100, default 90) sets the RCAS strength.
  `docs/render-resolution.md` describes the pipeline and its limits.
- The passes run only when the final image fits inside the swapchain and is
  smaller on at least one axis. Otherwise the linear blit runs.
- Verified on Intel with a 1920x1080 window, `Fixed` 1280x720, and a bounded
  dump of the stage images in a temporary diagnostic that has been removed.
  The beat 'em up was upscaled from 1280x720 to 1920x1080, sharper than
  bilinear. Its mean channels matched a bilinear upscale within 0.004, so
  there is no transfer-function shift. At 1:1 the racing title differed from
  its re-encoded source by an MAE of 0.0044, which is the RCAS sharpening
  alone. The passes add work at 1:1 for no added detail, so equal extents
  skip them. The FSR cost against `Linear` at the same window size has not
  been measured.
- Not a performance fix on its own. `Fixed` with the default `Display` scope
  scales only the display attachment cohort; see the opt-in render-target scope
  above. On AMD the beat 'em up stayed flat: 7.1, 4.1 and 3.6 fps under
  `Fixed` against 6.9, 4.1 and 3.8 at `Native`, GPU about 80% busy. Its
  3840x2160 offscreen passes keep their guest size. That title opens VideoOut
  and sets 3840x2160 buffer attributes without querying the resolution or
  output status, so the reported display mode cannot lower it.
- Temporal upscalers (FSR 2+, FSR 4, XeSS) and frame generation are not
  offered. They need the renderer's motion vectors, depth and jitter. The
  FidelityFX SDK requirements are listed in `docs/render-resolution.md`.

## Indexed physical mapping lookup (2026-10-12)

- In beat 'em up combat on the AMD host, `PhysicalMemory::Find(vaddr)` took
  27% of the command-processor thread. It is reached through
  `KernelQueryMappedRange` and `KernelQueryMemoryProtection` from the
  GPU-memory hash and descriptor paths. Each call scanned all views (about
  26,700) and then all protection blocks (about 26,700).
- Views do not overlap: each holds its host pages until `CompleteUnmap`
  removes it. The lookup now takes the last view that starts at or below the
  address from the existing address index, including views pending unmap as
  before.
- Protection blocks also do not overlap, and every helper that changes them
  keeps them in address order. `Map` now inserts its block in order, and the
  lookup is a binary search.
- Equivalence: a temporary build computed the old and the new answer on every
  call. The beat 'em up on AMD (1,048,576 calls into combat), the Unity
  fishing title on Intel (720,896 calls) and the racing title on Intel showed
  zero mismatches.
- Measured against the previous build (`a45f6a70`) on the same routes:

  | Host | Title, window | Before | After |
  | --- | --- | --- | --- |
  | Intel | beat 'em up, combat perf-7/8/9 | 12.2 / 10.2 / 9.6 fps | 14.2 / 12.7 / 11.6 fps |
  | Intel | Unity fishing title, perf-4..7 | 6.9 / 10.2 / 10.1 / 9.6 fps | 10.4 / 10.5 / 9.5 / 8.3 fps |
  | AMD | beat 'em up, combat perf-7/8/9 | 6.6 / 3.9 / 3.8 fps | 6.9 / 4.1 / 3.8 fps |

  - The Intel beat 'em up p50 fell from 80-103 ms to 72-87 ms at the same
    draw counts.
  - The Unity fishing title keeps only about 620 views, so its result is
    within run-to-run variance.
  - On AMD the command-processor thread fell from 30% to 25% CPU, while the
    GPU's gfx engine rose from 77% to 83% busy. That scene is GPU-bound, so its
    frame rate did not move.
  - The racing-title pair landed on different scenes (142 and 360 draws per
    frame) and is not used.
- Intel frontier sweep with this change: the atmospheric platformer, the
  gothic action platformer, the ninja platformer, the roguelike dungeon
  crawler, the pixel exploration title, the artillery title, the robot
  platformer and the 2D title that dereferences guest pointers all reach their
  time limits as before.
- Open, not caused by this change: the mechanical-fist platformer often ends
  with a host access violation in `PthreadStaticObjects::CreateObject`
  (`Kernel/PthreadObjects.cpp:21`, reached from `PthreadMutexLock` and
  `PthreadMutexUnlock`). The guest passes the mutex address `0x58`, a member of
  a null object, and the static-initializer check reads it. Previous build:
  2 of 2 runs. This build: 2 of 3 runs; the third stopped at the known Gen5
  volume-layout refusal. This is likely the open object-lifetime race recorded
  for that title. Direction: find which guest object is null when the lock is
  taken, starting with the thread that took it.

## Shared physical device-address windows (2026-10-12)

- On the AMD host the beat 'em up never left its loading screen within its
  260 s route (2.0-3.5 fps, 3-4 draws per frame). Every submit paid RADV's
  validation of each imported memory object, and the title keeps about 6,000
  small direct-memory ranges, each imported on its own.
- A temporary discard census on that host (not committed) counted 55-70
  direct-memory allocations per second through the whole run, about 0.6 per
  submit. Windows that are dropped and rebuilt whenever an allocation reuses
  their pages would therefore thrash on this host.
- Change:
  - Physical ranges read direct memory through shared windows. A window is
    an aligned power-of-two interval of whole 2 MiB blocks, at most 64 MiB,
    imported once and used by every range whose pages it holds; windows are
    disjoint. A range adds table entries, not imports.
  - Kernel records each window's pinned interval
    (`KernelMapPinnedPhysicalWindow`, `KernelExportPinnedPhysicalWindowDmaBuf`,
    `KernelUnpinPhysicalWindow`). Every backing discard (allocation, release
    and the two unmap paths) now zeroes pinned intervals in place and
    punches only the rest. A punched page under a dma-buf or host-pointer
    pin kept serving the device while the CPU faulted in a fresh one.
  - `KernelExportPhysicalDmaBuf` is removed; graphics resolves the backing
    offset with `KernelResolvePhysical`.
- Tests:
  - New: direct memory released and allocated again under a window that a
    neighbouring range keeps imported reads zero on the CPU, and the device
    reads what the CPU then writes. With the in-place zeroing disabled the
    device read the previous owner's marker on RADV.
  - The test device now enables dma-buf import where the device offers it,
    as the runtime device does. Before, every physical device-address test
    failed on RADV (the device refuses shared host pointers); all 14 pass
    on both hosts. Full unit suite on Intel: 2,380 pass, 12 skipped. One
    audio rack test timed out once under the full suite and passed 3 of 3
    runs alone.
- Measured on the same routes, `KYTY_RUN_PERF=1`, base binary first or
  alternated, peak cgroup memory:

  | Host | Title | Window | Base (fps) | Windows (fps) | Peak MB base / windows |
  | --- | --- | --- | --- | --- | --- |
  | Radeon 610M, RADV | Beat 'em up | perf-2..5 | 2.7, 9.1, 5.2, 2.2 | 12.1, 11.5, 6.9, 9.3 | 5,026 / 5,152 |
  | Radeon 610M, RADV | Beat 'em up | perf-7..9 | 2.1, 2.2, 2.0 (loading) | 5.0, 3.8, 3.7 (in game) | |
  | Radeon 610M, RADV | Racing title race | perf-4 | 42.4 | 40.9 | 1,994 / 2,110 |
  | Arc A770, xe | Racing title race | perf-4 | 87.3 | 86.8 | 1,999 / 2,206 |
  | Arc A770, xe | Beat 'em up combat | perf-7..9 | 12.5, 10.8, 9.9 | 12.3, 10.5, 9.6 | 6,331 / 6,294 |
  | Arc A770, xe | Unity fishing title | perf-4..7 | 8.5, 10.6, 10.2, 9.5 / 8.1, 9.0, 9.6, 8.7 | 6.7, 8.4, 10.4, 9.3 | 4,031-4,914 / 4,126 |

  - The beat 'em up on AMD now leaves its loading screen and reaches the
    first in-game scene and dialogue at 230-314 draws per frame; the base
    binary stays on the loading screen at 4 draws per frame. The captures
    are correct.
  - The fishing title's windows run was one window behind the base run
    (its perf-5 has the draw count of the base perf-4), so its per-window
    rows are not paired.
- The Unity fishing title's frame-3 boot stall is not caused by this change,
  and is not specific to the excluded index-buffer change below: in 16
  interleaved 45 s boots per binary it stalled once with the windows and
  once with the base binary. A further 200 s windows run also stalled there.
  Open; it now has a base-binary reproduction.
- Frontier with the change:
  - AMD: the 2D title that dereferences guest pointers, the atmospheric
    platformer and the roguelike dungeon crawler reach the time limit.
  - Intel: the atmospheric platformer, the gothic action platformer, the
    ninja platformer, the roguelike dungeon crawler, the pixel exploration
    title, the artillery title and the robot platformer reach the time
    limit, with peaks of 2,783, 3,383, 1,195, 5,751, 1,323, 1,946 and
    3,544 MB. The mechanical-fist platformer stops at its known strict
    volume-layout refusal.
- Remaining on AMD: the beat 'em up's in-game scene runs at 3.7-5 fps.
  Not profiled yet.

## Native wave32 compute in whole host subgroups (2026-10-12)

- On the AMD host the robot platformer reached its PS logo level (102
  presents, agent phase `interactive`). The strict run then stopped at about
  60 s with `native-wave compute coordinate layout unsupported or
  inconsistent`.
- A bounded diagnostic named the dispatch: a wave32 compute program that reads
  wave state (`native_wave_sensitive`), with a 1x1x1 thread group, no LDS and
  no thread limits.
- The native route rebuilt each guest lane index as host subgroup id x 32 +
  subgroup lane. Full host subgroups need a host X that is a multiple of 32, so
  the route refused every guest group whose X was not. A group of fewer than
  32 lanes is one partial guest wave: the lanes past the group never launch on
  the hardware.
- Change: the host workgroup is now `(ceil(N / 32) x 32, 1, 1)` for a guest
  group of N lanes (`ShaderNativeWave32PhysicalLocal`). Guest coordinates are
  still rebuilt from the linear, X-first lane index. Lanes at or past N start
  with EXEC clear, which is the same rule as for lanes past the dispatch
  thread limits; the thread-limit test ANDs with it. `LocalSize` and
  `gl_WorkGroupSize` use the physical shape. The dispatch plan, the SPIR-V
  validator and the Vulkan subgroup attach all check the same shape.
  Translator version 131.
- Tests: the dispatch plan maps a 1x1x1 wave32 group to (32,1,1) and a 16x8
  group to (128,1,1). The Vulkan attach admits the padded shape and still
  refuses a host X that is not a multiple of 32.
- Verification. Unit suite: 2,380 pass. Boundary and table gates pass.
  Intel frontier sweep, one run each:
  - The atmospheric platformer, the gothic action platformer, the ninja
    platformer, the roguelike dungeon crawler, the pixel exploration title, the
    artillery title and the robot platformer reach the time limit, with peaks of
    2,845, 3,361, 1,424, 5,792, 1,291, 1,950 and 3,541 MB.
  - The mechanical-fist platformer stops at an unresolved graphics import. The
    same stop appears on runs of earlier binaries, alternating with its known
    stops.
  - The racing title races at 86.9 fps in a different scene (147 draws per
    frame, against 123 in the earlier baseline). It is a single unpaired
    run, so it is not a speed comparison. The Unity fishing title runs at
    8.6-11.0 fps, and the beat 'em up at 9.4-10.3 fps. Both are within their
    earlier ranges.
  On AMD:
  - The racing title races at 39.1 fps.
  - The 2D title that dereferences guest pointers, the atmospheric platformer
    and the roguelike dungeon crawler reach the time limit.
  - The beat 'em up runs at 2.2-3.0 fps against an earlier 2.4-3.4. This is
    the known per-submit import cost.
- Result on AMD: the strict run passes the former stop. It now stops at about
  60 s on `!create_all_the_same`. A 960-wide StorageTexture at
  0x53bcf0000 overlaps 11 live objects of a 26 MB transient pool: two
  CPU-uploaded StorageBuffers, two 1920x1080 RenderTextures (Crosses), a
  2432-wide StorageTexture, a 2432x1368 RenderTexture (Contains), and a
  640-wide StorageTexture (IsContainedWithin). This is open. The pool reuses
  the same memory across passes, so it needs an aliasing policy for transient
  GPU-owned memory, not another relation special case.
- On Intel the same title stops presenting at about 26 s, before this
  dispatch. The command processor spends about 99% of its time inside the
  Intel Vulkan driver creating a compute pipeline from
  `GraphicsRenderDispatchDirect`, measured with the perf-event stack sampler.
  The process stays alive, which is why the frontier sweep counts it as
  reaching the time limit.
  - A bounded capture of each compute pipeline creation names the cause. It is
    not a hang. Two paired wave64 programs (16x16 groups) translate to block
    dispatcher modules of 321,491 and 346,836 words: 925 blocks and 353
    function variables. The Intel driver compiles the first in 32.2 s, and the
    second does not finish inside the 60 s window.
  - Smaller modules show the same superlinear cost: 104,131 words take 2.8 s
    and 126,719 take 3.6 s.
  - This is the compile-capacity limit recorded under the paired dispatcher
    entries below. Optimizer passes, native structuring, broadcasts and
    Workgroup materialization are already excluded there as sufficient fixes,
    so they were not repeated.
  - The emitted code re-derives the lane EXEC bit for every VALU write in both
    banks (about 12 operations per guest operation). That is a size lead, not
    a proven fix for the interference-graph peak.

## Physical device-address imports in whole blocks (2026-10-11)

- The racing title's physical ranges imported a chunk for every newly resident
  run. Guest writes reach those pages one or two at a time, so one 2 MiB range
  held 364 chunks for 455 imported pages. The title kept about 4,200 imports
  over only 241 GPU-visible physical views. On the AMD host each submit then
  cost about 4.6 ms, and the race issues 16 submits per frame (see the next
  section).
- A physical range now imports every 2 MiB block, counted from the range base
  and clipped to the range, that holds a resident page. Neighbouring blocks
  share a chunk up to the existing 64 MiB chunk bound.
  - Pages the import itself populates belong to the imported block, so
    preparation does not chase residency it created (the self-induced growth
    recorded on 2026-09-29).
  - A block with no resident page is not pinned.
  - The pages belong to the same live allocation, which is discarded only at
    allocation and release, so the GPU and the CPU keep seeing the same pages.
  - Flexible (snapshot) ranges are unchanged.
- Focused test
  `EmulatorGuestDeviceAddress.PhysicalImportCoversResidentBlock` covers:
  - Two separate writes in one block give one entry.
  - Repeated preparation adds nothing and leaves the untouched block
    unpopulated.
  - A write in the second block adds one entry.

  It fails without the change (two entries).
- Measured on the same routes, alternated runs, `KYTY_RUN_PERF=1`:

  | Host | Title | Window | Before (fps) | After (fps) |
  | --- | --- | --- | --- | --- |
  | Radeon 610M, RADV | Racing title race | perf-4 | 13.4, 15.4 | 40.0, 29.2 |
  | Radeon 610M, RADV | Beat 'em up loading | perf-3..4 | 6.0, 7.2 | 7.0, 5.7 |
  | Arc A770, xe | Racing title race | perf-4 | 99.3 | 102.6 |
  | Arc A770, xe | Unity fishing title gameplay | perf-4..7 | 10.6, 9.9, 9.8, 10.0 | 8.7, 8.8, 9.4, 10.7 |
  | Arc A770, xe | Beat 'em up combat | perf-7..9 | 10.4, 9.6, 8.7 / 10.9, 9.6, 8.5 | 9.7, 8.0, 7.2 / 10.9, 9.3, 8.4 |

  Notes on the table:
  - In the second AMD racing run the route went faster. Its perf-4 is a
    heavier race scene (270 draws per frame, against 170), and the race ended
    on "time over" within the run. Its captures are correct.
  - The beat 'em up's ranges are small and were already imported whole, so
    it does not change on either host. Its first Intel pair differed by
    10-17% with higher fence waits; the reversed second pair is equal.
  - The first Intel racing pair reached different scenes: 41.5 fps at 210
    draws per frame with the change, against 96.0 fps at 123 without. The
    base binary never reached the heavier scene, so a regression in heavier
    race scenes is not excluded on Intel.
  - From about 00:55 UTC another application loaded the host. That covers
    every Intel row, which widens their error bars. The AMD racing pairs ran
    before it.
- Peak run memory (cgroup `memory.peak`):
  - AMD beat 'em up: 3,161 to 3,530 MB.
  - Intel beat 'em up: 5,894-5,927 to 5,902-6,166 MB. The run limit is
    `MemoryHigh=7G`.
  - Intel racing title: the same within noise (1,929-2,321 against
    2,131-2,618 MB).
- The 2D title that dereferences guest pointers runs on both hosts with no
  import failure: 102-111 fps on Intel and 28 fps on AMD.
- Not verified: audio. From about 00:55 UTC another application was playing
  audio on the host. After that the audio probe measured decaying or no
  signal for every binary, including the base binary and one built without
  the two AJM commits. That excludes this change and those commits, but leaves
  audio unchecked for these runs.
- Frontier sweep with the change on Intel, one 90 s run each (150 s for the
  robot platformer), with peak memory:
  - The atmospheric platformer, the gothic action platformer, the ninja
    platformer, the roguelike dungeon crawler, the pixel exploration title,
    the artillery title and the robot platformer all reach the
    time limit.
  - Peaks: 2,514, 3,079, 1,164, 5,916, 1,320, 2,011 and 3,542 MB. On the base
    binary the roguelike dungeon crawler peaks at 6,176 MB.
  - The mechanical-fist platformer stops at 26-32 s with either binary:
    - Base (2 of 2): a strict volume-layout refusal at 32 s.
    - With the change: 2 of 3 runs crash in `PthreadMutexLock` on a guest
      mutex pointer of 0x58 at 27-28 s, and the third run is a strict import
      refusal. The crash is the open weak-pointer race recorded for that
      title (about 26 s). No device-address frame is on its stack.
- The Unity fishing title on AMD with the change: 24 fps in the menu window,
  then the same device loss at about 85 s. The failing submit is on the
  utility queue, whose context the kernel reports as innocent, and RADV
  creates one kernel context per queue. The faulting work is therefore on
  another of the emulator's queues.
- Remaining on AMD: the live count of GPU views (next section). The beat
  'em up holds about 6,000 small ranges, each one import.

## Guest device-address imports and per-submit cost on RADV (2026-10-11)

Open performance defect on AMD. Fragmentation within a range is fixed in the
previous section; the count of live ranges stays open.

- Host: Radeon 610M integrated GPU, amdgpu kernel driver, RADV
  (`VK_DRIVER_FILES` pointing at the RADV ICD). Strict runs, scripted routes.
- Measured frame rates on this host:
  - The racing title's race: 15.4 fps.
  - The beat 'em up: stays on its loading screen at 1.4-2 fps for the whole
    window.
  - The Unity fishing title: loses the device at gameplay load, about 88 s
    into the hold route (see the defect below).
- Temporary submit timers (not committed) around `vkQueueSubmit` in
  `CommandBuffer::Execute`
  (`source/emulator/src/Graphics/GraphicsRenderCommandBuffer.cpp`), on the
  beat 'em up:
  - The cost of one submit grows linearly with the number of live imported
    device memory objects: 0.34 ms at 879 imports, 5.78 ms at 6,895.
  - After about 90 s there had been 7,661 graphics submits and 12,819
    utility-queue submits, totalling 20 s and 29 s inside `vkQueueSubmit`.
  - The cost follows the import count. About 20 driver-allocated memory
    objects were live throughout, so their share was not measured. This is
    consistent with RADV passing every imported buffer object to the kernel
    on each submit when buffer device address is enabled.
- The imports are guest device-address chunks
  (`source/emulator/src/Graphics/GuestDeviceAddress.cpp`), one per resident
  run of a GPU-mapped guest range. Censuses at the same moment, roughly 90 s
  into the run:
  - Device-address registry: 6,168 live ranges and 6,365 chunks. Releases
    work: no range is overwritten or left overlapping, and every unmap or
    physical release removes its chunks.
  - Kernel: 12,458 live GPU-visible views (2.85 GB) and 7,642 direct-memory
    allocation blocks (3.26 GB). Of those blocks, only 5 are 64 MiB or larger.
    1.99 GB of the physical backing is populated.
  - The live set is therefore the title's own and keeps growing while it
    loads. On Intel it reaches about 12,000 imports at 150 s. The xe driver
    does not show the per-submit cost, but the count is not bounded there
    either.
- Exclusions:
  - Merging a new run with adjacent chunks of the same range does not help.
    Of 14,078 new runs, 13,784 were isolated physical runs and every sampled
    one was a whole fresh range (no chunk yet, offset 0).
  - Importing per allocation block does not bound the count: 7,642 blocks.
  - Merging exactly contiguous physical runs would save about 14%: 1,798 of
    12,800 imports touch the end of an earlier one.
- What would bound it: importing the physical backing in fixed windows.
  12,800 imports fall into 794 windows of 2 MiB, or 29 of 64 MiB. Two
  constraints stand in the way:
  - Every allocation, and every release with no remaining view, punches the
    backing (`DiscardSharedBackingRange`, `source/emulator/src/Kernel/Memory.cpp`
    in `AllocateUnlocked` and the release paths). A window that pinned those
    pages keeps the old ones, so the GPU would read stale data.
  - A window also pins and populates its unallocated gap pages.

  A window design has to invalidate on every discard, or zero pages in place
  inside pinned windows, and it needs its own lifetime rules for tables and
  publications.
- Discard census for the beat 'em up (2026-10-12): a temporary counter on
  the four `DiscardSharedBackingRange` sites (allocation, release with no
  view, and the two unmap paths), one strict 260 s run on the combat route.
  The guest's allocation pattern does not depend on the host GPU; this run
  was on the Intel host.
  - Loading (first 157 s): 28,220 allocation discards, 12,081 MB in total.
    The release and unmap sites together fired 14 times (77 MB).
  - Combat windows perf-7..9 (83 s): 91 allocation discards (36 MB), about
    one per second. The release and unmap sites did not fire.
  - So in combat on Intel the direct-memory layout is stable. Loading
    churns, and the AMD host spends its whole run loading (see "Shared
    physical device-address windows"), so windows dropped on every
    overlapping discard would thrash there. The implemented design zeroes
    pinned pages in place instead.
- Open defect: on the AMD host the Unity fishing title raises a GPU
  page-permission fault at gameplay load, about 88 s in. The fault is in the
  shader caches (SQC/TCP), at a different address each run, and the device is
  lost. The binary from before this session's changes does the same. Next
  step: take a native capture of the last submit before the fault and check
  its device-address table and resource descriptors against live imports.

## Range-query cache invalidation through compact spans (2026-10-11)

- A stack sampler on the Unity fishing title's gameplay (Intel A770, scripted
  hold route, 15 s window) put 6.9% of command-processor samples in
  `GpuMemory::CreateBlock` and 6.3% in `QueryOverlapsLocked`.
  - Every object creation and deletion calls `InvalidateRange` on the
    range-query caches. Each call walked all 4,096 slots of the overlap
    snapshot cache, about 1.4 MB of entries, to test one key per slot.
- `GpuMemoryRangeQueryCache` and `GpuMemoryAdaptiveRangeQueryCache` now keep
  a compact `[begin, end)` span per slot, as the materialization cache
  already does. An invalidation reads a full entry only where its span
  overlaps. The full-key overlap test and the lookup rules are unchanged.
- The same sampler afterwards: `CreateBlock` 1.0%, `QueryOverlapsLocked` 2.0%.
- Frame rate on the same routes, `KYTY_RUN_PERF=1`:

  | Title | Windows | Before (fps) | After (fps) |
  | --- | --- | --- | --- |
  | Unity fishing title (A/B binaries, alternated) | perf-5..7 | 9.6, 9.6, 9.5 | 9.1, 8.4, 10.4 |
  | Racing title race | perf-4 | 88.3 | 87.8 |
  | Beat 'em up | perf-4 | 11.9 | 12.5 |

  The racing and beat 'em up "before" values are the last recorded windows,
  not alternated runs. The change is within run-to-run noise in frame rate.
  Captures are correct.
- Why less command-processor work does not raise the frame rate: over a 20 s
  gameplay window the command-processor thread used 68% of one core and the
  render engine was busy 44% (`drm-cycles-rcs`). Neither is saturated. The
  title is bound by serialization between them: about 80 submits per frame,
  fence waits 19-23% and `WAIT_REG_MEM` 11-15% of the window.
- `EmulatorAudio.StandardSamplerRackDestroyWaitsForACallbackRunningOnAnotherThread`
  times out (10 s) in about one of four runs of the `EmulatorAudio.*` suite.
  It does so with and without this change, and it passes when run alone.
  Fixed since (ad8d4c76): a lease release woke the rack destroy only at
  zero pins, while the destroy waits for its own last pin.

## Table-loaded comparison samplers paired by instruction (2026-10-11)

- A sampler sharp read from a descriptor table through `S_LOAD` is registered
  twice: once as static metadata (slot 2, regular operation) and once as a
  dynamic sampler whose slot is its dword offset in the table. The image
  instruction resolves to the dynamic sampler. Its slot matches no texture
  slot, so `ShaderSamplerDepthComparisonEligible` refused comparison. Pure
  depth-reference samples then took the manual filter-then-compare path.
- The eligibility check now uses the parse's instruction pairing
  (`sampler_indices_mask`) first. A sampler qualifies only when every image it
  is paired with is a normalized, pure depth-reference 2D view. The slot match
  and the all-descriptors fallback still apply to samplers with no pairing.
  The same answer drives the translator's Dref lowering and the bound
  `VkSampler` compare state, so the two stay consistent. Translator identity
  advances to 130.
- An env-gated, temporary check computed the old and new answers on every
  call during strict runs (Intel A770):
  - The racing title: every mismatch was its two shadow receivers' sampler
    (dynamic slot 4) moving to the comparison path. All 8 receiver modules now
    lower to `OpImageSampleDrefExplicitLod`, with no manual path left, on a
    clean build.
  - The Unity fishing title: about 112,000 mismatches in 4.2 million calls, all
    one shape. One sampler compares a depth-reference image and also filters a
    regular image. The old slot match enabled comparison on that shared
    `VkSampler`, and sampling a regular image through a comparison sampler has
    no defined result in Vulkan. The new check keeps the non-comparison
    sampler. Captures on the same route differ from the previous HEAD only in
    animated regions.
  - The beat 'em up: 0 mismatches.
- Strict runs on the checked build reached their bounds: the Unity fishing
  title 202 s, the beat 'em up 162 s, the racing title 126 s. Twelve
  interleaved 110 s racing runs with and without the change all reached the
  rolling start. Across the earlier runs the change hung once in 10 runs at
  race load (frame ~17620), against 0 in 8 without it, which does not separate
  the two. It is recorded as an open intermittent on that route.
- Frame counts at 110 s average 23,012 with the change and 22,993 without; load
  time dominates that window, so this shows no measurable difference, not a
  race frame rate.
- 2,379 unit tests pass, including a pairing contract for a table-loaded
  depth-reference sampler.
- Open in the racing title: a distance-centred dark disc and curved bands on
  the road around the camera, and stepped shadow edges. Both appear identically
  with and without this change, so it does not fix the visible shadow defect.
  Excluded for them:
  - Caster depth bias: front and back enabled with offset -4 and scale -40
    (slope -2.5 after the `/16` scale shared by three independent
    implementations), 16-bit unorm DB format (`NEG_NUM_DB_BITS` -16).
  - Atlas layout: two 1024x1024 cascades in a 2048x1024 D16 target, with
    viewports and scissors matching it.
  - A per-frame copy of the shadow map: it is bound directly as depth, so it
    has no measured frame-time cost.
  - Later exclusions (frame mismatch of the cascade matrices, bias magnitude,
    depth clip versus clamp, cross-submit hazards, a second host image for
    the atlas) are recorded with their measurements in the graphics handoff.
    The disc is the receiver's cascade-0 region holding real road depth with
    a shortfall between about 32 D16 units and 1/256.
- Next step: read back one receiver's reference and texel inside the
  cascade-0 region in a disc frame, and compare its interpolated world
  position with the caster vertex of the same road triangle.

## Dynamic scalar-load spans memoized per program (2026-10-11)

- `ShaderDynamicSLoadScalarSpan` scanned the whole program for every storage
  buffer reached through a dynamic descriptor load, on every draw. Its result
  depends only on the program and the mapping's load PC, last consumer PC,
  destination register and dword count.
- The per-program `ShaderCodeResourceMemo` now keeps that result. The caller
  folds it in with the same maximum the scan used, so required bytes and the
  dynamic-offset flag are unchanged.
- Verified on the racing title's moving race (Intel A770):
  - An env-gated, temporary equality check compared the memoized and the
    direct result on 7.4 million calls with no mismatch.
  - The function left the command-processor thread's top samples (1.5%
    before).
  - Three 200 s strict runs: two held to the bound. One ended at 183 s on
    the open depth-reference refusal below, which is outside the storage
    path this change touches.
- 2,378 unit tests pass.
- Open, intermittent (1 of 4 runs on this tree, 0 of 7 earlier): a
  comparison sample of a 1x1 16-bit unorm depth image at an address that a
  60x33 render target in use overlaps stops the run with
  `unsupported depth-reference image binding ... materialize=depth-unsupported`
  (`GraphicsRenderBind.cpp`, `ResolveDepthReferenceImageView`). The overlapping
  object's dependencies were not complete when it was bound, so ordering
  against the producing submit is the first lead.

## Instruction type names resolved once (2026-10-11)

- Per-draw shader analyses classify instructions by name prefix
  (`ShaderInstructionTypeStartsWith`), mostly from `ShaderGetInputInfoPS`
  through `ShaderScalarLoadNeedsGuestAddress`, and from
  `ShaderHasUnboundBufferLoad`. Each call resolved the enum name through the
  generic enum reflection.
- `ShaderInstructionTypeName` now reads a table built once over the enum
  range. Names and prefix results are unchanged.
- Measured on the racing title's moving race (Intel A770, task-clock samples
  of the command-processor thread over 20 s):
  - `ShaderInstructionTypeStartsWith` fell from 4.5% to 0.7% of samples.
  - The thread's share of process samples fell from 87.3% to 82.9%.
  - Frame rate stayed within the route's scene variation (28-42 fps at
    405-619 draws per frame).
- 2,378 unit tests pass.
- The remaining per-draw shader analysis on that thread is about 6%:
  `ShaderParseUsage2` 2.4%, `ShaderCollectDynamicScalarResources` 1.7% and
  `ShaderDynamicSLoadScalarSpan` 1.5%. It depends on the program and not on
  the draw, so it is the next memoization target.

## Materialization range scans per draw: excluded (2026-10-11)

- On the Unity fishing title's command-processor thread, `GpuMemory::ResetHash`
  took 4.8-6.2% of samples. It is called once per draw for each render-target
  attachment, and its cost is the scan of the 2,048 materialization spans.
- Tried: skip the scan when the range lies inside a recently scanned range
  and no entry was stored since that scan (invalidation only removes
  entries, so the skip is exact).
- Result: `ResetHash` still took 5.5% of samples. Every draw stores at least
  one materialization between two invalidations of the same attachment, so
  the skip never fired. Discarded.
- The next work on this site needs a span index that answers overlap without
  visiting every slot.
- Tried on 2026-10-12, together, and measured as one change:
  - visit only the slots stored in the current epoch, with swap-remove on
    invalidation;
  - classify `SLoad` instruction types from a table built once, instead of a
    mnemonic prefix compare per instruction and draw.
- Result on Intel, unchanged route, perf-5..7: 10.5, 10.5 and 10.3 fps
  against 10.6, 10.3 and 10.8 fps. Within run noise. Reverted.
  - The `ResetHash` share was not profiled again, so this does not show that
    the scan cost went away. The cache is near full during gameplay, so
    visiting live slots still visits most of them.
- A frame-pointer profile of the same window put `GraphicsRenderDrawIndex`
  at 73% of on-CPU time on the command-processor thread:

  | Path | Inclusive |
  | --- | --- |
  | `BindDescriptors` | 32% |
  | `GpuMemory::CreateObject` | 21% |
  | `ShaderGetInputInfoPS` | 10.6% |
  | `CaptureTransientSnapshotBuffer` | 10% |
  | `GpuDirtyPageTracker::BeginRead`, almost all in `Rearm` | 6.8% |

  - In steady state `Rearm` walks every page of an already armed range, one
    table lookup and one failed compare-exchange per page.
- The window averages about 63 µs per draw at about 1,160 draws per frame,
  plus about 21 ms per frame of fence waits that an on-CPU sampler does not
  see. 60 fps would need about 14 µs per draw. That calls for reusing
  per-draw binding work across draws, not for trimming single leaves.

## Index buffer objects through the mapping: excluded (2026-10-11)

- A fence-wait backtrace counter over the Unity fishing title's 200 s route
  (Intel A770) attributed the waits as follows:

  | Source | Share of waits |
  | --- | --- |
  | `UtilFillImage` texture creates from `PrepareTextures` | 32% |
  | `WAIT_REG_MEM` completing the current producer submission | 28% |
  | Device-address draw retry (`RetryGuestDeviceAddressDraw` in `DrawIndex`) | 19% |
  | Index buffer object create/update (`UtilCopyBuffer`) | 14% |

- Tried: index buffer objects in host-visible coherent memory, written
  through the mapping, the same contract as the vertex buffer objects.
  - Fence waits fell from 43.6 to 33.6 per frame, and fence time from 21.7
    to 15.8 ms per frame.
  - Frame rate was neutral: perf-5 ran at 10.7 fps with 1,158 draws per
    frame, as before.
  - The title's boot stalled at frame 3 in 4 of 16 runs, against 0 of
    about 25 runs before the change and 0 of 12 runs after reverting it.
    In the stall, the command-processor thread was runnable inside the
    driver's graphics pipeline creation (`CreatePipelineInternal`), and
    every other thread waited.
- Reverted, root cause unknown. The waited utility copy changes the timing
  around pipeline creation; that path needs its own investigation before
  index uploads drop the fence again.
- 2026-10-17: the same frame-3 boot stall occurred in 2 of 10 boots. Both
  stalled before any index buffer was bound (see "Index snapshots over
  linked surfaces"), so the stall can occur without the index upload path.
- The Unity fishing title's frame time is per-draw binding work at about
  1,160 draws per frame: resource binding 40-44%, texture descriptors 18%,
  state setup 16-18%. Removing fence waits alone does not move it.

## AJM batch input budget (2026-10-11)

- After the resample-marker fix, the beat 'em up's strict combat run ended
  at 307-329 s with "batch exceeds the host write-range capacity".
- An opt-in capture showed what happened before the failure:
  - The first refused batch held 36 jobs, 8 of them ATRAC9 decodes of short
    effects (1,792 input bytes, 12,288 output bytes, stereo), so 65-77
    superframes of 128 bytes each.
  - The host refused any batch start that decoded more than 64 ATRAC9
    superframes in total, returning `AJM_ERROR_OUT_OF_RESOURCES`.
  - The guest kept appending to the same batch and retried. The start
    failed 75 times, the batch grew to 872 jobs, and it then crossed the
    write-range capacity.
- The 64-superframe count had no guest evidence behind it. The direct
  decode preflight now bounds the ATRAC9 input admitted by one batch start
  by the same 1 MiB input bound as the largest single direct job.
- Test: one start of 72 one-superframe decodes is admitted, and each result
  reports its superframe consumed.
- Result: the next strict run reached the 320 s bound with no AJM refusal
  and no error. Combat windows perf-7..9 ran at 11.8, 10.2 and 9.2 fps,
  which is neutral.
- Open defect: when `AjmBatchStart` fails, the jobs of that batch stay
  queued under its address (`source/emulator/src/AudioAjm.cpp`, batch jobs
  are erased only on a successful start). A guest that appends to the same
  batch after a refusal therefore accumulates jobs until a capacity check
  fails. Direction: capture the guest's retry contract and decide whether a
  refused start must consume or keep its jobs.

## AJM combined resample markers (2026-10-11)

- The beat 'em up's strict combat run ended at `SetResampleParametersEx`.
  An opt-in capture at the rejection recorded:
  - ATRAC9, instance flags `0x1002`.
  - Ratio one, zero increment, flags three.
  - The fresh initialize-then-clear window: no prior profile or decode.
  - A finite gapless stream of 3,072 frames with 256 priming frames.
- Flags three sets both previously evidenced bits:
  - Bit zero: the reset marker of a fresh stream.
  - Bit one: the end marker on a finite stream's last calls, from the
    current ratio.
- On a stream whose first call is also its last, the cleared stream's
  current ratio is the neutral one. Flags three is therefore admitted only
  for the neutral profile inside the reset window. `GetResampleInfo` accepts
  the stored value.
- Flags are admission qualifiers only; resampled output does not depend on
  them.
- The former `UnknownFlag3` refusal scenario asserted exactly this observed
  sequence and was removed. A positive gapless case and two refusals
  replace it: flags three at a non-unity ratio, and flags three after a
  profile closed the window.
- Result: the next strict run passed the marker and reached 307 s, with
  non-zero output RMS. It then stopped at the AJM batch write-range
  capacity, which is the next item.

## Vertex buffer objects without a staging fence (2026-10-11)

- Beat 'em up combat baseline (Intel A770, 260 s route, windows perf-7..9):
  8.0, 7.4 and 6.6 fps, p50 122-142 ms, 257-350 draws and 262-343 submits
  per frame. Fence waits took 10.6 s of a 26 s window.
- A fence-wait backtrace counter attributed 71% of all waits to
  `UtilCopyBuffer` inside the vertex buffer object's update.
  - Every create or update allocated a staging buffer, then submitted a
    one-shot utility-queue copy and waited for its fence.
  - It ran on two paths: from `BindVertexBuffers` when the title binds
    vertex data that a compute dispatch wrote as a storage buffer, and from
    the storage write-back at end-of-pipe, which refreshes the linked vertex
    objects.
- Excluded on the same route:
  - `WAIT_REG_MEM` totalled 30 ms per window; its waits take the
    consolidated barrier path.
  - The guest device-address storage write-back fired 24 times in a full run.
  - The guest device-address snapshot-reader wait fired three times.
- Vertex buffer objects now use the transient snapshots' host-visible,
  coherent memory, and update writes guest bytes through the mapping.
  - Coherent host writes are visible to every later submission, which is
    the ordering the waited copy provided.
  - In-flight readers were unsynchronized before as well.
  - On the A770 both kinds resolve to the same system-memory type.
- Measured on the same routes:

  | Title | Windows | Before (fps) | After (fps) | Notes |
  | --- | --- | --- | --- | --- |
  | Beat 'em up combat | perf-7 | 8.0 | 12.0 | Submits fell from 262 to 5 per frame; upload time from 7.8 s to 0.08 s per window |
  | Unity fishing title | perf-5..7 | 10.7, 9.6, 10.1 | 10.7, 10.3, 10.6 | |
  | Racing title | perf-5/6 | 43.0, 42.6 | 36.5, 36.5 | See below |

  - The racing title's windows did not land on the same scene (381-413 versus
    468-479 draws per frame). Its draw-processor cost per draw fell from
    47.2 to 45.9 µs, and it keeps only 4 to 5 vertex buffer objects alive.
- Remaining waits in beat 'em up combat:
  - About 1,500 fence waits per window still take 6.8 s.
  - The next site is the full quiesce on guest direct-memory release:
    `release_direct_memory` → `GraphicsInvalidateGpuMappingRanges` →
    `RunQuiesced`.
- The index buffer object keeps the same staging copy and fence. The Unity
  fishing title creates about 1,600 index buffer objects per window.
- Open defects:
  - `AudioAjm.cpp:271` rejects `SetResampleParametersEx` and ends the beat
    'em up's strict run in combat (2 of 4 runs, at 248 and 269 s). The faster
    combat makes it more likely to reach that point. Still open on
    2026-10-14 (1 of 3 runs, near frame 706): the refusal is the profile
    admission `IsAjmResampleProfileAdmitted` (`AudioAjm.cpp:2580-2583`).
    Direction: capture the refused job's ratio, increment and flags at that
    point and extend the admission only from that evidence.
  - Beat 'em up combat shows a flat lilac parallelogram above the companion,
    probably a bubble or prompt drawn without its content.

## Atomic container reference counts (2026-10-11)

- In the Unity fishing title's gameplay profile, mutex lock, trylock and
  unlock were 10.8% of the command-processor thread. Most callers were
  `Core::Vector::Add`, `String8` construction and container copies.
- `RefCounter`, the base of every `SimpleArray`, took a `Core::Mutex`
  around each reference-count read or change. Each array also heap-allocated
  that mutex when it was constructed.
- The count is now one `std::atomic`.
  - Copy-on-write copies before it drops its reference. The old order
    released first and copied afterwards, which could read a freed array if
    another owner released at the same time.
  - The lock never protected the array contents, so concurrent mutation of
    one container was not synchronized before either.
- `ShaderInstructionLoweringPreconditions`, which runs for every instruction
  of every NGG front proof, built an allocated mnemonic string per
  instruction. It now uses the shared MUBUF/MTBUF table and non-allocating
  prefix tests.
- The two changes were measured together. Same route, gameplay windows 5-7:
  - The title went from 8.2-9.4 to 9.6-10.7 fps (p50 104-114 to 93-99 ms),
    and the capture is correct.
  - The racing title's race window rose from 82.9 to 88.3 fps.
  - The beat 'em up rose from 11.1 to 11.9 fps.
  - Both captures are correct.
  - The full unit suite passes (2377 tests).

## Vertex snapshots from the first fetched record (2026-10-11)

- The Unity fishing title copied about 48 GB of guest memory per 26 s
  gameplay window, almost all of it command-buffer vertex snapshots. A
  temporary per-site counter over 10 s showed:
  - 36.5k fresh vertex captures, 14.0 GB in total.
  - 11.3k of them (10.0 GB) reused the base address of an earlier snapshot
    in the same command buffer, with a larger size. The guest declares a
    vertex range that grows from draw to draw over one shared buffer, so
    each capture copied again from record 0.
  - No capture repeated an earlier base and size with changed contents.
  - Only 373 captures (0.3 GB) fit inside an earlier snapshot. Reusing a
    contained view with an explicit offset would recover about 2% of the
    bytes on this title; it was not implemented.
  - The declared range always equalled the captured size, so capturing the
    whole declared range would not help either.
  - No index scan reached the restart value.
- Every vertex binding steps per vertex, and the draw's vertex offset and
  binding offset are unchanged. A draw can therefore fetch only records
  `[first, last]` of its index (or linear vertex) range.
  - The snapshot now copies only from the first fetched record. Bytes are
    placed at their own offsets in a buffer of the same size, so the
    shader sees the same addresses.
  - The bytes before the first record hold no guest data. The entry records
    where its snapshot begins.
  - Exact reuse requires an earlier snapshot that begins at or before the
    request, and compares only the requested range inside the same
    dirty-page transaction.
  - Index, storage and depth/stencil-copy captures still start at byte 0.
- Same route, gameplay windows 5-7:
  - Uploaded bytes fell from 47.5-49.2 GB to 10.5-11.9 GB per window.
  - Snapshot upload time fell from 2.8 s to 1.0 s per window.
  - The title measured 8.2-9.4 fps (p50 104-114 ms), against 8.4-8.7
    before. The capture is correct.
- Controls, with correct captures: the racing title's race window measured
  82.9 fps and the beat 'em up 11.1 fps.
- Open defect, made wider by this change:
  - In `GraphicsRenderDraw.cpp`, the indexed draw computes the vertex range
    from live guest index memory (`ResolveIndexedVertexRecords`, line 1640).
    It captures the index snapshot later (line 1767), outside that read.
  - If the guest rewrites indices in between, the draw uses indices whose
    range was never scanned. Records above the scanned maximum already read
    the zeroed tail. Records below the scanned minimum now read stale bytes
    from a recycled entry.
  - Suggested direction: capture the index snapshot first, and derive the
    range from the captured bytes.
- Open defect, unchanged: in `ShaderComputeWaveAnalysis.cpp` (line 1197),
  the native lowering admission tests the prefix `Tbuffer`. The mnemonic is
  `TBuffer`, so MTBUF instructions are never classified as memory there.
  Fixing it changes which programs are admitted. It needs its own focused
  test and a strict run.

## Per-draw scan costs (2026-10-11)

- Two per-draw scans on the command-processor thread were removed from the
  Unity fishing title's profile.
  - `ShaderVectorBufferDescriptor` classified every instruction of the
    program as MUBUF or MTBUF by comparing its type name, on every draw.
    The classification is now a table built once from the same names.
    Name comparison fell from 5.1% to 2.2% of the thread; the remainder
    comes from other callers.
  - `ResolveIndexedVertexRecords` found the index range in a loop that
    switched on the index width per element. It now runs one loop per width,
    which the compiler vectorizes. The function left the profile (5.7%
    before).
- Same route, gameplay windows 5-7: the title went from 8.0-8.1 to 8.4-8.7
  fps (p50 121 to 112 ms), and the capture is correct.
- Controls, both within run-to-run noise and with correct captures:
  - The racing title's race window measured 81.0 fps.
  - The beat 'em up measured 11.2 fps.

## Descriptor-set reverse index (2026-10-11)

- `DescriptorCache::FreeDescriptor` scanned every cached set, each several
  KB of resource ids, whenever an image or buffer was destroyed. On the
  Unity fishing title, the image variant was 7.1% of the command-processor
  thread, and nearly all of another thread's samples went there as well.
- The cache now indexes live sets by the unique id of every image and
  buffer they reference. Images and buffers share one id counter.
  - A release looks up candidates, applies the same per-variant reference
    rule as before, and frees the sets in index order.
  - Insertion, release and reset keep the index in sync.
- Same route, gameplay windows 5-7: the title went from 7.2-7.3 to 8.0-8.1
  fps (p50 121 ms, p95 132-146 ms), and the capture is correct.
- Controls:
  - The racing title's race window rose from 79.6 to 83.1 fps.
  - The beat 'em up held at 11.5 fps.
- Open defect, not changed:
  - In `GraphicsRenderDescriptor.cpp`, the image release rule
    (`FreeDescriptor(VulkanImage*)`) does not test
    `textures2d_sampled_depth_id`.
  - A set that references a destroyed image only through a depth-sampled
    slot therefore stays cached until reset.
  - Unique ids are never reused, so the set cannot be matched again. It
    only keeps its pool slot.
  - Suggested direction: include the depth-sampled ids in the rule after
    checking that a depth sample view owns no other lifetime.

## Transient tail clearing (2026-10-11)

- When a command-buffer transient entry is larger than the bytes it
  receives, `Commit` zeroes the rest so reads past the logical size see
  zero. It cleared the whole tail on every commit. On the Unity fishing
  title, that `memset` was about 4% of the command-processor thread
  (`CaptureTransientSnapshotBuffer` from vertex binding).
- Each entry now records where its possibly non-zero bytes end. A new
  entry counts as non-zero to its full size. Uploads and capture attempts,
  including refused ones, raise the mark before they write. `Commit` clears
  only `[size, mark)` and then lowers the mark to `size`. Only host uploads
  and captures write these read-only entries, so every byte past the
  logical size still reads as zero.
- Same route, gameplay windows 5-7: the `memset` samples fell from 601 to
  259, and the title went from 6.6-7.2 to 7.2-7.3 fps (p50 128 ms).

## Range-filtered label fence snapshots (2026-10-11)

- A sampler that records the user stack showed where the mutex samples on
  the command-processor thread of the Unity fishing title came from. About
  13% of that thread was `LabelFenceRegistry::Snapshot`, called by every
  label-aware upload. Each call copied the whole registry, where entries
  stay until unmap, into `Vector`s whose `Add` takes a lock per element.
- `Upload` and `Copy` use only destinations that overlap the object, and
  `MemcpySkipAbsoluteRanges` already drops holes outside the copied range.
  `Snapshot` and `SnapshotExcluded` now take that range and return only
  the overlapping entries, so the result does not change.
- Same route, gameplay windows 5-7, with the shader memo in place and the
  lap timers compiled in: 6.1-6.2 to 7.3-7.6 fps.
- Clean build with both changes, against the materialization-span run:
  - The Unity fishing title went from 5.5-5.8 to 6.6-7.2 fps, and p50 frame
    time from 169 to 131 ms. The capture shows the same scene.
  - The racing title's race window went from 69.8 to 79.6 fps, with p95
    falling from 58 to 44 ms.
  - The beat 'em up's late window went from 11.3 to 11.6 fps.
  - The command-processor thread is still the limit, at about 81% CPU while
    the render engine is about 30% busy.
- Open items:
  - The registry size was not measured.
  - `Register`, `Complete` and `NeedsUpload` remain linear scans.
- Next costs on the same thread, ranked:
  - `memcpy` in `GpuMemory::CaptureSnapshotReadOnlyBuffer`, from the
    display and depth-only resolution cohorts, plus `memset` in
    `CommandBuffer::CaptureTransientSnapshotBuffer`, from
    `BindVertexBuffers` and `MaterializeRenderColorInfo`: about 18%
    together. The stripped libc reports these routines under the wrong
    symbol.
  - `DescriptorCache::FreeDescriptor`: 5.8%.
  - Indexed vertex-record scans: 5.7%.
  - Dirty-tracker rearm: 4.9%.
  - `ShaderInstructionTypeStartsWith`, a name-string compare from the
    assembled buffer-descriptor scan: 4.8%.
  - `GpuMemory::ResetHash`: 4.2%.

## Per-program shader resource analyses (2026-10-11)

- Lap timers in `ShaderParseUsage2` on the Unity fishing title (gameplay,
  about 7,000 pixel-shader parses per second) put about 1.1 s per 10 s
  in storage-use, direct-image and sampler-operation scans and 0.7 s in
  the consumer scans of dynamic scalar loads. These results depend only on
  the program code and a start register. The entry values come from the
  code's own cached scalar flow.
- The pixel-shader analysis cache entry now owns a `ShaderCodeResourceMemo`
  next to the code and scalar flow. It holds those results per start
  register, plus the consumer analysis of every whole-descriptor S_LOAD
  computed once per program. Descriptor words, EUD spans and table reads
  stay per draw. Vertex, compute and fold-capture callers pass no memo and
  analyze directly.
- With the lap timers compiled in, the scans fell from 368/412/309 ms to
  9/101/37 ms per interval, and the dynamic scalar-load lap fell from 720 ms
  to 165 ms. Gameplay fps went from 5.5-5.8 to 6.0-6.3. The S_LOAD part
  alone gave no measurable fps change, because the label snapshot below was
  the larger cost on the same thread.

## Materialization range invalidation (2026-10-11)

- A user-space IP sampler on the command-processor thread of the Unity
  fishing title (gameplay, 15 s) put 8.6% of its samples in
  `GpuMemory::ResetHash`. Each draw resets the hash of its attachments, and
  every reset walked all 2,048 materialization cache entries, about 450 KB
  of full keys, to drop the overlapping ones (about 7 us per call).
- The cache now keeps one compact `[begin, end)` span per slot that covers
  all of the key's ranges. Invalidation scans those 32 KB and checks the full
  key only where a span overlaps, so the result is the same exact per-range
  check. The existing cache tests cover disjoint, multi-range and adjacent
  keys.
- Measured on the same route, gameplay windows 5-7: p50 frame time 185-187 ms
  to 169 ms, 5.0-5.3 to 5.5-5.8 fps. The gameplay capture is unchanged. The
  racing title (69.8 fps race window) and the beat 'em up (11.3 fps late
  window) show no regression.
- The same profile ranks the next costs: per-draw shader resource analysis
  in `ShaderParseUsage2` (about 21% in storage-use, SGPR-liveness and
  dynamic scalar-load scans that depend only on program code), mutexes
  (about 12%), dirty-tracker rearm (3.3%), indexed vertex-record scans
  (3.2%) and label fence snapshots (3.1%).

## Dirty-tracker identities revoked by unmap (2026-10-11)

- In gameplay on the Unity fishing title (held-cross route), every
  `stable_buffer_create` fell back (1,359 of 1,359 per 26 s window) and
  content hashing read about 48 GB per window, all of it through the hash
  fallback. A temporary build counted registration refusals by reason: all
  were the page identity budget, with the table at 131,072 identities and
  only 104-135 live ranges. No range was marked fallback after registering.
- Classifying the table: 102,960 identities were retired pages whose token
  an unmap had already revoked, 11,984-15,022 were retired with a live
  token, and 13,092-16,225 were referenced. Those revoked identities had
  retired with a live token and were unmapped afterwards.
- A revoked token restores nothing. A late fault over such an identity
  fails the captured-writable check and is reported untracked, which is the
  same result as having no slot. `UnregisterRange` already frees untokened
  pages at once on that basis. Admission now frees retired, unreferenced
  identities with a revoked token when the budget would otherwise refuse
  a registration. A publisher-side counter gates the sweep, so a refusal
  with nothing revoked costs no scan. The sweep runs on the registration
  thread under the registration mutex and frees each slot through the
  existing identity-writer and restorer fence. Retired identities with a
  live token keep their late-fault evidence, and the budget is unchanged.
- Measured on the same route, gameplay windows 5-7: fallback hashing went
  to zero, all hashing fell from about 48 GB to 5.3 GB per window,
  `stable_buffer_create` captured 1,345 of 1,345, and the frame rate went
  from 4.7-5.0 to 5.0-5.3 fps. Gameplay captures are unchanged. The racing
  title (68.4 vs 70.3 fps in the race window) and the beat 'em up (10.4 vs
  10.7 fps) are unchanged within run noise.
- Open: the Unity fishing title is still bound by per-draw resource binding
  (41% of the window), state setup (24%) and about 31 GB of uploads in
  430,000 calls per window. Retired identities with a live token still count
  against the budget. The run audio probe measured silence on the racing
  title and only a few seconds of signal on the Unity fishing title, in the
  baseline and the change runs alike; this was not investigated here.

## Physical view index and residency sweeps (2026-10-11)

- On the beat 'em up, guest device-address preparation took about 15% of the
  run, about 40% of command-processor time. Each change in physical
  population starts a residency sweep, about 13 per second. A sweep took
  about 12 ms: 0.6 ms collecting about 10,000 unimported spans, 10 ms finding
  the unpopulated ones and 1.6 ms importing the roughly 1,000 newly populated
  pages.
- Inside the 10 ms: 4.8 ms of SEEK_DATA queries (one per span, about 0.48 us
  each), 1.4 ms of view lookups, and 3.5 ms re-sorting the index of 26,000
  guest views. The guest maps and unmaps views every frame, so the index was
  stale at every sweep.
- The index now stays sorted through every insertion and removal. Equal
  addresses keep insertion order, as the stable rebuild did. A temporary
  build compared the incremental index with a full stable rebuild at 1,000
  sweeps and found zero mismatches.
- Measured on the same route: the beat 'em up's late window went from
  10.4 fps (p50 101 ms) to 10.7 fps (p50 91 ms), and residency fell from 14%
  to 11%. The racing title is unchanged. The Unity fishing title was
  measured in gameplay (held-cross route) only with this change built, at
  about 4.7-5.0 fps; there is no before/after pair for it.
- Open: the remaining sweep cost is one SEEK_DATA per unpopulated span. That
  title is latency-bound: guest, command processor and GPU each use about a
  third of the frame, one after another. Rendering at a fixed 1280x720
  extent gave the same frame rate and the same GPU engine occupancy.

## Per-draw vertex-stream reader scan (2026-10-11)

- Timers around each part of the draw state setup on the racing title
  (discrete device, race scene) put about 65 us of every draw into one
  check: whether a vertex-stream SSBO keeps a reader after the generator
  rewrites format loads into fetches. Each draw repeated the full symbolic
  fetch detection over the linked program and named every instruction
  through its enum string. That was about 36% of the command-processor
  thread.
- The detection depends only on the immutable program code and the two
  fetch-table registers. It now runs once per cached vertex program and
  register pair, and keeps each fetch candidate with its attribute semantic.
  Each draw resolves only those semantics against its own input resources,
  so the verdict stays per draw.
- Equivalence: a temporary build computed both verdicts on every draw:
  376,832 draws on the racing title, 114,688 on the Unity fishing title
  (its autosave notice, before gameplay) and 16,384 on the beat 'em up, with
  zero mismatches.
- Measured on the same route against two baseline runs: the race window
  rose from 45-46 to 70 fps (p95 frame time 91-101 ms to 60 ms), and the
  three earlier windows rose 27-60%. Captures of the race scene are
  unchanged.
- Remaining per-draw costs from the same timers: pixel input info and probe
  validation (25-40 us per draw on the racing title) and guest device-address
  read preparation, which costs about 1.4 ms per draw on the beat 'em up and
  is its dominant cost.

## Guest physical memory on host-pointer-restricted drivers (2026-10-11)

- The integrated RADV device stopped every title that dereferences guest
  pointers: importing the shared direct-memory alias returned
  `VK_ERROR_INVALID_EXTERNAL_HANDLE`. A standalone probe on the same host
  shows that the amdgpu driver imports only private anonymous host pointers
  (shared anonymous, shared memfd and private memfd mappings are refused),
  while the discrete device accepts all four. Guest physical memory is a
  shared memfd, so the capability check at device creation was not enough.
- At the first physical import, one shared scratch page decides the import
  path for the device lifetime. If the device refuses the host pointer,
  physical ranges are exported from the backing as dma-bufs through
  `/dev/udmabuf` and imported with `VK_EXT_external_memory_dma_buf`. The
  backing memfd is now sealed against shrinking, which udmabuf requires;
  punch-hole discards are unaffected. Devices that accept the host pointer
  keep the previous path unchanged.
- The probe verified GPU writes visible through the CPU mapping and CPU writes
  read by the GPU on both devices. udmabuf needs read-write access to
  `/dev/udmabuf` (a seat ACL on the reference host) and limits one export to
  `size_limit_mb` (64 MiB by default, equal to the import chunk bound). When
  neither path works, the run reports it once and still stops on the existing
  strict refusal.
- Verified on the integrated device: the roguelike, the watercolour
  platformer, the beat 'em up, the Unity fishing title, the zoo title, the
  insect platformer, the pixel-art well title, the racing title, the dream
  platformer, the fighting title and the 2D ninja title run their bounded
  routes with captures. The tower-defence title loses the device on a GPUVM
  read fault from scalar loads (`SQC`, permission fault) after about 100 s;
  open. The discrete device keeps its reference results on the roguelike,
  the watercolour platformer, the ninja title and the tower-defence title.

## ATRAC9 decoder checkpoint (2026-10-11)

- ATRAC9 superframes decode through the bundled LibAtrac9 instead of FFmpeg.
  FFmpeg's decoder rejects valid superframes whose baseline-relative scale
  factors wrap past 31; two titles stopped on that strict AJM rejection, and
  both now reach gameplay with audio. Inputs are copied into a zero-padded
  buffer, frame consumption is bounded by the superframe, and a failed frame
  restarts the stream.
- `SetGaplessDecode` with reset one is admitted on an ATRAC9 instance that
  already decoded: it restarts the priming trim and the finite output limit.
  A title reconfigures decoded instances that way, always with no pending PCM
  in the observed run. Reset zero after a decode, an unlimited total, prior
  skipped frames and the same reset on Opus instances are still refused.

## Contract-closure checkpoint (2026-10-10)

These are working-tree results at translator129, not a compatibility claim or
validation of every later edit. The latest focused audio checkpoint ran 101
cases: 100 passed and the no-FFmpeg admission case skipped because this host
has the backend. The full CPU suite ran 2,357 cases: 2,352 passed, with five
documented platform/environment skips and no failures. Strict emulator
boundaries and all 13 graphics-table provenance checks passed. The Vulkan
validation layer was confirmed at instance and device creation in the suite,
with no matching validation errors. Temporary, default-off investigation
instrumentation remains in this build; it is not a final clean-build result.
An earlier scalar native integration passed 81 numerical cases, including 12
signed 64-bit bitfield cases, on the available Intel device. AMD, NVIDIA,
Windows, the pinned Nix environment and live CI were not exercised.

- The optional raw shader dump now owns the same immutable words used by the
  parser, has a bounded registered extent and excludes unsupported fused or
  truncated forms. The exact four-byte extent and adjacent-byte canary fixture
  passes; no fresh guest-memory read is used to produce the raw dump.
- A missing-heap optional alias query returns an empty result before indexing
  the heap container. Its public query fixture passes. This does not admit
  resource creation from unregistered memory or qualify every alias consumer.
- Two native reader-lifetime fixtures first reproduced a tracked snapshot
  changing after recording or becoming refreshable while a queued reader was
  pending. Both establish a nonempty table and a successful initial device
  load; the pending fixture uses an enabled timeline semaphore. Reader-aware
  preparation now defers changed snapshots, retains the latest reader per
  queue, and acknowledges exact submission completion after publication.
  Draw and dispatch retries wait outside recording locks and prepare against
  the current command buffer. A further native fixture reproduced unbounded
  retired-table growth while a reader remained pending. Retired snapshots now
  have a 32-table cap; a full reader-bearing set defers preparation rather than
  invalidating its contents. The 12-case address suite passes. A second guard
  at the actual refresh point handles dirty writes after the initial preflight;
  that exact interleaving lacks a
  deterministic fixture. Independent source review found no supported reader
  lifetime defect. Native fixtures do not yet exercise production command-
  processor rotation and retry; fresh guest regression remains pending. Clean
  reader-bearing preparations scan tracked chunks twice, an unmeasured cost.
  No workload corruption or performance cause has been demonstrated.
- Direct ATRAC9 initialization, decoding and reset retain per-instance decoder
  state and stage guest writes behind mapping-identity validation. The positive
  synthetic fixture has independently decoded, nonzero PCM. Reset retention
  and the result counter remain bounded implementation qualifications; no
  broader guest decoder contract or real-title audio acceptance is claimed.
  The live decoder budget includes committed and staged replacements before
  opening another decoder. Full-budget rejection and raw batch-queue error
  clearing pass their atomic-publication fixtures.
- Direct mono/stereo Opus split decoding now retains decoder state, snapshots
  descriptor arrays, validates complete length-prefixed packet records, admits
  whole packet records, scatters actual PCM bytes and
  publishes results behind mapping-identity validation. Synthetic numerical,
  reset, partial-progress, descriptor-lifetime and malformed-input cases pass.
  A private trace ties the queued job to its result argument. Static analysis
  subsequently establishes that the factory publishes the same live state
  object to both the submission registry and the consumer's owner field. The
  submission loop passes that object to the enqueue path. The unrelated outer
  readiness callback receives a different object and is not result evidence.
  The cumulative counter's frame units remain an implementation qualification,
  not demonstrated hardware equivalence.
  A fresh strict guest run still stopped after an autosave
  notice, before a confirmed menu. A bounded debugger capture identifies the
  first rejected operation as `SetGaplessDecode` in the queued-job pre-scan,
  before any initialization or decode in that six-operation batch. The batch
  also requests `SetResampleParametersEx` and `GetResampleInfo`; captured rate
  arguments are one, zero increment and zero flags. The same-object consumer
  reads a float at the resample result's offset eight and a signed count at
  offset twelve, clamping negative counts to zero before sample accounting.
  These accesses establish a result prefix, not the full output extent or the
  semantics of other rate profiles. The admitted initial gapless profile now
  removes priming frames once and caps the finite decoded output across calls.
  Neutral resampling requires explicit configuration, and its info query stages
  only the known 16-byte prefix behind the same mapping-identity checks. Clear
  and initialization reset both qualifications. Numerical split-call trimming,
  exact-prefix/tail preservation and unsupported-profile atomicity cases pass.
  The updated focused audio suites ran 105 cases: 104 passed and the no-backend
  admission case skipped on this FFmpeg-enabled host. A fresh strict run then
  reached a visually inspected new-game menu. One consumed input edge progressed
  to another strict `SetResampleParametersEx` rejection. An entry-point capture
  shows ratio one and zero increment with flag bit zero set, on another newly
  initialized and cleared Opus instance. The guest's producer sets this bit at
  initialization and clears it after successful submission. The neutral profile
  now admits that marker only in the fresh initialize-then-clear window, with no
  prior resample configuration or decode. Configuration or decode consumes the
  window; later clears cannot reopen it without another initialization. Positive
  numerical cases cover both flag values, and unsupported sequences preserve
  output canaries. The focused audio suites ran 106 cases: 105 passed and the
  no-backend admission case skipped on this FFmpeg-enabled host. Broader flag,
  rate-change and reset semantics remain unqualified. A fresh strict run reached
  the same new-game menu and progressed after input beyond the former marker
  rejection. During the following video-opening sequence it rejected an Opus
  input that did not parse as complete U16LE packet records. Capturing the input
  boundary and descriptor contents is the next audio frontier; gameplay and
  audible-output acceptance remain open.
  A later transaction-confirmed observation records a zero-input-consumption,
  zero-output result followed by reuse of the same input address with a changed
  head and preserved overlapping tail. This excludes a claim that the earlier
  planned cursor alone proves guest consumption; the native writer and result
  consumer remain unobserved. Opus split decoding now drains retained PCM first,
  admits a complete packet even when only part of its decoded PCM fits, and
  retains at most one packet's permitted tail. Preflight simulates pending
  frames and gapless state in job order, including Initialize and Clear; runtime
  checks its result against that plan. The cumulative counter still counts
  delivered per-channel frames. Numerical cases cover exact PCM across partial
  output and an input-free drain, scatter boundaries, priming and finite output
  limits, and same-batch Clear ordering. Independent clean-build validation ran
  106 focused audio cases: 105 passed and the no-backend admission case skipped.
  Strict emulator boundaries and all 13 graphics-table checks passed. One fresh
  strict run animated the opening for six minutes and consumed one input edge;
  a repeat on the same binary again rejected an incomplete length-prefixed
  packet stream before input. The partial-PCM defect is covered, but the
  intermittent guest input-boundary failure is not closed. Counter equivalence,
  audible output and gameplay remain unverified. The existing decoder-state
  mutation before guest-write commit also remains a transaction qualification.
  A later bounded observation localizes a failing input's lifetime: the complete
  enqueue-time snapshot contains one valid length-prefixed packet, but the same
  descriptor ranges contain a changed head at batch-start preflight. The output
  and result ranges are separate from that input. The instance still has pending
  PCM and remains below its finite output limit, excluding completion at that
  limit for this occurrence. Static consumer dataflow independently shows that
  result offset eight advances a bounded input cursor in bytes, and offset
  sixteen participates in cumulative output accounting. The ordinary native
  producer appends at the ring tail; a separate transition path can reset the
  cursor without clearing occupancy. Neither path has been observed writing the
  changed interval yet. A subsequent bounded ring observation found unchanged
  input through successful submissions, with cursor advances matching committed
  consumed bytes. In a later failing sequence, the first batch-start rejection
  was an ordinary invalid-parameter return while the input still contained a
  complete packet. Repeated failed starts retained queued jobs; later enqueues
  hit the resource limit, and a retained job then failed packet validation after
  its input storage was reused. A focused follow-up identified that first return
  as exhaustion of the host transaction's 16-range limit when adding the batch
  error range. The prior ranges passed extent, non-overlap and mapping-snapshot
  validation. This demonstrates an internal batch-capacity defect; it does not
  establish the failed-batch disposal contract or justify taking a payload
  snapshot earlier. The transaction now accepts up to 1,024 ranges under one
  mapping lock, while AJM independently bounds a complete batch to 546 plans:
  32 jobs with at most 16 output descriptors and one result each, plus the batch
  error and identifier. Per-range byte and mapping-segment limits are unchanged.
  Stable leases cover both whole-batch preflight and publication; ranges are
  never committed in chunks. The old capacity reproduced failures in a
  1,024-range transaction fixture and an 18-range audio batch. With the repair,
  163 focused cases ran: 159 passed and four backend/platform cases skipped.
  The late read-only-range fixtures preserve all earlier output canaries, and
  successful audio publication matches independently decoded PCM. A further
  focused run passes the maximum 546-plan audio batch and verifies that retrying
  the same queued batch after a late read-only identifier failure preserves
  decoder progress and produces the expected PCM, counters and canaries. Strict
  emulator boundaries and all 13 graphics-table checks passed. Independent
  source review found no material capacity defect. A fresh strict run on the
  clean repaired build reached a later unsupported `SetResampleParametersEx`
  request. A bounded follow-up captures ratio one, zero increment and flag two
  on an already configured decoder near its finite output limit, with pending
  PCM still present. Static native caller dataflow derives flag two from a
  source-record bit, permits an empty-input decode when it is set, and prevents
  fetching a new source block at that boundary. A bounded opt-in experiment
  preserves pending decoder PCM across this neutral-rate marker. Two finite
  streams then publish partial output across successive batches, consume an
  additional complete record where needed, and reach their configured output
  limits with no pending decoder PCM. Separate resample-info counts remain
  zero. The marker therefore does not prove that source input is empty, and
  decoder-held PCM must not be conflated with resampler-held samples. A repeat
  captures the next rejected setter before publication: a constant ratio of
  approximately 0.91565, zero increment and zero flags on a freshly cleared
  stereo Opus instance. The experiment's setter budget was not exhausted.
  Native producer dataflow budgets source frames as the ceiling of requested
  output frames times the larger of the current and target ratios. It therefore
  supports source-step ratio direction, rather than treating the request as
  neutral. Stateful non-unit resampling and its source/output accounting remain
  unimplemented. The experiment and its temporary test modes have been retired;
  the restored source and rebuilt executables match the clean capacity repair.
  The experiment is not a production admission or native-equivalence claim;
  consumer completion, input lifetime, audio output, and shared-title acceptance
  remain open. Executable validation is Linux-only.
  Unsupported-operation failures now reach the existing operation-specific
  diagnostic instead of an earlier generic unimplemented assertion.
- AudioOut2 currently snapshots a non-main port's PCM but omits it during
  context submission, leaving that PCM unconsumed. This is an identified host
  routing gap, not yet a verified explanation of a particular title's audio.
  Object type and attribute contracts, spatial mixing, rate alignment and
  consumption require native evidence before this route can be admitted.
  Object routing does not establish streaming append semantics. A bounded native
  argument capture now records an eight-channel float main port at 48 kHz with
  a 512-frame grain. Its attribute list contains the PCM pointer and eight
  float gains; the current one-entry admission rejects this two-entry list.
  Mono object ports also receive PCM, gain and four-byte attributes numbered
  five or eight. Their routing and metadata semantics remain unqualified.
  Two fresh strict shared-audio runs on the clean capacity repair stop in
  `SetGaplessDecode`. One request is now captured at both enqueue and rejection:
  initialized ATRAC9, instance flags 0x401, reset one, finite total 488448,
  priming 256 and zero prior-skipped frames. The Opus-only admission rejects it
  before parsing the trimming fields. A private codec-independent candidate is
  prepared but has not been applied, compiled or behaviorally validated. The
  second title's debugger stopped at a guest memory fault before audio; that
  stop does not establish a fatal emulator fault or its gapless profile.
- Direct pixel-resource and geometry-state packets now interpret the supplied
  count as values only, consistently with the outer parser. Focused packet
  cases pass. Pixel export analysis recognizes a wave32 EXEC-low clear before
  a null final color export and invalidates depth coverage proof on EXEC
  restoration. Waterfall analysis accounts for EXEC-high writes and distinguishes
  relative-source from relative-destination moves. Wide scalar buffer loads
  include the signed immediate offset with modular address arithmetic. Focused
  numerical/analysis regressions reproduced the affected behavior before the
  fixes and pass afterward. None establishes the cause of the menu glyph fault.

The remaining menu glyph investigation has a narrower verified frontier. The
affected draw samples a single-channel atlas whose entire CPU upload backing
contains `0xcd`; a valid live shader probe independently samples alpha
`205/255`. This explains solid glyph rectangles for that occurrence. A complete
retained tracked-write history has no overlapping DMA or immediate-write entry,
and the recorded owner is a CPU-upload texture. These observations do not cover
untracked native CPU stores or every possible GPU-only alias. A bounded hardware
access watch now links a verified native `0xcd` fill to a
CPU layout/scatter routine: its source load touches the watched filled range,
and its destination base matches the subsequently bound single-channel atlas.
The decoded load/store sequence confirms that sampled source-to-writer path,
not a complete whole-atlas write history. The earlier indirect content-preparation
method now resolves to an internal command-stream producer. Its live source is
a separate, entirely zero-filled scratch buffer; the observed receiver state
selects the stream path. A separate CPU-descriptor update is gated off in that
entry snapshot. The stream consumer, completed upload and latest texture owner
remain unresolved; the return probe did not complete, so no whole-update result
is inferred. HLE-only memory traces cannot exclude native guest writers because
exported guest symbols may take precedence. An earlier image-copy diagnostic
lacked transfer-source usage
and is excluded from evidence. One observed black transition remains intermittent.
A fresh strict run retained the colored menu through multiple no-input captures
and reached another visible panel after a bounded input hold. The glyph blocks
persisted in both states. This does not establish that the black transition is
fixed or that every input route is qualified. A later bounded command probe
observed the filled-source access but no command-producer entry. Its
thread-restricted breakpoint was removed during shutdown, and the capture
gate did not open. A process-wide follow-up accepted the producer on that
same thread, but its child-method entry probe did not fire. Neither run
demonstrates premature guest-thread exit or completed command publication;
the actual indirect parent call and its return need observation.
A further guarded probe confirms the parent call's register-derived target and
arms its thunk entry, but neither the thunk nor the child entry is observed
before bounded teardown. The later filled-source access still reads unchanged
bytes. No command-publication witness or qualifying capture is present, and
the debugger's client exit cannot qualify the unknown inferior termination.
Repeating an unobserved entry probe is not evidence of a guest-thread failure;
the next experiment must observe the call's actual next instruction directly.

One strict performance investigation stalled after two presents, with no
menu capture and no reported strict-breaker event. Its snapshots show completed
initial graphics work and no pending device-address preparation. A repeat
with temporary traces disabled advanced to an in-world scene, including one
unreadable dialogue panel, without a captured main menu. Neither run sent
runner input; effective physical or keyboard input was not observed. This
single pair does not establish that tracing caused the stall. A fresh
shader-performance-configured run reached a visually inspected menu
after an explicit bounded hold on the initial confirmation screen. Its stable
20.035-second window produced 96 presents (4.792 per second), below the required
threshold. It compiled no shaders or pipelines in that window. Draw state setup
and resource binding remained substantial CPU stages; their nested counters
cannot establish exclusive costs. The earlier below-threshold menu sample used
a different dirty binary and shader setting, so the small rate difference is not
a controlled improvement result. A separate host-sampling attempt stopped at two
presents before any menu or input. The profiler reports conflicting signal/timer
configuration, so its timing attribution is not accepted as a menu measurement.
Its samples nevertheless identify event-flag waits and condition-variable
broadcasts as a bounded investigation lead. A focused idle-waiter reproduction
confirmed that broadcasts after wait returns let unmatched waiters wake each
other without a guest state change. Rebuilding both source versions for the
same isolated fixture measured 537.518 ms of aggregate process CPU in a 250 ms
window before the change and 0.123 ms afterward. Wait returns now notify only
when cancellation drains the last sleeping waiter; call completion notifies
only when deletion drains the last active call. Explicit Set and cancellation
and deletion state-change broadcasts remain intact. The lifetime suite passed
eight cases with one unavailable native-busy fixture skip. This proves the
isolated wake-storm correction, not a menu-rate improvement or the cause of
every startup stall. A subsequent strict, visually reviewed menu window recorded
121 presents in 20.034 seconds (6.040 per second), still below the threshold.
The reset-bounded counters recorded 128,656 draws, 5.382 seconds of state setup
and 8.077 seconds of resource binding. Binding includes 2.888 seconds of storage
preparation and 2.670 seconds of texture preparation; these nested measurements
must not be added to their parent. The different binaries and independent runs
do not isolate the cause of the rate difference. Completed multi-workload
acceptance remains open.

## Unverified ISA leads from a fork comparison (2026-10-24)

- **Context.** A comparison against a derivative tree surfaced four general
  instruction contracts that our decoder still refuses. They are secondary
  implementation leads to verify against the published ISA and a live trace,
  not evidence-backed contracts, and none is the current platformer blocker.
  - `ds_cmpst_rtn_b32` (opcode 0x30): returns the pre-compare value and stores
    only on match.
  - 64-bit returning LDS forms (`ds_min_rtn_u64`, `ds_max_rtn_u64`,
    `ds_cmpst_rtn_b64`, opcodes around 0x68-0x70), including the 64-bit source
    width.
  - D16 XY/XYZ formatted buffer loads and stores (MUBUF forms 0x80-0x87).
  - Raw D16 depth bits through integer image loads (an integer view over a live
    depth target), which matches the open defect recorded for integer T# over
    depth binds.
  The same review confirmed that no existing derivative implements the
  level-aware linear R32F composite seed this title's tutorial needs; the
  mixed-view storage alias remains a local design task.
- **Lineage.** The reviewed derivative tree belongs to an unrelated project
  lineage: it uses different top-level paths (`src/graphics`, `src/kernel`) and
  shares no commit with our repository, so merging it is neither possible nor
  appropriate. Its synchronisations also mention two split-wave leads worth
  verifying later: sharing SMEM load results across emulated wave64 halves and
  allocating split-wave dispatcher spill IDs in program order. Both overlap
  files we own; treat them as leads, never as patches.
