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

## Quick reproduction and verification

Use a Release build with silent guest logging. Keep the private workload path
in an environment variable and keep captures under ignored scratch:

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Do not set `KYTY_BRINGUP_*`, trap-skip, permissive GPU, or fabricated input
flags in an acceptance run. Automatic input may shorten a discovery capture,
but it cannot prove interactive playability.

After a graphics semantic change, run the focused regression suites:

```bash
_build_linux/fc_script '{kyty_run_tests()}' \
  --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
```

The expected result at this handoff is 203 passing tests. A runtime change is
accepted only when the focused tests pass and a strict re-run either preserves
the gameplay-era checkpoint or advances the first failure.

## Problem-to-solution guide

| Symptom | Proven producer | Resolution | Regression evidence |
| --- | --- | --- | --- |
| Structured exit: unknown `ds_read2_b32` | Gen5 DS parser/SPIR-V generator lacked the two-result LDS read | Decode both 8-bit offsets, scale each by four bytes, retain byte-addressed `vaddr`, and load consecutive destination VGPRs from Workgroup memory | Focused packet-to-SPIR-V test and strict runtime advancement |
| White or horizontally corrupted world after a valid earlier frame | RenderTexture update re-entry reset a GPU-owned tiled image to `VK_IMAGE_LAYOUT_UNDEFINED` | Preserve the current image layout on Update; use `UNDEFINED` only for initial creation or an evidenced invalidation/discard | Focused GPU memory/render-target state test |
| Striped or missing output around multi-render-target shaders | Null MRT export tails lost their discard/no-write semantics during SPIR-V generation | Preserve null MRT0–3 tails as no-write exports instead of fabricating color output or truncating the export contract | Focused shader/SPIR-V export test |
| Opaque black rectangles in transparent sprite or prop bounds | Kill-enabled `EarlyZThenLateZ` pixel shaders were emitted with Vulkan `EarlyFragmentTests`, allowing depth commit before `OpKill` | Omit `EarlyFragmentTests` for pixel-kill shaders so discarded fragments cannot write depth; retain it for opaque early-Z shaders | Red/green SPIR-V test and native gameplay-era capture |
| Scene reached only with automatic Cross input | Input automation bypasses the real press/release acceptance contract | Do not change graphics or synthesize completion. Re-run with real keyboard/controller edges and treat inability to reach gameplay as a separate input/synchronization frontier | Pending real-input acceptance |

### Unknown `ds_read2_b32`

Capture the instruction words and decoded fields before editing. The observed
instruction reads two LDS dwords. Its `vaddr` is a byte address while `offset0`
and `offset1` are dword-scaled:

```text
address0 = vaddr + offset0 * 4
address1 = vaddr + offset1 * 4
vdst     = Workgroup[address0]
vdst + 1 = Workgroup[address1]
```

Resolve this at the shared DS decode/SPIR-V seam. Do not special-case the
captured program counter and do not treat the offsets as raw bytes. Verify that
the generated module uses the same Workgroup storage and address convention as
`ds_write_b32`.

### RenderTexture becomes undefined during Update

First distinguish image creation from update re-entry:

1. Record the resource relation, ownership, old layout, and requested
   transition.
2. Confirm whether CPU write-back invalidated the exact resource or only an
   overlapping alias.
3. If the GPU still owns valid contents, preserve its current layout.
4. If an evidenced invalidation requires discard, transition from
   `VK_IMAGE_LAYOUT_UNDEFINED` and do not claim content preservation.

Do not make this decision from GPU vendor IDs. The resource state and Vulkan
layout contract determine the transition.

### Null MRT tails lose discard semantics

Inspect the complete export sequence rather than only MRT0. A shader can write
one or more MRTs and end with null exports that carry termination/no-write
semantics. Keep those tails in the normalized shader representation so the
SPIR-V generator does not invent output for an inactive target.

Validate both forms:

- active MRT exports still write their declared components;
- null MRT0–3 tails produce no color write and preserve control flow.

Do not substitute zero color for a null export. Zero is observable output;
no-write is a different contract.

### Transparent quads become black rectangles

Use the producer/consumer boundary to avoid misdiagnosing this as texture
tiling or blending:

1. Confirm the sampled texture contains meaningful alpha.
2. Confirm alpha reaches the pixel shader and the shader emits `OpKill`.
3. Confirm zero-alpha blending would preserve the destination.
4. Inspect execution modes. If a kill-enabled shader also declares
   `EarlyFragmentTests`, depth may commit before the discard.

The implemented policy is:

```text
pixel kill enabled  -> omit EarlyFragmentTests, commit depth after discard
opaque early-Z      -> retain EarlyFragmentTests
```

Do not add a second pattern-matched alpha test, disable depth globally, or add
a vendor-specific workaround. Those changes hide the ordering bug and can
break opaque geometry.

### Automatic input is not a graphics fix

`KYTY_AUTO_CROSS` is useful only to expose a later graphics frontier quickly.
It does not validate keyboard/controller routing, press/release edges, scene
control, or synchronization. Keep discovery captures labeled accordingly.

For acceptance, start from a strict environment, deliver real input edges,
move in both directions, perform one action, and confirm that frame
presentation continues without reintroducing the visual defects above.

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
