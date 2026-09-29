# Fragment execution across host subgroups

## Status

The current renderer cannot execute a guest fragment wave wider than the
host subgroup when the program observes values from both halves. The current
strict compiler stop is a full-mask OR row shift, followed by a cross-row
permutation and scalar reads of lanes 31 and 63. Decoding is complete for the
captured program; lowering and guest execution are not.

A separate Vulkan 1.4 experiment has verified one prerequisite on the current
host: exporting complete native fragment quads, including helper values, to a
buffer and consuming them in a 64-invocation compute workgroup. This document
records that result and the integration design. No runtime strategy has been
enabled by the experiment. The guest still stops at the same strict failure.

## Contracts

The local RDNA2 ISA is the primary source for guest execution:

- Section 3.12 initializes EXEC from the workitem-valid mask. It does not
  establish equivalence between one 64-lane wave and two independent waves.
- Section 13.3.9 defines 16-lane DPP rows, source EXEC, row and bank masks,
  fetch-inactive and bound control.
- Section 12.12 defines row permutation and the scalar selector table.
- A read of lane 63 can observe state contributed by the upper half even
  when the final value is uniform. A host subgroup reduction over only the
  lower half loses that state.

The [Vulkan shader execution specification](https://docs.vulkan.org/spec/latest/chapters/shaders.html)
provides the transport boundary:

- Fragment quad broadcasts launch helpers for missing covered locations.
- The quad has four adjacent subgroup indices aligned to four, with one
  primitive, layer and sample identity.
- Helper storage writes to buffers have no effect. A covered invocation must
  therefore export helper values obtained through quad broadcasts.
- Quad broadcasts have the helper-participation guarantee independently of
  general subgroup shuffles. Wider exchanges have additional convergence
  requirements.

This boundary transports rasterizer inputs. It does not relax guest lane
selectors, drop a half-wave, or supply invented helper inputs.

## Verified transport experiment

The standalone experiment contains original, synthetic geometry and shaders;
it is stored outside the repository. It requires Vulkan 1.4, fragment quad
operations, fragment storage writes and the geometry feature for fragment
primitive identity. Every required feature is checked and enabled before
creating its device. It performs the following sequence:

1. Rasterize two overlapping triangles with an interpolated two-component
   value. Keep the normal color attachment as an independent reference.
2. Broadcast each quad's four interpolated values, fragment coordinates,
   fine derivatives and coverage bits before any writer-selection branch.
3. Select the first covered quad member. Only that member stores the complete
   quad. Primitive identity keeps overlapping triangles' records distinct.
4. End rendering and apply a fragment-write to compute-read memory dependency.
5. Consume the records in workgroups of 64. Shared storage preserves all 64
   logical values. Execute OR row shifts by 1, 2, 4 and 8, exchange the adjacent
   row's last value, and OR scalar reads of lanes 31 and 63.
6. Copy the color reference to host memory after the required image and buffer
   dependencies, then wait for a bounded fence.

Three independent checks passed: captured covered inputs match the final
native color values bit for bit; captured fine derivatives agree with finite
differences of the captured quad values; each GPU reduction matches a CPU OR
of every covered contribution in its complete 64-lane group. Empty quad slots
have an explicit zero coverage mask and identity contribution. They are not
guest registers guessed to contain zero.

On Intel Arc A770, Xe, Mesa 26.2.3, Vulkan 1.4, the following single samples
completed with zero mismatches:

| Square extent | Captured quads | Covered inputs | Helper inputs | Native color comparisons | Upper half changes aggregate | Capture GPU ms | Reduction GPU ms | Record bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 700 | 2,577 | 223 | 2,060 | 70 | 0.036 | 0.031 | 294,912 |
| 256 | 10,537 | 41,240 | 908 | 32,939 | 758 | 0.040 | 0.169 | 4,718,592 |
| 1024 | 165,742 | 659,366 | 3,602 | 526,826 | 10,745 | 0.583 | 2.745 | 75,497,472 |

The upper-half count makes this evidence sensitive to dropping lanes 32–63.
All three shader modules also pass `spirv-val --target-env vulkan1.4`.
Fence waits are bounded to 30 seconds and the process to 40 seconds. The
largest probe allocates about 72 MiB for its records. No validation layer was
available on this host; these results do not imply validation-layer approval.

An earlier oracle assumed that native interpolation equaled an analytic
linear plane within 1e-5. It rejected the 64-pixel case, whose measured
deviation was about 2.7e-5. No precision cause was established. The final
transport oracle instead compares the actual native color values bit for bit;
derivatives are checked against captured values. Do not change shader
precision to satisfy the discarded analytic oracle.

These are synthetic transfer timings, not a guest frame-time benchmark.
The experiment uses a dense array per primitive and dispatches empty slots.
That allocation policy is unsuitable for a real draw with many primitives.

## Verified native resolve experiment

A second version of the same experiment adds a compute-produced color,
discard bit and depth value for every captured fragment. The original geometry
is rasterized again; a resolve fragment module loads those results and applies
discard and `FragDepth` through the attachment pipeline. A separate native
fragment module calculates the same expression directly from its original
interpolants. Both paths use the same clear values, LESS depth test, depth
writes and source-alpha color blending.

The two triangles overlap, have different blue contributions and change their
depth order across the image. Both paths discard a diagonal region and another
primitive-specific region. Final color and D32 depth images compare bit for
bit with zero mismatches at 64, 256 and 1024 square extents: 4,096, 65,536 and
1,048,576 pixel comparisons respectively. The capture, helper, derivative and
complete-wave reduction checks remain passing. The additional fragment and
compute modules pass `spirv-val --target-env vulkan1.4`.

This verifies composition for that bounded synthetic input. It does not
verify guest exports, arbitrary blend modes, stencil, MRTs, samples, layers,
storage side effects, or guest discard/EXEC interactions. The larger version's
compute pass takes about 7.0 ms at 1024 square, including production of dense
color and coverage results; this reinforces the need to avoid dispatching and
writing unused slots. It is not an end-to-end guest performance result.

## Integration design

The planned execution strategy has three GPU phases. The existing native
strategy remains appropriate where its capability and semantic proof succeeds.
Selection must depend on the requested operations and enabled host features.

### Capture

Run the original vertex transformation and rasterization with a generated
fragment capture module. Export complete quads through one covered writer,
including every active input required by the guest pixel input layout:
system values, ordinary attributes, custom barycentrics, and per-vertex
parameters. Preserve integer bits and interpolation qualifiers. Capture the
native values; reconstructing helper values from neighboring covered pixels
has not been verified.

Use bounded compact records and a bounded lookup keyed by draw-local
primitive, framebuffer quad, layer and sample. Capacity arithmetic must be
checked before allocation and every append must detect overflow. A dense
`primitive_count * framebuffer_area` record allocation is excluded. A
capacity strategy must preserve ordering when it splits work; overflow cannot
silently omit fragments. Publish no unbounded cache or host readback in the
normal draw path.

Capture does not apply guest color writes, discard, blend or depth commit.
Replay of vertex execution is only admissible after excluding observable
vertex side effects or retaining its original transformed output.

### Shade

Pack sixteen complete quads into each logical wave64, preserving quad order,
coverage, helper identity and primitive ownership. Execute the decoded guest
program using both register banks and architectural SGPR, VCC and EXEC state.
The existing compute wave machinery is a reusable execution primitive, but
its compute input layout does not replace the pixel ABI.

Pixel system inputs, interpolation, image operations, implicit derivatives,
discard and exports require explicit adapters. In particular, compute memory
writes must suppress helper effects and a guest discard must update fragment
coverage while preserving helper execution where needed. Compute termination
is not a substitute for that behavior. Every widened EXEC and later inactive
register read must retain its architectural meaning.

Derivative operations need an enabled compute derivative feature and a layout
that preserves each guest quad. Both banks must select the correct captured
inputs. The target host reports compute derivative support, but that alone is
not an enabled execution strategy.

### Resolve

Rasterize the original transformed geometry in original primitive order.
Look up each fragment's computed exports and coverage. Apply the original MRT
formats, component enables, depth output, late discard, depth/stencil state
and blending through the normal attachment pipeline. Keep attachment and
buffer dependencies explicit between all three phases.

Early rejection combined with observable shader storage side effects needs
a separate proven coverage contract. Framebuffer-dependent reads, fragment
interlock, multisampling, layers and vertex side effects also need admission
rules. Unsupported cases must remain structured failures until implemented.

## Ordered completion checks

1. Preserve the verified quad transport for ordinary and custom pixel inputs,
   primitive overlap and helpers; bound record allocation and overflow.
2. Verify resolve against native execution for discard, depth/stencil,
   blending and MRT component masks before attaching the guest program.
3. Translate the complete captured pixel program with both halves observable;
   validate the complete SPIR-V module and its descriptor layout.
4. Run the first failing draw with the original inputs, capture completed GPU
   output, and recapture the next strict frontier.
5. Measure the same strict guest scene with native resolution and Silent
   logging. Synthetic transport timings cannot establish usable guest FPS.
6. Complete controls, scored gameplay and stability acceptance before any
   compatibility claim or push.
