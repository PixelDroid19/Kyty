# Internal resolution policy

## Decision

The resolution core is a capability-neutral policy, separate from Vulkan and
from guest descriptors. `InternalResolutionWidth` and
`InternalResolutionHeight` are the future host render-target inputs and retain
1280x720 defaults. `ScreenWidth` and `ScreenHeight` remain independent
window/swapchain inputs. A guest display extent is registered independently,
and guest dimensions, pitch, tiling, addresses, descriptors, and ABI remain
unchanged.

Three designs were considered:

1. A global floating-point multiplier. It is small, but produces unstable
   rounding at resource and rectangle boundaries and cannot express why a
   resource must remain native.
2. Vulkan-side image and viewport rewriting. It reaches the renderer quickly,
   but couples policy to one backend and risks changing guest-visible state.
3. A pure integer-rational policy with explicit resource eligibility and
   stable identity. It provides deterministic floor-origin/ceil-end mapping,
   supports downscaling and upscaling, and can be tested before integration.

Option 3 is selected. The uniform scale fits the registered guest display
inside the target without changing aspect ratio. Color and depth attachment
cohorts can therefore share one exact scale.

## Initial safety boundary

Only uncompressed, two-dimensional, single-mip, single-sample color or
depth/stencil attachments are eligible. Sampled-only images, storage images,
buffers, shader-writable images, CPU upload/readback resources, compressed
images, and ambiguous aliases remain native. A native-reason enum keeps this
boundary observable and extensible.

The strict launcher maps `KYTY_INTERNAL_RESOLUTION_WIDTH` and
`KYTY_INTERNAL_RESOLUTION_HEIGHT` to those new configuration values. VideoOut
records the extent only after buffer registration succeeds. Bounded agent
telemetry reports target, guest display, candidate host extent, scale,
classification, native reason, and `scaling_applied=false`.

The policy does not change Vulkan behavior yet. Vulkan integration must:

- consume the policy already constructed from
  `Config::GetInternalResolutionWidth/Height`;
- register the actual guest display extent from VideoOut evidence;
- add host extent and scale to resource/pipeline identities without replacing
  guest extent;
- scale an entire compatible MRT/depth cohort together;
- map viewport, scissor, resolve, blit, and copy rectangles through the same
  floor-origin/ceil-end transform;
- keep presentation filtering as a separate, capability-driven decision;
- reject mixed or unsafe cohorts instead of partially scaling them.

## Shader-coordinate boundary

Pixel shaders that consume `FragCoord` (`ps_pos_xy`) need the active
attachment's host-to-guest ratio. The pixel interface carries this as explicit
reduced integer X/Y fractions with a 1:1 default. It is part of shader-module
identity.
At non-native scale, generated SPIR-V multiplies the host `FragCoord` X/Y by
guest/host before exposing the values to guest VGPRs. The 1:1 path retains the
existing direct loads.

This does not make sampled aliases safe to scale. The current shader binding
model has no per-texture guest/host extent:

- `OpImageFetch` receives guest integer texel coordinates but would address
  host texels directly.
- `OpImageQuerySizeLod` currently observes the host image extent; guest shader
  semantics require the guest extent. It is also used internally while
  materializing offset sampling.

Therefore resources used by either path must remain native. A later phase must
add exact per-binding scale/extent identity, transform integer fetch
coordinates, and provide guest-visible query sizes with deterministic tests
before expanding eligibility. Storage-image writes remain a separate blocked
case and are not implied by `FragCoord` support.
