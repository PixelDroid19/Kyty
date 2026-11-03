# Tessellation (hull and domain stages) design

Status: design only. No code is changed by this document. Written 2026-10-29 against commit
`b24ae77ab`. Code paths are relative to `source/`. Files under
`source/emulator/` also omit the `emulator/` part (for example `src/Graphics/GraphicsRenderDraw.cpp`
is `source/emulator/src/Graphics/GraphicsRenderDraw.cpp`). Every statement is tagged with its
source. Items tagged **[unverified]** are leads that have not been confirmed against evidence and
must not be implemented as facts.

Sources named in this document:

- **RDNA2 Shader ISA**: the AMD "RDNA 2" Instruction Set Architecture reference guide. It is
  cited by section number and table title only.
- Code citations are `file:line` at commit `b24ae77ab`.
- Other repository documents are cited by repository-relative path.

Scope: how a PS5 guest draws with the hull (HS) and domain (DS/ES) stages, what Kyty has
today, how the guest pipeline could map to Vulkan, and a staged plan with gates.

## 0. Decisions in this document

1. Tessellation has no translation in the base. The first deliverable is an explicit
   refusal of PATCH draws, plus the evidence that S0 collects. No translation is written
   before the evidence exists.
2. The target mapping is native Vulkan tessellation control and evaluation shaders with
   `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`. It is used only when the device exposes the
   `tessellationShader` feature. Without that feature, PATCH draws keep the explicit refusal
   from S0.
3. A compute emulation of the tessellator is not chosen. It is a later decision, because it
   needs its own tessellator with exact guest semantics.
4. PATCH draws are refused explicitly from S0 at both draw entry points, before any other
   check in those entries, so that no skip site can drop a PATCH draw first (section 6, S0).
   PATCH is primitive type 9 **[unverified]** (section 10, P14). The refusal uses the existing
   not-implemented macro `EXIT_NOT_IMPLEMENTED` (`include/Kyty/Core/DbgAssert.h:85-86`), not a
   hand-written `Report` call. The macro calls `dbg_not_implemented_handler`
   (`lib/Core/src/DbgAssert.cpp:115-133`), which calls `Core::BringUp::ReportNotImplemented`
   (`lib/Core/src/BringUp.cpp:801-804`). The identity is the stringified macro argument:
   `patch_draw_indexed` and `patch_draw_auto`.
   - Why the macro: its unsafe continue branch returns to the caller without exiting
     (`DbgAssert.cpp:118-122`). Its strict halt prints the not-implemented line with file and
     line (`DbgAssert.cpp:125-127`). A hand-written pair of `Report` and `EXIT` would need its own
     Continue check, and `EXIT` sends an `exit` host fault instead of `not_implemented_halt`.
   - The macro does not avoid a host crash. When no debugger is attached, the strict path
     runs in this order. The decision hook emits `bringup_halt` (`BringUp.cpp:741`, hook at
     `src/Agent/AgentLifecycle.cpp:108-137`). The handler sends `not_implemented_halt`
     (`DbgAssert.cpp:130`), which the agent records as `bringup_halt`
     (`AgentLifecycle.cpp:414-421`), and then runs `ShutdownActiveSubsystems`
     (`DbgAssert.cpp:131`). The macro then calls `dbg_exit(321)` (`DbgAssert.h:41-47`), which
     sends a `dbg_exit` host fault. The agent records that fault as a `host_crash` Fatal event
     (`DbgAssert.cpp:157-161`, `AgentLifecycle.cpp:441-443`). Under a debugger, `ASSERT_HALT`
     traps instead of calling `dbg_exit`.
   - Strict mode, the default: `Report` returns Halt (`BringUp.cpp:737-743`). The default mode
     is strict when `KYTY_BRINGUP_MODE` is unset or set to `strict` (`BringUp.cpp:367-369`).
     The process prints the not-implemented line and exits with status 321, which is status 65
     on POSIX (`DbgAssert.cpp:163`, `DbgAssert.h:43`). Whether a run is classified
     `unsupported` is **unverified** and is not claimed here (section 6, milestone; section
     10, P0).
   - Unsafe bring-up mode, opt-in and not a supported runtime mode: `Report` returns Continue.
     The draw is not emitted. The site is counted under `continues_by_feature` and logged once
     per site (`lib/Core/src/BringUp.cpp:783-786`). Unsafe mode still halts when the burst
     breaker trips (`BringUp.cpp:762-779`).

   Today a PATCH draw is dropped by one of several existing skip sites, and none of them
   counts PATCH draws (section 1). A strict-mode refusal can stop a title that reaches a
   PATCH draw today. That is a frontier change. The S0 code change is blocked until the S0
   gate (section 6) has been run, and the maintainer decides on the result. The reason for
   the change: AGENTS.md forbids skipped draws as a fix, so a dropped draw is not evidence
   that the title is correct. The frontier rule allows a replaced behaviour to change only
   when focused evidence shows that it was wrong.
5. Guest partitioning modes with no direct Vulkan equivalent (for example power-of-two
   partitioning) are refused, never approximated by another spacing.
6. How the LS stage is placed (a separate vertex pass, or inline inside the TCS) is an open
   decision, D-LS. It is taken at the start of S2 (section 10).

## 1. Current state in the base

| Area | Location | Observed state |
| --- | --- | --- |
| Shader stage types | `include/Emulator/Graphics/Shader.h:41-48` | `ShaderType` has only `Unknown, Vertex, Pixel, Fetch, Compute`. No hull, local or domain type. No file whose name contains `essell` exists in the export. |
| Vulkan pipeline | `src/Graphics/GraphicsRenderPipeline.cpp:900` | `pipeline_info.pTessellationState = nullptr;` |
| Input assembly | `src/Graphics/GraphicsRenderPipeline.cpp:548` | `topology` comes from `static_params->topology`. No path produces PATCH. |
| Device features | `src/Graphics/Window.cpp:2289-2290` | `geometryShader`, `depthClamp` and others are copied. `tessellationShader` is not queried anywhere under `emulator/`. |
| Draw plan resolver | `src/Graphics/GraphicsRenderDraw.cpp:1025-1072` | `switch (primitive_type)` at 1036 covers types 1-7, 17 and 19. Type 9 (PATCH **[unverified]** value, P14) falls to `default: return false;` at 1067. |
| Indexed draw entry | `src/Graphics/GraphicsRenderDraw.cpp:1635` (function), guards 1640-1648, env A/B switches 1650-1658, render lock 1660, `ResolveGeometryDraw` 1665, skip check 1666 | The `EXIT_IF` guards end at 1648. Insertion point for the refusal: after line 1648. |
| Auto draw entry | `src/Graphics/GraphicsRenderDraw.cpp:2866` (function), guards 2872-2876, env A/B switches 2879-2885, render lock 2888, `ResolveGeometryDraw` 2893, skip check 2894 | The `EXIT_IF` guards end at 2876. Insertion point for the refusal: after line 2876. |
| Geometry pass admission | `src/Graphics/GraphicsRenderDraw.cpp:2731-2736` | Admits only primitive types 1, 4 and 6. A PATCH draw can never take the geometry-pass branch. |
| Geometry-engine skip | `src/Graphics/GraphicsRenderDraw.cpp:1230-1301, 1276-1286` (called from `SkipGuestDrawForUnsupportedVertexState`, 1306-1315) | `ShouldSkipUnsupportedGeShader` returns true on NextGen configurations when one of four checks fails (1265-1274). The first is that the stage mask is not the passthrough value. The detail is built at 1276-1284, and the draw is recorded with `GraphicsRecordSkippedDraw` at 1286 under reason `unsupported_ge_state`. The Warn event is in `src/Graphics/GraphicsRenderCommandBuffer.cpp:724-746`. |
| Invalid vertex shader skip | `src/Graphics/GraphicsRenderDraw.cpp:1309-1311` | Also inside `SkipGuestDrawForUnsupportedVertexState`. |
| Other indexed skip sites | `src/Graphics/GraphicsRenderDraw.cpp:1672-1695` (env A/B), `1722-1757` (colour decompress, fast-clear, colour resolve, depth-stencil copy), `1759` (`shader_disable_reason`), `1818`, `1834` | Each returns before the resolver. All of them lie after the refusal insertion point (1648). |
| Indexed resolver skip | `src/Graphics/GraphicsRenderDraw.cpp:1850-1859` | `if` at 1850. Rate-limited debug log at 1855-1856, diagnostic dump at 1858, `return` at 1859. The guest sees no error. |
| Auto resolver skip | `src/Graphics/GraphicsRenderDraw.cpp:3053-3062` | `if` at 3053. Rate-limited debug log at 3058-3059, diagnostic dump at 3061, `return` at 3062. Earlier auto skip sites: env A/B at 2879-2885 and 3007, `invalid-vs-resources` at 3037, and the depth-stencil copy at 2943-2962, `shader_disable_reason` at 2965. |
| Depth-stencil copy path | `src/Graphics/GraphicsRenderDraw.cpp:2400-2698` (`GraphicsRenderDepthStencilCopy`), resolver call 2430, log-and-continue branch 2452-2457 | Its only callers are 1754 (indexed) and 2960 (auto). Both are after the refusal insertion points. |
| Draw routes into the entries | `src/Graphics/GraphicsRun.cpp:2444` (`DrawIndex`), 2481 (`DrawIndexOffset`), 2519 (`DrawIndexIndirect`), 2626 (`DrawIndirect`), 2814 (`DrawIndexAuto`) | Each calls `GraphicsRenderDrawIndex` (2475, 2513, 2620) or `GraphicsRenderDrawIndexAuto` (2679, 2863). The opcode parsers call them at `src/Graphics/GraphicsRunOpParsers.cpp:357, 371` (DrawIndex), `425` (DrawIndexOffset), `444, 457` (DrawIndexAuto), `533` (DrawIndexIndirect) and `548` (DrawIndirect). Zero-count indirect draws return before the entry (`GraphicsRun.cpp:2563-2577`, `2662-2664`); no PATCH work is dropped there. Two multi-draw HLE builders also exist: `GraphicsDcbDrawIndirectMulti` (`src/Graphics/Graphics.cpp:6510`, header `0xc0082c00`, opcode 0x2C, `IT_DRAW_INDIRECT_MULTI`, `include/Emulator/Graphics/Pm4.h:39`) and `GraphicsDcbDrawIndexIndirectMulti` (`Graphics.cpp:6551`, header `0xc0083800`, opcode 0x38, `IT_DRAW_INDEX_INDIRECT_MULTI`, `Pm4.h:46`). They are exported at `src/Graphics/GraphicsHleExports.cpp:232-233`. No dispatch entry names either opcode, so the CP dispatch reports a missing handler and exits with `unsupported PM4 op` (`src/Graphics/GraphicsRun.cpp:2317-2320`). The multi-draw route ends in an explicit EXIT today, not in a skip. |
| Prim state creation | `src/Graphics/Graphics.cpp:2844` (function), `2862-2886` (checks) | An HS owner is required for prim type 9 (PATCH **[unverified]** value, P14): `prim_type == 9u && hs != nullptr` (2881). Otherwise the function exits (2884). |
| Stage ownership bits | `src/Graphics/Graphics.cpp:2863-2865` | `gs_enable = 1<<5`, `hs_enable = 1<<2`, `ls_hs_enable = 0x7`. These are our own interpretation, not defined by RDNA2 Shader ISA (section 2.3). For a non-null HS, the checks at 2874-2879 require bit 2 set and bit 5 clear in the HS stage value. |
| Tessellation register writes | `src/Graphics/GraphicsRunJmpTables.cpp:475-481`; lambda `ignore_cx` at 432-437 | `VGT_HOS_MAX_TESS_LEVEL` (475), `VGT_HOS_MIN_TESS_LEVEL` (476), `VGT_TESS_DISTRIBUTION` (479), `VGT_LS_HS_CONFIG` (480) and `VGT_TF_PARAM` (481) are accepted and discarded. The comment at 469-471 says that geometry depending on them needs the proper setters later. |
| Reset defaults | `src/Graphics/Graphics.cpp:1298-1304` | `VGT_LS_HS_CONFIG` 0 (1298), `VGT_TESS_DISTRIBUTION` 0x88101000 (1303), `VGT_TF_PARAM` 0 (1304). |
| TF ring base export | `src/Graphics/Graphics.cpp:925-937`; export table `src/Graphics/GraphicsHleExports.cpp:56` | `AllocAligned` (0x20000, 256) at 929, `Free` at 930, `AllocFixed` at 931 (failure only logged at 932), `Free(addr)` at 933, then `return addr` at 937. **Defect:** the returned address was freed at 933. Not fixed in this design (S1). |
| Driver globals | `src/Graphics/Gen5Driver.cpp:30-34`; setters at 183 and 193 | `GraphicsDriverSetTFRing` stores base and size (186-187). `GraphicsDriverSetHsOffchipParam` stores three values (196-198). Exports at `src/Graphics/GraphicsHleExports.cpp:326-327`. No load of these globals exists in `src/Graphics/`. |
| Shader binary types | `src/Graphics/Graphics.cpp:2335-2357` | Type 3 (HS) maps to `SPI_SHADER_PGM_LO_LS` (2352). Type 7 (HS back half) maps to `SPI_SHADER_PGM_LO_HS` (2354). Types 4, 5 and 8 carry no program address (2355-2357). |
| Register addresses | `include/Emulator/Graphics/Pm4.h` | `VGT_HOS_MAX_TESS_LEVEL` 0x286 (578), `VGT_HOS_MIN_TESS_LEVEL` 0x287 (579), `VGT_TESS_DISTRIBUTION` 0x2D4 (607), `VGT_SHADER_STAGES_EN` 0x2D5 (608), `VGT_LS_HS_CONFIG` 0x2D6 (609), `VGT_TF_PARAM` 0x2DB (610), `VGT_HS_OFFCHIP_PARAM` 0x24F (1010), `VGT_TF_MEMORY_BASE` 0x250 (1011). HS program and user-data registers: `SPI_SHADER_PGM_CHKSUM_HS` 0x100 (926) to `SPI_SHADER_PGM_RSRC2_HS` 0x10B (934), and `SPI_SHADER_USER_DATA_HS_0` 0x10C (935). |
| Unit tests | `source/unit_test/src/emulator/` | No test exercises tessellation behaviour. Two tests reference Vulkan tessellation structures or flags only: `source/unit_test/src/emulator/UnitTestEmulatorComputeWaveVulkan.cpp:200-201` and `source/unit_test/src/emulator/UnitTestEmulatorNativeWaveAdmission.cpp:518`. `UnitTestEmulatorPrimitiveState.cpp` covers `GraphicsCreatePrimState` with an HS owner and is the natural place for a PATCH-refusal death test if one is later wanted (section 7). |

Consequence. Today a PATCH draw is never emitted. Which skip drops it depends on the
configuration, and none of the paths counts PATCH draws:

- On NextGen configurations, `ShouldSkipUnsupportedGeShader` (`GraphicsRenderDraw.cpp:1230-1301`)
  skips a draw when one of four checks fails (1265-1274). The first is the context stage mask
  not being the passthrough value (`unsupported_stage_mask`, 1265). Whether a PATCH draw carries
  such a mask is **[unverified]**. The HS checks at `Graphics.cpp:2874-2879` constrain the HS
  stage register value. They do not test the context mask that 1265 tests, so they do not show
  that this check runs first for a PATCH draw. When the skip fires, the draw is counted and
  published as a Warn event under `unsupported_ge_state`. The exact stage value of an HS draw is
  **[unverified]**.
- Otherwise a resolver skip is reached (1850-1859 and 3053-3062), or an earlier skip in the
  same entry (section 1 table).
- For prim type 9 without an HS owner, `GraphicsCreatePrimState` exits (`Graphics.cpp:2881-2884`).

So on NextGen a PATCH skip may be counted and visible, but it is still a skip and not a refusal,
and it is not labelled as PATCH. Whether any title that works today draws PATCH is unknown.
S0 measures it.

## 2. Guest pipeline (what the design must model)

### 2.1 Stages

The stage order below is the expected guest flow. Only the existence of a merged LS+HS
wave is supported by RDNA2 Shader ISA (section 2.2). The rest is **[unverified]** until
the S0 evidence exists.

1. **LS (local shader)**: vertex-like. RDNA2 Shader ISA shows LS and HS sharing one wave.
   How LS outputs reach HS is **[unverified]**.
2. **HS (hull shader)**: runs per control point and per patch. Where it reads LS data from
   and where it writes patch outputs and tessellation factors (LDS, off-chip memory, or the
   TF buffer) is **[unverified]**. The S0 evidence decides the HS output destination.
   RDNA2 Shader ISA has no HS section (section 2.4).
3. **Fixed-function tessellator**: reads the tessellation factors and generates domain
   points. The VGT registers that set its behaviour (spacing, winding, partitioning,
   primitive kind) are **[unverified]** in layout (section 2.3). No section of RDNA2 Shader
   ISA describes it (section 2.4).
4. **DS / ES (domain stage)**: vertex-like. It reads patch data from the off-chip ring and
   the generated domain location, and outputs like a VS, or feeds a GS.
5. **GS (optional)**, then **VS/PS** as already implemented for non-tessellated draws.

### 2.2 Wave initial state

Source: RDNA2 Shader ISA, section 3.12 "Initial Wave State", table "Front End Shader
(LS+HS)". The table is on page 24 of 283 in the copy of the vendor document used for this
design (the page header of that copy; not checked against the PDF itself).

| SGPR | Content | Enable |
| --- | --- | --- |
| 0 | HS user data address low | `SPI_SHADER_USER_DATA_LO_HS` |
| 1 | HS user data address high | `SPI_SHADER_USER_DATA_HI_HS` |
| 2 | Off-chip LDS base [31:0] | automatic |
| 3 | `{first_wave, lshs_TGsize[6:0], lshs_PatchCount[7:0], HS_vertCount[7:0], LS_vertCount[7:0]}` | automatic |
| 4 | TF buffer base [15:0] | automatic |
| 5 | Shared scratch offset | `SPI_SHADER_PGM_RSRC2_HS.scratch_en` |
| 6, 7 | HS shader address low / high | `SPI_SHADER_PGM_LO_HS` / `_HI_HS` |
| then | HS user SGPRs | `SPI_SHADER_PGM_RSRC2_HS.user_sgpr` |

The table gives field names and widths but not the bit position of `first_wave` in SGPR3
**[unverified]**. The "Vertex Shader (VS)" table and the "Geometry Shader (ES+GS)" table in
the same section gate their off-chip LDS pointer with `oc_lds_en`. The LS+HS table has no
such gate, and its rows are marked "automatically enabled".

### 2.3 Registers and their status

| Register | Address | Role (to verify) | Kyty status |
| --- | --- | --- | --- |
| `VGT_SHADER_STAGES_EN` | 0x2D5 | Stage enables. RDNA2 Shader ISA names `VGT_SHADER_STAGES.primgen_en` only, in the "Geometry Shader (ES+GS)" table. The HS and GS bit values used by Kyty are our own assumption. | Read by `GraphicsCreatePrimState`. Bit values unverified. |
| `VGT_LS_HS_CONFIG` | 0x2D6 | LS/HS configuration. Field layout not known here. Control point counts are expected here **[unverified]**. | Written and discarded (`GraphicsRunJmpTables.cpp:480`). |
| `VGT_TF_PARAM` | 0x2DB | Tessellation factor parameters (partitioning, distribution, factor mode). Layout unknown. | Written and discarded (481). |
| `VGT_TESS_DISTRIBUTION` | 0x2D4 | Distribution across shader engines. No effect on results in a single-device emulator **[unverified]**. | Written and discarded (479). |
| `VGT_HOS_MAX_TESS_LEVEL` / `VGT_HOS_MIN_TESS_LEVEL` | 0x286 / 0x287 | Clamp of tessellation levels. Exact semantics **[unverified]**. | Both discarded (475-476). |
| `VGT_HS_OFFCHIP_PARAM` | 0x24F | Off-chip buffer parameters. | Appears only in a default-value table (`Graphics.cpp:1623`, reset 0). No write handler exists under `source/emulator/`. The HS offchip values reach the driver through the HLE setter (`Gen5Driver.cpp:196-198`). |
| `VGT_TF_MEMORY_BASE` | 0x250 | TF buffer base. | Appears only in a default-value table (`Graphics.cpp:1625`, reset 0). No write handler exists under `source/emulator/`. The TF base reaches the driver through the HLE setter (`Gen5Driver.cpp:186`). |
| `SPI_SHADER_PGM_*_HS` | 0x100-0x10B, 0x10C (Pm4.h:926-935) | HS program and user-data registers. | Used by the shader binary mapping (`Graphics.cpp:2335-2357`). |

No field layout in this table may be coded from memory. Each field is decoded only from a
live trace of the guest, a published contract, or the guest's own disassembly, in that
order (AGENTS.md, rule "Never invent guest behavior").

### 2.4 What RDNA2 Shader ISA does not cover

- It has no hull or domain shader section. The words "tessellation" and "hull" do not occur.
  "Domain" occurs in the introduction ("data domain"), in the "Global Data Share (GDS)"
  overview ("domain launch ordered append/consume"), in integer-domain wording of instruction
  descriptions, and in a note on the fract instruction ("other domains"). None of these refers
  to a hull or domain shader.
- The fixed-function tessellator, the TF buffer record layout, and the off-chip ring record
  layout are not described.
- In section 3.12, a note mentions combined HS+LS loads, and the "Front End Shader (LS+HS)"
  table (section 2.2) describes the LS+HS wave. These are the only tessellation-related content
  found in this search.

So the semantics of the TF buffer and of the tessellator must come from S0 evidence.

## 3. Mapping to Vulkan

### 3.1 Target (primary): native tessellation shaders

| Guest stage | Vulkan stage | Notes |
| --- | --- | --- |
| LS | Open decision D-LS: a separate vertex pass, or inline inside the TCS | The guest runs LS and HS in one merged wave (RDNA2 Shader ISA, section 3.12). The handoff from LS to HS is **[unverified]**. Section 10 lists the evidence each option needs. |
| HS | tessellation control shader (TCS) | Reads LS data from LDS, which must be rewritten as TCS input reads (`gl_in[]`). Writes tess factors as `gl_TessLevelOuter/Inner`. Patch outputs as `patch` variables. |
| DS / ES | tessellation evaluation shader (TES) | Reads patch data as `patch` inputs and vertex data from `gl_in[]`. |
| GS | existing geometry path | Only when `VGT_SHADER_STAGES_EN` says GS is active. |

Pipeline requirements (to verify against the Vulkan specification; see section 3.4):

- `topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`, and `pTessellationState` non-null with
  `patchControlPoints` equal to the guest input control point count. Currently
  `GraphicsRenderPipeline.cpp:900` sets nullptr, and line 548 only takes a topology from
  `static_params`.
- The device feature `tessellationShader` must be queried in `Window.cpp` near line 2289
  and enabled. Today it is not queried at all.
- The TCS output vertex count (`OutputVertices` execution mode) and the TES spacing,
  winding and primitive kind (execution modes) come from the guest registers, not from the
  pipeline state. The Vulkan tessellation state holds only `patchControlPoints` and
  reserved flags (Vulkan tessellation chapter excerpt, to verify).

**The central translator problem.** On the guest, LS to HS and HS to DS data go through
LDS and the off-chip ring, addressed by computed offsets. Vulkan TCS and TES read per-vertex
and per-patch variables by declared location or by `gl_InvocationID`. The translator must
decode each LDS or off-chip address into `(vertex index, attribute slot)` or
`(patch index, patch slot)`. Any address that is not statically decodable is refused with an
explicit `EXIT` (the existing style), never guessed.

**Tess factors.** The HS writes factors to the TF buffer. The TF base is given in two
places: SGPR4 `[15:0]` of the LS+HS wave (RDNA2 Shader ISA, section 3.12, table "Front End
Shader (LS+HS)"), and the `VGT_TF_MEMORY_BASE` register (0x250). Which source is
authoritative, and how the upper bits are formed, is an S0 question. The translator maps the
factor stores to `gl_TessLevelOuter/Inner` only after the S0 evidence confirms the record
layout. Until then the mapping is undefined, and PATCH draws stay refused (decision 4).

**Draw count.** No code in the base interprets the guest count of a PATCH draw, because the
draw is refused before a count is used. The design assumes the count is in control points.
That is **[unverified]** until the S0 evidence measures it on a PATCH draw. A count that is
not a multiple of the input control point count is refused explicitly, never rounded or
truncated. The source of the input control point count (`VGT_LS_HS_CONFIG` or another
register) is an S0 question.

**Guest-visible outputs.** If DS writes memory visible to the guest (not only the
rasterised output), the mapping needs a read-back. That is S4.

### 3.2 Alternative: compute emulation (later decision)

A compute pass would run the tessellator in software and write triangles to a buffer, then
draw them as `TRIANGLE_LIST`. It is an undecided later option (decision 3). It would be
considered only if the device lacks `tessellationShader`, or if a guest mode has no Vulkan
equivalent. It needs its own tessellator with exact spacing and partitioning rules. Those
rules come from evidence, not from any reference implementation. It is not scheduled in
S0-S4. Power-of-two partitioning is refused whether or not this option is later chosen
(section 3.3).

Without `tessellationShader`, the draw is refused explicitly with the reason, not emulated
silently.

### 3.3 Mode mapping (contract, populated only from evidence)

| Guest mode | Vulkan target | Status |
| --- | --- | --- |
| Equal spacing | `SpacingEqual` | Guest encoding unknown. The table cannot be filled before S0. |
| Fractional odd / even | `SpacingFractionalOdd` / `SpacingFractionalEven` | Guest encoding unknown. |
| Power-of-two partitioning | none | **Refused.** Vulkan has no such spacing. Approximating it by equal spacing changes the guest-visible geometry silently. |
| Triangles / quads / isolines / points | `Triangles` / `Quads` / `Isolines` / `PointMode` | Quads and isolines are S4. |
| Winding | `VertexOrderCw` / `VertexOrderCcw` | Guest encoding unknown. |

The S1 unit test for this table needs S0 evidence first. Writing it earlier would encode a
guess.

### 3.4 Vulkan facts to verify before sign-off

Sources: the Vulkan specification's tessellation chapter (an excerpt was fetched; it is
partial) and search snippets for the pipelines chapter and `VkPhysicalDeviceLimits`. None was
read in full.

- **[verify]** Tessellation is enabled when both TCS and TES are present in the pipeline.
- **[verify]** `PATCH_LIST` topology and a non-null `pTessellationState` are required in that
  case. The exact valid-usage texts were not confirmed in this pass.
- **[verify]** `patchControlPoints` must be greater than 0 and not greater than
  `maxTessellationPatchSize`. `OutputVertices` is bounded the same way.
- **[verify]** The maximum tessellation generation level (`maxTessellationGenerationLevel`)
  is a device limit. Guest level clamps must be checked against it.
- **[verify]** Vulkan has no power-of-two spacing (section 3.3).

## 4. Known driver pitfalls (observations, not facts)

- **[unverified]** An external discussion reports, for a specific driver version, that when
  a TCS writes a per-vertex output at a dynamic index, the TES reads zero from its
  non-constant patch outputs. The suggested fix is to use constant offsets when the offset
  is uniform across lanes.
- **Our handling:** prefer constant offsets wherever the decoded index is provably uniform.
  Verify with a standalone GLSL repro on each test host before relying on the
  pattern. Record the result in this document. Do not generalise the observation to other
  drivers.

## 5. Gaps to close, by location

| Gap | Location | Stage |
| --- | --- | --- |
| PATCH draws can be dropped by any skip site in the two entries and no PATCH count exists | Refusal goes after `GraphicsRenderDraw.cpp:1648` (indexed) and after `2876` (auto). Sites that the refusal precedes, so they cannot take a PATCH draw: `1650-1658`, `1660`, `1665-1666` (geometry-engine and invalid-VS skips, 1230-1315), `1672-1695`, `1722-1733`, `1736-1757` (copy path), `1759`, `1818`, `1834`, `1850-1859`; auto: `2879-2885`, `2888`, `2893-2894`, `2898-2919`, `2929-2940`, `2945-2962` (copy path), `2965`, `3007`, `3037`, `3053-3062`. | S0 |
| Multi-draw indirect opcodes `IT_DRAW_INDIRECT_MULTI` (0x2C) and `IT_DRAW_INDEX_INDIRECT_MULTI` (0x38) | No dispatch handler. The CP dispatch exits with `unsupported PM4 op` (`GraphicsRun.cpp:2317-2320`), so no draw entry is reached and no PATCH draw is skipped on this route. The S0 refusal does not cover this route. | Recorded. A future handler must route PATCH draws through the S0 refusal. |
| Depth-stencil copy path: resolver call and log-and-continue branch | `GraphicsRenderDraw.cpp:2430` (`GraphicsResolvePrimitiveDrawPlan`), `2452-2457` (logs and continues). Called only from 1754 and 2960, after the refusal, so unreachable for PATCH after the S0 refusal. Before S0 the path is reachable for PATCH when no earlier check in the entry skips the draw. The indexed copy branch (1736-1757) and the auto copy branch (2945-2962) run when the render control requests a depth or stencil copy, and they have no PATCH check. The resolver returns false for type 9, so the branch at 2452-2457 logs and continues. The A/B gate (section 6) confirms the post-S0 state. | S0 (no change) |
| Skip sites that stay for other primitive types (out of scope) | `GraphicsRenderDraw.cpp:1850-1859` and `3053-3062` (type 19 with count not a multiple of 4, and unknown types through `1067`), the geometry-engine and invalid-VS skips for non-PATCH draws (1230-1315), `GraphicsRun.cpp:2563-2577` and `2662-2664` (zero-count indirect draws) | Recorded only; a separate item |
| No tessellation stage types | `Shader.h:41-48` | S2 |
| `pTessellationState = nullptr` | `GraphicsRenderPipeline.cpp:900` | S3 |
| `tessellationShader` not queried or enabled | `Window.cpp:2289-2290` (neighbourhood) | S3 |
| PATCH not in the draw plan | `GraphicsRenderDraw.cpp:1036-1067` | S3 |
| Tess registers discarded | `GraphicsRunJmpTables.cpp:475-481` | S1 |
| TF ring stub returns a freed address | `Graphics.cpp:925-937` | S1 (fix the defect; a freed address must never be returned) |
| TF and off-chip values stored, never read | `Gen5Driver.cpp:30-34`, `186-198` | S1 (typed state), S2 (read) |
| Stage-enable bit values unverified | `Graphics.cpp:2863-2865` | S0 (evidence, section 6) |
| Prim state accepts PATCH only with an HS owner | `Graphics.cpp:2881-2882` | S3 (no change before then) |

**Output mode rule (local precedent, unverified here).** The bring-up history records
"explicit GS output when GS_EN, HS output for tessellation without GS" (`docs/bringup/history.md`,
record near line 2180; per-title record near line 626). Neither file is in this export. The
rule must be re-checked against the history file before S3.

## 6. Staged plan

Each stage has a verifiable milestone and a gate. A stage that fails its gate is not merged,
and the next stage waits.

### S0 Explicit refusal of PATCH draws, and evidence

- **Work, step 1 (the only code change in S0):**
  - In `GraphicsRenderDrawIndex` (`GraphicsRenderDraw.cpp:1635`), insert after the `EXIT_IF`
    guards (after line 1648) and before the env A/B switches (1650):

    ```
    const bool patch_draw_indexed = ucfg->GetPrimType() == 9u;
    if (patch_draw_indexed)
    {
    	<one bounded fields line>
    	EXIT_NOT_IMPLEMENTED(patch_draw_indexed);
    	return;
    }
    ```

    The value 9 is **[unverified]** (section 10, P14).

  - In `GraphicsRenderDrawIndexAuto` (`GraphicsRenderDraw.cpp:2866`), insert the same block
    after line 2876 with the identity `patch_draw_auto`.
  - Why here: all skip sites of both entries are after these insertion points (section 5).
    The primitive type is read from `ucfg->GetPrimType()`, the same source the resolver uses
    (`GraphicsRenderDraw.cpp:1850`, `3053`).
  - Why the macro and not `Report`: the macro keeps the existing unsafe continue branch
    (`DbgAssert.cpp:118-122`). A `Report` call followed by `EXIT` would need its own Continue
    check, and `EXIT` sends an `exit` host fault instead of `not_implemented_halt`. Both routes
    end in a `host_crash` event, because the final `ASSERT_HALT` calls `dbg_exit(321)` when no
    debugger is attached (section 0, decision 4; section 10, P0). The macro does not change the
    terminal event. The existing indirect-register refusal at
    `src/Graphics/GraphicsRunOpParsers.cpp:901-907` uses `Report` and then `EXIT` (section 10,
    P1). It is not used as a pattern here.
  - Feature: `NotImplemented`. `kDefaultUnsafeConfig` enables it and the Graphics subsystem
    (`BringUp.cpp:69-77`). `kDefaultStrictConfig` enables no feature (`BringUp.cpp:79-87`),
    so strict mode halts. The subsystem comes from `ClassifySourceFile`, which maps paths
    containing `/source/emulator/src/Graphics/` to Graphics (`BringUp.cpp:629, 644-647`).
  - Bounded fields line: the identity carries no stage mask, primitive type or count. The
    fields line (`ctx->GetShaderStages()`, `ucfg->GetPrimType()`, `index_count`) is written
    with `KYTY_LOG_LIMIT(Log::Level::Warn, 8, ...)` (`Log.h:162-170`). It is kept on purpose,
    because the continue line of the existing bring-up contract carries none of these fields
    (`BringUp.cpp:785-786`). It is best-effort: `Log::Emit` prints nothing under
    `PrintfDirection = Silent` (`src/Log.cpp:300-305`). The guaranteed evidence is the
    halt line (`dbg_not_implemented_handler`, `DbgAssert.cpp:127`, printed with `KYTY_LOGE`,
    which is `printf` on Linux, `include/Kyty/Core/Common.h:63-66`) and the once-per-site
    `KYTY_BRINGUP_CONTINUE` line in unsafe mode.
  - Evidence kept for PATCH draws: on NextGen, today's geometry-engine skip publishes the
    stage mask and primitive type (`GraphicsRenderDraw.cpp:1276-1286`). After step 1 that
    event is no longer published for PATCH draws. The stage mask is recorded only in the
    best-effort fields line above.
  - Env-gated dump helpers (`MaybeDumpIndexDrawSkip`, `MaybeDumpAutoDrawSkip`, lines 610-640)
    are not changed.
  - Effect on the diagnostic A/B switches: `KYTY_AB_SKIP_ALL_DRAWS`, `KYTY_AB_SKIP_INDEXED_DRAWS`
    and `KYTY_AB_SKIP_AUTO_DRAWS` skip every draw type before the old checks. After step 1 a
    PATCH draw under those switches is refused, not skipped. Other draw types are unchanged.
- **Work, step 2 (check of existing channels, no code).** The table below answers what the
  S0 questions can be answered with today.

  | Question | Existing channel | Result |
  | --- | --- | --- |
  | Strict mode: is the PATCH refusal visible? | stdout line `Not implemented (patch_draw_indexed) in <file>:<line>` (`DbgAssert.cpp:127`), process exit status 65 (`dbg_exit(321)` truncated by POSIX; `source/unit_test/src/emulator/UnitTestEmulatorAudio.cpp:150`) | Available. A `bringup_halt` event is also recorded, but the agent server is stopped before the run ends (section 10, P0). Counters are not readable after a strict halt (`AgentSubsystem.cpp:26-29`, `DbgAssert.cpp:31-37`). |
  | Unsafe mode: refusal count and identity | `KYTY_BRINGUP_CONTINUE ... identity=patch_draw_*` once per site (`BringUp.cpp:785-786`); `continues_by_feature`, `total_continuations` through the `diagnostics` tool (`src/Agent/Protocol.cpp:899-935`) | Available while the process runs. |
  | Skipped-draw counts | `GraphicsGetSkippedDrawCounts` (`include/Emulator/Graphics/GraphicsRender.h:581`) | Counts geometry-engine and invalid-vertex-shader skips only. No PATCH reason. |
  | Skip dump | `KYTY_DUMP_DRAW` (`GraphicsRenderDraw.cpp:626-640`) | Skip type and count only. |
  | Stage mask and primitive type of a PATCH draw | fields line (best-effort) | Not in the event stream after step 1. |
  | Other register values | Agent tools (`src/Agent/Protocol.cpp:140-164`), debug snapshot components (`src/Agent/DebugSnapshot.cpp:35-40`), `KYTY_TRACE_*` and `KYTY_DUMP_*` options found by grep | None reads context or shader registers. The capture tool produces PNG images. `KYTY_DUMP_INDIRECT` prints a set_base line (`src/Graphics/GraphicsRun.cpp:2404`), a draw_index_skip line (2572) and a draw_index line (2602). None prints a register value. |

  Result: after step 1, a PATCH refusal is visible in the halt line (strict) or the continue
  line and counters (unsafe). The stage mask and primitive type are best-effort only. No
  existing channel observes the other tessellation register values (LS/HS config, TF
  parameters, level clamps, ring bases, off-chip values). Those values therefore need step 3.
- **Work, step 3 (fallback, only with explicit approval; not started by this design).** A
  temporary, env-gated probe placed at the five `ignore_cx` sites
  (`GraphicsRunJmpTables.cpp:475-481`) and at the entry of `GraphicsCreatePrimState`
  (`Graphics.cpp:2844`). Its tag and environment variable name are chosen at approval. Its
  output is capped at 64 lines per run, it writes to stderr, and it is never committed. The
  diff must not contain any diagnostic tag before staging (AGENTS.md, temporary diagnostics).
- **Milestone (strict, bounded run):** a run of a title that draws PATCH shows the not-implemented
  line for `patch_draw_indexed` or `patch_draw_auto`, and exits with status 65. The run's
  PATCH refusal count is recorded with the host, GPU, driver and commit.
- **Milestone (classification), unverified:** the run is classified `unsupported` from a
  `bringup_halt` event. This is **not** claimed by this design. Under the current shared halt
  path the classification depends on a race and is expected to fall to `loader_failed` when
  the poll misses the event (section 10, P0). The classification milestone is closed only
  after P0 is decided and a bounded strict run confirms it.
- **Gate (A/B against the current binary). The S0 code change is blocked on this gate.** It is
  not landable before items 1-4 pass.
  1. Titles that never reach a PATCH draw: their capture scores and their first-present point
     are unchanged.
  2. Titles that reach a PATCH draw are listed one by one, with the mode (strict or unsafe)
     they run in. If a title that is supported today reaches a PATCH draw, the maintainer
     decides, and only one of two paths is open: (a) focused evidence, recorded in the title
     record, shows that the dropped draw was itself wrong, which is the AGENTS.md frontier
     exception, and S0 may then merge; (b) otherwise S0 stays unmerged and the title stays as
     it is in the frontier record. No path keeps a silent skip of a PATCH draw.
  3. Positive control: before a zero count is trusted, an unsafe-mode run that reaches a PATCH
     draw shows the `KYTY_BRINGUP_CONTINUE` line for each identity and a non-zero
     `continues_by_feature` count. A strict run shows the halt line and status 65.
     (AGENTS.md, investigation workflow, rule 2.)
  4. Then `kyty_agent playable-regression`, and the three CTest gates named in AGENTS.md
     (`KytyArchitectureBoundaryIntegration.Repository`, `KytyArchiveDagIntegration.Repository`,
     `KytyHostShaders.CheckedInIncludes`).
- **Tests:** no new unit test. The refusal is a path choice over the existing macro and
  `Report` contract. Its evidence is the bounded runs (section 7).

### S1 Store the tessellation state and fix the TF stub

- **Work:**
  - Replace the `ignore_cx` entries for the five tessellation registers with typed storage in
    the context state, following the pattern of neighbouring stores. The comment at
    `GraphicsRunJmpTables.cpp:469-471` already says these fields need proper setters. Decode
    only the fields that the S0 evidence confirms.
  - Fix `GraphicsGetTheTessellationFactorRingBufferBaseAddress` (`Graphics.cpp:925-937`) so it
    never returns a freed address. Return a live allocation, or `EXIT` when the allocation
    cannot be made. The warn-only branch at line 932 is a silent fallback and is removed.
- **Milestone:** registers are stored and readable in the diagnostics dump. The TF base is a
  live address.
- **Gate:** A/B on the PATCH-drawing title and on a sample of titles that work today. The TF
  stub change can alter titles that call the export, so this A/B is mandatory.
- **Tests:**
  - Unit test of the decoder for the fields confirmed by the S0 evidence (place it under
    `source/unit_test/src/emulator/`, next to `UnitTestEmulatorGraphicsPackets.cpp`).
  - Unit test of the ring address stride math, only if the ring layout is confirmed (real
    logic).
  - No test of the spacing table or of the power-of-two refusal until S0 evidence exists
    (section 3.3).

### S2 Translator for LS and HS (SPIR-V)

- **Decision D-LS, taken first, with evidence:**
  - Option A, a separate vertex pass: LS outputs are captured to a buffer and read by the TCS.
    It needs the handoff layout from the S0 evidence.
  - Option B, inline: the TCS runs the LS program for each control point. RDNA2 Shader ISA,
    section 3.12, is the only local evidence (a merged LS+HS wave). It does not describe the
    handoff, so option B also needs the S0 evidence.
- **Work:** add stage types for TCS and TES (and the LS/HS front ends) in `Shader.h`. Put the
  new translation in focused new files next to their neighbours, for example
  `ShaderSpirvTessControl.cpp` and `ShaderSpirvTessEval.cpp`, each with a header comment,
  because the existing shader files are large. Implement the LDS and off-chip address decode
  with an explicit refusal for undecodable addresses.
- **Note for implementation:** adding translated output normally requires bumping the shader
  translator cache version. This item forbids the bump now. The implementation stage must
  decide it explicitly.
- **Milestone:** an HS and LS pair from a trace translates and validates with the existing
  SPIR-V validation path (to confirm).
- **Gate:** the decode unit tests pass, and no non-tessellated shader output changes.

### S3 PATCH_LIST pipeline and draw plan

- **Work:**
  - Query and enable `tessellationShader` in `Window.cpp`. Without the feature, the S0 refusal
    stays in place, and it is explicit.
  - Add the PATCH case to `GraphicsResolvePrimitiveDrawPlan` (`GraphicsRenderDraw.cpp:1036`).
    The S0 refusal is then limited to the case where the feature is absent or the S2
    translation is missing. The S0 block is removed from the entries at that point.
  - Set `pTessellationState` and `PATCH_LIST` at `GraphicsRenderPipeline.cpp:900` and `548`.
- **Milestone:** a PATCH draw of the S0 title reaches the GPU and produces an image.
- **Gate:** the integration test (section 7), a strict run of the title, `playable-regression`,
  A/B on the titles that share the path, and the three CTest gates named in AGENTS.md.

### S4 Off-chip rings, TF read-back, spacing, winding, quads, isolines

- **Work:** one sub-milestone per mode, each with its own evidence. Read-back is needed only
  if the guest reads TF or off-chip memory written by HS or DS.
- **Gate:** per mode, a trace-backed run and an A/B.

## 7. Tests

| Stage | Test | Type | Notes |
| --- | --- | --- | --- |
| S0 | Strict bounded run of a PATCH-drawing title: the not-implemented line for `patch_draw_*` and exit status 65 | Integration (run) | No unit test. The refusal is a path choice over the existing macro. |
| S0 | Unsafe bounded run of the same title: the `KYTY_BRINGUP_CONTINUE` line for each identity and a non-zero `continues_by_feature` count | Integration (run) | Positive control for the refusal count (section 6, gate 3). |
| S0 | Classification `unsupported` from a `bringup_halt` event | Integration (run) | Not claimed until P0 is decided (section 10). |
| S0 | A/B gate on the frontier titles (section 6, S0) | Gate | Required before the merge. |
| S1 | Decoder for the confirmed register fields | Unit | Real logic, next to `UnitTestEmulatorGraphicsPackets.cpp`. |
| S1 | Ring address stride math | Unit | Only if the ring layout is confirmed. |
| S1 | Mode mapping table | Unit | Only after the S0 evidence (section 3.3). |
| S0-gated | Refusal of power-of-two partitioning | Unit | Written only after the S0 evidence confirms the guest encoding of that mode. Until then the refusal is a design rule, not a tested contract. |
| S2 | Address decode for LDS and off-chip access | Unit | Real logic. |
| S3 | PATCH draw through the executor with guest LS, HS, DS and PS; target value checked on the GPU | Integration | Kyty integration harness (executor-level draw, GPU readback). Run with `ctest --no-tests=error`. |
| S0, S3 | Strict run of a PATCH-drawing title, `kyty_agent playable-regression`, the architecture gate, A/B | Gate | Required for each merge. |

No test may check presentation, log wording, or text that changes often (AGENTS.md testing
rules). The halt and continue lines are used as run evidence only, not asserted in a test.

## 8. Risks

1. **Strict refusal stops titles that reach a PATCH draw.** Today such a draw is dropped. After
   S0 the run stops with an explicit refusal, and that changes frontier results. The S0 gate
   decides this per title (section 6).
2. **Refusal placement.** The refusal sits after the guard checks of both entries and before
   every skip site (section 6, S0 step 1). Its side effects: the render lock is not taken for a
   refused draw, and the diagnostic A/B switches no longer skip PATCH draws silently (section 6,
   S0 step 1, last bullet).
3. **Guest ABI guesses.** The LS/HS/ES ring layout and the VGT field layouts are not in RDNA2
   Shader ISA. Without S0 evidence, a field layout would violate the no-invented-behaviour
   rule.
4. **Optional Vulkan feature.** Without `tessellationShader`, the design refuses explicitly.
   Compute emulation is an undecided later option (decision 3), not scheduled in S0-S4.
5. **Freed TF address.** `Graphics.cpp:925-937` returns freed memory. Any consumer faults or
   aliases. The defect is fixed in S1, after the S0 evidence gate, as its own change, with
   A/B (section 6, S1).
6. **Driver pitfall.** The dynamic per-vertex output observation (section 4) is unverified.
   Test with a standalone GLSL repro before relying on either form.
7. **Semantic choices.** Quads, isolines and power-of-two partitioning are semantic choices.
   Equal spacing used as an approximation of power-of-two partitioning would produce wrong
   geometry silently, so it is refused.
8. **Unverified reference claims.** Claims about commits that landed in another branch are not
   in this base and are not used as evidence.
9. **Other skips remain.** The skips for other primitive types and for the geometry-engine and
   invalid-vertex-shader cases are out of scope. They still drop non-PATCH draws, with counts but
   without refusal (section 5).
10. **Best-effort evidence under Silent.** With `PrintfDirection = Silent`, the fields line is
    not printed. The halt line and the continue line still are. The wall-clock runs of AGENTS.md
    rely on those two lines.

## 9. Reference material status (lead only)

Reference material outside the repository covers hull and domain behaviour. Its status as of
this design:

- Two attempts were closed without merge. The closure reasons are not recorded in the captured
  material, so they are treated as unknown.
- Two attempts are open. Their designs are leads only.
- None is merged into this base, which has no tessellation code.
- A claim that one closed attempt landed in mainline commits is **unverified** in this base.

Nothing from that material is copied. Behaviour may be reimplemented in Kyty's own types, and
every guest-visible claim needs S0 evidence first (AGENTS.md).

## 10. Open issues

- **P0 (decision owner: the orchestrator). Strict classification of not-implemented halts.**
  This is shared behaviour, not specific to PATCH, and it blocks the classification milestone.
  The chain, with evidence:
  1. `Report` calls the decision hook on a strict halt (`lib/Core/src/BringUp.cpp:737-743`,
     hook call at 741). The hook emits `bringup_halt` (`src/Agent/AgentLifecycle.cpp:108-137`).
  2. `dbg_not_implemented_handler` then sends `not_implemented_halt` (`DbgAssert.cpp:130`), which
     the agent also records as `bringup_halt` (`AgentLifecycle.cpp:414-421`). It then calls
     `ShutdownActiveSubsystems` (`DbgAssert.cpp:31-37`). That calls `UnexpectedShutdown` on each
     subsystem (`lib/Core/src/Subsystems.cpp:203-217`), and the agent subsystem calls `Stop`
     (`src/Agent/AgentSubsystem.cpp:26-29`). `Stop` interrupts the listener and joins the server
     thread (`src/Agent/AgentServer.cpp:1429-1450`).
  3. `ASSERT_HALT` then calls `dbg_exit(321)` (`DbgAssert.h:41-47`), which sends `dbg_exit`. The
     agent maps that to a `host_crash` Fatal event (`DbgAssert.cpp:157-161`,
     `AgentLifecycle.cpp:414-421, 441-443`). The event ring still accepts it
     (`src/Agent/EventRing.cpp:109`). The in-process `LastError` returns the newest Error or Fatal
     record (`EventRing.cpp:238-259`), so after this step it returns `host_crash`. No reader
     remains after the server stops, so an external runner cannot read this event.
  4. The runner polls `last_error` over the socket (`agent/src/runner/GamesMatrix.cpp:372-391`).
     Its outcome is `unsupported` only if that poll caught the `bringup_halt` before the shutdown
     in step 2. Otherwise the process exit (65, with no error code) maps to `loader_failed`
     through `post_agent_exit` (`GamesMatrix.cpp:716-736`).

  Options: (a) write a bounded halt record (identity, file, line) to the run's evidence directory
  before `ShutdownActiveSubsystems`, and have the runner read it; (b) the runner classifies the
  run from the not-implemented line in the process output. Whether the runner captures that output
  is **[unverified]**. Exit status 65 alone cannot be used, because `ASSERT`, `EXIT_IF`, `EXIT`
  and the not-implemented macro all end in `dbg_exit(321)` (`DbgAssert.h:54-55, 81, 86`); (c) keep
  the agent server up until the runner has read the halt event. This design recommends (a), as a
  separate item to be decided before S0 merges. Until then the classification milestone stays
  unverified.
- **P1 (defect, not fixed here).** `src/Graphics/GraphicsRunOpParsers.cpp:901-907` calls
  `Core::BringUp::Report` with feature `GraphicsPermissive`, then `EXIT`. In strict mode the Report
  call emits a `bringup_halt` decision event before the exit. The `EXIT` then sends an `exit` host
  fault, and `dbg_exit` sends a second one. Both are recorded as `host_crash`. The site does not
  print the not-implemented line and does not send a `not_implemented_halt` fault. Trigger: an
  unknown indirect context register in strict mode. The classification effect is the same as for
  the macro (P0). Whether this site should use the not-implemented macro is an owner decision, not
  a fix: the macro uses `Feature::NotImplemented`, so the change would also change the feature
  reported for this site.
- **P2.** Approval of the S0 step 3 probe: its tag, environment variable name and output cap.
- **P3.** The S0 A/B outcome for titles that reach a PATCH draw (section 6). If any frontier title
  is affected, the decision goes to the orchestrator.
- **P4.** Confirm the Vulkan valid-usage texts listed in section 3.4 against the specification.
- **P5.** Confirm the `SPI_SHADER_PGM_*_HS` addresses (Pm4.h:926-935) and the VGT field layouts from
  evidence.
- **P6.** Confirm the stage-enable bit values (`Graphics.cpp:2863-2865`), and whether an HS draw
  presents the stage mask that the geometry-engine check sees (section 1), from a trace.
- **P7.** Re-check the output-mode rule against the bring-up history file (not in this export).
- **P8.** Decide the shader translator cache version bump when S2 lands. Not decided here.
- **P9.** Confirm the input control point count source and the draw-count unit for PATCH draws
  (section 3.1, draw count), from the S0 evidence.
- **P10.** D-LS: choose the LS placement (separate vertex pass, or inline in the TCS) at the start of
  S2, with the evidence listed in section 6, S2.
- **P11.** `docs/agent-tools.md` is not in this export. Its tool descriptions must be checked against
  the tool list in the agent source (`src/Agent/Protocol.cpp:140-164`) before the S0 step 2 table is
  signed off.
- **P12.** The other skips in section 5 (`GraphicsRenderDraw.cpp:1062-1067`, `1230-1315`, `3037`,
  `GraphicsRun.cpp:2563-2577`) are not covered by this design. They need a separate item.
- **P13.** Whether the indirect and zero-count paths in section 1 need any change. This design
  decides no change for PATCH, since the draw is refused before any count is used.
- **P14.** Confirm that primitive type 9 is PATCH, from a trace or a published PM4 contract. The only
  local evidence is Kyty's own code: `Graphics.cpp:2881` treats type 9 as PATCH when an HS owner is
  present, and the resolver has no case for 9 (`GraphicsRenderDraw.cpp:1036-1067`). The word PATCH
  does not occur in the RDNA2 Shader ISA copy used for this design.
- **P15.** Decide whether the multi-draw opcodes 0x2C and 0x38 (section 1, draw routes) need a
  handler, and, if they do, how their PATCH draws reach the S0 refusal. The S0 refusal does not cover
  these routes today.
- **P16.** Line numbers in this document are for the base export. Another checkout of the same code
  base has different line numbers for the same functions (for example `GraphicsRenderDrawIndex`).
  The S0 insertion points must be re-derived on the branch that lands S0.
