# GPU-indexed descriptor tables and ray-tracing dispatch: design

Status: design only. No code. Every implementation stage is gated by a recorded
Kyty strict run (section 7). Section 5.3 is incomplete: the table path is not
landable until its device-address store check has a source of truth (section 7,
stage 3).

Scope: descriptors (T# images and S# samplers) that the shader selects at run
time from a guest table, which the translator does not represent today, and what
the ray-tracing BVH instruction needs beyond its present, partial lowering.

Citation convention: ISA references name the section or table of the document
titled "RDNA2 Shader ISA". Code references name the repository-relative file and
the function or constant, with the line number at commit `b24ae77ab`. Repository paths start at the repository root. A file name given without
a directory is in `source/emulator/src/Graphics/`. Line numbers are checked again
when the stage that touches the line is implemented.

## Decision summary

- A table access is a scalar load of a descriptor from an SGPR-offset address.
  The element size `E` is the number of bytes that the feeding S_LOAD reads for
  the descriptor: 32 bytes for a 256-bit T#, 16 bytes for an S#. The element
  index is `a / E`, where `a` is the byte offset. No element stride is assumed,
  so interleaved guest layouts need no special case.
- Every element has a cause word per class, computed at snapshot time. Zero
  means valid. A nonzero word names why the element cannot be used. Snapshot
  never fails for one element. Element problems become cause bits, and only an
  access that reaches a nonzero cause faults.
- An index at or beyond the capacity `C` is defined by a translator-emitted guard
  `index < C`, where `C` is a specialization constant. The guard reads no
  descriptor and no cause word, sets the IN_RANGE cause bit, and returns zero. There
  is no clamp, no placeholder descriptor and no scan window (section 5.1).
- A table is a bind record with its own descriptor set layout, its own
  descriptor set in the pipeline layout and its own pool. It is not an extra
  `desc[]` slot and it does not share the general pool.
- Table arrays use `PARTIALLY_BOUND`. An element whose cause is nonzero is never
  written and never read, because the guard checks the cause word first.
- A table snapshot is an immutable descriptor set, written before first use and
  never updated after bind. `UPDATE_AFTER_BIND` is not used.
- Table elements are read only through the chunked, validated capture of
  section 3.6. A bytes-not-captured element gets the UNMAPPED cause.
- Divergent indices are decorated `NonUniform`. The guest waterfall loop is
  translated as written.
- GPU-written tables are detected and refused. The four ordering checks of section
  5.3 run before every snapshot. A pending producer of any table byte is an explicit
  refusal of the draw or dispatch, which stops the run with an explicit EXIT. No stale
  snapshot is taken, no retry is made and the table is not resolved on the GPU. A
  title that writes a table on the GPU before reading it is therefore unsupported
  until the maintainers choose the deferral alternative in section 10.11. Check 4
  (device-address stores) has no source of truth yet, so the table path is not
  landable until stage 2 adds it (section 5.3).
- Implicit-LOD table samples are rewritten to an explicit-gradient sample in the
  fragment stage. Bias samples and implicit-LOD samples outside the fragment stage
  are refused at translation (section 5.2).
- Fault bits are not stored in the immutable table set. They go to a fault ring
  slot owned by one recording, zeroed when the recording starts and read at that
  submission's completion. Each slot sits at a stride that keeps its dynamic offset
  a multiple of `minStorageBufferOffsetAlignment` (section 3.5).
- Capacity `C` is computed per pipeline layout from the Vulkan limit fields named
  in section 3.4. A policy cap of 4096 applies until stage 3 measures the bind cost.
- Table elements do not use the zero-read convention of non-table loads. Section
  5.4 gives the reason.
- Translator output changes at stage 2, so `kShaderTranslatorVersion` is bumped
  in that same change (section 3.7). This document does not bump it.

## 1. Problem and ISA contract

### 1.1 Descriptors are scalar operands

- The resource operand of an image instruction is an SGPR quad or octet. Section
  8.2.6 says the T# is "in four or eight consecutive SGPRs", and section 13.7.1
  gives the size selector R128 (1 = 128-bit, 0 = 256-bit). Section 8.2.6
  describes the same resource.
- A descriptor is therefore a wave-uniform scalar value in the encoding. The
  hardware has no per-lane T# operand.
- The size of a T# is fixed by the instruction, not by the element. The element
  size used by a table is therefore the size the feeding load reads (section 3.2).

### 1.2 Tables are read with scalar loads

- A guest table is read with S_LOAD. Section 7.2.1 ("Scalar Memory Addressing")
  gives `ADDR = SGPR[base] + inst_offset + { M0 | SGPR[offset] | 0 }`, in bytes,
  with the two LSBs ignored.
- The same section says that any "index * stride" must be calculated manually in
  shader code and added to the offset before the load. The stride is therefore a
  property of the guest code, not of the ISA.
- A raw S_LOAD has no bounds. Section 7.2.1 says buffer constants add bounds
  checking through `num_records` and `stride`; this applies to buffer-constant
  reads, not to a raw S_LOAD. The table extent is therefore not known from the
  ISA. The capacity in section 3.4 is a host value.
- S_BUFFER_LOAD reads through a buffer constant, so its bound is a V# field.
  This design does not admit S_BUFFER_LOAD as a table source (section 5.2).

### 1.3 Divergent indices are guest waterfall loops

- Because the descriptor is an SGPR, a divergent index cannot be expressed
  directly. The guest uses a waterfall loop. The idiom is in section 12.3, in the
  S_ANDN2_WREXEC_B64 entry: `v_readfirstlane_b32`, `v_cmpx_eq` on the index, the
  operation under the narrowed EXEC, then `s_andn2_wrexec_b64`, repeated until EXEC
  is zero. The S_ANDN1/S_ANDN2 WREXEC entries say they are "intended to accelerate
  waterfalling".
- The hardware does no per-lane descriptor selection. The guest loop is the
  contract that defines the result.
- Translation of the loop: `V_READFIRSTLANE_B32` is lowered against the packed
  guest EXEC (`source/emulator/src/Graphics/ShaderSpirvVector.cpp:3539`,
  `Recompile_VReadfirstlaneB32_SVdstSVsrc0`). `source/emulator/src/Graphics/ShaderWaterfall.cpp`
  recognizes only the relative-move waterfall (`MatchWaterfall` at line 179,
  `ShaderLowerWaterfallMoves` at line 289). It does not recognize an
  image-operation waterfall. This design does not detect or collapse guest
  waterfalls. Stage 1 confirms that the translated loop selects the right T# in
  each iteration.

### 1.4 NonUniform

- Host legality: an index into a descriptor array that is not known to be
  dynamically uniform must be decorated `NonUniform`.
- Decision: every table access is decorated `NonUniform`. A waterfall index is
  constant across the active lanes of one iteration but differs between
  iterations and between subgroups, so it cannot be assumed uniform over the
  invocation group. The exact decoration rule text of the Vulkan descriptor
  indexing section was not fetched; it is confirmed at stage 3 (open item 10.1).
- The bundled SPIR-V header gives the values used below: decoration `NonUniform`
  is 5300, capability `ShaderNonUniform` is 5301, and capability
  `SampledImageArrayDynamicIndexing` is 29
  (`source/3rdparty/vulkan/include/spirv-headers/spirv.h`). The header records no spec
  revision for the rule text, so no SPIR-V text is quoted in this document.
- Current translator state: no `NonUniform` decoration and no `ShaderNonUniform`
  capability are emitted. The "NonUniform" matches in `ShaderSpirv*.cpp` are
  `OpGroupNonUniform*` subgroup operations, which are unrelated to descriptor
  indexing.

## 2. Current base

### 2.1 Dynamic S_LOAD mapping accepts only constant offsets

- `source/emulator/src/Graphics/ShaderResources.cpp:274` (`ShaderTryGetDwordOffset`) accepts only
  an inline integer or a literal constant, dword-aligned and not above `INT_MAX`.
- `source/emulator/src/Graphics/ShaderResources.cpp:289` (`ShaderGetSmemConstantDwordOffset`)
  adds the instruction's immediate offset. An S_LOAD whose offset is an SGPR fails
  this test. The caller at `source/emulator/src/Graphics/ShaderResources.cpp:415` takes the constant path only
  when the test succeeds.
- The same file already matches S_LOAD definitions by destination width and EUD
  pointer register (`source/emulator/src/Graphics/ShaderResources.cpp:401-420`). The
  loop calls `ShaderMatchesEudPointer` at line 410, and that function is defined at
  line 309. The match takes a destination of 4 SGPRs (`SLoadDwordx4`) or 8 SGPRs, fed
  from the pointer register. This pattern is the base for the srsrc rule of section 3.2.
- A texture reached from a constant-offset S_LOAD is created with
  `dynamic_sload = true` in `ShaderAddDynamicTextureResource`
  (`source/emulator/src/Graphics/ShaderResources.cpp:717`), and the mapping is recorded by
  `ShaderAddDynamicSLoadMapping` (`source/emulator/src/Graphics/ShaderResources.cpp:597`). The record
  type is `ShaderDynamicSLoadMapping` (`source/emulator/include/Emulator/Graphics/Shader.h:1816`).
- Not yet traced: what an SGPR-offset S_LOAD that feeds an image instruction
  becomes in the final SPIR-V. The existing zero-read paths are
  `ShaderAddLoadedZeroImageResource` and `ShaderAddZeroDescriptorLoad`
  (`source/emulator/src/Graphics/ShaderResources.cpp:737-738`, `:1514`), and `EmitZeroScalarLoad` in
  `source/emulator/src/Graphics/ShaderSpirvImage.cpp`. Stage 1 measures whether a table access
  reaches them. Table elements must not use them (section 5.4).

### 2.2 Capacity limits in the base

- `ShaderTextureResources::RES_MAX` is 64 (`source/emulator/include/Emulator/Graphics/Shader.h:2085`,
  with a static assert at line 2086). Sampled and storage images share it, and
  the write-only masks use one `uint64_t` bit per resource.
- `ShaderSamplerResources::RES_MAX` is 16 (`source/emulator/include/Emulator/Graphics/Shader.h:2119`).
- `ShaderDescriptorLayoutPlan::MAX_BINDINGS` is 13
  (`source/emulator/include/Emulator/Graphics/ShaderDescriptorLayoutPlan.h:22`). The plan is a
  fixed array. A table binding must not be added to a general plan that is
  already full, so tables use their own plan (section 3.5).
- A table binding cannot be a `desc[]` entry, because `RES_MAX` is 64 and the masks
  use one bit per entry (section 3.1).

### 2.3 Shape classes

- A virtual sampled descriptor carries a shape tag. `ShaderGen5SampledTextureShape`
  has three values, TwoDimensional, TwoDimensionalArray and ThreeDimensional
  (`source/emulator/include/Emulator/Graphics/Shader.h:1345-1350`).
- `ShaderGen5SampledTextureShapeForType` (`source/emulator/include/Emulator/Graphics/Shader.h:1352-1363`) maps the T# type
  field: type 10 (3D) to ThreeDimensional; types 11 (cube), 12 (1D array) and 13
  (2D array) to TwoDimensionalArray (`ShaderGen5TextureTypeUsesArrayAddressing`,
  `source/emulator/include/Emulator/Graphics/Shader.h:1330-1334`); every other image type (8 = 1D, 9 = 2D) to TwoDimensional.
  Cube is therefore an array class, not a 2D class.
- Vulkan sampled-image arrays have a static view type. A 2D, a 2D-array and a 3D
  image cannot share one array, even when their guest descriptors come from one
  table.
- The layout writer uses seven separate sampled-image bindings per general set
  (`source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp`, the `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`
  writes at lines 871, 887, 903, 967, 983, 999 and 1015). They are the 2D, 2D depth,
  2D-array, 3D and the three integer-format variants. Each binding has its own
  count up to `TEXTURES_SAMPLED_MAX`.
- Sampled-image bank selection for the general path happens in `PrepareTextures`
  (`source/emulator/src/Graphics/GraphicsRenderBind.cpp:3450`), which fills separate output banks for
  sampled, depth, array, 3D and unsigned variants.

### 2.4 Descriptor pool and layout counting

- `source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp:253` (`DescriptorCache::CreatePool`):
  `max_sets = 512` (line 260), one pool for all general sets, flag
  `FREE_DESCRIPTOR_SET_BIT`. The sampled-image pool size is
  `max_sets * TEXTURES_SAMPLED_MAX * 7` (lines 265-266). The 7 is the number of
  sampled-image bindings of a general set, not a per-slot factor.
- `DescriptorCache::Allocate` (`source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp:297-360`) takes a
  recycled set of the same layout, or allocates from a pool. When every pool is full
  it calls `CreatePool()` again and retries once. The pool count
  therefore grows without a bound. Table sets must not use this growth path
  (section 3.5).
- `source/emulator/src/Graphics/ShaderDescriptorLimits.cpp:36` (`CountLayout`) charges each of the
  seven sampled-image bindings with the same count, `sampled_descriptors`, which
  is the worst-case reading of the header comment at
  `source/emulator/include/Emulator/Graphics/ShaderDescriptorLimits.h:76-77`. The constant
  `SAMPLED_IMAGE_BINDINGS = 7` is at line 44 and has no comment giving its reason.
- A table with `C` elements in one general set would take `C` of the pool's
  `512 * 64 * 7 = 229,376` sampled descriptors for every set. Tables therefore get
  their own pool (section 3.5).
- Set lifecycle today: `RecordSetUse` (line 1170) records the submissions that used
  a set. `ReleaseSet` (line 1156) recycles it only after `GpuMemorySubmissionsComplete`,
  otherwise frees it. `IndexSet` (line 1105) indexes the set under every resource it
  references, and `FreeSetsReferencing` (line 1263) releases the sets of a deleted
  resource. `TrimSets` (line 1231) bounds the live count with `LIVE_SETS_MAX = 16384`
  (`source/emulator/src/Graphics/GraphicsRenderInternal.h:477`) and `TRIM_SETS = 2048` (line 478).

### 2.5 Device features

- `source/emulator/src/Graphics/Window.cpp:2273-2275` sets `robustBufferAccess`,
  `vertexPipelineStoresAndAtomics` and `fragmentStoresAndAtomics` unconditionally in
  `device_features`.
- The device-selection checks at `source/emulator/src/Graphics/Window.cpp:2019-2028` set `skip_device = true`
  when `fragmentStoresAndAtomics` or `vertexPipelineStoresAndAtomics` is missing.
  A device without them is not used. The fault path of section 5.1 relies on them.
- `device_features_12` (lines 2325-2328) carries the 1.2 aggregate. The comment at
  line 2324 says its pNext chain must not also carry
  `VkPhysicalDeviceBufferDeviceAddressFeatures` (VUID-02830).
- No descriptor-indexing feature is requested. No source under `source/emulator/` names
  `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound`,
  `runtimeDescriptorArray`, `robustImageAccess` or `nullDescriptor`.
- In the bundled `source/3rdparty/vulkan/include/vulkan/vulkan_core.h`,
  `shaderSampledImageArrayDynamicIndexing` is a field of `VkPhysicalDeviceFeatures`
  (1.0). The other three are fields of `VkPhysicalDeviceVulkan12Features`.

### 2.6 Ray tracing

- See section 6.

## 3. Representation

### 3.1 A table is a bind record

A table is not an extra `desc[]` entry. It gets a record in a new file pair
(`ShaderDescriptorTable.h` and `ShaderDescriptorTable.cpp`), because
`source/emulator/src/Graphics/ShaderResources.cpp` is already large. The record holds:

- `kind`: texture (T#) or sampler (S#);
- the table base source: the user-data SGPR pair that the S_LOAD `SBASE` names
  (section 3.2);
- the element size `E` in bytes, measured from the feeding load (section 3.2);
- the shape classes used by the table (section 3.3);
- the capacity `C` of the pipeline (section 3.4);
- the snapshot descriptors and cause words, and the descriptor set that holds them
  (section 3.6).

### 3.2 Access form, element size and element index

- The image instruction's SRSRC names four or eight SGPRs. The table path admits
  only the 8-SGPR form (R128 = 0, 256-bit T#, 32-byte element). The 4-SGPR form
  (R128 = 1) is refused explicitly. Its Table 45 row (section 8.2.6) lists the
  128-bit resource without the type field at bits [127:124], so the element class
  cannot be checked from the words. Section 8.2.10 requires R128 = 1 for the BVH
  form (section 6).
- The SRSRC must be the destination of exactly one S_LOAD in the same code
  path, with a destination width of 8 SGPRs. The definition search follows the
  pattern of `source/emulator/src/Graphics/ShaderResources.cpp:401-420`. An SRSRC that is assembled from parts,
  is partly overwritten, or is not produced by an S_LOAD of the user-data pointer
  is refused explicitly.
- Sampler tables follow the same rule with a 4-SGPR destination, `E = 16`. They
  are not designed in detail (section 9).
- Table base: the S_LOAD `SBASE` pair must resolve to a user-data pair at draw or
  dispatch time. The existing dynamic texture path does the same resolution through
  `HW::UserSgprInfo` (`ShaderAddDynamicTextureResource`, `source/emulator/src/Graphics/ShaderResources.cpp:717`).
  A base computed by shader code is refused explicitly at translation. Stage 1
  measures whether any title uses another base source.
- Element index: the S_LOAD address is `base + inst_offset + SOFFSET_value`
  (section 7.2.1). The translator computes the unsigned byte offset
  `a = SOFFSET_value + inst_offset` as a SPIR-V value, then:
  - `index = a / E` (unsigned division);
  - the access is placed when `a mod E == 0`. An unplaced access is not an
    invalid element. The hardware would ignore only the two LSBs of the
    address, so the table path reports it with its own cause (section 5.1);
  - a negative sum is undefined in the ISA (section 7.2.1). As an unsigned value
    it is out of range and takes the IN_RANGE cause.
- SOFFSET form. `SOFFSET_value` is the value of an SGPR. The M0 form and the zero
  form of section 7.2.1 are refused explicitly at translation (section 5.2). Whether
  a zero-SOFFSET access is better served by an existing path is open (section 10.13).
- Consistency: one table is accessed with one `E`. Two accesses to the same table
  with different `E` are refused at translation.
- No pattern-matching of the offset expression is required. The guard of section
  5.1 checks the value at run time. Stage 1 records the offset expressions that
  occur, to size the guard cost and to confirm the base source.

### 3.3 Cause words and element classes

Every element `i` has one cause word per class `K` that the table uses. Zero means
the element is valid for `K`. Each nonzero value is a set of cause bits (section 5.1):

| Bit | Name | Set in the element cause word when |
| --- | --- | --- |
| 4 | UNMAPPED | the element's bytes were not captured (section 3.6, `NotAllocated`) |
| 8 | INVALID | the element is zero, a non-image type (`ShaderGen5TextureTypeIsImage` at `source/emulator/include/Emulator/Graphics/Shader.h:1337` is false), or the non-exiting resolve refuses it |
| 16 | CLASS | the element is an image of another shape or bank than `K` |

The access-side bits are IN_RANGE (1, `index >= C`) and PLACEMENT (2, `a mod E != 0`).

Element class rule:

- Shape: `ShaderGen5SampledTextureShapeForType` on the element's type field
  (T# bits [127:124], Table 45, section 8.2.6). Types 8 and 9 map to TwoDimensional,
  10 to ThreeDimensional, 11 to 13 to TwoDimensionalArray. This is the helper the
  repository already uses. The helper is not changed.
- Bank: `PrepareTextures` (`source/emulator/src/Graphics/GraphicsRenderBind.cpp:3450`) selects, for a
  resource, one of its sampled, depth, array, 3D and unsigned banks. The table path
  has one class per shape, and each class is the sampled bank of that shape. An
  element whose bank is not the sampled bank has CLASS for every class `K`. The
  stage-2 non-exiting resolve reports the bank (below). Whether the bank is a
  function of the T# words alone is not verified (open item 10.3).
- Resolve: `PrepareTextures` exits on an unsupported tile mode or format
  (`source/emulator/src/Graphics/GraphicsRenderBind.cpp:3525`, `:3533`, `:3631`). It cannot be called for a table
  element as it is. Stage 2 adds a non-exiting resolve result with the same checks,
  which returns INVALID for an element it refuses and reports the bank of an element
  it accepts. The general path keeps its exit.

Write rule. The class-`K` array holds the descriptor of element `i` only when
`cause_K[i]` is zero. For any other element the array entry is not written, so
`PARTIALLY_BOUND` (section 3.5) leaves it unbound, and no access reads it. The
cause words of all elements and classes are kept in the cause buffer (section 3.5).

Worked example, `C = 4`, one table, one 2D sampled access (no depth or integer
variant):

| index | element | 2D cause word |
| --- | --- | --- |
| 0 | 2D image | 0 (valid) |
| 1 | zero T# | INVALID (8) |
| 2 | 3D image | CLASS (16) |
| 3 | 2D image | 0 (valid) |

An access with index 0 or 3 samples the array. An access with index 1 faults
with cause INVALID. An access with index 2 faults with cause CLASS. An access with
index 4 faults with cause IN_RANGE, and reads no cause word. The element at index
2 is a fault only when an access reaches it.

### 3.4 Capacity `C`

`C` is per pipeline layout. It is the largest count that the device limits and the
general sets of that pipeline leave room for. The device inputs are the
`VkPhysicalDeviceLimits` fields, read through `ShaderDescriptorLimitsFromVulkan`
(`source/emulator/src/Graphics/GraphicsRenderDescriptorLimits.h:10-22`). Field names are those of
`source/3rdparty/vulkan/include/vulkan/vulkan_core.h` (lines 2557-2570, and line 2613 for
`minStorageBufferOffsetAlignment`):

- `maxPerStageDescriptorSampledImages` (L_img): sampled images per stage;
- `maxPerStageResources` (L_res): the sum CheckPerStage applies to sampled and
  storage images, storage and uniform buffers and color attachments
  (`source/emulator/src/Graphics/ShaderDescriptorLimits.cpp:133-143`);
- `maxPerStageDescriptorStorageBuffers` (L_sto), for the cause buffer and the
  fault binding of each visible table;
- `maxDescriptorSetSampledImages` (L_set): sampled images in one pipeline layout;
- `maxBoundDescriptorSets` (L_sets): descriptor sets used by one pipeline;
- `maxPerStageDescriptorSamplers` (L_smp), for sampler tables;
- `maxDescriptorSetStorageBuffersDynamic` (L_dyn), for the fault binding of each
  table set;
- `minStorageBufferOffsetAlignment` (L_off), which sets the fault ring stride
  (section 3.5).

Pipeline inputs, per stage `s` of the pipeline (`CountStage`,
`source/emulator/src/Graphics/GraphicsRenderPipelineLimits.cpp:30-38`, and `CountLayout` charges
seven sampled-image bindings per general descriptor,
`source/emulator/src/Graphics/ShaderDescriptorLimits.cpp:36-47`):

- `g_s` = the general sampled images of stage `s`, as `CountStage` reports them
  (the seven-binding charge is included);
- `R_s` = the rest of CheckPerStage's resource total for `s` without tables
  (`g_s` is part of it), so that `R_s + tables <= L_res`;
- `o_s` = the general storage buffers of `s`;
- `t_s` = table sampled-image bindings visible to `s`, one per (table, class);
- `v_s` = storage buffers added to `s` by tables, two per visible table set (the
  cause buffer and the fault binding);
- `G` = sum of `g_s` over the stages, and `T` = sum of `t_s` over the stages, in
  the same per-stage accumulation that `CheckPipelineLayout` uses for its total.

Formula:

```
C_stage(s) = floor((L_img - g_s) / t_s)              when t_s > 0
C_res(s)   = floor((L_res - R_s - v_s) / t_s)        when t_s > 0
C_set      = floor((L_set - G) / T)                  when T > 0
C          = min(C_POLICY, C_stage(s), C_res(s), C_set   over all s, and the L_smp form for samplers)
```

`C_POLICY = 4096` is a decision. A snapshot of a changed table reads at most
`C * E` bytes (128 KiB at `C = 4096`, `E = 32`) and writes `C` cause words per
class (16 KiB per class at 4096). Unchanged tables skip the read when the tracker
confirms the bytes did not change (section 3.6). Stage 3 measures the cost, and the
cap can be lowered then.

Feasibility, checked before `C` is used. The pipeline is refused explicitly, with
the failing limit name from `FailureName` (`source/emulator/src/Graphics/ShaderDescriptorLimits.cpp:189`),
when:

- `C < 1` (no room for a table);
- `o_s + v_s > L_sto` for any stage `s`;
- the table sets of the pipeline exceed `L_dyn` (dynamic storage buffers);
- the set count of the pipeline is larger than `L_sets` (section 3.5);
- `L_off` is zero, so no fault ring stride can be formed (section 3.5).

Worked example. The values are hypothetical and are not device measurements. They
are read at run time from the names above. Assume `L_img = 512`, `L_res = 512`,
`L_set = 2048`, `L_sto = 64`, `L_dyn = 8`, `L_sets = 8`. The general layout of one
stage has `g_s = 7 * 64 = 448` and `o_s = 0`, one table with one class is visible
(`t_s = 1`, `v_s = 2`), and `R_s = 448`. Then:

- `C_stage = floor((512 - 448) / 1) = 64`;
- `C_res = floor((512 - 448 - 2) / 1) = 62`;
- `C_set = floor((2048 - 448) / 1) = 1600`;
- `C = min(4096, 64, 62, 1600) = 62`.

The per-stage resource total binds first. A table larger than 62 elements is not
refused. Its indices at or above 62 fault at run time (section 5.1).

Sampler tables use the same formula with `L_smp` in place of `L_img` and the
sampler charge in `g_s`.

### 3.5 Pool, layout and slots

- Table layout. Built from its own plan, not from the general plan, so the general
  plan keeps its 13 slots (`source/emulator/include/Emulator/Graphics/ShaderDescriptorLayoutPlan.h:22`) and its
  `CacheKey` (`source/emulator/src/Graphics/ShaderDescriptorLayoutPlan.cpp:120`). Bindings:
  - one `SAMPLED_IMAGE` (or `SAMPLER`) array per class, `descriptorCount = C`,
    binding flag `PARTIALLY_BOUND`;
  - one `STORAGE_BUFFER` binding holding the cause words of all classes of the
    table (section 3.3), part of the immutable snapshot;
  - one `STORAGE_BUFFER_DYNAMIC` binding that points at the fault ring slot of the
    recording (section 3.5, below). Its offset is supplied at bind time, so the
    table set holds no fault word and stays immutable.
- Fault ring. One fixed-size `VkBuffer` for the render context, with one slot per
  command-buffer slot. The slot count is the command-buffer ring size
  `VK_BUFFERS_NUM` (`source/emulator/src/Graphics/GraphicsRunInternal.h:424`). Slot `n` starts at byte
  offset `n * S`, where `S` is the slot stride: the least multiple of
  `minStorageBufferOffsetAlignment` that is at least 4 bytes. The buffer size is
  `VK_BUFFERS_NUM * S` bytes. The Vulkan specification requires each dynamic
  storage-buffer offset to be a multiple of `minStorageBufferOffsetAlignment`. That
  rule text is not quoted from the repository (open item 10.1). A recording takes
  its slot when it starts, and the slot word is zeroed at that point by a fill
  recorded before any table access, followed by a barrier (stage 3). The slot is
  read at the completion of that recording's submission, through the completion
  actions (`GpuCompletionActionSink`, `source/emulator/include/Emulator/Graphics/GpuSubmissionTracker.h:87`).
  A slot is reused only after its submission completes. The EXIT names that
  submission. The fault word is therefore per submission, not per set.
- Slots. Each set index is fixed by the pipeline kind. The code asserts indices
  (`source/emulator/src/Graphics/GraphicsRenderPipeline.cpp:322`): a bind's
  `descriptor_set_slot` must equal the next free index in the layout array.
  - graphics: the vertex general set is 0. The pixel general set is 1 when the
    vertex stage uses a descriptor, and 0 otherwise (`source/emulator/src/Graphics/Shader.cpp:4506`).
    The transport set is 2 and is bound at index 2 by the capture and resolve draws
    (`source/emulator/src/Graphics/GraphicsRenderDraw.cpp:1561`, `:1582`).
    Today the code pads to two sets only inside the transport branch
    (`source/emulator/src/Graphics/GraphicsRenderPipeline.cpp:767-769`). Without a transport set nothing
    pads, so the next free index is the number of general sets present. The clip-probe
    set is added only when there is no transport set (`:778`). Its index is the
    caller's `descriptor_set`, which must equal the next free index and be below 3
    (`:786`).
    The table set is index 3. This needs stage 3 changes: the layout array at
    `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp:750` holds three entries and
    must grow to four. A graphics table pipeline pads the general sets below index 2
    with `EmptyDescriptorSetLayout`. Index 2 is then the transport set if present, else
    the clip-probe set if enabled, else an empty layout. The table set is appended at
    index 3. The clip-probe check at `:786` keeps its index at 2 in that case.
    Sets 0 to 3 are the most a graphics table pipeline uses, so `maxBoundDescriptorSets`
    must be at least 4, and the device value is checked explicitly.
  - compute: the general set is 0 (or an empty layout) and the table set is 1. The
    compute recipe assertion at `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp:1040` (`descriptor_set_slot
    != 0`) changes at stage 3 to admit the table slot.
  - the translator emits the table set's `DescriptorSet` decoration with this index.
- Pool. A dedicated pool serves table sets only. Its size is fixed by
  `TABLE_SETS_MAX`, a decision value of 64 that stage 3 measures. For each table
  layout with `C` elements per class:
  - `SAMPLED_IMAGE` (or `SAMPLER`): `TABLE_SETS_MAX * sum over classes of C`;
  - `STORAGE_BUFFER`: `TABLE_SETS_MAX` (cause buffer);
  - `STORAGE_BUFFER_DYNAMIC`: `TABLE_SETS_MAX` (fault binding).
  The pool is created once with these sizes and never grows. `CountBindings`
  (`source/emulator/src/Graphics/ShaderDescriptorLimits.cpp:58`) is used only as the count check
  for the layout. General sets never draw from this pool, and table sets never
  draw from the general pool.
- Exhaustion. A table set request on a full pool does not call `CreatePool()`. It
  waits for the oldest submission that holds a table set and retries, at most 64
  times, the bound of the existing loops (`source/emulator/src/Graphics/GraphicsRun.cpp:2423`). If
  that submission is the current recording, the wait submits it first:
  `CommandProcessor::WaitSubmission` submits a recording submission of the current
  buffer (`source/emulator/src/Graphics/GraphicsRun.cpp:742-745`). The table wait holds no processor
  or render lock while it waits, as the compute loop does (the rule stated at
  `source/emulator/src/Graphics/GraphicsRun.cpp:2726-2727`). The request then retries against the new
  current buffer. This is a resource wait. It does not wait on a producer of table
  bytes, so it is not a refusal under section 5.3. If no table set is held by an
  in-flight or current submission, or the bound is reached, the request is an
  explicit EXIT that names the pool.
- Pipeline recipes. Graphics pipeline creation and the compute precompile recipe
  (`source/emulator/src/Graphics/ComputePipelinePrecompile.cpp:225-250` encodes the plan bindings)
  must carry the table plan and `C`. Stage 3 bumps the recipe encoding version in
  the same change, and the pipeline cache key includes the table plan.

### 3.6 Snapshot, reuse and set lifetime

- Snapshot. A snapshot is the descriptor words and cause words for one table base
  and one `C`. It is taken at record time, after the ordering checks of section 5.3.
- Chunked capture. The read is `C * E` bytes from the base. It is done in chunks of
  64 elements through `GpuMemoryCaptureSnapshotReadOnlyBuffer`
  (declared at `source/emulator/include/Emulator/Graphics/Objects/GpuMemory.h:1504-1509`, wrapper
  defined at `source/emulator/src/Graphics/Objects/GpuMemory.cpp:736`). That function returns `bool` and does not say why it
  refused. Stage 2 adds a result with three values: `Read`, `NotAllocated`
  (the range is not inside an allocated guest range) and `Refused` (an admission
  rule refused it). A chunk that returns `NotAllocated` is re-read element by
  element. An element that returns `NotAllocated` gets the UNMAPPED cause. A
  `Refused` result after the ordering checks have cleared is an explicit EXIT.
- The ordering checks of section 5.3 run over the whole range `[base, base + C * E)`.
  The range is bounded by `C`, which is bounded by section 3.4.
- Reuse. The repository already has the protocol this needs:
  `TransientBufferPool` in `source/emulator/src/Graphics/GraphicsRenderCommandBuffer.cpp:962`
  keeps guest-byte snapshots. A snapshot at least `kMinTrackedSnapshotSize`
  (4096 bytes, line 967) is tracked. `BeginTracking` (line 1518) arms
  write-tracking before the compare, `ChangedSince` (line 1490) lets a later probe
  skip the compare, and `FinishTracking` (line 1544) commits the generation only if
  `ReadObservationIsStable` holds (`source/emulator/include/Emulator/Graphics/GpuDirtyPageTracker.h:114,
  :148-149`). The table snapshot uses the same steps. The threshold depends on the
  element kind. A 32-byte T# table is tracked from `C >= 128` elements. A 16-byte
  S# table is tracked from `C >= 256` elements.
- The skip of an unchanged table depends on write-back notifications. The write-back
  copy's own write notifications advance the range's generation
  (`source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp:246-248`), so a change written back into
  the table bytes is seen by the probe. Without that notification the tracked skip
  would not see the change.
- Repeat. A read whose `ReadObservationIsStable` is false is repeated. The repeat
  count is bounded at 3 (stage 3 may change it). A read still unstable after the
  bound is an explicit EXIT.
- Compare. When a table is not tracked, or its tracker mode is not `PageFault`
  (`GpuDirtyTrackingMode::PageFault`, `source/emulator/include/Emulator/Graphics/GpuDirtyPageTracker.h:17-21`),
  the snapshot compares full words with the last snapshot. A hash alone is not used, because a
  collision would bind a wrong descriptor.
- Immutable sets. When the descriptor words or cause words change, a new set is
  allocated from the table pool (section 3.5), written once, and bound. The
  previous set is released through `ReleaseSet`. Sets are never updated after bind.
  `UPDATE_AFTER_BIND` and `descriptorBindingSampledImageUpdateAfterBind` are not
  used: the existing cache writes a set before bind and recycles it only after the
  submissions that used it complete (`source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp:1156`).
- Indexing. A table set is indexed under every image, sampler and cause buffer it
  references, through `IndexSet`. A deleted image then frees the sets that name it.
- Limits. `LIVE_SETS_MAX` and `TrimSets` bound the general sets. The table pool is
  bounded by `TABLE_SETS_MAX` and its own exhaustion rule (section 3.5).

### 3.7 Translator version (decision for stage 2)

- Translated SPIR-V for a table-bearing shader changes at stage 2. The cache keys on
  `key.translator_version` (`source/emulator/src/Graphics/ShaderTranslationCache.cpp:27`), the
  version is `kShaderTranslatorVersion` in `source/emulator/include/Emulator/Graphics/ShaderTranslationCache.h`
  (line 19), and the same value is part of the cache suffix (line 26).
- Decision: stage 2 bumps `kShaderTranslatorVersion` in the change that alters
  translated output for table-bearing shaders. That invalidates every translation
  cache entry, once per title. The cost is accepted because this is the
  invalidation path that is already wired.
- `C` is not part of the translated SPIR-V. The array length is runtime
  (`OpTypeRuntimeArray`, section 4) and the guard `index < C` reads `C` as a
  specialization constant set at pipeline creation. So the translation key does not
  depend on the device, and the version bump is the only translation invalidation.
- This document does not bump the constant.

## 4. NonUniform and feature gating

| Requirement | Used for | Where it is set or checked |
| --- | --- | --- |
| SPIR-V capability `SampledImageArrayDynamicIndexing` (29) | non-constant index into a sampled-image array | Vulkan 1.0 feature `shaderSampledImageArrayDynamicIndexing`, a field of `VkPhysicalDeviceFeatures`; set in `device_features` |
| Decoration `NonUniform` (5300) and capability `ShaderNonUniform` (5301) | divergent index | Vulkan 1.2 feature `shaderSampledImageArrayNonUniformIndexing`, a field of `VkPhysicalDeviceVulkan12Features`. The Vulkan reference page for it describes the capability `SampledImageArrayNonUniformIndexing`. The header name of 5301 is `ShaderNonUniform`; the mapping is confirmed at stage 3 |
| Capability `RuntimeDescriptorArray` | runtime-length table arrays | Vulkan 1.2 feature `runtimeDescriptorArray`, a field of `VkPhysicalDeviceVulkan12Features` |
| Binding flag `PARTIALLY_BOUND` | unbound elements that the guard never reads | Vulkan 1.2 feature `descriptorBindingPartiallyBound`, a field of `VkPhysicalDeviceVulkan12Features` |
| Storage writes from a fragment or vertex stage (fault path) | the atomic `atomicOr` of section 5.1 | Vulkan 1.0 features `fragmentStoresAndAtomics` and `vertexPipelineStoresAndAtomics`, already set at `source/emulator/src/Graphics/Window.cpp:2274-2275` and required by `source/emulator/src/Graphics/Window.cpp:2019-2028` |
| Dynamic storage buffer | the per-recording fault binding | limit `maxDescriptorSetStorageBuffersDynamic` (section 3.4); the feature is core |
| not used | `descriptorBindingSampledImageUpdateAfterBind` (section 3.6) | |

The Vulkan wording of the feature descriptions is paraphrased. It is not quoted
here. The exact text is confirmed at stage 3 (section 10.1).

Gating rules:

- The 1.2 fields go into `device_features_12` (`source/emulator/src/Graphics/Window.cpp:2325`), the
  single 1.2 aggregate. No second pNext struct carries them, because of the
  VUID-02830 comment (line 2324). `VkPhysicalDeviceDescriptorIndexingFeatures` is
  not added, because the aggregate carries the same fields.
- The 1.0 field goes into `device_features`, read from the same
  `supported_features` struct as the existing fields.
- Enable each feature when the device reports it. Do not fail device creation,
  because titles without tables must keep working.
- Fail explicitly at pipeline creation when a pipeline needs a table and a feature
  is missing. The message names the feature. No fallback to direct lookup.
- The same rules apply to sampler tables.

## 5. Access contract, translation refusals and GPU-written tables

### 5.1 The contract for a table access

Every table access is guarded by the translator. The guard is evaluated per lane:

```
in_range = index < C                        // C: specialization constant
placed   = (a % E) == 0                     // a: SOFFSET_value + inst_offset, unsigned
cause    = (in_range ? 0 : IN_RANGE) | (placed ? 0 : PLACEMENT)
dPdx     = OpDPdx(coord)                    // fragment only, above the guard
dPdy     = OpDPdy(coord)                    // fragment only, above the guard
if (in_range)                               // the cause word is not read out of range
    cause |= causes_K[index]                // host-written; 0 = valid for K
if (cause == 0) {
    color = sample_K(array_K[nonuniform index], coord, Grad(dPdx, dPdy))
} else {
    atomicOr(fault_slot, cause)             // only lanes that fail
    color = vec4(0)                         // defined value on the error path
}
```

- Bits: IN_RANGE = 1, PLACEMENT = 2, UNMAPPED = 4, INVALID = 8, CLASS = 16. The
  access bits are 1 and 2. The element bits are 4, 8 and 16 (section 3.3).
- Implicit LOD. `image_sample` with implicit LOD is emitted as `OpImageSampleExplicitLod`
  with `Grad`, using `OpDPdx` and `OpDPdy` of the coordinates. The derivatives sit at the
  sample's own position in the translated code, where the guest's implicit sample already
  has them, so the guard is emitted after them. The explicit-gradient form is needed
  because an implicit-LOD sample inside a per-lane guard has undefined derivatives. The
  Vulkan rule that makes it undefined is confirmed at stage 3 (section 10.1). An explicit
  LOD sample (`image_sample_l`, `_lz`) keeps the guest's LOD value.
- The fault word is written only by the atomic, and only by failing lanes. It is in the
  fault ring slot of the recording (section 3.5), never in the table set.
- The fault slot is read at the completion of its submission. A nonzero word is an
  explicit EXIT. The EXIT names the submission, the table base, the class, and every cause
  bit that is set. The word is not cleared, so the failure is repeated, not hidden.
- The zero value returned on the error path is a defined value for the access, not a
  success. The run ends at the completion of that submission. A frame that used a faulted
  element is never reported as correct.
- A divergent guard is per lane. The atomic is executed only by lanes that fail.
- An index at or above `C` reads no descriptor and no cause word. It is therefore never
  a scan window and never a placeholder read.

### 5.2 Translation-time refusals

These are decided before any table contents exist, and each is an explicit refusal
at pipeline creation or translation:

- a table base that is not a user-data SGPR pair (section 3.2);
- an SRSRC that is not the whole destination of one 8-SGPR S_LOAD of the user-data
  pointer (section 3.2), including an SRSRC built from parts or partly overwritten;
- an image op with R128 = 1 (4-SGPR T#) on a table (section 3.2);
- two accesses to one table with different element sizes `E`;
- an M0 or a zero SOFFSET form on a table access (section 3.2);
- bias sampling on a table element (`SAMPLE_B`-class forms), or implicit LOD outside the
  fragment stage;
- a S_BUFFER_LOAD source (section 1.2): its bound is a V# field, not admitted;
- a missing device feature (section 4) or a limit failure (section 3.4);
- `C < 1`, or a set count above `maxBoundDescriptorSets`;
- a graphics table pipeline on a device with `maxBoundDescriptorSets` below 4.

The SGPR offset form is not refused at translation, because the run-time guard in 5.1
covers it. The bank of an element is not checked here. It is decided at snapshot time
(section 3.3).

### 5.3 GPU-written and CP-written tables: detect and refuse

A host read at record time sees guest bytes only as far as they have been written
back. The table path must never take a snapshot while a producer of its bytes is
pending. The design detects the producer and refuses the draw or dispatch with an
explicit EXIT. It does not resolve the table on the GPU, and it does not take a stale
snapshot.

Why not GPU resolution: a Vulkan sampled-image array element must be a host-written
`VkImageView` descriptor. The device extensions requested at
`source/emulator/src/Graphics/Window.cpp:3057-3068`, and those enabled after feature checks at
`source/emulator/src/Graphics/Window.cpp:3131-3185`, include no descriptor-buffer extension (`VK_EXT_descriptor_buffer`
does not appear in the source tree). A device table of guest T# words therefore cannot
produce those handles. The GPU-side resolution the repository already has
(`GpuMemoryCollectStorageForGuestSeed`, called at `source/emulator/src/Graphics/GraphicsRenderBind.cpp:2589`) copies
data into vertex objects, not descriptors.

The ordering checks run before the snapshot read, over the range `R` of section 3.6:

1. Storage uses, host form. The existing `GpuMemoryPendingStorageWriteBack`
   (`source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp:522`) with `publishable = nullptr`
   reports every incomplete use of a writable storage object in `R`, including a use
   on the consumer's own queue and the current recording (uses are registered at
   record time through `RecordUse(ObjectInfo*, CommandBuffer*)`,
   `source/emulator/src/Graphics/Objects/GpuMemoryCreate.cpp:1052`). It is not sufficient alone. `CollectWritableStorage`
   (`source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp:478-479`) skips an object whose writes are all published
   on the consumer queue, so a published object still owing guest bytes is not
   reported. A host read needs the guest bytes, so stage 2 adds a host-read form of
   the query that does not skip published objects. The justification is that
   publication copies into the imported view on the GPU, not into the guest bytes
   that the host reads. The new query is not needed for the device-address path,
   which reads on the GPU.
2. Non-storage write-back. `GpuMemoryPendingWriteBackOutsideStorage`
   (`source/emulator/include/Emulator/Graphics/Objects/GpuMemory.h:1345`) over `R`. The compute path
   also checks `GuestDeviceAddressPendingWriteBackOutsideStorage()` when processor
   write-back is pending (`source/emulator/src/Graphics/GraphicsRenderDraw.cpp:3595`).
3. Command-processor producers. `GpuSubmissionTracker::FindPendingProducer`
   (`source/emulator/src/Graphics/GpuSubmissionTracker.cpp:289`) over `R`, with a mask that matches
   any write into `R`. `CommandProcessor::HasPendingDeferredWrite`
   (`source/emulator/src/Graphics/GraphicsRun.cpp:1294`) shows the call form.
4. Device-address stores. A shader that stores through a guest pointer writes into
   `R` without naming it in its bind list, so checks 1 to 3 may not see it. Whether
   such a store is recorded against any object is not verified (section 10.9). The
   flag `bind.device_address_used` (`source/emulator/include/Emulator/Graphics/Shader.h:2195`) is set
   for device-address use in general, loads included (`source/emulator/src/Graphics/Shader.cpp:4627-4639`,
   `:4837`), so by itself it does not identify stores. The search of `source/emulator`
   found no use of it in `GpuSubmissionTracker`. Check 4 therefore has no source of
   truth yet. The intended interim rule is to treat any pending submission whose shader
   uses device addresses as a producer of every table. Whether a pending submission can
   name that shader is not verified. Stage 2 adds a store flag and a query (section 7).
   Without them the table path is not landable.

Any nonzero result of the checks is a refusal. The existing read preparation
`PrepareGuestDeviceAddressForRead` (`source/emulator/src/Graphics/GraphicsRenderDraw.cpp:1379-1413`)
reports `PendingWriteBack` and `PendingReader` (`GuestDeviceAddressReadPreparation`,
`source/emulator/src/Graphics/GraphicsRenderDraw.cpp:1372-1377`). Its table preparation result `Failed`
(`GuestDeviceAddressPrepareResult`, `source/emulator/include/Emulator/Graphics/GuestDeviceAddress.h:59-64`)
ends in an EXIT (`source/emulator/src/Graphics/GraphicsRenderDraw.cpp:1409-1410`). The compute loop also
maps `ProcessorWriteBackRequired` (`source/emulator/src/Graphics/GraphicsRun.cpp:2718-2724`). The table
path does not use the retry loops of the device-address path (`RetryGuestDeviceAddressDraw`,
`source/emulator/src/Graphics/GraphicsRun.cpp:2423-2440`, and the compute loop, `source/emulator/src/Graphics/GraphicsRun.cpp:2697-2730`). It
refuses on any pending outcome in both draw paths, so the graphics and compute outcome
sets do not need to match.

A use in the current recording counts as pending for check 1 and is refused too. No
submission is forced to clear it. A refusal is an explicit EXIT that names the table
base, the kind of producer (storage use, write-back, command-processor producer or
device-address store) and the draw or dispatch.

Only after the checks are clear does the snapshot read run, under the dirty-page
protocol of section 3.6 (`BeginRead`, `ReadObservationIsStable`). A guest CPU write
during the read changes the observation, and the read is repeated a bounded number of
times (section 3.6). An exhausted repeat count is an explicit EXIT.

Dirty-page tracking is not the guard for pending GPU writes. Checks 1 to 4 cover
those. The write-back copy's own write notifications advance the range's generation
(`source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp:246-248`), so a write-back into table bytes is
seen by the dirty-page tracker. Section 3.6 depends on this.

Cost and evidence: stage 3 counts the refusals on the traced title and on the titles
that share the path. A refusal on a title the feature targets stops that title, so the
count decides whether the deferral alternative of section 10.11 is needed. GPU-side
resolution is not adopted unless a measurement shows that refusal cannot serve the
feature.

### 5.4 Zero reads are not used for table elements

- Zero reads for non-table loads stay as they are (`ShaderAddLoadedZeroImageResource`
  and `ShaderAddZeroDescriptorLoad`, `source/emulator/src/Graphics/ShaderResources.cpp:737-738`,
  `:1514`). They serve loads the base already handles.
- A table element that is not valid is not zero-read. Its access faults with a cause
  bit (section 5.1). A zero-read in place of a missing element would be a silent
  default, which the project rules forbid.

### 5.5 Robustness

- `robustBufferAccess` is the only robustness feature requested
  (`source/emulator/src/Graphics/Window.cpp:2273`), so buffer reads through a V# keep the existing `num_records`
  behaviour.
- `robustImageAccess` is not requested. It is not needed on the table path, because
  the guard in 5.1 prevents any out-of-range image access. `nullDescriptor` is not
  requested and not planned.

### 5.6 Raw S_LOAD

A raw S_LOAD of a table entry has no bounds in the ISA (section 7.2.1). The
translator checks the base (3.2) and the guard (5.1) checks the index and the
placement. The table extent is not read from the ISA. The extent is the capacity `C`
that the host decides (section 3.4).

## 6. Ray tracing

### 6.1 Present state

- Parsed: `image_bvh_intersect_ray` (MIMG opcode 0xe6, next-generation encodings
  only, `source/emulator/src/Graphics/ShaderParseBVH.cpp:10`). It requires the 32-bit node pointer
  and eleven address VGPRs (Table 48 of section 8.2.10, BVH A16=0), the fixed header
  (`source/emulator/src/Graphics/ShaderParseBVH.cpp:15`), and a four-SGPR resource.
- Section 8.2.10 requires R128 = 1 for the BVH form ("256 bit T#s are not
  supported"). The 4-SGPR resource is therefore the BVH's own form and is not the
  Table 45 element of section 3.2.
- Rejected with explicit "not implemented" exits:
  - `image_bvh64_intersect_ray` (opcode 0xe7), `source/emulator/src/Graphics/ShaderParseMIMG.cpp:43`;
  - the A16 form of the 32-bit instruction. A16 is MIMG bit [62] (section 13.7),
    which is bit 30 of dword 1. The dword-1 mask at `source/emulator/src/Graphics/ShaderParseBVH.cpp:16`
    (`buffer[1] & 0xffe00000u`) rejects it. Line 15 checks dword 0 only;
  - the BVH64 forms with 12 or 9 address VGPRs (Table 48).
- Lowering: `source/emulator/src/Graphics/ShaderSpirvBvh.cpp:22`
  (`Recompile_ImageBvhIntersectRay_Vdata4BvhAddressSrsrc4`). It returns false unless
  the guest device address is in use (line 25), the address count is 11 and the
  destination size is 4. The T# words are read as SPIR-V values from the srsrc SGPRs,
  and the EXEC-gated store is the only side effect.
- The node-address and node-type rules in the lowering's comment come from the
  RTIP node formats. Those are not in the RDNA2 Shader ISA document and are open
  (section 10.5).

### 6.2 What table-selected ray tracing needs

- The BVH srsrc is a 128-bit resource (4 SGPRs, R128 = 1) and the lowering consumes its
  words as data. A table of acceleration structures therefore uses 16-byte elements,
  read as words under the same guard (section 5.1), with no Vulkan descriptor. The
  element size is the one the BVH form fixes, so `E = 16` is measured per form.
- The lowering already requires the guest device address (line 25). A
  table-selected acceleration structure inherits that precondition.
- The srsrc of a BVH instruction loaded through an SGPR offset has the same
  classification gap as a texture T# (section 2.1). Stage 1 covers it.
- `IMAGE_GET_RESINFO` on a table element would read dimensions from the snapshot
  words. It is not in the base and is open (section 10.4).
- Each Table 48 layout (A16 and the BVH64 forms) needs decode tests and lowering
  before it is admitted. A form is admitted only after a trace shows it.

## 7. Staged plan

Each stage is gated by a Kyty strict run on the current HEAD, recorded with the
`kyty_agent` evidence commands in `docs/agent-tools.md`. Per-title records follow the
content-ID naming rule in `docs/titles/`. A stage is not complete until the frontier
in `docs/BRINGUP.md` is not regressed.

- **Stage 0 (this document).** Design only. No code.
- **Stage 1. Measure the scalar-offset path.** Use the existing `KYTY_TRACE_*` traces
  and `kyty_agent`, with no temporary diagnostic code, on a real title that selects
  descriptors at run time. Record for each S_LOAD form that reaches an image srsrc or
  sampler: the SOFFSET expression, the table base source (user SGPR or not), the
  destination width (8 or 4 SGPRs), the element size `E` the stride implies, the
  element occupancy of `[0, C)`, the maximum index the shader uses, the alignment of
  the offsets, and the bank each element resolves to. Also record whether
  a table range has a pending storage use, a pending write-back, a CP producer, or a
  device-address store in a pending submission (section 5.3). Gate: each form has a
  measured outcome; the base source is confirmed; the bank each measured element
  resolves to is recorded, and the CLASS rule of section 3.3 is checked against it;
  the occupancy and index measurements size the guard cost and the refusal count.
- **Stage 2. Classification, table record and the new queries.** New
  `ShaderDescriptorTable` files. Refuse an unsupported base source, SRSRC form,
  element size or sample form explicitly (section 5.2). Add the non-exiting resolve
  result and the bank it reports (section 3.3), the non-exiting capture result
  (section 3.6), the host-read storage query and the device-address store flag and
  query (section 5.3). Bump `kShaderTranslatorVersion` in this change (3.7). Gate: the
  stage-1 forms are classified as predicted; a unit test covers the placement and
  cause rules (section 8); the strict run shows the table path engaged.
- **Stage 3. Host binding.** `NonUniform` decoration, capabilities, feature gating
  (section 4, with the Vulkan confirmation of the capability mapping), the dedicated
  pool and slots (3.5), the fault ring, immutable snapshots with tracking (3.6), the
  ordering checks and refusal (5.3), the guard and fault bits (5.1), and the
  capacity formula (3.4). Gate: the table path is not landable before the stage-2
  device-address store flag and query exist and check 4 of section 5.3 uses them;
  strict run on the traced title shows the expected draw results; the feasibility
  checks fail explicitly when a device lacks a limit; the refusal count and the
  per-bind snapshot cost are recorded; `TABLE_SETS_MAX` and the repeat bound are set
  from the measurement.
- **Stage 4. Ray-tracing variants.** A16 and BVH64 decode, lowering for each Table 48
  layout, and table-selected acceleration structures. Gate: a trace of each admitted
  form on a real title.
- **Stage 5. Validation.** Strict runs before and after on the titles that touch the
  path, plus `kyty_agent playable-regression` where a scene contract exists. Report
  commit, host, GPU, driver, duration and limitations.

## 8. Tests

Tests are added only for real logic. The design document itself is not tested. The
expected behaviour below is pinned; a pin changes only with a recorded stage-1
measurement and a matching update here.

- Element index, placement and cause (pure function, stage 2): the index `a / E`, the
  placement test, the IN_RANGE and PLACEMENT bits for an access, and the element cause
  for each element kind (zero, buffer, reserved type, a 4-SGPR form refused, class
  mismatch, valid). Real decoding logic with several branches, so it is unit-tested.
- Capacity (pure function, stage 3, `UnitTestEmulatorShaderDescriptorLimits`): given
  the limit fields of section 3.4 and per-stage counts, `C` is the minimum of the
  section 3.4 terms and the policy cap, and the worked example of section 3.4 (`C =
  62`) is an input. `C < 1` returns the named limit failure with the requested and
  limit values. The test asserts the minimum and the failure kind and numbers.
- Layout (`UnitTestEmulatorShaderDescriptorLayoutPlan`): the table plan has one
  sampled binding per class with `count = C` and `PARTIALLY_BOUND`, and the general
  plan built with a table present has the same bindings and `CacheKey` as without
  it. This pins the separation rule of 3.5. Pool sizing is covered by the stage-3
  integration run, not by this pure function.
- Dynamic mappings (`UnitTestEmulatorShaderDynamicMappings`, existing behaviour
  pinned): an inline constant offset feeding a MIMG srsrc still gets a dynamic
  mapping. An SGPR offset gets no constant mapping (`ShaderGetSmemConstantDwordOffset`,
  `source/emulator/src/Graphics/ShaderResources.cpp:289`). The table classification for it is added at stage 2.
- Waterfall (`UnitTestEmulatorShaderWaterfall`): add an image-operation case only if
  the recognizer is extended. Today it covers relative moves only.
- Mimg and SMEM decoding (`UnitTestEmulatorShaderMimg`,
  `UnitTestEmulatorShaderSmemEncoding`): bvh64 (0xe7) and A16 decode cases, once
  admitted (stage 4). Each pins the decoded Table 48 fields, not the rejection text.
- Integration (not a unit test): strict runs before and after each stage, with
  `kyty_agent` evidence, on a title that uses GPU-selected descriptors, including
  one that writes its table on the GPU first (section 5.3).

Existing suites that cover the area: `UnitTestEmulatorShaderDynamicMappings`,
`UnitTestEmulatorShaderDescriptorLimits`, `UnitTestEmulatorShaderDescriptorLayoutPlan`,
`UnitTestEmulatorShaderMimg`, `UnitTestEmulatorShaderSmemEncoding`,
`UnitTestEmulatorShaderWaterfall`, `UnitTestEmulatorShaderResourceBounds`.

## 9. Risks

- Divergent selection is defined only by the guest waterfall (1.3). The design keeps
  the loop as written and does not add a second selection rule.
- A title that accesses an element with a nonzero cause stops with an explicit
  failure. That is the intended contract, but a title that reads only a few valid
  elements is supported only if its accesses stay in the valid set.
- Ordering refuses a draw or dispatch whose table has a pending producer. A title that
  writes a table on the GPU before reading it stops at this refusal. Stage 3 measures
  how often it fires on the traced title (5.3).
- The capacity is device-dependent. On a device with small per-stage limits the table
  is small, and tables larger than `C` fault at run time.
- Per-bind snapshot cost: a changed table costs `C * E` bytes of reads and `C` cause
  words per class. A table that changes at every bind creates a new set at every bind.
  Tracking skips the compare only for an unchanged table.
- The fault slot is sticky for its submission. A faulted submission ends the run, so
  there is no partial recovery path.
- Sampler tables are not measured in this document. They follow section 3 with the
  sampler limit and a 16-byte element, and are not designed in detail.
- The translator version bump (3.7) recompiles every cached shader once per title.
  It is an accepted cost.
- Unverified lead, not confirmed: an exception-based early exit under the project's
  no-exception build could leave a lock held. Do not unwind across texture-cache
  locks; use an explicit failure instead.

## 10. Open issues

- 10.1 The `NonUniform` decoration rule text, the derivative rule behind the
  explicit-gradient form of section 5.1, the mapping of the Vulkan feature
  `shaderSampledImageArrayNonUniformIndexing` to capability 5301, and the Vulkan rule
  that each dynamic storage-buffer offset is a multiple of `minStorageBufferOffsetAlignment`
  (section 3.5) are not confirmed against the Vulkan specification text. Confirm at stage 3.
- 10.2 Stage 1 decides how often one table is read through more than one class. The
  design supports it with one array per class, but the cost of extra arrays is not
  measured.
- 10.3 The depth and integer variant of an element. `PrepareTextures`
  (`source/emulator/src/Graphics/GraphicsRenderBind.cpp:3450`) selects the bank for a resource. This
  document assumes the stage-2 resolve reports the bank per element and that an element
  outside the sampled bank has CLASS (section 3.3). Not verified: whether the bank is a
  function of the T# words alone, which other inputs `PrepareTextures` reads, and whether
  a depth-compare or integer sample form needs a class of its own.
- 10.4 `IMAGE_GET_RESINFO` on table elements is not designed.
- 10.5 The RTIP node-format rules in the BVH lowering are not in the RDNA2 Shader ISA
  document and are not verified against it.
- 10.6 Whether a table base can come from a shader-computed SGPR in a real title.
  Stage 1 measures it. The current design refuses it.
- 10.7 The repeat count of the snapshot read (section 3.6), `TABLE_SETS_MAX` (section 3.5) and the policy cap `C_POLICY`
  (section 3.4) are fixed at stage 3 from measurement.
- 10.8 Registration of this document in the bring-up index is outside this change and
  belongs to the maintainers.
- 10.9 Device-address stores: whether a shader store through a guest pointer is recorded
  against any object over its written range is not verified. The device-address use flag
  (`bind.device_address_used`) is set for loads as well, so it does not by itself identify
  stores. Section 5.3 check 4 has no source of truth until stage 2 adds the store flag and query.
- 10.10 Stage 2 adds the non-exiting resolve result and the non-exiting capture result.
  The current `PrepareTextures` and `GpuMemoryCaptureSnapshotReadOnlyBuffer` do not
  provide them.
- 10.11 GPU-written tables. Two options exist: detect and refuse, or resolve on the
  GPU. This document takes detect and refuse (section 5.3). The
  alternative is deferral through the existing dependency outcomes with a bounded
  retry of 64 attempts (`RetryGuestDeviceAddressDraw`, `source/emulator/src/Graphics/GraphicsRun.cpp:2423-2440`, and
  the compute loop, `source/emulator/src/Graphics/GraphicsRun.cpp:2697-2730`). It is recorded here and not adopted.
  Adopting it needs a maintainer decision, after the stage-3 refusal count on the
  traced title is known.
- 10.12 Cube-array implicit LOD. The general path pins lod 0 for cube-array implicit
  samples (`source/emulator/src/Graphics/ShaderSpirvImage.cpp:572-580`, `array_sample_op`), because
  implicit LOD takes derivatives across cube face seams. The table path does not pin it:
  it keeps the guest's implicit LOD through the explicit gradient of section 5.1. Whether
  the table path needs the same pin for cube coordinates is open. Derivatives at the face
  seams are what the general pin avoids, so the same effect is possible here.
- 10.13 Zero SOFFSET access. Whether a table access whose SOFFSET is the zero form should
  use an existing path instead of the refusal of section 3.2 is not checked here.

## 11. Sources

- RDNA2 Shader ISA (local copy): section 7.2.1 Scalar Memory Addressing (S_LOAD
  address, two LSBs, index * stride, buffer-constant bounds, M0 or SGPR or zero offset);
  Table 27 SMEM Encoding (SOFFSET); section 8.2.1 Image Instructions (DIM); section
  8.2.6 Image Resource and Table 45 (type at bits [127:124], 256-bit column,
  128-bit column without type; T# in four or eight consecutive SGPRs); section 8.2.10
  Ray Tracing, Table 48 and the image_bvh restrictions (R128 = 1); section 12.3 SOP1
  Instructions (S_ANDN1/S_ANDN2 WREXEC and the waterfall example in the S_ANDN2_WREXEC_B64
  entry); section 13.7 Vector Memory Image Format (SRSRC [52:48], A16 [62]) and section
  13.7.1 MIMG (R128: 1 = 128bit, 0 = 256bit).
- Vulkan limit and feature names, from the bundled header
  `source/3rdparty/vulkan/include/vulkan/vulkan_core.h`: `VkPhysicalDeviceLimits` fields
  (lines 2557-2570, including `maxDescriptorSetStorageBuffersDynamic`, and line 2613,
  `minStorageBufferOffsetAlignment`) and the feature fields of `VkPhysicalDeviceVulkan12Features`.
  The header provides field names, not the limit semantics; the semantics used in section 3.4 are those of the Vulkan
  specification, to be confirmed at stage 3 (section 10.1). The minimum values are not
  relied on: the formula reads device values.
- Bundled SPIR-V header `source/3rdparty/vulkan/include/spirv-headers/spirv.h` (decoration and
  capability values).
- Repository files named in this document: `source/emulator/src/Graphics/ShaderResources.cpp`,
  `source/emulator/src/Graphics/ShaderSpirvImage.cpp`, `source/emulator/src/Graphics/ShaderSpirvBvh.cpp`,
  `source/emulator/src/Graphics/ShaderSpirvVector.cpp`, `source/emulator/src/Graphics/ShaderParseBVH.cpp`,
  `source/emulator/src/Graphics/ShaderParseMIMG.cpp`, `source/emulator/src/Graphics/ShaderWaterfall.cpp`,
  `source/emulator/src/Graphics/ShaderDescriptorLimits.cpp`, `source/emulator/src/Graphics/ShaderDescriptorLayoutPlan.cpp`,
  `source/emulator/src/Graphics/ShaderTranslationCache.cpp`, `source/emulator/src/Graphics/ComputePipelinePrecompile.cpp`,
  `source/emulator/src/Graphics/GraphicsRenderDescriptor.cpp`, `source/emulator/src/Graphics/GraphicsRenderDescriptorLimits.h`,
  `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp`, `source/emulator/src/Graphics/GraphicsRenderPipelineLimits.cpp`,
  `source/emulator/src/Graphics/GraphicsRenderDraw.cpp`, `source/emulator/src/Graphics/GraphicsRenderBind.cpp`,
  `source/emulator/src/Graphics/GraphicsRenderCommandBuffer.cpp`, `source/emulator/src/Graphics/GraphicsRenderInternal.h`,
  `source/emulator/src/Graphics/GraphicsRun.cpp`, `source/emulator/src/Graphics/GpuSubmissionTracker.cpp`,
  `source/emulator/src/Graphics/Objects/GpuMemory.cpp`, `source/emulator/src/Graphics/Objects/GpuMemoryCreate.cpp`,
  `source/emulator/src/Graphics/Objects/GpuMemoryWriteback.cpp`, `source/emulator/src/Graphics/Window.cpp`,
  `source/emulator/src/Graphics/Shader.cpp`,
  `source/emulator/include/Emulator/Graphics/Shader.h`, `source/emulator/include/Emulator/Graphics/ShaderDescriptorLimits.h`,
  `source/emulator/include/Emulator/Graphics/ShaderDescriptorLayoutPlan.h`,
  `source/emulator/include/Emulator/Graphics/ShaderTranslationCache.h`,
  `source/emulator/include/Emulator/Graphics/GpuSubmissionTracker.h`,
  `source/emulator/include/Emulator/Graphics/GpuDirtyPageTracker.h`,
  `source/emulator/include/Emulator/Graphics/GuestDeviceAddress.h`,
  `source/emulator/include/Emulator/Graphics/Objects/GpuMemory.h`.
