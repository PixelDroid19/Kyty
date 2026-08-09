# Gen5 depth-reference sampling design

## Objective

Implement the Gen5 depth-reference, level-zero image-sampling contract as a
complete shader-to-Vulkan path. The change must preserve regular sampling,
reject ambiguous bindings, and advance the first strict renderer failure
without weakening unsupported-behavior checks.

Success for this cycle requires all of the following:

- the captured instruction parses to the existing depth-reference operation;
- shader resource analysis marks its sampler as comparison-enabled;
- Vulkan sampler creation uses the descriptor's depth comparison function;
- SPIR-V generation emits a validated depth-reference level-zero sample for
  supported 2D, 2D-array, and cube descriptors;
- a strict guest run advances beyond the previous shader-emitter failure; and
- the established 2D catalog retains its strict rendering frontier.

This cycle does not claim complete 3D compatibility. A later first failure is
the next unit of work.

## Evidence and current failure

The strict runtime currently reaches an interactive screen and exits after a
deliberate input edge because the shader generator has no emitter for the
already parsed depth-reference instruction. The parser records the packed
address as depth reference followed by coordinates, and the sampler descriptor
already exposes its three-bit comparison function.

The missing contract is downstream:

- resource analysis records sampler descriptors but not the operation that
  consumes them;
- sampler-cache identity contains only the four descriptor words;
- sampler creation always requests regular sampling; and
- the depth-reference SPIR-V emitter deliberately rejects every shape.

The public instruction reference confirms that the comparison form adds one
depth-reference address component and that the level-zero form fixes the mip
level at zero. Vulkan requires the SPIR-V depth-reference operation and sampler
comparison state to agree.

## Chosen design

### Sampler operation evidence

Each materialized sampler carries one `ImageSampleOperation` value alongside
its descriptor, slot, and source register. Resource analysis derives the value
from actual shader consumers:

- ordinary sample and gather operations produce `Regular`;
- the depth-reference operation produces `DepthReference`;
- a sampler register consumed by both forms is `Mixed` evidence and is rejected
  before shader generation.

`Mixed` is analysis state, not a Vulkan sampler mode. It prevents Kyty from
silently choosing one behavior for two incompatible consumers. Supporting that
case later would require separately represented sampler bindings and direct
evidence that the guest uses such a pattern.

Static metadata, direct resources, and dynamically loaded descriptors all use
the same classifier. Deduplication requires both equal descriptor words and an
equal operation so comparison and regular bindings cannot alias accidentally.

### Vulkan sampler identity and creation

`SamplerCache::GetSamplerId` receives the classified operation. Cache entries
compare both the four descriptor words and the operation. Sampler creation
passes the operation to the existing comparison-state resolver, then maps the
descriptor's comparison function to `VkCompareOp`.

Unnormalized-coordinate policy remains authoritative. If it disables
comparison, a depth-reference binding is rejected rather than downgraded to a
regular sampler.

### Image view compatibility

The texture binding must prove that a depth-reference instruction resolves to
a depth-capable sampled image view. Existing depth images use the dedicated 2D
or 2D-array depth view. A regular color view, integer view, three-dimensional
image, missing depth view, or unsupported descriptor shape is a structured
failure.

Cube descriptors use the array-compatible depth view and retain the existing
face-to-layer coordinate convention. No image is reinterpreted as depth solely
because a shader requests comparison.

### SPIR-V emission

The emitter accepts only the already decoded single-component forms for 2D,
2D array, and cube. It loads the selected depth image and comparison sampler,
constructs coordinates from the decoded address operands, and emits
`OpImageSampleDrefExplicitLod` with `Lod 0`.

Address inputs are:

- 2D: depth reference, x, y;
- 2D array: depth reference, x, y, layer;
- cube: depth reference, x, y, face/layer.

The scalar result is written to the one destination component. Any unexpected
mask, type, shape, descriptor index, or missing binding returns failure through
the existing strict generator path.

## Alternatives rejected

### Enable comparison whenever the descriptor function is nonzero

The comparison-function bits exist in every sampler descriptor. Treating them
as an operation flag would change ordinary texture sampling and could make the
same descriptor behave differently depending on unrelated bit contents.

### Enable comparison for every sampler in a shader containing one depth sample

This loses per-register evidence and can corrupt unrelated regular textures in
the same stage.

### Duplicate every sampler binding preemptively

This expands descriptor layout and push-constant behavior without evidence
that mixed use occurs. The smaller strict design represents only observed,
unambiguous consumers.

## Failure behavior

The implementation must fail before Vulkan submission when:

- a sampler has mixed regular and depth-reference consumers;
- comparison is incompatible with another sampler policy;
- the image descriptor or resolved image view is not depth-capable;
- the sample shape or destination mask is unsupported; or
- resource analysis cannot associate the instruction with one sampler.

No fallback sampler, color image reinterpretation, default comparison
function, or skipped instruction is permitted.

## Verification

The implementation follows one red-green cycle:

1. Replace the existing rejection fixture with tests that require operation
   classification, distinct cache identity, and valid depth-reference SPIR-V.
2. Run the focused test and confirm it fails because comparison state or the
   emitter is missing.
3. Implement the minimum resource, cache, binding, and emitter changes.
4. Validate generated modules with the repository's SPIR-V validation path.
5. Run the focused GraphicsPackets and GraphicsState filters.
6. Run the Vulkan graphics diagnostic covering sampler creation and a real
   depth-reference sample where the host test device supports it.
7. Rebuild the strict runtime and repeat the exact input transition. Success is
   removal of the previous emitter failure and either a later structured first
   failure or a rendered 3D checkpoint.
8. Re-run the established strict catalog with native captures and continued
   presents. A window or a single non-flat frame is not sufficient.

## Commit boundaries

Use separate conventional commits for the design, the tested implementation,
and any later independent frontier. Commit messages and tracked text contain no
private workload identifiers, local paths, captures, or references to other
emulator implementations.
