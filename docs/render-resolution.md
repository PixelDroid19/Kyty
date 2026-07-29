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
- `PresentationFilter`: `Linear` or `Nearest`.

The matching environment variables are:

- `KYTY_RENDER_RESOLUTION_MODE`
- `KYTY_RENDER_RESOLUTION_WIDTH`
- `KYTY_RENDER_RESOLUTION_HEIGHT`
- `KYTY_PRESENTATION_FILTER`

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

## Diagnostics

The native agent protocol reports render-resolution state under
`render_resolution` in protocol version `5`. The snapshot includes mode,
presentation filter, target extent, guest display extent, candidate host extent,
scale, classification, native reason, and whether scaling was applied.

Runtime reports must distinguish a boot, a rendered frame, and sustained
gameplay. Do not claim compatibility from a window or one frame alone.
