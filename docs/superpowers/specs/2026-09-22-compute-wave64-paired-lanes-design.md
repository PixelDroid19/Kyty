# Compute wave64 through paired native lanes

Status: written design scope and modular implementation plan approved by the
user; implementation is in progress. Backend completion and gameplay acceptance
are not established by this document.

## Intent and scope

Advance strict compute execution on a host whose largest native subgroup is
32, while preserving a guest wave of 64 work-items. Keep the existing dirty
`main` worktree, unrelated changes and unpushed commits intact. Use focused
modules and small numerical regressions; do not implement a permissive opcode
skip or publish a compatibility claim from compilation alone.

This is compute-only. Graphics-stage wave lowering, ray intersection, computed
guest-address memory access and unrelated renderer repairs are separate
contracts. Correct paired lanes are a prerequisite for the current frontier,
not a promise that the entire workload will then execute.

## Verified problem

The runtime dispatch requests wave64: its raw initiator has `CS_W32_EN`, bit
15, clear. The local workgroup has 256 invocations. The host reports a maximum
native subgroup size of 32. Current vector comparisons represent a mask as a
per-invocation boolean; a numeric read of the architectural packed mask cannot
consume that boolean as though it were the complete wave.

The parsed compute program contains real shared-memory reads, writes, an
atomic and two workgroup barriers. A later per-wave branch can bypass the
missing comparison. Therefore neither splitting the guest workgroup into
independent host workgroups nor inserting a workgroup barrier at the comparison
has a correctness proof.

The host also advertises `VK_EXT_subgroup_size_control`,
`subgroupSizeControl`, `computeFullSubgroups`, and required-size support for
compute. These are physical-device observations, not features already enabled
by Kyty. The application currently requests Vulkan 1.2.

## Decision and alternatives

Use one native subgroup of 32 invocations to represent one guest wave of 64.
Each host invocation owns two guest VGPR banks. Keep one host workgroup per
guest workgroup, including its single shared-memory allocation and original
barrier relationships. Wave collectives remain subgroup operations; they do
not introduce workgroup barriers inside per-wave control flow.

Native subgroup64 is appropriate only on a device that actually supports and
enables it; it cannot solve execution on the current host. A workgroup-wide
software instruction scheduler would require a substantially broader control-
flow redesign. It is not justified merely because different guest waves take
different branches. Reconsider that architecture only if a concrete barrier
or control-flow contract cannot be represented by the paired design.

## Execution layout

Let `N` be the checked product of the original three local dimensions. The
first mode requires nonzero dimensions and `N % 64 == 0`. Partial guest waves
and partial-threadgroup dispatch controls are explicitly unsupported until
their initialization and later EXEC-write behavior are established.

- Guest waves per workgroup: `N / 64` after validation.
- Physical local shape: `(32 * (N / 64), 1, 1)` with checked arithmetic.
- Physical dispatch group counts: unchanged from the guest group counts.
- Guest wave identity: native `SubgroupId` within the workgroup.
- Two lane identities: `SubgroupLocalInvocationId` and that value plus 32.
- Logical local index for bank `b`: `64 * wave_id + lane_id + 32 * b`.
- No invented guest padding lanes are introduced in this initial mode. In
  particular, do not permanently mask a hypothetical padding lane without
  knowing what a later guest write of all ones to EXEC means for it.

For 256 guest invocations, the physical workgroup has 128 invocations and four
full native subgroups. No additional dispatches or per-wave workgroups are
created. Host workgroup counts and all logical IDs used by resource preparation
remain associated with the original guest grid.

The initial paired mode accepts group-count dispatches only. It rejects
`USE_THREAD_DIMENSIONS` and partial-threadgroup controls before any ceil
division. Validate all local dimensions and resulting dispatch group counts
against device limits before issuing work; use one admission helper for direct
and indirect paths. Do not leave the current warning-only zero-dimension check
in front of an unchecked division and expect a later layout check to protect it.
Zero group counts retain their established no-work behavior. A future extension
of thread-count dispatch must use checked normalization and independently
established partial-group semantics.

Guest local XYZ reconstruction must use a verified guest lane-order contract;
do not assume the host's physical LocalInvocationId is a guest coordinate.
The proposed X-fastest mapping must be corroborated before enabling arbitrary
multidimensional guest shapes. Special TG-size/wave-ID SGPR initialization,
partial-threadgroup controls and ordering controls must likewise be decoded or
rejected explicitly, not silently inherited from physical dimensions.

## Register and instruction contracts

### Scalar state and packed masks

SGPR values remain logically uniform within one guest wave. VCC and EXEC are
explicit two-word architectural masks, replicated across that wave's native
subgroup, not overloaded per-invocation booleans. Ordinary SGPR pairs receiving
vector comparison results have the same packed-mask semantics.

Numeric scalar reads consume both mask words as data. Lane predicates extract
the bit belonging to the relevant guest lane and bank. SCC and EXECZ are
derived from their documented scalar contracts, not a single host lane's bit.
Resources and GETPC metadata preserve their existing per-dispatch lifetime and
relocation behavior.

### Vector effects

Ordinary vector instructions operate on both VGPR banks. Sources and incoming
masks are captured before committing an instruction's destinations so aliases
cannot accidentally observe the first bank's new state. Each bank observes its
own guest EXEC bit. Inactive guest lanes produce no memory/image/
atomic effects and preserve destinations when the instruction requires it.

Comparison and carry outputs combine two 32-bit ballots into the architectural
64-bit result. Do not issue collectives inside a per-lane write guard. CMPX,
save/restore EXEC, conditional moves, numeric mask reads and scalar branches
must share this representation; supporting only the new U64 comparison would
leave conflicting mask semantics in the same shader.

Lane reads, writes, first-active-lane selection, lane counts and permutations
operate on logical lane numbers 0 through 63. A lane exchange chooses the bank
from the logical lane and the native source lane from its low five bits.
Existing scalar-spill shortcuts are reusable only with a valid reaching-
definition proof and equivalent logical lane semantics.

### Memory and control flow

Both banks address the same guest workgroup LDS object using guest addresses.
Atomics produce separate per-guest-lane results; they must not collapse into
one host-lane effect. Global resources retain their existing descriptor,
ownership and synchronization contracts.

Guest scalar branches use one wave-uniform decision computed from complete
architectural state. Different guest waves may take different branches.
Original guest workgroup barriers remain workgroup barriers at their genuine
reconvergence points. The implementation must prove the supported control-flow
shape reaches the same dynamic barrier instance across the workgroup; an
unproved shape is unsupported. A branch bypassing a later subgroup collective
does not itself invalidate an earlier, reconverged workgroup barrier.

## Vulkan selection, identity and limits

Query and explicitly enable the required subgroup-size-control features via
the extension path for the current Vulkan 1.2 application. Extension presence
alone is not sufficient. Pipeline creation must request a supported size of
32, require full subgroups, and validate compute-stage support, local-size
limits, subgroup-count limits and shared-memory limits.

Concretely, enable `subgroupSizeControl` and `computeFullSubgroups` through
`VkPhysicalDeviceSubgroupSizeControlFeaturesEXT` in device creation, chain
`VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT` with size 32 into the
compute stage, and set `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT`.
Do not set `ALLOW_VARYING_SUBGROUP_SIZE`. Physical local X must be divisible by
32 and the invocation count must not exceed
`32 * maxComputeWorkgroupSubgroups`, independently of the ordinary workgroup
limits. Derive a logical local index from subgroup ID and lane ID; Vulkan does
not promise that this is the physical `LocalInvocationIndex` value.

Keep guest and physical local shapes in distinct fields. Cache and pipeline
identity include the execution strategy, guest width, logical shape and
physical shape. Runtime addresses and dispatch counts are not shader identity.
Increment the translator identity when lowering changes so old modules cannot
bypass new semantics. Direct and indirect dispatches must converge on the same
validated execution-layout decision.

## Module boundaries

| Responsibility | Planned boundary |
| --- | --- |
| Width decode, checked guest/physical layout, capability decision | `ShaderComputeWaveLayout.h/.cpp` |
| Typed wave/mask analysis and instruction eligibility | `ShaderComputeWaveAnalysis.h/.cpp` |
| Two VGPR banks, logical IDs and compute prolog | `ShaderSpirvComputeWave.cpp` |
| Packed mask production, consumption and EXEC/SCC handling | `ShaderSpirvWaveMask.cpp` |
| Logical lane exchanges and first-active-lane operations | `ShaderSpirvWaveLane.cpp` |
| Existing opcode integration and shared lowering contracts | Focused edits in existing emitter modules |
| Vulkan feature enablement and pipeline state | Existing window/context/pipeline boundaries |
| Numerical GPU evidence | Small wave-focused integration modules beside existing compute probes |

Do not copy entire existing emitters into a second backend or use global text
substitution to reinterpret register semantics. Share arithmetic and memory
lowering through explicit lane/register interfaces. Do not turn the existing
large graphics-packet test file into the wave-emulation test suite.

## Evidence gates and validation

1. Verify guest lane ordering, dispatch controls and Vulkan full-subgroup
   requirements before enabling the layout for a private workload.
2. A small pure layout test covers zero dimensions, overflow, supported 64/256
   logical invocations, rejection of partial 1/32/33/65-invocation wave64
   shapes, thread-count/partial-dispatch rejection before conversion, zero
   dispatch groups, per-axis group-count limits and local host-limit rejection.
3. Numerical GPU cases exercise a predicate true only at lane 63, both mask
   halves, all-zero masks, partial EXEC and source/destination preservation.
   Read/write-lane cases cross the 31/32 boundary and include lane 63.
4. A two-wave kernel diverges after a real shared-memory barrier and performs
   subgroup-only collectives; results must not deadlock or mix waves. A
   separate shared atomic/read/write case verifies one LDS object per original
   workgroup. Invalid barrier shapes fail before pipeline submission.
5. Test changed contracts red-to-green through production lowering and real
   Vulkan execution. Retain the existing scalar/GETPC numeric cases and focused
   shader, graphics-state and packet regressions. Build with two jobs, then run
   the existing boundary/table gates and independent review.
6. Only after these gates, launch the original strict workload through native
   diagnostics. Preserve the first failure and compare it with the recorded
   frontier. Later missing memory/ray/shader contracts remain separate work.
7. Gameplay requires actual controlled progression and the existing capture/
   playable-regression gates, not a window, compilation or isolated GPU probes.

## Explicit unresolved contracts

The observed U64 comparison aliases its VCC source and destination. RDNA2 ISA
section 6.2.4 warns that wave64 VALU read/write of the same SGPR can be
unpredictable. Paired banks do not define that missing hardware contract.
Resolve it with stronger primary or hardware evidence, or reject the aliased
form explicitly. Instruction-level full VCC writes do not prove how each
physical half-pass writes the register. Do not choose a convenient result to
advance startup.

The retained parsed CFG shows that EXEC-dependent branches before the original
two barriers rejoin before them; the later comparison-bypassing branch is after
both. The only earlier exit is based on a scalar-buffer load indexed by
workgroup-uniform SGPR inputs. A new metadata capture confirms that its group
ID starts at SGPR16, with only group X enabled, two thread-ID components and
1664 LDS dwords. Convergence is supported under stable, race-free scalar input;
that is not an independent proof excluding concurrent resource aliasing.

Multidimensional lane ordering still requires its own evidence. The design
does not assume every previously accepted shader
already has correct EXEC, lane-exchange or barrier lowering. New unsupported
forms must report the contract they need. Increased register pressure is a
performance risk to measure after correctness, not grounds for skipping a bank.

There is now production-driver corroboration for regular GFX10.3 linear
dispatch: Mesa's AC lowerer derives a local index from wave ID and `mbcnt`,
and RADV's TID optimization uses an X-fastest conversion to XYZ. The same code
distinguishes quad-derivative dispatch and newer interleave controls. This is
one shared driver implementation, not two independent proofs or a captured
guest-hardware trace. Verify that the guest dispatch uses the corresponding
ordinary layout before promoting the mapping to admitted runtime state.

A later bounded entry-to-failure join confirms the failing dispatch has raw
local dimensions `(16,16,1)` with zero partial-count halves and `TG_SIZE_EN=0`.
The two initialized VGPR ID components, X-only group ID and user-SGPR count
match the preceding input capture. This closes the optional TG-size initial
state question for that dispatch. RADV's derivative-group quad transformation
is compiler-side, not a separate GFX10.3 hardware initial-ID control; do not
require unavailable derivative metadata to initialize ordinary guest VGPR IDs.
Admission still validates these actual controls rather than treating every
Gen5 dispatch as ordinary. The driver corroboration is not a silicon trace.

## Sources and review status

- [AMD RDNA2 ISA](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture): initial compute state, masks and wave64 alias restrictions.
- [Linux GFX10.3 registers](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h): compute width bit.
- [LLVM PAL wave-size regression](https://github.com/llvm/llvm-project/blob/main/llvm/test/CodeGen/AMDGPU/mixed_wave32_wave64.ll): independent primary width-bit corroboration.
- [Vulkan subgroup local invocation ID](https://docs.vulkan.org/refpages/latest/refpages/source/SubgroupLocalInvocationId.html): full-subgroup mapping guarantees.
- [Vulkan pipeline stage requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineShaderStageCreateInfo.html): explicit subgroup size, feature and full-subgroup constraints.
- [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html): collective and control-barrier requirements.
- [Mesa AC local-index lowering](https://gitlab.freedesktop.org/mesa/mesa/-/blob/6ac7466d40be92bfec4fe3dfe21376a6af45a8e0/src/amd/common/nir/ac_nir_lower_intrinsics_to_args.c): production wave-ID/lane-rank computation.
- [RADV TID optimization](https://gitlab.freedesktop.org/mesa/mesa/-/blob/6ac7466d40be92bfec4fe3dfe21376a6af45a8e0/src/amd/vulkan/nir/radv_nir_opt_tid_function.c): linear XYZ mapping and its qualifications.

Independent read-only reviews rejected the added-workgroup-barrier and
independent-wave-workgroup shortcuts. A scheduler-necessity claim was withdrawn
after separating original guest barriers from proposed emulation barriers.
Written-spec review and a separate implementation plan precede product edits.
