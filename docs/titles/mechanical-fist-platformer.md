# Mechanical-fist platformer

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named descriptively only (`AGENTS.md`).

## Mechanical-fist platformer startup frontier (2026-10-08, not gameplay)

These runs used the full local candidate, including changes held from publication. They used Silent logging,
the five-second cross route and a 180-second bound. The observations below do not establish the frontier of the
selected published source; none is a gameplay or visual acceptance.

- **One-byte 64 KiB render-target volumes (published 2026-10-16).** The title samples an R8 400x5x5 volume with tile mode 27 (`SW_64KB_R_X`).
  The local `Sw64kRxWithinBlockOffset` candidate uses a one-byte equation inferred from a 16-pipe address pattern: bits 0..15 are
  `x0 x1 x2 y1 y0 y2 x3 y4 | x3^y3 x4^y4 x6^y5 x5^y6 | y6 x6 y7 x7` inside a 256x256 block. The existing two- and
  eight-byte equations match the same published table exactly, which ties the new form to the same source. A
  round-trip test (400x260, pitch 512) constructs its input and reference with the production offset helper, so it
  does not independently establish the one-byte equation. Publication requires an independent oracle. The
  four-byte micro tile no longer follows the depth (`Z_X`) order: guest-written atlas levels showed the
  `x0 x1 y0 y1 y2 x2` order on 2026-10-16. Open: the slice XOR of the published
  pipe bits is not applied because volumes are detiled as independent thin slices.
  Oracle search (2026-10-16, still open): decoding the published 16-pipe 64 KiB tables shows the shipped two- and
  eight-byte equations equal `SW_64K_R_X` and the four-byte one equals `SW_64K_Z_X` exactly. For one byte the two
  tables share bits 8..15 but differ in the micro-tile (`R_X`: `x0 x1 x2 y1 y0 y2 x3 y4`; `Z_X`:
  `x0 y0 x1 y1 x2 y2 x3 y4`), so the table cannot choose the order the guest uses. The racing title's eboot lists the
  guest GPU-address library among its linked libraries, but no table in the published per-bit encoding (64-bit
  `x|y|z|s` masks, per-channel mask arrays or per-bit records of 8 to 64 bits) occurs in it; other eboots were not
  searched. The next oracle is the library's own code or a
  guest-written one-byte target sampled back.
  Guest-content oracle (2026-10-16, excluded as decisive): no GPU object overlaps the volume at bind, so its ten
  64 KiB blocks hold guest CPU data. Nearly every byte is non-zero, with an eight-byte repeating pattern. Scoring
  the mean neighbour difference of each candidate's detiled image (x/y) gives the held equation 37.5/11.3, linear
  92/77 and the standard 64 KiB equation 71/36. With y bits 3..5 in order `y0 y1 y2` instead of `y1 y0 y2` the
  score is 37.5/9.1. The data supports `x0 x1 x2` in bits 0..2 followed by y bits. It cannot place x3 and above
  (the content repeats every eight texels) or y3 and above (height 5). The one-byte equation therefore stayed held.
  Volume oracle (2026-10-16, decisive for the slice XOR): a later dump of the volume holds 0xFF in every texel the
  guest wrote, and other bytes elsewhere in its ten blocks. With the published pipe XOR of the slice
  (`s0->bit 11, s1->bit 10, s2->bit 9, s3->bit 8`, the same XOR the depth layout already applies per layer) all
  2000 in-bounds texels of each of the five slices land on 0xFF; without the XOR, or with the bit order reversed,
  slices 1..4 read foreign bytes. The guest data does not decide between `y1 y0` and `y0 y1` in bits 3..4, so the
  published table order is kept. `TileGetSw64kRxOffset` now accepts one-byte elements and a slice; single-level
  `SW_64KB_R_X` arrays and volumes pass the layer as the slice (CPU detile; the GPU detile path refuses slices).
  Verified: the mechanical-fist platformer passes the volume in a strict Intel run and next stops at the
  unresolved `sceAgcDcbDrawIndirect` import (implemented below). Open: multi-level `SW_64KB_R_X` mip chains are still detiled without
  the slice XOR.
- **Storage-image channel order.** A formatted store into a BGRA-ordered `R8G8B8A8_UINT` storage image wrote red and
  blue swapped. Only RGBA8 UNORM/SRGB storage images get a BGRA host view; the shader now applies the descriptor's
  destination select to stores (inverse permutation) whenever the host view cannot (`ShaderStorageImageSwizzleInShader`
  with the format-aware `VulkanStorageHasRedBlueView`).
- **`v_cvt_pk_u16_u32` / `v_cvt_pk_i16_i32` saturate.** The ISA defines `D.u16_lo = u32_to_u16(S0.u32)`; the
  conversion clamps to 0xffff (signed: to [-0x8000, 0x7fff]). Two independent compiler contracts agree, so the
  range-proof guard that only admitted provably in-range sources is replaced by `UMin` / `SClamp` for any source.
  The signed emitter also stopped selecting a `uint` into a `float` destination, which produced invalid SPIR-V.
- **UINT16 color exports (historically held local candidate).** `SPI_SHADER_COL_FORMAT` UINT16_ABGR (7) into a UINT color buffer locally declares a
  `v4uint` fragment output and stores the raw 16-bit components of each export dword (`OpBitFieldUExtract`), keeping
  disabled components. FP16 (mode 4) exports into UINT buffers keep the established float interface; the sandbox
  regression recorded above came from keying on the buffer number type alone. `R16G16_UINT` (format 5, type 4) is a
  local render-target format. **Status update at translator126:** the five-case native UINT16 matrix described above
  now passes exact readback on the available Intel Vulkan device. This supersedes the earlier absence of native
  writeback evidence for those formats, but does not establish guest-route or cross-vendor acceptance.
- **Non-indexed indirect draw (published 2026-10-16).** `sceAgcDcbDrawIndirect` (`1q1titRBL6o`) takes the same
  operands as the indexed builder (command buffer, byte offset from the `SetBaseIndirectArgs` base, draw modifier)
  and emits `IT_DRAW_INDIRECT` with the non-indexed patch offsets. The processor checks and copies the 16-byte
  argument record (vertex count, instance count, start vertex, start instance) and issues an auto-index draw whose
  first vertex adds the start vertex to `GE_INDX_OFFSET` and whose first instance is the start instance. Like the
  indexed path, the patch offsets are not written to user SGPRs. Verified: the mechanical-fist platformer resolves the
  import in a strict Intel run and stops about four seconds later at an unrelated storage-image overlap; no capture
  was taken, so draw output is not yet verified.
- **Storage image over recycled guest-byte caches (published 2026-10-16).** The same title creates a 1024x1024
  two-byte storage image over a 2 MiB range that still holds a CPU-uploaded linear 1024x540 texture and two read-only
  0xa0 vertex buffers. Mixed parent types stopped at `!create_all_the_same`. Each parent only caches guest bytes, so
  the new multi-parent rule reclaims the read-only vertex buffers (as under an incoming texture) and links the sampled
  texture as the single-parent rule does. It requires CPU-upload origin and no pending write-back for the texture,
  and applies only to mixed vertex-buffer and texture parents, which previously always stopped. Verified: the next
  strict Intel run passes the overlap and stops about 30 s in at a missing SDWA `v_cvt_f32_f16` emitter.
- **SDWA half-to-float conversion (published 2026-10-16).** The half-precision emitter refused every SDWA form. The
  widening conversion now admits SDWA with a DWORD destination and a WORD_0, WORD_1 or DWORD source select without
  sign extension (ISA SDWA fields); the operand load already extracts the selected word, and SDWA NEG/ABS apply to
  the half. Byte selects, partial destinations and SDWA on other half operations stay refused. The CPU SPIR-V
  evaluator checks the high-word and negated low-word forms. Verified: the next strict Intel run passes the shader
  and stops about 35 s in at the unparsed `image_sample_d` (MIMG opcode 0x22).
- **Two-dimensional `image_sample_d` (published 2026-10-16).** SAMPLE_D supplies the four slopes of a 2D coordinate
  before x,y (ISA VGPR usage table) and derives each pixel's LOD from its own slopes, so it is an explicit-gradient
  sample per invocation. It shares the 2D float emitter of the coarse `image_sample_cd`; only the coarse form keeps
  the pixel-stage, quad-uniform gradient proof. Other dimensions, NSA forms with one extra dword and A16/G16
  controls stay refused at parse. Unit tests parse the opcode and validate the emitted module with unproven slopes.
  Verified: the next strict Intel run passes the parser and stops about 35 s in at the fragment wave transport
  refusal for a pixel shader whose memory writes would expose helper lanes (2688-wide draw), the same open
  contract as the sandbox title's new-world stop.
- **Pixel memory writes under a helper-free EXEC (published 2026-10-16).** The refused pixel shader saves the launch
  EXEC, enters WQM, removes discarded lanes from the saved copy with `s_andn2`, and before its only
  `buffer_store_format_x` sets `exec = saved & exec`. The fragment mask flow admitted device-memory writes only before
  the first EXEC write. The guest/host difference is helper lanes alone: a guest helper in EXEC writes, a Vulkan helper's
  store has no effect. So a buffer, image or GDS-counter write is now admitted wherever the flow proves EXEC holds no
  helper lane (the `kWide` bit an export already requires). That subsumes the launch-prefix rule. LDS and scratch
  writes, and GDS append/consume after the prefix, stay refused. The proof had a gap: an `S_*_SAVEEXEC` destination
  took its width from the explicit sources, so a saved WQM EXEC counted as helper-free. It now inherits EXEC's width.
  Unit tests refuse a store under WQM and after restoring a saved WQM EXEC. They admit a store after narrowing by the
  launch EXEC; reverting the SAVEEXEC change fails the latter test. Offline, the title's refused shader is now
  admitted as quad local. No saved program of the sandbox title's 285 s new-world stop exists, so that stop is still
  untested. Verified: the next strict Intel run passes the shader and stops about 40 s in
  at `unsupported Gen5 image numeric type: format=27` (`ShaderSpirvImage.cpp`).
- **Sampled 16_16_UINT images (published 2026-10-16).** The same title samples a Gen5 format-27 texture. The
  RDNA2 image-format table lists 27 as `16_16_UINT`, between `16_16_SSCALED` (26) and `16_16_SINT` (28). It is now a
  four-byte unsigned-integer `R16G16_UINT` image in the Gen5 format table and in the element-size table. Storage
  reinterpretation views are unchanged. Verified: the next strict Intel run passes it and stops about 52 s in at a
  missing pixel-stage emitter for `s_load_dwordx4`.
- **Open: descriptors spilled to VGPR lanes (recorded 2026-10-16).** The same title's next stop, about 52 s in, is
  `shader emitter missing ... SLoadDwordx4 s[32:35], s[28:29], 896` at pc 0x4 of a pixel shader. The shader is under
  heavy SGPR pressure and spills its descriptors with static-lane `v_writelane` into one VGPR: the V# from EUD+896
  (lanes 17-20), the V# from EUD+848 (lanes 14-16, 21), and an S# from EUD+720 (lanes 0-3), with more later.
  It reloads them with `v_readlane` into other quads (`s[96:99]`, `s[16:19]`, `s[88:91]`, `s[92:95]`) and uses them
  as V# and S#. `ShaderFindDynamicSLoadUse` (`ShaderResources.cpp`) drops the EUD+896 mapping, because the spill is a
  read that is neither a consumer nor a clobber, so `recompile_sload_from_extended` finds no row.
  - Excluded: simply ignoring the spill. A storage row's word 0 is the host binding index (`UpdateAddress48(i)` in
    `PrepareStorageBuffers`), and a sampler or texture row's word 0 is a host index too. The reloaded copy has no
    resource record at its register, so it would take the pixel guest-address path
    (`PixelScalarBufferUsesGuestAddress`) and read guest memory at that index.
  - Direction: carry descriptor provenance through static-lane spill slots, from the `v_writelane` of each word to a
    `v_readlane` reassembling all words in order with the slot not rewritten in between. Each reload consumer then gets
    a dynamic record for the same resource, so the runtime word 0 selects the binding as for the original register.
    T# halves assembled from two loads plus reloads in the same shader need their own evidence.
- **SGPR104 and SGPR105.** The RDNA2 scalar operand range is 0..105; the parser stopped at 103, and the shared lowering
  preconditions and compute-wave analyses used the same limit, so a pixel shader reading `s104` was refused as an
  invalid instruction before native wave analysis. The limits follow the ISA; the NGG passthrough proof, which keeps
  VCC in slots 104/105, rejects the two registers instead of aliasing them, and the fetch decoder bounds its SGPR table.
- **Non-final pixel Z export.** Target 8 (MRTZ) with `done=0` ahead of the color exports (`PixelZVsrc0Vm`) uses the
  same depth emitter as the final form; the depth-export proofs accept both.
- **SGPR104/105 in the shared lane-flow numbering.** The fragment mask-flow proof refused the same shader with "scalar
  destination out of range": the numbering shared by the lane-flow proofs (`ShaderLaneFlow.h`, NGG passthrough
  dependencies) packed SGPR0..103 and then VCC/EXEC/M0/SCC. It now holds SGPR0..105 followed by VCC (106), EXEC (108),
  M0 (110) and SCC (111). The 2688x1512 depth prepass, whose only mask use is the save, `s_wqm_b64 exec`, restore
  bracket around its derivative fetches, is then proven lane-local and runs on 32-wide host subgroups without the
  fragment wave transport.
- **`SW_256B_S` mip chains.** The 256-byte swizzle modes have no mip tail: every level is padded to whole 256-byte
  blocks and the chain is stored from the smallest level up, so level 0 ends the allocation (published address-library
  `ComputeSurfaceInfoMicroTiled`). `Gen5GetStandard256BTextureMipLayout` / `Gen5DetileStandard256BTextureMipChain`
  place and detile each level; tiled mip chains use the canonical pitch (width), not the linear row rule. A test pins
  the offsets of a 16x16, five-level chain and round-trips every level against the per-element equation.
- **MRTZ export with no channel.** Target 8 with `en=0`, `done=1`, `vm=1` carries only the valid mask, exactly like
  the NULL target, and decodes as `NullVmDone` (the discard tail of a depth-exporting shader).
- **Depth export with control flow.** `ShaderCodeHasSafePixelDepthExport` admitted only straight-line programs. It now
  admits forward control flow when every `S_ENDPGM` closes a block that holds the exact Pixel-Z export or is a discard
  tail (`s_mov_b64 exec, 0`, then the valid-mask-only export); a branch target starts a new block, a conditional
  branch's fall-through continues its own, and indirect jumps stay refused. A tail that keeps lanes alive without a
  depth write is still refused (tested).
- **Pixel translation failures name their cause.** `ShaderRecompilePS` now fails with the SPIR-V assembler/validator
  message instead of returning an empty module that a later assertion reported without a reason.
- **AudioOut2 context parameters (local candidate; held).** `ContextQueryMemory` and `ContextCreate` compared the whole 0x40-byte block with
  two captured profiles. A gdb trace shows this title filling six leading 32-bit fields (max ports, max object
  ports, guaranteed object ports, queue depth, grain at +0x10, flags; for example 1/0/0/2/256/1 and 8/64/0/2/256/1)
  and leaving the reserved tail uninitialized after `ContextResetParam`, so all eleven attempts were refused; the
  engine fell back to `sceAudioOut` and its `sceAudioOut2PortCreate` calls failed with INVALID_PORT. The six fields
  are read and the reserved area ignored (grain bounded to the 64..4096 frames the host mixer handles); both contexts
  and their ports now open. Titles that used to fall back now take the AudioOut2 path: the camp brawler needed
  `sceAudioOut2Set3DLatency` (NID TViD1EZXkNI), which accepts the system user with latency mode 1 or 2 and refuses
  anything else. It returns success without storing or applying the setting; the current absence of object-port
  rendering does not establish that this is a valid no-op. Context port limits and flags also lack an evidenced
  admission contract. The whole context/queue/latency bundle remains local. The camp brawler (203 s) and the
  isometric roguelike (163 s) advanced with this full candidate, not with the selected published source.

Remaining startup race (timing dependent; upstream cause unresolved). About 26-32 s after launch, the multiband-compressor submix
effect takes its channel-count re-initialization path and reads its preset weak pointer twice in the same
audio-render call: the first read succeeds, the second returns null and the guest locks the preset's mutex at offset
0x58, which faults in `PthreadMutexLock`. Earlier passing runs did not take that path. A subsequent bounded debugger
run did take it: the channel count changed from zero to eight and both preset reads returned the same valid pointer,
before a later shader refusal. Channel re-initialization alone therefore does not imply the null fault. At the fault
the weak pointer and its object item are valid again and only 26 of 21416
objects are unreachable; because gdb stops the other threads milliseconds later, this does not exclude a
garbage-collection mark phase at the moment of the second read. Excluded: input (two runs without a route fault at
27-28 s), guest thread priorities (lower host nice for low-priority guest threads: 4 of 4 still fault), low-memory
garbage collection (the title queries only the direct memory size, 16 GiB). Most Silent runs fault there;
debugger-attached runs pass more often.

The subsequent audio trace created eight-channel MAIN ports and a stereo type-6 port, then destroyed and recreated
the type-6 port immediately before the zero-to-eight channel transition. It made no `PortGetState` or
`GetSpeakerInfo` calls during the observed startup, so incorrect replies from those APIs do not explain this
transition. The temporal association with type-6 creation is not proof of causality. `ContextCreate` also discarded
the parsed queue depth and retained the default of four: the captured depth-two profile reported four available
grains. Propagating the defined field makes it report two; the real context lifecycle regression failed before the
change and the focused AudioOut2 suite passes all 19 tests afterward in the local bundle. The bundle remains held
from publication. Advance/reservation timing, type-6 routing and
the weak-pointer race remain unverified contracts; the queue repair is not claimed to resolve the race.
The focused comparison of four local behavioral references does not settle those contracts: port-state writers
agree on the channel offset but use different sizes and channel-selection rules; Advance ranges from reservation
or clock updates to a no-op, with different Push pacing and queue-drain rules. Type 6 is generally separated from
the main speaker path, but routing differs, and user-ID validation and destroy lifetimes also disagree. These are
implementation observations, not hardware specifications; no further audio behavior is changed from them alone.

**Standalone VCC high-word destinations.** The captured pixel shader `0x01000150/0xed53afa8` loads one scalar-buffer
dword into `vcc_hi`, immediately before a scalar move. The zero-resource lowering used the indexed operand helper,
which supported the VCC pair but omitted standalone VCC_HI and emitted `OpStore % %uint_0`. The indexed helper now
maps the one-word destination to the existing uint variable; zero-resource lowering also validates all destinations
before appending any stores. The real fragment fixture reproduces the assembler's invalid ID before the change and
passes assembly and validation afterward. Translator version 119 invalidates the earlier modules. The next bounded
debugger run emitted `OpStore %vcc_hi %uint_0` for the same captured shader and reached a later storage-buffer
materialization refusal after about 49 seconds. The two captures remained mostly dark; this is translation progress,
not a visual or gameplay acceptance.

**Storage-buffer failure provenance.** The next strict debugger run passes the captured shader and refuses a
copied static scalar-memory span after about 47 seconds. The descriptor declares a much larger range, but the
existing exact-access proof requests only 2296 bytes. The CPU copy succeeds; the critical transient upload returns
null, and this branch never calls GPU-object creation. There is no GPU-heap allocation or overlap for that span.
`TransientBufferPool::Upload` can return null on this route only when `Acquire` rejects its capacity predicate:
per-usage entries, total entries or byte capacity. That capture does not identify which limit failed. A subsequent strict run with the bounded critical-pool
rejection report refuses a four-byte unmapped-base carrier at the 512-entry per-usage limit: 674 total entries and
16,234,178 bytes remain below the total-entry and 16 MiB byte limits. Its failure bits identify only the per-usage
cap. The report uses the existing capacity predicate and emits at most eight critical refusals; ordinary snapshot
fallbacks remain quiet. No duplicate immutable payload sample has been captured. Do not increase the limits or add
content reuse without proving the needed lifetime and byte-identity contract.
The failure diagnostics distinguish the preparation paths (including the separate early unmaterialized-range fatal),
the exact byte arguments, CPU-copy result,
wrapper versus Vulkan handle, mapping, overlapping objects and backing provenance. Translation and failure
localization are verified; the startup race, buffer materialization and gameplay remain open.

Also observed in the regression sweep: the isometric roguelike faulted twice (9-11 s, of six runs) in a host `memcpy`
called by the guest at eboot+0x308054, a row copy into a staging buffer whose base pointer, returned through the
out-parameter of eboot+0x31a8e0, is null. It advanced in the other runs, including the full 163-second route.
