# Sandbox title

Title: PPSA17221 - Minecraft.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

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
    - Guard: none exists to reuse. An in-place object's device reads guest memory when it executes
      (`StorageBuffer.cpp`, `MapBacking`), so a read-only bind would rest on the open reuse-signal
      question above. In-place objects also skip the overlay check (`GpuMemoryWriteback.cpp`,
      line 761), which a read-only bind would have to keep.
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
    recording was not built (design and bounds: `docs/bringup/parallel-recording-design.md`), and the in-place read-only binds above stay measured, not built.
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
      from the scene, at most about 0.6 ms;
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
    is under 1 ms; the alias write-back below is a new line that needs a design. Outside the ledger:
    a driver division loop under buffer copies took 0-7.5 ms per frame depending on the run
    (`docs/bringup/cross-title.md`, buffer copies with awkward sizes).
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
      processing and a 107 ms frame. The earlier estimate: with A and the three per-draw items of
      B about 44 ms of command processing is left, about 26-37 ms with parallel recording, and the
      frame is that short only with the gap overlapped (27-38 fps). No combination reaches 16.7 ms.
    - Full-stack check on the pushed code (2026-11-11, 64 KB stacks, 1,444 of 1,459 samples reach
      the thread start; 103 world frames of 97 ms while profiling, 4,995 draws, 71.2 ms of
      command-processor CPU time per frame against 80.1-82.5 ms in the earlier runs, not separated
      by cause): inside draws 63.2 ms (56.4 was the lower bound). No entry above is higher by more
      than 2 ms: read-only binds 15.4 (storage 7.8, index 5.0, vertex 2.6), vertex binding 8.4,
      descriptor sets 6.3, alias write-back 2.5, shader input 7.1, pipeline
      lookup without the identities 1.5 (building them, 2.8), driver calls 8.0 in all.
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
