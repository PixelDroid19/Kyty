# Preschool Unity title

Title: PPSA03706 - My Friend Peppa Pig.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Preschool Unity title: unreadable EUD snapshot (2026-10-21, fixed 2026-10-25)

- **First occurrence.** Intel Arc A770, Mesa ANV 26.2.5, strict, HEAD
  89e3b892, generic pad route: after about frame 30,400 (91 s) the run
  stopped on `unreadable Gen5 EUD snapshot: dwords=16` with the table
  pointer in user register 28 (`Shader.cpp:3366` on main). The guest
  words at the table address could not be read when the shader was bound.
- **Not reproduced.** A 150 s repeat of the same binary and route ran
  without a stop at 290-367 FPS.
- **Second occurrence (2026-10-22).** HEAD 36161708, same route: the run
  stopped at about 26 s (frame 7,500) on the same message, again with 16
  dwords and user register 28, at an ordinary 48-bit address. The
  role-playing title's high-bit pointer (`0x0010...`) was a different
  defect, fixed by the 48-bit pointer read (see that title's record).
- **Excluded (2026-10-22).**
  - Completion ordering. The snapshot is taken while the command processor
    prepares the draw or dispatch, in packet order. No later label,
    `WRITE_DATA`, flip or end-of-pipe event becomes visible to the guest
    before it. No other thread reads live tables: the only scoped reader is
    the fold-capture replay.
  - GPU-only memory. Every guest protection value that carries GPU access
    bits also grants CPU read. Bare GPU-only values are refused, so no table
    can sit in CPU-unreadable GPU memory.
  - Fixed-map replacement window. A fixed direct map over a live view cuts,
    unmaps and re-reserves the range before mapping it again. A 150 s run
    widened that window by 2 ms per replacement and logged every guest
    unmap, decommit and protection change. It saw no fixed map over a live
    view, no decommit and no stop. It did see 25 guest unmaps of
    0x4000-0x32C000 bytes during the intro. It also logged about 572,000
    eight-byte protection changes during boot, all from relocation (see
    "Relocation protection round trips at module load" in the
    cross-title records).
- **Changes.**
  - The snapshot also reads host allocations the HLE libc lent the guest
    before its heap existed, as the audio paths already do. A table
    there was refused before. Whether this title places tables there is
    not proven.
  - The stop message now adds `guest_owned`: 0 means the table's range
    was unmapped, 1 means it was protected or decommitted.
- **Direction.** On the next occurrence, `guest_owned` picks the class.
  Then log the guest unmap or protection call that covers the table
  address, to tell a guest that frees the table early from a host-side
  read race.
- **Root cause (2026-10-25).** On the Intel route the stop repeated at
  about frame 3,440 on every run, with `guest_owned=0` and a pointer whose
  high dword was 1. The 1 was not a guest store: it was a pending
  end-of-pipe label store, landing in a command buffer the guest had
  already rebuilt at the same address.
  - The guest's command buffer clears a label, releases it at end of pipe,
    waits on it with `WAIT_REG_MEM`, then writes a "consumed" counter with
    `WRITE_DATA`.
  - The wait's producer was a plain label store of the same queue, so the
    command processor consolidated it (queue order already covers it) and
    went on. The following `WRITE_DATA` was then stored immediately, at
    parse time, about 5 ms before the label landed.
  - The guest saw the counter, reused the command buffer memory and wrote
    new packets there. The late label store overwrote one of their
    dwords, the EUD pointer's high half.
  - The "completion ordering" exclusion above was therefore wrong: a write
    behind a consolidated wait did become visible before an earlier label.
- **Fix.** A one- or two-dword `WRITE_DATA` that follows a consolidated
  wait in the same batch is deferred to end of pipe, like a write behind
  an earlier deferred write to its address. Longer writes still store
  immediately.
- **Verification.** Suite 113/113. Intel, 100 s: frame 5,609 at 60 FPS,
  no stop (every run before stopped at about 3,440). AMD, 100 s:
  gameplay at 17.7 FPS, as before, no stop. Intel regression runs of the
  racing, insect platformer, isometric action and first-person puzzle titles
  showed no new errors and the same frame rates as the baseline at the
  same duration.

## Preschool Unity title: sky-blue speckles on every outline (2026-10-19)

- **Symptom.** In gameplay, outlines, grass blades and text strokes showed
  sky-blue blocks. The defect is as old as the title's first gameplay runs.
- **Localization (Intel Arc A770, Mesa ANV 26.2.5, strict 90 s run).** Render
  targets were dumped at present 25000 (`KYTY_DUMP_VIDEOOUT`, `KYTY_DUMP_RT`).
  - The scene targets before the post-process are clean.
  - The post-process is morphological anti-aliasing: edge detection, then
    blend weights, then neighbourhood blending.
  - The edge target is correct: one-pixel lines with values 0 or 1.
  - The blend-weight target was wrong. 29 413 of its 41 167 non-zero pixels
    have no edge. Every non-zero pixel lies in a 4x4-aligned block that holds
    an edge pixel, with no exception, and 2236 of 2727 such blocks are full.
    A 4x4 block is one host fragment subgroup on this GPU.
  - Blending with those weights fetches distant texels, which clamp to the
    sky at the screen border.
- **Cause.** The weight shader keeps the edge's red channel in a VGPR across
  the horizontal search. Inside that EXEC-narrowed block it also runs
  `V_INTERP_P1/P2_F32` into the same VGPR. The translator stored
  interpolation results for every lane and ignored EXEC. A lane without an
  edge that shared a subgroup with an edge lane therefore got an
  interpolated coordinate (> 0) in place of its zero red channel. It then
  took the vertical-edge branch and wrote weights. EXEC masks every
  vector-ALU encoding, VINTRP included (ISA 3.3, 6.1). The fragment-compute
  path already predicated these writes.
- **Fix.** `RecompilePixelInterpolatorLoad` (both its parameter and its
  default-value loads) and the per-vertex `V_INTERP_MOV_F32` load now store
  through an EXEC select, like the other VALU results. Shader translator
  version 138.
- **Excluded on the way, with evidence.**
  - BC3 texture data, detiling and mip chains: the decoded levels match.
  - The 4 KiB standard swizzle of the sampled textures.
  - Alpha-to-mask: `DB_ALPHA_TO_MASK` was 0x0000aa00, which is disabled.
  - The stencil state of the post-process draws: stencil test is off.
  - `V_CMPX` writing VCC: the translator writes EXEC only, as the ISA's
    opcode table states. The shader relies on this (VCC holds a saved mask
    across a `V_CMPX`).
  - Image offsets of `IMAGE_SAMPLE_LZ_O`, including NSA addresses: decoded
    as signed six-bit x and y fields, and correct.
  - The OMOD halving of the search accumulator: emitted inside the EXEC
    select.
- **Verification.**
  - The new weight target has non-zero pixels only on edge pixels (6535,
    none off an edge). The present shows clean outlines.
  - 2481 unit tests pass. `check_emulator_boundaries.py --strict` and
    `check_graphics_tables.py` pass.
  - Strict Intel sweep, one title at a time, scripted routes:
    - The card title, the racing title, the roguelike, the watercolour
      platformer and the pixel exploration title ended without a stop. Their
      segment fps match earlier sweeps.
    - The traveller role-playing title stopped at frame 1194 on the open
      one-layer view (`StorageTexture - Contains - StorageTexture`, see
      "Open: a one-layer view inside the grown array"). The sweep before
      this change stopped there too, at frame 550.
    - The void platformer stopped once at `GpuMemoryCreate.cpp:1545` on a
      relation not recorded before:
      `DepthStencilBuffer - Crosses - DepthStencilBuffer`. A new 960x540
      depth target at 0x6ab80000 (0x140000 bytes) crosses a live one at
      0x6a610000 (0x140000 bytes). Both carry a 0x10000 metadata range at
      0x10b40000. The rerun ran 200 s clean. It is open, not attributed,
      and looks like allocation placement, like the other overlap stops.
    - The fishing title exited by itself once, about 80 s into play, with
      exit status 0 and no error. A second run hit the known frame-3 boot
      stall. A third ran 200 s clean at 26-29 fps. The clean exit is not
      attributed and stays open until it repeats.
    - The last capture of each title was compared by eye with the same
      route in the sweep before the change. No new speckles, wrong mips or
      edge artefacts were seen. The racing, fishing and void platformer
      captures show other scenes than their baselines, because their
      routes ended at different points.
  - Not tested: AMD (wave64 lane layout differs), and further present
    frames of this title beyond the one dumped and the harness captures.
- **Open, recorded for later.** `get_stencil_op`
  (`source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp:1472`) maps
  every op that needs a value to the one Vulkan reference.
  - For `AddClamp`..`SubWrap` (0x05..0x09) the reference becomes
    `STENCILOPVAL`, so the compare uses the op value, not
    `STENCILTESTVAL`.
  - `Ones` sets the reference to 0xFF.
  - The add/sub ops step by 1 in Vulkan; the guest may step by the op
    value.
  - Suggested direction: keep the reference at `STENCILTESTVAL` whenever the
    op does not consume it, and refuse op values Vulkan cannot express. A
    title that uses these ops with differing values is needed as evidence.
