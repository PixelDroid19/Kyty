# Kyty Gen5 runtime graphics investigation handoff

Updated: 2026-07-18

Status: the runtime advances into sustained gameplay-era presentation without
a process-killing error. The opaque black sprite/prop rectangles are absent
after correcting pixel-kill depth ordering. A persistent, device-qualified
Vulkan pipeline cache now removes most repeated driver pipeline compilation
cost across runs. Full playability acceptance and sustained-FPS optimization
remain open.

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

On the exact tracked branch state, Linux Release passed 205 focused
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

The expected result at this handoff is 205 passing tests. A runtime change is
accepted only when the focused tests pass and a strict re-run either preserves
the gameplay-era checkpoint or advances the first failure.

## Problem-to-solution guide

| Symptom | Proven producer | Resolution | Regression evidence |
| --- | --- | --- | --- |
| Structured exit: unknown `ds_read2_b32` | Gen5 DS parser/SPIR-V generator lacked the two-result LDS read | Decode both 8-bit offsets, scale each by four bytes, retain byte-addressed `vaddr`, and load consecutive destination VGPRs from Workgroup memory | Focused packet-to-SPIR-V test and strict runtime advancement |
| White or horizontally corrupted world after a valid earlier frame | RenderTexture update re-entry reset a GPU-owned tiled image to `VK_IMAGE_LAYOUT_UNDEFINED` | Preserve the current image layout on Update; use `UNDEFINED` only for initial creation or an evidenced invalidation/discard | Focused GPU memory/render-target state test |
| Striped or missing output around multi-render-target shaders | Null MRT export tails lost their discard/no-write semantics during SPIR-V generation | Preserve null MRT0–3 tails as no-write exports instead of fabricating color output or truncating the export contract | Focused shader/SPIR-V export test |
| Opaque black rectangles in transparent sprite or prop bounds | Kill-enabled `EarlyZThenLateZ` pixel shaders were emitted with Vulkan `EarlyFragmentTests`, allowing depth commit before `OpKill` | Omit `EarlyFragmentTests` for pixel-kill shaders so discarded fragments cannot write depth; retain it for opaque early-Z shaders | Red/green SPIR-V test and native gameplay-era capture |
| Large first-run stalls recur after restarting Kyty | `VkPipelineCache` was always created empty and never persisted | Validate the standard cache header against vendor/device/UUID, load compatible bounded data, and save dirty cache data atomically at a rate limit | Header tests plus isolated cold/warm driver measurements |
| Scene reached only with automatic Cross input | Input automation bypasses the real press/release acceptance contract | Do not change graphics or synthesize completion. Re-run with real keyboard/controller edges and treat inability to reach gameplay as a separate input/synchronization frontier | Pending real-input acceptance |

### Pipeline compilation hitches across restarts

Kyty previously passed an empty `VkPipelineCache` to every graphics and compute
pipeline creation. A process restart therefore discarded the opaque driver
cache even when guest shader and render state were unchanged.

The cache store now:

- uses `KYTY_VULKAN_PIPELINE_CACHE` when an explicit test location is needed,
  otherwise a per-user cache directory;
- includes the Vulkan vendor ID, device ID, and pipeline-cache UUID in the
  default filename;
- accepts at most 64 MiB and validates the standard version-one header before
  passing bytes to Vulkan;
- retries with an empty cache if a driver rejects otherwise compatible data;
- writes a sibling temporary file and replaces the destination;
- saves after the first new pipeline and consolidates later dirty data at most
  once every five seconds.

With Mesa's independent shader cache disabled to isolate this path, a bounded
cold run spent 268 ms in 87 `vkCreate*Pipelines` calls (maximum 25 ms). The
equivalent warm run spent 6 ms in 84 calls (maximum 6 ms), a 97.8% reduction
in the measured driver-pipeline stage. Cache snapshots were approximately
0.6 MiB and took about 1–2 ms each.

This does **not** prove that every pipeline miss is cheap: guest shader parsing,
SPIR-V generation/optimization, and application pipeline lookup occur outside
`vkCreate*Pipelines`. It also does not improve the established steady-state
gameplay rate by itself.

If the cache is suspected after a driver update:

1. Point `KYTY_VULKAN_PIPELINE_CACHE` at a new empty path for one run.
2. Confirm the new file has a nonzero size and a second run still reaches the
   same strict frontier.
3. Compare Release+Silent runs with the same resolution and shader-cache state.
4. If a file is stale, remove only that cache file. Do not disable validation,
   invent a pipeline, or substitute a placeholder shader.

Malformed, foreign-device, oversized, and unreadable files are ignored; cache
I/O failure is a performance miss, not a guest-visible semantic fallback.

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

The current performance frontier is separate from graphics correctness. A
Release+Silent gameplay sample showed roughly 6 FPS after warm-up even though
the menu exceeded 100 FPS. Read-only probes attributed approximately
105–122 ms/s to full-range memory hashing and 140–180 ms/s to immediate
submit/fence waits; neither alone explains the remaining frame time. Pipeline
miss bursts explain severe transient freezes, while persistent low FPS still
requires producer-level work in shader reuse, GPU memory tracking, command
submission, and resource upload. Change one contract at a time and compare
against the same correct gameplay capture.

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
