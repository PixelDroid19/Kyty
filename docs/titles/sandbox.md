# Sandbox title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

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
