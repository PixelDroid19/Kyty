# Kyty Bring-Up Manual (extended)

This is the extended strict-frontier manual: investigation loop, phase gates,
frontier history, and the auxiliary-agent handoff template. Consult it when
advancing the strict PS5 compatibility frontier. Day-to-day rules and
invariants live in the repository root `AGENTS.md`, which takes precedence for
process weight (do not apply this full loop to ordinary code tasks).

Note: frontier facts below are a snapshot — always re-capture the first strict
fail on the current HEAD before acting on them.

## Platformer pair: intro cinematics were black (2026-10-17)

Both Unity platformers showed black for their intro cinematics. HK's intro
ends in a second video, and after the first one the screen stayed black.
The contract, the guest evidence and the exclusion are in
`docs/avplayer-video-path.md` ("Jump and loop contract").

- **Cause, part 1: the jump while paused.** The guest prepares each video in
  four steps:
  1. play;
  2. take one frame;
  3. pause and jump to 0;
  4. wait up to 10 s for the jump warning and a frame with a new timestamp.

  `JumpToTime` only reset counters, and nothing was delivered while paused, so
  every video timed out after six frames and was stopped.
- **Cause, part 2: looping.** The guest enables looping and detects the end
  through the loop's backward timestamp. The real decoder never looped, so
  after the last frame the guest waited forever.
- **Fix:**
  - `Decoder::Seek` stops the decode worker, seeks the container, flushes the
    codecs and the resampler, and drops frames that end before the target.
    `EndOfStream()` stays false throughout.
  - `JumpToTime` seeks while it holds the player's decoder lock exclusively,
    restarts the media clock at the target, and opens a one-frame window
    while paused.
  - A looping real stream is rewound at its end.
- **Verified (Intel, strict, clean binary):**
  - Insect platformer, with a route that does not skip: video 1 plays all
    906 frames (0–29.9 s) and video 2 plays to 38 s. The title then reaches
    gameplay with the HUD.
  - The sequel's 79.6 s intro plays to the end. The starting area then loads
    in about 20 s and reaches gameplay.
  - Normal play route: insect platformer 95–315 fps, sequel 76–294 fps
    (capture method).
  - Delivery is paced by the guest at the media rate: 33 ms per frame at
    30 fps, 40 ms at 25 fps.
  - `EmulatorAudio.*`: 121 tests pass, including the new seek test, with a
    local synthetic clip in `KYTY_AVPLAYER_TEST_MEDIA`.
- **Regression set.** Titles that import the jump or loop NIDs and ship video:
  - Puzzle-platformer: 158–236 fps, no regression.
  - Garden title: 74–295 fps, no regression.
  - First-person puzzle title: stalls early, at frame 177 on a
    `gfx_storage_frontier` refusal. Earlier runs stalled at frames 92, 332
    and 134, so the stall predates this change.
  - Brawler: stops at 61 s on a read at `0x58`, with the same crash signature
    as 5 earlier runs since 2026-10-14. Pre-existing.
  - Other importers ship no video files.
- **Exclusion:** delivering the jump warning on the next guest call instead of
  synchronously changed nothing.
- **Not run:** AMD (the iGPU still needs a host reboot);
  `kyty_playable_regression.py` (gameplay checked from captures only). The
  regression set came from a static scan for video files. A title that keeps
  video in compressed bundles would not show up in it. That class is left to
  the next main sweep.

## Platformer pair: remaining device-address waits and boot time (2026-10-17)

Measured on 9988930f (Intel, strict) after the two changes below. No code
change came out of it. These are the results and the exclusions.

- **Device-address draw waits are one storage class.** After the index
  change, the sequel's dominant CPU wait is still the device-address draw
  retry: `CommandProcessor::WaitSubmission` from `DrawIndex`, 11.6k waits and
  8.6 s in a ~100 s window (symbolized fence stacks). Present waits were
  2.4 s and texture fills 19 ms.
  - A refusal census (opt-in counter in
    `GpuMemory::PendingStorageWriteBack`, one 200 s play run) recorded 20,000
    refusals, all on one 32 KiB storage buffer crossing a depth target on the
    graphics queue:
    - 14,457: the only failing condition was the overlap link (`others`
      non-empty).
    - 5,542: the overlap link, and the storage was also bound as that
      target's HTILE (`depth_meta_bound`).
  - Caveat: the counter fires on the first non-publishable object and
    returns. Other pending storage behind it was never counted, so publishing
    this object alone is not proven to unblock the draw.
  - Decision: left waiting. Device publication skips the completion
    write-back, and HTILE clears are recognized from the bytes at write-back
    (`DepthMetaMarkClear`). Both adopt paths refuse `depth_meta_addr`
    deliberately. Moving that recognition touches every title with depth
    (the racing title's HTILE clear fix included), for at most ~9% of CP
    time on titles already at 86-127 fps.
  - Open question: in the first class, the storage crosses a depth target
    but is not bound as its metadata. Either it is the same HTILE range and
    the binding missed it, or it is a different range. If it is the same
    range, write-back and flush never signal clears for it.
    - The binding sites are `GpuMemoryCreate.cpp` near 2712 (binds only
      when the depth target exists at storage creation) and near 2125
      (binds only under `multi_depth_stencil_reclaim`).
    - Direction: add the storage address and the parent's
      `PARAM_HTILE_ADDR`/`PARAM_HTILE_SIZE` to the census key, and iterate
      past the first blocker.
- **Boot and loading are not emulator-bound on Intel.**
  - Timeline (sequel, cold caches, 120 s run with captures every 600
    frames): title menu at ~21 s after fixed-length splash screens.
    New-game loading screens last 3 s and 7 s. Act title card at ~100 s,
    after the route's first-boot settings screens.
  - Per-thread CPU every 5 s over the same run: no thread above 45% of a
    core during boot or loading.
  - Shader work in the first 25 s window: 40 SPIR-V compiles in 0.67 s,
    78 pipeline creates in 0.18 s, pipeline misses 0.9 s in all. The insect
    platformer is the same: 37 compiles in 0.66 s.
  - File reads go through the AMPR command path (`Ampr::ExecuteRead`).
    Timing counters over one run:
    - Boot: 2,554 reads of 1.6 GB in ~16 s. Time inside `ExecuteRead` was
      1.9 s: host-write lease begin and end 1.1 s
      (`GpuDirtyPageTracker::BeginHostWrite`/`EndHostWrite`, two page walks
      per read), open and path resolution 0.4 s, the read itself 0.38 s
      (page cache).
    - New-game load: ~700 reads of 400 MB, 0.33 s in all.
  - Fixed, the host-write lease cost. Every span over 64 pages scanned the
    whole 262,144-slot dirty-page table, once in `BeginHostWrite` and once in
    `EndHostWrite` (~0.2 ms each), whatever the read size. Spans up to 2,048
    blocks now read one 64-page block summary (`page_mask`) per block and
    look up only the pages it lists. A block whose lookup passes the probe
    limit falls back to per-page lookups for that block. Larger spans keep the
    single table scan. Sequel boot, 120 s, same route, ~3,370 reads of
    2 GB: lease 703 -> 4 ms, release 771 -> 0 ms. Regression at 200 s: the
    sequel 81-82 fps in play, the insect platformer 102-105, the fishing
    dock 21-22, the card title correct; all reach the same state as before.
  - Not verified: the slow loading the maintainer reported was seen on the
    AMD iGPU (14-26 fps in play). If the guest's loading work is
    frame-paced, it scales with frame time there. The iGPU needs a host
    reboot before any AMD evidence (see the regression sweep below).
  - Probe patches (local only): `diag-gdacensus-20261017.patch`,
    `diag-amprio-20261017.patch`.

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

## Main regression sweep after the #18 merge (2026-10-17)

Build: `main` at 71814344, strict runs, one title at a time, scripted input
routes, 100-240 s each. Hosts: Intel Arc A770 (Xe KMD, Mesa) and the AMD
Raphael iGPU (RADV, 2 CUs). Frame rates are frames between two native
captures divided by the seconds between them, taken in the gameplay window;
single `status` samples are instantaneous and swing widely (an earlier
"86 fps" for the atmospheric platformer was such a sample; its gameplay
window averages 59-61 fps on Intel and 30-31 on AMD on both builds).

No title regressed against its previous strict run. Reached gameplay on
Intel: card title, platformer pair, racing title, Unity fishing title, beat
'em up, sandbox title, roguelite action platformer, watercolour platformer,
isometric roguelike, gothic action platformer, dream platformer, zoo title,
preschool Unity title, pixel exploration title, artillery title, roguelike
dungeon crawler and tower-defence title. The ninja platformer's generic route
stops at its title screen, the anime fighting title stays in its story intro
and the role-playing title reaches the traveler selection, as before.

Open defects found by the sweep, none of them caused by the merge:

- Intermittent early stall (both hosts). Presents stop with no fatal error
  and `phase=stalled`: the ninja platformer on Intel at frame 3812, the
  gothic action platformer on AMD at frame 391. The rerun of each reached
  its usual state (2 stalls in 45 runs). No live evidence yet: the harness
  queried the agent only after exit. Next evidence: `threads`,
  `sync-waits` and `events` taken while `phase=stalled`.
- Gothic action platformer, AMD only: every capture is black for 240 s while
  the title runs at 53-64 fps in `phase=interactive`; one capture shows a
  single large grey triangle. Intel shows gameplay on the same route. First
  AMD run of this title, so there is no AMD baseline. Evidence so far:
  both hosts present `B8G8R8A8_SRGB` 1920x1080 and the AMD frames are RGB
  zero, so readback is not the cause. The grey triangle is a flat white
  triangle with one vertex at the screen centre and two at the right corners,
  a fullscreen pass with one collapsed vertex. The translated SPIR-V of every
  shader is identical on both hosts except for baked guest-address
  constants, so the difference is in how the host runs the same module.
  `RADV_PERFTEST=gewave32` and `gewave32,pswave32,cswave32` kept the screen
  black (stages whose subgroup size Kyty pins to the guest wave are not
  affected by that variable), but those runs came after the gfx ring timeouts
  described below, so this exclusion is void until it is repeated on a
  freshly booted host. The geometry shader synthesizes the
  guest NGG wave info (`s3`) and `exec` from the host subgroup ballot; next
  evidence: the fullscreen vertex shader's per-lane `gl_VertexIndex`,
  `gl_SubgroupInvocationID` and output position on AMD for one draw.
- Excluded: the artillery title's white and blue terrain on AMD is a
  randomly generated arctic map (falling snow, different team names), not a
  colour error; the Intel run drew a stone map. Compare like for like before
  treating a terrain difference as a defect.
- Puzzle title and role-playing title, AMD only: device lost
  (`vkQueueSubmit` result -4) on queue 8. On Intel the puzzle title stalls at
  boot frame 332 without a capture, as in all earlier runs.
- **AMD runs after a gfx ring timeout are invalid until the host reboots.**
  - What happened: the kernel log shows six `ring gfx_0.0.0 timeout` events,
    each followed by a ring reset and "device wedged, but no recovery needed".
    Five came from the puzzle title's run (05:35Z) and one from the
    role-playing title's run (05:38Z), the two device-lost runs above.
  - What followed: every later AMD run rendered wrongly while the title kept
    running. The card title, clean at 45-51 fps before the timeouts, gave
    only black captures at about 78 fps. The atmospheric sequel, which
    reached its start room cleanly before the timeouts, drew its intro, title
    card glyphs and first room as stretched, tilted polygons in two
    consecutive runs. Intel runs of the same binary in the same hour were
    clean.
  - The binary differed from the sweep build only by a diagnostic sink that
    is disabled without its environment variable, so the change is the host
    state, not the code. It is not yet confirmed by a clean run after a
    reboot.
  - Consequences for this sweep: the anime fighting title's AMD row was taken
    after the timeouts and must be repeated. So must the AMD runs behind the
    gothic platformer's `RADV_PERFTEST` exclusion. Its black screen itself
    predates the timeouts (05:16Z run).
  - Rule: before reading an AMD result, check
    `journalctl -k | grep 'ring gfx.*timeout'` since the last boot. Run titles
    that lose the device last in an AMD sweep, or alone.
- Role-playing title: a route that starts a new game now gets past the title
  and stops at a missing `ImageSampleCd` emitter (pixel stage) while loading.
  Its title screen background is flat magenta on Intel; earlier captures of
  that screen were dimmed by a loading overlay, so it is not known whether
  this is new.
- AMD routes: the input routes were timed on Intel, so on the slower AMD host
  the preschool Unity title ends in its pause menu, the tower-defence title in
  its name entry and the roguelike dungeon crawler in its dark first room.
  Their AMD frame rates are menu or intro rates, not gameplay.

## Platformer pair: unreadable glyphs were dropped command-buffer packets (2026-10-17)

- Observed (Intel and AMD, strict): menu and HUD text in both platformers drew
  as solid blocks. The runtime glyph atlases (one 512x512 and several 256x256
  one-byte textures) kept the engine's `0xcd` creation fill through gameplay.
- Root cause: the engine's texture upload path was traced on the guest side
  (insect platformer, function breakpoints counted with a bounded script).
  It ran 353 times with no error branch, and every upload reached its copy
  dispatch recorder. The command processor still executed only one copy
  dispatch, and no dispatch bound an atlas. The engine replays those copies
  into a fresh Agc command buffer whose four pointers (bottom, top, cursor
  up, cursor down) are all null and whose out-of-space callback is set. The
  guest callback handles a null bottom by handing over the first block.
  `CommandBuffer::IsValid` rejected the all-null buffer, so `ReserveDW`
  never called the callback, and `sceAgcCbDispatch` and its siblings
  returned null. Every packet written to a fresh buffer was dropped without
  any error.
- Correction: the earlier conclusion of an "upload never submitted /
  downstream DCB splice" was wrong. The old per-thread tracker compared
  HLE-side addresses with the command processor's copy of the buffer.
- Changed (`Graphics.cpp`): an all-null buffer is valid with zero capacity,
  so the first reservation calls the callback. `GetAvailableSizeDW` clamps
  the reserved tail instead of wrapping. Tests
  `RefillsEmptyCommandBufferBeforeFirstPacket` and
  `RejectsEmptyCommandBufferWithoutRefillCallback` cover the contract.
- Unblocked by the fix: two strict stops that the dropped packets had hidden.
  1. The sequel's startup compute kernel (`1,1,1` and `5,1,1` groups) runs
     `ds_write_b32` with the GDS bit set. M0 is `s_bfm(2,14)`, that is base
     0 and size 48 KiB (RDNA2 ISA §3.7 and §10.4.3: address = M0.base + ADDR
     + offset; an out-of-range or misaligned GDS access is a memory
     violation, §3.11). The parser had decoded it as an LDS store, so the
     paired-wave path refused it. It now has its own format,
     `VaddrVdataOffsetGds`. The GDS buffer is bound whenever a program uses
     GDS memory through a constant M0 (`ShaderGdsBufferBound`). The
     paired-wave emitter stores per lane only when the lane is active, both
     ADDR and ADDR + offset are below M0.size, the byte address is dword
     aligned, and the index is inside the host GDS window
     (`kGraphicsGdsDwords`). The native path refuses the instruction rather
     than lowering it to LDS. Tests: `PairedGdsWriteStoresOnlyInsideM0AndTheHostWindow`
     and `NativeGdsWriteIsRefusedNotLoweredToLds`.
  2. At about 133 s the sequel stopped on `DCC decompression requires an
     existing render texture`. A colour DCC decompress (CB mode 6, op 0xcc)
     now reached a display buffer; before the fix that packet had been
     dropped. The host display image holds expanded colour exactly like a
     host render texture, so the decompress keeps its pixels. It only checks
     that the display image is registered.
- Verified (strict, bounded runs):
  - Insect platformer on Intel, 150 s: readable menus, HUD in play, 86 fps.
  - Insect platformer on AMD, 200 s: 26-28 fps in play.
  - Sequel on Intel, 150 s: readable title menu and calibration screens,
    reaches gameplay.
  - Sequel on AMD, 200 s: readable text and HUD in play, 14-26 fps.
  - Regression on Intel: the fishing title (150 s, dock with HUD and tutorial
    text) and the racing title (125 s, in race).
  - Full unit suite: 2445 pass. Both gate scripts pass.

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

## Platformer pair: open defects seen after the glyph fix (2026-10-17)

Recorded so that each one starts from evidence, not from scratch. None of them
is attributed yet.

- **Black region that does not fade (sequel, also likely the insect
  platformer).**
  - Symptom: walking right from the sequel's start area, a large opaque
    black shape covers half the screen. The shape has scalloped, terrain-like
    edges and a soft teal fog drawn over its lower part. The player walks
    into it and disappears, which makes the area unplayable. The insect
    platformer's first area also shows a large black region, but that area
    legitimately has dark foreground silhouettes. Do not chase it without a
    reference image of the same spot.
  - When it was found: the maintainer saw it on the current tree. It predates
    the glyph fix; earlier scripted routes never left the start area, so
    captures did not show it. The scripted platformer route walks right only
    from about 130 to 132 s, then back left. To reach the region, walk
    right longer.
  - Hypothesis, not verified: both engines cover hidden or adjacent rooms
    with black mask sprites whose alpha the game fades to zero when the
    player enters. A mask that stays opaque points at the fade never reaching
    the GPU: stale vertex colour, or a stale per-renderer colour constant.
    The alternative is a missed draw that should cut the mask out.
  - Next evidence: during the scene, query `kyty_agent events` while the
    process is live for `draw_skipped`. The harness only queries after exit,
    which returns `endpoint_not_live`. Then dump the draws of one frame with
    `KYTY_DUMP_DRAW=all` and `KYTY_DUMP_DRAW_FRAME=<n>-<n>` to find the mask
    draw: its blend state, its vertex colour source, and whether its inputs
    change across frames.
  - 2026-10-17, Intel, what was measured:
    - No draw is skipped in the dark scene: the events ring had
      `dropped=0` and no `draw_skipped`.
    - Frame structure from the draw dump: the game first renders the far
      layers into a 640x360 target and blurs it in ping-pong passes. It then
      draws that target onto the 1920x1080 scene as an opaque, depth-tested
      10x10-unit plane mesh (121 vertices, 600 indices). The sprites follow,
      sorted far to near.
    - The plane draw, its texture and its 16-byte constant (all zero) are the
      same in the bright start room and in the dark room. The 640x360
      targets hold the blurred far layers in both rooms.
    - The dark framing reached by walking right is a small side cave. The
      camera follows the player into it, and the frames stay static while
      the player stands against its far wall. This alone is not shown to be
      wrong.
  - The maintainer's screenshot is different: a soft-edged black mass over a
    lit forest, with the player at the right edge. It was taken on the AMD
    iGPU before the ring timeouts recorded above, and it is not yet
    reproduced on Intel. Next: reproduce it on AMD after a reboot, compare
    the black-with-alpha shadow sprites (vertex colour (0,0,0,199..251), BC7
    atlas) with the same draws on Intel, and match them by index count and
    texture.
- **Overlapping text (both platformers).** The maintainer reports that some
  texts sometimes draw over each other. The sequel's opening credits fade
  names in and out over gameplay, so first check whether two fading strings
  overlapping is the title's own transition. No capture isolates it yet;
  capture the moment before attributing it.
- **Slow loading screens and first boot (sequel).** First start and some
  loading screens take much longer than later ones. Not measured yet. The
  persistent SPIR-V cache and the Vulkan pipeline cache store (`PipelineCacheStore.cpp`) both exist, so the first step is to
  separate translation and pipeline creation from guest file I/O, by
  comparing a cold-cache boot with a warm-cache boot on the same binary.
- **AMD frame rate (both).** In play the insect platformer runs at 26-28 fps
  and the sequel at 14-26 fps on the integrated RDNA2 GPU. On Intel the
  insect platformer runs at 86 fps. Whether AMD is bound by the GPU or by the
  command processor CPU is not measured yet.

## Platformer pair: index range over a depth target (2026-10-16)

- Observed (Intel, strict): both platformers stopped intermittently on
  `unknown relation: DepthStencilBuffer - Crosses - IndexBuffer`, a small
  index range (12 or 0x1e0 bytes) over memory still tracked by an active
  depth target. The vertex rule already links the same Crosses form.
- Changed: `GpuMemoryAllowsIndexContainedInSurface` links an index buffer
  that crosses a depth target; Contains still refuses.
- Verified: the insect platformer passed the former 73 s stop and reached
  gameplay on Intel (200 s, 72-103 fps in play) and AMD (200 s, 26-30 fps
  in play). The sequel reached gameplay on Intel (150 s, ~64 fps in play,
  bounded diagnostics built in) and AMD (220 s, 23-25 fps). Menu text in both still
  draws as solid glyph blocks (graphics handoff, open).

## Card title: streamed music and an offline service retry (2026-10-16)

- Observed (Intel, strict, card title): music and most effects were silent or restarted
  constantly. The audio stream thread was recreated about 1225 times in 40 s. Long runs stopped
  after about four minutes on a full guest thread registry.
- Evidence (live trace of the NGS2 calls): each music voice adds one PCM block with blocks flags
  1 (continue), a sample count covering the whole song and `data_size` 16384, then appends pieces
  of 7.7-8.9 KiB with flags 3 (continue and append). It plays the voice before the first add and
  reads the 56-byte voice state. Kyty refused flags 1 and 3, refused play on an empty voice and
  refused a stereo mastering voice rendering into an eight-channel buffer.
- Change: PCM sampler blocks stream (see `docs/AUDIO.md`, NGS2 sampler voices), play is accepted
  before any block, a stereo mastering voice writes the front pair of an eight-channel buffer, and
  GetState takes the 56-byte layout.
- Evidence (thread leak): the title's network module retries its service init once per second
  while the network state is offline. Each call builds a new activity manager whose event-queue
  thread waits on a condition forever; the old manager is dropped. One blocked guest thread per
  second is guest behavior when offline. Kyty's 256-entry guest thread registry aborted when full,
  although the host thread-local flag already marks guest threads; the registry is removed.
- Verified (Intel, strict, card title, 330 s, in-emulator RMS tap): the mixed output is audible in
  328 of 329 one-second windows, the stream thread is created 14 times in 90 s, and the run
  holds 348 live threads with no abort. Menus run at about 235 fps and an in-game hand at about
  100 fps. Unit tests: `CoreMemoryAlloc.*` and `EmulatorAudio.*` (121 pass).
- The white hovered card seen in one capture is the title's own focus flash (see the graphics
  handoff, Evidence and exclusions), not a rendering defect.
- Regression check (Intel, strict, 100 s, `generic` route): a pixel-art title that imports NGS2 is
  audible in 92 of 97 one-second windows (52 of 95 in an earlier run) and reaches its
  opening scene with no error.

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

- Observed (Intel, strict, `minecraft-enter`): in a newly created sandbox-title world, the run
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
- Run (Intel, strict, `minecraft-enter`, 300 s, with the compare fix below): the world loaded and
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

- Observed (Intel, strict, `minecraft-enter` route): in sandbox-title gameplay, plants and leaves
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
- Verified (Intel, strict, `minecraft-enter`, 300 s): the run opened a newly created world and
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
  Unit test `SamplesThroughAllZeroDescriptorReturnZero` covers the parse,
  the emitted zero stores and a T# rewritten before the read.
- Named-DMASK sample formats leave `mimg_dmask` unset; the zero read uses
  the decoded destination size, which every MIMG format sets to the DMASK
  component count.

## Sandbox title: drifting dark patch on terrain (2026-10-16, open)

- Observed (Intel, strict, `minecraft-enter` route, no input after the spawn): from about frame
  10,600 on, a dark translucent quad lies on the grass in front of the player. It moves and grows
  between captures (frames 10,692, 10,854 and 11,016). Earlier frames on the same route do not
  show it.
- It also appears in a run at 97034ebc (frame 11,400), before the index-snapshot and vertex-tail
  changes, so it is not caused by them.
- Not yet known whether this is a guest effect or a rendering error. Suggested direction: confirm
  against reference footage of the same world type first. If it is an error, take a native capture
  at the frame and use the operation trace to find the draw that writes the quad (likely a
  translucent or shadow pass), then compare its inputs with a frame where the quad is absent.

## Insect sequel: intermittent depth/index overlap (2026-10-16, open)

- A 150 s Intel run reached gameplay (start of Act 1) without the earlier
  `unknown relation: DepthStencilBuffer - Crosses - IndexBuffer` stop (a
  12-byte index read inside a live 1920x1080 depth buffer). The stop is
  intermittent; overlap diagnostics now print every parent range to show
  which plane it crosses when it recurs.

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

## Survival title: hosting an offline world (2026-10-15, open)

- Symptom: Host Game showed "Network Connection Lost". With the connection
  check passed, it showed "Launching Server", then "Server Failed To Start",
  and on retry "Networking Failed To Start". The title's offline mode is a
  LAN-only server running in the same process; it reaches it through
  `SOCK_DGRAM_P2P` sockets on the loopback.
- Fixed, in order of discovery:
  - `sceNetInetPton` left the address in host byte order, so the title's
    self-addressed datagram went to the wrong host. It now follows the BSD
    contract and returns 1, 0, or an error.
  - P2P datagram sockets (type 6) carry a virtual port at bytes 8-9 of the
    address. Two server sockets bind the same P2P port with different virtual
    ports. Each now gets its own host UDP port, and a process-wide endpoint
    table routes local sends.
  - **Root cause of "Server Failed To Start": `sceNetGetSockInfo`.** A bounded
    HLE trace showed the server threads calling only `sceNetGetSockInfo` in a
    loop for about 2 s, then tearing the server down. The guest's loop expects
    three things:
    - the number of 160-byte records as the return value;
    - the record whose socket id at 0x24 and type at 0x28 match;
    - state 2 (opened) at 0x44.

    Until then it calls `usleep(1000)`, up to 2000 times. The old
    implementation returned 0 and a host sockaddr. With records and states
    reported, the call succeeds once and the server starts.
  - POSIX `sendmsg`/`recvmsg` were ENOSYS stubs; they are now implemented.
    This was a defect, not this title's blocker (see the exclusions).
  - World generation prints progress dots through `std::cout`. The exported
    `std::cout`/`std::cerr` were zero-filled, and the guest's inlined inserter
    faulted reading the basic_ios offset from a null vtable. Both are now
    constructed with the layout taken from the guest's inlined inserter,
    `std::endl` and `basic_streambuf` constructor. The classic ctype facet's
    `do_widen` also returned 1 for every character; it now returns its
    argument.
- **Local-only network contract (NetCtl).** `sceNetCtlGetState` reported
  Disconnected (0) while `sceNetCtlGetInfo` returned the loopback address as
  the assigned IP. With Disconnected the title never launches its LAN server
  ("Network Connection Lost"). The emulated network is local only: the
  loopback address is assigned, P2P datagrams are routed inside the process,
  and nothing leaves the host. NetCtl now reports that state consistently:
  IP-obtained (3) and `GetInfo(LINK)` connected (1). The address stays
  127.0.0.1 because P2P routing is process-local. Verified with strict runs
  on the current build: this title reaches day one (86 us per draw, 9.7 fps,
  same as with the earlier diagnostic override). Other titles that import
  `sceNetCtlGetState` or `GetInfo`: the sandbox title enters its world at the
  same capture as before (two runs; a bounded HLE trace of the load showed a
  handful of socket calls within 1 ms), the beat 'em up title reaches
  gameplay, the racing route runs at 45-47 us per draw, and no
  `NetCtlGetInfo` unknown-code exit appeared. The UE4 brawler title still
  stops before its first frame on the unsupported volume layout refusal
  (`400x5x5`, tile 27) that an older build already hit.
- Excluded: `poll`, `recvmsg`, `sendmsg` and `recvfrom` were not called while
  the server was starting (DIAG trace during "Launching Server"). The
  `KernelPoll` "every fd ready" answer was not involved.
- Verified live (Intel Arc A770): the server starts and its sockets exchange
  datagrams over the loopback. World generation runs past the former
  `std::cout` fault and completes. Survivor select and day one of the world
  are reached about 3 minutes after boot in a strict run, with the player
  character moving (input from a person and from the agent pad).
- Harness memory limit, not the title: the run harness capped the cgroup at
  `memory.high` 7G with no swap, and the title keeps about 7.2 GB of guest
  shared memory. Generation then sat in `__mem_cgroup_handle_over_high`
  (full memory pressure about 27%), with presents stalled for up to 48 s.
  With 12G/13G the pressure is zero and generation takes about a minute.
  Heavy titles need `KYTY_RUN_MEMORY_HIGH`/`KYTY_RUN_MEMORY_MAX` above the
  default.
- Draw cost in the world (day one, 20 s windows, about 850 draws per frame):
  - Before 40c887ad: 128 us per draw, 8.2 fps. The command-buffer
    transient pool filled at 15.7 MB, all of its 1,970 entries in use, with
    vertex snapshots of up to 120 KB. Refused index ranges became GpuMemory
    objects whose `UtilCopyBuffer` upload waits on its own fence: 94 fence
    waits and 97 submits per frame.
  - With a 64 MiB pool (40c887ad): no refusals for lack of space, peak
    31.4 MB per pool, 5 fence waits and 8 submits per frame, 89 us per draw,
    9.9 fps. Anonymous memory did not grow (1.33 against 1.44 GB).
  - Same build, other titles. Racing route: 45-48 us per draw against
    47-48. Fishing route: about 15 fps in both, four boots of 45 s pass.
    That fishing title still fills 64 MiB, with vertex snapshots of 2.4 MB.
    Beat 'em up route: captures identical to the previous run.
- Fence waits in the world (fixed in e50014c6 and ddd43522):
  - Attribution (temporary capped diagnostics, removed): about 99% of fence
    time was the device-address draw retry
    (`CommandProcessor::DrawIndex` → `RetryGuestDeviceAddressDraw` →
    `Gpu::WaitSubmission`). The pending storage came from a 64x1x1 compute
    fill with 16-byte records whose value is uniform per dispatch.
  - Ceiling (diagnostic only): forcing device-side publication of every
    uniform fill took 86 to 40 us per draw and 9.7 to 11.1 fps, so the
    retry, not the draws, set the frame time.
  - Root causes:
    - `GpuMemory::QueryRangeProvenance` walked at most 64 candidates over
      1 MiB buckets and reported `truncated` for the 3840x2160 render target,
      which has two real overlaps. The uniform-fill identity then refused at
      random. The query now reports the exact `FindBlocks` overlap set and is
      truncated only above 16 entries.
    - Display buffers (`VideoOutBuffer`) that equal the fill range were not
      admitted as color-image aliases.
    - Fills of a depth target's depth plane or stencil plane were never
      admitted by `MarkStorageGuestPublished`, and the depth plane was never
      propagated. The depth-stencil update function is a no-op and a tiled
      display buffer upload returns early, so invalidating those parents
      after a write-back had no content effect. Propagation is what gives
      them correct content.
  - Fix: exact provenance; display buffers join the uniform-fill image
    aliases; a uniform depth fill (32-bit words, value in [0, 1], D32 depth
    formats, address equal to the depth plane, image already initialized,
    no sample-location state) clears the depth aspect on the device; depth
    and stencil plane propagation stamps the device write time
    (`GpuMemoryResetPlaneHash`); publication admits an equal color image or
    a depth plane (not the HTILE plane) only when its device write is newer
    than the guest write.
  - Result (day one, Intel Arc A770, strict): 86 to 35.5 us per draw, fence
    time 40 to 8.6 ms per frame, 9.7 to 11.8 fps (p50 86 ms). Captures are
    equivalent to the previous build.
  - Regression, same binary: the 2D action title's walk route renders
    correctly (about 134 fps, 67 us per draw); the beat 'em up reaches combat
    with correct captures (163-179 us per draw against 167-169); the sandbox
    title enters its world with the HUD (29.6-35.4 against 31.7-38.0 us per
    draw, terrain still missing as before); the racing route renders the
    road correctly (44.8 against 47.0 us per draw at about 480 draws per
    frame); the fishing
    dock is unchanged visually at 44 us per draw and about 17 fps.
  - AMD (integrated Radeon, same binary, strict): day one is reached and the
    captures match the Intel ones. 31 us per draw, 2 fence waits for 3.3 ms
    per frame, about 10 fps with a 98 ms median frame; the command processor
    takes 30.5 ms of it while `gpu_busy_percent` averages 77% over 40 s, so
    on this host the frame is bound by the GPU at 3840x2160.
  - Still open in this title: a 1920x1080 zero fill contained in a texture
    still refuses (about 8.6 ms per frame of fence waits remain). The frame
    is not bound by the command processor: a sampler at day one shows the
    processor thread about 33% busy and the main guest thread about 10%,
    with three guest threads spinning on `gettimeofday`. The title renders
    at 3840x2160; GPU time is the next measurement.
- Open:
  - **World generation speed:** the 1 fps runs were the harness memory limit
    above. Three guest threads still spin on `gettimeofday` with nanosleep;
    they cost CPU but did not block generation.
  - `std::cin` (imported) and the 104-byte versioned `cout`/`cerr`/`clog`
    objects are still zero-filled. Their layout is a different ABI without
    evidence yet.
  - These are rejected and may be defects:
    - `SO_LINGER` with a 4-byte value (EINVAL);
    - `IPPROTO_IP` option 2 on UDP (ENOPROTOOPT).

    The title ignores both results.
  - Imported but never called:
    - `sceNetGetMemoryPoolStats`;
    - `sceNetCtlGetResult`;
    - `sceNetCtlUnregisterCallback`;
    - three unnamed NIDs.
  - P2P peers outside this process are not routed.

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
    for `minecraft:death_camera` and the other six.
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

## Robot platformer: title screen save cards (2026-10-14, open)

- Observed (Intel A770, strict, `generic` route, 390 s, no error): the title
  screen shows the warp background, the logo and the top save card with its
  "NEW GAME" pill. The two lower save cards stay black and a faint second
  logo remains. Reference imagery of the same screen shows three lit cards
  and no second logo.
- Localized: the tiled light classification dispatch (120x68 groups) writes
  an empty light list for every tile. Its output was read through the
  current-guest-bytes path and is a fact. An offline evaluation of its inputs
  passes 0 of 8,040 tiles; the angle term is the failing predicate because
  the half-resolution normal buffer read back as a constant flat normal.
  That readback is not reliable (next item), so constant normals are an
  unverified cause.
- Diagnostic caveat: the debug image dump (`UtilDumpVulkanImageRgba8Png`)
  reads back through its own utility command buffer and does not submit the
  recording in progress, and the per-frame bound-image dump is overwritten
  by the last bind of the frame. State read this way shows an earlier point
  of the frame, not the bind being studied. Use `docs/graphics-captures.md`
  or a readback at a submission boundary when in-frame order matters.
- Observed only through the debug image dump (not in-frame ordered, so
  not exclusions): the temporal antialiasing output and its history image
  are already black on the lower cards, and so is the HDR scene target
  except near the ring.
- Excluded:
  - The stencil plane read by the lighting passes, as cause of the black
    cards and the second logo. The guest clears the plane (2048x1280, 8-bit)
    with the uniform fill kernel each frame. Its stencil writes on that depth
    target use ALWAYS, REPLACE_TEST 128 and write mask 0x80; a capped probe
    of every depth state on the target over 200 s saw no other writing
    state. Four lighting dispatches sample it; the half-resolution normal
    encoder and the light classifier do not read it at all, so the empty
    light lists do not depend on it. With d72457e5 (fill clears
    the host stencil, confirmed on every frame) and 55cd634d (the 8_UINT
    sample binds the device stencil, confirmed by the sample bind catalog),
    the screen is unchanged on Intel.
  - Ordering between the dispatches: memory barriers separate them in the
    command stream.
  - Translation of the classifier's control flow switch, `ds_append`,
    `vcc_hi` and `v_mbcnt` (checked against the ISA).
  - A render-target/storage alias of the normal buffer: its writer and all
    four readers bind the same exact storage view, with no older alias.
  - The G-buffer pass: MRT export translation (compressed, MRT0-4), per-MRT
    write masks (0xf, blending off) and depth-stencil state (LEQUAL with
    write) are correct. Its stencil state is the ALWAYS / REPLACE_TEST 128 /
    mask 0x80 state above; an earlier note of EQUAL, REPLACE, reference 4 is
    not supported by the register probe.
  - An earlier exclusion that read a 1x1x1 indirect dispatch as "counter 0"
    was flawed: it used stale indirect arguments (fixed below).
- Changes made while localizing (none changes this screen visibly):
  - c8e3537c: an exact storage texture lookup returns the view with the
    newest storage write among all views of that range, instead of the first
    listed one; a recycled range can hold views in several formats.
  - 6d1b178d: dispatch-indirect arguments are read only after any GPU writer
    in the recording or on another queue has published them.
  - 55565820: a depth-only clear of a D32S8 target loads stencil instead of
    discarding it (depth and stencil load operations are independent).
  - 18d6e5be: HTILE clears done by the generic uniform fill kernel are
    published in recording order, and the clear word `0xfffffff0` (ZMask 0,
    maximum Z 1.0 in both HTILE layouts, SMem 3 with stencil tiling) is
    translated to a depth clear when DB_DEPTH_CLEAR is 1.0. On this screen
    the G-buffer depth clear (D32S8) is now consumed at the next bind; D16
    and D32 targets translate as well.
- Verified on the final tree: robot platformer (Intel, 390 s) same state, no
  error. Racing title (Intel, 125 s twice and 200 s): no error, in-race
  present rate 38-40, inside the 34-45 range of the 13-14 October runs; a
  temporary counter showed it never takes the dispatch-indirect path. 2D
  action title (AMD, 120 s): no error, gameplay without trails or white-out,
  29-30 presents/s. Fishing title (AMD, 200 s): no error, reaches frame 3,093
  against 2,008 for the earlier baseline in the same time, 5-8 presents/s
  against 4-6. Watercolour platformer (AMD, 90 s): same captures and rates
  (72.5 and 38.5). Unit tests: 2,419 pass. Table and boundary gates pass.
  `kyty_playable_regression.py` reports `honest_skip_no_guest_root` here (no
  local profile or guest root); it must run without `python3 -I` because it
  imports a sibling module.
- Robot platformer on AMD (390 s, no error) on d72457e5..5236078d: 3.3-4.2
  presents/s on the title screen, then about 0.4 once the next scene loads,
  as on 13 October; it reaches frame 888 at the seventh capture against 865
  before, and the last captures show the tunnel instead of black. The
  racing, 2D action, fishing and watercolour titles show no regression on
  that range (in-race 34-43 on Intel; 28-29, 5-7 and 68/43 presents/s on
  AMD).
- Open defects found here:
  - A guest compute fill of the stencil part of HTILE is not applied to the
    host stencil plane: `DepthMetaComputeFillTranslatable`
    (`GraphicsRenderAttachments.cpp:500`) only translates depth. This title
    clears the stencil plane itself and its HTILE word marks stencil
    expanded (SMem 3), so no SMem-cleared word has been observed yet.
    Direction: translate SMem-cleared words to a stencil clear when the
    stencil clear register agrees.
  - `FindDepthStencil` (`GraphicsRenderDescriptor.cpp:1535`) asks for the
    first object only, so a recycled range with depth views in two formats
    can bind the stale one, as storage textures did before c8e3537c.
    Direction: pick the most recently written view.
  - `CommandProcessor::DrawIndexIndirect` (`GraphicsRun.cpp:2115`) waits only
    for device-address write-backs before its CPU read of the arguments, so
    a draw whose arguments an earlier dispatch of the same recording wrote
    can use stale counts. Direction: the same admission as 6d1b178d.
  - Exact storage views are not separated by image type; a 2D/3D filter was
    tried and dropped because no failure needs it.

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

## Robot platformer: vertex attribute format words (2026-10-14)

- Observed: the flat blue streak shapes over the warp tunnel (open item of
  the entry below) come from additive particle draws whose pixel program
  receives NaN in every interpolant. A temporary NaN-count probe in the
  pixel program showed NaN in all 2.8 million fragments of one draw; the
  NaN came from the vertex program's exported parameters, which all derive
  from a normalized attribute.
- Cause: the vertex attribute control word carries a format override in
  bits 5..13, laid out as `(buffer data format << 2) | (components - 1)`.
  The guest's own fetch code decodes it that way (`s_bfe_u32` of bits 5..6
  for the component count, 7..13 for the format, then a DST_SEL table
  X001/XY01/XYZ1/XYZW). Kyty matched the whole 9-bit field against a
  partial table plus a raw-format-id fallback, and kept the V# format for
  anything else. The normal (`0x10b`, 16_16_16_16_SNORM, 4 components) was
  therefore fetched as 32_32_32_FLOAT. `0xcb` (2_10_10_10_UNORM) and
  `0x117` (16_16_16_16_UINT) were also silently replaced.
- Change (4ea75ee0): decode format and component count separately, take
  the DST_SEL from the component count, and refuse an override whose format
  has no vertex input mapping (the "keep the V# format" path and the raw-id
  fallback are removed). The Gen5 vertex format table now lists every float,
  normalized and unsigned format of the ISA format table that has a
  mandatory Vulkan vertex-buffer equivalent. Signed-integer and scaled
  formats stay unmapped until the input path models them.
- Verified (Intel A770, strict, `generic` route, 330 s, no error): the warp
  tunnel shows thin lit streaks with no flat shapes; the studio logo, title
  logo and the ship are visible. Distinct override fields seen in that run:
  `0x05d 0x0cb 0x0e3 0x101 0x10b 0x117 0x11f 0x12a 0x137`. Racing title
  (Intel, 125 s), fishing title (AMD, 200 s) and watercolour platformer
  (AMD, 90 s): no error, same states as the baselines; all three use only
  field 0 (the V# format), so the decode change does not touch them. Unit
  tests: 2,419 pass. Table and boundary gates pass.
- Open: an override with fewer components than its format (for example
  `0x10a`, four-component storage read as XYZ1) is refused by the vertex
  input layout when the shader reads four registers; not seen in a live run.

## Robot platformer: exposure fill and DCC metadata clears (2026-10-14)

- Observed: after `TUNNEL_ENABLE` the Intel intro showed only the UI over a
  black scene, with stale publisher text and a smeared logo. Bound-image
  readbacks at present 1400 showed the 3D scene rendered correctly into its
  2432x1368 RGBA16F targets (space backdrop, streaks, the console-shaped
  ship). So the black came from the final composite, not from skipped
  geometry.
- Cause 1, exposure: the composite pixel program multiplies the tonemapped
  scene by an exposure read from a 1x1 RGBA32F storage image. The guest
  resets the exposure state each frame with the uniform compute fill kernel,
  `(1, -1, 0.5, 0.18)`. The fill decoder refused RGBA32F, so the sampled
  texture stayed zero and the exposure pass wrote zero. Fixed in 582c6662: a
  16-byte texel is the fill quad itself.
- Cause 2, UI layer: the UI target (3840x2160 RGBA16F, DCC on) has its
  metadata filled with key `0x40` (color 0, alpha 1, per the Mesa
  `ac_descriptors.h` keys) before the UI draws. Kyty ignored metadata fills,
  so the UI accumulated earlier frames. The composite uses the UI alpha as
  scene transmittance, so the accumulated UI also hid the scene.
- Cause 3, lighting: the lighting target's metadata is filled with `0x00` and
  four lighting compute passes then write the target as a storage image
  before its first render-target bind. Consuming the clear at that bind
  erased the lighting and left the ship as a black silhouette.
- Change (fabd1ecb, contract in `docs/graphics-compute-uniform-color-fill.md`):
  - A metadata fill with a clear key is a pending clear.
  - The first access consumes it before the access happens: a render target
    with that DCC base, or a shader image, read or written, whose T# enables
    compression with that metadata base.
  - It is applied only to single-level, single-slice, single-sample surfaces
    whose fill holds one key per 256 bytes of texels.
- Verified (Intel A770, strict, `generic` route, 330 s, no error, 1,482
  frames): the captures show the publisher text, the studio logo over the warp
  tunnel, the title logo and the lit ship. A diagnostic run of the same
  tree applied seven metadata clears in present 1400. Racing title (Intel,
  125 s), fishing title and watercolour platformer (AMD, 200 s and 90 s): no
  error, and the same states as the 1d692f53 baselines. The fishing title
  keeps its in-game rate (about 150 frames per 26-30 s capture interval in
  both runs). Unit tests: 2,418 pass, 12 skipped, 0 failures. Table and
  boundary gates pass.
- Open, found while verifying:
  - Flat blue streak shapes cover the scene (fixed: see "Robot platformer:
    vertex attribute format words"). They are drawn by instanced
    particle draws (48 vertices x 150 and x 300 instances) into the 1920x1080
    effect layer and the 960x540 half-resolution layer, and composited by the
    pass that writes the scene before TAA. The shapes exist with and without
    the metadata clears; only their color changes.
  - Some frames show a black top-left quadrant in the half-resolution layer
    (960x540 written into a 1920x1080 surface).
  - The skip prompt's text is drawn as filled squares.
  - Not modelled: the pipe-dependent padding of the metadata plane, mip
    chains, arrays, MSAA, reversed component swaps, and register-key clears
    read through the texture unit.

## Robot platformer: Gen5 dispatch TUNNEL_ENABLE (2026-10-13)

- Observed: with the 64-entry table, a 600 s Intel run passed the title
  screen and loaded the intro level. It stopped at 461 s on
  `compute dispatch admission rejected: mode=0x0000a041
  reason=unsupported_dispatch_mode` (`GraphicsRenderDraw.cpp`). The
  initiator is COMPUTE_SHADER_EN, ORDER_MODE and CS_W32_EN plus bit 13.
- Contract:
  - The GFX10.3 register header (`gc_10_3_0_sh_mask.h`, Linux amdgpu) names
    bit 13 of COMPUTE_DISPATCH_INITIATOR `TUNNEL_ENABLE`.
  - A Mesa Vulkan driver sets it on every GFX10+ dispatch and says the
    vendor's own driver does too. The kernel driver honours it only on
    high-priority compute queues; elsewhere it is a no-op. It asks the
    scheduler to prioritise the dispatch's waves and does not change
    groups, lanes or results.
- Change: `ShaderComputeDispatchExecutionMode` clears `TUNNEL_ENABLE` on
  Gen5 before the dispatch plan, the lane order and the input info see the
  mode. Every other unknown bit stays refused. Gen4 is unchanged.
- Verified:
  - Intel robot platformer: 723 s with no error, 1,989 frames. The intro
    level starts (`Level has started: intro_next`). The captures show the
    title menu (logo and buttons) over a black scene, with the publisher
    logo text still drawn under it; the intro level is dark. That is the
    next problem.
  - The guest logs `JsParticle heap memory is full!` from the intro level
    on (32 lines). Not investigated.
  - Intel racing title: 14.9k / 19.4k / 24.9k / 25.7k frames, no error
    (previous run 25.6k).
  - Unit tests: the 154 compute-wave and dispatch tests pass.

## Robot platformer: 64-entry shader texture table (2026-10-13)

- Observed: after the storage-image alias fix, the Intel run stopped at
  253 s on `shader texture resource capacity exceeded`
  (`ShaderGetTextureBuffer`, `Shader.cpp:2181`). The table held 32 entries.
  The stopping pixel program reads 43 distinct non-empty 8-dword image
  descriptors. 40 of them are in the extended user data, each read by a
  constant-offset `s_load_dwordx8`; the program has no dynamic descriptor
  loads. So the guest really binds more than 32 images in one stage.
- Host limits: `maxPerStageDescriptorSampledImages` is 33,554,432 on the
  Intel A770 (ANV) and 8,388,606 on the AMD Raphael iGPU (RADV). The table
  size is a host bookkeeping cap, not a Vulkan limit.
- Change (`Shader.h`): `ShaderTextureResources::RES_MAX` is 64. The
  storage-image write-only and seed-skip masks, which hold one bit per table
  entry, become `uint64_t` (`Shader.cpp`, `GraphicsRenderDraw.cpp`,
  `GraphicsRenderBind.cpp`, `GraphicsRenderInternal.h`). The descriptor-cache
  arrays and the metadata push-constant bound follow `RES_MAX`.
- Verified:
  - Intel robot platformer: past the stop, 300 s with no error, 1,287
    frames at 2.9-7.2 fps. The captures still show the dark loading screen.
  - Intel racing title: 15.3k / 18.1k / 24.5k / 25.6k frames, at or above
    the previous run.
  - AMD (RADV): racing title 9.9k frames, unchanged; watercolour platformer
    3.4k and 3.9k frames.
  - AMD fishing title, same tree and route, 200 s. Steady rate is frames per
    30 s over the last three capture intervals:

    | Table size | Perf counters | Final frame | Steady rate |
    | --- | --- | --- | --- |
    | 32 | on | 2,860 | 188-207 |
    | 64 | on | 2,304 (slow load) | 192-196 |
    | 64 | off | 2,792 / 2,814 / 2,858 | 136-225 |

    The two sizes overlap. Runs on earlier binaries reached 3,058-3,106
    frames at 206-238 frames per 30 s. That gap is not attributed:
    - The storage-image alias change only replaces an EXIT that this title
      never reached.
    - Foreign processes loaded the host during some of the later runs
      (load average 4-12).
    - A repeat on HEAD under a load average of 12 gave 141-152 frames per
      30 s, so it does not separate the two causes.
    - The HEAD run before it stalled at frame 3 at boot. That is the known
      intermittent boot stall, which also reproduces on older binaries.
  - Unit tests: 2,413 pass. The one failure is the known intermittent
    `StandardSamplerRackDestroyWaitsForACallbackRunningOnAnotherThread`
    timeout (see the 2026-10-11 entry); it passes alone 3/3.
- Open: per-draw structures grow with the table (`ShaderPixelInputInfo`
  6,392 bytes, `ShaderVertexInputInfo` 8,984 bytes), and `BindDescriptors`
  zero-initializes and scans the full arrays rather than the bound counts.
  No cost was measured above noise.

## Robot platformer: storage image over a sampled and a buffer view (2026-10-13)

- Observed: after the pixel append fix, the Intel run stopped at 228 s on
  `!create_all_the_same`. The new object is a 128x128 R32 StorageTexture
  (64 KiB). Its range equals two live objects:
  - a sampled Texture with the same format, pitch, extent, tile and swizzle,
    created with its guest upload skipped;
  - a StorageBuffer of 4,096 16-byte records, uploaded from guest memory,
    with a write-back, not in use.
  The single-parent path already links each of these relations
  (`GpuMemoryAllowsTextureStorageAlias`). The multi-parent path had no rule
  for mixed parent types, so it reached the EXIT.
- Change (`GpuMemoryCreate.cpp`): `multi_storage_texture_alias` replaces only
  that EXIT. It links the new image when every parent independently passes
  `GpuMemoryAllowsTextureStorageAlias` and has published its content to
  guest memory: no texture materialized from a device image, and no buffer
  with an unfinished writable write-back. The image seeds from guest bytes.
  No earlier branch changes, so titles that did not reach the EXIT keep
  their path.
- Verified:
  - Intel robot platformer, twice: past the overlap, 1,277 frames at
    3.4-10.9 fps. It stops at 253 s on the 32-entry texture table: the
    shader declares more read-only texture slots. The 64-entry table entry
    above takes it past that stop.
  - Unit tests: 2,414 pass.
- Open (coherence, not verified):
  - Storage images have no write-back
    (`StorageTexture.h:53`).
  - A buffer write-back does not invalidate a StorageTexture parent
    (`GpuMemory.h:775`).
  - So the image's writes do not reach the sampled view (which holds no guest
    bytes) or the buffer view. The other direction works: a buffer
    write-back changes guest bytes, and the image's hash check reloads them.
  - Suggested direction: publish storage-image writes to linked views, by
    copy or write-back. Judge it with a scored capture of a scene that uses
    this range.

## Robot platformer: pixel GDS append under the launch EXEC (2026-10-13)

- Observed: the same linked-list pixel program allocates its node slot as
  `v_mbcnt_hi v7, exec_hi, 0` / `s_mov m0, s20` / `ds_append v6 gds` /
  `v_mbcnt_lo v0, exec_lo, v7`, before any EXEC write or branch. The
  program starts with `exp null vm done` and has no `s_wqm`. The native
  wave proof refused it twice: MBCNT observes EXEC as a number, and a
  memory write runs while helper lanes are live in the host invocation.
- Contract:
  - RDNA2 ISA: EXEC is the wave's valid mask until the program writes it,
    and `exp ... vm` sends EXEC as the valid mask.
  - DS_APPEND adds `count_bits(EXEC)` at `M0.base + offset` and returns the
    pre-op value to every valid lane.
  - V_MBCNT_HI is a plain source move in Wave32, so MBCNT over EXEC is the
    exclusive prefix of valid lanes at both widths.
  - Vulkan: helper invocations have no effect on stores and atomics, and
    their atomic results are undefined.
- Change:
  - `ShaderLaunchExecEnd` marks the first EXEC write or control-flow
    boundary. `ShaderCountsLaunchExec` accepts only MBCNT_LO/HI over
    EXEC_LO/EXEC_HI before it.
  - The partial-wave admission and the fragment mask flow accept those
    MBCNTs and GDS append/consume (and device memory writes) before that
    point. LDS and scratch writes stay refused.
  - Pixel emission builds a valid-pixel ballot (`exec_lane_lo` and not a
    helper). MBCNT counts it. One valid lane updates the counter by its bit
    count, and the pre-op value is shuffled to every valid lane.
  - The GDS pointer SGPR is the M0 source at the GDS instructions, not the
    last M0 write in the program (here M0 is later reloaded for
    interpolation).
- Verified:
  - The dumped program proves `FragmentPartialWave` at both widths.
  - Unit tests: admission at both widths; refusal after a branch and for a
    non-EXEC mask; SPIR-V with one counter atomic, no elect or broadcast,
    and validated by the toolchain.
  - Strict AMD (RADV) runs, no error:

    | Title | Frames |
    | --- | --- |
    | racing title | 5.6k / 7.5k / 9.3k / 9.9k |
    | watercolour platformer | 4.1k at 60 s |
    | fishing title | 2.0k, then 3.1k on a repeat, at 180 s (the first run loaded slower; same steady rate) |
  - Intel robot platformer: 1,160 frames at 1.8-7.9 fps, past the
    linked-list program. It stops at 228 s on `!create_all_the_same`: a
    64 KiB StorageTexture whose range equals a sampled Texture and a
    StorageBuffer. That is the next stop.

## Robot platformer: image atomic swap with a GLC return (2026-10-13)

- Observed: after the Wave32 OR reductions, the Intel run stopped at
  `ShaderParseMIMG.cpp:49`. The parser admitted GLC only on
  IMAGE_ATOMIC_ADD (0x11). The stopping program is a per-pixel linked list:
  `image_atomic_swap v9, v[0:1], s[0:7] glc` exchanges a new node index into
  a 2D R32_UINT head image, and the node is written through buffer stores.
- Contract:
  - RDNA2 ISA: IMAGE_ATOMIC_SWAP (opcode 15) is `tmp = MEM; MEM = DATA;
    RETURN = tmp`.
  - GLC on an atomic selects the pre-op return (ISA 8.1.10).
  - Load and store cache policy (GLC on non-atomics) is still refused.
- Change:
  - `ImageAtomicSwap` is a new instruction type. It shares the parse,
    admission (`ShaderImageAtomicSupported`), storage-usage and emission
    paths of IMAGE_ATOMIC_ADD through `ShaderIsImageAtomic`.
  - `Recompile_ImageAtomic` emits `OpAtomicExchange` or `OpAtomicIAdd` on
    one R32 texel, in compute and pixel stages.
- Verified: unit tests cover both opcodes with and without GLC, and the
  sampler-operand refusal for both.

## Robot platformer: Wave32 whole-wave OR reductions (2026-10-13)

- Observed: once the Wave32 lane masks were narrowed, the Intel pixel
  program stopped at pc 0x166c with `fragment row DPP source participation is
  unproven`. The construct is the Wave32 form of the wave64 worldmap
  reduction:
  - `s_mov_b32 s32, exec_lo` ... `s_orn2_saveexec_b32 vcc_lo, s32`;
  - `v_cndmask_b32 v23, 0, v23, s33`, with `s33 = s32 & s64`;
  - four `v_or_b32` with `row_shr:1/2/4/8`;
  - `s_mov_b32 exec_lo, vcc_lo`, an unrelated `v_rsq_f32`, then
    `v_readlane` of lanes 15 and 31 and `s_or_b32`.
- Contract:
  - RDNA2 ISA: V_READLANE_B32 takes `lane % 32` in a wave32 and ignores EXEC.
  - The upper halves of EXEC and VCC are ignored in a wave32.
  - Row DPP stays within a 16-lane row in both widths.
- Change:
  - The neutral-region proof (`ShaderFragmentNeutralRegionEnd`) takes the
    guest wave size. For Wave32 it accepts the B32 save, ORN2_SAVEEXEC,
    AND and restore forms on single SGPRs. It runs on the narrowed program,
    and the select is classified with its encoded pair condition.
  - Every other region user (tier, partial-wave reads, emitter) still asks
    for the Wave64 form only. In a Wave64 program a B32 entry widens only
    EXEC_LO, so it never forms a region.
  - The reduction proof uses the guest lane count, refuses a constant lane
    at or above it, and follows the B32 mask algebra for the residual
    (stale-lane) obligation.
  - The mask flow result records the guest width it was proven for.
  - `FragmentPartialWave` is no longer limited to Wave64. Host selection
    already limits it to host subgroups no wider than the guest wave, so a
    host invocation id is the guest lane.
  - At both widths, instructions that name no network register, keep EXEC,
    and are neither branches nor labels may now sit between the restore and
    the reads.
- Verified:
  - The dumped program now proves `FragmentPartialWave`, with one reduction
    whose reads cover lanes `0xffff` and `0xffff0000`.
  - Unit tests: 2,412 pass. New cases:
    - the Wave32 sequence and its SPIR-V emission;
    - refusal at Wave64;
    - refusal for lane 32;
    - stale lanes after a B32 WQM;
    - independent versus observing instructions before the reads (Wave64).
  - Strict AMD (RADV) runs, no error:

    | Title | Frames |
    | --- | --- |
    | racing title | 7.6k / 9.4k / 11.2k / 12.0k |
    | watercolour platformer | 4.1k at 60 s |
    | fishing title | 3.1k at 180 s |
- Robot platformer, cold SPIR-V caches:
  - AMD: 303 s without error, 941 frames at 3.3-5.3 fps, still on the
    publisher logo. The previous run loaded at about 0.4 fps.
  - Intel (Arc, no concurrent GPU job): 1,190 frames at 3.5-8.3 fps. It
    passes the logo into a dark loading transition, then stops at 251 s on
    `ShaderParseMIMG.cpp:49`: `glc == 1` on a MIMG opcode other than 0x11.
    That is the next stop.

## Robot platformer: wave32 lane masks are one dword (2026-10-13)

- Observed: after the SDWA fix, the same Intel pixel program (guest wave32)
  was refused at pc 0xf48 with `reason=mask observed as a number`:
  - 0xf30 writes a number into `vcc_hi` (`s_bfe_u32`).
  - 0xf40 runs `v_cmp_eq_u32 vcc, 3, vcc_hi`.
  - 0xf48 and 0xf5c read `vcc_hi` back as that number.
  The mask flow had marked both VCC words as lane bits at 0xf40.
- Contract (RDNA2 ISA 2.1: "the upper half of EXEC and VCC are ignored for
  wave32 waves"; 3.x: "when used by a wave32, the upper 32 bits of VCC are
  unused and only the lower 32 bits contribute to VCCZ"):
  - In a wave32 program, every VALU lane mask is one dword: compare and
    carry-out destinations, carry-in and the `v_cndmask` condition.
  - The upper SGPR of the encoded pair is neither written nor read, so it
    may hold a number across the instruction.
  - The guest relies on this, as shown above.
- Implementation:
  - `ShaderWave32LaneMasks` narrows those operands; `S_BFM_B64` shares the
    compare format and is excluded.
  - `ShaderAnalyzeFragmentMaskFlow` takes the guest wave size. At 32 it
    walks the narrowed program, branches on VCC_LO only, and accepts the B32
    mask algebra (AND, OR, ANDN2, XOR, ORN2, NOT, MOV, AND_SAVEEXEC) wherever
    the region rules accepted the B64 forms.
  - Wave64 behaviour is unchanged.
  - The emitter already kept the upper word in wave32: `ResolveMaskAccesses`
    stores only the low ballot word and drops the high-word clear.
- Next stop: the same program now clears the mask flow and stops at
  pc 0x166c with `fragment row DPP source participation is unproven`. This
  is a wave32 whole-wave OR reduction:
  - `s_orn2_saveexec_b32`, then a `v_cndmask` seed;
  - four row-DPP `v_or_b32`;
  - `v_readlane` 15 and 31, then `s_or_b32`.
  The partial-wave reduction proof (`ShaderFragmentLaneReduction.cpp`)
  covers only wave64: `S_ORN2_SAVEEXEC_B64`, 64 lanes and B64 mask algebra.
  Extending it to wave32 (lane sets 0-15 and 16-31) is the direction. The
  DPP precondition stays: a host shuffle from an inactive or absent lane is
  undefined.
- Verified:
  - The mask flow proof was checked offline on the dumped program.
  - Unit tests: 2,410 pass. A new case runs the same program at both widths:
    wave32 keeps the number in VCC_HI, wave64 refuses it. The
    whole-pair VCC branch case now states the wave32 result.
  - A temporary per-program check (env-gated, removed) showed that no pixel
    program of the racing title, the watercolour platformer or the fishing
    title runs as wave32. Their analysis is unchanged.
  - Strict regressions on AMD (RADV, warm), no error:
    - racing title: 5.6k / 7.4k / 9.3k / 10.2k;
    - watercolour platformer: 3.6k at 60 s;
    - fishing title: 3.1k at 180 s.
  - On Intel the three titles ran without new errors, under a concurrent
    host GPU job on the same device, so their frame rates are not
    comparable:
    - One racing title run hit the open intermittent depth-reference
      refusal recorded below.
    - One stopped in `String::Size` before the first frame, and did not
      reproduce on two reruns.

## Robot platformer: SDWA low destination selects (2026-10-13)

- Observed: the Intel stop at pc 0x157c of a 1920x1080 pixel program is
  `v_cvt_i32_f32` in SDWA form with control dword 0x0006140d
  (`dst_sel=WORD_0`, `dst_unused=PRESERVE`). The lowering preconditions
  refused every destination select other than DWORD.
- Contract (RDNA2 ISA, SDWA table 88; the LLVM AMDGPU modifier syntax gives
  the same ranges):
  - `DST_SEL` 0-3 select a byte, 4 bits [15:0], 5 bits [31:16], 6 the dword.
  - `DST_UNUSED` 0 pads the other bits with zeros, 1 sign-extends the
    field, 2 keeps the prior bits, 3 is reserved.
  - Neither source says where the result sits for `WORD_1`/`BYTE_1-3`
    (shifted into the field or taken from the matching bits). Only one
    secondary implementation shifts it, which is a lead only.
- Implementation:
  - `ShaderSdwaLowDestinationSupported` admits `BYTE_0` and `WORD_0` with
    `DST_UNUSED` 0-2.
  - It applies to VOP1 and VOP2 with a plain single-VGPR destination, no
    clamp or output modifier, and no MAC or dot forms.
  - The per-lane emitter (`EmitSdwaLowDestinationInstruction`) runs the
    ordinary instruction as a whole-dword write, then merges its low field
    into the prior value (pad, sign-extend or preserve). An inactive lane
    keeps the prior value.
  - Paired and native compute-wave banks still refuse these selects, and so
    do shifted selects, reserved `DST_UNUSED`, clamp and output modifiers.
- Verified: a unit test pins the admitted and refused control words. A
  strict Intel run of the robot platformer passed pc 0x157c and stopped at
  the wave32 VCC case above.
- Defect found, not fixed: the host-visible compute scratch pool
  (`TransientBufferPool::Scratch`, `GraphicsRenderCommandBuffer.cpp:1090`) frees
  entries only in `Free()`. A command buffer that detiles many large
  resources fills its 16 MiB, and the detile then falls back to the CPU with
  `ResourceUnavailable`. A per-submit reset or a size-aware reuse that keeps
  the two-buffer detile rule is the direction.

## Robot platformer: raw texels between aliased depth and colour views (2026-10-13)

- Observed: one 960x540 range (1024x640x4 bytes, 64 KiB blocks) is
  time-shared every frame by a B10G11R11 render target (mode 27), a
  10_11_11 storage view (mode 27), an R32F storage view (mode 24, one level)
  and a D32 depth target with no HTILE. A bounded diagnostic showed the depth
  bound read-only (`LEQUAL`, depth writes off, no clear) and the R32F
  storage view as its producer: image-store analysis marks it fully
  overwritten. The previous code had no relation for depth over storage, so
  the read-only depth started undefined and hit the read-only clear refusal.
- Contract:
  - The 4-byte depth (mode 24) and render-target (mode 27) layouts were
    taken to address texels alike, pinned by a unit test that used the
    production helpers. Disproved on 2026-10-16: the guest's mode 27 micro
    tile differs (see "Four-byte render-target micro tile"); the seed now
    admits mode 24 views only.
  - A depth target over an equal-range render target or matching storage
    view, and a storage view over such a depth target, copy the raw 32-bit
    words of the newest writer (`create_texel_bits_seed`), then retire it.
  - Matching means:
    - one sample, one layer, one level;
    - an equal extent;
    - a pitch equal to the depth's 64 KiB-aligned width;
    - R32F or B10G11R11 colour;
    - no HTILE;
    - host extent equal to guest.
  - Any later write to another overlapping view refuses the seed.
- Copy (`UtilCopyTexelBits`): image to buffer to image, with no conversion.
  Its staging is a device-local per-command-buffer pool
  (`AllocateTransientCopyBuffer`). The host-visible compute scratch pool did
  not work for it: on Intel, two detile entries (8.3 MB and 6.6 MB, other
  usage masks) already held 14.9 of its 16 MiB, so the 2 MB copy was
  refused at about 140-150 s.
- Limit: Intel Arc (DG2) does not expose `VK_EXT_depth_range_unrestricted`,
  so a depth upload outside [0,1] is outside VUID-07931 there. RADV exposes
  it.
- Excluded:
  - Clamping the words into a depth image destroys the bit pattern the
    colour views read back, so it was rejected.
  - The nondeterministic `!create_all_the_same` at another range in the same
    title (78 s, Intel) is the documented transient-pool class. This seed
    path did not run there.
- Next stop (Intel, reproduced twice at 178-189 s, 1920x1080 pixel stage):
  - `native_admission{... pc=0x157c reason=invalid instruction before native
    wave analysis}`.
  - The instruction is `v_cvt_i32_f32` in SDWA form (control dword
    0x0006140d: `dst_sel=WORD_0`, `dst_unused=PRESERVE`, `src0_sel=DWORD`),
    a 16-bit partial write that keeps the high half of the VGPR.
  - The parser kept the destination select and the lowering preconditions
    refused it (fail closed); the per-lane merge is in "SDWA low destination
    selects" above.
- Verified:
  - The robot platformer on AMD (RADV, `Fixed` + RenderTargets) ran 600 s
    and then 300 s, both status 1 with no error. It is still loading the
    worldmap at about 0.4 fps, with black captures.
  - On Intel it ran 178-189 s, until the SDWA stop above.
  - Regressions (strict, no errors):

    | GPU | Title | This change | Baseline |
    | --- | --- | --- | --- |
    | Intel A770 | racing title | 16.0k / 20.6k / 26.1k / 27.5k frames at 30 / 60 / 90 / 120 s | 15.4k / 20.1k / 25.7k / 27.0k |
    | Intel A770 | watercolour platformer | 10.7k at 60 s | about 10.5k |
    | Intel A770 | fishing title | 5.6k at 180 s | about 5.5k |
    | AMD (warm cache) | racing title | 6.3k / 8.4k / 10.3k / 11.2k | 5.7k / 7.5k / 9.4k / 10.3k |
    | AMD (warm cache) | watercolour platformer | 4.1k at 60 s | about 4.1k |
    | AMD (warm cache) | fishing title | 3.1k at 180 s | about 3.0k |

    The first AMD pass ran right after a host reboot. It was 25-35% lower
    from the first window on, and recovered on rerun.
  - Unit tests: 344 graphics-state and detile tests. Gate scripts:
    boundaries, graphics tables.

## Robot platformer: whole-wave OR reductions as partial waves (2026-10-13)

- Fix: the worldmap pixel shader's whole-wave OR reduction is admitted with
  a new proof, `FragmentPartialWave`. A Wave64 pixel program on a host
  subgroup of any width is a partially populated guest wave. When its only
  whole-wave observations are proven OR reductions and reads that stay
  inside launched quads, the proof holds at every host width. It therefore
  needs no `requiredSubgroupSize` for the fragment stage, which the Intel
  Arc host cannot provide.
  - Reduction contract (`ShaderFragmentLaneReduction.cpp`): a proven neutral
    region with a `v_cndmask` seed whose condition holds only lanes of the
    entry EXEC (no helper, no lane outside the launched quads). Its steps are
    row-shift DPP ORs with full row and bank masks, `v_permlanex16` and
    plain ORs. The reads are static `v_readlane` after the EXEC restore,
    before the first export or indirect jump.
  - Lowering: each read is one `OpGroupNonUniformBitwiseOr` over the seeds of
    its guest lane set (lane 31 reads lanes 0-31; lane 63 reads 32-63),
    followed by a quad-uniform broadcast. The network steps are not emitted.
  - Network liveness: after the reads, every path must overwrite the network
    registers before any other read. An overwrite under a non-full EXEC
    leaves the register "residual", stale only in launched lanes outside the
    restored mask. A residual register may be read only while EXEC stays
    inside that mask (tracked through mask algebra, joined monotonically
    across loops), and never through DPP, readlane/writelane/permlane, DS or
    a non-lane-local image read.
  - The fragment mask flow now records, per instruction, whether EXEC holds
    only lanes of the launched quads, all of them, and which `v_cndmask`
    conditions are live-only.
- Excluded, from a temporary diagnostic in the analysis: requiring a full
  EXEC for the overwrite. The worldmap shader kills the network registers
  under the discard mask (`s_andn2 ... vcc` then `s_wqm`), so that rule
  refused it on both of its paths.
- Excluded: exact-state deduplication in the liveness walk. A loop head in
  the same shader produced more than 16 distinct states; the walk now joins
  states monotonically.
- Current frontier on both hosts: a wave64 vertex program, refused with
  `ngg_front_refusal{... reason=complete program exceeds the 1024-instruction
  proof bound}`. Neither Intel nor RADV can pin the vertex stage to 64 lanes.
- Verified:
  - The robot platformer passes the worldmap pixel stop and stops at the
    vertex bound on Intel Arc A770 (172 s) and on AMD RADV (272 s, fixed
    render-target resolution), strict runs.
  - Regressions, one strict run each on both hosts (status 1, no error): the
    racing title is in a race, the watercolour platformer is in the intro
    and the fishing title is docked in gameplay. The capture frame counts
    match the previous runs.
    - The first Intel run of the racing title after a host reboot (cold disk
      cache and a fresh SPIR-V cache) was still loading at 90 s. A warm rerun
      matched the previous runs frame for frame.
  - Unit tests: 980 tests across the shader, fragment, compute, NGG, packet,
    state and detile suites. New admission cases cover the proof, refusal
    of observed network values and of row-masked steps, stale lanes under
    WQM, and SPIR-V emission validated with `spirv-val`. Gate script:
    boundaries (strict).

## Robot platformer: worldmap admission chain (2026-10-13)

- Four general fixes move the robot platformer from its early stops to the
  worldmap level load on both hosts:
  - **Tiled storage writers composed into linear views.** A linear storage
    view over memory last written through single-level 2D storage images in
    the 64 KB R_X layout is composed from those images, as it already was from
    tiled render targets. The newest complete writer of each range wins.
  - **Loop exits that break through an arm.** The CFG structurizer accepts a
    loop exit that enters an arm placed between the header and the latch when
    every path of the arm stays under the header, leaves only to the merge and
    shares no block with another exit or with what follows the merge
    (`ShaderControlFlowGraph.cpp:IsBreakArm`). Only graphs refused before are
    affected.
  - **Wave32 lane masks in scalar flow.** A wave32 `V_CMP`/carry-out writes only
    the low SGPR of its pair. The scalar flow read every program as wave64, so
    a compare into `s[13:14]` hid that `s14` still held its entry value. A
    compute shader's extended user data load through `s[14:15]` then lost its
    descriptor mapping (emitter refusal on `S_LOAD_DWORDX4` at 0x1740). The flow
    now keys on the guest wave size. Compute resource analysis passes the
    dispatch plan's size; the other stages keep the conservative wave64
    reading.
  - **Any-lane mask tests.** `S_CMP_LG_U64`/`S_CMP_EQ_U64 mask, 0` followed by a
    forward SCC branch asks what `S_CBRANCH_VCCZ` asks. The fragment mask flow
    opens a divergent region for it instead of refusing the compare as a
    numeric observation. The summary is cleared by any other SCC writer or a
    join. Every other SCC reader and back edges stay refused. Masks that may
    hold helper bits under WQM qualify, as they do for VCCZ.
- Current frontier on both hosts: a 1920x1080 worldmap pixel shader (wave64,
  3 colour targets, depth write, blend) is refused by native wave admission.
  It is now classified `FragmentNeutral32` (it was `ExactSubgroup` before the
  any-lane fix). The refusal is participation: "fragment row DPP source
  participation is unproven" at the first of four `V_OR_B32` row-shift steps
  (`row_shr` 1/2/4/8, `bound_ctrl` 0), in a whole-wave OR reduction.
  - The reduction is: `s_orn2_saveexec` region,
    `v_cndmask 0, x, live&exec`, the row scan, `v_permlanex16`, then
    `v_readlane` 31 and 63 after the restore and `s_or` of the two halves.
  - The same shader also has three `v_readfirstlane` waterfall loops, refused
    with "target quad is not proven to retain a participating real member".
  - Helpers relay partial ORs through the scan, so neutral initial values do
    not make a non-participating helper's shuffle source defined. This
    refusal is correct and must not be relaxed.
  - A whole-wave reduction proof is a viable direction: lower the readlanes
    as subgroup OR reductions of the initializer, valid at every host width.
    It needs the initializer's condition proven narrow, the scan registers
    dead after the region, and EXEC after the region proven within the
    region's entry EXEC.
- Intel-specific gate: the Arc A770 lists only COMPUTE, TASK and MESH in
  `requiredSubgroupSizeStages`, and fragment subgroups vary from 8 to 32.
  `FragmentNeutral32` therefore has no legal mapping on Intel
  (`host=unavailable mapping=0`) even with the participation proof. Intel
  needs a proof valid at every width, or the fragment wave transport (the
  compute stage can pin 32). AMD (RADV) can pin fragment 64 and reports
  `mapping=1`.
- Scoping experiment (temporary, uncommitted bypass of the participation
  refusals): AMD then stops at a vertex program with
  `ngg_front_refusal{... reason=complete program exceeds the 1024-instruction
  proof bound}`. RADV cannot pin the vertex stage to 64 either.
- Open gap: the pixel and vertex resource analyses still read scalar flow as
  wave64 (conservative, not a correctness defect).
- Verified:
  - Intel Arc A770, strict runs with a fresh SPIR-V cache:
    - the racing title (125 s) is in a race;
    - the watercolour platformer (90 s) is in the intro;
    - the fishing title (200 s) is docked in gameplay.
    - All three end with status 1 and no error.
    - The same three runs on AMD (RADV, fixed render-target resolution):
      racing, intro and docked gameplay, each with status 1 and no error.
  - The robot platformer reaches the worldmap level load and stops at the
    pixel shader above, on Intel (160 s) and on AMD (267 s). AMD stopped at
    about 61 s before.
  - Unit suites: 596 shader, fragment, compute and NGG tests, including new
    break-arm, wave32 mask and any-lane cases. Gate script: boundaries
    (strict).

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
  The cause is not investigated.

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

## Racing title race-window correction (2026-10-11)

- The racing title's "race window" figures above (70 to 88 fps) were
  measured with the car stationary on the grid. The route pressed R2 only
  after the race clock ran out. A scripted R2 also set only the digital bit,
  and the title reads the analog trigger.
- With scripted triggers reporting full travel and acceleration from the
  race start, the moving race measured 43.0 and 42.6 fps (p50 21-22 ms,
  381-413 draws per frame). The time-over flyby that follows measured
  33.8 fps at 538 draws per frame.
- These are the baselines for the racing title from now on. The grid
  figures remain valid only as A/B pairs measured on the same scene.
- The beat 'em up's 11-12 fps window is its intro card (6-7 draws per frame),
  not combat. Its gameplay has not been measured yet.
- Moving captures show small grey squares around the car, likely particles.
  This is not yet investigated.

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

## Mission

Kyty is an experimental PlayStation 4 and PlayStation 5 emulator. This fork is
bringing the PS5 path from early execution to correct, interactive rendering
while preserving a design that can run on macOS, Linux, and Windows and on AMD,
Intel, NVIDIA, and Apple GPUs.

Accuracy comes before superficial progress. A frame that is merely non-black,
a process that survives because unsupported behavior was ignored, or a build
that succeeds without exercising the runtime is not compatibility.

## Non-negotiable invariants

1. **Evidence before code.** Reproduce the problem, identify the first strict
   failure, and trace the bad state to its producer before editing.
2. **Never invent guest behavior.** Do not guess NIDs, ABI signatures, packet
   layouts, register meanings, formats, tiling, alignments, or return codes.
3. **One behavior, one implementation.** Direct and indirect encodings of the
   same GPU state share a decoder. Resource sizes, offsets, and pitches come
   from one layout model consumed by every caller.
4. **No behavioral fallbacks.** Never continue with assumed RGBA8, linear
   tiling, default success, skipped state, placeholder shaders, or fabricated
   resources. Unsupported behavior fails with enough evidence to implement it.
5. **Capability-driven rendering.** Select Vulkan strategies from features,
   limits, formats, queues, and extensions. Vendor IDs are diagnostic data, not
   policy switches.
6. **Keep platform code at the boundary.** OS-specific memory, exceptions,
   threads, windows, surfaces, controllers, and dynamic loading stay in focused
   platform modules. Guest HLE and GPU semantics are platform-neutral.
7. **Do not regress the working frontier.** Preserve existing execution,
   rendering, input, and build behavior unless a test proves that behavior is
   itself incorrect.
8. **Report reality.** Distinguish verified behavior, captured evidence,
   hypotheses, and untested assumptions in code reviews and handoffs.

## Investigation and advancement methodology

This section is the practical operating system for agents and humans advancing
the PS5 path. It does not replace the invariants above; it explains *how* to
obey them day to day. Speed without this loop is noise.

### Core principle: one frontier, one failure, one hypothesis

Compatibility is a chain. The only unit of progress is **advancing the first
strict failure** while preserving everything behind it. Parallel “also fix”
branches, multi-hypothesis edits, and opportunistic refactors inside an open
failure destroy the signal that the next run is comparable.

Before any code change, answer out loud (or in the session handoff):

1. **What is the current verified frontier?** (last place the title runs without
   diagnostic flags)
2. **What is the first strict failure after that frontier?** (exact file, line,
   values)
3. **What single producer created the bad state?** (encoder, parser, HLE ABI,
   layout, resource update—not only the assertion that fired)
4. **What one falsifiable hypothesis will I test next?**
5. **What evidence would prove the hypothesis wrong?**

If any answer is missing, investigate first. Do not edit.

### How plans are formed

Plans are ordered checklists, not wish lists:

1. **Baseline** — clean tree, known HEAD, green focused tests, working build.
2. **Reproduce** — strict run of the private fixture; capture the *first* fail
   completely under an untracked scratch directory.
3. **Classify** — HLE/ABI, PM4 encode, PM4 parse, surface layout, GPU memory
   relation, shader, VideoOut, sync/label, or host Vulkan.
4. **Hypothesize** — one cause, expected packet/state delta, success criterion.
5. **Test-first** — smallest deterministic fixture or unit test that fails for
   that cause.
6. **Minimal implement** — only the behavior the test (and capture) require.
7. **Verify** — focused green, then strict re-run; expect the same or a *later*
   frontier.
8. **Commit or revert** — commit evidenced behavior; revert failed experiments
   before trying the next hypothesis.
9. **Handoff** — frontier note: previous fail, new fail or checkpoint, residual
   hypothesis, no private paths.

Do not plan modularization, performance campaigns, or multi-NID sweeps while a
strict post-Play (or earlier) blocker is open. Delivery order below is absolute.

### Focus before advancing

- **Stay on the first failure.** Logs after a crash are often wreckage, not
  new work items.
- **Name the seam.** Touch only the module that owns the bad contract
  (e.g. packet encoder vs CP parser vs GpuMemory update). Cross-cutting drive-bys
  are out of scope for that cycle.
- **Prefer producers over symptoms.** A null WaitRegMem address is fixed by
  finding who should write the address (guest patch HLE, adjacent ReleaseMem,
  encoder contract)—not by making the waiter skip null.
- **Freeze working behavior.** If menu reach, flips, or focused tests regress,
  stop and undo before inventing a second fix.
- **Reject “make it continue” patches.** Silent success, assumed RGBA8, linear
  tile, or fabricated labels without a documented encode/execute contract are
  not progress.

### Investigation loop (read-only until evidence is enough)

1. **Baseline comparison**
   ```bash
   git status --short
   git log -5 --oneline --decorate
   ninja -C _build_macos fc_script
   ```
2. **Strict reproduce** (no `KYTY_BRINGUP_*`, no legacy stub/permissive flags):
   ```bash
   _build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
   # Optional silent runner for speed; record that logging was Silent.
   ```
3. **Capture the full fail** outside Git: message, file:line, PM4 header/body,
   register values, submit id / command offset when available, and whether
   AUTO_CROSS or other diagnostics were set (diagnostics never count as
   acceptance).
4. **Map encode → execute.** For GPU issues, locate:
   - HLE builder in `Graphics.cpp` (guest call, arguments, returned pointer)
   - Packet dwords at encode time
   - Whether the guest patches later (`GetDataPacketPayloadAddress`, EopPatch,
     direct stores through the returned header)
   - CP parser in `GraphicsRun.cpp` and the values at execute time
5. **Use Silent vs Console deliberately.** `PrintfDirection = Silent` is for
   wall-clock and long runs; it hides HLE prints. For encode/patch sequences,
   use Console or temporary `stderr` probes that do not depend on Printf, then
   **delete probes before commit**. To surface the diagnostic dumps (shader
   dumps, command-processor traces) that are compiled as `KYTY_LOG_DEBUG`, set
   `PrintfLevel = Debug` in the config; `Error`/`Warn`/`Info`/`Debug` mirror the
   severity gates used by other emulators. The gate is a single relaxed atomic
   read, so a level that does not pass costs nothing.
6. **Compare working vs failing forms.** Same export with non-null address
   earlier in the run is ABI evidence; a post-Play null pair is a different
   contract to explain, not a free pass to invent addresses.
7. **Consult references only for names and patterns.** Public references may
   inform vocabulary and architecture, but every PS5-specific claim must
   reappear in a local capture or test. No incompatible code paste.

### Hypothesis and trial-and-error discipline

Trial and error is allowed; **uncontrolled** trial and error is not.

| Rule | Practice |
| --- | --- |
| One variable | Change a single contract (offset, accepted bit, opcode case). |
| Falsifiable | “If PayloadAddress returns cmd+1 for WaitMem, guest patches non-null.” |
| Time-box | If the capture does not move after a clean experiment, stop and re-classify. |
| Revert failed experiments | Do not stack dead ends; `git diff` should only show the live hypothesis. |
| Record negatives | “Return payload not header breaks SizeDw” is permanent evidence. |
| Prefer structure over silence | Unknown stays `EXIT` / guest error with body dump, not success. |

When a hypothesis fails, write one line: *hypothesis, observation, next
hypothesis*. Then remove the code from that experiment before coding the next.

### Debug and execution tactics

- **First failure only.** Fix order is boot → logos → menu → Play/load →
  gameplay. Never jump ahead because a later log line looks interesting.
- **Encoder dump vs CP dump.** Print or test packet dwords at HLE return *and*
  at parser entry; many bugs are “guest never patched” vs “parser wrong layout.”
- **Adjacent packets.** ReleaseMem/WaitRegMem, WaitFlipDone neighbors, and
  SizeDw residual registers (pointer arithmetic at ±packet size) often explain
  deferred address fills.
- **Return-value contracts.** Gen5 builders often return the **packet header**
  for SizeDw / EopPatch. Returning a mid-packet payload pointer can “fix” a
  store test and break SizeDw—prove return use from capture.
- **Thread races.** Render-thread IndexBuffer update vs CP-thread WaitRegMem
  can both be real; fix the first process exit, then re-run for the next.
- **Input.** `KYTY_AUTO_CROSS` is discovery only. Acceptance needs real edges
  or an explicitly recorded non-claim.
- **Performance.** Never compare FPS under Console logging to Silent; record
  logging mode, resolution, and shader-cache state.
- **Scratch evidence.** Save logs under untracked `_scratch_playable/` or a
  session scratch dir. Never commit guest paths, title IDs, screenshots, or raw
  multi-megabyte logs.

### Red → green → strict

For every semantic change:

1. **Red** — add or extend a unit test with sanitized PM4/HLE args that fails
   for the missing contract.
2. **Green** — implement the minimum; focused filter passes.
3. **Strict** — rebuild `fc_script`, rerun the private fixture, confirm the
   old fail is gone and no earlier fail returned.
4. **Regress** — re-run GraphicsPackets/GraphicsState (and other touched
   suites). Unfiltered full suite may include a historical date-dependent test;
   do not use it to hide new failures.
5. **Commit** — message describes emulator behavior only
   (`fix(graphics): …`), no fixture identity.

If strict does not advance, the change is not done—even if unit tests pass.

### What “done” means for one cycle

A cycle is complete when **either**:

- **(A) Frontier advanced:** the previous first fail no longer occurs under
  strict flags; a new first fail or a stable checkpoint is captured; focused
  tests pass; change is committed; or
- **(B) Blocker documented:** evidence is insufficient to implement without
  inventing; the fail remains structured and informative; capture and
  residual hypothesis are recorded; temporary probes are removed.

“Process survived” or “non-black frame once” without a strict, flag-free path
is not done.

### Anti-patterns (do not do these)

- Editing while the fail is unreproduced on the current HEAD.
- Multiple hypotheses in one commit.
- Keeping a failed experiment “just in case.”
- Broad renames/refactors on an open compatibility blocker.
- Vendor or OS branches inside guest decode/layout.
- Claiming playability with AUTO_CROSS, stubs, or permissive GPU skips.
- Leaving permanent dual implementations or feature-flagged legacy paths.

### Multi-title bring-up

When switching private fixtures (or adding a second root):

1. Keep each title’s root in an **env var only** (`$KYTY_GUEST_ROOT` or a second
   untracked env). Never write absolute private paths into tracked files.
2. Re-capture the first strict fail for **that** title; do not assume the other
   title’s frontier applies.
3. Prefer HLE exports that are **named and sized** (measure APIs, standard libc)
   before open-ended stubs. Unknown Share/Ampr NIDs may log arguments and return
   success only when that is the smallest way to reach the next evidenced fail—
   document residual name/ABI debt in scratch, not as playability claims.
4. After any dependency bump (SDL, etc.), re-run **both** focused unit filters
   and the primary title’s strict path before claiming no regression.

### Vendored dependency bumps

- Prefer official upstream releases into `source/3rdparty/` behind the existing
  CMake wrappers (static SDL, etc.).
- **One high-impact dep at a time** (SDL first). Rebuild `fc_script`, focused
  tests, then the primary strict fixture.
- Hold Vulkan headers and other ABI-sensitive pins unless MoltenVK/runtime is
  revalidated. Prefer SDL2 over SDL3 for this tree.
- Commit messages: host/build behavior only (e.g. `build: upgrade vendored SDL2
  to 2.32.10`), never private title names.

## Current status and verified frontier

### Dirty-page lookup and admission during a camp transition (2026-10-09)

Observed on a Ryzen 5 7600 and Arc A770 with Mesa 26.2.3, native render
resolution, silent console output, shader optimization disabled and shader
validation enabled. Ten saved captures from the 303-second strict baseline
showed no camp or dialogue; the final image was almost black with a small gold
sliver. The reference feature revision reached the camp conversation in a
separate strict run.

Debugger samples of the lookup-only candidate before adding the admission
budget found all 262,144 page-table slots occupied, including roughly 100,000
retired protection tokens. The page lookup can scan every slot for a missing
host-write address. Retiring a page does not imply its token can be discarded.

The correction retains exact page-presence bits per block and checks at most
eight speculative page and block probes for normal-thread small writes. A
positive or inconclusive block lookup restarts the exact page lookup. Identity
publication excludes block-slot reuse during the negative check; the signal
fault path keeps its original exact lookup and does not acquire this gate.

Registration now shares the existing 131,072-page budget across active and
retired identities. It checks the complete cover before changing references
or reactivating retired protection. Refusal leaves late-fault evidence intact;
Objects with readable mapped guest data continue through the existing
content-hash update path on later submissions. No table capacity was increased,
retirement token discarded, wait skipped or workload condition added.

Validation: 114 focused tracker, lookup and VM cases ran (111 passed, three
host-platform skips); seven label-publication cases passed. The boundary and
13-table provenance gates passed, as did 77 playable-harness tests. The
standalone playable guest gate was not run; the native checkpoint below uses
scripted diagnostic input and does not establish gameplay or audio support.

Native A/B: no saved baseline capture showed camp or dialogue at the
303-second stop. The same baseline plus this selected change reached it in two
cold-cache runs, by sampled times of 223 and 163 seconds. The first retained
that dialogue for at least 82 seconds; the repeat advanced to a visible HUD
and characters and remained live until the 307-second stop. Both runs ended
without a fatal or memory-guard stop (312 and 307 seconds). A fresh reference
feature control also reached the camp/HUD and remained live for 304 seconds.
Camera positions differed after the scripted route; this proves the camp
checkpoint, not equivalent rendering or complete playable behavior.

A shared-path roguelike control and candidate each ran for 183 seconds. Both
reached the house interior with character/HUD and healthy final capture
metrics. Both retained storage-frontier errors and early capture warnings;
no fatal, guard stop or WAIT_REG_MEM suspension was recorded. The comparison
does not establish complete controls, audio or visual correctness.

Limits: tombstones retain probe history, so admission does not guarantee an
empty slot or a short exact lookup under arbitrary churn. Retired identities
remain counted even after a later unmap invalidates their protection token;
new registrations can therefore use hashing until identities are safely
reused. (Since 2026-10-11 admission frees revoked identities; see "Dirty-tracker
identities revoked by unmap".) This preserves lifetime evidence at a capacity and performance cost.
Long-session performance, other GPUs, complete gameplay and audio remain
unverified. Dark camp captures also receive a low-entropy heuristic verdict,
including the reference capture; scene recognition requires visual inspection.

### Contract-closure validation checkpoint at translator127 (2026-10-09)

On retry 2, the focused CPU suite passed 278/278 and selected native integrations
passed 37/37; the Vulkan validation scan found no matching identifiers. Native
PARAM validation covers 30 nonzero-mask pair cases and two EXEC controls, with
the enabled-component results proven. Repeated partial-vertex retention,
compressed PARAM exports and VSKIP remain unqualified.

The full CPU suite completed on retry 4 in 55.84 seconds: 2,317 cases across
147 suites, with 2,313 passed, four skipped and zero failed. The skips were
three OS-owned virtual-memory cases and
`EmulatorKernelLifetimes.FailedPthreadDestroyPreservesNativeObject`, which is
unavailable on this host. The fresh `LastTest` evidence was 526,062 bytes
(SHA-256 `a317373c...e6b364`); the validation scan found no VUID and the
foreign-monitor count was 0. All eight source, table, shader and archive gates
exited 0 on retry 4. The pinned Nix environment was unavailable; validation
used the host build.

Earlier attempts are historical: full CPU retry 2 stopped at
`VertexExportsDeclareParametersUsedByTheShader` because `exp_control=0xff` was
unknown; the same default was present in PARAM7/8, which that run did not reach.
Setting it to zero made the exact three-test filter pass 3/3 on retry 3, but the
full retry 3 was interrupted when an external guest run began.

No guest execution result is included in this checkpoint. It establishes no
menu, gameplay, 20 FPS performance or cross-vendor result.

**Historical finding — guest device-address snapshot refresh and in-flight readers.** Static source inspection showed
`SnapshotGuest` copies refreshed guest bytes into `chunk.copy` in place, and tracked chunks pass the same allocation
to host-pointer import (`GuestDeviceAddress.cpp:183-199, 429-504, 961-973`). `CommandBuffer::Execute` submits work
and returns before fence completion; command-buffer rotation can begin recording another slot while earlier
submitted work remains pending (`GraphicsRenderCommandBuffer.cpp:1562-1613; GraphicsRun.cpp:537-582`). The
write-back dependency path waits for pending storage write-backs, not for every asynchronous device-address reader
(`GraphicsRun.cpp:2779-2795; GuestDeviceAddress.cpp:843-852`). Range invalidation and release are explicitly
quiesced operations, while `GuestDeviceAddressPrepare` refresh has no corresponding reader-fence wait
(`GuestDeviceAddress.cpp:761-813, 961-973`). Escalation found no visible reader-completion guarantee for this refresh;
host coherency does not establish device execution completion. If refresh overlaps a reader, a host/device data gap is
possible, but the guest trigger is unknown and no runtime cause has been proven. No runtime overlap or corruption has
been observed. The reader-aware repair and its focused native validation are
recorded in the current checkpoint above. Real workload causality remains
unknown; page-granular refresh remains on hold.

### Historical contract-closure validation checkpoint at translator126 (2026-10-09)

This was a frozen validation checkpoint for shader translator 126 and geometry translator 86. The available default
and integration targets build successfully. No guest was launched with this checkpoint, so it establishes no guest
frontier, menu, performance, compatibility or cross-vendor result. A later translator change requires a new checkpoint
and fresh validation.

The broader acceptance target remains a visibly verified main menu held above 20 FPS. Earlier pre-126 measurements
recorded 5.33 FPS (median 185 ms, p95 201 ms) for the older binary and 3.79 FPS (median 259 ms, p95 358 ms) for a
combined candidate. Those runs did not meet the target and do not describe translator126. A prior run of the remaining
action workload also failed to reach a menu; its guest-memory import observation is historical and was not repeated
with translator126.

The focused CPU filter passed 277/277 tests. The full CPU suite passed 2,312 of 2,316 tests, with four skips and no
failures; the skips were three host virtual-memory cases unavailable on this host and one native pthread-destroy
preservation case. Selected native integration passed 36/36 tests. The native `ParameterExecWriteback` fixture passed
its full-EXEC and zero-EXEC cases. A five-case native UINT16 writeback matrix passed for R16, RG16 and RGBA16 formats,
including partial component masks, mixed MRT attachments and the bypass case. These fixtures ran on the available
Intel Vulkan device and do not establish guest output or behavior on other GPUs. All eight source, table, shader and
archive gates exited successfully. The Vulkan validation error scan returned no matching identifiers.
The pinned Nix environment was unavailable; validation used the configured host build.

At translator126, the validated PARAM EXEC fixture used the full mask; its partial-mask path still wrote a whole
vector and remained under repair.

The retained scalar-flow analysis with immutable program owners builds and passes the focused and full CPU suites;
no performance comparison exists. Remaining holds include DCC plane profile, ownership and completion; indexed
vertex, texture and sampler bounds and lifetime; AudioOut2 and append semantics; the independent one-byte
`SW_64KB_R_X` offset oracle; NGG and fragment transport; ShaderDumpGuestProgram extent and lifetime; conditional
command-buffer branch semantics; and Windows physical-memory proof. AMD/NVIDIA GPU execution, other OS execution,
the pinned Nix environment and live CI remain unverified. The zero-fill AJM fake-success path now produces a strict
unsupported-run refusal without publishing output, result or batch state; the real compressed decoder is still
unimplemented.

No title route, performance acceptance or remaining open contract is closed by this checkpoint.

### Interop integration and single-precision MODE contracts (2026-10-09)

Historical integration snapshot at translator 122; the translator126 validation checkpoint above supersedes its
build and test status. The runtime observations in this section are not results for translator126.

This branch integrates the published fronts branch at `a54f6c7c` into the interop branch (`1605d37a`) with a
three-way merge from their common base `b208fb3e`. Both sides are kept: the bounded SRC1 translation, partial-export
refusals, memory, loader, libc, network, VideoOut and compute-coordinate repairs of the interop branch, and the
published queue-depth, `vcc_hi`, gradient, waterfall, export-flow, tiling, descriptor-bound, view-identity and
write-observation work. The guest descriptor snapshot keeps the published vector form and reads through the scoped
descriptor reader, so exact resource-fold capture still sees every descriptor-table read. Translator versions 103
and 120 both described emitters that this tree no longer produces; the merged emitter is version 121 and the MODE
changes below are version 122. The geometry translator stays at 86.

**CLAMP, OMOD and IEEE mode (f32 and f16).** Kyty dropped the VOP3 `CLAMP` bit in IEEE mode. The ISA text is
ambiguous here: section 6.2 mentions `CLAMP` in the same paragraph that ends "output modifiers are ignored if the
IEEE mode bit is set", while section 3.5 lets only `DX10_CLAMP` choose the clamped NaN result. The decision rests on
two independent sources that agree IEEE mode keeps the clamp: published RDNA2 hardware measurements of
`v_mul`/`v_add` with `clamp` saturate in both IEEE modes, and an LLVM 23 compile for gfx1030 folds a `[0, 1]` clamp
into the `clamp` bit of a kernel whose descriptor sets `ieee_mode 1`. The f32 and half emitters now apply `CLAMP` in every mode; this supersedes the translator-34 entry
below for `CLAMP` only. `OMOD` now follows the ISA: it is ignored in IEEE mode and whenever single-precision output
denormals are enabled (`FP_DENORM` bit 5). When applied, it steps the exponent on the bit pattern: zero and
denormal results become +0, `mul:2`/`mul:4` saturate to infinity, a `div:2` result that leaves the normal range
becomes +0, and infinities and NaNs pass unchanged. A host `OpFMul` had kept -0 and denormals and ignored both mode
conditions. Open divergence: the ISA flushes -0 to +0, but the same published measurements return -0 for `div:2`
of the smallest negative normal. As for the min/max case below, Kyty keeps the ISA result and the published
contract outranks the single secondary source until RDNA2 hardware confirms either.

**V_MIN/V_MAX/MIN3/MAX3/MED3_F32.** The emitters follow the ISA 12.7/12.12 pseudo-code: in IEEE mode a signaling
NaN is returned quieted (first operand first); otherwise a NaN first operand returns the second operand and a NaN
second operand returns the first; the minimum of signed zeros is -0 and the maximum +0; MED3 returns MIN3 when any
operand is NaN. Previously signaling NaNs passed unquieted in IEEE mode, signed zeros used the host's unspecified
choice, and MIN3/MAX3/MED3 used host `FMin`/`FMax` NaN handling. Open divergence: with two NaN operands the ISA
returns the second operand, but the published hardware rows of the same secondary project return the first. Kyty
keeps the ISA order (a published contract outranks a single secondary source); the payload choice needs a
measurement on RDNA2 hardware.

Compute programs use their recorded launch controls; unknown compute controls keep the previous defaults (no
DX10 clamp, no IEEE, f32 output denormals flushed). The MODE word already enters every stage's shader identity.
Half-precision clamps keep their existing admission: with f16 output denormals enabled they are refused outside
IEEE mode but admitted in IEEE mode. A half clamp is exact, so the IEEE case applies it; the asymmetry is recorded,
not resolved, and no workload in the sweep reached either case.

Verification: `EmulatorShaderArithmetic.FloatModeClampOutputModifierAndMinMaxContracts` emits and validates
production modules for each mode and evaluates them with the CPU contract evaluator; a compute variant runs the
same cases on compute modules with recorded launch controls. It failed on the previous
emitters (the IEEE clamp returned NaN for DX10 clamp) and passes after the change. Two earlier assertions that
encoded the old behaviour were updated with the evidence above: the half conversion clamp in the same suite and
the IEEE clamp-source check in the graphics diagnostics integration. The SDWA `mul:4` and min/max selection packet
tests now check the new emission. No GPU execution of these exact cases exists on AMD hardware.

**Host context requests.** `HostContextTrampoline` now publishes `completed` with a single-byte store issued after
the saved context is copied back and after RSP has left the request-owned stack; the raising thread no longer
reads the request after queuing it. A real-signal regression (`OverlappingContextRequestsCompleteAndReclaim`)
delivers 800 overlapping requests from two raisers to two guest threads, checks every handler call, both targets'
resumed execution and the final reclamation. It passes 3 of 3 runs on the fix. The old ordering also passed 50 runs, all with `MALLOC_PERTURB_` (25 also with
the opt-in context trace, which produced no output in the test host), so the use-after-free window itself is not
reproduced; the test is a completion,
reclaim and liveness regression. Still open: a request queued to a thread that exits before taking the signal is
never completed, so its 1 MiB stack is retained for the process lifetime (`LibKernel.cpp`, `KernelRaiseException`).
This repair is not linked to the fishing adventure's startup freeze.

**Diagnostic bounds.** The opt-in material trace and the rejected vertex-layout logger now bound their traversals
to the fixed attribute, resource and sampler arrays while printing the decoded counts unchanged. The vertex input
decoder refused nothing and wrote past those arrays when a layout merged more than 16 attributes into one buffer,
used more than 16 buffers or resources, or named a semantic beyond the 16-entry fetch table; each case is now an
explicit refusal. A fetch-table index above 15 only reads guest memory and keeps its warning.

**New refusal evidence.** A critical transient-pool refusal now also reports the refused usage's used and free
entries, used four-byte and four-byte-zero entries, snapshots and duplicate payloads. The D16 depth-reference refusal
now names each containing render target's format, extent, samples, layers, mips and content stamp, and every
overlapping object's content origin, sequence, submission, update times, in-use, dependency and write-back state.

**Runtime observations (Intel Arc A770, Mesa 26.2.4, strict, Native, Silent).**

| Run | Workload | Result |
| --- | --- | --- |
| d1900, d1901 | Robot dungeon workload, wait deadline 15 s and sync-wait tracking | 202 s each, advancing; zero suspended waits and zero producer-not-found; stall not reproduced |
| d1902 | Mechanical-fist platformer | 27 s, guest locks null+0x58 from the compressor preset path (startup race reproduced) |
| d1903 | Mechanical-fist platformer, debugger attached | Passes the race; stops at 30 s on the unresolved `sceAgcCbBranch` import |
| d1904 | Arcade racing workload | 162 s, race reached, no D16 refusal; frame-time profile matches the previous sweep |
| d1905 | Isometric roguelike, allocator error-edge probe | 165 s, probe armed, zero error edges, zero null faults |
| d1906 | Fishing adventure, sync-wait tracking | 162 s, advancing; startup freeze not reproduced |
| d1907 | Pixel cave platformer | Excluded: a long blocking agent request starved the observer, which stopped the run at 105 s |
| d1908 | Pixel cave platformer, route ends at 120 s, agent holds | Cave scene reached; held Square and Cross were each read by the guest in 90 samples; character unchanged |

`sceAgcCbBranch` exists only in held local work whose compare semantics are unproven, so the mechanical-fist
platformer cannot reach its earlier storage-capacity refusal on the published contracts; the capacity front stays
unmeasured on this tree. The pixel cave platformer's earlier "wake up" endpoint did not reproduce on this tree:
both the shortened route (d1908) and the unchanged sweep route (d1919) reach the cave. Its earlier cause is not
established, and that endpoint is not evidence of lost input.

**Final sweep (d1910-d1922, one executable for every route, SHA-256
`2bdf2ccdb2e8750bbb43d1f9b16247ff0410d54809899d628a5d819cab1f048f`).** The final source differs from that executable
only in the `div:2` underflow sign above and in tests. No route stopped on a fatal error except the
mechanical-fist platformer, and no event-ring drops occurred. Final captures were reviewed visually against the
previous sweep's endpoints. All observer `gameplay_accepted` values are false; scene and action acceptance needs
the playable gate's checkpoints.

| Workload | Seconds | Final scene |
| --- | --- | --- |
| Fishing adventure | 163 | Boat, sailing tutorial and HUD |
| Perspective puzzle | 292 | Lit room with desk; transient loading stall, then healthy watches to the end |
| Sword platformer sequel | 162 | Cave, character and HUD |
| Isometric roguelike | 163 | Initial area, character and HUD; no null row copy |
| Watercolor platformer | 162 | Pale figure on a nearly white image, as before; acceptance needs a same-scene reference |
| Penitent platformer | 161 | Cavern, character and HUD |
| Prison roguelike | 162 | Initial area, item tooltip and HUD |
| Camp brawler | 202 | Black throughout (see below) |
| Sandbox | 203 | Seeded ready-to-play setup with complete text |
| Pixel cave platformer | 161 | Cave with ladders and character |
| Robot dungeon workload | 203 | Tutorial scene with HUD and movement prompt; healthy watches; earlier present stall not reproduced |
| Arcade racing workload | 165 | Race with HUD; no D16 refusal |
| Mechanical-fist platformer | 58 | Passes the startup race; refuses the one-byte `SW_64KB_R_X` 400x5x5 volume (fixed later the same day; see below) |

The camp brawler stays black and issues about 3% of its earlier draws while presenting normally. A build of the
merge result without this entry's changes reproduces the same black endpoint (d1923), so the cause precedes this
change set. Inference, not tested: the earlier endpoint came from the local candidate with the held audio context
bundle, which this tree does not contain, so the missing context admission is the leading candidate. A Console
printf run that shows whether the AudioOut2 context calls are refused would decide it. The mechanical-fist
platformer's one-byte volume equation is also held local work, so it remains a refusal here.

The strict playable gate on the isometric roguelike install with the final source fails as measured: input is
delivered (three taps, six guest read-state samples), but `no_runtime_failure` reports the first error
`gfx_storage_frontier`, no local baseline exists, and no reviewed scene/action checkpoint fixture exists. No
gameplay acceptance is claimed.

### Classic runtime and input boundaries (2026-10-08)

This slice completes observed parser/runtime failures and the native compute
wave-width gap. It does not establish new title compatibility.

- Failed or busy mutex destruction retains the guest object and handle.
  Condition waits save and restore the guest recursion depth around the single
  native lock, including timeout returns. A depth-two signaling flow and a
  depth-two timeout exercise the public APIs; this is historical BSD/POSIX
  compatibility evidence, not a new console measurement.
- Resolved integer conversions preserve the classic base-0/base-2 `0b` token
  boundary and use 64-bit guest LP64 results independently of host `long`.
  Linux/glibc scanf forwarding explicitly uses the retained C99 entry point,
  avoiding the host's C23 `%i` binary-prefix extension. Other scanf host ABI
  paths, Annex K sized strings and non-glibc C23 behavior remain unqualified.
- ELF input reads check physical file extent, seek/read status and exact byte
  count. Program/section tables and dynamic termination are bounded. RELA and
  image-relative JMPREL are checked against a declared loadable memory extent;
  OS relocation offsets are checked against the separate dynlibdata buffer.
  Empty tables retain valid end-boundary pointers. SELF containers may omit
  declared section payloads. Export preflight refuses ranges larger than its
  32-bit write interface before creating output, and omitted SELF sections
  cannot be saved as a complete ordinary ELF. Failed input cleanup is reusable;
  repeated dynamic segments retain the prior last-entry behavior without leaking
  superseded buffers. Eager dynamic, dynlibdata and section-name buffers share
  a 64 MiB aggregate host resource budget, checked before allocation with
  replacement accounting. This is an implementation policy, not an ELF ABI
  limit; modules exceeding it are refused. Adjacent-segment relocation tables
  are refused; symbol/string/hash/PLTGOT bounds are outside this slice.
- Native Gen5 wave32 compute derives sensitivity from guest wave-state uses,
  then selects exact width 32 from the queried default or enabled compute
  subgroup-size control. Local dimensions, invocation count, shared memory
  and subgroup-count limits are checked. Full subgroups and X divisible by 32
  are required for sensitive native programs. The actual SPIR-V 1.6 module
  supplies that guarantee; older modules need the enabled full-subgroup feature
  and stage flag. Guest coordinates are reconstructed from subgroup ID and
  lane, including dispatch thread-limit masks; Vulkan does not prescribe their
  equality to LocalInvocationId. Partial native workgroups remain unsupported.
  The paired wave64 path keeps its separate requirements. Sensitivity and
  required width enter cache identity; translator version 103 invalidates older
  coordinate emission. These conditions follow the published
  [subgroup index](https://docs.vulkan.org/refpages/latest/refpages/source/SubgroupLocalInvocationId.html)
  and [full subgroup](https://docs.vulkan.org/spec/latest/chapters/shaders.html#shaders-full-subgroups)
  contracts.
- Output options initialize exactly 64 bytes with byte 2 set to `0xff`.
  Validation checks handle, reserved arguments, mode, then option words; word 3
  is caller controlled. ConfigureOutput consumes the full five-argument ABI.
  Full-range option copies hold the guest VM mapping lock and validate ownership,
  permissions and overflow. Invalid non-null pointers return INVALID_ADDRESS
  as a host-safety decision; that return has not been measured on the console.
  The default cadence is supported; high refresh is reported unavailable and
  configuration refuses it until the presentation clock implements it.
  Console observations disagree on selector 18's error (`0x80290016` versus
  `0x8029001e`); current unknown-selector classification follows the latter
  observation and is not established across firmware revisions.
- POSIX host-unreachable socket failures retain the corresponding guest error.
- Conditional LDS exchange opcodes no longer execute as ordinary additions.
  Both widths refuse before lowering. Opcode availability is documented, but
  exact older-target operational semantics and pair atomicity remain unproved.

The owned Linux Release build passes with parallelism limited to two. The
focused runtime/parser/graphics suite passes 209 tests; five direct integration
binaries and 22 loader/table CTests pass. Native wave32 execution on an Intel Arc
host produces two distinct full-wave ballot masks. A production-emitter fixture
with local shape 32x2 verifies seeded X/Y coordinates and initial EXEC masks
`0xffffffff`/`0x00000000` for a Y thread limit of one. Paired-wave fixtures remain
green. Sparse metadata-budget cases fail with the preceding loader object, and
the invalid-options-pointer case faults with the preceding video object; both
pass with the corrected objects. A process-local host-connect failure probe
verifies the public socket error path. Emulator-boundary and table-provenance
gates pass.
The emitter-precondition fixture now gives its synthetic end instruction a
forward PC and checks CFG validity: its previous backward PC returned a
nonempty rejection token before exercising the intended emitter. All ten
precondition cases pass after this test-only correction.
The complete suite through the script host passes 2,122 tests with eight skips:
three Windows/macOS mapping cases and five media-fixture cases were not exercised.
The development supervisor's live lifecycle cases run and pass through this
host entry point. Nix is unavailable on this host; the existing development
definition is retained and no dependency is introduced by this slice.

A strict 31.6-second roguelike replay delivers three taps and records six guest
read-state samples, but retains the prior `gfx_storage_frontier` failure. Its
playable regression gate fails; the scene/action checkpoints remain unproved.
A second strict run holds the streaming title's menu for 17 seconds with no
runtime error. Its scored menu capture was labelled `hot_corruption` by the
native heuristic. That raw label is a palette false positive: the
maintainer-supplied expected menu reference shows the same warm yellow, orange
and red palette, and the composition is visually consistent with it. The menu
comparison establishes neither gameplay acceptance nor a performance improvement,
and it does not qualify the whole title's rendering.

No AMD, NVIDIA, Windows or macOS execution has been performed for this slice.
The direct GPU mask fixture currently requires the paired probe's enabled
size-control path; no-node default-width behavior has modeled coverage only.
The coordinate fixture observes initial EXEC and thread-limit masking; later
guest EXEC writes and possible reactivation of initially inactive lanes remain
unqualified.

Secondary blend outputs have bounded translation and offscreen host coverage; a
consumed guest pass is still needed to qualify compatibility.
Sampled-parent freshness and depth reuse need producer/owner/content evidence;
indexed tables need a nonzero consumed index, extent, stride and generation;
compression metadata needs its plane, encoding and completed first write.
PCM/ATRAC9 append needs observed cursor, starvation, lifetime and completion
transitions. None of those behaviors is inferred from another implementation.

### Bounded secondary blend translation (2026-10-08)

Admission (all required): Gen5 pixel program, stage enabled and not embedded;
effective blend0 uses an SRC1 factor (0x0f..0x12) after the paired SRC_ALPHA
override rule; one active physical color destination; the only color exports are
MRT0 and MRT1, each non-null export to both targets has EN=0xF, and each target
uses one packing. Full (32_ABGR, mode 9) and COMPR (FP16_ABGR, mode 4) are both
admitted when the modes match. Operations are ADD, SUBTRACT or REVERSE_SUBTRACT.
Standard component order is required. Union enable masks alone are insufficient,
because partial writes can leave secondary components undefined.

Resolution uses the parsed program and the materialized draw state before the
shader, module and pipeline keys, on both indexed and auto draw paths. MRT1 is
emitted as Location 0 Index 1; the primary output remains Location 0. The DSB1
identity word is added only to the dual variant, so ordinary identities and
decorations are unchanged. Location admission counts one destination, while
combined-output resource accounting keeps both declared Output variables.

Host admission queries enabled dualSrcBlend, maxFragmentDualSrcAttachments and
COLOR_ATTACHMENT_BLEND_BIT of the actual optimal format. There are no vendor or
title branches, no new DB_SHADER_CONTROL selector and no EXP lowerer changes.
Other unqualified SRC1 states retain strict refusal. Ordinary non-SRC1 draws
retain their existing admission rules. No secondary values are synthesized.

Verification on this host: red tests reproduced the missing secondary decoration,
the missing cache distinction and the unsafe partial and split enables before the
fixes. A fresh Linux Release build passed. The 68 focused contract tests passed.
The script host run reported 2137 tests, 2129 passing, 8 skipped (three platform
memory cases and five media-fixture cases) and no failures. The compute, wave64,
host and memory integrations passed, as did the graphics default and NaN baseline.
24 loader, table and raster CTests passed. The strict architecture gate and the
13-table provenance gate passed.

The offscreen Vulkan integration runs encoded guest FP32 and FP16 programs
through the parser, the production resolver, ShaderRecompilePS, SPIR-V
validation and a real pipeline. On an Intel Arc A770 (DG2) with Mesa 26.2.4
vulkan-intel on Linux 7.2.9, the interior pixel is RGBA(128,191,191,255) and the
untouched corners are (0,255,0,0) for both variants. A disabled real feature and
a nonblendable integer format are both refused. Readback uses a color-to-transfer
layout transition, a transfer-to-host visibility barrier, an exact fence and a
non-coherent invalidate. This proves the bounded host translation only, not a
consumed SRC1 pass in a game.

Strict runtime evidence: a 32.9-second roguelike replay delivered 3 taps and 6
guest read-state samples, and it keeps the gfx_storage_frontier and the failed
playable gate. Two streaming runs of about 17 seconds each had no runtime error;
the first final frame was a loading scene and is not comparable with a menu
reference. The repeated menu shows the expected composition and warm palette. Its
native `hot_corruption` label is a palette false positive for this menu. The
reference qualifies the observed menu appearance only; gameplay, performance
and a consumed SRC1 pass remain unverified.

Not performed: AMD or NVIDIA physical execution, Windows and macOS runs, and
Vulkan VUID or synchronization-layer validation. The validation layer and Nix
are unavailable on this host; the existing development definition is retained and
no dependencies were introduced. Indexed tables need a consumed nonzero index,
extent, stride and generation. DCC needs the guest layout, the full metadata plane
and the writer completion. APPEND needs flags, cursor, lifetime, starvation and a
callback oracle. These were investigated and not activated.

### Shared runtime and resource contracts (2026-10-08, bounded validation)

This change set starts from committed feature HEAD `b208fb3e` in a separate
checkout. The source checkout's uncommitted work was not imported or changed.
The results below describe shared contracts; they do not establish additional
title support or supersede the earlier gameplay reports.

- **Sampler audio.** Standard PCM8/16/24/32 and float blocks render through the
  existing NGS2 lifecycle, with source-rate conversion, pitch, gain, queue reset,
  repeat/end handling and bounded callback dispatch. Mixing accumulates before
  one final clamp. A bounded ATRAC9 decoder uses the existing FFmpeg dependency.
  The owned compressed fixture is silent: non-silent quality, skip placement and
  block seams remain unverified. VAG/HEVAG playback is not added. Audio/video URI
  replacement callbacks are exercised with an owned media file; this does not
  establish general guest-VFS-backed decoding. See `docs/AUDIO.md`.
- **Dirty memory.** Sparse, bounded range metadata admits more than 512 disjoint
  ranges. Write-watch leases retain guest permission authority across protect,
  unmap, aliases and concurrent restorers. Signal handling performs no lazy
  tracker initialization and never waits on a permission fence. Normal writers
  wait for permission publication; range queries are serialized with registration.
  Read and execute faults are not claimed as writes on the verified Linux path.
  Process-wide fault state remains alive until process termination.
- **Layered sampled images.** Type-12 descriptors retain the one-dimensional
  coordinate and array slice through classification, bank counts, Vulkan views
  and image emission. The GPU fixture covers 12 load/sample cases, including
  mip level, base slice and wrap/edge/border addressing. Production guest upload
  is qualified only for linear uncompressed, single-level data; the host fixture's
  mip chain does not prove a general guest mip-upload contract.
- **Shared GDS transfers.** Command DMA and shader access use one backing store,
  with checked windows, recorded transfer barriers, bounded publication and labels
  ordered after completion. The production append fixture seeds a counter with
  41, adds 16 active lanes, publishes 57 and preserves the neighbouring word.
  GPU-produced source bytes replace stale CPU bytes in the transfer fixture.
  Eight malformed/protected/unmapped cases refuse strictly. Raw image sources,
  exclusive queue-family migration and discard semantics remain unqualified.
- **Structured shader flow.** Canonical block, edge, dominator and loop analysis
  replaces three local loop decisions for the non-banked emitter. Nested loops,
  shared continues and terminating/discard tails validate; unresolved targets,
  irreducible or unsupported multi-level control flow refuse. Analysis budgets
  are explicit. Existing wave-bank, EXEC and lane analyses remain separate.
- **Exact replay and diagnostics.** Schema-v3 resource-fold capture records the
  owned evaluator inputs, five caller-provided output seeds and ordered guest
  reads. Other prepopulated bind state is unrepresentable. Eight fresh private
  captures replayed byte-equal, including a pixel-stage layout offset and slot;
  these eight captures contained no descriptor-table reads. Synthetic contracts
  cover missing, reordered, unused and failed reads. Operation records are
  default-off and bounded; an owned producer-to-file-to-Python flow preserves its
  dependency edge and external input. This is not synchronization evidence.

**Validation environment:** Linux x86_64, GCC 16.2.1, Vulkan 1.4.363, Intel Arc
A770 (DG2), Mesa Xe driver version 109060100. A dedicated Release build used at
most two compilation jobs. All affected Linux executable targets built. Windows and macOS were not
compiled. Focused C++
validation passed 887 cases with three expected host/platform skips; the Python
tool suites passed 115 cases, and nine wrapper/real replay CLI cases passed.
Nine production-path integration executions passed, including GDS append, the
layered-image fixture, graphics diagnostics, compute, kernel memory and audio.
The paired logical-wave fixture also executed on the GPU. Architecture and all
13 graphics-table provenance gates passed.

The integer endpoint runs the production ISA parser, emitter and Vulkan path.
Five operations matched an independent host integer reference over 460 edge and
seeded pairs; a further 1024-pair run used all 64 admitted dispatches, and an
external 67-pair input completed. The physical subgroup was 32 and the logical
wave 64. This is Vulkan execution evidence, not a native AMD comparison. AMD
hardware/HIP and Nix were unavailable; the exact dependency declaration was
source-checked but neither entered nor evaluated.

**Strict guest limits:** a 33-second scripted opening route delivered all three
input edges and produced a scored, visually inspected opening image. It retained
the baseline's first `gfx_storage_frontier`; both baseline and changed runs failed
the gameplay gate because no scene/action checkpoint was reached. A separate
roughly 18-second startup held presentation with no runtime error; its captures
showed branding/loading and a flat transitional image, not gameplay. No cross-run
performance improvement or additional compatibility claim follows from these runs.

CPU-demand publication still lacks a measured steady-state bottleneck. Indexed
sampled tables still lack a consumed runtime-key pattern, extent, stride and
generation contract. Raw compression metadata still lacks its plane, encoding,
ownership and first-write completion contract. These remain investigation items;
no guessed fallback or metadata write was introduced.

### Portable resource completion (2026-10-08)

The follow-up removes host capability assumptions and closes a deferred write
lifetime defect. Admission depends on queried and successfully enabled Vulkan
features, memory type bits and mapping ownership, without vendor selection.
The implementation source is recorded at `ae228c7746527f3f6eeba0332efda0d8a5f57363`.
Validation compiled the identical source before those commits were recorded;
the private run metadata therefore reports `f2795437` with pending changes.

- **Core subgroup features.** The window device and compute probe query and
  enable subgroup size control through one Vulkan 1.3 feature aggregate. The
  existing Vulkan 1.4 floor makes the older extension name and revision
  diagnostic only. Enabled state is published after device creation succeeds.
  Exact subgroup width, full subgroup, stage and operation requirements remain
  enforced. A compiled comparison of the previous and current capability
  helpers refuses and then admits the same core-enabled, extension-absent
  paired request. Modeled admission also covers ranges containing 32 and a
  64-only native request; it is not execution evidence on those hosts.
- **Probe cache maintenance.** The integration compute probe prefers compatible
  coherent host-visible memory and can use compatible noncoherent memory. It
  maps the entire dedicated allocation, flushes the output seed once and each
  metadata write before submission, and invalidates output after the host
  barrier and successful fence wait. Failed cache maintenance refuses the
  submission or read. A native-call model compiled the actual probe and checked
  separate host/device views, repeated submissions, incompatible type bits and
  flush, wait and invalidate failures. Available hardware exposes only coherent
  host-visible types; actual noncoherent hardware remains untested. This change
  does not add noncoherent support to production resource pools.
- **Allocation refusals.** The production allocator refuses an unsatisfied
  memory-type request before modifying allocation state or calling the native
  allocator. Buffer creation, allocation and binding errors stop before an
  invalid resource can be used. A native-call model linked the production
  implementations and verified rejected requests, successful type selection,
  cleanup and the strict native error paths. Requested production memory flags
  remain requirements; they are not silently relaxed.
- **Deferred guest ownership.** A bounded snapshot records the mapping
  instances covering a GDS destination. Mapping replacement, commit and
  decommit issue fresh identities; splits and protection changes retain them.
  Publication checks all identities and current guest write access in the same
  VM transaction as the copy, so a partial mismatch copies nothing. Capture
  requires ownership only: a read-only destination that becomes writable before
  completion still receives its bytes. A GPU fixture using the previous
  callback accepted an unmap/remap before completion; the current fixture
  strictly refuses it. Normal, armed and read-only-at-record publications and
  their neighbouring bytes pass. The existing tracker lease and write-generation
  update remain unchanged. Unregistered end-of-pipe destinations still receive
  nothing, while unsafe registered destinations refuse.

**Validation:** a dedicated Linux Release build of all affected executables
passed with two compilation jobs. Focused validation passed 1079 cases with
three expected host/platform skips. Nine sequential production-path integration
executions passed, including nine GDS refusal scenarios, append, layered images,
compute, graphics diagnostics, memory and audio. The logical-wave fixture passed
on the GPU, and five integer operations again matched the host reference over
460 edge and seeded pairs. Architecture and all 13 table provenance gates pass.
The available device is an Intel Arc A770, Mesa 26.2.4 with Xe. AMD and NVIDIA
hardware, Windows and macOS execution remain untested. No dependency was added
by this follow-up.

**Strict guest recheck:** the approximately 32-second opening route again
delivered three input edges, advanced presentation and stopped without a host
crash, while retaining the baseline's first `gfx_storage_frontier`. Its scored
capture was darker than the preceding run. A bounded repeat retained four
non-stripey detailed opening images (entropy 3.73–3.89, 504–572 quantized colours),
including the earlier subtitle, before a flat final image. This excludes a
persistent loss of opening detail in that repeat; the brightness variance and
later flat output remain unlocalized. Both gameplay gates fail for the known
resource frontier and absent scene/action checkpoint. These observations do not
establish complete visual equivalence, additional title support or a performance
improvement.

A separate 17-second startup run had no last-error event and advanced 7518
presents during a 15-second watch. Its captures showed branding and then a
visually inspected detailed introductory scene; no input/action gameplay
checkpoint was exercised. In the subsequent 15028 ms performance interval,
497 writebacks transferred 6115328 bytes in 15430055 ns. Unlike the earlier
loading-only interval, this one has steady publication activity, but it still
does not establish a CPU-demand publication bottleneck or an A/B speedup.

The focused commands are `cmake --build <owned-build> --parallel 2`,
`<owned-build>/kyty_unit_test --gtest_filter='CoreVirtualMemory.*:EmulatorComputeWave*.*:EmulatorNativeWave*.*'`,
`<owned-build>/integration_test/kyty_gds_transfer_integration`, its `--append`
mode, `kyty_shader_compute_integration`, `kyty_shader_wave64_integration`,
`check_emulator_boundaries.py --strict source`, and
`check_graphics_tables.py source/emulator/src/Graphics/Tables/manifest.sha256`.
Private guest routes, native captures and call-model artifacts stay outside Git.

Runtime-indexed sampled tables, raw compression metadata publication and
CPU-demand publication remain subject to the evidence limits in the preceding
section. Exclusive GDS queue-family migration also requires a proven ordering
contract; changing resource sharing alone would not establish synchronization.

### Flexible snapshot lifetime and standard math (2026-10-08)

A production device-address-table fixture reproduced stale data after a
GPU-visible flexible owner changed from CPU read-only to writable: a Vulkan
load retained the previous marker instead of the new CPU-written marker. A
second fixture reproduced the same defect through a CPU-only writable interval
followed by restored GPU read access. Existing residency and cross-page load
controls passed. The double-precision standard distance-function export was
also absent from the real symbol database.

The bounded fix makes a non-writable interval within one previously GPU-visible
flexible owner writable through a lifecycle completion while GPU admission is
closed and submissions are drained. Successful completion updates guest rights
and protection records under the owner lock, then invalidates only its
device-address imports before admission reopens. Ordinary GPU objects and
VideoOut registrations retain their lifetime. Writable snapshot runs end where
CPU write eligibility changes, so a read-only neighbour does not prevent ongoing
write tracking. Retired address tables are destroyed at quiesced invalidation
or release; the current table remains available until its next rebuild.

Deferred writable protection also retains the original mapping identity. The
completion validates every page of the requested interval under the same VM
transaction that changes rights, without the deferred-copy size budget. Two
executed race fixtures first demonstrated that a completed unmap and same-address
replacement could receive an old owner's protection change. Both now refuse
with ENOENT, preserving the replacement's bytes and rights and performing no
snapshot invalidation. A pending unmap instead returns EBUSY without mutation.

The POSIX fixed-allocation path without native no-replace support now reserves
the exact free interval before committing its mapping. A frozen production
backend with native no-replace disabled reproduced a partial-overlap allocation
that replaced live bytes, rights and identity. The corrected backend refuses
that overlap and preserves all three. Private and shared mappings use the same
reservation operation; the Apple native reservation does not request overwrite.

The missing double export delegates to the standard host operation through the
existing C calling convention. Its symbol-database fixture checks a finite
5-12-13 result, large and small scaling, positive zero, infinity and NaN.

**Verification status:** the original stale-marker and missing-export failures
and both replacement races were executed before their fixes. The final eight
affected Linux targets build with two jobs. The focused contract suite passes
170 cases with three existing platform-specific omissions. All six actual
Vulkan device-address fixtures pass, including repeated permission cycles,
partial ranges, ongoing CPU writes and refused pending-unmap protection. The
forced no-replace-absent backend passes its five collision and identity cases.
Its full VM suite also passes 40 cases with the same three omissions. Six
sequential kernel, GDS copy/append, compute, graphics and host integrations exit
successfully. The strict emulator-boundary check and all 13 graphics-table
provenance checks pass. The GDS integration's armed-destination case remains
unexercised there; the device-address fixtures install the real write-fault
route and exercise tracked CPU writes. Independent reviews found no remaining
blocker in the permission transaction or native reservation. Retired-table
reclamation has no measured host-memory improvement. Windows and macOS behavior
and AMD/NVIDIA hardware have not been executed in this audit.

The precommit Linux snapshot is based on `565287d7`, with the changes in this
entry present and reported as dirty. Execution used Linux 7.2.9-arch1-1,
GCC 16.2.1, an Intel Arc A770 (DG2), Mesa 26.2.4 with Xe KMD, two-job builds,
silent guest output and strict masks of zero. The Nix runtime was unavailable;
the existing host build environment was retained. A bounded roguelike route
still reports `gfx_storage_frontier` and fails
the gameplay gate because scene/action evidence is absent. Three input edges
were consumed; its detailed opening capture passes the offline material-health
gate, with entropy 3.9678 and 459 quantized colours. This does not establish
scene equivalence or gameplay. A separate 17-second startup run reaches a
visually inspected detailed title menu, advances 9098 presents during a
15-second watch and records no last-error event. Its menu capture passes the
offline material-health check (entropy 6.8164, 628 colours); the native heuristic
still warns about hot-colour regions. No matched visual reference or gameplay
action was exercised. Its 15026 ms steady interval has zero writebacks, which
does not establish a publication speedup or a CPU-demand bottleneck.

Focused validation uses:

```text
cmake --build <owned-build> --parallel 2 --target kyty_unit_test fc_script kyty_agent kyty_kernel_memory_integration kyty_gds_transfer_integration kyty_shader_compute_integration kyty_graphics_diagnostics_integration kyty_host_platform_integration
<owned-build>/kyty_unit_test --gtest_filter='CoreVirtualMemory.*:EmulatorGuestDeviceAddress.*:EmulatorKernelMemory.*:EmulatorGraphicsDirtyTracking.*:EmulatorGpuMemoryFault.*:EmulatorGuestMemory.*:EmulatorModuleLoad.Libc*'
<owned-build>/integration_test/kyty_kernel_memory_integration
<owned-build>/integration_test/kyty_gds_transfer_integration
<owned-build>/integration_test/kyty_gds_transfer_integration --append
<owned-build>/integration_test/kyty_shader_compute_integration
<owned-build>/integration_test/kyty_graphics_diagnostics_integration
<owned-build>/integration_test/kyty_host_platform_integration
python3 scripts/check_emulator_boundaries.py --strict source
python3 scripts/check_graphics_tables.py source/emulator/src/Graphics/Tables/manifest.sha256
python3 scripts/kyty_capture.py score <private-capture>
```

The strict gameplay gate uses `scripts/kyty_playable_regression.py`; private
guest arguments, scripted routes and native-backend proof artifacts stay
outside Git.

**Limits:** the permission transaction covers a writable request contained in
one flexible owner with a retained GPU cleanup obligation. A request spanning
multiple owners still follows the generic path in `KernelMprotect` in
`source/emulator/src/Kernel/Memory.cpp:3259` and can retain an immutable flexible
import. Extending it requires an interval-wide owner and permission transaction.
Physical aliases, broader GPU permission transitions and NoAccess snapshot
semantics are unchanged. Unknown audio timestamp, null-drain and mastering
contracts and conflicting zero-count save-memory rules remain unimplemented.
The existing evidence limits for indexed resources, compression metadata and
CPU-demand publication continue to apply.
### Publication selection and held contracts (2026-10-09)

Historical publication-selection snapshot at translator 120. The translator126 checkpoint above supersedes this
section's build and test counts; its held-contract notes are updated below where later evidence changed their status.

The selected source revision is `b7456c71da384bcfbc484990a5363c121a8fdd29`, with translator version 120.
It contains the reviewed mutex, video-decoder, scalar/EUD, image-gradient, export-flow, pixel-LDS, memory-observation,
storage-snapshot, format-view, Standard256B and vertex-input corrections. Storage-upload and depth-refusal diagnostics
preserve the existing strict failures. Version 120 invalidates modules generated by the full local candidate's
experimental unsigned emitter; retaining version 119 for this different compiler policy could reuse those modules.

An independent Release build of this exact selected revision links `fc_script`, `kyty_unit_test` and `kyty_agent`.
Its executable SHA-256 is `3472cd4dd067d0a2cb20343ca129a0f2e66ebf86d5a11da33d10565789f9ebe2`.
The full suite passes 1,960 of 1,968 tests in 43.101 seconds, with zero failures and eight optional/media skips.
The AVC/HEVC configured-frame-bounds fixture executes and passes. Strict emulator dependency checks, the 13-table
provenance lock, five table-checker fixtures and 77 playable-runner harness tests pass. The native playable gate
records an explicit missing-guest skip; the harness results are not gameplay evidence.

Earlier startup and twelve-route sweep results below used the full local candidate, including unpublished changes.
They do not validate this selected source. This publication does not establish a new native title frontier,
visual equivalence or gameplay acceptance. The local files are retained for further investigation.

Independent review holds these contracts from publication:

- **AudioOut2 context/queue/latency bundle:** port limits and flags lack a verified admission contract, while
  `AudioOut2Set3DLatency` returns success without storing or applying latency. The absence of current object-port
  rendering does not prove that global latency has no effect. The queue-depth correction's local tests do not
  establish a safe independent subset of the startup path.
- **Non-indexed indirect draw:** the earlier local candidate that omitted the vertex/instance patch offsets is
  absent from the translator126 target. The current indexed-indirect path validates and copies its complete
  20-byte argument span through checked guest-memory access. This bounds the indexed record only; non-indexed
  offset semantics and behavior remain unimplemented and unverified.
- **One-byte `SW_64KB_R_X` detile:** the round-trip input and reference use the same production offset helper.
  An independent offset oracle is still required, including block transitions and slice behavior.
- **Unsigned 16-bit exports and new attachment formats:** the earlier source audit had only synthetic SPIR-V evidence.
  At translator126, a five-case native raster/readback matrix now validates production translation and writeback for
  R16, RG16 and RGBA16, including partial component masks, mixed MRT attachments and a bypass case on the available
  Intel Vulkan device. This closes those bounded fixture cases only; guest execution and other GPUs remain untested.

The adjacent-mapping indirect-register snapshot change retains a separate range check followed by a host copy.
It bounds register count and validates the complete allocated span, but does not prove atomicity against a concurrent
guest free/remap. Submission-time guest ownership remains an unresolved condition; this is not a lifetime-race fix.

### Loading regression under expanded dirty-page admission (2026-10-09, unresolved)

A fresh strict run of the selected published source above reached the beat 'em
up's camp cinematic at about 184 seconds. The maintainer confirmed manually
closing that run at about 200 seconds; exit zero was not an unexplained guest
termination. A bounded scripted route and one observed agent tap were used.
This establishes that cinematic checkpoint, not combat, audible audio,
sustained playability or native-like performance.

Two immutable binaries from a separate dirty integration candidate continued
presenting the loading glyph in bounded runs of about 284 and 304 seconds,
without a recorded fatal error. Those binaries include uncommitted changes;
the observation cannot be attributed to the candidate's committed HEAD alone.
The second run used debugger profiling. All runs used Native resolution,
Silent logging, shader validation, no shader optimization, isolated cold shader
caches, and the same private route and Intel Vulkan driver. These settings and
debugger pauses are not a normal-performance benchmark.

A paired, read-only debugger probe sampled dirty-page metadata using each
binary's own type layout. It persisted aggregate counts, not guest bytes:

| Approximately 75-second diagnostic sample | Published source | Dirty integration candidate |
| --- | ---: | ---: |
| Page-table capacity | 262,144 | 262,144 |
| Occupied slots | 123,195 | 262,144 |
| Empty slots | 138,949 | 0 |
| Pages with active references | 87,519 | 161,538 |
| Token-valid retired pages without references | 35,676 | 100,606 |

The integration candidate already had no empty slots at the 35-second sample.
At the owned stop its loading thread was in `FindPage`, reached through
`VisitWritePages`, `NotifyWrite` and `c_memcpy` for a 16-byte write. The actual
binary's lookup loop was inspected separately. `GpuDirtyPageTracker::FindPage`
(`source/emulator/src/Graphics/GpuDirtyPageTracker.cpp:113` in the selected
source) stops on an empty key or a match, otherwise scans the full table.
Consequently every absent-page lookup costs 262,144 probes when no empty key
remains. The expanded candidate permits many more registered ranges while
retaining that fixed page-table capacity. The published source's smaller range
admission was full at 512 entries at its owned stop. Restoring that cap would
not establish a correct scalability repair.

Table saturation and lookup activity were observed on the loading path. The
sampled call's hit/miss, cost and frequency remain unmeasured; its causal
contribution to the branch difference is still a hypothesis. Loading
allocation/release counters continued to advance. A later-window sample recorded
256 distinct direct memory releases, all returning zero, with a maximum duration below 380 ms.
Seeing the same release caller in two stack samples therefore did not establish
one permanently blocked release or an infinite GPU wait.

Do not erase token-bearing retired slots merely because their reference count
is zero: unregistration attempts to restore ordinary permissions, and
`UnregisterRetainsLateWritableFaultEvidence` requires a later write fault to
remain handled after a successful restore. Failed or uncertain restores must
retain the corresponding authority and fault evidence. A repair must bound
absent-page lookup cost while preserving late-fault evidence, mapping identity, authority fencing and denied-protection
behavior. Reclamation inside a VM authority callback must not wait on the
publisher count held by that callback. Next acceptance requires a deterministic
saturated-table reproducer with a bounded lookup-cost contract and checks for
active, re-registered and retired pages, denied-write/unmap transitions, failed
restores and changed authority. Any early reclamation proposal also needs a
deterministic pending-fault test paused after page lookup and before restorer
announcement. A controlled A/B must establish the repair's loading effect, followed by a strict run reaching and holding the cinematic
checkpoint and continuing beyond it without a fatal error. Native captures and
real action-response evidence remain required before any gameplay claim. No
runtime repair was made by this investigation.

### Mechanical-fist platformer startup frontier (2026-10-08, not gameplay)

These runs used the full local candidate, including changes held from publication. They used Silent logging,
the five-second cross route and a 180-second bound. The observations below do not establish the frontier of the
selected published source; none is a gameplay or visual acceptance.

- **One-byte 64 KiB render-target volumes (published 2026-10-16).** The title samples an R8 400x5x5 volume with tile mode 27 (`SW_64KB_R_X`).
  The local `Sw64kRxWithinBlockOffset` candidate uses a one-byte equation inferred from a 16-pipe address pattern: bits 0..15 are
  `x0 x1 x2 y1 y0 y2 x3 y4 | x3^y3 x4^y4 x6^y5 x5^y6 | y6 x6 y7 x7` inside a 256x256 block. The existing two- and
  eight-byte equations match the same published table exactly, which ties the new form to the same source. A
  round-trip test (400x260, pitch 512) constructs its input and reference with the production offset helper, so it
  does not independently establish the one-byte equation. Publication requires an independent oracle. The
  four-byte micro tile no longer follows the depth (`Z_X`) order: guest-written atlas levels showed the
  `x0 x1 y0 y1 y2 x2` order on 2026-10-16. Open: the slice XOR of the published
  pipe bits is not applied because volumes are detiled as independent thin slices.
  Oracle search (2026-10-16, still open): decoding the published 16-pipe 64 KiB tables shows the shipped two- and
  eight-byte equations equal `SW_64K_R_X` and the four-byte one equals `SW_64K_Z_X` exactly. For one byte the two
  tables share bits 8..15 but differ in the micro-tile (`R_X`: `x0 x1 x2 y1 y0 y2 x3 y4`; `Z_X`:
  `x0 y0 x1 y1 x2 y2 x3 y4`), so the table cannot choose the order the guest uses. The racing title's eboot lists the
  guest GPU-address library among its linked libraries, but no table in the published per-bit encoding (64-bit
  `x|y|z|s` masks, per-channel mask arrays or per-bit records of 8 to 64 bits) occurs in it; other eboots were not
  searched. The next oracle is the library's own code or a
  guest-written one-byte target sampled back.
  Guest-content oracle (2026-10-16, excluded as decisive): no GPU object overlaps the volume at bind, so its ten
  64 KiB blocks hold guest CPU data. Nearly every byte is non-zero, with an eight-byte repeating pattern. Scoring
  the mean neighbour difference of each candidate's detiled image (x/y) gives the held equation 37.5/11.3, linear
  92/77 and the standard 64 KiB equation 71/36. With y bits 3..5 in order `y0 y1 y2` instead of `y1 y0 y2` the
  score is 37.5/9.1. The data supports `x0 x1 x2` in bits 0..2 followed by y bits. It cannot place x3 and above
  (the content repeats every eight texels) or y3 and above (height 5). The one-byte equation therefore stayed held.
  Volume oracle (2026-10-16, decisive for the slice XOR): a later dump of the volume holds 0xFF in every texel the
  guest wrote, and other bytes elsewhere in its ten blocks. With the published pipe XOR of the slice
  (`s0->bit 11, s1->bit 10, s2->bit 9, s3->bit 8`, the same XOR the depth layout already applies per layer) all
  2000 in-bounds texels of each of the five slices land on 0xFF; without the XOR, or with the bit order reversed,
  slices 1..4 read foreign bytes. The guest data does not decide between `y1 y0` and `y0 y1` in bits 3..4, so the
  published table order is kept. `TileGetSw64kRxOffset` now accepts one-byte elements and a slice; single-level
  `SW_64KB_R_X` arrays and volumes pass the layer as the slice (CPU detile; the GPU detile path refuses slices).
  Verified: the mechanical-fist platformer passes the volume in a strict Intel run and next stops at the
  unresolved `sceAgcDcbDrawIndirect` import (implemented below). Open: multi-level `SW_64KB_R_X` mip chains are still detiled without
  the slice XOR.
- **Storage-image channel order.** A formatted store into a BGRA-ordered `R8G8B8A8_UINT` storage image wrote red and
  blue swapped. Only RGBA8 UNORM/SRGB storage images get a BGRA host view; the shader now applies the descriptor's
  destination select to stores (inverse permutation) whenever the host view cannot (`ShaderStorageImageSwizzleInShader`
  with the format-aware `VulkanStorageHasRedBlueView`).
- **`v_cvt_pk_u16_u32` / `v_cvt_pk_i16_i32` saturate.** The ISA defines `D.u16_lo = u32_to_u16(S0.u32)`; the
  conversion clamps to 0xffff (signed: to [-0x8000, 0x7fff]). Two independent compiler contracts agree, so the
  range-proof guard that only admitted provably in-range sources is replaced by `UMin` / `SClamp` for any source.
  The signed emitter also stopped selecting a `uint` into a `float` destination, which produced invalid SPIR-V.
- **UINT16 color exports (historically held local candidate).** `SPI_SHADER_COL_FORMAT` UINT16_ABGR (7) into a UINT color buffer locally declares a
  `v4uint` fragment output and stores the raw 16-bit components of each export dword (`OpBitFieldUExtract`), keeping
  disabled components. FP16 (mode 4) exports into UINT buffers keep the established float interface; the sandbox
  regression recorded above came from keying on the buffer number type alone. `R16G16_UINT` (format 5, type 4) is a
  local render-target format. **Status update at translator126:** the five-case native UINT16 matrix described above
  now passes exact readback on the available Intel Vulkan device. This supersedes the earlier absence of native
  writeback evidence for those formats, but does not establish guest-route or cross-vendor acceptance.
- **Non-indexed indirect draw (published 2026-10-16).** `sceAgcDcbDrawIndirect` (`1q1titRBL6o`) takes the same
  operands as the indexed builder (command buffer, byte offset from the `SetBaseIndirectArgs` base, draw modifier)
  and emits `IT_DRAW_INDIRECT` with the non-indexed patch offsets. The processor checks and copies the 16-byte
  argument record (vertex count, instance count, start vertex, start instance) and issues an auto-index draw whose
  first vertex adds the start vertex to `GE_INDX_OFFSET` and whose first instance is the start instance. Like the
  indexed path, the patch offsets are not written to user SGPRs. Verified: the mechanical-fist platformer resolves the
  import in a strict Intel run and stops about four seconds later at an unrelated storage-image overlap; no capture
  was taken, so draw output is not yet verified.
- **Storage image over recycled guest-byte caches (published 2026-10-16).** The same title creates a 1024x1024
  two-byte storage image over a 2 MiB range that still holds a CPU-uploaded linear 1024x540 texture and two read-only
  0xa0 vertex buffers. Mixed parent types stopped at `!create_all_the_same`. Each parent only caches guest bytes, so
  the new multi-parent rule reclaims the read-only vertex buffers (as under an incoming texture) and links the sampled
  texture as the single-parent rule does. It requires CPU-upload origin and no pending write-back for the texture,
  and applies only to mixed vertex-buffer and texture parents, which previously always stopped. Verified: the next
  strict Intel run passes the overlap and stops about 30 s in at a missing SDWA `v_cvt_f32_f16` emitter.
- **SDWA half-to-float conversion (published 2026-10-16).** The half-precision emitter refused every SDWA form. The
  widening conversion now admits SDWA with a DWORD destination and a WORD_0, WORD_1 or DWORD source select without
  sign extension (ISA SDWA fields); the operand load already extracts the selected word, and SDWA NEG/ABS apply to
  the half. Byte selects, partial destinations and SDWA on other half operations stay refused. The CPU SPIR-V
  evaluator checks the high-word and negated low-word forms. Verified: the next strict Intel run passes the shader
  and stops about 35 s in at the unparsed `image_sample_d` (MIMG opcode 0x22).
- **Two-dimensional `image_sample_d` (published 2026-10-16).** SAMPLE_D supplies the four slopes of a 2D coordinate
  before x,y (ISA VGPR usage table) and derives each pixel's LOD from its own slopes, so it is an explicit-gradient
  sample per invocation. It shares the 2D float emitter of the coarse `image_sample_cd`; only the coarse form keeps
  the pixel-stage, quad-uniform gradient proof. Other dimensions, NSA forms with one extra dword and A16/G16
  controls stay refused at parse. Unit tests parse the opcode and validate the emitted module with unproven slopes.
  Verified: the next strict Intel run passes the parser and stops about 35 s in at the fragment wave transport
  refusal for a pixel shader whose memory writes would expose helper lanes (2688-wide draw), the same open
  contract as the sandbox title's new-world stop.
- **Pixel memory writes under a helper-free EXEC (published 2026-10-16).** The refused pixel shader saves the launch
  EXEC, enters WQM, removes discarded lanes from the saved copy with `s_andn2`, and before its only
  `buffer_store_format_x` sets `exec = saved & exec`. The fragment mask flow admitted device-memory writes only before
  the first EXEC write. The guest/host difference is helper lanes alone: a guest helper in EXEC writes, a Vulkan helper's
  store has no effect. So a buffer, image or GDS-counter write is now admitted wherever the flow proves EXEC holds no
  helper lane (the `kWide` bit an export already requires). That subsumes the launch-prefix rule. LDS and scratch
  writes, and GDS append/consume after the prefix, stay refused. The proof had a gap: an `S_*_SAVEEXEC` destination
  took its width from the explicit sources, so a saved WQM EXEC counted as helper-free. It now inherits EXEC's width.
  Unit tests refuse a store under WQM and after restoring a saved WQM EXEC. They admit a store after narrowing by the
  launch EXEC; reverting the SAVEEXEC change fails the latter test. Offline, the title's refused shader is now
  admitted as quad local. No saved program of the sandbox title's 285 s new-world stop exists, so that stop is still
  untested. Verified: the next strict Intel run passes the shader and stops about 40 s in
  at `unsupported Gen5 image numeric type: format=27` (`ShaderSpirvImage.cpp`).
- **Sampled 16_16_UINT images (published 2026-10-16).** The same title samples a Gen5 format-27 texture. The
  RDNA2 image-format table lists 27 as `16_16_UINT`, between `16_16_SSCALED` (26) and `16_16_SINT` (28). It is now a
  four-byte unsigned-integer `R16G16_UINT` image in the Gen5 format table and in the element-size table. Storage
  reinterpretation views are unchanged. Verified: the next strict Intel run passes it and stops about 52 s in at a
  missing pixel-stage emitter for `s_load_dwordx4`.
- **Open: descriptors spilled to VGPR lanes (recorded 2026-10-16).** The same title's next stop, about 52 s in, is
  `shader emitter missing ... SLoadDwordx4 s[32:35], s[28:29], 896` at pc 0x4 of a pixel shader. The shader is under
  heavy SGPR pressure and spills its descriptors with static-lane `v_writelane` into one VGPR: the V# from EUD+896
  (lanes 17-20), the V# from EUD+848 (lanes 14-16, 21), and an S# from EUD+720 (lanes 0-3), with more later.
  It reloads them with `v_readlane` into other quads (`s[96:99]`, `s[16:19]`, `s[88:91]`, `s[92:95]`) and uses them
  as V# and S#. `ShaderFindDynamicSLoadUse` (`ShaderResources.cpp`) drops the EUD+896 mapping, because the spill is a
  read that is neither a consumer nor a clobber, so `recompile_sload_from_extended` finds no row.
  - Excluded: simply ignoring the spill. A storage row's word 0 is the host binding index (`UpdateAddress48(i)` in
    `PrepareStorageBuffers`), and a sampler or texture row's word 0 is a host index too. The reloaded copy has no
    resource record at its register, so it would take the pixel guest-address path
    (`PixelScalarBufferUsesGuestAddress`) and read guest memory at that index.
  - Direction: carry descriptor provenance through static-lane spill slots, from the `v_writelane` of each word to a
    `v_readlane` reassembling all words in order with the slot not rewritten in between. Each reload consumer then gets
    a dynamic record for the same resource, so the runtime word 0 selects the binding as for the original register.
    T# halves assembled from two loads plus reloads in the same shader need their own evidence.
- **SGPR104 and SGPR105.** The RDNA2 scalar operand range is 0..105; the parser stopped at 103, and the shared lowering
  preconditions and compute-wave analyses used the same limit, so a pixel shader reading `s104` was refused as an
  invalid instruction before native wave analysis. The limits follow the ISA; the NGG passthrough proof, which keeps
  VCC in slots 104/105, rejects the two registers instead of aliasing them, and the fetch decoder bounds its SGPR table.
- **Non-final pixel Z export.** Target 8 (MRTZ) with `done=0` ahead of the color exports (`PixelZVsrc0Vm`) uses the
  same depth emitter as the final form; the depth-export proofs accept both.
- **SGPR104/105 in the shared lane-flow numbering.** The fragment mask-flow proof refused the same shader with "scalar
  destination out of range": the numbering shared by the lane-flow proofs (`ShaderLaneFlow.h`, NGG passthrough
  dependencies) packed SGPR0..103 and then VCC/EXEC/M0/SCC. It now holds SGPR0..105 followed by VCC (106), EXEC (108),
  M0 (110) and SCC (111). The 2688x1512 depth prepass, whose only mask use is the save, `s_wqm_b64 exec`, restore
  bracket around its derivative fetches, is then proven lane-local and runs on 32-wide host subgroups without the
  fragment wave transport.
- **`SW_256B_S` mip chains.** The 256-byte swizzle modes have no mip tail: every level is padded to whole 256-byte
  blocks and the chain is stored from the smallest level up, so level 0 ends the allocation (published address-library
  `ComputeSurfaceInfoMicroTiled`). `Gen5GetStandard256BTextureMipLayout` / `Gen5DetileStandard256BTextureMipChain`
  place and detile each level; tiled mip chains use the canonical pitch (width), not the linear row rule. A test pins
  the offsets of a 16x16, five-level chain and round-trips every level against the per-element equation.
- **MRTZ export with no channel.** Target 8 with `en=0`, `done=1`, `vm=1` carries only the valid mask, exactly like
  the NULL target, and decodes as `NullVmDone` (the discard tail of a depth-exporting shader).
- **Depth export with control flow.** `ShaderCodeHasSafePixelDepthExport` admitted only straight-line programs. It now
  admits forward control flow when every `S_ENDPGM` closes a block that holds the exact Pixel-Z export or is a discard
  tail (`s_mov_b64 exec, 0`, then the valid-mask-only export); a branch target starts a new block, a conditional
  branch's fall-through continues its own, and indirect jumps stay refused. A tail that keeps lanes alive without a
  depth write is still refused (tested).
- **Pixel translation failures name their cause.** `ShaderRecompilePS` now fails with the SPIR-V assembler/validator
  message instead of returning an empty module that a later assertion reported without a reason.
- **AudioOut2 context parameters (local candidate; held).** `ContextQueryMemory` and `ContextCreate` compared the whole 0x40-byte block with
  two captured profiles. A gdb trace shows this title filling six leading 32-bit fields (max ports, max object
  ports, guaranteed object ports, queue depth, grain at +0x10, flags; for example 1/0/0/2/256/1 and 8/64/0/2/256/1)
  and leaving the reserved tail uninitialized after `ContextResetParam`, so all eleven attempts were refused; the
  engine fell back to `sceAudioOut` and its `sceAudioOut2PortCreate` calls failed with INVALID_PORT. The six fields
  are read and the reserved area ignored (grain bounded to the 64..4096 frames the host mixer handles); both contexts
  and their ports now open. Titles that used to fall back now take the AudioOut2 path: the camp brawler needed
  `sceAudioOut2Set3DLatency` (NID TViD1EZXkNI), which accepts the system user with latency mode 1 or 2 and refuses
  anything else. It returns success without storing or applying the setting; the current absence of object-port
  rendering does not establish that this is a valid no-op. Context port limits and flags also lack an evidenced
  admission contract. The whole context/queue/latency bundle remains local. The camp brawler (203 s) and the
  isometric roguelike (163 s) advanced with this full candidate, not with the selected published source.

Remaining startup race (timing dependent; upstream cause unresolved). About 26-32 s after launch, the multiband-compressor submix
effect takes its channel-count re-initialization path and reads its preset weak pointer twice in the same
audio-render call: the first read succeeds, the second returns null and the guest locks the preset's mutex at offset
0x58, which faults in `PthreadMutexLock`. Earlier passing runs did not take that path. A subsequent bounded debugger
run did take it: the channel count changed from zero to eight and both preset reads returned the same valid pointer,
before a later shader refusal. Channel re-initialization alone therefore does not imply the null fault. At the fault
the weak pointer and its object item are valid again and only 26 of 21416
objects are unreachable; because gdb stops the other threads milliseconds later, this does not exclude a
garbage-collection mark phase at the moment of the second read. Excluded: input (two runs without a route fault at
27-28 s), guest thread priorities (lower host nice for low-priority guest threads: 4 of 4 still fault), low-memory
garbage collection (the title queries only the direct memory size, 16 GiB). Most Silent runs fault there;
debugger-attached runs pass more often.

The subsequent audio trace created eight-channel MAIN ports and a stereo type-6 port, then destroyed and recreated
the type-6 port immediately before the zero-to-eight channel transition. It made no `PortGetState` or
`GetSpeakerInfo` calls during the observed startup, so incorrect replies from those APIs do not explain this
transition. The temporal association with type-6 creation is not proof of causality. `ContextCreate` also discarded
the parsed queue depth and retained the default of four: the captured depth-two profile reported four available
grains. Propagating the defined field makes it report two; the real context lifecycle regression failed before the
change and the focused AudioOut2 suite passes all 19 tests afterward in the local bundle. The bundle remains held
from publication. Advance/reservation timing, type-6 routing and
the weak-pointer race remain unverified contracts; the queue repair is not claimed to resolve the race.
The focused comparison of four local behavioral references does not settle those contracts: port-state writers
agree on the channel offset but use different sizes and channel-selection rules; Advance ranges from reservation
or clock updates to a no-op, with different Push pacing and queue-drain rules. Type 6 is generally separated from
the main speaker path, but routing differs, and user-ID validation and destroy lifetimes also disagree. These are
implementation observations, not hardware specifications; no further audio behavior is changed from them alone.

**Standalone VCC high-word destinations.** The captured pixel shader `0x01000150/0xed53afa8` loads one scalar-buffer
dword into `vcc_hi`, immediately before a scalar move. The zero-resource lowering used the indexed operand helper,
which supported the VCC pair but omitted standalone VCC_HI and emitted `OpStore % %uint_0`. The indexed helper now
maps the one-word destination to the existing uint variable; zero-resource lowering also validates all destinations
before appending any stores. The real fragment fixture reproduces the assembler's invalid ID before the change and
passes assembly and validation afterward. Translator version 119 invalidates the earlier modules. The next bounded
debugger run emitted `OpStore %vcc_hi %uint_0` for the same captured shader and reached a later storage-buffer
materialization refusal after about 49 seconds. The two captures remained mostly dark; this is translation progress,
not a visual or gameplay acceptance.

**Storage-buffer failure provenance.** The next strict debugger run passes the captured shader and refuses a
copied static scalar-memory span after about 47 seconds. The descriptor declares a much larger range, but the
existing exact-access proof requests only 2296 bytes. The CPU copy succeeds; the critical transient upload returns
null, and this branch never calls GPU-object creation. There is no GPU-heap allocation or overlap for that span.
`TransientBufferPool::Upload` can return null on this route only when `Acquire` rejects its capacity predicate:
per-usage entries, total entries or byte capacity. That capture does not identify which limit failed. A subsequent strict run with the bounded critical-pool
rejection report refuses a four-byte unmapped-base carrier at the 512-entry per-usage limit: 674 total entries and
16,234,178 bytes remain below the total-entry and 16 MiB byte limits. Its failure bits identify only the per-usage
cap. The report uses the existing capacity predicate and emits at most eight critical refusals; ordinary snapshot
fallbacks remain quiet. No duplicate immutable payload sample has been captured. Do not increase the limits or add
content reuse without proving the needed lifetime and byte-identity contract.
The failure diagnostics distinguish the preparation paths (including the separate early unmaterialized-range fatal),
the exact byte arguments, CPU-copy result,
wrapper versus Vulkan handle, mapping, overlapping objects and backing provenance. Translation and failure
localization are verified; the startup race, buffer materialization and gameplay remain open.

Also observed in the regression sweep: the isometric roguelike faulted twice (9-11 s, of six runs) in a host `memcpy`
called by the guest at eboot+0x308054, a row copy into a staging buffer whose base pointer, returned through the
out-parameter of eboot+0x31a8e0, is null. It advanced in the other runs, including the full 163-second route.

### Four-front verification (2026-10-08, bounded; acceptance still open)

An isolated Release Ninja build with two build jobs succeeds for `fc_script`, `kyty_unit_test` and `kyty_agent`.
The final full suite runs 1975 tests: 1967 pass and eight optional/environment cases skip, with no failures. The
51-test focused shader/cache/precondition/AudioOut2 filter also passes. `check_emulator_boundaries.py --strict`
and the 13-table graphics provenance gate pass. The 77 tests of `test_kyty_playable_regression.py` pass; the actual
private-workload acceptance gate is skipped because its guest root is unset, so this is not gameplay acceptance.
Two behavioral regressions were observed failing before repair: the captured queue-depth context lifecycle and the
fragment shader's invalid standalone-VCC_HI store. Both pass afterward, and the strict workload passes that shader.

Runtime observations use Native resolution, Silent logging, shader validation and the existing scripted routes on
an Intel Arc A770 with Mesa 26.2.3. No permissive runtime repair, skipped wait or per-title branch is introduced.
Debugger runs characterize failures and are not performance measurements. The final twelve-workload sweep is
complete on the same executable for every route: SHA-256
`90de16a98a248e206325c2bc5feea1023055ff0a36bfcbef0b7e939da0cca0be`, based on
`b208fb3e9d3240ad749b9832f06ee195ca76ddda` with the preserved local changes and translator version 119.
There were no observed fatal errors in that sweep and no event-ring drops. All observer gameplay-acceptance
results remain false. A nonfatal last-error event or a progressing frame counter alone is not visual or gameplay
acceptance. First and last available captures are scored offline; the final available capture of each route is
also reviewed visually. For the stalled route, the only image precedes the stall.

| Workload | Observed seconds | Final observed scene and limit |
| --- | --- | --- |
| Fishing adventure | 163 | Boat, navigation tutorial and HUD; presents advance, startup freeze not reproduced |
| Perspective puzzle | 283 | Lit room and wall at the route endpoint; presents advance despite the coarse FROZEN label |
| Sword platformer sequel | 164 | Dark cave, character at its edge and HUD; low-entropy score alone does not identify a defect |
| Isometric roguelike | 163 | Initial hall, character and HUD; null row-copy fault not reproduced |
| Watercolor platformer | 162 | Pale character and ground on a nearly white image; white-world score, visual acceptance open |
| Penitent platformer | 162 | Cavern, character and HUD |
| Prison roguelike | 162 | Initial combat/tutorial area, character and HUD |
| Camp brawler | 207 | Campsite, characters and HUD; agent phase remains loading/graphics |
| Sandbox | 204 | Seeded ready-to-play setup with complete text; no gameplay acceptance |
| Pixel cave platformer | 161 | Dark wake-up prompt; presents advance but the route does not pass the prompt |
| Robot dungeon workload | 47 | Present stall at frame/present 3492/3491; only a preceding logo capture is available |
| Arcade racing workload | 161 | Race track and continue/retry screen; intermittent depth refusal not reproduced |

The perspective puzzle has an earlier transient present stall during loading, then resumes through the room and
ends with healthy watches. Its final pixels are static because the route faces a wall. The watercolor image
needs a retained same-scene reference before attributing the pale output to a new regression or accepting it.
The pixel cave route presses Cross and moves, while its final prompt shows a different button icon. One additional
bounded run with Square configured at route seconds 124-127 still ends at that prompt after 161 seconds, with
healthy presents and no fatal. The route clock starts at the first guest pad read; actual delivery at the prompt
was not recorded. This does not prove a rendering regression or successful input delivery, and the prior cave
frontier is not revalidated. No further repeat sweep or speculative render change is made.

### Open source-audit findings (2026-10-08)

Update 2026-10-09: the context-request ordering and both trace traversals below are repaired; see the interop
integration entry above. The original finding is kept for its evidence.

Source audit found a separate Linux context-request lifetime defect in `LibKernel.cpp`,
`HostContextTrampoline` / `ReclaimCompletedHostContextRequests`: `completed` is published before the trampoline's
last request reads and before it leaves the request-owned stack. A raise to another thread may reclaim both at
that point. The post-delivery `raise_queued` trace also reads the request after the target can have completed it.
Reclamation must wait until both delivery-side reads and context restoration have ended; a real-signal regression
must exercise overlapping requests before changing this path. This source defect is open and has not been linked
to the fishing adventure's startup freeze. A stored `last_wait_return` with `wait: None` is a historical wait site,
not a sample of the thread's current instruction pointer.

Two additional source-only bounds findings remain outside these repairs: `GraphicsRenderBind.cpp:1952`
(`EmitDrawMaterialTrace`) trusts `vertex.attr_num` while indexing the fixed 16-entry attribute arrays, and
`GraphicsRenderBind.cpp:229` (`FindDrawMaterialTraceSamplerIndex`) trusts `samplers_num` while indexing the fixed
sampler arrays. The triggering state is an opt-in material trace with an excessive decoded count; no title run
demonstrates that state. Reject excessive counts before tracing or bound the diagnostic traversal, without changing
guest-visible rendering to conceal a decode failure.

### Recursive mutex condition waits (2026-10-08)

`pthread_cond_wait` on a recursive mutex held more than once returned EINVAL; the wait now saves the recursion count,
releases the mutex fully and restores the count after waking. `pthread_mutex_destroy` deletes the mutex only when it
succeeds, so EBUSY keeps it usable. Kernel-process unit tests cover both. Effect on the intermittent roguelike stall
(main thread waiting on an event its render thread sets after a GPU label copy) is not established: 2 of 6 earlier
runs stalled at 37-47 s, and 0 of 5 later runs did (one of them with a binary without this change).

### Role-playing title menu defect (2026-10-08, deferred; not gameplay)

Observed defect: the title menu has a flat magenta background, and the traveler selection screen has a black center
instead of its scene. Some earlier runs also omitted whole glyphs from the traveler prompt. These visuals are not
accepted, and the maintainer has deferred their repair. No title-specific color replacement, skipped consumer, or
permissive runtime change was introduced to hide the defect.

Reproduction: use strict Native rendering with Silent logging and a bounded 600 s run. Advance past initial settings,
enter the title menu, and select a new game. Compare captures of the title menu and traveler prompt. The last 600-second investigation candidate
shows the full prompt, but also reproduces the magenta background and black center; earlier good and bad prompt runs
mean one good capture does not establish that the intermittent glyph defect is fixed. That investigation ends after 469 s
at the existing unsupported `SW_256B_S` mip-chain frontier (32 by 32, six levels) in `Tile.cpp`. It was a runtime refusal,
not the time limit or a resource guard. Gameplay and performance acceptance remain open.

Confirmed investigation evidence: a lookup volume sampled by the frame was all zero, and a live merged geometry producer
was refused. Their shared resource identity was verified. This does not prove that the volume causes the magenta output.
The producer uses LDS, barriers, cross-wave operations and geometry exports; a passthrough approximation cannot preserve
its behavior. Further work needs a verified merged-stage launch ABI, workgroup execution, export capture and layered
render-target publication, followed by a causal pixel comparison. Do not relax the current admission guard as a fix.

Two independent source defects were corrected and reviewed:

- `GpuMemoryCreate.cpp` (`CreateObject`): dirty-range registration and `BeginRead` precede hashing and upload, and
  publish the same observation, so a CPU write during creation remains pending. An actual-object red test uploaded the
  older byte while guest memory held the newer one; it passes after the correction.
- The CPU-readable range validator walks adjacent mapped and protection segments, hashes only when the entire span is
  valid, and rejects holes and no-access pages. A split-mapping actual-object red test uploaded the old byte; it passes
  after the correction. Guard-page tests isolate the mapping and protection checks.

The diagnostic candidate passes 51 focused tests and 1952 of 1960 full-suite tests (8 skipped), strict boundary checks,
13 graphics-table checks and the diff check. Independent review found no blocking defect in the two memory corrections.
The native run exercised them, but did not establish that either caused or fixed the intermittent glyph symptom.
Temporary investigation code is removed before publication; the production binary requires its own regression runs.

Remaining limits: an unknown hash of zero compared with zero is not evidence of unchanged bytes, and other such cases
are not claimed fixed. The optional `DUMP_RT` diagnostic retains a raw render-image pointer across presents and may outlive
its object; it remains disabled and unfixed, with no evidence that it causes the production defect.

### Publication regression containment (2026-10-08)

The native validation sweep exposed a new startup refusal in the sandbox
workload after about 22 seconds. The experimental unsigned color interface
selected a UINT16 emitter from the color-buffer number type alone. The live
active attachment used an unsigned 16-bit format, but its shader export mode
was FP16. The producer explicitly packed floating-point values into half
pairs before three compressed MRT exports. Treating those payloads as raw
unsigned components would change their meaning.

A focused nonfinal VM/DONE correction passed its synthetic sequence, but the
next native replay still refused this FP16/unsigned-target combination. It
therefore did not repair the observed regression. The experimental unsigned
shader interface and emitter were withdrawn from publication, preserving
the established FP16 export path. The attachment format mapping predates this
experiment. Its precise hardware behavior for this mismatched pairing remains
unresolved; restoring execution is not evidence of correct integer writes.
Unsigned color-interface support needs a verified export/attachment contract
and an end-to-end native writeback comparison before reintroduction.

**Status update at translator126:** a five-case native UINT16 matrix now
validates exact production writeback for R16, RG16 and RGBA16 integer-export
cases, including partial component masks, mixed MRT attachments and bypass on
the available Intel Vulkan device. This resolves the earlier lack of native
writeback evidence for those tested combinations only. The FP16-export to
UINT16-attachment pairing observed above remains unresolved, as do guest-route
and cross-vendor acceptance.

The preceding candidate passed all 24 export tests and 1,959 full-suite tests
with eight optional/environment skips, yet failed native validation. Those
unit results are historical evidence only. After withdrawal, translation cache
version 117 invalidates earlier modules. A packed-half fixture reproduces the
raw control-word refusal before withdrawal and passes afterward through the
production SPIR-V validator. All 21 export tests and 1,956 full-suite tests
pass, with eight optional/environment skips (1,964 total). Independent review
confirmed the bounded removal preserves the other changes.

The strict native sandbox replay now completes its 200-second target window
(202 seconds observed) without the refusal and reaches the reference ready-to-play
setup screen with the character and complete text visible. This restores the tested startup/menu frontier; it is
not new gameplay or integer-writeback acceptance. Shared-path native validation
is still in progress. Temporary metadata diagnostics are excluded from
publication.

### Publication validation snapshot (2026-10-08, startup and short routes)

The production source snapshot is based on `b208fb3e9d3240ad749b9832f06ee195ca76ddda`
with local changes and translator version 117. Its executable SHA-256 is
`af5c75bf117b531dad585769847548fcf54852b40d1596d29a60eb92d44c636a`.
The host is Linux 7.2.9 on an AMD Ryzen 5 7600 and Intel Arc A770. Vulkan 1.4
uses the same isolated Intel Mesa driver as the reference runs, SHA-256
`b76b9113c4fd7247fc1e3e6898fbd49a2b0fcabb2def13c303747317265a6209`.
This is not validation of other GPUs or the system-installed driver.

Runs use native resolution, shader optimization None, shader validation,
unmodified guest waits, one emulator at a time and recorded input routes.
No permissive bring-up flags are enabled. The limits remain CPU 200%, memory
high/max 7/8 GiB, no swap and a 300-second ceiling; preflight reserves 10 GiB
of available host memory. Logging is Silent except for the explicitly noted
reference-console comparison. No build or heavy analysis overlaps a run.
Captures are scored with the existing capture tool and reviewed visually,
including an independent pass. Scene, camera, pose and load-time differences
limit exact frame and performance comparisons.

| Workload | Target / observed seconds | Observed endpoint |
| --- | --- | --- |
| Sandbox | 200 / 202 | Reference ready-to-play setup, character and complete text |
| Fishing adventure | 160 / 167 | Boat and sailing tutorial, scene and HUD |
| Perspective puzzle | 280 / 286 | First lit room, desk, paper and materials |
| Sword platformer sequel | 150 / 161 | Cave, decorated wheel, character and prompt |
| Isometric roguelike | 150 / 163 | Initial area, character and HUD |
| Watercolor platformer | 150 / 162 | Intro character, dress and ground line |
| Penitent platformer | 150 / 162 | Starting cavern, character and HUD |
| Prison roguelike | 150 / 162 | Initial area and readable NPC dialogue |
| Camp brawler | 200 / 204 | Campsite, characters, scene and HUD |
| Pixel cave platformer | 150 / 161 | Reference cave, ladders and character |
| Robot dungeon workload | 150 / 162 | Tutorial character, HUD and movement prompt; reference Console/Warn settings |

These comparisons found no new material visual or runtime defect at the tested
endpoints after withdrawal of the unsigned export experiment. They do not
establish full playthroughs, general compatibility, stability or performance.
The sandbox uses the same seeded setup route, SHA-256
`0f9fdb039e8cd9a01e4219715a904849f1cd17f87ccf75219258910d6aa8232e`.

Existing limits remain visible: the sword sequel's central transition text
appears as bars in both reference and candidate captures; later prompts are
readable. The penitent platformer's empty HUD medallion also occurs in the
same initial scene of an earlier candidate, before the illustrated medallion
appears at a later state. Coarse STATIC/FROZEN capture labels alone do not
prove a runtime hang.

The robot dungeon workload refuses indirect SH offset `0x1` in
`GraphicsRunOpParsers.cpp` with Silent logging. The exact earlier reference
executable reproduces the same refusal under the current Silent settings;
the candidate completes the earlier Console/Warn reference route with its
title cache. This excludes the new source changes as a necessary cause of
that refusal, but does not identify its cause. Console logging is a comparison
condition, not a newly introduced fix or a performance measurement.

The official playable gate honestly skips because its reviewed guest/scene
fixture is unset. Its 77 harness tests pass; neither that skip nor the native
observer's process-survival result constitutes gameplay acceptance. Additional
startup-only observations are recorded separately after comparison.

### Publication blockers from the expanded inventory (2026-10-08)

Publication is held while the expanded native sweep is investigated. The
maintainer requested preservation of the previously working frontiers and repair
of failures within that agreed cohort before publication. The deferred
role-playing menu colors and intermittent glyph defect remain excluded. The
robot-platformer observations below are out of the repair and publication-gate
scope; their inspection does not authorize a new compatibility campaign.

- The mechanical-fist platformer still has the timing-sensitive preset weak-pointer startup race. The detailed
  section above records the repaired AudioOut2 queue-depth propagation and standalone VCC_HI scalar-buffer
  destination. A subsequent strict debugger run passes that shader and reaches a copied-static-SMEM upload
  refusal: the 2296-byte CPU copy succeeds, but the critical transient pool denies capacity. GPU-object creation
  is not called in this branch. A later four-byte carrier upload is specifically denied by the 512-entry per-usage limit,
  with the total-entry and byte budgets still available. Whether immutable repeated uploads could avoid it remains
  unproved; startup, visuals and gameplay are not accepted.
- The arcade racing workload has an intermittent D16 depth-reference refusal after reaching the race. The request
  is 1 by 1, pitch 256, 64 KiB, tile 24, physically mapped, with one containing RenderTexture (`8/3/1`). The
  depth-source classifier intentionally does not accept a RenderTexture as a byte source. Object overlap alone
  does not establish that the parent is stale, that it has a recent color write, or that its bytes can be reinterpreted.
  The remaining evidence is the parent's format, full range, last write and completion state at the refusal.
  A new bounded debugger run reaches the race and advances for about 163 seconds without the refusal. Its parent
  probe cannot read an optimized local, so it contributes no parent format or last-write evidence. The final
  161-second sweep also shows the race track without the refusal. Keep admission
  unchanged until a correct write-back, copy or view contract is established.
- The isometric roguelike intermittently copies a row into a null upload-buffer base. Static analysis identifies a
  resource allocator that creates metadata even when its backing allocation fails; the caller later uses that
  empty backing. The allocation error status and failing provider have not been captured. In a new bounded
  debugger run the initial 4 GiB direct allocation and mapping return success, and mspace creation returns its
  valid mapped base. The error-only allocator probe is armed, reports no error-edge or null-fault hit, and the
  workload advances for about 164 seconds. This excludes failure of those initial operations in that run only;
  the final 163-second sweep also reaches the initial hall without the null copy. Host exhaustion, fragmentation
  and a game defect are unproven explanations. No allocation fallback is added.
- The fishing adventure's earlier bad run stops after two presents. Its main thread has `wait: None`, which is
  not a live instruction-pointer sample; its stored wait-return address is historical. A worker exists but has no
  active condition wait. Good runs, including the new 105-second debugger observation, show the main thread
  waiting on a semaphore and ten workers on conditions while captures advance through a ship cinematic.
  The early worker-wait pattern differs, but the signalling dependency is not established. The new run does not
  reproduce the startup freeze or establish gameplay. The final 163-second sweep reaches the boat and navigation
  tutorial with healthy presents. A bad run needs live main/worker registers and stacks
  together with the event cursor before changing synchronization or context restoration.
- The robot dungeon workload now records Gen5 `SPI_SHADER_PGM_RSRC4_PS`
  through both direct and indirect register packets. The direct parser
  requires the single-register payload length, and the legacy architecture
  remains refused. Raw state recording does not emulate compute-unit placement
  or establish a performance change. The production candidate passes all
  1,956 existing tests with eight optional/environment skips. The former
  indirect-register refusal is gone, but one Silent run stalls on a suspended
  memory wait. A subsequent debugger run with identical cache seed advances
  for 163 seconds without that wait. This is timing-sensitive evidence, not
  startup acceptance; the suspended wait's tuple and producer remain unknown.
  The final Silent sweep reproduces a present stall in a 47-second bounded observation: a 20-second watch holds
  frame/present at 3492/3491, the main thread record is waiting on EventFlag, and the final capture times out.
  Last-error is null, imports have no recorded failures and the event ring has no drops. Interval diagnostics
  report one suspended wait and one producer-not-found event, but not their compare tuple or provider; those
  counters do not prove they caused this stall. Sync-wait tracking is disabled, so zero tracked waits is not
  evidence of no waits. No thread is identified as the GPU thread in the snapshot, and stored wait-return
  addresses are not live PCs. Keep the intermittent blocker open rather than deriving a producer or skipping a wait.
- Out-of-scope inventory observation: the robot platformer ends after 62 seconds without a
  present or capture. A longer observation is stopped by the observer after
  confirming no progress, rather than completing its requested window. A
  debugger replay reaches a mixed-parent depth-object ownership refusal in
  `GpuMemoryCreate.cpp`: a larger new depth plane overlaps an older depth
  image, and its metadata range overlaps a writable storage buffer. Both
  parents are in use, but the captured state does not establish completion
  or whether old contents must survive. No in-flight object is discarded and
  no metadata clear is fabricated to bypass that refusal.

Additional observed startup states include the hair platformer scene, a
fighter introduction, and the zoo-management tutorial with readable text.
These are bounded observations, not full-route acceptance or comparisons
against retained same-scene references.

### A roguelike reaches gameplay (2026-10-07)

A 2D roguelike froze after its second frame: the main thread waited on an event flag that its render thread only
sets after the GPU copies a frame label, and the render thread polled that label forever. Three general fixes take
it through the title menu into its first areas (walking verified, about 140 fps under the harness limits):

- **Data packet payloads.** `GetDataPacketPayloadAddress` type 0 describes a NOP data packet whose payload starts
  right after the header. The guest sizes it exactly: a 3-dword NOP carries one 64-bit value that a DMA copies into
  the label the render thread polls; larger payloads are aligned by the guest itself. The payload was returned one
  dword later, so the 64-bit store overwrote the next packet's header and the copy never ran. Type 1 (register
  packets, values after the offset dword) is unchanged.
- **Ngs2 waveform parsing.** `ParseWaveformData` and `CalcWaveformBlock` are implemented for VAG (including the
  HEVAG flag layout, loop start/repeat/end bits in the low flag nibble) and RIFF WAVE with an ATRAC9 extensible
  format (`fact` gives the sample count and decoder delay, `smpl` the loop). The info is 232 bytes: a 24-byte
  format, u32 offsets and counts, `numBlocks` at +0x44 and four 40-byte blocks with 64-bit offset and size. A
  looped waveform splits into lead-in, a loop block repeating forever (the guest replaces that count with its own)
  and the tail. Header-only buffers parse (streams pass the first 0xa8 bytes). Playback of these formats is still
  silent; the sampler rack only renders the custom PCM path.
- **`image_sample_o`** shares the offset path of `image_sample_lz_o` with an implicit LOD; the offset is applied
  in base-level texels.

### UE4 JRPG startup frontier (2026-10-07, not gameplay)

An Unreal Engine 4 JRPG aborted on missing imports and shader forms one after another. Fixed so far, in the order
the runs hit them:

- **Videodec2.** The compute-queue and decoder entry points are implemented over the FFmpeg elementary decoder
  (AVC and HEVC, slice threads). Every structure carries its `thisSize` (compute memory 0x18, compute config 0x10,
  decoder config and memory 0x48, input 0x30, frame buffer 0x20, output 0x30); decoded pictures are written as
  linear NV12 with a 256-byte aligned pitch into the guest frame buffer. Movie playback itself is not verified yet.
- **`buffer_atomic_swap`** lowers to `OpAtomicExchange` (it was decoded as a barrier).
- **Image format 23** (R16G16 UNORM) samples, and color target format 14 with FLOAT channels is RGBA32F.
- **NGG image loads** read the address VGPRs of their dimension (plus the level of a mip load), not the decoder's
  padded operand width.
- **`image_gather4_lz_o`** takes its texel offset from the first address VGPR (two signed 6-bit fields), shifting
  the coordinates by offset / level size.
- **`v_readfirstlane` of a uniform VGPR** in a fused vertex front is a uniform copy, not a lane exchange.

The wave64 relative-index waterfall is now lowered only when its EXEC remainder snapshot, exact loop shape,
external-entry exclusion, and dead scalar/M0 values are proven. The scalar destination must not alias the remainder
mask; live scalar/M0 values across indirect PC transfers and unsupported destination modifiers are refused. Each relative
move uses its own lane's index; the remaining mask updates preserve the empty remainder, empty EXEC and clear SCC on loop exit. Strict native runs pass
this former admission refusal and reach the title menu and traveler selection, whose remaining visual defects are
recorded above. Seven synthetic proof controls cover admission and refusals. The overlap, indirect-transfer and
modifier cases fail before the added guards and pass after them; translation cache version 115 invalidates earlier
results. This advance does not establish gameplay.

The current live frontier reaches the initial settings screen but has not
established gameplay. A subsequent vertex program stopped in `ShaderParseEXP.cpp`
on full-precision POS1.X with EN=1. Its output state enables the first
clip-distance vector and clip distance zero, with the miscellaneous vector
disabled. The parser and translator now preserve physical X as a float in
`ClipDistance[0]` under active EXEC, only for that supported output contract.
The NGG proof retains the export without relaxing allocation, source definition,
mask or final-position ordering. Unsupported device clipping is refused before
pipeline creation; custom interpolation still refuses this output until its
geometry stage can preserve it.

The synthetic reproduction failed with the original parser rejection and now
passes full-module Vulkan validation. Fifty-four focused tests and the full
suite pass (1,876 passed, eight skipped). Native integration of this clipping
change remains unverified. A separate ES/GS-back cohort is rejected for its
stage and geometry limits. Its decoded body has real LDS exchanges, barriers,
allocation and geometry exports; it must not be admitted as passthrough without
that execution contract. It has not been causally joined to the interface
artifacts or to the parser failure.

A subsequent strict native run removes the POS1 parser stop and reaches the
native-wave admission in the same vertex program after about 73 seconds. The
fused-front proof refuses a VCC branch as non-uniform. The preceding comparison
reads a VGPR sum of two scalar-load results and another scalar-load result;
later vector arithmetic does not write that comparison's VCC. An opt-in,
bounded provenance trace confirms full logical EXEC and clean uniform sources
at the comparison. Its whole-dword SDWA tuple negates one source; the proof's
identity-tuple check rejects that modifier even though source negation preserves
lane uniformity. The body proof now accepts source NEG/ABS only after validating
the remaining whole-dword SDWA controls, and records a uniform comparison mask
only under full logical EXEC. Restoring EXEC after a partial comparison cannot
make that mask uniform. The proof also accounts for the implicit old-destination
read in multiply-accumulate instructions, checking both its definition and
uniformity before updating the destination.

A sanitized float reproduction accepts the unmodified tuple and fails with the
same VCC refusal when modifier recognition is disabled. Separate negative cases
exposed the partial-EXEC and implicit-accumulator proof errors before correction.
All 20 focused front-proof tests and the full suite pass (1,879 passed, eight
skipped); independent source review found no material issue. The temporary
trace is removed. A subsequent strict native run passes the modified-source
comparison and stops at a later VCC branch after ANDing two comparison masks.
Both reaching comparisons read clean scalar values under full logical EXEC;
`ScalarAlu` dropped it for the intersection of two uniform comparison masks.
The proof now retains a two-word AND result only when both complete inputs are
uniform masks. An AND involving EXEC also requires full logical EXEC; the
negative partial-EXEC fixture exposed an unsound retained copy case before
correction. The sanitized intersection reproduction fails with the original
VCC refusal and all 23 focused front-proof tests now pass. The full suite passes
1,882 tests with eight skips. A strict native run passes both comparison-mask
proof blockers and stops after about 73 seconds at an unsupported single-level
Standard256B texture array (RGBA8, one texel per layer, six layers). The binding
used a linear-aligned pitch, and the array layout and detiler did not admit this
swizzle mode. A second strict replay records the cube descriptor, zero array
pitch and last-slice field; the 64-texel pitch was derived by the host. The
published microtiled layout aligns each two-dimensional slice to its block
dimensions and selects layers by the resulting slice size. For RGBA8, one
8-by-8 block is 256 bytes, so the six single-texel faces occupy 1,536 bytes.
The binding now uses canonical tiled pitch for Standard256B arrays, and the
shared array layout and upload detile each single-level uncompressed slice.
Mip chains and compressed arrays on this mode remain unimplemented, rather
than being treated as linear. A sanitized six-layer fixture fails before the
change and verifies distinct face colors, upload regions and truncated-input
rejection afterward. Independent address-equation fixtures verify block
crossings and layer separation for one-, two-, four-, eight- and sixteen-byte
elements. Checked row-byte arithmetic also rejects unrepresentable staging
sizes before tiled sizing; the old arithmetic admitted a wrapped sixteen-byte
element case. All 47 detile tests and the full suite pass (1,885 passed, eight
skipped). The subsequent native run records the six-layer cube with canonical
pitch and passes the array-layout stop. It ends after about 72 seconds at a
full-channel POS0 export with DONE=0 and VM=0, which the parser previously
requires to carry DONE. A bounded read-only debugger capture records the next
instruction as POS1.X with DONE, followed by parameter exports and the real
program terminator. Distinct nonfinal POS0 and final POS1.X formats now preserve
that sequence for exact full-precision vertex encodings. The whole-program
proof requires one full POS0, at most one typed POS1 and DONE on the last
position export; full parameter exports can follow. The load-bearing body
proof carries a set of combined position histories through forward branch
joins and requires POS0 plus final DONE on every terminating path. It accepts
position events only under the proved full logical EXEC root; divergent
masked position alternatives remain outside that proof. Sanitized fixtures
reproduced the old acceptance of positions after DONE and a branch that
reached the terminator without POS0. POS0 lowering now guards its output store
and the existing clip probe with resolved per-invocation EXEC. Typed POS1.X
still requires the exact first clip-plane output metadata. Raw target,
control and source tuples must agree; mutated internal-IR fixtures reproduced
the former validation gaps. All 73 focused export and front-proof tests pass;
the full suite passes 1,899 tests with eight skips. The architecture boundary
check and the provenance check of all 13 generated graphics tables also pass.
The first native replay of this position change stops after about 32 seconds,
earlier than the preceding frontier. The new full-root position check refuses
a previously admitted vertex program at its final POS0 export because its
symbolic EXEC is partial or unknown. The run has no resource-guard stop and
produces no capture. This is an integration regression of the new proof;
the captured program saves the complete vertex mask from VCC into an SGPR
pair before a conditional scalar load overwrites VCC. It then restores EXEC
from that intact pair before its only position export. The proof discarded
this saved-node provenance on scalar-to-scalar copies. Two-word moves now
snapshot the exact source node before invalidating destination metadata and
transfer that node to the destination pair. The full-root position gate is
unchanged. A sanitized mask-copy chain fails before the correction; copied
partial masks and pairs with an overwritten half still fail at position
admission. Independent source reviews find no material issue. All 75 focused
tests pass, and the full suite passes 1,901 tests with the same eight skips.
The architecture boundary and all 13 graphics-table provenance checks pass.
The next native replay passes that full-root refusal and the former ordered
position-export parser stop. It ends after about 79 seconds at a sampled-image
descriptor-resolution failure in a later vertex program, with no resource
guard stop. That program loads a texture-sized scalar block from its user-data
pointer and samples it later; the current resolver cannot identify a bound
sampled descriptor at that scalar destination. A bounded, read-only capture
at the resolver entry confirms that the final binding already contains both
EUD texture and sampler table entries, but has no dynamic scalar-load mapping
to their shader destinations. The EUD pointer uses API index 24 and shader
index 32 with the fused-front base of eight; the declared table has 76 dwords,
matching the largest constant load span in that program. The fused-front
parser omitted EUD instruction discovery together with the deliberately
disabled generic scalar-buffer fallback. EUD-only instruction discovery now
uses a separate code input while generic metadata analysis remains disabled
for the reused projection descriptor. Full and split scalar loads use the same
base-aware pointer match, requiring both words to retain their entry values on
every path to the load. Snapshot sizing uses that same provenance and the
existing 256-dword cap. Local dynamic storage mappings use the separate code
for their scalar read span without dereferencing the disabled generic code.
A sanitized fixture first reproduces the missing mapping and short snapshot;
another failing run proves that overwriting either pointer word could still
materialize the wrong entry table before the lifetime guard. The corrected
cases pass alongside the base-zero control, null dynamic storage and retained
metadata-only projection cases. Independent source and downstream reviews find
no material issue. All 1,902 tests pass with the same eight skips; boundary and
13 graphics-table checks pass. The next native replay passes the descriptor
failure and holds the initial-settings screen through the 300-second bound,
without a fatal error or resource-guard stop. Its final sampled status reports
about 6.9 frames per second. Repeated face-button input does not advance
that screen. An isolated menu-confirm press with the same binary advances
through the following logos; that replay stops after about 164 seconds at
vertex-input layout validation with two resources and two buffers. The exact
rejected descriptors are recovered by a later failure-entry capture: one
12-byte RGB32 float stream is valid, while a separate four-byte-strided stream
uses the unified unsigned eight-bit scalar format missing from the vertex
format table. A sanitized two-binding fixture first reproduces the undefined
format and layout refusal. The shared table now resolves that format as
`R8_UINT`, preserving integer input declarations and raw-bit fetches through
the existing emitter. The translator version invalidates cached float input
declarations for that resource. The combined storage, vertex,
emitter-precondition and translation-cache checks pass all 107 focused tests;
the complete suite passes 1,911 tests with eight existing skips. A native
replay of these corrections remains pending. This establishes
the resource-frontier advance, not gameplay or correct clipping output. The
next diagnostic repeat stops earlier in GPU-memory overlap classification: a
new storage image is contained by a render image and contains 502 read-only
storage buffers. The render image reports a GPU update newer than its CPU
update and unknown content origin; the smaller buffers report CPU uploads.
Those timestamps do not prove a coherent owner for the entire new image.
Preserve the rejection until the live ownership and range contract is known;
do not seed the image from guest bytes merely to accept the mixed relation.
Further repeats reach the same mixed-parent refusal. A bounded breakpoint at
the standalone compute seed-proof helper produces no dispatch snapshot: the
optimized dispatch caller inlines that helper. The absent snapshot does not
exclude a compute caller or prove full-image overwrite. Capture the actual
creation caller before fatal shutdown before extending an overlap or coverage
contract; the last logged compute program alone does not identify that caller.
The two distinct first failures remain separate investigations. The
latest failure-entry capture recovers the compute checksum directly and joins
it to the rejected storage image. The shader forms a two-dimensional grid,
intersects both unsigned bounds predicates with EXEC, adds the parameter
origin, then stores every component present in the image format. Its parameter
buffer places origin and bounds after an earlier coefficient pair. The current
coverage recognizer cannot prove this static metadata-buffer form, and the
actual creation parameter still requires seeding. Both input-buffer ranges
are disjoint from the output range. The host prefix shows zero origin and
matching bounds, but does not establish a coherent GPU snapshot. A general
offset-grid proof and a bounded runtime origin/bounds snapshot now recognize
that static metadata form. The proof derives the byte offsets from the scalar
load, requires its complete LGKM-zero wait before the bounds comparisons, and
rejects descriptor, coordinate or EXEC clobbers. Insufficient-wait fixtures
first reproduce false coverage and now reject it while the positive fixtures
retain coverage. Native acceptance remains pending. The new origin-based path
also checks the exact immutable storage-buffer snapshot uploaded for the
dispatch before texture creation. A separate guest-memory read cannot prove
that later upload uses the same parameter bytes. Missing snapshot ownership
or changed origin/bounds clears the provisional seed-skip bit.
The next strict replay stops after about 183 seconds without a resource-guard
stop at EUD snapshot preflight. Its metadata declares 268 dwords, exceeding
the fixed 256-dword host snapshot policy; no table bytes are copied before
the refusal. This is a later recorded first failure, not gameplay acceptance
or a complete native proof of either preceding correction. The metadata
field supports larger declarations, but the live caller, descriptor spans
and readable table range must be recovered before changing that policy.
A one-shot failure-entry capture identifies a fragment caller with 30 user
scalars and an EUD offset base of 32. Its three storage-buffer metadata sharps
occupy EUD dwords 256--267, matching the declared 268-dword extent. The host
mapping covers that complete range, but this capture reads metadata only and
does not prove guest-table readability, stability or instruction consumers.
The next contract test must preserve the full declared extent, validate the
actual descriptor spans, and retain the stable double-read before binding.
The local correction sizes both snapshot reads exactly to the checked extent,
bounded by the metadata field's 16-bit representation. Image-sharp expansion
resizes and recaptures the entire prefix before exposing the final snapshot.
A sanitized table ending at a protected page verifies the high descriptor and
forces a retry by changing its final word between the two reads. Its actual
scalar-load consumer is discovered through the existing dynamic binding path;
unused static metadata is still pruned. Pointer tables behind SRT retain their
separate 256-dword admission limit. The representational bound permits larger
candidate scalar-load spans, including unreachable instructions whose scalar
flow retains entry values. It does not prove all such guest ranges readable;
unreadable or unstable ranges continue to fail strictly. Candidates beyond the
former limit already failed preflight, so no regression of a previously
successful parse is established by that expanded admission alone.
The earlier metadata-only capture could not recover optimized locals and does not
establish a table snapshot. The initial-settings capture still has fragmented
interface elements; no causal connection to the separately rejected geometry
cohort is established. No gameplay, clipping output or complete subgroup-width
equivalence is established yet.

An adjacent pre-existing risk remains in offset-zero bounded-grid seed
eligibility: its pre-bind guest snapshot is not coupled to the later uploaded
storage buffer. A guest update between those reads can make eligibility refer
to different bounds from those consumed by the shader. No native trigger is
established here. The next direction is the same exact-bound-snapshot check
for that coverage form; the current origin-based change preserves its behavior.

Extended-data emission now stores only actual descriptor fields in a sparse
map. A high-index static binding first reproduced the former fixed-map bounds
failure. Checked lookup rejects negative, missing and out-of-range fields;
the scalar-load emitter resolves every field before emitting any loads. The
existing dynamic-PC mapping remains first, with static lookup restricted to
populated rows. Overlapping rows retain the established binding order. This
closes the fixed-map capacity and fabricated row-zero risks locally; native
acceptance of the complete correction remains pending. The next strict native
replay passes the former snapshot-span refusal and stops after about 188 seconds
at fragment admission. The mask-flow proof reports a memory-write refusal;
its shared prefix classifier also includes some reads, so the reported text
does not establish the rejected instruction's side effects. The draw uses one blended color
target and no depth attachment. No resource guard or debugger stopped the run;
the retained captures do not establish gameplay. Recover the rejected program
and its EXEC/helper-mask flow before changing that admission contract.
A same-binary repeat instead stops after about 178 seconds at mixed storage
image parents before reaching fragment admission. Its image size and relation
shape match the earlier creation refusal, but no current caller, shader or
descriptor identity is captured. These distinct first failures require a
same-event caller/descriptor join; the earlier compute identity cannot be
assigned to this repeat from the last shader log.
A failure-entry capture on the next same-binary run joins the actual compute
dispatch to its shader and the current, bound and creation image descriptors.
The originating static coverage is empty, the final seed mask is zero and the
creation record confirms that seeding was not skipped. The decoded store uses
the parser's sequential coordinate form, with no explicit address tuple. The
origin-grid recognizer currently requires an explicit X/Y tuple, unlike the
existing bounded-grid recognizer. This is a concrete representation mismatch
to reproduce with a parsed fixture. The encoded image dimension and several
earlier scalar-read predicates were not captured, so it does not establish
that this is the first rejection or that correcting it alone closes the native
failure. The run stops after about 180 seconds; no gameplay is established.
A two-dimensional, no-NSA store fixture in the parser's admitted flag subset
reproduces the empty coverage
despite matching the proven origin registers. Origin and bounded-grid proofs
now share the coordinate matcher: explicit X/Y operands must match exactly;
sequential coordinates require a plain VGPR span of at least two words and
adjacent proven X/Y registers. Dimension, full-store and resource-identity
checks remain intact. Short spans, mismatched coordinates, another dimension
and modified operands retain rejection. The focused suite passes locally;
native acceptance of this correction remains pending. The fixture verifies
coordinate representation, not complete instruction-flag conformance; the
pre-existing UNRM admission debt remains unchanged.
The next strict replay passes the recorded creation failure and reaches
fragment admission after about 179 seconds. Its failure-site program dump
identifies the rejected instruction as an add-thread-id LDS write. The broad
memory classifier's inclusion of reads therefore does not explain this
particular refusal. The program uses three such stores and corresponding
add-thread-id reads while M0 is zero, then restores M0 and the original EXEC
before exporting. Investigate a proof of per-lane slot ownership and mask
lifetime before admitting these operations through the existing private-spill
lowerer; no general LDS-write relaxation is justified. The retained image is
the title logo, not gameplay, and no resource guard stopped the replay.
A same-binary failure-entry capture confirms two extra pixel-LDS blocks and
30 user scalar words. On GFX10 those blocks provide 1 KiB; the three complete
wave64 planes occupy 768 bytes. The packed launch parameter follows the user
scalars, but its numerical offset was not captured. A recorded word outside
the user count must not be mistaken for that implicit parameter.
The bounded admission change requires disjoint wave64 planes inside the
declared extra allocation and the existing private array, a dominating constant
aligned M0, preceding stores for every read, and unchanged EXEC/M0 through one
straight-line interval. Other LDS access forms remain excluded. Interpolation
after a spill must recover the original parameter M0 through exact scalar
moves; numeric or unknown interpolation bases fail closed. Reserving the extra
storage before the parameter region is supported by older and newer published
hardware contracts and target-generation compiler behavior, but remains an
inference for this target rather than a captured numerical boundary. Mask
monotonicity cannot bypass this spill proof. The analysis and module identities
include the launch allocation and parameter scalar index. Focused validation
includes 88 passing fragment, LDS, native-wave and cache tests,
including explicitly validated pixel SPIR-V. The complete suite passed with
1,922 tests and eight fixture-dependent skips before the final additional
boundary cases, which passed in the focused rerun. Both affected targets and
the graphics provenance/boundary gates pass. Independent source and native
program audits found no structural rejection in the captured pattern. The
next strict native replay passes the recorded fragment admission failure and
reaches character selection before stopping after about 290 seconds, without
a resource guard or debugger stop. It rejects a single-component unsigned
16-bit color target at `GraphicsRenderAttachments.cpp:886`; the shared
`RenderTexture.cpp` format resolver has no mapping for that CB format/type
tuple. A focused contract test reproduces the missing mapping. The title
menu still has a magenta background; no causal connection to this new refusal
or gameplay acceptance is established. Join the active attachment and shader
export interpretation before exposing this integer target to the pipeline.
A bounded draw-boundary diagnostic confirms an active write at the first
attachment, unsigned number type, standard component order and the packed
unsigned-16 export mode. The format mapping now preserves two bytes per
element and selects the host unsigned-16 attachment; seven focused format
tests pass. The pixel interface now carries each attachment number type into
the module identity, declares unsigned outputs for unsigned attachments, and
extracts packed unsigned-16 components without floating-point conversion.
Compressed masks outside the documented paired-source set remain refused.
An inactive intermediate export skips its stores and can continue after an
EXEC restore; a final valid-mask export retains discard semantics. The embedded
floating-point template is refused against a materialized unsigned attachment
at location zero, before pipeline lookup. All 63 focused tests and the full
suite pass: 1,931 tests and eight fixture-dependent skips. Both targets build,
the source/table gates pass, and independent review found no blocker.

This is not native acceptance. The checked diagnostic parse first revealed a
missing Gen5 unsigned-pack opcode. A synthetic bounded decode reproduces it;
the decoder now retains its two unsigned inputs and strictly rejects unsupported
modifiers. The existing lowering also referenced an undefined mask constant
and selected an integer as a floating-point result. A validated in-range
fixture reproduces that defect; constant resolution and a bit-preserving cast
repair the module. The numerical overflow behavior is still unverified:
the published Gen5 pseudocode names a conversion helper without defining its
out-of-range rule, compiler selection alone supplies no numerical oracle, and
inspected execution fixtures stay in range. Earlier architecture rules cannot
resolve that question. The bounded native diagnostic now records the complete
nineteen-instruction pixel program. Immediately before the unsigned pack, an
unmodified bit-field insert chooses bits from two inline integer arms; their
union bounds the result to three low bits. Both packed sources are the same
VGPR. The program has no branch or EXEC mutation. This proves in-range inputs
for that program without choosing an overflow rule.

Gen5 unsigned packing now admits unmodified in-range immediates or an adjacent,
single-word bit-field-insert producer with in-range inline arms. The proof
rejects entries that bypass the producer, dynamic PC transfers, nonadjacent
instructions and unproved banked VGPR layouts. An unbounded-source fixture
first reproduced acceptance where strict refusal was required. Numerical
fixtures check the generated packing on admitted inputs. Translator version
112 invalidates modules compiled before this admission rule. The temporary
draw-boundary stop and raw-opcode logging have been removed. Both targets
build; all 66 focused tests and
the full suite pass, with 1,934 passed tests and eight fixture-dependent skips.
Source boundaries and all thirteen graphics tables pass their checks. The
diagnostic run's final capture is black and supplies no gameplay acceptance or
general overflow contract. The production run reaches a visible traveler
information panel, then fails after 489 seconds without a resource guard or
debugger stop. Its first refusal is a writable image declaration at
`ShaderSpirvImage.cpp:895`: the shared format table admits the single-component
16-bit normalized format for sampling, but leaves its storage usage undefined.
This declaration failure occurs before instruction lowering, so the new
unsigned-pack path still has no direct native execution proof. The selector
retains a black center; no gameplay acceptance is established.

The published target-generation ISA maps that raw descriptor format to
single-component 16-bit UNORM and specifies conversion on ordinary image
stores. The shared table now selects `VK_FORMAT_R16_UNORM` for storage as well
as sampling. It retains the existing floating-point, formatless `Unknown`
storage declaration; integer output and atomic admission rules are unchanged.
A single writable-descriptor fixture first failed storage planning, then
passed production SPIR-V generation and validation after the mapping change.
The existing format-contract test also checks the exact host format, numeric
domain and two-byte element size. Translator version 113 invalidates prior
modules. On the hash-verified Intel discrete-GPU driver, a metadata-only Vulkan
query reports storage and formatless read/write support and accepts the actual
combined image usage with both ordinary and mutable creation flags. That
query does not allocate an image or create its views. Independent review
confirms the existing mutable format list remains compatible and unchanged.
Both targets build; 88 focused tests and the full suite pass, with 1,935 passed
tests and eight fixture-dependent skips. The final additional table assertions
also pass in the focused rerun. Source boundaries and all thirteen graphics
tables pass. The next strict native replay does not repeat the storage
declaration refusal. Its exact pixel-program probe contains the captured
bit-field insert, unsigned pack and compressed export, with generated and
optimized SPIR-V declaring unsigned output and preserving the packed bits.
A matching draw trace resolves the active attachment to the host unsigned-16
format after pipeline return and descriptor binding. The trace precedes the
draw command, so it does not prove submission, execution or presentation of
that draw. Actual normalized-image/view creation is not separately traced.
The replay stops after 521 seconds on MIMG opcode `0x68` at
`ShaderParseMIMG.cpp:524`. That switch labels the opcode as a derivative sample; its
target-generation semantics and raw operands need independent evidence before
an implementation decision. Both scheduled input batches were delivered,
with matching guest state-read samples and no cancellations. The final
post-input capture is completely black. There is no resource-guard or debugger
stop; the last measured memory-event counters contain no high/max/OOM event.
Gameplay and performance acceptance remain open.

The following bounded native replay reaches the same coarse derivative sample
after about 460 seconds, again without a resource-guard or debugger stop. A
bounded raw snapshot and decoded prefix join its six logical addresses to four
supplied slopes and two coordinates. A canonical WQM interval contains complete
floating-point multiply-add writes, fixed quad broadcasts and subtraction
producers for all four slopes, before the first EXEC narrowing. All encoded
DPP controls outside the modeled fields are zero in that captured interval.
The final capture remains black; this is instruction evidence, not gameplay.

Gen5 two-dimensional coarse derivative sampling now has a separate decoded
instruction and emitter. Explicit NSA addresses are checked individually;
the no-NSA form retains its six-register span. The supplied operand order is
the two horizontal slopes, the two vertical slopes, then the two coordinates.
The emitter retains compact DMASK channel order and the existing EXEC guards
for inactive destination lanes. Other dimensions, integer image domains and
unmodeled sample variants remain refused.

Published older hardware contracts describe coarse sampling as choosing LOD
per quad, and target-generation compiler definitions retain the distinct
coarse opcode with this derivative schema. Those sources do not establish a
general target-generation lane-selection rule or bit-exact cross-GPU LOD
arithmetic. Lowering to supplied SPIR-V `Grad` is therefore a bounded
operational inference: it is admitted only when each supplied slope is proved
equal across its quad, making a per-quad slope selection immaterial. Arbitrary
coarse gradients are not admitted.

The proof requires an initial canonical WQM interval and complete writes before
using a fixed quad broadcast. It distinguishes complete vector writes from
destination-shaped read operands, partial DPP or SDWA writes and redirected
memory results. Only the demonstrated plain move, subtraction and full-dword
multiply-add forms establish complete writes; an accumulating multiply-add
also needs a previously defined accumulator. Unsupported buffer status/LDS
controls and dynamic register writes fail closed. Later writes clear gradient
facts, control-flow joins intersect them, and an edge that re-enters the
initial proof interval is refused. Unmodeled DPP bits are retained by decoding
and rejected by this proof. Translator version 114 invalidates prior modules.
The temporary raw-byte diagnostic has been removed. Both affected targets
build; all 93 focused tests pass, including production SPIR-V validation,
operand ordering, inactive-lane preservation and the proof's rejection cases.
The full suite passes with 1,948 tests and eight fixture-dependent skips.
Source boundaries and all thirteen graphics tables pass. Independent review
found no remaining blocker. Native execution of this candidate and
gameplay/performance acceptance remain unverified.

The next strict native replay reaches the title and traveler-selection menus,
then stops after about 206 seconds on an unsupported sampled-array allocation
at `GraphicsRenderBind.cpp:3315`. The recorded input is a six-layer, ten-level
512-square texture in Standard64KB tiling. The array planner currently excludes
multi-mip layouts outside Standard4KB. This is an explicit refusal, not a
resource-guard stop; it does not establish a descriptor-shape regression or
execution of the new coarse-sample lowering. The scheduled input batch had not
reached its target when the endpoint ended. The visible title background is
magenta, and the selector has incomplete text. Comparing those states with
earlier builds and identifying their actual output-producing draws is the
current visual investigation; neither defect has a confirmed connection to
this array refusal.

**Audit-closure update at translator126 (2026-10-09).** The following statuses
supersede specific observations recorded on 2026-10-07:

- The tested nonterminal pixel color exports now skip inactive stores and
  rejoin later shader code. Terminal exports retain their discard behavior.
  This does not establish parameter-export enable-mask semantics.
- The packed-mip image-store path and GDS append/consume operations now gate
  their side effects by EXEC. Focused packed-mip coverage and the selected
  native GDS integration pass; unrelated or unmodeled forms remain excluded.
- The whole-pair VCC propagation risk is fixed: one-word writes do not promote
  a varying two-word mask to uniform. Focused positive and negative proof cases
  pass.
- The native `ParameterExecWriteback` fixture passes its full-EXEC and
  zero-EXEC variants. This establishes only EXEC behavior for its full-enable
  mask; partial full-precision PARAM masks, accumulation, compressed PARAM and
  VSKIP remain unqualified or refused. Pixel color-export masks are a separate
  contract.
- The current working tree bounds the optional vertex-layout logger's walks
  over attribute, buffer and resource arrays to their 16-entry capacities.
  That source change is not in HEAD at this checkpoint and has no separate
  logger-specific runtime validation.
- The `ShaderDumpGuestProgram` extent and mapping-lifetime finding remains
  open. Its raw-byte extent still needs a mapped-range clamp and one lifetime
  owner.

The following observations retain their original 2026-10-07 date. Integer
color output evidence is now limited to the translator126 five-case UINT16
native matrix described above; other integer formats remain unqualified. The
inactive nonterminal pixel-export finding is superseded for the tested forms,
with no established connection to the deferred menu defect.

The `ShaderDumpGuestProgram` diagnostic finding was recorded at
`Shader.cpp:782`: its raw-byte extent used the final instruction PC plus eight
without clamping to the registered shader range, and parsing and raw dumping
did not share a lifetime lock. A four-byte final instruction could include
adjacent readable bytes, while a mapping replacement could pair a listing
with different bytes. The extent and lifetime snapshot remain open; use a
bounded decoded listing until they are repaired.

The 2026-10-07 side-effect observation named a packed-mip image store and GDS
append/consume operations that lacked EXEC guards. Those findings are
superseded by the EXEC-gated implementations and test evidence above. Other
shader memory operations still require their own proof and admission.

The 2026-10-07 VCC observation described whole-pair uniform promotion after a
one-word write. That behavior is superseded by the complete-pair rule above;
unsupported mask forms remain subject to strict refusal.

The 2026-10-07 parameter-export observation described unguarded writes and an
optional UV probe. The translator126 fixture establishes EXEC behavior only
for its full-enable-mask case. Parameter component-enable behavior remains an
open contract question, and no claim is made about partial-mask accumulation.

The 2026-10-07 vertex-layout logger observation described raw-count walks past
fixed arrays after validation refused the layout. The current working tree
caps those walks at 16 attributes, buffers and resources while retaining the
original counts in the report. This change is not in HEAD at the translator126
checkpoint and still needs independent logger-specific validation.

### Implicit sampling preserves volume coordinates (2026-10-07)

A mixed set of flat and volume sampled images could route an implicit sample
through the flat image bank, dropping its third coordinate. Dedicated component
formats now select the typed path from the bound descriptor shape; the generic
component-mask form also accepts three-dimensional addresses. Enabled channels
remain packed in destination order, including when the dedicated format carries
the mask and the instruction's raw mask field is zero. The shader translator
version is incremented to invalidate earlier cached translations.

Synthetic mixed-bank, volume-only, array, and cube contracts pass. At that
validation checkpoint, the full suite passed 1,874 tests with eight skips.
A strict native run on the
Intel host exercised the repaired three-coordinate sample for about 291 seconds;
the captured room has straight wall unions without the earlier dark speckling.
The earlier capture uses a different camera view, so this is not a pixel-exact
scene comparison.

### Two Unity titles reach their menus and gameplay (2026-10-07)

A 2D action title stopped at boot and a first-person perspective-puzzle title on its loading screen. Each fix
below is general and was found on the first blocker of a real run; nothing changes for shaders, objects or
imports that worked before (those paths aborted).

- **HLE.** `sceNetShutdown` is exported by libSceNet over the existing socket shutdown.
  `sceKernelGetOperationMode(mode, submode)` reports a base console (0, 0); the observed caller zeroes both and
  only acts on submode 1. The Unity `Share.prx` service plugin validates the same 16-byte `module_start`
  descriptor as the other native service plugins (size 0x10, version 0x200, callback).
- **Command processor.** An indirect `COMPUTE_TMPRING_SIZE` is scratch-ring metadata, like `SPI_TMPRING_SIZE`.
- **Primitive state.** `sceAgcCreatePrimState` outputs can lie in GPU-visible pages the dirty tracker keeps
  read-only; the host protection table then reports them unwritable although the guest kernel maps them
  CPU-writable (seen with protection 0xf2). Such a tracked range is validated against the guest mapping and
  written through the tracker's host-write ownership.
- **Shaders.** `TBUFFER_LOAD_FORMAT_XYZ` (Gen5 RGB32F and RGBA32F elements); `IMAGE_SAMPLE` with any
  enabled-component set through the dmask-driven format; formatted image stores apply a four-channel
  `DST_SEL` selection the storage view cannot express (BGRA stays a BGRA8 view); native `S_CBRANCH_EXECNZ`
  decides on the packed EXEC pair, as EXECZ does; forward branches inside a do-while loop to the instruction
  after its conditional back edge are breaks into the loop merge. `IMAGE_SAMPLE_L` takes any enabled-component
  set on 2D, 3D, cube and 2D-array addresses; the T# shape selects the descriptor bank.
- **Comparison samples.** `IMAGE_SAMPLE_C_LZ` on an arrayed (cube-face) descriptor compares the gathered
  texels in the shader; an anisotropic filter there reduces to its point or linear base filter, since a
  level-zero sample has no derivatives. A comparison sample of a color surface compares in the shader on every
  descriptor path, including descriptors the shader loads from memory.
- **Wave64 pixel shaders on a 32-wide host.** A compiler spill VGPR may receive a `V_WRITELANE` whose slot is
  never read back while its other lanes are; that write is a dead spill store, not a lane exchange, so the
  shader keeps the lane-local proof and runs on the host's 32-wide subgroups.
- **Bindings.** A draw that samples its own depth attachment while writing stencil uses
  `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL` (depth store NONE, stencil STORE) and its sampled descriptor
  declares the same layout. A depth-comparison sample of a color surface (no depth tile or format) samples the
  texture and compares in the shader, with a non-comparison sampler.
- **Memory objects.** A storage view of one mip level of a mip-chained surface is linked to the chain's sampled
  texture and level-0 render target; the texture's per-level copy takes each level from the surface of that
  extent. A 3D texture in the 64 KiB render-target swizzle is thin: each depth slice is a 2D render-target
  slice, laid out like a 2D-array layer (the addressing library keeps thick 3D blocks for the Z and standard
  swizzles only; titles carry its 64 KiB 3D block table). A storage view of a GPU-written render target's
  surface (a compute pass on the rendered image, viewed as UNORM while the target is sRGB) seeds from the
  target's pixels; 8-bit four-channel formats alias byte for byte, and edge blocks copy only the texels both
  images cover. Sampled views of that surface stay linked, and a texture that skipped its guest upload under a
  live surface no longer counts as newer guest data.

Both titles now reach gameplay: the 2D title in its first area, the puzzle title in its first room after the
intro (about 35 fps under the harness limits). Open: the 2D title's menus draw text from an 8-bit atlas as
translucent boxes (atlas, swizzle, vertex fetch and clamps are verified; the text shader's camera-space
branch is next).

Follow-up the same day, the puzzle title's deferred passes:

- **Render targets written with the alternate swap alias their samples.** A G-buffer target written with the
  alternate component swap is a BGRA8 (or A2R10G10B10) image whose bytes are the guest's; its sampled view is an
  RGBA8 sRGB (or 10_10_10_2) T# with a BGRA selection. The alias was rejected, so each sample built a separate
  texture that skips its guest upload and was cleared to transparent black: ambient occlusion and lighting read
  stale or empty normals and albedo (speckled, unblurred darkening along creases). Such a surface is now a
  sample alias; its view exchanges the red and blue selectors (an RGBA8 sample with a BGRA selection reads the
  BGRA image with identity). The existing RGBA8 UNORM case now composes the selectors the same way.
- **`V_LOG_F32` of zero is negative infinity**, as the ISA defines; GLSL `Log2` leaves it undefined. `pow` lowers
  to log, multiply and exp, so a zero base (a vignette outside its ellipse) needs it.
- **Recycled allocations.** A storage view over memory another storage view or render target used with a
  different format (or over the exact range with another extent) is linked and seeds from guest bytes; a
  storage view over several GPU-written targets of its element size seeds from their blocks in write order.
- The image dumps keep the last eight remembered targets and run for every present format.

### Integration tests link again (2026-10-06)

Every integration target failed to link: the kernel event queue called the network HLE directly, and archives
link the kernel after the HLE. The calls now go through `SocketEventPort`, which the composition root installs,
and `scripts/check_emulator_boundaries.py` rejects the network header in kernel sources.

All 66 registered integration tests pass. The 8 that failed once they could run again found one emulator defect;
the rest had drifted from contracts the emulator had since changed on guest evidence:

- **Emulator fix: 64-bit status sources.** `S_CMOV_B64` (and every scalar 64-bit operation) can read SCC, EXECZ or
  VCCZ as a 64-bit source whose high dword is zero; the operand loader already emitted that, but the lowering
  preconditions admitted these operands only as one dword, so such a shader aborted with `reason=lowering-preconditions`.
  M0 keeps the one-dword rule.
- Shader probes (`ScalarContracts`): seeds are whole SPIR-V lines placed after the native initial-EXEC setup, which
  derives EXEC from the live lanes, and the direct SGPR count is the probe's own fixed 32.
- Shader fixtures (`SerializationAndBounds`, `VertexClipProbe`): synthetic buffer instructions set their cache
  flags (`buffer_flags` defaults to unknown, which fails closed); compute inputs give a nonzero workgroup size; MIMG
  sample fixtures use the dmask-driven formats the decoder produces. Assertions follow the current lowering: CMPX
  writes the packed ballot to both EXEC words in wave64, `V_BFM` computes `((1 << count) - 1) << offset` without
  `OpBitFieldInsert` (undefined for offset + count > 32), and a wave-uniform `V_READFIRSTLANE` is a plain copy (the
  subgroup invocation builtin now comes from the initial EXEC setup).
- Index-to-render-target alias (`RenderTargetIndexAlias`): a render target allocated over two cached index views,
  one inside it and one crossing its end, aborted object creation in a guest; index views are read-only fetch
  caches, so crossing, contained and containing views link. Equality stays rejected.
- Wide classification (`LibcWide*`): the guest's format-specification scanner copies characters while
  `_Iswctype(c, 2)` is clear and stops at the conversion letter, and titles also call class 4, so the Dinkumware
  table holds (2 alpha, 4 digit). The C locale classifies nothing above 0x7f instead of rejecting it, `%s` in
  `vswprintf` is a narrow string, and a result that does not fit returns -1. Only an unknown class is rejected.
- Archive membership (`ArchiveDag`): the kernel archive owns `FiberContext.S` and the HLE archive owns
  `AudioPropagation.cpp`.

### Storage uploads and physical residency (2026-10-06, guest verified)

Scope: strict runs on the reference host (two host cores, 8 GiB cgroup, shader validation on). Costs were measured
with a temporary counter build (residency phases, tracker write faults, changed pages per upload, write-back waits
by cause) next to the native `perf-snapshot`.

- **Storage buffers upload only the pages the CPU wrote.** A storage buffer the guest rewrites in part was uploaded
  whole whenever its range generation moved: the isometric roguelike re-uploaded a 7 MB buffer about 3,000 times in
  180 s, and in one 10 s window only 216 of the 239,260 uploaded pages had changed. A storage buffer of two or more
  pages now keeps the tracker generation of each page at its last upload; an in-place update copies only the pages
  whose generation moved and refreshes the write-back snapshot for those pages. A page's generation moves on every
  write after it was armed (a fault or a host notification, including the write-back of GPU results), so an unmoved
  page still holds the uploaded bytes. A completed label publication, depth-metadata (HTILE) storage, a new backing,
  or a hash change without any moved page keeps the whole upload. Over 120 s the title uploaded 7.0 GB instead of
  19.8 GB (upload time 8.4 s to 6.5 s); its menu, intro and gameplay captures match.
- **Physical residency is asked in one sweep of the backing.** After the direct-memory population changes, each
  preparation asked whether every unimported span of every physical range was populated, one `lseek(SEEK_DATA)` per
  span (about 13,000 spans in the .NET beat 'em up). The spans are now sorted by backing offset and swept: one query
  clears every span before the next populated page. Ranges count their imported pages, so a range with none is one
  span and a fully imported range is skipped without walking its bitmap. Residency time over a 200 s run went from
  49.1 s to 45.9 s (10 s windows 15-35% lower). The roguelike gains nothing: its ~4,300 gaps alternate with imported,
  populated runs, so each gap still needs its own query.

- **Vertex buffers reuse a covering object.** A draw requests the vertex bytes it reads (records times stride), so
  draws over one mesh request different prefixes of the same allocation. Any overlap between vertex buffers reclaimed
  the older object, so the Unity fishing title alternated a 2.4 MB and a 3.2 MB request on one base and reclaimed and
  re-created the buffer about 116,000 times in 180 s, uploading it whole each time (6-7 ms, 315 GB in the run). A
  live vertex buffer that starts at the requested address and covers the request is now reused, as index buffers
  already were (bound at offset 0; the draw reads only its own records). Reclaims fell to 1,411 and covered reuses
  rose to 238,689; vertex binding went from 68.8 s to 9.6 s and the title from 2 to 4 fps in its first dialogue.
  Its next cost is hashing: about 17,000 live storage buffers exceed the dirty tracker's 512 ranges, so most fall
  back to full hashes at every use (398 GB hashed, 172 GB still uploaded). A/B against the previous build on the
  same routes (runs d1244-d1251): the pixel-art Metroidvania's 20 s windows match (139-75 fps before, 152-75 after,
  14,534 presents against 14,139), the action platformer 65 and 65 fps, the beat 'em up 7 against 6, and two runs of
  the roguelike at 15 and 13 fps without its 8 s crash. One earlier beat 'em up run lost the device at 166 s with
  the driver reporting `execbuf` ENOMEM (memory pressure under the 7 GiB cgroup limit); its A/B rerun did not.

Measured and not shipped:

- `cachestat(2)` per physical range, to skip a range whose populated bytes equal its imported bytes: the call walks
  the cached pages of the interval. On a 3 GiB memfd with 87,000 populated pages it costs 2.0 ms, the same as the
  5,462 per-gap queries it would replace.
- Device-side publication of every pending storage result whose guest bytes the CPU had not written (a copy on the
  consumer's queue instead of the CPU write-back). In the sandbox title's menus the write-back waits fell from about
  2,100 to 500 per 10 s, but the waiting time stayed at about 5.8 s per 10 s (one wait per frame for the whole
  frame) and the frame rate did not change (49-52 fps). The remaining blockers are storage buffers aliased by
  vertex buffers (equal 32-64 B objects, and 2 MB objects containing vertex buffers) and by other storage buffers;
  those aliases need the in-order write-back. In the beat 'em up no pending object qualified.

- Persistent mapping of storage buffers with the write-back baseline deferred to the first writable use (from
  another integration line): the beat 'em up presented 1,240 frames in 181 s, and 867 without the deferral, against
  1,580-1,682 with per-use mapping on the same route. Index buffers placed in host-coherent memory mapped pooled
  blocks a second time, and an sRGB swapchain re-encodes UNORM sources the presentation scaler copies directly.

Open, measured:

- The sandbox title waits for device-address write-backs about 56% of its menu time (2,100 waits per 10 s, 2.6 ms
  each) and about 60% in its world: compute writes small storage buffers in direct memory that later draws read as
  vertex buffers and through guest pointers, so each batch goes back to the CPU and is uploaded again. Direction: a
  device-side copy from the written storage object into its aliasing vertex and storage objects, in queue order, so
  those aliases no longer need the in-order write-back (`CommandProcessor::WaitDeviceAddressWriteBacks`,
  `GpuMemory::MarkStorageGuestPublished`).
- The roguelike: detiling takes 18% of the processor (about 0.2 GB/s) and residency 11%. Its documented null-block
  crash at 8 s appeared in two consecutive runs of an intermediate build and in neither of two runs of the final one.
- The Unity fishing title is not frozen: its autosave notice needs a held confirmation (a ring fills while the
  button is held), so tap routes stay on it. With held confirmations it plays its opening and reaches the first
  dialogue (see the vertex-buffer entry below for its frame rate).

Regression with both changes (runs d1228-d1236, 120-240 s each): the roguelike 12 and 14 fps (12 before) with no
crash in either run, the action platformer 69 (70), the painterly platformer 144 (146), the pixel-art Metroidvania
117 (124; its run streamed more textures, 5,023 texture changes against 3,371, a path these changes do not touch),
the pixel-art exploration title 27 (26) now showing its world, the beat 'em up 8 (6), and the sandbox title's menus
46 (49) with correct UI captures. Unit tests: 1,870 pass, none fail.

### CPU/GPU overlap and per-draw cost (2026-10-05, guest verified)

Scope: same strict configuration on the reference host (Intel Arc A770, Mesa `xe`). Profiled with gperftools on the
command-processor thread and the DRM `fdinfo` engine counters for the device. A pixel-art title whose vertex front is
bindless (V#s loaded from tables at runtime offsets) and whose only compute shader is the SDK's 16-byte buffer fill
went from 41 ms to 22 ms per frame (24 to 45 fps) on its start screen. Regression set (runs d586-d602, 90 s each,
previous run in parentheses): the gothic action platformer 107 fps (98), the Construct title 248 (251), the zoo builder 239 (220),
the racing title 229 in the race (181), the ninja platformer 594 (378), the 2D action title 201 (112), the cave exploration title 37 in its
new-game route (23), the artillery title 59 (59), the anime fighting title 220 (203), the isometric roguelike 59 (49), the .NET beat 'em up 11 (11). The fishing title ran clean once
(66 fps, was 52) and twice stopped at 12-14 s on its known stale extended-user-data V# (the same words as runs d221,
d466 and d571).

- **Instruction mnemonics without allocation.** The shader-usage pass runs for every draw and asked `magic_enum` for
  each instruction's name as a heap `String8` to test a prefix; with the SRT, assembled-descriptor and scalar-load
  collectors that was 13% of the processor. `ShaderInstructionTypeName` and `ShaderInstructionTypeStartsWith` return a
  `string_view` from the one translation unit that declares the enum range. Two translation units used magic_enum
  without that declaration (default range -128..127), so the names of later instructions (`SLoad*`, `TBuffer*`,
  `VCmpx*`) could read empty depending on which instantiation the linker kept.
- **Uniform buffer fills published on the device.** Before recording a draw or dispatch that dereferences guest
  addresses, every storage buffer a compute pass had written was written back: submit, CPU fence wait, then a page
  comparison of the whole buffer. A title clearing buffers with the fill shader paid that about 25 times per frame
  (13 waits, 16 ms per frame). For a dispatch the existing uniform-fill proof shows to store one value to every record
  of its bound range, the filled object is now copied on the device into the imported view of that range, between
  barriers, so later device-address reads in that queue see the result in GPU order, as on unified memory. Only
  chunks that alias the guest's physical memory qualify: a snapshot chunk could be refreshed from stale guest bytes
  before the CPU write-back. Device-address consumers on that queue then skip the write-back; the completed
  submission still writes the object back, and because its content is the known pattern the page cache adopts it
  without reading the GPU copy (constant time when the same pattern repeats). Objects with label fences or depth
  metadata in range keep the byte write-back.
- **WAIT_FLIP_DONE waits for that buffer's flip.** It waited for the device to finish everything recorded so far,
  which serialized recording and execution every frame. A GPU flip enters the flip queue when its submission
  completes, so the processor now waits only for the submission that recorded the latest flip to the requested
  display buffer, then for the flip queue as before.
- **Batch completion is asynchronous.** A batch whose completion payload is a flip, an end-of-pipe interrupt or a
  consolidated label wait no longer blocks the processor on its fence. The guest observes those through memory, event
  queues and flip status, never through its submit call; the fence poll of the next submission or the ring's idle
  pump (1 ms) publishes them. A wait on a plain label store of the current submission no longer splits it: a full
  barrier keeps the order.
- **Scalar flow shared per program.** The memoized scalar analyses (live-at-entry, entry-value sources, holding
  sets) were looked up three to five times per draw by materializing the program signature, and each lookup copied
  per-instruction vectors. The signature is now hashed and compared field by field without allocation, and per-draw
  callers share one `ShaderScalarFlow`.
- **Draw-time snapshots without page protection.** Small read-only buffers are copied per draw; each copy and each
  comparison armed and restored write protection around the read (two `mprotect` calls and a TLB shootdown, 18% of
  the processor). A copy is now validated by a second read: unchanged bytes held their value between the two passes,
  unless a store restored the old value in between. A comparison runs alone; a store racing it is the race the
  console GPU has with a pending draw, which guests order with labels.

Measured after these changes on the pixel-art title: the processor is busy about 9 ms per frame and the render
engine about 11 ms; the title submits its next frame after the previous flip, so the two still add up. Next:
per-draw shader usage parsing (about 10% of the processor) and submitting earlier within a frame.

Second round (same day):

- **HTILE size outside the table.** Gen5 depth sizes not in the table fell back to 8x8 cells of 8 bytes over the
  unpadded width, 0x70000 bytes for a 2500x1400 target. The game clears its HTILE with the SDK metadata-fill compute
  over 0x48000 bytes, so the clear never matched the depth target's HTILE range and was never applied: the target kept
  the previous frames' depth, and a racing title's car-select preview (reverse-Z, GEQUAL) showed only the edges of the
  rotating car. HTILE is one 4-byte word per 8x8 tile over whole 1024x512-pixel metadata blocks (32 KiB); this
  reproduces every table entry (720p, 1080p, 2160p) and the guest's clears (0x48000, and 0x8000 for small targets).
  A 32-bit depth row is padded to the 128-pixel width of its 64 KiB block (the colour target of the same pass already
  used pitch 2560).
- **Multisampled HTILE clears.** The metadata-fill clear was translated only for single-sample targets; the preview
  target is 2x MSAA. A zero HTILE word marks every sample of its tile cleared and the attachment clear covers every
  sample, so multisampled targets now take it too.
- **Guest-address residency scan.** Ranges without physical backing were still asked whether their physical range
  was unpopulated, a linear search over all physical mappings that never matches; per-range statistics timers called
  the clock thousands of times per preparation. A .NET title went from 43 to 37 ms per frame (table preparation
  28 to 21 ms). Its physical ranges still rescan whenever the physical population changes (18 ms per frame).

- **Idle queues publish their completions.** The 3D fighting title froze when its story mode started: it submits one
  ACB to each compute queue and spins on the ACB's end-of-pipe label (`while (*label != 1)`, found by profiling the
  frozen run: two guest threads at 100% in the spin and its caller). A plain label store carries no completion
  callback, so nothing polled that queue's fence once its ring went idle; earlier the label was published only when an
  unrelated wait drained completions, which the asynchronous batch completion above removed. Every ring now stays in
  its completion pump until its submissions retire, compute queue rings included.
- **More guest compute queues than host processors.** The same title probes nine queue handles (0x20, 0x21, 0x28,
  0x29, 0x30, 0x38, 0x40, 0x48, 0x50) and the ninth aborted with "exceeds available independent queues". Once each of
  the eight host compute processors has a handle, a new handle shares the processor with the fewest handles; its
  submissions stay in order there. The title now reaches its story-mode cutscenes (textures on characters are still
  corrupted and the scene runs at about 5 fps, open).

Verification: `run-check.sh <n> <slug> [route] [seconds]` in the scratch harness runs one title and prints a verdict
(FATAL with the error line, FROZEN when the observer confirms no presents, BLACK, STATIC, or ADVANCING) with the lit
fraction and change of each capture, so a fix is checked on the affected title instead of the whole set.

- **C++ stream facets follow the guest library's slot order.** The block-building sandbox title saved its float
  options (safe zone, UI scale, volumes) as `0xd` and `0x1000`: Kyty's classic-locale `num_put` listed `double`,
  `long double` and the pointer overload before `long long`, so `ostream << double` reached `do_put(const void*)` and
  printed an integer register. The title read the values back, laid its whole JSON-UI start screen out at one point
  (logo, buttons and button glyphs had zero size; text still drew) and only the splash and helper labels were visible.
  Every bundled `libc.prx` checked (20 titles) orders `num_put` bool, long, unsigned long, long long, unsigned long
  long, double, long double, pointer, and `num_get` (17 entries) bool, unsigned short, unsigned int, long, unsigned
  long, long long, unsigned long long, float, double, long double, pointer; Kyty's `num_get` had its only overload
  in the float slot. Both tables now match the guest library, found by resolving the vtable relocations of the
  bundled runtime, and the format helper's flag constants were checked against its `_Ffmt`/`_Ifmt` disassembly.
- **The same title's first-run path.** Its pixel programs needed two flow-analysis admissions (derivative fetches in
  lane-divergent loops, whose branches take quad votes, and AND/OR of uniform compare masks), its character previews
  an `8_8_8_8_UINT` vertex attribute, world creation a Linux `rename`-based file move and `_Thrd_sleep`, and its
  sockets BSD-to-host message flag translation. It now reaches the start screen, the first-run screens and world
  generation; in the harness (two cores) the world had not finished loading after 260 s.
- **The same title's network loop and menu surfaces.** Its LAN transport binds an IPv6 UDP socket (family 28, 28-byte
  address) and waits for it on a kernel event queue: sockets now carry both address families and apply socket
  options, `sceKernelAddReadEvent`/`AddWriteEvent` accept sockets (level-triggered, a host watch triggers the event
  when the socket becomes ready), and a receive buffer longer than any datagram is no longer refused with EMSGSIZE;
  the loop now reads its packets. `s_bfe_i32` is implemented (it was a missing-emitter placeholder). A sampled
  view with the depth swizzle (tile 24) whose address holds no depth buffer now reaches the render-target and
  storage-image lookups: the menu samples a 1920x1080 `32_FLOAT` surface a dispatch writes, and reading guest bytes
  instead left a dark rectangle behind the character preview.
- **Its HTTP client.** `sceHttp2SendRequestAsync`/`ReadDataAsync` never signalled the completion target the title
  passes (`{event queue, ident, udata}`, after `sceKernelAddUserEventEdge` for the request; its handler loop
  dispatches on `sceKernelGetEventUserData` and then calls `sceHttp2WaitAsync`), so every request stayed pending
  for the whole session. Finished operations now trigger that user event with its udata; the send ends with the
  synchronous call's result, which the handler reports as a failed request. That path then needed
  `std::future_category` (name and messages from the bundled C++ library).

- **Its menu surface: versioned register defaults.** The UI renderer repaints several dirty rectangles in one batch
  and confines it with a stencil test. `GraphicsGetRegisterDefaults2` handed an AGC version-13 client the older
  depth group (sixteen pairs) while that client reads seventeen, with `DB_HTILE_SURFACE` before the size and clear
  values: the depth-size maxima landed in the clear register, the depth target resolved to 1x1 and the stencil test
  was dropped. Full-screen translucent backgrounds were then blended over the whole surface (accumulating darkening,
  missing tiles, text and buttons, menus that looked frozen but still accepted Back). Version 13 now gets its own
  seventeen-pair group; other versions are unchanged. The HTILE control default is a secondary-reference lead, not an
  observed driver value. An earlier exclusion of stencil had inspected only the derived attachment state: check the
  guest's register state before excluding a mechanism.
- **Its normal-menu vertex layout.** Vertex formats 11 (`16_UINT`) and 57 (`8_8_8_8_SNORM`) resolved to undefined
  and aborted a five-attribute layout; both now map to their Vulkan equivalents (public GFX10 buffer formats).
- **Its storage figures.** Savedata search and mount info reported a fixed 100000 blocks per directory, and the title
  multiplies the search blocks by 64 KiB: three small saves appeared to use 18 GiB and it warned about storage. The
  allocation a mount requests is now recorded beside the title roots (`.kyty-capacity`, outside the payload; 32 KiB
  blocks for the standard mount calls, 64 KiB for the native one). Mount info and search report it with the blocks the
  payload uses, and delete drops it. A directory without a record (created earlier) mounts normally and reports its
  usage until a mount requests an allocation. SaveDataMemory backing moved out of the enumerated title directory
  (`.kyty-memory/<title>/memory/`), where a search had listed it as a save named `memory`.
- **Its sign-in dialog.** `libSceSigninDialog` was missing, so one variant of the first-run flow aborted when it
  opened the sign-in dialog. It is now a common dialog that finishes as soon as it opens with the user-canceled
  result, since there is no network account to sign in to. Contract from the title's wrapper: a 16-byte open parameter
  (size `0x10`, user, reserved), status 1-3, and a 16-byte result whose first word is 0 (signed in), 1 (canceled) or
  an error.
- **Releasing part of an address reservation.** Consuming part of a reserved range released the whole block and then
  re-reserved its prefix and suffix; a host mapping made by another thread in that window took the address, the
  rollback could not re-reserve the block and the run aborted at `Memory.cpp` while the sandbox title loaded its
  world. Where the host supports it (Linux), only the consumed part is now released (`VirtualMemory::FreeRange`);
  hosts without partial release keep the previous path.

With these the title passes its first-run screens with an intact interface, creates the world database and logs the
player's connection and spawn within the 300 s harness window. Controllable gameplay and shared-path regressions are
not yet verified.

Investigated and left open:

- **Register-write routes (2026-10-06).** Nonzero Type-0 headers with a body reach `CommandProcessor::Run`
  (`GraphicsRun.cpp`) but are consumed without applying registers, `cp_op_one_reg_write`
  (`GraphicsRunOpParsers.cpp`) consumes its body without a setter, and the direct context table has no depth-size
  callback. Establish their packet encodings before implementing dispatch; none occurred in the sampled menu window.
- **Input method parameters (2026-10-06).** One path through the sandbox title's world list aborts on the unresolved
  `sceImeParamInit` (`WmYDzdC4EHI`, `Ime_v1`). The IME library has no parameter initialization or real session yet;
  its contract must come from evidence before it is implemented.
- Earlier sandbox runs appeared to stop advancing at a varying point (character select, its transition, or the world's
  "Loading" at about half of the bar): presents continue at 40-50 fps and every frame submits its draws, but the
  picture no longer changes and pad input has no effect. Sampled during the freeze: the main thread waits on the
  frame task inside the frame builder (the game class update), one worker runs the render task, every job worker
  waits for work, the network threads wait on their event queues and no level-server thread exists; the world
  container never gets its `db/` directory. Excluded, with the evidence: a lost wakeup in the C11 `_Cnd`/`_Mtx`
  layer (static objects are created under a lock; `_Xtime_get_ticks` is microseconds and the workers' timed waits
  are 90 ms-10 s ahead), a failing file operation (savedata trace: no failed open/rename/mkdir; `/temp0` and
  `/download0` are created by the title itself), a pending system service or dialog (per-frame HLE histogram: only
  the usual pad/user/system/flip polls), the socket event change (the freeze also happens with it disabled), the
  pending HTTP requests (they now complete; the freeze stays), the title's task-queue timer (no delayed submit
  fails and every timed wait has a sane deadline: job workers 0-10 s, the RakNet loop 10 ms, the WebRTC thread 5 s
  on its monotonic condition; the libHttpClient-style request path is not the one the title uses), and its
  network/PSN view (NetCtl reports disconnected and NP signed out).
  The static, darkened menus were the stencil defect above, not a stall (Back always worked). The remaining Unity
  title has a similar visual symptom (below), but a shared cause has not been established. The title also imports
  `sceImeOpen`/`SetText`/`SetCaret`/`GetPanelSize`; a real IME session (open event through `sceImeUpdate`, text
  input) is not implemented and these were not called on the first-run path.
- The remaining Unity title sometimes stays on its autosave notice ("Begin") for the whole window (262 s, spinner
  frozen, presents continue) with code identical to a run that advanced at about 75 s (d1027 vs d1072/d1073 on the
  same sources, same environment and logs). This is not caused by the network changes: it never sets a socket
  option or blocks in a receive on that screen.

- The .NET beat 'em up stops at about 1.7 s in some runs (also before this work, runs d474 and d530): its runtime
  reads a null table pointer plus 0x120 (`mov r11, [rax + r10*8]` with rax = 0) while summing allocation statistics.
- The pixel-art title's new game starts with a "wake up" prompt that none of the scripted pad buttons pass (all face
  buttons, shoulders, triggers, sticks, d-pad, options and touchpad were tried, tapped and held); the title menu
  before it works.

Third round (same day):

- **Published fills skipped write-backs that still had work (regression from the first round).** The 2D action title's gameplay
  smeared sprites horizontally (trails behind the character) and, some seconds later, turned the whole scene white;
  only the HUD survived. It happened in most runs since the device-side publication of uniform fills, and in some
  runs never, depending on how submissions overlapped. Bisection: publishing the fill's bytes into the guest view
  was correct; the defect was the exemption it granted. A published object is skipped by the in-order write-back
  that precedes every device-address draw, so its write-back runs only when a completion finds all of its uses
  retired. The title rewrites the same fill objects every frame, so with submissions in flight that point may never
  come (run d724: after the first gameplay seconds, none of the 642x362 fill objects was written back again). One of
  them is the depth target's HTILE, cleared to `0xfffffff0` by the same fill shader; the depth clear is recognized
  from that write-back, so the depth target kept old depth, the G-buffer draws failed the depth test against it, and
  the stale colour stayed (trails) until the light pass saturated (white). The publication mark now requires that the
  write-back has nothing left to do: no depth metadata on the object, and every overlapping object an exact
  colour-image alias the device wrote after the object's last write (the fill's propagated clear, or later
  rendering). Depth/HTILE, other buffers and partial aliases keep the in-order write-back. Two runs of the walk route
  render correctly at 47-55 fps (the unsound exemption gave 92-100; the depth clear costs one wait per frame again).
- **Write-backs keep newer device content.** Each GPU object records the logical time of its latest writable use
  and of the latest device write into it. A completed write-back no longer reloads or invalidates an overlapping
  object the device wrote after the written-back object's last write; it takes the written guest bytes as that
  object's baseline (hash and dirty generation) so its next use does not reload them over the device content either.
- **Guest-address residency without quadratic lookups.** The .NET beat 'em up maps 12,516 GPU ranges, almost all of
  them physical. Whenever the physical population changed, the preparation asked for each unimported span whether it
  was unpopulated, and each question scanned every physical mapping (122 ms preparations). Physical mappings keep an
  index ordered by guest address, rebuilt only after the list changes; live views never overlap, so the view holding
  an address is the nearest live view at or below it (a miss falls back to the conservative answer). Preparation also
  walks only flexible ranges and ranges holding tracked snapshots instead of all ranges and chunks. Over 180 s the
  preparation total went from 32.7 s to 7.1 s (maximum 123 to 59 ms).

- **The isometric roguelike reaches gameplay.** Three independent defects stood between its title and its hub:
  - *Branch-heavy shader compile never finished.* After cross, the command processor sampled 70% of the process in
    `ArmReachesBeforeJoin`: a 2,299-instruction pixel shader with 254 branches. Selection-join queries were repeated
    for every label and branch during emission, located instructions by scanning the program, and ordered a join's
    edges by recomputing nesting depth per comparison. Instructions are now found by bisection, reconvergence and
    nesting results are kept per thread for one program and label state (disabled labels change them), and depths
    are computed once per edge.
  - *SW_64KB_S volumes.* A 32x32x32 RGBA8 lookup volume in the 64 KiB standard swizzle was rejected. The GFX10
    64 KiB standard 3D pattern keeps the 4 KiB pattern in its low 12 bits and adds `X3 Z3 Y4 X4` (4-byte elements;
    the table holds every element size), verified against the addrlib pattern tables.
  - *Invisible menu text.* Tile 9 sampled textures uploaded guest bytes only for four formats; the menu's R8 glyph
    atlases (CPU-written) stayed empty, so each label showed only its plate. The tile-9 host detiler matches the
    GFX10 SW_64KB_S pattern for 1, 2, 4, 8 and 16-byte elements, so any such uncompressed format now uploads when no
    live surface covers it.
  The isometric roguelike now shows its menus and walks in its hub at about 10 fps (open: performance, and a crash at 8-11 s in
  some runs where its GPU allocator returns a null block and a row copy writes to 0x40; its direct-memory allocate
  and map calls do not fail in those runs).

- **Linear mip chains.** The anime fighting title's characters showed shredded textures: almost every material is a linear (tile 0)
  BC1/BC3 texture with a full mip chain, and the linear fallback laid levels out from level 0 with halved pitches.
  GFX10 linear mip chains (addrlib `HwlComputeSurfaceInfoLinear`) store the levels from the smallest one to level 0,
  each with its element pitch aligned to 256 bytes, element sizes halving on their own (4x4 blocks for BC). The
  fallback now computes that layout and each mip upload uses its own pitch; the rule reproduces all 2,135
  multi-level entries of the SDK-generated linear RGBA8 table (offsets, sizes, padded pitch and total).

- **SDK pattern fill.** The isometric roguelike clears its 1920x1152 targets every frame with the SDK pattern-fill kernel
  (`buffer_store_format_x` of `values[i % period]` for `i` below a count, 32-bit UINT records). It was never
  recognized, so the clear reached the storage buffer but not the render-target images: rooms with strong lighting
  showed saturated green, smeared columns and stale silhouettes. The uniform-fill proof now matches that kernel's
  exact template (instruction types, branch targets, operands); with a live period of 1 it is a uniform fill,
  propagated to the exact image aliases and published like the 16-byte form (publishing: 14% more frames, write-back
  time 11.8 s to 0.8 s over 120 s). A first attempt never matched: the proof rejected programs with indirect labels,
  which every conditional branch records for its fall-through, so its measured cost was not the fill's.
- **Device-address scans over writable storage only.** Before a draw or dispatch that reads guest addresses, GPU
  memory looked for storage buffers with pending GPU writes by walking every live storage buffer, twice; the anime fighting title keeps
  about 4,000 alive and the ordered-set walk was the top host hotspot of its cutscene. The scans now walk the
  in-use, writable subset.
- **Partially released direct mappings keep their live part.** The .NET beat 'em up's runtime shrinks a buffer by
  releasing the tail of its direct memory (0x504000 of a 0x55c000 mapping) and maps new memory over that tail. The
  release marked the whole view released, so the fixed map dropped the head too; the unmap then removed only
  protection blocks that start inside it, so the whole-mapping block survived and a virtual query of the head still
  reported 0x55c000 bytes. Its release loop (query, then release `va - start + offset`) then released memory that
  already backed other allocations, or, for the dropped head (a reservation, offset 0), physical address 0: the
  1 GiB "User Malloc" mspace whose header lives there. The title died within 2 s in about half of the runs, with an
  `OutOfMemoryException` from its garbage collector, a corrupted mspace mutex, or a fault on reused GC pages. A
  release now splits the views it covers in part (the released piece stays a released view), an unmap frees exactly
  its view's pages (`FreeRange`, as cuts already did) and trims the protection blocks, and a queried region never
  extends past its mapping. With released views made inaccessible as a check, five runs never touched one.
- **Written-back writable buffers retire.** The same title binds ring-buffer sub-ranges as writable storage views;
  linked writable views never retired, so one 288 KiB region held 4,000 objects and grew by ~1,000 every 25 s, and
  each overlap query returned hundreds of them. A writable buffer owns no content once its writes are written back
  (`in_use` clears), so it now retires after 120 idle frames like a read-only one.
- **Write-back and overlap scans use indexes.** Completed-submission write-backs walked every object of every heap
  (12,546 heaps, ~16,000 objects, on every completion); they now visit an index of in-use writable objects (18 on
  average). Overlap queries walked every heap; the heap span index now lists every heap covering a span. Loading
  frames went from 18-23 to 30-36 fps on that title and the 2D action title's walk route from 47-63 to 86 fps.
- **Unsigned-only shaders read the unsigned descriptor banks.** The .NET beat 'em up's logo video was a uniform
  green screen: its Theora planes are written by a dispatch as R8_UINT storage images and sampled by a shader that
  samples only unsigned textures, which bound the float banks and read zero (BT.709 of zero planes is (0, 77, 0)).
  Such shaders now bind the unsigned banks. Storage images are created with `MUTABLE_FORMAT` and a format list of
  the same-size table formats when supported, so an R8_UINT image can be sampled through an R8_UNORM view whose
  usage is restricted to sampling (`st-reinterpret`).
- **Every block-compressed format copies from its storage blocks.** The title uploads sprites and glyphs by a
  dispatch that stores each 4x4 block through a uint view (R32G32B32A32_UINT for 16-byte blocks, R32G32_UINT for
  8-byte ones). Only the 16-byte formats were copied into the sampled texture, so BC1 text drew black. A cached
  texture is copied again when a dispatch wrote its storage image after the texture's content was established
  (process-wide stamps), never when the texture is newer.
- **Storage images the GPU wrote are not retired.** Frame retirement freed unlinked storage images idle for 120
  frames. Storage images are not written back, so those holding uploaded sprite blocks took the only copy with them;
  the sprites were then created again from guest memory that was still zero (BC3: transparent, so characters were
  invisible; BC1: opaque black rectangles over the scenery). Logging every BC upload showed 142 all-zero guest
  uploads per run before the change and none after. A storage image with a writable use now lives until the guest
  releases or rewrites its range.
- **`__dynamic_cast` walks the guest RTTI.** Continuing a save crashed with an execute fault in data: ICU's
  `Calendar::makeInstance` asks its service for a calendar, which may return a `UnicodeString` alias, and tells the
  two apart with `dynamic_cast`. The export only applied the `src2dst` hint, so with hint 0 every cast succeeded and
  a calendar was read as a string; the second lookup returned the alias string and its "calendar" vtable slot 23
  pointed past the string's 11-slot vtable into type_info data. The cast now finds the most-derived object, walks
  the `__class_type_info`/`__si_class_type_info`/`__vmi_class_type_info` records that guest type_info objects
  relocate to (the exported vtables) and applies the downcast and cross-cast rules; unknown kinds keep the hint.
- **`sceAcmContextCreate` takes one argument.** It creates a 32-bit handle; the title tests it with a 32-bit compare
  and tail-calls the export with leftover registers. Reading those as a size cleared 0x10b bytes past the handle,
  wiping the audio engine's globals, and the title crashed about 117 s in.
- **Overlap caches are sized to their heaps.** Each of the title's 12,500 heaps allocated a 4,096-entry overlap
  cache up front (about 4.5 GiB, mostly empty), which tripped the host memory guard. The cache is created by the
  first query and grows with the heap's object count.
- **Allocation and invalidation costs.** Profiling the title's intro (gperftools, `CPUPROFILESIGNAL`) showed 41% of
  CPU time in the driver's GPU virtual-address allocator and 19% in its ioctls: every upload created a staging buffer
  with its own `vkAllocateMemory`. Allocations up to 4 MiB are now sub-allocated from 64 MiB blocks per memory type
  and tiling kind (host-visible blocks stay mapped), bringing those to 13-16% and 4-5%. Releasing direct memory
  invalidated the GPU view of the whole mapping and walked all 15,000 device-address ranges (22% of CPU time);
  only the released pages are invalidated now, through the ordered range map, dropping only the imports that cover
  them. Overlap candidates spanning several 1 MiB buckets are deduplicated through a set past 32 ids (the linear
  dedup took 12%).

Investigated and left open:

- The anime fighting title's in-engine cutscenes run at about 5 fps. The guest spends most of its time spinning on GPU labels
  (`while (*label != 1)` after an ACB submission, and two similar loops), so the frame rate is the round trip of its
  compute work. Each of its ~20 dispatches per frame completes with a write-back of a 15 MB storage buffer of which
  about 53 KB changed; the page comparison reads all 15 MB (3 ms per dispatch, 27% of the time). Skipping it is not
  correct (the CPU may read the results); the planned fix copies GPU-written pages on demand. The dirty-page tracker
  also accepts only 512 ranges, so most of the anime fighting title's buffers fall back to full hashes (250 GB hashed in 2 minutes).
- The .NET beat 'em up now reaches gameplay with correct sprites, text and HUD glyphs, but runs at 7-25 fps under
  the harness (two CPU cores, shader validation). Remaining host costs: every upload still waits on its own
  util-queue fence (about 11 s of fence waits per minute), the device-address residency rescan after population
  changes (about 10 s per minute across ~16,000 mappings, one `lseek(SEEK_DATA)` per unimported span), and
  `PhysicalMemory::Find` scanning every mapping for `sceKernelQueryMemoryProtection`.
- The .NET beat 'em up is not frozen: its black screen is a loading screen (its two sprite draws are black, vertex
  colour (0, 0, 0, 0.88), on a black target) while it uploads textures through compute image copies, about 20 per
  frame. Each copy uploads its source and destination images and writes the destination back, each a separate
  util-queue submission with a fence wait (`UtilFillImage`/`UtilFillBuffer`): 92,541 waits in 180 s, about 23 ms of
  every 48 ms frame. Next: record uploads into the consuming command buffer. Its host footprint (4.5 GB of guest
  memory plus 4 GB of emulator heap) also exceeds the harness cgroup's 7 GiB `memory.high`, which throttles it.
- The 2D action title sometimes stops after about a minute of gameplay on a G-buffer pixel shader that needs fragment wave
  transport (a derivative fetch inside a lane-divergent loop at pc 0x114, 642x362, four targets); seen in runs d472,
  d721 and d729, so it predates this round.

### Performance, new-title repairs and add-on content (2026-10-04, guest verified)

Scope: strict runs on the reference host (Intel Xe, Vulkan 1.4, Native, `KYTY_SHADER_OPTIMIZATION=None`, shader
validation on). A 2D action title went from 25 to 62 fps; a 3D fighting title boots through its intro and title
screen at 120-140 fps with correct UI art. The existing suite passes (1,873 tests).

- **Performance.** GPU heap lookups use an index of disjoint spans; device-address storage queries are batched;
  physical population is gated; completions are asynchronous with consolidated waits; scalar liveness is memoized
  by content signature (profiled with gperftools, not guessed).
- **Linear block-compressed rows.** Gen5 linear BC rows are padded in block columns (256 bytes of 4x4 blocks), not
  texel columns; aligning texels sheared every block row of BC1/BC3 UI art. The tile-0 size estimate counted texels
  as blocks (16x too large). See `graphics-texture-layout-troubleshooting.md`.
- **WRITE_DATA ordering.** An immediate write to an address whose end-of-pipe write is still pending is deferred
  behind it; landing first let the older value overwrite it (label order inversion, boot stall).
- **AMPR command buffers.** Measure functions depend only on the file offset; the `_04_00` write-address and
  kernel-event forms take a completion-mode flag; record ids are used as-is (0 is valid); every record executes in
  order, completion actions last, and the first failure is the execution result read by `AndGetResult`.
- **Guest malloc before the application heap.** Host-heap allocations are zeroed (a path buffer read stale bytes)
  and GPU objects in those ranges are hashed directly (UI vertex data rewritten every frame looked unchanged).
- **Loader and HLE.** SELF segments are bounded by the real file size (one title declares 8 bytes more); libc is
  mirrored into LibcInternal and POSIX across libkernel by the registry instead of duplicate tables; event flag
  queue/thread attributes are decoded; offline Remoteplay, NpAuth (requests complete SIGNED_OUT) and UDS modules.
- **Add-on content.** One inventory answers AppContent and NpEntitlementAccess from `dlc_emu.ini` and installed
  package folders; mount and unmount follow the published contract. See `addon-content.md`.

Second round (same day):

- **Auto-index draws start at `GE_INDX_OFFSET`.** It is the first vertex of a `DrawIndexAuto` as it is the base vertex
  of an indexed draw; the auto path passed 0, so a pixel-art title drew every tile batch from the start of its shared
  vertex buffer (scrambled streaks). It now reaches gameplay.
- **NGG fronts with a user-data base vertex.** `v_sad_u32 vN, sK, 0, vN` adds the base vertex/instance before the GS
  allocation under an EXEC of all lanes. The proof models three-source lane-local ALU, scalar words derived only from
  user data, and lane-local writes under an EXEC covering the vertex mask (exports keep the exact mask). An artillery
  title now runs at about 58 fps (output is a flat colour, open below).
- **Missing guest data symbols.** Unresolved data imports pointed at the invalid-memory sentinel; the C++ runtime's
  fundamental `type_info` objects (`T`, `T*`, `const T*`) and two vtables come from one table.
- Format 50 had no element size (zero-size texture, fatal overlap query); `localeconv` (Dinkumware layout,
  `decimal_point` at +0x48), `wcsftime`, `pthread_getname_np`, save data parameters per save directory, trophy info
  arrays, `WaitOnAddress` command sizes and graphics user data in indirect SH register lists.

Third round (same day), mostly runtime-library correctness found by Unreal Engine 4 boots:

- **Wide printf.** `vswprintf` accepted only `%%`, `%i` and `%08x` and returned -1 otherwise; string builders grow the
  buffer and retry on -1, so two UE4 titles spun forever in startup. One wide engine (`FormatWide`) now formats every
  conversion through the narrow engine, with `%ls`/`%lc` wide and `%s`/`%c` narrow; the narrow engine accepts `%ls`.
- **Dinkumware character classes.** `_Iswctype` classes are 1 alnum .. 12 blank (titles scan format specifications
  with 2 = alpha and trim with 9 = space); class 2 had been read as "digit". `_Towctrans` (1 lower, 2 upper) and
  `std::_Xinvalid_argument` were registered as data objects; `_Xout_of_range`/`_Xlength_error` returned instead of
  throwing. All four now behave as functions; the `_X` helpers throw `logic_error`-derived exceptions with what().
- **Direct memory mappings may span adjacent allocations.** A title allocates direct memory in chunks and maps 4 GiB
  windows across them; `Map` required a single allocation (EACCES) while `Release` already accepted spans.
- **Guest entry stack.** `InvokeOnStack` placed the terminating frame record below the guest stack pointer, where the
  callee's pushes overwrote it; frame-pointer stack walks then ran off the end. It now sits above the guest stack.
- **Files larger than 4 GiB.** `sys_file_size` truncated through `uint32_t ftell`; a 17 GB pak was reported as
  390 MB and its footer read at the wrong offset. Sizes come from `stat`/`fstat`.
- **Kernel AIO** (`sceKernelAio*`): requests run when submitted and are complete when polled or waited for.
- **Scalar spans of dynamically loaded descriptors.** A buffer descriptor loaded with `S_LOAD` is bound only over the
  bytes its scalar buffer loads read. The span came from the last consumer alone, so constants read earlier through the
  same descriptor fell outside the binding and read as zero. A Unity title's colour-grading LUT was baked from zeroed
  parameters into solid black, which turned every 3D view black while 2D overlays stayed correct; geometry whose
  matrices came through such a descriptor was also scrambled. The span is now the union of every consumer in the
  mapping's window; a consumer that is not a constant-offset scalar load keeps the whole descriptor bound.
- **APR entry points are raw system calls.** Every title links an SDK wrapper around the fourteen
  `sceKernelApr*` imports (resolve, file size/stat, submit, wait) that turns a negative return into
  `0x80020000 | errno` and reads a zero errno as success. The HLE returned the SCE code without setting errno, so a
  missing loose file "resolved" with an uninitialised size; a UE4 title then allocated that size (the out-of-memory
  report printed the 0x7FC0000000 reserved range). The exports now return -1 with errno set.
- **Host-side contention.** Every guest mutex lock took the static-object registry lock and two state locks; guest
  malloc/free took up to four registry and API locks. Existing objects are now returned lock-free (handles are
  published after initialization), mutex ownership is an atomic only the owner writes, and the allocator API table is
  an immutable snapshot.
- **Middleware the title ships.** An HLE library named after a title's own SDK module kept the loader from loading
  the real module; it is removed and the shipped module loads as a package sidecar.
- `wcstof`/`wcstod`/`wcstol`/`wcstoul`/`wcstoll`/`wcstoull`, guest `exit`/`quick_exit`/`abort` and abnormal-termination
  reports log the guest caller, and `NpManager` has one export list.

- **Smaller imports a UE4 title needs at boot.** VoiceQoS is registered under module version 1.1 (the import
  never resolved under 0.0); `sceNetCtlGetInfo(LINK)` reports the link state
  consistent with `sceNetCtlGetState` (connected since the local-only network contract); `sceAgcDcbSetPredication` writes the SET_PREDICATION packet, and the command processor accepts clearing
  predication and stops on the query-dependent operations, which no title has issued yet.

- **Clear-state scissors.** CLEAR_STATE and the context-state operations rebuilt the context with every scissor at
  zero, an empty rectangle; the hardware clear state (and the AGC register defaults) cover 16384x16384 with the
  window offset disabled. A title whose video and fade quads relied on the default state showed only its solid cyan
  clear colour, and another title's 3D attract scenes were black.
- **Adaptive mutexes refuse a relock by their owner** (EDEADLK, as error-checking ones do in the BSD thread library).
  Kyty counted the relock as recursion, so a UE4 title that relocks an adaptive mutex and unlocks it once kept it
  held while its game thread polled, and the worker threads that had to finish the polled work blocked forever.
  Normal mutexes keep the recorded recursion behaviour.
- **AGC jumps and predication.** `sceAgcDcbJump`/`sceAgcAcbJump` (INDIRECT_BUFFER with CHAIN) and their GetSize,
  `sceAgcSetPacketPredication`, and SET_PREDICATION with the wait hint, which never skips a packet here because the
  command processor runs ahead of the GPU work that writes the result.
- **Images a UE4 title creates at boot:** 8_8_8_8_UINT and 16_UINT storage images, a BGRA view of an RGBA8 UNORM
  storage image, one-element volumes in any swizzle mode, SW_256B_S (tile mode 1) single-level textures, and a
  storage image created over a smaller sampled texture (linked; it seeds from guest memory).
- **AvPlayer frames use 256-byte rows** in both NV12 planes, the row alignment GPU linear surfaces require; a UE4
  title copies its movie frames with that pitch, and tight rows produced the image repeated about fifteen times
  across the screen. The extended frame info reports the padding as `crop_right_offset = pitch - width`: a Unity
  title sizes its movie texture by the pitch and crops by that field, and with a zero crop the 128 padding columns
  of a 1920-wide movie showed as a green bar (zeroed NV12). `sceAvPlayerGetVideoData` (no pitch in its frame info)
  keeps rows of the visible width.
- **Linked read-only buffers retire one at a time.** Overlapping read-only storage views are linked to each other
  and to the large buffers they alias. Retirement used to free a linked buffer only together with its whole linked
  component (at most 64 nodes, all idle); a roguelike's per-frame views joined one component that also held its
  always-live buffers, so none was ever freed: live objects grew from 98 to 22,064 in 80 s and the frame rate fell
  from 33 to 11. A read-only buffer owns no content (its bytes are guest memory or a copy of a peer, and a pending
  GPU write keeps an object writable), and freeing it drops both link directions, so each one idle for 120 frames
  now retires alone. The same title then holds about 100 live objects and a steady rate over 220 s.
- **Standard 4 KiB arrays use the tiled pitch.** A SW_4KB_S 2D-array texture took the linear row rule (256-byte rows,
  or the T# word-4 pitch), so a 1115x306 RGBA8 slice was laid out with pitch 1152 and its 32-row blocks reached
  0x168000 bytes past a 0x160000-byte guest mapping; the detile read faulted. Hardware applies a custom pitch to
  linear surfaces only, and the canonical pitch (1120) gives 0x15E000, exactly what the title mapped. 2D textures
  already used it; arrays now do too.
- **Descriptors behind shader resource table pointers.** A pixel shader whose SRT holds three 64-bit pointers loads
  its S# from `ptr0+0` and its T# from `ptr2+0` (`s_load_dwordx4/x8` with a constant offset). Such loads, while the
  pointer still holds its user-data value and every consumer is an image or sampler operand, are materialized like
  the extended-user-data (EUD) loads: the table is snapshotted at draw time (two equal reads) and the S_LOAD is
  rewritten through the same PC-keyed mapping. Buffer descriptors behind those pointers keep their existing paths.
  The emitter now takes a mapped load's table offset from its mapping, so a nonzero SMEM immediate also resolves.
- **GLOBAL loads.** `global_load_dword{,x2,x3,x4}` (FLAT encoding, GLOBAL segment) read a per-lane 64-bit address (a
  VGPR pair, or a 64-bit SGPR base plus a 32-bit VGPR offset) plus a signed 12-bit offset through the guest device
  address table, which imports every GPU-visible guest mapping. Any other FLAT/SCRATCH/GLOBAL opcode still stops
  at decode.
- **`sceSslGetCaCerts`/`sceSslFreeCaCerts`** return an empty CA list for a live SSL context (the emulator exposes no
  system certificate store; verification against an unknown issuer then fails as it would). An Epic online SDK
  module calls it at boot.
- **`__powisf2`** (float to an integer power) is exported next to `__powidf2`; both now square and multiply, as the
  guest's compiler runtime does, instead of calling `pow()`.
- **SGPR copy provenance.** The scalar dataflow that told which SGPRs still hold their user-data value now tracks
  which user-data word every SGPR holds at each instruction through S_MOV_B32/B64 copies (a must-analysis over the
  decoded CFG; the old predicate is its "holds its own word" case). A compute shader that moves a V# out of its
  user-data position, or builds two V#s that share their stride, record and format words (s1..s4 and s0 plus
  s2..s4), gets one storage binding per distinct descriptor and register, and the V# registers are written with
  that binding's metadata right before each buffer instruction.
- **More descriptor loads behind SRT pointers.** One `s_load_dwordx16` holding two T# (or an 8-dword load holding two
  S#) is split into descriptor blocks, each analysed as its own load of the same PC; several mapping records of one
  PC are valid when their dword ranges are disjoint. A descriptor loaded before a conditional branch and consumed
  after it is followed across the branch (the scan stops at the first clobber in program order and at unconditional
  branches). The EUD collector keeps its straight-line scan.
- **Scalar data through guest pointers in pixel shaders.** A pixel shader S_LOAD that neither the EUD path nor a
  descriptor mapping resolves (for example two dwords of an SRT-pointed table into VCC, then a load through VCC) now
  enables guest device addressing for that shader only; descriptor loads stay on their bindings, so the earlier
  slowdown of marking every pointer-loading pixel shader does not return. VCC is accepted as a 64-bit S_LOAD base.
  The constants these lowerings name are declared up front; a missing one made the SPIR-V refer to an undefined id.
- **Typed buffer loads through an unbound V#** (a descriptor loaded at a runtime offset, as a vertex front with
  bindless vertex streams does) read through guest device addressing with the formats the bound lowering decodes:
  32-bit components (single component only for formats 20/22/36/39), R16G16_FLOAT and R16G16B16A16_FLOAT; other
  formats read zero, as the bound path leaves them unread. The raw and typed variants share one address helper.
- **Fixed direct maps replace what they cover.** `sceKernelMapDirectMemory` with MAP_FIXED (0x10) and without
  MAP_NO_OVERWRITE (0x80) replaces the mappings in its range, as the BSD kernel and other PS5 implementations do. A
  .NET runtime commits its GC heap by allocating, mapping at a fixed address inside a 256 GiB reservation and
  releasing; after a release it maps a new 256 KiB view straddling two released 256 KiB views, which Kyty refused
  with EBUSY (it replaced only an exactly equal released view), and the title died with OutOfMemoryException. A
  released view is now dropped whole (its pages may already back another allocation) with its parts outside the
  new range returned to the reservation, a live view is replaced when it lies inside the range, and the range
  becomes a reservation the normal fixed-map transaction consumes.
- **Partial unmaps.** `sceKernelMunmap` accepted only an exact mapping or a range inside one reservation block. A sandbox
  title's allocator reserves a large block, maps an aligned part, decommits pieces by reserving over them and later
  frees a span covering many blocks; it also maps over part of a live mapping. Following BSD munmap, a range now
  unmaps every mapping and reservation part it covers, a direct mapping crossing the range edge is cut (the host
  unmaps the page-aligned part and the mapping, its physical offset and protection blocks split; the GPU side
  releases only the cut range), and a fixed map cuts the part of a live mapping it overlaps. Cuts run before any
  other change, so an unsupported cut leaves the state untouched. The host split is implemented on Linux; on
  Windows the views and placeholder reservations are not split yet and such unmaps fail as before. A flexible
  mapping crossing the range edge is still refused.
- **What every guest thread waits on.** The blocking HLE waits (contended mutex, condition, semaphore, event flag,
  event queue, sleep, join) record their kind, object and guest return address in the thread for their duration;
  the agent `threads` tool reports them for the main thread too (it was missing from the list) together with the
  loaded module bases, so a freeze names the waiting code without a debugger. Only a contended mutex lock is
  recorded, so the uncontended path stays a single trylock.

Regression set after these repairs (run d406-d417, 90 s each, same host): the watercolour platformer 119 fps (104 before), the gothic action platformer
85 (70), the Construct title 195 (89), the zoo builder 202 (83), the ninja platformer 320 (269), the 2D action title 94 (82), the anime fighting title 87
(54), the cave exploration title 22 (20), the racing title back to its 3D views at about 110-120 fps in the race. A longer
run of the gothic action platformer reaches gameplay; its intro frames vary with load timing between runs.

Open blockers (one root cause each, none investigated past the point stated):

- The UE4 fighting title now reaches its startup movie in some runs. Open, not yet root-caused: runs end
  nondeterministically around 25 s, either on a guest `scePthreadMutexLock` of a null object plus 0x58 or in a
  stall; a 400x5x5 R11G11B10 volume in SW_64KB_R_X (tile 27) is not laid out yet. About 100 AGC entry points it
  imports (mostly `*GetSize`) are still missing and are added as they are called.
- The isometric action title now runs its whole observation window without a fatal (about 50 fps), but the frames are
  black: its vertex front loads V#s from descriptor tables at runtime offsets (`s_load_dwordx4 s[16:19], s[14:15],
  vcc_lo`) and fetches typed vertex data through them. The typed guest-address loads above cover the decoded
  formats; whether those reads return the sprite data (and which other path still yields nothing) is next.
- A beat 'em up (.NET with SDL_GPU) now runs its whole observation window: the GC heap commits succeed with the
  fixed-map replacement above. Frames stay black (one solid green) at about 11 fps; a fifth of the time is spent
  preparing the guest device address table.
- A first-person puzzle title stalls after one frame: its main thread sleep-polls inside the FMOD Studio module
  (`Sleep(ms)` wrapper at +0x2440) while the Studio threads wait on semaphores and an FMOD core thread sleep-polls;
  the asynchronous bank load or mixer it waits for never completes. Not root-caused yet.
- The arena fighter that froze when its attract sequence started now plays it (3D scenes at about 200 fps for the
  whole window).
- The remaining Unity title still stops 25-80 s in with a garbage V# in extended user data dwords 40-43 of its
  colour-grading pass (the same slot holds valid LUT parameters in earlier runs and in another Unity title); the
  bound span is now the full 272 bytes, so the remaining defect is the stale EUD contents.
- A sandbox title now passes its allocator's partial unmaps and fixed remaps and loads further (about 21 s); it stops
  on a pixel shader with a derivative fetch inside a lane-divergent loop, which needs the fragment wave transport
  the renderer does not select yet.
- A roguelike exits by itself (exit 0, no frame) without calling libc exit.
- A first-person puzzle title: stalls after one frame.
- AMPR `_04_00` counter and wait commands are still no-ops; most `sceAgc*GetSize` entry points are added only when a
  title calls them, from the size the matching Kyty builder writes.

### Title runtime repairs (2026-10-04, guest verified on seven of eight titles)

Scope: strict runs of the eight-title regression set on the reference host (Intel Xe, Vulkan 1.4, Native,
`KYTY_SHADER_OPTIMIZATION=None`, shader validation on, 300 s limit). Seven titles reach gameplay with no fatal for
the whole observation window (five Unity titles, a Construct title and a Haxe title); a sixth Unity title
still stops on an unmapped constant-buffer V# (open item below). Every repair has a focused
red->green test; 1,859 unit tests pass in one process; boundary, table, playable-regression and capture gates pass.

- **Interrupt context id.** `sceAgcDriverGetEqContextId` (`Zw7uUVPulbw`, name confirmed by NID hash) returns the
  `interrupt_ctx_id` of the ReleaseMem that raised a graphics event. The 8-dword ReleaseMem envelope carries it in
  its last dword; the command processor passes it to the interrupt label and the event delivers it as `data` (no
  title reads an EOP timestamp there). The guest indexes per-frame GPU timing slots with it.
- **Case-insensitive guest paths.** An IL2CPP player loads `Il2CppUserAssemblies.prx` that its package stores as
  `Il2cppUserAssemblies.prx`, once, and works on hardware. Mounted paths resolve each missing component to the
  only host entry equal ignoring ASCII case (`Path::MatchCaseInsensitive`); the exact entry wins, an ambiguous one
  stays unresolved, and missing tails stay verbatim so creation and ENOENT follow the guest.
- **VideoOut blank flips and GPU-queued flips.** Index -1 (`SCE_VIDEO_OUT_BUFFER_INDEX_BLANK`) presents a black
  frame (`WindowDrawBlank`) instead of being rejected or re-flipping the current buffer. A flip the command
  processor decoded counts as pending (`gcQueueNum`, included in `flipPendingNum`) until the flip queue owns it
  (`VideoOutFlipPending`); a guest that waits for `IsFlipPending() == 0`, flips blank and unregisters its buffers no
  longer races a flip still in its command stream.
- **Quiesce before the first submission.** A guest munmap of GPU-mapped memory quiesces the GPU; a command
  processor that never recorded has nothing to complete and no longer creates command buffers before the render
  context exists.
- **Malloc replacement initializer.** A Construct title declares `initialize` as a bare `ret`, so rax still holds
  the callback address; libc publishes the allocator regardless of the initializer's return value. The duplicate
  table scan and second `initialize` call carried by the libc runtime port were removed.
- **`v_nop`.** VOP1 `v_nop` carries no operands (its encoding fields are ignored), and the NGG passthrough proof
  treats it as an inert scheduling hint like `S_NOP`.
- **NGG front scalar spills.** A `v_writelane`/`v_readlane` pair with a constant lane that only moves a scalar
  through one VGPR lane is lowered to a private scalar slot; the fused-front body proof now carries the scalar's
  taint through that slot instead of refusing it as a lane exchange. Run-time lane indices stay refused.
- **Ported from `feature/reviewed-main`:** guest `.eh_frame` registration (`d0cdd035`), the libc runtime and C++
  exception helpers (`65edd72c`: `log10`, `log2`, `difftime`, `__cxa_*`, `_Unwind_*`, ...), and the proven SMEM
  span for dynamically loaded storage descriptors plus the readable-memory copy of tiny static spans
  (`290862c0`, `32a1b60c`). 69 further commits of that branch conflict with the current tree and are not
  integrated (list in the handoff).

Open: the remaining Unity title's colour-grading pass loads its constant-buffer V# from extended user data dwords
40-43, which the guest never writes; they hold two 64-bit guest pointers (deterministic across runs). The bind now
requests only the 208 proven bytes, but the base is not mapped, so the strict run stops (12-71 s). The September
binary binds the same slot because the address was readable in its memory layout. Intermittently the same title
also stalls at boot (main thread after `RequestThreadContext`, another thread in `KernelWaitEventFlag`). A
Haxe title shows a flickering element while walking and breaking objects (user report, no root cause yet).

### Reference workload runs d58-d96: stable 266 s, no fatal, still black (2026-10-04)

Scope: this section records what changed between the vertex-front proof below and run d96, what the strict run
now does, and the two open items. It is not a compatibility claim: every capture of the longest runs
(d92, d96; 10 frames each, frames 23-808) scores `entropy=0`, one quantized colour (RGB 0,0,0), so no menu,
input or gameplay is accepted. Host: Intel Xe (experimental KMD), Vulkan 1.4, Native resolution, Silent printf.

What the run does now (d92/d96): no fatal for the whole 265 s window; the guest reaches `StartLevel title`,
`Continue: worldmap` and `LoadLevelResources worldmap`; about 13.5k draws, 60.8k dispatches and 807 flips
(p50 frame 247 ms, about 4 FPS); runtime events only `gfx_stencil_frontier` and `low_entropy`.

Fixes in this chain, each with a focused red->green test (122 suites, 1,808 tests, 0 failures; boundary and table
gates pass):

- **Fused vertex front, body proof.** Scalar loads, wave-uniform branches, lane-bit selects and vertex fetch after the
  width-neutral prologue (`ShaderNggFrontBody`). A MUBUF address holds one VGPR per enabled part (`idxen` plus
  `offen`); the decoder's operand width over-counts `offen` alone. An SDWA compare or VOP2 with whole-dword
  selects and no sub-dword modifiers is the plain instruction (`ShaderComputeWaveSdwa*` predicates); any other
  select stays refused.
- **Fragment mask flow** (`ShaderFragmentMaskFlow`). Flow-sensitive mask provenance now runs on every pixel program
  (the whole-program pass alone cannot account for the WQM widen/restore write of EXEC). It handles divergent
  blocks, the kill pattern with an inert dead tail (now also the dedicated `exp null` valid-mask export, not only
  the MRT-null spelling) and loops: states are iterated to a fixpoint over back edges; a loop whose exit or repeat is
  decided by EXEC or a non-uniform VCC marks every scalar its body writes as diverged once control leaves it
  (lanes that stopped early ran a different iteration count on the host subgroup than on the guest wave);
  implicit-derivative fetches are refused inside such a loop. The refusal reason now names the flow's own
  construct and pc instead of the monotone pass's register-reuse artefact.
- **Sampled/storage aliasing.** `Gen5PickSampleSurfaceAliases` only accepts a surface that holds every mip level and
  array layer the T# views. Gen5 colour storage images (tilings 0, 5, 9, 27; 2D; several levels) are a true mip backing
  with every level the extent allows (`StorageTextureUsesMipBacking`, `StorageTextureMipBackingLevels`), one
  single-level view per level, and descriptors that differ only in BASE_LEVEL are the same object. Captured
  descriptors address level 6 of a resource whose T# says MAX_MIP = 5 (a downsample loop; the 480x270 linear
  chain's allocation of 0x151300 bytes holds exactly six levels plus padding, so the store lands in the slack after
  the allocation on the hardware). The image therefore carries the whole chain rather than failing or clamping the
  store onto a real level. Tile 9 is a different layout on Gen4, so the colour case is Gen5 only.
- **Diagnostics** (bounded, opt-in): the alias refusal, the storage-view refusal and `!create_all_the_same` now
  print the backing image and every overlapping parent with its parameters; the paired-wave compute refusal and the
  pixel transport refusal share `ShaderDumpGuestProgram` (`KYTY_TRANSPORT_DUMP`).

Superseded on 2026-10-14 (see "Robot platformer: exposure fill and DCC metadata clears"): the black screen was the
composite multiplying the scene by a zero exposure, under an accumulated UI layer; the skipped merged ES/GS
programs are particle emitters. Earlier reading, kept for the record: the skipped-draw report names a geometry
state the renderer does not model: GE stages `0x00002030` (merged ES/GS), ES and GS-back programs bound, legacy GS
zero, `max_vert=0x48`, `out_prim=2`, `ge_ngg=0x46`, `max_out=0xd8`, primitive/vertex group sizes 3 and 0x18
(`ShouldSkipUnsupportedGeShader` models only the `0x02002000` passthrough). The decoded GS-back program (540
instructions) is not a culling pass: it is a procedural geometry program that zero-fills LDS, barriers, fetches
through `idxen` buffer loads, computes transformed positions in repeated unrolled blocks guarded by `v_cmpx`/
`s_cbranch_execz`, writes vertices and packed primitive indices to LDS with a running count, and then exports.
The draw census after present 200 (20 unique pixel programs) contains no draw with depth testing enabled, which
is consistent with all mesh geometry being omitted. Implementing this stage is a separate feature (an NGG
geometry-shader execution model: workgroup-wide LDS, barrier, emit counters, host primitive assembly from the
emitted indices); the skip must not become fatal before that exists.

Open items:

1. **Merged ES/GS NGG geometry** (above). Suggested direction: model the merged stage as one host workgroup per guest
   subgroup that runs ES then GS with real LDS and barriers and writes an index/vertex buffer the host then draws,
   proven against the recorded GS-back listing; record each unmodelled instruction class rather than skipping it.
2. **Timing-dependent compute refusal.** One run (d95) stopped at 249 s with `paired-wave dispatch admission
   unsupported: mode=0x41 pc=0x5a8 reason=instruction DsWriteB32 is outside the paired compute-wave admission set;
   native-equivalence rejected pc=0x570 reason=lane-conditional branch is not a forward region`. Two later runs
   (d92, d96) completed the window without reaching it, so it depends on how far the slow world-map load gets. The
   program was not captured; the refusal now writes `cs_<checksum>.txt/.bin` when `KYTY_TRANSPORT_DUMP` names an
   existing directory, so the next occurrence is classifiable. It is the compute analogue of the loop support added
   to the fragment flow: a lane-conditional loop around an LDS write.

Known approximations and debt recorded by this chain:

- A colour mip-backed storage image seeds only level 0 from guest memory for the tiled layouts (no level layout is
  implemented for tiles 5, 9, 27) and is not re-seeded from a newer render target each frame, unlike a sampled
  `Texture` built from a surface parent (which is reclaimed when the parent is newer). A per-frame refresh is still
  needed for a chain whose level 0 is a render target.
- `ShaderParseMUBUF.cpp:53` sizes the vaddr operand from the raw field and ignores `idxen`; `SBarrier` is still the
  placeholder for roughly 659 unimplemented parser sites; the host has no demote-to-helper discard (only `OpKill`).
- The fragment flow merges taint by OR, so a register that is a mask on one path and a number on another is reported
  as a mask (a numeric use is refused, an EXEC assignment is accepted); it is sound for refusals but imprecise.
- `KYTY_DUMP_VIDEOOUT` writes present-source PNGs only for 8-bit and FP16 formats; the reference workload presents a
  10-bit format, so that dump (and the paired `KYTY_DUMP_RT`) produces nothing there.

### NGG passthrough front width neutrality (2026-10-03, CPU verified; strict run advances)

The first draw's vertex stage (GE stages `0x02002000`, an ES program fused with the
GS prologue, guest Wave64) used to stop at native-wave admission: the prologue writes
EXEC, so `ShaderUsesNativeWaveState` classified it `ExactSubgroup` and the Intel host
(subgroup range 8..32) has no 64-lane subgroup. The refusal was correct, not a GPU
fault. Two routes were open: an exact lane map for graphics (as `Paired64On32` is for
compute) or a proof that the prologue is width-neutral. The second is implemented.

`ShaderProveNggFrontLaneLocal` (`ShaderNggFront.h`) quantifies the existing
count-specific analyzer `ShaderAnalyzeNggPassthrough` over **every** launch a wave of
the guest width can carry: all ES vertex counts and all GS primitive counts in
`1..W` (W = 32 or 64). Per launch the analyzer proves the allocation payload, that
EXEC is exactly the low-lane mask of the relevant count at every retained
instruction, that `exp prim` forwards the hardware primitive unmodified, and that the
retained vertex instructions read no wave-derived scalar/EXEC/VCC and exchange nothing
between lanes. A constant mask correct for one launch (a plausible wrong shape) is
refused. `ShaderApplyNggFrontProof` then upgrades `ExactSubgroup` to `LaneLocal`
only when the decoded GE kind is `NggPassthrough` with a known word and no unknown
bits; the merged ES/GS form (`0x00002030`), legacy VS, partial or missing stage state
keep the generic verdict. The verdict is computed once per immutable cached program
and width (`ShaderNggFrontVerdict` inside `ShaderVertexProgram`), not per draw.

The host executes the scalar prologue for real, so the generator's initial `s3` was
made part of the proof: it is now `kShaderNggFusedFrontWaveInfo` (one ES vertex, one GS
primitive, `0x0101`), a launch inside the proved set (`static_assert`ed), replacing
the former `1` (zero primitives, which the analyzer rightly treats as invalid). The
constant is named `%uint_0x00000101` in the module.

What this does not establish: the renderer contract that the host assembles
primitives itself (the proof needs the decoded `NggPassthrough` shape and does not
replace a topology/launch proof); the Layer output contract the analyzer still reports
(`requires_layer_output_contract`); any program with branches, continuations, LDS,
memory or lane operations (all refused); floating-point exactness of the retained
vertex ALU on the host. Decoding gaps also fail closed: unimplemented SOP2 opcodes are
decoded as `SBarrier` placeholders (`ShaderParseSOP2.cpp` lines 64, 159, 171, 181,
185, and 659 "treated as" sites across the parsers), which no proof accepts but which
are silently wrong elsewhere; they remain unfixed debt.

Evidence: red->green `EmulatorNggFront` (7 tests; with the upgrade disabled the
admission test fails and the six refusal/plumbing tests pass), `EmulatorNggPassthroughProof`
(18), `EmulatorShaderExport` (15, now pins the proved `s3`), 120 separately executed CPU
suites: 1,768 passes, 0 failures. Boundary and table gates pass. Strict run d57 of the
reference workload: the native-wave refusal of d56 (2.5 s) is gone, inferred from the
shader dump order (d56 stops after the vertex and pixel modules; d57 goes on to compile
two more compute shaders); no present was observed. The real program's module stores
`%uint_0x00000101` into `s3` and keeps both 64-bit `s_lshr_b64 exec` writes. The run
ends after 2.4 s at a **new** frontier, a compute shader that writes a 3D storage image
(`ShaderSpirvImage.cpp:743`, `unsupported storage image declaration: ... 3D writable
images require a 3D storage declaration`; the storage bank is 2D-only, see
`ShaderPlanStorageImages`). No frame was presented; this is not gameplay.

Suggested direction for the next frontier: a typed 3D storage bank end to end
(descriptor plan and layout, `OpTypeImage ... 3D ... Storage` declaration and 3D
`ImageStore` coordinates, a 3D storage `VkImageView`, and barriers for volume
layouts). Its RED test is the existing rejection, so write the generator test first.

### Writable 3D images, linear volumes, fragment mask flow and the full vertex front (2026-10-03, CPU verified; strict run stalls)

Four blockers in a row stopped the reference workload after the vertex-width proof above. Each was
fixed at its cause, with a red test first (the admission or shape test fails with the change disabled,
its refusal tests keep passing). Strict runs d59-d65; each one got further than the last (2.6 s, 12 s,
29 s, then 67 s without a fatal).

1. **Writable 3D images** (`ShaderSpirvImage.cpp`, `ShaderSpirvGenerator.cpp`). `ShaderPlanStorageImages`
   rejected every 3D writable descriptor. The writable bank now has one shape per module (2D, 2D array
   or 3D, `ShaderStorageImagePlan::three_dimensional`); mixing shapes is refused with
   "separate shape banks"; packed-mip stores and the R32 atomic alias stay 2D only. The declaration is
   `OpTypeImage ... 3D ... 2 <format>` and `image_store` writes a three-component bounds-checked
   coordinate; a 2D-coordinate instruction on a 3D descriptor is refused. The sampled 2D types keep their
   own dimension. The resource side (3D image and view in `StorageTexture.cpp`, `VIEW_3D`) already existed.
2. **Linear volumes** (`Gen5TextureVolumeLayout`, now `Gen5GetVolumeTextureLayout`). SW mode 0 is admitted
   for any non-block-compressed element size, one mip: `linear_size = pitch * height * depth * bytes`,
   allocation rounded up to 256 bytes (T# base-address granularity), upload is a checked copy. The T# stores
   no slice stride for a linear surface, so this is the tight convention shared with the 2D linear
   estimate (`TileGetTextureSize2`), not a measured padding; only a GPU-side consumer has exercised it.
   Standard 4KB stays 4-byte only; every other mode and any mip chain is still refused.
3. **Fragment mask flow** (`ShaderFragmentMaskFlow`). A Wave64 pixel shader of the form
   `s_mov_b64 S, exec; s_wqm_b64 exec, exec; ... s_cselect_b64 vcc, exec, 0; v_cndmask; s_mov_b64 exec, S`
   (with the descriptor registers reused as `S` after a scalar buffer load wrote `vcc_lo` numerically) was
   refused because the whole-program mask proof is monotone: once a register ever holds EXEC it is a mask
   everywhere. The new proof is flow sensitive for straight-line programs (a numeric overwrite ends the
   taint, SCC is cleared only by a compare, a select under a non-mask SCC is mask algebra, EXEC accepts only
   masks) and validates the `save / s_wqm / restore` bracket (save area intact, no export or memory write
   inside, no memory write after the restore so helper lanes stay unobservable). It only runs when the
   monotone proof refuses, so earlier verdicts cannot change.
4. **Full vertex fronts** (`ShaderNggFrontBody`, `ShaderAnalyzeNggPassthroughPrologue`). The second draw's
   vertex program has the same wave-count prologue followed by scalar descriptor loads, a wave-uniform
   `s_cbranch_vccnz`, `s_cselect_b64 ..., exec, 0` feeding `v_cndmask`, and vertex fetch. The exact
   analyzer now has a prologue mode that stops where EXEC becomes the vertex-count mask and reports which
   scalar words still depend on wave-level data; the body is proved once by a forward flow walk (launch data
   never reaches a vector instruction, scalar-load address, branch condition or EXEC; mask words are lane
   bits; VCC branches need a comparison of wave-uniform values; forward branches only; vector sources
   defined on every path; no stores, atomics, LDS or lane exchange). `s3` is overwritten by a descriptor load
   in that program, which is why the kill sets matter. Both real vertex programs are proved at Wave32 and
   Wave64.
5. **Texture shape authority** (`ShaderGen5InstructionShapeAppliesToType`). A compute kernel read a plain
   2D T# (1920x1080 R11G11B10F, SW mode 27, depth 1) with an `image_load` whose DIM is 3D; the descriptor
   adopted the volume shape and then had no layout. The texture unit addresses by the T# type, so a volume
   DIM no longer turns a non-3D resource into a volume (the array-shaped reinterpretations that tests pin
   are unchanged). This assumes the third coordinate is zero or ignored for the 2D resource.

Diagnostics added: the volume-layout fatal now names the T# type, base array, usage, whether the shape came
from an instruction and the descriptor register; the protected launcher sets `KYTY_TRANSPORT_DUMP` for the
fragment-transport refusal (the decoded pixel program is written under `programs/`).

Evidence: 121 separately executed CPU suites, 1,789 passes, 0 failures; `EmulatorNggFront` (12),
`EmulatorFragmentMaskFlow` (9), `EmulatorGraphicsResources` (34), `EmulatorGraphicsState` shape test; boundary and
table gates pass. Run d65 (binary built from this tree): no fatal for 67 s, 7 draws, 33 dispatches, 70
submits, then the observer stops it because GPU work no longer advances for more than 15 s. Frame counter 1,
no present, phase `booting`; three bounded `gfx_storage_frontier` events (`r=metadata_only`, unknown address
source) and one `frame_stalled` warning. The threads snapshot lists 32 started guest threads and sync-wait
tracking was off, so the wait site is not yet identified. **This is a stall, not an accepted run.**

Debt recorded, not fixed: unimplemented SOP2 opcodes decode as `SBarrier` placeholders
(`ShaderParseSOP2.cpp` lines 64, 159, 171, 181, 185; 659 "treated as" sites across the parsers). The body
analyzer and the pure analyzer refuse them, but other paths treat them as barriers silently.

### First-draw metadata contract (2026-10-03, observation only)

Further bounded observations identify the rejected draw's color-control mode as
fast-clear elimination, with CMASK enabled and DCC disabled. AMD's documented
operation materializes the retained clear color into blocks marked fast-cleared;
the pixel shader's ordinary export is not that operation's attachment result.
The same-context host image is newly created with an undefined layout. An
existing expanded-image no-op therefore does not cover this instance.

The source-bound input integration build passes 119 isolated CPU suites
(1,742 passes and the same 16 environmental skips) and required CPU/static
checks. A subsequent read-only observation records a compute producer whose
production-decoded descriptor starts at the observed CMASK address, spans
4 KiB, and supplies four zero fill words. Its bounded program parse contains
the indexed vector store. Function returns and copied bytes do not establish
GPU completion, a whole-plane CMASK extent/equation, or CPU/GPU coherence.
The first observation stops at the diagnostic output quota before the original
admission failure; preserve it as partial evidence. Compact serialization keeps
the same quota and permits a later complete failure replay. It observes the
same 4 KiB requested and returned storage-buffer range and descriptor range,
then the normal post-dispatch marker for four workgroups of 64 invocations.
The following flush-event parser returns record GPU memory barriers, not host
fence completion. The same-context image is still undefined, and the replay
ends after 10.11 seconds at the unchanged native-wave refusal. Native input and
all six SPIR-V cache containers match the earlier complete-program build.
Subsequent audio edits remain outside this captured binary's qualification.

The inspected public GFX10 CMASK equation takes the **FMASK** swizzle as its
input. Its accepted swizzles do not include the observed zero FMASK swizzle;
the separate color tile mode cannot replace that input. The bound 4 KiB view
therefore still supplies no complete addressed-plane proof. This is a gap in
applying the public PC layout contract to the captured configuration, not a
claim that the console's configuration is invalid.
The mask prerequisite is separate: copied register inputs confirm shader mask
3 and target mask F. The strongest GFX10-applicable public FCE guidance programs
both CB masks to F; no mask-3 exception or mask-independent preservation rule
was established. A two-word export is not a four-channel CB-mask certificate,
and the GFX11 packed-export exception cannot supply the missing GFX10 rule.

The on-chip control observations decode to equal vertex/primitive capacities
of 64, output capacity 64 and amplification 1. These constrain a conditional
single-productive-wave argument but still do not exclude consequential
zero-role waves in the captured launch mode. A count-specific CPU proof and
conditional ordinary rectangle interior coverage are insufficient to admit
the actual metadata operation. Real surface ownership/materialization,
primitive execution and enabled observers remain prerequisites. Gameplay,
action response, the playable gate and sustained runs remain unestablished.
The zero-allocation alternative also remains unproved: a zero-role wave in a
productive subgroup does not own a separate empty allocation. Published
zero-output compiler paths suppress exports or perform explicit completion
operations; they do not establish that an extra zero request followed by
full-mask exports is inert in this mode.

The pinned Intel subgroup tutorial demonstrates subgroup partitioning. The
examined current and release vkd3d-proton implementations reject a required
64-lane width on a device whose maximum is 32; native-width heuristics and
size-query workarounds do not implement the guest's numeric EXEC/NGG contract.
Neither source authorizes replacing strict admission with a forced proof bit.

The portability correction in
`ShaderComputeWaveVulkan.cpp:ShaderSelectNativeSubgroup` and
`ShaderWave32FragmentNativeLaneExchangeSupported` accounts for the fact that absence of
`ALLOW_VARYING_SUBGROUP_SIZE` does not fix SPIR-V 1.6 stages to the device default.
The Vulkan [varying-size contract](https://github.com/KhronosGroup/Vulkan-Docs/blob/e4e53e4b31e13eeaee1ad99fb940aa72b2ec1b14/chapters/shaders.adoc#L1940-L1974)
allows varying sizes for these modules. A width-sensitive stage on a multi-size
device needs an explicitly supported required size, or a proof covering every
legal size. Final admission now reads the actual checked module version;
fragment preflight conservatively includes variation. Core property queries
retain genuine bounds even without the promoted extension name. A covered
multi-size selection explicitly means an unspecified width, not physical size
zero. Exact native64 and the composed neutral32/exact64 case remain valid;
exact64 on max32 still refuses. Paired-compute lowering is unchanged.

All six retained containers have actual SPIR-V 1.6 headers. The unchanged
independent 40-case CPU fixture goes from 22 failures to 40 passes against the
corrected production archive. Two owned modules differing only in the 1.5/1.6
version word validate under Vulkan 1.4. Ten focused regression tests and the
five selected adjacent suites pass. Full-tree qualification passes all 119
isolated suites (1,753 passes, the same 16 exact environmental skips) and the
required CPU/static checks, including the preserved input and audio changes.
Fresh binary-bound helper controls and the Vulkan 1.4 device preflight pass.
The strict replay reaches the unchanged guest64 vertex refusal after 10.33
seconds, with actual module version 1.6 and queried host range 8..32. Native
input, the 4 KiB storage-view observation and all six cache containers retain
their earlier identities. No new narrower NGG proof or gameplay result follows;
the playable runtime gate remains unavailable with exit 2.

Independent source review finds no new blocking issue in the eight-file
correction. It records a pre-existing conditional defect at
`GraphicsRenderPipeline.cpp:412-420`: defining `KYTY_FINAL` without
`ASSERT_ENABLED` makes `EXIT_IF` omit evaluation of the side-effecting subgroup
attachment call. That configuration is not established for the qualified run.
Before supporting it, move required attachment mutations outside disabled
assertions and retain an unconditional failure path.

### Vertex-wave admission frontier (2026-10-03, passthrough program captured)

The complete-program integration build binds Gen5 vertex-wave analysis, module identity and
both renderer compilation paths to one immutable complete program. A copied
front/back program is acquired under a mapping-generation lease; its stable
content fingerprint separates different continuations even when their front
checksum and wave classification agree. Map, continuation and debug changes
invalidate parsed-cache generations; already acquired draws retain their code.
Translator identity is 101; generated geometry remains 86.

The unchanged independent fixture changes from three failures to five passes:
back-only lane and numeric-EXEC reads require guest64, distinct neutral backs
compile separately, and neutral/nonterminal controls retain their behavior.
All 20 owned-program regressions pass, including emission after guest storage
release with Vulkan 1.4 module validation. The four-target build passes 119
separately executed CPU suites: 1,737 passes and the same 16 exact environmental
skips from 1,753 cases. The new suite and four adjacent suites also pass two
same-process iterations; required CPU/static gates pass.

Fresh binary-bound layouts, host-main checks and the exact Vulkan 1.4/Wayland
device preflight pass. The strict run ends after 3.68 seconds at the same vertex
guest64 admission refusal, before any observed present. Its complete shader,
draw, launch and output records match the preceding capture, including
known/written output primitive 3. All six cached SPIR-V payloads are unchanged
and validate offline; their cache keys use the new translator identity. The
captured vertex module's content fingerprint matches the separately observed
288-byte program. This is value agreement between observations, not atomic
guest-memory provenance. Gameplay, action response and the two sustained runs
remain unestablished.

The preceding qualified build corrects primitive-state output ownership and passes
118 separately executed CPU suites: 1,717 passes and 16 exact environmental
skips from 1,733 cases. Its 13 new producer controls and four adjacent suites
pass two same-process iterations. The unchanged independent 21-case fixture
now passes every case, compared with 19 failures before the correction.
Required CPU/static gates and both freshly bound native-device preflights pass.
Two strict observations end after about 4.03 seconds at the same vertex guest64
admission refusal, before any observed present. The first draw now has output
primitive **3**, explicitly known and written. The second trace observes the
actual indirect-register callback write 3 and the later report read 3 from the
same Context. Constructor-return to command-memory copying and complete
last-writer attribution remain unobserved; optimized-out data stays unknown.
The captured program and all six shader-cache containers match the earlier
build. These short observations do not establish gameplay or a completed
five-minute window.

The preceding extended-snapshot build passes 117 separately executed CPU
suites: 1,704 passes and 16 explicit environmental skips from 1,720 cases.
Its 18 conditional NGG-proof controls and 16 native-wave snapshot controls pass
two same-process iterations, as do adjacent GE/packet/state suites. Required
CPU/static gates, fresh binary layouts and the exact-device preflight pass.
The extended strict capture again stops at vertex wave admission after about
3.25 seconds. Its program bytes and all retained shader-cache containers match
the earlier capture. These are input observations, not an elapsed gameplay run.

Before the producer correction, four output words had explicit same-draw
known/written provenance:
VS output configuration `0x80`, position format `0x44`, output control
`0x01240000`, and GS output primitive **zero**. This disproves the hypothesis
that the reported zero was merely an unassigned reset field. The subsequent
producer correction below needs no hardware ignore-zero rule. The pixel-program
identity and
bounded effective interpolation/raster state are also retained; effective
state without assignment tracking is labeled separately from raw provenance.

The previous qualified diagnostic build passes 116 separately executed CPU suites:
1,684 passes and 16 explicit environmental skips from 1,700 enumerated cases.
All 14 native-wave snapshot controls pass two same-process iterations; adjacent
packet/state/admission/export repeat controls and required CPU/static gates
also pass. Its rebound production native32 probes compare 12,288 words with
observed fences and zero validation errors. Fresh binary-bound layouts and the
exact Vulkan 1.4/Wayland device preflight pass before the strict capture.

The first wave-sensitive vertex input is now captured: an NGG passthrough draw,
stage word `0x02002000`, three auto-indexed vertices with input primitive type 7, one draw
instance, zero user-SGPR count and zero LDS. Its 288-byte mapped program is
complete; both GS-back and legacy GS addresses are explicitly zero. The six
tracked raw registers are known and written. This is an earlier draw than the
retained merged `0x2030` failure, whose actual program pair remains uncaptured.
Bounded production parsing yields 26 instructions, all satisfying lowering
preconditions. The three cached Vulkan modules are byte-identical to the
previous qualified run. Strict execution still refuses vertex guest64 on the
host without native64, before any observed present.

The program derives allocation and packed EXEC masks from the system word's
vertex/primitive counts, forwards the primitive-input VGPR, then transforms
vertex/instance inputs without memory or lane-exchange instructions. That is a
candidate for an explicit independent-vertex proof, not permission to classify
all such vertex programs as lane-local. The current `s3=1` seed, physical host
lane numbering and empty primitive-export emitter do not implement its launch
contract. A proof must account for allocation, primitive forwarding, actual
logical lanes, every live scalar/mask dependency and any unprovided system bits.
Execution strategy and gameplay acceptance remain pending.

AMD's public PAL topology contract distinguishes type 7 (`2D_RECTANGLE`, the
bounding box of an arbitrary 2D triangle) from type 17 (`RECTLIST`, three corners
of an axis-aligned rectangle). Both consume three vertices. The existing shared
triangle-strip conversion is not evidence that their raster contracts are
identical; arbitrary type-7 inputs also refute a generic affine fourth-corner
rule. Keep the three guest vertices and any four-vertex host completion
separate. Same-draw output-control and raster
state were missing from the first snapshot; the qualified additive diagnostic
extension supplies the evidence above. An output or reset value without a recorded
assignment must not be promoted to a known raw register word.
The generation-qualified AMD LLPC enum assigns GFX10+ output 3 to `RECT_2D`
and output 4 to `RECTLIST`; the local secondary GFX10 register table instead
names output 3 `RECTLIST`. Preserve that discrepancy rather than relying on a
same-spelled name. The primary references are PAL
[`palCmdBuffer.h`](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L87-L107)
and LLPC
[`AbiMetadata.h`](https://github.com/GPUOpen-Drivers/llpc/blob/40cb8d95ad8d6f7f1652e3fd47d39667594cce08/lgc/include/lgc/state/AbiMetadata.h#L895-L902).
The corrected output-mode selection is recorded below; exact raster
observables still require evidence before this class can be admitted.
The producer audit identified an unconditional metadata copy in
`GraphicsCreatePrimState` (`Graphics.cpp`). Three further captures of the same
qualified executable each record 31 complete constructor returns, including
13 calls for the captured vertex program with input type 7, no hull shader,
no GS owner, and output zero. The independent owned-memory controls reproduce
19 failures out of 21; the two explicit-GS controls pass. These observations
establish the constructor defect, while final consumer/last-writer attribution
remains unresolved. The qualified source correction
selects explicit GS output when `GS_EN` is set, HS output for tessellation
without GS, and input-derived output otherwise (including 7→3 and 17→4 on
GFX10+). A zero output is not a sentinel: explicit point output survives.
Fixed guest metadata and requested output ranges are validated before
publication; unsupported ownership/input combinations refuse explicitly.
This correction alone does not establish vertex-wave or raster equivalence.
`ShaderNggPassthroughProof` is a CPU-only, count-specific prerequisite. Its
18 tests pass, and it accepts the complete captured 26-instruction program
under explicit guest64/three-vertex/one-primitive premises, retaining the 12
original vertex ALU/export/end instructions. It tracks known scalar bits without choosing unknown launch
values, checks allocation/primitive identity and every live vertex mask, and
returns original indices for scalar-free independent vertex instructions. Its
result does not authorize a renderer strategy: the caller still owes complete
program binding, real launch inputs, primitive assembly and output-state proof.
The current native admission path does not consume it. This result is
conditional on those input counts and identities; it does not establish the
actual initialization or the raster contract.

A partition-independent proof cannot simply replace the missing launch
contract. The captured mask construction has a full active mask at count zero;
its positive-count algebra does not establish which waves hardware can issue.
Allocation payload equality also does not prove allocation ownership or
retirement. Public AMD descriptions scope `THDS_PER_SUBGRP` to fast launch
([PAL ABI](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/g_palPipelineAbiMetadata.h#L910-L915))
and describe NGG partial-ES-wave flushing
([PAL pipeline](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9GraphicsPipeline.cpp#L1259-L1311)), but do not supply the required
population/allocation rule for the captured ordinary passthrough mode. A
same-mode authoritative contract or fully bound target-hardware observation
is still needed. The available Intel host can validate an eventual translated
implementation, but cannot observe native AMD-initialized launch registers.
Primitive connectivity, type-7 sample coverage and enabled observers remain
separate requirements; strict admission stays in place.

The EXP producer correction passes all 15 focused tests, including 73 complete
Vulkan 1.4 module cases, and the full source-bound build: 115 CPU suites,
1,670 passes and 16 explicit environmental skips from 1,686 enumerated cases.
The required CPU/static gates pass. Both production native32 numerical probes
pass all 12,288 compared words with observed fences and zero validation errors.
Fresh host-main layouts and exact Vulkan 1.4/Wayland device preflights pass.

Strict execution advances past the EXP precondition to
`GraphicsRenderPipeline.cpp:CreatePipelineInternal`'s native-wave admission:
a vertex guest64 program has `ExactSubgroup` proof and requested width 64,
while the tested host provides subgroup32 and cannot select native64. The
reported refusal PC zero is an unset diagnostic field, not an identified
offending instruction. The vertex analyzer currently returns after its broad
wave-state predicate; its more precise mask-use analysis is Pixel-only.

The retained cache contains one vertex candidate whose identity has the same
guest/proof/requested widths. Its generated source includes the existing GS
prolog's fixed `s3=1` and scalar shifts writing architectural EXEC. All three
unique retained modules validate offline. This does not prove width neutrality
or a correct merged launch. The bounded input capture above supplies the actual
first draw counts and mapped program; merged-GE capture and its launch contract
remain separate work. Native-wave admission stays strict, with gameplay and
action-response acceptance pending.

The corrected program owner retains a front view for the existing resource
ABI and a complete view for wave analysis and compilation. The parsed cache
retains at most 256 entries and 64 MiB of accounted IR; larger/debug-bearing
owners bypass retention. Code replacement must participate in the mapping
lifecycle: direct in-place writes without a new generation are not detected on
cache hits. Resource metadata remains shallow, and front/back copying is not
atomic against unsynchronized guest stores. The native report remains a
separate live observation rather than an owner snapshot.

Two compiler leads from the reference follow-up do not demonstrate new defects
in the inspected Kyty paths. The B32/B64 SAVEEXEC templates read their source
words before saving EXEC to the destination; the scalar mask adapter preserves
that ordering and packed representation. The generic paired emitter explicitly
emits both banks and refuses a failed rewrite, rather than skipping the high
bank after an unsuccessful low-bank value lookup. These are source-level
exclusions of specific external defects, not new GPU or launch-equivalence
results.

A source-proven integration gap remains in `DepthStencilCopy.cpp:408-475,617-639`:
the specialized guest-geometry copy path creates a guest vertex stage without
the normal pipeline's native-wave selection, subgroup-capability checks or
required-size attachment. A wave-sensitive VS on that type-4/type-6 route needs
shared admission/attachment logic and a focused reproducer before qualification.
This route is distinct from the captured type-7 draw; immutable program
ownership alone does not close it.

The successful VS probe write in `Shader.cpp` passes a null generated-source pointer to
`ShaderProbeWrite`, recreating the same file and losing its earlier generated
text. Preserve phase evidence with the bounded diagnostic writer; the ordinary
file shader log retains both phases but lacks an internal session quota.

A bounded opt-in native input report now covers the earlier admission seam,
with actual draw arguments and separate front/back program copies; the qualified
capture is recorded above. Its control-character fixture exposed a reader limitation in
`Core/JsonReader.cpp:Json::parse_string`: every `\u` escape is rejected,
including correctly escaped control characters. That fixture uses the existing
Unicode-capable agent parser to validate the unchanged full report and decoded
fields. General Core JSON Unicode support remains separate debt; the native
writer must continue emitting valid JSON escapes.
Same-process GraphicsPackets repetition also exposed process-lifetime
`ShaderInit` in `AcceptsComputeShaderWithoutWorkgroupId`'s parent fixture.
Initialization, mapping, parse assertions and cleanup now belong to its
isolated child; the runtime's one-initialization guard remains intact.

### Export operand-tail correction (2026-10-03, CPU and strict advancement verified)

The source-bound revision-99 build completes 114 separately executed CPU
suites: 1,655 passes and 16 explicit environmental skips from 1,671 enumerated
cases. GraphicsState, SaveData, diagnostic persistence and translation-cache
suites also pass two same-process iterations. Diff, emulator boundaries,
graphics tables, four Python gate suites and three CPU diagnostics-integration
modes pass. The native agent is included in the four-target build identity.
The actual playable gate remains unexecuted.

Both production native32 numerical probes were rebound to those final archives
and pass all 12,288 compared words together, with two observed fences, no
exhaustion and zero validation errors. Fresh binary-bound layouts, host-main
and exact Vulkan 1.4/Wayland device preflights pass. These certify that build,
not the subsequent parser correction.

The first strict guest run then stops before GE capture, at
`ShaderSpirvGenerator.cpp:GenerateSource`'s lowering precondition for a
one-source primitive export. `ShaderParseEXP.cpp:shader_parse_exp` initially
decodes four VGPR operands, then reduces `src_num` without clearing the unused
tail. A sanitized probe linked to the retained production archives reproduces
the refusal; its four-source position-export control passes. The correction
belongs at the parser's final operand construction, preserving the selected
active sources and strict admission. Translator identity 100 separates this
correction from earlier cached modules. The verified results and next strict
failure are recorded above. Merged-GE execution, gameplay/action response and
the two sustained gameplay runs remain unverified.

Related EXP debt remains in `ShaderSpirvVector.cpp:Recompile_Exp`: parameter
exports with EN other than `0xf` are parsed as four physical slots, but their
emitter writes the full vector without applying EN. Add evidenced component
accumulation or precise unsupported admission with value controls. The existing
PRIM path only checks `gs_prolog` and emits no primitive operation; its fixed
prolog initialization and complete-module validation cannot prove merged-GE
launch, allocation or primitive output. The operand-tail correction does not
resolve either contract.

### Integrated contract corrections (2026-10-02, intermediate evidence)

The earlier 1,412-test result below belongs to its earlier binary. Subsequent
arithmetic, architectural mask, resource, lifecycle, publication and I/O changes
use translator identity 99. All three affected targets now link. The latest
completed build passed 1,655 tests across 114 separately executed suites, with
16 explicit platform/runtime skips out of 1,671 enumerated cases. Boundary and
graphics-table gates and the four runner/capture/matrix/playable script suites
also pass. These results bind that completed build, not later fixture edits.
Newly linked suites must appear in the executable's actual test enumeration.
Death tests default to re-execution (`threadsafe`) because earlier threaded
suites can leave locks unsafe to inherit through a plain fork.

Four bounded historical READFIRSTLANE consumer controls have now executed on a
full 32-lane compute subgroup with Vulkan 1.4 validation. Each compares 6,144
words against an independent integer oracle. The packed-bit consumer passes
with packed EXEC and fails with lane-local predicates; the nonzero-predicate
consumer has the opposite results. Both failures retain residual EXEC and
explicitly report exhaustion after exactly 64 shader iterations (963 mismatched
words and 192 exhausted lanes per failing case). All four submissions reach
observed fence completion with zero validation errors; the matching controls
have no mismatched words. The allocator count/capacity guard stays at 75,000,
and the replay stays within the two-GiB, 45-second resource envelope.

Those four historical controls preserve only the consumer instructions. Separate
native32 probes now pass for current production scalar-written and CMPX-derived
EXEC: each compares all 6,144 readback words with zero mismatches or exhaustion,
an observed fence, and zero Vulkan validation errors. The complete generator
supplies mask production, save/restore, comparison, selection and depletion; the
external test scaffold still supplies the fixed 64-iteration loop. Complete
native32/native64 fixture modules assemble and validate, but native subgroup64
is unavailable on the tested host. This proves neither wave64-on32 fragment
equivalence nor the cause of the retained game device loss. Presented gameplay,
reproducible action response and the required sustained runs remain unverified.

An initial CMPX32 replay exposed a harness error: its nonempty test included
the ignored upper EXEC word, and its oracle assumed the scalar-written numeric
high word was zero. All 576 mismatches were saved/restored high words or the
empty wave's count/hash. The corrected harness follows RDNA2 §3.3: wave32
activity ignores upper EXEC while scalar numeric copies remain separately
checked. The original failure is retained; no output fields were discarded.

The GE decoder now preserves exact register words and assignment provenance.
The skipped-state report keeps ES, GS-back and legacy pseudo-stage identities
separate; unknown raw words serialize as null, not an observed zero. An opt-in,
metadata-bounded snapshot can retain the actual skipped programs. Classification
of merged ES+GS is not implementation of its launch, shared LDS, allocation or
primitive-emission contract.

Publication debt remains explicit in `Graphics/Objects/Label.cpp`,
`LabelStoragePublication::Acquire/Copy`, and `Objects/GpuMemoryCreate.cpp`:
an ordered storage writer recorded before label completion cannot reacquire
the newly completed publication while its backing is still in use. Closing
that case needs per-command writer coverage and versioned staging/backing
ownership; queue order alone does not identify each changed byte. Clipped or
unaligned synchronization words remain excluded. Whole-word compare/exchange
protects observed intervening CPU stores, but cannot detect a complete CPU ABA
that finishes before publication without an observed mismatch. The page-cache
copy counters also still include excluded bytes. These limitations are not
resolved by the new quiescent upload/publication controls.

The first integrated CPU batch passes 107 lifecycle/I/O tests, including all
31 descriptor/stdio tests, five AMPR read/error tests, and 46 dirty-tracking
tests. Fault-report persistence and the formerly unlinked suites also pass
same-directory repeats. The shader batch exposes shared admission rejecting
canonical parsed instructions and duplicate image-type declarations; those
failures were corrected and rerun. The GE decoder's 14 controls, resource
suite's 28, mode-propagation suite's ten, native-wave admission suite's 16,
architectural-mask suite's 16 and lane-execution suite's four now pass.
All 12 arithmetic tests pass, including 291 complete-module validation calls
and value-level controls; all 17 LDS tests pass. Wider suite execution exposed
an unsampled MIMG decoder source-tail defect and obsolete mask fixture
expectations. Their producer/fixture corrections pass, including all 26 MIMG
tests and the complete GraphicsPackets suite. Subsequent fixture-isolation
verification and the first native failure are recorded above.

The SaveData mount death-test fixture also removed empty ancestors through
`Core::File::DeleteDirectories`, deleting the temporary root needed by a later
translation-cache test. It now uses a parent-owned unique directory inherited
by the child, retains the parent-side persistence assertion, and removes only
its owned tree. Both SaveData and translation-cache suites pass afterward.
Same-process repeated suite execution then exposed a second fixture issue:
the CommonDialog initialization contract is process-lifetime, while four
graphics resource fixtures retained heap registrations or reused already
completed submission IDs. The dialog probe now runs in a fresh child; graphics
fixtures use monotonic mock submissions and scoped owned-range cleanup.
Four adjacent topology fixtures also freed host arrays before unregistering
their GPU heaps; later allocator address reuse exposed that dangling state.
Scoped teardown now unregisters those owned ranges before releasing the arrays,
and the complete repeat batch passes. GoogleTest's `result="skipped"` XML attribute must be counted as
an omission even when no nested `<skipped>` element exists.

Scoped stdio-buffer leases now cover the registered HLE/native bridge and
normal `CloseAll`; all 31 descriptor controls, including 14 new buffer/export/
capacity cases, pass on the integrated binary. Remaining boundaries: raw global flushes in Core's
`DbgAssert.cpp`/`DbgAssert.h`, test-framework fatal paths and skipped teardown
can bypass those leases. Review exit ordering or restrict generic diagnostics
to host-owned streams without adding a Core-to-Emulator dependency. Windows
CRT append-state mutation still returns an unsupported error; alternate-libc
buffer retention and Windows/macOS execution remain unverified. `Errno.cpp`
still has process-global guest errno storage, and the reserved standard
descriptor policies remain incomplete POSIX redirection/dup/fstat behavior.

Image-module integration now reuses identical sampled/storage image types and
preserves their descriptor bindings. Remaining numeric dispatch debt is in
`ShaderSpirvImage.cpp`: mixed float/uint sampled bindings with array/3D resinfo
paths, and mixed-numeric gather, still lack the per-descriptor numeric branch.
The homogeneous uint shape controls do not cover those cases. Add exact
numeric dispatch and value controls, or precise unsupported admission before
emission; type-uniqueness validation alone cannot prove the sampled view.

Samplerless MIMG decoding now omits the unused sampler operand at its producer,
and admits only zero SSAMP pending evidence for nonzero values. Broader encoding
debt remains in `ShaderParseMIMG.cpp`: its UNRM guard rejects one even on store/
atomic forms whose ISA contract specifies it; A16/D16 and unidentified word-zero
bit 6 / word-one bits 29:26 lack complete validation. The new operand-tail
controls cover the currently admitted flag subset. Complete the evidenced
per-opcode flag/type contract before widening these forms.

Raw-register coverage remains incomplete in `GraphicsRunJmpTables.cpp` and
`GraphicsRunOpParsers.cpp`: individual VS RSRC1 writes and direct PS RSRC1
`SET_SH_REG` starts lack table entries. The generic handler warns about a null
parser and then calls it. Packed setup/update and the existing PS indirect
path carry the recorded overflow mode, but do not cover those missing packet
forms. Add bounded shared full-word decoders and packet controls, or terminate
with an explicit unsupported diagnostic before the null call.

### Pad, keyboard and mouse record contracts (2026-10-03, CPU verified, not gameplay)

An independent native-homebrew write-up of the input libraries (GPL, console
firmware 6.02; a lead, not a dependency) agrees field by field with our 120-byte
`ScePadData`, the 96-byte keyboard record and the 40-byte mouse record, and
differs in four places that the HLE now follows; another emulator's `scePad`
sources (read locally, behavior only) corroborate the first three.

- `ScePadControllerInformation` is 28 bytes: the structure ends in `reserve[8]`,
  which ours lacked, so the trailing bytes of the guest buffer were never
  written and `scePadGetControllerInformation` did not clear the structure at all.
  The extended record embeds it, which makes it exactly the 0x40 bytes its
  comment already claimed.
- `scePadReadState` and `scePadRead` filled individual fields only, so padding,
  the extension unit and the reserve kept stale guest bytes, and the native
  `connected` (0x4C) reads as a 32-bit boolean. Both now clear each record first.
  They also continued after a null pointer, a bad handle or a capacity outside
  1-64 (the guest's pointer was then dereferenced); they now return `0x80920001` or `0x80920003` like the rest of the pad entry points,
  and so do `scePadSetVibration`, `scePadSetLightBar`, `scePadResetLightBar`,
  `scePadSetMotionSensorState` and `scePadGetControllerInformation`.
- Acceleration is in G, not m/s^2. The virtual pad reported 9.8 from
  `scePadReadState` and 0 from `scePadRead`; both now report 1.0 on the axis the
  HLE already used. The axis is not verified against a physical DualSense, and a
  real sensor path (host gyro/accelerometer scaled to G) is still unimplemented.
- `sceMouseRead` sized a `memset` from the guest's capacity with no bound; it is
  now limited to 64 records (the documented loss-resistant drain) like the
  keyboard's 16.

`sceAudioOutOpen` reports an unknown format as `0x80260007` (the host layer
already refused it, but the guest saw a full port table), and
`sceAudioOutSetVolume` and `sceAudioOutGetPortState` return `0x80260004` for a
null pointer instead of dereferencing it.

Layout assertions pin all four records at compile time. The unit tests fill the
guest buffers with a sentinel and prove that each read writes exactly one complete
zeroed record and nothing beyond it; they fail against the previous
`Controller.cpp` (50, 50 and 19 assertions, and a crash on the null paths).

### Debt resolution and compute-wave contracts (2026-10-02, CPU verified, not gameplay)

The specific debt in that working-tree review is closed, and the 56 recorded
failures (55 compute-wave tests and one GETPC test) are explained and fixed. Nothing here ran on a
GPU or against the guest. Evidence: 91 unit suites run one at a time under a
resource scope (1,412 tests, 0 failures, 8 environmental skips), `git diff
--check`, `check_emulator_boundaries.py`, `check_graphics_tables.py` (13 tables)
and `test_kyty_playable_regression.py` (its guest-root case skips honestly).
Translator 98 invalidates every cached module because admission changed.

Added fallback markers. `git diff -U0 -- source` now adds no "condition ignored
(continuing)" line and no `TODO() check` comment. `EmitMovrelMove` and the
`V_LDEXP_F32` emitter return false for operands they cannot lower, and the
generator then refuses the shader (`shader emitter missing`, exit 65) instead of
emitting invalid SPIR-V; the `IMAGE_SAMPLE_LZ` dmask 0xb type checks are one
refusal. The decoder landmines they sat next to were real: SOPK `S_ADDK_I32`
(15) and `S_CMOVK_I32` (2) fell to "treated as SBarrier (continuing)" and were
silently dropped, while the undefined SOPK opcodes 17 and 20, the hardware-
register opcodes 18, 19 and 21, and SOPC opcode 16 (RDNA2 has no `S_SETVSKIP`)
could be mis-decoded. `S_ADDK_I32` now decodes as `S_ADD_I32 D, D, signext(imm)`
(signed-overflow SCC) and `S_CMOVK_I32` as `S_CMOV_B32` with an inline constant,
per the RDNA2 ISA SOPK table; the rest are `KYTY_UNKNOWN_OP()`.

Diagnostic dumps. `KYTY_TRANSPORT_DUMP` and `KYTY_PIPELINE_FAIL_DUMP` share
`DiagnosticDump.h`: the directory must already exist and is never created; the
file name is a fixed component of `[A-Za-z0-9._-]`, at most 96 characters and not
starting with `.`; the joined path must fit in 1,023 bytes without truncation;
the file is created exclusively (an existing file, symlink or FIFO fails); each
file is capped at 16 MiB and the process at 64 MiB, and a dump cut by a cap is
reported as truncated. `StorageTexture.cpp` prints the swizzle with a 64-bit cast
matching `PRIx64`.

Why the compute-wave tests failed. They were written for the per-opcode paired
allowlist that commit 704f4ad3 replaced, and they predate this working tree: the
layout and runtime sources and tests they exercise are unmodified against HEAD.
Each failure was classified by probing the real analysis and emitters, not by
loosening assertions:

- Rejection moved from analysis to emission. The "first unsupported
  instruction" sentinel was `S_GETPC_B64`, which the paired analysis now admits;
  the tests use `v_interp_p1_f32` (`0xc8000001`, pixel-only), which stays
  outside the compute set, and name it `VInterpP1F32`.
- Layout: a trailing partial wave is admitted (the missing lanes stay
  inactive), so the wave count is `ceil(invocations / 64)`. Runtime: when the
  paired layout is refused but the per-lane route is valid, the plan is
  `Supported` with `native_equivalence_required`; Gen5 `USE_THREAD_DIMENSIONS`
  converts counts with a checked ceiling division, so a zero local size is
  `InvalidLocalSize`.
- Classification: the specialized paths (banked ALU, scalar mask, scalar shift,
  scalar copy, ternary ALU) claim only their exact tuples; anything else falls
  to the generic per-lane lowering, so the tests assert what each path claims
  rather than `Unsupported`. `V_CNDMASK_B32` neg/abs are lowered in ISA order by
  `EmitCndmaskData` and stay on the banked path.
- Buffer loads: `ShaderAnalyzeComputeWaveVectorBufferLoad` and
  `ShaderAnalyzeComputeWaveScalarBufferLoad` remain the descriptor-provenance
  proof of the specialized synchronous route and still refuse unproven
  descriptors; `AnalyzePairedWaveCode` then falls back to the generic load. Both
  layers are asserted. The compare emitter evaluates both banks' predicates
  before storing the mask pair, so a mask destination that overlaps an SGPR
  source is lowered read-before-write instead of rejected.
- Control flow and barriers run through the block dispatcher
  (`ShaderSpirvBlockDispatch.cpp`), which resumes each guest wave at its own
  barrier phase, so branches, back edges and barriers in arms are admitted.
- Retired, not repaired. `ShaderAnalyzeComputeWaveLdsAccesses`,
  `ShaderAnalyzeComputeWaveLdsSafety` (both source files) and
  `ShaderAnalyzeComputeWaveWaitcnt` had no caller outside their tests after
  704f4ad3 (every admitted memory operation completes at its own PC and LDS
  accesses take the ordered generic path), and an unused `lds_safety` stub in
  `AnalyzePairedWaveCode` is gone. The exact-tuple predicates that
  are still used (`ShaderComputeWaveLdsInstructionSupported`,
  `ShaderComputeWaveIsExactWait`, `ShaderComputeWaveIsLgkmZeroOnlyWait`) stay.
  Their descriptions in the LDS paragraph below are history.

Independent of this list, four `EmulatorGraphicsPackets` tests were stale:
three asserted the pre-704f4ad3 DS identifier spelling (`%lds_byte_addr_0`, now
`%lds_byte_addr_0_0`), and the dynamic split storage-image test used no-ops where
descriptor flow analysis follows real `s_load_dwordx4` instructions and an
extended pointer; with real loads the dynamic descriptor shadows the static one.

New fail-closed checks found while doing this. Falling back to the generic path
would otherwise have admitted encodings the lowerings do not model, so
`ShaderComputeWaveGenericVectorSupported` now refuses a MUBUF access whose
preserved flags carry LDS (the data goes to LDS), TFE (an extra status VGPR) or
undefined encoding bits, and both it and the generic compare refuse SDWA control
words with a source select of 7 or reserved bits 22 and 30 (and bits 31:24 of a
one-source VOP1). Neg, abs, omod, clamp, `DST_U` with a DWORD destination and an
SGPR source stay admitted because the decoder folds them into operands. The
tests pin both sides.

Recorded defects (found, not fixed in this session):

1. Generic DS emitters do not bound the LDS index. `ShaderSpirvBuffer.cpp`,
   `LdsWordPointer` (line 2199) and the atomic and read2 emitters that follow it
   (`Recompile_DsAddU32_VaddrVdataOffset` 2245, `Recompile_DsAtomic_XXX_...` 2288,
   `Recompile_DsAtomicIncDec_VaddrOffset` 2332, `Recompile_DsRead2B32_...` 2396) build `OpAccessChain
   %lds` from a guest VGPR address. RDNA2 ISA 3.6.1 says an out-of-range LDS
   write is discarded and a read returns zero. Trigger: any `ds_*` with
   `(ADDR + offset) / 4 >= lds_dwords`, now including paired wave64 programs
   because 704f4ad3 removed the constant-address proof. Direction: compare the
   word index with the existing `lds_length` constant, skip the store or atomic
   and return zero; the ISA leaves a partially out-of-range multi-dword write
   undefined, so needs a numerical GPU probe, not a guess. The banked paired
   emitter already guards its accesses.
2. The decoders drop flags the native lowerings do not model.
   `ShaderParseMUBUF.cpp:29-32` (GLC, SLC, LDS, TFE) and
   `ShaderParseMTBUF.cpp:30-32` continue, and so do the SDWA select checks in
   `ShaderParseVOP1.cpp:41`, `ShaderParseVOP2.cpp:58,63` and
   `ShaderParseVOPC.cpp:39,41`. Paired admission now refuses LDS/TFE/undefined
   MUBUF bits and SDWA selects above 6; the non-paired native path still lowers
   them as if the flag were clear. Direction: have those emitters return false
   on `buffer_flags` LDS/TFE and on reserved SDWA fields, then measure which
   captured shaders change.
3. Not verified: the generic vector predicate does not inspect `dst.clamp` or
   `vop3_op_sel`, so an integer VOP3 with CLAMP (for example `V_ADD3_U32`) is
   admitted without evidence that its lowering saturates. Direction: a decoded
   case per affected opcode, then refuse or implement.
4. `AnalyzePairedWaveCode` consults `ShaderAnalyzeComputeWaveScalarBufferLoad`
   only when the generic scalar predicate refuses an `S_BUFFER_LOAD_DWORD`, so
   the specialized scalar-buffer route is effectively pre-empted for uniform
   operands. Direction: decide whether that route still earns its keep.
5. Inherited inventory at HEAD: 1,885 "ignored (continuing)" lines in 92 files, 573
   "treated as SBarrier (continuing)" lines in 13 files and 82 `TODO() check`
   comments in 3 files (the working tree has 1,877 after the SOPK fixes). They
   are not a list of bugs: triage by decode path reachable from captured
   shaders, and replace with an explicit decode or `KYTY_UNKNOWN_OP()` one file
   at a time with a regression per site, never with a mass rewrite.

### Shader float-mode bits (2026-10-02, partial implementation)

The RDNA2 MODE register (ISA 3.5) carries `FP_ROUND` (bits 3:0, single in [1:0],
double and half in [3:2]; 0 nearest even, 1 +infinity, 2 -infinity, 3 toward
zero), `FP_DENORM` (bits 7:4, same split; 0 flushes input and output denorms, 1
allows input only, 2 allows output only, 3 allows both), `DX10_CLAMP` (bit 8,
clamps NaN to zero), `IEEE` (bit 9, quiet and propagate signaling NaN) and the
sticky status bit `LOD_CLAMPED` (bit 10, set by a texture access whose LOD was
clamped). `S_ROUND_MODE` and `S_DENORM_MODE` (SOPP 36 and 37) set the first two
from an immediate.

`ShaderSpirvF16.h` now consumes the initial graphics controls and recorded
compute controls (`fp_mode_known`). Narrowing models all four static half
rounding modes; half arithmetic admits nearest-even with explicit denormal
and supported clamp handling, and rejects unsupported mode/modifier domains.
Initial FP16 overflow control and explicit provenance now reach compute,
VS/GS and PS metadata and cache identities. The integrated arithmetic suite
passes its numerical and complete-module controls; see the current frontier
for the separate remaining qualification blockers.

Other SPIR-V float operations generally still use host defaults. Outside these
explicit half contracts, a non-default mode can therefore produce incorrect
rounding or denormal behavior without a diagnostic. `FloatClampModifier`
applies the shader's `DX10_CLAMP` setting. `V_LDEXP_F32` is exact apart from results in
the denormal range, so only `FP_DENORM` can change it, and `IEEE` only quiets
signaling NaNs that `Ldexp` passes through. `LOD_CLAMPED` has no producer here
because the parser rejects the image `LWE` bit, the only way to read it;
`IMAGE_SAMPLE_LZ` fixes the level at zero regardless. This is one shader-wide gap
and the comment in `ShaderSpirvVector.cpp` points here instead of carrying a
per-instruction `TODO`. Dynamic `S_ROUND_MODE`, `S_DENORM_MODE` and `S_SETREG`
forms currently terminate in unsupported decoding. Direction: evidenced
decoding and dynamic MODE propagation with a case that programs each mode,
then host float-controls capability checks across AMD, NVIDIA and Intel.

### Arithmetic lowering and cached-module validation (2026-10-01, not gameplay)

Focused production-emitter replays expose separate scalar subtraction contracts:
`S_SUB_U32` must report unsigned borrow, while `S_SUB_I32` must compare sign
bits for signed overflow (zero is not a third sign). The previous lowering
fails 50 and 14 of 162 cases respectively. The corrected parser and emitters
pass both sets. Scalar and vector `BFM_B32` previously used `OpBitFieldInsert`
outside its defined domain when masked offset plus width exceeds 32. Shifted
low-bit masks preserve the ISA's truncation and pass 8,192 cases per form,
including inactive vector destinations. The four modules validate for Vulkan
1.4, and the two focused unit tests pass. Translator version 86 invalidates
the former module results; no guest visual improvement is established yet.

The former `Recompile_VCvtF16F32_SVdstSVsrc0` reversed the conversion direction
and failed 11 of 26 replay cases. The current shared half helper replaces that
implementation; compiled numerical and full-module validation now passes.
General device transcendental accuracy and
paired-wave execution still require separate validation.

Remaining arithmetic debt: signed scalar BFE identities are absent in
`ShaderParseSOP2.cpp`. The suspected unsigned 64-bit BFE zero-width helper
issue is excluded by source review: `FUNC_SHIFT_LEFT` and `FUNC_SHIFT_RIGHT`
branch around shifts at counts 0, 32 and 64, returning zero at 64. That review
is not a compiled value-test result. Packed integer
conversion/V16 templates in `ShaderSpirvVector.cpp` retain uint/float select
type mismatches. Correct each with bounded value and complete-module controls.

Offline Vulkan 1.4 validation of 104 retained translator-85 module entries
finds 17 invalid entries (some share identical payloads), including undefined
IDs, maximal-reconvergence CFG violations and selection-construct escapes.
This identifies invalid cached output, not the first module in the next run
or the cause of black output. Locate the live first failure with shader
validation enabled before another GPU execution of the invalid programs.
The existing control-flow limitation is recorded in
`docs/fragment-wave-execution.md`, Native fragment-wave tier. Preserve those
diagnostics rather than disabling validation to pass the frontier.

The next validated strict run stops before any presentation because its
vertex module is empty after SPIR-V validation rejects an undefined `%buf`.
`ShaderSpirvGenerator.cpp:2693`, `WriteFunctions`, emits legacy load helper
bodies from instruction types alone, even when all raw loads use guest-device
addresses and no storage-buffer interface is declared. The captured module
calls neither helper. Four synthetic raw-load widths with zero storage slots
reproduce the undefined ID; the corresponding one-slot controls validate.
Removing only the two uncalled helper bodies from the captured source makes
that exact source validate for Vulkan 1.4. This is source-level isolation,
not proof of guest pixels. Helper publication must require the backing
interface without removing any guest instruction or inventing a buffer.
The boot-time debugger state is explicitly unpaused and not minimized;
that early failure does not explain the older post-presentation pause stall.

Publishing legacy buffer helpers only with a declared storage interface now
passes eight Vulkan 1.4 fixtures and five focused tests. The next validated
strict run passes the former vertex failure and reaches 77 presents before
rejecting a pixel module. `ShaderSpirvControlFlow.cpp:467` redirects a diamond's
selection merge to its synthetic join only when an outer incoming edge exists.
`WriteLabel` and `Recompile_SBranch_Label` also use synthetic joins for standalone
diamonds, leaving their two-predecessor block undeclared as a merge. This violates
`SPV_KHR_maximal_reconvergence` section 2.11. Changing only the merge declaration
to the existing synthetic join makes the exact failed source validate. Align
selection merge ownership with branch and label ownership; retain maximal
reconvergence, both guest arms and their instructions. This is the new first
validated failure, not evidence that it explains the older black output.

The adjacent three-arm regression fixture also exposes a source-identity
collision: `ScJoinFindParent` and `ScJoinFindOwner` use zero for no source,
although a conditional at the first instruction has PC zero. Its nested link
therefore bypasses the actual parent and violates structured selection entry.
Represent absence separately from valid source PCs in both analysis and emission.
The invalid miniature modules are retained offline and have not been sent to
the GPU. This defect is a focused CFG control case, not a new live failure.

Both CFG corrections now pass six focused tests, five Vulkan 1.4 miniature
modules and 128 GPU branch cases. Translator 88's strict rerun validates the
previously rejected pixel module and reaches 850 presents. The next first
failure is a fragment input collision: a smooth parameter and a flat parameter
are declared at the same location and components. The guest uses both P1/P2
and `V_INTERP_MOV` selector 2; merging the inputs would lose their different
values. `ShaderPixelInterpolator.cpp:171` enables the existing geometry remap
only for raw per-vertex inputs, not for mixed qualifiers referencing the same
parameter export. Route that alias through distinct geometry output locations
while retaining the original export source and interpolation qualifiers.
Captures still fail the visual gate; presentation and the agent's phase name
do not establish menu, input or gameplay. The failing run is unpaused and not
minimized, with an empty flip queue; it does not explain the older pause stall.
Early captures show a readable opening caption. Their low entropy and one
zero midline-difference metric are not evidence of corrupted horizontal bands;
later captures are uniformly black. Do not diagnose tiling from that metric.

The mixed-qualifier route now uses the existing geometry interface to duplicate
the actual vertex parameter into distinct fragment locations. Identical views
still share their canonical input; distinct source exports retain the direct
path. Nine focused tests and six Vulkan 1.4 modules pass. A bounded offscreen
replay compares the production pixel/geometry pair with an independent direct
GLSL reference: six cases rotate the provoking vertex and vary clip W, checking
98,304 components with zero differences. All 24,576 covered pixels distinguish
the smooth and flat values, so the check detects collapsing the views. Translator
89 and the qualified-alias input identity invalidate the old pixel results.
The strict guest rerun validates the three formerly rejected modules and loads
the next scene's resources before rejecting another pixel module. Its single
empty diamond arm jumps directly to the raw guest continuation while
`ShaderSpirvControlFlow.cpp:486` declares the synthetic join as the merge.
Retargeting that one empty arm through the already-emitted synthetic merge
makes the exact full source validate for Vulkan 1.4. The merge and the guest
continuation have the same guest PC; no guest operations may be removed.
Align taken-edge ownership when the taken PC equals the chosen merge PC.
This is the next first failure, not evidence that the older black output or
pause stall is resolved. Focused fixtures still do not establish useful pixels.

Empty diamond arms now branch through their declared synthetic merge, retaining
the continuation at the same guest PC. A focused regression fails before this
change; seven focused tests and eight Vulkan 1.4 modules pass afterward. GPU
replays check both the ordinary and empty diamond over 128 cases each, with zero
differences in destination values or SCC. Translator 90 invalidates the former
CFG output. The bounded strict run validates that formerly rejected full module
and reaches a new fragment failure at 849 flips. It remains unpaused and not
minimized; the flip queue is empty. There is still no gameplay acceptance.

The new failure has three conditional branches to a discard tail. Branch
emission uses local fallthrough merges and separate real discard bodies, but
`ShaderSpirvScJoin.cpp:832` collects two orphan reconvergence merges for that
terminating destination. Their unreachable chain adds an extra predecessor to
the first real discard block, violating maximal reconvergence. Removing only
the two unreferenced synthetic blocks makes the exact source validate for
Vulkan 1.4; all original branches, guest operations and three `OpKill` bodies
are preserved. Exclude terminating discard destinations from merge ownership
instead of merging or omitting guest discard paths.

The discard-destination exclusion passes ten focused tests and six Vulkan 1.4
modules (one/two/three discard edges, complete replay shader and references).
An offscreen GPU comparison exercises each of the three original discard
branches and the non-discard path: 98,304 components match, with 12,288 pixels
discarded and 12,288 covered. Translator 91 invalidates the former orphan-join
output. The next bounded strict run validates all 113 captured modules,
including the former discard-tail failure, but stops with no present progress
after the later resource load. The final native capture times out; the agent
still serves events, threads and diagnostics. Opening-caption captures remain
readable and subsequent captures remain black; all nine visual gates fail and
no guest pad reads are observed. This is not image or gameplay acceptance.

At the last native snapshots, completed pipeline, dispatch and submit counts
stop advancing. The last generated source is a large compute module, but source
order does not prove the active host stack. The debugger stalls while resolving
its first state expression without a compilation-unit scope and the observer terminates the
service; no final pause/flip state is valid from that run. A compilation-unit-
qualified offline lookup obtains the real host-field offsets in half a second.
The next diagnostic run uses relocated PIE globals and bounded raw reads before
stacks, without target function calls or changed runtime behavior. Do not skip
waits or assign the stall to pause or GPU execution without the captured state.

The raw-state rerun confirms unpaused, not minimized and an empty flip queue
at the later stall, with 852 completed flips. Its 28 regenerated modules all
validate for Vulkan 1.4. Top frames are host waits, including one graphics-batch
decode-completion waiter and a file read; there is no active allocator top frame
in that snapshot. The bounded reverse-order stack dump ends before the early
processor threads, so it cannot identify the wait's producer yet. Prioritize
those host ownership stacks next. Pipeline-count asymmetry alone must not be
reported as proof of an allocator stall or a GPU hang. Captures still fail all
visual gates; input and gameplay remain unaccepted.

The host-priority rerun identifies the graphics worker waiting for the shared
render mutex in the EOP clock-counter recorder. Its coordinator waits for that
worker; the window thread waits for a flip request, not a pause event. No flip
is pending at the snapshot. Sixteen regenerated modules validate for Vulkan
1.4, and all nine image gates still fail. The owner has not yet been captured:
obtain the host mutex owner TID and its stack before assigning a lock cycle or
editing synchronization. A verified offline DWARF/glibc layout lookup supports
that bounded diagnostic; it is local evidence, not a portable runtime contract.

The owner-first snapshot identifies a live owner inside the system Intel
Vulkan driver, with a libc top frame. Automatic shared-library loading was
disabled, so the intervening stack is not a reliable call chain yet. This does
not prove a lock cycle or a GPU wait. The next run explicitly loads Vulkan
unwind information and captures the actual pending compute module, layout and
stage flags. The owner snapshot still has no pause or pending flip, and all
nine image gates fail. A debugger's explicit exit zero is not guest success.

Synchronization audit finds an additional baseline defect:
`GraphicsRunHelpers.cpp::SuspendedWaitTimeoutMs` defaults to one second, and
`GraphicsRun.cpp::WaitForSuspendedRuns` can set `skip_wait` and resume after an
unsatisfied guest wait. This contradicts the strict no-skip contract. The recent
diagnostic runs did not explicitly disable this default; activation has not
been evidenced, so do not assume either that a wait was skipped or that these
runs satisfy strict acceptance. Disable the timeout bypass explicitly in the
next diagnostic and replace that policy with a focused regression: a diagnostic
timeout may report or stop, never resume past an unsatisfied guest packet.

With that timeout explicitly disabled, an actual Vulkan-call capture now
identifies the render-mutex owner in compute-pipeline creation. Fifty-eight
calls have returned successfully and one remains pending. Its 2,848,336-byte
module, actual descriptor layout, required subgroup32 and full-subgroup flag
are retained offline. All 101 captured modules validate for Vulkan 1.4; nine
image gates still fail, with no pause or pending flip. The pending module has
not been submitted, so its compilation stall is not evidence of its GPU hang.
Separate two-GiB pipeline-only probes of the system and earlier byte-aware
driver each reach a sixty-second limit without completing. Both initially
request 66,673 allocator nodes; owner-checked probes stop before allocating
that large graph. No guest dispatches are queued by these probes. The earlier
compiler correction is therefore not sufficient evidence for this distinct
module's compilation capacity.

For that distinct module, an owner-checked pre-allocation probe finds 66,669
VGRFs and 107,212 backend instructions, with 1,317 peak interval register units.
Of the values at that peak, 484 span almost the complete dispatcher loop. Their
long intervals do not alone prove dead state. A bounded external native-CFG
experiment retains all 277 guest blocks exactly once and all calls, stores and
host barriers while replacing its three loops and forward branches. The module
validates, but both drivers still miss the sixty-second compilation gate; initial
peak pressure increases to 1,535 units. This excludes native structuring alone
as a sufficient initial-pressure fix for this input. No such runtime strategy
is activated. Trace the retained values' definitions and consumers next.

The backend walk now identifies all 647 values at that peak. The 484
dispatcher-wide values have 328 initial SIMD16 selects, 150 SIMD16 moves,
four scalar NoMask moves and two SIMD16 inversions. All have consumers, so
this census does not justify removing them as dead state. An external fold of
92 bind-time storage-slot metadata loads preserves other descriptor fields
and validates, but only changes interval pressure from 1,317 to 1,314 units.
Neither translation is activated; complete compilation and GPU preservation
of this fold remain unverified. Compare actual block live-in/live-out sets
with those conservative intervals before changing the allocator.

The actual block-boundary live sets still peak at 1,105 incoming and 1,111
outgoing register units. The external native CFG raises both to 1,267. Thus
this pressure is not merely overlapping intervals of mutually exclusive
blocks, and those counts do not authorize a different interference contract.

Further bounded experiments on this same input are also excluded as sufficient
compilation fixes. Broadcasting scalar reads leaves 1,314 peak register units;
113 provably unwritten loads represented as undefined leave 1,305. Direct
Workgroup materialization fits this input's 32 physical invocations and 2,048
LDS bytes: register-major storage uses 36,352 bytes and peaks at 594 units;
invocation-major storage uses 36,608 bytes and peaks at 538. The latter still
has 102,689 VGRFs and 159,296 instructions. Scalar broadcasts raise that peak to
605, native structuring to 1,174, block-local caching to 801, and volatile scalar
reads to 558 with 180,558 VGRFs. None of those large graphs was allocated or
dispatched. The shared peak includes 288 units of SLM send responses and 156
units of address additions, not only retained architectural registers.

Broadcasting the dispatcher PC reads reproduces the original backend exactly.
Real-CFG dead-state clears are valid but leave the original 1,317-unit peak with
undefined values and raise it to 1,953 with concrete definitions. Must-constant
SGPR folding leaves 1,314, and scalar-write broadcasts leave 1,318. A first
constant-folding harness incorrectly specialized the synthetic dispatcher PC;
its two-instruction output is rejected, not reported as a compilation success.
All these experiments remain outside production, without GPU equivalence or
complete compilation. Mutable descriptor fields must not be inferred constant.

A separate graph-phase measurement locates the next bottleneck more precisely:
the original graph has 66,673 nodes and 38,182,554 edges, with 557,177,088 bytes
of neighbor-list capacity. Construction takes about 1.54 seconds; initial
allocation retries fail in about 0.23 seconds each. The second spill attempts
to double node capacity from 66,688 to 133,376, quadrupling the dense adjacency
bitset. A bounded probe stops before that allocation. An external growth fix
covers bulk resize requests and caps spare nodes in large graphs; 96 spills
still leave 46 failed allocations and 68,877 nodes at 29.91 seconds. The input's
register pressure and compile-time barrier remain real.

Focused external allocator tests also reproduce three independent contracts:
64-bit triangular edge indices were truncated during set/clear; bulk resize
could reserve fewer nodes than requested; and reset removed edges but retained
the node's old interference weight and neighbor-list allocation. Owned external
corrections pass 35 contract checks and five upstream allocator tests. The
original pipeline nevertheless misses the same two-GiB, sixty-second gate with
each corrected driver. These are host-allocator findings, not evidence of GPU
preservation, guest integration, a fixed historical host freeze or playability.
No installed driver, guest module, resource ceiling or runtime policy is changed.

An external sparse-row membership experiment preserves the same initial node,
instruction and edge counts and passes 5,132 independent add/reset/migration,
wide-index, weight and coloring checks, in addition to the prior focused tests.
Its complete compile still reaches sixty seconds, with a 1.1-GiB memory peak and
about sixty CPU seconds. Lower reserved adjacency memory alone is not a usable
compile-time fix. No guest dispatch or GPU-preservation claim follows from those
CPU tests. Supported host spill batching at rate one also misses the same
sixty-second gate. A valid bounded phase trace records four failed non-spilling
attempts, followed by spill emission; 8,207 additional spill nodes trigger the
capacity guard at 26.41 seconds. The earlier phase probe lacked a valid owner
at an inlined breakpoint and is harness failure, not compiler evidence.

Further shared-storage experiments are also closed as sufficient fixes. A
case cache produces 79,387 VGRFs and 121,225 instructions with a 793-unit peak;
removing 1,512 dead entry restores leaves identical backend output. Conditional
masked stores validate but do not reach allocation within the 25-second
preflight. Branchless private-cell atomic masking passes 331,410 CPU bit checks
and structural cell-disjointness checks, but raises the backend to 127,485
VGRFs, 197,742 instructions and 1,188 peak units. None was dispatched.

A separate host-frontend census finds 937 NIR register declarations: 673 marked
divergent and 264 marked uniform, all scalar 32-bit. The existing frontend
allocates every declaration as a vector. An owned external candidate retains
scalar representation only for uniform single-component 32-bit declarations;
seven focused frontend contracts pass, including divergent, wide and vector
controls. On the original unchanged module it lowers the interval peak from
1,317 to 1,016 units, with 66,978 VGRFs and 105,684 instructions. Its compile
still stops before capacity 75,200 at 23.22 seconds, with 71,104 actual nodes
and 4,122 spill nodes. This is partial CPU evidence, not a completed pipeline,
GPU equivalence or an integrated fix. Tightening sparse graph growth to a
512-node quantum passes 22,552 new checks and the prior focused contracts, but
the unchanged shader still stops during spill emission at 24.73 seconds:
74,688 nodes, 7,706 spill nodes and 4,416 scratch bytes. This closes spare
capacity as a sufficient fix; do not shrink the quantum repeatedly to evade
the unchanged 75,000-node guard or increase the two-GiB ceiling. A next external
representation experiment tests invocation-private Function arrays with
runtime-bounded indices, retaining all architectural cells and guest behavior.
Its valid complete module has a 553-unit peak but 143,954 VGRFs and 268,707
instructions, so it too stops before graph allocation and remains undispatched.
The next host-compiler seam is reuse of instruction-local spill temporaries,
with unioned original-live interference and distinct simultaneous temporaries.

That external host candidate now completes the original unchanged pipeline.
Six focused reuse contracts pass; the bounded trace retains only nine spill
nodes but initially still reaches its 45-second deadline. A follow-up avoids
redundant hazard-edge setup on instructions untouched by a spill, while a full
rescan after each of three small spills adds no missing edges. Eight contracts
pass. The original Vulkan pipeline then completes in 42.589 seconds, with a
real successful compiler return and real target exit zero, under the same
two-GiB ceiling and 75,000-node guard; the journal reports a one-GiB peak.
No dispatch was queued. This establishes compilation capacity only, not GPU
preservation, the exact guest VkDevice, useful images or gameplay. The driver
remains external and uninstalled. Focused GPU preservation now passes: synthetic
uniform/divergent loops check all final cells at SIMD8/16/32 against a CPU
oracle, both with forced spills and with natural SIMD32 spilling. The corrected
ALU modules also pass with observed forced spilling. There are 69,704 checked
components across these focused runs, without differences. This does not prove
equivalence of the complete private module on arbitrary inputs.

An initialization-only capture stops before logical-device creation and records
the current runtime's actual features, extensions and queue inputs. Reproducing
that configuration exactly also completes the unchanged original pipeline in
42.474 seconds, with a real target exit zero and no dispatch. Window/render
debugger schemas are regenerated for the new wait-policy binary and checked at
host main before guest execution. The next run may now test guest integration
using the isolated external ICD, with unchanged strict shader/runtime settings,
the existing host guard and diagnostic compiler stops, not fabricated progress.
Menu, useful images, response to input, gameplay and host stability remain open.

The first protected integration of that driver and the new wait-policy binary
does not preserve the prior visual frontier: 21 compute pipelines complete and
command processing continues, but the frame counter stays at one with zero
presents until the unchanged 300-second runtime limit. The former pending
module is not reached in this run. Its measured cgroup peak is 2.4 GiB and host
available memory stays above 22 GiB; no resource guard or fatal guest error is
observed. Do not blame the later shader for this earlier failure. The observer
must capture native diagnostics and the raw wait owner for a stalled initial
frame as well as a stalled nonzero presentation count before the hard deadline.

The following raw initial-frame snapshot identifies the main thread in
`XIfEvent` inside SDL's `X11_ShowWindow`, before acquire or present. The render
mutex is free, the guest is unpaused and two flips are queued. This is the
already-documented X11/window-manager blocker below, not a new compiler or
guest-wait regression. The external runner had forced X11 and removed the
active Wayland session variables, preventing the existing native-Wayland
selection. Correct the runner environment, not guest waits or SDL mapping.
The corrected runner's initialization-only gate confirms the native Wayland
backend and an unchanged logical-device profile. Integration then completes the
first flip and captures a black frame; all 44 captured modules validate for
Vulkan 1.4 and its first 21 compute modules match the prior baseline exactly.
The observer stopped between completed compilation and continuing descriptor
work, so that stop does not establish a new hang. Confirm present progress and
recent command-processing work before a diagnostic stall stop; the resource,
compiler and total-runtime ceilings are unchanged. No visual acceptance follows.

The confirmed-progress native run reaches the later resource load with 840
presents and 58 completed compute calls. The unchanged former pending module
begins only about 290 seconds into the run, and the original 300-second cap
stops it ten seconds later. This is neither a compiler-deadline failure nor an
integrated completion. All 101 captured modules validate, all eight retained
image gates fail, and no guest pad reads are observed. No kernel error is
recorded in that run's journal window. Its cgroup peak is about 7.15 GiB, below
the 7.5-GiB stop threshold, with over 17 GiB host available memory. A bounded,
normally keyed driver cache is the next way to separate startup compilation
cost from guest execution without extending the runtime cap. Cache warmth must
be disclosed; cached pipeline success is still not GPU or gameplay proof.
The captured compute inputs now create successfully in 59 isolated calls and
one repeated original call, each with a real target exit zero. The original
cold call takes 42.730 seconds; a normally keyed disk-cache repeat takes 0.010
seconds with no allocator assignment callback. The isolated cache is about
7.1 MiB and bounded to 256 MiB. No dispatch or guest execution occurs in this
cache validation. A checksum-verified copy is used for the next protected guest
run; runtime waits, shaders, binary, driver and resource ceilings are unchanged.
That guest run observes all 59 compute creations complete, including the former
pending module's real successful return in 0.008 seconds. Presentation continues
past that call to at least 885; the native image at frame 886 remains uniform
black. All 101 modules validate and all nine image gates fail. Its resource
peak stays below the 7.5-GiB diagnostic-stop threshold, and no kernel error is
recorded in the correlated interval. No pad read is observed. The compiler
blocker has been passed, but pipeline success and global dispatch counts do not
prove that specific dispatch's completion or output correctness. Correlate its
pipeline, command buffer, successful queue submission and observed fence result
before returning to the upstream-color investigation. A new debugger helper
must keep a private namespace: overwriting the raw-schema `word` helper caused
an instrumentation-only failure before the guest's first main loop.
After helper isolation and focused preflights, the dispatch-specific trace
observes the unchanged pipeline record 390 groups, successfully submit the same
command buffer, and complete its fence on another host thread about 39 ms later.
The later native frame is still uniform black and no pad reads occur. At the
final snapshot the guest is unpaused, the window is visible and the render mutex
is free; that point-in-time snapshot is not a sustained guest-wait diagnosis.
Synchronization tracking is disabled, so its zero blocked count proves nothing.
The protected run records no correlated kernel error. This advances one real
compute dispatch through GPU completion, not arbitrary-input equivalence or
useful images. Next use the existing bounded native material/binding census at
the later scene to follow the color producer; keep strict Silent/Native/None.

That native late-scene census now records 21 distinct pixel draws, including
material targets, intermediate consumers and the final compositor. Binding
records alone do not prove their output values. One existing output-preserving
sparse sample probe then observes 259,200 elected compositor samples with
finite RGB zero and alpha one at its first scene-color sample. This locates
observed black before final color conversion, not a missing final draw or proof
that every texel in the source image is zero. All 101 ordinary and 102 diagnostic
modules respectively validate for Vulkan 1.4; all nine captures in each run fail
their image gate, and guest pad reads remain zero. The next discriminator is the
upstream consumer's scene-color sample, preserving its normal shader outputs,
waits and resource limits. No useful image or gameplay is accepted.

The upstream consumer now also completes its existing sparse observation:
104,576 elected samples read finite RGB zero and alpha one from an exact
storage-texture binding. Its 102 actual modules validate; all nine native image
gates still fail and no pad reads occur. This does not identify the producing
dispatch or exclude bad sample coordinates. The next boundary is the earlier
HDR render-target consumer, before the storage-texture composition chain.
Diagnostic shutdown exhausted its stop grace and was killed; a collected
unit's later default `Result=success` is not the original service result or a
normal guest exit. Keep the journal, live resource limits and observer outcome
as separate evidence.

The earlier HDR sample selector returns zero observations on both conditional
read sites; neither result proves black source pixels. A paired existing vertex
probe then observes three exports with no finite post-divide extrema and a live
GETPC-derived buffer base incorrectly reaching the descriptor-array resolver.
The fault is in register namespace comparison: Gen5 vertex user data is emitted
at API register + 8, but `ShaderHasUnboundBufferLoad` and the vector buffer
emitter compare its unshifted metadata register. Thus an inline descriptor at
s[0:3] collides with API binding zero actually emitted at s[8:11]. Correct both
classification and lowering with the same namespace contract, keeping negative
stream-binding sentinels non-registers; first add focused red tests. This is not
proof of correct HDR content or gameplay after that correction.

Two sanitized GETPC/raw-load regressions reproduce both sides of this namespace
error: the constructed s[0:3] descriptor incorrectly takes the array path, and
the actual bound s[8:11] descriptor incorrectly takes the guest-address path.
The shared binding predicate now applies the caller's user-data base and rejects
negative stream sentinels. Classification and all four raw vector load widths
use it, while pixel/compute retain base zero. Nine focused assembly tests pass,
including both red-to-green cases. Their original synthetic fixtures omitted
ENDPGM, so those early results do not prove Vulkan validation. After explicit
validation and fixture termination, the inline case validates but the bound-only
case exposes a separate undeclared s3 prolog variable; see below. Translator
revision 92 invalidates stale lowering. Fresh DWARF schemas, a raw-host-main
preflight and the exact unchanged native VkDevice contract pass before protected
guest integration; image/input/gameplay advancement is not yet verified.

The protected paired-probe integration confirms the repaired vertex contract:
three finite positions now have W=1, X/Y extrema -1..3 and Z=0, with varying
parameters and no erroneous descriptor-array resolver invocation. All 103
actual modules validate and the original compute dispatch still completes.
The selected alternate HDR sample remains unobserved; it is a conditional site,
not evidence of black texels. Eight available native captures fail the image
gate, but the last is before this delayed color checkpoint and the final
capture times out. No image advance is established. Observe the other HDR read
now that its input geometry is valid; retain unchanged outputs and guest waits.

The next protected repeat completes all 59 computes and validates all 103
actual modules, but stops at present 854 before the diagnostic threshold 855.
No probe event is delivered, so no inference about the selected HDR sample is
possible. Eight available captures fail the image gate and no pad read occurs.
The next bounded diagnostic starts observation at present 840 without changing
guest work or the 300-second ceiling. This is not gameplay acceptance.

Focused test defect (2026-10-02): `ShaderSpirvGenerator.cpp` emits an s3 store in
the fused-vertex prolog (line 1902), but `DetectVariables` (line 3277) only
declares its VGPR system inputs. A valid bound-buffer-only vertex fixture that
does not otherwise reference s3 therefore fails validation with an undefined
store ID. The inline fixture incidentally declares s3 through its descriptor,
and the observed guest prolog also references it; this is not the proven cause
of its remaining black image. The unconditional prolog destination is now
declared in the same variable-discovery branch. Both complete namespace fixtures
validate explicitly, all nine focused tests pass, and translator revision 93
invalidates old modules. Fresh binary-bound DWARF, raw-host-main and exact
Wayland/device preflights pass. The older atomic-only assembly fixture lacks ENDPGM
when run with global validation; do not describe its assembly pass as validation.

Earlier diagnostic selection now observes 65,280 elected finite HDR samples,
all RGBA zero, with three valid fullscreen positions. This locates the observed
black upstream of final composition without proving every source texel. Ten
native captures fail their gate. A subsequent paired vertex/MRT/attachment probe
of an indexed HDR producer observes 1,361 finite exports, all with nonpositive
W, zero color exports and an unchanged all-zero attachment. Depth/stencil is
disabled for that draw. Both runs validate all 103 actual Vulkan 1.4 modules and
complete the original compute submission; no pad read or gameplay gate passes.

New producer defect (2026-10-02): `ShaderParseUsage2` in `Shader.cpp` strips both
words of the vertex descriptor-table and attribute-table pointers from direct
SGPR bindings (lines 2024/2037). When native guest-address S_LOAD is used,
`ShaderSpirvBuffer.cpp:3467` reads those same shader pointer pairs before any
initialization; the observed fused-front prolog never stores s24..s27. Retain
the actual API pointer words in the direct-SGPR namespace and its existing +8
mapping. Prove this with sanitized parser-to-prolog regressions before changing
the producer; do not replace the loads with constants/no-ops, repair positions,
or blame MRT retention for a draw with no raster coverage.
Both sanitized pointer regressions fail on the original parser and pass after
retaining those words. An old SRT-span control also fails on the preserved
revision-93 binary: its ENDPGM-only fixture never reads the descriptor it expects
to be bound. Add a real descriptor consumer to that fixture, preserving the
existing liveness policy and its original span expectation. That complete
fixture and the other focused controls now pass: 18 tests, with explicit Vulkan
1.4 validation of the native pointer-load module. The minimal parser correction
retains actual table pointers as direct API words and uses the existing fused
register mapping; no load, wait, coordinate or color is replaced. Revision 94
invalidates cached metadata layouts. Fresh binary-bound DWARF and exact native
Wayland/device preflights pass. The same paired producer probe is now running
with this build. That repeat confirms stores of all four table-pointer words
before native S_LOAD, but the same producer still has 1,361 nonpositive-W
exports, no MRT coverage/color, and the same all-zero attachment hash. All 103
modules validate and all 59 compute creations plus the selected dispatch
complete; ten capture gates fail and no guest pad read occurs. Pointer retention
is a demonstrated contract correction, not a sufficient fix for this image.
The next observation must distinguish real scalar-buffer results from the later
vertex arithmetic, with original instructions and outputs retained. Do not
repeat the lost-pointer hypothesis or add a matrix/coordinate replacement.

The next output-preserving vertex diagnostic appends eight scalar-buffer load
sites to the existing opt-in clip-probe buffer. Each site elects one invocation,
retains the input descriptor/byte offset before the original instruction and
copies at most sixteen actual result words afterward. Unobserved sites stay
unclaimed; a fence precedes native event publication. Two initially red tests
now pass with explicit Vulkan 1.4 validation, proving instruction retention,
descriptor/destination overlap ordering, disabled normal emission and the site
cap. Sixteen focused tests and the probe layout/serialization integration pass.
Translator revision 95 and a separate diagnostic identity invalidate old layouts;
fresh binary-bound schemas and native Wayland/device preflights pass. The next
private producer run uses no attachment retries so its GPU words can be compared
with the same first draw's bounded CPU constant trace. No guest result, entire
wave equivalence, image or gameplay improvement is yet established.

That native repeat observes six scalar-buffer sites: all 73 result words match
the same first draw's bounded CPU constants bit for bit, including four complete
matrix loads. This excludes zero/corrupted scalar constants at those observed
sites, not the entire guest wave. All 103 modules validate; eight capture gates
still fail and pad reads remain zero. The draw's vertex layout reports failure
and zero attributes: unified format 66 (16_16_16_16_SNORM) is missing from
`VulkanVertexInputFormat.cpp` although the shared image catalog already maps it
to `VK_FORMAT_R16G16B16A16_SNORM`. A single missing format rejects the whole
interleaved stream, including UVs and normals. `GraphicsRenderPipeline.cpp:440`
then warns and proceeds with zero binding/attribute counts. Regression-test the
format and the complete packed layout before adding that evidenced table entry.
The warning-and-continue consumer is separate fail-closed debt; do not treat
zero attributes as a valid fallback or fabricate input data.

Two sanitized regressions now fail on the missing mapping and the complete
packed layout, then pass after adding only unified format 66 to the vertex
format table. It has four normalized floating-point components, shared with the
established image mapping; no unobserved attribute alias or conversion shader
is added. All 24 focused format/layout/fetch/register controls pass. Revision
96, a new preserved binary, freshly extracted schemas and native Wayland/exact
device preflight pass. A repeat of the same producer must establish whether
the layout now binds all three attributes and geometry/color improve. Image,
input and gameplay remain unaccepted until that native evidence exists.

The post-fix native diagnostic confirms one binding and all three packed
attributes, 1,361 finite vertex exports with positive W extrema, varying
parameters and 2,073,600 finite MRT0 observations with nonzero RGB. The same
73 scalar-load words still match the original CPU constants. All 103 modules
validate and the original compute dispatch completes. This advances the
producer's observed geometry/color, not the presented image: all nine scored
captures precede that draw, the final capture times out, and no pad read occurs.
The diagnostic stop's exit 1 is not a guest rendering failure or successful
completion. The next protected run removes all shader probes, binding traces,
shader dumps and the debugger while retaining Native/Silent/None, validation,
the verified external ICD and inherited bounded cache. Confirm a native image
and guest input before claiming this resolves the displayed black frame.

The uninstrumented Native/Silent/None run instead terminates with
`VK_ERROR_DEVICE_LOST` from `GraphicsRenderCommandBuffer.cpp:823` while waiting
on queue 8, slot 2. A correlated Xe coredump identifies a started but incomplete
render job; this is a real host-GPU failure, not the former compile wait or an
observer exit. Resource guards remain intact. Retained batch memory places
IPEHR at the PIPE_CONTROL immediately before ACTHD, rather than at ACTHD itself;
offline decoding must preserve that distinction. Identify the preceding work
and correlate it with native submission evidence before changing shaders or
driver policy. No image, input or stability acceptance follows from this run.
The bounded offline path reaches ACTHD through 816 commands and 27 indexed
draws, with no compute walker in the retained path. One predicated branch is
chosen statically and is not execution proof. The final draw has 7,110 indices
and two instances with the preceding draw's pixel kernel. Native checksum and
submission correlation remain necessary. All eight image gates fail; the 25
retained unique cache payloads validate, but persistence is not a complete live
module census. The native fault report's opt-in logger is suppressed by Silent
(`GraphicsRenderCommandBuffer.cpp:90/107`, `Log.cpp:302`), and the agent does not
expose its trail. A launch-only SHA-bound, call-free fault read is justified for
that missing diagnostic; it must resume the original fatal path and never
count debugger exits as guest success.

The bounded diagnostic repeat stops at frame 839 / present 838, before its
minimum-present 840 census gate. It observes neither a fault report nor the
target's original exit. All 101 captured modules validate and all eight native
image gates fail; pad reads remain zero. This is an inconclusive reproduction,
not evidence that device loss is fixed. Keep the existing runtime limits and
replace the missing Silent-mode fault evidence with an opt-in, bounded native
report rather than another debugger-dependent observation.

That diagnostic gap is now regression-tested: the old implementation loses
the requested report under Silent; the corrected native path writes only the
first device-loss report (eight attempts, 8 KiB maximum, exclusive file
creation) and publishes a native fatal event independently of logging. Fence
failures retain the caller's queue, slot, sequence and guest command context
without claiming submit return or fence completion. Fourteen focused CPU tests
pass, including disabled tracing, existing-file preservation and write failure;
the architecture boundary check and fresh host-main/Wayland/device preflights
pass. This changes observation only, not waits, shaders, submissions or the
original fatal path. The protected native repeat without GDB or shader probes
reproduces the real device loss at a queue-8 fence and successfully preserves
the native report. Its failing caller matches one of the eight attempts whose
submit returned `VK_SUCCESS`; that does not establish GPU completion. A
correlated Xe kernel journal records a render-engine reset and timed-out job.
The draw census contains 21 distinct shaders, but only their first occurrence,
not the failing later guest submission (`GraphicsRenderBind.cpp:170–203`).
Consequently neither the fault shader nor its draw is identified yet. Fifty-eight
logged modules and 25 unique persisted cache payloads validate; neither is a
complete live-module census. All eight image gates fail and pad reads remain
zero. The new coredump read times out with zero bytes; do not claim it was
captured or reuse the older dump as evidence for this job. Next collect bounded
recent draw identities correlated to the native submission, and preserve a new
dump through local authorization when available. No further GPU retry, shader
policy change, image/input/gameplay or cross-vendor acceptance follows from
this diagnostic advance.

That recent-identity diagnostic now exists (2026-10-02, CPU verified, no GPU
run yet). An opt-in ring of 1–256 records (default 64) is filled after each
guest `vkCmdDrawIndexed`, `vkCmdDraw` and `vkCmdDispatch`, not at the command
processor call, because `GraphicsRenderDrawIndex` can still return without
emitting. Records join on queue plus host sequence, which the submission
tracker assigns monotonically from one per queue, so slot reuse cannot merge
recordings. Recording, submit call, submit return and observed fence signal
remain separate stages; unobserved values are `null`. The report is written
once at the existing device-loss latch with exclusive creation; disabled,
existing-file and open-failure paths keep the original fault JSON and fatal
event. Twelve new focused tests and the fourteen existing submit-fault tests
pass; before implementation the test binary failed to link the report and
trace contracts. The runtime binary is rebuilt and versioned separately from
the d48 binary. A fresh binary-bound preflight is required before the single
protected repeat; until then the d48 fault shader and draw remain unknown.

Three defects found during that work, none shown to cause the d48 fault, are
now corrected with red-to-green tests:
- The submit trail's `frame` was `Gpu::Done()`'s count, which the Gen5 path
  never advances, so d48's `frame: 0` was a real but mislabeled counter.
  Fault report version 2 records `presented_frame` instead.
- Attempts without a host submission or command-processor context wrote
  `host_sequence`, `guest_submit` and PM4 fields as zero (d48's queue-9 entry).
  Explicit known flags now emit `null`; the Console mirror uses the same JSON.
- `ShouldSkipUnsupportedGeShader` and the invalid-vertex-shader check dropped
  guest draws with an Info log that Silent hides. Every skipped draw is now
  counted per reason; the first of each reason is a native `draw_skipped`
  Warn event, and the counts are in the recent-draw report. The skip itself is
  retained until a run shows whether it fires: converting it to a fatal
  failure without that evidence could only move the frontier backwards, and
  the correct fix is to implement the observed GE configuration.

Two neighboring unit expectations were stale rather than the code wrong:
`Gen5CodeAvailable...DirectStorageDescriptor` asserted the binding policy
before `342e7938` (direct user-SGPR slots need a storage consumer), and the
tile-9 BC1 assertion predated `b632b49e`, whose package upload was verified
against independently decoded source bytes. Both now assert those contracts;
all 315 `EmulatorGraphicsState` and detile tests pass. A third stale
expectation, the vertex-stage `S_GETPC` rejection test, predates 8d08fb38 and
now asserts that a bound program base resolves while a missing base or an
invalid destination is still rejected.

Lane-local EXEC in cross-lane lowering (2026-10-02; device loss not reproduced
in two runs; not gameplay). The recent-draw ring reproduces the real device
loss once more (d50, queue 8, PM4 op 16 at dword 11828, the same point as
d48) and names the work inside the failing submission: 28 recorded indexed
draws, 18 of them one vertex/pixel pair with different counts. The pixel
module has four backward guest branches, three of whose loop bodies contain
one of its three `V_READFIRSTLANE` sites (waterfall loops); the vertex
module's loops are bounded binary searches of the address table and always
terminate. Outside paired wave banks each
invocation is one guest lane and holds only its own EXEC bit in `exec_lo`
(0 or 1), with `exec_hi` kept at zero, as `V_CMPX`, `S_CBRANCH_EXECZ`, `S_WQM`
and the DPP lowering already assume. `V_READFIRSTLANE` and `V_MBCNT` instead
tested bit `1 << lane` of that pair, which can only ever select subgroup
lane 0. A waterfall loop then keeps re-reading a finished lane and cannot
terminate, matching a render-engine timeout. Both lowerings now test the
lane-local value; a literal or SGPR mask operand of `V_MBCNT` stays a
whole-wave mask indexed by lane. The high-half `V_MBCNT` lane offset also
used its lane value before defining it, which the new tests caught. Four
focused tests (real parser, production generator, real toolchain assembly)
fail on the old lowering with the contract exit code and pass afterwards.
Translator 97 invalidates every cached module.

Two protected runs with the corrected binary (d52, d54) complete the full
264-second observation window with no device loss; the earlier reproductions
lost the device at 214-221 seconds (d45, d48, d50). The correlated kernel
journals of d52 and d54 show no render reset, and the 58 logged modules of each
validate for Vulkan 1.4. Both exit zero only because
the observer stops at its time limit, not because the guest exited. This is
two runs on one Intel Xe host with the isolated experimental driver, not proof
of the cause: no isolated GPU replay of the waterfall loop exists yet, and
AMD and NVIDIA are unvalidated. Every capture is still black at about 3 FPS
in the loading phase, no pad read occurs, and no menu, input or gameplay is
accepted.

The skipped-draw counters expose the next frontier candidate. d50 skipped
4,458 guest draws and d54's first-occurrence report names the state:
`stages=0x00002030`, ES bound, legacy GS base zero, `max_vert=0x48`, `out_prim=0x2`,
`ge_ngg=0x46`, `max_out=0xd8`, primitive and vertex group sizes 3 and 0x18, first
seen at presented frame 1. `ShouldSkipUnsupportedGeShader` models only the
`0x02002000` NGG passthrough mask, so this configuration is an unimplemented
guest state, not a defect of the skip predicate. An omitted draw is a strict-
mode violation and a plausible cause of the black output, but no capture
links it to the missing image. Implement the observed GE/NGG configuration
from evidence (ISA, local captures, independent implementations compared, not
copied) before turning the skip into a fatal failure.

The original `gs=0` diagnostic inspected `gs_regs.data_addr`, the legacy
pseudo-stage field. GS program-register writes populate the separate
`gs_back_addr`. The original report therefore leaves the back-program identity
unknown. The version-2 skipped-draw report records both identities, GE registers
and shader resource state; it does not itself establish support for that state.

Working-tree review (2026-10-02, 83 entries, nothing staged): `git diff
--check` is clean and no added line carries a private path or title
identifier. That review listed eight added
"condition ignored (continuing)" checks and seven `TODO() check ...` comments
(VSKIP, SP_DENORM, SP_ROUND, DX10_CLAMP, IEEE, LOD_CLAMPED), the unbounded
`KYTY_TRANSPORT_DUMP` hook and 56 failing tests (55 compute-wave and one GETPC). All
three are closed; see "Debt resolution and compute-wave contracts" above. The
classifier predicates those tests exercise are no longer unchanged: the generic
vector and compare predicates gained fail-closed checks described there.

Reference-review boundary (2026-10-02): this work corrects Kyty's own contracts.
Other emulators supply behavioral leads to verify against local state, never
drop-in implementations or proof of the current failure's cause. Attachment
write masks and guest-wave versus host-subgroup handling require local
correlation; heuristic mip-stat reports and asynchronous draw omission do not
satisfy Kyty's strict contract. The local
`GraphicsRunOpParsers.cpp:1260` query handler currently consumes GET_LOD_STATS
without a guest-visible producer, and `GraphicsRenderBind.cpp:3095` ignores
mip-counter fields. No live correlation with this fault is established; retain
this debt for an actual packet/counter/sample/fence implementation, not invented
counts. All renderer corrections must remain capability-driven across AMD,
NVIDIA and Intel. Xe dump addresses and the isolated ANV experiment are local
diagnostics only; no vendor branch, host-address rule or cross-vendor validation
claim follows from them.

Separate recorded generator defect (2026-10-02): `ShaderSpirvGenerator.cpp`,
`WriteTypes`' non-compute `lds_addtid` template (around line 1280), passes raw
SPIR-V percent identifiers to `String8::FromPrintf` without escaping them.
The compiler reports consumed `%ld`/missing `%u` arguments. A non-compute
DS-addtid module needs a sanitized validation regression and literal-percent
escaping before that path can be trusted. The currently selected HDR producer
does not contain those operations, so this is not its demonstrated blocker.

The production wait policy now removes the bypass state and downstream resume
cursors entirely. Missing/invalid diagnostic deadlines disable it; a valid
opt-in deadline reports and terminates rather than executing past a pending
guest packet. Parsing also rejects nanosecond overflow and signed values.
Fifty-two focused synchronization/packet tests and seven process-environment
helper cases pass, and the owned Release executable builds. The subsequent
protected guest runs use that versioned executable with explicit zero timeout;
the older versioned binaries still require an explicit zero timeout. The new
integration does not establish correct wait producers, image, input or gameplay.

### Fragment wave compiler (2026-09-29, compiler and GPU replay verified)

An explicit compiler interface retains guest Pixel IR and metadata while
emitting a host compute module with paired wave64 execution on a full physical
subgroup32. Bounded inputs carry allocated lanes, initial EXEC, coverage,
initial vector words, native attributes and raw parameter triples. Optional
per-wave parameter state initializes the system SGPR identified by the PS
user-SGPR count. Its entry copy to M0 must preserve the parameter-cache
identity; later reuse of that SGPR after the copy is allowed.

EXP decoding now retains VM, DONE and COMPR. Compressed masks are restricted
to complete half pairs. VM updates pixel validity independently of color,
and every termination path must have VM and DONE exports. Helpers continue
through guest branches; several terminal blocks converge on one output
epilog without OpKill. Storage writes, LDS, atomics, barriers and unverified
interpolation pairs remain rejected.

The complete 2,300-instruction private pixel input generates a 286,670-word
module that passes assembly and validation for Vulkan 1.4. An original GPU
replay checks 19 modules, 160 cases and 356,862 output words against independent
references with zero differences, including both banks, WQM helpers, partial
exports, VM changes, multiple exits, multiple waves, excess dispatch groups
and truncated buffers. A masked packed conversion followed by EXEC restore
previously exported zero instead of retained VGPR bits (160 replay differences);
refreshing each bank's shadow before conversion eliminates those differences.
Fifteen malformed cases are rejected; a control case
permits SGPR reuse after its M0 copy. The existing paired-wave replay still
passes 320 cases and 42,064 register observations.

This interface does not enable the renderer strategy. Compact raster capture,
required host feature enablement, native MRT/stencil resolve and strict guest
execution of the new module remain pending. There is no new guest FPS or
gameplay claim from these results. Unit suites remain deferred as requested.

### Cold compute compilation capacity (2026-09-29, unresolved)

A strict Silent/Native retry of the compiler-interface build collects native
readiness, doctor, events, last error, threads, synchronization and performance
snapshots. It remains before the first present and reaches a cgroup OOM kill
after 269.732 seconds, with an 8 GiB service peak. Completed compute-pipeline
creation already accumulates 44.875 seconds in the early snapshot; the active
command-processor host stack remains in the Intel driver. Guest thread
liveness and an unsupported metadata diagnostic are not frame-rate evidence.
No warm FPS, new capture or gameplay result exists for this retry.

The 8 GiB bound is insufficient for this cold compile plus guest state. A
read-only debugger snapshot supplies driver instruction addresses, but stripped
symbols do not identify the specific compiler pass. Resolve those addresses
with the matching driver build before choosing another shader transformation.
The matching symbol and string sections now resolve the captured frames to
`update_pq_info`, `add_node_to_stack`, `ra_allocate`, `brw_assign_regs` and
`brw_allocate_registers`. The snapshot therefore locates work in register
allocation rather than identifying a NIR pass. This is one stack sample, not
a complete CPU profile. The prior local-load elimination experiment did not
remove the memory barrier; the next bounded experiment must constrain register
liveness while preserving guest control flow and enabled shared-memory limits.
Unit suites remain deferred, and no compatibility push is authorized yet.

An external checkpoint experiment moves eighty cross-block FP32 variables to
invocation-private cells in Workgroup memory and restores them at case entry.
It retains 181 dispatcher cases, uses 48,656 shared bytes against the queried
49,152-byte device limit, and dispatches no guest GPU work. Its module validates
for Vulkan 1.4. Three original branch/export GPU fixtures pass 24 cases and
52,224 output comparisons with no differences. The large pipeline-only replay
still reaches its 4 GiB cgroup OOM bound after 172.141 seconds. Thus
checkpointing these FP32 variables alone is insufficient; scalar state and the
remaining register-allocation pressure need measured evidence before another
transformation. No checkpoint policy or renderer behavior is shipped from this experiment.

Matching-build debugger probes now measure the allocator graph at entry and
terminate the pipeline-only child before further compilation. The original
module requests 35,442 nodes, 26,788,659 interference edges and 416,195,416 bytes
for adjacency lists and their triangular bitset. Five initial scheduling
attempts fail allocation; the first spill grows graph capacity from 35,456 to
70,912 nodes, quadrupling the bitset to 314,277,552 bytes. These measurements
locate an initial allocation barrier; they do not account for the complete
8 GiB guest peak or prove which later allocation causes the OOM.

The eighty-variable checkpoint increases the initial graph to 69,202 nodes and
1,002,890,076 adjacency bytes. Other external candidates retain the original
guest branches and validate for Vulkan 1.4, but do not reduce this initial
barrier:

| External candidate | Initial nodes | Initial interference edges |
| --- | ---: | ---: |
| CFG-liveness candidate using Undef | 35,442 | 26,788,659 |
| CFG-liveness candidate clearing locals | 35,674 | 27,334,053 |
| Ordered case traversal with barrier guards | 35,506 | 26,901,155 |
| Direct invocation-private Workgroup storage for eighty FP32 locals | 42,137 | 27,358,252 |
| Scalar register loads broadcast within each physical subgroup | 35,502 | 26,189,746 |
| Twelve grouped dispatcher loops | 35,470 | 27,300,973 |
| Local optimizer passes without global SSA promotion | 35,477 | 26,841,149 |

Direct Workgroup storage and scalar broadcasts also fail their first allocation
attempt. These are compiler diagnostics with zero queued guest dispatches;
they supply no pixel-equivalence or runtime-speed result. No such policy is
shipped. Before another structural change, isolate the contribution of repeated
address-translation searches and measure later spill growth. Merely decreasing
SPIR-V byte count or moving variables to shared memory is not a verified fix.
Broadcasting the barrier-generation exit or all dispatcher control values
produces the same initial graph as the original, with a failed first allocation.

Sixty bounded allocator entries expose later growth: the original reaches
72,594 nodes and a capacity of 141,824 after 24.662 debugger seconds. Its bitset
alone is 1,257,119,072 bytes. All 59 completed attempts fail; the probe stops
before allocating further. These debugger durations are not guest frame times.

A separate region experiment preserves the two generation barriers, admits
single-entry cyclic regions and emits direct acyclic paths with distinct
selection merges. The 181-block input has three phase entries, two cyclic
components of 19 and 116 blocks, and 46 acyclic regions. It emits 182 case
copies and validates for Vulkan 1.4. Three original fragment fixtures pass
52,224 GPU comparisons; a compute loop and a loop between two barriers pass
16 cases and 2,096 register comparisons, including partial EXEC and both banks.
The sixty-entry probe reaches 54,033 nodes rather than 72,594. Nevertheless,
the complete pipeline-only run still reaches its 4 GiB OOM bound after
167.393 seconds. Early graph reduction therefore does not establish a capacity
or speed fix, and this region policy is not shipped.

The captured CFG has seven natural loop headers. Both barrier cases are outside
the cyclic components. Five natural loops have one exit and contiguous blocks;
two nested loops have four backedges and two exits. The next structural
experiment must retain their exact exit state while removing internal
dispatchers. An external native-loop prototype now emits all seven loops,
validates for Vulkan 1.4, and passes the same 52,224 fragment and 2,096 compute
register comparisons. Its sixty-entry probe reaches 50,385 nodes, but the
complete compile again exhausts 4 GiB after 153.637 seconds. Removing the
internal dispatchers alone therefore does not resolve compilation capacity.

The two barrier resumes also form a finite chain of three phases with 5, 7 and
170 reachable blocks. A separate external prototype removes the enclosing
phase loop only when each phase has a unique barrier resume and the chain is
acyclic and bounded. It retains both generation barriers and continues retired
waves through empty later phases. Eight original two-wave cases pass 4,096
independent VGPR observations and 4,160 original/transformed word comparisons,
including four early retirements and different per-wave loop counts. The
fixture explicitly initializes observed SGPRs before early retirement; reading
unwritten SGPRs is not an equivalence contract. Nevertheless, its complete
pipeline-only run exhausts 4 GiB after 123.777 seconds. The finite-phase
transformation is therefore not a capacity fix and is not shipped.

Twelve matching-build spill selections from that module occupy classes with
6, 7 or 9 contiguous host registers and interfere with 15,078 to 32,350 nodes.
Those classes are host allocator metadata, not identified guest registers.
Further transformations must measure complete spill growth rather than treat
smaller initial graphs as acceptance. Direct invocation-private Workgroup
storage now also passes 52,224 fragment comparisons and the same 4,096 two-wave
VGPR and 4,160 equivalence-word checks. Its complete pipeline-only compile
nevertheless exhausts 4 GiB after 168.951 seconds. The shared-memory bound
allows eighty variables in that module; offloading them does not establish a
capacity fix. No Workgroup state policy is shipped.

A separate external Private-array candidate uses disjoint two-cell ranges for
102 direct cross-block FP32 locals, indexed by a stable invocation selector.
It requires 816 private bytes per invocation and does not consume Workgroup
memory or add a descriptor. Pointer escapes remain excluded. Its module
validates for Vulkan 1.4 and passes the same 52,224 fragment, 4,096 two-wave VGPR
and 4,160 equivalence-word comparisons. Its complete pipeline-only compile
still exhausts 4 GiB after 174.765 seconds. Its sixty-entry allocator probe
reaches 76,992 nodes, a capacity of 88,256, and 1,053,255,284 adjacency bytes.
Private FP32 storage alone is not a capacity fix and is not shipped.

The next external candidate also moves direct unsigned locals and does not
limit admission to locals referenced from several dispatcher cases. It selects
234 scalar locals (107 FP32, 127 unsigned), requires 1,872 private bytes per
invocation, validates for Vulkan 1.4, and passes the same fragment and two-wave
checks. Pointer escapes remain excluded. Its complete compile nevertheless
exhausts 4 GiB after 176.993 seconds, with 174.134 CPU seconds. A small parallel
read-only liveness calculation makes this wall duration unsuitable for a speed
comparison. Exact liveness finds 102 simultaneous direct FP32 locals and 102
colors; putting all of them in invocation-private Workgroup cells would need
59,920 bytes, above the queried device limit. Neither Private storage nor
Workgroup aliasing is accepted as a capacity fix.

The captured module has 256 guest invocations and 128 physical invocations,
with four complete guest waves and no thread-limit predicates in its initial
lane mask. The next bounded probe examines whether making that verified full
initial mask explicit reduces preserved inactive-register state. Partial waves
and dispatch thread limits must retain their existing predicates. No runtime
policy or new FPS result has been established.

Replacing only the initial lane predicates with true produces the same first
allocator graph and the same sixty-entry node growth as the original. Explicit
full mask words reduce the initial node count only from 35,442 to 35,140 and
still grow to 70,524 nodes before the bounded probe stops. Applying those
verified full mask words to the finite native-phase prototype reaches 57,173
nodes at sixty entries, above that prototype's prior 48,880. These are allocator
measurements, not complete pipeline or frame-time results. Full initial masks
alone therefore do not explain the allocation barrier; no mask optimization
is activated from these observations.

The matching Intel compiler exposes a host spill-batching option. Its default
value is eleven; setting `shader_spilling_rate=0` in one external child makes
each failed allocation spill one value per retry. At sixty allocator entries,
the unchanged original module reaches 46,886 nodes and 690,323,376 adjacency
bytes, compared with 72,594 nodes and 1,859,236,448 bytes under the default.
All 59 completed allocation attempts still fail in both probes. A complete
pipeline-only retry with the single-spill setting reaches its eight-minute
runtime limit without completing compilation. It consumes 475.415 CPU seconds
and peaks at approximately 1.9 GiB; no guest dispatch is queued. Lower spill
growth therefore does not establish a usable compilation time or an FPS fix.
No global driver configuration or runtime spill policy is changed. The next
investigation must measure the live values responsible for allocation failure,
rather than extend an unbounded sequence of spill retries.

A matching-build read-only probe now checks the allocator owner's live-range
metadata and class sizes. The original module has 35,438 virtual registers
and 58,580 backend instructions; its maximum interval overlap is 992 values,
requiring 2,172 contiguous register units. This is the allocator's conservative
interval model, not a count of live guest registers. The finite native-phase
candidate still reaches 761 overlapping values and 1,771 units.

An external rotated Private-array candidate places the same 234 direct locals
in 936 bytes per invocation, using a stable bijection of array indices. The
compiler actually reserves 29,952 scratch bytes for its dispatch, but the
initial graph rises to 51,068 nodes and fails allocation. Its maximum interval
overlap remains 1,063 values and 2,125 register units. Thus preventing all these
locals from becoming SSA does not remove the remaining temporary pressure.
The module validates for Vulkan 1.4, but has not passed GPU equivalence or a
complete compile and is not activated. Investigation proceeds to temporary
lifetimes within address-translation helpers rather than another storage-only
variant.

The owner-checked backend walk identifies all 992 values overlapping the
original peak. Of them, 588 share the interval from instruction 141 to 58,575,
almost the entire enclosing dispatcher loop. Their definition histogram
includes vector selects and additions, single-channel shifts and moves, and
undef initializations. This locates conservative temporary retention but does
not map those values to specific guest instructions.

Further external probes keep zero queued guest dispatches:

| Candidate | Initial graph nodes | Maximum interval register units |
| --- | ---: | ---: |
| Pack 28 low/high word pairs with a bit-preserving vector cast | 35,442 | 2,172 |
| Six-level radix lookup retaining full chunk-fit checks | 38,355 | 2,254 |
| Inline helpers, then clear state dead on the decoded guest CFG | 37,386 | 2,388 |
| XOR-indexed Private arrays for the original 234 direct locals | 49,630 | 2,101 |
| XOR-indexed Private arrays for 492 scalar locals after helper inlining | 79,862 | 2,220 |

All modules validate for Vulkan 1.4. The packed-pair variant produces the same
backend instruction count and initial graph as the original. The radix variant
removes binary-search loops from the address helper but increases the initial
pressure, so that helper's loops alone do not explain the barrier. Inlining
admits previously escaping scalar temporaries to CFG analysis, but adding
59,887 dead-state clears still increases pressure. Private storage also leaves
large temporary intervals even after those helpers are inlined. These probes
do not establish full compilation, GPU equivalence, runtime speed or gameplay;
none is shipped. The next bounded question is whether rematerializing Private
slot addresses at use removes their long intervals, within the shared-memory
limit, before considering any production strategy. That probe now uses 512
additional shared bytes for stable invocation ownership and requests 324,733
allocator nodes before its 3 GiB cgroup OOM kill, after 30.409 wall seconds.
It does not reach live-range measurement. Private-slot rematerialization via
shared ownership is also excluded. The external probe now stops graph requests
above 100,000 nodes before allocating them. Production memory and compiler
policies remain unchanged.

A separate external StorageBuffer candidate materializes 490 direct scalar
locals after helper inlining and retains the two dispatcher-control locals.
Disjoint invocation slices preserve raw 32-bit values; the proposed workspace
requires 262,144 bytes per physical workgroup. Its matching-layout Vulkan 1.4
module validates and reduces peak interval pressure to 1,263 units, but raises
the initial graph to 90,084 nodes and backend size to 124,093 instructions.
Inlining alone reproduces the original graph and pressure, so it is not the
source of that reduction. A complete pipeline-only run still reaches its
180-second limit without finishing, with 177.828 CPU seconds and a 3.6 GiB
service peak. No guest dispatch or runtime workspace is activated.

Combining that materialization with the finite native phases also validates,
but increases the peak to 1,612 units; its initial graph contains 84,377 nodes.
Neither smaller early pressure nor a new buffer representation establishes
compile-time or frame-time acceptance. Further work must preserve the known
subgroup-uniform scalar state when assessing any materialization strategy;
broadcasting arbitrary unsigned temporaries would alter guest vector values.

A fresh bounded strict Silent/Native source capture with Vulkan 1.4 reaches
its 120-second process limit, with 126.545 CPU seconds and a journal-reported
peak of about 4 GiB. Its native fifteen-second watch records zero frame and
present progress. The last generated compute source has 175 dispatcher cases;
assembly reproduces the corresponding captured module body exactly. This
recovers the actual scalar-register names without guessing a map from the
larger 181-case cached fixture. Source order alone is not a complete host stack
or proof of which later pipeline is active at timeout.

That source-matched program requests 34,519 initial allocator nodes and has
2,156 interval register units. External materialization of 487 direct scalar
locals reduces the peak to 1,263, with 86,618 graph nodes. Preserving the known
subgroup-uniform SGPR, EXEC, EXECZ, SCC and VCC state on their materialized loads
reduces the peak only to 1,217 and raises the graph to 87,292. Arbitrary unsigned
temporaries remain excluded from the broadcast. Representing the one dedicated
immutable metadata buffer with point-of-use StorageBuffer reads changes that
peak only to 1,209, with 88,358 nodes. These Vulkan 1.4-valid diagnostics do not
establish complete compilation, GPU preservation or a runtime policy.

Two other bounded address-liveness questions are also excluded as sufficient
fixes. Volatile Uniform loads in the original 181-case module change the peak
from 2,172 to 2,105 and raise nodes to 36,608. In its materialized counterpart,
a stable per-invocation workspace base read at each basic block changes 1,263
to 1,181 while raising nodes to 96,406; the workspace size remains unchanged.
Neither result meets the compile or execution gate.

An external unsigned word-pair representation preserves the 175-case guest
CFG, carry, borrow, high/low unsigned comparisons, the observed constant
shifts and physical pointer bit patterns. It validates for Vulkan 1.4 and
replaces all unsigned 64-bit value operations; an unused signed 64-bit type
and capability remain. The initial graph instead grows to 38,579 nodes, and
interval pressure barely changes from 2,156 to 2,154. Thus native unsigned
64-bit arithmetic alone does not explain the barrier. No GPU equivalence or
complete compile result exists for this representation, and it is not shipped.

Matching driver source excludes partial register writes from full block-def
kills. Read-only probes identify many width-one definitions with intervals
spanning the outer loop, but that does not prove those values are unnecessary
or that the driver is incorrect. Trace their actual producer and consumers
before another shader representation or allocator change. All external
pipeline probes queue zero dispatches; production compiler, memory, descriptor
and driver policies remain unchanged.

The width-one shift producers use an immediate count of eight. Matching
sampler-send lowering constructs packed surface/sampler descriptor bits with
that shift; the traced values have multiple partial writes and depend on
width-one sends. This is a resource-lowering lead, not evidence that guest
64-bit address arithmetic is responsible or that every such interval is
semantically unnecessary.

An external immutable-sampler layout leaves the original 175-case graph and
pressure unchanged: 34,519 nodes and 2,156 units. Folding eleven known sampler
metadata indices validates for Vulkan 1.4 but only changes those values to
34,341 and 2,138. Conservative continuation-CFG propagation, including
function-parameter write effects, resolves sixty image-register reads and
still produces that same backend graph and pressure. Immutable samplers and
these sampler-index constants alone are therefore excluded as sufficient
compile-capacity fixes. No complete pipeline, guest dispatch or FPS improvement
is established by these probes; all remain outside the production renderer.

A read-only resource-layout capture confirms the source-matched metadata
ordering. Folding its thirty-four known texture/sampler index loads validates
for Vulkan 1.4 and changes the initial graph to 33,930 nodes, with 1,740 interval
units. This includes typed image-array tags; other descriptor fields remain
runtime inputs. It is still compiler-only evidence, without complete compilation
or GPU equivalence, and is not activated.

A separate matching-layout descriptor-buffer probe admits the advertised host
feature and keeps the original shader unchanged. Its initial graph has 34,017
nodes and 1,841 units; singleton classes at the peak fall from 308 to 51.
Combining descriptor buffers with the external scalar-state workspace produces
87,278 nodes, 163,928 backend instructions and 937 units. The complete pipeline
still reaches its 175-second limit, after 173.572 CPU seconds and a 3.4 GiB
service peak, without a pipeline result. No guest dispatches are queued. These
representations do not establish a capacity fix or frame-time improvement.

An independently authored descriptor-loop reproducer contains no workload
code or data. Both eight-case and sixty-four-case modules validate for Vulkan
1.4. Their initial interval pressure is 43 and 155 units respectively. In the
larger program, 128 packed-descriptor temporaries span the outer loop from
instruction 5 to 2,826. Each is consumed by width-one OR/AND operations reading
its low word at byte offset zero; the remaining register bytes are not read.
This isolates the partial-register-def question independently of the private
program. The next compiler experiment must account for bytes actually read
and preserve predication, masking, wider reads and indirect access; it does
not authorize treating every long interval in the original program as dead.
No driver modification or system installation is validated by this evidence.

An external Mesa 26.2.3 build now tests that question in
`brw_analysis_liveness.cpp`, without changing the installed driver or Kyty's
renderer policy. It conservatively collects the byte spans of all VGRF reads.
An additional block-def kill requires a contiguous NoMask write covering all
read bytes, and excludes predicated non-select writes and indirect/subregister
addressing. Wider reads, high-word reads, masking, predication, prior reads and
noncontiguous writes retain their existing lifetime. Two independently authored
contract cases fail before the correction and pass afterward; the six negative
cases pass in both versions. All 299 compiler tests pass with the correction.

The same public sixty-four-case module falls from 155 to 28 interval register
units. Its complete pipeline creation changes from 132.950 to 36.105 ms in the
two local builds, with the shader cache disabled. Seven GPU dispatches exercise
distinct sampler LOD clamps and check 14,336 values against expected mip
colors; both builds produce zero differences. These are focused compiler and
GPU-preservation results, not a workload FPS benchmark.

The original 175-case module retains its 56,898 backend instructions and 34,515
VGRFs, while interval pressure falls from 2,156 to 1,896 units. With the external
correction, complete pipeline creation now finishes in 104.767 seconds under
the 4 GiB bound, with a 2.2 GiB service peak and no queued dispatches. This passes
the probe's complete-compilation gate, but remains a long cold compile. A
scoped warm-cache retry of the same module and probe layout without push
constants finishes in 9.236 ms with 28,280 KiB peak RSS. This is one cached pipeline,
not the complete workload. Private-program GPU execution, strict runtime
progress, frame time and gameplay acceptance still require measurement. No system
driver installation or vendor-specific production policy is made.

A strict Silent/Native run with the external driver and scoped cache still
ends in its 4 GiB cgroup OOM bound after 210.015 wall seconds and 221.135 CPU
seconds. At the last native snapshot, 153.961 seconds into the run, seventeen
compute pipelines have completed, totaling 81.357 seconds of Vulkan creation
time; thirty-five dispatches and 272 submissions are recorded, with zero
presents. Completed fence waits total only 0.186 seconds. No usable frame-time
sample or native graphics capture exists. The next evidence must identify the
actual pending compute module and stage flags; successful compilation of an
earlier source-matched fixture does not establish which pipeline exhausts the
runtime bound. This remains an unresolved compile-capacity failure, not proof
of improved gameplay or a current Xe reset.

A subsequent bounded Vulkan-call capture identifies the eighteenth application
compute pipeline before creation. Its SPIR-V matches the 175-case fixture
byte-for-byte; its full-subgroup flag and required size of thirty-two also
match. Its actual layout differs from the earlier probes: seven sampled-image
bindings each reserve seventeen descriptors, including unused typed banks,
whereas the probe layout has only the three declared sampled arrays of ten,
six and one. Both use eight storage buffers, two storage images, seven
samplers and one metadata UBO, with no push range in the runtime layout. Thus
the standalone completion and cache timing must retain that layout qualifier.
`GraphicsRenderDescriptor.cpp:146` reserves the aggregate sampled count for
every sampled bank; measure the captured layout before changing that shared
contract or attributing the remaining capacity failure to different shader
code or subgroup flags. No descriptor compaction is activated.

A warm-prefix retry with an 8 GiB service bound also ends in cgroup OOM,
after 176.239 wall seconds and 185.944 CPU seconds. Seventeen cached compute
pipelines complete in 3.074 ms total at the last native snapshot, but the
eighteenth remains pending; there are still zero presents. Host available
memory stays above 11 GiB. Increasing this service bound or caching the
completed prefix therefore does not establish a capacity fix.

The captured layout's initial interval pressure is 1,888 units, with 34,514
VGRFs and 56,895 instructions. This is slightly lower than the compact probe's
1,896 units, so unused descriptor banks do not explain higher initial pressure.
A bounded allocator trace nevertheless reaches 128,931 graph nodes, 179,395
instructions and 484,608 logical scratch bytes after 124.881 seconds, without
successful allocation. The probe stops at its RSS/time bound and queues zero
dispatches. Compare complete allocation trajectories with the same device
features before attributing that later growth to descriptor padding.

After relocating the runtime executables, external driver, caches and evidence
to a local btrfs filesystem, the compact-layout trace completes the unchanged
module in 94.518 seconds, with 2,351,668 KiB peak RSS and zero dispatches. Its
ninety-third allocation attempt succeeds with 120,677 graph nodes. The two
probe sources differ only in their descriptor-binding include. A complete
captured-layout run instead reaches its 4 GiB cgroup OOM bound after 156.595
wall seconds and 155.318 CPU seconds. Thus this capacity failure is reproduced
without executing the emulator or its caches on NTFS. This is layout-dependent
compiler evidence under the probe's device configuration, not private-program
GPU equivalence or authorization for an unverified renderer change.

### Per-array descriptor layouts (2026-09-29, strict compile progress)

The renderer now derives each layout from the separate image-array sizes
already declared by its shader generator. It preserves sparse binding numbers,
depth arrays, storage resources and every declared numeric dispatch bank.
Layout and descriptor-set caches include the complete binding/type/count key;
per-stage and pipeline limit checks count the same plan. Layout handles remain
live for their dependent sets and pipelines, with a bounded cache. The shader
module and descriptor writes are unchanged.

An independent public image/sampler program compares padded and separate-array
layouts on both the installed and external compiler builds. Distinct image
views and seven sampler LOD clamps produce 57,344 checked GPU values with zero
differences across four runs. This is preservation evidence, not a private
workload rendering result or a frame-rate benchmark. The Release executable
and native agent build successfully in a dedicated btrfs build directory;
boundary and graphics-table provenance gates pass. Focused unit cases for
shape keys, numeric banks, counts and binding collisions compile; their
execution remains deferred until the workload runs, as requested.

A strict Silent/Native run using the external compiler completes eighteen
compute pipelines and reaches thirty-six dispatches and 290 submissions at
the last native snapshot, with zero presents. It ends at its 6 GiB cgroup OOM
bound after 263.393 wall seconds and 277.620 CPU seconds. A bounded Vulkan-call
capture confirms that pipeline eighteen uses the byte-identical earlier
175-case module and unchanged stage/subgroup flags, with sampled descriptor
arrays of ten, six and one instead of seven arrays of seventeen. The next
pending pipeline is a different, larger module using that same corrected
layout. Thus aggregate descriptor padding is a verified compile-capacity
contributor, but is insufficient to solve the remaining compilation cost.
The next investigation must trace this nineteenth pipeline's allocation
trajectory and device feature admission. No current frame-time, graphics
capture, controls or gameplay acceptance exists.

### Immutable resource-index specialization (2026-09-30, strict compile progress)

The nineteenth pending module uses the corrected separate-array layout. A
matching-runtime-device pipeline probe reaches its 120-second bound without
completing compilation, with 118.965 CPU seconds and a 2.6 GiB service peak.
An owner-checked allocator trace separately reaches 125,659 nodes, 174,599
instructions and 416,544 logical scratch bytes after 114.731 seconds; it stops
at its RSS bound without dispatching the program.

A source-matched resource capture recovers the immutable image/sampler indices
that the host binding code writes into metadata word zero. Replacing only
thirty-four such loads, with all other descriptor words retained as runtime
inputs, completes the same module's pipeline in 44.080 seconds and 1,733,468
KiB peak RSS. Layout, subgroup flags and enabled runtime device features match;
the shader cache is disabled and zero private-program dispatches are queued.
A public image/sampler loop compares dynamic and known index loads over seven
metadata variants on both driver builds: 57,344 GPU values have zero
differences. That preservation result does not cover private-program pixels.

The production candidate derives these indices from the bind's existing
spatial/numeric banks, handles mapped scalar loads and initial SGPR metadata
through one emitter, retains mutable loads and versions the module cache.
The image-versus-storage classification participates in module identity.
Focused cases compile but remain deferred. The Release build and boundary/table
provenance gates pass. A strict Silent/Native integration run advances from
eighteen completed compute pipelines and zero presents to forty-eight completed
compute pipelines and 643 observed presents. Its native snapshot has a 172 ms
median and 241 ms p95 frame time; these are current timings, not a matched
frame-rate comparison. A scored native capture at present 643 is entirely
black (`low_entropy`), and cannot establish a visual or gameplay fix. The run
logs the title transition and subsequent map resource load, then reaches its
6 GiB cgroup OOM bound after 356.215 wall seconds and 337.890 CPU seconds.
Separate the remaining workload-memory and visual frontier from the resolved
nineteenth pipeline compilation; a warm-cache run must capture the opening,
transition and later state before input/gameplay acceptance.

### Descriptor-array feature admission (2026-09-29, unresolved)

`Window.cpp:2083` initializes device features to zero and leaves sampled-image,
storage-image and storage-buffer array dynamic indexing disabled. A read-only
Vulkan-call capture confirms those disabled feature values. The pending
compute module contains 194 nonconstant UniformConstant-array access chains;
its declared capabilities also omit the corresponding array dynamic-indexing
capabilities. This is an admission/emission contract to audit against Vulkan's
shader-interface requirements, independently of the compiler capacity issue.
The [Vulkan shader-interface contract](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)
defines the required features and capabilities for each descriptor-index class.
The existing standalone compiler probe enables all supported core features,
so it does not reproduce that device configuration. No validation-layer result
or causal performance claim exists for this discrepancy. Check the resource
index uniformity and required capabilities, then admit supported features and
reject unavailable requirements before enabling a production correction.

### Loaded image resource lifetime (2026-09-29, compiler verified)

Sampled-texture and sampler lookup now checks a live mapped load before the
initial SGPR binding. An initial flat texture and a later array descriptor
share one SGPR home in the captured pixel program. The later load's recorded
consumer lifetime identifies the array descriptor, but lookup previously
returned the initial flat binding. Comparison sampling then rejected the
instruction's four array coordinates. With the lookup priority corrected,
complete-program replay selects the loaded array and emits both lane banks.
This is resource-selection and compiler evidence; resulting guest pixels
and frame time remain unverified.

### Paired wave primitives (2026-09-29, GPU replay verified)

Shared paired execution now implements bitwise DPP rows and row-table
permutations with architectural source EXEC and destination masks, signed
SDWA integer conversion, whole-quad masks and inactive image destination
preservation. The combined Vulkan 1.4 GPU replay covers 320 cases and 42,064
register observations with zero differences against independent ISA
references. Eight malformed DPP tuples are rejected. The local Linux build,
emulator boundary gate and graphics-table provenance gate pass. Details and
remaining fragment adapters are recorded in `fragment-wave-execution.md`.

This does not enable compute fragment execution or provide a new guest FPS
measurement. Strict integration attempts end before any present: one is
terminated by host-wide OOM and another reaches its 6 GiB service limit
during pipeline compilation. Preservation of the strict guest frontier is
therefore still awaiting a run with sufficient memory.

A separate pipeline-only experiment tests whether eliminating redundant
local loads solves the compiler memory barrier. A single-block elimination
pass reduces the large module from 77,860 to 70,865 operations without adding
phi nodes; both variants still reach a 4 GiB cgroup OOM limit after roughly
three minutes. GPU reference checks remain exact. The capacity hypothesis is
disproven, and no optimizer policy change is shipped. The next investigation
must measure the driver's memory producer rather than infer it from SPIR-V
operation counts alone. Unit tests and gameplay gates remain pending.

### Vulkan 1.4 baseline (2026-09-29, opening image verified)

The active build now requires Vulkan 1.4 SDK headers and external SPIRV-Tools
with the Vulkan 1.4 target environment. Instance creation and device selection
reject older runtime versions. Both assembly and optimization use that same
environment, and the persistent cache target changes so older compiled entries
cannot bypass it. Historical Vulkan 1.2 validation results below remain
historical evidence rather than the current build contract.

Configuration verifies the installed SDK and compiler target. The full local
build succeeds. The generated detile module and the complete captured compute
program, including the array comparison correction, validate under Vulkan 1.4
with SPIR-V 1.6. A strict guest run records an actual instance request of 1.4.0
and preserves the opening caption in a scored native present-228 capture. This
first run spends 555 seconds creating compute pipelines; one 358,848-word
module takes 395 seconds inside the Intel driver. A read-only stack locates
that cost at `GraphicsRenderPipeline.cpp:917`; subsequent frames resume. The
warm interval presents 473 through 648 in 57.301 seconds (3.054 per second),
with no new SPIR-V compilation or Vulkan pipeline creation. The corrected
comparison-image dispatch completes on its recorded queue before a later
unsupported scalar instruction stops parsing. Later scene output and gameplay
still require separate evidence. Hosted release builds now install the
required SDK components. Windows and macOS builds and distribution packaging
remain untested locally.

### Dispatch address preparation timing (2026-09-29, native verified)

Native `perf-snapshot` and `diagnostics.performance` now expose call counts,
total time and maximum time for dispatch write-back, guest-address preparation,
residency discovery and tracked-snapshot refresh. Preparation includes registry
locking and table work; residency and refresh are nested intervals. They overlap
with processor and fence timings and are not exclusive frame costs.

The owned Linux build passes, as do strict source boundaries and the graphics
table manifest. A strict Silent/Native run under read-only debugger probes
verifies all twelve new fields in both serializers and their snapshot/reset
window. A 56.366-second warm interval
contains 216 presents, a median frame time of 245 ms and 124 submissions per
present. Dispatch processing accumulates 29.44 seconds, address preparation
1.90 seconds, residency 1.81 seconds and write-back 1.19 seconds. The scored
present-791 capture is uniformly black. Translation still stops on a pixel DPP
row shift at PC 0xfec; no scene or gameplay result is established. These counters
identify costs without changing guest synchronization or residency decisions.

### Compute wave-analysis preparation cost (2026-09-29, bounded verified)

Read-only host stacks locate repeated owned-string allocation and destruction
inside `ShaderComputeWaveNativeEquivalence.cpp`. Enum classification now uses
views of the same static names. Label validation and CFG target lookup use a
binary search after proving decoded PCs are sorted; reordered IR retains the
original linear search, and duplicate PCs retain their first matching index.
No admission condition, shader instruction or synchronization rule changes.

Two actual captured compute programs are replayed thirty times before and after
the change. The 2,924-instruction program retains its rejection, PC and reason;
median analysis time changes from 103.025 to 9.278 microseconds. A separate
137-instruction program captured at live analysis retains its successful result;
its median changes from 142.952 to 72.728 microseconds. These measure this CPU
analysis alone and do not establish an equivalent frame-rate improvement.

`ninja -C _build_linux_<slug> -j2 fc_script` succeeds. Strict source
boundaries and the graphics-table manifest pass. A fresh strict Silent/Native
run under read-only debugger probes preserves the same first pixel-emitter
failure at PC 0xfec. A 58.031-second warm window contains 229 presents with a
243 ms median frame time; the scored
present-813 capture remains uniformly black. Gameplay and acceptable runtime
performance are still unverified; focused unit tests remain deferred until
gameplay as requested.

### Compute dispatch input preparation (2026-09-29, bounded measured)

The command processor previously performed the complete compute input analysis
once to decide whether to drain guest-address writes, then again to record the
dispatch. Preparation and recording now share that analysis under the render
lock. A guest-address dispatch requests its processor drain before touching the
command buffer, then retries with newly analyzed inputs. Peer submission waits
still release both recording locks and retry with fresh inputs. The initial
processor drain does not consume the existing sixty-four peer retry budget.
No resource snapshot is cached across a wait.

Two strict Silent/Native runs without a debugger measure forty-five-second
warm introduction windows beginning after present 153. Both use Vulkan 1.4,
native resolution, the same compiled shader cache and resource bounds; neither
window compiles SPIR-V. The earlier executable differs only in these dispatch
preparation objects. Its launch uses a PTY while the new run uses a pipe;
logging is Silent in both measured windows. This transport difference and the
single sample per arm limit the comparison.

The earlier window contains 194 presents in 45.038 seconds (4.307 FPS), with a
226 ms median frame time. The new window contains 228 in 45.039 seconds
(5.062 FPS), with a 194 ms median. Input analyses per processor dispatch change
from 2.0001 to 1.0725, and accumulated dispatch processing per present changes
from 128.815 to 97.968 ms. Dispatches per present remain 77.81 and 77.73;
submissions per present remain 126.60 and 127.09. These samples measure a
17.5 percent introduction frame-rate increase, not a gameplay benchmark or
acceptable overall performance.

The owned Linux build, strict source boundaries and graphics-table manifest
pass. Both native captures are uniformly black and their offline score gates
exit 1. The new strict run preserves the first pixel-emitter failure at PC
0xfec and exits 65. The earlier guest also exits 65 according to its service
journal; its PTY proxy loses output, so that arm's final error text is unavailable.
Gameplay and the missing wave64 pixel lowering remain unresolved. Focused unit
tests remain deferred until gameplay as requested.

### Metadata descriptor liveness preparation (2026-09-29, bounded measured)

Read-only host stacks also locate per-instruction hash-node destruction in
`ShaderStorageAnalysis.cpp` while preparing sampler metadata. Each storage or
sampler query rebuilt the same PC-to-instruction index. The liveness traversal
now validates strictly increasing PCs and searches the existing instruction
array directly. Reordered IR retains the previous hash lookup; duplicate PCs
retain the conservative all-live result. Joins, unresolved targets, indirect
transfers and descriptor overwrite rules are unchanged. No analysis is cached
across guest updates.

Two captured programs are replayed thirty times, querying storage and sampler
evidence at fifty-one SGPR starts per iteration. Complete result digests are
identical before and after. The 137-instruction compute program's median query
batch changes from 540.648 to 81.916 microseconds; the 2,300-instruction pixel
program's median changes from 9,545.339 to 1,556.091 microseconds. These measure
analysis alone, not an equivalent frame-rate increase.

The owned Linux build and strict source/table gates pass. A fresh strict
Silent/Native run without a debugger reaches present 750, then preserves the
same first pixel-emitter failure at PC 0xfec and exits 65. Its forty-five-second
warm introduction interval contains 249 presents in 45.039 seconds (5.529 FPS),
with a 169 ms median frame time. The preceding dispatch-preparation run had
228 presents in the same duration (5.062 FPS), with a 194 ms median. Accumulated
dispatch processing per present changes from 97.968 to 76.095 ms; dispatches
and submissions per present remain approximately 77.7 and 127.1. Both windows
start after present 153, use the same resource bounds, native resolution and
compiled shader cache, and contain no SPIR-V compilation. This single-sample
comparison is limited to the introduction. The new native present-403 capture
is uniformly black and its offline score gate exits 1. Acceptable performance,
scene output and gameplay remain unresolved; unit tests remain deferred until
gameplay as requested.

### Sparse physical residency query (2026-09-29, not gameplay)

Read-only sampling found descriptor preparation repeatedly scanning an entirely
unpopulated two-gigabyte physical-backing interval. The owned Linux backing can
prove the complete interval empty with `SEEK_DATA`. A fresh query on each
preparation now avoids that interval's `mincore` scan; unproven intervals and
unsupported hosts keep the existing residency path. No absence is cached.

In a strict Silent/Native comparison, the warm opening interval changes from
0.884 to 2.969 presents per second. Descriptor preparation falls from 423.33 to
5.14 milliseconds per present, or 25.02 to 0.30 milliseconds per draw. The
native present-173 capture preserves the opening caption. The run reaches
present 809 and the same unresolved comparison-image binding, with no observed
earlier failure. The image after the caption and gameplay remain unverified;
this is a measured host-side cost reduction.

### Exercised custom pixel interpolation gap (2026-09-29, unresolved)

An address-correlated native lifetime trace identifies two draws writing the
sampled overlay image before final composition: an indexed six-index material
draw, then a three-vertex draw that exports zero to two channels. The first
draw has four pixel inputs, including control `0x424` for input three, and its
decoded program uses `V_INTERP_MOV_F32` selectors 2, 0 and 1 on that input.
The prior sampled context alone did not identify an executed writer.

`ShaderPixelInterpolator.cpp:104` rejects that parameter-cache pass-through
control. `ShaderSpirvVector.cpp:2802` continues after the failed decode, and
`Recompile_VInterpMovF32_VdstVsrcAttrChan` loads the same ordinary varying for
all three selectors. This general custom-interpolation gap, previously
excluded for a different program with only P1/P2 instructions, is now exercised
by the observed overlay writer. Resolve the producer parameter and distinct
per-vertex values, preserving their raw packed bits and the guest barycentric
inputs. The 89,132-byte pixel module read from the live translation cache fails
`spirv-val --target-env vulkan1.2`: conflicting fragment input location zero,
component zero (`VUID-StandaloneSpirv-OpEntryPoint-08721`). This supplies an
actual-module red validation case. A corrected strict comparison must establish
whether this is the producer of the nonfinite overlay values; the code mismatch
alone does not yet prove that causal result.

The first correction transports raw parameter values through a generated
triangle geometry stage, using distinct flat unsigned vectors for each
vertex and perspective/linear center or centroid coordinates for manual
interpolation. It selects the route from the consumed controls and the host
geometry capability, checks interface limits and gives that route its own
pixel/module identity. Ordinary inputs retain their interpolation modes.
The exercised pixel module (30,488 bytes) and geometry module (1,868 bytes),
read from the live translation cache, both pass Vulkan 1.2 SPIR-V validation;
the pixel source and optimized disassembly also validate. The original
duplicate-location failure is gone, with four component loads from each of
the three vertices and initialized barycentric VGPRs. Native captures at
presents 93 and 184 remain uniformly black. This corrects the exercised
interface and selector translation but does not establish useful rendering
or identify the remaining producer of zero/nonfinite output. PARAM0's
ambiguous custom/default encoding, sample/pull-model barycentrics and layered
geometry still require separate contracts.

A subsequent native probe of the corrected material's MRT0 export at the
100-present threshold counts 409,600 invocations: every result is finite and
RGBA zero. The probe's generated and optimized modules validate for Vulkan
1.2, and capture 157 is still uniformly black. This observes zero at the
material output; it does not yet locate its first zero-valued input or prove
per-pixel correspondence with the earlier nonfinite final sample.

### Storage write-back dependency retry (2026-09-29)

A later Silent/Native diagnostic run passed at least 598 presents, then stopped
after 708 seconds at `Objects/GpuMemoryWriteback.cpp:214`: a writable
`StorageBuffer` still had incomplete exact submission dependencies when
`WriteBackObjectLocked` was requested. This occurred before the JSON assignment
call in that run. An earlier shorter
occurrence is already recorded below. Capture the current caller, resource
dependencies and completed queue sequences before changing synchronization;
neither skipping write-back nor treating a pending fence as complete is valid.

A subsequent run captures the exact cross-queue state. A compute command
buffer on queue 7 calls `GuestDeviceAddressWriteBack` through
`GraphicsRenderBind.cpp:4188`; the selected writable storage object depends
on queue 8 sequence 48643, while the published completion is 48642. Queue 8
sequence 48643 is still the graphics processor's active recording buffer,
not an executing Vulkan submission. `CommandProcessor::DispatchDirect`
already calls `WriteBack`, but that drains its own compute processor only.
The later registry-wide storage publication therefore encounters another
processor's recorded resource use. Capture the access role of that use and
move exact dependency preparation outside recording/mutation locks before
publishing host-visible bytes; adding a fence wait inside `BindDescriptors`
would hold the render lock across the other processor's completion path.
This run stops before exercising the pending JSON string conversion.

Dispatch preparation now checks the same writable storage objects and exact
submission dependencies as publication, while holding the render recording
lock. A pending use returns to the command processor before command-buffer
mutation; the processor releases recording locks, waits for that submission,
and retries with a bounded attempt count. Indirect dispatch shares this path.
A strict live trace exercised three retries against graphics queue 8 sequences
24691, 24692 and 24693. All three dispatch calls returned with queue 8 completed
through 24700, and the run continued through present 807 to an unsupported
image-atomic instruction. This verifies the exercised cross-queue retry;
gameplay acceptance and deferred focused tests remain outstanding.

The exercised custom-interpolation program also reads its incoming
`FRONT_FACE` VGPR. The pixel prolog in `ShaderSpirvGenerator.cpp` initializes
XY position but does not initialize that field. The captured module declares
the register without an initializer and first reads it at an integer compare;
its first store comes after that read. This is undefined input, not a proven
zero-valued front-face flag. Its live `SPI_BARYC_CNTL` is `0x01000000`. AMD's public
[register guide, page 189](https://docs.amd.com/api/khub/documents/9fuBVmqajj07G~5~aeTUig/content)
defines bit 24 as selecting integer one/zero for front/back, versus floating
positive/negative one when clear. Shader metadata now carries that control,
and the enabled input adds a versioned identity with its encoding. The pixel
prolog loads Vulkan `FrontFacing`, selects the requested bit representation
and initializes the VGPR before guest instructions.

A subsequent Silent/Native run validates the live 30,596-byte ordinary pixel
module, its 33,828-byte diagnostic variant and both 1,868-byte geometry
modules for Vulkan 1.2. Their identities contain the new front-face version
and integer encoding. The diagnostic source and optimized module also
validate; the source initializes VGPR4 with integer one/zero before the old
first read. The floating representation still requires focused validation.
At the same 100-present threshold, all 409,600 material invocations remain
finite RGBA zero, and native capture 134 remains uniformly black. This
corrects the missing system input but excludes it as a sufficient fix for
that black output.

A read-only snapshot at the diagnostic draw's actual descriptor bind records
its scalar input buffer. Both opacity factors loaded at byte offsets 16 and
24 are floating one. The zero at offset 16 seen at an earlier first draw is
therefore not evidence for this later all-zero export. Follow the texture
sample and packed per-vertex alpha at this same draw before changing either
buffer contents or guest media state.

The implicit-LOD 2D RGBA sample is now also admitted by the bounded native
sample probe. At the same threshold it records 409,600 finite RGBA-zero
vectors immediately after sampling, before material arithmetic. Its actual
33,300-byte cached diagnostic module, 30,596-byte ordinary pixel module and
both geometry modules pass Vulkan 1.2 validation, as do the generated and
optimized diagnostic modules. Capture 129 remains uniformly black. The
correlated descriptor is a 1600-by-256 BC7 sRGB image with Standard64KB tiling
and eleven allocated levels; all four vertex colors are opaque white.

`Utils.h:155` excludes uncovered Standard64KB BC formats from guest upload.
`Objects/Texture.cpp:533` consequently seeds a zero image when that policy is
selected. The Standard64KB sample path also lacks a compressed mip-chain
layout. This is a concrete unsupported-package defect; confirm the actual
backing role and bytes before attributing the observed sample to it. A later
cache snapshot no longer contains that opening texture, so it does not prove
which materialization path was used at the probed draw. Capture the creation
and implement the evidenced block-coordinate mip layout, preserving live
GPU-owned surface dependencies.

The retired object record does retain the exact opening descriptor: eleven
levels, a 6,553,600-byte claimed span and `skip_guest_upload=1`. Its content
origin is recorded as CPU upload even though that path clears the image.
The still-readable source bytes decode into a coherent nonzero opening
caption with a meaningful alpha channel using Standard64KB block addressing
and the reverse mip-chain placement; they were read after object retirement,
not at the original draw. The
[public GFX10 layout](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/imported/addrlib/src/gfx10/gfx10addrlib.cpp)
and two local implementations agree on a shared tail block followed by the
larger levels in reverse order. For this descriptor the compressed block
model requires 983,040 bytes, with mip zero at byte 524,288 and the tail
beginning at level four. The original texel-based size and zero-seed policy
are therefore both incorrect for this CPU package texture. A corrected run
must capture its upload and sample before claiming visual improvement.

The corrected Silent/Native run captures creation with the expected
983,040-byte span and guest upload enabled. The shared detiler returns all
eleven compact levels; its first level is byte-identical to the independently
decoded source. At the same 100-present threshold, all 409,600 sampled
vectors are finite: RGB ranges from 0.99115 to one and alpha from zero to one.
Native capture 173 now contains the opening caption, although it is blue.
The offline capture gate still rejects it as gameplay. The affected build,
source-boundary gate and thirteen-table provenance check pass; unit tests
remain deferred until the requested runtime milestone. This verifies the
package upload correction, not gameplay or correct final composition.

The remaining color defect has a separate concrete trigger:
`GraphicsRenderHwCheck.cpp:547` incorrectly describes every color mode except
resolve as an ordinary draw, while both draw entry points route only mode
three through fixed-function handling. A bounded live trace captures mode
six with DCC enabled on the same HDR overlay after its material draw. The
ordinary shader exports zero to red and green, preserving blue and alpha;
that shader export is not the decompression operation's color result. Check
the existing expanded host backing and submission dependencies, then handle
the metadata operation before ordinary rasterization. Mode two also occurs
on other targets and denotes fast-clear elimination; its clear-state contract
must be handled separately from DCC decompression.

The expanded-surface handler now consumes mode-six operations before either
indexed or automatic ordinary draws. It requires one existing initialized
render texture with exactly matching extent, format and memory span, a single
sample/mip/layer, and no fast-clear key. It records the submission use through
the GPU-memory lookup. A bounded live probe confirms three enabled DCC
attachments return through this preservation path. With context restoration
corrected, native capture 89 contains the opening caption in white instead of
blue. Both native and offline scores classify this simple caption as low
entropy and not gameplay. The emulator/executable build, boundary gate and
thirteen-table provenance gate pass; focused test execution remains deferred.

The same run reports a separate stencil frontier from
`GraphicsRenderHwCheck.cpp:339`: stencil testing is enabled with neither read
nor write base present. It has not caused a process stop here. Capture the
producer and format before deciding whether that state is inactive hardware
state or a missing attachment; the opening caption does not validate stencil.
A later bounded read-only capture records both depth and stencil formats as
zero, with both stencil base addresses zero. `GraphicsState.cpp:146` still
classifies the enabled test as a missing plane before checking the disabled
format contract. Trace the format and test-enable interaction before changing
this classification; the observation alone does not validate stencil output.

### Register-default lookup and context restoration (2026-09-29)

A strict diagnostic run with expanded-surface DCC handling stops at present
35 on a 2432-by-1368 HDR target with no live GPU-memory object. Its ordinary
postprocess draw still has color mode six after the preceding metadata
operation. Original guest indirect lists and their immutable snapshots agree
at the failure. Creating a zero backing or recognizing operations by shader
words would conceal the missing state restoration.

A producer hardware watchpoint confirms API version thirteen reads seventeen
pairs from the sixteen-pair depth default in `Graphics.cpp`. The seventeenth
read lands on the next `RegisterDefaultInfo` header. This table-size defect
remains unresolved; verify the versioned depth layout before extending it.
The ordinary color-control default itself is copied correctly, and its
observed type lookup reads only the bank and pointer index. Upper index
metadata is not a supported explanation for the stale mode.

A separate missing target-zero blend key supplies an all-ones pair, later
used as an invalid register offset. Adding the key with the existing
`CB_BLEND0_CONTROL` default changes the live normal draw to the valid blend
register and removes the inherited reserved bits. The same DCC failure
remains, excluding this lookup defect as the stale-mode cause. The emulator
and executable builds, source-boundary gate, and graphics-table gate pass;
focused tests remain deferred until gameplay by the active investigation
instruction.

The guest surrounds its metadata helper with context-state operations three
and two. The import resolves to `GraphicsCbType2Pad` in `Graphics.cpp:4079`,
which dropped the operation argument and emitted one padding word. The
corrected HLE encodes clear/push/pop/push-clear and retains the helper's
allocation boundaries and byte-size query. The processor owns one saved CX
bank; shader and UCONFIG banks remain separate. In the strict comparison,
twenty pops restore the complete saved context byte-for-byte, including a
mode-six utility returning to normal mode one. The run passes the old
35-present failure. This verifies context restoration, with the pending
expanded-surface DCC handler present in that comparison; it is not gameplay.

The first comparison also exposes an older, separate reset-queue encoding
problem in `Graphics.cpp:3424`: it writes the third argument's low four bits
as a native `CLEAR_STATE` operation. The live value thirteen is outside the
hardware clear/push/pop/push-clear range. Tightening the native packet decoder
therefore stops earlier at queue initialization. Its existing behavior is
preserved for the context-helper comparison; trace reset-queue arguments and
its actual packet contract before correcting that encoder and decoder.

### Title-transition admission (2026-09-29, not gameplay)

The context-restored Silent/Native run reaches the title transition after
15 minutes 49 seconds. Two threads stop there. The 147-instruction compute
program now decodes scalar absolute value correctly, then fails in
`ShaderComputeWaveAnalysis.cpp:528`: the last mapped EUD load precedes two
separate ordinary writes to the former pointer registers. Both words are
replaced before later arithmetic reads. The prior lifetime proof rejected
any partial write and could not accumulate these definite per-word writes.
The correction tracks possible and definite word masks across the existing
control-flow edges. It retains rejection of mapped loads after either original
pointer word may have changed and of ordinary reads lacking a definite write.

The original and transformed programs now pass admission. With the subsequent
image-descriptor correction, the complete module passes Vulkan 1.2 SPIR-V
validation, and the recorded dispatch's queue advances beyond its submission.
Focused branch, partial-write and back-edge tests remain deferred as requested.

The next strict run admits the original and transformed 147-instruction
programs. Emission then fails at PC `0x298` in `ShaderSpirvImage.cpp`: a
storage-image descriptor has moved from its original SGPR range through four
scalar pair copies. A second store uses a descriptor loaded from the extended
table. Both resources are present as writable 2D arrays; the static register
lookup does not follow either origin. Track complete descriptor words through
copies, mapped loads, and control-flow joins, rejecting partial or conflicting
origins. This is descriptor selection, not a missing image dimensionality.

The first descriptor-origin comparison stops earlier at a different store.
Its producing scalar load maps to a sampled descriptor, while a separate
writable binding contains the identical eight guest words. Treating binding
usage as guest identity therefore rejects a valid sampled/storage alias. The
origin proof must select the unique writable binding with the same complete
T#, preserving the existing split binding model; register coincidence or a
matching address alone is insufficient. This closes the assumption that an
origin must already name a writable binding.

The alias correction selects the existing writable binding in that earlier
program. The next comparison exposes an analysis bug: a scalar load into VCC
was rejected even though it cannot change any tracked ordinary SGPR word.
Such a load preserves the descriptor-origin state; any later copy from that
untracked special register still produces an unknown origin. The captured
failure occurred before a later mapped texture load, so it does not disprove
that store's complete descriptor identity.

The corrected analysis passes both earlier programs and resolves the two
original stores to their expected writable bindings. The actual complete
module passes Vulkan 1.2 validation; its recorded dispatch completes before
the next strict failure. Complete eight-word origins survive scalar copies
and verified loads, while conflicting control-flow joins remain unknown.
Build, strict boundary and all 13 table checks pass. Focused origin-analysis
tests remain deferred, and no gameplay acceptance has been established.

### Image atomic and class translation (2026-09-29, compiler verified)

The next title-transition program stops at `ShaderParseMIMG.cpp:41` with
MIMG opcode `0x11`, 2D dimension, one data component and `GLC=1`. The RDNA2
ISA identifies `IMAGE_ATOMIC_ADD`; for atomics, GLC requests the pre-operation
value in VDATA. The parser also leaves this opcode unimplemented at its switch
case. Implement resource write classification, the atomic read/modify/write
and its conditional old-value return together, preserving EXEC and image
coordinate rules. Removing the GLC rejection alone cannot implement this
instruction. The preceding translated dispatch has completed on its exact
queue; this new failure is a later decoder boundary.

The implementation resolves the atomic's complete descriptor, uses its typed
R32_UINT storage alias, and retains the required VDATA source and conditional
old-value return. Full admission also exposed VOPC/VOP3 opcode `0x88` being
substituted with `SBarrier`. Both parsers now decode `V_CMP_CLASS_F32`; the
emitter classifies the ten binary32 classes from their bits and uses the
existing paired-wave mask packing. The captured SDWA form uses DWORD sources,
an ordinary scalar destination pair and class mask `3`.

Complete-program replay then exposed an array-view gap in
`Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF`. Its existing typed sampler now
handles explicit level zero while preserving the array layer and separately
encoded coordinates. The prior atomic formatting failure was an IR format
that declared two sources while the parser retained the third VDATA read;
the corrected format preserves all three operands.

The complete 1,429-instruction captured compiler input produces a module of
177,518 words that passes `spirv-val --target-env vulkan1.2`. Generated-source
checks cover both atomic lane banks, the numeric class comparisons and array
level-zero samples. The emulator and executable build, strict source boundary
check and all thirteen graphics-table checks pass. A strict Silent/Native run
shows the opening caption and admits the complete program; its cached live
module also passes Vulkan 1.2 validation. At present 805, resource preparation
stops before this dispatch is submitted. Focused unit tests are deferred until
gameplay, as requested. These are compiler milestones, not gameplay acceptance.

The next binding failure is `GraphicsRenderBind.cpp:3760`: a pure comparison
sample selects a 2D-array view of an uploaded format-56 color image.
`ResolveDepthReferenceImageView` rejects that view, while the array emitter
already uses a regular sample followed by an explicit comparison. The live
sampler has linear minification and magnification, so merely relaxing the
view check would leave compare-after-filter semantics. Resolve comparison
filtering and color/depth view compatibility together; do not substitute a
texture or accept the view solely to pass the binding gate.

Array comparison now gathers the four base-level texels, compares each before
bilinear filtering, and admits the floating color-array view through the
existing regular sampled-image path. The complete captured program validates
for Vulkan 1.4. A strict run records its dispatch at queue 8, sequence 96,288;
the completion ledger reaches 96,294 on that same queue. The later parser stop
is SOP1 opcode `0x44` in another compute program, after present 808. This
proves completion of the former blocked dispatch; the black interval and
subsequent scene output are still unverified.

The new stop is `ShaderParseSOP1.cpp:280`, decoding
`S_ANDN1_SAVEEXEC_B32` (RDNA2 SOP1 opcode 68). The captured input is a
32-lane compute dispatch, saving EXEC_LO to a scalar destination and updating
EXEC_LO from the complement of its scalar source. Implement the decoder,
implicit EXEC/SCC effects and mask-aware analysis together; do not map it to
a barrier or widen it to the existing 64-bit instruction.

Captured-program replay then reaches `S_AND_SAVEEXEC_B32` in the same input.
The 32-bit SAVEEXEC family now decodes and emits its operation on EXEC_LO,
saves the old low word before any destination alias can overwrite the source,
and derives SCC from the low result while retaining the high EXEC word.
Implicit-mask analysis excludes these operations from linear storage coverage
and rejects treating a wave64 low-half update as wave-width independent.
The full 2,404-instruction captured program now decodes all seven observed
SAVEEXEC operations (AND and ANDN1). A strict run reaches source generation
and stops earlier in that program at `ShaderSpirvVector.cpp:772`: the floating
comparison emitter unconditionally asks for a second destination word, but a
captured SDWA comparison targets only VCC_HI. The integer comparison path
already handles this single-word form. Preserve that destination width in the
floating path; do not widen VCC_HI to a pair. GPU execution of this program was
still pending at that stop. Focused unit tests remain deferred until gameplay.

After preserving the single-word floating comparison destination, the complete
program emits source and assembles for Vulkan 1.4. Validation exposes a separate
native control-flow defect in `ShaderSpirvWriteLabel.cpp:288`: backward branches
sharing a destination have their loop header deferred to a later exit test.
The back edges consequently target ordinary blocks, rejected by `spirv-val`.
The failing graph contains nested loops with several back edges. Derive one
header and continue target from each loop's complete predecessor set, retaining
its real exit and every guest branch. Do not route this through the paired-wave
dispatcher without proving the native lane and barrier semantics.

The native lowering now recognizes single-entry interval loops with a common
exit, emits the loop header at the real entry, and routes every return through
one continue block. It also keeps selections inside their enclosing loop,
closes loop exits through the owning selection, and proves arm reachability
before nesting shared joins. Header repair alone was insufficient: validation
then exposed a selection merge outside its loop, a skipped inner selection
merge, and sibling cases incorrectly chained as parent/child. Those failures
are preserved in the local compiler evidence. The complete captured program,
both before and after performance optimization, now validates for Vulkan 1.4;
the preceding 1,429-instruction program also still validates. A strict
Silent/Native Vulkan 1.4 run records the new dispatch at queue 8, sequence
96,272, and the completion ledger reaches 96,273 on that queue. Its scored
native present-195 capture preserves the opening caption; present 726 is
black. The runtime then starts the title and requests the map resources,
before a different 2,924-instruction compute program stops in source emission.
These are dispatch and loading milestones, not scene or gameplay acceptance.
The structural rules are defined in the
[SPIR-V specification, section 2.11](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_structured_control_flow).

The new stop is `ShaderSpirvVector.cpp:4204`: reverse subtraction with borrow
reads VCC_HI in a native 32-lane dispatch, but the provenance guard accepts
only the low VCC/EXEC word or a proven SGPR pair. A preceding single-word
unsigned comparison produces VCC_HI; an intervening reverse subtraction
writes VCC_LO. The shared carry emitter also clears the high destination word,
which may destroy that independent mask. Verify the wave32 mask-width contract
against the ISA and retain the captured producer/consumer sequence before
changing either the guard or the store width.

The captured program reproduces that exact emitter failure independently of
the runtime. RDNA2 wave32 mask rules and the
[LLVM GFX10 assembler's wave32/wave64 carry cases](https://github.com/llvm/llvm-project/blob/main/llvm/test/MC/AMDGPU/gfx10_asm_vop2.s)
confirm that carry inputs and outputs occupy one scalar word in wave32. The
native compute emitter now preserves the adjacent word, decodes an explicit
high-VCC operand as one word and requires a same-block comparison producer
before consuming it as reverse borrow. The full 2,924-instruction program
validates before and after Vulkan 1.4 optimization; its generated consumer
loads the original comparison value without an intervening high-word clear.
A strict Silent/Native run dispatches it at queue 8, sequence 96,168; the
completion ledger reaches 96,171 on that queue. Native capture 225 preserves
the opening caption and capture 592 remains uniformly black. The next stop
is a different 496-instruction compute program at
`ShaderSpirvImage.cpp:3171`: IMAGE_GET_RESINFO reads a descriptor classified
as read/write storage, while its emitter only accepts sampled images. The
captured instruction asks for width and height, and later operations use
the same storage descriptor. Verify the requested mip and descriptor view
before implementing the storage-image query. No scene or gameplay output is
established by the completed carry-mask dispatch. Focused unit tests remain
deferred until gameplay.

The storage-query replay confirms a zero LOD produced by an unmodified vector
move in the entry block, a plain 2D descriptor with base/last/max mip zero,
and a width/height-only result. Its later image store references the same
descriptor. The new lowering uses the proven writable binding and
`OpImageQuerySize` for that single-mip spatial query. Unknown LODs, control-flow
bypasses, other mip views and unimplemented result components remain rejected.
Storage mip metadata participates in the translation key so cache reuse cannot
bypass those checks. The entire 496-instruction program validates before and
after Vulkan 1.4 optimization. A strict Silent/Native run submits it at queue 8,
sequence 96,483; that queue's completion ledger reaches 96,485. Native capture
213 retains the opening caption, capture 707 is uniformly black, and the run
reaches present 810 before a new allocation failure. This establishes execution
of the queried program, not correct scene output or gameplay.

The new stop is `Objects/GpuMemoryCreate.cpp:1895`: a single-mip 32x32 storage
image equals one sampled Texture range and crosses a second sampled Texture.
Both parents have CPU-upload origin, equal CPU/GPU markers and no write-back
function. The first parent's format, extent, pitch, tiling and mip parameters
match the incoming storage image. The multi-parent classifier has no path for
this mixed relation set. Verify guest upload ownership and preserve both sampled
backings before extending the classifier; do not seed from stale GPU aliases.

The classifier now admits a single-mip 2D storage view only when every parent
is a CPU-uploaded sampled Texture with equal CPU/GPU markers, no write-back
function and an Equals or Crosses relation. An exact parent must also match
the full sampled layout. Normal storage creation uploads the guest bytes;
existing links preserve the sampled backings and their pending reads. A strict
Silent/Native run observes this path, dispatches the formerly blocked work at
queue 8, sequence 96,778, and completes through 96,779. Native capture 279
preserves the fading opening caption; no scene capture was obtained before
the next failure. Source/table gates and the Linux build pass.

The next first failure is a 335-instruction pixel shader at PC 0x44c:
`S_GETPC_B64` writes a scalar pair, but `ShaderGetInputInfoPS` never supplies
the program-base metadata required by `ShaderSpirvProgramAddress.cpp:13`.
Vertex and compute input analysis already supply it. Capture its consumer
chain and apply the same runtime base-address contract to the pixel stage;
do not bake the mapped address into the translated module.

The captured consumer builds a 128-byte V# over an inline table immediately
following the program, then reads sixteen pairs at scalar byte offsets. A
second defect in `ShaderSpirvBuffer.cpp`, the scalar-buffer load emitters,
would use that constructed base word as a storage-descriptor index whenever
another bound buffer exists. Merely enabling pixel GETPC metadata is therefore
insufficient. Pixel input analysis now supplies the runtime program base
and routes unbound scalar V# reads through the existing guest-address table.
It preserves statically bound and dynamically materialized buffer paths,
checks each DWORD against the descriptor extent and reads all source words
before writing overlapping destinations. Scalar execution stays independent
of EXEC. Cache policy hints and combined register/immediate forms outside the
admitted subset remain rejected. The complete captured 335-instruction module
passes Vulkan 1.4 validation before and after optimization. A strict
Silent/Native run records that draw at queue 8, sequence 96,400, and the
completion ledger reaches 96,424. Its live metadata has both runtime address
blocks enabled and spills 160 bytes through the existing uniform-buffer path.
Native capture 318 is uniformly black; the run reaches present 809 before
another pixel program stops in `ShaderParse.cpp:131` on family 0x33, word
0xcc20701a at PC 0xb8. The complete 13,920-byte program and stack are captured
for decoding against the ISA. No scene or gameplay output is established;
focused unit tests remain deferred until gameplay.

The next captured family is RDNA2 VOP3P. Its first operation is
V_FMA_MIX_F32; the complete byte range contains twelve independently decoded
instances with different FP16/FP32 source selections. `ShaderParse.cpp` lacks
family 0x33, while `ShaderParseVOP3.cpp` incorrectly treats different-family
opcodes 0x320/0x321/0x322 as ordinary FP32 FMA, including approximated packed
destinations. `ShaderSpirvDispatch.cpp` also routes VFmaMixF32 through a generic
three-FP32 emitter. A correct implementation needs a separate family decoder,
source precision and half selection, inline constants at the selected width,
and modifiers applied after conversion. The RDNA2 operand table and sections
12.10/13.3.6 specify those semantics. LLVM's gfx1030 disassembler independently
confirms all twelve captured operations and places OPSEL_HI source bits at
59, 60 and 14 respectively; the older PDF's prose reverses two of those source
names. The full-program parser replay reproduces the first failure with exit
65 before implementation. The Linux build and source/table gates pass with the
new decoder and mixed-source emitter. Parser replay now reaches PC 0x100c and
rejects VOP3 opcode 0x378 (V_PERMLANEX16_B32); it has decoded all twelve mixed
operations before that stop. Live translation now emits all twelve mixed
operations before stopping at
PC 0xfec on a DPP row shift. The retained source prefix contains twelve Fma
operations and twenty-eight FP16 unpack operations; it is not a complete
validated module or a completed draw. Unknown packed operations, unverified
packed destination forms and upper-half inline FP16 constants stay rejected.

The captured prefix also contains V_OR_B32 DPP row shifts with controls 0x111,
0x112, 0x114 and 0x118, followed by the cross-row permutation and reads of
lanes 31 and 63. `ShaderSpirvOperands.cpp:395` admits only quad permutations,
so adding the missing parser opcode alone cannot translate this program.
The later wave-width and consumer captures below constrain the host subgroup
lowering. `GraphicsRenderPipeline.cpp:371` currently
leaves the fragment stage pNext empty, even when pixel input analysis requests
an exact subgroup width. Its capability check covers quad operations alone;
`ShaderSpirvVector.cpp:3545` also forwards readlane selectors without the ISA
wave-width mask. These are unresolved contract defects. Any correction must
preserve helper participation and inactive-lane rules; a 32-lane host cannot
be assumed equivalent to a 64-lane guest without a program-level proof.

A strict Silent/Native rerun confirms the same full program bytes and stops
at PC 0x100c after 706 decoded instructions. The live SPI_PS_IN_CONTROL value
is 0x4005: PS_W32_EN is clear, so this is a wave64 input. The captured host
reports a maximum subgroup width of 32 and does not support required fragment
subgroup sizes. A new parser representation preserves PERMLANE16/PERMLANEX16
FI and BC bits and rejects their reserved modifiers. Full-program replay now
parses 2,300 instructions through PC 0x3260 with exit 0; those operations still
have no admitted backend lowering. This closes a decoding gap only.

The same run reaches present 808; native capture 328 is uniformly black.
A 152.56-second native interval under read-only debugger probes contains 445
presents, a median frame time of 299 ms and roughly 146 submissions per present. Dispatch
processing accumulates 92.14 seconds; fence waits accumulate 33.38 seconds
and WAIT_REG_MEM waits 39.52 seconds. These times overlap across processors
and must not be added as an exclusive frame-time breakdown. Shader cache hits
are 251 with no misses or SPIR-V compilations in that interval. Coherency and
submission preparation are the next performance measurement, separate from
the missing scene shader. Timers for dispatch write-back, address preparation,
residency discovery and snapshot refresh are now verified through native
diagnostics. The later plain-run comparisons above measure the two dispatch
preparation improvements separately from this debugger interval.

A related summary-operand defect remains unverified in this workload:
`ShaderSpirvOperands.cpp:573` combines VCC_LO and VCC_HI when reading VCCZ,
even for a native wave32 dispatch. RDNA2 defines that summary from the low
word alone in wave32. An independent nonzero high word with a zero low word
therefore supplies the wrong summary. Capture an actual VCCZ consumer before
changing its lowering; scalar branch lowering already has a separate path.

### Integer value references (2026-09-29)

A missing integer getter in `LibJson2.cpp` stops the title-transition caller.
The captured caller accepts signed or unsigned integer tags and dereferences
the returned address. The HLE now validates either integer tag and returns a
reference to its existing payload. A strict Silent/Native run observes the
unsigned value 100, a return address exactly sixteen bytes after the value
object, and the same payload when that address is read. The missing import is
passed; the concurrent compute program now stops later during image-store
emission. The emulator and executable builds, source-boundary gate, and
thirteen-table provenance gate pass. Focused tests remain deferred until the
requested gameplay milestone; this getter observation is not gameplay.

### Zero-LOD HDR sample observation (2026-09-29, not gameplay)

The bounded native sample probe now observes 2D `ImageSampleLz` RGB and RGBA
results as well as the existing biased sample. It records the original sampled
vector after the normal destination stores and does not replace shader output.
The RGB form is verified in a live Silent/Native run: at present 100, all
8,294,400 samples are finite, with RGB zero and alpha one. Generated and
optimized SPIR-V both pass `spirv-val --target-env vulkan1.2`. A native capture
at present 200 remains uniformly black. A second live run selects the RGBA
overlay sample: it records 8,294,400 samples, 409,600 nonfinite vectors and
RGBA zero for every finite vector. Both SPIR-V forms validate for this variant
too. The nonfinite count matches the earlier final-export observation and
places invalid values before final composition, although individual pixels
have not been correlated across those runs.

This locates zero color before the final shader's arithmetic for that opening
occurrence. It does not establish why the sampled storage image contains no
color, whether every later occurrence is black, or whether its producer has
run. The HDR input can legitimately be empty during a logo scene. Follow the
overlay image's writer and lifetime before changing texture decoding,
arithmetic or presentation. A broad format-only lifetime trace selected two
earlier 1024-square targets and did not identify the overlay producer; use a
selector correlated with the actual sampled image.

### JSON value lookup and assignment (2026-09-29, not gameplay)

The title transition now passes `Value::operator[](uint64_t)`. The missing
NID matches the const array-index operator's mangled name. Kyty validates
the value and owned array, bounds the index and returns the existing child;
absent elements share the same immutable null value as object lookup.

A Silent/Native call trace sees index zero of a one-element array and verifies
that the returned object is that owned child. That run then reached the next
unresolved import, `4zrm6VrgIAw[Json2_v1][Json_v1.1]`, the value assignment
operator. Its caller immediately assigns the indexed value to another
32-byte value. The initial copier rejected the host-owned source by asking the
guest virtual-memory registry to read it. Explicit node ownership and string
allocation byte counts now distinguish those HLE allocations from validated
guest mappings. Assignment clones the bounded tree before releasing the
destination, retains its parent context and gives copied children new owners.

A later Silent/Native run verifies the actual return: the destination is
returned, all five nodes have equal content, storage is independent and child
parent links are correct. The copied object contains two strings and two real
values. The run passes the earlier write-back occurrence and reaches the next
missing import after the title transition at 15 minutes 23 seconds:
`Ncel8t2Rrpc[Json2_v1][Json_v1.1]`, `Value::toString(String&) const`. The caller
constructs an empty string, converts a string-valued JSON node and consumes
its `c_str()` before destruction. The string-valued conversion now copies the
bounded contents into independently owned storage, allocating the replacement
before releasing the destination. Other value kinds fail explicitly until
their conversion contract is verified. A later Silent/Native run verifies
two actual returns of 36 and 12 bytes: both match their source, preserve it
and own independent storage. Value assignment's five-node ownership check
also still passes in that run. Nonempty destination replacement and
embedded-NUL cases still need focused tests, deferred until gameplay per the
requested validation order.

The next exercised import, `3qrge7L-AU4[Json2_v1][Json_v1.1]`, is
`Value::getReal() const`. The caller checks for real type four, passes an
owned value and immediately reads a double through the returned pointer.
The getter now validates the value and returns its existing real field.
A Silent/Native run verifies two actual returns: each points into the source
value at offset 16, preserves its exact double bits and leaves all 32 source
bytes unchanged. The earlier string and assignment checks also still pass.
Build, strict boundary and 13-table provenance checks pass; focused tests
remain deferred until gameplay as requested.

That run reaches the title transition at 15 minutes 10 seconds and stops at
`zTwZdI8AZ5Y[Json2_v1][Json_v1.1]`, `Value::getBoolean() const`. The caller
checks type one, passes an owned boolean value and reads one byte through
the return pointer. The boolean getter now validates that type and exposes
the existing field. The next Silent/Native run verifies its actual return:
the pointer is source plus 16, the byte agrees and all 32 source bytes are
unchanged. Real, string and assignment return checks still pass.

After the title transition at 15 minutes 18 seconds, that run stops at
`wLsJlmgEIaI[Json2_v1][Json_v1.1]`, the mutable
`Value::referValue(const String&)` lookup. The caller constructs a string
wrapper, passes an owned object, destroys the wrapper and checks the returned
pointer for null before further lookup. Its ten-byte key is absent from the
five-member object. The bounded wrapped-key lookup now returns the existing
owned child or null without insertion. A Silent/Native trace verifies twelve
actual returns: five existing children and seven absent keys, with source
and key contents preserved on every call. The earlier string, boolean, real
and assignment checks still pass. At the title transition after 16 minutes,
the next missing import is `rDMyAf1Jhug[libc_v1][libc_v1.1]`, whose name hash
matches `__isinff`. The caller passes a float in XMM0, tests the integer return
and clamps only when it is zero. The float predicate follows the public
[libc contract](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/lib/libc/gen/isinf.c):
integer one for either infinity and zero otherwise. The next Silent/Native
run verifies twelve actual finite inputs and zero returns, matching their
captured float bits. It advances beyond all previously missing JSON imports
and this libc call, then stops in compute-wave admission after the title
transition. Infinity and NaN cases still need the deferred focused tests.
Useful rendering, controls and both gameplay acceptance windows remain
unverified.

The new compute gate at `Shader.cpp:3129` reports `SBarrier` at PC 0x40,
but the raw word is SOP1 opcode 0x34, `S_ABS_I32`, according to the local
RDNA2 ISA. The parser's placeholder is not a real barrier and must remain
rejected until the scalar instruction and its SCC result are implemented.
The captured workgroup is 5 by 5 by 1 with one paired guest wave; native
equivalence separately rejects a cross-lane operation at PC 0x90. Preserve
that rejection while correcting the first decoded instruction.

The scalar parser and emitter now implement the ISA's signed absolute value,
including the unchanged INT_MIN bits and SCC for a null destination. With the
subsequent EUD lifetime and image-descriptor corrections, the actual
147-instruction program translates to an 8,105-word module that passes
`spirv-val --target-env vulkan1.2`. Its dispatch is recorded and the same
queue's completed sequence advances beyond that submission before the next
strict failure. Native-equivalence rejection of the cross-lane operation is
preserved. This verifies the exercised translation and completion; focused
integer edge-case tests remain deferred until gameplay as requested.

Two existing string-contract gaps were found while reviewing value ownership:
`LibJson2.cpp:394` uses `strlen` for `JsonStringLength`, although the parser
retains embedded NUL bytes. Verify the length export's guest contract and use
the owned byte count if the full parsed string is required. Separately,
`LibJson2.cpp:406` frees a string's data before `JsonStringAssign` measures its
input; passing that same data pointer or a suffix therefore reads freed
storage. Build a replacement before release when correcting that API. Neither
case is yet observed in the live workload, and neither is gameplay evidence.

### Unsupported-swizzle diagnostic (2026-09-29, corrected 2026-10-02)

The build identified a diagnostic-only type mismatch at
`Objects/StorageTexture.cpp:536`: rejection of an unsupported component
swizzle passed its 32-bit value to a `PRIx64` format, so the printed swizzle
read undefined upper bits. The argument is now widened explicitly, matching
the neighboring unsupported-format report (line 484). The rejection path has
no cheap unit fixture (it needs a live storage-texture object), so the change
is verified by the clean build only. It was never exercised by a run and is
not evidence of the black-output cause.

### Mixed sampled-image numeric selection (2026-09-29, compiler verified)

A separate captured-program review exposes a numeric-selection gap in
`ShaderSpirvImage.cpp:3236`, `Recompile_ImageLoad_VdataVaddr3StDmask`: runtime
unsigned/float selection is emitted only when all sampled views are flat 2D.
With both flat and array views present, the shape branch uses the shader-wide
numeric choice and ignores the per-descriptor unsigned tag. The captured
program copies a complete direct R32_UINT descriptor into the image-load
registers while also binding floating-point array views. Keep numeric selection
inside each selected shape's fetch path. Complete-program replay confirmed
that the unsigned predicate was previously defined but never consumed. The
corrected emitter selects the typed view in each shape, preserves integer bits
at the numeric join and uses that join as the outer phi predecessor. Both lane
banks now emit the unsigned fetch and the complete module passes Vulkan 1.2
validation. Resulting pixels and live GPU completion remain unverified.

### Residency preparation cost (2026-09-29, not gameplay)

`GuestDeviceAddress` now queries every interval of pages still awaiting
import, while existing aliases and tracked snapshots keep their established
refresh and invalidation paths. Newly faulted pages are checked on every
preparation; quiesced invalidation clears the imported-page bitmap.

One warm Silent/Native comparison in the same opening phase measures draw
descriptor finalization at 459.4 ms per present before and 426.8 ms afterward.
The 79- and 73-present windows have 23.04 and 23.00 draws per present and
78.0 and 77.5 dispatches per present; observed throughput is 0.836 and 0.890
FPS. This is a modest local CPU improvement, with one comparison and no
gameplay acceptance. The corrected run passes 800 presents without an import
failure. `fc_script`, the strict boundary gate and the 13-table gate pass.

### Pixel NULL export admission (2026-09-29, not gameplay)

The clean Silent/Native run stopped after 347 seconds at pixel target 9,
`DONE=1`, `VM=1`, `COMPR=0`, `EN=0`. RDNA2 defines this NULL export as a
valid-mask update without a color or depth payload. The decoder now admits
that form for Gen5, preserves per-invocation discard, retains the pixel stage and
omits color outputs for programs whose exports are all NULL. Such programs
also disable Vulkan color writes: live guest state leaves both CB masks
enabled, which otherwise permits undefined fragment outputs to reach color.
Admission is limited to the Gen5 path that carries this export analysis into
the pipeline's write mask.

The actual failing program now executes. Its generated and optimized SPIR-V
both pass `spirv-val --target-env vulkan1.2`, with discard and no color output.
The first corrected run reaches the title transition and exits after 969
seconds on missing `XlWbvieLj2M[Json2_v1][Json_v1.1]`, the const array-index
operator. `LibJson2.cpp` lacks that export despite retaining parsed array
elements; the next HLE work is bounded lookup returning the owned element.
The final build with color writes suppressed also passes the former export
failure and reaches present 324. `fc_script` and the strict boundary gate pass.

The media pause is an explicit guest preload transition after its first
decoded frame, not a decoder failure. Native captures remain black. An
output-preserving probe of the final color export observes 8,294,400
invocations across the full 3840-by-2160 target, 409,600 nonfinite results,
and RGB zero in all finite results. Its CPU constants are populated and its
three textures are bound; the remaining color investigation is upstream of
presentation. Neither the scene transition nor these probes prove gameplay.

### Media file replacement callbacks (2026-09-29, not gameplay)

The later startup scene writes the same two packed-color images that VideoOut
presents, but its media player had no decoder. A live entry trace proves that
the guest supplied a complete file replacement table, which initialization
discarded. The source URI was therefore mapped to a nonexistent host path.

AvPlayer now retains that table and provides its bytes to the host demuxer
through a bounded random-access source. The decoder owns the source until its
worker has stopped, then closes it outside player publication locks. Failed
opens return failure without announcing READY or accepting playback.

A Silent/Native run now opens the actual 4K stream, prepares six decoded frames
and delivers its first frame. Subsequent live state is paused by the guest,
with no backend error; captures at presents 36 and 103 are still black.
The next frontier is the pause/resume sequence and its guest-side condition.
Rendering, controls and the two gameplay acceptance windows remain unverified.

### Async DMA argument layout and counter reset (2026-09-29, not gameplay)

The async compute DMA export used the graphics entry point even though its
ABI omits the engine selector and trailing parser-block argument. Live entry
registers and stack words show four-byte immediate clears; the wrong function
interprets those arguments as one-byte requests and rejects them. The async
entry now decodes its own arguments and uses the shared hardware DMA encoder.

Combined with the GDS byte-address correction in `b3288fb7`, a bounded live
trace now observes the intended counter clears and 48 indirect dispatches
with group counts of one across consecutive cycles. The uncontrolled counter
accumulation is no longer present in that sample. A Silent/Native run reaches
its five-minute limit, advances into the first startup scene and has scored
black captures at presents 28, 172 and 222. The largest completed fence in
the retained diagnostic sample is about 65 ms. These are execution and
contract observations, not visible rendering, controls or gameplay acceptance.
The next color investigation must correlate the later startup scene's final
output with the presented buffer; the older present-20 black-source evidence
alone does not establish that later producer.

### Physical aliases across CPU protection boundaries (2026-09-29, not gameplay)

The intermittent guest-device import failure was reproduced with a 16 KiB
resident span: its first three host pages belonged to a CPU NoAccess segment,
and the fourth to a read/write segment of the same physical mapping. The
alias lookup incorrectly bounded the request by the first protection segment;
its subsequent CPU snapshot could not read those NoAccess pages. The alias
now validates containment against the physical mapping while holding its lock.
It follows the backing offset without changing the guest view's permissions.

The focused reproduction fails on the previous implementation and passes with
the correction, checking byte identity, coherent alias writes, unchanged guest
rights and rejection beyond the backing mapping. A strict 120-second run
passed the former import exit and reached a scored, still-black present 43.
This does not establish gameplay or long-run device stability.

The remaining slow-GPU trace identifies four device-addressed indirect
dispatches whose X group counts grow across consecutive cycles: initially
1/1/2/3, then 4/5/6/7, then 8/9/10/11. Their argument values are unchanged
before and after the existing write-back boundary. A later writer watchpoint
and binding trace identifies a GPU shader whose four GDS appends collapse
onto one incorrectly scaled counter address. The decoder drops their byte
offsets, and both emitters omit conversion from bytes to dwords. The correction
preserves the offset and shares the byte address calculation between both
emitters. A clean Silent/Native rerun reaches 70 presents with a black scored
capture. Four counters now advance separately by one per cycle, so reset
remains broken. A live ABI trace finds that the async DMA entry is incorrectly
aliased to the graphics entry and rejects valid counter clears. The handoff
records that next correction and the separate native-wave limitation. This
is not gameplay acceptance.

### Guest-address imports creating their own residency (2026-09-29, not gameplay)

A clean strict Silent/Native run of `ae78d70a` reached 45 presents with a
uniform-black scored capture, then lost the device in `vkQueueSubmit` after
172 seconds. A bounded timing probe subsequently found about 98 ms per
`GuestDeviceAddressPrepare` call, 63,588 chunk entries and 499 retained tables
at call 500; that run ended with Mesa `execbuf` ENOMEM. A second sample had
64,200 entries, of which 64,066 adjoined another chunk.

A synthetic Vulkan reproduction proves a self-induced growth mechanism:
importing a resident run plus one guard page makes that next page resident.
The next preparation imports it and another guard page, without any new guest
write. The correction imports exact page spans and translates the two pages
of a crossing shader load separately. Two native Vulkan regressions fail on
the previous implementation and pass with the correction: repeated preparation
does not populate untouched neighbours, and 128-byte loads preserve both
pages (or the zero tail when the second page is absent).

The strict 120-second measurement reached a scored, still-black present 39
and stopped at its configured time limit, with a 6.1 GiB service memory peak.
At call 500 it had 1,804 entries, 120 retained tables and 76.5 ms mean
preparation time. This bounds the artificial import growth in that run; it
does not establish long-run stability, correct color output or gameplay.
The experimental physical read-ahead was removed before this run because it
would amplify the newly proven residency mechanism.

Later cache inspection qualifies that runtime evidence: `d8950414` did not
invalidate translator version 56, so retained modules could still use the
old single-span shader loads. The focused Vulkan regression exercised the
new split-load emitter, but the cached workload run did not establish its
integration. Regeneration during the GDS investigation produces a larger,
valid SPIR-V module and spends several minutes in the host driver's compute
pipeline compiler before dispatch. Shader semantic edits must invalidate
the module cache, and compilation time is distinct from a GPU fence stall.

Recorded separately: `ShaderSpirvGuestAddress.cpp`, in the lookup after
`gda_merge`, reads entry zero even when the table count is zero. An empty
registry allocates only the zero prefix, so those entry reads are outside the
buffer. Guard the empty-table path before reading entry fields; the current
workload evidence has not identified an empty table at its remaining failure.

Validation also exposed a pre-existing test mismatch at
`UnitTestEmulatorShaderGetpc.cpp:170`: the rejection test expects a vertex
`s_getpc_b64` with valid program-base metadata to exit, although that path is
now admitted. The same assertion fails with the guest-address source restored
to `ae78d70a`. Update that case to validate successful vertex lowering while
retaining the missing-metadata and invalid-destination rejection checks.

A subsequent clean `d8950414` run stopped after 64 seconds on a 24 KiB
non-writable resident import. The later per-page reproduction and physical
alias correction are recorded above; the residency correction alone did not
resolve that separate failure.

### Raw render alias composition and explicit mip fetch (2026-09-29, not gameplay)

Commit `28db4aa3` materializes a partially written linear storage image from
the raw bytes of its overlapping render-target parents, ordered by observed GPU
write time. A live Vulkan test compared every byte of a mixed-format,
two-block alias against the CPU tile-address equation; the focused geometry
tests and source gates passed. A strict Silent/Native run crossed the former
mixed-parent creation exit and stopped later at Gen5 `IMAGE_LOAD_MIP` opcode
`0x01`. Commit `e169d95b` decodes its explicit level and emits an
`OpImageFetch` LOD operand. Its focused parse/SPIR-V test and the MIMG test
filter passed; a strict run crossed that parser failure and reached 42
presents. This proves the two former exits were passed, not correct pixels.

Native captures at presents 20, 40, and 42 were identically uniform black
(`entropy=0`, one quantized color). The longer follow-up run stopped after
40 presents and 189 seconds at `GraphicsRenderCommandBuffer.cpp:823` with
`vkWaitForFences` device loss. The kernel recorded an Xe timed-out job and
then a high-order atomic page-allocation failure while capturing its
devcoredump. The latter occurred on the coredump path and does not establish
the cause of the timed-out GPU job. The follow-up binary had the tested shader
change but still reported the pre-commit build revision; rebuild before the
next strict run. At present 20, a bounded readback of the packed VideoOut
source found zero RGB bits in all 8,294,400 pixels; its first word was
`0xc0000000`. The PNG conversion did not erase color. The exact registered
range had one VideoOut image and one CPU-uploaded storage-buffer view, with no
live render-target view. An opt-in GPU write-history recorder covered the same
range and found zero direct guest-memory writes before that present. The agent
still reported the loading phase, so this identifies an unwritten early output
buffer but does not prove which later compositor or transition is missing.
The next visual step is to correlate the first final-color producer with the
VideoOut range. No controlled gameplay, five-minute stable run, or playable
regression gate has passed.

Two shorter strict runs also stopped at `GraphicsRenderBind.cpp:4260` because
`GuestDeviceAddress.cpp:288` could not import a resident, non-writable guest
chunk. A bounded stage probe emitted no host-pointer Vulkan failure before
that exit; a later run did not reproduce it and showed optional guard-page
imports crossing unreadable next pages before successful unguarded retries.
The earlier fatal chunk's exact copy/protection failure remains unproven.
Measure its unguarded retry and mapping protection before changing the
device-address table or guest access policy. All temporary probes were removed.

### Scalar and vector lowering, RGBA16 storage, and mixed image ownership (2026-09-29, not gameplay)

The strict Silent/Native workload passed the earlier absolute async indirect
dispatch, scalar 64-bit equality, single-word high-VCC mask, and vector bit-count
failures. The vector bit-count emitter had loaded a float-typed VGPR pointer as
an unsigned integer and stored an unsigned integer through it. Commit
`f9b56b0c` bitcasts at the VGPR boundary; a red/green SPIR-V toolchain test
and a strict run passed the former pipeline-creation failure. That run stopped
at an unsupported Gen5 storage image with format 65. The local format table
and independent decoder observations identify four 16-bit UNORM channels. The
host Vulkan device reports storage-image and formatless read/write support for
`VK_FORMAT_R16G16B16A16_UNORM`. Commit `f4db885b` maps the storage use to that
format. Its focused test failed before the change and passes afterward.

A strict run with that format reached 36 presents and then stopped at
`GpuMemoryCreate.cpp:1825` while creating a linear 960×540 RGBA16 UNORM
storage image with `skip_seed=0`. Its range crosses 12 existing views: storage
buffers, render targets, sampled textures, and storage textures. The saved
GPU-memory database records a larger tiled RGBA16F storage image covering the
whole incoming range. A bounded diagnostic found its content origin is
`Unknown`, as are the render targets'; their object-wide update markers do not
prove which image owns the latest bytes of every tile. The first compute
writer uses a table of tile positions and dispatches 140 groups of 8×8
threads. Its two full-channel image stores can touch at most 8,960 of the
518,400 texels in the incoming image; the table gate can reduce that count.
Skipping the seed is therefore incorrect. The incoming range crosses two
recently GPU-written render targets with different pixel widths, so a single
covering image or object-wide timestamp cannot supply its prior bytes. Trace
per-range ownership and a format-aware raw materialization path before
extending the mixed-parent alias policy. A native frame-36 capture
scored `entropy=0`, one quantized color, and `gameplay_like=false` with
`scripts/kyty_capture.py`; presents are not gameplay evidence. Other bounded
retries stopped earlier at intermittent Xe `execbuf` ENOMEM / `vkQueueSubmit`
device loss in `GraphicsRenderCommandBuffer.cpp:656`. No controlled gameplay
or visual acceptance has been observed.

### Depth mip storage views, D16 arrays, and paired compute frontier (2026-09-28, not gameplay)

A strict Silent/Native run passed a mixed-parent storage-image creation that
previously stopped at `!create_all_the_same`. The incoming R32 image lay inside
live render targets but used depth tiling, so their color texels could not be
copied into it. The parsed compute program has one image store and no other
side-effecting operation. An exact read-only scalar binding supplies a uniform
value that is currently zero; its unsigned compare clears `EXEC`, and the
branch restores `EXEC` from the same zero `VCC` before every store. A bounded
snapshot confirmed the guest value, and the new control-flow check omits only
dispatches for which that value remains zero and the source range can be read
without a writable GPU alias. Focused tests reject a bypass around the gate,
an altered restore, other writes, an unproved binding, and a nonzero compare.
The next R32 depth-tiled image has eight mips (480×270, pitch 512). The old
size fallback covered only mip zero (768 KiB); the GFX10 depth-64KB mip layout
occupies 1152 KiB, with the smaller levels in a shared 64 KiB tail. A focused
red/green size test, detile test, public AddrLib layout comparison, and the
adjacent guest allocation boundary support that layout. The strict run passed
the former upload exit.

The consuming compute shader uses `IMAGE_STORE` with an 8×8 group and a
resource view at base level 6; another descriptor selects base level 7 of the
same eight-level backing. Storage writes now use single-level Vulkan views of
one mipmapped image, while its sampled view retains the full chain. Reuse
requires matching format, extent, pitch, tiling, mip count, and seed policy;
the mixed older color surfaces remain linked without being copied as depth.
Four focused tests and a strict Silent/Native run passed both former
`!create_all_the_same` exits. The next depth-reference binding had a 2048×2048
D16 depth-64KB array descriptor with raw depth two, normalized to three
layers. A bounded trace found the complete 24 MiB span physically mapped and
overlapping two CPU-uploaded Texture views with no write-back ownership.
Overlap summaries group equal type/relation objects; source admission now
requires matching individual provenance and rejects GPU-materialized or
write-back-capable Texture aliases. The full overlap count also bounds a
provenance scan that hits its page cap after visiting every matching object.
The matched GFX10 16-pipe Z_X equation XORs array-layer bits into within-block
address bits 8–11; CPU upload now detiles every layer with its slice index.
The storage-backed inline path remains limited to one layer because its shader
fills only layer zero. Focused source-classification and layout tests pass.

A subsequent strict Silent/Native run passed that D16 binding and reached
present 32, then stopped at `paired-wave dispatch admission unsupported` for
an instruction represented as `SBarrier` at PC 0x66c. A bounded retry
identified SOP2 opcode 0x25, `S_BFM_B64`: the parser had substituted a barrier
for this bitfield-mask operation. Its real 64-bit mask semantics are now
lowered in two 32-bit SPIR-V words without writing SCC, and paired admission
requires an exact scalar tuple. The barrier gate also requires the real SOPP
opcode 0x0a; a placeholder opcode stays rejected. Focused red/green tests
and SPIR-V toolchain validation passed. A strict run with this change passed
PC 0x66c and stopped next at `BufferAtomicUmax` at PC 0x74c. The captured
atomic is an aligned, no-index, no-offset, no-return unsigned dword maximum
through a unique, writable, guarded raw V# binding. Paired admission now
requires that exact tuple and binding; the existing Gen5 atomic emitter emits
one guarded operation per bank. Focused red/green admission, resource-use, and
SPIR-V tests pass, including negative return, index, and binding cases. Two
strict runs passed the former PC 0x74c gate and stopped at PC 0x560. A
bounded trace of that 368-instruction program found six mapped EUD loads, a
full write to their former `s12:s13` base by `SBufferLoadDwordx2` at PC
0x560, two later ordinary reads, and 18 static branches, all forward.
Paired admission now tracks possible and definite writes through the parsed
control-flow graph: every mapped EUD load requires the original pair on all
paths, while an ordinary read requires a full replacement on all paths.
Partial writes and unresolved branch targets stay rejected. Eight focused
tests cover reuse, a back edge, a skipped write, and the earlier guard cases.
A strict diagnostic explicitly recorded admission of this program before a
later storage-texture overlap exit. This confirms admission, not correct GPU
execution or gameplay. A separate retry stopped earlier at a storage
write-back dependency, which needs a causal reproduction. Native wave-width
equivalence remains rejected at PC 0x208 for a lane-crossing read. A native
frame-10 capture was uniformly black (`entropy=0`, one color). No controlled
gameplay has been observed. Several bounded retries also stopped before shader
admission when Xe reported `execbuf` ENOMEM and `vkQueueSubmit` failed in
`GraphicsRenderCommandBuffer.cpp:656`; correlate live GPU allocation budget
and competing Vulkan clients before changing submission policy.

### Bounded storage-image overwrite and next mixed alias (2026-09-28, not gameplay)

Linking older render, storage, and sampled views around a new storage image
depends on proof that its first dispatch writes every texel. The prior
write-only image-use bit alone did not provide that proof: it allowed seed
skipping when symbolic tile coverage was zero. Commit `4f8f0719` makes zero
coverage retain the seed and admits a bounded-grid pattern only when group
and lane IDs form the full 2D grid, the shader's sole `EXEC` guard excludes
coordinates beyond a scalar width/height pair, a full-channel image store
reaches every in-bounds lane, and a read-only snapshot of that pair matches
the destination extent at dispatch time. Altered branches, coordinates,
masks, source binding, or missing snapshots fail closed. A focused test failed
on the old fallback and 16 focused graphics/image tests now pass.

The strict Silent/Native run passed the former three-parent storage overlap
after that runtime proof. It entered the logo-level startup and stopped at a
different storage-image overlap with nine linked parents: six render targets,
two storage images, and one storage buffer; eight relations cross the new
range and one existing render target is contained within it. No native
capture or controlled gameplay was obtained. Determine the new image's
actual first-write coverage and each parent's content provenance before
extending the overlap policy. A prior uniformly black native frame remains
scored evidence from this bring-up phase.

Commit `69586773` proves a second whole-image pattern in paired wave64
compute. A symbolic 64-lane check follows the shader's local-coordinate
permutation, verifies that the tile contains each texel exactly once, and
requires two full-channel stores to the same dynamic image descriptor on
complementary `EXEC` paths. It rejects destination reads, missing or altered
branches, coordinate clobbers, incomplete masks, and unbound image reads.
Only a matching 2D dispatch can skip the initial seed. The positive test
failed before the change; the focused tests and a strict Silent/Native run
passed the former nine-parent overlap. The next first strict failure is in
`GpuMemoryCreate.cpp` at `!create_all_the_same`: a 768 KiB storage image lies
inside six older views (four render targets, one storage image, and one
storage buffer). The new image's format, first-write coverage, and live byte
owner remain to be established. No native capture or gameplay was observed.

Commit `2c589a47` proves the first writer of that six-parent storage image is
also a full overwrite. Its paired wave64 shader has one full-channel store
to the destination, no destination read or divergent path, and a bijective
permutation of the 64 local lane coordinates. The 2D dispatch covers every
valid destination texel. A positive test failed before the change, 19 focused
graphics/image tests and the source-boundary and graphics-table gates passed,
and a strict Silent/Native run crossed the former alias. That run stopped
later at unknown Gen5 VOP3 opcode `0x369` in `ShaderParseVOP3.cpp`; the local
RDNA2 ISA identifies it as `V_CVT_PKNORM_U16_F32`. Its actual encoded tuple
and lowering must be verified before adding parser support. The run reached
only the logo; it did not establish a playable or correctly rendered state.

A bounded trace found two VGPR float sources, a VGPR destination, and no
modifiers for opcode `0x369`. Commit `f41b2f06` decodes the two-source tuple
and uses the SPIR-V normalized-pair conversion, which clamps, scales, rounds,
and packs the first source into the low half. The prior emitter converted
clamped floats directly to integers and had a float/uint type mismatch.
The synthetic parser/emitter test failed before the change and passes with
SPIR-V validation; both affected targets build and the source/table gates
pass. The strict Silent/Native run passed `0x369` and stopped at the adjacent
unknown VOP3 opcode `0x368` later in the same program. The local RDNA2 ISA
identifies that opcode as the signed normalized pair conversion. Verify its
observed tuple and numeric lowering before enabling it. No gameplay or
correct-rendering claim follows from this parser advance.

The bounded trace for `0x368` found a tied VGPR source, an inline zero
second source, a VGPR destination, and no modifiers. Commit `6e0600ea`
decodes the two-source form and lowers it with validated SPIR-V
`PackSnorm2x16`; the old shared emitter also omitted the normalization
scale and passed a uint to a float selection. The synthetic red/green test
and prior unsigned test pass. Both targets build and the source/table gates
pass. One strict run was interrupted by the previously observed Xe
`execbuf` ENOMEM / `VK_ERROR_DEVICE_LOST` in `GraphicsRenderCommandBuffer.cpp`.
A single strict retry passed the former `0x368` parser exit and stopped at a
lazy `Agc_v1.1` import in `RuntimeLinker.cpp:1081`. Its exact contract was
unverified at that point. This is still logo-stage execution with no controlled gameplay.

A bounded live call trace and independent local implementations established
that the async indirect-dispatch import receives an ACB, a full 64-bit
argument address, and a modifier. The native packet carries the absolute
address in two dwords; the DCB form remains relative to the indirect-argument
base. The new synthetic packet test failed to link before implementation and
passes with its full high address preserved. Both affected targets build and
the source-boundary and graphics-table gates pass. A strict Silent/Native run
crossed the former lazy import and both indirect-dispatch packet forms,
reached the logo-stage level, then stopped at unrecognized SOPC opcode
`0x12` in `ShaderParseSOPC.cpp`. A native capture at 28 presents scored
`low_entropy` (one color bin). Neither the presentations nor that capture
prove correct rendering or gameplay; the SOPC operands and 64-bit equality
lowering are the next verified frontier.

### Strict compute/storage and libc string frontier (2026-09-28, not gameplay)

On the reference Gen5 workload, the owned Linux build in strict Silent/Native
mode now passes the former dynamic-storage write-back, fused ES+GS register,
offset image-sample, storage-image alias, render-target-format, first
audio-propagation imports, bounded libc string imports, and three later
storage-image overlap cases. A blockwise GPU copy now carries the live bytes
from a compatible tiled render target into the first partially written storage
image. The strict run passed the former single-parent
`RenderTexture Crosses StorageTexture` exit. With SDL's Wayland backend, a raw
32-to-16-bit tiled alias transfer now passes the former `!create_all_the_same`
exit for the second partially written storage output. It reads the live source
image, swizzles its raw words, and uploads the destination in the current
command buffer. The multi-parent policy requires a newer packed render target
whose guest range covers every other parent's overlapping bytes and whose
full host extent received a render-pass `CLEAR`. The destination still takes
its uncovered prefix from guest memory. A focused red/green test exposed that
the old two-byte tile-27 path mapped 32,768 texels to only 8,192 distinct
offsets per block. The revised mapping is bijective and aligned over 64 KiB;
a synthetic cross-format test checks the raw word address mapping, and the
embedded compute shader passes `spirv-val`.

The 180-second strict run reached present 17 without the former structured
exit. Its native capture is uniform black (`entropy=0`, one quantized color,
`gameplay_like=false`). A second 110-second run with the full-clear gate
reached present 9 and recorded the five-parent storage creation as linked;
it ended at its runtime limit without a guest error. Performance telemetry
measured that creation near 30 ms while frames took about 8 seconds. The
source image bytes, revised two-byte tiling on real hardware, and cause of
black/slow output still need direct validation. There is no controllable
gameplay evidence.

A temporary audio probe counted 3,000 empty blocking pushes in about 34
seconds at 512 samples and 48 kHz, close to a real-time audio cadence. GDB
placed that wait on thread 60 and the internal flip on thread 6 in the same
strict run. This rules out a direct sleep on the flip thread; it does not
identify the remaining frame-time cost. The probe was removed.

Bounded GDB and packet probes now attribute much of the roughly eight-second
frame interval to repeated one-second `WAIT_MEM64` fallbacks in the graphics
ring. The slow waits compare a 64-bit label against `1` with a low-32-bit mask;
the live word stays `0` and the submission tracker finds no pending producer.
In contrast, waits paired with a preceding immediate `RELEASE_MEM` find their
current submission's producer. A guest store initializes the slow label to
`0`; a later graphics-label completion writes `1` only after several fallback
timeouts. No compute ring was mapped or advanced during the measured opening
window, and the first 40 dispatches belonged to the graphics queue. Returning
from DCB submit immediately after command-buffer snapshotting did not improve
the measured pace (about 0.11 FPS in a bounded A/B run), so that timing change
was removed. The earlier producer had not yet been identified; changing the
wait timeout or fabricating the label would conceal it. This explains slow
progress, not the uniform black native captures.

The first color-chain readback located a separate raster-state defect. The
context register decoders handle viewport indices 0 through 15, but
`HW::ScreenViewport` allocated only 15 entries. Writes to viewport 15 landed
in the following generic-scissor fields; a strict trace observed the bit
patterns for `0.5` and `1.0` as its left and top coordinates, leaving Vulkan
with a zero-area scissor. A focused test reproduced those exact integers
before the array was expanded to 16 and passed afterward. The corrected
strict trace resolves the HDR writer's scissor to its full 2432×1368 target.
Nevertheless, a 125-second strict run reached present 15 with every retained
capture uniformly black (`entropy=0`, `gameplay_like=false`), and its measured
pace remained about 0.12 FPS. Direct readbacks showed the packed VideoOut
image, its 2432×1368 HDR source, and the preceding 1920×1080 HDR source all
zero in the opening presents. The viewport defect is corrected, but visual
output and playability are not established. Trace the first nonzero producer
upstream of the HDR postprocess chain before changing the final compositor.

The next strict probes reached the first full-screen HDR source writer. Its
2,073,600 fragment exports and the immediate 1920×1080 attachment were all
RGB-zero. The guest-uploaded single-channel input was readable but contained
zero in every word of its 8,847,360-byte span at that draw. A second
full-screen branch likewise exported zero RGB over 2,073,600 fragments; its
storage-image producer is a compute upscaler that reads an earlier HDR target,
so it is not an independent source of scene color. First-draw censuses at the
opening present and at present 11 each found the same 14 pixel shaders; every
first occurrence used three nonindexed vertices and no depth attachment.
Those samples do not prove that every repeated draw has the same shape. A
scheduled diagnostic `cross` was delivered at present 2, yet a present-6
native capture still scored zero entropy and the input counters recorded no
ordinary guest pad reads. At this point the live boundary was the missing nonzero scene
source or scene submission, together with the unresolved graphics-label wait;
no evidence justifies changing the compositor, sampled-image selection, or
audio memory contract to force visible pixels. The temporary probes were
removed before the clean build.

A later paired-submit trace identified the first slow label's producer. The
opening ACB on one async queue contains five waits for that label; a later
DCB contains its matching `RELEASE_MEM`. Kyty had sent both through the
graphics ring, so the producer could run only after the ACB's timeout
fallback. ACB handles now retain separate ordered compute command processors,
and GPU write-back waits until every queue using a writable object has
completed. Two focused tests and the boundary/table gates passed. In the
first strict run after the correction, the agent reached present 15 in about
36 seconds (snapshot FPS about 0.44); 32 `WAIT_REG_MEM` suspensions remained.
The native present-19 capture still scored `entropy=0`, one color bin, and
`healthy=false`. The run then exited on missing strict import
`S5JxQnoGF3E[Json2_v1][Json_v1.1]`, identified as `Parser::parse`. This
does not establish gameplay or a controlled performance benchmark. The next
work is a contract-backed JSON data path and the still-missing scene source.

The JSON data path now parses bounded UTF-8 input into owned values and
supports object lookup by a bounded C-string key. A focused test covers a
nested document, numeric value, missing key, Unicode escape, malformed trailing
input, and destructor cleanup. A strict Silent/Native probe reached the former
parser import with a 14-byte document: parsing returned success with an object,
and a seven-byte key lookup found an array. It then stopped at the next
unresolved `SHtAad20YYM[Json2_v1][Json_v1.1]` (`Value::getType`) import, after
19 presents in about 53 seconds. That type accessor now returns the value's
guest-visible enum, with its own focused assertion. The bounded probe's
result-only diagnostics were removed. Error codes for malformed input and
invalid arguments remain inferred from secondary implementations rather than
measured on hardware. A subsequent strict Silent/Native run with the final
JSON path passed the former `getType` import and reached present 22, but the
host Vulkan submission failed with `vkQueueSubmit result=-4` at
`GraphicsRenderCommandBuffer.cpp:656` after the Intel driver reported repeated
`execbuf` ENOMEM. The service used 3.8 GiB at peak under a 16 GiB limit, while
the host reported 15 GiB of available RAM and full swap. These counters do not
identify whether device memory, driver address space, or another resource was
exhausted. Reproduce and measure the Vulkan allocation/submission state before
changing the queue or memory policy. A repeat with the same binary did not
reproduce ENOMEM: the agent reached 21 presents in strict mode, then the guest
stopped at missing `RBw+4NukeGQ[Json2_v1][Json_v1.1]` (`Value::count`). Near
that point, the agent reported 159 live GPU memory objects and a maximum of
three submissions in flight; Xe fdinfo attributed about 1.58 GiB resident
VRAM and 665 MiB resident GTT to the process. This excludes deterministic
failure at sequence 2000 and leaves intermittent host/device failure to
measure separately. No captured image or controlled gameplay validates the
scene.

`Value::count` now reports parsed array and object sizes, with focused checks
for array, object, and scalar values. In the next strict Silent/Native run, the
guest passed the former count import, logged that a level had started, and then
stopped at missing `CPLV6G-eXmk[AudioPropagation_v1][AudioPropagation_v1.0]`,
catalogued as `sceAudioPropagationSystemRegisterMaterial`. The 61-second run
did not establish that the level image was displayed or controlled. The audio
material ABI, output handle, lifetime, and acoustic behavior require a guest
call trace before implementation; secondary implementations in the local
comparison either return success without state or depend on unverified record
sizes.

A bounded strict call probe of `SystemRegisterMaterial` found a live system,
a readable 64-byte material record with descriptor ID `0x010107d1`, and a
writable eight-byte output handle. Kyty now retains the complete opaque record
under that system, enforces its material count, and releases it with the
system. A focused lifecycle test passed. The next strict Silent/Native run
passed the former material import and stopped at missing
`kIdb+iQUzCs[AudioPropagation_v1][AudioPropagation_v1.0]`, catalogued as
`sceAudioPropagationSystemSetAttributes`. Registration alone does not
implement acoustic propagation. The request for a native capture did not
complete before the missing import stopped the run; no visual result is
established.

The next strict call to `SystemSetAttributes` supplied one 24-byte entry:
ID `0x20000`, an eight-byte value pointer, and size eight. Guest code wrote
only the low ID dword, so the following dword cannot be treated as a required
flag. A bounded live-object probe proved the pointed-to value was a room
handle owned by the system, not a material handle. Kyty retains that room
association and clears it when the room is destroyed; unsupported attribute
IDs remain rejected. A focused red/green lifecycle test passed, and a strict
Silent/Native run passed this import before stopping at missing
`ht-QXT3zGxo[AudioPropagation_v1][AudioPropagation_v1.0]`, catalogued as
`sceAudioPropagationSystemGetRays`. A native capture from the preceding probe
was uniform black (`entropy=0`, `healthy=false`); there is still no gameplay
evidence.

The strict `SystemGetRays` call passes a writable count initialized to 64 and
a caller-owned array of 64 records, each initialized by the guest with a
0x58-byte descriptor. Guest code scans the resulting records for nonzero
coordinates. Kyty's CPU-only propagation system has no ray producer, so its
current result is an empty set: it writes count zero and leaves the caller's
array untouched. A focused red/green test passed. The following strict run
passed this import and reached a logged level start, then stopped in graphics
at `unknown mimg format for opcode: 0x24`, `dmask: 0x1`. The present-32 native
capture still scored `entropy=0`, `healthy=false`. The empty result does not
establish acoustic propagation or a visually correct level.

The graphics decode failure was a Gen5 `IMAGE_SAMPLE_L` (`0x24`) with
`DIM=1` (2D), one scalar destination selected by `DMASK=0x1`, and NSA
addresses. The local RDNA2 ISA defines this as an explicit-LOD texture
sample; the parser previously accepted only three- or four-component forms.
The new scalar emitter reads the three selected address VGPRs, samples with
the explicit LOD, and writes only the red component. A synthetic parse/SPIR-V
source test passed, and the strict run passed the former MIMG exit. It next
stopped at `!create_all_the_same` for a storage texture with three containing
GPU-memory objects. The present-34 native capture remained uniform black
(`entropy=0`, `healthy=false`). This is shader admission, not visual proof.

The SDL X11 backend can stop earlier at zero presents on this host.
At 78 draws, 133 dispatches, and 549 submissions, the graphics worker waits
for flip completion while the main thread waits inside SDL's X11 window show
for a map event; the guest submitter then waits for decode completion while
holding a guest mutex. The X11 window remained unmapped. A standalone program
linked to the same SDL build reproduced the blocked hidden-window show with
and without `SDL_WINDOW_VULKAN`; the same program returned normally with
`SDL_VIDEODRIVER=wayland`. On Linux Wayland sessions, the host window now asks
SDL to try `wayland,x11` when no video driver was explicitly selected. A strict
Silent/Native run with the driver unset reached the storage overlap exit above.
This identifies and routes around an environment-dependent host-window blocker,
not a rendering or gameplay fix.
Showing the window early did not resolve the X11 stall and was reverted.

`AudioOut2ContextPush` also returned immediately for empty PCM in a blocking
context: a bounded strict probe observed 10,000 calls in 529 ms. A focused
red/green test now requires one grain of pacing, and the implementation releases
the context mutex while waiting and revalidates its lifetime afterward. The
strict run confirmed the changed path executes, but X11 still stopped at the
first flip; audio pacing alone did not advance the graphics frontier.

- A scalar-loaded storage descriptor consumed by vector stores or atomics is
  now classified writable and merged with its equal-descriptor users. Compute
  dispatches using guest device addresses receive ordered GPU write-back
  before descriptor preparation. The strict run passed the former zeroed
  linked-list input; standalone shader replay alone was not the acceptance.
- The observed fused ES+GS program receives the GS user-data address in
  `s0:s1`; the indirect ES resource register is parsed. The observed 2D
  `image_sample_lz_o` masks 1 and 2 now produce their selected components.
  Translator identity 54 separates these modules from older cached SPIR-V.
- For an observed wave64 storage-image writer, symbolic lane/group analysis
  proved four stores cover each 16×16 tile without reading that destination.
  The 152×86 dispatch covers the 2432×1368 image, so it may omit an initial
  guest-memory seed while retaining overlapping live render and storage
  parents. GDB confirmed skip mask `0x2` at the actual dispatch. The strict
  run passed the former `!create_all_the_same` exit. The proof rejects
  incomplete coverage and destination reads in focused tests.
- A later native 16×16 reduction dispatch loads its destination descriptor
  after sampling a separate source, then writes one R32 texel per selected
  2×2 block. A bounded symbolic proof tracks the EXEC reset, even-coordinate
  predicate, and 8×8 output tile; 152×86 and 76×43 dispatches cover the
  observed output extents. The fully overwritten images can retain live render,
  storage, and sampled-image parents in the captured overlap relations. Another
  8×8 dispatch writes two destinations under bounds read from a buffer; the
  observed bounds match its 1216×684 output and the computed 152×86 grid
  covers it. Focused tests and strict runs passed all three former exits.
- The next render target was 3840×2160, tile `0x1b`, with color format
  `0x9`, UNORM type `0`, and alternate component order `1`. A focused
  red/green test and the next strict run confirmed the 4-byte
  `VK_FORMAT_A2R10G10B10_UNORM_PACK32` mapping moves beyond that exit.
- The first partial storage output has the same RGBA16F format, sample count,
  and 64 KiB tile layout as its single live render-target parent. Their
  overlap begins on a tile boundary. A copy plan maps each shared guest tile
  to source and destination image rectangles, while CPU upload seeds the
  non-overlapping range. It rejects partial tiles, mismatched formats,
  incompatible extents, and unaligned ranges. A red/green byte-address test
  and the strict run verified the former exit is passed; neither proves the
  resulting pixels are visually correct.

The guest's `sceAudioPropagationSystemQueryMemory` call passes a 56-byte option
record and a 48-byte memory record, then reads the CPU size from memory-record
offset `+0x18`. The new CPU-only HLE reports its own 64-byte caller-owned
system state, validates and initializes that memory in `SystemCreate`, and
tracks room lifetime under a live system. A focused test and strict run passed
the former query and room imports. This establishes only those lifecycle
operations: no acoustic rendering or broader library compatibility is proven.
Two local secondary emulators report a 1 MiB size and 256-byte alignment and
return success from the remaining entry points; one explicitly ports the
other. They provide no independent hardware measurement of the size, alignment,
creation behavior, or return codes. Keep the guest-traced record layout and
Kyty's current CPU-only HLE until a real trace or independent published
contract establishes a broader one.
The former `.agx` graph-name assertion came from a wrong libc NID binding:
`NC4MSB+BRQg` was handled as an errno-style formatter, which overwrote the
destination. The guest passes destination, capacity, source, and count and
expects `strncat_s` to append; an independent local export catalog labels the
same NID `strncat_s`. A hardware watchpoint showed the previous binding
replaced an existing name with only `.agx`. A focused test and strict run
confirmed the corrected append contract and advanced to the substring import.
Keep raw guest strings, disassembly, and addresses in scratch.

The subsequent `Xnrfb2-WhVw` import is `strnstr`: two independent local
emulator catalogs agree on the name, and the live guest passes haystack,
needle, and a haystack byte limit. A focused bound test and strict run passed
that import, reached later load phases, then stopped at the separate mixed
`StorageTexture` overlap. The later reduction and bounded dual-output writers
were traced separately and passed as described above. Their proofs do not
apply to the current partially written image.

The first present uses `VK_FORMAT_A2R10G10B10_UNORM_PACK32` at 3840×2160.
Native capture now converts both packed 10:10:10:2 channel orders to RGBA8 and
also accepts 8-bit UNORM sources. A focused channel-order test passed, and the
strict Silent/Native run saved four 1280×720 PNGs with matching metadata. All
four have the same black-image hash and fail the official scene/gameplay score;
presentation and capture alone do not establish rendering or playability.

### Generic wave64 compute frontier (2026-09-26, not gameplay)

Strict Silent/Native runs on the reference workload now admit, translate and
create pipelines for several wave64 compute programs that previously stopped at
the paired allowlist. No presentation, input or gameplay state is claimed.

- A wave64 program that is proven wave-width independent runs one guest lane
  per invocation on the native recompiler over the unchanged guest workgroup
  (`ShaderComputeWaveNativeEquivalence`). The fail-closed proof rejects lane
  crossing, mask/data mixing (flow-sensitive), SCC produced by mask logic and
  any scalar value written inside an EXEC/VCC-conditional region that is live
  at its join, which is what makes an independently skipping host subgroup
  equivalent to the guest wave.
- Paired lanes no longer depend on a per-opcode allowlist: per-lane VALU,
  vector memory and DPP16 instructions re-emit the native lowering once per
  bank; all VOPC compares ballot a native lane predicate into the architectural
  mask; DS accesses are followed by a subgroup barrier (one guest wave is one
  converged subgroup); uniform scalar instructions, saveexec and GETPC use the
  native lowering on the architectural SGPR/VCC/EXEC words; `v_mbcnt`, GDS
  `ds_append`/`ds_consume` and `v_*_co_ci_u32` have dedicated wave emitters.
- Partial waves and `USE_THREAD_DIMENSIONS` dispatches are admitted: the
  initial EXEC is the ballot of lanes inside the workgroup and inside the
  per-dispatch thread limits (runtime metadata, not a cache key), and every
  EXEC write is clamped to it.
- ISA decoder corrections (RDNA2 tables, confirmed against the live guest
  operands): VOP3 `0x365`/`0x366` are `v_mbcnt_lo`/`v_mbcnt_hi` with a VGPR
  destination (the former `v_bcnt_i32_b32` decode did not exist); single-address
  DS instructions use the 16-bit `{OFFSET1, OFFSET0}` offset; `ds_read`/`ds_write`
  `b64`/`b96`/`b128` move all their dwords; `ds_wrxchg_rtn_b32` exchanges;
  `s_bitcmp*`, `s_ff1_i32_b*`, `s_trap` (no trap handler: not taken) and
  `v_cmp_*_u64` are decoded instead of the `SBarrier` placeholder.
- Guest-memory device addressing: GPU-visible guest mappings are imported as
  host-pointer device memory (`GuestDeviceAddress`); shaders translate computed
  guest pointers through a device-resident range table and read through
  `PhysicalStorageBuffer` pointers (scalar loads off the extended pointer,
  `image_bvh_intersect_ray`). Write-back coherency before such reads is not
  implemented yet.
- `image_bvh_intersect_ray` (RTIP 1.1, 32-bit pointer, non-A16) follows the ISA
  T#/VGPR contract and AMD's open-source ray-tracing library for node layouts,
  box/triangle arithmetic, sorting and barycentric rotation. Return mode 0's
  `triangle_id` is taken from the node's triangle-id dword (unverified).
- Paired programs with branches run as a block dispatcher (loop around a switch
  on the guest block id); branch conditions are uniform per subgroup. Their
  SPIR-V is not run through spirv-opt: SSA promotion of the per-bank registers
  across the dispatcher loop makes the driver compile unbounded (offline: -O and
  -Os exceed 12 GB; unoptimized compiles in 25 s / 0.9 GB).
- Wave64 programs proven wave-width independent now take the native route even
  when a paired layout exists (half the generated code).
- Current first failure (strict Silent, frame 1, 0 presents): the compute
  dispatches that use guest device addressing and `image_bvh_intersect_ray`
  reset the render engine (`Engine reset engine_class=rcs`, `Timedout job`,
  `vkWaitForFences` result -4, sequence 54). Excluded in
  `docs/kyty-runtime-graphics-investigation-handoff.md`: an infinite block
  dispatcher, a device-address load that merely overruns its imported span,
  and `s_barrier` scope. A scalar load whose descriptor base is a guest
  address still indexes the storage-buffer array with that word; decoding the
  descriptor in a per-load helper is valid SPIR-V but pipeline creation does
  not finish. Skipping those dispatches reaches a vertex `s_getpc_b64` with
  no compute program base (`ShaderSpirvProgramAddress.cpp`). Not gameplay.

Recorded, not yet fixed:
- `source/emulator/src/Graphics/ShaderComputeWaveAnalysis.cpp:759` uses a
  broad scalar fallback after exact paired-wave classifiers reject a tuple.
  A malformed `SMovB64` pair and `SGetpcB64` were reclassified as
  `ScalarGeneric` in the existing compute-wave tests. Replace the fallback
  with audited instruction/tuple admission before using those tests as a
  strictness gate; do not treat a generic scalar register shape as sufficient.
- `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp:6495`
  expects the older direct LDS `ds_write_b32` SPIR-V text. The current emitter
  no longer produces its two asserted instruction strings in that fixture.
  Inspect the emitted program and update the test to check the actual LDS
  address/write contract; a source-string mismatch alone does not prove a
  runtime LDS defect.
- `source/emulator/src/Graphics/ShaderParse*.cpp` still map about 600 opcodes
  to an `SBarrier` placeholder and the DS parser keeps "treated as" substitutes
  (for example `ds_write2_b32`, `ds_rsub_u32`, `ds_cmpst_b32`). Each silently
  changes guest semantics; decode each from the ISA when it is reached.
- The native `ds_read`/`ds_write` emitters previously ignored EXEC; they now gate
  on it, but the other native DS atomics without a return value still do not.
- Storage image stores used the sampled-3D flag to pick three coordinates; they
  now follow the declared 2D (optionally arrayed) storage type. Real 3D storage
  images remain unsupported.
- Kyty names float constants with six decimals, so distinct small values (for
  example 2^-24 and 0) collide; emitters must use bit patterns for them.

### Paired compute admission frontier (2026-09-22, not gameplay)

The restricted wave64-on-native32 compute path is connected to direct and
indirect dispatch admission on this dirty `main`: checked guest/physical local
sizes and host capabilities precede pipeline and descriptor work, and
translation identity separates the two layouts. This remains an instruction
allowlist, not general wave64 emulation. Strict Silent/Native runs first
stopped at PC `0x0` (`S_INST_PREFETCH`), then PC `0x4`
(`S_LOAD_DWORDX4`), then PC `0xC` (`S_LSHL_B32`). Exact SOPP opcode
`0x20` modes 1–3 are admitted as instruction-cache hints, with parser aliases
rejected. The latest owned build and strict rerun completes resident loading
and stops at PC `0x10`: `SWaitcnt is outside the paired compute-wave admission
set` (dispatch mode `0x41`). The cache-hint interpretation follows the
[AMD RDNA2 ISA](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture).
This is admission progress only; no guest compute
dispatch, presentation, input-to-gameplay, capture, or playability gate passed.

At PC `0x4`, the captured tuple loads `s16..s19` from type-5 EUD pointer
`s12:s13` at byte offset `0x50`. A paused diagnostic found the nonzero pointer,
24 declared EUD dwords, zero GLC/DLC, and the normal resource collector's
storage-buffer descriptor mapping for EUD dwords 20–23 with a consumer at PC
`0x14`. The paired admission now runs **after** resource mapping and requires
that exact per-PC producer, offset, span, flags, binding source, and consumer
record. The emitter fails closed if any mapped word is missing. A separate
128-logical-lane Vulkan probe on Arc A770 passes all four descriptor words in
both banks of two waves, but supplies its mapping manually: it proves mapped
SPIR-V lowering, not production mapping or a guest dispatch. The collector
diagnostic and its focused mapping tests provide distinct provenance evidence.
Because the EUD base pair is not initialized as ordinary SGPR data, paired
analysis rejects each ordinary word read until every control-flow path writes
that word. It rejects mapped EUD loads after any path writes either word.
The later per-word lifetime correction permits separate ordinary writes once
mapped EUD loads are dead. The gate keeps combined SGPR plus
`smem_imm_offset` loads rejected; `recompile_sload_from_extended` does not add
that extra offset. Unrepresented SMEM reserved bits still require a fail-closed
decoder check. A later EUD load at PC `0x490` lacks a verified mapping; inspect
its actual consumer only if it becomes the first strict frontier.

The PC `0xC` scalar shift is admitted only for a plain one-word SGPR input,
inline shift 0–31, and `VccHi` destination. Two synthetic GPU cases on Arc
A770 verify nonzero/zero results, SCC, VCC-low and EXEC preservation across
128 logical lanes. The preceding complete regression checkpoint passed 1221
unit tests with eight skips, three compute/graphics integrations, emulator
boundaries, graphics-table provenance and `git diff --check`; the newer EUD
guard passed 23 focused units and the shift passed 20 focused units and the
paired GPU integration. Full regression after these additions is still due.
A separate portability risk remains in
`GraphicsRenderPipeline.cpp`: native W32 compute shaders using subgroup
operations do not currently request subgroup size 32, so a host whose default
subgroup is not 32 can execute incorrect lane-width semantics. The Arc A770
used for these probes defaults to 32, so this has not reproduced locally.
Require size 32 only for subgroup-sensitive W32 modules when supported, without
the paired path's full-subgroup flag; otherwise reject that sensitive pipeline.
Even a forced size 32 does not by itself prove subgroup lane ordering.

### Latest startup shader frontier (2026-09-22, not gameplay)

The latest strict run passes the texture-resource and scalar-load mapping
ceilings described below, then stops at the missing `S_GETPC_B64` emitter.
Dynamic scalar-load mappings now use copy-on-write records bounded by parsed
instruction count, not a 64-entry parallel-array table. A 65-load shared-resource
fixture failed on the prior ceiling and passes with preserved mapping lifetimes
and copy isolation. Independent review found no blocking migration findings.
Three separate regression processes passed 49 shader, 225 state and 231 packet
tests, and both compute and graphics integrations passed. This does not erase
the separately recorded combined-suite retirement-counter failures or establish
any presentation/gameplay. The next contract is a runtime, cache-relocatable
compute program address; fused graphics program addresses remain unrepresented.

The subsequent compute GETPC implementation passes that stop and reaches an
unsupported vector 64-bit comparison. Its program base is per-dispatch metadata
in the existing push/UBO path, while cache identity contains only presence and
layout. The 55 shader tests pass. On Intel Arc A770, thirteen numeric GPU cases
pass, including two-base same-pipeline GETPC dispatches through push and UBO
metadata, low-word carry, empty EXEC execution and preservation of an active
EXEC low bit. These do not prove nonzero EXEC high-half behavior. Independent
production and harness reviews found no remaining blocking findings. No
presentation or gameplay is proven.

The next comparison consumes a vector-comparison-produced VCC mask. The current
backend stores that mask as a per-invocation boolean, not the architectural
packed wave mask; comparing that boolean numerically with zero would therefore
be incorrect. A bounded dispatch-entry capture reports initiator `0x41` and a
256-invocation workgroup, while the host supports subgroups of at most 32.
The public [GFX10.3 register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h)
place `CS_W32_EN` at bit 15; the [LLVM PAL wave-size regression](https://github.com/llvm/llvm-project/blob/main/llvm/test/CodeGen/AMDGPU/mixed_wave32_wave64.ll)
corroborates its use for wave32. The captured dispatch requests wave64.

Do not replace the numeric whole-wave comparison with a native subgroup32
vote or request an unsupported native subgroup64. The actual parsed shader
contains LDS reads, writes, an atomic and two real workgroup barriers, so
splitting its workgroup into independent waves is not justified. A preceding
EXEC-dependent branch can bypass the comparison, so inserting a workgroup
barrier at the comparison also requires a convergence proof that is not
currently available. Complete wave64 execution remains an architectural
prerequisite; the bounded implementation below does not yet admit this shader.
Retain these exclusions when continuing.
The observed comparison also aliases its VCC source and destination. RDNA2
ISA section 6.2.4 warns that wave64 VALU same-SGPR read/write can be
unpredictable; the generic packed-mask rule alone does not establish this
aliased sequence's hardware result. Do not infer per-half write behavior from
the instruction-level statement that VCC is fully written.
Follow-up comparison with public implementations did not close this gap:
snapshotting U64 sources in another emulator and host-Vulkan tests with
authored expected values are not target-hardware measurements. No verified
same-SGPR exception or half-pass visibility rule was found. A lawful synthetic
target-device result with initial/final masks and repeatability is still
needed before choosing snapshot or forwarding semantics for this alias.

The checked paired-layout module and optional Vulkan feature path now build.
A real Vulkan host-layout probe verifies four guest waves per workgroup across
two workgroups, with 512 distinct logical-lane output slots, on the Arc A770.
The thirteen existing scalar/GETPC probes remain passing. The production
paired prolog and non-CMPX U32 comparisons now pass eleven numerical GPU cases:
packed lane63, zero/low/high/both/partial EXEC, ordinary SGPR mask destination,
and three guest-coordinate axes across two waves. The full unit command reports
1185 passes and eight skips. This is a bounded compare-only capability, not
complete wave execution or gameplay; runtime paired admission remains disabled.

The GPU canary caught an integration defect before admission: the execution
mode used physical32 but the decorated WorkgroupSize constant retained guest64.
[WorkgroupSize takes precedence over LocalSize](https://docs.vulkan.org/refpages/latest/refpages/source/WorkgroupSize.html),
so duplicate wave records overwrote the canary. Both now use physical dimensions;
all eleven numerical cases preserve the canary. Do not weaken that oracle or
attribute this reproduced source mismatch to the probe's readback.

The lane-operation stage reproduced and corrected an existing contract in
`ShaderSpirvVector.cpp`, `Recompile_VReadfirstlaneB32_SVdstSVsrc0`: empty EXEC
returned literal zero instead of lane0's value. A native32 GPU regression first
returned zero for lane0=42; removing the post-broadcast zero selection makes it
return42. Twelve paired read/write/readfirst cases also pass, including upper
halves, modulo64 and EXEC-independent lane accesses. This does not establish
complete wave execution or runtime admission. After the related parser fixes,
all thirteen paired lane/move cases pass. The integrated checkpoint has82
focused passes and1191 full-suite passes with8 known skips; both scalar and
wave GPU integration targets pass without skips.

`ShaderParseDS.cpp`, opcode0x20 (DS_ADD_RTN_U32), previously discarded VDST
and the upper byte of its offset. Focused parser regressions reproduced both
losses; the parser now retains a distinct return tuple and full byte offset.
Paired LDS lowering has passed its bounded numerical and safety checks.
The isolated native `ShaderSpirvLdsAtomic.cpp` handler now preserves the old
value, full offset, operand aliases and inactive EXEC. Eight numerical GPU
cases pass, including 32 contending invocations with distinct returned values.
Translator version 46 invalidates binaries with the earlier native semantics.
The paired LDS numerical counter probe passes128 logical increments with
all returned old values0..127, but independent review found an admission gap:
`ShaderComputeWaveLds.cpp`, `ValidateLdsAddress`, checks bounds without proving
that non-atomic constant-address writes have a unique active writer or that
later cross-invocation effects are synchronized. Such accepted programs can
produce a host data race under the [Vulkan memory model](https://docs.vulkan.org/spec/latest/appendices/memorymodel.html).
This finding is now closed for the narrow admitted subset by the separate
`ShaderComputeWaveLdsSafety.cpp` proof: whole-workgroup singleton writer
provenance, generic SGPR-half invalidation, and real-barrier effect phases.
Independent review and numerical tests pass, including a lane63-only atomic
that preserves inactive destinations. Current validation:91 focused passes,
1199 full-suite passes and8 known skips; both GPU targets pass on Arc A770.
Do not strengthen DS writes into invented
atomics, insert extra barriers, or treat the passing counter as general LDS
acceptance. Runtime paired admission stays disabled.
*Superseded (2026-10-02):* commit 704f4ad3 moved LDS accesses to the ordered
generic path and the address and singleton-writer proofs described above no
longer gate admission; `ShaderComputeWaveLdsSafety.cpp` and the LDS address
analysis were removed as unreferenced. The index bound they implied is a
recorded defect in "Debt resolution and compute-wave contracts".
The strict native-return rerun passes that missing-emitter frontier and again
reaches the unresolved U64 comparison after resident-load completion. There
is still no presentation or gameplay evidence.
Related native defect recorded, not fixed here: `ShaderSpirvBuffer.cpp`,
`Recompile_DsWriteB32_VaddrVdataOffset` (line2067) and
`Recompile_DsReadB32_VdstVaddrOffset` (line2228) emit unconditional accesses;
an inactive EXEC can therefore still write LDS or change a VGPR. Existing
non-returning LDS atomics share this gap. Add focused inactive-effect tests
and native predicate guards before claiming general native LDS correctness.

The paired scalar subset now admits exact S_AND/OR/XOR_B64 and
S_AND_SAVEEXEC_B64 (ordinary SGPR save destination only). Twelve numerical
cases exercise high-half masks, SCC/EXECZ, source/destination aliasing and
vector EXEC consumption. Full-EXEC initialization and singleton-writer
proofs are conservatively invalidated by their explicit/implicit writes;
four RED regressions reproduced stale-proof acceptance before correction.
Current validation: 93 focused passes, 1201 full passes and 8 known skips,
both compute GPU targets passing. These remain opt-in translation probes;
paired runtime admission and general control-flow/memory support are pending.
The next bounded control-flow slice now parses SOPP `0x09` as a real
`S_CBRANCH_EXECNZ` and admits only one forward EXECZ/EXECNZ diamond with a
shared join. A canonical workgroup barrier may precede or follow that diamond,
but not appear inside either divergent arm; a branch-containing shader rejects
all LDS effects until a CFG-aware LDS proof exists. The paired branch predicate
tests both EXEC words, while the native subgroup path remains unchanged. A
real compare-to-lane-63 fixture failed numerically on the old low-word-only
predicate, then passed both EXECZ and EXECNZ across two guest waves on Arc A770
with a 64-lane output canary. The three structural tests, 50 focused tests,
1205 full tests (8 existing skips), and three compute/diagnostics integration
targets pass. Translator version 47 invalidates earlier branch binaries.
The newly decoded EXECNZ remains fail-closed outside paired mode: a native
low-word-only predicate cannot prove a wave64 result.
This is probe-only progress: the strict Silent workload still stops after
resident load at the unsupported aliased U64 comparison, PC `0x784`, with no
presentation or gameplay evidence. Paired runtime admission is still disabled.

Independent review found a separate native LDS atomic host-safety gap in
`ShaderSpirvLdsAtomic.cpp`: dynamic `vaddr + offset` is shifted into an
`OpAccessChain` without proving it is within `lds_dwords`; an address at the
allocation end can therefore access outside the host Workgroup array. A
native shader with 128 allocated dwords and `vaddr=512` bytes is a synthetic
trigger. The [RDNA2 ISA reference](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
describes general LDS out-of-range reads/writes but does not establish the
returned value of this atomic form. Do not clamp or synthesize a return value.
Next: establish that return contract or a sound address-range proof, add an
in-range/boundary/wrap red test, and guard before constructing the host pointer.
The passing in-range native atomic probes are not a general OOB-safety claim.
The restricted forward diamond does not generalize to loops or multiway
branches. Linear LDS proofs remain unusable across arbitrary joins, including
an LDS operation only in the suffix after a divergent arm.
The
[RDNA2 ISA reference](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
sections12.8,12.12,12.13 also specify that readlane/writelane ignore EXEC and
select modulo64 in wave64; writelane must not acquire an ordinary vector EXEC guard.

DS decoder hypothesis excluded: do **not** move the Gen5 OP/GDS fields to
bits17/16 based on the old PDF table94. AMD's newer
[machine-readable RDNA2 specification](https://gpuopen.com/machine-readable-isa/)
defines OP at bit18 (8bits), GDS at bit17 and VDST at bit56 (8bits). Its XML
SHA256 is `d671ecbc36543674ab59e9b2feffd56718fd2137e7ad913c314f2e2508b9f4f7`.
LLVM22 gfx1030 decoding of independent synthetic read/write/add-return words
agrees, as do28 DS instructions in the private workload capture. The source
also agrees with [LLVM's GFX10 DS encoding](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/DSInstructions.td).
Retain the current field positions; the defect to fix is the lost return
destination and unsupported aliases, not these bit positions.

The new exact-admission tests also exposed inactive DPP fields populated from
the next instruction by the VOP1/VOP2/VOPC parsers. Plain V_MOV then appeared
modified and was rejected. The parser-local correction reads DPP controls only
for actual DPP encoding. Preserve SDWA encoding and VOP3 OMOD metadata before
destination replacement, and reject those unsupported variants in paired
analysis; erased controls are not evidence of a plain instruction.

The dispatch audit additionally found an existing size-validation defect in
`GraphicsRenderDraw.cpp:2533`, `GraphicsRenderDispatchDirect`: thread-dimension
ceil division uses `(count + local_size - 1) / local_size` in `uint32_t` and
only warns for a zero local size. Large counts can overflow and zero local
dimensions can reach division by zero. A focused checked-ceil-division
regression and validation against host dispatch limits are needed; no size
semantics were changed during the wave investigation.

The full unit command now reports 1164 passes and 8 skips (1172 total), and both
compute/graphics integrations pass. Its initial isolated tap failure was a stale
test: the existing dirty controller/header/docs explicitly hold two pressed
guest samples, while the unchanged test expected one. The test now asserts
release/press/press/release and one delivered tap without changing controller
behavior. The earlier order-dependent retirement-counter failures remain a
separate recorded subset-order issue; they did not reproduce in this full run.

A fresh Linux Release run of the current working tree, using Native resolution,
Silent output and no permissive flags, passes the earlier allocator/audio import
boundaries and completes resident loading. This supersedes the startup stop
described in the older bounded-repair entries below, not the independent original
reference-workload frontier. No presentation or controllable gameplay has been
established for this startup workload.

Two incomplete depth-array edits prevented compilation: the D16 span variable
escaped its declaration scope in `GraphicsRenderBind.cpp`, and the
`UtilFillDepthImage` definition in `Utils.cpp` omitted the layer parameter already
declared in its header. Those declaration/signature errors are repaired without
changing the array layout contract.

The next observed shader failure was Gen5 `V_SUBREV_CO_CI_U32`. The decoder now
handles its VOP2 and VOP3B forms; lowering computes `src1 - src0 - borrow_in` with
two unsigned subtract-with-borrow operations, combining their borrow outputs.
The existing scalarized mask is normalized to zero/one. Unimplemented modifiers
and SDWA controls that would be discarded fail closed for this new opcode.
Synthetic regressions demonstrated the original parser rejection and validate
the generated SPIR-V. The workload still stops during whole-shader parsing,
before its full shader can be lowered or dispatched; this is not a workload
emitter-execution or GPU numeric-conformance claim.

Independent review caught and corrected an unconditional VGPR destination
write in the new lowering: inactive EXEC lanes now preserve their old value.
The older shared `V_ADD_CO_CI_U32` text in `ShaderSpirvVector.cpp` still writes
its destination unconditionally before testing EXEC; that pre-existing defect
needs a separate inactive-lane regression and repair. The observed reverse-
borrow SGPR mask is produced by a vector comparison, which this backend stores
as a per-invocation boolean. A raw packed SGPR mask is a different representation;
blindly extracting a lane bit from a comparison-produced boolean is incorrect.
`ShaderMaskAnalysis.cpp` now admits ordinary SGPR-pair inputs only when a
full-pair supported comparison is proven in the same straight-line block;
unknown producers, partial overwrites and branch-entry bypasses fail closed.
VCC/EXEC retain the existing backend mask representation. The translator cache
version is incremented so older generated modules cannot bypass the new policy.
These are deliberately not claims of arbitrary packed-wave-mask support.

The new tests are separate shader-focused modules, leaving the pre-existing
graphics-packet test file unchanged relative to the starting working tree.
The earlier Linux build and 317 focused shader/cache/graphics/heap/audio tests
passed. Subsequent parser/scalar work below advances the failure to shader
emission, still with zero presentations; a playable/capture-scoring gate cannot
pass in this state.

The subsequent strict run stops at `IMAGE_BVH_INTERSECT_RAY`. GFX10 MIMG encodes
the eighth opcode bit separately in word-zero bit zero; masking to seven bits
misidentifies opcode `0xe6` as `0x66`. The decoder now preserves that bit and
reports the unsupported BVH instruction explicitly rather than treating its
128-bit resource as a texture. The [LLVM encoding definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/SIInstrFormats.td)
and [opcode definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/MIMGInstructions.td)
corroborate the identity. Removing the `r128` rejection, inventing a hit/miss,
or skipping the instruction would not implement it. AMD's public
[legacy GPURT node definitions](https://github.com/GPUOpen-Drivers/gpurt/tree/7b226d48b46b7e92fec3b9ecc5712e5bf2bf3dd9/src/shadersClean/common/gfx10)
provide the FP16/FP32 box and two-triangle node layouts. The
[software intersection path](https://github.com/GPUOpen-Drivers/gpurt/blob/7b226d48b46b7e92fec3b9ecc5712e5bf2bf3dd9/src/shaders/IntersectCommon.hlsl)
is a vendor reference for arithmetic, not proof of every hardware edge case.
Mode-zero triangle-ID/status encoding remains unverified.

The live compute shader constructs the BVH descriptor after a 16-dword scalar
load through a computed pointer, then extracts the base field and forms the
size/type/mode words with scalar arithmetic. It is not a four-dword BVH
descriptor copied directly from the EUD table. Extending the existing dynamic
V# resource classifier alone would therefore be insufficient. Required next
work is bounds-checked, coherent memory access for this computed resource plus
real box/triangle intersection lowering; Vulkan ray query is not a drop-in
replacement for this per-node instruction.

In particular, the current `ShaderSpirvBuffer.cpp::recompile_sload_from_extended`
reads snapshotted descriptor metadata, not arbitrary computed guest pointers.
It only warns when the source register is not the EUD register, then continues
with metadata indexing. `S_LOAD_DWORDX16` also has no dispatch-table emitter.
Do not request sixteen outputs from the eight-element local destination array,
or reinterpret a computed pointer load as EUD metadata. Add a bounded
address-to-backing contract and dedicated computed-load regressions first.

A further live metadata capture exposes a preceding resource-classification
defect: the same compute stage declares nonempty SRT and EUD regions together,
with a type-5 pointer and scalar loads through that pair. However,
`ShaderResources.cpp::Gen5HasEudPointer` previously required `srt_size_dw == 0`, so this
dispatch reaches emission with `extended.used == false`. Its direct pointer
also enters the generic four-word storage path in `ShaderParseUsage2`. Do not
use the resulting candidate bindings as proof of computed-pointer coverage.
The mixed-region predicate now has a red/green regression and recognizes the
explicit EUD pointer. The generic four-word fallback now preserves a partially
overlapping SRT span as raw scalar data. A conflicting direct buffer use is
rejected only when existing CFG analysis proves an incoming descriptor reaches
the consumer; later SGPR overwrites are not evidence about the incoming pointer.
Focused red/green cases cover the two-word pointer, a live-in contradiction,
and descriptor reuse after overwrite. This is initialization preservation, not
an implementation of computed memory resolution.
The next snapshot guard incorrectly capped API sharp slots by dividing backing
dwords by four. Sparse or aliased slots need not occupy distinct storage; both
span passes now accept them while retaining the actual 256-dword access bounds.
A separate small resource-pointer suite covers sparse aliases and out-of-range
spans.

The full-precision, 32-bit-node BVH form now has a dedicated parser with eleven
address registers and a four-word resource tuple. Contiguous and NSA addressing
have synthetic regressions; other formats remain unsupported. This is parser
support only, not an intersection emitter. Fresh raw SOP1 opcode evidence also
identifies two descriptor-producing instructions as `S_BITSET1_B32`, not GETPC
or FF1: the earlier probe printed their inline bit indices, not opcodes. A
dedicated bitset emitter preserves the other destination bits and SCC. The next
parser stop, `S_CMP_LG_U64`, now compares both scalar words and writes SCC without
requiring host Int64 support. Ordinary pair bounds are checked. Six focused
parser/scalar tests pass, including SPIR-V validation; GPU numeric validation
is still pending.

The fresh strict run now reaches `Spirv::WriteInstructions`. A bounded backtrace
identified its first unknown-format instruction as `S_PACK_LL_B32_B16`.
That instruction now has a dedicated scalar packing module, tested with distinct
halves, aliased inputs/destination and VCC_HI. Ten hermetic Vulkan numerical
cases covering bitset, scalar inequality and packing pass on an Intel Arc A770,
including scalar execution with EXEC zero and SCC preservation where specified.
These synthetic shader results are not proof of workload shader dispatch.
The formatter assertion at `ShaderDebug.cpp:216` can
hide the underlying unimplemented instruction when printing placeholder IR.

Admitting the real EUD table exposed a host memory-corruption defect:
`ShaderGetTextureBuffer` warned at its sixteen-entry capacity but still appended,
corrupting counters later read by `ShaderAddDynamicTextureResource`. The strict
launch produced an access violation in that later loop; a focused capacity
regression proves an append at capacity previously returned instead of rejecting
the write. The subsequent strict run identifies a seventeenth combined
sampled/storage resource, not a Vulkan device limit. A focused test reproduces
sixteen sampled entries followed by a writable entry. The logical table is
being expanded to thirty-two entries while preserving separate sampler and
storage-buffer bounds and the thirty-two-bit image-write mask. Do not truncate
resources or merge distinct sampled and writable uses. The dense layout cache
remains bounded (approximately 29 MiB across the three stages).
Generated layouts need checks against actual host limits, including all seven
sampled-image arrays, and pipeline layouts need aggregate checks across sets.
Standalone samplers do not contribute to `maxPerStageResources`; they retain
their own per-stage and pipeline limits (see the
[Vulkan limits specification](https://docs.vulkan.org/spec/latest/chapters/limits.html)).
The expanded table passes focused capacity/limit tests, ten numerical Vulkan
shader checks, and the graphics diagnostics integration. A strict run now
retains eighteen textures and advances to a different ceiling: dynamic scalar
resource collection exhausts its sixty-four instruction-PC mapping records.
Those mappings represent producers, not distinct Vulkan resources. Repeated
loads may share one resource while requiring distinct lifetime mappings; replace
the fixed mapping ceiling with instruction-count-bounded storage rather than
dropping producers. The SRT-preserving build reaches the same mapping frontier.
Presentation and gameplay remain unverified.

An additional broad-test limitation remains recorded: the two
`EmulatorGraphicsState` linked-buffer retirement tests pass individually,
together, and in the entire graphics-state suite, but adding the graphics-packet
suite before the pair produces nine extra storage-buffer delete callbacks in
the second test. `EnsureGpuMemoryForTests` initializes a process-wide singleton
once, while these assertions reset global callback counters and compare global
free deltas. The origin of the prior objects is unproven; isolate ownership or
drain only test-owned objects before interpreting this as a retirement-contract
regression. No production retirement fix or complete broad-suite pass is claimed.

The two retirement tests now use fixture-owned callback tokens and exact backing
presence, with one reusable dedicated range and scoped cleanup. The ordered
packet-suite plus pair run passes 233 tests; the pair passes three repetitions.
The broader run passes 1182 tests with eight skips when excluding only the
currently pending wave-mask RED case. No production retirement policy changed.
An additional repeat-only fixture defect remains: repeating the entire packet
suite reaches `AcceptsComputeShaderWithoutWorkgroupId` a second time and calls
`ShaderInit()` again, failing its `g_shader_map != nullptr` guard in
`Shader.cpp:770`. Isolate that test's shader-global initialization or make its
fixture lifecycle explicit; do not weaken the production initialization guard.

Important strictness limitation: `ShaderParseSOP1.cpp` and the other scalar
parsers still map several unimplemented operations to `SBarrier` with warning-
only diagnostics, including get-PC and find-first-one. They retain an unknown format rather than
the real barrier's empty format; parser survival is not evidence that an emitter
can execute them. A launch without permissive environment flags does not validate
these unimplemented semantics. Identify and implement the actual producer
operations from their encoded instructions before claiming a correct BVH binding
or compatibility; do not perpetuate the placeholders.

Related depth-array limits still open:

- The source span now uses the normalized layer count and CPU upload detiles
  each slice with the matched Z_X layer term. The inline storage-backed D16
  path still fills only layer zero and is therefore admitted only for a
  single-layer descriptor. Layered GPU-owned depth sources need a separate
  evidenced multi-layer producer and upload contract.
- The type-13 one-layer case (raw DEPTH zero) remains rejected by the D16
  gate; the depth-array view is created only when logical depth exceeds one.
  Establish the guest descriptor/view contract before extending this case.

The new speaker/privacy HLE paths pass the observed startup route, but their
broader ABI assumptions remain unverified: `Audio.cpp::AudioOut2GetSpeakerInfo`
places availability bits at output offset four whereas the earlier caller
trace consumes offset eight; full write extent, angles and additional selector
semantics are not established by that trace. `LibUserService.cpp` interprets
the privacy query's first argument as a user ID without an independently
verified invocation contract. Preserve these as open evidence requirements;
passing self-consistent HLE tests does not establish native ABI equivalence.

### Process allocator startup (bounded repair, not gameplay acceptance)

Libc startup now consumes the declared `PT_OS_PROCPARAM -> libc parameters ->
malloc replacement` chain. The replacement record has a two-qword header;
its initializer is not the first entry of the direct kernel heap API. A
captured failure reached the first allocation without ever calling this
initializer. Calling the declared initializer before publishing the direct
API fixes that null-mspace boundary without scanning load segments, replaying
main-image constructors, or substituting a host heap.
Both HLE libc need-flags were already set in the failing run, and the main CRT
already called its constructors: changing those flags or adding a second
constructor pass does not explain the missing allocator producer.

The common pointer offsets and version-one record are corroborated by the
[public OpenOrbis CRT](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/af1619c2d1bfffe85a9534c9abb3fb43eed45dfd/src/crt/crt1.S).
The version-two tail and successful initializer return were checked against
local runtime evidence. Publish-after-success is Kyty's safety policy, not a
claim about undocumented system ordering. Empty default tables preserve the
default allocator. Reentry and concurrent startup have focused regressions;
guest callbacks run outside host publication locks.

Known unresolved lifecycle defect: `Libs/ApplicationHeap.cpp`'s process-global
API and startup identity outlive `Loader/RuntimeLinker.cpp`'s destructor and
`Clear()`. Reusing one host process for successive or nested guest runtimes can
retain callbacks to unmapped guest code; the new once-state can also reject a
different parameter address or mistake a reused address for the old process.
The direct-API lifetime defect predates this startup repair. Fix direction:
owner-generation-scoped state, owner-conditional retirement, and quiescence of
in-flight allocator callbacks before guest unmapping. An unconditional reset
in a nested linker's destructor is not safe. Current verification uses a fresh
host process per guest run and does not establish multi-runtime reuse safety.

### Nested arenas and AudioOut2 startup (bounded repair)

The next allocation failure was a legitimate child mspace rejected by the
registry's blanket overlap check in `Core/MSpace.cpp::MSpaceRegister`. Admission
now requires a live canonical parent allocation containing the entire child;
invalid overlaps are rejected before writing the child header. Backing storage
cannot be freed, moved, or destroyed while a child is registered. A transient
realloc pin prevents an OOM callback from creating a child on a moving chunk,
without holding the global registry lock across that callback. Copied alignment
markers and raw-prefix subpointers have red-to-green regressions. The allocator
and adjacent libc/loader suite passes 135 tests; arbitrary allocator-metadata
corruption is not a supported contract.

The strict workload then reached an audio-startup polling loop with zero GPU
submissions. This was not a nested-heap deadlock: the context-parameter whitelist
in `Audio.cpp::ReadSupportedContextParam` rejected a second captured 0x40-byte
configuration. The guest ignored the failed memory query, passed a zero-size
workspace to context creation, and retried port creation on a missing context.
Do not force a successful port result or bypass this guest wait.

The decoder now accepts only the two complete measured configurations. The
second configuration's producer/consumer trace establishes 512 frames per
submission; the corresponding F32 eight-channel snapshot is 16384 bytes, not
the legacy 8192. Header fields remain opaque and unknown blocks still fail
closed. A guard-page regression checks the full PCM read, alongside mutation
tests that leave rejected outputs untouched. The existing 64 KiB workspace is
explicitly a shared HLE reservation policy, **not** a measured native query
result or native workspace layout for the new configuration. Native-equivalent
workspace sizing and audible PCM routing remain unverified.

The final core/audio/heap/loader/symbol validation ran 197 tests: 192 passed and
five media-fixture-dependent tests skipped. The next strict original-workload run
exited at the unresolved lazy import `sceAudioOut2GetSpeakerInfo` in
`AudioOut2_v1 / AudioOut_v1.1`, with no draws or presentations. The export is
deliberately absent from `Libs/LibAudio.cpp` until its argument/output contract
is evidenced; a public name/NID mapping alone does not justify a success stub.
The caller explicitly prepares an output pointer and a zero second argument,
then consumes a byte at output offset zero and bit zero of a dword at offset
eight; it does not test the return value. This proves neither the complete
output size nor the meanings of these fields. The old log-and-success binding
from `908d78d4` was deliberately removed in `ea839c67`; restoring it, zeroing a
guessed structure, or returning an error without an evidenced output contract
does not solve this boundary. Public HLE layouts are research leads, not a
verified native contract.

Next requirement: a valid contract or a successful reference trace for the
observed invocation, with return value, pre/post output, actual write extent,
and a second controlled speaker configuration to identify the consumed fields.
No speaker implementation is added in this repair. Gameplay is still unproven
for this workload. The original reference-workload frontier below is independent
of these startup repairs.

### Original reference workload

The local reference workload reaches Vulkan device creation, guest engine
startup, Gen5 shader creation, indexed draws, VideoOut submission, repeated
swapchain presentation, logos, a recognizable menu, Play / mode selection,
loading-card presentation, and controllable gameplay under **strict** flags
(no `KYTY_BRINGUP_*`). The latest Linux Release+Silent validation used bounded
diagnostic controller input to traverse the menus, held a direction for 180
presentations in gameplay, produced a healthy native capture, and reported no
runtime error. A reset 601-frame gameplay window reported 41.688 FPS with
p50/p95/p99 frame times of 27/35/40 ms and one frame above 50 ms. Diagnostic
input proves the runtime frontier and control path, but is not formal
playability acceptance.

Recent strict bring-up (evidence-backed, focused tests where noted) includes:

- A captured Gen5 compute metadata writer now has a fail-closed semantic path
  to the exact D32S8 HTILE incarnation it initializes. The classifier matches
  the decoded linear `v0` invocation-index, bounds guard, uniform source read,
  and one typed dword store per destination record; runtime guards additionally
  require exact descriptors, full dispatch coverage, immutable zero source and
  parameters, writable storage generations, and submission order. It neither
  skips the guest dispatch nor keys behavior by title, shader checksum, address,
  extent, or host GPU. One bounded strict run consumed the event once, selected
  `CLEAR` with depth zero plus the guest's stencil-zero clear on first use, then
  retained `LOAD`/reverse-Z writes. A temporal capture sequence showed coherent
  track and vehicle geometry across multiple cameras. This closes the missing-
  geometry depth-initialization producer for that observed route, but does not
  establish playability or correct color/material rendering. Independent review
  subsequently tightened the local-ID and combined D32S8 guards; the affected
  emulator and graphics integration targets build and the short integration
  contract passes, while that final hardening has not yet been re-run through
  the private strict route.
- The remaining visible defect is now downstream of coherent geometry: some
  surfaces are black, materials are washed out or over-saturated, and UI layers
  can remain visible across scene/camera changes. A same-scene HDR trace already
  places those pixels in the stable full-resolution producer before downsample,
  with defined `LOAD`, no CMASK fast clear, no target remap and no direct
  sample/attachment feedback. A later checksum-scoped strict draw trace joined
  the same format-122 producer class and found `dcc_enable=0`, a zero DCC base,
  disabled CMASK, `LOAD`, blend disabled, and the expected RGB write mask. This
  closes DCC/CMASK and immediate blend/write-mask as causes for that draw. Do not
  reopen vertex/NaN or depth-clear hypotheses for this symptom. The rebuilt
  translator-v34 module already lowers the sole `v_readfirstlane_b32` as a
  direct copy because its selected value is scalar-uniform. Its remaining
  `OpGroupNonUniformAny` guards a comparison whose operands are also uniform in
  the exact ISA, so the two host subgroup-32 halves take the same branch. This
  closes the observed wave64 operations as the color cause for this draw; the
  unsupported host wave64 width remains a general capability gap. The next
  bounded discriminator is the existing output-preserving final-MRT plus
  same-fence attachment probe, not subgroup emulation or a title-specific rule.
  Two bounded attempts could not select the newly observed material draw in a
  no-input route, and the input-gated attempt remained in loading until its
  watchdog; these are reproduction failures, not shader results. A following
  strict playable-regression run delivered and guest-sampled all three pad taps
  and sustained 38,726 presents without a host error, but never observed the
  required post-input loading transition and therefore correctly produced no
  acceptance capture. Playability and the stable car-selection checkpoint
  remain unproven on the current tree.
- One later present-addressed strict route reached `interactive` after two
  delivered guest-read taps, then exited with a write access violation at guest
  RIP `0x900a1c336` and address `0x350c8390`. Do not symbolize that RIP against
  the host PIE: offline loading proves it is eboot code, specifically a
  `vmovups` zero store to `r14+0x10`, while the destination belongs to a
  direct-memory `memfd` mapping. An identical bounded route subsequently mapped
  the destination read/write, produced two native captures, reported no runtime
  error, and ended only at its 150-second watchdog. The fault is therefore
  intermittent and not reproduced; it does not implicate host COW/string code
  or the capture request. Before changing memory semantics, capture the dirty-
  page tracker state, original protection token and native page protection for
  the exact faulting page from a signal-safe bounded record.
- Graphics pipelines now preserve the decoded Gen5 Z-clipping request instead
  of disabling Vulkan depth clipping unconditionally. Native depth-clip,
  optional core depth-clamp fallback, feature negotiation, and both cache-key
  bits are covered by the graphics integration contract. The first bounded
  strict validation remained on a UI-only `PLAY` frame after both scheduled
  inputs, so this is a corrected renderer contract, not evidence that the
  damaged 3D scene or playability has recovered.

- Gen5 hint-less direct-memory mappings use the guest user-address window with
  a monotonic placement cursor, and physical releases accept fully covered
  subranges or contiguous allocation spans. Focused split/coalescing tests pass;
  a strict 60-second bounded run ended only at its timeout with a 4.6 GiB host
  peak instead of the earlier 8 GiB cgroup termination at 36.7 seconds.
- GpuMemory multi-parent alias policies (Texture/Storage/Vertex/RenderTexture
  relations as captured; inverse or unobserved relations stay strict).
- GpuMemory: multi-parent VertexBuffer with surface link + peer VB reclaim;
  Texture mixed parents (VB reclaim/link, SB/RT/Texture
  Contains/IsContainedWithin/Crosses); **IndexBuffer Contained in Texture**
  (and other surfaces) **links**, does not reclaim the Texture
  (`GpuMemoryAllowsIndexContainedInSurface`; captured IB size `0xe4`).
- WriteBack multi-parent classification: Equals → propagate hash; Crosses /
  Contains / IsContainedWithin → invalidate only (partial overlap).
- GPU-owned tiled RenderTexture (no write-back): `update_func` must **not**
  force `VK_IMAGE_LAYOUT_UNDEFINED` on Update re-entry. StorageBuffer WriteBack
  invalidates alias parents; UNDEFINED→COLOR transitions **discard** prior
  render-pass contents (user-visible white intermediate world with HUD still
  drawing). Create still starts UNDEFINED once.
- Gen5 tile mode 27 (`SW_64KB_R_X`) **size** and **CPU detile for 4 bpp** sample
  textures (16-pipe non-RbPlus pattern table reimplemented from public MIT
  ADDRLIB vocabulary; visual sample quality still needs post-playability QA).
- Gen5 sample formats in PrepareTextures: Ufmt 56 (RGBA8), 14 (RG8 linear
  pitch), 71 (RGBA16F RT alias). Tile 27 pure CPU upload remains format-56 only.
- Gen5 EUD: direct resource type 5 as EUD pointer when `eud_size_dw != 0` and
  `srt_size_dw == 0`; overflow sharp offsets map through EUD base
  `round_up(user_sgpr_num, 4)`.
- Multi-RT `CB_SHADER_MASK` full-channel nibbles (`0` or `0xf` per RT).
- EXP Param5 (`0x25`) / Param6 (`0x26`) and multi-MRT compressed / null EXP for
  MRT0–3 (including `done=0` / `vm=0` variants observed on load).
- Guest `EarlyZThenLateZ` with pixel kill must not lower to Vulkan
  `EarlyFragmentTests` alone: kill-enabled shaders use late depth commit so
  transparent quad pixels cannot write depth before `OpKill`; opaque early-Z
  shaders retain `EarlyFragmentTests` (`9b026e53`).
- SPIR-V structured loops for backward `S_BRANCH` (`OpLoopMerge` + body +
  continue / unreachable as required); do not regress CFG.
- `v_cvt_i32_f32` (VOP1 `0x8` / VOP3 `0x188`); VOP1/VOP2 SDWA (encoding 249).
- Indexed and automatic draws preserve the guest instance count; indexed
  indirect draws also preserve first instance and signed base vertex, including
  the guest-geometry depth/stencil-copy path. The base vertex is added exactly
  once to the existing register/shader-derived vertex offset before sizing
  vertex reach and issuing `vkCmdDrawIndexed`; out-of-range signed sums or
  negative effective vertex ranges skip the invalid draw with a bounded warning.
  Indirect instance count remains persistent for later direct draws. The current
  failing material trace used zero/one/zero for base/instance/first-instance, so
  this general contract correction is not evidence of a 3D recovery.
- A remaining cross-title indexed-draw defect is recorded at
  `GraphicsRenderDraw.cpp`: command processing preserves guest index type `2`
  and the indirect path sizes it as one byte per index, but the renderer only
  materializes types `0` and `1`. Type `2` therefore reaches the render path
  with a zero upload size and the default 16-bit Vulkan type. The general fix
  should widen the guest bytes to a supported 16-bit index buffer (including
  primitive-restart semantics) or use a proven enabled host 8-bit-index
  contract. The exact 3,564-index material draw used type `0`, so this defect
  cannot explain that capture and must not be mixed into its next experiment.
- Gen5 MUBUF/MTBUF address generation now preserves the RDNA2 `IDXEN+OFFEN`
  VADDR contract: lane 0 is the element index and lane 1 is the byte offset.
  The previous lowering exchanged those lanes, so indexed material-table loads
  could read the wrong in-range records without producing an OOB failure.
  `IDXEN`-only and `OFFEN`-only retain their scalar VADDR behavior. The existing
  addressing integration test covers all three forms, and translator version 32
  prevents reuse of stale modules. An exact regenerated material module validates
  and forms `index * stride + offset` from the corrected lanes. A later bounded
  strict route delivered both requested input edges and captured the intended
  PLAY-era checkpoint at present 8,095. The image still contained only small
  warm fragments over an otherwise black world (`healthy=false`, entropy
  `0.2896`, 176 bins), so the address correction is necessary shader semantics
  but is not sufficient to recover the vehicle or world. 3D recovery remains
  unproven.
- The storage-analysis inconsistency is closed conservatively: an unmatched
  `DirectResource` remains `Unknown` with `NoMatchingInstruction`, while only
  proven-unused `MetadataSharp` entries may be removed before binding. Focused
  unit and graphics-integration coverage enforce that distinction. An earlier
  strict private run with the same conservative policy did not reach its present
  gate before timeout, so it is not compatibility evidence. Before changing
  global storage pruning, isolate the exact direct binding and prove its
  ownership, size, and real shader consumer.
- SMEM dual offset (SGPR soffset + 21-bit imm) and variable-offset
  `s_buffer_load_dword` / `x2` / `x4` with imm constants registered for SPIR-V.
- `s_buffer_load_dwordx8` still lacks that dual-offset/variable-SOFFSET
  lowering in `ShaderSpirvBuffer.cpp`; its current emitter requires a constant
  SOFFSET and omits `smem_imm_offset`. Port the proven x1/x2/x4 address
  formation before accepting an x8 shader that uses either field. The current
  exact Gen5 material instruction uses null SOFFSET and immediate zero, so this
  general defect does not explain that draw's black lighting.
- `image_sample` dmasks including single-channel `0x2`/`0x4` and `0xb` (R+G+A).
- Captured `ds_read2_b32` decodes its two dword-scaled offsets while preserving
  the byte-addressed `vaddr`, and reads through the same Workgroup storage as
  `ds_write_b32` (`990b9a40`).
- Gen5 extended NGS2 rack `max_voices` at option offset `+0x50` when option size
  ≥ `0xb0` (focused Audio tests).
- PS user SGPR window up to 32; CB blend1–7, BufferLoadFormatXyzw, and related
  register/shader contracts from earlier cycles.

**Last accepted strict frontier:** the earlier Linux Release+Silent validation
completed more than 24,000 presents without a structured failure;
`ds_read2_b32` remains implemented and covered by focused parser/SPIR-V tests.
The current 2026-08-23 worktree is not accepted at that frontier: two
separately created bounded artifacts record the same Vulkan device loss in
`vkQueuePresentKHR` after the first diagnostic input edge. Their guest logs are
byte-identical and contain no run configuration or submit join, so durable
evidence does not independently establish two differently configured launches.
A later manifest-backed strict diagnostic run with the submit trace enabled did
not reproduce device loss: one input edge was delivered, present/frame advanced
from roughly 11,000 to 14,500, and the run was terminated deliberately with no
last error. The older failures and this negative run are not configuration- or
binary-equivalent evidence, so neither a persistent failure nor a correction is
established. Do not report the historical 24,000-present run or the later
negative diagnostic as validation of the present worktree.

A later bounded Release+Silent strict run exercised translator version 32 and
regenerated the exact material shader with the corrected MUBUF VADDR order. It
reached present 8,003, delivered exactly one requested input edge, and advanced
to present 9,049 before presentation stopped advancing while frame processing
continued. No second edge or capture was requested. `last-error` stayed null,
no synchronization wait was blocked or suspended, and the sole process was
stopped deliberately. The cumulative diagnostics include command/draw/dispatch
processing calls lasting several seconds, but do not identify their producer or
connect the presentation stall causally to the address correction. This run is
integration-failure evidence only, not visual validation or acceptance.

A subsequent bounded run with the exact descriptor discriminator did not
repeat that stall. It reached present 10,513, delivered exactly two input edges
with 40-present deltas, and captured at present 10,617 before deliberate exit.
The material V# was linear (`stride=16`, `records=31`) with `ADD_TID=0`,
swizzle disabled, index stride zero, and `OOB_SELECT=0`, excluding those
descriptor modes for this shader. The capture reached the transmission-choice
screen and showed a coherent but mostly gray scene; its automated score was
low entropy (`healthy=false`, entropy 2.3809, 134 color bins, no directional
stripes). It is not the gameplay checkpoint, a visual A/B, or 3D acceptance.
`last-error` remained null and the only process was stopped immediately after
the bounded capture.

The next ordinary translator-32 route started the native agent before guest
initialization, waited for present 8,000, delivered exactly two `cross` edges
with 40-present deltas, and captured at present 8,095. It reached the PLAY-era
checkpoint but still showed only small warm fragments over a black world; the
score was `healthy=false`, entropy 0.2896, 176 color bins, with no stripe
classification. This is the first aligned visual evidence for the corrected
MUBUF module and proves that the lane-order correction alone does not restore
the vehicle or world. It does not invalidate the corrected RDNA2 address
contract.

The exact 41,910-index live-resolver discriminator now reuses the bounded
vertex probe rather than logging MUBUF activity. Its `VCPROB7` layout records
at most one first-executed embedded-MUBUF decision after the normal resolver
has chosen validity, slot, and byte offset; ordinary shader modules contain no
diagnostic SSBO or resolver-probe symbols. The focused graphics integration,
`fc_script` build, and independent isolation review pass. An initial strict
run was inconclusive, but a later bounded run completed the exact fence and
reported `c=0`: no embedded-MUBUF address setup executed. That is not an
invalid descriptor. The persisted module has distinct position, normal, and
UV attribute loads, so the exact draw uses semantic `Fetch` and does not
consume the appended stream SSBO through the live resolver. Close this seam;
do not change MUBUF or `DetectFetch` on this evidence.

The repeated gate delay exposed a separate bounded performance issue. Slow
records showed 525-to-849-KiB immutable vertex buffers just above the 512-KiB
snapshot ceiling. Snapshots now accept at most 1 MiB while retaining the
existing read-only/alias validation and authoritative fallback. D16 scratch
uses a distinct command-buffer-owned 16-MiB pool, so larger snapshots cannot
consume critical detile capacity; the shared pool remains 16 MiB. Focused
tests, graphics integration, build, and final independent review pass. The
first 45-second checkpoint improved from present 4,231 at about 8 FPS to 5,769
at 68.3 FPS, with roughly 2.15 GiB host memory and zero cgroup swap. Treat this
as workload evidence, not a general benchmark.

The completed run reached present 8,225 and delivered both scheduled taps, but
its native capture was the credits screen. The scorer's `stripey` result came
from horizontal white text, not the reported 3D tearing. It is not gameplay
evidence and does not validate the Z-clipping correction. The current frontier
is reproducing PLAY under the faster renderer without a third input before
another graphics semantic is changed.

The exact draw fence cannot replace the first input milestone: a single strict
no-input attempt reached present 32,329 at roughly 309 FPS without emitting
the event, then stopped cleanly with zero cgroup swap. The draw is downstream
of input. Do not repeat that wait or infer a shader failure from its absence.

Absolute presentation timing is also not a stable visual milestone after the
snapshot performance change. Two strict runs scheduled exactly two `cross`
taps at presents 8,000 and 8,080. Both delivered both taps without cancellation
and emitted the same finite 41,910-index clip/parameter aggregate between the
two inputs. One later frame at present 9,161 showed the coherent `PLAY` prompt
without the vehicle; the other advanced continuously through a 20-second watch
(273 presents, no blocked sync wait or structured error) but captured the logo
at present 8,982. The latter run slowed from about 23 to 11 FPS rather than
hanging, stayed below a 2-GiB observed cgroup current value with zero cgroup
swap, and was stopped deliberately. Do not classify a timed-out absolute
`wait-present` as a graphics deadlock while `watch` still observes progress,
and do not use either UI-only frame as 3D acceptance.

Capturing immediately after the exact resolver event with only the first
scheduled tap did not turn that event into a scene fence. The event arrived
443 ms after the tap began, with `c=0` and the same finite clip/`param0`
aggregate, but the native frame at present 8,169 was the credits screen with a
small central color fragment. Its score was `scene_ok=false`,
`gameplay_like=false` (entropy `0.5394`, 167 quantized colors). Therefore the
41,910-index draw executes in more than one UI phase or is otherwise not unique
enough to identify the vehicle scene. Keep the resolver exclusion, but stop
using this draw alone to trigger PLAY captures.

The same selected probe now appends six draw-bounded clip-population counters
without changing guest-visible state. Raw NaN/Inf positions are counted once;
finite `w <= 0` terminates before division; positive-W positions are classified
against raw `+/-w`, `0..w`, and `-w..w` bounds. The 37-word layout has a new
diagnostic identity, ordinary modules remain free of probe storage, maximum
counter serialization is bounded in a separate event, blocking and
nonblocking fence integration modes pass, `fc_script` builds at `-j1`, and an
independent correction review passes.

One strict one-edge run reported all 27,937 selected invocations in disjoint
classes: `wnp=25677`, `oxy=2260`, and zero Z-outside or inside vertices under
both clip conventions (`oz01=0 in01=0 ozn=0 inn=0`). `nf=0`, `last-error` was
null, the event ring did not overflow, cgroup memory peaked at 1,889,259,520
bytes, cgroup swap stayed zero, and the process was stopped deliberately. For
this occurrence, every position is rejected by W/XY clip before depth testing;
an incorrect depth attachment, stale clear, or HTILE/read-write identity cannot
be the reason this exact mesh occurrence is absent. This does not generalize to
the 3,564-index occurrence (previously inside and fragment-tested) or establish
PLAY/gameplay, because the 41,910-index selector is not a unique scene fence.
The retained same-material trace named the 39,120-index draw as the next
bounded correlation target after an off-screen 41,910 occurrence. A strict
one-edge run has now closed that target too: all 26,079 selected invocations
were rejected before depth (`wnp=24679`, `oxy=1400`, all four Z counters zero,
`nf=0`). The result arrived 560 ms after the input edge, the event ring had no
drops, and cgroup memory was about 1.74 GiB with zero swap. An immediate native
capture request timed out after 20 seconds and produced no file; the 90-second
service watchdog then stopped the run. Do not repeat that capture or claim a
visual scene correlation from it. Both large-draw results are exact-occurrence
depth exclusions, not evidence that the scene-wide transform is wrong or that
PLAY has been reached.

The probe can now preserve its one-shot reservation until an optional strict
decimal present threshold (`KYTY_VS_CLIP_PROBE_MIN_PRESENT`, with the analogous
pixel-input setting). With the setting absent it still arms immediately; a
malformed value fails closed. Before the threshold, matching draws remain
ordinary and do not acquire diagnostic shader or pipeline identities. Because
VS and PS share one lifecycle, selecting both waits for both thresholds (the
effective maximum) rather than letting the earlier stage consume the one-shot.
The focused graphics integration, unequal-threshold regression, and serial
`fc_script` build pass under the bounded zero-swap build envelope.

That gate produced the first phase-correlated result. One strict Silent run
scheduled exactly two `cross` edges at presents 8,000 and 8,080, selected the
41,910-index draw only from present 8,500, and completed after both inputs. Of
27,937 invocations, 19,217 had non-positive W, 6,722 positive-W invocations
were outside XY, and 1,998 were inside XY and Z under both conventions; no
position was nonfinite and no Z-only rejection occurred. The resolver still
reported no executed embedded-MUBUF address setup and `param0` stayed finite.
Both taps were delivered without cancellation, the event ring had no drops,
the last live cgroup snapshot peaked at 1,889,488,896 bytes with zero swap, and
the 90-second watchdog ended the process shortly after the event. `last-error`
was therefore unavailable after shutdown; do not infer it was null. Unlike the
earlier credits-phase occurrences, this later occurrence sends 1,998 vertices
past clip, so depth/clear/HTILE remains a live explanation for its missing
fragments.

That pixel-input discriminator is now complete on the same delayed draw and
two-input route. The late-test diagnostic observed 207,953 fragment-shader
invocations, all finite, with `input0.xy` ranges
`[-0.0910642,0.86913] x [0.0101471,0.958984]`. Thus the later occurrence has
real raster coverage and finite interpolation before depth; total clipping,
culling/no rasterization, and a nonfinite first varying are excluded for it.
The event arrived before the 95-second watchdog; the process then ended before
status/`last-error` could be queried, and the journal reported a rounded 2-GiB
peak. Do not claim `last-error=null` or a visual fix. The next bounded question
is whether any samples pass the unchanged depth/stencil test, versus failure
later in the pixel shader/color path.

The selected renderer probe now wraps an applicable exact draw in one
host-only Vulkan occlusion query. It resets before the render pass, begins/ends
inside the pass around every chunk of the selected draw, and reads availability
only after the same command-buffer fence. The final event is
`depth_stencil_probe` and reports explicit depth, stencil, and depth-bounds
applicability flags plus `precise=0 any_passed=<0|1>`; color-only matches report
`applicable=0` without issuing the query. Three separately registered focused
integration modes cover a real empty render-pass query/readback, depth-bounds
alone, and color-only handling; the original lifecycle gate remains separate.

A strict VS-only run retained the guest pixel shader's normal early tests. Its
prototype event returned a raw zero, which the corrected final contract
expresses only as `depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0`, for the
delayed 41,910-index occurrence. Its companion aggregate had 7,283 vertices inside
the clip volume, `nf=0`, and finite positive `z/w`; the retained material trace
also has stencil disabled, GEQUAL depth, depth writes, LOAD, clear value zero,
and no explicit depth clear. Both inputs were delivered, `last-error` was null,
the ring had no drops, cgroup peak was 2,037,301,248 bytes, swap stayed zero,
and the process was stopped cleanly. Thus all covered samples for this exact
occurrence are rejected by depth, not stencil or pre-depth geometry loss.
This does not yet prove the attachment is wrong: earlier GEQUAL world draws may
legitimately occlude it. One attempt to query the earlier 15,366-index draw with
the same present-8,500 gate emitted no selected event before the 95-second
watchdog; the retained sequence places that draw earlier, so the absence is a
timing exclusion, not shader/query evidence. Do not repeat that threshold or
change clear, HTILE, compare, or identity from the 41,910 result alone.

A single corrected control lowered only that gate to present 8,090 while
keeping the same two inputs. The 15,366-index draw completed 1,452 ms after the
second input with `depth=1 stencil=0 bounds=0 ready=1 any_passed=1`. Its 9,151
invocations were finite: none had non-positive W, 1,476 were outside XY, and
7,675 were inside XY and Z under both conventions. `param0` was finite and the
resolver remained `c=0`. Both taps were delivered without cancellation,
`last-error` was null, the ring had no drops, cgroup memory peaked at
2,149,572,608 bytes with zero swap, and the process was stopped cleanly. Thus
the shared depth image and GEQUAL sequence can pass substantial geometry; the
41,910 zero is local to its ordering/coverage and may be legitimate occlusion.
Do not change attachment identity, clear, HTILE, or compare from that draw.
Return to the material/color path on the passing 15,366 draw.

The material path now has a separate output-preserving pixel sample probe.
`KYTY_PS_SAMPLE_PROBE=<checksum>:@<instruction-ordinal>` selects one exact
`ImageSampleB` together with the existing exact draw and minimum-present
selectors. The selected PS retains its normal early fragment tests and MRT
dataflow; a host-only one-shot SSBO records RGBA count, nonfinite observations,
and ordered extrema after the sample and before its destination stores. The
absolute instruction ordinal, probe kind, descriptor set, and raw-layout
revision are part of shader/module identity. The focused graphics integration
validates the 47-word layout, fail-closed parser, absolute-ordinal contract,
SPIR-V ordering, early tests, and binary validity.

One strict Silent attempt selected ordinal 24 of the 15,366-index shader and
reported 737,427 finite post-depth observations with nonzero ranges
`R=0.611765..0.623529`, `G=0.184314`, `B=0.0941176..0.105882`, and `A=1`;
the paired fixed-test query reported `any_passed=1`. However, agent timing
reached present 8,689 before the first input, and the probe event occurred
1.1 seconds after that first edge but before the second edge. This excludes a
globally null/nonfinite base sample for that first-edge occurrence only. It is
not evidence for the intended post-two-input PLAY occurrence and is not a 3D
advance. Both taps eventually delivered, `last-error` was null, the ring had
no drops, memory peaked at 2,239,434,752 bytes with zero cgroup swap, and the
service was stopped. Do not change sample, LOD, tiling, or color arithmetic
from this timing-misaligned result; a future discriminator must gate the same
probe strictly after both input edges.

One timing-corrected retry moved the reservation gate to present 9,200 and
placed the entire `wait 8000 -> CROSS -> +80 -> CROSS -> event` route in one
local command. Its cold diagnostic cache reached only present 4,902 in the
unchanged 70-second gate, so the command timed out before sending either input
and no sample event was possible. The live process was still interactive at
8.925 FPS with `last-error=null`, no ring drops, no pending/delivered taps, a
2,149,572,608-byte cgroup peak, and zero swap; it was stopped immediately.
This is pre-gate performance evidence only. Do not lengthen or repeat the same
cold route. A later material discriminator must reuse a proven warm diagnostic
cache or otherwise reach the two-edge checkpoint without changing the render
contract.

The native controller scheduler now provides that reproducible route: one
request commits exactly two `cross` taps at presents 8,000 and 8,080 before the
first frame. A warm strict run using that route showed why minimum present alone
was insufficient. The first exact `indexed:15366` match after present 8,090
completed after both scheduled starts but reported `sn=0` together with
`any_passed=0`; it was a depth-rejected occurrence, not a null texture sample.
The sample probe therefore accepts the optional zero-based
`KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL=N`. Skipped exact matches stay ordinary and
do not allocate diagnostic Vulkan resources or consume the process one-shot;
invalid paired PS selection clears its VS peer. Parser/lifecycle integration,
a fresh-process Vulkan skip/select path, emitted `m=N` provenance, serial
`fc_script`, and independent review are green.

One strict warm run changed only that selector to `N=1`. Both scheduled inputs
were delivered, none was cancelled, and the second exact occurrence emitted:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 ord=24
sn=1618 snf=0 sfin=1
r=0.623529:0.623529 g=0.184314:0.184314
b=0.105882:0.105882 a=1:1 m=1
```

Its paired fixed-test query reported `depth=1`, `stencil=0`,
`any_passed=1 m=1`. `last-error` was null, the event ring had no drops, the
service used a rounded 1.6 GiB peak with cgroup swap disabled, and it was
stopped immediately without a capture. This excludes a null, nonfinite, or
black base `ImageSampleB` result for the contributing occurrence. Do not change
base image decode, sampler, LOD, tiling, attachment depth, clear, or HTILE from
this evidence. It is not visual/gameplay acceptance: final shader export,
blend/color attachment, and target lifetime remained unproven at that point.

The next bounded discriminator now observes one active MRT0 export by absolute
instruction ordinal immediately after final RGBA assembly and before the
unchanged `OpStore`. `KYTY_PS_MRT_PROBE` is mutually exclusive with coordinate,
sample-result, and fragment-tap diagnostics; it rejects inactive color-output
modes, zero-channel exports, wrong targets, and wrong opcodes before reserving
the process one-shot. It retains guest early fragment tests and reuses the
bounded 51-word aggregate under a distinct module identity and paired
`ps_mrt_probe` / `ps_mrt_coverage`
event. The existing focused integration validates source and binary SPIR-V,
output-store preservation, fail-closed selection, and the `m=1` skip/select
lifecycle.

One strict warm run kept the same exact draw, 8,090 present threshold,
zero-based occurrence `m=1`, and scheduled inputs at presents 8,000 and 8,080.
Both inputs delivered without cancellation. At present 8,090 the selected
final MRT0 export emitted:

```text
mrt=0 ord=229 on=408 onf=0 ofin=1
r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1
```

`last-error` was null, the event ring had no drops, cgroup swap was disabled,
and the service was stopped immediately without a capture. This proves that
the contributing occurrence reaches its unchanged MRT0 store with a finite,
nonblack assembled color. Combined with the preceding finite base sample and
passing fixed-test query, the live branch is now downstream of pixel-shader
material arithmetic. Do not change sample decode, sampler, LOD, tiling, depth,
clear, HTILE, or MRT export arithmetic from this evidence. It remains
diagnostic rather than visual/gameplay acceptance: blend state, color write
mask/attachment identity and layout, render-target lifetime, and later
composition remain unproven and are the next causal seam.

The exact second MRT occurrence is now disproven as a PLAY/gameplay scene
fence. A later output-preserving run kept the same selector and two scheduled
inputs while adding only host `FragCoord` extrema to the selected MRT export.
After both taps started, the paired events reported:

```text
mrt=0 ord=229 on=129111 onf=0 ofin=1
r=0.452881:1.33496 g=0.133789:0.381104
b=0.0769043:0.191284 a=1:1 m=1
x=427.5:1479.5 y=618.5:1079.5 cfin=1 m=1
```

The fixed-test query also reported `any_passed=1 m=1`. The immediate native
capture at present 8,212 showed the logo/PLAY prompt without the vehicle and
scored `low_entropy` (`0.2047`, 149 bins). Both taps delivered, none was
cancelled, the event ring had no drops, `last-error` was null, memory peaked at
a rounded 1.7 GiB with cgroup swap disabled, and the runtime was stopped
deliberately. Thus this exact draw executes broadly across the lower/right
host framebuffer and reaches a finite, nonblack unchanged MRT export; its
visible vehicle contribution is lost after that observation point. This rules
out zero/tiny raster coverage and total depth rejection for the selected draw,
but does not prove correct primitive placement or identify the later loss.
Investigate blend/write mask, bound attachment identity/view/layout, obsolete
clear or depth/HTILE state, and read/write lifetime through composition. Do not
use this occurrence as a semantic scene fence and do not claim a 3D advance.

The optional host-only post-blend readback now copies one eligible color
attachment after its render pass and maps it only after the owning command
buffer fence. It accepts a single-sampled transfer-source image in one of the
bounded known formats, caps the copy at 64 MiB, restores the tracked image
layout, and reports packed RGB occupancy plus a raw-byte hash. Its private
staging allocation is fail-closed and nonfatal; it does not use the renderer's
fatal general allocator.

The first strict use of that discriminator did **not** reproduce the preceding
positive occurrence. With the same checksum/draw/export selector, `m=1`, and
scheduled inputs, the selected draw instead reported `on=0`, `cfin=0`, and
`any_passed=0`. The completed 1920x1080 B10G11R11 attachment copy was uniformly
RGB-zero (`nz=0`, `in=0`), which is expected for a draw with no surviving
fragment invocation and therefore says nothing about blend retention. Both
inputs delivered without cancellation, `last-error` was null, the ring had no
drops, the cgroup peaked at a rounded 1.7 GiB with swap disabled, and the
immediate capture was the main menu rather than the failing 3D checkpoint.
This independently reconfirms that minimum-present plus ordinal is not a
stable scene/occurrence identity. Do not repeat that route or interpret the
zero attachment as a renderer fix or failure.

Attachment observation now boundedly re-arms after an empty fenced attempt.
It discards at most four empty exact matches, carries the already-consumed
match ordinal across each retry and context teardown, releases each staging
buffer only after its owning fence, suppresses intermediate events, and emits
the discarded count as `r=N`. The fifth attempt is terminal even when empty,
so the opt-in diagnostic cannot loop indefinitely.

One subsequent strict run reached the terminal `r=4` attempt with finite MRT
coverage (`b=1`) and read back the 1920x1080 B10G11R11 attachment as 82,791
RGB-nonzero pixels overall and 55,937 inside that same attempt's host
coverage box. This excludes a uniformly black immediate attachment for the
selected contributing attempt. It does **not** yet prove that those pixels
were written by that draw: the readback has no pre-draw snapshot and can count
content retained from earlier writers. The native client recovered the event,
but the 120-second service cap stopped the process before a capture or final
input/status query; the service used a rounded 1.8 GiB peak with swap disabled
and wrote no crash report. The next discriminator is a bounded before/after
attachment delta or an exact downstream overwrite/clear identity, not another
shader/sample probe or guessed ordinal.

That before/after boundary is now implemented without changing guest output.
For `LOAD` color attachments with defined contents, the probe copies the same
selected image immediately before its render pass and again after it, using two
32 MiB-capped coherent buffers (64 MiB total). Both copies stay on the owning
command buffer and are mapped only after its fence. `CLEAR`/`DONT_CARE` and
undefined initial state fail closed for the delta while preserving the existing
post-only event. The copy helper now restores
`COLOR_ATTACHMENT_OPTIMAL` with color-attachment read **and** write access;
the read scope is required for the implicit attachment load. The focused
contract and ordinal integrations, serial emulator build, and an independent
barrier/lifetime review passed with zero swap.

One strict Silent run then delivered exactly the two scheduled input edges at
presents 8,000 and 8,080 and selected the contributing 15,366-index occurrence
without a retry. Its exact-fence events were:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
on=120494 onf=0 ofin=1 r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
cfin=1 x=537.5:929.5 y=720.5:1079.5 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
f=b10g11r11 e=1920x1080 nz=122244 b=1 in=96923
h=e728d902f16790b5 ok=1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
l=0 d=94978 b=1 in=94978 up=94804 dn=0 m=1 r=0
cs=0a0005c0ef41d630 k=i n=15366 s=2 applicable=1
depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=1 m=1
```

Thus the selected draw changes 94,978 RGB pixels inside its own coverage after
a defined `LOAD`; 94,804 change from RGB-zero to nonzero and none change in the
opposite direction. Together with `any_passed=1`, this excludes total depth
rejection, a discarded render-pass load, a uniformly black immediate color
attachment, and a completely suppressed color write for this occurrence. The
native frame still showed only the logo/PLAY prompt, no vehicle or world, and
scored `low_entropy` (entropy 0.2083, 171 bins), so there is no visual advance.
The process was stopped deliberately at present 8,484 with `last-error` null;
the cgroup peaked at 1,937.6 MiB with zero swap. The next discriminator is now
the first later clear, overwrite, alias, resolve, or consumer of this exact
image before present. Do not change depth compare, MRT export arithmetic, or
the immediate blend/write path from this result.

The existing render-target lifetime trace now accepts an optional strict color
guest-address selector and includes `guest_count` in `WRITE` records. When the
color selector is active it no longer arms or emits depth lifetime events; an
unfiltered attempt had produced 128 unrelated depth records and reduced the
runtime to roughly 8 FPS. The filtered color path restores bounded diagnostic
cost and preserves later guest-or-host identity matching for rematerialization.
A subsequent audit also found that descriptor-sample auto-promotion could arm
an unrelated RT despite the selector; that promotion now requires the same
address match. The serial build and focused contract integration stayed green.

Two bounded attempts exposed a process-lifetime identity problem rather than a
renderer result. The first recovered the selected target as guest address
`0x4f250000`. It showed a first `CLEAR` writer, then `LOAD` writers, and a later
sample by the 1920x1080-to-960x540 downsample. That occurrence itself was the
fifth empty probe match (`r=4`, no fixed-test pass), so its sequence cannot be
used to explain the positive draw. In the next process the active full-size HDR
target was instead at `0x51b50000`; the stale address selector could not arm the
selected target, and the 15,366-index draw never appeared in that route. Both
scheduled inputs delivered in each attempt, both services were stopped
deliberately, cgroup peaks stayed below 2.0 GiB with zero swap, and there is no
visual evidence from either attempt.

This closes a cross-process guest address as a stable selector. Do not repeat a
filtered run using an address learned from another process. The next minimal
diagnostic must arm the lifetime target dynamically from the exact MRT probe
reservation inside the same process, then correlate its `guest_count=15366`
`WRITE` with the first later `WRITE`, `SAMPLE`, or `RESOLVE`.

That same-process arming is now available through the lifetime trace's explicit
MRT-probe mode. A successful exact FinalMrtResult reservation publishes its
current guest address and host allocation before descriptor binding and before
`TraceRenderTargetLifetimeDraw`; generic color arming, depth tracing, and sample
auto-promotion are disabled in this mode. A retry that selects a new ping-pong
image emits `PROBE_REMAP`. Mixed depth/color selectors fail closed.

One bounded run demonstrated the ordering, although all five selected probe
attempts were depth-rejected and the terminal result was again empty. The first
attempt emitted `PROBE_ARM` for a 1920x1080 format-122 image followed immediately
by the exact indexed `guest_count=15366`, pixel-shader `210005b0766a27a5`
`WRITE`. All later writers used `LOAD`; no later clear was observed. The first
consumer was the `210001e03375e575` downsample into 960x540, followed by the
existing derived chain and the `210006800bf364a9` full-resolution compositor.
The next frame selected the other 1920x1080 ping-pong image and correctly
emitted `PROBE_REMAP` before the same exact draw/consumer order.

This proves that the dynamic trace follows the exact target and that the normal
path is mesh writes -> downsample -> compositor, not an intervening clear. It
does **not** close a later overwrite or consumer defect for the prior positive
94,978-pixel occurrence because this run's selected draws had `any_passed=0`.
Do not change render-pass load ops or remove later guest draws from this empty
sequence. The next useful run is the same bounded dynamic trace only when the
probe also reports `d>0`, `in>0`, and `any_passed=1`; then inspect the first
later significant write and downsample result from that exact attempt.

A draw-scoped fragment-tap retry for the 3,564-index material was allowed one
unchanged 60-second gate after the diagnostic shader and pipelines had already
been cached. It completed both requested inputs and captured at present 8,093
with no structured error, but input timing placed the workload in the main menu
rather than the PLAY-era vehicle draw. The menu capture scored
`hot_corruption` (entropy 1.7525, 78 bins) and cannot establish whether the
vehicle's `param4/location4` reached the pixel shader. Do not treat the absence
of the tap color in that frame as a producer failure, and do not repeat the
same route merely to force the desired checkpoint.

Offline extraction of the persisted translator-32 modules then narrowed the
post-HDR path. The downsample uses one `ImageSampleImplicitLod` with Bias; the
active compositor uses two equivalent HDR samples plus four explicit LOD-zero
LUT samples. Their `{bias,x,y}` and LOD-zero lowering agrees with the current
Gen5 image contract, and all extracted modules pass SPIR-V validation. The
compositor also has a distinct no-input variant whose implicit sample
coordinates are `(0,0)`; the active variant instead consumes a smooth
`Location 0` varying. `ShaderGetIdPS` already separates those interfaces, so a
cache collision is not established. The bounded render-target lifetime sample
trace now records `input_num`, the first four interpolator settings, source and
destination host extents/formats, view, and layouts. It remains opt-in and does
no image readback. One strict two-edge cycle captured the first downsample and
the compositor with a real destination: both used `input_num=1`, interpolator
zero `0x00000000`, and the exact 1920x1080 HDR image. The downsample wrote
960x540 and the compositor wrote 1920x1080, all in host format 122 with view
zero. This excludes the no-input constant-coordinate variant, a cache-interface
collision, and an attachment identity/view/format/extent mismatch for that
cycle. It does not inspect interpolated coordinates, scale arithmetic, the
sample result, blend, or the compositor output. Its native capture remained
black apart from the title/HUD and a thin horizontal fragment
(`healthy=false`, entropy `0.2337`, 164 bins).

The lifetime trace can now start at a minimum presentation without depending
on the broader material trace, preserves exact guest-plus-host identities for
derived render targets, reserves a consumer event, and promotes only an exact
render target sampled into a distinct smaller destination. A later strict
Silent no-input run reached present 8,017 with no structured error but executed
neither known downsample nor compositor shader; it produced no sample or
derived-chain event. It is not evidence for or against compositor identity or
content. Do not repeat that no-input route. The next bounded integration must
use the established two-edge route and stop after the first complete derived
cycle.

A subsequent cgroup-bounded strict Silent two-edge cycle did exercise one
complete derived edge. The half-resolution producer sampled the exact
full-resolution HDR guest-plus-host image and wrote a 960x540 format-122 target;
the immediate consumer sampled that exact 960x540 identity in the same cycle.
Offline inspection identifies this consumer as a positive-weight nine-tap
horizontal blur rather than the full-resolution compositor. This closes a
missing bind, guest-upload substitution, and host-image identity mismatch at
that edge, but not the sampled texel contents, blur destination, or a later
overwrite. The native capture at present 9,840 still showed coherent PLAY UI
over a black world (`healthy=false`, entropy 0.2089, 173 bins), with no
structured error or blocked synchronization wait. Do not repeat the lifetime
trace for the same identity question. The next bounded integration should tap
the 3,564-index material's raw base sample at ordinal 24 and compare it with the
retained draw-scoped `param4/location4` coverage before testing final material
channels or changing renderer semantics.

Two attempts to collect that raw-base discriminator produced no material
evidence. The first split the agent route across host calls and let the hot
pre-input loop overshoot into the credits screen before the input sequence
completed. The one allowed warm retry reached present 8,000 and delivered the
first edge at 8,006, but its local route driver aborted after misreading the
valid JSON value `tap_pending=false` as a failed predicate; it sent no second
edge and requested no capture. Both runtimes were stopped cleanly. A future
attempt must use one atomic local route and non-predicate boolean parsing; do
not lengthen the gate or use either failed attempt as shader evidence.

The corrected atomic route subsequently completed both delivered edges and
both 40-present deltas, then captured the draw-scoped raw four-channel base
sample at present 8,098. It exposed additional nonblack material fragments but
still no coherent vehicle or world (`healthy=false`, entropy 0.3657, 183 bins),
with no structured error, blocked synchronization wait, or excessive resident
memory. A retained exact trace contains one 3,564-index instance in its bounded
window, but the older `attr3` image is eleven presents earlier and visualizes
only one scalar channel. Their low mask overlap cannot establish incorrect UV
or geometry coverage.

The remaining parent-differential candidate was the BC mip tail: this material
declares eleven 1024x1024 BC1 levels while the current host policy stops at 4x4.
Vulkan permits complete 2x2/1x1 compressed mips at a subresource edge. A
test-first experiment preserved all guest levels and passed the existing
focused mip contracts, but its strict runtime did not reach present 8,000
inside the unchanged 70-second gate. It remained healthy near present 5,843
yet fell to roughly 4.5 FPS as per-frame texture upload/recreation work grew to
thousands of calls and tens of MiB; no input or capture occurred. The experiment
and test were restored and both binaries rebuilt. Record the truncated tail as
a general correctness gap, not the demonstrated 3D producer. Before retaining
all levels, capture the effective material LOD and eliminate the newly exposed
texture lifetime/upload churn; do not repeat the same toggle.

The next bounded attempt selected only compositor checksum
`0x210006800bf364a9` after present 8,080 to inspect its dynamic storage values.
The workload stalled before input at present 6,264 for about 154 seconds;
`phase=stalled`, `last-error=null`, and the trace filter had not armed. The
process was stopped deliberately without retrying or extending the timeout.
This is liveness-failure evidence only and provides no compositor constants or
visual A/B. A deterministic bounded route still has to distinguish consumer
coordinates/sample arithmetic, dynamic storage values, compositor output, and
a downstream overwrite; do not change Bias, LOD, SLOAD, interpolation, or depth
speculatively.

A later pre-input checksum-scoped trace did capture all 28 floats read by that
compositor variant. The 112-byte range was fully materialized and in bounds;
its active values were coherent with neutral sampling and a 1024x32 packed LUT
(`bias=0`, texel steps `1/1024` and `1/32`, dimension-minus-one `31`, unit
gain, and the optional radial mask disabled). None of those coefficients can
turn every finite nonnegative HDR sample black by itself. This draw targeted a
different pre-input host format and address than the failing gameplay-era
consumer, so it excludes only an obvious coefficient/range defect in that
variant. It does not prove the gameplay compositor input, sample results, or
output.

The pixel sample probe now has an opt-in sparse observation mode. It elects
one host invocation per fragment subgroup before the existing aggregate, so a
full-resolution diagnostic no longer executes the aggregate atomics for every
fragment. Sparse modules have a distinct diagnostic identity, require fragment
stage basic subgroup support, and fall back to the unchanged ordinary draw
before lifecycle reservation when that host capability is absent. Malformed
configuration fails closed. The existing graphics integration validates the
generated structured SPIR-V, unsupported-host fallback, exact fence lifecycle,
and explicit `sparse=1` event provenance; serial builds and independent review
passed.

One strict two-edge run applied the sparse probe to the active full-resolution
compositor's first HDR sample. It recorded 66,561 elected subgroup lanes, zero
nonfinite values, `R/G=0..0.215088`, `B=0..0.0562439`, and `A=1`, with both
scheduled inputs delivered, no cancellations, no ring drops, and no structured
error. The bounded service peaked at 2,021,982,208 bytes with zero swap and was
stopped immediately. The native capture at present 8,183 showed only the
logo/PLAY screen and scored `low_entropy` (`0.2046`, 150 bins). Therefore this
excludes a globally black or nonfinite compositor input for that exact title
screen occurrence only. It is not gameplay, visual acceptance, or evidence
that the later compositor output/attachment is correct. Do not cite it as a 3D
advance or repeat the same route merely to obtain a later screenshot.

The Gen5 HLE `EVENT_WRITE` encoder previously discarded every non-null address
and always emitted the two-dword form. Addressed event type `0x39` now
emit the evidenced four-dword packet with event index one and an aligned 64-bit
destination; the PM4 parser consumes either form. Until native host occlusion
queries exist, event `0x39` publishes ready, monotonically increasing begin/end
counter values to the sixteen interleaved DB pairs after validating the guest
range. The existing graphics diagnostics integration executes the addressed
packet through the real PM4 parser, checks all sixteen DB pairs, monotonic
increment, following-packet alignment, and rejection of a misaligned target;
it also protects the ordinary short encoder boundary. One bounded strict
two-edge run reached present
11,752 with no structured error or blocked wait, but its capture remained
nearly black (`healthy=false`, entropy `0.2024`, 158 bins). No retained counter
proves that this workload executed event `0x39`, so this is a general contract
correction, not evidence that occlusion caused or fixed the missing 3D. Event
`0x38` remains on the short form until its addressed semantics have independent
evidence and a matching consumer.

Two general shader-contract defects remain recorded but are not established as
the current visual producer. First, pixel analysis calculates an exact
`required_subgroup_size` for shaders using guest-wave operations, while graphics
pipeline creation does not attach a
`VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT`; device creation also
does not enable the subgroup-size-control feature. The current Intel host
reports a default subgroup size of 32 and a controllable range of 8 through 32,
so this omission does not demonstrate a mismatch for the observed material.
A general correction must query and enable the feature, include the required
width in pipeline creation/identity, and reject a guest width the host cannot
represent rather than silently falling back. Second, `ShaderParseEXP.cpp`
retains the guest `EN` nibble for parameter exports, but the `ParamN` SPIR-V
lowering writes all four source channels. The shader probe now includes that
already-decoded nibble in its opt-in instruction dump. A bounded strict probe
resolved the exact VS masks as `param0=0x3`, `param1=0x7`, `param2=0x7`,
`param3=0x1`, and `param4=0x7`; `Pos0` remained `0xf`. Thus all three
`param4.xyz` channels that feed the observed material normal/color path are
enabled and only its unused `w` source is outside the guest mask. Partial
parameter lowering remains a general contract gap, but it does not explain the
failing draw's dark XYZ or missing position geometry. Do not change export
behavior without a red partial-mask consumer contract; neither defect is
strict 3D acceptance.

A narrower translator correction now avoids native lane exchange for
`V_READFIRSTLANE_B32` only when a same-basic-block reaching-definition proof
shows a wave-uniform source and unchanged initial EXEC. The proof rejects
vector VCC writers, every supported implicit `VCMPX`/`S*SAVEEXEC` EXEC writer,
multi-register VGPR overwrites, control-flow boundaries, and incoming edges
that can bypass a producer; all other cases retain the original subgroup
ballot/broadcast path. On the exercised material module this removed the
ballot/count/find-first/broadcast sequence, and the rebuilt SPIR-V validated.
It did not remove the module's subgroup requirement: the selected module still
contains a VCC-wide branch reduction and a lane-ID-addressed buffer operation,
so the host trace correctly remained `required_subgroup=64`. A strict bounded
route delivered exactly the two scheduled input edges and captured a coherent
main menu, not gameplay. This is a verified general translation improvement,
not evidence that wave width caused or fixed the missing 3D.

The mask probe was produced before the diagnostic input gate. Its bounded
`wait-present --min 8000` attempt timed out at present 6,094 after 70 seconds;
`last-error` was null, presentation was still advancing near 9.7 FPS, and
`sync-waits` reported no blocked wait. No input or capture was requested, and
the sole process was stopped immediately after reading the probe. This run is
parser evidence only, not a visual result or a liveness regression.

**Host submission failure contract:** the current worktree publishes and
reuses a command buffer only after successful submit and fence completion.
Timeout, not-ready, device-loss, and success fixtures cover the pure policy;
blocking waits are bounded and no incomplete fence drains callbacks, resets the
buffer, or clears its in-flight state. `KYTY_SUBMIT_FAULT_TRACE=1` adds a
CPU-only eight-attempt trail across every existing submit and emits it once on
the first observed device loss. A submit failure identifies that exact host
attempt; a loss first observed by fence, acquire, or present leaves only a
bounded predecessor set. It does not identify a raw Vulkan command and is not a
visual fix. One guest run has now exercised the enabled no-loss path; because no
loss occurred, the one-shot fault emission and submit join remain
runtime-unexercised.

**Linked-buffer lifetime frontier:** the same post-input snapshot grew from 15
live GPU objects to 5,038, including 4,659 live `StorageBuffer` objects and
3,916 `NewLinked` creations, while instantaneous FPS fell below 4. Static
tracing confirms that distinct overlapping storage ranges create distinct
linked backings and that periodic retirement skips every object with alias
links. This is evidence of an unbounded-lifetime mechanism, not yet proof that
all observed objects are stale, that it caused the earlier device loss, or that
it produces the missing geometry. Any correction must preserve submission
dependencies, GPU-owned surface state, and writable-buffer publication.
The current worktree adds anonymous aggregate `NewLinked` topology categories
for buffer-only read-only, surface-connected, mutable/other, and truncated
components. Classification is limited to 64 unique nodes and 128 examined
parent/link edges, and every `NewLinked` event contributes to exactly one
category. One later strict, 150-second cgroup-bounded run reset this window
before submitting one diagnostic `cross`. Over the following 107.7 seconds it
recorded 7,445 linked storage creations: 1,767 buffer-only read-only, one
mutable/other, no proven surface connection, and 5,677 traversal-truncated.
Live storage objects reached 8,145, live objects overall reached 8,524, and
instantaneous FPS was 6.754. Host memory peaked at 2,156,904,448 bytes with no
swap before the timeout terminated the process. The dominant truncated result
means the run does not prove those components surface-free; it rules out an
unconditional linked-object retirement and does not yet establish a safe or
effective lifetime correction.
`FrameDone` now has one conservative correction for the proven subset: it may
retire a component containing storage only after a complete current preflight
shows no more than 64 SB/VB/IB nodes and 128 links, all read-only, `Common`, at
least 120 frames old, free of bound depth metadata, and with completed
submission dependencies. It frees the complete component or nothing, and a
2,048-unit global scan budget bounds each retirement pass. Host tests prove
successful whole-component retirement with a real storage write-back callback,
and prove that truncated and surface-connected components remain intact. No
visual acceptance has exercised this correction. A later single strict run did
exercise it: before input, one linked storage creation had one logical free and
five live storage objects. After resetting the window and submitting one
diagnostic `cross`, 57.752 seconds produced 8,135 linked storage creations but
only 102 logical frees; storage live count reached 8,902, total live objects
9,249, and instantaneous FPS fell to 2.723. Of those links, 6,320 were
traversal-truncated and 1,815 buffer-only read-only. The bounded watch still
observed frame/present progress and no last error, while cgroup memory peaked at
2,185,240,576 bytes with zero swap before timeout exit 124. This falsifies
fixed-size whole-component retirement as a sufficient containment mechanism;
it does not prove that lifetime growth causes the 3D failure.
Resolved later: each idle read-only buffer member now retires on its own (see
"Linked read-only buffers retire one at a time" above).

The current host-only follow-up routes safe read-only buffer snapshots up to
512 KiB through the command-buffer-owned transient pool instead of creating a
persistent alias. The pool was then capped at 16 MiB (64 MiB since 40c887ad), reserves 1 MiB for
mandatory UBO/scratch uploads, clears the unused tail of reused slabs, and keys
storage/UBO descriptors by both backing identity and logical range. Writable,
surface-connected, unallocated, oversized, and truncated overlap states retain
the authoritative persistent path. One subsequent 105-second strict diagnostic
run reset the counters before its first input edge. After 58.497 seconds and two
bounded input edges it had created only 18 linked storage objects, retained 30
live storage objects and 116 live GPU objects overall; the previous comparable
post-input window created 8,135 linked storage objects and retained 8,902
storage objects and 9,249 objects overall. The sampled cgroup peak after the
first transition was 2,149,568,512 bytes with zero swap, and timeout ended the
sole process with exit 124. Native captures showed a coherent credits screen
and main menu with no directional stripes, but the capture gate correctly
classified both as non-gameplay scenes. This establishes containment on that
observed route, not correct PLAY geometry, long-session memory stability, or
playability.

**Visual frontier (not yet playability acceptance):** horizontal stripes and
opaque black sprite/prop rectangles are absent after the RenderTexture layout,
null MRT discard-tail, and pixel-kill late-depth fixes. The rectangle producer
was Vulkan `EarlyFragmentTests` committing depth before an existing `OpKill`
in a guest `EarlyZThenLateZ` shader. A gameplay-era native discovery capture
shows coherent background, props, character, lighting, and transparency.
The latest strict diagnostic route additionally exercised sustained directional
movement and stable presentation with healthy output. Formal acceptance still
requires a repeatable non-diagnostic controller run, an action beyond movement,
and validation-clean output.

The current Gen5 missing-geometry investigation has also narrowed one material
path. A selected VS exported finite clip position and finite `PARAM0.xy`; a
diagnostic-only late-depth PS variant then observed 167 finite input-zero
fragments whose aggregate extrema matched that producer. Ordinary shaders and
selected shaders with any retained guest side effect keep their guest
`EarlyFragmentTests`; only the cache-separated, statically side-effect-free
host probe executes before the ordinary depth test. Thus the earlier zero PS
count was early-depth rejection, while the new aggregate excludes only zero,
grossly out-of-range, and non-finite input for this occurrence. It does not
prove per-fragment correspondence, derivatives, or exact interpolation. The
selected draw occupies only a thin NDC band and is not evidence for the full
missing vehicle silhouette. Strict 3D recovery remains unproven; first
correlate the actual large-geometry producer or occluder with a same-scene
native capture before changing depth, interpolation, texture coordinates, or
renderer lifetime.

The exact FinalMrtResult diagnostic now also retains the last depth clear for
up to 64 exact `{guest depth address, host image identity}` pairs from process
start, independently of the later trace threshold. Normal indexed/auto passes
and both direct depth-copy render-pass routes feed the same bounded tracker;
the newest 256 possible depth writers remain a separate recent-history window.
The focused graphics integration and serial `fc_script` build pass under the
zero-swap cgroup. Independent review found the render-pass coverage and tracker
mechanics sound, but noted that eviction and direct-copy behavior do not yet
have their own automated contract; the strict runtime below is the retained
integration evidence for the exact pre-threshold identity.

One corrected strict Silent run scheduled only `cross` at presents 8,000 and
8,080. Five selected 15,366-index attempts at presents 8,109 through 8,113 had
the same exact depth/stencil read-write bases, host image, HTILE range,
`LOAD`, defined attachment layout, enabled depth test/write, and reverse-Z
`GEQUAL`. For every attempt the tracker recovered the same last clear at
present 8,009: `load=CLEAR`, initial and tracked layout `UNDEFINED`,
`clear=1`, `suppress=0`, and the same depth/host/HTILE identity. No later
normal or direct-copy clear replaced it. The run peaked at 1,911,508,992 bytes
with zero cgroup swap and was stopped deliberately. A native capture at
present 8,497 still showed only the logo/PLAY screen and scored `low_entropy`
(`0.2084`, 171 bins), so this is diagnostic evidence, not a 3D advance.
It proves that the recent empty occurrences did not use an unknown split
attachment or an unobserved later clear; it does not prove the old clear is
wrong, because preceding `GEQUAL` writers may legitimately occlude them and a
prior passing 15,366-index occurrence already proves this attachment sequence
can pass substantial geometry. Do not synthesize a per-frame clear, disable
depth, or reopen HTILE identity from this result. Return to the first later
color mutation/consumer of a contributing occurrence or to a separately
captured renderer invariant.

The FinalMrtResult attachment probe can now require a significant contribution
with the host-only positive decimal
`KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS=N`. It is valid only when attachment
readback is enabled, defaults to one, leaves shader identity unchanged, and
re-arms sub-threshold fenced results within the existing eight-retry bound. The
existing graphics integration was extended rather than adding a new suite; its
RED compile, GREEN serial build, and focused contract run are retained.

One strict two-edge run with a 10,000-invocation minimum selected 17,443 finite
MRT invocations, passed fixed depth tests, and changed 17,008 pixels inside its
coverage from zero to nonzero. Same-process lifetime tracking then recorded 118
later `LOAD` writers with the same exact HDR identity before the known
half-resolution downsample sampled that image as `rt-exact`. No later clear,
identity split, guest-upload substitution, or resolve preceded the consumer.
The 128-event cap ended before the full-resolution compositor, and the native
capture was a later credits frame, so neither compositor output nor same-scene
visual recovery is proven. Do not reopen immediate MRT/depth/attachment
identity or remove later guest draws. The next bounded seam is one intervening
writer's actual overlap/delta or the downsample's input-to-output content.

The first post-material writer is now identified as PS
`2100099068cc5c23` with VS `0a0005c092436153`. A strict bounded trace captured
four 252-index occurrences: all sampled indices fit 250 declared records, the
stride-48 position/normal/half-UV layout is valid, sampled inputs and known
object coefficients are finite, all textures are bound, depth provenance is
exact, and DCC/CMASK are off. A NaN in the generic fixed `VS_SLOT_OFF272` peek
is not evidence because no read of that skybox-oriented offset is established.
Two exact post-transform probe attempts (70 and 100 seconds) ended at their
watchdogs before present 8,090, below 2.21 GiB and with zero cgroup swap; they
yielded no vertex result and must not be repeated or lengthened. Continue at a
bounded writer attachment delta or downsample input/output boundary.

That attachment-delta boundary now excludes the first writer. A targeted
ShaderProbe established its only final MRT0 export at ordinal 402. The exact
`indexed:252` MRT probe then exhausted the first occurrence plus eight bounded
retries with zero MRT invocations, zero coverage, `any_passed=0`, and zero
changed attachment pixels, while the attachment remained valid. Both inputs
delivered, no event was dropped, `last-error` was null, peak memory was about
1.81 GiB, and cgroup swap was zero. These draws cannot alter the earlier
material contribution; do not reopen their VS or depth identity. The next
unclassified lifetime writer is PS `210001009057ad42`, `indexed:2112`.

The 2,112-index writer is now excluded in the correct later phase. Its
alpha-tested PS has one enabled MRT0 export at ordinal 18. At present 8,093 it
produced 858 finite candidate colors over finite coverage while the exact HDR
attachment already contained 40,995 nonzero pixels, but fixed depth reported
`any_passed=0` and the before/after attachment delta was exactly zero. Inputs,
event ring, error state, and the 1.76-GiB zero-swap resource envelope remained
clean. Candidate shader output is not a color write: this writer cannot cause
the observed smear. Continue with PS `21000220c3cdade2`, `indexed:1536`.

The procedural 1,536-index PS is also excluded. Its only enabled MRT0 export
is ordinal 43, and an exact present-8,092 run exhausted nine bounded
occurrences with zero MRT invocations, zero coverage, `any_passed=0`, and zero
attachment delta while 95,457 nonzero pixels remained present. Runtime health
and the 1.76-GiB zero-swap envelope stayed clean. The repeated PS `21000870488b957d`, `indexed:2418` candidate is now excluded.
Targeted ShaderProbe identified its enabled MRT0 export at ordinal 363. An exact
probe across match 0 (load_op=1 / CLEAR) and match 1 (LOAD) exhausted all 9 bounded
occurrences with zero MRT invocations (`on=0`), zero finite coverage (`cfin=0`),
`any_passed=0`, and zero attachment delta (`d=0`). This definitively closes the
census of posterior writers: no posterior draw alters or occludes the material
contribution. The investigation focus shifts exclusively to the vertex shader
`0a0005c0ef41d630` dataflow, numerical lowerings, and vertex-to-pixel interface.

The existing VS aggregate was next paired with the significant MRT selector
for VS `0a0005c0ef41d630`, PS `210005b0766a27a5`, `indexed:15366`, MRT0
ordinal 229, match 1, and a 10,000-invocation minimum. The exact fence produced
9,151 finite clip and `PARAM0.xy` observations with zero NaN/Inf. Clip
population was `wnp=6177`, `oxy=1731`, `in01=1243`; `PARAM0.xy` stayed in
`[-0.088562,0.973145] x [0.788574,0.845215]`. Fixed depth passed and the draw
changed 86,376 pixels from zero to nonzero. The run used the exact taps at
presents 8,000 and 8,080, had no dropped events or structured error, peaked at
1.9 GiB, and used zero cgroup swap.

The native frame at present 8,223 was the main menu. Its `hot_corruption`
classification is a menu-color false positive, so the wide finite clip range
and nonpositive-W population are not evidence about the reported damaged 3D
scene. Do not sanitize position, change matrix offsets, or alter clipping from
this occurrence. Reuse the now-working paired probe only after establishing a
same-scene selector for a frame that visually contains the progressive 3D
stretching.

A subsequent strict create-order failure identified one real missing lifetime
contract: an existing IndexBuffer may be wholly contained by a newly registered
RenderTexture. The inverse direction and Texture form already preserve both
views. The render-target policy now links only the observed
`IndexBuffer IsContainedWithin RenderTexture` relation. A focused graphics
integration derives this relation through the production create path, failed
with the same `DATAERR` before the change, and now proves that both views remain
live while partial, reverse-containment, and exact forms stay strict. The rebuilt strict
runtime passed the former `DATAERR`, reached present 8,150, delivered exactly
the taps at 8,000 and 8,080, and left no process after its watchdog. Build and
runtime cgroups both used zero swap; runtime peak stayed near 2 GiB.

This advance is not the 3D correction. The new render target occupies
`0x478d0000..0x478e0000`; the retained 41,910-index draw uses index range
`0x47a00030 + 83820` and vertex range beginning at `0x47900020`. The ranges do
not overlap. Do not add image-to-index readback for that draw or generalize a
foreign buffer-cache policy from this alias.

A cold-cache strict replay with the correct VS selector and floor 8,000 reached
damaged gameplay at present 8,932. The selected 41,910 occurrence completed
before the second tap: all 27,937 outputs were finite, but 25,677 had
non-positive W, the remaining 2,260 were outside XY, no invocation entered
either Z population, and fixed depth saw no passing sample. Moving only the
floor to present 8,500 reached gameplay but produced no later matching result
before the 115-second watchdog. Both runs peaked around 2 GiB with zero swap.
Checksum plus index count is therefore not a unique gameplay fence; do not
repeat or lengthen that selector. Correlate a bounded gameplay-phase draw trace
with the damaged frame before changing transforms, exports, depth, or resource
materialization.

A bounded post-8,800 PS census then found one 234-index blended draw whose valid
80-record RGB32F position stream contains NaN triples at indexed records 39,
58, and 77--79. Its index range, stride, format, semantic split, and Vulkan
layout are internally coherent, and the persisted VS directly propagates the
loaded position through multiply/add/FMA to the clip export. This proves a
non-finite bound stream, not its provenance or visual ownership. The census was
too intrusive for scene correlation: it slowed to 0.672 FPS at present 8,813
while still `loading`, produced no capture, peaked at 2.16 GiB, and used zero
cgroup swap. Do not repeat the 32-entry census or add an unconditional NaN
clamp.

The later exact draw trace closed the float-mode question without authorizing a
global NaN rewrite. The fused vertex stage reported `dx10_clamp=1` and
`ieee_mode=0`, but the RDNA2 contract applies that mode to the per-instruction
floating `CLAMP` output modifier: ordinary unclamped `V_ADD`/`V_MUL`/`V_FMA`
still propagate NaN. Kyty now carries the effective VS/GS/PS mode into shader
identity and emits the mode-correct post-operation clamp (`NaN -> +0` only for
DX10 clamp, preserve NaN otherwise, and ignore output modifiers in IEEE mode). Superseded 2026-10-09 for `CLAMP`:
IEEE mode ignores `OMOD` only; see the interop integration entry.
Translator version 34 prevents cache aliasing with earlier modules. Focused
SPIR-V integration validates the DX10, non-DX10, unclamped, and IEEE variants;
this is a general ISA correction, not a compatibility claim. The exact shader
contains no instruction clamp in its position chain, so this change cannot
repair its non-finite output and must not be broadened into input sanitization.

The same strict trace found no currently live `GpuMemory` owner or alias for the
960-byte position span. That excludes a current tracked writable storage/RT
object at the selected bind, but does not exclude a historical writer that was
freed before the draw or guest CPU-authored sentinel data. A subsequent exact
vertex-output probe after both established inputs observed 80 invocations, 74
non-finite position exports, and only six finite outputs; the finite subset lay
inside both supported Z clip conventions. The paired fixed-function query saw
depth enabled and no passing sample. For this draw the failure therefore exists
before depth, clear, HTILE, rasterization, or fragment shading. Its raw format-74
RGB32F bytes and translator output agree, so the next investigation boundary is
the producer/lifetime of that guest range or a proven cross-API non-finite clip
rule, not a depth override, format substitution, or global clamp.

The empty live-owner result was a topology snapshot, not temporal provenance:
the vertex path first captures eligible read-only guest bytes into a transient
buffer, so no persistent `GpuMemory` identity is expected. An opt-in writer
history now records effective DMA, immediate `WriteData`, constant-RAM dumps,
addressed occlusion `EVENT_WRITE`, and GPU writeback operations as distinct
classes. Exact `addr:size` mode retains only overlaps in a fixed 128-entry
ring. Relocatable `auto` mode lazily allocates a bounded 65,536-entry ring and
retains covered events until the draw can query its actual guest VA; the large
ring is not reserved in disabled or exact mode. Both modes expose at most the
latest 16 matches and report per-recorder totals, retained/dropped counts, the
total matching count, and output truncation. The recorder is disabled by
default and uses an atomic fast path when unarmed.

An exact-range attempt using the prior process's VA was inconclusive because the
position allocation moved in the next process. The subsequent strict `auto`
run removed that process-relocation bias: at the selected draw it covered the
actual 960-byte range, retained 62,746 eligible events with zero drops, and
found zero overlaps. Recorder totals were 43,493 normal DMA operations and
19,253 immediate `WriteData` operations; custom DMA and all GPU writeback
classes were zero. That historical run preceded the constant-RAM and addressed
`EVENT_WRITE` hooks, so it does not exclude those classes retroactively. Both
scheduled input edges were delivered, `last-error` was
empty, and the process was stopped deliberately after the trace with a 1.9 GiB
memory peak and cgroup swap disabled. This falsifies only the classes covered in
that run. Direct guest CPU stores and deferred EOP `WriteData` remain explicit
blind spots, so the trace does not prove comprehensive provenance or that the
NaN records are intentional. The real command-processor execution integration
now proves that constant-RAM dumps and addressed `EVENT_WRITE` publish history
after their effective host write and flush.

AMD's public RDNA performance guidance explicitly recommends culling a primitive
from the vertex shader by setting any vertex position to NaN. Vulkan defines
clip-volume inequalities but does not state that cross-vendor hosts must preserve
that AMD primitive-assembly behavior. This makes a post-shader, per-primitive
NaN cull the leading general portability hypothesis on the Intel host. It does
not justify treating infinity the same way, changing guest buffers, using a
per-vertex `CullDistance`, killing fragments, or skipping the whole draw. The
bounded history has excluded its covered writer classes without proving whether
the remaining data came from direct guest CPU stores, deferred EOP publication,
or an intentional sentinel. The next rendering experiment must be
primitive-aware NaN culling with a deterministic post-shader contract, not a
numeric sanitizer.

That host-behavior experiment rejects the need for an inserted geometry stage
on the current Intel Vulkan path. A color-only 8x8 integration with depth,
stencil, and face culling disabled measured 18 occlusion samples for a finite
control triangle and zero samples for the same triangle with only one final
`Position.z` changed to quiet NaN. The exact runtime draw also produced zero
passing samples. Therefore the host already suppresses the NaN primitive in the
observed configuration; adding a pass-through geometry shader would not change
this draw and is not justified as the next 3D fix. Retain the AMD rule as a
portability requirement for hosts that classify the diagnostic differently,
but return the active investigation to a visually correlated damaged-gameplay
capture and its actual producing draw. Infinity and finite-but-explosive clip
outputs remain separate cases; do not infer their behavior from the NaN result.

The current strict UI route is now reproducible without relying on wall-clock
menu guesses. Scheduled `cross` taps at 80-present intervals establish these
checkpoints: three taps reach track selection, four reach difficulty, five
reach the black vehicle-preview `PLAY` screen, and six enter the next loading
phase. A six-tap run delivered every edge with no cancellation and no runtime
error, but did not reach gameplay within a 180-second bound. During the second
half it remained in `phase=loading` at roughly 3.4--3.8 FPS, advancing only from
about present 10,285 to 10,721 while the native capture stayed a black logo
card. Host memory peaked at 1.9 GiB with cgroup swap disabled.

The bounded native performance snapshot for that same route is complete; do
not repeat it. Shader generation and pipeline compilation are not the sustained
cost: only seven SPIR-V source/compile operations occurred, and slow frames did
not correlate with pipeline misses. The 119-second window instead attributed
about 95.7 seconds to command processing, 57.2 seconds to draw processing, and
31.0 seconds to draw-state setup. Slow frames repeatedly performed thousands
of `GpuMemory::CreateObject` calls and up to roughly 20,000 uploads (about
72 MiB) per frame. Transient read-only probes accepted about 99% of candidates,
but acceptance means that a command-buffer-owned snapshot was uploaded, not
that content was reused. Fence waits and `WAIT_REG_MEM` were frequent but
individually bounded; the evidence does not show a deadlock. The first strict
blocker is therefore identifying the exact resource type/outcome or draw-state
producer behind that repeated materialization, then changing one ownership or
reuse contract. Do not add a broad cache, overwrite in-flight transient data,
or select another gameplay shader until that producer is causal.

That producer is now narrowed to transient snapshot capacity and large
read-only vertex views. Command-buffer-owned snapshots can reuse the most
recent exact `address/size/usage` entry only when a byte-for-byte comparison,
performed inside a temporary dirty-page transaction, proves that current guest
contents are unchanged. `BeginRead` samples generation before and after arming
protection, capture/compare validates the same observation after the read, and
first-use ranges acquire and release a temporary tracker reference. A host
notification or write fault during either window therefore fails closed before
the transient entry is committed or reused. Changed contents and ranges with a
mutable GPU overlap fall through to a fresh capture or authoritative
`GpuMemory` path. The earlier 1.72-million-reuse run predated this correction;
its 0.15 seconds measured only `memcmp`, not tracker arm/restore cost, and its
visual output must not be treated as accepted evidence.

The remaining fallback was explicit: vertex views between roughly 1.3 and
3.1 MiB exceeded the old 1-MiB per-snapshot ceiling, repeatedly met 24--33
overlap candidates, and were reclaimed/recreated. One run recorded 2,847
`VertexBuffer` `reclaim_new` outcomes and hashed about 6.2 GiB of vertex input.
Raising only the per-entry ceiling to 4 MiB, while retaining the 16-MiB pool and
its critical reserve, removed all vertex reclaims in one warm-cache run and
reduced vertex hashing to about 1 MiB. That run's car-selection capture showed
an enlarged, gray, torn vehicle, but exact reuse still had a CPU-writer race at
that point. The frame is therefore a useful historical symptom, not accepted
evidence for the corrected renderer.

The 4-MiB ceiling alone is not sufficient. A second bounded route exhausted
the unchanged snapshot pool and again recorded 1,850 vertex reclaims plus about
4.2 GiB of vertex hashing, even though sampled slow-create sizes were only
1.67--2.46 MiB. Do not grow the pool globally: the next ownership experiment
must let a previously captured larger vertex range serve an unchanged contained
view with an explicit Vulkan vertex-buffer offset. Storage descriptors and
changed/mutable ranges must retain exact captures. Manual present-scheduled taps
also landed on different screens after the speed change; correlate future draw
probes with a one-input-at-a-time captured route rather than tap counters alone.

The corrected temporary transaction has now been exercised in three bounded
strict runs with hard memory and zero-swap limits. One no-input route measured
1,309,061 transient probes, 1,302,623 hits, and 716,848 exact reuses; tracker
validation consumed 5.75 seconds of an 86.6-second snapshot, versus 0.096
seconds in byte comparison. This overhead is material but not the dominant
70.8 seconds of command processing. A later one-input-at-a-time route reached
the vehicle-preview `PLAY` card after three delivered edges. UI and logo were
coherent, but the vehicle was completely absent; the process was stopped there
at about 2.06 GiB with cgroup swap disabled. The race correction did not recover
3D, and no corrected-run car-selection or gameplay capture exists yet.

The fallback path exposes a separate general ownership defect: existing
`GpuMemory::CreateObject` and `GpuMemory::Update` hash and upload directly from
guest pointers. Host/HLE writers notify before storing and do not take the
GpuMemory mutexes, so a concurrent write can still tear the CPU-to-GPU bytes for
that submission even though the next update retains dirty evidence. A postcheck
after publishing cannot undo the upload. The current bounded experiment stages
read-only vertex/index updates and initial creates up to 4 MiB into a
postvalidated immutable CPU copy. Initial create registers the range before
copying and transfers the stable dirty observation to the published object;
update keeps the previous backing when its observation races. Textures,
storage, larger buffers, and hash-fallback ranges remain on the established
non-immutable path; do not claim the broader race fixed until their
source/publish contracts are refactored and validated.

One strict no-input run exercised that full bounded VB/IB seam in a 3D
`ROLLING START` scene. It recorded 268 stable initial captures totaling about
538 MiB and 81 stable updates, with zero create fallbacks or update deferrals.
The native frame retained recognizable 3D perspective, road, building,
vegetation, and HUD, but most of the world remained black and bright colors
appeared dragged or disconnected. Peak cgroup memory was about 2.15 GiB, swap
was zero, and the process was stopped deliberately. This closes torn bounded
VB/IB source bytes as a sufficient explanation for the remaining visual defect.
The active symptom is downstream color-target corruption, not a demonstrated
vertex-position failure.

The render-target lifetime trace now covers two previously invisible seams. A
bounded `PASS_BEGIN` event observes exact versus guest/host-only attachment
identity, old and initial layouts, load operation, fast-clear state and clear
words before barriers mutate host state, including clear-only passes. A second
bounded event detects when a resolved sampled image aliases an active color
attachment before either layout transition. Both remain disabled by default
and do not alter rendering. Initial attempts did not reproduce the damaged
scene; their zero alias count is therefore inconclusive and must not be used to
change clear or feedback semantics.

Two further scheduled-input attempts confirmed that delivered taps alone do
not identify a visual checkpoint: one reached the difficulty screen and the
other returned to the main menu while all queued taps were reported delivered.
The latter bounded trace observed the full-resolution HDR target begin with a
shader-read-to-color clear followed by defined-layout `LOAD` writers and an
exact downsample chain, with no sampled-image/color-attachment alias in that
menu window. This is route and menu-pipeline evidence only; it does not close
feedback, stale-clear, or lifetime hypotheses for the damaged 3D scene. Stop
using absolute present schedules as a same-scene selector. Drive one edge only
after a stable interactive phase and confirm each screen before interpreting a
graphics trace.

The normal indexed and auto draw paths already emit a post-render-pass Vulkan
memory dependency from color attachment writes to later shader and color
attachment reads. Its destination access scope omitted the following color
attachment write, leaving the consecutive W-to-R/W contract incomplete even
though the color-output stage was ordered. Both paths now include
`VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` in the destination mask. This is a
general synchronization correction; the affected targets build and the
existing graphics integration passes, but no same-scene 3D capture exists yet.
Do not claim that it fixes the black/color-spread corruption until a strict
damaged-scene A/B demonstrates that result.

The first phase-driven strict integration after that synchronization change
delivered exactly three taps, observed `loading -> interactive`, captured
through the native agent, and shut the guest down cleanly. It reached the car
selection screen, but the vehicle remained absent. Offline scoring reported
`scene_ok=false`, `gameplay_like=false`, and entropy `2.2764`; the service
peaked at about 1.5 GiB with cgroup swap disabled. This is a reproducible
post-change visual failure and proves that the completed color write access
scope is not sufficient to restore the missing 3D. It is not the later damaged
`ROLLING START` scene, so it does not falsify a contribution to the progressive
black/color-spread symptom. The historical visual baseline is no longer present
in scratch and cannot be used for an automated comparison.

A later one-edge-at-a-time strict run did reproduce the active gameplay defect
after the synchronization correction. The native frame retained road
perspective, vehicle silhouette and coherent HUD, while most world and vehicle
color was black and small yellow, red, green and white regions were detached or
dragged across otherwise recognizable geometry. `last-error` remained null;
the bounded service peaked at about 2.16 GiB with cgroup swap at zero and was
stopped deliberately. This is accepted symptom evidence for downstream
color-target retention/composition. It is not evidence of a new exploding-
vertex or non-finite-position failure.

The lifetime trace now accepts a strict Vulkan color-format selector and a
successful-manual-capture ordinal gate. The format-only trial correctly armed
two alternating full-resolution HDR images, but repeated writes, pass begins
and samples exhausted its 64-event window within 17 presents, long before the
damaged scene. The capture gate replaces guessed present thresholds: only the
selected newer successful explicit capture opens the trace on the following
frame; automatic, failed, timed-out or superseded requests do not count. Its
build and existing graphics integration pass. The first bounded navigation
attempt preserved the closed gate through five manual captures but hit its
180-second watchdog before the selected ordinal, so exact damaged-scene HDR
lifetime evidence remains open. Do not infer clear, feedback or identity
behavior from that run.

That same-scene color boundary is now captured. A bounded score-driven selector
opened the one-shot trace only after a native frame showed the damaged 3D world:
road and vehicles remained recognizable, while black regions expanded and
yellow/white scene fragments became detached. Over the following frame the
selected 1920x1080 format-122 HDR image kept one guest address and one host
identity. All 59 observed pass begins used `LOAD` from a defined color-
attachment layout with no CMASK fast clear, no identity mismatch and no active
sample/attachment alias. The first downsample sampled that same image as
`rt-exact`. This closes stale color clear, target remap and direct feedback as
causes for that exact frame; it also places the corruption before the
downsample rather than in the final compositor.

Every recorded HDR writer in the damaged frame had depth test and depth write
enabled. The visible frame lost most opaque surfaces over only a few presents
while bright lights survived, so retained or incorrectly published depth is the
next falsifiable producer. It is not yet confirmed: color and depth lifetime
selectors are intentionally mutually exclusive, and the subsequent depth-only
run did not re-enter the demo before its watchdog. The next run must visually
select the same damaged frame, arm only the 1920x1080 D32S8 depth selector, and
classify its first `DEPTH_USE`, clear source and identity before changing any
load operation or reverse-Z rule.

The earlier HTILE publication experiment was withdrawn after runtime and
independent review. Publishing an exact storage view in a separate operation
before depth materialization left a TOCTOU gap and ignored pending or ambiguous
owners; it also did not improve the frame. Storage destruction no longer
creates a metadata-clear event merely because the mapped bytes resemble a
clear. Depth-image creation also no longer fabricates an HTILE clear; Vulkan
first use remains governed by the existing `UNDEFINED` load-op resolver. Keep
the provenance diagnostics, but do not restore address-only teardown marks or
the separate pre-materialization writeback call. The remaining pending-event
map in `DepthMeta.cpp` is still keyed only by address and is not bounded; a
future semantic change must add size plus allocation/depth generation and a
fail-closed bound before relying on it across retirement or address reuse.

`ThreadFlag` bit `0x1` (mode `0x21`, 40 ms waits, no observed Set in earlier
captures) remains a **later** suspected synchronization symptom: do not fake
the bit from `WaitEventFlag`, timers, or the render loop. Identify the producer
only after the GPU/shader chain no longer aborts earlier. EventFlag **handle
registry** (reject unregistered/garbage pointers with `ESRCH`) is host safety,
not a substitute for Set.

Linux host path builds with `_build_linux` / Ninja Release; macOS continues to
use `_build_macos`. Prefer default `CMAKE_BUILD_TYPE=Release` on single-config
generators. Default `scripts/run_guest.lua` uses `PrintfDirection = 'Silent'`
for usable FPS; Console logging is evidence-only and destroys frame-time
comparability. Session evidence may live under a local untracked directory
(e.g. Documents `Kyty-implementer/` copy of implementer scratch); never commit
guest paths, title IDs, raw multi-megabyte logs, or `_Shaders/` dumps.

**Always re-capture the first strict fail on the current HEAD.** This is not
gameplay acceptance. Diagnostic input, stubs, permissive GPU skips, and
console logging are not supported runtime modes.

## Architecture map

- `source/emulator/src/Loader/`: guest image loading, relocation, NID resolution,
  native-call trampolines, and exception integration.
- `source/emulator/src/Kernel/`: guest memory, direct/flexible allocation,
  pthreads, synchronization, files, and time.
- `source/emulator/src/Libs/`: HLE export registration and guest API contracts.
- `source/emulator/src/Graphics/Graphics.cpp`: PS4/PS5 command-buffer builders
  and AGC-facing exports.
- `source/emulator/src/Graphics/GraphicsRun.cpp`: PM4 parsing and normalized
  graphics-state updates.
- `source/emulator/include/Emulator/Graphics/HardwareContext.h`: normalized
  guest GPU state.
- `source/emulator/src/Graphics/GraphicsRender.cpp`: Vulkan resource binding,
  pipelines, draw/dispatch recording, and synchronization.
- `source/emulator/src/Graphics/ShaderParse.cpp` and `ShaderSpirv.cpp`: guest
  shader decoding and SPIR-V generation.
- `source/emulator/src/Graphics/Tile.cpp`: guest surface layout and addressing.
- `source/emulator/src/Graphics/Objects/`: Vulkan-backed resources and guest
  memory tracking.
- `source/emulator/src/Graphics/VideoOut.cpp` and `Window.cpp`: display buffers,
  Vulkan device/swapchain setup, and presentation.
- `source/lib/`: reusable host runtime, platform, threading, memory, filesystem,
  math, and script infrastructure.
- `source/unit_test/`: GoogleTest registration and deterministic fixtures.

Keep guest API decoding, guest GPU semantics, normalized state, Vulkan objects,
and host platform adapters conceptually separate even where legacy files still
contain more than one responsibility. Improve the seam being touched; do not
perform unrelated mass refactors.

## Auxiliary-agent handoff prompt

The following prompt is the canonical brief for an auxiliary agent. Give the
agent a private guest root through `KYTY_GUEST_ROOT`; never paste that path,
title identifiers, binaries, keys, save data, shaders, textures, screenshots,
or logs into tracked files or commit messages.

```text
You are a senior emulator/runtime engineer working inside the Kyty repository.
Your mission is to advance the strict PS5 runtime from the current controllable
gameplay frontier to stable, validated playability, then freeze that frontier
and only afterward perform carefully bounded modularization. Correctness,
evidence, portability, and preservation of working behavior outrank speed or
line-count reduction.

CURRENT FRONTIER

- Build works on Linux (`_build_linux`) and macOS (`_build_macos`); use the host
  you are on. Prefer Release + `PrintfDirection=Silent` for wall-clock.
- Vulkan device/swapchain, Gen5 shaders, indexed draws, VideoOut flips, logos,
  recognizable menu, Play/mode transitions, loading-card pixels, and
  controllable gameplay are exercised under strict flags. A reset 601-frame
  Release+Silent gameplay window reported 41.688 FPS with p50/p95/p99 frame
  times of 27/35/40 ms. Diagnostic controller input reached and moved through
  that scene; this proves the frontier, not formal acceptance or stable 60 FPS.
- In tree (do not regress): GpuMemory multi-parent (VB reclaim + surface link;
  Texture mixed parents; IndexBuffer-in-Texture link; WriteBack parent
  classify); GPU-owned RT layout preserve on Update; tile-27 size+4bpp detile;
  Gen5 EUD type-5; formats 14/29/56/71; multi-RT CB_SHADER_MASK; EXP Param5/6 +
  multi-MRT; structured SPIR-V loops; `v_cvt_i32_f32`; SDWA; SMEM dual-offset +
  variable SBuffer; image_sample dmasks 0x2/0x4/0xb; `ds_read2_b32`; null MRT
  discard tails; kill-enabled `EarlyZThenLateZ` late depth commit; NGS2
  extended max_voices.
- **First strict fail (re-capture on HEAD):** none observed through more than
  24,000 presents in the latest Linux Release+Silent strict run. The next
  structured EXIT or host fault is the process unit of work.
- **Visual frontier:** horizontal stripes and opaque sprite/prop rectangles are
  absent in a gameplay-era native capture. A bounded diagnostic route consumed
  sustained directional input for 180 presentations. Formal acceptance still
  requires a repeatable non-diagnostic controller route, one action beyond
  movement, stable presentation, and validation-clean output.
- **Later symptom only:** `ThreadFlag` bit `0x1` (mode `0x21`, 40 ms) with no
  observed Set. Never fabricate the signal. EventFlag live-handle registry
  (garbage → ESRCH) is not Set. Trace the producer after earlier GPU/shader
  aborts are gone.

IMMEDIATE OBJECTIVE AND SUCCESS CONDITION

Advance the strict post-Play path to **stable, correctly rendered 60 FPS
gameplay** without diagnostics or fabricated success. Formal acceptance must
use a real controller route, exercise movement plus another action, and remain
validation-clean. Process survival, diagnostic input, and HUD-only correctness
are not playability.

If dual-strict shows a process EXIT, that is first priority (GpuMemory, shader,
format, HLE). If the process survives but the world is wrong, treat that as the
rendering frontier: identify the first bad producer at the bound sample,
writer MRT, or consumer/composite boundary with capture evidence — do not
paper over it with permissive flags.

`ThreadFlag` remains deferred while earlier GPU/render issues dominate:

- one event named `ThreadFlag` is created with initial bits `0x0` in older
  captures;
- loading/wait mode `0x21` for bit `0x1` with 40 ms timeout was observed;
- do not Set the bit from Wait, timers, or the render loop.

PRIMARY ORDER OF WORK (DO NOT REORDER)

1. Reproduce the strict frontier with the current checkout and private fixture
   (`$KYTY_GUEST_ROOT` only; never name the title in commits).
2. Fix the first strict failure (re-capture on HEAD) with a documented
   hypothesis and a focused deterministic test or sanitized fixture.
3. Re-run strict execution and advance one failure at a time until the title
   reaches the first controllable gameplay scene under the playability table.
4. Prove real keyboard/controller press+release, movement both ways, one
   action, stable flips, correct geometry/colors, no device-loss, validation
   clean where available. Record Silent FPS + resolution + shader cache.
5. Freeze this working frontier with regression/characterization tests and a
   short evidence report (untracked scratch; no private paths in Git).
6. **Only after steps 1–5 pass and gameplay is reproducible from a baseline
   commit**, modularize oversized files one seam at a time. Every extraction
   must be behavior-neutral and must preserve the frozen gameplay evidence.
   Do not start modularization while a post-Play strict blocker is open.

PHASE GATES AND REQUIRED DELIVERABLES

Phase 0 — Baseline and reproducibility:

- Record HEAD, branch, `git status --short`, build result, focused test result,
  host GPU/capability summary, logging mode, resolution, and shader-cache state.
- Confirm no permissive/stub/trap-skip environment variable is active.
- Reproduce the loading frontier twice so a one-off race is not mistaken for a
  stable contract.
- Save all raw output beneath ignored scratch. The tracked report contains only
  sanitized facts, durations, counts, and source locations.

Phase 1 — Resolve the synchronization frontier:

- Map `ThreadFlag` creation to the guest call site and owning subsystem.
- Map every possible producer path to its HLE export, worker entry point,
  queue/command input, and expected `SetEventFlag` or equivalent completion.
- Capture thread start/exit and the last successful contract on the producer
  thread. The first earlier failure on that thread supersedes the wait timeout.
- Add a deterministic test for the evidenced missing contract before changing
  implementation. A generic EventFlag test alone is insufficient if EventFlag
  itself is behaving correctly and the producer never runs.
- Implement one semantic change and prove the signal now originates from the
  real producer. Record the next strict frontier.

Phase 2 — Reach and prove gameplay:

- Advance one strict blocker at a time through loading and scene creation.
- Use real keyboard/controller press and release edges for acceptance.
- Demonstrate a controllable character, movement in both directions, and at
  least one jump/attack/interact action while frames continue presenting.
- Inspect the scene for correct geometry, colors, texture interpretation,
  viewport/scissor behavior, and stable frame progression.
- Run with Vulkan validation where supported and record zero relevant errors,
  no device loss, no render-thread timeout, and no stuck GPU label.
- Measure performance only with silent function logging, fixed resolution, and
  recorded cache state. Do not make a target FPS claim from console logging.

Phase 3 — Freeze the working frontier:

- Add characterization tests for every compatibility seam required to reach
  gameplay, using sanitized packets/descriptors/ABI arguments only.
- Create a sanitized frontier report containing commit, commands, test counts,
  input sequence, frame/flip evidence, validation result, and performance
  conditions. Do not include the private fixture identity.
- Establish a baseline commit before any architectural extraction. If the
  strict scenario cannot be reproduced from that commit, the freeze is invalid.

Phase 4 — Architecture inventory:

- Measure files and functions, but classify them by responsibility, mutable
  state, ownership, threading, callers, dependencies, and existing tests.
- Current size signals include `ShaderSpirv.cpp` (~8,290 lines),
  `GraphicsRender.cpp` (~5,725), `GraphicsRun.cpp` (~4,521),
  `ShaderParse.cpp` (~3,473), `Shader.cpp` (~3,186), `Pthread.cpp` (~2,807),
  `Graphics.cpp` (~2,740), `Audio.cpp` (~2,716), `Window.cpp` (~2,539), and
  `GpuMemory.cpp` (~2,515). Recount before planning; these numbers are a
  snapshot, not acceptance thresholds.
- Produce an extraction table for each candidate: responsibility to move,
  proposed typed interface, inputs/outputs, owner, thread contract, error
  contract, mutable globals removed or retained, dependency direction,
  characterization tests, and strict-runtime verification command.
- Reject any boundary that cannot be described without generic `Utils`,
  `Common`, `Manager`, forwarding aliases, or bidirectional dependencies.

Phase 5 — Incremental modularization:

- Extract one cohesive responsibility per commit. Do not combine behavior
  changes with file movement or rename campaigns.
- Add characterization coverage first, move the implementation second, delete
  the old implementation in the same change, then rebuild and re-run gameplay.
- Preserve public behavior and one source of truth. Direct and indirect PM4
  paths must still share decoders; all surface consumers must still share one
  layout model; renderer policy must still depend on explicit capabilities.
- Revert an extraction if build, focused tests, menu, loading time, gameplay,
  input, frame output, validation state, or performance materially regresses.
- Update module documentation after each accepted extraction: purpose, public
  interface, invariants, ownership, thread safety, dependency direction, error
  behavior, and tests. Comments must explain contracts, not restate code.

NON-NEGOTIABLE RULES

- Read this entire AGENTS.md before editing. Do not weaken its invariants.
- Reproduce before editing. Capture the first strict error, packet/register
  values, submit ID, command offset, guest call path, and relevant state.
- Never invent a NID, ABI, structure layout, register meaning, tile mode,
  pitch, alignment, return code, or synchronization result. Triangulate from
  guest evidence, local call sites, upstream references, and a test.
- Never use `KYTY_BRINGUP_MODE=unsafe`, trap skipping, default success, assumed
  RGBA8/linear layout, fabricated resources, or placeholder shaders in
  acceptance runs. Unsafe bring-up is discovery-only diagnostics.
- One behavior has one implementation. Direct and indirect PM4 paths share a
  decoder; all resource consumers share one descriptor-to-layout calculation.
- An unsupported behavior must fail structurally and informatively. Do not
  hide it behind a generic fallback, compatibility alias, vendor check, or
  duplicated legacy path.
- Guest semantics remain platform-neutral. macOS, Linux, Windows, Vulkan
  extension, and GPU-vendor details belong at explicit host seams.
- AMD, Intel, NVIDIA, and Apple are capability inputs, never correctness
  policy switches. Select strategies from features, limits, formats, queues,
  and tested semantic alternatives.
- Do not add private fixture paths, title IDs, keys, binaries, saves, assets,
  screenshots, crash dumps, or raw logs to Git. Keep them under an ignored
  local directory and refer to them only as `$KYTY_GUEST_ROOT`.
- Do not use a commit message that identifies the private workload. Describe
  emulator behavior, for example `fix(graphics): validate Gen5 barrier range`.
- Preserve existing working behavior. If an experiment regresses menu reach,
  pixels, input, flips, or build, remove/revert the experiment before trying
  the next hypothesis.

REPRODUCTION AND VERIFICATION COMMANDS

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux
```

Configure and build on macOS:

Line count is a signal, not the goal. Do not split an atomic eight-line
function. Do split a long function that mixes parsing, state mutation,
allocation, Vulkan calls, and logging. Each extracted function/module must have
one purpose, explicit inputs/outputs, ownership, thread contract, error
contract, and a focused test. Delete the superseded implementation in the same
change; do not leave permanent forwarding aliases or duplicate semantics.

REFERENCE MATERIAL

Use reference material only for behavioral facts, architecture patterns, and
test ideas. Every imported claim must be verified with a local capture or a
focused test before it affects Kyty behavior. Do not copy incompatible code,
private assets, proprietary SDK material, firmware, keys, decrypted content, or
implementation details that cannot be relicensed into Kyty.

For each confirmed fact, record the provenance, license, behavior learned, and
the local evidence that proves it. Reimplement confirmed behavior using Kyty's
own types, boundaries, and diagnostics.

HANDOFF REPORT TEMPLATE

End every auxiliary-agent session with:

- Commit/base revision (no private fixture names).
- Build and focused-test commands plus pass/fail output.
- Strict command and first failure or verified gameplay checkpoint.
- Evidence table: verified fact, source, local reproduction, confidence.
- Files changed and why each belongs to the seam.
- Regression checks and performance measurement conditions.
- Exact next blocker and one proposed falsifiable hypothesis.
- Explicit statement that no diagnostic flag was required for acceptance.
- Phase and gate status: baseline, synchronization, gameplay, freeze,
  inventory, or extraction.
- If gameplay is not reached, the exact producer-side first failure and one
  falsifiable next hypothesis; never report the loading screen as success.
- If modularization began, the frozen gameplay commit and before/after evidence
  proving the extraction was behavior-neutral.

If a required fact cannot be evidenced, stop at a structured unsupported error,
report the blocker, and do not paper over it with a fallback or broad refactor.
```

Windows supports the generators and toolchains defined by the CMake files and CI.
Do not invent a Windows command from a Linux or macOS layout; inspect the active
workflow and generator first.

Build only the main script runtime when a full build is unnecessary:

```bash
ninja -C <build-dir> fc_script
```

Run focused tests through `fc_script`:

```bash
<build-dir>/fc_script '{kyty_run_tests()}' \
  --gtest_filter='SuiteName.TestName'
```

Confirm that a new or renamed filter actually selects the intended tests:

```bash
<build-dir>/fc_script --gtest_list_tests '{kyty_run_tests()}'
```

Run an authorized private fixture only when the task requires runtime validation
and `KYTY_GUEST_ROOT` is already available:

```bash
<build-dir>/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

The strict run sets no `KYTY_BRINGUP_*` variables (and no removed legacy flags).
Capture the first error completely, including packet/register values and the
guest/host call path when available.

### 3. Form one hypothesis

State the suspected root cause and the evidence supporting it. Change one
variable at a time. If a hypothesis fails, remove the experiment before testing
the next one.

### 4. Work test-first

For every behavior change:

1. Add the smallest deterministic failing test.
2. Run it and confirm the expected failure.
3. Implement only the behavior required by the test.
4. Run the focused test until it passes.
5. Build and re-run the strict integration scenario.

Sanitized PM4 packets and surface descriptors are acceptable fixtures. Guest
code and assets are not.

### 5. Verify the real outcome

For graphics changes, a successful build and non-black pixels are insufficient.
Verify geometry, colors, resource interpretation, completed flips, absence of
Vulkan errors, and a recognizable correctly proportioned frame. Preserve local
visual evidence outside Git.

### 6. Refactor only behind a frozen frontier

After strict menu and gameplay acceptance exists, inventory oversized modules
with line counts, responsibilities, dependency direction, mutable globals, and
test coverage. Select one cohesive extraction at a time. For each extraction:

1. Record the pre-refactor strict frontier and focused test results.
2. Add missing characterization tests without changing behavior.
3. Move one responsibility behind a narrow interface.
4. Remove the original implementation rather than leaving an alias.
5. Build, run focused tests, and reproduce the strict frontier.
6. Revert the extraction if the frontier, frame, input, or validation state
   regresses.

## HLE and ABI rules

- Every export needs an evidenced name, NID, signature, calling convention,
  argument validation, return code, and side effect.
- Prefer guest error returns for expected invalid input. Assertions and process
  exits are for violated emulator invariants, not ordinary guest errors.
- Do not map a new NID to a convenient existing function until their contracts
  have been compared, including failure behavior.
- Keep registration centralized in the owning `Lib*.cpp` module.
- A generic missing-symbol stub may be used to discover which import is called;
  it must never be required by acceptance runs or releases.

## Graphics rules

### PM4 and normalized state

- Packet envelope validation belongs to packet parsing.
- Register bit decoding belongs to one state-decoder function.
- Direct and indirect packet handlers call the same decoder.
- Unknown registers report packet type, register, value, submit ID, and command
  offset, then stop in strict mode.
- Never label an unknown register harmless without proving its semantics and
  showing that the workload does not depend on it.

### Surface layout

- Format, block geometry, pitch, mip levels, depth/array layers, sample count,
  tile mode, metadata, size, and alignment form one descriptor-to-layout
  calculation.
- Compressed formats use block dimensions; bytes-per-pixel arithmetic is not a
  substitute.
- CPU upload/detiling, overlap tracking, Vulkan allocation, and writeback consume
  the same layout.
- An unknown descriptor returns a structured unsupported error. It does not
  assume four-byte texels or linear memory.

### Vulkan and GPU portability

- Collect device capabilities once and pass them explicitly to consumers.
- Classify each capability as required, optional with a semantically equivalent
  tested strategy, or diagnostic-only.
- A correct alternative for an absent extension is not a behavioral fallback:
  it must preserve guest-visible semantics and have tests for both strategies.
- Do not add AMD-, Intel-, NVIDIA-, Apple-, MoltenVK-, or driver-specific paths
  to guest state decoding or surface layout.
- Keep Vulkan validation clean when the platform supports the required layers.

## Platform portability rules

- macOS is a distinct supported host, not a Linux build label.
- Use portable C++ and existing Core/Sys abstractions in shared code.
- Confine `__APPLE__`, `_WIN32`, and Linux-specific branches to platform-facing
  implementation files.
- Do not use Apple frameworks, Win32 APIs, or Linux syscalls in HLE, PM4,
  shaders, surface layout, or renderer policy.
- Treat host CPU architecture separately from host OS. Preserve the current
  native x86-64 path while keeping future execution backends possible.

## Diagnostic flags

Default runtime mode is **strict**: `EXIT_NOT_IMPLEMENTED` aborts with stack and
subsystem shutdown; missing imports do not receive stubs; unknown indirect GPU
registers are fatal. Strict acceptance runs must not set any `KYTY_BRINGUP_*`
variable (`scripts/run_guest.lua` rejects them unless
`KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1` is set for an authorized smoke only).

### Centralized unsafe bring-up (`Kyty::Core::BringUp`)

Diagnostic continuation is centralized. Do **not** invent per-game exceptions
or cite unsafe survival as compatibility. Neighbor PRX soft-preload is
**unsafe-only** (`prx_preload` feature); strict acceptance never auto-preloads.

| Variable | Meaning |
| --- | --- |
| `KYTY_BRINGUP_MODE=unsafe` | Enable diagnostic policy (absent ⇒ strict). |
| `KYTY_BRINGUP_FEATURES` | CSV: `not_implemented`, `missing_function_import`, `gfx_permissive`, `prx_preload`. Absent under unsafe enables the first three only; **`prx_preload` is always explicit**. |
| `KYTY_BRINGUP_SUBSYSTEMS` | CSV scopes: `core,loader,kernel,graphics,audio,network,hle,other`. Absent ⇒ all. |
| `KYTY_BRINGUP_BURST_LIMIT` | Max hits per site inside the window (default 10000). |
| `KYTY_BRINGUP_BURST_WINDOW_MS` | Window length in ms (default 1000). |

Unknown, empty, zero, or contradictory values abort at process start
(`BringUp::InitFromEnvironment` from Core subsystem init; no silent strict
fallback after a parse error). Circuit-break on a site re-enters the normal
strict abort after printing a summary. Repeated `EXIT_NOT_IMPLEMENTED` continues
log once per site (no full stack spam on every hit). The policy never
fabricates EventFlags, fences, memory, or sync results. Only **Func** imports
may receive missing stubs (with a minimal return-class taxonomy); Object / TLS /
NoType stay strict `EXIT`. Neighbor PRX scan is **not** part of default unsafe
features — set `prx_preload` explicitly.

**Removed (intentional break):** `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE`.
Using them is a configuration error.

Other diagnostics (unchanged, still not acceptance modes):

- `KYTY_FAULT_LOG=1`: signal-safe fault diagnostics.
- `KYTY_CRASH_REPORT=/absolute/path.json`: writes the bounded fatal-fault JSON
  report. `KYTY_CAPTURE_DIR` supplies `crash-context.json` by default when an
  explicit report path is absent.
- `KYTY_CRASH_MEMORY=1`: on supported POSIX hosts, adds at most 24 fault-safe
  64-byte windows around plausible guest-data pointers found on the captured
  stack. It is disabled by default and its output may contain guest data; keep
  the report in untracked scratch.
- `KYTY_TRACE_LIBC=1`: targeted single-step tracing.
- `KYTY_SKIP_UD2=1`: skips a guest trap for diagnostics; invalidates normal
  execution. **Not** part of the bring-up policy.
- `KYTY_PIPELINE_FAIL_DUMP=<directory>` and `KYTY_TRANSPORT_DUMP=<directory>`:
  opt-in local evidence dumps (SPIR-V of a failed pipeline; the guest program
  that needs fragment wave transport). The directory must already exist and is
  never created. File names are fixed components, created exclusively (an
  existing file, symlink or FIFO is not overwritten), each file is capped at
  16 MiB and the process at 64 MiB, and a cut dump is reported as truncated.
  Each file's status is reported in the fatal transport message or the debug
  log; a rejected dump never changes the outcome of the run. Keep the directory
  in untracked scratch.

Agent diagnostics JSON protocol version is **5** and includes `bringup.mode`,
features, subsystems, limits, unique sites, continuations, missing-import
metrics, and last circuit-break (`BringUp::WriteDiagnosticsJson`).

No diagnostic flag is enabled by default or cited as proof of compatibility.

Integration matrix (process-isolated):

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script kyty_bringup_integration
ctest --test-dir _build_linux --output-on-failure -R KytyBringUpIntegration
```

## External references and licensing

Reference implementations may establish names, concepts, architecture patterns,
and test ideas. Kyty is MIT-licensed; do not copy GPL implementation code into
this repository. Record the behavioral fact and provenance, then implement it
against Kyty's own types after verifying it locally.

Use PS5-focused references for guest ABI and AGC evidence, mature emulators for
renderer/capability architecture, and official Vulkan documentation for host API
semantics. Do not assume another console's GPU behavior applies to PS5.

## Code quality

- Follow `source/.clang-format` and the existing C++17 style.
- Prefer focused functions and explicit types over duplicated bit manipulation.
- Keep headers minimal and ownership clear.
- Avoid broad renames, compatibility aliases, dead code, commented-out paths,
  magic constants without provenance, and unrelated cleanup.
- Comments explain evidence, invariants, and non-obvious hardware semantics; they
  do not narrate obvious code or advertise another project.
- Treat warnings, `git diff --check`, and new validation messages as failures to
  investigate.

## Completion checklist

Before committing a behavior change:

1. The focused test failed before implementation and passes afterward.
2. `ninja -C _build_macos` succeeds.
3. `git diff --check` succeeds.
4. The strict local scenario advances or renders more correctly.
5. No missing-symbol stub or permissive register skip is needed for the claimed
   behavior.
6. No tracked file contains fixture information or generated evidence.
7. No OS or GPU vendor was made a hidden correctness requirement.
8. Existing working behavior was rechecked.
9. The commit message describes emulator behavior without identifying a private
   compatibility fixture.
10. Any refactor preserves the frozen strict frontier and leaves one active
    implementation of each behavior.
11. New or extracted modules have a documented responsibility, ownership model,
    dependency direction, and focused tests.
