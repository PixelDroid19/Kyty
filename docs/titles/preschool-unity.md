# Preschool Unity title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Preschool Unity title: unreadable EUD snapshot (2026-10-21, open)

- **Observed once.** Intel Arc A770, Mesa ANV 26.2.5, strict, HEAD
  89e3b892, generic pad route: after about frame 30,400 (91 s) the run
  stopped on `unreadable Gen5 EUD snapshot: dwords=16` with the table
  pointer in user register 28 (`Shader.cpp:3366` on main). The guest
  words at the table address could not be read when the shader was bound.
- **Not reproduced.** A 150 s repeat of the same binary and route ran
  without a stop at 290-367 FPS.
- **Direction.** On the next occurrence, record the mapping state of the
  table address (mapped, protected or released, and by which call) to
  tell a guest that frees the table early from a host-side read race.

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
