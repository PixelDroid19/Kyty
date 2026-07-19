# Kyty Gen5 runtime graphics investigation handoff

Updated: 2026-07-18

Status: the strict runtime advances into sustained gameplay-era presentation
without a process-killing error. Rendering is not yet accepted because opaque
black rectangles remain around sprite and prop bounds.

This document intentionally excludes private workload names, identifiers,
paths, binaries, screenshots, shader hashes, and raw logs. Keep those only in
ignored scratch and address the workload through `$KYTY_GUEST_ROOT`.

## Verified advances

The current graphics branch contains three isolated, tested changes:

| Commit | Contract | Verification |
| --- | --- | --- |
| `990b9a40` | Decode and lower Gen5 `ds_read2_b32` with two dword-scaled offsets over byte-addressed Workgroup LDS | Focused parser/SPIR-V test plus strict runtime |
| `14633fe6` | Preserve the layout of GPU-owned RenderTextures across update re-entry | Focused graphics state test plus strict runtime |
| `9cc21524` | Preserve discard semantics for null MRT0–3 export tails | Focused shader/SPIR-V test plus strict runtime |

On the exact tracked branch state, Linux Release+Silent passed 202 focused
GraphicsPackets/GraphicsState tests. A strict run without `KYTY_BRINGUP_*`,
automatic input, or permissive fallbacks exceeded 2,300 presents. No structured
EXIT, host fault, Vulkan device loss, or validation error was observed in that
run.

The historical horizontal stripe corruption was absent. The remaining visual
defect consists of opaque black rectangles whose positions and extents follow
scene sprites and props. HUD, character, background, lighting, and frame
progression remain recognizable around them.

## Current frontier

There is no known process failure before the visual defect on the current
branch. Always re-capture: a new structured EXIT or host fault supersedes this
visual investigation.

The first proven boundary containing the rectangles is native VideoOut output.
Pixel comparison between native BMP and derived PNG captures rules out the PNG
conversion path. The rectangles are local sprite footprints rather than a
fixed overlay or a global tile-period pattern.

Captured pixel shaders establish a likely producer/consumer chain:

1. A sprite/G-buffer writer samples an RGBA atlas and exports four MRTs.
2. Its MRT3 alpha is derived from the sampled alpha and interpolated factors.
3. A later lighting consumer samples MRT3 alpha, compares it with a threshold,
   and discards or shades the pixel.
4. The composed result reaches native VideoOut with the black rectangles
   already present.

A previous run retained the rectangles even when the observed alpha-test
shader lowered its discard path to `OpKill`. Therefore, adding another
pattern-matched discard is not an evidenced fix. The value entering the test,
the attachment receiving it, or the downstream composite remains wrong.

## Evidence and exclusions

- Source RGBA8 atlas dumps contain meaningful alpha, so transparent source
  content exists. This does not prove that the affected draw binds that exact
  backing.
- Historical four-MRT dumps contain distinct alpha distributions. Because
  they were captured only at the end of a different frame, they cannot identify
  which attachment first introduced the current rectangle.
- The final VideoOut image is fully opaque as expected; final alpha cannot
  diagnose intermediate coverage.
- A run with alpha-kill lowering still showed the defect, which deprioritizes
  “missing fragment kill” as the root cause.
- The shape is inconsistent with the known periodic signature of the pure CPU
  tile-27 detile path. Tile/layout can still matter indirectly through the
  selected Vulkan backing or alias.
- RenderTexture update preservation fixed the earlier global white-world
  failure. It does not explain content-following rectangles by itself.

## One falsifiable hypothesis

The highest-priority hypothesis is that the affected draw samples incorrect
alpha/content from the Vulkan backing selected for its atlas or alias.

The next capture must follow one affected draw and preserve alpha at all three
boundaries:

1. The actual sampled Vulkan image, with descriptor format, tile mode, extent,
   pitch, swizzle, selected object, rejected candidates, and explicit path:
   RenderTexture, StencilTexture, VideoOut, guest upload, or guest skip.
2. MRT0–3 immediately before and after the writer draw, including load
   operation, clear value, blend state, write mask, and attachment identity.
3. The first lighting/composite target and native VideoOut for the same
   present, together with the consumer threshold.

Interpret the capture as follows:

- Bound alpha is already wrong: fix backing selection, alias, descriptor,
  swizzle, or upload semantics.
- Bound alpha is correct but writer MRT3 is wrong: fix shader export, fragment
  kill, blend, load, clear, or channel mapping.
- Writer MRT3 is correct but the consumer output is wrong: fix consumer
  sampling, threshold, attachment binding, or composite semantics.
- All intermediate targets are correct and only VideoOut is wrong: investigate
  the display/composition boundary.

Any instrumentation must be opt-in, bounded to the selected draw, write only
to scratch, and be removed before the semantic commit.

## Validation gate for the next change

1. Reproduce the current rectangles on the exact branch before editing.
2. Capture the first wrong boundary for one affected draw.
3. Add a sanitized deterministic test that fails for that specific contract.
4. Implement one semantic change.
5. Run the focused GraphicsPackets/GraphicsState suites.
6. Run the private workload twice under strict Release+Silent conditions.
7. Confirm the captured wrong boundary is corrected, the rectangles disappear,
   and no earlier process or visual regression returns.
8. Commit and push the evidenced change; record the new frontier here.

Process survival, a clean HUD, or a single recognizable frame is not
playability acceptance. Do not fabricate clears, alpha tests, resources,
signals, formats, or fallbacks to make the workload continue.
