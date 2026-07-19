# Internal resolution policy

## Decision

The resolution core is a capability-neutral policy, separate from Vulkan and
from guest descriptors. `ScreenWidth` and `ScreenHeight` are the future host
target inputs and retain their existing 1280x720 defaults. A guest display
extent is registered independently, and guest dimensions, pitch, tiling,
addresses, descriptors, and ABI remain unchanged.

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

The policy does not change runtime behavior yet. Vulkan integration must:

- construct the policy from `Config::GetScreenWidth/Height`;
- register the actual guest display extent from VideoOut evidence;
- add host extent and scale to resource/pipeline identities without replacing
  guest extent;
- scale an entire compatible MRT/depth cohort together;
- map viewport, scissor, resolve, blit, and copy rectangles through the same
  floor-origin/ceil-end transform;
- keep presentation filtering as a separate, capability-driven decision;
- reject mixed or unsafe cohorts instead of partially scaling them.
