# Render resolution architecture

Kyty separates guest render resolution from final presentation. Render resources
are planned before materialization, while presentation scaling only transforms
the final image into the swapchain. Guest-visible sizes, addresses, pitches,
tiling, descriptors, and ABI data remain guest values.

## Configuration

The active render-resolution keys are:

- `RenderResolutionMode`: `Fixed` or `Native`.
- `RenderResolutionWidth`: fixed render target width. The default is `1280`.
- `RenderResolutionHeight`: fixed render target height. The default is `720`.
- `PresentationFilter`: `Linear`, `Nearest` or `Fsr`.
- `PresentationFsrSharpness`: RCAS sharpness for `Fsr`, `0` to `100`. The
  default is `90`.

The matching environment variables are:

- `KYTY_RENDER_RESOLUTION_MODE`
- `KYTY_RENDER_RESOLUTION_WIDTH`
- `KYTY_RENDER_RESOLUTION_HEIGHT`
- `KYTY_PRESENTATION_FILTER`
- `KYTY_PRESENTATION_FSR_SHARPNESS`

`ScreenWidth` and `ScreenHeight` configure only the host window and swapchain.
They do not select the guest render resolution.

Invalid enum values or zero render extents stop startup during configuration
loading with a direct diagnostic. The fixed extent is a maximum bound: Kyty
chooses one uniform scale that is not greater than `1`, preserves aspect ratio,
and keeps exact guest-to-host and host-to-guest transforms.

## Planning

`RenderResolutionCoordinator` owns render-resource planning for each pass. It
receives color attachments, depth/stencil attachments, aliases, views, shader
usage, and VideoOut registration before resources are materialized. The output
is an immutable decision for the pass.

The coordinator treats the framebuffer as one compatibility group. If any
member requires native resolution, the full group is native. Mixed host extents
inside one framebuffer are rejected instead of silently replacing individual
attachments. This applies to color, depth/stencil, offscreen, depth-only, and
VideoOut paths.

Resource identity includes guest extent, host extent, scale, resource kind,
sample count, alias information, shader usage, and the fixed/native mode.
Resources that share memory propagate the selected state across aliases before
materialization. When a late binding changes the state, the framebuffer is
replanned and rebound. Non-converging plans are errors; they are not substituted
with a best-effort extent.

## States

Render resources move through these states:

- `Eligible`: the resource can participate in fixed-resolution rendering.
- `Scaled`: the resource is materialized at the planned host extent.
- `NativeRequired`: the resource or one of its aliases forces the whole group
  to guest-native extent.

Native requirements are explicit and diagnostic-friendly. Examples include
compressed images, unsupported dimensions, mipmapped resources, multisampled
resources until a resolve path is selected, shader-writable images, CPU
transfer resources, ambiguous aliases, invalid extents, identity scale, and
arithmetic overflow.

## Coordinates

Viewport, scissor, copy rectangles, resolve rectangles, and shader coordinate
scales use the same integer transform. Guest origins are floored after scaling;
guest ends are ceiled after scaling. Pixel shaders that consume guest position
receive an explicit reduced host-to-guest fraction in shader identity.

## Presentation

Presentation scaling is intentionally separate from render planning. The
presentation stage consumes one final image and scales it into the swapchain
using the configured filter after validating Vulkan support. It preserves the
current framing and aspect-ratio policy.

`Fsr` upscales the final image with AMD FidelityFX Super Resolution 1.0 (MIT,
`source/3rdparty/fidelityfx_fsr1`):

- A same-size blit converts the final image to half float. EASU then upsamples
  it to the swapchain extent, RCAS sharpens it, and a same-size blit writes the
  swapchain image.
- Both passes run on display-encoded values. A linear-light source (sRGB or
  float format) is encoded in the EASU input callbacks. For an sRGB swapchain,
  RCAS decodes its result so that the final blit encodes it once.
- The sharpness percentage maps to RCAS attenuation stops as
  `(100 - value) / 50`. `100` is the sharpest, and `90` is 0.2 stops.
- FSR 1.0 only upscales. It runs when the final image fits inside the
  swapchain and is smaller on at least one axis. An equal or larger final
  image uses the configured blit path instead (linear for `Fsr`), because the
  passes would add cost without adding detail.
- To use it as an upscaler, make the window larger than the final image: for
  example `ScreenWidth = 1920`, `ScreenHeight = 1080` with
  `RenderResolutionMode = Fixed` at 1280x720.
- `Fsr` does not change the render resolution or the guest's work. `Fixed`
  scales only the display attachment cohort (the VideoOut buffer and its
  depth). Offscreen targets keep their guest size. A title whose GPU cost sits
  in offscreen passes, or a title bound by the command processor or the guest
  CPU, keeps its frame rate. The AMD beat 'em up renders 3840x2160 offscreen
  passes. Under `Fixed` 1280x720 its display was scaled
  (`scaling_applied: true`), yet it measured 7.1, 4.1 and 3.6 fps against
  6.9, 4.1 and 3.8 fps at `Native`, with the GPU about 80% busy in both.
- The shaders live in `source/emulator/src/Graphics/Shaders`.
  `scripts/build_presentation_fsr_shaders.py` regenerates the embedded SPIR-V
  in `PresentationFsrShaders.inc` (requires `glslangValidator`).
- Verified on Intel with a 1920x1080 window. The beat 'em up presented a
  1280x720 final image and was upscaled to 1920x1080. Its mean channel values
  matched a bilinear upscale of the same frame to within 0.004. The racing
  title at 1:1 matched its re-encoded source with an MAE of 0.0044 (RCAS
  only), which is what led to the rule that equal extents skip the passes.
  The swapchain on both hosts is UNORM BGRA, so the sRGB-swapchain branch has
  not run.
- Temporal upscalers are not offered. FSR 2 and later, FSR 4 (ML) and XeSS
  all need the renderer's per-pixel motion vectors, depth and jittered
  projection. A presentation stage, or an emulator in general, does not have
  these for an arbitrary guest. On top of that, the FidelityFX SDK documents:
  - FSR 4 needs its signed binary distribution and "an AMD 7000 series
    discrete GPU or AMD 9000 series GPU or later".
  - ML frame generation needs "Windows 11, DirectX 12 Agility SDK 1.4.9 and
    an AMD 9000 series GPU or later".
  - Its known-issues table states "Vulkan is currently not supported in SDK".

## Diagnostics

The native agent protocol reports render-resolution state under
`render_resolution` in protocol version `5`. The snapshot includes mode,
presentation filter, target extent, guest display extent, candidate host extent,
scale, classification, native reason, and whether scaling was applied.

Runtime reports must distinguish a boot, a rendered frame, and sustained
gameplay. Do not claim compatibility from a window or one frame alone.
