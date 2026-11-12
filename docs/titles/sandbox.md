# Sandbox title

Title: PPSA17221 - Minecraft.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Sandbox title: boot pauses with no GPU work before the main menu (2026-11-11, open, intermittent)

- **Observation.** On one binary (the device-address storage reads and the queue wait fix, built 2026-11-11), four
  consecutive strict boots stopped presenting after about 17-32 s and stayed on the loading screen with the GPU idle
  for 30 to more than 95 s; the next four boots reached the main menu at about 20 s with no pause. During one pause
  the runtime status was `stalled` (present 287, 33 s since the last present), every guest thread but two sat in
  a condition, event-queue, semaphore or sleep wait, and the host CPU was nearly idle. No TCP connection was open
  (socket list at 10-40 s).
- **Not excluded.** The queue wait change of the same day, the storage reads by device address, and a guest wait
  on a host network call: `sync-waits` tracking was disabled in that run, so its empty list is not evidence.
- **Consequence.** A route with fixed press times misses the menu after a pause. Runs now enter the world by
  presses that check each screen.
- **Direction.** One paused boot with condition-wait tracking on and the stall supervisor
  (`docs/devtools/runtime-stall-snapshot.md`), compared with a boot that did not pause.

## Sandbox title: vertex-front buffers read by device address (2026-11-11, built, command-processor time only)

Follow-up to the record below ("What is still copied"). The fused vertex front parses its resources without the
program code, so its read-only metadata V# had no load evidence, was copied for every draw and got a new
descriptor set every draw.

- **Measured cause (temporary counter on the set lookup, world run).** Vertex-stage sets were found in the cache
  for 8.6 to 9.1 % of lookups against 94 % for pixel and 98 % for compute. 98 % of the vertex misses were storage
  binding 0 from the metadata table with no flow proof, no dword evidence and no code.
- **Change.** After the front's resource parse, the front code alone proves the read: the scalar flow must
  resolve, every consumer still holding the V# must read it only with dword loads, and the same checks as other
  stages apply. A program with a continuation keeps every buffer on the descriptor path. A buffer that fails the
  proof keeps its parsed evidence.
- **Register reuse.** The four hottest world fronts read the metadata V# at s[8:11] with two scalar loads at
  entry, then load another V# from the vertex table into s[8:11] for a typed fetch, whose lowering this change
  leaves as it was (slot path). Consumers whose register no longer holds the bound V# are recorded per program as
  slot-only: the by-address read skips them and the list is part of the shader id. The front does not record them
  as runtime-descriptor consumers, because the vertex stage has no guest-address lowering for scalar loads. A
  slot-only consumer must never receive the bound row, which may hold a device address, so its register must have
  one write that reaches it on every path; when only some paths redefine it (a join clears the held-value bit
  too), its buffer keeps the descriptor path. Translator version 215.
- **Result.** With temporary counters on the set lookup, vertex sets were found in 99.97 % of lookups. One clean
  profiled world run (10 s, 1,299 command-processor samples, mostly on the death screen at 2,882 draws per frame
  against 5,452 in the earlier profile, so per-draw figures only): descriptor binding 1.70 us per draw against
  3.44, the per-draw copy 0.05 against 0.76, indexed-draw recording 8.68 against 10.07. The front proof itself
  (the redefinition scan) costs 0.19 us per draw. No frame-rate gain is claimed from this run.
- **Verification.** Strict 125 s runs on the generic first-gameplay route, Intel A770 (ANV), a new private
  shader cache, 60 fps limit, binary built from 4ef52548c plus this change:
  - Exit 0, same results as the matrix for the record below: `PPSA28311 - Absolum`, `PPSA09076 - DREDGE`,
    `PPSA09809 - OCTOPATH TRAVELER II`, `PPSA21402 - Balatro`, `PPSA21567 - Astro Bot` (space intro capture
    correct), `PPSA34971 - Hollow Knight: Voidheart Edition` and `PPSA17221 - Minecraft` (menu panorama
    correct). The `gfx_storage_frontier` events of OCTOPATH TRAVELER II and Hollow Knight have identical fields;
    Astro Bot's last event names another vertex-front metadata buffer (binding 2, user SGPR 16) of the same
    unknown-access class, which stays on the descriptor path.
  - The path is live: 736 of the 1,135 SPIR-V modules the matrix built on the A770 and 12 of 17 on RADV carry the
    by-address base, against 457 of 1,521 and 6 of 17 before this change.
  - The mechanical-fist platformer stops at the same cold-cache stall as before this branch.
  - `PPSA21402 - Balatro` with the validation layer loaded: exit 0. On the AMD integrated GPU (RADV): exit 0.
  - `kyty_agent playable-regression` exits 3 as before this branch.
  - 2,648 unit tests and the four repository gates pass.
- **Every-path check, verified.** `EmulatorShaderGetpc.SlotOnlyConsumerNeedsTheRegisterReloadedOnEveryPath`
  builds two fronts by hand: a straight-line reload makes the consumer slot-only, a reload on one branch refuses
  the buffer. A world run with a temporary probe hashed a sample of the guest ranges draws read at bind
  (253,199 sampled, 253,197 checked) and again when the consuming submission completed, before its labels were
  published: 0 changed, and 62 of 62 hand-built changes were reported. Reading the guest pages in place is sound
  under the current submit; an early-returning submit lets the guest run sooner and needs this check again.
- **Frame split (same 10 s world window, 214 frames, 45.7 ms each).** The command processor runs 33.0 ms per
  frame, waits 1.15 ms on fences and under 0.1 ms on acquire, present and flips; the other ~11.5 ms it idles
  until the guest submits again (two runs per frame). The render engine was 59 % busy at 800-1,250 MHz of a
  2,400 MHz maximum, so the GPU is not the limit. Overlap with the guest gains nothing in this title (see
  "Rechecked with the early return engaged" below): the limit is command-processor time per draw, about 11 us.
- **Open.** The front proof runs on every draw; its verdict could be kept on the program, keyed only on code
  facts. The per-draw cost (shader input parsing, pipeline lookup, command emission) is the limit.
- **Draw-plan repeat rate (2026-11-11, one world run, `5f15a2b16` plus a temporary counter at the indexed
  draw's pipeline lookup, 1.8 million draws).** Only 169 distinct keys (vertex and pixel program address and
  checksum, render-pass compatibility, topology). A key seen before gave the same vertex and pixel shader ids
  as its last draw in 99.97 % of hits and the same pipeline in 99.83 %. Keyed on the program pair alone the
  ids repeat in 99.92 %. The misses show the key lacks descriptor shape (ids) and fixed-function state
  (pipeline), so a plan that skips input parsing and the id and pipeline lookups needs guards on both.
  Input information and pipeline setup are about 2.7 us of the 11 us per draw; resource binding, 3.1 us,
  is the next block. All 488 id misses were counted before the world loaded; in the world the ids repeated
  on every keyed hit.
- **Per-draw price (2026-11-12, world, `b5f22b2a2` and `0be0c39a2`, emulator stage counters over 72,933
  draws plus temporary timers on 1 draw in 16).** 11.36 us per draw: state setup 3.79 (vertex input 1.12,
  pixel input 0.80, guest-address read preparation 0.72, before the vertex input 0.55, index range and
  stream preparation 0.46), resource binding 3.16 (vertex buffers 1.04, descriptor finalize 0.54, textures
  0.50, storage 0.47, index buffer 0.23), command emission 1.39, pipeline setup 1.34 (shader ids 0.53, pipeline
  lookup 0.20), materialization 0.48, unattributed about 1.2. 2,805 draws a frame make 31.9 ms of the 44.8 ms
  frame. `0be0c39a2` keeps each thread's recent vertex programs (0 disagreements in 28,672 sampled comparisons
  with the full lookup) and visits only the load instructions in the per-draw guest-memory checks: setup fell
  from 4.25 to 3.95 us, about 0.8 ms a frame. Without overlap, 60 fps needs about 1.3 us per draw. Overlap is
  bounded by the submit contract: submit returns only after the batch is decoded, because the title rewrites
  its descriptor tables after submit, so the decode cannot run behind the guest. Priced against that contract,
  a draw plan (worth about 1.5 us: pixel inputs already use a cached analysis and vertex inputs are mostly the
  descriptor gather) and a two-stage command processor together reach about 31-36 fps at this load, not 60:
  with full overlap the frame is the command processor's 31.9 ms, 28 ms with the plan.
  (Corrects the 2026-11-12 estimate of "overlap plus 5.9 us per draw".)
- **Write notification cost on the submitting thread (2026-11-12, world, perf at 199 Hz).** Guest
  `memcpy`/`memset`/`memmove` notify the dirty-page tracker of every write, about a million times a second, and
  almost all of them land on single pages the tracker does not hold (0.16% found, temporary counters). On
  `0be0c39a2` the tracker held 150 of the 347 samples of the submitting thread (43%). A 16 KiB bit map with one
  bit per 64-page block, set before a page identity is published and never cleared, now answers an untracked
  page without probing the 2M-slot page table: 108 of 316 samples (34%). Two finer 1 MiB per-page maps, hashed
  and direct, measured 103 of 331 and 107 of 337, so the lookup is no longer what costs. A shift and mask in
  place of the page divisions removed `PageStart` from the profile, but the total stayed at 104 of 357 (29%):
  about 50 ns per notification moves between the frames of the call path. The change is about 2 ms/s of CPU on
  the submitting thread (one baseline run, sampling noise about 16 samples); frame rate did not move measurably
  (14.2 against 14.3 fps at the stop point). Not checked: how full the map gets over a long session, since its
  bits are never cleared while this title maps and releases direct memory every frame.
- **Host-GPU wait inside the release drain, excluded as a frame lever (2026-11-12, world, temporary timers in
  `Gpu::WaitLocked`, serial submit).** In 18 s of world the drains ran 33 times a second: 25.7 ms/s waiting
  for the rings to finish decoding and 6.6 ms/s in `CommandProcessor::SubmitAndWait` for the host GPU, under
  0.5 ms a frame. (Before the world: 164 drains a second, 231 and 31 ms/s.) Skipping the host wait for
  releases that overlap nothing is therefore not worth its risk; it would also not be sound as the code
  stands: `GpuMemory::Free` runs the global `WriteBackAllCompleted` and the device-address release runs
  `DestroyRetiredTables` (`GuestDeviceAddress.cpp:1546`), which frees retired Vulkan tables with no reader
  check, on every release, both relying on the host GPU being idle. The drain matters only for an early
  submit return, where it puts the batch back on the guest (see the holder record above).
- **Vertex-buffer binding repeat, excluded (2026-11-12, world, temporary counters in `BindVertexBuffers`).** Of
  the last 1,000,000 vertex streams bound, 97 % were fetched from imported guest pages; only 1.8 % had the same
  address, size and read start as the same slot of the previous draw (0.9 % in the object path), because each
  draw reads a different window of its pool. A bind cache keyed on the previous draw cannot remove the 1.04 us.
- **Open, checked against the RDNA2 ISA (no title trace yet, behaviour unchanged).** The ISA ignores the two low
  bits of V#.base and of the final address for scalar loads and for untyped dword buffer loads. Kyty refuses an
  unaligned base for the in-place path (`StorageGuestPagesAddress`, `GraphicsRenderBind.cpp:3051`) and the copy
  path then hands `WriteStorageAddressRow` (`GraphicsRenderBind.cpp:3062`) the copy's address plus the guest
  offset. An in-place object keeps the low bits in that offset, which stops the emulator; a snapshot copy may
  instead start at the unaligned guest byte, which reads bytes shifted from the ISA's. Direction: align the base down before both
  paths, as the shader already masks the offset (`EmitAddressScalarBufferLoad`, `ShaderSpirvBuffer.cpp:3645`).
  The ISA also leaves the destination of out-of-range scalar-load dwords unwritten, where both paths return zero.

## Sandbox title: storage buffers read by device address (2026-11-11, built, command-processor time only)

Second slice of the rework ("What comes next" in the record below). A read-only storage buffer whose every
consumer is a dword load is read by the shader through a device address held in its V# row, instead of a
descriptor-set binding to a per-draw copy.

- **Decision, at translation.** A buffer is read by address only if it is read-only, raw, bound from a user-data
  or table register, every instruction that consumes its V# is `BUFFER_LOAD_DWORD[x2..x4]` or
  `S_BUFFER_LOAD_DWORD[x2..x16]`, it is not loaded from a dynamic scalar address, not indirect, not a stream
  row read by a typed load, not extended, and the device has buffer device addresses. Any other buffer keeps the
  descriptor path. The program's scalar flow must be resolved, and every consumer whose register still holds
  the bound V#'s word 0 must also hold words 1 and 2 on every path; a consumer whose word 0 was redefined reads
  its own V# and keeps its path. The per-buffer decision is part of the shader id (translator version 214).
  The address emitters choose by the V# register; an emitter that would resolve a stream of a buffer marked for
  address reads stops translation.
  - Found on the way: the storage compaction that drops unused metadata buffers did not move the dword-load
    evidence with the buffer it belongs to, so a compacted buffer could carry another one's. It moves it now.
  - Found by the first matrix: `PPSA21567 - Astro Bot` stopped at translation on a `BUFFER_STORE_DWORD` whose
    register had been redefined, which the register lookup had matched to a read-only buffer; and on the AMD
    driver a device address in the upper canonical half (`0xffff8001...`) did not fit 48 bits. The emitters now
    skip redefined and dynamic consumers, the row holds the low 48 bits, and the shader sign-extends bit 47.
- **Row contract.** The CPU writes the 48-bit device address into words 0 and 1 of the row (stride bits kept)
  and the bytes the address backs into word 2. A word loads only if its byte offset plus 4 fits in word 2;
  otherwise it reads zero, as robust descriptor access does. An address that does not fit 48 bits or is not
  dword aligned stops the run.
- **Address source.** A range inside one imported run of guest memory is read in place at the import's device
  address. Every other range keeps its copy (snapshot pool, buffer object or scalar-memory copy), created with the
  device-address usage, and is read at that copy's address. The descriptor slot then holds one fixed zero
  carrier, so set contents repeat. In-place reads rely on the same quiesce as the import windows: imports are
  destroyed only while every submitted batch has completed (`Gpu::RunQuiesced`). That a replaced range keeps its
  imports alive is recorded in `docs/bringup/cross-title.md`.
- **Engagement (temporary counter, world run, 3,943 draws per frame, measured before the proven-flow and
  redefined-row guards below narrowed eligibility).** Read-only buffers: 2,471,421 in place,
  2,635 snapshot copies, 57 buffer objects, 5,325,887 kept on the descriptor path; 172,463 writable.
- **Price (one profiled world run, 10 s, 1,324 command-processor samples, a lighter view than the 5,012-draw
  profile it is compared with, so per-draw figures only).** Storage preparation 1.18 us per draw against 1.86,
  its copy path 0.75 against 1.24, descriptor lookup 1.02 against 1.43, the indexed-draw recording 9.27 against
  9.98. This is command-processor time inside the spread between runs; no frame-rate gain is claimed.
- **What is still copied.** The copies left under storage preparation take 2.94 ms per frame in that profile
  (93 samples), the ceiling for every refusal class together. In a run that stayed outside the world the
  largest refused class was table V#s of the fused vertex front with no instruction evidence: that stage parses
  its resources without the program code on purpose (its general analysis zeroed a reused constant buffer), so
  a later slice must derive load evidence separately and refuse any V# register defined more than once. Its
  share in the world is not counted. Buffers loaded from a dynamic scalar address stay excluded.
- **Verification.** Strict 115 s runs on the generic first-gameplay routes, Intel A770 (ANV), private shader
  caches, 60 fps limit, translator 214:
  - Exit 0 and no error event new to this change: `PPSA21567 - Astro Bot` (interactive at frame 2548, intro
    capture correct), `PPSA17221 - Minecraft` (world, interactive at frame 6341), `PPSA21402 - Balatro`,
    `PPSA09076 - DREDGE`, `PPSA28311 - Absolum`, `PPSA09809 - OCTOPATH TRAVELER II` and
    `PPSA34971 - Hollow Knight: Voidheart Edition`. The `gfx_storage_frontier` events of Astro Bot, OCTOPATH
    TRAVELER II and Hollow Knight also appear in runs before this branch (Astro Bot's with identical fields on a
    2026-11-09 binary).
  - The path is live on the final translator: 457 of the 1,521 SPIR-V modules the matrix built on the A770 and 6
    of 17 on RADV carry the by-address base (1,549 and 32 sites); a cache built with the reads switched off has
    none.
  - `PPSA21402 - Balatro` on the AMD integrated GPU (RADV): exit 0. RADV hands out upper-half device addresses,
    which the sign extension of bit 47 is for.
  - `PPSA21402 - Balatro` with the validation layer: exit 0, no report, after the queue wait-idle fix
    (`docs/bringup/cross-title.md`); before it, 2 of 3 runs stopped on that unrelated race.
  - The mechanical-fist platformer reaches gameplay (frame 1529) on a warm cache; its cold-cache stall at frame
    665 predates this change (`docs/titles/mechanical-fist-platformer.md`).
  - `kyty_agent playable-regression` fails the same gates as a run on 2026-11-09, before this branch, including
    the intermittent `input_sequence_complete` / `input_delivered` tap failure.
  - 2,648 unit tests and the four repository gates pass.
- **Open.** The snapshot and buffer-object address paths ran in this title only (2,692 binds) and not under the
  validation layer. The early-returning submit is still last.

## Sandbox title: vertex streams fetched from the imported guest pages (2026-11-11, built, frame time unchanged)

First slice of the rework of how draws reach guest memory (stages in "What comes next" below). It removes the
per-stream copy for vertex data; it does not change the frame time of this title by itself.

- **Change.** A vertex stream whose guest range lies inside one imported run of direct memory, and over which no
  live GPU object may hold a result the guest bytes do not show, is bound from the import buffer at the guest
  offset. No copy, snapshot lookup or compare runs for it. Every other stream keeps the copy path: a stream that
  fetches past its records (the copy provides the zero tail), a range outside one imported run, and a range that
  shares a page with such an object.
  - Guard: a per-page count of the live objects that may hold a host result. Surfaces always count; storage,
    vertex and index buffers count only while writable and in use, because their write-back copies the result
    to guest memory before the use clears, under the same lock. The count changes only where an object's state
    changes (creation, free, heap removal and the one function every `in_use` / `read_only` change calls).
  - Lookup: the chunk last resolved for each 64 KiB of guest memory answers without the registry lock, and
    holds while no chunk has left the registry. No buffer is created per range and nothing is imported on demand.
  - Device gate: the import buffers get the vertex usage only when every import handle type the device uses
    reports itself importable with that usage; otherwise nothing is bound in place.
- **Premise, measured first (hazard probe, temporary).** A probe hashed the guest bytes of every vertex, index
  and storage range a draw bound, and hashed them again when the draw's submission completed. No range changed:
  `PPSA17221 - Minecraft` 166,425 ranges checked, `PPSA21402 - Balatro` 462,256, `PPSA09076 - DREDGE` 1,879,841.
  A hand-made write between the two hashes was reported every time (41 of 41, 113 of 113, 459 of 459). This was
  measured with the serial submit, where the guest thread waits for the submission; a submit that returns early
  must repeat the probe before it relies on the result.
- **Guard checked against the slow walk (temporary census, always-on run).** For one in 16 streams bound in
  place the page count was compared, under one lock, with a walk of every object overlapping the stream's pages:
  180,311 checked in the world run, none where the walk found an object the count lacked. For one in 16 refusals
  the count's answer was replaced by "none" and the walk had to find the object: 16,344 of 16,344.
- **Engagement (same census).** World run, 5,261 draws per flip: 98.8 % of streams bound in place, 1.2 % refused
  by the page count (all of them writable storage in use), 427 streams with a tail fetch, none unresolved. The
  chunk lookup took the registry lock for 36,219 of 857,046 streams. Other titles and the second GPU are in
  `docs/bringup/cross-title.md` ("Vertex streams from guest pages: engagement by title and GPU").
- **Measurement (Intel A770, Mesa 26.2.6, strict, 60 fps limit, world route from one save copy, one always-on and
  one always-off run of the same binary).** Time the submitting thread holds a submission, matched by vertex
  binds per submission in 1 s buckets: 5,200-5,600 binds 79.98 ms on (n=16) against 82.71 off (n=8); 5,600-6,200
  82.99 (n=20) against 87.42 (n=15). That is 3-4 ms of command-processor time from one pair of runs with an
  unpinned GPU clock (median 850 against 750 MHz, render busy 40-43 % in both). World frame median 102.3 ms on,
  102.7 ms off; a third always-on run gave 94.8 ms (p10 87.1, p90 103.1), so the spread between runs (about 8 ms)
  is larger than the gain. This is not an fps gain.
- **Images.** The always-on and the always-off capture each show a correct night world with intact terrain,
  HUD and text. They are different views (the route ends wherever the night's mobs leave the player), so the
  material scores are not comparable and neither passes the gate on a dark scene (70 and 77 quantized colors).
  One always-on run ended on the death screen with the letters missing that the record below lists as open;
  that defect predates this change.
- **Build without the census.** One world run: exit 0, world from 103 s, frame median 102.8 ms (p10 93.8,
  p90 125.5) at 5,256 draws per flip; the capture shows the forest with intact terrain, HUD and text. That run
  predates two later edits, line wraps and the write-back fix's `in_use` condition, neither on the bind path.
  The final build passed 351 tests of the graphics-state and memory-manager suites and the three repository
  gates.
- **Vulkan validity.** Every chunk the lookup can return gets its buffer from the one import site that adds the
  vertex usage. One 63 s run of `PPSA21402 - Balatro` on the final build with the validation layer loaded
  (errors stop the run) ended with exit 0 and no report; the census run of that title bound 67 % of its
  streams in place.
- **Draw-to-draw repetition (temporary census, last 40 s of the world run, 2,254,171 indexed draws, each compared
  with the previous indexed draw on its thread).** Same pipeline 97.7 %, same framebuffer 98.7 %, same textures
  and samplers 91.7 %, same storage descriptors 0.0 % (2.1 % when the base address is ignored: sizes and record
  counts also differ), same direct register values and table pointer 1.7 %, same table bytes 98.2 %; 25.3 bytes of
  guest tables per draw. Nearly every draw keeps its pipeline, target and textures and changes only its storage
  descriptors, register values and vertex and index ranges.
- **What comes next.** (1) constants the same way; (2) buffers out of the descriptor sets, since the set key
  has the range and not the offset, which decides between the per-shader recipe of
  `docs/bringup/cp-draw-plan.md` and a path that records only what changed since the previous draw; (3) the
  submit that returns before the host is done, last, with its own review and the hazard probe repeated.
- **Prepare step (same day, built).** Per stream, the prepare step asked the memory manager whether writable
  storage overlaps the stream: two locks and a block search, 0.71 us per draw in the profile of the build
  above. Every object that search accepts is counted on its pages, so the search now returns at once for a
  range without a counted page. One profiled world run of the new build (10 s, 133 frames, 3,128 draws per
  frame, 75.0 ms per frame, a lighter view than the 5,200-draw one above): that step 0.08 us per draw, the
  page query 0.05, the address lookup 0.07, the vertex bind 0.36. The indexed-draw recording stays at 13.3 us
  per draw; its largest draw-input part is now the index copy path, 1.55 us per draw, and the copy pool under
  index and storage binds together takes 3.06 us per draw (compare 1.04, capture 0.64, slot lookup 0.34).
- **Index buffers (same day, built).** An index range inside one imported run, aligned to its element size in
  the import buffer, at most 16 MiB and on pages without a counted object is bound from the import buffer at the
  guest offset; the draw's vertex range comes from one scan of the same guest memory. Imports carry the index
  usage beside the vertex usage under the one per-device capability query. The transport draw and every refused
  range keep the snapshot. The scan and the GPU's read are two reads of guest memory: the premise above (no bound
  range changed before completion) covers them, and robust buffer access, which device selection requires,
  bounds the fetch of a copied stream if an index did change.
  - Pricing (temporary counter, world half of one run, 1,310,720 indexed draws): 3,535 index bytes per draw, all
    16-bit, none indirect; 99.9 % eligible, 0.09 % on a counted page, none unresolved or misaligned.
  - One profiled world run of the build (10 s, 130 frames, 4,069 draws per frame, 76.7 ms per frame): index side
    0.43 us per draw (scan 0.37; the address lookup and the page query total 0.08 each with the streams) against
    1.71 in the profile above (copy path 1.55, snapshot range 0.16), with no sample left in the index copy path.
    The indexed-draw recording is 11.4 us per draw against 13.3. The two profiles are different views, so the
    frame times are not compared: this is command-processor time, about 5 ms per frame at that draw count and
    inside the spread between runs, not a frame-rate gain by itself.
  - Verification: the world capture shows terrain, plants, HUD and text. `PPSA21402 - Balatro` with the
    validation layer mapped in the process, 70 s, exit 0, no report. `PPSA09076 - DREDGE`, 100 s, exit 0,
    2,757,586 of 2,768,896 indexed draws in place, town scene correct. `PPSA21402 - Balatro` on the AMD
    integrated GPU, 70 s, exit 0, 425,983 of 425,984 in place, table and cards correct. The two counted runs
    used the same code plus a temporary counter.
- **Open.** A debug check that re-hashes in-place ranges at completion is not shipped. Ranges outside an
  imported run are not imported on demand. On the AMD driver the decision rests on the reported capability and
  one title; no GPU time was compared there. Two consecutive runs of one build (2026-11-11) stayed on a menu
  at 43 draws per frame with no world preload in the title's log, and the scripted route never entered the
  world; the next six runs of the same binary showed the usual 51 draws per frame and entered. Cause not found,
  and no capture of that screen exists; the run script now stops and captures when the world is not reached.

## Sandbox title: frame load and command-processor ceiling in the world (2026-11-10, measured)

- **Observation (Intel A770, Mesa 26.2.6, strict, 60 fps present limit, world route, build of
  `562d7f7a4`).** In the world the title runs at 8-10 fps. Each frame carries 5,100-6,300 draws at
  15.8 us of draw processing each, about 126 submits, and the command-processor thread is running
  about 80% of the time (75% user, 5% kernel, from `/proc` per-thread counters). Throughput is about
  60k draws per second, so 60 fps at this load needs about 3 us per draw.
- **Frame structure (temporary census of every indexed draw, three frames at world entry, about
  3,100-3,200 indexed draws each; steady frames have more):**
  - 1,177 distinct meshes (index address, count, vertex offset) per frame and no exact repeat.
  - 204 meshes are drawn in four passes on four target pairs. The depth-only shadow-cascade target
    takes 1,348 draws, up to four per mesh.
  - 53 shader and target states per frame in runs of 50 draws on average (360 at most); user data
    changes on every draw.
- **The load is the title's own choice under Kyty.** Every save root, including fresh games-matrix
  roots nobody configured, holds `graphics_mode:2`, `deferred_viewdistance:20` and
  `gfx_viewdistance:576`. What value 2 selects was not confirmed from a published source, but the
  publisher states that its deferred mode (Vibrant Visuals) is the default graphics mode on
  compatible devices, PS5 among them (https://www.minecraft.net/en-us/vibrant-visuals-update), so a
  console defaults to the same heavy mode.
- **Reported direct memory does not choose the load (excluded).** Kyty reports 16 GiB of direct
  memory to PS5 titles. A temporary override reporting 13.5 GiB on a fresh save root: the title read
  the direct-memory size, never called the available-size query, and wrote the same six options.
- **GPU clock.** The render engine is busy about 42% of the time at 600-700 MHz, the device minimum
  (maximum 2,400 MHz), which is about 50 ms of GPU time per frame at that clock. The GPU time at a
  higher clock, and what keeps the clock at its minimum, are open.
- **Measured ceilings (`perf` user-time profile of the command-processor thread, 10 s in the world):**
  - Work that depends only on context state, plus per-draw shader input re-analysis, is 14.3% of the
    thread (8.7% and 6.4%). A fast path for draws with unchanged state was therefore not built.
    Input analysis follows user data, which changes on every draw, so state equality cannot skip it
    (the per-shader recipe in `docs/bringup/cp-draw-plan.md` is excluded). The context-only part is
    about 4-6 ms per frame in pieces of at most about 2% (pipeline lookup 2.2%, target materialization
    2.0%, viewport and color description 0.8% each, framebuffer lookup 0.7%).
  - The read-only snapshot family (capture, compare, upload) is in about 17% of samples.
  - `GpuMemory` frames are in at least 30% of samples (call stacks are truncated). The per-stage bound
    on recording draws on several threads is in "Parallel recording ceiling" below.
- **Read-only binds (temporary census, two runs, 30 steady flips each, about 5,200 draws per frame).**
  - Per flip: about 5,000 index, 5,000 vertex and 9,600 other read-only probes over about 12,000
    distinct ranges in about 90 2-MiB windows of imported guest memory. Every probe lies in one
    physical run inside a 64 MiB window, so that condition does not separate them.
  - Cost per frame: index snapshots 5.7 ms (99.5% reused), other snapshots 8.2 ms (21% reused),
    vertex binding about 12.6 ms (2.4 us per draw), together about 26 ms of the 104 ms the command
    processor runs per frame.
  - 99.8% of vertex snapshots are refused (this census; a count on 2026-11-11, on a later revision
    and with the cause of the difference not identified, found 25% accepted, reuse included, and
    every refusal made by a writable storage buffer over the range: the ledger below). The bind
    then uses a host copy of the guest bytes (94%) or a storage object already bound in place (6%);
    only 750 of 140,530 host-copy binds had device results not yet in guest memory.
  - 54% of index addresses are only 2-byte aligned, which a per-object in-place buffer cannot hold;
    one buffer per import window bound at an offset can. Other probes are all 16-byte aligned and the
    device storage offset limit is 4.
  - Snapshots live in host-visible cached memory, as the imported guest pages do, so reading in place
    does not move device reads out of device-local memory.
  - Reading in place sees bytes at host execution, a snapshot sees them at recording. End-of-pipe
    label writes are recorded as events set at the bottom of the pipe, so they fire only after the
    work recorded before them executes. The other ways the guest can signal reuse were not read, so
    whether a reuse signal can fire before the reading submission executes is not established.
  - Rewrites before completion, counted (2026-11-11, one world run with a temporary count, 93 frames
    in the window): one in eight snapshot binds hashed the guest bytes it reads at capture and again
    just before its submission's labels were published. Of 108,235 storage and 58,134 index binds
    checked, none had changed; a host array written by hand between the two hashes reported a change
    41 times of 41. Vertex binds were not covered (56 checked: they mostly bind GPU memory objects,
    not snapshots). One title and one scene. Exposure after the submit returns was not separated
    from exposure while the submitting thread was blocked, so this supports reading in place only
    under the decode-wait contract, not with submit overlap.
  - In-place storage and index binds, priced on paper (2026-11-11, the call-graph run of the B
    ledger below, split by caller) and closed unbuilt: at most about 5 ms, under the 10 ms bar.
    - Storage, 7.6 ms under the bind: the copy itself is 0.8 ms; the guest-range checks inside the
      copy and compare calls, 3.4 ms (overlap admission 2.0, allocated range 1.4), stay. The copy,
      snapshot lookup and pool bookkeeping, about 3.9 ms, go; a window lookup per bind, about 7,600
      per frame at the 0.3-0.5 us a lookup costs here, adds 2.3-3.8 ms. Net 0.1-1.6 ms.
    - Index, 4.9 ms: almost all are reused, so there is no copy. The checks stay (2.8 ms); compare,
      lookup, lease and expiry, 1.7 ms, go; about 5,000 window lookups add 1.5-2.5 ms. Net about 0.
    - Sets: a buffer bound at an offset needs the offset in the set key (`unique_id` and range
      today), and window buffers lose the cache skip fresh snapshots take, so their sets are
      indexed and trimmed. The creation side, 3.4 ms, is the bound only if every miss then hit;
      whether the guest ranges repeat across frames was not counted.
    - Guard: none found in what was read (`StorageBuffer.cpp`, `MapBacking` and its update; the
      in-place branches of `GpuMemoryWriteback.cpp`, lines 761 and 948). An in-place object's device
      reads guest memory when it executes, so a read-only bind would rest on the open reuse-signal
      question above. In-place objects also skip the overlay check (line 761), which a read-only
      bind would have to keep.
  - Ceiling: about 1.25x alone. It also removes work done under the snapshot pool lock, which parallel
    recording needs.
  - Vertex slice checked against a 10 ms per frame bar (2026-11-10, code reading plus the same census;
    not built). Bind calls cost 14.2 ms per flip (2.85 us each); 2.5 ms of it is snapshot attempts that are
    refused. Admitting those snapshots is no lever: the vertex ranges total about 297 MB per flip (8.9 GB in
    30 flips) and a snapshot copies its range, while a vertex object re-copies only ranges its page
    tracker reports changed. Binding the imported window in place still needs the query for pending device
    results that `CopyPendingStorageIntoVertices` (`GraphicsRenderBind.cpp`) runs on every object bind, whose
    cost was not separated, so the saving is at most about 11.7 ms and not shown to clear the bar. Two
    correctness questions are open as well: a fetch past the stream's records reads neighbouring guest
    bytes instead of the zero tail the object path writes, and the reuse signals named above.
- **State setup split (temporary stage timers, one run, 1.4 million draws in the world, 4,945 draws
  per frame, 7.6 fps mean, 131 ms frames).** The timed span costs 5.2 us per draw (25.7 ms per frame) and no stage
  dominates. The standing counter reads 6.4 us per draw over the same run; the gap is not attributed.
  - Vertex input analysis 1.25 us; primitive plan and pixel input analysis 0.88 us. Both rebuild a
    multi-kilobyte input description on every draw.
  - Index snapshot and vertex range 1.20 us; vertex stream write-back check 0.71 us; device-address
    preparation 0.58 us.
  - Target description and sample checks 0.28 us; everything else 0.3 us together.
- **Per-draw cuts checked against a bar of 5% of the frame (2026-11-10 to 2026-11-11, code reading
  plus the counters of world runs; not built).** At 3,600 draws and 90 ms per frame the bar is about
  1.25 us per draw; in a walking window of 5,000 draws and 116 ms, about 1.16 us. No cut is shown
  above it, for the stages the counters split or for the time outside them (last two items). Two
  stages left uncounted were counted on 2026-11-11 (one world run with a temporary tag, 56 frames
  of 5,077 draws while the player walks): the snapshot count closes; the shader-input structure
  repeats (second run below), but a third run excludes the recipe at the fields the built slice
  guards.
  - Vertex binding: 2.2 us per draw (8.0 ms per frame) in a run with one temporary tag, 3.5 us in
    the run with stage timers and the read-only census. Per stream the object path takes the
    registry lock up to six times: the admission of a snapshot probe that is refused (94 of 158,141
    accepted in the census, about 0.5 us each; the refusal cause was not counted),
    `GpuMemory::FindStorageContaining`, `FindVertexContaining` (once or twice), `CreateObject` and
    `CollectStorageForGuestSeed`. The write-back check before it (`PrepareVertexStreamsForRead` calls
    `WriteBackStorageForGuestSeed` per stream, about 0.8 us per draw with timers) and the last call
    both walk the in-use writable storage over the draw's fetch window (`CollectWritableStorageIn`);
    the second walk found nothing for 99.6% of binds (636 of 145,718 did). In one world run with
    temporary timers (4,476 draws and 108 ms per frame) the write-back check took 2.64 ms per frame
    (0.59 us per call) and the bind's query 0.57 ms (0.15 us per call). The set was empty in 93% of
    scans and held at most 8 objects in 99.9%; `FindStorageContaining` took 1.11 ms per frame.
    Built (2026-11-11): the walk returns at once while the in-use writable set (`m_writable_storage`)
    is empty; the set holds every object the walk can return (only the storage-buffer class has that
    type, and its write-back is never null). Same timers, one world run at 5,191 draws per frame: 0.09
    and 0.04 us per call, about 3.1 ms per frame less; the set was empty in 93.6% of scans.
  - Shader input information: 2.1 us per draw with timers and no reuse path. The per-shader recipe is
    excluded (`docs/bringup/cp-draw-plan.md`, stage 2); reopening it needs a count of repeated input
    structure in this title. Counted so far, exact inputs only: over about 249,000 indexed draws the
    vertex stage's input (program address, stage and shader registers, and every descriptor word
    the parse reads through its reader, none for that stage) never matched the previous input or one
    of the last four of the same program (28 vertex programs). The pixel key included the vertex
    key, so its result (53 programs, no repeat) follows from the vertex one. A memo on exact inputs
    cannot hit. Structure, counted in a second run (about 304,000 calls per stage over 64 walking
    frames, pixel stage keyed on its own): with the user-data value words zeroed, the stage and shader
    registers matched one of the last four inputs of the same program in 100% of calls for both
    stages (99.7% and 100% the previous input; the program address alone, the control, 100%). Exact
    keys matched 0% (vertex) and 47.5% (pixel). The words that change are user-data values only:
    words 0, 4 and 6 of the vertex front's user data (every call, 99% and 99%), and words 16, 0, 28
    and 12 of the pixel stage's (23%, 21%, 9% and 6% of calls). The vertex parse read no descriptor
    words, the pixel parse 1.5 bytes per call; the vertex stage still reads guest memory, the
    attribute and buffer tables that two user-data address pairs point to. This is a ceiling: a
    recipe still needs guards on the user-data values and the guest memory it reads.
  - Recipe build gate (third run, 125 walking frames, about 604,000 calls per stage): the vertex
    stage takes 1.07 us, of which the usage parse is 0.15 us, and the pixel stage 0.78 us, parse
    0.49 us. A mock hit (program lookup, compare of the user-data values with the program's last
    ones, copy of the produced information) takes 0.24 and 0.19 us, so a free-guard recipe would save
    at most about 1.41 us per draw, above the bar; guarding the attribute and buffer tables it reads
    would lower that. The changing words are not all buffer bases or record counts, the only fields
    the built slice guards. Vertex word 0 and pixel words 12 and 28 hold buffer descriptor bases.
    Vertex words 4 and 6 are consistent with the low halves of two 64-bit guest addresses (words 5
    and 7 hold the same high half; the register indices the parse uses were not read), not a base
    and record count. Pixel words 0
    and 16 are image descriptor bases (type nibble 9), and for one program pixel word 2, the image
    size, changes too. Excluded at that field set. A recipe that also masks image bases and the two
    addresses would need guards on the image descriptor's control reads and on the null check of
    those addresses; not designed. Where the 0.92 us of the vertex stage outside the parse goes was
    not split.
  - Index snapshots: 1.17 us per probe in the census run, 99.5% reused, about 4.2 ms per frame (the
    binding counter reads 4.0 ms per frame in the run with one tag). A reuse takes the
    snapshot pool lock twice and the registry lock once and runs cached admission lookups. Snapshots
    under 4 KiB (`kMinTrackedSnapshotSize`) are compared with guest memory instead of tracked; the
    average index probe is 3.6 KB, and the share below the limit was not counted.
  - Other read-only snapshots: 1.7 us per draw. 79% of probes are captured, not reused: each is the
    first probe of its range in that frame (none was captured twice in one frame). Counted: all
    7,619 captures per frame were of an address, size and read start that the previous frame did not
    probe, none seen with equal or other bytes, and 86% found no pool entry at that base. Reused
    probes were 94% seen in the previous frame with equal bytes, which checks the classifier. Reuse
    from the previous frame cannot remove these captures.
  - Admission checks over every probe: 6.1-6.9 ms per frame. At the average rate, reuses account for
    about 2.2-2.4 ms of it (an estimate).
  - Dispatches: 2.5 ms per frame for 64-67 dispatches (about 38 us each) in two world runs, under the
    bar; the 18.8 us per draw above divides all command processing, dispatches included, by draws.
  - Remaining stages, ranked offline on the walking window of an earlier world run (151 frames of
    5,000 draws, 116 ms per frame, bar 5.8 ms): pipeline setup takes 5.0 ms per frame in all (lookup
    0.8 ms, every draw a hit). Command emission takes 6.25 ms around the Vulkan draw call, which a
    cut would have to remove almost whole. Descriptor-set finalize takes 7.4 ms; its set cache keys
    on buffer identity, 79% of the read-only probes it binds are captures (above), and the first
    capture of a snapshot key marks its buffer's sets uncached. Counted in the second run below: of
    1.69 set lookups per draw, 16% hit, 70% are uncached and 14% miss; the create path (allocation
    through write) takes 4.1 ms per frame (0.56 us per set), under the bar. The binding sub-counters
    sum to 7.24 us per draw against 6.84 us because the index counter adds the index upload, which
    is timed inside state setup.
  - Time in the draw processor outside its five stage timers: 1.73 us per draw in that window.
    Split in two world runs with temporary timers (2026-11-11, 83 and 134 walking frames of
    5,000-5,200 draws, 120-122 ms per frame, bar about 6.0 ms): the early-submission check made
    after each draw takes 1.50 us of the 1.80 us per draw outside the stages in the first run (the
    timers sum to 86% of it) and 1.33 us in the second. Retried draw attempts and their waits take
    0.01 us, and the span between pipeline setup and resource binding, fragment-transport bodies
    included, 0.02 us. In the second run the check takes 6.96 ms per frame, above the bar: 939
    completion polls at 2.8 us (2.6 ms; the fence-status call is 1.47 us of each) and 127
    submissions at 32 us (4.1 ms). 86% of polls found the oldest submission still pending, 500 us or
    more after the last submission; whether from GPU work or submission latency is not known.
    Removing the check is excluded in `docs/bringup/cross-title.md` (another title needs it). The
    largest cut short of that, no polls with the recorded one-per-millisecond limit, which about
    halves the submissions, is about 4.6 ms, under the bar. The submission cost is the open item
    there.
- **A free draw path does not reach 60 fps (2026-11-11, recorded runs plus two runs).** A graphics
  submit returns after its batch is decoded (below), so the guest's time between submits and the
  command processor's run of the batch are serial. In the walking window of 116 ms frames above,
  command processing takes 90 ms, draws 85.6 ms of it. In three world runs with temporary counters
  (130-139 ms frames, 5,016-5,158 draws per frame, one submit and one wait per frame) the
  command-processor thread waited 24-29 ms of each frame for the next batch. The gap grows with the
  draw load: 15.6 ms at 2,866 draws in the lighter run below, 26.4 ms at 5,000 here (5.4 and 5.3 us
  per draw). With draw processing free the frame would stay at about 26 + 4.4 = 31 ms (about 32
  fps). If the gap does not shrink, overlapping the two (the bound below) leaves it as the floor,
  24-29 ms (35-42 fps). 60 fps at this load needs all three: command processing under about 17 ms,
  the submit overlapped with it, and the gap under about 17 ms. One batch of overlap was measured below
  and did not shrink the gap; what holds the guest is not identified. In the lighter run below the
  submitting thread was on a CPU 11.5 ms of a 15.6 ms gap.
  - The submitting thread in the same three runs: 17.9-19.8 ms of each frame on a CPU (user
    16.7-18.6 ms), 0.1-1.1 ms waiting to run. A fourth run sampled its user time for 10 s (75
    frames of 134 ms, 4,000-5,900 draws per frame, 19.7 ms on a CPU per frame): game code 23% (4.2
    ms), emulator code 52% (9.5 ms), host C library 24% (4.5 ms). The largest emulator item is the
    host-write notification that the C-string handlers (and one memory-control handler) make for
    guest writes: six symbols grouped by name, 36% (6.5 ms, 4.9% of the frame; the capture has no
    call chains). The notifier copies its callback table under a global lock on every call, then
    the dirty-page lookups run. Host mutex calls are 16% (2.9 ms); their callers were not sampled,
    and on the other guest threads mutex samples outnumber notifier samples five to eight times.
    That thread is off a CPU for 6-10 ms of the gap; what it waits on is not split.
  - Fixed, the notifier lock (2026-11-11). A counted run (its counters slowed the frame from 134
    to 154 ms, so only its counts are used) gave about 602,000 notifications per frame on the
    thread(s) it flagged, 41 bytes each, and 182,000 on the other threads; 99.4% of the page lookups
    end at an empty slot (3,467 hits per frame, no table walk). The notifier now reads its one
    callback pointer without the lock; installation keeps it. In one sampled world run afterwards
    (85 frames of 118 ms, about 5,100 draws per frame) the submitting thread spent 12.3 ms of each
    frame in user code against 16.7-18.6 ms in the four runs before, and host mutex calls plus the
    notifier fell from 25% to 4.9% of its samples, consistent with the lock being most of those
    mutex calls. About 4 ms of the drop is attributed (mutex calls 2.9 to 0.5 ms, the notifier 1.7
    to 0.1 ms); the other 2 ms is spread over game code, other emulator code and other C-library
    calls and the change does not explain it. The dirty-page part of the path is unchanged at
    4.7-4.9 ms per frame (4.0% of the frame, under the bar; closed, see `docs/bringup/cross-title.md`).
    No frame-rate gain is claimed: the gap varies by 5 ms between runs. The counters' 20 ms slowdown of
    thread moved the command processor's wait for the next batch from 24-29 to 47 ms per frame (one
    pair of runs), consistent with time on that thread landing in the gap.
  - A run with every draw skipped from boot (`KYTY_AB_SKIP_ALL_DRAWS`, a diagnostic switch) stayed
    at the 60 fps limit with 42-73 draw-processor calls and 22-35 dispatches per frame; world-level
    counts were not seen. The cause is not established, and it is not a floor.
  - A run that skipped the path behind the draw entry (preparation, recording and the GPU work it
    queues; decode, waits, labels, dispatches, flips and completion kept) in alternate 4 s phases
    once in the world (2026-11-11, one run, temporary probe, A770, 60 fps limit, +8 s route): the
    submit hold fell from 114-142 ms to 3-5 ms at 5,300-5,500 draws entered per flip, and from
    49-70 ms to about 2 ms at about 2,800 (16 intervals), with equal entered-draw counts in both
    phases. At about 2,800 draws per flip the skip phases sat at the 60 fps limit (gap about
    15 ms). The full-load number is void under the rule stated before the run (three intervals at
    world entry, under the four required); the load fell at the same point of the route in a run
    with nothing skipped (about 2,500 draws per flip). Those normal phases ran 143-167 ms per flip
    against 102-112 ms in clean runs (world entry, two foreign processes at about 30 % CPU), so
    their hold is not the steady one.
    - A second run held the skip for 16 s from world entry, with perf on every thread (10
      intervals at 5,200-5,800 draws entered per flip). Six calm intervals ran 18-22 ms per flip
      (19.6 ms over 307 flips: submit hold 3.1, time between submits 16.4) and three world-entry
      spikes 27-40 ms. In the calm intervals the submitting thread spent 13.0 ms of that time in
      user code, 3.6 ms in guest code and 9.4 ms in the emulator (about 4.9 ms of it the dirty-page
      notification under the C-library copy; an attribution, not a saving: rejecting 94.7 % of
      those calls recovered 0.7-1.35 ms), and 3.4 ms outside user code (kernel time and waits,
      unclassified). With the path behind the draw entry at zero the full-load frame is about
      20 ms, so 60 fps at this load also needs about 3 ms off the submitting thread. The skip
      removed the GPU work too, so this is not an fps figure for a faster draw path.
  - The path behind the draw entry priced by unit of work (2026-11-11, on paper from one
    full-stack profile at 5,017 draws per frame; ceilings, about +-1 ms; no run). For each part the
    question is how often its answer can change: per shader, per key per frame, per 2 MiB window per
    index change, or per draw. The range queries under the draw entry are three: object overlap
    (refused only by a buffer that is not all read-only, a surface not linked under the queried
    view, or a truncated snapshot; read-only vertex objects pass), which a per-window count of the
    refusing kinds could answer; allocated-range checks, already answered from the heap index except
    the allocated-prefix lookup; and block finds inside the vertex rows. The shader input
    information keeps its structure in every call once the user-data values are masked; the recipe
    the exclusion above left undesigned (image bases and the two table addresses masked too) needs
    guards on the image descriptor checks and on the attribute and buffer tables, whose bytes after
    masking were never counted. Buckets, as ceilings: exact by construction about 5 ms (pipeline
    ids, storage objects, textures) plus under 1 ms for the allocated-prefix lookup; exact once
    counted, object overlap about 4 ms; behind a dirty-page fence whose no-write cost is unpriced,
    in-place storage and index about 6 ms; with no guard evidence, in-place vertex about 9 ms; the
    masked recipe about 5 ms. The countable rows reach about 29-30 ms before the fence, under the
    30 ms build gate plus the noise; descriptor sets, targets and device-address preparation (up to
    about 7 ms more) cannot be counted. Closed under the gate, not built. Combined with
    parallel recording it stays closed too (`docs/bringup/parallel-recording-design.md`, "Decision").
    - Found, not fixed: `GpuMemory::GetAllocatedRangePrefix` (`GpuMemory.cpp:249`) memoizes a
      heap-index search through a range-query cache, and in the profile the cache's lookups and
      stores outweigh the search, as on the validation path before it was answered from the heap
      index. Direction: answer the prefix from the heap index under the same lock.
  - The small cuts in this record together: polls 2.6 ms, half the early submissions 2.0 ms, set
    creation 4.1 ms, the vertex walks 3.2 ms, index snapshots 4.2 ms and admission reuse 2.3 ms,
    about 18 ms of a 120 ms frame, or about 10 fps.
- **A lighter world frame is not a target (2026-11-11, offline reading, no run).** Another
  implementation's record of about 56 fps in a world was not measured at this load. Its own notes
  call that frame recognisable but not correct and no performance claim. A later entry there
  describes incomplete distant terrain at about 14 fps, with about 470 draws and 23-29 dispatches
  per frame at 60/121 us per draw (p50/p95). Here a world frame holds 3,600-4,500 draws (4,476
  draws and 60.8 dispatches in one run). The title's options file in that implementation's save
  directory has the same graphics mode (2) and view distance (576) as ours, but it looks freshly
  created (onboarding not dismissed), so it shows the defaults, not the settings of those runs. At
  18.8 us per draw, 470 draws would cost about 9 ms of command processing (command processing only;
  the time outside it at that load is not measured). No guest-visible answer was found to
  explain the gap: the title's executable imports neither the game-presets query nor the Neo-mode
  query (its bundled modules were searched without a control), and the system-parameter answers
  there are locale values with the same numbers except the language, which ours takes from the
  configuration. Excluded: taking that figure as a target at this
  load; it carries no recording mechanism to reuse, and lowering the title's load is out of scope.
- **Parallel recording ceiling (measured, not built).** The same run's counters, per frame of 105 ms
  of command processing, classed by whether the work depends on the order of draws:
  - Order-dependent, about 61 ms: the index snapshot, the vertex write-back check, device-address
    preparation and conflict scope, resource binding (it creates and registers memory objects and
    records their use per submission), target materialization, fence waits and dispatches.
  - Shared caches, about 13 ms: pipeline setup and descriptor-set finalization.
  - Independent of other draws, about 22 ms: register-only state setup and command emission.
  - Not attributed by any counter, about 10 ms.
  - The 61 ms include 12-16 ms per frame of host waits for GPU completion (13-23 waits per frame in
    six runs of the route; a seventh waited 1.3 ms per frame, not explained). These are figures from
    before `d2ff690bc` and `443032ed0`; the run verifying the latter waited 2.09 ms per frame. Every
    wait comes from a draw whose device-address preparation finds a storage result that has not
    reached guest memory: the draw waits for the submission that writes it and retries (`RetryGuestDeviceAddressDraw`,
    `GraphicsRun.cpp:2475`). The wait lies outside the draw-stage spans, so it is not counted twice.
    The waits do not follow power-limit throttle windows or other processes' GPU use. Temporary tags
    in three runs split them into two causes whose shares vary between runs:
    - 8-85%: the result may not be written over guest memory by the device (under 64 KiB and not
      64-byte aligned), and the overlay that would serve readers on this queue is refused at the
      16 MiB snapshot staging (two runs tagged it; in the one that told the cases apart, the staging
      was full).
    - 15-92%: the dirty-page tracker reported a change in the range since the device wrote it
      (`GpuMemory::StorageIsWholeGuestContent`, page-granular). The tagged ranges were 4-64 KiB and
      not page-aligned. In the run that tagged the cause, the one range waited on had not changed: a
      write-back on a shared page had marked it (below, fixed in `443032ed0`).
  - With four recording threads, command-processor time is bounded at 1.2x (only independent work in
    parallel) to 1.7x (caches, unattributed work and the double-count case in parallel too). Parallel
    recording was not built (design and bounds: `docs/bringup/parallel-recording-design.md`), and of the in-place read-only binds above only vertex streams were built (2026-11-11, first record of this file); storage and index binds stay measured, not built.
- **Snapshot staging was released only at end-of-pipe write-backs (superseded snapshots fixed in
  `d2ff690bc`; one change excluded).** Before the fix, overlay snapshots freed their staging only in `GpuMemory::PublishStorageSnapshotsLocked`
  (`Objects/GpuMemoryWriteback.cpp:420`). It is reached only from `WriteBackCompletedSubmission`,
  which only the end-of-pipe callbacks call (`GraphicsRenderEop.cpp:312`, `:339`, `:419`, `:527`);
  the general completion path (`CommandProcessor::PublishCompletedSubmissions`, `GraphicsRun.cpp:694`)
  only retires deferred deletions. Between end-of-pipe events the staging stayed held by snapshots of
  completed submissions, and later overlays were refused.
  - Excluded: on any refused overlay, publish the snapshots of completed submissions and retry the
    overlay once. One run with the change and one clean control on the same route and save, both on
    the death screen at night at 130 s: waits fell from 23.4 to 8.7 per frame (14.9 to 6.3 ms), but
    allocations rose from 3.1 to 134 MB per frame, stable-buffer creation from 1.8 to 77 MB, uploads
    from 44 to 170 MB and object creation from 7.5 to 28 ms per frame. Frames were 148 ms against
    143 ms at 4,686 against 4,038 draws per frame, so the frame effect is not established; the churn
    excludes the change. Why publishing earlier causes it is not established. The change was not kept.
  - Measured (two runs with temporary counters, same route and save): the staging level stays at
    the cap. At 16 MiB it read the cap at every allocation from about 10 s on, and a counter on the
    snapshot take (`GuestDeviceAddressTakeSnapshots`) stayed at zero for the 128 s run; the same
    counter fired on `PPSA21567 - ASTRO BOT` (below), so the take does not run here. With the cap
    raised to 64 MiB the level rose from boot at about 1.4-1.8 MiB/s, reached the cap at about 40 s
    and no later allocation read it lower; all 481 allocations in the world were refused, and fence
    waits read 16.3 ms per frame (12-16 ms at 16 MiB). Excluded: a larger cap only delays the full
    state.
  - Fixed (`d2ff690bc`): submission completion (`GpuMemory::CompleteSubmission`) frees the snapshots
    that their object's own write-back superseded, once their copy has finished. Publication and the
    pending-snapshot check already skipped those snapshots, so guest memory receives the same bytes;
    unlike the excluded change, nothing is published early. One run against the populated-page run
    (`67d6e9a95`), same route and save, with temporary counters:
    - Every pending snapshot here was superseded. With the fix the pending staging stayed under
      0.01 MiB, and on all 110 counter lines the bytes of pending snapshots equalled the cap minus
      the free space. In the world 594 snapshots were freed this way and none of 595 staging
      allocations was refused (500 of 500 in the world of the reference run).
    - Fence waits 11.8 against 13.0 ms per frame (13.6 against 18.3 waits), allocations 2.7 against
      2.2 MB per frame. One pair, no frame-rate claim. The expected share of about 7 ms was not
      reached. Three later world runs on the fix read 1.8, 11.8 and 5.8 ms per frame: the fix removes
      the refusals, and the fence time it leaves varies between runs with the waits attributed in the
      next item.
    - The release scan costs 0.04 us per completed submission (at most 32 pending snapshots).
    - `PPSA21567 - ASTRO BOT`, intro cinematic after a diagnostic tap, against a run of the
      `67d6e9a95` code with the same tap: the snapshot take ran (242,271 calls, 4,844 snapshots
      taken), 3,183 superseded snapshots were freed at completion, the staging sums matched on all
      144 lines and no staging allocation was refused. The capture showed the same cinematic shot
      as the control, the last error was the one the control reports, and allocations in the
      cinematic read 0.00 against 0.01 MB per frame.
- **The waits the staging fix leaves came from a page shared with another object's write-back
  (fixed in `443032ed0`).** Two runs on `a099c9097`, same route and save, with
  temporary tags at the wait site:
  - In the first, 4.36 of the 5.80 ms per frame of fence waits were waits the tracker attributed to
    a change in the range; time agreed with the draw-to-`WaitSubmission` backtraces (823 against
    789 ms). All of them came from one 30,720-byte storage range at a non-page-aligned address with
    three fully covered pages.
  - A content hash kept at each re-baseline that leaves the stored hash at zero matched the guest
    bytes in 150 of 150 tagged waits; one kept hash in sixteen was deliberately corrupted, and all
    13 of those read different. The object's bytes had not changed.
  - Only the partial last page had changed, and no guest-fault mark was seen on the object's pages
    since its baseline: the write-back of another storage range, 1 KiB starting 80 bytes after the
    object's end, notified its own bytes, and the tracker marks whole pages.
  - A notification on an armed page also restores write access (`NotifyPageWriteEntered`,
    `GpuDirtyPageTracker.cpp`), so a later guest write to that page leaves no mark until the next
    re-arm. A tracker-side refinement such as byte extents of notifications therefore cannot be
    sound; only comparing bytes can.
  - Probe (temporary, behind an environment variable): when the tracker reported a change, re-arm the
    range, compare the guest bytes with the kept hash and, on a stable match, take the range as whole
    guest content and re-baseline its generation. In the world it applied 339 times over 341 frames,
    and the corrupted hashes never passed (103 waited). Fence waits read 2.21 against 5.80 ms per
    frame in one pair, 0.69 ms of them the corrupted controls. The capture showed the death screen at
    125 s with the known glyph defect, as on the clean binary. One pair in different scenes, no
    frame-rate claim.
  - Fix (`443032ed0`): a storage write-back that re-baselines the range's generation also keeps
    hashes of the bytes on its partly covered first and last pages, in a field of their own (Update
    relies on a zero object hash to upload label holes), with the generation and content sequence
    they were taken at, and only when the read they were taken under is stable. When the tracker
    reports a change, `GpuMemory::ConfirmStorageEdgesUnchanged` re-arms the range, requires every fully
    covered page to be clean by page epoch (`GpuDirtyPageTracker::InteriorChangedSince`) and the two
    slices to match under a stable read, then re-baselines the generation. Anything else waits as
    before.
  - Verification, one world run on `443032ed0` with a temporary tag on each outcome and one kept
    pair in sixteen deliberately corrupted: in 35 s of the world (about 389 frames) the check passed
    490 times, all on one 30,720-byte range; there were 32 waits on corrupted pairs, and no uncorrupted
    pair read different or had a changed interior page. The 102 other waits there failed a
    precondition of the tracked case (`GpuMemory::StorageContentTracked`); this run did not split
    them. Fence waits read 2.09 ms per frame, memory allocation 1.7 MB per frame, and the capture
    showed the death screen with the known glyph defect, as on the clean binary. One run, no
    frame-rate claim. A first run missed the world on the route and was discarded. A later world run
    on the same source with temporary timers and no induced waits (4,476 draws per frame) read
    1.32 ms of fence waits per frame. In `PPSA21567 - ASTRO BOT` (intro cinematic, same tap) the
    check ran rarely: at least one pass on a 16-byte range, 48 stale pairs; last error, allocations
    (0.55 against 0.62 MB per frame) and draws matched the control (`2022c3607`, before
    `d2ff690bc`). Of four captures, the title screen and the planet shot matched the control's; the other two fell at other points of
    the cinematic, with no defect seen. Command-processor time read 35.8 ms per frame, inside the
    30.8-41.6 ms of four earlier runs on the same tap; the added work is two sub-page hashes and one
    tracker query per write-back record, about 13 per frame there.
  - Open: snapshot publication (`GpuMemoryWriteback.cpp:484`) keeps the page-granular gate; it runs
    only from end-of-pipe write-backs, which this title does not raise.
- **GPU estimate.** In the same run the render engine was busy 37% of 131 ms frames at 600-650 MHz,
  about 48 ms. If that time scaled with the clock it would be about 13 ms at 2,400 MHz; this is an
  estimate, not a measurement.
- **Skipping whole draws (2026-11-11, offline reading, no run).** Counters of one clean world run
  after the notifier fix (138 walking frames, 5,059 draws, 111 ms frames): 91.7 ms of command
  processing per frame (17.1 us per draw over 86.8 ms of draw processing) and the gap of about
  19 ms. 30 fps needs about 14 ms of command processing with the serial submit, or about 33 ms if
  the submit overlapped it, so a candidate has to remove at least half of the run (about 46 ms) at
  once. Grouped by what each stage follows, against the draw's counterpart in the previous frame
  (from the notes of Experiment 0 in `docs/bringup/cp-draw-plan.md`, 2026-10-27, whose record names
  no titles and whose run evidence is gone: in this title the draw key repeated against one of the
  previous two frames in 99.1% of draws, the user-data values never did); state-setup parts are the
  stage timers above scaled to this run:
  - Repeats, about 28 ms: index buffer binding 5.1 ms (99.5% of index snapshots reused), texture
    descriptors 2.3 ms (image and sampler words change on 1-2% of key hits), pipeline setup
    4.8 ms, target materialization 2.5 ms, and in state setup the shader input recipe's ceiling
    (1.41 us per draw, 7.1 ms, before guards), the index snapshot and vertex range and the target
    description (about 6.4 ms).
  - Same layout, rotating storage object: descriptor-set finalization, 7.0 ms (98% of set misses
    differ only in the storage object; a push-descriptor set would remove its bookkeeping and keep
    the descriptor writes).
  - New every frame, about 34 ms: vertex buffer binding 15.5 ms (buffer bases move on most key hits
    and sizes on 55% of this title's, same notes; 79% of read-only probes are first captures of a range
    the previous frame did not probe), storage descriptors 10.0 ms (ring slots), command emission
    6.1 ms (recorded against those sets each frame) and the recipe's guard, about 2.2 ms.
  - Not classified, about 23 ms: the rest of state setup (vertex write-back check, device-address
    preparation and the untimed part, about 12 ms), draw time outside the stage timers (about
    6 ms), dispatches 2.9 ms, and command processing outside draws and dispatches 2.0 ms (fence
    waits 1.5 ms).

  The rows that could follow a previous-frame match (repeats, finalization, not classified) are
  about 58 ms, above half the run, and still about 47 ms with every countable unclassified row
  scored new: about 12 ms of them is time with no named input, so no count settles the threshold
  and no run is planned. If all of them were skipped at no cost, about 34 ms of command processing
  would remain: frames of about 53 ms with the serial submit, so skipping alone does not reach
  30 fps there, and the 30 fps line with the submit overlapped. Most repeat rows are already hit
  paths (index reuse, the texture memo, a pipeline lookup of 0.78 ms inside pipeline setup), so the
  table overstates what a skip removes. The other implementation's in-world entry (above: 60 and
  121 us per draw at p50 and p95, about 470 draws) adds no per-draw mechanism.
  - Cross-cutting counters of the same run, placed by call site (they nest, so they are not summed):
    object lookups 7.5 ms (16,000 per frame, nearly all reuses: about 5,750 render-target, 4,830
    depth, 4,880 vertex, 320 texture and 260 storage lookups, about two target lookups per draw
    against 53 target states per frame); snapshot validation 7.8 ms over 19,900 probes; capture
    uploads 3.8 ms (7,900 per frame, 45.7 MB); hashing 2.9 ms (39 calls, 108 MB).
  - Closed by records this table does not change: vertex binding (the vertex slice above: the
    in-place saving is not shown to clear its bar), validation (the admission checks above, in
    parts under the bar), object lookups (target lookups are context-only pieces, at most about
    5 ms at the average cost per lookup; the vertex ones sit in vertex binding), finalization and
    command emission (per-draw cuts above), and the time outside the stage timers (the
    early-submission check above).
  - Closed as well, storage descriptors, 10.0 ms (`PrepareStorageBuffers` into
    `TryUploadTransientReadOnlyBuffer`, `GraphicsRenderBind.cpp`). Binding the imported window in
    place would drop the copies of about 7,600-7,900 read-only ranges per frame (the capture
    uploads, 3.8 ms) and at most the validation of those roughly 9,600 probes (about 3.8 ms at the
    average cost per probe), part of which an in-place bind still pays (the query for pending device
    results). Finalization is not credited: sets still differ per offset without the parked
    push-descriptor stage. At most about 7.6 ms, under the 10 ms per frame bar the in-place vertex
    slice was held to; the in-place reads together stay at about 1.25x ("Read-only binds" above). A
    read in place also sees the bytes when the host GPU executes, after the submit has returned, and
    a title is recorded rewriting memory its commands point to as soon as the submit returns
    (`docs/bringup/cross-title.md`, "A graphics submit returns after its batch is decoded";
    descriptor tables, data buffers not observed).

  No single lever cleared the bar it was held to; the ledger below sums them. What holds the guest
  between batches is not identified ("Command-processor idle between batches" below).
- **Ledger of the levers under their bars (2026-11-11, offline, from the records above; ceilings
  unless marked otherwise).** Frame of the clean run: 91.7 ms of command processing plus a gap of
  about 19 ms, 111 ms.
  - Narrow and contract-preserving (A), overlaps removed:
    - the dirty-page tracker on the submitting thread, up to 4.7 ms (measured share; time on that
      thread lands in the gap about one for one): the `PageStart` divides, or an early exit for an
      untracked page; measured 2026-11-11: the divides were 0.9 ms, not kept, and the rest is the
      table probe both variants pay (`docs/bringup/cross-title.md`, the notifier record);
    - early-submission polls, about 4.6 ms (estimate: no polls inside the recorded
      one-per-millisecond limit, about half the submissions); out: that limit is excluded
      (`docs/bringup/cross-title.md`, early submission);
    - context-only state for draws whose state did not change, 4-6 ms (profile share: pipeline,
      target and framebuffer lookups, viewport and color description; the target lookups counted
      above sit inside it); priced by the calls that go (2026-11-11): 95.9% of draws bind the
      pipeline their command buffer bound last (temporary counter, an upper bound: other graphics
      bind sites are not seen), and the driver's pipeline bind leaves are about 1.2 ms, so skipping
      the repeated bind is about 1.1 ms; the shader id copies of the pipeline lookup
      (`PipelineCache::CreatePipeline`, `ShaderGetIdVS`/`PS`) allocate about 0.9-1.2 ms. Built
      (2026-11-11): a draw skips a pipeline bind that repeats the recording's last one, 96.2% of
      5,149 draw binds per frame, and the driver's bind leaves went from 1.18 to 0.14 ms per
      frame; identities are built in a per-thread buffer, 9,644 per frame that each allocated and
      freed 2 KiB before. The allocator leaves went from 4.5 to 3.9 ms per frame at the same share
      of the thread's samples (4.7% and 4.6%) in a lighter scene: that saving is not separated
      from the scene, at most about 0.6 ms. On AMD (RADV, the host's integrated GPU, one run of
      110 s, 2026-11-11) `PPSA21402 - Balatro` held gameplay at 60 fps with both changes present,
      two captures correct by inspection (not scored); nothing counted whether the skip fired
      there, and this title was not run on AMD;
    - index snapshot reuse, up to 4.2 ms (two pool locks and one registry lock per reuse); dropped
      unbuilt: every mutex lock and unlock on the thread is about 2.7 ms per frame in the leaf
      profile, so three per reuse cannot remove half of it;
    - the descriptor-set create path, up to 4.1 ms (allocation through write; the writes stay);
      the part a change removes is allocation, unindexing and list nodes, about 1.2-1.7 ms; the
      lookups and hashes are paid by hits as well; dropped unbuilt (2026-11-11): 1.2-1.7 ms is
      those functions' whole share, and a cheaper recycler removes at most about 0.5-0.7 ms;
    - the vertex write-back walk and the bind's query, up to 3.2 ms (the in-use writable set was
      empty in 93% of scans); built, about 3.1 ms by the same timers (above);
    - refused vertex snapshot attempts, up to 2.5 ms; counted again 2026-11-11 (temporary
      counters, 138 world frames): of 5,140 attempts per frame 1,288 are accepted and every one of
      the 3,853 refusals is made at the overlap check, by a writable storage buffer over the
      range (the accepted ones include reuse). The admission leaves cover all probes and vertex
      attempts are about a quarter of them: about 1-1.5 ms. Looking first for a storage object
      that contains the range is not narrow: `GpuMemoryFindStorageContaining` also returns
      read-only storage objects, whose ranges a snapshot admits today; in the call graph that
      lookup is 1.6 ms of the bind's 9.4 ms per frame.
    - range lookups of the GPU object index, a new entry (2026-11-11): about 25,900 per frame,
      24% cache hits, 1.2 candidates per miss; every miss allocated and grew its candidate and
      result arrays, every hit allocated an unused result. Built: allocator leaf time on the
      command-processor thread 6.2-6.7 ms per frame in four runs, 4.5 ms after.

    The run that measured the lookups (124 ms per frame, 5,237 draws) carried a heavier scene than
    the previous run, per frame 101 against 44 MB uploaded, 22 against 4 object creations (17.8
    against 8.0 ms) and 127 against 77 MB hashed: about 5 ms more in guest-data copies, hashing and
    object creation, which the change does not touch; the driver division loop took no time in it.
    The run after it (106 ms per frame, 83.6 ms of command-processor CPU time) ended under water, a
    lighter scene (41 MB uploaded, 1 object creation, 85 MB hashed per frame); its shorter frame is
    not a result of the changes it measured. In that scene only, the thread called `fstat` about
    1 ms per frame (user time, spread over the whole window, absent from four other runs):
    `PrepareLocked` (`source/emulator/src/Graphics/GuestDeviceAddress.cpp`, line 2104) queries the
    guest memory backing on every preparation while a partly imported physical range exists. The
    call-graph run below has no `fstat` sample, and the kernel half is not visible (the thread's
    kernel time was 6.8 ms per frame in that scene against 8.3 ms in a run without the calls), so
    it stays unpriced.

    Together up to about 28 ms as first summed. Re-priced 2026-11-11: about 3.1 ms delivered by the
    write-back walk, then about 1.7 ms of allocator time by the range lookups and 1.0 ms by the
    repeated pipeline bind; 0.9 ms measured and not kept, 8.8 ms out; the rest (context-only
    state, set creation, refused attempts, about 11-13 ms) are a function's whole share, not the
    part a change removes, and the tracker delivered about a fifth of its share. What is left of A
    is under 1 ms; the alias write-back below is a new line that needs a design. Outside the
    ledger, a driver division loop under buffer copies took 0-7.5 ms per frame depending on the
    run (`docs/bringup/cross-title.md`, buffer copies with awkward sizes). The guest-range checks
    of the read-only binds, about 7.4 ms in the full-stack run below, were priced after
    (2026-11-11); the allocated-range check was then fixed and the rest is open:
    - Overlap test, 5.7 ms: the miss walk 2.9 (storage 1.5, vertex 0.8, index 0.5), the cache
      lookup every call pays 1.7 (a miss looks up twice, `GpuMemoryFind.cpp`, lines 299 and 381),
      and the writable-storage test 0.9, all of it under index binds. Index binds are almost all
      reused, so their misses are repeated keys, evicted by another key or cleared by an
      invalidation; which of the two is not separated. On the reused path the lookup takes 0.5 ms.
    - Allocated-range check, 1.7 ms, of which its own cache's lookup and store were 1.4: the cache
      had 4,096 slots against about 12,000 distinct read-only ranges per frame, so most lookups
      missed, around a search of about 0.3 ms. Fixed: each span of the heap index keeps the
      furthest end among the heaps open over it, so the check is one binary search and one compare
      (`GpuMemory.cpp`, `ValidateAllocatedRangeLocked`), and the cache is gone (0.4 MB of
      `GpuMemory`). A unit test covers two overlapping heaps. In the verification run below the
      check took 0.05 ms per frame (one sample); that binary also carried the temporary counters
      and the larger overlap table, and that run is the change's only title evidence.
    - Counted (2026-11-11, one run of about 150 s with temporary counters; 191 world frames of
      115 ms, 5,187 draws per frame; lookups matched the bind probes and index lookups the draws).
      Per frame, overlap test outside index binds: 17,028 lookups, of which 7,099 hit, 164 found an
      empty slot, 3,462 a slot left by a whole invalidation and 6,303 another key; under index
      binds 5,209 (3,321, 60, 613, 1,214); allocated-range check 23,061 (8,077, 0, 1,616,
      13,368). Range invalidations cleared 78 live overlap entries per frame. Whole invalidations
      come only from adding or removing a heap and ran 0.83 times per frame; the two later runs
      with heap counters saw only heap adds (0.97 and 0.80 per frame) and no removal. The count
      tests the generation before the key, so a slot left by a whole invalidation may also hold
      another key: among the other lookups 47 % did (26 % under index binds, 62 % in the
      allocated-range check).
    - Priced at about 0.25 us per overlap miss and 0.05 us per allocated-range miss, from the
      full-stack run, which had the same bind probes per frame (20,407 against 20,487). Another
      key costs about 2.3 ms in the overlap test, counting its share of the invalidated slots;
      invalidating only the heap's range instead of the whole caches would save about 0.6 ms.
      These prices overlap and do not add. The 2.3 ms is not a lever as it stands: four times the
      slots did not shorten the miss walk (below). The slot is the key's mixed 64-bit hash masked
      by the capacity (`GpuMemoryRangeQueryCache.h`), so it spreads with the table; the likelier
      reading is that most of those misses are keys not yet stored in the frame, which miss at any
      size. Misses were not counted in the verification run, so that stays unconfirmed.
    - A larger overlap table was tried and reverted: 65,536 slots (table 8.0 to 32.0 MB, about
      24 MB more resident), kept only if the miss walk plus the lookup fell by 1 ms. In the
      verification run they rose from 4.61 to 5.02 ms per frame (5,271 draws against 5,183), and
      the span index's store and unlink from 2.56 to 3.41 ms.
    - Heap-range invalidation is a lead, not built: it is under 1 ms, and a stale entry would reach
      every title. The argument for it: a heap is added empty and removed after its objects are
      freed, overlap results depend on objects only, and allocation validity changes only for
      queries that overlap the heap. Lines 312-340 of `QueryOverlapsLocked`
      (`GpuMemoryFind.cpp`) were not read.
    - Verification run (2026-11-11, one run ended 40 s after the world loaded; both changes and
      temporary heap and drain counters; 168 world frames of 103.7 ms, 5,271 draws per frame;
      command-processor CPU 75.1 ms per frame against 74.7 in the full-stack run; one sample is
      0.054 ms per frame). The read-only capture fell from 15.0 to 13.4 ms and the read-only upload
      from 16.1 to 14.1 ms, about the allocated-range gain within about 0.9 ms of noise.
    - The heap events themselves: in the full-stack run no sample of any thread fell in adding or
      removing a heap, the heap index rebuild or the queue drain, so their user time is under
      about 0.15 ms per frame (3,001 user-CPU samples, symbolized by address against a rebuilt
      binary whose layout was checked on 1,268 addresses of the processor thread). Kernel time and
      waits are not seen by samples, so the verification run counted the drains.
    - Drains, open (verification run, world window of 17 s, 164 frames): 4.46 per frame, all from
      releasing direct memory (`GraphicsInvalidateGpuMappingRanges`); none came from removing a
      heap, and none was seen for a cover heap or a video buffer (those counters never fired). A
      drain waits for the submission admission gate, then for the GPU to go idle: 6.9 ms of drain
      per frame, the gate held 7.1 ms. Of 732 invalidations, 619 found no GPU object, device-address
      window or video buffer, 48 a GPU object and 113 a device-address window. A Gen5 submit
      returns only after its batch is decoded and holds the gate meanwhile, about 82 ms per frame,
      so a release waits for the decode in flight; the releasing threads waited about 75 ms per
      frame in total, in parallel behind the decode, which is not a frame cost by itself.
    - Drain bound on the frame: the frame is the submitter's cycle, about 82 ms inside the submit
      and a 21.8 ms gap (counter window, 103.7 ms world frames). The submitting thread made 94 of
      about 97 submits and ran 1.31 drains per frame, waiting 1.9 ms for the gate before them. Its
      user CPU, normalized per draw to a 5,271-draw world frame because the perf window held a
      lighter scene for 4.1 of its 10 s, was 13.4 ms, of which the release path was 0.6 ms; guest
      work may not scale with draws. That leaves about 6.5 ms of the gap for its drains, kernel
      time and any other wait, so the drains cost the frame between about 2 and 8 ms (1.31 drains
      at the mean 1.54 ms give about 2 ms more than the wait). Admitted submits waited 0.005 ms per
      frame at the gate, which is why the first reading, gate waits of submits only, missed it.
    - Load-time lead: in the first 92 s the drains held the gate 22-64 % of each 15 s window and
      10,899 of 12,630 invalidations (86 %) found nothing (2,415 of 2,417 between 16 and 31 s).
    - Write notifications on guest memory copies, a lead: the guest's `memcpy`, `memset` and
      `memmove` notify the dirty-page tracker before writing (`LibCString.cpp`,
      `GpuDirtyPageTracker::NotifyWrite`), so a write to an armed page does not fault. A counter
      run (2026-11-11) saw 4.5-6.4 million notifications per second in the world, about 615,000
      per world frame, of which about 224 found a tracked page: more than 99.9 % find nothing. On
      the submitting thread they cost about 4.1-4.3 ms per 5,017-draw frame in the two profiled
      runs (32-33 % of its samples, inside the gap), about 6-8 ns per call; the processor thread's
      1.8 ms in the tracker is queries, not notifications. A page shift instead of the runtime
      division by the page size was built and reverted: tracker time on both threads fell 0.26-0.66
      ms per frame against a 1 ms rule, in a run whose window was a lighter scene (2,832 draws per
      frame), so a small gain is not excluded. That run was also the first of the pushed
      allocated-range change without the larger overlap table; it ended clean.
    - Reverted under the rule, a region filter before the hashed probe (2026-11-11): one bit per
      2 MiB of host address space, set before a page key is published and never cleared, let a
      write whose regions hold no tracked page return early. One run flipped it on and off each
      second, with counters in both phases (8 s of world, 71.5 frames of 112 ms, 5,574 draws per
      frame). 94.7 % of calls were rejected; the submitting thread's notification path fell from
      8.3 to 7.2 ns per call, 0.7 +/- 1.2 ms per world frame (31 against 37 samples, 1 sigma), and
      1.35 +/- 0.8 ms per call over the whole 20 s window (60 against 79 samples), against a
      1.5 ms rule: the fall is not shown to reach 1.5 ms and could be up to about 3 ms. At equal
      call counts over 20 s the probe-side samples fell from 61 to 8 and the check added 10, but
      the caller-side samples rose from about 18 to 42 (`Instance` 1 to 12, `NotifyWrite` 9 to 19):
      about half the probe's apparent cost reappeared in the callers, cause not established.
      Caller-side samples with the filter on are about 4.7 ns per call, 2.9 ms per world frame, an
      upper bound since `NotifyWrite` carried the counters. Hold plus gap per submit was 111.6
      against 109.9 ms (36 submits per phase over four alternations, no spread computed).
    - A lead, not built: a reject at the call site, before the callback hop into the tracker,
      would be needed to recover the caller-side cost (2.9 ms per frame at most).
    - The gap levers (the drain, the notification path, other submitter CPU work) share one
      21.8 ms budget per frame and substitute for the serial-submit lever rather than add to it.
      Even a zero gap leaves a frame of about 86 ms or more against 16.7 ms, so they stay
      cross-title CPU leads, not a route to the frame-time goal.
    - Priced on paper for a lever of 30 ms or more of command processing (2026-11-11, no run;
      one basis: the full-stack check below, 74.7 ms per frame at 5,017 draws in 102 ms frames;
      the 21.8 ms gap is from the drain-split run):
      - Repeat across frames: draw keys repeat in 94-99 % of draws within two frames (four
        titles, about 99 % in this world; `docs/bringup/cp-draw-plan.md`), but all descriptor
        words match in under 1 % of hits, buffer bases change in 44-92 % and buffer sizes in
        21-55 %, so a reused draw plan still captures bytes and binds objects. It could remove
        the shader input information (7.4 ms, its recipe behind a recorded exclusion), the
        pipeline identities and lookup (4.6), descriptor sets with bindless indices (6.6) and a
        part of the register state that run did not split: about 18.6 ms plus that part, 11.2
        outside the exclusion. Read-only binds (16.1), vertex binding (8.9), the alias
        write-back (2.7) and driver calls (8.3) stay.
      - Independence: the title submits one command buffer per frame (1.01 submits per world
        frame in the filter run), so there is nothing to decode side by side. Splitting inside
        it is the parallel-recording design (`docs/bringup/parallel-recording-design.md`); on
        this basis it moves at most the pipeline lookup (1.6), the sets (6.6) and the driver
        calls (8.3), about 16.5 ms, overlapping the repeat class on pipeline and sets.
      - Absent elsewhere, a different load (see "A lighter world frame is not a target" above):
        the other implementation's tree (a lead only) decodes on one thread, returns from a
        submit once the batch is queued, copies vertex bytes per draw, uses per-draw descriptor
        sets, and has no write notifications on guest copies, no overlap admission and a sparse,
        probabilistic texture check. One profiled run there (another host) records a median of
        101 us per draw after the player connected, no mean, from a timer no longer in its
        tree, so its scope is unverified: one thread at that cost handles at most about 10,000
        draws per second, against about 50,000 here in the filter run (71.5 frames of 5,574
        draws in 8 s, 14.9 us per draw). Its only draw counters for this title come from one
        session on this host that logged black-frame and shader-refusal events (35,206 draws in
        all, at most seven frames of this world's load; another session, 4,847). Its frame rate
        is not evidence that this load runs at 50-60 fps, and the steps it lacks are not priced
        against it. At about 54 draws per flip (loading and menus) this build ran 49-51 flips
        per second in three runs, a different scene.
      - Bound over the priced items only, not a floor: no single class reaches 30 ms. Outside
        recorded exclusions the union of the two classes is about 19.5 ms plus the unsplit
        register-state part, which would leave about 55 ms of command processing less that part
        and a frame of about 77 ms less that part with the gap kept (about 13 fps). 60 fps would
        need about 3.3 us of command processing per draw, with the gap overlapped, against about
        14.9 us now. The skip-phase run (above) shows that the per-item gate hid most of the
        hold: the path behind the draw entry is over 90 % of it.
  - Needing a design, or changing when guest bytes are read (B), re-priced 2026-11-11 on one
    call-graph run (61 world frames of 107 ms, 5,081 draws, 82.5 ms of command-processor CPU time
    per frame, 921 samples, so about 1 ms of noise at 10 ms; whole-function shares). Its 16 KB
    stacks reach the thread start in 70 of 921 samples, so shares of outer functions are lower
    bounds; the same split gives an earlier run's vertex binding 8.9 ms against the 9.4 ms recorded.
    - Behind a recorded exclusion or hazard, about 21.4 ms: the shader input recipe, 1.41 us per
      draw or 7.2 ms before guards (excluded at its field set, above); in-place storage reads, up to
      7.6 ms (their part of the read-only capture); in-place vertex binds, up to 6.6 ms (upload
      2.9, object creation 2.4, pending storage copy 1.3; the lookups, 3.2 ms, stay), was 9.2 ms
      on the older census. Both in-place entries read the bytes when the host GPU executes (the
      rewrite-after-submit record under "Closed as well"); a count then found no storage or index
      source rewritten before completion ("Read-only binds", above). Priced on paper, in-place
      storage and index binds net at most about 5 ms and are closed; the vertex entry stays. The
      read-only capture is 15.4 ms in all (copy call 4.9, of which the copy is 0.8 and guest-range
      checks 3.8; compare 3.7, snapshot lookup 1.2, admission 0.9, pool bookkeeping about 3); index
      snapshots take 4.9 ms of it (`GraphicsRenderDraw.cpp`, lines 2060 and 2172), owned by no entry.
    - Needing a design, with nothing recorded against it: a reusable descriptor set, about 3.4 ms
      (the creation side: allocation 1.3, driver writes 1.9, deferral 0.2, in an older clean run of
      80.1 ms command-processor time; a reused set still pays hashing, lookup, trimming and use
      recording), about 1.5 ms with a descriptor buffer, which keeps the writes. Both need a stable
      buffer identity; push descriptors are excluded on this driver (`docs/bringup/cp-draw-plan.md`).
      The alias write-back below, 3.0 ms. Parallel recording, at most about 14.5 ms (pipeline lookup
      without the shader identities, which the draw record carries, 2.7; set lookup and creation
      6.2; command emission 5.6), 1.07-1.21x of command processing, under the design's 2x gate.
    - Descriptor-set misses (temporary counters in that run, per frame): 8,819 lookups, 1,488 hits,
      7,331 misses, 6,537 of them for sets of a fresh snapshot, which skip the cache. The storage
      buffer identities differ in every miss and the other fields in at most 416, so at least 94%
      differ only in which snapshot buffer they bind. Those sets still pay the hash and the lookup.
      Skipping both is exact only when a storage buffer has no cached set: pool entries are reused
      with their identity, and their cached sets stay until the buffer is deleted, so a fresh
      snapshot can hit a set from the entry's earlier use. That form saves at most about 0.6 ms,
      the case of the recycler above; not built.
    - Device-address write-back into aliases, a new lead: `GpuMemory::WriteBackStorageRanges`
      (`source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp`, line 926) runs before
      device-address reads and spends 3.0 ms per frame (2.1 ms in the older run) in
      `WriteBackObjectLocked` re-reading guest bytes into the written object's equal aliases
      (`Update`, line 395, through this caller only: copy 1.1, hash 0.8, the object's update 0.6,
      vector resize 0.5). It is not the walk built
      above. Not priced as a change.
    - Nothing in B is buildable without a design. Outside the exclusions about 6.4 ms are left,
      plus parallel recording at most 14.5 ms that overlaps the set entry, of 82.5 ms of command
      processing and a 107 ms frame. The earlier estimate, which counts items behind recorded
      exclusions: with A and the three per-draw items of B about 44 ms of command processing is left,
      about 26-37 ms with parallel recording, and the frame is that short only with the gap overlapped
      (27-38 fps). No combination reaches 16.7 ms.
    - Full-stack check on the pushed code (2026-11-11, 64 KB stacks, 1,444 of 1,459 samples reach
      the thread start, no lost samples; 98 world frames of 102 ms while profiling, 5,017 draws):
      74.7 ms of command-processor user CPU time per frame. The processor's own time counter reads
      80.6 ms per frame there and 79.5 ms after. The run before the four A changes above read
      80.9 ms: per draw that is 15.1 against 15.6 us, about 2.5 ms per frame, inside the counter's
      80-85 ms spread between runs, so it neither confirms nor refutes the 5.8 ms that A credits
      from function shares. Inside draws 66.3 ms (56.4 was the lower bound). No entry
      above is higher by more than 2 ms: read-only binds 16.1 (storage 8.2, index 5.2, vertex 2.7),
      vertex binding 8.9, descriptor sets 6.6, alias write-back 2.7, shader input 7.4, pipeline
      lookup without the identities 1.6 (building them, 3.0), driver calls 8.3 in all.
- **Open.** 60 fps at this load needs about 3.3 us of command processing per draw against about 21 us
  then (2026-11-10; about 17 us at 5,342 draws on 2026-11-11, and the gap of about 19 ms between
  batches alone is longer than a 60 fps frame, so 60 fps also needs the gap cut). None of the
  measured levers, alone or together, reaches that. In the profiled run (110 ms
  frames) the command-processor thread was on a CPU about 73 ms of each frame. The fence waits are the
  only instrumented wait above 0.1 ms per frame (12-16 ms in six other runs before `d2ff690bc`, 2.09 ms
  in one run after `443032ed0`; in the clean control run
  of 143 ms frames: fence 14.9 ms, flip-done 0.07 ms, render lock 0.04 ms, WAIT_REG_MEM 0.03 ms), so
  roughly 20-25 ms of a frame are in no instrumented wait; this mixes runs and is an estimate. No other
  thread was above 14%, but 13-14% is 15-19 ms of a frame; the next item shows that the submitting
  thread runs while the command processor waits.
- **Command-processor idle between batches (measured; one run with temporary counters, 80.7 ms frames
  at 2,866 draws per frame; clean runs are 98-110 ms).** Among the graphics submit functions the title
  imports only the multi-DCB driver submit, and it submits one graphics batch per frame (12.6 per
  second). The graphics command-processor thread waits about 15.6 ms of each frame for the next batch
  (194 ms/s, mostly one wait of 8-20 ms per frame). One guest thread makes 565 of 566 submits and is on
  a CPU 11.5 ms of each frame; the command-processor thread 53.5 ms.
  - A graphics submit returns only after the command processor has run its whole batch:
    `GraphicsRing::Submit` waits on the decode completion, which `GraphicsRing::ThreadBatchRun`
    signals after the batch, its flush and its flip (`GraphicsRun.cpp:1949`). The only earlier
    release, while decode waits on a value no submitted GPU work writes, did not occur in that run (no
    suspended waits). This is a recorded guest-visible contract (`docs/bringup/cross-title.md`,
    "A graphics submit returns after its batch is decoded"), so the guest's work after a submit and
    the command processor's run of the batch are serial.
  - Bound: overlapping the two recovers at most this gap, about 1.24x (12 to 15 fps in that run). An
    early return in the fighting title gained nothing because its guest then waited on an event flag
    (`docs/titles/anime-fighter.md`), so whether this guest would start its next frame earlier is not
    established. Overlap needs a snapshot, taken before the submit returns, of the guest memory the
    batch reads, as the contract record says; its scope and cost are not measured. The work between
    the batch's last guest read and the signal (graphics interrupt queueing, flush, flip,
    `GraphicsRun.cpp:1930-1949`) also holds the submitter (measured in the next item).
  - Measured, one batch ahead does not shrink the gap (2026-11-11, one world run with temporary code
    and private shader caches, 132 walking frames of 111 ms at 5,342 draws). Each submit waited for the
    previous batch's decode instead of its own, so the guest could run one batch ahead. The command
    processor still waited 18.9 ms of each frame for the next batch: 24-29 ms in three runs before
    the notifier fix, about 18-23 ms expected after it (time on the submitting thread lands in the
    gap about one for one), not near zero. The world drew normally and the run did not stop.
    Excluded: more than one batch per frame (about one submit per frame, all from one thread).
    Not identified: what holds the guest. The probe counted no early returns, so the early return
    may not have engaged; if it did, the guest waits on something else, an end-of-batch event of its
    own or a host-side flip or present wait on its thread (the flip-done counter, 0.07 ms per frame,
    times the command processor's packet wait, not the guest's thread). Open, with no run of its
    own: an early-return count and a sync-wait snapshot. The batch tail (interrupt queueing, flush
    and flip between the batch's last guest read and the signal) took 0.04 ms per batch.
  - Rechecked with the early return engaged (2026-11-11, `c8f187722` plus temporary code, private
    shader caches, the saved world entered from the menu, 10 s from 8 s after the world loads). Without
    the change the submitting guest thread waits 29-32 ms in each submit for its batch and runs 10-12 ms
    until the next one, one submit per frame: 44.7 ms frames, 32.5 ms of command processing at 2,935
    draws per frame. With each submit waiting on the previous batch instead, every submit returned
    before its own batch was done (counter 4,080-4,200, rising by one per submit), so the earlier probe
    had not engaged. The previous batch was always done by the next submit (wait 0.0-1.2 ms), yet the
    thread took 70-78 ms between submits against 61.1 ms of command processing at 5,458 draws per frame:
    74.6 ms frames, the same command processing plus guest time as before. The thread is not spinning:
    no thread but the command processor ran more than 14.4 ms of CPU per frame (57.5 ms for the command
    processor), and `threads` shows no guest wait object in 240 of 262 polls, as it also does while the
    thread is blocked in the submit's decode wait. The two runs reached scenes of different load, but
    command processing per draw is the same, 11.1 and 11.2 us.
  - What holds it (2026-11-11, same early return plus a temporary timer per guest thread and call site
    on the interruptible host waits). The submitting thread spent 691 ms of each second in one wait:
    the guest releases direct memory every frame, and `release_direct_memory` (`Memory.cpp:2978`) calls
    `GraphicsInvalidateGpuMappingRanges` (`Graphics.cpp:130-153`), which runs the invalidation under
    `GraphicsRunWithQuiescedSubmissions` (`GraphicsRun.cpp:3613`), whose `Gpu::WaitLocked` waits in
    `GraphicsRing::WaitForIdle` (`GraphicsRun.cpp:1630`) until the command processor has run every
    queued batch. `sceGnmSubmitDone` is not called by this title (a timer on `GraphicsRing::Done` never
    fired). With the serial submit the ring is already idle there, so the wait costs nothing; with an
    early return it puts the whole batch back on the guest. The quiesce protects the emulator's own GPU
    objects and device-address registry, which the command processor uses while it runs, not a guest
    contract. An early return gains this title up to its ~11 ms of guest time per frame only if the
    invalidation runs in submission order on the command processor instead of draining it, and the
    descriptor-table contract above is met.
- **Device-address residency (measured; populated-page query fixed in `67d6e9a95`).** Before the
  fix, in the world the residency timer read 8.1 and 9.1 ms per frame in two runs without temporary
  code, and 1.46 ms in the run with temporary counters, whose world is a lighter regime: the level of
  live mappings whose direct memory was released moved 0.72 per frame there against 3.89 in the other
  two. In all three, residency per sample follows the change of that level (correlation 0.61-0.62,
  2.0-2.5 ms per unit of it). Command-processor system time follows residency time (correlation 0.86
  and 0.95 in the two runs; 0.75 in a run with read-only census code), and seconds above 300 ms/s held
  6 of 9 frames over 250 ms in them. Table rebuilds are about 0.2 ms per frame in all three.
  - Light regime (the run with counters, 43 s of world): 322 populated spans imported at about 1.1 ms
    each; spans of 16 pages cost 1.5-2.5 ms while the backing did not grow, so the cost is per call,
    not per page.
  - Loading only, same run: for four seconds the check ran up to 1,916 sweeps per second, each over
    one unimported span, and imported no page in about 6,700 sweeps at up to 104 ms/s. Each sweep is
    one `SEEK_DATA` query from the span start (`FindUnpopulatedSharedBackingSpans`,
    `source/lib/Core/src/VirtualMemory.cpp`), 31-57 us there against about 3 us in quiet seconds, and
    is triggered by a population change anywhere in the shared backing (it grew 20-25 MB/s while
    discards rose 2,500-5,000 per second).
  - Split (a later run with temporary counters, same route and save, 27 s of world, level +0.88 per
    frame): the populated-page query (`sys_virtual_query_shared_backing_data_pages`,
    `source/lib/Sys/src/SysLinuxVirtual.cpp`) cost 3.6 ms per import call over 0.56 MiB, 94% of import
    time (37 of 39 ms/s); physical resolve 6 us and the run import 211 us per call. Each call issued
    about one `SEEK_DATA` and one `SEEK_HOLE`, and the `SEEK_HOLE` walked on average 900 MiB of
    populated backing past the span end before it reached a hole. On the host, a `SEEK_HOLE` over a
    1 GiB memfd costs about 6 us per populated MiB it walks; a `SEEK_DATA` on a populated page costs
    0.3-0.4 us and one across a 1 GiB hole 0.4 us.
  - Fixed (`67d6e9a95`): the query issues one `SEEK_DATA` per page, so its cost follows the pages
    inside the span and never the backing beyond it. One run with temporary counters on the same route
    and save: 44 us per call (106 queries) in the world against 3,626 us, and 41 us against 921 us in
    the rest of the run; import time 3.4 against 39 ms/s; residency 0.63 ms per frame against 4.18,
    with the level moving 1.92 per frame (2.0-2.5 ms per unit of it before the fix, so 3.8-4.8 ms
    expected). Pages imported per page queried were 1.00 against 0.99 in the world and 0.92 against
    0.89 in the rest of the run; the unit test covers a span inside a longer populated run. The world
    capture at 125 s rendered correctly; the player had died to a zombie, as in the clean control on
    this save. The heavy regime (+3.89 per frame) was not run again. No frame-rate claim: residency
    was 4-9 ms of 100-130 ms frames before the fix.
  - Limit: the cost is now linear in the span's pages, about 0.1 ms per densely populated MiB, once
    per import. It was cheaper than the old walk in both loading and world here; the largest spans
    other titles import are not measured.
  - `PPSA21567 - ASTRO BOT`, same session, old and new query, same save and route: the scripted route
    missed the title screen on both binaries, and a diagnostic agent tap took both to the intro
    cinematic; residency was 0.19 against 0.17 ms per frame.

## Sandbox title: letters missing from the death message (2026-11-10, open)

- Observed (Intel, strict, the world route of the frame-load record, world saved at night): at 130 s
  the player has been killed and the death screen shows "yty was slain  y  om ie" for "Kyty was slain by
  Zombie" and "R SPAWN" for "RESPAWN". The capital K, Z and E and the lowercase b are missing
  in both runs; one run also drops the p of the "Respawn" prompt. The other text on the screen and the
  HUD prompts during play render whole.
- Seen on the clean binary at 446e97dbf and on the excluded staging change above, so neither change
  caused it.
- No reference to the system font library in the three binaries searched (the executable and the
  two user-interface modules at the game root, string search), so the font library record of
  2026-10-14 is not a lead; the title's other modules were not searched.
- Glyph atlas (temporary probe on texture binds, one run that reached the death screen): the text
  samples a 1024x1024 R8 texture, tile mode 9, at one fixed address. Its guest bytes never changed
  (the same nonzero count in every dump), while the host copy gained six regions between presents
  4033 and 4066, so the device fills it; which command writes it was not identified. Seven small R8
  textures (36-48 x 40-45) were first sampled at present 4036, their guest bytes already holding the
  glyph (host and guest contents equal); their nonzero counts match the six new regions (one region holds two), and
  they are the glyphs that appeared (capital P and G in a thin style, capital R, S, P, W and N in a
  bold style). No small source for E, K,
  b or Z was sampled in presents 4000-4039, and the atlas did not change again up to present 4238;
  presents 4040-4059 were not dumped.
- A second run from the same saved world, dumping every new small source from present 3950, did not
  reach the death screen (the player was alive at 160 s), so it gave no answer.
- The pause menu, captured in a later run, renders all its text, but none of it contains K, Z, E or b,
  so it does not test this defect.
- Direction: when the death screen is reached again, dump the small sources without gaps and log
  every write to the atlas range, to tell whether the missing glyphs' sources are never produced,
  produced empty, or produced and not copied.

## Sandbox title: snapshot comparisons in the world (2026-10-24, improved)

- **Observation (Intel A770, strict, 60 fps present limit, world-entry route,
  profile 225-265 s).** In the world the command processor spends about
  100-108 ms per frame on about 4300 draws (23.8 us/draw); waits are below
  1 ms. Sampling the command-processor thread put 12.5% of it in the
  comparison of transient read-only snapshots against guest memory, 93% of
  that in `memcmp`.
- **Cause.** A temporary per-probe count showed about 30 000 snapshots of
  16 KiB or more per 400 000 probes, averaging about 490 KB, matching guest
  memory on almost every probe and compared in full each time.
- **Change.** A matching snapshot of at least 4 KiB is registered with the
  dirty page tracker before its comparison and reused without one until a
  write reaches its range (`TransientBufferPool`, `GraphicsRenderCommandBuffer.cpp`).
  The admission check still runs on every probe, so unpublished GPU writes
  are still refused. A key whose tracking fails waits a number of probes that
  doubles with each quick failure, up to 1024; a key whose tracked bytes
  served at least 16 probes is tracked again at once after a write. A
  capture now drops the entry's previous snapshot before copying, so a
  refused copy never leaves partial bytes indexed.
- **Verified.** 20.8 us/draw (89.6 ms per frame at the same draw count),
  `memcmp` gone from the comparison profile; world draws textured. Racing
  (60 fps menu), fishing (dock) and beat 'em up (intro) reach their usual
  states with no stop. Unit 2459/2459, integration 74/74.
- **Excluded on the way.** A sticky "compare only" mark per key (no retry)
  tracked only a third of the large probes: keys rewritten during loading
  kept the mark in the world.
- **Open.** The remaining per-draw cost: descriptor and storage preparation,
  admission checks (`TransientReadOnlyOverlapsAllowedLocked`, `FindBlocks`),
  `GpuMemory::CreateObject`, shader input parsing and about 380 Vulkan
  submits per frame.

## Sandbox title: GPU hang on the first world frame (2026-10-23, fixed)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit,
  world-entry route).** About two seconds after the player spawned, the
  kernel reset the render engine ("Timedout job", always the same GuC
  context) and the run stopped with `VK_ERROR_DEVICE_LOST` on queue 8. Nine
  of eleven runs without instrumentation hung. In every submit-fault report
  the unfinished command buffer was the post-processing and UI pass of the
  first world frame. All eleven queues share one queue family and run on the
  render engine, so the reset alone does not name the queue.
- **Cause.** With the Vulkan validation layer (core checks; no emulator
  instrumentation) the run still hung, and the layer reported one error in
  the whole run, at the hang: the depth slice array
  (`DepthStencilBufferGetSliceArray`) had TRANSFER_DST and SAMPLED usage only,
  but was transitioned to and sampled in DEPTH_STENCIL_READ_ONLY_OPTIMAL, a
  layout valid only for images with depth-stencil attachment usage
  (VUID-VkImageMemoryBarrier-oldLayout-01210). The shadow cascades are first
  sampled through that array on the first world frame.
- **Change.** The array is created with depth-stencil attachment usage, as
  sampled depth views already are (`Texture.cpp`).
- **Verified.** A validation run reports no error and reaches and draws the
  world. Three runs (one with validation, two without) hold the world with no
  engine reset, where nine of eleven hung before. Unit 2446/2446,
  integration 74/74.
- **Excluded on the way.**
  - Runaway guest loops: the five guest shaders with loops in that command
    buffer (three pixel shaders bounded by constant-buffer values, a histogram
    compute whose counter stops at 255, a 3D blur bounded by its radius)
    translate to bounded loops. Skipping them, all five compute dispatches or
    the other post-processing pixel shaders still hung; a skip also feeds its
    consumers stale data, so these runs exclude little on their own.
  - GPU-side waits: the renderer records no event waits and no Vulkan
    indirect commands.
  - The `condition ignored` warnings before the hang appear in clean runs too.
  - Localizing by timing did not work: a fence-polling watcher and per-command
    buffer markers (top and bottom of pipe, or top only) each gave clean runs.
    The validation layer did not hide the hang.
  - A stop on the "Get started" menu in earlier runs that day was host input
    reaching the window during the runs, not an emulator defect.
- **Open.** The driver path that turns the invalid layout into a hang was not
  examined; the device core dump was not read.

## Sandbox title: shadow cascades bound as slice ranges (2026-10-23, fixed)

- **Observation (Intel A770, Mesa ANV, strict run, 60 fps present limit,
  world-entry route).** After the depth-slice change of 2026-10-22 the title
  stopped with "layered depth rendering is not implemented" right after the
  player spawned. Temporary logging (removed) showed the shadow cascades on
  the 2048x2048 D32 target bound with `DB_DEPTH_VIEW` slices 0..3, 1..3, 2..3
  and 3..3: each pass starts at its cascade and keeps SLICE_MAX at 3, and
  none sets `PA_CL_VS_OUT_CNTL.USE_VTX_RENDER_TARGET_INDX`.
- **Contract.** Without that bit the render target index is zero, so a draw
  renders only SLICE_START; SLICE_MAX only bounds the indices an exported
  layer could select. The colour path already applies this rule
  (`CollapseRenderColorLayersWithoutLayerExport`).
- **Change.** `DescribeRenderDepthInfo` refuses a slice range only when the
  draw exports a layer index. Otherwise the pass renders the single slice
  SLICE_START, exactly as a single-slice view does.
- **Excluded.** Making each range a layered depth image over
  SLICE_START..SLICE_MAX: the overlapping ranges reclaimed each other's
  images on every pass, the cascades lost their content and the world ran at
  5.5 fps. One run with that experiment ended in a GPU hang on queue 8.
- **Verified.** Two 150-170 s strict runs reach the world with no stop, at
  7-8 fps in world, with terrain and shadows as before the depth-slice
  change. Only the case that used to stop changes behaviour.
- **Open.** A depth slice range with an exported layer index still stops; no
  title has reached it.

## Sandbox title: drifting dark patch on terrain (2026-10-16, open)

- Observed (Intel, strict, world-entry route, no input after the spawn): from about frame
  10,600 on, a dark translucent quad lies on the grass in front of the player. It moves and grows
  between captures (frames 10,692, 10,854 and 11,016). Earlier frames on the same route do not
  show it.
- It also appears in a run at 97034ebc (frame 11,400), before the index-snapshot and vertex-tail
  changes, so it is not caused by them.
- Not yet known whether this is a guest effect or a rendering error. Suggested direction: confirm
  against reference footage of the same world type first. If it is an error, take a native capture
  at the frame and use the operation trace to find the draw that writes the quad (likely a
  translucent or shadow pass), then compare its inputs with a frame where the quad is absent.

## Sandbox title: missing terrain in the world (2026-10-15, fixed 2026-10-16)

The world shows sky, clouds, HUD and an all-water view without land; the
inventory screen works. Recorded so the excluded paths are not re-tested.

- In-world frame structure (one presented frame, temporary draw list): G-buffer
  and depth clear; 152 depth-only terrain draws; a rectangle-copy compute from
  an R32F view of the depth target into the live depth plane; shadow cascades on
  a 2048x2048 D32 target; light computes; deferred lighting and composite; a
  depth prepass with writes and the G-buffer fill (152 draws); fog, water,
  bloom, tonemap.
- Excluded:
  - The depth-plane rectangle copy. It runs every frame from boot. Dropping its
    store (diagnostic only) left the world image identical.
  - Terrain draws dropped on the CPU side. A histogram of every skip reason of
    the indexed and auto paths over a 300 s run showed about 2,200
    depth-writing draws per presented frame reaching command emission and no
    draw taking a skip reason; `gda-not-ready` outcomes are retried waits.
  - Stale sampling of depth or render targets, the fill depth content (fill
    depth function forced to ALWAYS, terrain still missing) and the block atlas
    content (atlas painted a solid colour, no such terrain appeared).
  - The terrain vertex transform and vertex fetch. The fill VS computes
    `Proj * (World * pos)` with position as 16_16_16_16_FLOAT at offset 0 and
    valid guest bytes. An earlier note that clip W <= 0 hid the terrain is
    retracted: the probed chunk was behind the camera by construction.
  - The guest floating-point environment. The bundled libc default is FPUCW
    0x037F and MXCSR 0x9FC0 (FTZ and DAZ); Kyty starts guest threads at the
    host 0x1F80. Starting them with the guest default changed neither the world,
    the camera nor timing. Not merged (no observed benefit); revisit with a
    title-wide A/B since it is the guest contract.
  - HLE libm: all 58 math registrations hash-match their names, and the title's
    bundled libc exports take precedence over HLE anyway.
  - World database I/O. A guest file trace on the world database showed every
    open, write and close succeeding (manifest and `CURRENT` created and fully
    written); the `ENOENT` stats are the normal fresh-database probe.
  - Guest file system for pack data. A full trace (228,000 operations in
    185 s) showed no failing application path that exists on the host
    (case-insensitive), no failing read, and every file read to its size except
    random-access fonts and streamed music.
  - Condition-variable timed waits. The worker pool waits through
    `std::condition_variable` over HLE `cnd_timedwait`/`cnd_wait`; deadlines come
    from `_Xtime_get_ticks` (microseconds of `CLOCK_REALTIME`), the host
    condition uses the same clock, and the result codes match the set the guest
    accepts.
- Excluded (later the same day):
  - The world generator and server-side placement. Saving the world from the
    pause menu (driven live through `kyty_agent pad tap`) wrote a 724 KB
    database log: stone, deepslate, dirt, grass, logs, ores, water and animals,
    and a player record at the spawn standing on terrain. Reloading that world
    (spawn already resolved) still shows only sky, so creation order is
    excluded as well.
  - The client simulation. With the coordinate HUD enabled the player is at
    the spawn, walks and collides correctly; only the image stays empty.
  - Stale constant data on the GPU side. A temporary bind-time probe compared
    the vertex-stage constant buffers of the terrain draws with live guest
    memory: they are copied from guest memory at bind time and hold exactly
    what the guest wrote. No terrain constant changed after it was bound (the
    only late changes were UI ring-buffer slots reused by the next frame).
  - Camera-relative offsets. Every terrain world translation is a multiple of
    16 while the player stands at x=74 (74 mod 16 = 10), so they cannot be
    offsets from the player.
  - Pad motion data. `PadRead`/`PadReadState` report an identity orientation
    and resting gravity, so no NaN reaches a camera that folds motion in.
  - Camera definition I/O. An earlier guest file trace shows the resource
    pack's camera archive opened during world entry and read in full: header,
    table of seven entries and all 6,612 data bytes, which add up to the file
    size. The behavior pack's preset archive is also read in full. (Reading was
    correct; the parse rejected the data, see the fix below.)
- Observed (guest state, not the GPU path):
  - The guest writes a view-projection that is a pure perspective (fovY 60,
    16:9, near 0.025 for a 0..1 depth range) with no rotation or translation,
    and stick input does not change it.
  - Every fill draw's world translation is a multiple of 16 and the drawn chunk
    set is a disc centred on the origin. Almost every fill draw is a 16x16 water
    surface sheet at sea level; there are no lower subchunks.
  - Two bind-time dumps 350 presents apart, with the player moved 8 blocks,
    drew the same chunk set (three chunks added): the render camera stays at
    the origin.
  - The right stick does not turn the player: walking forward after holding it
    still moves along +z. The title's first-person camera definition routes
    look input through the camera and copies the player's yaw back from it, so
    this is the same defect as the frozen render camera, not a pad defect.
  - Every camera entity in that archive declares a 66-degree field of view and
    a 0.025 near plane. The guest's projection has the same near plane but a
    60-degree vertical field of view, so it may come from a fallback path
    rather than from one of those camera entities (not proven: how the title
    combines the camera value with its options is unknown).
- Fixed (2026-10-16): the camera definitions were rejected by the classic
  locale's `ctype<char>`.
  - Hardware execute-breakpoint counters on the title's own log sites showed
    "Failed to load any valid camera" and "Core camera settings ... missing"
    once per camera file (7 of 7), although every file was found. A gdb Python
    breakpoint on the definition loader's return dumped the parser error:
    `"identifier" must contain a namespace and a name, separated by a colon`
    for the namespaced death-camera identifier and the other six.
  - The camera schema validates the identifier with the regex `^\w+:\w+$`.
    The title's `regex_traits::lookup_classname` compares one-letter class
    names through `ctype<char>::do_tolower` (vtable +0x20). Kyty's classic
    facet implemented that slot as a no-op returning the facet pointer, so
    every comparison matched and `\w` resolved to the first one-letter entry,
    `d` (digits). Every camera identifier failed, no camera entity existed, and
    the client rendered from an identity view at the origin with the stick
    ignored: the frozen camera and the missing terrain recorded above.
  - Contract, from the vtable relocations and code of the bundled runtime
    (identical 12-slot `ctype<char>` vtable in all 25 readable bundled
    `libc.prx`): object 0x30 bytes with the classification table at +0x10, the
    lowercase and uppercase tables at +0x18/+0x20 and a delete flag at +0x28;
    slots +0x20/+0x28 `do_tolower` (char, range), +0x30/+0x38 `do_toupper`,
    +0x40/+0x48 `do_widen`, +0x50/+0x58 `do_narrow`. The facet now carries the
    C-locale case tables (shared with `_Getptolower`/`_Getptoupper`, now
    constant data instead of host `tolower`/`toupper`) and implements all eight
    slots.
  - Verified (Intel, strict, 240 s): no camera error site is reached, the
    title-screen panorama shows terrain, mobs and flowers for the first time,
    and world entry advances to a strict stop on a wave64 pixel shader that needs
    fragment wave transport (`mask observed as a number`), the same open
    feature as the card title.
- Fixed (2026-10-16): that stop was a false refusal of the fragment mask flow,
  not a need for the transport.
  - A transport dump (`KYTY_TRANSPORT_DUMP`) of the refused wave64 pixel
    program shows lane 13 of one VGPR used as two unrelated spill slots: a
    number at pc 0x914, read back at 0x9e8 and used by `v_mul_f32` at 0x9f8,
    then the saved EXEC high word at 0x1f14 (`s_and_saveexec_b64`). No back
    edge leads from 0x1f14 to before 0x9e8.
  - The flow kept one taint per (VGPR, lane) slot as a union over the whole
    program, so the later mask reached the earlier read.
  - Change: slot taint is part of the flow state on each path. A
    `V_WRITELANE` (which ignores EXEC) replaces it, joins take the union, and
    back edges carry it through the existing fixpoint. A slot written inside a
    lane-divergent region or a divergent loop is marked divergent after the
    join or exit, as a scalar word is; the old union never did that.
  - Verified: the dumped program is lane local in the mask flow and the
    wave64 admission proves it quad local, which needs no subgroup pinning.
    Intel, strict, 240 s: the title reaches gameplay in the generated world
    (HUD, hotbar, hand, mobs, sky and clouds) without a stop. Racing (200 s)
    and painterly (150 s) controls unchanged. Unit test
    `ASpillSlotCarriesTheWordLastSpilledOnEachPath` covers a reused slot, the
    loop-carried case and the divergent-region case.
  - Next observation: terrain blocks draw black (silhouettes only) at about
    3 FPS; fixed below.
- Fixed (2026-10-16): the black terrain was the block atlas mip chain that the
  CPU writes into guest memory and Kyty never loaded.
  - Observed: before the first draw into the atlas, the guest bytes already
    hold the whole four-level 2048x2048 chain. The 64 KiB occupancy follows the
    reverse-order 64 KiB layout (level 3 at 0, level 2 at 0x40000, level 1 at
    0x140000, level 0 at 0x540000 of a 0x1540000 chain). The color target
    (`MAX_MIP` 3, drawn only at `MIP_LEVEL` 0, about 160 draws per frame) was
    created as one empty level over the first 16 MiB and, being GPU owned and
    tiled, never loads guest bytes; only the animated strips reached it.
  - The sampled side had two gaps: `TileGetTextureSize2` sized a tile-27
    multi-level texture as one level (the fallback that ignores `levels`), and
    `Gen5PickSampleSurfaceAliases` rejected the one-level target for a
    four-level view, so the sample fell to `guest-skip-live-cover`, which
    zero-fills.
  - Change: a tiled color target with `MAX_MIP` > 0 and one slice is the whole
    64 KiB render-target chain (`Gen5GetRenderTarget64KBMipLayout`, from the
    element size). Its image has every level, is created from the detiled guest
    chain, and each pass renders the `MIP_LEVEL` view at that level's extent; the
    whole image stays a color attachment between passes. Mip chains are never
    resolution scaled. Tile-27 multi-level textures use the same chain layout
    and detiler, so the sample and the target describe the same range and the
    sample binds the target.
  - Verified (Intel, strict, 240 s): the generated world draws fully textured
    terrain (grass, dirt, leaves, birch, flowers) for the first time, at about
    3 FPS. Unit test `RenderTarget64KBMipChainMatchesSampledChain` pins the
    chain offsets observed in guest memory.
  - Excluded on the way: binding the one-level target anyway (diagnostic only)
    stayed black, since its level 0 lacked the static textures. A diagnostic
    that only cleared the live-cover flag was not a test of the guest bytes:
    the tile-27 upload policy still skipped the guest, so the sample stayed
    zero; forcing the guest upload of the chain showed the textured terrain.
- Defects found, not fixed:
  - Layered mip-chained targets (the 128x128 cubemaps, 6 or 18 slices, 8
    levels, drawn at every level 0-7) keep one level per slice, so every
    `MIP_LEVEL` lands on level 0.
  - Tile-27 storage images with several levels now take the chain size, but
    `StorageTexture.cpp` still detiles level 0 from the start of the range
    instead of its chain offset.
  - The inventory character preview draws legs and torso on black with the head
    detached; a dark streak crosses the sky in later captures. The pause-menu
    dressing room draws the same character correctly, so the defect is limited
    to the inventory preview.
- `KYTY_PRINTF_DIRECTION=Console` logs every HLE call: 3.5 GB in 270 s at
  about 3 FPS, and the world is never reached. Not usable for this title.
