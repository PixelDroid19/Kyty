# Insect platformer pair

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Platformer pair: intro cinematics were black (2026-10-17)

Both Unity platformers showed black for their intro cinematics. The insect
platformer's intro ends in a second video, and after the first one the screen
stayed black.
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
  - 2026-10-17, Intel, walking right with a capture every 3 s: the player
    passes the cage and reaches the start room's right edge. There, a
    soft-edged black corner covers the top right of the lit forest, and the
    player walks into it and leaves the view. This edge frame matches the
    maintainer's description (black mass over the lit forest, player at the
    right edge). About 3 s later a small cave pocket is drawn centred on
    screen and framed in black, with fireflies over the black.
    - No scene load separates the two: the 3 s perf windows around them have
      no SPIR-V compile, no pipeline miss, allocations under 35 MB and no
      frame-rate dip. The camera moved within one scene, so the black around
      the pocket is not shown to be a room border. A cover mask that should
      fade when the player enters (black sprites with alpha 199-251, above)
      remains the live lead.
    - A published walkthrough places a Shell Shards cave to the right of the
      start. Its screenshot of the start room shows dark soft-edged corners
      too, but no hardware image of the cave was found.
    - Excluded on Intel for this sequence, a cover mask stuck partway:
      draws dumped one frame in 100 from the edge through the pocket (a
      bounded diagnostic, not committed). Every sprite drawn with black
      vertex colour carries alpha 255 in the guest's vertex data in every
      sampled frame, and none has a partial alpha. The set of black sprites
      changes in the frame where the pocket appears: other atlas sprites
      enter and leave the view, as when the camera reaches a new area. The
      guest asks for these opaque black silhouettes and the GPU draws them.
    - Still open: whether the silhouettes' shapes are right. Their soft
      edges come from the alpha of BC7 atlas textures. A wrong alpha decode
      would widen them, but a decode fault would also change the shapes of
      the lit sprites drawn from the same atlases, which look correct.
  - The maintainer's screenshot was taken on the AMD iGPU and was not kept
    by the session; the AMD-only reading rested on it. Its description
    (soft-edged black mass over a lit forest, player at the right edge)
    matches the Intel edge frame above. It was taken before the AMD ring
    timeouts recorded above. After an AMD reboot, compare the black-with-alpha
    sprites (vertex colour (0,0,0,199..251), BC7 atlas) on both hosts, matched
    by index count and texture.
- **Overlapping text (both platformers).** The maintainer reports that some
  texts sometimes draw over each other. The sequel's opening credits fade
  names in and out over gameplay, so first check whether two fading strings
  overlapping is the title's own transition. No capture isolates it yet;
  capture the moment before attributing it.
- **Slow loading screens and first boot (sequel).** First start and some
  loading screens take much longer than later ones.
  - Two causes found and fixed on 2026-10-17: each intro cinematic waited
    out a 10 s guest timeout (see "Platformer pair: intro cinematics were
    black"), and the host-write lease of every AMPR read scanned the whole
    dirty-page table twice (~1.5 s per boot).
  - Measured, Intel, cold caches (every harness run starts with empty SPIR-V,
    Mesa and pipeline caches), agent perf snapshot every 5 s:
    - Pipeline misses cost about 1.9 s across the whole boot, in bursts of
      0.1-0.5 s when a new scene starts. Shader translation is not the long
      wait.
    - From the new-game prompt: the prologue poem shows "Loading..." until
      the load ends, then "Press any button to skip". A cave scene with
      "Loading..." follows, then ~16 s of black, then the first area fades in
      over ~10 s.
    - During those ~16 s the guest presents ~360 fps at ~10 draws per frame
      and the command processor is busy 1.7 s of every 5 s. Per-thread CPU
      sampled inside the black (104-114 s): the busiest thread is the command
      processor at 36% of a core and no other thread passes 10%. The guest
      threads sleep on futexes (no
      blocked guest condition wait with `KYTY_SLOT_TRACE`, no agent event).
      AMPR and kernel AIO complete synchronously in the submitting thread, so
      I/O completion latency is excluded.
    - The same ~15 s of black follows the unskipped intro video. The
      published description of the opening has the cage fall into a pit
      before the player wakes at the bottom, which fits a scripted black
      stretch. Not verified against hardware timing.
  - Next, if the black still feels long: time the same transition on
    hardware or a reference recording, and check whether the guest waits on
    an audio cue (an audio clock that runs slow would stretch it).
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

## Insect sequel: intermittent depth/index overlap (2026-10-16, open)

- A 150 s Intel run reached gameplay (start of Act 1) without the earlier
  `unknown relation: DepthStencilBuffer - Crosses - IndexBuffer` stop (a
  12-byte index read inside a live 1920x1080 depth buffer). The stop is
  intermittent; overlap diagnostics now print every parent range to show
  which plane it crosses when it recurs.
