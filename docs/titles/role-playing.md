# Role-playing title

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Role-playing title: merged geometry stage fills the grading volume (2026-10-20)

Intel A770, strict runs of the traveler-selection route on 0d25769e plus the
changes below, with temporary diagnostics (removed).

- **Result.** The traveler-selection map, black before (mean 39/32/26),
  is lit (mean 96/81/23) in a 100 s strict run at about 40 fps. A 120 s run
  of the build without diagnostics shows the same map (97/82/24). It exits
  cleanly, but its last capture (about 110 s, in the story start after the
  selection) is black, and the agent reports no present for 6.9 s at exit.
  The colour balance has not been checked against a reference image.
- **Merged ES+GS draws run as a compute pass.** A draw whose stages are
  ES plus a merged NGG GS runs the linked program as one compute workgroup
  per guest subgroup. A prologue stands in for the geometry engine: it sets
  the vertex and primitive counts in `s3`, the corner slots in `v0`/`v1`,
  and the primitive, vertex and instance ids. The program's exports and its
  `GS_ALLOC_REQ` go to a transient device buffer. A host vertex stage then
  draws that buffer as triangles. Unallocated, null and out-of-range
  primitives collapse to the origin, and `pos1.z` supplies the layer when
  `USE_VTX_RENDER_TARGET_INDX` is set. The compute stage has no
  vertex-input stage, so the ES reads its streams through the buffer
  descriptors it loads, at guest device addresses.
  - The front half's user SGPRs (0d25769e). The HLE fuse of the two halves
    copied the back half's `RSRC2_GS` (user SGPR count 0); the front's
    register declares 8. The constant buffer at `s[8:11]` was then not bound.
  - A readback of the output buffer for group 0 of the 32-instance draw:
    header 0x2006 (6 vertices, 2 primitives), primitives (0,1,2) and
    (3,4,5), positions at the quad corners (+-1), layer 0, UV in `param0`.
- **Volume colour targets.** Both layered draws target 3D surfaces
  (`CB_COLOR_ATTRIB3.RESOURCE_TYPE` 2): a 64x64x64 16_16_16_16 target and a
  32x32x32 2_10_10_10 target, tile mode 27. `SLICE_MAX` equals the depth
  (one past the last slice). The texture path already reads a tile-27 volume
  slice by slice with the array layout, so only the host image type changes:
  a 3D image (one mip, one sample, guest size) with 2D-array-compatible
  views. Each slice is attached as a 2D view, and sampling uses a 3D view.
- **Writer pixel shader: uniform select of EXEC.** The 32x32x32 writer
  stopped with "fragment wave transport is required" (proof ExactSubgroup,
  no host Wave64 on Intel). The mask flow treated `s_cselect_b64 vcc, exec,
  0` on a compare of a uniform scalar as divergent, so a scalar written on
  one arm of `s_cbranch_vccz` counted as divergent after the join. Such a
  select gives every active lane the SCC bit, as `s_and` of EXEC with a
  uniform mask does. It is now uniform when SCC is clean and EXEC still holds
  a lane of every launched quad (no `kOuter`/`kMissesQuad`). Each host
  subgroup then holds an active lane and decides as the guest wave does.
  Under a lane-narrowed EXEC it stays refused, and a unit test checks that.
  The program is now LaneLocal.
- **If/else merge when both arms end in `S_BRANCH`.** The same program then
  failed SPIR-V validation ("branches to the selection construct, but not to
  the selection header"). At 0x3338 the fallthrough arm jumps into the taken
  arm's tail (0x3458), whose own `S_BRANCH` leaves for the outer join
  (0x35ac). The merge was taken from the taken arm's branch because that arm
  ended in `S_BRANCH`, which placed it outside the enclosing construct. The
  merge is now the landing of whichever arm jumps into the other, and the
  fallthrough arm comes first. A unit test reproduces the validator error
  without the fix.
- **Not this defect: the well platformer's d0354e07 stop.** That program
  fails validation with the same message, but its cause is different. A
  uniform `s_cbranch_scc0` at 0x38 targets 0x98. Its fallthrough masks EXEC
  and then `s_cbranch_execz` skips to 0xe0, past 0x98. `FindReconvergence`
  rule 1 (`ShaderSpirvScJoin.cpp:540`, "fallthrough reaches taken") merges
  the uniform branch at 0x98. The fallthrough can bypass 0x98, so the real
  post-dominator is 0xe0, and 0x98 is shared by two regions that are not
  nested. The stop has appeared at 77-122 s on the new-game route since
  f7aad0aa and never in the runs of 2026-10-19 and earlier. The suspect
  range is 10611591..f7aad0aa. It is not fixed. Two directions: drop an
  `s_cbranch_execz` whose skipped range has only EXEC-guarded effects and
  whose scalar definitions are dead at the target (here `vcc` is dead after
  0xe0); or require post-dominance in rule 1 and restructure.
- **Open.**
  - The hull/domain fusion path of these programs is not exercised.
  - The register window defect: fetch reads `s4..s7` regardless of the
    user SGPR count, while the descriptor binding follows the count
    (`Shader.cpp` input info vs `Gen5SharpNeedsEud`). With the front-half
    fix both agree for this program; the rule itself is unresolved.
  - The story scenes have not been re-checked with the filled table.

## Role-playing title: story exposure and colour grading (2026-10-20)

Intel A770, strict runs of the story route on 7ec32b45..50adfe84 plus
temporary diagnostics (removed).

- **Symptom.** Story scenes are dark and tinted. Every scene pixel has one
  channel at zero and the other two equal: (0, v, v) on the ship, (v, 0, v)
  in the night town. The HUD keeps its colours.
- **Exposure inputs (fixed, 7ec32b45).** The exposure histogram pass (pixel
  shader writing a 16x2 RGBA32F target) has `SPI_PS_INPUT_ENA` 0x220:
  linear centre and `POS_Y_FLOAT` only. Its row test reads `v2`. The native
  prolog filled `v2`/`v3` only when both X and Y were enabled, so `v2` was
  an uninitialised variable. A dump of the 16x2 target and the three 1x1
  exposure targets read the same constants every frame (1.414, 1.414,
  0.236, 0.707). Two more pixel shaders had position masks 0x102 and 0x202.
  Now each enabled position field 8-11 stores its `gl_FragCoord` component
  (x and y scaled to guest pixels) into its own register. After the fix
  the histogram row holds per-bin values and the exposure value changes
  from frame to frame (0.0073, 0.0066, 0.0071 at presents 4001 and 4501).
  The brightness of the presented frame did not change: the exposure was
  wrong, but it is not the cause of the dark tinted image.
- **Colour grading volume (open, cause localized).** The last scene pass
  before the display copy encodes colour with a log curve (scale 1/14,
  offset 0.6107) and samples a 32x32x32 volume at `x * 31/32 + 1/64`.
  - A bind trace (one run, 200 s) shows that this pass samples a format 50
    (2_10_10_10) mode-27 32x32x32 volume. That volume is uploaded once, at
    present 2, from guest memory that is all zero, and never again.
  - No RenderTexture, StorageTexture or StorageBuffer is created over its
    range, and no emitted draw has a colour target at its base, so no
    tracked GPU write ever fills it.
  - The other 32x32x32 volume in these runs (format 71 RGBA16F, tile 9, a
    CPU upload at present 336) is not a grading table. Its values are
    signed (about -4 to +4), alpha is 0, and no channel rises along an
    axis. The detiled data is smoother than the same bytes read as linear,
    and equally smooth along x, y and z (mean neighbour difference 2.0,
    2.2, 2.1 against 2.6, 4.3, 6.1). That supports the standard-volume
    detile.
  - Every run reports skipped draws with reason `unsupported_ge_state`
    (more than 1,000 by 47 s), all from one program. The first is at
    present 2, the present at which the volume is created. Its state:
    stages 0x2030 (ES and merged GS back, NGG), `max_vert_out` 3,
    triangle-strip output, and `PA_CL_VS_OUT_CNTL` 0x1240000, which sets
    `USE_VTX_RENDER_TARGET_INDX`.
  - A disassembly of the retained ES and GS programs shows a pass-through
    geometry stage. The ES loads a position and a UV, and stores the
    instance id plus a constant as the layer in LDS. The GS reads each
    input triangle, emits it unchanged as a three-vertex strip, and
    exports `prim`, `pos0`, the layer in `pos1.z` and the UV in `param0`.
    One instanced draw thus writes one quad into each slice.
  - The renderer does not run merged ES+GS programs (see the robot
    platformer's skipped geometry-shader draws), so the table stays zero.
    The skip report does not record the colour target, so the target
    address of the skipped draw is inferred from its timing and layer
    output, not observed. Why a zero table gives two equal channels
    instead of a black image is not explained yet.
  - Next: execute merged ES+GS programs, then check that the grading
    table fills and the tint disappears.
- **Stops removed (not seen again after the fix).**
  - b3fa459b: a 16x1020 RGBA16F mode-27 storage view of 0x100000 bytes
    over an idle 0x90000 StorageBuffer whose writes were already written
    back stopped on `StorageBuffer - IsContainedWithin - StorageTexture`.
    Such a buffer holds the guest bytes, so the storage image seeds from
    guest memory and links to it.
  - 50adfe84: a 512x512 BC3 texture (tile 9) over a 512x512 D16 target at
    the same address and size stopped on `DepthStencilBuffer - Equals -
    Texture`. The texture uploads guest bytes and links to the depth
    object. Open: the texture object is 0x80000 bytes. The standard 64 KiB
    layout of a one-level 512x512 BC3 texture is 0x40000, and the reason
    for the difference is unknown.
- **Other open items in these runs.**
  - `!create_all_the_same` at 0x2109b90000 (size 0x120000), 127 s, one run.
  - Device lost (`vkWaitSemaphores` result -4), 103 s, one run of six.
  - A guest write to address 0 (status 139, rip in guest code) at 138 s,
    one run.
- **Regression sweep.** The eight-title sweep on the same build reached the
  same states as the previous sweep. One run of the well platformer
  stopped at 77 s: a pixel shader (crc d0354e07) failed SPIR-V validation
  ("branches to the selection construct, but not to the selection
  header"). Four more runs, one with an empty SPIR-V cache, did not reach
  that shader. Not attributed. The change only touches the pixel prolog,
  not control flow.

## Role-playing title: world vertex inputs read the wrong data (2026-10-20)

- **Symptom.** At story start the world drew as large stretched triangles
  over a dark red frame. The normals target held position-like gradients and
  the frame ran at about 9 fps.
- **Evidence (strict Intel runs, temporary diagnostics removed).**
  - The world vertex program reads six streams with `BUFFER_LOAD_FORMAT`.
    Position and an instance row come through V#s it loads from the
    attribute buffer table, with the format and DST_SEL taken from the
    attribute table at run time. Tangent frames (format 57, R8G8B8A8_SNORM,
    stride 4, two per vertex) and two UV sets (format 29) come through
    user-data V#s, at computed indices (`2*v+1`, `2*v`, `s1*v+k`).
  - A CPU read of the guest data at the draw (index buffer, position stream,
    instance row, per-draw constants) found every index in range
    (max 1298 of 1299 records), finite positions and an orthogonal world
    matrix. The guest data was sane.
  - The translated program read every one of the six loads from the same
    vertex attribute (`%attr0`, the position stream): the fetch detector
    typed any format load as an attribute fetch and only logged when its
    operands did not match the pattern. Register roles also survived later
    writes, so a load whose index register had been overwritten still
    counted as indexed by the vertex id.
- **Contract implemented.**
  - A format load is an attribute fetch only when its index is the
    vertex/instance select (`v_cndmask v8, v5`), its V# was loaded from the
    buffer table at an attribute-derived offset, and its SOFFSET comes from
    the attribute table. Any other format load stays a buffer load through
    the live V# path. A register loses its role when any instruction writes
    it (`DetectVertexFetchSemantics`).
  - Gen5 `BUFFER_LOAD_FORMAT_*` decodes the element in the format of its
    V# (RDNA2 ISA table 31: data format and DST_SEL come from the resource;
    table 47: the format list). Each channel is converted to its number
    type (UNORM, SNORM with a -1 clamp, USCALED/SSCALED, UINT/SINT, FLOAT16,
    11- and 10-bit floats), then DST_SEL is applied: SEL_0 gives 0, SEL_1
    gives 1 (integer 1 for UINT/SINT formats, 1.0 otherwise), a channel the
    format lacks reads 0. Out-of-range elements read 0 (SEL_1 still 1).
    `ShaderGen5BufferFormat` holds the table; the SPIR-V helpers are in
    `ShaderSpirvBufferFormat.cpp`, used by bound and runtime-address loads.
  - Packed formats name channels from the high bits, so X is the low field:
    format 36 (10_11_11) matches the B10G11R11 image format and 50
    (2_10_10_10) matches A2B10G10R10 (`VulkanImageFormat.cpp`). Formats
    43-49 follow the same convention; no title has been seen using them.
  - The integer value of SEL_1 on UINT/SINT formats follows the ISA's
    integer-return rule; no live trace has confirmed it.
  - `MTBUF` and the legacy (Gen4) typed helpers are unchanged: their format
    numbers are dfmt/nfmt pairs (36 = 32_UINT, 39 = 32_FLOAT), not this
    table.
- **Unit tests.** `Gen5BufferFormatsCoverIsaTableAndAgreeWithVertexInput`,
  `FormatLoadsDecodeDescriptorFormatOnBothPaths`,
  `OnlyAttributeTableFormatLoadsAreVertexFetches`.
- **Instance streams (the cause of the stretched triangles).** With the
  loads above decoded, a per-shader dump of the normals target still showed
  stretched triangles from two world vertex programs.
  - Bit 26 of a Gen5 vertex attribute word selects the instance index; the
    program itself picks `v8` (instance id) or `v5` (vertex id) with a
    `v_cndmask` on that bit. A temporary attribute log (removed) showed
    dozens of world programs with instance rows (format 77, four
    components) and an opaque depth prepass with a one-component instance
    attribute.
  - `ShaderParseAttrib` ignored the bit and the Vulkan layout stepped every
    binding per vertex, so instance matrix rows came from per-vertex
    records.
  - Contract: an attribute with the bit set is an instance stream. Streams
    of different rates never share a binding, the binding steps per
    instance, and it fetches records `[0, first_instance + instance_count)`
    with no vertex offset. Vertex-record limits ignore instance streams, and
    the vertex shader identity includes the rate.
  - Unit test: `InstanceAttributeStepsInItsOwnBinding`.
- **Result.** The next strict run's normals target at frame 4600 showed the
  temple, stairs and sprites with no stretched triangles, and the story
  scene runs at about 15 fps (from 9). The frame is still dark: open, see
  below and "the environment cube read its cleared storage alias".
- **Open: intermittent stops in the story load window (frames 1300-2600).**
  Each one appears in some strict runs and not in others with the same
  binary and input route. They are more frequent in runs slowed by
  per-draw diagnostics.
  - `unstable or unreadable Gen5 EUD snapshot: dwords=16`
    (`Shader.cpp`, the snapshot loop in the resource parse) at frame 2530:
    two reads of the EUD table differed twice, or the read failed. The
    message now names which (`unstable` or `unreadable`) and gives the table
    address and pointer register.
  - `shader emitter missing` for `s_load_dwordx4 s[32:35], s[28:29], 320`
    at pc 4 of pixel shader crc `513ca09c` (`ShaderSpirvGenerator.cpp`,
    the emitter-missing exit) at frame 2196: `recompile_sload_from_extended`
    found no mapping for EUD dwords 80-83.
  - A sampler bound with words `e2000005 e1000005 e1000005 e0000005`
    (`GraphicsRenderPipeline.cpp`, the sampler-mode exit) at frame 1331.
    These words do not look like an S#.
  - Suggested direction: all three read a descriptor table that the guest
    may be rewriting. Check whether the command processor reads a table
    after the guest has been told that the draw which used it has
    completed (label or EOP ordering) before relaxing any check.

## Role-playing title: the environment cube read its cleared storage alias (2026-10-20)

- **Symptom.** The story scene's reflection cube was constant: the tile-9
  cube the scene samples held one value in every texel, and the GGX
  prefilter wrote `(0, 10000, 0, 0)`. `10000` is `1/max(x, 1e-4)` with
  `x = 0`, the filter's response to an all-zero source.
- **Capture chain (strict Intel runs, temporary diagnostics removed).**
  - A compute pass clears two 128x128 format-71 cubes (tile 27, eight
    levels, six slices) through storage views to `(0, 10000, 0, 0)`.
  - A pixel shader copies a 32x32 guest cube into level 0 of each face of
    the first cube (alpha 1, mean about 0.13). A second one generates
    levels 1-7 from the same cube with LZ sampling. A third adds a constant
    pass. A fourth prefilters into the second cube (`ImageSampleL`, lod > 0).
  - A compute pass copies the second cube from tile 27 to tile 9, and the
    scene samples the tile-9 cube.
- **Evidence.**
  - Every capture, mip and filter draw targets the final six-slice target at
    the right slice and level (excluded: the capture draw missing face 0;
    the mip draws targeting a retired prefix image).
  - The mip and filter shaders sampled the storage image the clear wrote,
    not the target the draws had filled. So level 1 read back the clear
    value and the filter read zero.
  - The cube's range also held a stale 708x295 render target (format 44,
    size 0x120000) with the same address and size. `GpuMemory::FindFast`
    returns only the first exact object, so the exact render-target lookup
    returned that stale target alone. The alias picker rejected it, and the
    bind fell through to the storage image.
- **Contract implemented.** An exact render-target lookup returns every
  exact alias of the range (`FindRenderTexture` uses
  `GpuMemoryFindEqualObjectsForSubmission`, as `FindLatestStorageTexture`
  already did), and callers choose among them. DCC decompression, which
  needs one backing, now takes the single alias that matches the guest
  extent and format and stops when none or several match.
- **Layered mip-chained targets.** The cube is a six-slice target whose
  slices each hold a whole mip chain.
  - The slice pitch is the chain size (`DescribeRenderColorMipChain` returns
    it to `ResolveRenderTextureArrayView`), not the level-0 size.
  - Mip-chained targets may have several slices. The guest fill detiles
    each slice's chain on its own, and copying a target into a grown one
    copies every level both images hold.
  - Unit test: `RenderTarget64KBChainIsTheArraySlicePitch`.
- **Result.** In the dumps taken after the copy pass, level 0 of face 0 of
  the filtered cube and of the tile-9 copy match: mean `0.135 / 0.138 /
  0.155`, alpha 1, no `10000`. The final story frame did not change: dark
  teal silhouettes, the same mean as before. The dark scene therefore has
  another cause and stays open.
- **Open: a stale boot render target blocks a recycled range.** The 708x295
  target is the first object the title creates (id 1). It stays linked
  after the guest reuses its range for a tile-9 texture and the cube.
  - Twice on 2026-10-20 the one-slice cube storage view stopped on
    `!create_all_the_same` (`GpuMemoryCreate.cpp:2507`). The parents were
    that target (Equals, last device write at counter 866479) and a
    0x160000 tile-9 texture created later (Contains, origin Unknown, write
    time 2426327).
  - The texture skipped its guest upload because the stale target counted
    as a live cover (`GraphicsRenderBind.cpp`, `live_cover`). It therefore
    holds neither guest bytes nor the target's content.
  - Write-back invalidation does not reach the target: storage images are
    not written back, and GPU-owned targets are skipped there.
  - Suggested direction: when a device write through a linked view covers
    a render target's whole range, mark the target stale. A stale target
    should count neither as a live cover nor as a creation parent. This
    changes render-target lifetime for every title and needs its own sweep.
- **Open: intermittent device loss.** Five of twelve story runs on
  2026-10-20 lost the device (`vkQueueSubmit`, `vkWaitForFences` or
  `vkWaitSemaphores` result -4; the kernel driver logs a job timeout). Two
  of them were at 105-115 s, before the cube chain ever runs, and three at
  221-227 s. Four runs held the whole 300 s. The losses occur with and
  without the lookup change, so the cube chain is not their cause.

## Role-playing title: mipmapped storage arrays and cube chains (2026-10-19)

- **Symptom.** At story start the traveller role-playing title stopped on
  `unknown relation: StorageTexture - Contains - StorageTexture`, then, once
  that passed, on `Unsupported Gen5 2D-array layout: format=179 32x32
  levels=6 tile=1 layers=6 type=11`.
- **Evidence (strict Intel runs, temporary diagnostic removed).**
  - The live backing is a 128x128 format-71 array: eight levels, tile 27,
    six slices of 0x30000 bytes (a cube).
  - Compute passes at frames 1116 and 3433 store into it through one-slice
    descriptors: `base_level` 1..7 for each `base_array` 0..5, then sample
    the whole cube. This is per-face mip generation.
  - Array storage images were created with one host level, and the
    descriptor window was the fixed `VIEW_STORAGE_ARRAY` slot. A one-slice,
    level-1 descriptor had no matching view.
- **Contract implemented.**
  - Gen5 arrays and cubes on tiles 5, 9 and 27 get a storage backing with the
    whole chain (`StorageTextureUsesMipBacking`). Upload covers every level
    both the layout and the image hold.
  - A storage descriptor of a live array is a view of it: its slice window
    (`StorageTextureGetArrayViewRanges`) at its base level, one level. The
    view comes from a bounded per-image cache freed with the image
    (`StorageTextureResolveArrayStorageView`,
    `VulkanGetOrCreateCachedImageView`).
  - A smaller descriptor at the base of a live array with the same format,
    pitch, extent, chain, tile and swizzle reuses that array
    (`StorageTextureCanReuseLargerBacking`) instead of creating a second
    image.
  - Standard256B (tile 1) chains in arrays and cubes: every slice stores its
    whole chain, smallest level first, with no tail; BC formats included
    (`Gen5GetTextureArrayLayout`, `Gen5DetileStandard256BTextureMipChain`).
- **Unit tests.** `StorageTextureArrayDescriptorsAreViewsOfTheLiveArray`,
  `StorageTextureArrayStorageViewSelectsDescriptorWindowAndLevel`,
  `Standard256BMipChainArrayStoresOneChainPerSlice`.
- **Next stop: a layered target across two guest mappings.** A 32x32
  RGBA16F cube target (tile 27, six levels, six slices of 0x10000) stopped
  at `layered render-target backing is not fully allocated` while drawing
  slice 5. The registered heap at its base covered 0x50000 of 0x60000; the
  next 0x10000 was a separate, adjacent registration.
  - A temporary mapping log (removed) recorded 14417 GPU mapping
    registrations in 240 s and no release: 11793 of them are 64 KiB, and
    13590 adjacent pairs abut exactly. The guest maps GPU memory in small
    contiguous pieces, so one resource may span several.
  - Fix: the layered-target check asks whether adjacent registrations cover
    the whole backing (`GpuMemoryGetMappedRangePrefix`), not whether one
    heap does. Unit test: `GpuMemoryMappedPrefixSpansAdjacentAllocations`.
  - Found, not fixed: `GpuMemory::GetHeapId` (`GpuMemory.cpp:308`) files an
    object that spans mappings under its first heap only. Overlap queries
    rooted in a later heap do not see it, so a view at base+0x50000 of that
    cube would not link to it. A design that coalesces adjacent mappings
    needs a split-on-unmap path; no release was observed to exercise one.
- **Storage image inside a CPU-uploaded storage buffer.** The 128x128
  eight-level view at the base of a recycled range stopped on
  `!create_all_the_same` (also seen on 2026-10-18, before this work). Its
  parents: a live 708x295 target (Contains), a read-only CPU-uploaded
  0x1fa400 storage buffer (Contains) and read-only vertex buffers (inside
  and crossing). The storage buffer had no rule. It only caches guest bytes,
  so it is linked like a sampled texture
  (`GpuMemoryAllowsStorageTextureOverStorageCache`), alone or with the
  other parents.
- **Image read through a buffer-typed T# (inexact contract).** Stops at
  random frames on `shader emitter missing ... ImageSample* ... tex_index=-1`
  blocked about half of the strict runs, with a different shader each time.
  - Evidence. For one stop the T# user SGPRs held `00000600 00fff000
    06f00000 00000000` twice: two sampler descriptors (word 1 is MAX_LOD
    0xfff), whose dword 3 type nibble is 0, a buffer encoding.
  - A temporary packet ring (removed) recorded the gfx packets and the
    SH-indirect pairs before the stop. The pixel shader bind did not change
    before the failing draw. Both SH-indirect blocks were applied pair for
    pair (6 and 12 entries, as their headers state). No other pixel-shader
    update appeared in the window. The submit snapshot already captures
    those blocks. So the guest itself left user data from another layout
    under an unchanged pixel shader.
  - The ISA defines only an all-zero resource as unbound. A descriptor of
    buffer type under an image instruction is undefined there. Two
    independent implementations bind a null image for a T# whose address is
    0, whose format is invalid, or whose base level is out of range. Neither
    checks the type nibble, so they only suggest that an unusable T# reads
    as unbound.
  - Contract: an image read whose T# is still the entry user data and holds
    no image descriptor reads zero, like an all-zero T#: every dword zero, or
    a type nibble of 0-7. A T# of image type that fails to resolve still
    stops. A T# loaded from memory is covered too; see "Descriptors loaded
    from memory that bind nothing" below.
  - The shader identity already carries the zero-image set, so a later draw
    of the same shader with a real image compiles its own variant.
  - Each non-zero application raises a bounded `image_read_non_image` agent
    event (crc, register, words 0 and 3), so a wrong application stays
    visible.
  - Unit test: the type cases in `SamplesThroughUnboundDescriptorReturnZero`.
- **Export to a target the CB does not write.** A second intermittent stop
  was `shader emitter missing ... Exp ... mrt_color0 ... compr` on the
  UINT16_ABGR export of the copy into an R16_UINT target.
  - The failing compile had `CB_TARGET_MASK` and `CB_SHADER_MASK` both 0.
    MRT0 carried SPI_SHADER_COL_FORMAT 7 (UINT16_ABGR) and a leftover FLOAT
    number type in CB_COLOR0_INFO. The good compiles of the same shader had
    mask 0xf and a UINT target.
  - The CB writes a target only through the channels both masks enable, and
    the draw already masks the attachment write the same way
    (`ResolveColorWriteMask`). `ShaderGetInputInfoPS` now gives every other
    target export mode Zero, so the shader drops an export the hardware
    discards. No visible result changes; the stop is gone.
- **Dropped indirect register pairs are visible.** CX, SH and UC indirect
  blocks drop pairs outside their bank. That drop was a debug log, never
  seen in Silent runs. It is now a bounded `indirect_register_dropped` agent
  event.
- **Cube storage view over other recycled ranges.** About one strict run in
  ten stopped at story entry while the same one-slice storage view of the
  128x128 cube (params `4653056 128 ... 4012 13`, varying base level and
  slice) was created over a recycled range.
  - 8 of 133 runs stopped on `!create_all_the_same`, 6 on `unknown relation:
    Texture - Contains - StorageTexture` with one CPU-uploaded 0x1e0000
    texture as the only parent.
  - Parent mixes captured since (`create_dbg_parents`, now with each
    parent's device write time): the CPU-uploaded storage buffer (Contains)
    with 707x294 and 708x295 targets (Equals) and a CPU-uploaded texture
    (Equals) plus 66 vertex buffers; the storage buffer with two targets
    (Contains) and a texture materialized from one of them (Contains); a
    CPU-uploaded texture (Contains) with an earlier storage view of the
    first slice (IsContainedWithin, a cube growing from one face to two).
  - Contract: a CPU-uploaded texture caches guest bytes like the storage
    buffer, so a storage view inside it or equal to it is linked
    (`GpuMemoryAllowsStorageTextureInsideCpuTexture`, Contains or Equals).
    The link applies only while the texture holds no device write since its
    last upload (`device_write_time < gpu_update_time`, both stamped from
    the same strictly increasing counter) and has no pending write-back;
    otherwise the GPU bytes win and the view is not linked. The same rule
    is used in the single-parent path, the multi-parent path and the
    storage-growth path, so all three accept the same mixes.
  - A storage view inside a texture materialized from a target is linked
    for Contains as well as Equals
    (`GpuMemoryAllowsStorageTextureOverMaterializedTexture`): the
    materialized texture already follows its target's GPU bytes.
  - Unit test: the Equals and alias-materialization cases in
    `ReclaimsReadOnlyVertexBuffersUnderStorageTexture`.
  - Result: three 270 s story runs after the change reached the story
    without the stop. One clean run proves little at a one-in-ten rate; the
    route is repeated in the sweep below.

## Role-playing title: selection screen, story entry and stop fixes (2026-10-19/20)

Each fix below came from a strict Intel run of the role-playing title on the
traveller-selection route. Temporary diagnostics are removed.

- **NULL-target discard tail.** A dithered fade shader produced SPIR-V that
  failed validation when the menu opened. Its exit block is `exec = 0`,
  `exp null vm done`, `s_endpgm`, and `IsDiscardInstruction` only recognized
  the null-MRT form of that tail. The branch into it therefore was not
  emitted as a terminating arm. The NULL target carries only the valid mask,
  so it is now the same tail. Unit test (fails before the change, with
  validation on): `NullTargetDiscardTailInsideSkipRegionIsTerminatingArm`.
- **Stencil reference.** `get_stencil_op` returned the op value
  (STENCILOPVAL) as the Vulkan reference for ADD/SUB clamp and wrap and for
  INVERT. Vulkan's increment, decrement and invert ops never read the
  reference, so that value replaced the compare value (STENCILTESTVAL) for
  the whole face. These ops no longer claim the reference. Open
  (pre-existing): ONES replaces with 0xff, and REPLACE_OP with an op value
  other than the test value, still displace the compare value; Vulkan has
  one reference per face. In this title the ops that use the reference
  appear only with compare ALWAYS.
- **Sampler W address mode (CLAMP_Z).** A sampler with `clamp=0,0,4` (W
  mode ClampHalfBorder, which has no host mode) stopped the run at frame
  1086 while it sampled a 2D-array view. The W mode wraps the R coordinate
  of 3D views only; 2D views have none, and array and cube views select a
  layer or face without it. The sampler key now records whether any paired
  view is 3D (`BindSamplerUsesRCoordinate`), and the W mode is resolved only
  then. The next run passed frame 1086. Unit test:
  `SamplerZClampAppliesOnlyToVolumeViews`. The coordinate-lowering stop now
  prints the four sampler words.
- **Pixel-shader stage elision.** An opaque depth prepass (target mask 0,
  shader mask 0, no kill, no Z export) inherited a pixel shader from an
  earlier 1024x1024 pass, and that shader samples the depth target the
  prepass writes. It stopped on `same-draw depth sampling conflicts with
  attachment writes`. All 40 prepass draws of a later run had that
  inherited shader. A shader with no unmasked color target, no depth,
  stencil or mask export, no kill, no memory effect and no alpha-to-mask is
  not observable; depth and stencil tests run without it. So
  `PixelShaderStageRequired` no longer counts depth/stencil enable or an
  export format alone. `DB_ALPHA_TO_MASK` is now decoded, because
  alpha-to-coverage of the MRT0 export does need the shader. The next run
  held 232 s with no stop. Unit test:
  `RequiresPixelShaderOnlyForObservableFragmentWork`. The depth-conflict
  stop now names the sampling stage, slot, operation and address.
- **HTILE clear of a combined depth/stencil target.** The selection map draw
  (depth GEQUAL, no depth write) passed no fragment (`any_passed=0` from an
  occlusion query on the draw). The guest clears the 4K D32S8 HTILE with the
  generic uniform-fill kernel (word 0). That clear was not applied, for
  three reasons:
  - The HTILE storage buffer was cut at the end of its first mapping
    (`655360` bytes requested, `589824` mapped in one registration). Storage
    buffers now use `GpuMemoryGetMappedRangePrefix`, like layered targets.
  - The HTILE sits in another guest mapping than the depth plane, and object
    creation searches one mapping, so the buffer was never marked as the
    target's metadata. `GpuMemoryLinkDepthMetaStorage` marks the storage
    buffer that exactly covers the HTILE when the depth target is
    materialized.
  - A zero HTILE word with stencil tiled in HTILE (no
    `TILE_STENCIL_DISABLE`) means depth = `DB_DEPTH_CLEAR` and stencil =
    `DB_STENCIL_CLEAR` (SMem 0). The translation required the draw to clear
    stencil to zero as well and otherwise dropped the fill. It now clears
    both planes of the whole image whatever the draw's clear bits say.
  - Result: `any_passed=1`, the clear is published and consumed. The route
    moved past the map before a capture of it was taken.
  - Also seen there: a small vertex buffer inside the active depth target of
    a recycled range stopped on `DepthStencilBuffer - Contains -
    VertexBuffer`. It is linked like the Crosses form
    (`GpuMemoryAllowsVertexContainedInSurface`).
- **Descriptors loaded from memory that bind nothing.** An intermittent stop
  on `unallocated gpu object range` (frames 1084-1096, before the map
  draw) came from a T# that a shader loads from extended user data with an
  S_LOAD, where those words held a buffer descriptor: read as a T#, its base
  became a garbage address.
  - The unbound-T# contract above now covers loaded descriptors. A
    read-only T# of buffer type loaded by an S_LOAD reads zero for the reads
    up to its last consumer, and an S# load whose every consumer reads such
    a T# binds nothing either.
  - Those S_LOADs write zero to their destination SGPRs instead of reading
    guest memory (`ShaderZeroDescriptorLoads`). The first version dropped
    the load and stopped on `shader emitter missing ... SLoadDwordx8`.
  - The zero-image set holds 32 entries (from 16), and the zero-load set two
    per image: a shader stopped on `shader zero descriptor load capacity
    exceeded: count=16`.
  - The shader identity includes the load sites, so a draw whose table holds
    a real image compiles its own variant. The stop did not recur in five
    later runs.
- **Gen5 64 KiB block-compressed arrays.** A one-layer tile-9 BC array view
  (`format=177 128x128 levels=8 tile=9 layers=1`) stopped on `Unsupported
  Gen5 2D-array layout`. `Gen5GetTextureArrayLayout` excluded BC formats
  from the 64 KiB chain; the 2D path already lays tile-9 BC chains out that
  way, so arrays now do too. Unit test:
  `Standard64KBBcArraySliceMatchesTwoDimensionalChain`.
- **Storage growth and cache sets on recycled ranges.**
  - A one-slice cube view growing over its prefix may also overlap a live
    render target it links (a view inside one, or a redescription of its
    bytes that no render-alias copy applies to), CPU-uploaded caches,
    a texture materialized from that target, and read-only vertex buffers.
    The growth path accepts those parents with the same rules as a storage
    image over caches. The prefix image may link only parents of the new
    view.
  - Four small CPU-uploaded read-only storage buffers inside a new 0x100000
    storage image stopped on `unknown relation: StorageBuffer -
    IsContainedWithin - StorageTexture`: a set made only of caches was
    excluded from the cache rule when all parents had the same relation. It
    is now linked like any other cache set.
- **DCC clears through constant and loaded fills.** See
  `docs/graphics-compute-uniform-color-fill.md`. Unit tests:
  `ComputeUniformBufferFillRecognizesConstantStore`,
  `ComputeUniformBufferFillRecognizesLoadedStore`.
- **Diagnostics.** `shader emitter missing` now prints the shader crc.
- **Open.** A sampler with `clamp=7,7,7` and unnormalized coordinates
  (words `ffffffff 00000000 00000000 00000000` in one run) stopped twice at
  `unsupported sampler coordinate lowering`. The words do not look like an
  S#; it belongs with the descriptor-table stops listed under "world vertex
  inputs".

## Role-playing title: story start, three stops removed (2026-10-17)

Route: the menus route until 70 s, then cross every 2 s. It starts a
traveler's story at about 85 s. Intel Arc A770, strict, Silent.

### 1. `SAMPLE_CD` slopes after a restored WQM copy (5e5d54a4)

- Stop: `shader emitter missing ... inst=ImageSampleCd` at pc 0x3dc, 0x4a8 or
  0x4fc, depending on which shader compiled first.
- Shape, in four pixel shaders (ray-march passes, four `SAMPLE_CD` each):
  - `S_WQM_B64`, then `S_MOV_B64 sX, exec` inside the plain WQM prefix.
  - Each loop narrows EXEC (`V_CMPX`, `S_ANDN2_B64 exec`). After it,
    `S_MOV_B64 exec, sX` restores the copy.
  - The next loop's slopes are fixed quad broadcasts (DPP quad_perm
    0x00/0x55/0xaa/0xff) subtracted after that restore.
  - The fourth loop reuses slopes built after an earlier restore, untouched
    across the narrowed loop in between.
  - Sources include `V_ADD_F32`, `V_MUL_F32` and a `V_MAC_F32` whose
    accumulator was written in the prefix.
- The proof accepted only slopes built in the prefix. The `SAMPLE_CD` emitter
  needs quad-uniform user gradients.
- Contract implemented (`ShaderImageGradientProof.cpp`):
  - One forward walk over the static CFG from the prefix boundary.
  - Outside a whole-quad interval, a write clears both facts for its
    registers. The facts are "uniform across the quad" and "fully written
    while the quad was whole". EXEC may cover part of a quad there, and the
    other lanes keep stale values.
  - A whole-quad interval is the straight run after `S_MOV_B64 exec, sX`. It
    requires all of:
    - sX was copied from EXEC inside the WQM prefix;
    - nothing overwrites sX between the copy and the restore;
    - no edge returns from past the restore to it or before it;
    - nothing enters the run except through the restore.
  - Inside it, the prefix transfer runs again.
  - `V_ADD_F32` and `V_MUL_F32` with plain sources count as full writes, like
    `V_MAD_F32`.
  - A copy taken before `S_WQM` (the live mask) never opens an interval.
- Verified:
  - A temporary diagnostic (removed) printed the verdict for every
    `SAMPLE_CD` the title parsed. All 15 sites in 4 shaders were admitted:
    `0ca445a8`, `51db2b69`, `bf209fbf` and `ee9c2b26`.
  - An offline replay of the dumped programs agreed.
  - Unit tests: `EmulatorImageGradientProof*` and `EmulatorShaderMimg*`, 64
    tests (eight new). The shader and compute-wave suites passed (472 tests).
- Not verified: the story draws themselves. Every capture so far is from the
  first 80 s, and the title stops about 15 s after the story starts.

### 2. Compute store into a D16 depth plane (fbc469af)

- Stop: `storage image has no compatible Vulkan view`, with these values:
  - backing D16 (`vk_format=124`), 2048×2048;
  - descriptor format 7 (R16 UNORM), tile 24, 2D array with one layer.
- Producer:
  - A generic fill kernel: an 8×8 group storing a constant at
    (x, y, layer).
  - The depth was an exact match, and the stage was compute.
- Vulkan has no storage depth image. The depth storage plane (7fe90864)
  covered only D32 with an R32F copy.
- Fix:
  - The plane format follows the depth texel size: R32F for D32 and D32S8,
    R16 UNORM for D16 and D16S8.
  - `UtilCopyTexelBits` pairs a depth plane only with a colour format of the
    same texel size, and sizes its staging by that texel.
  - The depth-aspect buffer copy of D16 is 16 bits per texel, so the bits are
    unchanged both ways.
- Verified: the next strict run passed that dispatch.

### 3. Growing a mipmapped storage array (8d31a270)

- Stop: `unknown relation: StorageTexture - IsContainedWithin -
  StorageTexture`. The array is 128×128, format 71, eight mips, tile 27,
  type 13. A one-layer view at base 0 was followed by a two-layer view at
  base 1, at the same address.
- The growth copy (a34392d5) required one mip level.
- Contract: each Gen5 slice holds its whole mip chain. This is
  `Gen5GetTextureArrayLayout`'s tile-27 mip slice, and slice size
  0x30000 = 1/2 of 0x60000.
- Fix: when both descriptors name the same chain, the prefix layers are
  copied at every level the two images hold. Correction (2026-10-19): array
  storage images were created with one host level until the change below, so
  for arrays this copied level 0 only.
- Verified:
  - The next strict run grew the array to six layers.
  - Unit test: `StorageTextureGrowthCopiesGpuOwnedArrayPrefix`, which covers
    an equal and a shorter chain.

### Resolved: a one-layer view inside the grown array

- Fixed on 2026-10-19; see "Role-playing title: mipmapped storage arrays and
  cube chains" at the top of this manual.

### Open: device loss on queue 9

- `vkWaitForFences failed: result=-4 queue=9` at
  `GraphicsRenderCommandBuffer.cpp:1974`.
- Seen in 1 of 3 runs of this route: the run with shader logging, on a
  build that carried inactive env-gated diagnostics.
- The last shader compiled was an array image copy compute shader: an
  `IMAGE_LOAD`/`IMAGE_STORE` whose layer is `s16 + s26`.
- Not attributed.

## Role-playing title: wave64 resolve pass under paired transport (2026-10-17)

The role-playing title ran its menus and the traveler selection at about
9 fps on Intel Arc A770. The cause was one compute pass that the
native-equivalence proof refused, so it ran under paired 64-on-32 transport.

- Symptom, measured on the traveler selection:
  - The GPU was busy: Intel GT idle residency was 4–16 ms per second.
  - The command-processor thread spent 66% of its samples in
    `drm_syncobj` waits.
  - The perf snapshot showed 15 of 20 s in fence waits, with 122 draws and
    30 dispatches per frame.
  - The title is GPU-bound, not CPU-bound.
- Excluded tool: Mesa `INTEL_MEASURE=draw` crashed the process (SIGSEGV)
  as soon as measuring began and wrote no data. This held for three start
  frames (700, 560 and 30). Do not use it with this renderer.
- Measurement with a temporary diagnostic (removed):
  - It wrote a bottom-of-pipe timestamp after each draw and dispatch for 3 s
    and summed the deltas between completions per shader.
  - One compute pass took 2055 ms of the 3026 ms window: 28 dispatches of
    129600 groups × 64 lanes, which is one lane per 3840×2160 pixel.
    That is 73 ms per dispatch.
- Cause:
  - The pass is a wave64 program. Native equivalence refused it at a
    `V_READFIRSTLANE_B32` with "reads another lane", so it ran under paired
    transport.
  - Under paired transport, each invocation carries two guest lanes and every
    register lives in a Function variable inside a switch-dispatched loop.
  - The value read is an image load at the constant coordinate (0, 0),
    multiplied by a word that `S_BUFFER_LOAD` placed in VCC_LO. Every lane
    holds the same value.
- Fix (f985041d):
  - The wave-uniform proof covers two more cases:
    - an image load whose DIM coordinates and T# are shared by the wave;
    - VCC filled by a scalar memory load.
  - Contract for the image load: the RDNA2 ISA sets MIMG DIM to "bits [3:1]
    of the resource type", and table 42 fixes the coordinates each DIM reads.
    A 2D load reads x and y only.
  - Native equivalence admits a READFIRSTLANE whose source is proven uniform
    under the launch EXEC, and classifies its destination as data.
- Result:
  - The pass runs one lane per invocation at 1.65 ms per dispatch.
  - The same 3 s GPU window completed 89 frames instead of 28, about 30 fps
    instead of 9.
  - The traveler selection image is unchanged.
  - In 120 s the title now reaches frame 1309; before, it reached 520.
- Open, not yet attributed: the remaining GPU costs per frame at the traveler
  selection on Intel.
  - A pair of draws (48 vertices × 10 instances): about 9.5 ms.
  - A full-screen triangle: about 7.9 ms.
  - A compute pass of 1152 groups: about 2.3 ms.
- Correction (same day): an earlier version of this entry said the
  `ImageSampleCd` stop in the main sweep below happens on main only. That was
  wrong. The 180 s run behind it stayed on the traveler selection. Once a
  route starts a traveler's story (about 85 s in), travel stops at the same
  `SAMPLE_CD` refusal. The faster title reaching it sooner is frontier
  movement, not a regression; see the next entry.
- Unit tests: `EmulatorComputeWaveNativeEquivalence` (16, two new). The
  shader suites passed (506 tests).
- Regression check, Intel, strict, 22 titles: status and capture count match
  the 1d9c60d7 sweep for every title except one. The penitent platformer
  crashed once (below); two reruns on the same binary reached 9 captures
  each.
- Gameplay fps from capture intervals: the sequel measured 67–68 in the sweep
  and 71–91 in a rerun (78–80 before); the insect platformer 76–78 and 79–88
  (92 before). The rerun is within the range these titles show from run to
  run, and both proofs only turn a lane search into a copy or admit the native
  path, so the drop is treated as variance, not as a regression.

## Role-playing title menu defect (2026-10-08, deferred; not gameplay)

Observed defect: the title menu has a flat magenta background, and the traveler selection screen has a black center
instead of its scene. Some earlier runs also omitted whole glyphs from the traveler prompt. These visuals are not
accepted, and the maintainer has deferred their repair. No title-specific color replacement, skipped consumer, or
permissive runtime change was introduced to hide the defect.

Reproduction: use strict Native rendering with Silent logging and a bounded 600 s run. Advance past initial settings,
enter the title menu, and select a new game. Compare captures of the title menu and traveler prompt. The last 600-second investigation candidate
shows the full prompt, but also reproduces the magenta background and black center; earlier good and bad prompt runs
mean one good capture does not establish that the intermittent glyph defect is fixed. That investigation ends after 469 s
at the existing unsupported `SW_256B_S` mip-chain frontier (32 by 32, six levels) in `Tile.cpp`. It was a runtime refusal,
not the time limit or a resource guard. Gameplay and performance acceptance remain open.

Confirmed investigation evidence: a lookup volume sampled by the frame was all zero, and a live merged geometry producer
was refused. Their shared resource identity was verified. This does not prove that the volume causes the magenta output.
The producer uses LDS, barriers, cross-wave operations and geometry exports; a passthrough approximation cannot preserve
its behavior. Further work needs a verified merged-stage launch ABI, workgroup execution, export capture and layered
render-target publication, followed by a causal pixel comparison. Do not relax the current admission guard as a fix.

Two independent source defects were corrected and reviewed:

- `GpuMemoryCreate.cpp` (`CreateObject`): dirty-range registration and `BeginRead` precede hashing and upload, and
  publish the same observation, so a CPU write during creation remains pending. An actual-object red test uploaded the
  older byte while guest memory held the newer one; it passes after the correction.
- The CPU-readable range validator walks adjacent mapped and protection segments, hashes only when the entire span is
  valid, and rejects holes and no-access pages. A split-mapping actual-object red test uploaded the old byte; it passes
  after the correction. Guard-page tests isolate the mapping and protection checks.

The diagnostic candidate passes 51 focused tests and 1952 of 1960 full-suite tests (8 skipped), strict boundary checks,
13 graphics-table checks and the diff check. Independent review found no blocking defect in the two memory corrections.
The native run exercised them, but did not establish that either caused or fixed the intermittent glyph symptom.
Temporary investigation code is removed before publication; the production binary requires its own regression runs.

Remaining limits: an unknown hash of zero compared with zero is not evidence of unchanged bytes, and other such cases
are not claimed fixed. The optional `DUMP_RT` diagnostic retains a raw render-image pointer across presents and may outlive
its object; it remains disabled and unfixed, with no evidence that it causes the production defect.

## UE4 JRPG startup frontier (2026-10-07, not gameplay)

An Unreal Engine 4 JRPG aborted on missing imports and shader forms one after another. Fixed so far, in the order
the runs hit them:

- **Videodec2.** The compute-queue and decoder entry points are implemented over the FFmpeg elementary decoder
  (AVC and HEVC, slice threads). Every structure carries its `thisSize` (compute memory 0x18, compute config 0x10,
  decoder config and memory 0x48, input 0x30, frame buffer 0x20, output 0x30); decoded pictures are written as
  linear NV12 with a 256-byte aligned pitch into the guest frame buffer. Movie playback itself is not verified yet.
- **`buffer_atomic_swap`** lowers to `OpAtomicExchange` (it was decoded as a barrier).
- **Image format 23** (R16G16 UNORM) samples, and color target format 14 with FLOAT channels is RGBA32F.
- **NGG image loads** read the address VGPRs of their dimension (plus the level of a mip load), not the decoder's
  padded operand width.
- **`image_gather4_lz_o`** takes its texel offset from the first address VGPR (two signed 6-bit fields), shifting
  the coordinates by offset / level size.
- **`v_readfirstlane` of a uniform VGPR** in a fused vertex front is a uniform copy, not a lane exchange.

The wave64 relative-index waterfall is now lowered only when its EXEC remainder snapshot, exact loop shape,
external-entry exclusion, and dead scalar/M0 values are proven. The scalar destination must not alias the remainder
mask; live scalar/M0 values across indirect PC transfers and unsupported destination modifiers are refused. Each relative
move uses its own lane's index; the remaining mask updates preserve the empty remainder, empty EXEC and clear SCC on loop exit. Strict native runs pass
this former admission refusal and reach the title menu and traveler selection, whose remaining visual defects are
recorded above. Seven synthetic proof controls cover admission and refusals. The overlap, indirect-transfer and
modifier cases fail before the added guards and pass after them; translation cache version 115 invalidates earlier
results. This advance does not establish gameplay.

The current live frontier reaches the initial settings screen but has not
established gameplay. A subsequent vertex program stopped in `ShaderParseEXP.cpp`
on full-precision POS1.X with EN=1. Its output state enables the first
clip-distance vector and clip distance zero, with the miscellaneous vector
disabled. The parser and translator now preserve physical X as a float in
`ClipDistance[0]` under active EXEC, only for that supported output contract.
The NGG proof retains the export without relaxing allocation, source definition,
mask or final-position ordering. Unsupported device clipping is refused before
pipeline creation; custom interpolation still refuses this output until its
geometry stage can preserve it.

The synthetic reproduction failed with the original parser rejection and now
passes full-module Vulkan validation. Fifty-four focused tests and the full
suite pass (1,876 passed, eight skipped). Native integration of this clipping
change remains unverified. A separate ES/GS-back cohort is rejected for its
stage and geometry limits. Its decoded body has real LDS exchanges, barriers,
allocation and geometry exports; it must not be admitted as passthrough without
that execution contract. It has not been causally joined to the interface
artifacts or to the parser failure.

A subsequent strict native run removes the POS1 parser stop and reaches the
native-wave admission in the same vertex program after about 73 seconds. The
fused-front proof refuses a VCC branch as non-uniform. The preceding comparison
reads a VGPR sum of two scalar-load results and another scalar-load result;
later vector arithmetic does not write that comparison's VCC. An opt-in,
bounded provenance trace confirms full logical EXEC and clean uniform sources
at the comparison. Its whole-dword SDWA tuple negates one source; the proof's
identity-tuple check rejects that modifier even though source negation preserves
lane uniformity. The body proof now accepts source NEG/ABS only after validating
the remaining whole-dword SDWA controls, and records a uniform comparison mask
only under full logical EXEC. Restoring EXEC after a partial comparison cannot
make that mask uniform. The proof also accounts for the implicit old-destination
read in multiply-accumulate instructions, checking both its definition and
uniformity before updating the destination.

A sanitized float reproduction accepts the unmodified tuple and fails with the
same VCC refusal when modifier recognition is disabled. Separate negative cases
exposed the partial-EXEC and implicit-accumulator proof errors before correction.
All 20 focused front-proof tests and the full suite pass (1,879 passed, eight
skipped); independent source review found no material issue. The temporary
trace is removed. A subsequent strict native run passes the modified-source
comparison and stops at a later VCC branch after ANDing two comparison masks.
Both reaching comparisons read clean scalar values under full logical EXEC;
`ScalarAlu` dropped it for the intersection of two uniform comparison masks.
The proof now retains a two-word AND result only when both complete inputs are
uniform masks. An AND involving EXEC also requires full logical EXEC; the
negative partial-EXEC fixture exposed an unsound retained copy case before
correction. The sanitized intersection reproduction fails with the original
VCC refusal and all 23 focused front-proof tests now pass. The full suite passes
1,882 tests with eight skips. A strict native run passes both comparison-mask
proof blockers and stops after about 73 seconds at an unsupported single-level
Standard256B texture array (RGBA8, one texel per layer, six layers). The binding
used a linear-aligned pitch, and the array layout and detiler did not admit this
swizzle mode. A second strict replay records the cube descriptor, zero array
pitch and last-slice field; the 64-texel pitch was derived by the host. The
published microtiled layout aligns each two-dimensional slice to its block
dimensions and selects layers by the resulting slice size. For RGBA8, one
8-by-8 block is 256 bytes, so the six single-texel faces occupy 1,536 bytes.
The binding now uses canonical tiled pitch for Standard256B arrays, and the
shared array layout and upload detile each single-level uncompressed slice.
Mip chains and compressed arrays on this mode remain unimplemented, rather
than being treated as linear. A sanitized six-layer fixture fails before the
change and verifies distinct face colors, upload regions and truncated-input
rejection afterward. Independent address-equation fixtures verify block
crossings and layer separation for one-, two-, four-, eight- and sixteen-byte
elements. Checked row-byte arithmetic also rejects unrepresentable staging
sizes before tiled sizing; the old arithmetic admitted a wrapped sixteen-byte
element case. All 47 detile tests and the full suite pass (1,885 passed, eight
skipped). The subsequent native run records the six-layer cube with canonical
pitch and passes the array-layout stop. It ends after about 72 seconds at a
full-channel POS0 export with DONE=0 and VM=0, which the parser previously
requires to carry DONE. A bounded read-only debugger capture records the next
instruction as POS1.X with DONE, followed by parameter exports and the real
program terminator. Distinct nonfinal POS0 and final POS1.X formats now preserve
that sequence for exact full-precision vertex encodings. The whole-program
proof requires one full POS0, at most one typed POS1 and DONE on the last
position export; full parameter exports can follow. The load-bearing body
proof carries a set of combined position histories through forward branch
joins and requires POS0 plus final DONE on every terminating path. It accepts
position events only under the proved full logical EXEC root; divergent
masked position alternatives remain outside that proof. Sanitized fixtures
reproduced the old acceptance of positions after DONE and a branch that
reached the terminator without POS0. POS0 lowering now guards its output store
and the existing clip probe with resolved per-invocation EXEC. Typed POS1.X
still requires the exact first clip-plane output metadata. Raw target,
control and source tuples must agree; mutated internal-IR fixtures reproduced
the former validation gaps. All 73 focused export and front-proof tests pass;
the full suite passes 1,899 tests with eight skips. The architecture boundary
check and the provenance check of all 13 generated graphics tables also pass.
The first native replay of this position change stops after about 32 seconds,
earlier than the preceding frontier. The new full-root position check refuses
a previously admitted vertex program at its final POS0 export because its
symbolic EXEC is partial or unknown. The run has no resource-guard stop and
produces no capture. This is an integration regression of the new proof;
the captured program saves the complete vertex mask from VCC into an SGPR
pair before a conditional scalar load overwrites VCC. It then restores EXEC
from that intact pair before its only position export. The proof discarded
this saved-node provenance on scalar-to-scalar copies. Two-word moves now
snapshot the exact source node before invalidating destination metadata and
transfer that node to the destination pair. The full-root position gate is
unchanged. A sanitized mask-copy chain fails before the correction; copied
partial masks and pairs with an overwritten half still fail at position
admission. Independent source reviews find no material issue. All 75 focused
tests pass, and the full suite passes 1,901 tests with the same eight skips.
The architecture boundary and all 13 graphics-table provenance checks pass.
The next native replay passes that full-root refusal and the former ordered
position-export parser stop. It ends after about 79 seconds at a sampled-image
descriptor-resolution failure in a later vertex program, with no resource
guard stop. That program loads a texture-sized scalar block from its user-data
pointer and samples it later; the current resolver cannot identify a bound
sampled descriptor at that scalar destination. A bounded, read-only capture
at the resolver entry confirms that the final binding already contains both
EUD texture and sampler table entries, but has no dynamic scalar-load mapping
to their shader destinations. The EUD pointer uses API index 24 and shader
index 32 with the fused-front base of eight; the declared table has 76 dwords,
matching the largest constant load span in that program. The fused-front
parser omitted EUD instruction discovery together with the deliberately
disabled generic scalar-buffer fallback. EUD-only instruction discovery now
uses a separate code input while generic metadata analysis remains disabled
for the reused projection descriptor. Full and split scalar loads use the same
base-aware pointer match, requiring both words to retain their entry values on
every path to the load. Snapshot sizing uses that same provenance and the
existing 256-dword cap. Local dynamic storage mappings use the separate code
for their scalar read span without dereferencing the disabled generic code.
A sanitized fixture first reproduces the missing mapping and short snapshot;
another failing run proves that overwriting either pointer word could still
materialize the wrong entry table before the lifetime guard. The corrected
cases pass alongside the base-zero control, null dynamic storage and retained
metadata-only projection cases. Independent source and downstream reviews find
no material issue. All 1,902 tests pass with the same eight skips; boundary and
13 graphics-table checks pass. The next native replay passes the descriptor
failure and holds the initial-settings screen through the 300-second bound,
without a fatal error or resource-guard stop. Its final sampled status reports
about 6.9 frames per second. Repeated face-button input does not advance
that screen. An isolated menu-confirm press with the same binary advances
through the following logos; that replay stops after about 164 seconds at
vertex-input layout validation with two resources and two buffers. The exact
rejected descriptors are recovered by a later failure-entry capture: one
12-byte RGB32 float stream is valid, while a separate four-byte-strided stream
uses the unified unsigned eight-bit scalar format missing from the vertex
format table. A sanitized two-binding fixture first reproduces the undefined
format and layout refusal. The shared table now resolves that format as
`R8_UINT`, preserving integer input declarations and raw-bit fetches through
the existing emitter. The translator version invalidates cached float input
declarations for that resource. The combined storage, vertex,
emitter-precondition and translation-cache checks pass all 107 focused tests;
the complete suite passes 1,911 tests with eight existing skips. A native
replay of these corrections remains pending. This establishes
the resource-frontier advance, not gameplay or correct clipping output. The
next diagnostic repeat stops earlier in GPU-memory overlap classification: a
new storage image is contained by a render image and contains 502 read-only
storage buffers. The render image reports a GPU update newer than its CPU
update and unknown content origin; the smaller buffers report CPU uploads.
Those timestamps do not prove a coherent owner for the entire new image.
Preserve the rejection until the live ownership and range contract is known;
do not seed the image from guest bytes merely to accept the mixed relation.
Further repeats reach the same mixed-parent refusal. A bounded breakpoint at
the standalone compute seed-proof helper produces no dispatch snapshot: the
optimized dispatch caller inlines that helper. The absent snapshot does not
exclude a compute caller or prove full-image overwrite. Capture the actual
creation caller before fatal shutdown before extending an overlap or coverage
contract; the last logged compute program alone does not identify that caller.
The two distinct first failures remain separate investigations. The
latest failure-entry capture recovers the compute checksum directly and joins
it to the rejected storage image. The shader forms a two-dimensional grid,
intersects both unsigned bounds predicates with EXEC, adds the parameter
origin, then stores every component present in the image format. Its parameter
buffer places origin and bounds after an earlier coefficient pair. The current
coverage recognizer cannot prove this static metadata-buffer form, and the
actual creation parameter still requires seeding. Both input-buffer ranges
are disjoint from the output range. The host prefix shows zero origin and
matching bounds, but does not establish a coherent GPU snapshot. A general
offset-grid proof and a bounded runtime origin/bounds snapshot now recognize
that static metadata form. The proof derives the byte offsets from the scalar
load, requires its complete LGKM-zero wait before the bounds comparisons, and
rejects descriptor, coordinate or EXEC clobbers. Insufficient-wait fixtures
first reproduce false coverage and now reject it while the positive fixtures
retain coverage. Native acceptance remains pending. The new origin-based path
also checks the exact immutable storage-buffer snapshot uploaded for the
dispatch before texture creation. A separate guest-memory read cannot prove
that later upload uses the same parameter bytes. Missing snapshot ownership
or changed origin/bounds clears the provisional seed-skip bit.
The next strict replay stops after about 183 seconds without a resource-guard
stop at EUD snapshot preflight. Its metadata declares 268 dwords, exceeding
the fixed 256-dword host snapshot policy; no table bytes are copied before
the refusal. This is a later recorded first failure, not gameplay acceptance
or a complete native proof of either preceding correction. The metadata
field supports larger declarations, but the live caller, descriptor spans
and readable table range must be recovered before changing that policy.
A one-shot failure-entry capture identifies a fragment caller with 30 user
scalars and an EUD offset base of 32. Its three storage-buffer metadata sharps
occupy EUD dwords 256--267, matching the declared 268-dword extent. The host
mapping covers that complete range, but this capture reads metadata only and
does not prove guest-table readability, stability or instruction consumers.
The next contract test must preserve the full declared extent, validate the
actual descriptor spans, and retain the stable double-read before binding.
The local correction sizes both snapshot reads exactly to the checked extent,
bounded by the metadata field's 16-bit representation. Image-sharp expansion
resizes and recaptures the entire prefix before exposing the final snapshot.
A sanitized table ending at a protected page verifies the high descriptor and
forces a retry by changing its final word between the two reads. Its actual
scalar-load consumer is discovered through the existing dynamic binding path;
unused static metadata is still pruned. Pointer tables behind SRT retain their
separate 256-dword admission limit. The representational bound permits larger
candidate scalar-load spans, including unreachable instructions whose scalar
flow retains entry values. It does not prove all such guest ranges readable;
unreadable or unstable ranges continue to fail strictly. Candidates beyond the
former limit already failed preflight, so no regression of a previously
successful parse is established by that expanded admission alone.
The earlier metadata-only capture could not recover optimized locals and does not
establish a table snapshot. The initial-settings capture still has fragmented
interface elements; no causal connection to the separately rejected geometry
cohort is established. No gameplay, clipping output or complete subgroup-width
equivalence is established yet.

An adjacent pre-existing risk remains in offset-zero bounded-grid seed
eligibility: its pre-bind guest snapshot is not coupled to the later uploaded
storage buffer. A guest update between those reads can make eligibility refer
to different bounds from those consumed by the shader. No native trigger is
established here. The next direction is the same exact-bound-snapshot check
for that coverage form; the current origin-based change preserves its behavior.

Extended-data emission now stores only actual descriptor fields in a sparse
map. A high-index static binding first reproduced the former fixed-map bounds
failure. Checked lookup rejects negative, missing and out-of-range fields;
the scalar-load emitter resolves every field before emitting any loads. The
existing dynamic-PC mapping remains first, with static lookup restricted to
populated rows. Overlapping rows retain the established binding order. This
closes the fixed-map capacity and fabricated row-zero risks locally; native
acceptance of the complete correction remains pending. The next strict native
replay passes the former snapshot-span refusal and stops after about 188 seconds
at fragment admission. The mask-flow proof reports a memory-write refusal;
its shared prefix classifier also includes some reads, so the reported text
does not establish the rejected instruction's side effects. The draw uses one blended color
target and no depth attachment. No resource guard or debugger stopped the run;
the retained captures do not establish gameplay. Recover the rejected program
and its EXEC/helper-mask flow before changing that admission contract.
A same-binary repeat instead stops after about 178 seconds at mixed storage
image parents before reaching fragment admission. Its image size and relation
shape match the earlier creation refusal, but no current caller, shader or
descriptor identity is captured. These distinct first failures require a
same-event caller/descriptor join; the earlier compute identity cannot be
assigned to this repeat from the last shader log.
A failure-entry capture on the next same-binary run joins the actual compute
dispatch to its shader and the current, bound and creation image descriptors.
The originating static coverage is empty, the final seed mask is zero and the
creation record confirms that seeding was not skipped. The decoded store uses
the parser's sequential coordinate form, with no explicit address tuple. The
origin-grid recognizer currently requires an explicit X/Y tuple, unlike the
existing bounded-grid recognizer. This is a concrete representation mismatch
to reproduce with a parsed fixture. The encoded image dimension and several
earlier scalar-read predicates were not captured, so it does not establish
that this is the first rejection or that correcting it alone closes the native
failure. The run stops after about 180 seconds; no gameplay is established.
A two-dimensional, no-NSA store fixture in the parser's admitted flag subset
reproduces the empty coverage
despite matching the proven origin registers. Origin and bounded-grid proofs
now share the coordinate matcher: explicit X/Y operands must match exactly;
sequential coordinates require a plain VGPR span of at least two words and
adjacent proven X/Y registers. Dimension, full-store and resource-identity
checks remain intact. Short spans, mismatched coordinates, another dimension
and modified operands retain rejection. The focused suite passes locally;
native acceptance of this correction remains pending. The fixture verifies
coordinate representation, not complete instruction-flag conformance; the
pre-existing UNRM admission debt remains unchanged.
The next strict replay passes the recorded creation failure and reaches
fragment admission after about 179 seconds. Its failure-site program dump
identifies the rejected instruction as an add-thread-id LDS write. The broad
memory classifier's inclusion of reads therefore does not explain this
particular refusal. The program uses three such stores and corresponding
add-thread-id reads while M0 is zero, then restores M0 and the original EXEC
before exporting. Investigate a proof of per-lane slot ownership and mask
lifetime before admitting these operations through the existing private-spill
lowerer; no general LDS-write relaxation is justified. The retained image is
the title logo, not gameplay, and no resource guard stopped the replay.
A same-binary failure-entry capture confirms two extra pixel-LDS blocks and
30 user scalar words. On GFX10 those blocks provide 1 KiB; the three complete
wave64 planes occupy 768 bytes. The packed launch parameter follows the user
scalars, but its numerical offset was not captured. A recorded word outside
the user count must not be mistaken for that implicit parameter.
The bounded admission change requires disjoint wave64 planes inside the
declared extra allocation and the existing private array, a dominating constant
aligned M0, preceding stores for every read, and unchanged EXEC/M0 through one
straight-line interval. Other LDS access forms remain excluded. Interpolation
after a spill must recover the original parameter M0 through exact scalar
moves; numeric or unknown interpolation bases fail closed. Reserving the extra
storage before the parameter region is supported by older and newer published
hardware contracts and target-generation compiler behavior, but remains an
inference for this target rather than a captured numerical boundary. Mask
monotonicity cannot bypass this spill proof. The analysis and module identities
include the launch allocation and parameter scalar index. Focused validation
includes 88 passing fragment, LDS, native-wave and cache tests,
including explicitly validated pixel SPIR-V. The complete suite passed with
1,922 tests and eight fixture-dependent skips before the final additional
boundary cases, which passed in the focused rerun. Both affected targets and
the graphics provenance/boundary gates pass. Independent source and native
program audits found no structural rejection in the captured pattern. The
next strict native replay passes the recorded fragment admission failure and
reaches character selection before stopping after about 290 seconds, without
a resource guard or debugger stop. It rejects a single-component unsigned
16-bit color target at `GraphicsRenderAttachments.cpp:886`; the shared
`RenderTexture.cpp` format resolver has no mapping for that CB format/type
tuple. A focused contract test reproduces the missing mapping. The title
menu still has a magenta background; no causal connection to this new refusal
or gameplay acceptance is established. Join the active attachment and shader
export interpretation before exposing this integer target to the pipeline.
A bounded draw-boundary diagnostic confirms an active write at the first
attachment, unsigned number type, standard component order and the packed
unsigned-16 export mode. The format mapping now preserves two bytes per
element and selects the host unsigned-16 attachment; seven focused format
tests pass. The pixel interface now carries each attachment number type into
the module identity, declares unsigned outputs for unsigned attachments, and
extracts packed unsigned-16 components without floating-point conversion.
Compressed masks outside the documented paired-source set remain refused.
An inactive intermediate export skips its stores and can continue after an
EXEC restore; a final valid-mask export retains discard semantics. The embedded
floating-point template is refused against a materialized unsigned attachment
at location zero, before pipeline lookup. All 63 focused tests and the full
suite pass: 1,931 tests and eight fixture-dependent skips. Both targets build,
the source/table gates pass, and independent review found no blocker.

This is not native acceptance. The checked diagnostic parse first revealed a
missing Gen5 unsigned-pack opcode. A synthetic bounded decode reproduces it;
the decoder now retains its two unsigned inputs and strictly rejects unsupported
modifiers. The existing lowering also referenced an undefined mask constant
and selected an integer as a floating-point result. A validated in-range
fixture reproduces that defect; constant resolution and a bit-preserving cast
repair the module. The numerical overflow behavior is still unverified:
the published Gen5 pseudocode names a conversion helper without defining its
out-of-range rule, compiler selection alone supplies no numerical oracle, and
inspected execution fixtures stay in range. Earlier architecture rules cannot
resolve that question. The bounded native diagnostic now records the complete
nineteen-instruction pixel program. Immediately before the unsigned pack, an
unmodified bit-field insert chooses bits from two inline integer arms; their
union bounds the result to three low bits. Both packed sources are the same
VGPR. The program has no branch or EXEC mutation. This proves in-range inputs
for that program without choosing an overflow rule.

Gen5 unsigned packing now admits unmodified in-range immediates or an adjacent,
single-word bit-field-insert producer with in-range inline arms. The proof
rejects entries that bypass the producer, dynamic PC transfers, nonadjacent
instructions and unproved banked VGPR layouts. An unbounded-source fixture
first reproduced acceptance where strict refusal was required. Numerical
fixtures check the generated packing on admitted inputs. Translator version
112 invalidates modules compiled before this admission rule. The temporary
draw-boundary stop and raw-opcode logging have been removed. Both targets
build; all 66 focused tests and
the full suite pass, with 1,934 passed tests and eight fixture-dependent skips.
Source boundaries and all thirteen graphics tables pass their checks. The
diagnostic run's final capture is black and supplies no gameplay acceptance or
general overflow contract. The production run reaches a visible traveler
information panel, then fails after 489 seconds without a resource guard or
debugger stop. Its first refusal is a writable image declaration at
`ShaderSpirvImage.cpp:895`: the shared format table admits the single-component
16-bit normalized format for sampling, but leaves its storage usage undefined.
This declaration failure occurs before instruction lowering, so the new
unsigned-pack path still has no direct native execution proof. The selector
retains a black center; no gameplay acceptance is established.

The published target-generation ISA maps that raw descriptor format to
single-component 16-bit UNORM and specifies conversion on ordinary image
stores. The shared table now selects `VK_FORMAT_R16_UNORM` for storage as well
as sampling. It retains the existing floating-point, formatless `Unknown`
storage declaration; integer output and atomic admission rules are unchanged.
A single writable-descriptor fixture first failed storage planning, then
passed production SPIR-V generation and validation after the mapping change.
The existing format-contract test also checks the exact host format, numeric
domain and two-byte element size. Translator version 113 invalidates prior
modules. On the hash-verified Intel discrete-GPU driver, a metadata-only Vulkan
query reports storage and formatless read/write support and accepts the actual
combined image usage with both ordinary and mutable creation flags. That
query does not allocate an image or create its views. Independent review
confirms the existing mutable format list remains compatible and unchanged.
Both targets build; 88 focused tests and the full suite pass, with 1,935 passed
tests and eight fixture-dependent skips. The final additional table assertions
also pass in the focused rerun. Source boundaries and all thirteen graphics
tables pass. The next strict native replay does not repeat the storage
declaration refusal. Its exact pixel-program probe contains the captured
bit-field insert, unsigned pack and compressed export, with generated and
optimized SPIR-V declaring unsigned output and preserving the packed bits.
A matching draw trace resolves the active attachment to the host unsigned-16
format after pipeline return and descriptor binding. The trace precedes the
draw command, so it does not prove submission, execution or presentation of
that draw. Actual normalized-image/view creation is not separately traced.
The replay stops after 521 seconds on MIMG opcode `0x68` at
`ShaderParseMIMG.cpp:524`. That switch labels the opcode as a derivative sample; its
target-generation semantics and raw operands need independent evidence before
an implementation decision. Both scheduled input batches were delivered,
with matching guest state-read samples and no cancellations. The final
post-input capture is completely black. There is no resource-guard or debugger
stop; the last measured memory-event counters contain no high/max/OOM event.
Gameplay and performance acceptance remain open.

The following bounded native replay reaches the same coarse derivative sample
after about 460 seconds, again without a resource-guard or debugger stop. A
bounded raw snapshot and decoded prefix join its six logical addresses to four
supplied slopes and two coordinates. A canonical WQM interval contains complete
floating-point multiply-add writes, fixed quad broadcasts and subtraction
producers for all four slopes, before the first EXEC narrowing. All encoded
DPP controls outside the modeled fields are zero in that captured interval.
The final capture remains black; this is instruction evidence, not gameplay.

Gen5 two-dimensional coarse derivative sampling now has a separate decoded
instruction and emitter. Explicit NSA addresses are checked individually;
the no-NSA form retains its six-register span. The supplied operand order is
the two horizontal slopes, the two vertical slopes, then the two coordinates.
The emitter retains compact DMASK channel order and the existing EXEC guards
for inactive destination lanes. Other dimensions, integer image domains and
unmodeled sample variants remain refused.

Published older hardware contracts describe coarse sampling as choosing LOD
per quad, and target-generation compiler definitions retain the distinct
coarse opcode with this derivative schema. Those sources do not establish a
general target-generation lane-selection rule or bit-exact cross-GPU LOD
arithmetic. Lowering to supplied SPIR-V `Grad` is therefore a bounded
operational inference: it is admitted only when each supplied slope is proved
equal across its quad, making a per-quad slope selection immaterial. Arbitrary
coarse gradients are not admitted.

The proof requires an initial canonical WQM interval and complete writes before
using a fixed quad broadcast. It distinguishes complete vector writes from
destination-shaped read operands, partial DPP or SDWA writes and redirected
memory results. Only the demonstrated plain move, subtraction and full-dword
multiply-add forms establish complete writes; an accumulating multiply-add
also needs a previously defined accumulator. Unsupported buffer status/LDS
controls and dynamic register writes fail closed. Later writes clear gradient
facts, control-flow joins intersect them, and an edge that re-enters the
initial proof interval is refused. Unmodeled DPP bits are retained by decoding
and rejected by this proof. Translator version 114 invalidates prior modules.
The temporary raw-byte diagnostic has been removed. Both affected targets
build; all 93 focused tests pass, including production SPIR-V validation,
operand ordering, inactive-lane preservation and the proof's rejection cases.
The full suite passes with 1,948 tests and eight fixture-dependent skips.
Source boundaries and all thirteen graphics tables pass. Independent review
found no remaining blocker. Native execution of this candidate and
gameplay/performance acceptance remain unverified.

The next strict native replay reaches the title and traveler-selection menus,
then stops after about 206 seconds on an unsupported sampled-array allocation
at `GraphicsRenderBind.cpp:3315`. The recorded input is a six-layer, ten-level
512-square texture in Standard64KB tiling. The array planner currently excludes
multi-mip layouts outside Standard4KB. This is an explicit refusal, not a
resource-guard stop; it does not establish a descriptor-shape regression or
execution of the new coarse-sample lowering. The scheduled input batch had not
reached its target when the endpoint ended. The visible title background is
magenta, and the selector has incomplete text. Comparing those states with
earlier builds and identifying their actual output-producing draws is the
current visual investigation; neither defect has a confirmed connection to
this array refusal.

**Audit-closure update at translator126 (2026-10-09).** The following statuses
supersede specific observations recorded on 2026-10-07:

- The tested nonterminal pixel color exports now skip inactive stores and
  rejoin later shader code. Terminal exports retain their discard behavior.
  This does not establish parameter-export enable-mask semantics.
- The packed-mip image-store path and GDS append/consume operations now gate
  their side effects by EXEC. Focused packed-mip coverage and the selected
  native GDS integration pass; unrelated or unmodeled forms remain excluded.
- The whole-pair VCC propagation risk is fixed: one-word writes do not promote
  a varying two-word mask to uniform. Focused positive and negative proof cases
  pass.
- The native `ParameterExecWriteback` fixture passes its full-EXEC and
  zero-EXEC variants. This establishes only EXEC behavior for its full-enable
  mask; partial full-precision PARAM masks, accumulation, compressed PARAM and
  VSKIP remain unqualified or refused. Pixel color-export masks are a separate
  contract.
- The current working tree bounds the optional vertex-layout logger's walks
  over attribute, buffer and resource arrays to their 16-entry capacities.
  That source change is not in HEAD at this checkpoint and has no separate
  logger-specific runtime validation.
- The `ShaderDumpGuestProgram` extent and mapping-lifetime finding remains
  open. Its raw-byte extent still needs a mapped-range clamp and one lifetime
  owner.

The following observations retain their original 2026-10-07 date. Integer
color output evidence is now limited to the translator126 five-case UINT16
native matrix described above; other integer formats remain unqualified. The
inactive nonterminal pixel-export finding is superseded for the tested forms,
with no established connection to the deferred menu defect.

The `ShaderDumpGuestProgram` diagnostic finding was recorded at
`Shader.cpp:782`: its raw-byte extent used the final instruction PC plus eight
without clamping to the registered shader range, and parsing and raw dumping
did not share a lifetime lock. A four-byte final instruction could include
adjacent readable bytes, while a mapping replacement could pair a listing
with different bytes. The extent and lifetime snapshot remain open; use a
bounded decoded listing until they are repaired.

The 2026-10-07 side-effect observation named a packed-mip image store and GDS
append/consume operations that lacked EXEC guards. Those findings are
superseded by the EXEC-gated implementations and test evidence above. Other
shader memory operations still require their own proof and admission.

The 2026-10-07 VCC observation described whole-pair uniform promotion after a
one-word write. That behavior is superseded by the complete-pair rule above;
unsupported mask forms remain subject to strict refusal.

The 2026-10-07 parameter-export observation described unguarded writes and an
optional UV probe. The translator126 fixture establishes EXEC behavior only
for its full-enable-mask case. Parameter component-enable behavior remains an
open contract question, and no claim is made about partial-mask accumulation.

The 2026-10-07 vertex-layout logger observation described raw-count walks past
fixed arrays after validation refused the layout. The current working tree
caps those walks at 16 attributes, buffers and resources while retaining the
original counts in the report. This change is not in HEAD at the translator126
checkpoint and still needs independent logger-specific validation.
