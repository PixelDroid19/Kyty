# Kyty Gen5 runtime graphics investigation handoff

Updated: 2026-07-18

Status: the runtime advances into sustained gameplay-era presentation without
a process-killing error. The opaque black sprite/prop rectangles are absent
after correcting pixel-kill depth ordering. Full playability acceptance still
requires a repeatable real-input run without diagnostic automation.

This document intentionally excludes private workload names, identifiers,
paths, binaries, screenshots, shader hashes, and raw logs. Keep those only in
ignored scratch and address the workload through `$KYTY_GUEST_ROOT`.

## Verified advances

The current graphics branch contains four isolated, tested changes:

| Commit | Contract | Verification |
| --- | --- | --- |
| `990b9a40` | Decode and lower Gen5 `ds_read2_b32` with two dword-scaled offsets over byte-addressed Workgroup LDS | Focused parser/SPIR-V test plus strict runtime |
| `14633fe6` | Preserve the layout of GPU-owned RenderTextures across update re-entry | Focused graphics state test plus strict runtime |
| `9cc21524` | Preserve discard semantics for null MRT0–3 export tails | Focused shader/SPIR-V test plus strict runtime |
| `9b026e53` | Keep pixel-kill shaders on late Vulkan depth commit while retaining early fragment tests for opaque shaders | Red/green SPIR-V test plus gameplay-era native capture |

On the exact tracked branch state, Linux Release passed 203 focused
GraphicsPackets/GraphicsState tests. The earlier strict Release+Silent baseline
without `KYTY_BRINGUP_*`, automatic input, or permissive fallbacks exceeded
2,300 presents. New gameplay-era visual captures used automatic Cross only to
reach the scene and therefore are discovery evidence, not input acceptance.
No structured EXIT, host fault, or Vulkan device loss was observed.

The historical horizontal stripes and the later opaque sprite/prop rectangles
are absent in the post-fix native capture. Background, props, character,
lighting, transparency, and frame progression remain recognizable.

## Current frontier

There is no known process failure or repeatable visual corruption before the
current gameplay-era checkpoint. Always re-capture: a new structured EXIT,
host fault, or earlier visual regression supersedes later work.

The first proven bad boundary had been native VideoOut. The writer sampled a
valid RGBA8 atlas and emitted coverage into a four-MRT G-buffer. A later pixel
shader performed an alpha comparison and reached `OpKill`, but its guest depth
mode requested early rejection followed by late depth commit. Kyty translated
that mode to Vulkan `EarlyFragmentTests`, which can commit depth before
`OpKill`. Transparent portions of a sprite quad therefore occluded later work
as opaque rectangular footprints.

The fix omits `EarlyFragmentTests` when `shader_kill_enable` is active, allowing
Vulkan depth commit after fragment discard. Opaque shaders keep the existing
early-fragment path.

## Evidence and exclusions

- Source atlas dumps contain meaningful alpha; the sampled descriptor used
  RGBA8 UNORM with identity swizzle and guest upload.
- Blend factors, compressed MRT component order, and sampled alpha propagation
  matched the captured contracts. With source alpha zero, blending preserves
  the destination.
- Adding another pattern-matched alpha discard did not help because the
  original failure was the timing of depth commit relative to an existing
  discard.
- Pure CPU tile-27 detile and final VideoOut conversion were not the producer.
- A red test proved the old generator emitted `EarlyFragmentTests` for a
  kill-enabled shader. The same test now proves kill-enabled shaders omit it
  while opaque early-Z shaders retain it.
- Temporary MRT, descriptor, and frame-selection instrumentation was removed
  before the semantic commit.

## Validation gate for the next change

1. Re-capture the gameplay-era checkpoint on the exact branch.
2. Use real keyboard/controller press and release edges; do not use
   `KYTY_AUTO_CROSS` for acceptance.
3. Confirm the scene remains free of stripes, black quad footprints, white
   world output, and stale UI overlays.
4. Exercise movement in both directions and one action while presents advance.
5. Run with Vulkan validation where supported and record relevant errors.
6. Re-run the focused GraphicsPackets/GraphicsState suites.
7. Treat any new structured EXIT, host fault, or earlier visual regression as
   the new first frontier.

Process survival, a clean HUD, or a single recognizable frame is not
playability acceptance. Do not fabricate clears, alpha tests, resources,
signals, formats, or fallbacks to make the workload continue.
