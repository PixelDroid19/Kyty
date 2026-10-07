# Fragment execution across host subgroups

## Status

The current renderer cannot execute a guest fragment wave wider than the
host subgroup when the program observes values from both halves. The current
strict compiler stop is a full-mask OR row shift, followed by a cross-row
permutation and scalar reads of lanes 31 and 63. An explicit paired compute
compiler now translates the complete captured pixel input; the renderer has
not yet selected or executed that strategy.

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

## Native fragment-wave tier

Architectural guest width, physical subgroup width and helper participation
are separate obligations. The integrated admission metadata retains the guest
width even when a narrower host mapping has a proof. EXEC, VCC and saved masks
remain packed numeric words; vector write predicates are derived separately.

- `ShaderAnalyzeNativeWave` examines the whole program, including numeric mask
  escapes, scalar branches and SGPR mask propagation across back edges.
  Lane-local and quad-local proofs can retain native execution on a narrower
  complete-quad host subgroup. An advertised size range alone is insufficient.
- `ShaderAnalyzeFragmentNativeWaveTier` remains a neutral-value analysis. Its
  `FragmentNeutral32` result does not prove that ordinary subgroup shuffles can
  fetch an in-range helper or unavailable invocation. Row DPP, PERMLANE and
  non-spill READLANE retain an explicit participation refusal until that
  independent obligation is implemented.
- Initial-EXEC READFIRSTLANE has a narrower represented-target-quad proof.
  Scalar EXEC writes, exports and unproven control flow invalidate it; numeric
  mask observations still require their own participation proof.
- VS/PS pipeline identity includes guest width, proof and preferred size.
  Fresh and cached modules are checked for subgroup operations, stage support,
  enabled dynamic broadcast, quad-stage support and maximal reconvergence.
  Required-size nodes preserve other stage `pNext` nodes and never apply the
  compute-only full-subgroups flag to graphics stages.

Earlier driver acceptance and neutral-region controls did not establish
whole-program native wave64-on32 correctness. Current CPU admission controls
pass, but integrated runtime and cross-device verification remain pending.
Validation stays required for the strict runtime work; older unvalidated
control-flow observations are historical evidence, not acceptance.

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

## Verified banked row execution

The paired compute backend now emits DPP moves and bitwise operations with
architectural source EXEC, destination row/bank masks, fetch-inactive and
bound-control handling. It also emits `PERMLANE16` and `PERMLANEX16` with
their two-word selector table and source EXEC. Both banks' sources are loaded
before either destination bank is stored, preserving aliased operands.
The admitted DPP controls are quad permutation, row shifts, row rotation,
row mirror and half-row mirror. Other tuples remain rejected.

An original GPU integration replay uses the production compiler's output and
compares every retained vector register plus scalar reads of lanes 31 and 63
against an independent ISA reference. Eighteen modules and eight EXEC masks
per module pass on the current host: 144 cases, 18,864 register observations,
zero mismatches. There are 138 cases in which the upper half changes the
aggregate. The matrix includes both empty destination masks, inactive source
lanes, partial destination masks, source/destination aliasing, both FI values
and both BC values. All modules validate with the Vulkan 1.4 target.

Admission also rejects eight malformed tuples. Six previously escaped to a
generic emitter or ignored unused operands. Unadmitted bitwise DPP tuples
now fail before generic lowering; source counts, unused sources, destination
modifiers and DS metadata are checked explicitly.

Full destination masks are folded at translation time instead of generating
row and quarter-bank indexing. The complete synthetic reduction module
changes from 834 to 746 SPIR-V operations with identical GPU results. These
counts include its observation code and declarations; they do not establish
driver compile-time, memory or guest frame-rate improvements.

This implements shared execution primitives. It does not enable a compute
fragment pipeline or advance the strict fragment-emitter frontier. A first
strict integration attempt terminates before any present because the host
kernel invokes its global OOM killer during pipeline compilation. The service
reports a 6.8 GiB memory peak, while the kernel reports exhausted host RAM and
swap; this is not evidence of a shader semantic failure or a Xe timeout.
The integration comparison therefore remains pending.

A second attempt reuses the completed pipelines but reaches the 6 GiB
service memory limit while compiling the next pipeline. The kernel attributes
this termination to the service's memory cgroup, not a GPU reset. Neither
attempt reaches a present; neither supplies a new frame-rate result.

## Verified signed subword conversion

The captured pixel IR contains twenty-four `VCvtF32I32` operations selecting
one SDWA word with `SRC0_SEXT`. The unsigned operand loader positioned the
selected bits correctly but did not extend their sign. Conversion now extends
the selected byte, word or dword before converting the signed integer to
FP32. For example, selected word `0x8000` converts from `-32768`. Admission
requires the exact plain VGPR source, full destination and modifier tuple;
unsupported encodings remain rejected. The ISA contract is section 13.3.7.

The decoded signed tuple failed paired admission before the correction.
Seven additional GPU replay modules cover all valid source selections with
positive and negative values and independent lower/upper EXEC masks. The
combined row, permutation and conversion replay passes 200 cases and 26,200
register observations, with zero differences; all modules validate for
Vulkan 1.4. Inactive destinations retain their original bits. Strict guest
integration remains pending because of the pipeline-compilation memory limit.

## Verified whole-quad masks

Paired execution now admits the exact scalar-pair `SWqmB64` tuple and
expands each nonempty four-bit group in both words. The native fragment
`exec, exec` no-op is not used when registers hold an architectural mask.
The existing scalar emitter reads both source words before either store and
updates SCC from the resulting pair. Section 12.3 defines these effects.

A decoded `exec, exec` replay failed paired admission before the correction.
Six GPU modules now cover EXEC, aligned SGPR pairs and VCC, including
source/destination aliasing and distinct sources. Independent ISA references
check both result words, SCC and subsequent masked writes. Combined with
the preceding replay, 248 cases and 32,632 observations pass with zero
differences and valid Vulkan 1.4 modules. This is compiler contract evidence;
the compute fragment pipeline is still pending.

## Verified image destination preservation

The paired generic emitter previously applied image destination guards after
renaming native registers to their bank names. The guard recognized only
native VGPR names, so samples could overwrite inactive destinations. The
generic emitter now guards the native snippet before rewriting it. Rewriting
then substitutes each bank's EXEC bit into the guard; the native path keeps
its existing guard placement.

Three GPU replay modules sample a real RGBA32F image with component masks
1, 2 and 8 and independent lower/upper EXEC masks. The old placement produces
981 mismatches, including every destination under empty EXEC. After correction,
the combined replay passes 272 cases and 35,776 observations with zero
differences. All modules validate for Vulkan 1.4. This proves register
preservation; no guest capture has attributed a visual symptom to this defect.

The final combined replay adds a four-iteration row operation controlled by
uniform scalar branches. The unchanged production output passes 280 cases
and 36,824 observations across 35 modules, with zero differences. Boundary
and graphics-table provenance gates also pass; unit tests remain deferred
until the requested gameplay checkpoint.

## Explicit fragment compiler

`SpirvGenerateFragmentComputeSource` retains the guest Pixel stage and its
bindings while choosing a host Compute scaffold. The caller supplies separate
allocated, initial EXEC and coverage masks, raw initial VGPR words and sixteen
words per attribute: four native interpolants and twelve P10/P20/P0 parameter
words. An optional wave header supplies the raw parameter-state SGPR located
after the user SGPRs (ISA section 3.12.2). It and the parameter triples must
describe the same parameter cache. M0 initialization must stay in the entry
prefix, and later copies cannot read an overwritten system register. Reusing
the SGPR after copying it to M0 is allowed.

Ordinary P1/P2 pairs use the native final attribute only when the intermediate
destination is unobserved, execution stays unchanged and I/J retain their
initial native values until each instruction consumes them. P1 may overwrite
I after consuming it; requiring I to stay unchanged through P2 incorrectly
rejected a captured pair. Other arithmetic interpolation remains unsupported.
Parameter moves load the actual captured raw words. Each output lane has
34 words: pixel validity, an MRT component mask and 32 raw color components.

EXP decoding preserves VM, DONE and COMPR instead of relying on coarse format
names. VM changes validity without requiring color data; an export with VM=0
preserves the last validity update. Every reachable termination path must have
VM and DONE exports. Compressed channel enables admit only complete pairs.
Packed-half shadows are distinct for both banks and are refreshed from the
retained VGPR before a masked conversion. A later EXEC restore can export
lanes that did not convert; the original replay had 160 zero-value differences
without this refresh, and none after it. Storage writes, atomics,
LDS, barriers and unsupported exports remain rejected. Multiple exits flush
once through the shared dispatcher exit; no helper invocation is killed.

The complete private pixel input contains 2,300 instructions and generates
286,670 SPIR-V words with a Compute entry, linear derivative groups and no
OpKill. The module assembles and validates for Vulkan 1.4. An original GPU
replay passes 19 modules, 160 cases and 356,862 output-word comparisons with
zero differences. It covers distinct bank inputs, helper allocation and
coverage, WQM, parameter state, partial color pairs, VM updates, multiple
terminal blocks, two waves, excess dispatch groups and undersized buffers.
Fifteen malformed cases fail admission; later SGPR reuse is a passing control.

The [Vulkan quad contract](https://docs.vulkan.org/spec/latest/chapters/shaders.html)
places each compute derivative quad in one subgroup with four consecutive,
four-aligned subgroup indices. The generated bank layout preserves those
quads. Execution requires enabled linear compute derivatives and full
physical subgroups of 32. The interface does not enable those host features
or select a renderer path. Real compact capture and native resolve remain
necessary before guest execution or performance can be accepted.

### Shared floating DPP source mask

The generic bank wrapper now supplies the complete architectural EXEC word
to quad DPP source loads. FI=0 selects zero for an inactive source before
applying ALU modifiers, while destination writes retain their separate lane
mask. Five original `VSubF32` modules with the captured quad controls exposed
216 GPU differences before the correction. They now agree with the independent
ISA reference under all eight EXEC masks. The combined replay passes 320
cases and 42,064 observations across 40 modules, with zero differences and
valid Vulkan 1.4 modules. This corrects shared source-mask behavior; no guest
visual cause or frame-rate improvement is attributed to the replay.

## Compiler memory experiment

A pipeline-only Vulkan 1.4 replay isolates the large compute module from
guest memory and dispatches no GPU work. Under a 4 GiB service limit, the
original module reaches a cgroup OOM termination after about three minutes.
Applying only the SPIRV-Tools local single-block load/store elimination pass
reduces its operations from 77,860 to 70,865 and loads from 18,191 to 11,980.
Both modules validate, and the independent 280-case GPU replay remains exact
after that pass. The module still reaches the same 4 GiB OOM limit after
about three minutes. The hypothesis that these redundant loads were enough
to remove the compilation memory barrier is disproven.

No optimizer policy change is shipped from this experiment and no guest
frame-time improvement is claimed. Further compiler-memory work needs a
measured cause beyond the number of local loads; strict runtime comparison
also needs sufficient host memory for guest state and driver compilation.

## Integration design

### Primitive identity through interpolation geometry

The generated interpolation geometry stage now copies its input
`PrimitiveId` to every emitted vertex. Its output-component limits include
that additional integer. The geometry translation version advances separately
from vertex, pixel and compute translations, preserving their warm entries.

The public capture replay previously received primitive 1,044,885,012 from a
two-primitive draw and failed before recording a quad. The geometry stage had
not written the builtin. Vulkan requires that write whenever the following
fragment stage reads `PrimitiveId`; see the
[builtin contract](https://docs.vulkan.org/refpages/latest/refpages/source/PrimitiveId.html).
After the correction, the same draw captures both primitive owners. Six GPU
cases at 64, 128 and 256 pixels per edge pass with two driver builds, preserving
coverage, helpers, raw parameter bits and input-to-wave mapping. The comparison
uses the device's subpixel precision for mathematical interpolation checks;
transported words and raw parameter triples are compared bit for bit.

This is a geometry contract fix. A strict Silent/Native guard run still reaches
the existing fragment DPP rejection. It does not establish scene rendering or
a frame-rate improvement. The cache regression case is compiled; execution of
the unit suite remains deferred until gameplay, as requested.

The planned execution strategy has three GPU phases. The existing native
strategy remains appropriate where its capability and semantic proof succeeds.
Selection must depend on the requested operations and enabled host features.

### Compact host transport modules

The host now has bounded production modules for capture, wave scan, packing
and ordered native resolve. The aggregate transient budget is 128 MiB, with
checked 64-bit allocation arithmetic, device limits, saturating counters and
bounded hash probes. Each primitive can own a partial wave; capacity includes
that case. Record-to-output references preserve fragment identity independently
of the atomic record order. Transport padding represents unobserved slots,
not fabricated guest inputs.

Capture preserves the decoded system values, active ordinary attributes and
raw per-vertex triples. P1/P2 admission derives center, centroid and perspective
qualifiers from the actual barycentric source registers; conflicting evaluation
modes for one captured attribute are rejected. Four qualifier modules validate
for Vulkan 1.4 and a conflicting pair is rejected before assembly.

The public GPU pack replay passes 26 cases across two driver builds, with
3,178,036 word observations and no differences. The production capture and
geometry replay passes six cases and 11,923,224 observations, including raw
parameter bits, helper values and both primitive owners. Production resolve
passes fourteen cases against an independent native fragment implementation:
MRT colors, discard, blend, fixed depth, stencil and component write masks
compare bit for bit. Capacity failure and missing export components produce
error flags and preserve clear attachments in the tested cases.

These modules are not yet selected by a native draw. Their error flags require
host completion checks before accepting output. Native resource binding,
ordered tiling, command-buffer lifetime and capability admission remain
integration work. The compute color producer in the resolve fixture is
synthetic; it is not evidence of guest gameplay or guest frame rate.

### Partial-wave reduction (2026-10-01)

`ShaderSpirvGenerator.cpp` clamps EXEC to the capture allocation bitmap after
every instruction that writes it. A public fragment program saves its current
mask, explicitly widens EXEC, initializes unused lanes to the OR identity,
reduces both rows and reads lanes 31 and 63. The clamp prevented that
initialization in uncaptured lanes, so retained values reached the scalar
reduction instead of the active contributions. Before the correction, six
partial-quad counts fail with 132 output differences on both driver builds;
the complete sixteen-quad control passes. After it, quad counts 1, 4, 7, 8,
9, 15 and 16 pass on both builds with zero differences, and the module
validates for Vulkan 1.4. Do not replace missing rasterizer inputs with zero
to conceal this failure, and do not remove the clamp globally: compute and
every unproven fragment program keep it.

ISA section 6.9 requires full EXEC and neutral unused lanes for a DPP scan.
`ShaderFragmentNeutralRegion.cpp` lets only the widening
`s_orn2_saveexec_b64` skip the clamp, and only for a closed region it proves:

- the saved mask is a copy of EXEC (or a restore from such a copy) with no
  intervening write to the mask or to EXEC, label or transfer, so it is a
  subset of the captured allocation;
- the next instruction is a `v_cndmask_b32` that gives zero to every lane
  outside a condition that is that mask or an `s_and_b64` of it, with no
  alias between the saved pair and the condition;
- every later instruction is a single-VGPR bitwise operation or PERMLANE
  whose sources are already defined in all 64 lanes. A DPP destination that
  keeps its old value must be defined too. SDWA, `op_sel`, `omod`, labels,
  branches and any scalar, memory or export effect end the proof;
- the region ends by restoring EXEC from the saved pair.

The first draft indexed a 256-entry table with the unvalidated destination of
the initializer and admitted bitwise instructions carrying SDWA, `op_sel`
and `omod`, whose partial writes keep undefined bits. Both are fixed. Twelve
single-condition mutants of the analysis, each removing one check, are all
killed by the focused cases (the first run left two alive, which exposed the
missing non-DPP SDWA/`op_sel`/`omod` cases now covered). The same case source
ran without the unit-test framework against the production analysis: 14
cases, 67 checks, zero failures. The complete captured pixel program has one
region, recognized from its instructions rather than a hash or address. Its
paired module generates, assembles and validates for Vulkan 1.4, with exactly
one clamp fewer than before. The 19-module, 160-case fragment replay still
passes on both drivers with 356,862 observations and zero differences.

### Reads that can observe uncaptured lanes

`v_readlane` ignores EXEC, and DPP or PERMLANE with fetch-inactive set read
inactive lanes (ISA 12.12, 13.3.9). With fetch-inactive clear, an inactive
source reads as unavailable, which matches hardware where an absent lane has
EXEC clear. Quad-local DPP never leaves a captured quad. Paired fragment
admission now rejects every other such read, with its PC, unless a proven
region wrote the source register in all lanes and is the unconditional
predecessor of the read. Later narrower writes keep the other lanes, as on
hardware. An unadmitted region keeps the clamp, which is the 132-difference
result above, and previously nothing reported it. The public variant whose
condition is not derived from the captured mask is not admitted, and now stops
with `reads lanes outside the captured wave without a proven full-wave
initialization` at the lane read and emits no module.

This is deliberately conservative. A read separated from its region by a label
is rejected, and a set-inactive idiom that inverts EXEC instead of widening it
is not recognized. Generalize to a control-flow dataflow only when a rejected
real program shows the need. `v_readfirstlane` with EXEC empty reads lane 0
(ISA VOP1 table); whether lane 0 is captured depends on the packing policy and
is not analyzed. The native transport strategy remains unselected.

### Selection and renderer hook points (2026-10-01)

`FragmentTransportAdmission` selects the strategy only for pixel programs with
a DPP row control or PERMLANE and names the first host capability it lacks
(geometry shader, fragment quad operations, device addresses, enabled linear
derivatives, enabled subgroup size control and full subgroups, subgroup 32).
Quad-local DPP and ordinary programs stay native. The renderer does not call
it yet. The renderer code that a connection must change, as read from the
current tree:

- `PipelineCache::CreatePipeline` (graphics) parses and recompiles the guest
  pixel stage on a cache miss, which is where an unsupported native emission
  stops a strict run. The pipeline key is the render pass, the pixel and
  vertex shader identities and the static state, so a strategy needs its own
  identity bits. The interpolation geometry stage is already created there
  when custom interpolation is enabled, and capture needs the same stage.
- `GraphicsRenderDrawIndex` and `GraphicsRenderDrawIndexAuto` bind the
  pipeline, dynamic state, vertex and index buffers and both descriptor sets,
  then record one render pass. A transported draw replaces that sequence with
  capture (attachment-free pass), scan, pack, shade, then the resolve draw in
  the guest framebuffer pass, with explicit buffer barriers between phases.
- Graphics pipelines hold at most three descriptor sets, and `CreateLayout`
  requires the stage's slot to equal the running layout count. The vertex
  clip probe already appends a third draw-scoped set, which is the precedent
  for the transport set at index 2. The compute pipeline builder supports one
  descriptor set, so shade needs its own layout: empty set for an unused slot,
  the pixel resources (compute stage) at their slot, the transport set at 2.
  Capture, resolve, scan and pack read 28 bytes of push constants.
- `TransientBufferPool` is host-visible, capped at 16 MiB and may return one
  buffer for several equal requests, so it cannot hold the five transport
  buffers. A per-command-buffer device-local arena is needed, reset when the
  command buffer starts and completed through the same fence hook that
  `VertexClipProbeRenderer::Complete` uses. Error words are copied to a
  host-visible slot and consumed after that fence.

### First transported draw observed in a strict run (2026-10-01)

Pipeline creation now stops, before native compilation, when a pixel stage
needs the strategy, and prints the facts a connection needs. A strict Silent,
Native run (Vulkan 1.4, Intel Arc A770) reached the title level after 176 s
and stopped there with: every host capability enabled and available, virtual
parameter state and partial-wave reads admitted for the real program and
runtime inputs, 89 words per lane (8 initial VGPRs, 5 interpolants, 30 user
SGPRs), and the capture, shade and resolve modules generated and assembled
in-process (3,420, 285,305 and 2,017 words). This closes the question of
whether the real program passes admission with live inputs.

The draw is 1920x1080 with three color targets (write masks 7 and 3 on the
first two), blending on target 0, no depth attachment and one sample. That is a
full-screen pass, so it needs about 518,000 captured quads. The layout module's
128 MiB budget allows 30,215 quads for 1,000 primitives and 4,046 for 10,000
or 30,000, because every primitive may own a partial wave and the worst case is
allocated densely (about 33 KiB per quad when each quad is its own wave, about
3.5 KiB when a few primitives own full waves). Consequences for the connection:

- Whole-draw capture does not fit. Screen tiles are needed (about 350x350
  pixels for a few primitives), or the budget policy must change for this case.
- The resolve draw must run in the guest framebuffer pass, once per tile. A
  color load operation is CLEAR only while the tracked image layout is
  UNDEFINED or SHADER_READ_ONLY (`ResolveColorAttachmentLoadOps`), and
  `CommandBuffer::BeginRenderPass` then records COLOR_ATTACHMENT_OPTIMAL. Asking
  the framebuffer cache again for each later tile therefore yields a LOAD pass,
  compatible with the pipeline built for the first one; reusing the first
  framebuffer object for every tile would clear the earlier tiles.
- The captured parameter-state header has no producer: the capture module never
  writes the per-primitive control word and the host has no value for the guest
  parameter cache pointer. Only the virtualized form is usable, and it is
  admitted for this program.
- Capture, shade and resolve must derive `lane_words` from the same resolved
  interpolant count; `SpirvResolvePixelParameterCount` is now shared for that.

### Enabled compute derivatives

Device discovery queries `computeDerivativeGroupLinear` only when the ratified
KHR extension is advertised. Device creation explicitly enables that feature
and the extension together; the graphics context records queried support and
successful enablement separately. Hosts lacking the feature retain a false
capability. Vulkan 1.4 remains required. The
[feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR.html)
requires this device feature chain before using the generated derivative mode.

A strict Silent/Native guard with the feature enabled reaches the unchanged
fragment DPP rejection without exhausting host memory. A separate pipeline-only
compile of the complete current pixel module with the local driver build takes
13.77 seconds and peaks at 619,716 KiB RSS. It dispatches no GPU work. This
removes the observed compile barrier for that exact module and driver build;
it does not establish execution cost, guest frame rate or native draw selection.

### Virtual interpolation parameter state

The optional virtual parameter mode represents the initial parameter-cache
pointer as a normalized selector into a captured primitive-owned block. It
removes the raw wave header only after proving that the initial SGPR reaches
M0 alone and M0 never becomes ordinary guest data. Subsequent reuse of that
SGPR is allowed after a complete unconditional entry-prefix overwrite. Branches
and indirect control transfers end that prefix. Other flows remain rejected;
the default raw captured-header contract is unchanged.

A public production-compiler replay compares raw headers with four distinct
bit patterns against the normalized headerless module. Both banks, unallocated
lanes, helpers, coverage and two MRT exports agree in 21,760 output-word checks
across two driver builds. An independent expected-output comparison passes.
The raw-header module leaves all 2,176 output words untouched when given the
headerless packet; the admitted normalized module executes it. Six unsafe
parameter flows are rejected, including reads before overwrite, conditional
and partial overwrites, explicit M0 data reads and indirect jumps. Both modules
and the complete private pixel module validate for Vulkan 1.4. No runtime
strategy or performance claim follows from these compiler checks.

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
