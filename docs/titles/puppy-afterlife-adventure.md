# Puppy afterlife adventure

Title: PPSA33802 - My Little Puppy.

Per-title records, newest first. `docs/BRINGUP.md` keeps the invariants,
methodology, frontier and the index of every record.

## Puppy afterlife adventure: washed-out image and pipeline crash (2026-11-16, fixed)

- Observed (Intel Arc A770, mesa-git ANV, strict run, 60 fps limit): first a crash on pipeline
  creation; after that fix the whole picture turned cyan-white a few seconds into the menu.
- Crash evidence (failing SPIR-V dumped with `KYTY_PIPELINE_FAIL_DUMP`): two sources of one
  instruction with negate plus absolute modifiers produced the same SPIR-V temporary name, so
  the module defined one id twice. Change: those temporaries are named per source operand
  (`ShaderSpirvOperands.cpp`), translator version 220; regression test
  `EmulatorShaderArithmetic.NegatedAbsoluteModifiersOnTwoSources`.
- Washed-out evidence (RenderDoc capture, guest register dumps): the colour-grading user LUT
  (1024x32 RGBA8 with DCC) is drawn with additive blending (SRC_ALPHA, ONE) on load. The guest
  clears it every frame with a DCC metadata fill of key 0x40. A sampled texture partly overlaps
  the 0x2000-byte metadata range, and the metadata clear refused to publish whenever any
  non-exact image alias was present, so the LUT accumulated to white. Whether a texture overlaps
  depends on placement, which is why some runs looked correct.
- Change: a sampled texture that only partly overlaps metadata bytes no longer vetoes the clear;
  render targets, display buffers, storage images and depth still do
  (`GraphicsRenderAttachments.cpp`, `ResolveComputeColorFillIdentity`).
- Verified: three runs with the overlapping texture present show the clear published and
  consumed every frame, and the menu and world render with a blue sky, green tree and yellow
  grass. A full run counted more than 3500 LUT binds, all with the pending clear consumed.
  The scripted route reaches gameplay (walking the cloud field, meeting characters).
  Suika Game reaches its play field and Minecraft its world; Astro Bot runs its intro without
  errors (cold shader cache after the version bump, gameplay not reached in 120 s).
- Excluded: alpha-to-coverage. About 28 % of the draws enable `DB_ALPHA_TO_MASK` (value
  `0xaa01`, single sample) and Kyty creates every pipeline with alpha-to-coverage off. Turning
  it on from the guest bit (local experiment, pipeline key included) left foliage edges and
  shadow silhouettes unchanged on ANV in the same scene.
- Excluded: shadow sampling. The cascaded shadow atlas (D16, 4096x4096, reversed Z) holds the
  casters, the receiver samples it through a linear comparison sampler (GREATER_EQUAL), and a
  debugged ground pixel outside every caster is lit as expected.
- Open: the user reported flashes and badly placed shadows in gameplay. Neither has been matched
  to a cause yet: no luminance spike other than scene fades in a 700-frame sequence, and the
  hard, stepped shadow edges have not been compared with a reference. The texture object that
  partly overlaps the LUT metadata range is unidentified (it may be stale or oversized).
